// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/JitPPC64/Jit.h"

#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <sched.h>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/CoreTiming.h"
#include "Core/HW/MMIO.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/CPU.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/PPCAnalyst.h"
#include "Core/PowerPC/PPCTables.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

thread_local JitPPC64* JitPPC64::s_active_instance = nullptr;

// Dedicated page at a fixed address ABOVE 4 GB for the NCE context slot.
// The guest (32-bit PPC) cannot write here, preventing the guest from
// corrupting the NativeContext pointer during native execution (which
// shares SHM with the NCE mapping at addresses 0-4GB).
// Page base: 0x100000000 (4GB), offset 0x3FF8 → slot at 0x100003FF8.
static constexpr u64 NCE_SLOT_ADDR = 0x100003FF8ULL;

// Allocate the dedicated page during NCE guest mapping init.
static bool NCE_SlotPageAllocated = false;
static void AllocateNCESlotPage()
{
  if (NCE_SlotPageAllocated)
    return;
  void* page = mmap(reinterpret_cast<void*>(0x100000000ULL), 4096,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  if (page == MAP_FAILED)
    NOTICE_LOG_FMT(POWERPC, "NCE: slot page mmap at 0x100000000 failed, errno={}", errno);
  else
    NCE_SlotPageAllocated = true;
}

// =========================================================================
// Debug safety net: trivial handler that only modifies ucontext (no r2/r13).
// Registered for SIGSEGV/SIGILL/SIGTRAP to catch crashes from the SIGALRM
// asm entry without double-faulting the process.
// =========================================================================

extern "C" void nce_safety_handler(int sig, siginfo_t* info, void* uctx);

void nce_safety_handler(int sig, siginfo_t* info, void* uctx)
{
  (void)sig; (void)info;
  static_cast<ucontext_t*>(uctx)->uc_mcontext.regs->nip += 4;
}

// =========================================================================
// Test: asm entry for SIGALRM that restores host r2/r13 from nce_ctx_slot
// via the `oris` approach (already verified correct for address calculation)
// =========================================================================

extern "C" void nce_alrm_entry();
extern "C" void nce_bridge_alrm(int sig, siginfo_t* info, void* uctx);

void nce_bridge_alrm(int sig, siginfo_t* info, void* uctx)
{
  if (JitPPC64::s_active_instance)
    JitPPC64::s_active_instance->HandleSIGALRM(sig, info, uctx);
}

extern "C" void nce_segv_entry();
extern "C" void nce_bridge_segv(int sig, siginfo_t* info, void* uctx);

void nce_bridge_segv(int sig, siginfo_t* info, void* uctx)
{
  if (JitPPC64::s_active_instance)
    JitPPC64::s_active_instance->HandleSIGSEGV(sig, info, uctx);
}

extern "C" void nce_ill_entry();
extern "C" void nce_bridge_ill(int sig, siginfo_t* info, void* uctx);

void nce_bridge_ill(int sig, siginfo_t* info, void* uctx)
{
  if (JitPPC64::s_active_instance)
    JitPPC64::s_active_instance->HandleSIGILL(sig, info, uctx);
}

asm(
".globl nce_alrm_entry\n"
".type nce_alrm_entry, @function\n"
"nce_alrm_entry:\n"
"  li 12, 0\n"
"  oris 12, 12, 0x1000\n"
"  sldi 12, 12, 4\n"
"  addi 12, 12, 0x3FF8\n"
"  ld 12, 0(12)\n"
"  cmpdi 12, 0\n"
"  beq 1f\n"
"  ld 2, 0(12)\n"
"  ld 13, 8(12)\n"
"1:\n"
"  b nce_bridge_alrm\n"
"\n"
".globl nce_segv_entry\n"
".type nce_segv_entry, @function\n"
"nce_segv_entry:\n"
"  li 12, 0\n"
"  oris 12, 12, 0x1000\n"
"  sldi 12, 12, 4\n"
"  addi 12, 12, 0x3FF8\n"
"  ld 12, 0(12)\n"
"  cmpdi 12, 0\n"
"  beq 1f\n"
"  ld 2, 0(12)\n"
"  ld 13, 8(12)\n"
"1:\n"
"  b nce_bridge_segv\n"
"\n"
".globl nce_ill_entry\n"
".type nce_ill_entry, @function\n"
"nce_ill_entry:\n"
"  li 12, 0\n"
"  oris 12, 12, 0x1000\n"
"  sldi 12, 12, 4\n"
"  addi 12, 12, 0x3FF8\n"
"  ld 12, 0(12)\n"
"  cmpdi 12, 0\n"
"  beq 1f\n"
"  ld 2, 0(12)\n"
"  ld 13, 8(12)\n"
"1:\n"
"  b nce_bridge_ill\n"
);

JitPPC64::JitPPC64(Core::System& system) : JitBase(system)
{
  m_dcr.fill(DCR_INIT);
}

JitPPC64::~JitPPC64() = default;

void JitPPC64::Init()
{
  NOTICE_LOG_FMT(POWERPC, "NCE Init start");
  RefreshConfig();
  m_block_cache.Init();

  code_block.m_stats = &js.st;
  code_block.m_gpa = &js.gpa;
  code_block.m_fpa = &js.fpa;

  jo.fastmem_arena = false;
  jo.optimizeGatherPipe = false;

  asm volatile("mr %0, %%r2\n\t" : "=r"(m_host_sda[0]));
  asm volatile("mr %0, %%r13\n\t" : "=r"(m_host_sda[1]));
  NOTICE_LOG_FMT(POWERPC, "NCE Init done");
}

void JitPPC64::Shutdown()
{
  StopNativeTimer();
  UnpatchAllP0();
  UnpatchAllDCBZ();
  RemoveSignalHandlers();
  MakeGuestMemoryNonExecutable();

  if (m_nce_mapping_done)
  {
    m_system.GetMemory().ShutdownNCEGuestMapping();
    m_nce_mapping_done = false;
  }

  m_block_cache.Shutdown();
}

void JitPPC64::ClearCache()
{
  m_block_cache.Clear();
}

// ---------------------------------------------------------------------------
// P0 instruction detection
// ---------------------------------------------------------------------------

// Check if a guest instruction silently produces wrong results on PPC970.
// These instructions DON'T cause SIGILL (they execute as different PPC970
// ops or return wrong data).  They must be routed through the interpreter
// before NCE can run them.
static bool IsP0Instruction(u32 instr)
{
  const u32 opcd = (instr >> 26) & 0x3F;

  // psq_l/psq_lu (opcd 56,57) → lq/lq_u on PPC970 (16-byte load)
  // psq_st/psq_stu (opcd 60,61) → stq/stq_u on PPC970 (16-byte store)
  if (opcd == 56 || opcd == 57 || opcd == 60 || opcd == 61)
    return true;

  // ps_* (opcd 4) — Gekko Paired Single instructions.  PPC970 executes
  // them as AltiVec ops; most give wrong results or SIGILL.
  // ps_add(21)/sub(20)/mul(25) map correctly to vaddfp/vsubfp/vmulfp
  // and execute natively at full speed — leave them alone.
  if (opcd == 4)
  {
    const u32 xo = (instr >> 1) & 0x3FF;
    if (xo != 21 && xo != 20 && xo != 25)
      return true;
  }

  if (opcd == 31)
  {
    const u32 xo = (instr >> 1) & 0x3FF;

    // mftb (xo=371) → reads real PPC970 timebase, not emulated
    if (xo == 371)
      return true;

    // mfmsr (xo=83) → returns real PPC970 MSR, not Gekko MSR
    if (xo == 83)
      return true;

    // mtmsr (xo=146) → writes real PPC970 MSR (dangerous: can disable
    // interrupts, change endianness, etc.)
    if (xo == 146)
      return true;

    // mfspr (xo=339) with SPRs that don't SIGILL on PPC970 but return wrong
    // values.  Covers three categories:
    //   1. User-readable standard SPRs with different values (PVR, TL, TU)
    //   2. Implementation-specific SPRs that are user-readable on PPC970
    //      (HID0/HID1/HID2/L2CR)
    //   3. Gekko-specific SPRs >= 912 (GQRs, WPAR, DMAU, DMAL, ECID, HID4,
    //      IABR, DABR, ICTC, THRM) — PPC970 either returns 0 or ignores these
    //      silently instead of SIGILLing.
    if (xo == 339)
    {
      const u32 spr = ((instr >> 11) & 0x1F) << 5 | ((instr >> 16) & 0x1F);
      if (spr == 287 || spr == 268 || spr == 269 ||
          spr >= 912)
        return true;
    }
    // mtspr (xo=467): same categories as mfspr.  Writes to non-existent or
    // implementation-specific SPRs are silently dropped on PPC970.
    if (xo == 467)
    {
      const u32 spr = ((instr >> 11) & 0x1F) << 5 | ((instr >> 16) & 0x1F);
      if (spr >= 912)
        return true;
    }
  }

  return false;
}

// ---------------------------------------------------------------------------
// PPC instruction encoding helpers (for ps_* → AltiVec trampolines)
// ---------------------------------------------------------------------------

namespace {

// D-form: opcd | frS/rD<<21 | rA<<16 | d(signed)
u32 Dform(u32 opcd, u32 frs_rd, u32 ra, s32 d)
{
  return opcd << 26 | (frs_rd & 0x1F) << 21 | (ra & 0x1F) << 16 | (d & 0xFFFF);
}
// X-form (opcd 31): 31 | rD<<21 | rA<<16 | rB<<11 | xo<<1 | rc
u32 Xform31(u32 rd, u32 ra, u32 rb, u32 xo, u32 rc = 0)
{
  return 31u << 26 | (rd & 0x1F) << 21 | (ra & 0x1F) << 16 | (rb & 0x1F) << 11 | xo << 1 | rc;
}
// AltiVec VA-form (opcd 4): 4 | vD<<21 | vA<<16 | vB<<11 | xo (11-bit, bits 0-10)
u32 AV_VA(u32 vd, u32 va, u32 vb, u32 xo)
{
  return 4u << 26 | (vd & 0x1F) << 21 | (va & 0x1F) << 16 | (vb & 0x1F) << 11 | (xo & 0x7FF);
}
// B-form (AA=1 absolute): 18 | LI| 1<<30
u32 Babs(u32 target)
{
  return 18u << 26 | (target & 0x03FFFFFFu) >> 2 | 1u << 30;
}
u32 Addi(u32 rd, u32 ra, s32 si) { return 14u << 26 | (rd & 0x1F) << 21 | (ra & 0x1F) << 16 | (si & 0xFFFF); }
u32 Ori(u32 ra, u32 rs, u32 ui) { return 24u << 26 | (rs & 0x1F) << 21 | (ra & 0x1F) << 16 | (ui & 0xFFFF); }
u32 Lis(u32 rd, u32 si) { return 15u << 26 | (rd & 0x1F) << 21 | (si & 0xFFFF); }

// AltiVec XO constants (11-bit, PowerISA 2.01)
// VX-form logical: subop5<<6 | xo5
enum : u32
{
  AV_VADDFP = 26,
  AV_VSUBFP = 28,
  AV_VMULFP = 52,
  AV_VAND   = 964,   // 0x3C4  subop5=15(01111), xo5=4
  AV_VANDC  = 1092,  // 0x444  subop5=17(10001), xo5=4
  AV_VOR    = 1156,  // 0x484  subop5=18(10010), xo5=4
  AV_VXOR   = 1224,  // 0x4C8  subop5=19(10011), xo5=8
  AV_VNOR   = 1284,  // 0x504  subop5=20(10100), xo5=4
  AV_VSRAW  = 1542,  // 0x606  subop5=24(11000), xo5=6
  AV_LVX    = 103,   // 7-bit XO in X-form
  AV_STVX   = 231,   // 7-bit XO in X-form
};

}  // anonymous namespace

// ---------------------------------------------------------------------------
// ps_* → AltiVec trampoline generation
// ---------------------------------------------------------------------------
//
// Instructions that can be mapped directly to PPC970 AltiVec get small
// trampolines in the NCE K1 mapping (at PS_ALTIVEC_BASE) that do
// FPR→memory→VR→AltiVec→VR→memory→FPR and branch back.
//
// The trampoline replaces the original ps_* via `b tramp_addr` (AA=1)
// instead of an illegal instruction, avoiding a SIGILL round-trip (~2 µs).
//
// Currently:
//   ps_mr(72)     → vor with identity
//   ps_abs(264)   → vand with abs_mask        (constant pool at scratch+0)
//   ps_neg(40)    → vxor with neg_mask        (constant pool at scratch+16)
//   ps_nabs(136)  → vor with neg_mask         (constant pool at scratch+16)
//
// ps_add(21)/sub(20)/mul(25) execute NATIVELY on PPC970 as vaddfp/vsubfp/
// vmulfp — no trampoline or patch needed.
//
// All other ps_* (sel, div, res, rsqrte, merge*, sum*, muls*, madd*,
// cmp*, etc.) are P0-patched with an illegal instruction → SIGILL →
// interpreter fallback.

bool JitPPC64::IsPsArithInstruction(u32 instr)
{
  const u32 opcd = (instr >> 26) & 0x3F;
  if (opcd != 4)
    return false;
  const u32 xo = (instr >> 1) & 0x3FF;
  // Only instructions that get AltiVec trampolines (fast path).
  // add(21)/sub(20)/mul(25) execute natively as vaddfp/vsubfp/vmulfp.
  // sel(23) too complex for 16-instr trampoline → trap+interpreter.
  switch (xo)
  {
  case 72: case 264: case 40: case 136:  // mr, abs, neg, nabs
    return true;
  }
  return false;
}

void JitPPC64::GeneratePsTrampolines()
{
  if (!m_ps_trampoline_map.empty())
    return;

  // Write AltiVec constant pool to the scratch area.
  // Layout: abs_mask(16B) + neg_mask(16B) + shift31(16B) + working area.
  // The scratch page is in the NCE mapping (K1, 0x81FFF000) — demand-zero
  // from shm_open, so uninitialized bytes are 0.
  {
    u8* page = reinterpret_cast<u8*>(static_cast<uintptr_t>(PS_SCRATCH_ADDR));
    constexpr u32 abs_mask_val = 0x7FFFFFFFu;
    constexpr u32 neg_mask_val = 0x80000000u;
    constexpr u32 shift31_val = 31u;
    for (int i = 0; i < 4; ++i)
    {
      std::memcpy(page + i * 4, &abs_mask_val, 4);
      std::memcpy(page + 16 + i * 4, &neg_mask_val, 4);
      std::memcpy(page + 32 + i * 4, &shift31_val, 4);
    }
    __builtin___clear_cache(reinterpret_cast<char*>(page),
                            reinterpret_cast<char*>(page + 48));
  }

  auto& memory = m_system.GetMemory();
  const u32 ram_size = memory.GetRamSizeReal();
  const u32 base = PS_ALTIVEC_BASE;
  const u32 scratch = PS_SCRATCH_ADDR;

  m_ps_trampoline_next = base;

  for (u32 offset = 0; offset + 4 <= ram_size; offset += 4)
  {
    const u32 addr = 0x80000000u + offset;
    u32 instr;
    std::memcpy(&instr, reinterpret_cast<const void*>(static_cast<uintptr_t>(addr)),
                sizeof(instr));

    if (!IsPsArithInstruction(instr))
      continue;

    const u32 tramp = m_ps_trampoline_next;
    m_ps_trampoline_next += PS_TRAMP_STRIDE;
    if (m_ps_trampoline_next > base + PS_ALTIVEC_SIZE)
    {
      ERROR_LOG_FMT(POWERPC, "NCE: ps_* trampoline area exhausted at PC={:08x}", addr);
      break;
    }

    const u32 frD = (instr >> 21) & 0x1F;
    const u32 frB = (instr >> 11) & 0x1F;
    const u32 xo  = (instr >> 1) & 0x3FF;

    u32 code[16] = {};
    int ci = 0;

    // lis r12, scratch >> 16
    code[ci++] = Lis(12, scratch >> 16);
    // ori r12, r12, scratch & 0xFFFF
    code[ci++] = Ori(12, 12, scratch & 0xFFFF);

    constexpr u32 WK = 48;  // working area offset (past the 48-byte constant pool)

    switch (xo)
    {
    case 72:  // ps_mr frD, frB: frD = frB
      code[ci++] = Dform(54, frB, 12, WK);         // stfd frB, WK(r12)
      code[ci++] = Addi(12, 12, WK);
      code[ci++] = Xform31(0, 0, 12, AV_LVX);       // lvx v0, 0, r12  — frB+garbage
      code[ci++] = AV_VA(1, 0, 0, AV_VOR);          // v1 = v0 | v0 (copy)
      code[ci++] = Xform31(1, 0, 12, AV_STVX);       // stvx v1, 0, r12
      code[ci++] = Addi(12, 12, -WK);
      code[ci++] = Dform(50, frD, 12, WK);          // lfd frD, WK(r12)
      code[ci++] = Babs(addr + 4);
      break;

    case 264:  // ps_abs frD, frB: frD = |frB| → vand with abs_mask
      code[ci++] = Dform(54, frB, 12, WK);          // stfd frB, WK(r12)
      code[ci++] = Xform31(0, 0, 12, AV_LVX);        // lvx v0, 0, r12  — v0 = abs_mask
      code[ci++] = Addi(12, 12, WK);
      code[ci++] = Xform31(1, 0, 12, AV_LVX);        // lvx v1, 0, r12  — v1 = frB
      code[ci++] = Addi(12, 12, -WK);
      code[ci++] = AV_VA(1, 1, 0, AV_VAND);          // v1 = v1 & v0 = frB & ~sign
      code[ci++] = Addi(12, 12, WK);
      code[ci++] = Xform31(1, 0, 12, AV_STVX);       // stvx v1, 0, r12
      code[ci++] = Addi(12, 12, -WK);
      code[ci++] = Dform(50, frD, 12, WK);           // lfd frD, WK(r12)
      code[ci++] = Babs(addr + 4);
      break;

    case 40:  // ps_neg frD, frB: frD = -frB → vxor with neg_mask
      code[ci++] = Dform(54, frB, 12, WK);           // stfd frB, WK(r12)
      code[ci++] = Addi(12, 12, 16);
      code[ci++] = Xform31(0, 0, 12, AV_LVX);        // lvx v0, 0, r12  — v0 = neg_mask
      code[ci++] = Addi(12, 12, -16);
      code[ci++] = Addi(12, 12, WK);
      code[ci++] = Xform31(1, 0, 12, AV_LVX);        // lvx v1, 0, r12  — v1 = frB
      code[ci++] = Addi(12, 12, -WK);
      code[ci++] = AV_VA(1, 1, 0, AV_VXOR);          // v1 = v1 ^ v0 = frB ^ sign
      code[ci++] = Addi(12, 12, WK);
      code[ci++] = Xform31(1, 0, 12, AV_STVX);       // stvx v1, 0, r12
      code[ci++] = Addi(12, 12, -WK);
      code[ci++] = Dform(50, frD, 12, WK);           // lfd frD, WK(r12)
      code[ci++] = Babs(addr + 4);
      break;

    case 136:  // ps_nabs frD, frB: frD = -|frB| → vor with neg_mask
      code[ci++] = Dform(54, frB, 12, WK);           // stfd frB, WK(r12)
      code[ci++] = Addi(12, 12, 16);
      code[ci++] = Xform31(0, 0, 12, AV_LVX);        // lvx v0, 0, r12  — v0 = neg_mask
      code[ci++] = Addi(12, 12, -16);
      code[ci++] = Addi(12, 12, WK);
      code[ci++] = Xform31(1, 0, 12, AV_LVX);        // lvx v1, 0, r12  — v1 = frB
      code[ci++] = Addi(12, 12, -WK);
      code[ci++] = AV_VA(1, 1, 0, AV_VOR);           // v1 = v1 | v0 = frB | sign
      code[ci++] = Addi(12, 12, WK);
      code[ci++] = Xform31(1, 0, 12, AV_STVX);       // stvx v1, 0, r12
      code[ci++] = Addi(12, 12, -WK);
      code[ci++] = Dform(50, frD, 12, WK);           // lfd frD, WK(r12)
      code[ci++] = Babs(addr + 4);
      break;
    }

    // Pad with nop
    while (ci < 16)
      code[ci++] = 0x60000000;

    // Write to NCE mapping
    std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(tramp)), code, sizeof(code));
    __builtin___clear_cache(reinterpret_cast<char*>(tramp),
                            reinterpret_cast<char*>(tramp + sizeof(code)));

    m_ps_trampoline_map[addr] = tramp;
  }

  NOTICE_LOG_FMT(POWERPC, "NCE: generated {} ps_* AltiVec trampolines",
                 m_ps_trampoline_map.size());
}


// ---------------------------------------------------------------------------
// dcbz → trap patching
// ---------------------------------------------------------------------------
//
// On PPC970, dcbz zeroes 128 bytes instead of 32 (Gekko).  We patch every
// dcbz instruction in guest code with an illegal instruction (0x00000000)
// before NCE entry.  The SIGILL handler catches the SIGILL, reads the original
// dcbz from our map, and emulates it correctly (zero 32 bytes at EA).
//
// The patches are applied to the SHM-backed guest code (visible at K1/K2 NCE
// aliases and GetRAM()).  Before any interpreter fallback we unpatch, then
// re-patch before re-entering NCE.
// ---------------------------------------------------------------------------

void JitPPC64::ScanDCBZ()
{
  m_patched_dcbz.clear();

  auto& memory = m_system.GetMemory();
  const u32 ram_size_val = memory.GetRamSizeReal();

  // Scan K1 cached alias (0x80000000-0x8FFFFFFF) for dcbz.
  // This covers all aliases (K1, K2, K1 uncached, K2 uncached) that map
  // SHM offset 0 = the emulated RAM.
  for (u32 offset = 0; offset + 4 <= ram_size_val; offset += 4)
  {
    const u32 addr = 0x80000000u + offset;
    u32 instr;
    std::memcpy(&instr, reinterpret_cast<const void*>(static_cast<uintptr_t>(addr)),
                sizeof(instr));
    const u32 opcd = (instr >> 26) & 0x3F;
    if (opcd == 31)
    {
      const u32 xo = (instr >> 1) & 0x3FF;
      if (xo == 1014)  // dcbz
        m_patched_dcbz[addr] = instr;
      // Also handle dcbzl (locked dcbz variant) — same xo=1014 with bit 10 set
      // On PPC970 both variants behave identically (128-byte zero).
    }
  }

  NOTICE_LOG_FMT(POWERPC, "NCE: scanned RAM, found {} dcbz instructions",
                 m_patched_dcbz.size());

  // EXRAM scan is REMOVED because NCE doesn't map the 0x90000000 EXRAM
  // region.  Trying to read from unmapped NCE EXRAM would generate millions
  // of SIGSEGV signals (64MB / 4 = 16M iterations × signal-handler latency).
  // The RAM scan above already covers 0x80000000–0x81FFFFFF.

  // Add K2 aliases for all K1 patched addresses
  std::unordered_map<u32, u32> k2_aliases;
  for (const auto& [addr, orig] : m_patched_dcbz)
  {
    if ((addr >> 28) == 0x8)
    {
      const u32 k2_addr = 0x70000000u | (addr & 0x0FFFFFFFu);
      k2_aliases[k2_addr] = orig;
      const u32 k2u_addr = 0xB0000000u | (addr & 0x0FFFFFFFu);
      k2_aliases[k2u_addr] = orig;
      const u32 k1u_addr = 0xC0000000u | (addr & 0x3FFFFFFFu);
      k2_aliases[k1u_addr] = orig;
    }
    else if ((addr >> 28) == 0x9)
    {
      const u32 k1u_addr = 0xD0000000u | (addr & 0x0FFFFFFFu);
      k2_aliases[k1u_addr] = orig;
    }
  }
  // Only add entries for aliases that don't already exist (same physical SHM
  // page, so the trap is already written).
  for (const auto& [addr, orig] : k2_aliases)
  {
    if (m_patched_dcbz.find(addr) == m_patched_dcbz.end())
      m_patched_dcbz[addr] = orig;
  }

  NOTICE_LOG_FMT(POWERPC, "NCE: total {} patched addresses (including aliases)",
                 m_patched_dcbz.size());
}

void JitPPC64::PatchAllDCBZ()
{
  // If the game loaded new code (icbi was called), re-scan RAM for dcbz.
  // Game-loaded dcbz are not in the Init-time scan and would otherwise
  // execute natively on PPC970, zeroing 128 bytes instead of Gekko's 32.
  if (m_dcbz_needs_rescan)
  {
    if (m_dcbz_patches_applied)
      UnpatchAllDCBZ();
    m_patched_dcbz.clear();
    m_dcbz_needs_rescan = false;
    // Fall through to ScanDCBZ below since map is now empty
  }

  if (m_dcbz_patches_applied)
    return;
  if (m_patched_dcbz.empty())
  {
    ScanDCBZ();
    if (m_patched_dcbz.empty())
      return;
  }

  const u32 illegal = 0x00000000u;  // illegal instruction → SIGILL
  for (const auto& [addr, unused] : m_patched_dcbz)
  {
    (void)unused;
    std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(addr)), &illegal, sizeof(illegal));
  }

  // Clear icache for patched regions.  The patched addresses span the full
  // RAM/EXRAM ranges, so clear the entire K1/K2 ranges once.
  auto& memory = m_system.GetMemory();
  const u32 ram_size_val = memory.GetRamSizeReal();
  if (ram_size_val > 0)
  {
    __builtin___clear_cache(reinterpret_cast<char*>(0x80000000ULL),
                            reinterpret_cast<char*>(0x80000000ULL + ram_size_val));
    __builtin___clear_cache(reinterpret_cast<char*>(0x70000000ULL),
                            reinterpret_cast<char*>(0x70000000ULL + ram_size_val));
  }
  // EXRAM is not in the NCE mapping and has no patched dcbz — skip icache clear.

  m_dcbz_patches_applied = true;
}

void JitPPC64::UnpatchAllDCBZ()
{
  if (!m_dcbz_patches_applied)
    return;
  if (m_patched_dcbz.empty())
  {
    m_dcbz_patches_applied = false;
    return;
  }

  for (const auto& [addr, orig] : m_patched_dcbz)
  {
    std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(addr)), &orig, sizeof(orig));
  }

  // Clear icache for patched regions (same ranges as PatchAllDCBZ).
  auto& memory = m_system.GetMemory();
  const u32 ram_size_val = memory.GetRamSizeReal();
  if (ram_size_val > 0)
  {
    __builtin___clear_cache(reinterpret_cast<char*>(0x80000000ULL),
                            reinterpret_cast<char*>(0x80000000ULL + ram_size_val));
    __builtin___clear_cache(reinterpret_cast<char*>(0x70000000ULL),
                            reinterpret_cast<char*>(0x70000000ULL + ram_size_val));
  }
  // EXRAM is not in the NCE mapping and has no patched dcbz — skip icache clear.

  m_dcbz_patches_applied = false;
}

// ---------------------------------------------------------------------------
// P0 → trap patching (silently-wrong PPC970 instructions)
// ---------------------------------------------------------------------------

void JitPPC64::ScanP0()
{
  if (!m_patched_p0.empty())
    return;

  auto& memory = m_system.GetMemory();
  const u32 ram_size_val = memory.GetRamSizeReal();

  for (u32 offset = 0; offset + 4 <= ram_size_val; offset += 4)
  {
    const u32 addr = 0x80000000u + offset;
    u32 instr;
    std::memcpy(&instr, reinterpret_cast<const void*>(static_cast<uintptr_t>(addr)),
                sizeof(instr));
    if (IsP0Instruction(instr))
      m_patched_p0[addr] = instr;
  }

  NOTICE_LOG_FMT(POWERPC, "NCE: scanned RAM, found {} P0 instructions",
                 m_patched_p0.size());

  // Generate AltiVec trampolines for ps_* arithmetic (fast path)
  GeneratePsTrampolines();

  // Add K2 / uncached aliases
  std::unordered_map<u32, u32> aliases;
  for (const auto& [addr, orig] : m_patched_p0)
  {
    if ((addr >> 28) == 0x8)
    {
      aliases[0x70000000u | (addr & 0x0FFFFFFFu)] = orig;  // K2 cached
      aliases[0xB0000000u | (addr & 0x0FFFFFFFu)] = orig;  // K2 uncached
      aliases[0xC0000000u | (addr & 0x3FFFFFFFu)] = orig;  // K1 uncached
    }
    else if ((addr >> 28) == 0x9)
    {
      aliases[0xD0000000u | (addr & 0x0FFFFFFFu)] = orig;  // K1 uncached block 1
    }
  }
  for (const auto& [addr, orig] : aliases)
  {
    if (m_patched_p0.find(addr) == m_patched_p0.end())
      m_patched_p0[addr] = orig;
    // Also propagate ps_* trampoline mapping to all aliases
    if (auto tramp_it = m_ps_trampoline_map.find(addr & 0x0FFFFFFFu); tramp_it != m_ps_trampoline_map.end())
    {
      // addr is K1 0x80000000+offset, but aliases erase the block ID.
      // Map the original K1 addr → look up trampoline from the KEY that
      // matches the original addr.  Use a reverse lookup.
    }
  }

  // Propagate ps_* trampoline mapping to all aliases.
  // (Do this in a separate pass to avoid reverse-lookup complexity.)
  std::unordered_map<u32, u32> tramp_aliases;
  for (const auto& [addr, orig] : m_patched_p0)
  {
    if (m_ps_trampoline_map.find(addr) != m_ps_trampoline_map.end())
      continue;  // already mapped (K1 cached)
    // Check if any K1 cached address with the same physical offset has a trampoline
    u32 phys = addr & 0x3FFFFFFFu;
    u32 k1_addr = 0x80000000u | (phys & 0x0FFFFFFFu);
    auto src = m_ps_trampoline_map.find(k1_addr);
    if (src == m_ps_trampoline_map.end())
    {
      k1_addr = 0x80000000u | phys;  // for EXRAM (block 1, offset already 31 bits)
      src = m_ps_trampoline_map.find(k1_addr);
    }
    if (src != m_ps_trampoline_map.end())
      tramp_aliases[addr] = src->second;
  }
  for (const auto& [addr, tramp] : tramp_aliases)
    m_ps_trampoline_map[addr] = tramp;

  NOTICE_LOG_FMT(POWERPC, "NCE: total {} patched P0 addresses (including aliases)",
                 m_patched_p0.size());
}

void JitPPC64::PatchAllP0()
{
  if (m_p0_patches_applied || m_patched_p0.empty())
    return;

  for (const auto& [addr, orig] : m_patched_p0)
  {
    auto it = m_ps_trampoline_map.find(addr);
    if (it != m_ps_trampoline_map.end())
    {
      // ps_* arithmetic: patch with branch to AltiVec trampoline
      const u32 branch = Babs(it->second);
      std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(addr)), &branch, sizeof(branch));
    }
    else
    {
      // Other P0: patch with illegal instruction → SIGILL → interpreter fallback
      const u32 illegal = 0x00000000u;
      std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(addr)), &illegal, sizeof(illegal));
    }
  }

  auto& memory = m_system.GetMemory();
  const u32 ram_size_val = memory.GetRamSizeReal();
  if (ram_size_val > 0)
  {
    __builtin___clear_cache(reinterpret_cast<char*>(0x80000000ULL),
                            reinterpret_cast<char*>(0x80000000ULL + ram_size_val));
    __builtin___clear_cache(reinterpret_cast<char*>(0x70000000ULL),
                            reinterpret_cast<char*>(0x70000000ULL + ram_size_val));
  }

  m_p0_patches_applied = true;
}

void JitPPC64::UnpatchAllP0()
{
  if (!m_p0_patches_applied)
    return;
  if (m_patched_p0.empty())
  {
    m_p0_patches_applied = false;
    return;
  }

  for (const auto& [addr, orig] : m_patched_p0)
  {
    std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(addr)), &orig, sizeof(orig));
  }

  auto& memory = m_system.GetMemory();
  const u32 ram_size_val = memory.GetRamSizeReal();
  if (ram_size_val > 0)
  {
    __builtin___clear_cache(reinterpret_cast<char*>(0x80000000ULL),
                            reinterpret_cast<char*>(0x80000000ULL + ram_size_val));
    __builtin___clear_cache(reinterpret_cast<char*>(0x70000000ULL),
                            reinterpret_cast<char*>(0x70000000ULL + ram_size_val));
  }

  m_p0_patches_applied = false;
}

// ---------------------------------------------------------------------------
// Main execution loop — uses native trampoline
// ---------------------------------------------------------------------------

void JitPPC64::Run()
{
  NOTICE_LOG_FMT(POWERPC, "NCE Run start");
  NOTICE_LOG_FMT(POWERPC, "NCE pc={:08x} gpr[1]={:08x}", m_ppc_state.pc, m_ppc_state.gpr[1]);
  SyncGuestState();
  NOTICE_LOG_FMT(POWERPC, "NCE guest state synced, msr={:08x} srr0={:08x}",
                 m_guest.msr, m_guest.srr0);
  InstallSignalHandlers();
  NOTICE_LOG_FMT(POWERPC, "NCE signal handlers installed");
  MakeGuestMemoryExecutable();
  NOTICE_LOG_FMT(POWERPC, "NCE memory executable");

  if (!m_nce_mapping_done)
  {
    m_system.GetMemory().InitNCEGuestMapping();
    m_nce_mapping_done = true;
    NOTICE_LOG_FMT(POWERPC, "NCE guest memory mapped");

    // Save host stack pointer and host SDA pointers.  If a signal fires
    // before the first trampoline entry (practically never, but be safe),
    // RestoreHostRegsInContext reads from the NativeContext stored at the
    // address in the NCE slot.  We initialise it to &m_native_ctx as fallback;
    // the inner loop overwrites it with &local_ctx before each trampoline call.
    asm volatile("mr %0, %%r1\n\t" : "=r"(m_native_ctx.host_r1));
    m_native_ctx.host_r2 = m_host_sda[0];
    m_native_ctx.host_r13 = m_host_sda[1];
    // Set a sentinel return address so HandleSIGALRM doesn't call
    // RestoreHostRegsInContext (which would jump to 0) before the first
    // trampoline call sets a real return address via saved LR.
    m_native_ctx.return_addr = 0xFFFFFFFFFFFFFFFFULL;
    AllocateNCESlotPage();
    // Keep the slot null here — the trampoline writes it right before bctr.
    // Signal handler stubs (nce_*_entry) check for null and skip r2/r13
    // restore when no guest code is running, which is correct here.
    *reinterpret_cast<NativeContext**>(NCE_SLOT_ADDR) = nullptr;

    // Dump /proc/self/maps for guest memory regions
    {
      FILE* fp = fopen("/proc/self/maps", "r");
      if (fp)
      {
        char line[512];
        while (fgets(line, sizeof(line), fp))
        {
          // Show NCE guest ranges (0x0-0x02000000, 0x70000000-0x80000000,
          // 0x80000000-0x82000000, 0xB0000000-0xC0000000, 0xC0000000-0xC2000000,
          // 0xFE000000-0x100000000) plus any low mappings below 0x7f000000.
          unsigned long start;
          if (sscanf(line, "%lx", &start) >= 1 &&
              (start < 0x02000000ULL ||
               (start >= 0x70000000ULL && start < 0x80000000ULL) ||
               (start >= 0x80000000ULL && start < 0x82000000ULL) ||
               (start >= 0x90000000ULL && start < 0xA0000000ULL) ||
               (start >= 0xB0000000ULL && start < 0xC0000000ULL) ||
               (start >= 0xC0000000ULL && start < 0xC2000000ULL) ||
               (start >= 0xD0000000ULL && start < 0xE0000000ULL) ||
               (start >= 0xFE000000ULL && start < 0x100000000ULL) ||
               (start >= 0x7e000000ULL && start < 0x7f000000ULL)))
            fprintf(stderr, "NCE: MAPS: %s", line);
        }
        fclose(fp);
      }
    }

    // Enable AltiVec (MSR[VR]=1) so the ps_* trampolines can use VMX.
    // Gekko has no AltiVec, so the game never sets VR — leaving it=1
    // is harmless because mfmsr is P0-patched (guest never reads real MSR).
    // We CANNOT use mtmsrd to set VR from userspace (privileged instruction).
    // Instead, execute a harmless lvx — the kernel's Vector Unavailable
    // handler enables VR and re-executes the instruction.
    static constexpr u8 altivec_dummy[16] __attribute__((aligned(16))) = {};
    asm volatile("lvx %%v0, 0, %0" : : "r"(altivec_dummy) : "v0");
  }

  auto& core_timing = m_system.GetCoreTiming();
  auto& cpu = m_system.GetCPU();

  // Start the periodic timer for downcount estimation
  StartNativeTimer();

  NOTICE_LOG_FMT(POWERPC, "NCE entering main loop");

  // Compute NCE trampoline location.  The .S file's .text section is NOT
  // executable on this system (PaX/kernel restriction), but the SHM-backed
  // NCE mapping (at 0x80000000+) IS executable.  Copy the trampoline code
  // into the NCE mapping and call via its virtual alias.
  // WARNING: The arena RAM pointer (GetRAM()) is NOT the same physical memory
  // as the NCE alias — the arena maps at SHM offset 0x02040000 while the NCE
  // mapping starts at SHM offset 0.  Always write directly to the NCE alias.
  u32 tramp_size = static_cast<u32>(
      reinterpret_cast<uintptr_t>(&JitPPC64EnterGuest_end) -
      reinterpret_cast<uintptr_t>(&JitPPC64EnterGuest));
  // Place at an offset past guest RAM within the 32 MB NCE mapping.
  // Guest RAM is 24 MB (0x1800000); mapping covers 0x0-0x2000000 (32 MB).
  // The IPL binary loads at 0x80000000 and uses addresses up to ~2 MB
  // for code; offset 0x10000 (64 KB) previously overwritten IPL code there.
  // Offset 0x1FF0000 (32 MB − 64 KB) maps to SHM fake-vmem data that is
  // not touched during early IPL boot, so no guest code/data is corrupted.
  static constexpr u32 TRAMP_OFFSET = 0x1FF0000;
  u8* nce_tramp_base = reinterpret_cast<u8*>(
      static_cast<uintptr_t>(0x80000000ULL));
  u8* nce_tramp_dest = nce_tramp_base + TRAMP_OFFSET;
  void* nce_alias_ptr = nce_tramp_dest;  // same address — virtual == host

  // Scan guest code for dcbz and P0 instructions (patch them with trap).
  ScanDCBZ();
  ScanP0();

  // Debug markers via file (avoids stderr pipe blocking issues)
  int dfd = open("/home/link/nce_debug.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (dfd >= 0) { ::write(dfd, "AV\n", 3); ::close(dfd); }

  while ([&cpu]()
         {
           // Full memory barrier (sync on PPC64) — m_state is written by the GUI
           // thread without any barrier or atomic, and PPC64 has a weak memory
           // model.  Without this barrier the load of m_state can observe a stale
           // value (Running) indefinitely after the GUI thread set it to PowerDown.
           __sync_synchronize();
           return cpu.GetState();
         }() == CPU::State::Running)
  {
    core_timing.Advance();

    // Volatile load: the ALRM signal handler modifies downcount in memory
    // asynchronously.  __atomic_load_n with __ATOMIC_RELAXED does NOT prevent
    // GCC on PPC64 from hoisting the load before the loop (the compiler
    // caches the value in a register, so the signal handler's decrements
    // become invisible).  volatile forces a real lwz on every iteration.
    while (*(static_cast<volatile const int*>(&m_ppc_state.downcount)) > 0)
    {
      // Mark inner loop entry (truncate to show latest state)
      int il_fd = open("/home/link/nce_debug.log", O_WRONLY | O_APPEND, 0644);
      if (il_fd >= 0) { ::write(il_fd, "IL\n", 3); ::close(il_fd); }

      // Provide a temporary stack for boot code that hasn't set up its own yet
      // (gpr[1] stays at 0 until the IPL initializes it).  Without this, every
      // stwu faults at address 0xFFFFFFF0 (r1=0 + negative offset), causing an
      // infinite loop through ExitNCEFromSignal → same PC re-entry.
      if (m_ppc_state.gpr[1] == 0)
        m_ppc_state.gpr[1] = 0x80100000;

      // Copy trampoline into NCE mapping (fresh each iteration in case the
      // guest overwrote the guest RAM at this offset).  Write directly to
      // the NCE alias address (0x80000000+) — that's where we execute from,
      // and it has RWX permissions.  GetRAM() points to a different physical
      // mapping and cannot be used here.
      std::memcpy(nce_tramp_dest, reinterpret_cast<const void*>(&JitPPC64EnterGuest),
                  tramp_size);
      __builtin___clear_cache(nce_tramp_dest, nce_tramp_dest + tramp_size);

      auto nce_enter = reinterpret_cast<
          void (*)(const GuestRegs*, NativeContext*)>(nce_alias_ptr);

      GuestRegs guest_regs;
      FillGuestRegsForEntry(guest_regs);

      // Check whether the PC falls inside an NCE-mapped range.
      // K2 (0x70000000-0x7FFFFFFF) physical offset = EA & 0x0FFFFFFF.
      // K1 cached (0x80000000-0xBFFFFFFF) and K1 uncached (0xC0000000-0xFFFFFFFF)
      // both use EA & 0x3FFFFFFF for the physical offset.
      {
        const u32 pc = guest_regs.pc;
        const u32 ram_size = m_system.GetMemory().GetRamSizeReal();
        const u32 exram_size = m_system.GetMemory().GetExRamSizeReal();
        u32 pc_block = pc >> 28;
        bool in_nce_range = false;
        if (pc_block == 0x7)
          in_nce_range = (pc & 0x0FFFFFFFU) < ram_size;                // K2 cached
        else if (pc_block == 0x8)
          in_nce_range = (pc & 0x0FFFFFFFU) < ram_size;                // K1 block 0 = RAM
        else if (pc_block == 0x9)
          in_nce_range = exram_size > 0 && (pc & 0x0FFFFFFFU) < exram_size;  // K1 block 1 = EXRAM
        else if (pc_block == 0xB)
          in_nce_range = (pc & 0x0FFFFFFFU) < ram_size;                // K2 uncached
        else if (pc_block == 0xC)
          in_nce_range = (pc & 0x0FFFFFFFU) < ram_size;                // K1 uncached block 0 = RAM
        else if (pc_block == 0xD)
          in_nce_range = exram_size > 0 && (pc & 0x0FFFFFFFU) < exram_size;  // K1 uncached block 1 = EXRAM
        // Blacklist IPL exception vector code (0x81200200-0x81200500) from NCE.
        // The DSI/ISI/TLBMISS handlers at these addresses run with MSR[DR]=0/IR=0
        // (real addressing mode), but the real PPC970 always has DR=1/IR=1 — we
        // never change it.  Every load/store in the handler accesses physical
        // addresses that aren't in the NCE page table, causing a SIGSEGV loop.
        // The interpreter handles these correctly because it uses m_guest.msr.
        if ((pc & 0xFFFFF700U) == 0x81200200U)
          in_nce_range = false;
        if (!in_nce_range)
        {
          // Fall back to the interpreter for one instruction.
          // We CANNOT inject ISI via IPL vector code because the real
          // PPC970's SRR0/SRR1 are NOT set by m_ppc_state.spr[] —
          // mfspr from the IPL handler reads the real (wrong) SPR value.
          *reinterpret_cast<NativeContext**>(NCE_SLOT_ADDR) = nullptr;
          UGeckoInstruction inst;
          inst.hex = 0;
          // Only read the instruction if PC is in a valid address range
          // (avoids Unknown Pointer crash from Memory::Read_U32).
          u32 masked = m_ppc_state.pc & 0x3FFFFFFF;
          bool valid_inst = masked < ram_size;
          if (!valid_inst && exram_size > 0)
            valid_inst = ((masked >> 28) == 0x1) && ((masked & 0x0FFFFFFF) < exram_size);
          if (valid_inst)
            inst.hex = m_system.GetMemory().Read_U32(m_ppc_state.pc);
          m_ppc_state.npc = m_ppc_state.pc + 4;
          FallBackToInterpreter(inst);
          m_ppc_state.pc = m_ppc_state.npc;
          continue;
        }
      }
      // P0 instruction check — these silently produce wrong results on
      // PPC970 (psq_l/st loads/stores 16 bytes instead of 8, mftb/mfspr
      // PVR/TL/TU return real PPC970 values).  dcbz is handled separately
      // via PatchAllDCBZ before NCE entry (patched to illegal → SIGILL).
      //
      // NOTE: We check ONLY the current PC instruction, NOT the whole page.
      // Scanning the whole page (64 KB on this host) causes false positives
      // because data regions contain arbitrary bytes matching P0 opcodes.
      // If the guest branches to a P0 instruction during NCE, it will
      // execute natively with wrong semantics — but this is rare enough
      // that it's acceptable for now.  Future work: patch all P0 instrs
      // in the page before NCE entry (like PatchAllDCBZ).
      {
        auto& memory = m_system.GetMemory();
        const u32 pc = guest_regs.pc;
        const u32 ram_size_r = memory.GetRamSizeReal();
        // Map guest PC to a physical RAM offset
        u32 pc_b = pc >> 28;
        u32 phys_offset;
        if (pc_b == 0x7 || pc_b == 0xB)
          phys_offset = pc & 0x0FFFFFFFU;   // K2
        else
          phys_offset = pc & 0x3FFFFFFFU;   // K1
        u32 instr_at_pc = 0;
        if (phys_offset + 4 <= ram_size_r)
          std::memcpy(&instr_at_pc, memory.GetRAM() + phys_offset, sizeof(instr_at_pc));
        if (IsP0Instruction(instr_at_pc))
        {
          *reinterpret_cast<NativeContext**>(NCE_SLOT_ADDR) = nullptr;
          UGeckoInstruction gi;
          gi.hex = instr_at_pc;
          m_ppc_state.npc = m_ppc_state.pc + 4;
          FallBackToInterpreter(gi);
          m_ppc_state.pc = m_ppc_state.npc;
          continue;
        }
      }
      *reinterpret_cast<NativeContext**>(NCE_SLOT_ADDR) = &m_native_ctx;

      // Double-check that the PC is in an NCE-mapped range (redundant with the
      // guard above, but kept for safety).  If the PC somehow changed between
      // the guard and here, fall back to the interpreter for one instruction.
      {
        const u32 pc = guest_regs.pc;
        const u32 ram_size = m_system.GetMemory().GetRamSizeReal();
        const u32 exram_size = m_system.GetMemory().GetExRamSizeReal();
        u32 pc_block = pc >> 28;
        bool in_nce_range = false;
        if (pc_block == 0x7)
          in_nce_range = (pc & 0x0FFFFFFFU) < ram_size;                // K2 cached
        else if (pc_block == 0x8)
          in_nce_range = (pc & 0x0FFFFFFFU) < ram_size;                // K1 block 0 = RAM
        else if (pc_block == 0x9)
          in_nce_range = exram_size > 0 && (pc & 0x0FFFFFFFU) < exram_size;  // K1 block 1 = EXRAM
        else if (pc_block == 0xB)
          in_nce_range = (pc & 0x0FFFFFFFU) < ram_size;                // K2 uncached
        else if (pc_block == 0xC)
          in_nce_range = (pc & 0x0FFFFFFFU) < ram_size;                // K1 uncached block 0 = RAM
        else if (pc_block == 0xD)
          in_nce_range = exram_size > 0 && (pc & 0x0FFFFFFFU) < exram_size;  // K1 uncached block 1 = EXRAM
        // Blacklist IPL exception vector code from NCE (same reason as above).
        if ((pc & 0xFFFFF700U) == 0x81200200U)
          in_nce_range = false;
        if (!in_nce_range)
        {
      int cycles = m_system.GetInterpreter().SingleStepInner();
          m_ppc_state.downcount -= cycles;
          *reinterpret_cast<NativeContext**>(NCE_SLOT_ADDR) = nullptr;
          continue;
        }
      }

      // Use a stack-local NativeContext to avoid large-offset addressing
      // (&m_native_ctx is 525KB from this — exceeds addi range, compiler
      // may compute it incorrectly across function calls).  Stack-local
      // is r1+small_offset, always correct.
      NativeContext local_ctx;
      local_ctx = m_native_ctx;
      // Clear the NCE slot so ALRM during PatchAllDCBZ or trampoline
      // setup returns early (no valid guest state to save yet).  The
      // trampoline itself writes the slot address right before bctr,
      // closing the race between the old C++ set-slot and bctr.
      *reinterpret_cast<NativeContext**>(NCE_SLOT_ADDR) = nullptr;

      // Clear stage marker before trampoline entry
      u32 stage_zero = 0;
      std::memcpy(reinterpret_cast<void*>(0x100003FECULL), &stage_zero, sizeof(stage_zero));

      // Mark: about to enter NCE (confirms P0 check passed and we reached here)
      {
        int en_fd = open("/home/link/nce_debug.log", O_WRONLY | O_APPEND, 0644);
        if (en_fd >= 0) { ::write(en_fd, "EN\n", 3); ::close(en_fd); }
      }

      PatchAllDCBZ();
      PatchAllP0();
      nce_enter(&guest_regs, &local_ctx);
      UnpatchAllP0();
      UnpatchAllDCBZ();

      // Mark nce_enter return
      {
        int ea_fd = open("/home/link/nce_debug.log", O_WRONLY | O_APPEND, 0644);
        if (ea_fd >= 0) { ::write(ea_fd, "EA\n", 3); ::close(ea_fd); }
      }

      // First null out the NCE slot so any SIGALRM between now and the next
      // trampoline entry sees a null pointer and returns early (instead of
      // trying to exit the loop from C++ code and corrupting guest state).
      *reinterpret_cast<NativeContext**>(NCE_SLOT_ADDR) = nullptr;

      m_native_ctx = local_ctx;
      // Sentinel prevents the ALRM handler from exiting during the setup for
      // the next iteration (before the trampoline overwrites return_addr).
      m_native_ctx.return_addr = 0xFFFFFFFFFFFFFFFFULL;

      // Diagnostic: read trampoline stage marker at 0x100003FEC
      // stage=1 entered, stage=2 host saved, stage=3 about to bctr
      u32 diag_val = 0;
      std::memcpy(&diag_val, reinterpret_cast<const void*>(0x100003FECULL), sizeof(diag_val));

      // Write inner-loop exit marker to file (avoids stderr pipe issues)
      int ie_fd = open("/home/link/nce_debug.log", O_WRONLY | O_APPEND, 0644);
      if (ie_fd >= 0)
      {
        u32 instr_at_pc = 0;
        if (const u8* ptr = m_system.GetMemory().GetPointerForRange(m_ppc_state.pc, 4))
          std::memcpy(&instr_at_pc, ptr, sizeof(instr_at_pc));
        char ie_buf[128];
        int ie_len = std::snprintf(ie_buf, sizeof(ie_buf),
                                   "IE: pc=%08x instr=%08x dc=%d diag=%08x\n",
                                   m_ppc_state.pc, instr_at_pc,
                                   m_ppc_state.downcount, diag_val);
        ::write(ie_fd, ie_buf, static_cast<size_t>(ie_len));
        ::close(ie_fd);
      }

      // Full memory barrier (sync on PPC64) — m_state is written by the GUI
      // thread without any barrier or atomic, and PPC64 has a weak memory
      // model.  Without this barrier the load of m_state can observe a stale
      // value (Running) indefinitely after the GUI thread set it to PowerDown.
      __sync_synchronize();
      // Check if CPU state changed while NCE was running (e.g., user clicked
      // Stop).  Without this check, the inner loop only exits when downcount
      // falls to zero, which can take up to 2ms (the ALRM interval).
      if (cpu.GetState() != CPU::State::Running)
        break;
      // Yield after every NCE slice so the GUI/main thread can run (set CPU
      // state to PowerDown).  On single-core systems the CPU thread monopolises
      // the core; without yield() the GUI thread never gets scheduled to write
      // m_state, and the outer loop keeps re-entering NCE despite Stop().
      ::sched_yield();
    }
  }

  // Outer-loop exit marker
  int ex_fd = open("/home/link/nce_debug.log", O_WRONLY | O_APPEND, 0644);
  if (ex_fd >= 0) { ::write(ex_fd, "EX\n", 3); ::close(ex_fd); }
  StopNativeTimer();
  UnpatchAllP0();
  UnpatchAllDCBZ();
}

void JitPPC64::SingleStep()
{
  InstallSignalHandlers();
  auto& interpreter = m_system.GetInterpreter();
  auto& core_timing = m_system.GetCoreTiming();
  core_timing.Advance();
  int cycles = interpreter.SingleStepInner();
  m_ppc_state.downcount -= cycles;
}

void JitPPC64::FillGuestRegsForEntry(GuestRegs& regs)
{
  auto& ppc_state = m_ppc_state;

  // If r1 is zero the IPL's first stw/stwu will write to unmapped host memory.
  // On real hardware the boot ROM initialises the stack pointer; Dolphin's
  // interpreter handles r1=0 because it emulates all memory accesses, but NCE
  // needs r1 to be a valid mapped address.
  if (ppc_state.gpr[1] == 0)
    ppc_state.gpr[1] = 0x80000000 | (m_system.GetMemory().GetRamSizeReal() - 256);

  std::memcpy(regs.gpr, ppc_state.gpr, sizeof(regs.gpr));
  regs.pc = ppc_state.pc;
  regs.cr = ppc_state.cr.Get();
  regs.lr = ppc_state.spr[SPR_LR];
  regs.ctr = ppc_state.spr[SPR_CTR];
  u32 xer = 0;
  xer |= (static_cast<u32>(ppc_state.xer_stringctrl) & 0xFF) << 16;
  xer |= (static_cast<u32>(ppc_state.xer_ca) & 1) << 29;
  xer |= (static_cast<u32>(ppc_state.xer_so_ov) & 1) << 30;       // OV
  xer |= ((static_cast<u32>(ppc_state.xer_so_ov) >> 1) & 1) << 31; // SO
  regs.xer = xer;
}

void JitPPC64::SaveGuestRegsFromContext(void* uctx)
{
  auto* ctx = static_cast<ucontext_t*>(uctx);
  auto& ppc_state = m_ppc_state;

  for (int i = 0; i < 32; i++)
    ppc_state.gpr[i] = static_cast<u32>(ctx->uc_mcontext.regs->gpr[i] & 0xFFFFFFFF);

  ppc_state.pc = static_cast<u32>(ctx->uc_mcontext.regs->nip & 0xFFFFFFFF);
  ppc_state.spr[SPR_LR] = static_cast<u32>(ctx->uc_mcontext.regs->link & 0xFFFFFFFF);

  // The trampoline overwrites CTR with the guest PC (for the bctr jump).
  // If the guest did NOT modify CTR during native execution, the ucontext
  // CTR equals the overwrite value (guest PC) instead of the true guest CTR.
  // Detect this case and use the saved original guest CTR from NativeContext.
  u32 ctr_from_uctx = static_cast<u32>(ctx->uc_mcontext.regs->ctr & 0xFFFFFFFF);
  auto* slot = *reinterpret_cast<NativeContext const**>(NCE_SLOT_ADDR);
  if (slot && slot->overwrite_ctr == ctr_from_uctx && slot->overwrite_ctr != 0)
  {
    // Guest didn't modify CTR — restore the original guest CTR
    ppc_state.spr[SPR_CTR] = slot->guest_ctr;
  }
  else
  {
    // Guest modified CTR (or slot unavailable) — use the ucontext value
    ppc_state.spr[SPR_CTR] = ctr_from_uctx;
  }

  u64 xer = ctx->uc_mcontext.regs->xer;
  ppc_state.xer_ca = (xer >> 29) & 1;
  ppc_state.xer_so_ov = ((xer >> 31) & 1) << 1 | ((xer >> 30) & 1);
  ppc_state.xer_stringctrl = (xer >> 16) & 0xFF;
  ppc_state.cr.Set(static_cast<u32>(ctx->uc_mcontext.regs->ccr & 0xFFFFFFFF));
}

void JitPPC64::RestoreHostRegsInContext(void* uctx)
{
  auto* ctx = static_cast<ucontext_t*>(uctx);

  // Read the host context saved by the trampoline at nce_enter entry.
  // The trampoline saves the exact non-volatile register values (r14-r31,
  // r1, r2, r13, CR, and LR-as-return_addr) as they were at the function-
  // call boundary into the stack-local NativeContext whose address is stored
  // at the fixed NCE slot address (NCE_SLOT_ADDR, above 4 GB).  We restore those registers here so
  // that sigreturn resumes the C++ loop with the same register file the
  // compiler had at the call site — this includes any local variables that
  // the compiler assigned to non-volatile registers, such as 'this' (r31).
  auto* saved = *reinterpret_cast<NativeContext const**>(NCE_SLOT_ADDR);
  if (!saved)
    saved = &m_native_ctx;  // fallback — should not happen after loop starts

  ctx->uc_mcontext.regs->gpr[1] = saved->host_r1;
  ctx->uc_mcontext.regs->gpr[2] = saved->host_r2;
  ctx->uc_mcontext.regs->gpr[13] = saved->host_r13;

  for (int i = 0; i < 18; i++)
    ctx->uc_mcontext.regs->gpr[14 + i] = saved->host_gpr14_31[i];

  ctx->uc_mcontext.regs->ccr = saved->host_cr;
  // return_addr was set by the trampoline to LR = resume address (the
  // instruction after 'bl nce_enter' in Run()).  Jumping there re-enters
  // the C++ loop at the natural continuation point.
  ctx->uc_mcontext.regs->nip = saved->return_addr;
}

// ---------------------------------------------------------------------------
// FPR save/restore for psq_l/st and ps_* paired-single emulation
// ---------------------------------------------------------------------------
//
// The real PPC970 FPRs (in the ucontext) hold the current guest FPR values
// from native execution.  m_ppc_state.ps[] has stale values from the last
// interpreter/JIT run.  Before FallBackToInterpreter for any instruction
// that reads or writes FPRs (psq_l/st, ps_* arithmetic), we must copy the
// real values into m_ppc_state so the interpreter sees correct operands.
//
// Gekko packs two 32-bit floats into each 64-bit FPR:
//   bits  0-31 = PS0 (float, element 0)
//   bits 32-63 = PS1 (float, element 1)
//
// Dolphin's PairedSingle struct stores both halves as doubles (64-bit).
// The conversion at the ucontext boundary takes the packed 64-bit FPR,
// splits it into two floats, and promotes each to double for the struct.

void JitPPC64::SaveFPRsFromContext(void* uctx)
{
  auto* ctx = static_cast<ucontext_t*>(uctx);
  for (int i = 0; i < 32; i++)
  {
    double fpr_double = ctx->uc_mcontext.fp_regs[i];
    u64 raw;
    std::memcpy(&raw, &fpr_double, sizeof(raw));

    // Split packed Gekko format: lower 32 bits = PS0, upper 32 bits = PS1
    float ps0f, ps1f;
    std::memcpy(&ps0f, &raw, sizeof(ps0f));
    std::memcpy(&ps1f, reinterpret_cast<const char*>(&raw) + 4, sizeof(ps1f));

    m_ppc_state.ps[i].SetBoth(static_cast<double>(ps0f),
                              static_cast<double>(ps1f));
  }
}

void JitPPC64::RestoreFPRToContext(void* uctx, u32 fpr_index)
{
  auto* ctx = static_cast<ucontext_t*>(uctx);

  double ps0_double = m_ppc_state.ps[fpr_index].PS0AsDouble();
  double ps1_double = m_ppc_state.ps[fpr_index].PS1AsDouble();

  // Convert back to two 32-bit floats and pack into 64-bit FPR
  float ps0f = static_cast<float>(ps0_double);
  float ps1f = static_cast<float>(ps1_double);

  u64 packed;
  std::memcpy(&packed, &ps0f, sizeof(ps0f));
  std::memcpy(reinterpret_cast<char*>(&packed) + 4, &ps1f, sizeof(ps1f));

  double fpr_double;
  std::memcpy(&fpr_double, &packed, sizeof(fpr_double));
  ctx->uc_mcontext.fp_regs[fpr_index] = fpr_double;
}

// ---------------------------------------------------------------------------
// Periodic timer for downcount estimation
// ---------------------------------------------------------------------------

void JitPPC64::StartNativeTimer()
{
  struct sigevent sev = {};
  sev.sigev_notify = SIGEV_SIGNAL;
  sev.sigev_signo = SIGALRM;
  sev.sigev_value.sival_ptr = this;

  if (timer_create(CLOCK_MONOTONIC, &sev, &m_native_timer) != 0)
  {
    ERROR_LOG_FMT(POWERPC, "NCE: timer_create failed");
    return;
  }

  struct itimerspec its = {};
  its.it_value.tv_sec = 0;
  its.it_value.tv_nsec = 2000000;      // 2ms initial
  its.it_interval.tv_sec = 0;
  its.it_interval.tv_nsec = 2000000;   // 2ms period
  timer_settime(m_native_timer, 0, &its, nullptr);
}

void JitPPC64::StopNativeTimer()
{
  if (m_native_timer)
  {
    timer_delete(m_native_timer);
    m_native_timer = {};
  }
}

u64 JitPPC64::EstimateDowncount()
{
  return CYCLES_PER_BLOCK;
}

void JitPPC64::SyncGuestState()
{
  auto& ppc_state = m_ppc_state;
  auto& g = m_guest;

  g.msr = ppc_state.msr.Hex;
  g.srr0 = ppc_state.spr[SPR_SRR0];
  g.srr1 = ppc_state.spr[SPR_SRR1];
  g.hid0 = ppc_state.spr[SPR_HID0];
  g.hid1 = ppc_state.spr[SPR_HID1];
  g.hid2 = ppc_state.spr[SPR_HID2];
  g.hid4 = ppc_state.spr[SPR_HID4];
  g.l2cr = ppc_state.spr[SPR_L2CR];
  g.mmcr0 = ppc_state.spr[SPR_MMCR0];
  g.mmcr1 = ppc_state.spr[SPR_MMCR1];
  g.decrementer = ppc_state.spr[SPR_DEC];
  g.tbl = ppc_state.spr[SPR_TL];
  g.tbu = ppc_state.spr[SPR_TU];
  g.dsisr = ppc_state.spr[SPR_DSISR];
  g.dar = ppc_state.spr[SPR_DAR];

  for (int i = 0; i < 4; i++)
    g.sprg[i] = ppc_state.spr[SPR_SPRG0 + i];

  for (int i = 0; i < 8; i++)
  {
    g.ibatu[i] = ppc_state.spr[SPR_IBAT0U + i * 2];
    g.ibatl[i] = ppc_state.spr[SPR_IBAT0L + i * 2];
    g.dbatu[i] = ppc_state.spr[SPR_DBAT0U + i * 2];
    g.dbatl[i] = ppc_state.spr[SPR_DBAT0L + i * 2];
  }

  for (int i = 0; i < 16; i++)
    g.segment_regs[i] = ppc_state.sr[i];
}

// ---------------------------------------------------------------------------
// Guest memory execution permissions
// ---------------------------------------------------------------------------

void JitPPC64::MakeGuestMemoryExecutable()
{
  if (m_memory_executable)
    return;

  auto& memory = m_system.GetMemory();
  const long page_size = sysconf(_SC_PAGESIZE);

  u8* ram = memory.GetRAM();
  u32 ram_size = memory.GetRamSize();
  if (ram && ram_size > 0)
  {
    u32 aligned_size = (ram_size + page_size - 1) & ~(static_cast<u32>(page_size) - 1);
    mprotect(ram, aligned_size, PROT_READ | PROT_WRITE | PROT_EXEC);
  }

  u8* exram = memory.GetEXRAM();
  u32 exram_size = memory.GetExRamSize();
  if (exram && exram_size > 0)
  {
    u32 aligned_size = (exram_size + page_size - 1) & ~(static_cast<u32>(page_size) - 1);
    mprotect(exram, aligned_size, PROT_READ | PROT_WRITE | PROT_EXEC);
  }

  m_memory_executable = true;
}

void JitPPC64::MakeGuestMemoryNonExecutable()
{
  if (!m_memory_executable)
    return;

  auto& memory = m_system.GetMemory();
  const long page_size = sysconf(_SC_PAGESIZE);

  u8* ram = memory.GetRAM();
  u32 ram_size = memory.GetRamSize();
  if (ram && ram_size > 0)
  {
    u32 aligned_size = (ram_size + page_size - 1) & ~(static_cast<u32>(page_size) - 1);
    mprotect(ram, aligned_size, PROT_READ | PROT_WRITE);
  }

  u8* exram = memory.GetEXRAM();
  u32 exram_size = memory.GetExRamSize();
  if (exram && exram_size > 0)
  {
    u32 aligned_size = (exram_size + page_size - 1) & ~(static_cast<u32>(page_size) - 1);
    mprotect(exram, aligned_size, PROT_READ | PROT_WRITE);
  }

  m_memory_executable = false;
}

// ---------------------------------------------------------------------------
// JIT stubs
// ---------------------------------------------------------------------------

void JitPPC64::Jit(u32 em_address)
{
}

void JitPPC64::EraseSingleBlock(const JitBlock& block)
{
  m_block_cache.EraseSingleBlock(block);
}

std::vector<JitBase::MemoryStats> JitPPC64::GetMemoryStats() const
{
  return {};
}

std::size_t JitPPC64::DisassembleNearCode(const JitBlock& block, std::ostream& stream) const
{
  return 0;
}

std::size_t JitPPC64::DisassembleFarCode(const JitBlock& block, std::ostream& stream) const
{
  return 0;
}

void JitPPC64::FallBackToInterpreter(UGeckoInstruction inst)
{
  Interpreter::Instruction instr = Interpreter::GetInterpreterOp(inst);
  auto& interpreter = m_system.GetInterpreter();
  if (instr)
    instr(interpreter, inst);
  else
    UnknownInstruction(inst);
}

void JitPPC64::DoNothing(UGeckoInstruction inst)
{
}

void JitPPC64::UnknownInstruction(UGeckoInstruction inst)
{
  ERROR_LOG_FMT(POWERPC, "NCE: unknown instruction {:08x} at PC={:08x}", inst.hex, m_ppc_state.pc);
}

// ---------------------------------------------------------------------------
// Signal handler installation
// ---------------------------------------------------------------------------

void JitPPC64::InstallSignalHandlers()
{
  if (m_signals_installed)
    return;

  s_active_instance = this;

  static constexpr size_t ALT_STACK_SIZE = 262144;
  m_alt_stack.ss_sp = std::malloc(ALT_STACK_SIZE);
  if (!m_alt_stack.ss_sp)
  {
    ERROR_LOG_FMT(POWERPC, "NCE: failed to allocate signal alt stack");
    return;
  }
  m_alt_stack.ss_size = ALT_STACK_SIZE;
  m_alt_stack.ss_flags = 0;

  if (sigaltstack(&m_alt_stack, nullptr) != 0)
  {
    ERROR_LOG_FMT(POWERPC, "NCE: sigaltstack failed");
    std::free(m_alt_stack.ss_sp);
    m_alt_stack.ss_sp = nullptr;
    return;
  }

  // SIGSEGV uses the asm entry that restores host r2/r13 and calls
  // HandleSIGSEGV (MMIO emulation, slowmem fallback, DSI injection).
  // SIGALRM is NOT blocked — ALRM must fire during SIGSEGV to decrement
  // downcount while NCE handles MMIO-heavy guest code.  Without this,
  // downcount never reaches zero during extended MMIO sequences, preventing
  // NCE from exiting back to the Run() loop (infinite MMIO loop).
  struct sigaction sa = {};
  sigemptyset(&sa.sa_mask);
  sigaddset(&sa.sa_mask, SIGILL);
  sigaddset(&sa.sa_mask, SIGTRAP);
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sa.sa_sigaction = reinterpret_cast<decltype(sa.sa_sigaction)>(nce_segv_entry);
  sigaction(SIGSEGV, &sa, &m_old_sigsegv);

  // Safety net for ILL/TRAP — trivial ucontext-modifying handler (no r2/r13
  // needed).  This catches any double faults from the SIGALRM/SIGSEGV asm
  // entries without crashing the process.
  sigemptyset(&sa.sa_mask);
  sigaddset(&sa.sa_mask, SIGALRM);
  sigaddset(&sa.sa_mask, SIGSEGV);
  sigaddset(&sa.sa_mask, SIGTRAP);
  sa.sa_sigaction = reinterpret_cast<decltype(sa.sa_sigaction)>(nce_ill_entry);
  sigaction(SIGILL, &sa, &m_old_sigill);

  // SIGBUS (unaligned access, SHM page beyond file end) — route through
  // HandleSIGSEGV which falls back to the interpreter for guest-code faults.
  sigemptyset(&sa.sa_mask);
  sigaddset(&sa.sa_mask, SIGILL);
  sigaddset(&sa.sa_mask, SIGTRAP);
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sa.sa_sigaction = reinterpret_cast<decltype(sa.sa_sigaction)>(nce_segv_entry);
  sigaction(SIGBUS, &sa, &m_old_sigbus);

  sigemptyset(&sa.sa_mask);
  sigaddset(&sa.sa_mask, SIGBUS);
  sigaddset(&sa.sa_mask, SIGALRM);
  sigaddset(&sa.sa_mask, SIGSEGV);
  sigaddset(&sa.sa_mask, SIGILL);
  sa.sa_sigaction = reinterpret_cast<decltype(sa.sa_sigaction)>(nce_safety_handler);
  sigaction(SIGTRAP, &sa, &m_old_sigtrap);

  // SIGALRM uses the asm entry that restores host r2/r13 from nce_ctx_slot
  // and calls nce_bridge_alrm → HandleSIGALRM.
  sigemptyset(&sa.sa_mask);
  sigaddset(&sa.sa_mask, SIGALRM);  // prevent nested ALRM (would corrupt ucontext in save/restore)
  sigaddset(&sa.sa_mask, SIGSEGV);  // prevent SEGV from interrupting ALRM handler
  sigaddset(&sa.sa_mask, SIGILL);
  sigaddset(&sa.sa_mask, SIGTRAP);
  sa.sa_sigaction = reinterpret_cast<decltype(sa.sa_sigaction)>(nce_alrm_entry);
  sigaction(SIGALRM, &sa, &m_old_sigalrm);

  m_signals_installed = true;
}

void JitPPC64::RemoveSignalHandlers()
{
  if (!m_signals_installed)
    return;

  StopNativeTimer();

  sigaction(SIGSEGV, &m_old_sigsegv, nullptr);
  sigaction(SIGILL, &m_old_sigill, nullptr);
  sigaction(SIGBUS, &m_old_sigbus, nullptr);
  sigaction(SIGALRM, &m_old_sigalrm, nullptr);
  sigaction(SIGTRAP, &m_old_sigtrap, nullptr);

  if (m_alt_stack.ss_sp)
  {
    std::free(m_alt_stack.ss_sp);
    m_alt_stack.ss_sp = nullptr;
  }

  s_active_instance = nullptr;
  m_signals_installed = false;
}

// (SIGTRAP handler is now the file-scope asm entry nce_sigtrap_entry)

void JitPPC64::HandleSIGTRAP(int sig, siginfo_t* info, void* uctx)
{
  // R2/R13 are already host values — restored by the asm entry point.
  auto* ctx = static_cast<ucontext_t*>(uctx);
  u32 pc_val = static_cast<u32>(ctx->uc_mcontext.regs->nip & 0xFFFFFFFF);

  static constexpr const char trap_msg[] = "NCE: SIGTRAP\n";
  ::write(STDERR_FILENO, trap_msg, sizeof(trap_msg) - 1);

  // Advance past the trap instruction (4 bytes)
  ctx->uc_mcontext.regs->nip = u64(pc_val) + 4;
}

// ---------------------------------------------------------------------------
// SIGSEGV handler — MMIO access and slowmem fallback
// ---------------------------------------------------------------------------

void JitPPC64::HandleSIGSEGV(int sig, siginfo_t* info, void* uctx)
{
  // R2/R13 are already host values — restored by the asm entry point.
  auto* ctx = static_cast<ucontext_t*>(uctx);
  u64 pc_val_full = ctx->uc_mcontext.regs->nip;

  // Only handle faults in guest code range.  SIGSEGV from event callbacks,
  // core_timing, or other non-NCE code must NOT be handled here — treating
  // them as NCE faults would corrupt guest state (via ExitNCEFromSignal)
  // and create an infinite loop.
  // NOTE: Use full 64-bit NIP for the range check.  The PIE C++ binary on
  // PPC64 loads at 0x7fffXXXX0000+; masking to 32 bits would make these
  // addresses fall inside the guest range, causing false-positive exits.
  // Guest code is always at 32-bit addresses (zero-extended to 64 bits).
  bool in_guest_code =
      (pc_val_full >= 0x70000000ULL && pc_val_full < 0x82000000ULL) ||   // K2 + K1 cached
      (pc_val_full >= 0xB0000000ULL && pc_val_full < 0xC2000000ULL);      // K2 unc + K1 unc
  u32 pc_val = static_cast<u32>(pc_val_full & 0xFFFFFFFF);
  if (!in_guest_code)
  {
    // Skip the faulting instruction to avoid infinite SIGSEGV re-delivery.
    ctx->uc_mcontext.regs->nip = pc_val_full + 4;
    return;
  }

  u32 fault_addr = ctx->uc_mcontext.regs->dar;
  u32 dsisr_val = ctx->uc_mcontext.regs->dsisr;

  // Read instruction from emulated RAM.  GetPointerForRange uses EA & 0x3FFFFFFF
  // which is wrong for K2 (EA & 0x0FFFFFFF).  Compute the physical offset manually.
  u32 instr = 0;
  {
    u32 phys;
    if ((pc_val >= 0x70000000U && pc_val < 0x80000000U) ||  // K2 cached
        (pc_val >= 0xB0000000U && pc_val < 0xC0000000U))    // K2 uncached
      phys = pc_val & 0x0FFFFFFFU;
    else
      phys = pc_val & 0x3FFFFFFFU;  // K1 cached, K1 uncached, or physical
    const u32 ram_size = m_system.GetMemory().GetRamSizeReal();
    if (phys < ram_size)
      std::memcpy(&instr, m_system.GetMemory().GetRAM() + phys, sizeof(instr));
  }
  u32 opcd = (instr >> 26) & 0x3F;

  // Instruction-fetch fault — the guest tried to execute at an unmapped or
  // non-executable address.  We CANNOT inject ISI via IPL vector code because
  // the real PPC970's SRR0/SRR1 are NOT set by m_guest.srr0/srr1 — mfspr from
  // the IPL handler reads the real (wrong) SPR value.  Instead, exit NCE and
  // let the Run() loop handle it via interpreter fallback.
  if (fault_addr == pc_val)
  {
    SaveGuestRegsFromContext(uctx);
    ExitNCEFromSignal(uctx, pc_val, true);
    return;
  }

  // Translate virtual fault address to physical for MMIO detection.
  // The DAR contains the virtual (alias) address that caused the fault, but
  // MMIO registers, RAM, and EXRAM all live at physical addresses below 0x80000000.
  u32 phys_fault;
  if ((fault_addr >> 28) == 0xB)   // K2 uncached
    phys_fault = fault_addr & 0x0FFFFFFF;
  else if ((fault_addr >> 28) == 0x7)  // K2 cached (code fetch, not load/store)
    phys_fault = fault_addr & 0x0FFFFFFF;
  else
    phys_fault = fault_addr & 0x3FFFFFFF;  // K1 cached, K1 uncached, or physical

  // Check if this is an MMIO access (use physical address)
  if (MMIO::IsMMIOAddress(phys_fault, m_system.IsWii()))
  {
    bool is_read_mmio = (dsisr_val & 0x40000000) != 0;

    u32 reg_src = (instr >> 21) & 0x1F;
    u32 reg_dest = (instr >> 21) & 0x1F;
    u32 val = 0;
    bool is_store = false;
    int width = 4;

    // Log MMIO access
    {
      int mf = open("/home/link/nce_debug.log", O_WRONLY | O_APPEND, 0644);
      if (mf >= 0)
      {
        char mb[128];
        int ml = std::snprintf(mb, sizeof(mb), "MIO: %s phys=%08x pc=%08x opcd=%u\n",
                               is_read_mmio ? "RD" : "WR", phys_fault, pc_val, opcd);
        ::write(mf, mb, static_cast<size_t>(ml));
        ::close(mf);
      }
    }

    switch (opcd)
    {
    // ---- D-form loads (non-update) ----
    case 32: width = 4; break;   // lwz
    case 34: width = 1; break;   // lbz
    case 40: width = 2; break;   // lhz
    case 42: width = 2; break;   // lha
    case 36:                      // stw
      is_store = true;
      val = static_cast<u32>(ctx->uc_mcontext.regs->gpr[reg_src]);
      width = 4;
      break;
    case 38:                      // stb
      is_store = true;
      val = static_cast<u32>(ctx->uc_mcontext.regs->gpr[reg_src] & 0xFF);
      width = 1;
      break;
    case 44:                      // sth
      is_store = true;
      val = static_cast<u32>(ctx->uc_mcontext.regs->gpr[reg_src] & 0xFFFF);
      width = 2;
      break;

    // ---- D-form loads with update (read + RA write-back) ----
    case 33: // lwzu
    {
      u32 ra = (instr >> 16) & 0x1F;
      u32 v = MMIORead(phys_fault, 4);
      ctx->uc_mcontext.regs->gpr[reg_dest] = static_cast<u64>(v);
      if (ra != 0 && ra != reg_dest)
        ctx->uc_mcontext.regs->gpr[ra] = fault_addr;
      ctx->uc_mcontext.regs->nip = u64(pc_val) + 4;
      return;
    }
    case 35: // lbzu
    {
      u32 ra = (instr >> 16) & 0x1F;
      u32 v = MMIORead(phys_fault, 1);
      ctx->uc_mcontext.regs->gpr[reg_dest] = static_cast<u64>(v);
      if (ra != 0 && ra != reg_dest)
        ctx->uc_mcontext.regs->gpr[ra] = fault_addr;
      ctx->uc_mcontext.regs->nip = u64(pc_val) + 4;
      return;
    }
    case 41: // lhzu
    {
      u32 ra = (instr >> 16) & 0x1F;
      u32 v = MMIORead(phys_fault, 2);
      ctx->uc_mcontext.regs->gpr[reg_dest] = static_cast<u64>(v);
      if (ra != 0 && ra != reg_dest)
        ctx->uc_mcontext.regs->gpr[ra] = fault_addr;
      ctx->uc_mcontext.regs->nip = u64(pc_val) + 4;
      return;
    }
    case 43: // lhau
    {
      u32 ra = (instr >> 16) & 0x1F;
      u32 v = MMIORead(phys_fault, 2);
      ctx->uc_mcontext.regs->gpr[reg_dest] = static_cast<u64>(s16(v & 0xFFFF));
      if (ra != 0 && ra != reg_dest)
        ctx->uc_mcontext.regs->gpr[ra] = fault_addr;
      ctx->uc_mcontext.regs->nip = u64(pc_val) + 4;
      return;
    }

    // ---- D-form stores with update (write + RA write-back) ----
    case 37: // stwu
    {
      u32 ra = (instr >> 16) & 0x1F;
      u32 v = static_cast<u32>(ctx->uc_mcontext.regs->gpr[reg_src]);
      MMIOWrite(phys_fault, v, 4);
      if (ra != 0)
        ctx->uc_mcontext.regs->gpr[ra] = fault_addr;
      ctx->uc_mcontext.regs->nip = u64(pc_val) + 4;
      return;
    }
    case 39: // stbu
    {
      u32 ra = (instr >> 16) & 0x1F;
      u32 v = static_cast<u32>(ctx->uc_mcontext.regs->gpr[reg_src] & 0xFF);
      MMIOWrite(phys_fault, v, 1);
      if (ra != 0)
        ctx->uc_mcontext.regs->gpr[ra] = fault_addr;
      ctx->uc_mcontext.regs->nip = u64(pc_val) + 4;
      return;
    }
    case 45: // sthu
    {
      u32 ra = (instr >> 16) & 0x1F;
      u32 v = static_cast<u32>(ctx->uc_mcontext.regs->gpr[reg_src] & 0xFFFF);
      MMIOWrite(phys_fault, v, 2);
      if (ra != 0)
        ctx->uc_mcontext.regs->gpr[ra] = fault_addr;
      ctx->uc_mcontext.regs->nip = u64(pc_val) + 4;
      return;
    }

    // ---- Load/store multiple (lmw/stmw) ----
    case 46: // lmw
    {
      u32 ra = (instr >> 16) & 0x1F;
      s32 d = static_cast<s16>(instr & 0xFFFF);
      u32 ea = (ra == 0) ? 0 : static_cast<u32>(ctx->uc_mcontext.regs->gpr[ra]) + d;
      u32 phys_ea = ea & 0x3FFFFFFF;
      u32 r = reg_dest;
      while (r <= 31)
      {
        ctx->uc_mcontext.regs->gpr[r] = MMIORead(phys_ea, 4);
        phys_ea += 4;
        r++;
      }
      ctx->uc_mcontext.regs->nip = u64(pc_val) + 4;
      return;
    }
    case 47: // stmw
    {
      u32 ra = (instr >> 16) & 0x1F;
      s32 d = static_cast<s16>(instr & 0xFFFF);
      u32 ea = (ra == 0) ? 0 : static_cast<u32>(ctx->uc_mcontext.regs->gpr[ra]) + d;
      u32 phys_ea = ea & 0x3FFFFFFF;
      u32 r = reg_src;
      while (r <= 31)
      {
        u32 v = static_cast<u32>(ctx->uc_mcontext.regs->gpr[r]);
        MMIOWrite(phys_ea, v, 4);
        phys_ea += 4;
        r++;
      }
      ctx->uc_mcontext.regs->nip = u64(pc_val) + 4;
      return;
    }

    case 31: // X-form loads/stores
    {
      u32 ra_x = (instr >> 16) & 0x1F;
      u32 rb = (instr >> 11) & 0x1F;
      u32 base = (ra_x == 0) ? 0 : static_cast<u32>(ctx->uc_mcontext.regs->gpr[ra_x]);
      u32 index = static_cast<u32>(ctx->uc_mcontext.regs->gpr[rb]);
      u32 xo = (instr >> 1) & 0x3FF;
      u32 ea = base + index;
      u32 phys_ea = ea & 0x3FFFFFFF;

      switch (xo)
      {
      case 23: // lwzx
        ctx->uc_mcontext.regs->gpr[reg_dest] = MMIORead(phys_ea, 4);
        break;
      case 55: // lwzux
      {
        u32 v = MMIORead(phys_ea, 4);
        ctx->uc_mcontext.regs->gpr[reg_dest] = v;
        if (ra_x != 0)
          ctx->uc_mcontext.regs->gpr[ra_x] = ea;
        break;
      }
      case 87: // lbzx
        ctx->uc_mcontext.regs->gpr[reg_dest] = MMIORead(phys_ea, 1);
        break;
      case 119: // lbzux
      {
        u32 v = MMIORead(phys_ea, 1);
        ctx->uc_mcontext.regs->gpr[reg_dest] = v;
        if (ra_x != 0)
          ctx->uc_mcontext.regs->gpr[ra_x] = ea;
        break;
      }
      case 279: // lhzx
        ctx->uc_mcontext.regs->gpr[reg_dest] = MMIORead(phys_ea, 2);
        break;
      case 311: // lhzux
      {
        u32 v = MMIORead(phys_ea, 2);
        ctx->uc_mcontext.regs->gpr[reg_dest] = v;
        if (ra_x != 0)
          ctx->uc_mcontext.regs->gpr[ra_x] = ea;
        break;
      }
      case 343: // lhax (sign-extended)
      {
        u32 v = MMIORead(phys_ea, 2);
        ctx->uc_mcontext.regs->gpr[reg_dest] = static_cast<u64>(s16(v & 0xFFFF));
        break;
      }
      case 375: // lhaux
      {
        u32 v = MMIORead(phys_ea, 2);
        ctx->uc_mcontext.regs->gpr[reg_dest] = static_cast<u64>(s16(v & 0xFFFF));
        if (ra_x != 0)
          ctx->uc_mcontext.regs->gpr[ra_x] = ea;
        break;
      }
      case 151: // stwx
        MMIOWrite(phys_ea, static_cast<u32>(ctx->uc_mcontext.regs->gpr[reg_dest]), 4);
        break;
      case 183: // stwux
      {
        MMIOWrite(phys_ea, static_cast<u32>(ctx->uc_mcontext.regs->gpr[reg_dest]), 4);
        if (ra_x != 0)
          ctx->uc_mcontext.regs->gpr[ra_x] = ea;
        break;
      }
      case 215: // stbx
        MMIOWrite(phys_ea, static_cast<u32>(ctx->uc_mcontext.regs->gpr[reg_dest] & 0xFF), 1);
        break;
      case 247: // stbux
      {
        MMIOWrite(phys_ea, static_cast<u32>(ctx->uc_mcontext.regs->gpr[reg_dest] & 0xFF), 1);
        if (ra_x != 0)
          ctx->uc_mcontext.regs->gpr[ra_x] = ea;
        break;
      }
      case 407: // sthx
        MMIOWrite(phys_ea, static_cast<u32>(ctx->uc_mcontext.regs->gpr[reg_dest] & 0xFFFF), 2);
        break;
      case 439: // sthux
      {
        MMIOWrite(phys_ea, static_cast<u32>(ctx->uc_mcontext.regs->gpr[reg_dest] & 0xFFFF), 2);
        if (ra_x != 0)
          ctx->uc_mcontext.regs->gpr[ra_x] = ea;
        break;
      }
      case 534: // lwbrx (byte-reversed load word)
      {
        u32 v = MMIORead(phys_ea, 4);
        ctx->uc_mcontext.regs->gpr[reg_dest] = Common::swap32(v);
        break;
      }
      case 790: // lhbrx (byte-reversed load halfword)
      {
        u32 v = MMIORead(phys_ea, 2);
        ctx->uc_mcontext.regs->gpr[reg_dest] = Common::swap16(v & 0xFFFF);
        break;
      }
      case 662: // stwbrx (byte-reversed store word)
        MMIOWrite(phys_ea, Common::swap32(static_cast<u32>(ctx->uc_mcontext.regs->gpr[reg_dest])), 4);
        break;
      case 918: // sthbrx (byte-reversed store halfword)
        MMIOWrite(phys_ea, Common::swap16(static_cast<u32>(ctx->uc_mcontext.regs->gpr[reg_dest] & 0xFFFF)), 2);
        break;
      default:
        SaveGuestRegsFromContext(uctx);
        ExitNCEFromSignal(uctx, pc_val, true);
        return;
      }
      ctx->uc_mcontext.regs->nip = u64(pc_val) + 4;
      return;
    }
    default:
      // (MMIO with unknown opcd — signal-safe: no NOTICE_LOG_FMT here)
      SaveGuestRegsFromContext(uctx);
      ExitNCEFromSignal(uctx, pc_val, true);
      return;
    }

    if (is_store)
      MMIOWrite(phys_fault, val, width);
    else
      val = MMIORead(phys_fault, width);

    if (!is_store)
    {
      if (opcd == 42)
        ctx->uc_mcontext.regs->gpr[reg_dest] = static_cast<u64>(s16(val & 0xFFFF));
      else
        ctx->uc_mcontext.regs->gpr[reg_dest] = static_cast<u64>(val);
    }

    ctx->uc_mcontext.regs->nip = u64(pc_val) + 4;
    // Log MMIO result value (for stores: val written; for reads: val in GPR)
    if (is_store)
    {
      int mf2 = open("/home/link/nce_debug.log", O_WRONLY | O_APPEND, 0644);
      if (mf2 >= 0)
      {
        char mb2[96];
        int ml2 = std::snprintf(mb2, sizeof(mb2), "MIV: WR phys=%08x w=%d val=%08x\n",
                                phys_fault, width, val);
        ::write(mf2, mb2, static_cast<size_t>(ml2));
        ::close(mf2);
      }
    }
    else
    {
      int mf2 = open("/home/link/nce_debug.log", O_WRONLY | O_APPEND, 0644);
      if (mf2 >= 0)
      {
        u32 rd_val = static_cast<u32>(ctx->uc_mcontext.regs->gpr[reg_dest]);
        char mb2[96];
        int ml2 = std::snprintf(mb2, sizeof(mb2), "MIV: RD phys=%08x w=%d val=%08x\n",
                                phys_fault, width, rd_val);
        ::write(mf2, mb2, static_cast<size_t>(ml2));
        ::close(mf2);
      }
    }
    return;
  }
  else
  {
    // Handle cache-control instructions (dcbz, dcbf, dcbst, dcbi) that are
    // trapped by DSI because EA is at an address not mapped in the host page
    // table (e.g., EA=0 on a system with vm.mmap_min_addr > 0).  These are
    // cache-hint instructions; skipping them (NOP) at the unmapped address is
    // safe because the guest cache state is irrelevant to emulation accuracy
    // (the PPC970 manages its own cache).  For dcbz we also zero the 32 bytes
    // at EA in emulated RAM to match Gekko semantics.
    // This avoids the expensive NCE exit/re-entry cycle (~10µs) per iteration.
    if (opcd == 31)
    {
      const u32 xo = (instr >> 1) & 0x3FF;
      if (xo == 1014)  // dcbz (data cache block zero)
      {
        const u32 ra = (instr >> 21) & 0x1F;  // bits 6-10
        const u32 rb = (instr >> 11) & 0x1F;  // bits 11-15
        const u32 ea =
            ((ra == 0 ? 0 : static_cast<u32>(ctx->uc_mcontext.regs->gpr[ra] & 0xFFFFFFFF)) +
             static_cast<u32>(ctx->uc_mcontext.regs->gpr[rb] & 0xFFFFFFFF)) &
            ~31u;

        auto& mem = m_system.GetMemory();
        const u32 ram_size_v = mem.GetRamSizeReal();
        const u32 exram_size_v = mem.GetExRamSizeReal();
        const u32 masked = ea & 0x3FFFFFFF;
        if (masked + 32 <= ram_size_v)
          std::memset(mem.GetRAM() + masked, 0, 32);
        else if (exram_size_v > 0 && (masked >> 28) == 0x1 &&
                 (masked & 0x0FFFFFFF) + 32 <= exram_size_v)
          std::memset(mem.GetEXRAM() + (masked & 0x0FFFFFFF), 0, 32);

        ctx->uc_mcontext.regs->nip = u64(pc_val) + 4;
        return;
      }
      // dcbf (xo=86), dcbst (xo=54), dcbi (xo=470), dcbst (xo=470? no)
      // are cache flush/invalidate operations that fault at EA=0.
      // Just skip them — no memory write needed.
      if (xo == 86 || xo == 54 || xo == 470)  // dcbf, dcbst, dcbi
      {
        ctx->uc_mcontext.regs->nip = u64(pc_val) + 4;
        return;
      }
    }

    // Check if the fault address is outside valid guest memory (RAM or EXRAM).
    // If so, inject DSI instead of trying SlowmemDataAccess (which would
    // silently drop the access and corrupt guest state).
    auto& mem = m_system.GetMemory();
    u32 ram_size = mem.GetRamSizeReal();
    u32 exram_size = mem.GetExRamSizeReal();
    bool in_valid_mem = (phys_fault < ram_size);
    if (!in_valid_mem && exram_size > 0)
      in_valid_mem = ((phys_fault >> 28) == 0x1 &&
                       (phys_fault & 0x0FFFFFFF) < exram_size);
    if (!in_valid_mem)
    {
      SaveGuestRegsFromContext(uctx);
      ExitNCEFromSignal(uctx, pc_val, true);
      return;
    }

    if (SlowmemDataAccess(uctx, instr, pc_val, fault_addr, dsisr_val))
    {
      return;
    }

    // SlowmemDataAccess couldn't handle this opcode pattern — exit NCE and
    // let the Run() loop handle it via interpreter fallback.
    SaveGuestRegsFromContext(uctx);
    ExitNCEFromSignal(uctx, pc_val, true);
  }
}

bool JitPPC64::SlowmemDataAccess(void* uctx, u32 instr, u32 pc_val, u32 fault_addr, u32 dsisr_val)
{
  auto* ctx = static_cast<ucontext_t*>(uctx);
  u32 opcd = (instr >> 26) & 0x3F;
  u32 rs_rd = (instr >> 21) & 0x1F;
  u32 ra = (instr >> 16) & 0x1F;
  u32 sub = (instr >> 1) & 0x3FF;

  // Validate guest address without calling PanicAlertFmt (not async-signal-safe).
  // Memory::Read_U32 / Write_U32 call GetSpanForAddress which calls PanicAlertFmt
  // on out-of-range addresses, so we must pre-check.
  // If the fault address is outside valid guest memory (RAM or EXRAM), return false
  // so that the caller injects DSI instead of silently dropping the access.
  auto& mem = m_system.GetMemory();
  u32 ram_size = mem.GetRamSizeReal();
  u32 exram_size = mem.GetExRamSizeReal();
  auto valid_addr = [ram_size, exram_size](u32 addr, size_t size) -> bool
  {
    u32 masked = addr & 0x3FFFFFFF;
    if (masked + size <= ram_size)
      return true;
    // EXRAM at block 1 (masked 0x10000000-0x13FFFFFF after 30-bit mask)
    if (exram_size > 0 && (masked >> 28) == 0x1 && (masked & 0x0FFFFFFF) + size <= exram_size)
      return true;
    return false;
  };
  bool addr_ok = true;
  auto check_addr = [&](u32 a, size_t s) { if (!valid_addr(a, s)) addr_ok = false; };
  auto rd32 = [&](u32 a) -> u32 { check_addr(a, 4); return valid_addr(a, 4) ? mem.Read_U32(a) : 0; };
  auto rd16 = [&](u32 a) -> u32 { check_addr(a, 2); return valid_addr(a, 2) ? mem.Read_U16(a) : 0; };
  auto rd8 = [&](u32 a) -> u32 { check_addr(a, 1); return valid_addr(a, 1) ? mem.Read_U8(a) : 0; };
  auto wr32 = [&](u32 a, u32 v) { check_addr(a, 4); if (valid_addr(a, 4)) mem.Write_U32(v, a); };
  auto wr16 = [&](u32 a, u16 v) { check_addr(a, 2); if (valid_addr(a, 2)) mem.Write_U16(v, a); };
  auto wr8 = [&](u32 a, u8 v) { check_addr(a, 1); if (valid_addr(a, 1)) mem.Write_U8(v, a); };

  // D-form loads/stores: EA = (ra==0 ? 0 : GPR[ra]) + d
  // X-form loads/stores (opcd 31): EA = (ra==0 ? 0 : GPR[ra]) + GPR[rb]
  auto ea_dform = [&]() -> u32
  {
    s32 d = static_cast<s16>(instr & 0xFFFF);
    u32 base = (ra == 0) ? 0 : static_cast<u32>(ctx->uc_mcontext.regs->gpr[ra]);
    return base + d;
  };
  auto ea_xform = [&]() -> u32
  {
    u32 rb = (instr >> 11) & 0x1F;
    u32 base = (ra == 0) ? 0 : static_cast<u32>(ctx->uc_mcontext.regs->gpr[ra]);
    u32 index = static_cast<u32>(ctx->uc_mcontext.regs->gpr[rb]);
    return base + index;
  };
  auto advance = [&]()
  {
    ctx->uc_mcontext.regs->nip = u64(pc_val) + 4;
  };

  // opcd 31 covers many indexed load/store variants
  if (opcd == 31)
  {
    switch (sub)
    {
    case 23: // lwzx
    {
      u32 val = rd32(ea_xform());
      ctx->uc_mcontext.regs->gpr[rs_rd] = val;
      advance();
      return true;
    }
    case 55: // lwzux
    {
      u32 ea = ea_xform();
      u32 val = rd32(ea);
      ctx->uc_mcontext.regs->gpr[rs_rd] = val;
      if (ra != 0)
        ctx->uc_mcontext.regs->gpr[ra] = ea;
      advance();
      return true;
    }
    case 87: // lbzx
    {
      u32 val = rd8(ea_xform());
      ctx->uc_mcontext.regs->gpr[rs_rd] = val;
      advance();
      return true;
    }
    case 119: // lbzux
    {
      u32 ea = ea_xform();
      u32 val = rd8(ea);
      ctx->uc_mcontext.regs->gpr[rs_rd] = val;
      if (ra != 0)
        ctx->uc_mcontext.regs->gpr[ra] = ea;
      advance();
      return true;
    }
    case 279: // lhzx
    {
      u32 val = rd16(ea_xform());
      ctx->uc_mcontext.regs->gpr[rs_rd] = val;
      advance();
      return true;
    }
    case 311: // lhzux
    {
      u32 ea = ea_xform();
      u32 val = rd16(ea);
      ctx->uc_mcontext.regs->gpr[rs_rd] = val;
      if (ra != 0)
        ctx->uc_mcontext.regs->gpr[ra] = ea;
      advance();
      return true;
    }
    case 343: // lhax
    {
      u32 val = rd16(ea_xform());
      ctx->uc_mcontext.regs->gpr[rs_rd] = static_cast<u64>(s16(val & 0xFFFF));
      advance();
      return true;
    }
    case 375: // lhaux
    {
      u32 ea = ea_xform();
      u32 val = rd16(ea);
      ctx->uc_mcontext.regs->gpr[rs_rd] = static_cast<u64>(s16(val & 0xFFFF));
      if (ra != 0)
        ctx->uc_mcontext.regs->gpr[ra] = ea;
      advance();
      return true;
    }
    case 151: // stwx
    {
      wr32(ea_xform(), static_cast<u32>(ctx->uc_mcontext.regs->gpr[rs_rd]));
      advance();
      return true;
    }
    case 183: // stwux
    {
      u32 ea = ea_xform();
      wr32(ea, static_cast<u32>(ctx->uc_mcontext.regs->gpr[rs_rd]));
      if (ra != 0)
        ctx->uc_mcontext.regs->gpr[ra] = ea;
      advance();
      return true;
    }
    case 215: // stbx
    {
      wr8(ea_xform(), static_cast<u8>(ctx->uc_mcontext.regs->gpr[rs_rd]));
      advance();
      return true;
    }
    case 247: // stbux
    {
      u32 ea = ea_xform();
      wr8(ea, static_cast<u8>(ctx->uc_mcontext.regs->gpr[rs_rd]));
      if (ra != 0)
        ctx->uc_mcontext.regs->gpr[ra] = ea;
      advance();
      return true;
    }
    case 407: // sthx
    {
      wr16(ea_xform(), static_cast<u16>(ctx->uc_mcontext.regs->gpr[rs_rd]));
      advance();
      return true;
    }
    case 439: // sthux
    {
      u32 ea = ea_xform();
      wr16(ea, static_cast<u16>(ctx->uc_mcontext.regs->gpr[rs_rd]));
      if (ra != 0)
        ctx->uc_mcontext.regs->gpr[ra] = ea;
      advance();
      return true;
    }
    case 20: // lwarx
    {
      u32 val = rd32(ea_xform());
      ctx->uc_mcontext.regs->gpr[rs_rd] = val;
      advance();
      return true;
    }
    case 150: // stwcx
    {
      wr32(ea_xform(), static_cast<u32>(ctx->uc_mcontext.regs->gpr[rs_rd]));
      advance();
      return true;
    }
    default:
      return false;
    }
  }

  // D-form loads
  switch (opcd)
  {
  case 32: // lwz
  {
    u32 val = rd32(ea_dform());
    ctx->uc_mcontext.regs->gpr[rs_rd] = val;
    advance();
    return true;
  }
  case 33: // lwzu
  {
    u32 ea = ea_dform();
    u32 val = rd32(ea);
    ctx->uc_mcontext.regs->gpr[rs_rd] = val;
    if (ra != 0)
      ctx->uc_mcontext.regs->gpr[ra] = ea;
    advance();
    return true;
  }
  case 34: // lbz
  {
    u32 val = rd8(ea_dform());
    ctx->uc_mcontext.regs->gpr[rs_rd] = val;
    advance();
    return true;
  }
  case 35: // lbzu
  {
    u32 ea = ea_dform();
    u32 val = rd8(ea);
    ctx->uc_mcontext.regs->gpr[rs_rd] = val;
    if (ra != 0)
      ctx->uc_mcontext.regs->gpr[ra] = ea;
    advance();
    return true;
  }
  case 40: // lhz
  {
    u32 val = rd16(ea_dform());
    ctx->uc_mcontext.regs->gpr[rs_rd] = val;
    advance();
    return true;
  }
  case 41: // lhzu
  {
    u32 ea = ea_dform();
    u32 val = rd16(ea);
    ctx->uc_mcontext.regs->gpr[rs_rd] = val;
    if (ra != 0)
      ctx->uc_mcontext.regs->gpr[ra] = ea;
    advance();
    return true;
  }
  case 42: // lha
  {
    u32 val = rd16(ea_dform());
    ctx->uc_mcontext.regs->gpr[rs_rd] = static_cast<u64>(s16(val & 0xFFFF));
    advance();
    return true;
  }
  case 43: // lhau
  {
    u32 ea = ea_dform();
    u32 val = rd16(ea);
    ctx->uc_mcontext.regs->gpr[rs_rd] = static_cast<u64>(s16(val & 0xFFFF));
    if (ra != 0)
      ctx->uc_mcontext.regs->gpr[ra] = ea;
    advance();
    return true;
  }
  // D-form stores
  case 36: // stw
  {
    wr32(ea_dform(), static_cast<u32>(ctx->uc_mcontext.regs->gpr[rs_rd]));
    advance();
    return true;
  }
  case 37: // stwu
  {
    u32 ea = ea_dform();
    wr32(ea, static_cast<u32>(ctx->uc_mcontext.regs->gpr[rs_rd]));
    if (ra != 0)
      ctx->uc_mcontext.regs->gpr[ra] = ea;
    advance();
    return true;
  }
  case 38: // stb
  {
    wr8(ea_dform(), static_cast<u8>(ctx->uc_mcontext.regs->gpr[rs_rd]));
    advance();
    return true;
  }
  case 39: // stbu
  {
    u32 ea = ea_dform();
    wr8(ea, static_cast<u8>(ctx->uc_mcontext.regs->gpr[rs_rd]));
    if (ra != 0)
      ctx->uc_mcontext.regs->gpr[ra] = ea;
    advance();
    return true;
  }
  case 44: // sth
  {
    wr16(ea_dform(), static_cast<u16>(ctx->uc_mcontext.regs->gpr[rs_rd]));
    advance();
    return true;
  }
  case 45: // sthu
  {
    u32 ea = ea_dform();
    wr16(ea, static_cast<u16>(ctx->uc_mcontext.regs->gpr[rs_rd]));
    if (ra != 0)
      ctx->uc_mcontext.regs->gpr[ra] = ea;
    advance();
    return true;
  }
  default:
    return false;
  }
}

// ---------------------------------------------------------------------------
// SIGALRM handler — periodic downcount check
// ---------------------------------------------------------------------------

void JitPPC64::HandleSIGALRM(int sig, siginfo_t* info, void* uctx)
{
  m_ppc_state.downcount -= EstimateDowncount();

  if (m_ppc_state.downcount <= 0)
  {
    auto* ctx = static_cast<ucontext_t*>(uctx);
    u64 pc_val = ctx->uc_mcontext.regs->nip;
    static constexpr u32 TRAMP_OFFSET = 0x1FF0000;
    static constexpr u64 TRAMP_START = 0x80000000ULL + TRAMP_OFFSET;
    static constexpr u64 TRAMP_END = TRAMP_START + 1024;
    bool in_guest_code =
        (pc_val >= 0x70000000ULL && pc_val < TRAMP_START) ||
        (pc_val >= TRAMP_END && pc_val < 0x82000000ULL) ||
        (pc_val >= 0xB0000000ULL && pc_val < 0xC2000000ULL);
    if (!in_guest_code)
      return;

    auto* saved = *reinterpret_cast<NativeContext const**>(NCE_SLOT_ADDR);
    if (!saved || saved->return_addr == 0 ||
        saved->return_addr == 0xFFFFFFFFFFFFFFFFULL)
      return;

    SaveGuestRegsFromContext(uctx);
    RestoreHostRegsInContext(uctx);
  }
}

// ---------------------------------------------------------------------------
// SIGILL handler — supervisor instructions and Paired Singles
// ---------------------------------------------------------------------------

static bool IsPairedSingleOpcd(u32 opcd)
{
  return opcd == 4 || opcd == 59 || opcd == 63;
}

void JitPPC64::HandleSIGILL(int sig, siginfo_t* info, void* uctx)
{
  // R2/R13 are already host values — restored by the asm entry point.
  auto* ctx = static_cast<ucontext_t*>(uctx);
  u32 pc_val = static_cast<u32>(ctx->uc_mcontext.regs->nip & 0xFFFFFFFF);

  // Ensure MSR[SF]=0 for correct 32-bit guest semantics.
  // On PPC64, MSR bit 0 (SF, 64-bit mode) is set when running a 64-bit
  // process.  Clearing it makes arithmetic instructions produce 32-bit
  // results and CR0 compare correctly.
  if (ctx->uc_mcontext.regs->msr & 0x8000000000000000ULL)
  {
    ctx->uc_mcontext.regs->msr &= ~0x8000000000000000ULL;
    return;  // re-execute the faulting instruction with SF=0
  }

  // === Check for patched dcbz (trap-and-emulate) ===
  // The dcbz instruction was replaced with an illegal instruction (0x00000000)
  // in the NCE mapping.  If nip matches a patched address, emulate dcbz directly
  // without reading the instruction from memory (which would return the trap).
  auto dcbz_it = m_patched_dcbz.find(pc_val);
  if (dcbz_it == m_patched_dcbz.end())
  {
    // Try alias resolution: if nip is in K2/K2-uncached range, look up K1 alias
    const u32 pc_block = pc_val >> 28;
    if (pc_block == 0x7 || pc_block == 0xB)
    {
      // K2 cached or uncached → K1 cached alias at (EA & 0x0FFFFFFF) | 0x80000000
      const u32 k1_alias = 0x80000000u | (pc_val & 0x0FFFFFFFu);
      auto alias_it = m_patched_dcbz.find(k1_alias);
      if (alias_it != m_patched_dcbz.end())
      {
        dcbz_it = m_patched_dcbz.emplace(pc_val, alias_it->second).first;
      }
    }
    else if (pc_block == 0xC || pc_block == 0xD)
    {
      // K1 uncached or K1 uncached block 1 → K1 cached alias at (EA & 0x3FFFFFFF) | 0x80000000
      const u32 k1_alias = 0x80000000u | (pc_val & 0x3FFFFFFFu);
      auto alias_it = m_patched_dcbz.find(k1_alias);
      if (alias_it != m_patched_dcbz.end())
      {
        dcbz_it = m_patched_dcbz.emplace(pc_val, alias_it->second).first;
      }
    }
  }

  if (dcbz_it != m_patched_dcbz.end())
  {
    const u32 orig_instr = dcbz_it->second;
    const u32 ra = (orig_instr >> 21) & 0x1F;   // bits 6-10
    const u32 rb = (orig_instr >> 11) & 0x1F;   // bits 11-15
    const u32 ea = ((ra == 0 ? 0 : static_cast<u32>(ctx->uc_mcontext.regs->gpr[ra] & 0xFFFFFFFF)) +
                    static_cast<u32>(ctx->uc_mcontext.regs->gpr[rb] & 0xFFFFFFFF)) &
                   ~31u;  // dcbz aligns to 32-byte cache line boundary

    auto& mem = m_system.GetMemory();
    const u32 ram_size_v = mem.GetRamSizeReal();
    const u32 exram_size_v = mem.GetExRamSizeReal();
    const u32 masked = ea & 0x3FFFFFFF;

    // === Optimize dcbz loop: detect dcbz; addi rN, rN, 32; bdnz+ ===
    FILE* nce_log = fopen("/home/link/nce_debug.log", "a");
    if (nce_log)
    {
      fprintf(nce_log, "DBZ: dcbz at 0x%08X, EA=0x%08X, RA=%u RB=%u\n",
              static_cast<u32>(pc_val), ea, ra, rb);
      fclose(nce_log);
    }
    // When the game clears a large memory range via the idiom:
    //   dcbz 0, rN        (patched, traps here)
    //   addi rN, rN, 32   advance to next line
    //   bdnz+ loop        decrement CTR, branch back if non-zero
    // we emulate the entire remaining loop in one memset instead of N SIGILLs.
    {
      u32 next1 = 0, next2 = 0;
      std::memcpy(&next1, reinterpret_cast<const void*>(static_cast<u64>(pc_val) + 4),
                  sizeof(next1));
      std::memcpy(&next2, reinterpret_cast<const void*>(static_cast<u64>(pc_val) + 8),
                  sizeof(next2));

      const u32 n1_opcd = (next1 >> 26) & 0x3F;
      const u32 n1_rd = (next1 >> 21) & 0x1F;
      const u32 n1_ra = (next1 >> 16) & 0x1F;
      const s16 n1_simm = static_cast<s16>(next1 & 0xFFFF);

      const u32 n2_opcd = (next2 >> 26) & 0x3F;
      const u32 n2_bo = (next2 >> 21) & 0x1F;

      // addi rN, rN, 32 (opcode 14, RA=RD, SIMM=32)
      // followed by bc (opcode 16) with BO indicating bdnz/bdnz+/bdnz-
      const bool has_addi = (n1_opcd == 14 && n1_rd != 0 && n1_rd == n1_ra && n1_simm == 32);
      const bool has_bdnz = (n2_opcd == 16 && (n2_bo == 16 || n2_bo == 17 || n2_bo == 18));
      if (has_addi && has_bdnz)
      {
        // Verify the branch target points back to the dcbz instruction
        const u32 n2_bd = (next2 >> 16) & 0x3FFF;
        const s16 disp = static_cast<s16>(static_cast<u16>(n2_bd << 2));
        const u32 btarget = static_cast<u32>(pc_val) + 8 + static_cast<u32>(disp);

        // Canonical loop: dcbz uses ra=0 and rb=rN (the loop pointer)
        if (btarget == static_cast<u32>(pc_val) && rb == n1_rd && ra == 0)
        {
          const u32 ctr_val = static_cast<u32>(ctx->uc_mcontext.regs->ctr & 0xFFFFFFFF);
          const u32 total_bytes = ctr_val * 32;

          // Only optimize when at least 10 iterations remain (avoid overhead
          // for tiny loops where per-dcbz handling is already fast enough).
          if (total_bytes >= 320 && total_bytes <= 32 * 1024 * 1024)
          {
            const bool loop_zeroed = [&]() -> bool
            {
              if (masked + total_bytes <= ram_size_v)
              {
                std::memset(mem.GetRAM() + masked, 0, total_bytes);
                return true;
              }
              if (exram_size_v > 0 && (masked >> 28) == 0x1 &&
                  (masked & 0x0FFFFFFF) + total_bytes <= exram_size_v)
              {
                std::memset(mem.GetEXRAM() + (masked & 0x0FFFFFFF), 0, total_bytes);
                return true;
              }
              return false;
            }();

            if (loop_zeroed)
            {
              FILE* nce_log2 = fopen("/home/link/nce_debug.log", "a");
              if (nce_log2)
              {
                fprintf(nce_log2, "DBZ: loop opt at 0x%08X: %u lines, %u bytes, EA=0x%08X\n",
                        static_cast<u32>(pc_val), ctr_val, total_bytes, ea);
                fclose(nce_log2);
              }

              // Update loop register to final value (EA + total_bytes)
              const u32 rn_val = static_cast<u32>(ctx->uc_mcontext.regs->gpr[n1_rd] & 0xFFFFFFFF);
              ctx->uc_mcontext.regs->gpr[n1_rd] = static_cast<u64>(rn_val + total_bytes);

              // Set CTR to 0 (loop completed)
              ctx->uc_mcontext.regs->ctr = 0;

              // Advance nip past the loop (past the bdnz+ at pc_val+8)
              ctx->uc_mcontext.regs->nip = static_cast<u64>(static_cast<u32>(pc_val) + 12);

              return;  // loop fully emulated, continue native execution
            }
          }
        }
      }
      else if (!has_addi || !has_bdnz)
      {
        FILE* nce_loop = fopen("/home/link/nce_debug.log", "a");
        if (nce_loop)
        {
          fprintf(nce_loop,
                  "DBZ: no loop pattern at 0x%08X: next1=0x%08X(opcd=%u,rd=%u,ra=%u,simm=%d) "
                  "next2=0x%08X(opcd=%u,bo=%u) addi=%d bdnz=%d ra=%u rb=%u\n",
                  static_cast<u32>(pc_val), next1, n1_opcd, n1_rd, n1_ra, n1_simm,
                  next2, n2_opcd, n2_bo, has_addi, has_bdnz, ra, rb);
          fclose(nce_loop);
        }
      }
    }

    // === Fallthrough: single dcbz ===
    {
      bool zeroed = false;
      if (masked + 32 <= ram_size_v)
      {
        std::memset(mem.GetRAM() + masked, 0, 32);
        zeroed = true;
      }
      else if (exram_size_v > 0 && (masked >> 28) == 0x1 &&
               (masked & 0x0FFFFFFF) + 32 <= exram_size_v)
      {
        std::memset(mem.GetEXRAM() + (masked & 0x0FFFFFFF), 0, 32);
        zeroed = true;
      }
      if (!zeroed)
      {
        static constexpr const char dcbz_oob[] = "NCE: dcbz EA out of bounds\n";
        ::write(STDERR_FILENO, dcbz_oob, sizeof(dcbz_oob) - 1);
      }

      {
        FILE* nce_log3 = fopen("/home/link/nce_debug.log", "a");
        if (nce_log3)
        {
          fprintf(nce_log3, "DBZ: single at 0x%08X, EA=0x%08X %s\n",
                  static_cast<u32>(pc_val), ea, zeroed ? "zeroed" : "OOB");
          fclose(nce_log3);
        }
      }

      ctx->uc_mcontext.regs->nip = u64(pc_val) + 4;
      return;  // continue native execution
    }
  }
  else
  {
    u32 raw_instr = 0;
    std::memcpy(&raw_instr, reinterpret_cast<const void*>(static_cast<u64>(pc_val)), sizeof(raw_instr));
    FILE* nce_log4 = fopen("/home/link/nce_debug.log", "a");
    if (nce_log4)
    {
      fprintf(nce_log4, "DBZ: SIGILL at 0x%08X NOT in patched_dcbz, raw=0x%08X\n",
              static_cast<u32>(pc_val), raw_instr);
      fclose(nce_log4);
    }
  }

  // === Check for patched P0 (trap-and-emulate) ===
  auto p0_it = m_patched_p0.find(pc_val);
  if (p0_it == m_patched_p0.end())
  {
    const u32 pc_block = pc_val >> 28;
    if (pc_block == 0x7 || pc_block == 0xB)
    {
      const u32 k1_alias = 0x80000000u | (pc_val & 0x0FFFFFFFu);
      auto alias_it = m_patched_p0.find(k1_alias);
      if (alias_it != m_patched_p0.end())
        p0_it = m_patched_p0.emplace(pc_val, alias_it->second).first;
    }
    else if (pc_block == 0xC || pc_block == 0xD)
    {
      const u32 k1_alias = 0x80000000u | (pc_val & 0x3FFFFFFFu);
      auto alias_it = m_patched_p0.find(k1_alias);
      if (alias_it != m_patched_p0.end())
        p0_it = m_patched_p0.emplace(pc_val, alias_it->second).first;
    }
  }

  if (p0_it != m_patched_p0.end())
  {
    // Save the current guest register state from the ucontext into
    // m_ppc_state.  The guest has been executing natively up to this point,
    // and m_ppc_state only has the last ALRM-exit values — not the current
    // native modifications that are in the ucontext GPRs.
    SaveGuestRegsFromContext(uctx);

    m_ppc_state.pc = pc_val;
    m_ppc_state.npc = pc_val + 4;
    UGeckoInstruction gi;
    gi.hex = p0_it->second;

    // Handle mftb and mfspr TL/TU inline with correct timebase computation.
    // FallBackToInterpreter -> Interpreter::mfspr calls GetFakeTimeBase()
    // which returns a value derived from global_timer as of the last
    // Advance().  During NCE the guest may execute thousands of cycles
    // since the last Advance, but those cycles are invisible to
    // GetFakeTimeBase — the TB appears frozen and tight polling loops
    // (mftb; cmpwi; blt) never see it advance.
    //
    // We recompute the TB here just like Jit64 does: add the estimated
    // cycles_since_advance (from remaining downcount) to global_timer
    // to get the current cycle count, then compute TB = TB_start +
    // (current_cycles - TB_start_ticks) / TIMER_RATIO.
    const bool is_mftb = (gi.OPCD == 31 && gi.SUBOP10 == 371);
    const bool is_mfspr_tb = (gi.OPCD == 31 && gi.SUBOP10 == 339);
    u32 spr_index = 0;
    if (is_mftb)
    {
      spr_index = (gi.TBRU << 5) | gi.TBRL;
    }
    else if (is_mfspr_tb)
    {
      spr_index = (gi.SPRU << 5) | gi.SPRL;
    }
    const bool is_tb_read = (spr_index == SPR_TL || spr_index == SPR_TU);

    if ((is_mftb || is_mfspr_tb) && is_tb_read)
    {
      // Compute the current cycle count including cycles since last Advance().
      const auto& globals = m_system.GetCoreTiming().GetGlobals();
      const s64 downcount_cycles = static_cast<s64>(
          static_cast<double>(m_ppc_state.downcount) *
          globals.last_OC_factor_inverted);
      const s64 cycles_since_advance =
          globals.slice_length - downcount_cycles;
      const s64 current_cycles = globals.global_timer + cycles_since_advance;
      const s64 tb_delta = current_cycles - globals.fake_TB_start_ticks;

      // TB = TB_start + tb_delta / TIMER_RATIO  (TIMER_RATIO = 12).
      const u64 tb = globals.fake_TB_start_value +
                     (tb_delta > 0 ? static_cast<u64>(tb_delta) / 12 : 0ULL);

      m_ppc_state.spr[SPR_TL] = static_cast<u32>(tb);
      m_ppc_state.spr[SPR_TU] = static_cast<u32>(tb >> 32);

      const u32 rd = gi.RD;
      m_ppc_state.gpr[rd] = m_ppc_state.spr[spr_index];
      m_ppc_state.pc = pc_val + 4;
      {
        int sf = open("/home/link/nce_debug.log", O_WRONLY | O_APPEND, 0644);
        if (sf >= 0)
        {
          char sb[128];
          int sl = std::snprintf(sb, sizeof(sb),
                                 "SPR: mftb spr=%u val=%08x rd=r%u pc=%08x\n",
                                 spr_index, m_ppc_state.gpr[rd], rd, pc_val);
          ::write(sf, sb, static_cast<size_t>(sl));
          ::close(sf);
        }
      }
    }
    else
    {
      const u32 opcd = gi.OPCD;

      // mfspr (xo=339) / mtspr (xo=467): dispatch to EmulateMFSpr / EmulateMTSpr
      // instead of the interpreter, so m_guest.* storage is used consistently.
      if (opcd == 31 && gi.SUBOP10 == 339)
      {
        const u32 spr = (gi.SPRU << 5) | gi.SPRL;
        const u32 rd = gi.RD;
        m_ppc_state.gpr[rd] = EmulateMFSpr(spr);
        m_ppc_state.pc = pc_val + 4;
        {
          int sf = open("/home/link/nce_debug.log", O_WRONLY | O_APPEND, 0644);
          if (sf >= 0)
          {
            char sb[128];
            int sl = std::snprintf(sb, sizeof(sb),
                                   "SPR: mfspr spr=%u val=%08x rd=r%u pc=%08x\n",
                                   spr, m_ppc_state.gpr[rd], rd, pc_val);
            ::write(sf, sb, static_cast<size_t>(sl));
            ::close(sf);
          }
        }
      }
      else if (opcd == 31 && gi.SUBOP10 == 467)
      {
        const u32 spr = (gi.SPRU << 5) | gi.SPRL;
        const u32 rs = gi.RD;
        EmulateMTSpr(spr, static_cast<u32>(m_ppc_state.gpr[rs]));
        m_ppc_state.pc = pc_val + 4;
        {
          int sf = open("/home/link/nce_debug.log", O_WRONLY | O_APPEND, 0644);
          if (sf >= 0)
          {
            char sb[128];
            int sl = std::snprintf(sb, sizeof(sb),
                                   "SPR: mtspr spr=%u val=%08x rs=r%u pc=%08x\n",
                                   spr, static_cast<u32>(m_ppc_state.gpr[rs]), rs, pc_val);
            ::write(sf, sb, static_cast<size_t>(sl));
            ::close(sf);
          }
        }
      }
      else
      {
        // psq_l/st operate on FPRs.  Save real FPRs from the ucontext (where
        // native execution left them) before the interpreter reads stale ps[].
        const bool uses_fpr = (opcd == 56 || opcd == 57 || opcd == 60 || opcd == 61);
        // psq_l/lu (opcd 56/57) write the loaded value to destination FPR.
        // psq_st/stu (opcd 60/61) only read source FPRs — no FPR restored.
        const bool modifies_fpr = (opcd == 56 || opcd == 57);
        const u32 fpr_dest = gi.RD;

        if (uses_fpr)
          SaveFPRsFromContext(uctx);

        FallBackToInterpreter(gi);
        m_ppc_state.pc = m_ppc_state.npc;

        if (modifies_fpr)
          RestoreFPRToContext(uctx, fpr_dest);
      }
    }

    // Copy interpreter results back to ucontext.  The interpreter or our
    // inline handler above modified m_ppc_state from the (now-current) GPRs
    // read by SaveGuestRegsFromContext.  Without this copy, sigreturn would
    // resume with the old ucontext GPRs, losing the results.
    for (int i = 0; i < 32; i++)
      ctx->uc_mcontext.regs->gpr[i] = static_cast<u64>(m_ppc_state.gpr[i]);
    ctx->uc_mcontext.regs->ccr = m_ppc_state.cr.Get();
    ctx->uc_mcontext.regs->link = m_ppc_state.spr[SPR_LR];
    ctx->uc_mcontext.regs->ctr = m_ppc_state.spr[SPR_CTR];
    {
      u64 xer = 0;
      xer |= static_cast<u64>(m_ppc_state.xer_stringctrl) << 16;
      xer |= static_cast<u64>(m_ppc_state.xer_so_ov & 1) << 31;
      xer |= static_cast<u64>((m_ppc_state.xer_so_ov >> 1) & 1) << 30;
      xer |= static_cast<u64>(m_ppc_state.xer_ca) << 29;
      ctx->uc_mcontext.regs->xer = xer;
    }
    ctx->uc_mcontext.regs->nip = u64(m_ppc_state.pc);

    // Return to guest code (sigreturn restores the ucontext with host
    // registers in place, so the guest continues at the updated PC).
    RestoreHostRegsInContext(uctx);
    return;
  }

  // === Normal SIGILL dispatch ===
  u32 instr = 0;
  if (const u8* ptr = m_system.GetMemory().GetPointerForRange(pc_val, sizeof(u32)))
    std::memcpy(&instr, ptr, sizeof(instr));
  u32 opcd = (instr >> 26) & 0x3F;
  u32 xo = (instr >> 1) & 0x3FF;
  u32 reg_dest = (instr >> 21) & 0x1F;
  u32 reg_src = (instr >> 21) & 0x1F;

  bool continue_native = false;

  if (opcd == 31)
  {
    if (xo == 339)
    {
      u32 spr = ((instr >> 11) & 0x1F) << 5 | ((instr >> 16) & 0x1F);
      ctx->uc_mcontext.regs->gpr[reg_dest] = EmulateMFSpr(spr);
      ctx->uc_mcontext.regs->nip = u64(pc_val) + 4;
      continue_native = true;
      {
        int sf2 = open("/home/link/nce_debug.log", O_WRONLY | O_APPEND, 0644);
        if (sf2 >= 0)
        {
          char sb2[128];
          u32 rdv = static_cast<u32>(ctx->uc_mcontext.regs->gpr[reg_dest]);
          int sl2 = std::snprintf(sb2, sizeof(sb2),
                                  "SPR: mfspr spr=%u val=%08x rd=r%u pc=%08x\n",
                                  spr, rdv, reg_dest, pc_val);
          ::write(sf2, sb2, static_cast<size_t>(sl2));
          ::close(sf2);
        }
      }
    }
    else if (xo == 467)
    {
      u32 spr = ((instr >> 11) & 0x1F) << 5 | ((instr >> 16) & 0x1F);
      EmulateMTSpr(spr, ctx->uc_mcontext.regs->gpr[reg_src]);
      ctx->uc_mcontext.regs->nip = u64(pc_val) + 4;
      continue_native = true;
      {
        int sf2 = open("/home/link/nce_debug.log", O_WRONLY | O_APPEND, 0644);
        if (sf2 >= 0)
        {
          char sb2[128];
          u32 wv = static_cast<u32>(ctx->uc_mcontext.regs->gpr[reg_src]);
          int sl2 = std::snprintf(sb2, sizeof(sb2),
                                  "SPR: mtspr spr=%u val=%08x rs=r%u pc=%08x\n",
                                  spr, wv, reg_src, pc_val);
          ::write(sf2, sb2, static_cast<size_t>(sl2));
          ::close(sf2);
        }
      }
    }
    else if (xo == 83)
    {
      ctx->uc_mcontext.regs->gpr[reg_dest] = EmulateMFMSR();
      ctx->uc_mcontext.regs->nip = u64(pc_val) + 4;
      continue_native = true;
    }
    else if (xo == 146)
    {
      EmulateMTMSR(ctx->uc_mcontext.regs->gpr[reg_src]);
      ctx->uc_mcontext.regs->nip = u64(pc_val) + 4;
      continue_native = true;
    }
    else if (xo == 210)
    {
      u32 sr = (instr >> 16) & 0xF;
      m_guest.segment_regs[sr] = ctx->uc_mcontext.regs->gpr[reg_src];
      ctx->uc_mcontext.regs->nip = u64(pc_val) + 4;
      continue_native = true;
    }
    else if (xo == 595)
    {
      u32 sr = (instr >> 16) & 0xF;
      ctx->uc_mcontext.regs->gpr[reg_dest] = m_guest.segment_regs[sr];
      ctx->uc_mcontext.regs->nip = u64(pc_val) + 4;
      continue_native = true;
    }
    else if (xo == 470)
    {
      // dcbi — data cache block invalidate
      u32 ra_dcbi = (instr >> 16) & 0x1F;
      u32 rb_dcbi = instr & 0x1F;
      u32 ea = (ra_dcbi == 0 ? 0 : static_cast<u32>(ctx->uc_mcontext.regs->gpr[ra_dcbi])) +
               static_cast<u32>(ctx->uc_mcontext.regs->gpr[rb_dcbi]);
      m_mmu.InvalidateDCacheLine(ea);
      ctx->uc_mcontext.regs->nip = u64(pc_val) + 4;
      continue_native = true;
    }
    else if (xo == 166)
    {
      // mfdcr — move from device control register
      // DCR = ((instr>>11)&0x1F)<<5 | (instr>>16)&0x1F  (bits 11-15=upper, 16-20=lower)
      u32 dcr_num = (((instr >> 11) & 0x1F) << 5) | ((instr >> 16) & 0x1F);
      ctx->uc_mcontext.regs->gpr[reg_dest] = m_dcr[dcr_num];
      ctx->uc_mcontext.regs->nip = u64(pc_val) + 4;
      continue_native = true;
    }
    else if (xo == 454)
    {
      // mtdcr — move to device control register
      u32 dcr_num = (((instr >> 11) & 0x1F) << 5) | ((instr >> 16) & 0x1F);
      m_dcr[dcr_num] = static_cast<u32>(ctx->uc_mcontext.regs->gpr[reg_dest]);
      ctx->uc_mcontext.regs->nip = u64(pc_val) + 4;
      continue_native = true;
    }
    else
    {
      UGeckoInstruction gecko;
      gecko.hex = instr;
      FallBackToInterpreter(gecko);
      ctx->uc_mcontext.regs->nip = m_ppc_state.pc;
      // icbi (Flush Instruction Cache) is called after new code is loaded
      // into RAM (e.g., DOL loading via DVD).  Trigger a dcbz re-scan so
      // that dcbz instructions in the newly-loaded code get patched before
      // the next NCE entry — otherwise they execute natively on PPC970,
      // zeroing 128 bytes instead of Gekko's 32.
      if (xo == 982)  // icbi
        m_dcbz_needs_rescan = true;
    }
  }
  else if (opcd == 19 && xo == 50)
  {
    u32 new_pc = EmulateRFI();
    ctx->uc_mcontext.regs->nip = new_pc;
    continue_native = true;
  }
  else if (opcd == 17)
  {
    UGeckoInstruction gecko;
    gecko.hex = instr;
    FallBackToInterpreter(gecko);
    ctx->uc_mcontext.regs->nip = m_ppc_state.pc;
  }
  else if (IsPairedSingleOpcd(opcd))
  {
    EmulatePairedSingle(instr, uctx);
    ctx->uc_mcontext.regs->nip = u64(pc_val) + 4;
    continue_native = true;
  }
  else
  {
    ERROR_LOG_FMT(POWERPC, "NCE SIGILL: unknown opcd {} @ {:08x}", opcd, pc_val);
  }

  if (!continue_native)
  {
    SaveGuestRegsFromContext(uctx);
    RestoreHostRegsInContext(uctx);
  }
}

// ---------------------------------------------------------------------------
// SPR emulation
// ---------------------------------------------------------------------------

u32 JitPPC64::EmulateMFSpr(u32 spr)
{
  // Most Gekko-specific SPRs (GQRs, WPAR, DMAU, DMAL, thermal, etc.)
  // are stored in m_ppc_state.spr[].  For NCE they are read/written via
  // SIGILL handler, so read the canonical copy.
  switch (spr)
  {
  case 18:    return m_guest.dsisr;
  case 19:    return m_guest.dar;
  case 22:    return m_guest.decrementer;
  case SPR_TL:
  case SPR_TU:
  {
    const u64 tb = m_system.GetSystemTimers().GetFakeTimeBase();
    return (spr == SPR_TL) ? static_cast<u32>(tb) : static_cast<u32>(tb >> 32);
  }
  case SPR_PVR: return m_ppc_state.spr[SPR_PVR];
  case 284:   return m_guest.tbl;
  case 285:   return m_guest.tbu;
  case 26:    return m_guest.srr0;
  case 27:    return m_guest.srr1;
  case 1008:  return m_guest.hid0;
  case 1009:  return m_guest.hid1;
  case 920:   return m_guest.hid2;
  case 1011:  return m_guest.hid4;
  case 272: case 273: case 274: case 275:
    return m_guest.sprg[spr - 272];
  case 1017:  return m_guest.l2cr;
  case 936: case 952:
    return m_guest.mmcr0;
  case 940: case 956:
    return m_guest.mmcr1;
  // GQR0-7 — Graphics Quantization Registers
  case 912: case 913: case 914: case 915:
  case 916: case 917: case 918: case 919:
    return m_ppc_state.spr[spr];
  // WPAR — Write gather pipe address + BNE bit
  case 921:
  {
    u32 wpar_val = m_ppc_state.spr[SPR_WPAR];
    if (m_system.GetGPFifo().IsBNE())
      wpar_val |= 1;
    else
      wpar_val &= ~1;
    return wpar_val;
  }
  // DMAU/DMAL — Locked cache DMA
  case 922:  return m_ppc_state.spr[SPR_DMAU];
  case 923:  return m_ppc_state.spr[SPR_DMAL];
  // ECID — Chip ID (read-only, set during Init)
  case 924:  return m_ppc_state.spr[SPR_ECID_U];
  case 925:  return m_ppc_state.spr[SPR_ECID_M];
  case 926:  return m_ppc_state.spr[SPR_ECID_L];
  // User-mode performance monitor aliases
  case 937:  return m_ppc_state.spr[SPR_PMC1];  // UPMC1
  case 938:  return m_ppc_state.spr[SPR_PMC2];  // UPMC2
  case 939:  return m_ppc_state.spr[SPR_SIA];   // USIA
  case 941:  return m_ppc_state.spr[SPR_PMC3];  // UPMC3
  case 942:  return m_ppc_state.spr[SPR_PMC4];  // UPMC4
  // Performance monitor counters
  case 953:  return m_ppc_state.spr[SPR_PMC1];
  case 954:  return m_ppc_state.spr[SPR_PMC2];
  case 955:  return m_ppc_state.spr[SPR_SIA];
  case 957:  return m_ppc_state.spr[SPR_PMC3];
  case 958:  return m_ppc_state.spr[SPR_PMC4];
  // IABR/DABR — Instruction/Data breakpoints
  case 1010: return m_ppc_state.spr[SPR_IABR] & ~1;  // TE bit always 0 on read
  case 1013: return m_ppc_state.spr[SPR_DABR];
  // ICTC — Instruction cache timing control
  case 1019: return m_ppc_state.spr[SPR_ICTC];
  // THRM1-3 — Thermal monitoring
  case 1020: return m_ppc_state.spr[SPR_THRM1];
  case 1021: return m_ppc_state.spr[SPR_THRM2];
  case 1022: return m_ppc_state.spr[SPR_THRM3];
  default:
    if (spr >= 528 && spr <= 543)
    {
      bool upper = (spr % 2) == 0;
      const bool is_ibat = spr < 536;
      const u32 idx = is_ibat ? (spr - 528) / 2 : (spr - 536) / 2;
      if (is_ibat)
        return upper ? m_guest.ibatu[idx] : m_guest.ibatl[idx];
      else
        return upper ? m_guest.dbatu[idx] : m_guest.dbatl[idx];
    }
    if (spr >= 560 && spr <= 575)
    {
      bool upper = (spr % 2) == 0;
      const bool is_ibat = spr < 568;
      const u32 idx = is_ibat ? (spr - 560) / 2 : (spr - 568) / 2;
      if (is_ibat)
        return upper ? m_guest.ibatu[idx] : m_guest.ibatl[idx];
      else
        return upper ? m_guest.dbatu[idx] : m_guest.dbatl[idx];
    }
    // Log unknown SPR reads (async-signal-safe: use write, not ERROR_LOG)
    static constexpr const char unknown_mfspr[] = "NCE: unknown mfspr\n";
    ::write(STDERR_FILENO, unknown_mfspr, sizeof(unknown_mfspr) - 1);
    return 0;
  }
}

void JitPPC64::EmulateMTSpr(u32 spr, u32 val)
{
  switch (spr)
  {
  case 18:   m_guest.dsisr = val; break;
  case 19:   m_guest.dar = val; break;
  case 22:   m_guest.decrementer = val; break;
  case 284:  m_guest.tbl = val; break;
  case 285:  m_guest.tbu = val; break;
  case 26:   m_guest.srr0 = val; break;
  case 27:   m_guest.srr1 = val; break;
  case 1008: m_guest.hid0 = val; break;
  case 1009: m_guest.hid1 = val; break;
  case 920:  m_guest.hid2 = val; break;
  case 1011: m_guest.hid4 = val; break;
  case 272: case 273: case 274: case 275:
    m_guest.sprg[spr - 272] = val; break;
  // GQR0-7 — Graphics Quantization Registers
  case 912: case 913: case 914: case 915:
  case 916: case 917: case 918: case 919:
    m_ppc_state.spr[spr] = val; break;
  // WPAR — Write gather pipe address
  case 921:
    m_ppc_state.spr[SPR_WPAR] = val;
    m_system.GetGPFifo().ResetGatherPipe();
    break;
  // DMAL — Locked cache DMA trigger
  case 923:
  {
    m_ppc_state.spr[SPR_DMAL] = val;
    UReg_DMAL dmal;
    dmal.Hex = val;
    if (dmal.DMA_T)
    {
      UReg_DMAU dmau;
      dmau.Hex = m_ppc_state.spr[SPR_DMAU];
      u32 mem_address = dmau.MEM_ADDR << 5;
      u32 cache_address = dmal.LC_ADDR << 5;
      u32 length = (dmau.DMA_LEN_U << 2) | dmal.DMA_LEN_L;
      if (length == 0)
        length = 128;
      if (dmal.DMA_LD)
        m_mmu.DMA_MemoryToLC(cache_address, mem_address, length);
      else
        m_mmu.DMA_LCToMemory(mem_address, cache_address, length);
    }
    dmal.DMA_T = 0;
    m_ppc_state.spr[SPR_DMAL] = dmal.Hex;
    break;
  }
  // SDR1 — Page table base register
  case 25:
    m_ppc_state.spr[SPR_SDR] = val;
    m_mmu.SDRUpdated();
    break;
  // EAR — External Access Register
  case 282:
    m_ppc_state.spr[SPR_EAR] = val; break;
  // ICTC — Instruction cache timing control
  case 1019:
    m_ppc_state.spr[SPR_ICTC] = val; break;
  // THRM1-3 — Thermal monitoring
  case 1020:
  case 1021:
  case 1022:
    m_ppc_state.spr[spr] = val; break;
  // L2CR — L2 Cache Control
  case 1017: m_guest.l2cr = val; break;
  // MMCR0/MMCR1 — Performance monitor control
  case 952:
  case 956:
    m_ppc_state.spr[spr] = val;
    PowerPC::MMCRUpdated(m_ppc_state);
    break;
  default:
    if (spr >= 528 && spr <= 543)
    {
      bool upper = (spr % 2) == 0;
      const bool is_ibat = spr < 536;
      const u32 idx = is_ibat ? (spr - 528) / 2 : (spr - 536) / 2;
      if (is_ibat)
      {
        if (upper)
          m_guest.ibatu[idx] = val;
        else
          m_guest.ibatl[idx] = val;
      }
      else
      {
        if (upper)
          m_guest.dbatu[idx] = val;
        else
          m_guest.dbatl[idx] = val;
      }
    }
    else if (spr >= 560 && spr <= 575)
    {
      bool upper = (spr % 2) == 0;
      const bool is_ibat = spr < 568;
      const u32 idx = is_ibat ? (spr - 560) / 2 : (spr - 568) / 2;
      if (is_ibat)
      {
        if (upper)
          m_guest.ibatu[idx] = val;
        else
          m_guest.ibatl[idx] = val;
      }
      else
      {
        if (upper)
          m_guest.dbatu[idx] = val;
        else
          m_guest.dbatl[idx] = val;
      }
    }
    else
    {
      // Log unknown SPR writes (async-signal-safe: use write, not ERROR_LOG)
      static constexpr const char unknown_mtspr[] = "NCE: unknown mtspr\n";
      ::write(STDERR_FILENO, unknown_mtspr, sizeof(unknown_mtspr) - 1);
    }
    break;
  }
}

u32 JitPPC64::EmulateRFI()
{
  static constexpr u32 RFI_MASK = 0x87C0FF73;
  m_guest.msr = (m_guest.msr & ~RFI_MASK) | (m_guest.srr1 & RFI_MASK);
  m_guest.msr &= ~(1u << (31 - 13));
  m_ppc_state.msr.Hex = m_guest.msr;
  return m_guest.srr0 & ~3;
}

void JitPPC64::EmulateMTMSR(u32 val)
{
  m_guest.msr = val;
  m_ppc_state.msr.Hex = val;
}

u32 JitPPC64::EmulateMFMSR()
{
  return m_guest.msr;
}

// Exit NCE from a signal handler, restoring host registers from the
// NativeContext at NCE_SLOT_ADDR and returning to the Run() loop.
// Returns true if the exit succeeded; false if no NativeContext was found
// (in which case the caller should handle the fallback itself).
bool JitPPC64::ExitNCEFromSignal(void* uctx, u32 pc_val, bool skip_instruction)
{
  auto* ctx = static_cast<ucontext_t*>(uctx);
  auto* slot = reinterpret_cast<NativeContext**>(NCE_SLOT_ADDR);
  if (auto* nc = *slot)
  {
    // Sentinel return_addr means the NativeContext hasn't been fully
    // initialised yet (before first trampoline entry, or after we've
    // returned but before the next entry).  Don't restore nip to the
    // sentinel value — that would jump to 0xFFFFFFFFFFFFFFFF → SIGSEGV.
    // Instead, advance past the faulting instruction if applicable and
    // return false so the caller knows we couldn't fully exit NCE.
    if (nc->return_addr == 0xFFFFFFFFFFFFFFFFULL)
    {
      *slot = nullptr;
      if (skip_instruction)
        ctx->uc_mcontext.regs->nip = u64(pc_val) + 4;
      return false;
    }

    // For data faults (skip_instruction=true): advance past the faulting
    // instruction to prevent re-entry at the same pc → infinite loop.
    // For instruction-fetch faults (skip_instruction=false): keep the
    // current pc so the Run() loop's interpreter fallback handles it.
    m_ppc_state.pc = skip_instruction ? (pc_val + 4) : pc_val;
    ctx->uc_mcontext.regs->gpr[1] = nc->host_r1;
    ctx->uc_mcontext.regs->gpr[2] = nc->host_r2;
    ctx->uc_mcontext.regs->gpr[13] = nc->host_r13;
    ctx->uc_mcontext.regs->ccr = nc->host_cr;
    for (int i = 0; i < 18; i++)
      ctx->uc_mcontext.regs->gpr[14 + i] = nc->host_gpr14_31[i];
    ctx->uc_mcontext.regs->nip = nc->return_addr;
    *slot = nullptr;
    return true;
  }
  if (skip_instruction)
    ctx->uc_mcontext.regs->nip = u64(pc_val) + 4;
  return false;
}

void JitPPC64::EmulateDSI(void* uctx)
{
  auto* ctx = static_cast<ucontext_t*>(uctx);
  u32 pc_val = static_cast<u32>(ctx->uc_mcontext.regs->nip & 0xFFFFFFFF);

  m_guest.dsisr = static_cast<u32>(ctx->uc_mcontext.regs->dsisr);
  m_guest.dar = static_cast<u32>(ctx->uc_mcontext.regs->dar);
  m_guest.srr0 = pc_val;
  m_guest.srr1 = m_guest.msr;
  m_guest.msr &= ~0x04EF36;

  // Use K1 cached alias pointing to the actual IPL vector code.
  // The IPL binary is loaded by Boot.cpp at physical 0x01200000, so
  // the DSI vector (IPL file offset 0x300 → physical 0x01200200 →
  // K1 alias 0x81200200).  Address 0x80000300 is zeroed (no IPL code
  // at SHM offset 0x300).
  ctx->uc_mcontext.regs->nip = 0x81200200;
}

void JitPPC64::EmulateISI(void* uctx)
{
  auto* ctx = static_cast<ucontext_t*>(uctx);
  u32 pc_val = static_cast<u32>(ctx->uc_mcontext.regs->nip & 0xFFFFFFFF);

  m_guest.srr0 = pc_val;
  m_guest.srr1 = m_guest.msr;
  m_guest.msr &= ~0x04EF36;

  // ISI vector (IPL file offset 0x400 → physical 0x01200300 →
  // K1 alias 0x81200300).  See EmulateDSI comment re: vector placement.
  ctx->uc_mcontext.regs->nip = 0x81200300;
}

// ---------------------------------------------------------------------------
// Paired Singles (placeholder — G5 has no PS)
// ---------------------------------------------------------------------------

void JitPPC64::EmulatePairedSingle(u32 instr, void* uctx)
{
  auto* ctx = static_cast<ucontext_t*>(uctx);
  UGeckoInstruction gecko;
  gecko.hex = instr;

  // Save real FPRs, CR, and FPSCR from the ucontext before the interpreter
  // reads stale m_ppc_state values.  Paired-single arithmetic doesn't read
  // GPRs, so we skip SaveGuestRegsFromContext (which would overwrite the
  // ucontext GPRs with stale data later if we ever copy back).
  SaveFPRsFromContext(uctx);
  m_ppc_state.cr.Set(static_cast<u32>(ctx->uc_mcontext.regs->ccr));
  {
    u64 fpscr_raw;
    std::memcpy(&fpscr_raw, &ctx->uc_mcontext.fp_regs[32], sizeof(fpscr_raw));
    m_ppc_state.fpscr.Hex = static_cast<u32>(fpscr_raw);
  }

  FallBackToInterpreter(gecko);

  // ps_* arithmetic always writes to destination FPR FD (bits 21-25),
  // and may update CR1 (Rc bit) and FPSCR (exception bits).
  RestoreFPRToContext(uctx, gecko.FD);
  ctx->uc_mcontext.regs->ccr = m_ppc_state.cr.Get();
  {
    u64 fpscr_raw = static_cast<u64>(m_ppc_state.fpscr.Hex);
    std::memcpy(&ctx->uc_mcontext.fp_regs[32], &fpscr_raw, sizeof(fpscr_raw));
  }
}

// ---------------------------------------------------------------------------
// MMIO dispatch via Dolphin's existing MMIO mapping
// ---------------------------------------------------------------------------

u32 JitPPC64::MMIORead(u32 addr, int width)
{
  auto* mmio = m_system.GetMemory().GetMMIOMapping();
  switch (width)
  {
  case 1: return mmio->Read<u8>(m_system, addr);
  case 2: return mmio->Read<u16>(m_system, addr);
  default: return mmio->Read<u32>(m_system, addr);
  }
}

void JitPPC64::MMIOWrite(u32 addr, u32 val, int width)
{
  auto* mmio = m_system.GetMemory().GetMMIOMapping();
  switch (width)
  {
  case 1: mmio->Write<u8>(m_system, addr, static_cast<u8>(val)); break;
  case 2: mmio->Write<u16>(m_system, addr, static_cast<u16>(val)); break;
  default: mmio->Write<u32>(m_system, addr, val); break;
  }
}
