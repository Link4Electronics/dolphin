// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/JitPPC64/Jit.h"

#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/ConfigManager.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/PPCAnalyst.h"
#include "Core/PowerPC/PPCTables.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

// TrampolineDispatcher — defined in JitPPC64_BackPatch.cpp
extern "C" u64 TrampolineDispatcher(PowerPC::PowerPCState* state, u32 ea,
                                     u32 is_store, u32 access_size,
                                     u32 rd, u32 ra, u64 store_value);

// PPCState field offsets (computed at init from actual struct layout)
u32 PC_OFFSET = 0;
u32 GPR_OFFSET = 0;
u32 CR_OFFSET = 0;
u32 XER_CA_OFFSET = 0;
u32 XER_SO_OV_OFFSET = 0;
u32 DOWNCOUNT_OFFSET = 0;
u32 SPR_OFFSET = 0;
u32 MSR_OFFSET = 0;
u32 PS_OFFSET = 0;

// Signal handler for MMIO backpatching
JitPPC64* g_jit_ppc64_instance = nullptr;
static struct sigaction s_old_sigsegv;

// ppcState address stored in a global so enter_code can load r12 before first dispatch
static u64 s_ppc_state_addr = 0;

// code_region address for signal handler debug output
static const u8* s_code_region = nullptr;

static void InitOffsets(const PowerPC::PowerPCState& state)
{
  const auto base = reinterpret_cast<const char*>(&state);
  PC_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.pc) - base);
  GPR_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.gpr) - base);
  CR_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.cr) - base);
  XER_CA_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.xer_ca) - base);
  XER_SO_OV_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.xer_so_ov) - base);
  DOWNCOUNT_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.downcount) - base);
  SPR_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.spr) - base);
  MSR_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.msr) - base);
  PS_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.ps) - base);
}

// Combined JIT code memory layout:
// [-- 32 MB main JIT code --][-- 4 MB trampoline area --]
static constexpr u32 JIT_CODE_SIZE = 32 * 1024 * 1024;
static constexpr u32 TRAMP_CODE_SIZE = 4 * 1024 * 1024;
static constexpr u32 COMBINED_SIZE = JIT_CODE_SIZE + TRAMP_CODE_SIZE;

static u8* AllocateCodeRegion(size_t size)
{
  void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (ptr == MAP_FAILED)
  {
    ERROR_LOG_FMT(POWERPC, "JITPPC64: failed to allocate {} bytes of RWX memory", size);
    return nullptr;
  }
  return static_cast<u8*>(ptr);
}

static void FreeCodeRegion(u8* ptr, size_t size)
{
  if (ptr)
    munmap(ptr, size);
}

// Emit a 64-bit immediate load into the trampoline assembler.
// Uses existing assembler methods to avoid manual encoding bugs.
static void TrampMOVI64(PPC64Assembler& asm_, u32 rd, u64 imm)
{
  const auto h4 = static_cast<s32>((imm >> 48) & 0xFFFF);
  const auto h3 = static_cast<u32>((imm >> 32) & 0xFFFF);
  const auto h2 = static_cast<u32>((imm >> 16) & 0xFFFF);
  const auto lo = static_cast<u32>(imm & 0xFFFF);
  asm_.ADDIS(rd, 0, h4);
  asm_.RLDICL(rd, rd, 0, 32);          // clrldi rd, rd, 32
  asm_.ORI(rd, rd, h3);
  asm_.RLDICR(rd, rd, 32, 31);          // rotl lower 32 → upper 32, keep upper half
  asm_.ORIS(rd, rd, h2);
  asm_.ORI(rd, rd, lo);
}

// Stack frame for compiled blocks (ELFv2 PPC64 ABI)
// Stack frame layout:
//   0(r1)  : backchain
//  16(r1)  : LR save (caller's frame)
//  24(r1)  : TOC save (r2)
//  32(r1)..176(r1): callee-saved r14-r31 (18 × 8 = 144 bytes)
// 176(r1)..256(r1): alignment + parameter save
static constexpr u32 FRAME_SIZE = 256;
static constexpr u32 CALLEE_SAVE_BASE = 32;

// ===========================================================================
// JitPPC64BlockCache
// ===========================================================================

void JitPPC64BlockCache::WriteLinkBlock(const JitBlock::LinkData& source, const JitBlock* dest)
{
  const Common::ScopedJITPageWriteAndNoExecute enable_jit_page_writes;
  u8* location = source.exitPtrs;

  if (dest)
  {
    s64 distance = static_cast<s64>(dest->normalEntry - location);
    if (distance >= -0x2000000LL && distance <= 0x1FFFFFFLL)
    {
      u32 li = (static_cast<u32>(distance >> 2)) & 0x00FFFFFF;
      u32 branch = (18u << 26) | li | (source.call ? 1u << 31 : 0);
      std::memcpy(location, &branch, sizeof(branch));
    }
  }
  else
  {
    auto* jit = static_cast<JitPPC64*>(&m_jit);
    if (jit->m_dispatcher_entry)
    {
      s64 dist = static_cast<s64>(jit->m_dispatcher_entry - location);
      if (dist >= -0x2000000LL && dist <= 0x1FFFFFFLL)
      {
        u32 li = (static_cast<u32>(dist >> 2)) & 0x00FFFFFF;
        u32 branch = (18u << 26) | li;
        std::memcpy(location, &branch, sizeof(branch));
        return;
      }
    }
    u32 blr = 0x4E800020;
    std::memcpy(location, &blr, sizeof(blr));
  }
}

void JitPPC64BlockCache::WriteDestroyBlock(const JitBlock& block)
{
  const Common::ScopedJITPageWriteAndNoExecute enable_jit_page_writes;
  u32 trap = 0x7FE00008;
  std::memcpy(const_cast<u8*>(block.normalEntry), &trap, sizeof(trap));
}

// ===========================================================================
// JitPPC64
// ===========================================================================

JitPPC64::JitPPC64(Core::System& system) : JitBase(system) {}
JitPPC64::~JitPPC64() { Shutdown(); }

static void SIGSEGVHandler(int sig, siginfo_t* info, void* ucontext_arg)
{
  auto* uc = static_cast<ucontext_t*>(ucontext_arg);
  auto* ctx = &uc->uc_mcontext;

  // Dump registers and instruction at fault
  u32 fault_instr = 0;
  if (ctx->CTX_NIP)
    fault_instr = *reinterpret_cast<const u32*>(ctx->CTX_NIP);
  fprintf(stderr, "JITPROBE: SIGSEGV addr=0x%lx nip=0x%lx(%+ld) r12=0x%lx instr=0x%08X\n",
          (unsigned long)info->si_addr,
          (unsigned long)ctx->CTX_NIP,
          (long)(ctx->CTX_NIP - (unsigned long)(s_code_region ? s_code_region : (u8*)0)),
          (unsigned long)ctx->regs->gpr[12],
          fault_instr);

  uintptr_t access_addr = reinterpret_cast<uintptr_t>(info->si_addr);

  if (g_jit_ppc64_instance && g_jit_ppc64_instance->HandleFault(access_addr, ctx))
  {
    fprintf(stderr, "JITPROBE: SIGSEGV HANDLED addr=0x%lx nip=0x%lx\n",
            (unsigned long)info->si_addr,
            (unsigned long)ctx->CTX_NIP);
    return;
  }

  fprintf(stderr, "JITPROBE: SIGSEGV UNHANDLED addr=0x%lx nip=0x%lx — re-raising\n",
          (unsigned long)info->si_addr,
          (unsigned long)ctx->CTX_NIP);

  // Not a JIT MMIO fault — restore default handler and re-raise so the
  // process crashes with a proper core dump / debugger notification.
  struct sigaction sa_default = {};
  sa_default.sa_handler = SIG_DFL;
  sigaction(SIGSEGV, &sa_default, nullptr);
  raise(SIGSEGV);
}

void JitPPC64::Init()
{
  RefreshConfig();
  InitOffsets(m_ppc_state);

  // Allocate combined region
  m_code_region = AllocateCodeRegion(COMBINED_SIZE);
  if (!m_code_region)
    return;

  m_code_pos = m_code_region;
  m_code_end = m_code_region + JIT_CODE_SIZE;
  m_asm.SetBase(m_code_pos, JIT_CODE_SIZE);

  // Trampoline region starts right after main code
  m_tramp_region = m_code_region + JIT_CODE_SIZE;
  m_tramp_pos = m_tramp_region;
  m_tramp_end = m_tramp_region + TRAMP_CODE_SIZE;
  m_tramp_asm.SetBase(m_tramp_pos, TRAMP_CODE_SIZE);

  gpr.SetJit(*this, m_asm, REG_PPC_BASE);

  m_block_cache.Init();
  code_block.m_stats = &js.st;
  code_block.m_gpa = &js.gpa;
  code_block.m_fpa = &js.fpa;
  jo.fastmem_arena = false;
  jo.optimizeGatherPipe = false;

  s_code_region = m_code_region;
  s_ppc_state_addr = reinterpret_cast<u64>(&m_ppc_state);
  InitBackpatch();
  CompileDispatcher();

  NOTICE_LOG_FMT(POWERPC, "JITPPC64: initialized ppcState={} (code={}, tramp={}, combined={})",
                 fmt::ptr(&m_ppc_state),
                 fmt::ptr(m_code_region), fmt::ptr(m_tramp_region), COMBINED_SIZE);

  // Install SIGSEGV handler for MMIO backpatching
  g_jit_ppc64_instance = this;
  struct sigaction sa = {};
  sa.sa_sigaction = SIGSEGVHandler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, &s_old_sigsegv);
}

void JitPPC64::Shutdown()
{
  // Restore old SIGSEGV handler
  sigaction(SIGSEGV, &s_old_sigsegv, nullptr);
  g_jit_ppc64_instance = nullptr;

  ShutdownBackpatch();
  m_block_cache.Shutdown();
  m_fault_to_handler.clear();
  FreeCodeRegion(m_code_region, COMBINED_SIZE);
  m_code_region = nullptr;
  m_code_pos = nullptr;
  m_code_end = nullptr;
  m_tramp_region = nullptr;
  m_tramp_pos = nullptr;
  m_tramp_end = nullptr;
  m_enter_code = nullptr;
  m_dispatcher_entry = nullptr;
}

void JitPPC64::ClearCache()
{
  m_block_cache.Clear();
  m_code_pos = m_code_region;
  m_code_end = m_code_region + JIT_CODE_SIZE;
  m_asm.SetBase(m_code_pos, JIT_CODE_SIZE);
  m_tramp_pos = m_tramp_region;
  m_tramp_end = m_tramp_region + TRAMP_CODE_SIZE;
  m_tramp_asm.SetBase(m_tramp_pos, TRAMP_CODE_SIZE);
  m_fault_to_handler.clear();
  m_enter_code = nullptr;
  m_dispatcher_entry = nullptr;
  CompileDispatcher();
}

void JitPPC64::CompileDispatcher()
{
  m_asm.SetBase(m_code_pos, static_cast<size_t>(m_code_end - m_code_pos));

  // ── enter_code (called from Run()) ────────────────────────────────────
  // Sets r12 = &ppcState, then falls through to the dispatcher.
  // On PPC64, r12 is REG_PPC_BASE — the base pointer for all ppcState
  // loads/stores.  The block prolog also re-establishes it, but the
  // dispatcher itself needs r12 to read pc/downcount from ppcState.
  //
  // Mirror of Jit64's JitAsm.cpp: MOV(64, R(RPPCSTATE), Imm64(&ppc_state)).

  m_enter_code = m_code_pos;
  TrampMOVI64(m_asm, 11, reinterpret_cast<u64>(&s_ppc_state_addr));
  m_asm.LD(REG_PPC_BASE, 11, 0);     // r12 = *(s_ppc_state_addr)

  // Save r10 on a small stack frame — r10 is volatile in ELFv2, but
  // compiled blocks clobber it (CompileBC uses it as a not-taken flag).
  // We restore it before BCTR (block entry) or BLR (exit) so that Run()'s
  // C++ code always sees the original r10 value.
  m_asm.STDU(1, 1, -32);
  m_asm.STD(10, 1, 24);

  // ── dispatcher entry (called from block epilogs) ──────────────────────
  // Register state at entry:
  //   r12      = &ppcState  (re-established by enter_code or previous block's prolog)
  //   r14-r31  = host callee-saved regs (preserved by block epilog)
  //   LR       = return address inside Run()
  //   r1       = host stack — 32 bytes allocated above (r10 saved at r1+24)
  //
  // We emit branches with placeholder offsets and patch them after we know
  // all positions (the assembler has no label support).

  m_dispatcher_entry = m_asm.Code() + m_asm.Size();
  m_asm.MFLR(14);                              // save Run_LR in r14

  m_asm.LWZ(11, REG_PPC_BASE, DOWNCOUNT_OFFSET);
  m_asm.CMPWI(0, 11, 0);

  // ble exit (branch if downcount ≤ 0) — placeholder
  const u8* ble_pos = m_asm.Code() + m_asm.Size();
  m_asm.BC(4, 1, 0);  // BO=4 (false), BI=1 (GT) → "branch if not GT" = ble

  // r3 = ppcState.pc → call JitPPC64Dispatch(pc)
  m_asm.LWZ(3, REG_PPC_BASE, PC_OFFSET);
  TrampMOVI64(m_asm, 12, reinterpret_cast<u64>(&JitPPC64Dispatch));
  m_asm.MTCTR(12);
  m_asm.BCTRL();

  // beq exit (branch if r3 == 0, i.e. block not found) — placeholder
  m_asm.CMPLDI(0, 3, 0);
  const u8* beq_pos = m_asm.Code() + m_asm.Size();
  m_asm.BC(12, 2, 0);  // BO=12 (true), BI=2 (EQ) → beq

  // ── Success path: jump to block entry ──────────────────────────────────
  m_asm.LD(10, 1, 24);   // restore r10 from dispatcher frame
  m_asm.ADDI(1, 1, 32);  // tear down dispatcher frame
  m_asm.MTLR(14);        // restore Run_LR (block prolog will re-save it)
  m_asm.MTCTR(3);        // block → CTR
  m_asm.BCTR();          // jump to block (LR preserved = Run_LR)

  // ── Exit path: return to Run() ────────────────────────────────────────
  const u8* exit_pos = m_asm.Code() + m_asm.Size();
  m_asm.LD(10, 1, 24);   // restore r10
  m_asm.ADDI(1, 1, 32);  // tear down dispatcher frame
  m_asm.MTLR(14);        // restore Run_LR
  m_asm.BLR();

  // ── Patch placeholder branch offsets ───────────────────────────────────
  s32 ble_bd = static_cast<s32>(exit_pos - ble_pos);   // byte distance
  s32 beq_bd = static_cast<s32>(exit_pos - beq_pos);

  // BC encoding: |16(6)|BO(5)|BI(5)|BD(14b)|AA(1)|LK(1)|
  // BD at u32 bits 15:2, in word-offset units (hardware multiplies by 4).
  // The assembler stores (bd >> 2) & mask at bits 15:2.
  u32 ble_enc = (16u << 26) | (4u << 21) | (1u << 16) |
                (((ble_bd >> 2) & 0x3FFF) << 2);
  u32 beq_enc = (16u << 26) | (12u << 21) | (2u << 16) |
                (((beq_bd >> 2) & 0x3FFF) << 2);
  std::memcpy(const_cast<u8*>(ble_pos), &ble_enc, sizeof(ble_enc));
  std::memcpy(const_cast<u8*>(beq_pos), &beq_enc, sizeof(beq_enc));

  // ── Shared block exit sequence ─────────────────────────────────────────
  // This is branched to from EVERY compiled block (both via the epilog and
  // directly from branch compilers).  It restores host registers that the
  // block may have clobbered, tears down the block's stack frame, restores
  // LR from the parent frame, and BLRs back to Run().
  //
  // r1 must point to the block's frame (i.e., before frame tear-down).
  // r12 must be REG_PPC_BASE (&ppcState) for the STW/STB in the epilog.
  // PC_OFFSET must have already been written with the next guest PC.
  m_exit_sequence = m_asm.Code() + m_asm.Size();
  m_asm.LD(10, 1, static_cast<s32>(CALLEE_SAVE_BASE + (31 - 14 + 1) * 8));
  for (u32 i = 14; i <= 31; ++i)
    m_asm.LD(i, 1, static_cast<s32>(CALLEE_SAVE_BASE + (i - 14) * 8));
  m_asm.ADDI(1, 1, FRAME_SIZE);
  m_asm.LD(REG_SCRATCH, 1, 16);
  m_asm.MTLR(REG_SCRATCH);
  m_asm.BLR();

  m_code_pos = const_cast<u8*>(m_asm.Code() + m_asm.Size());
}

// ===========================================================================
// Prolog / Epilog
// ===========================================================================

void JitPPC64::EmitProlog()
{
  m_asm.MFLR(REG_SCRATCH);
  m_asm.STD(REG_SCRATCH, 1, 16);
  m_asm.STDU(1, 1, -static_cast<s32>(FRAME_SIZE));

  // Save volatile register r10 (clobbered by CompileBC as not-taken flag)
  // and callee-saved registers r14-r31 (used by RegCache)
  m_asm.STD(10, 1, static_cast<s32>(CALLEE_SAVE_BASE + (31 - 14 + 1) * 8));
  for (u32 i = 14; i <= 31; ++i)
    m_asm.STD(i, 1, static_cast<s32>(CALLEE_SAVE_BASE + (i - 14) * 8));

  u64 addr = reinterpret_cast<u64>(&m_ppc_state);
  if (addr > 0xFFFFFFFFULL)
  {
    u32 hi = static_cast<u32>(addr >> 32);
    u32 lo = static_cast<u32>(addr & 0xFFFFFFFF);
    // Load upper 32 bits (hi) into lower 32 of r12 using ADDIS (sign-extends but
    // RLDICR below will clear the upper 64-bit half, so it's harmless).
    m_asm.ADDIS(REG_PPC_BASE, 0, static_cast<s32>(hi >> 16));
    m_asm.ORI(REG_PPC_BASE, REG_PPC_BASE, hi & 0xFFFF);
    // Shift hi to the upper 32 bits; lower 32 are now zero.
    m_asm.RLDICR(REG_PPC_BASE, REG_PPC_BASE, 32, 31);
    // Add lo using ORIS/ORI — never sign-extends, so works even when
    // lo>>16 or lo&0xFFFF has bit 15 set (which ADDI/ADDIS would sign-extend).
    m_asm.ORIS(REG_PPC_BASE, REG_PPC_BASE, static_cast<u32>(lo >> 16));
    m_asm.ORI(REG_PPC_BASE, REG_PPC_BASE, lo & 0xFFFF);
  }
  else
  {
    m_asm.ORIS(REG_PPC_BASE, 0, static_cast<u32>(addr >> 16));
    m_asm.ORI(REG_PPC_BASE, REG_PPC_BASE, static_cast<u32>(addr & 0xFFFF));
  }

  gpr.Reset();
}

void JitPPC64::EmitEpilog(u32 next_pc)
{
  gpr.Flush();

  m_asm.LI32(REG_SCRATCH, next_pc);
  m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PC_OFFSET));

  // Branch to the shared exit sequence (register restore + frame tear-down + BLR)
  m_asm.BRel(m_exit_sequence);
}

// ===========================================================================
// Load/store helpers
// ===========================================================================

void JitPPC64::LoadGPR(u32 host_reg, u32 guest_reg)
{
  m_asm.LWZ(host_reg, REG_PPC_BASE, static_cast<s32>(GPR_OFFSET + 4 * guest_reg));
}

void JitPPC64::StoreGPR(u32 guest_reg, u32 host_reg)
{
  m_asm.STW(host_reg, REG_PPC_BASE, static_cast<s32>(GPR_OFFSET + 4 * guest_reg));
}

// ===========================================================================
// CR0 update (for RC-bit instructions)
// Clobbers r0, r11
// ===========================================================================

void JitPPC64::EmitCR0Update()
{
  m_asm.EXTSW(REG_SCRATCH2, REG_SCRATCH);
  m_asm.CMPWI(0, REG_SCRATCH2, 0);
  m_asm.MFCR(REG_SCRATCH2);
  m_asm.LBZ(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(XER_SO_OV_OFFSET));
  m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 0, 30, 30);
  m_asm.RLWIMI(REG_SCRATCH2, REG_SCRATCH, 27, 3, 3);
  m_asm.MTCRF(0x80, REG_SCRATCH2);
}

// ===========================================================================
// EmitBackpatchRoutine — emit fast path + trampoline slow path
//
// Called from CompileLoadStore after the EA is in REG_SCRATCH2 (r11) and
// the data register is loaded/stored.  We emit:
//   1. The fast path instruction (LWZ/STW/LBZ/etc.) in main code
//   2. A corresponding slow path in the trampoline region that:
//      - Saves volatile registers
//      - Calls TrampolineDispatcher with (ppcState*, EA, instruction, next_jit)
//      - For loads: stores the returned value to data_reg
//      - Restores registers
//      - Branches back to the instruction after the fast path
//
// On fault, the fast path is patched with `b slow_path_entry`.
// ===========================================================================
void JitPPC64::EmitBackpatchRoutine(u32 access_size, u32 opcd, u32 rd,
                                     u32 ra, u32 data_reg, bool is_load,
                                     bool is_fpr)
{
  // 1. Record fast path position
  const u8* fast_start = m_asm.Code() + m_asm.Size();

  // Emit the fast path access instruction
  if (is_fpr)
  {
    // FPU loads/stores go through FPR 0
    if (is_load)
    {
      if (access_size == 32)
        m_asm.LFS(0, REG_SCRATCH2, 0);
      else
        m_asm.LFD(0, REG_SCRATCH2, 0);
    }
    else
    {
      if (access_size == 32)
        m_asm.STFS(0, REG_SCRATCH2, 0);
      else
        m_asm.STFD(0, REG_SCRATCH2, 0);
    }
  }
  else
  {
    switch (access_size)
    {
    case 8:
      if (is_load)  m_asm.LBZ(data_reg, REG_SCRATCH2, 0);
      else          m_asm.STB(data_reg, REG_SCRATCH2, 0);
      break;
    case 16:
      if (is_load)  m_asm.LHZ(data_reg, REG_SCRATCH2, 0);
      else          m_asm.STH(data_reg, REG_SCRATCH2, 0);
      break;
    case 32:
      if (is_load)  m_asm.LWZ(data_reg, REG_SCRATCH2, 0);
      else          m_asm.STW(data_reg, REG_SCRATCH2, 0);
      break;
    default:
      return;
    }
  }
  const u8* fast_end = m_asm.Code() + m_asm.Size();

  // 2. Emit slow path in trampoline region
  const u8* slow_entry = m_tramp_pos;
  m_tramp_asm.SetBase(m_tramp_pos, static_cast<size_t>(m_tramp_end - m_tramp_pos));

  // Save volatile registers (r0, r3-r10) + LR
  m_tramp_asm.MFLR(0);
  m_tramp_asm.STDU(REG_SCRATCH, 1, -128);
  m_tramp_asm.STD(0, 1, 120);   // save LR
  m_tramp_asm.STD(3, 1, 112);
  m_tramp_asm.STD(4, 1, 104);
  m_tramp_asm.STD(5, 1, 96);
  m_tramp_asm.STD(6, 1, 88);
  m_tramp_asm.STD(7, 1, 80);
  m_tramp_asm.STD(8, 1, 72);
  m_tramp_asm.STD(9, 1, 64);
  m_tramp_asm.STD(10, 1, 56);

  // Arguments to TrampolineDispatcher:
  //   r3 = ppcState* (from r12)
  //   r4 = EA         (from r11)
  //   r5 = is_store   (0=load, 1=store)
  //   r6 = access_size (8/16/32)
  //   r7 = PPC register rd
  //   r8 = PPC register ra
  m_tramp_asm.MR(3, REG_PPC_BASE);
  m_tramp_asm.MR(4, REG_SCRATCH2);

  m_tramp_asm.LI(5, is_load ? 0 : 1);
  m_tramp_asm.LI(6, static_cast<s32>(access_size));
  m_tramp_asm.LI(7, static_cast<s32>(rd));
  m_tramp_asm.LI(8, static_cast<s32>(ra));

  // For stores: pass the actual value in r9 (avoids reading stale ppcState)
  if (!is_load)
  {
    if (is_fpr)
    {
      // FPU store: save f0 to stack and load into r9 as u64
      m_tramp_asm.STFD(0, 1, -8);
      m_tramp_asm.LD(9, 1, -8);
    }
    else
    {
      m_tramp_asm.MR(9, data_reg);
    }
  }

  // Save REG_PPC_BASE (r12) before the call — TrampolineDispatcher clobbers
  // it, but we need ppcState after the register restore to reload loaded
  // values for both integer (gpr[rd]) and FPU (ps[rd]) loads.
  m_tramp_asm.STD(REG_PPC_BASE, 1, 48);

  // Call TrampolineDispatcher via absolute address
  TrampMOVI64(m_tramp_asm, 12,
              reinterpret_cast<u64>(&TrampolineDispatcher));
  m_tramp_asm.MTCTR(12);
  m_tramp_asm.BCTRL();

  // Restore registers
  m_tramp_asm.LD(10, 1, 56);
  m_tramp_asm.LD(9, 1, 64);
  m_tramp_asm.LD(8, 1, 72);
  m_tramp_asm.LD(7, 1, 80);
  m_tramp_asm.LD(6, 1, 88);
  m_tramp_asm.LD(5, 1, 96);
  m_tramp_asm.LD(4, 1, 104);
  m_tramp_asm.LD(3, 1, 112);
  m_tramp_asm.LD(0, 1, 120);
  m_tramp_asm.MTLR(0);

  // Restore ppcState pointer (clobbered by TrampolineDispatcher)
  m_tramp_asm.LD(REG_PPC_BASE, 1, 48);

  m_tramp_asm.ADDI(REG_SCRATCH, 1, 128);
  m_tramp_asm.MR(1, REG_SCRATCH);

  // Reload the result from ppcState: TrampolineDispatcher wrote
  // state->gpr[rd] (integer) or state->ps[rd] (FPU), but the
  // register restore above would have clobbered data_reg with the
  // pre-fault value.  Reading from ppcState after the restore
  // correctly recovers the loaded value.
  if (is_load)
  {
    if (is_fpr)
    {
      m_tramp_asm.LFD(0, REG_PPC_BASE,
                       static_cast<s32>(PS_OFFSET + rd * 16));
    }
    else
    {
      // Integer load: use r0 (REG_SCRATCH = data_reg) for the result.
      // MOVI64 below uses r11 (REG_SCRATCH2), NOT r0, so the loaded
      // value in r0 survives to the fast_end continuation code.
      m_tramp_asm.LWZ(REG_SCRATCH, REG_PPC_BASE,
                       static_cast<s32>(GPR_OFFSET + rd * 4));
    }
  }

  // Branch back to the instruction after the fast path.
  // Use r11 (REG_SCRATCH2) for the target address — r0 (REG_SCRATCH)
  // holds the loaded value for integer loads and must survive.
  TrampMOVI64(m_tramp_asm, REG_SCRATCH2,
              reinterpret_cast<u64>(fast_end));
  m_tramp_asm.MTCTR(REG_SCRATCH2);
  m_tramp_asm.BCTR();

  // 3. Record the mapping
  u32 tramp_size = static_cast<u32>(m_tramp_asm.Size());
  m_tramp_pos += tramp_size;

  FastmemArea area;
  area.fast_access_code = fast_start;
  area.slow_access_code = slow_entry;
  area.is_load = is_load;
  area.rd = rd;
  area.ra = ra;
  m_fault_to_handler[fast_end] = area;

  // Flush trampoline icache
  __builtin___clear_cache(const_cast<u8*>(slow_entry),
                           const_cast<u8*>(slow_entry + tramp_size));
}

// ===========================================================================
// Jit() — compile a block
// ===========================================================================

void JitPPC64::Jit(u32 em_address)
{
  Jit(em_address, true);
}

void JitPPC64::Jit(u32 em_address, bool clear_cache_and_retry_on_failure)
{
  if (SConfig::GetInstance().bJITNoBlockCache)
    ClearCache();

  const u32 nextPC = analyzer.Analyze(em_address, &code_block, &m_code_buffer,
                                      static_cast<u32>(m_code_buffer.size()));

  if (code_block.m_memory_exception)
    return;

  // Only compile blocks where ALL instructions can be JITted
  for (u32 i = 0; i < code_block.m_num_instructions; ++i)
  {
    if (m_code_buffer[i].skip)
      continue;
    if (!CanCompileInstruction(m_code_buffer[i].inst))
    {
      NOTICE_LOG_FMT(POWERPC, "JITPPC64: can't compile block at {:08x} (instr {:08x} opcd={} at +{})",
                     em_address, m_code_buffer[i].inst.hex, m_code_buffer[i].inst.OPCD, i);
      return;
    }
  }

  size_t estimated_size = code_block.m_num_instructions * 64;
  if (m_code_pos + estimated_size > m_code_end)
  {
    if (clear_cache_and_retry_on_failure)
    {
      ClearCache();
      Jit(em_address, false);
    }
    return;
  }

  JitBlock* b = m_block_cache.AllocateBlock(em_address);
  if (!b)
    return;

  u8* block_start = m_code_pos;
  m_asm.SetBase(m_code_pos, static_cast<size_t>(m_code_end - m_code_pos));

  b->normalEntry = block_start;
  b->near_begin = block_start;
  b->near_end = block_start;

  EmitProlog();

  for (u32 i = 0; i < code_block.m_num_instructions; ++i)
  {
    PPCAnalyst::CodeOp& op = m_code_buffer[i];
    js.compilerPC = op.address;
    js.op = &op;

    if (op.skip)
      continue;

    CompileInstruction(op);
    i += js.skipInstructions;
    js.skipInstructions = 0;
  }

  EmitEpilog(nextPC);

  u8* block_end = m_code_pos + m_asm.Size();
  __builtin___clear_cache(block_start, block_end);

  b->near_end = block_end;

  m_block_cache.FinalizeBlock(*b, jo.enableBlocklink, code_block, m_code_buffer);
  m_code_pos = block_end;

  fprintf(stderr, "JITPROBE: compiled block at 0x%08X, %u instrs, size=%zu bytes (code_pos=0x%lx, block_start=0x%lx, block_end=0x%lx), next=0x%08X\n",
          em_address, code_block.m_num_instructions, m_asm.Size(),
          (unsigned long)m_code_pos,
          (unsigned long)block_start,
          (unsigned long)block_end,
          nextPC);
  for (u32 i = 0; i < code_block.m_num_instructions; ++i)
  {
    if (!m_code_buffer[i].skip)
    {
      u32 addr = m_code_buffer[i].address;
      u32 hex = m_code_buffer[i].inst.hex;
      fprintf(stderr, "  [%02u] 0x%08X: 0x%08X\n", i, addr, hex);
    }
  }
}

// ===========================================================================
// Run / SingleStep
// ===========================================================================

void JitPPC64::Run()
{
  auto& core_timing = m_system.GetCoreTiming();
  auto& cpu = m_system.GetCPU();

  while (cpu.GetState() == CPU::State::Running)
  {
    core_timing.Advance();

    while (m_ppc_state.downcount > 0 && cpu.GetState() == CPU::State::Running)
    {
      // Try to find or compile a block for the current PC
      if (!m_block_cache.GetBlockFromStartAddress(m_ppc_state.pc,
                                                    m_ppc_state.feature_flags))
      {
        Jit(m_ppc_state.pc);
        if (!m_block_cache.GetBlockFromStartAddress(m_ppc_state.pc,
                                                      m_ppc_state.feature_flags))
        {
          // Can't compile — fall back to interpreter
          m_system.GetInterpreter().SingleStep();
          m_ppc_state.downcount -= 1;
          continue;
        }
      }

      // Enter the JIT code via enter_code, which sets r12 = &ppcState and
      // falls through to the dispatcher.  The dispatcher chains blocks
      // internally (via JitPPC64Dispatch + block linking) until
      // downcount ≤ 0, then returns to Run().
      fprintf(stderr, "JITPROBE: Run() calling enter_code at pc=0x%08X downcount=%d\n",
              m_ppc_state.pc, m_ppc_state.downcount);
      reinterpret_cast<void (*)()>(m_enter_code)();
      fprintf(stderr, "JITPROBE: Run() returned from enter_code at pc=0x%08X downcount=%d\n",
              m_ppc_state.pc, m_ppc_state.downcount);
    }
  }
}

void JitPPC64::SingleStep()
{
  m_system.GetCoreTiming().Advance();
  m_system.GetInterpreter().SingleStep();
  m_ppc_state.downcount -= 1;
}

// ===========================================================================
// Overrides
// ===========================================================================

void JitPPC64::EraseSingleBlock(const JitBlock& block)
{
  m_block_cache.EraseSingleBlock(block);
}

std::vector<JitBase::MemoryStats> JitPPC64::GetMemoryStats() const { return {}; }
std::size_t JitPPC64::DisassembleNearCode(const JitBlock& block, std::ostream& stream) const { return 0; }
std::size_t JitPPC64::DisassembleFarCode(const JitBlock& block, std::ostream& stream) const { return 0; }

bool JitPPC64::CompileTable31(UGeckoInstruction inst)
{
  if (CompileTable31_Integer(inst))
    return true;
  if (CompileTable31_SystemReg(inst))
    return true;
  if (CompileTable31_LoadStore(inst))
    return true;
  if (CompileTable31_CA(inst))
    return true;
  return CompileMisc(inst);
}

void JitPPC64::FallBackToInterpreter(UGeckoInstruction inst) {}
void JitPPC64::DoNothing(UGeckoInstruction inst) {}
void JitPPC64::UnknownInstruction(UGeckoInstruction inst) {}

// Stubs
void JitPPC64::LoadCR(u32 host_reg) {}
void JitPPC64::StoreCR(u32 host_reg) {}
void JitPPC64::EmitCarryFromReg() {}
void JitPPC64::EmitBackpatchSlot() {}
