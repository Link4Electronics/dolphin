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
// Every load/store is followed by AddBackpatchEntry() so that HandleFault
// can handle MMIO accesses.  The regcache is NOT flushed here — instead,
// HandleFault reads the cached GPR values directly from the host register
// file saved in the signal ucontext (see ReadPPCGPR).
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
  {
    LoadGPR(REG_SCRATCH, ra);
    m_asm.ADDI(REG_SCRATCH2, REG_SCRATCH, d);
  }

  switch (opcd)
  {
  // Integer loads
  case 32: // lwz
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LWZ(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      StoreGPR(rd, REG_SCRATCH);
      return true;
    }
  case 33: // lwzu
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LWZ(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      StoreGPR(rd, REG_SCRATCH);
      StoreGPR(ra, REG_SCRATCH2);
      return true;
    }
  case 34: // lbz
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LBZ(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      StoreGPR(rd, REG_SCRATCH);
      return true;
    }
  case 35: // lbzu
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LBZ(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      StoreGPR(rd, REG_SCRATCH);
      StoreGPR(ra, REG_SCRATCH2);
      return true;
    }
  case 40: // lhz
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LHZ(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      StoreGPR(rd, REG_SCRATCH);
      return true;
    }
  case 41: // lhzu
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LHZ(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      StoreGPR(rd, REG_SCRATCH);
      StoreGPR(ra, REG_SCRATCH2);
      return true;
    }
  case 42: // lha
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LHA(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      StoreGPR(rd, REG_SCRATCH);
      return true;
    }
  case 43: // lhau
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LHA(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      StoreGPR(rd, REG_SCRATCH);
      StoreGPR(ra, REG_SCRATCH2);
      return true;
    }

  // Integer stores
  case 36: // stw
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      LoadGPR(REG_SCRATCH, rd);
      m_asm.STW(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      return true;
    }
  case 37: // stwu
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      LoadGPR(REG_SCRATCH, rd);
      m_asm.STW(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      StoreGPR(ra, REG_SCRATCH2);
      return true;
    }
  case 38: // stb
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      LoadGPR(REG_SCRATCH, rd);
      m_asm.STB(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      return true;
    }
  case 39: // stbu
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      LoadGPR(REG_SCRATCH, rd);
      m_asm.STB(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      StoreGPR(ra, REG_SCRATCH2);
      return true;
    }
  case 44: // sth
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      LoadGPR(REG_SCRATCH, rd);
      m_asm.STH(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      return true;
    }
  case 45: // sthu
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      LoadGPR(REG_SCRATCH, rd);
      m_asm.STH(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      StoreGPR(ra, REG_SCRATCH2);
      return true;
    }

  // FPU loads (D-form)
  case 48: // lfs
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LFS(0, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
      return true;
    }
  case 50: // lfd
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LFD(0, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
      return true;
    }

  // FPU stores (D-form)
  case 52: // stfs
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
      StoreGPR(ra, REG_SCRATCH2);
      return true;
    }
  case 54: // stfd
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
      m_asm.STFD(0, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      return true;
    }
  case 55: // stfdu
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
      m_asm.STFD(0, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      StoreGPR(ra, REG_SCRATCH2);
      return true;
    }

  // FPU loads (D-form) — update forms
  case 49: // lfsu
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LFS(0, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
      StoreGPR(ra, REG_SCRATCH2);
      return true;
    }
  case 51: // lfdu
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LFD(0, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
      StoreGPR(ra, REG_SCRATCH2);
      return true;
    }

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
    LoadGPR(REG_SCRATCH2, rb);
  else
  {
    LoadGPR(REG_SCRATCH, ra);
    LoadGPR(REG_SCRATCH2, rb);
    m_asm.ADD(REG_SCRATCH2, REG_SCRATCH, REG_SCRATCH2);
  }

  switch (xo)
  {
  // Integer indexed loads
  case 23:   // lwzx
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LWZ(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      StoreGPR(rd, REG_SCRATCH);
      return true;
    }
  case 55:   // lwzux
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LWZ(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      StoreGPR(rd, REG_SCRATCH);
      StoreGPR(ra, REG_SCRATCH2);
      return true;
    }
  case 87:   // lbzx
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LBZ(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      StoreGPR(rd, REG_SCRATCH);
      return true;
    }
  case 119:  // lbzux
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LBZ(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      StoreGPR(rd, REG_SCRATCH);
      StoreGPR(ra, REG_SCRATCH2);
      return true;
    }
  case 279:  // lhzx
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LHZ(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      StoreGPR(rd, REG_SCRATCH);
      return true;
    }
  case 311:  // lhzux
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LHZ(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      StoreGPR(rd, REG_SCRATCH);
      StoreGPR(ra, REG_SCRATCH2);
      return true;
    }
  case 343:  // lhax
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LHA(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      StoreGPR(rd, REG_SCRATCH);
      return true;
    }
  case 375:  // lhaux
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LHA(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      StoreGPR(rd, REG_SCRATCH);
      StoreGPR(ra, REG_SCRATCH2);
      return true;
    }

  // Integer indexed stores
  case 151:  // stwx
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      LoadGPR(REG_SCRATCH, rd);
      m_asm.STW(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      return true;
    }
  case 183:  // stwux
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      LoadGPR(REG_SCRATCH, rd);
      m_asm.STW(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      StoreGPR(ra, REG_SCRATCH2);
      return true;
    }
  case 215:  // stbx
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      LoadGPR(REG_SCRATCH, rd);
      m_asm.STB(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      return true;
    }
  case 247:  // stbux
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      LoadGPR(REG_SCRATCH, rd);
      m_asm.STB(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      StoreGPR(ra, REG_SCRATCH2);
      return true;
    }
  case 407:  // sthx
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      LoadGPR(REG_SCRATCH, rd);
      m_asm.STH(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      return true;
    }
  case 439:  // sthux
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      LoadGPR(REG_SCRATCH, rd);
      m_asm.STH(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      StoreGPR(ra, REG_SCRATCH2);
      return true;
    }

  // FPU indexed loads
  case 535:  // lfsx
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LFS(0, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
      return true;
    }
  case 567:  // lfsux
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LFS(0, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
      StoreGPR(ra, REG_SCRATCH2);
      return true;
    }
  case 599:  // lfdx
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LFD(0, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
      return true;
    }
  case 631:  // lfdux
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LFD(0, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
      StoreGPR(ra, REG_SCRATCH2);
      return true;
    }

  // FPU indexed stores
  case 663:  // stfsx
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
      m_asm.STFS(0, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      return true;
    }
  case 695:  // stfsux
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
      m_asm.STFS(0, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      StoreGPR(ra, REG_SCRATCH2);
      return true;
    }
  case 727:  // stfdx
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
      m_asm.STFD(0, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      return true;
    }
  case 759:  // stfdux
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
      m_asm.STFD(0, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      StoreGPR(ra, REG_SCRATCH2);
      return true;
    }

  // Byte-reversed loads/stores
  case 534:  // lwbrx
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LWBRX(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      StoreGPR(rd, REG_SCRATCH);
      return true;
    }
  case 662:  // stwbrx
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      LoadGPR(REG_SCRATCH, rd);
      m_asm.STWBRX(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      return true;
    }
  case 790:  // lhbrx
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LHBRX(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      StoreGPR(rd, REG_SCRATCH);
      return true;
    }
  case 918:  // sthbrx
    {
      const u8* addr = m_asm.Code() + m_asm.Size();
      LoadGPR(REG_SCRATCH, rd);
      m_asm.STHBRX(REG_SCRATCH, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      return true;
    }

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
  {
    LoadGPR(REG_SCRATCH, ra);
    m_asm.ADDI(REG_SCRATCH2, REG_SCRATCH, d);
  }

  for (u32 r = rt; r <= 31; ++r)
  {
    m_asm.LWZ(REG_SCRATCH, REG_SCRATCH2, 0);
    StoreGPR(r, REG_SCRATCH);
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
  {
    LoadGPR(REG_SCRATCH, ra);
    m_asm.ADDI(REG_SCRATCH2, REG_SCRATCH, d);
  }

  for (u32 r = rs; r <= 31; ++r)
  {
    LoadGPR(REG_SCRATCH, r);
    m_asm.STW(REG_SCRATCH, REG_SCRATCH2, 0);
    if (r < 31)
      m_asm.ADDI(REG_SCRATCH2, REG_SCRATCH2, 4);
  }
  return true;
}
