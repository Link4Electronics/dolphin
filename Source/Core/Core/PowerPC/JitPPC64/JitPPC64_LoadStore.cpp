#include "Core/PowerPC/JitPPC64/Jit.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/PowerPC.h"

static u32 PS_OFFSET_FR(u32 fr, u32 pair)
{
  return PS_OFFSET + fr * 16 + pair * 8;
}

// ===========================================================================
// D-form Load/Store (opcd 32-55)
//
// All use regcache for base/data registers: gpr.R(ra) returns a host
// register with the cached PPC value; gpr.W(rd) allocates a write register.
// This avoids dependency on r12 (ppcState pointer) being correct at runtime.
// ===========================================================================

bool JitPPC64::CompileLoadStore(UGeckoInstruction inst)
{
  u32 opcd = inst.OPCD;
  u32 rd = inst.RD, ra = inst.RA;
  s32 d = static_cast<s32>(static_cast<s16>(inst.SIMM_16));

  // Compute EA into REG_SCRATCH2 (r11)
  if (ra == 0)
    m_asm.ADDI(REG_SCRATCH2, 0, d);
  else
    m_asm.ADDI(REG_SCRATCH2, gpr.R(ra), d);

  switch (opcd)
  {
  // Integer loads
  case 32: // lwz
    {
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(32, opcd, rd, 0, host_rd, true);
    }
    return true;
  case 33: // lwzu
    {
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(32, opcd, rd, ra, host_rd, true);
      m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    }
    return true;
  case 34: // lbz
    {
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(8, opcd, rd, 0, host_rd, true);
    }
    return true;
  case 35: // lbzu
    {
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(8, opcd, rd, ra, host_rd, true);
      m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    }
    return true;
  case 40: // lhz
    {
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(16, opcd, rd, 0, host_rd, true);
    }
    return true;
  case 41: // lhzu
    {
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(16, opcd, rd, ra, host_rd, true);
      m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    }
    return true;
  case 42: // lha (signed, need LHA not LHZ)
    {
      u32 host_rd = gpr.W(rd);
      m_asm.LHA(host_rd, REG_SCRATCH2, 0);
    }
    return true;
  case 43: // lhau
    {
      u32 host_rd = gpr.W(rd);
      m_asm.LHA(host_rd, REG_SCRATCH2, 0);
      m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    }
    return true;

  // Integer stores
  case 36: // stw
    EmitBackpatchRoutine(32, opcd, rd, 0, gpr.R(rd), false);
    return true;
  case 37: // stwu
    EmitBackpatchRoutine(32, opcd, rd, ra, gpr.R(rd), false);
    m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    return true;
  case 38: // stb
    EmitBackpatchRoutine(8, opcd, rd, 0, gpr.R(rd), false);
    return true;
  case 39: // stbu
    EmitBackpatchRoutine(8, opcd, rd, ra, gpr.R(rd), false);
    m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    return true;
  case 44: // sth
    EmitBackpatchRoutine(16, opcd, rd, 0, gpr.R(rd), false);
    return true;
  case 45: // sthu
    EmitBackpatchRoutine(16, opcd, rd, ra, gpr.R(rd), false);
    m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    return true;

  // FPU loads (D-form)
  case 48: // lfs — FPU single, keep old backpatch (FPU MMIO extremely rare)
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LFS(0, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
      return true;
    }
  case 50: // lfd
    EmitBackpatchRoutine(64, opcd, rd, 0, 0, true, true);
    m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
    return true;

  // FPU stores (D-form)
  case 52: // stfs — FPU single, keep old backpatch
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
      m_asm.STFS(0, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      return true;
    }
  case 53: // stfsu
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
      m_asm.STFS(0, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      m_asm.MR(gpr.W(ra), REG_SCRATCH2);
      return true;
    }
  case 54: // stfd
    m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
    EmitBackpatchRoutine(64, opcd, rd, 0, 0, false, true);
    return true;
  case 55: // stfdu
    m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
    EmitBackpatchRoutine(64, opcd, rd, ra, 0, false, true);
    m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    return true;

  // FPU loads (D-form) — update forms
  case 49: // lfsu — keep old backpatch
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LFS(0, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
      m_asm.MR(gpr.W(ra), REG_SCRATCH2);
      return true;
    }
  case 51: // lfdu
    EmitBackpatchRoutine(64, opcd, rd, ra, 0, true, true);
    m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
    m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    return true;

  default:
    return false;
  }
}

// ===========================================================================
// Indexed Load/Store (opcd=31, specific XO values)
// ===========================================================================
// EA = (ra ? GPR[ra] : 0) + GPR[rb]

bool JitPPC64::CompileTable31_LoadStore(UGeckoInstruction inst)
{
  u32 xo = inst.SUBOP10;
  u32 rd = inst.RD, ra = inst.RA, rb = inst.RB;

  // Compute EA into REG_SCRATCH2 (r11)
  if (ra == 0)
    m_asm.MR(REG_SCRATCH2, gpr.R(rb));
  else
    m_asm.ADD(REG_SCRATCH2, gpr.R(ra), gpr.R(rb));

  switch (xo)
  {
  // Integer indexed loads
  case 23:   // lwzx
    {
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(32, inst.hex, rd, 0, host_rd, true);
    }
    return true;
  case 55:   // lwzux
    {
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(32, inst.hex, rd, ra, host_rd, true);
      m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    }
    return true;
  case 87:   // lbzx
    {
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(8, inst.hex, rd, 0, host_rd, true);
    }
    return true;
  case 119:  // lbzux
    {
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(8, inst.hex, rd, ra, host_rd, true);
      m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    }
    return true;
  case 279:  // lhzx
    {
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(16, inst.hex, rd, 0, host_rd, true);
    }
    return true;
  case 311:  // lhzux
    {
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(16, inst.hex, rd, ra, host_rd, true);
      m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    }
    return true;
  case 343:  // lhax
    {
      u32 host_rd = gpr.W(rd);
      m_asm.LHA(host_rd, REG_SCRATCH2, 0);
    }
    return true;
  case 375:  // lhaux
    {
      u32 host_rd = gpr.W(rd);
      m_asm.LHA(host_rd, REG_SCRATCH2, 0);
      m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    }
    return true;

  // Integer indexed stores
  case 151:  // stwx
    EmitBackpatchRoutine(32, inst.hex, rd, 0, gpr.R(rd), false);
    return true;
  case 183:  // stwux
    EmitBackpatchRoutine(32, inst.hex, rd, ra, gpr.R(rd), false);
    m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    return true;
  case 215:  // stbx
    EmitBackpatchRoutine(8, inst.hex, rd, 0, gpr.R(rd), false);
    return true;
  case 247:  // stbux
    EmitBackpatchRoutine(8, inst.hex, rd, ra, gpr.R(rd), false);
    m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    return true;
  case 407:  // sthx
    EmitBackpatchRoutine(16, inst.hex, rd, 0, gpr.R(rd), false);
    return true;
  case 439:  // sthux
    EmitBackpatchRoutine(16, inst.hex, rd, ra, gpr.R(rd), false);
    m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    return true;

  // FPU indexed loads
  case 535:  // lfsx — keep old backpatch (FPU single, rare)
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LFS(0, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
      return true;
    }
  case 567:  // lfsux — keep old backpatch
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LFS(0, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
      m_asm.MR(gpr.W(ra), REG_SCRATCH2);
      return true;
    }
  case 599:  // lfdx
    EmitBackpatchRoutine(64, inst.hex, rd, 0, 0, true, true);
    m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
    return true;
  case 631:  // lfdux
    EmitBackpatchRoutine(64, inst.hex, rd, ra, 0, true, true);
    m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
    m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    return true;

  // FPU indexed stores
  case 663:  // stfsx — keep old backpatch
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
      m_asm.STFS(0, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      return true;
    }
  case 695:  // stfsux — keep old backpatch
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
      m_asm.STFS(0, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      m_asm.MR(gpr.W(ra), REG_SCRATCH2);
      return true;
    }
  case 727:  // stfdx
    m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
    EmitBackpatchRoutine(64, inst.hex, rd, 0, 0, false, true);
    return true;
  case 759:  // stfdux
    m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
    EmitBackpatchRoutine(64, inst.hex, rd, ra, 0, false, true);
    m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    return true;

  // Byte-reversed loads/stores
  case 534:  // lwbrx
    {
      u32 host_rd = gpr.W(rd);
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LWBRX(host_rd, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
    }
    return true;
  case 662:  // stwbrx
    {
      u32 host_rs = gpr.R(rd);
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.STWBRX(host_rs, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
    }
    return true;
  case 790:  // lhbrx
    {
      u32 host_rd = gpr.W(rd);
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LHBRX(host_rd, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
    }
    return true;
  case 918:  // sthbrx
    {
      u32 host_rs = gpr.R(rd);
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.STHBRX(host_rs, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
    }
    return true;

  // stfiwx — store FPR as integer word
  case 983:  // stfiwx
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
      m_asm.STFIWX(0, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      return true;
    }

  default:
    return false;
  }
}

// ===========================================================================
// lmw/stmw — multi-word load/store (opcd 46/47)
// ===========================================================================

bool JitPPC64::CompileLMW(UGeckoInstruction inst)
{
  u32 rt = inst.RD;
  u32 ra = inst.RA;
  s32 d = static_cast<s32>(static_cast<s16>(inst.SIMM_16));

  if (ra == 0)
    m_asm.ADDI(REG_SCRATCH2, 0, d);
  else
    m_asm.ADDI(REG_SCRATCH2, gpr.R(ra), d);

  for (u32 r = rt; r <= 31; ++r)
  {
    m_asm.LWZ(gpr.W(r), REG_SCRATCH2, 0);
    if (r < 31)
      m_asm.ADDI(REG_SCRATCH2, REG_SCRATCH2, 4);
  }
  return true;
}

bool JitPPC64::CompileSTMW(UGeckoInstruction inst)
{
  u32 rs = inst.RS;
  u32 ra = inst.RA;
  s32 d = static_cast<s32>(static_cast<s16>(inst.SIMM_16));

  if (ra == 0)
    m_asm.ADDI(REG_SCRATCH2, 0, d);
  else
    m_asm.ADDI(REG_SCRATCH2, gpr.R(ra), d);

  for (u32 r = rs; r <= 31; ++r)
  {
    m_asm.STW(gpr.R(r), REG_SCRATCH2, 0);
    if (r < 31)
      m_asm.ADDI(REG_SCRATCH2, REG_SCRATCH2, 4);
  }
  return true;
}
