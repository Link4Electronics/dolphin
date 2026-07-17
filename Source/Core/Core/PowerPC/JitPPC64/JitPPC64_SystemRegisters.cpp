#include "Core/PowerPC/JitPPC64/Jit.h"
#include "Core/PowerPC/PowerPC.h"

// ===========================================================================
// System register compilers
//
// All GPR access uses regcache (gpr.R/gpr.W) to stay consistent with
// cached values from the integer/load-store compilers.
// ===========================================================================

bool JitPPC64::CompileMFCR(UGeckoInstruction inst)
{
  u32 rd = inst.RD;
  m_asm.LWZ(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(CR_OFFSET));
  m_asm.MR(gpr.W(rd), REG_SCRATCH);
  return true;
}

bool JitPPC64::CompileMTCRF(UGeckoInstruction inst)
{
  u32 rd = inst.RD;
  m_asm.MTCRF(inst.CRM, gpr.R(rd));
  return true;
}

bool JitPPC64::CompileMFSPR(UGeckoInstruction inst)
{
  u32 rd = inst.RD;
  u32 spr = (inst.SPRU << 5) | (inst.SPRL & 0x1F);
  if (spr < 1024)
  {
    m_asm.LWZ(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(SPR_OFFSET + 4 * spr));
    m_asm.MR(gpr.W(rd), REG_SCRATCH);
    return true;
  }
  return false;
}

bool JitPPC64::CompileMTSPR(UGeckoInstruction inst)
{
  u32 rd = inst.RD;
  u32 spr = (inst.SPRU << 5) | (inst.SPRL & 0x1F);
  if (spr < 1024)
  {
    m_asm.STW(gpr.R(rd), REG_PPC_BASE, static_cast<s32>(SPR_OFFSET + 4 * spr));
    return true;
  }
  return false;
}

bool JitPPC64::CompileMFMSR(UGeckoInstruction inst)
{
  u32 rd = inst.RD;
  m_asm.LWZ(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(MSR_OFFSET));
  m_asm.MR(gpr.W(rd), REG_SCRATCH);
  return true;
}

bool JitPPC64::CompileMTMSR(UGeckoInstruction inst)
{
  u32 rd = inst.RD;
  m_asm.STW(gpr.R(rd), REG_PPC_BASE, static_cast<s32>(MSR_OFFSET));
  return true;
}

bool JitPPC64::CompileMFTB(UGeckoInstruction inst)
{
  u32 rd = inst.RD;
  u32 spr = (inst.SPRU << 5) | (inst.SPRL & 0x1F);
  if (spr == SPR_TL || spr == SPR_TU)
  {
    // Call JitPPC64RefreshTimebase(ppcState) to refresh spr[TL/TU] from
    // CoreTiming's GetFakeTimeBase().  This ensures the timebase advances
    // even within backwards-branch loops (branch instructions stay inside
    // the compiled block and never hit the dispatcher).
    //
    // We save/restore REG_PPC_BASE (r12) around the call — the function
    // follows ELFv2 ABI and may clobber volatile registers.
    m_asm.STD(REG_PPC_BASE, 1, 24);  // save r12 at block frame + 24 (free area)
    m_asm.MR(3, REG_PPC_BASE);        // r3 = ppcState*
    TrampMOVI64(m_asm, 12, reinterpret_cast<u64>(&JitPPC64RefreshTimebase));
    m_asm.MTCTR(12);
    m_asm.BCTRL();                    // call — returns u64 in r3
    m_asm.LD(REG_PPC_BASE, 1, 24);   // restore r12

    // r3 = full 64-bit timebase value
    if (spr == SPR_TL)
      m_asm.CLR32(REG_SCRATCH, 3);   // extract lower 32 bits
    else
      m_asm.RLDICL(REG_SCRATCH, 3, 32, 32);  // extract upper 32 bits

    m_asm.MR(gpr.W(rd), REG_SCRATCH);
    return true;
  }
  return false;
}

bool JitPPC64::CompileTW(UGeckoInstruction inst)
{
  return true;
}

// ===========================================================================
// Cache / barrier / misc compiler (opcd=31, various XO)
// ===========================================================================
// These emit native PPC970 instructions where the semantics match Gekko.

bool JitPPC64::CompileMisc(UGeckoInstruction inst)
{
  u32 xo = inst.SUBOP10;

  switch (xo)
  {
  case 54:  // dcbst
    if (inst.RA == 0)
      m_asm.DCBST(0, gpr.R(inst.RB));
    else
    {
      m_asm.ADD(REG_SCRATCH2, gpr.R(inst.RA), gpr.R(inst.RB));
      m_asm.DCBST(REG_SCRATCH2, 0);
    }
    return true;
  case 86:  // dcbf
    if (inst.RA == 0)
      m_asm.DCBF(0, gpr.R(inst.RB));
    else
    {
      m_asm.ADD(REG_SCRATCH2, gpr.R(inst.RA), gpr.R(inst.RB));
      m_asm.DCBF(REG_SCRATCH2, 0);
    }
    return true;
  case 246: // dcbtst
    if (inst.RA == 0)
      m_asm.DCBTST(0, gpr.R(inst.RB));
    else
    {
      m_asm.ADD(REG_SCRATCH2, gpr.R(inst.RA), gpr.R(inst.RB));
      m_asm.DCBTST(REG_SCRATCH2, 0);
    }
    return true;
  case 278: // dcbt
    if (inst.RA == 0)
      m_asm.DCBT(0, gpr.R(inst.RB));
    else
    {
      m_asm.ADD(REG_SCRATCH2, gpr.R(inst.RA), gpr.R(inst.RB));
      m_asm.DCBT(REG_SCRATCH2, 0);
    }
    return true;
  case 470: // dcbi (privileged data cache block invalidate)
    if (inst.RA == 0)
      m_asm.DCBI(0, gpr.R(inst.RB));
    else
    {
      m_asm.ADD(REG_SCRATCH2, gpr.R(inst.RA), gpr.R(inst.RB));
      m_asm.DCBI(REG_SCRATCH2, 0);
    }
    return true;
  case 598: // sync
    m_asm.SYNC();
    return true;
  case 854: // eieio
    m_asm.EIEIO();
    return true;
  case 982: // icbi
    if (inst.RA == 0)
      m_asm.ICBI(0, gpr.R(inst.RB));
    else
    {
      m_asm.ADD(REG_SCRATCH2, gpr.R(inst.RA), gpr.R(inst.RB));
      m_asm.ICBI(REG_SCRATCH2, 0);
    }
    return true;
  case 1014: // dcbz — PPC970 zeros 128B, not 32B → emulate with 8 word-stores
  {
    // EA = (RA ? GPR[RA] : 0) + GPR[RB]
    if (inst.RA == 0)
      m_asm.MR(REG_SCRATCH2, gpr.R(inst.RB));
    else
    {
      m_asm.ADD(REG_SCRATCH2, gpr.R(inst.RA), gpr.R(inst.RB));
    }
    // Align EA to 32 bytes (Gekko cache line)
    m_asm.RLDICR(REG_SCRATCH, REG_SCRATCH2, 0, 58);
    // Copy to r3 (r0 can't be used as D-form base register)
    m_asm.OR(3, REG_SCRATCH, REG_SCRATCH);
    // Zero 32 bytes (8 × 4-byte stores)
    m_asm.ADDI(REG_SCRATCH2, 0, 0);
    for (int off = 0; off < 32; off += 4)
      m_asm.STW(REG_SCRATCH2, 3, off);
    return true;
  }
  default:
    return false;
  }
}

// ===========================================================================
// isync (opcd=19, SUBOP10=150)
// ===========================================================================

bool JitPPC64::CompileISYNC(UGeckoInstruction inst)
{
  m_asm.ISYNC();
  return true;
}

// ===========================================================================
// sc (opcd=17) — syscall → must deliver EXCEPTION_SYSCALL via interpreter
// ===========================================================================

bool JitPPC64::CompileSC(UGeckoInstruction inst)
{
  return false;
}

// ===========================================================================
// rfi (opcd=19, SUBOP10=50) — return from interrupt → interpreter fallback
// ===========================================================================

bool JitPPC64::CompileRFI(UGeckoInstruction inst)
{
  return false;
}

// ===========================================================================
// Table 31 system register dispatch
// ===========================================================================

bool JitPPC64::CompileTable31_SystemReg(UGeckoInstruction inst)
{
  u32 xo = inst.SUBOP10;

  switch (xo)
  {
  case 19:  // mfcr
    return CompileMFCR(inst);
  case 144: // mtcrf
    return CompileMTCRF(inst);
  case 339: // mfspr
    return CompileMFSPR(inst);
  case 467: // mtspr
    return CompileMTSPR(inst);
  case 83:  // mfmsr
    return CompileMFMSR(inst);
  case 146: // mtmsr
    return CompileMTMSR(inst);
  case 371: // mftb
    return CompileMFTB(inst);
  case 4:   // tw
    return CompileTW(inst);
  default:
    return false;
  }
}
