#include "Core/PowerPC/JitPPC64/Jit.h"

// ===========================================================================
// Integer I-form ALU compilers
// ===========================================================================

bool JitPPC64::CompileADDI(UGeckoInstruction inst)
{
  u32 rd = inst.RD, ra = inst.RA;
  s32 simm = static_cast<s32>(static_cast<s16>(inst.SIMM_16));
  if (ra == 0)
    m_asm.ADDI(REG_SCRATCH2, 0, simm);
  else
  {
    LoadGPR(REG_SCRATCH, ra);
    m_asm.ADDI(REG_SCRATCH2, REG_SCRATCH, simm);
  }
  StoreGPR(rd, REG_SCRATCH2);
  return true;
}

bool JitPPC64::CompileADDIS(UGeckoInstruction inst)
{
  u32 rd = inst.RD, ra = inst.RA;
  s32 simm = static_cast<s32>(static_cast<s16>(inst.SIMM_16));
  if (ra == 0)
    m_asm.ADDIS(REG_SCRATCH2, 0, simm);
  else
  {
    LoadGPR(REG_SCRATCH, ra);
    m_asm.ADDIS(REG_SCRATCH2, REG_SCRATCH, simm << 16);
  }
  StoreGPR(rd, REG_SCRATCH2);
  return true;
}

bool JitPPC64::CompileADDIC(UGeckoInstruction inst)
{
  u32 rd = inst.RD, ra = inst.RA;
  s32 simm = static_cast<s32>(static_cast<s16>(inst.SIMM_16));
  LoadGPR(REG_SCRATCH, ra);
  m_asm.ADDI(REG_SCRATCH2, REG_SCRATCH, simm);
  StoreGPR(rd, REG_SCRATCH2);
  // CA = 1 if result < ra unsigned (carry out)
  m_asm.CMPLW(0, REG_SCRATCH2, REG_SCRATCH);
  m_asm.MFCR(REG_SCRATCH);
  m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 0, 0, 0);  // keep CR0[LT] = bit 31
  m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 1, 31, 31); // shift bit 31→bit 0 for STB
  m_asm.STB(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(XER_CA_OFFSET));
  return true;
}

bool JitPPC64::CompileADDIC_(UGeckoInstruction inst)
{
  CompileADDIC(inst);
  LoadGPR(REG_SCRATCH2, inst.RD);
  EmitCR0Update();
  return true;
}

bool JitPPC64::CompileMULLI(UGeckoInstruction inst)
{
  u32 rd = inst.RD, ra = inst.RA;
  s32 simm = static_cast<s32>(static_cast<s16>(inst.SIMM_16));
  LoadGPR(REG_SCRATCH, ra);
  m_asm.MULLI(REG_SCRATCH2, REG_SCRATCH, simm);
  StoreGPR(rd, REG_SCRATCH2);
  return true;
}

bool JitPPC64::CompileANDI_(UGeckoInstruction inst)
{
  u32 ra = inst.RA, rs = inst.RS;
  LoadGPR(REG_SCRATCH2, rs);
  m_asm.ANDI_(REG_SCRATCH2, REG_SCRATCH2, inst.UIMM);
  StoreGPR(ra, REG_SCRATCH2);
  EmitCR0Update();
  return true;
}

bool JitPPC64::CompileANDIS_(UGeckoInstruction inst)
{
  u32 ra = inst.RA, rs = inst.RS;
  LoadGPR(REG_SCRATCH2, rs);
  m_asm.ANDIS_(REG_SCRATCH2, REG_SCRATCH2, inst.UIMM);
  StoreGPR(ra, REG_SCRATCH2);
  EmitCR0Update();
  return true;
}

bool JitPPC64::CompileORI(UGeckoInstruction inst)
{
  u32 ra = inst.RA, rs = inst.RS;
  if (ra == rs && inst.UIMM == 0)
    return true;
  LoadGPR(REG_SCRATCH2, rs);
  m_asm.ORI(REG_SCRATCH2, REG_SCRATCH2, inst.UIMM);
  StoreGPR(ra, REG_SCRATCH2);
  return true;
}

bool JitPPC64::CompileORIS(UGeckoInstruction inst)
{
  u32 ra = inst.RA, rs = inst.RS;
  LoadGPR(REG_SCRATCH2, rs);
  m_asm.ORIS(REG_SCRATCH2, REG_SCRATCH2, inst.UIMM);
  StoreGPR(ra, REG_SCRATCH2);
  return true;
}

bool JitPPC64::CompileXORI(UGeckoInstruction inst)
{
  u32 ra = inst.RA, rs = inst.RS;
  LoadGPR(REG_SCRATCH2, rs);
  m_asm.XORI(REG_SCRATCH2, REG_SCRATCH2, inst.UIMM);
  StoreGPR(ra, REG_SCRATCH2);
  return true;
}

bool JitPPC64::CompileXORIS(UGeckoInstruction inst)
{
  u32 ra = inst.RA, rs = inst.RS;
  LoadGPR(REG_SCRATCH2, rs);
  m_asm.XORIS(REG_SCRATCH2, REG_SCRATCH2, inst.UIMM);
  StoreGPR(ra, REG_SCRATCH2);
  return true;
}

bool JitPPC64::CompileCMPI(UGeckoInstruction inst)
{
  u32 crfd = inst.CRFD, ra = inst.RA;
  s32 simm = static_cast<s32>(static_cast<s16>(inst.SIMM_16));
  LoadGPR(REG_SCRATCH, ra);
  m_asm.EXTSW(REG_SCRATCH, REG_SCRATCH);
  m_asm.CMPWI(crfd, REG_SCRATCH, simm);
  return true;
}

bool JitPPC64::CompileCMPLI(UGeckoInstruction inst)
{
  u32 crfd = inst.CRFD, ra = inst.RA;
  LoadGPR(REG_SCRATCH, ra);
  m_asm.CMPLWI(crfd, REG_SCRATCH, inst.UIMM);
  return true;
}

// ===========================================================================
// Subfic (opcd=8)
// ===========================================================================
// r3-r10 are free (regcache uses r14-r31)

bool JitPPC64::CompileSubfic(UGeckoInstruction inst)
{
  u32 rd = inst.RD, ra = inst.RA;
  s32 simm = static_cast<s32>(static_cast<s16>(inst.SIMM_16));

  LoadGPR(REG_SCRATCH, ra);        // r0 = ra
  m_asm.OR(3, REG_SCRATCH, REG_SCRATCH);  // r3 = ra (copy for CA computation)

  // rd = simm - ra
  m_asm.SUBFIC(REG_SCRATCH2, REG_SCRATCH, simm);  // r11 = simm - ra
  StoreGPR(rd, REG_SCRATCH2);

  // CA = 1 if u32(simm) >= u32(ra) unsigned (no borrow)
  m_asm.ADDI(REG_SCRATCH, 0, simm);   // r0 = simm
  m_asm.CMPLW(0, REG_SCRATCH, 3);     // simm >= ra? r0:r3
  m_asm.MFCR(REG_SCRATCH2);           // r11 = CR
  // CA = !LT = !CR0[0] = !bit 31
  // rlwinm: keep bit 31, xor to invert, shift to bit 0
  m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH2, 0, 0, 0);   // r0 = bit 31 only
  m_asm.XORI(REG_SCRATCH, REG_SCRATCH, 0x80000000);    // invert: CA in bit 31
  m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 1, 31, 31);   // bit 31 → bit 0 for STB
  m_asm.STB(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(XER_CA_OFFSET));
  return true;
}


// ===========================================================================
// twi (opcd=3)
// ===========================================================================

bool JitPPC64::CompileTWI(UGeckoInstruction inst)
{
  return true;  // nop
}


// ===========================================================================
// Table 31 (opcd=31) — integer ALU subset
// ===========================================================================

bool JitPPC64::CompileTable31_Integer(UGeckoInstruction inst)
{
  u32 xo = inst.SUBOP10;
  u32 rd = inst.RD, ra = inst.RA, rb = inst.RB;
  bool rc = inst.Rc;

  switch (xo)
  {
  case 266: // addx
    if (rd == ra && rd == rb)
    {
      LoadGPR(REG_SCRATCH, ra);
      m_asm.ADD(REG_SCRATCH2, REG_SCRATCH, REG_SCRATCH);
    }
    else if (rd == rb)
    {
      LoadGPR(REG_SCRATCH, ra);
      LoadGPR(REG_SCRATCH2, rb);
      m_asm.ADD(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2);
      m_asm.OR(REG_SCRATCH2, REG_SCRATCH, REG_SCRATCH);
    }
    else
    {
      LoadGPR(REG_SCRATCH, ra);
      LoadGPR(REG_SCRATCH2, rb);
      m_asm.ADD(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2);
      m_asm.OR(REG_SCRATCH2, REG_SCRATCH, REG_SCRATCH);
    }
    if (rc) EmitCR0Update();
    StoreGPR(rd, REG_SCRATCH2);
    return true;
  case 40:  // subfx
    LoadGPR(REG_SCRATCH, rb);
    LoadGPR(REG_SCRATCH2, ra);
    m_asm.SUBF(REG_SCRATCH, REG_SCRATCH2, REG_SCRATCH);
    if (rc) EmitCR0Update();
    StoreGPR(rd, REG_SCRATCH);
    return true;
  case 10:  // addcx
    LoadGPR(REG_SCRATCH, ra);
    LoadGPR(REG_SCRATCH2, rb);
    m_asm.ADDC(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2);
    // CA = 1 if result < rb unsigned (carry out)
    m_asm.CMPLW(0, REG_SCRATCH, REG_SCRATCH2);
    m_asm.MFCR(REG_SCRATCH2);
    m_asm.RLWINM(REG_SCRATCH2, REG_SCRATCH2, 0, 0, 0);  // keep CR0[LT] = bit 31
    m_asm.RLWINM(REG_SCRATCH2, REG_SCRATCH2, 1, 31, 31); // shift bit 31→bit 0 for STB
    m_asm.STB(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(XER_CA_OFFSET));
    StoreGPR(rd, REG_SCRATCH);
    return true;
  case 75:  // mulhwx
    LoadGPR(REG_SCRATCH, ra);
    LoadGPR(REG_SCRATCH2, rb);
    m_asm.MULHW(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2);
    if (rc) EmitCR0Update();
    StoreGPR(rd, REG_SCRATCH);
    return true;
  case 11:  // mulhwux
    LoadGPR(REG_SCRATCH, ra);
    LoadGPR(REG_SCRATCH2, rb);
    m_asm.MULHWU(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2);
    if (rc) EmitCR0Update();
    StoreGPR(rd, REG_SCRATCH);
    return true;
  case 235: // mullwx
    LoadGPR(REG_SCRATCH, ra);
    LoadGPR(REG_SCRATCH2, rb);
    m_asm.MULLW(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2);
    if (rc) EmitCR0Update();
    StoreGPR(rd, REG_SCRATCH);
    return true;
  case 491: // divwx
    LoadGPR(REG_SCRATCH, ra);
    LoadGPR(REG_SCRATCH2, rb);
    m_asm.DIVW(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2);
    if (rc) EmitCR0Update();
    StoreGPR(rd, REG_SCRATCH);
    return true;
  case 459: // divwux
    LoadGPR(REG_SCRATCH, ra);
    LoadGPR(REG_SCRATCH2, rb);
    m_asm.DIVWU(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2);
    if (rc) EmitCR0Update();
    StoreGPR(rd, REG_SCRATCH);
    return true;
  case 28:  // andx
    LoadGPR(REG_SCRATCH, ra);
    LoadGPR(REG_SCRATCH2, rb);
    m_asm.AND(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2);
    if (rc) EmitCR0Update();
    StoreGPR(rd, REG_SCRATCH);
    return true;
  case 60:  // andcx
    LoadGPR(REG_SCRATCH, ra);
    LoadGPR(REG_SCRATCH2, rb);
    m_asm.ANDC(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2);
    if (rc) EmitCR0Update();
    StoreGPR(rd, REG_SCRATCH);
    return true;
  case 444: // orx
    LoadGPR(REG_SCRATCH, ra);
    LoadGPR(REG_SCRATCH2, rb);
    m_asm.OR(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2);
    if (rc) EmitCR0Update();
    StoreGPR(rd, REG_SCRATCH);
    return true;
  case 412: // orcx
    LoadGPR(REG_SCRATCH, ra);
    LoadGPR(REG_SCRATCH2, rb);
    m_asm.ORC(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2);
    if (rc) EmitCR0Update();
    StoreGPR(rd, REG_SCRATCH);
    return true;
  case 316: // xorx
    LoadGPR(REG_SCRATCH, ra);
    LoadGPR(REG_SCRATCH2, rb);
    m_asm.XOR(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2);
    if (rc) EmitCR0Update();
    StoreGPR(rd, REG_SCRATCH);
    return true;
  case 476: // nandx
    LoadGPR(REG_SCRATCH, ra);
    LoadGPR(REG_SCRATCH2, rb);
    m_asm.NAND(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2);
    if (rc) EmitCR0Update();
    StoreGPR(rd, REG_SCRATCH);
    return true;
  case 124: // norx
    LoadGPR(REG_SCRATCH, ra);
    LoadGPR(REG_SCRATCH2, rb);
    m_asm.NOR(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2);
    if (rc) EmitCR0Update();
    StoreGPR(rd, REG_SCRATCH);
    return true;
  case 284: // eqvx
    LoadGPR(REG_SCRATCH, ra);
    LoadGPR(REG_SCRATCH2, rb);
    m_asm.EQV(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2);
    if (rc) EmitCR0Update();
    StoreGPR(rd, REG_SCRATCH);
    return true;
  case 24:  // slwx
    LoadGPR(REG_SCRATCH, ra);
    LoadGPR(REG_SCRATCH2, rb);
    m_asm.SLW(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2);
    if (rc) EmitCR0Update();
    StoreGPR(rd, REG_SCRATCH);
    return true;
  case 8:   // subfcx
    LoadGPR(REG_SCRATCH, ra);
    LoadGPR(REG_SCRATCH2, rb);
    m_asm.SUBFC(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2);
    // CA = 1 if rb >= u32(ra) (no borrow).
    // After SUBFC: result = rb - ra. 
    // CA = (result <= rb unsigned) = !(result > rb) = !CR0[GT]
    m_asm.CMPLW(0, REG_SCRATCH, REG_SCRATCH2);
    m_asm.MFCR(REG_SCRATCH2);
    // CR0[GT] = bit 30. Keep only bit 30, shift to bit 0, invert for CA.
    m_asm.RLWINM(REG_SCRATCH2, REG_SCRATCH2, 0, 1, 1);  // keep CR0[GT] = bit 30
    m_asm.RLWINM(REG_SCRATCH2, REG_SCRATCH2, 2, 31, 31); // shift bit 30→bit 0 for STB
    m_asm.XORI(REG_SCRATCH2, REG_SCRATCH2, 1);           // invert: CA = !GT
    m_asm.STB(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(XER_CA_OFFSET));
    StoreGPR(rd, REG_SCRATCH);
    return true;
  case 136: // subfex
    // rd = ~ra + rb + XER[CA] = rb - ra - 1 + CA. CA in, CA out.
    // CA computation: after subfe (rb - ra - 1 + CA), CA = 1 if borrow didn't occur
    // Complex CA dependency — fallback to interpreter
    return false;
  case 138: // addex
    // rd = ra + rb + XER[CA]. CA in, CA out. Fallback.
    return false;
  case 200: // subfzex
  case 202: // addzex
  case 232: // subfmex
  case 234: // addmex
    return false;
  case 104: // negx
    LoadGPR(REG_SCRATCH, ra);
    m_asm.NEG(REG_SCRATCH, REG_SCRATCH);
    if (rc) EmitCR0Update();
    StoreGPR(rd, REG_SCRATCH);
    return true;
  case 512: // mcrxr — rare → fallback
    return false;
  case 986: // extswx — sign-extend 32-bit word to 64-bit (nop on 32-bit Gekko)
    LoadGPR(REG_SCRATCH, ra);
    m_asm.EXTSW(REG_SCRATCH, REG_SCRATCH);
    if (rc) EmitCR0Update();
    StoreGPR(rd, REG_SCRATCH);
    return true;
  case 536: // srwx
    LoadGPR(REG_SCRATCH, ra);
    LoadGPR(REG_SCRATCH2, rb);
    m_asm.SRW(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2);
    if (rc) EmitCR0Update();
    StoreGPR(rd, REG_SCRATCH);
    return true;
  case 792: // srawx
    LoadGPR(REG_SCRATCH, ra);
    LoadGPR(REG_SCRATCH2, rb);
    m_asm.SRAW(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2);
    if (rc) EmitCR0Update();
    StoreGPR(rd, REG_SCRATCH);
    return true;
  case 824: // srawix
    LoadGPR(REG_SCRATCH, ra);
    m_asm.SRAWI(REG_SCRATCH, REG_SCRATCH, rb);
    if (rc) EmitCR0Update();
    StoreGPR(rd, REG_SCRATCH);
    return true;
  case 0:   // cmpw
  {
    u32 crfd = inst.CRFD;
    LoadGPR(REG_SCRATCH, ra);
    LoadGPR(REG_SCRATCH2, rb);
    m_asm.EXTSW(REG_SCRATCH, REG_SCRATCH);
    m_asm.EXTSW(REG_SCRATCH2, REG_SCRATCH2);
    m_asm.CMPW(crfd, REG_SCRATCH, REG_SCRATCH2);
    return true;
  }
  case 32:  // cmplw
  {
    u32 crfd = inst.CRFD;
    LoadGPR(REG_SCRATCH, ra);
    LoadGPR(REG_SCRATCH2, rb);
    m_asm.CMPLW(crfd, REG_SCRATCH, REG_SCRATCH2);
    return true;
  }
  case 954: // extsbx
    LoadGPR(REG_SCRATCH, ra);
    m_asm.EXTSB(REG_SCRATCH, REG_SCRATCH);
    if (rc) EmitCR0Update();
    StoreGPR(rd, REG_SCRATCH);
    return true;
  case 922: // extshx
    LoadGPR(REG_SCRATCH, ra);
    m_asm.EXTSH(REG_SCRATCH, REG_SCRATCH);
    if (rc) EmitCR0Update();
    StoreGPR(rd, REG_SCRATCH);
    return true;
  case 26:  // cntlzwx
    LoadGPR(REG_SCRATCH, ra);
    m_asm.CNTLZW(REG_SCRATCH, REG_SCRATCH);
    if (rc) EmitCR0Update();
    StoreGPR(rd, REG_SCRATCH);
    return true;
  default:
    return false;
  }
}

// ===========================================================================
// CA-using ops (opcd=31, carry in/out)
// ===========================================================================
// All use 64-bit arithmetic on zero-extended 32-bit values. The carry out
// (bit 32 of 64-bit sum) is extracted via RLDICL.
// mcrxr is rare → still falls back to interpreter.

bool JitPPC64::CompileTable31_CA(UGeckoInstruction inst)
{
  u32 xo = inst.SUBOP10;
  u32 rd = inst.RD, ra = inst.RA, rb = inst.RB;
  bool rc = inst.Rc;

  // Conventions:
  //   REG_SCRATCH (r0) = CA byte after LBZ, then used for carry extraction
  //   REG_SCRATCH2 (r11) = final result (for StoreGPR)
  //   r3-r6 = scratch for intermediates
  // After compute, REG_SCRATCH2 holds the 32-bit result. If rc, copy to
  // REG_SCRATCH for EmitCR0Update (which reads from r0).

  m_asm.LBZ(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(XER_CA_OFFSET));  // r0 = CA

  switch (xo)
  {
  case 138: // adde rd, ra, rb  → rd = ra + rb + CA
    LoadGPR(3, ra);
    LoadGPR(4, rb);
    m_asm.ADD(3, 3, 4);
    m_asm.ADD(3, 3, REG_SCRATCH);
    m_asm.RLDICL(REG_SCRATCH, 3, 32, 63);
    m_asm.OR(REG_SCRATCH2, 3, 3);
    break;

  case 136: // subfe rd, ra, rb  → rd = rb + ~ra + CA
    // Load 32-bit NOT mask = 0x00000000FFFFFFFF
    m_asm.ADDI(3, 0, -1);
    m_asm.RLDICL(3, 3, 0, 32);
    LoadGPR(4, ra);
    m_asm.XOR(4, 4, 3);        // r4 = ~ra (32-bit)
    LoadGPR(3, rb);
    m_asm.ADD(3, 3, 4);
    m_asm.ADD(3, 3, REG_SCRATCH);
    m_asm.RLDICL(REG_SCRATCH, 3, 32, 63);
    m_asm.OR(REG_SCRATCH2, 3, 3);
    break;

  case 202: // addze rd, ra  → rd = ra + CA
    LoadGPR(3, ra);
    m_asm.ADD(3, 3, REG_SCRATCH);
    m_asm.RLDICL(REG_SCRATCH, 3, 32, 63);
    m_asm.OR(REG_SCRATCH2, 3, 3);
    break;

  case 200: // subfze rd, ra  → rd = ~ra + CA
    m_asm.ADDI(3, 0, -1);
    m_asm.RLDICL(3, 3, 0, 32);
    LoadGPR(4, ra);
    m_asm.XOR(4, 4, 3);        // r4 = ~ra (32-bit)
    m_asm.ADD(4, 4, REG_SCRATCH);
    m_asm.RLDICL(REG_SCRATCH, 4, 32, 63);
    m_asm.OR(REG_SCRATCH2, 4, 4);
    break;

  case 234: // addme rd, ra  → rd = ra + CA + 0xFFFFFFFF
    m_asm.ADDI(3, 0, -1);
    m_asm.RLDICL(3, 3, 0, 32);   // r3 = 0x00000000FFFFFFFF
    LoadGPR(4, ra);
    m_asm.ADD(4, 4, REG_SCRATCH);  // r4 = ra + CA
    m_asm.ADD(4, 4, 3);            // r4 = ra + CA + 0xFFFFFFFF
    m_asm.RLDICL(REG_SCRATCH, 4, 32, 63);
    m_asm.OR(REG_SCRATCH2, 4, 4);
    break;

  case 232: // subfme rd, ra  → rd = ~ra + CA + 0xFFFFFFFF
    m_asm.ADDI(3, 0, -1);
    m_asm.RLDICL(3, 3, 0, 32);    // r3 = 0x00000000FFFFFFFF (NOT mask)
    m_asm.ADDI(4, 0, -1);
    m_asm.RLDICL(4, 4, 0, 32);    // r4 = 0x00000000FFFFFFFF
    LoadGPR(5, ra);
    m_asm.XOR(5, 5, 3);           // r5 = ~ra (32-bit)
    m_asm.ADD(5, 5, REG_SCRATCH); // r5 = ~ra + CA
    m_asm.ADD(5, 5, 4);           // r5 = ~ra + CA + 0xFFFFFFFF
    m_asm.RLDICL(REG_SCRATCH, 5, 32, 63);
    m_asm.OR(REG_SCRATCH2, 5, 5);
    break;

  default:
    return false;
  }

  // Store CA result
  m_asm.STB(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(XER_CA_OFFSET));

  // Store result and optionally update CR0
  StoreGPR(rd, REG_SCRATCH2);
  if (rc)
  {
    m_asm.OR(REG_SCRATCH, REG_SCRATCH2, REG_SCRATCH2);
    EmitCR0Update();
  }
  return true;
}
