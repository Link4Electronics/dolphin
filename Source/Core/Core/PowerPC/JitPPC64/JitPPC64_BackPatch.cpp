#include "Core/PowerPC/JitPPC64/Jit.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

// ===========================================================================
// BackPatch infrastructure
//
// When a compiled load/store instruction accesses an unmapped address
// (typically MMIO), SIGSEGV fires. We:
//   1. Look up the instruction in our backpatch table
//   2. Compute the effective address + value from the host register file
//      (via the regcache mapping and the ucontext's pt_regs)
//   3. Call the appropriate Memory::Read/Write (which handles MMIO)
//   4. Rewrite the load/store to NOP so the CPU skips it on re-execution
//   5. For loads: write the loaded value to ppcState.gpr[rd] and
//      invalidate the regcache entry so subsequent JIT code reloads
//      from the updated ppcState
// ===========================================================================

struct BackPatchEntry
{
  const u8* code_addr;     // address of compiled instruction
  u32 guest_pc;            // PPC PC
  u32 guest_address;       // accessed address (0 = unknown at compile time)
  u32 original_inst;       // original PPC instruction word
  u32 rd;                  // dest/src register register field
};

static std::vector<BackPatchEntry> s_backpatch_entries;
static bool s_backpatch_initialized = false;

void JitPPC64::InitBackpatch()
{
  s_backpatch_entries.clear();
  s_backpatch_entries.reserve(4096);
  s_backpatch_initialized = true;
}

void JitPPC64::ShutdownBackpatch()
{
  s_backpatch_entries.clear();
  s_backpatch_initialized = false;
}

void JitPPC64::AddBackpatchEntry(const u8* code_addr, u32 guest_pc, u32 guest_address,
                                  u32 original_inst, u32 rd)
{
  s_backpatch_entries.push_back({code_addr, guest_pc, guest_address, original_inst, rd});
}

// ---------------------------------------------------------------------------
// Read a PPC GPR value from the signal context.
//
// The compiled code caches PPC GPRs in host r14-r31. The regcache mapping
// tells us which host register holds each PPC GPR.  The signal context
// (ucontext → mcontext → pt_regs) contains the saved host register file
// at the moment of the fault, so we can read any PPC GPR's value directly
// from the host register without needing to flush the cache first.
// ---------------------------------------------------------------------------
static u32 ReadPPCGPR(const JitPPC64RegCache& cache, const SContext* ctx, u32 ppc_reg)
{
  // Check the regcache: which host register holds this PPC GPR?
  for (u32 i = 0; i < JitPPC64RegCache::NUM_CACHED_REGS; ++i)
  {
    if (cache.m_entries[i].ppc_reg == ppc_reg)
    {
      // Found in cache → read from the host register saved in pt_regs
      const u32 host_reg = JitPPC64RegCache::FIRST_CACHED_REG + i;
      return static_cast<u32>(ctx->regs->gpr[host_reg]);
    }
  }
  // Not cached → the canonical value is already in ppcState
  return 0;  // caller must check ppcState directly
}

bool JitPPC64::HandleFault(uintptr_t access_address, SContext* ctx)
{
  // Find the backpatch entry for this code address
  auto it = std::find_if(s_backpatch_entries.begin(), s_backpatch_entries.end(),
                         [ctx](const BackPatchEntry& e) {
                           return e.code_addr ==
                                  reinterpret_cast<const u8*>(ctx->CTX_NIP);
                         });

  if (it == s_backpatch_entries.end())
    return false;

  const UGeckoInstruction inst(it->original_inst);
  const u32 opcd = inst.OPCD;

  // ---- Decode effective address (EA) ----
  u32 ea;
  u32 ra_val = 0;
  bool is_load = false;
  bool is_update = false;

  if (opcd >= 32 && opcd <= 55)
  {
    // D-form: EA = (RA ? GPR[RA] : 0) + sign_ext(d)
    const s32 d = static_cast<s32>(static_cast<s16>(inst.SIMM_16));
    if (inst.RA)
      ra_val = ReadPPCGPR(gpr, ctx, inst.RA);
    ea = ra_val + d;

    // Detect update forms (lwzu = 33, lbzu = 35, stwu = 37, stbu = 39,
    //                     lhzu = 41, lhau = 43, lfsu = 49, lfdu = 51,
    //                     stfsu = 53, stfdu = 55)
    is_update = (opcd & 1);
  }
  else if (opcd == 31)
  {
    // X-form: EA = (RA ? GPR[RA] : 0) + GPR[RB]
    if (inst.RA)
      ra_val = ReadPPCGPR(gpr, ctx, inst.RA);
    const u32 rb_val = ReadPPCGPR(gpr, ctx, inst.RB);
    ea = ra_val + rb_val;

    // Update forms have bits 11-12? Actually for X-form, update forms are
    // specific XO values (55=lwzux, 119=lbzux, 183=stwux, 247=stbux,
    // 311=lhzux, 375=lhaux, 567=lfsux, 631=lfdux, 695=stfsux, 759=stfdux)
    const u32 xo = inst.SUBOP10;
    switch (xo)
    {
    case 55: case 119: case 183: case 247: case 311: case 375:
    case 567: case 631: case 695: case 759:
      is_update = true;
      break;
    }
  }
  else
  {
    ERROR_LOG_FMT(POWERPC, "BackPatch: unsupported opcd {}", opcd);
    return true;
  }

  // ---- Handle the access based on instruction type ----
  const Common::ScopedJITPageWriteAndNoExecute enable_jit_page_writes;
  const u32 nop = 0x60000000;  // ori r0, r0, 0

  if (opcd >= 32 && opcd <= 55)
  {
    // ---- D-form loads/stores ----
    switch (opcd)
    {
    // Integer loads
    case 32: // lwz
    case 33: // lwzu
      is_load = true;
      m_ppc_state.gpr[inst.RD] = m_system.GetMemory().Read_U32(ea);
      break;
    case 34: // lbz
    case 35: // lbzu
      is_load = true;
      m_ppc_state.gpr[inst.RD] = m_system.GetMemory().Read_U8(ea);
      break;
    case 40: // lhz
    case 41: // lhzu
      is_load = true;
      m_ppc_state.gpr[inst.RD] = m_system.GetMemory().Read_U16(ea);
      break;
    case 42: // lha
    case 43: // lhau
      is_load = true;
      m_ppc_state.gpr[inst.RD] = static_cast<u32>(static_cast<s32>(static_cast<s16>(m_system.GetMemory().Read_U16(ea))));
      break;

    // Integer stores
    case 36: // stw
    case 37: // stwu
      m_system.GetMemory().Write_U32(ReadPPCGPR(gpr, ctx, inst.RS), ea);
      break;
    case 38: // stb
    case 39: // stbu
      m_system.GetMemory().Write_U8(ReadPPCGPR(gpr, ctx, inst.RS), ea);
      break;
    case 44: // sth
    case 45: // sthu
      m_system.GetMemory().Write_U16(ReadPPCGPR(gpr, ctx, inst.RS), ea);
      break;

    // FPU loads
    case 48: // lfs
    case 49: // lfsu
      is_load = true;
      {
        float val;
        m_system.GetMemory().CopyFromEmu(&val, ea, sizeof(val));
        m_ppc_state.ps[inst.RD].SetPS0(static_cast<double>(val));;
      }
      break;
    case 50: // lfd
    case 51: // lfdu
      is_load = true;
      {
        double val;
        m_system.GetMemory().CopyFromEmu(&val, ea, sizeof(val));
        m_ppc_state.ps[inst.RD].SetPS0(val);
      }
      break;

    // FPU stores
    case 52: // stfs
    case 53: // stfsu
      {
        const float val = static_cast<float>(m_ppc_state.ps[inst.RD].PS0AsDouble());
        m_system.GetMemory().CopyToEmu(ea, &val, sizeof(val));
      }
      break;
    case 54: // stfd
    case 55: // stfdu
      {
        const double val = m_ppc_state.ps[inst.RD].PS0AsDouble();
        m_system.GetMemory().CopyToEmu(ea, &val, sizeof(val));
      }
      break;

    default:
      ERROR_LOG_FMT(POWERPC, "BackPatch: unhandled D-form opcd {}", opcd);
      std::memcpy(const_cast<u8*>(it->code_addr), &nop, sizeof(nop));
      return true;
    }
  }
  else
  {
    // ---- X-form (opcd 31) indexed loads/stores ----
    const u32 xo = inst.SUBOP10;
    switch (xo)
    {
    // Integer indexed loads
    case 23:   // lwzx
    case 55:   // lwzux
      is_load = true;
      m_ppc_state.gpr[inst.RD] = m_system.GetMemory().Read_U32(ea);
      break;
    case 87:   // lbzx
    case 119:  // lbzux
      is_load = true;
      m_ppc_state.gpr[inst.RD] = m_system.GetMemory().Read_U8(ea);
      break;
    case 279:  // lhzx
    case 311:  // lhzux
      is_load = true;
      m_ppc_state.gpr[inst.RD] = m_system.GetMemory().Read_U16(ea);
      break;
    case 343:  // lhax
    case 375:  // lhaux
      is_load = true;
      m_ppc_state.gpr[inst.RD] = static_cast<u32>(static_cast<s32>(static_cast<s16>(m_system.GetMemory().Read_U16(ea))));
      break;

    // Integer indexed stores
    case 151:  // stwx
    case 183:  // stwux
      m_system.GetMemory().Write_U32(ReadPPCGPR(gpr, ctx, inst.RD), ea);
      break;
    case 215:  // stbx
    case 247:  // stbux
      m_system.GetMemory().Write_U8(ReadPPCGPR(gpr, ctx, inst.RD), ea);
      break;
    case 407:  // sthx
    case 439:  // sthux
      m_system.GetMemory().Write_U16(ReadPPCGPR(gpr, ctx, inst.RD), ea);
      break;

    // FPU indexed loads
    case 535:  // lfsx
    case 567:  // lfsux
      is_load = true;
      {
        float val;
        m_system.GetMemory().CopyFromEmu(&val, ea, sizeof(val));
        m_ppc_state.ps[inst.RD].SetPS0(static_cast<double>(val));;
      }
      break;
    case 599:  // lfdx
    case 631:  // lfdux
      is_load = true;
      {
        double val;
        m_system.GetMemory().CopyFromEmu(&val, ea, sizeof(val));
        m_ppc_state.ps[inst.RD].SetPS0(val);
      }
      break;

    // FPU indexed stores
    case 663:  // stfsx
    case 695:  // stfsux
      {
        const float val = static_cast<float>(m_ppc_state.ps[inst.RD].PS0AsDouble());
        m_system.GetMemory().CopyToEmu(ea, &val, sizeof(val));
      }
      break;
    case 727:  // stfdx
    case 759:  // stfdux
      {
        const double val = m_ppc_state.ps[inst.RD].PS0AsDouble();
        m_system.GetMemory().CopyToEmu(ea, &val, sizeof(val));
      }
      break;

    // Byte-reversed loads
    case 534:  // lwbrx
      is_load = true;
      m_ppc_state.gpr[inst.RD] = Common::swap32(m_system.GetMemory().Read_U32(ea));
      break;
    case 790:  // lhbrx
      is_load = true;
      m_ppc_state.gpr[inst.RD] = Common::swap16(m_system.GetMemory().Read_U16(ea));
      break;

    // Byte-reversed stores
    case 662:  // stwbrx
      m_system.GetMemory().Write_U32(Common::swap32(ReadPPCGPR(gpr, ctx, inst.RD)), ea);
      break;
    case 918:  // sthbrx
      m_system.GetMemory().Write_U16(Common::swap16(ReadPPCGPR(gpr, ctx, inst.RD)), ea);
      break;

    // stfiwx — store FPR as integer word
    case 983:  // stfiwx
      {
        const double val = m_ppc_state.ps[inst.RD].PS0AsDouble();
        u32 ival;
        std::memcpy(&ival, &val, sizeof(u32));
        m_system.GetMemory().Write_U32(ival, ea);
      }
      break;

    default:
      ERROR_LOG_FMT(POWERPC, "BackPatch: unhandled XO {}", xo);
      std::memcpy(const_cast<u8*>(it->code_addr), &nop, sizeof(nop));
      return true;
    }
  }

  // ---- Handle update forms ----
  if (is_update)
  {
    // Both D-form and X-form update form write the EA back to RA
    m_ppc_state.gpr[inst.RA] = ea;
  }

  // ---- Invalidate regcache for modified registers ----
  // For loads: the loaded value was written to ppcState.gpr[rd] but the
  // regcache has a stale copy.  Invalidate so the next JIT instruction
  // reloads from ppcState.
  if (is_load)
  {
    for (auto& e : gpr.m_entries)
    {
      if (e.ppc_reg == inst.RD)
      {
        e.ppc_reg = JitPPC64RegCache::REG_INVALID;
        break;
      }
    }
  }

  // For update forms: RA was also modified
  if (is_update && inst.RA != 0)
  {
    for (auto& e : gpr.m_entries)
    {
      if (e.ppc_reg == inst.RA)
      {
        e.ppc_reg = JitPPC64RegCache::REG_INVALID;
        break;
      }
    }
  }

  // ---- Rewrite the faulting instruction to NOP ----
  // The CPU will re-execute the instruction after the signal handler
  // returns.  NOP (ori r0, r0, 0) is harmless and execution continues
  // to the next real instruction.
  //
  // We must NOT leave the original load/store because it would fault
  // forever on the same address.
  std::memcpy(const_cast<u8*>(it->code_addr), &nop, sizeof(nop));

  // Flush icache on PPC64 — data cache write doesn't automatically
  // invalidate the instruction cache.  Without this, the CPU would
  // re-execute the old (faulting) instruction forever.
#if defined(_M_PPC_64)
  __asm__ __volatile__("dcbst 0, %0; sync; icbi 0, %0; isync"
                       :
                       : "r"(it->code_addr)
                       : "memory");
#endif

  WARN_LOG_FMT(POWERPC, "BackPatch: handled MMIO at PC {:08x}, EA {:08x} (opcd {})",
               it->guest_pc, ea, opcd);

  return true;
}
