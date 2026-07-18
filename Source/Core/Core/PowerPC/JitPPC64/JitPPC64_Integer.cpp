#include "Core/PowerPC/JitPPC64/Jit.h"

// ===========================================================================
// Integer I-form ALU compilers
//
// All use regcache: gpr.R(src) returns a host register with the source
// value; gpr.W(dst) returns a host register for the result (marked dirty).
// On block exit, gpr.Flush() writes all dirty registers to ppcState.
// ===========================================================================

bool JitPPC64::CompileADDI(UGeckoInstruction inst)
{
  u32 rd = inst.RD, ra = inst.RA;
  s32 simm = static_cast<s32>(static_cast<s16>(inst.SIMM_16));
  u32 host_rd = gpr.W(rd);
  if (ra == 0)
    m_asm.ADDI(host_rd, 0, simm);
  else
    m_asm.ADDI(host_rd, gpr.R(ra), simm);
  return true;
}

bool JitPPC64::CompileADDIS(UGeckoInstruction inst)
{
  u32 rd = inst.RD, ra = inst.RA;
  s32 simm = static_cast<s32>(static_cast<s16>(inst.SIMM_16));
  u32 host_rd = gpr.W(rd);
  if (ra == 0)
    m_asm.ADDIS(host_rd, 0, simm);
  else
    m_asm.ADDIS(host_rd, gpr.R(ra), simm);
  return true;
}

bool JitPPC64::CompileADDIC(UGeckoInstruction inst)
{
  u32 rd = inst.RD, ra = inst.RA;
  s32 simm = static_cast<s32>(static_cast<s16>(inst.SIMM_16));
  u32 host_rd = gpr.W(rd);
  u32 host_ra = gpr.R(ra);

  if (simm == 0)
  {
    // rd = ra (no carry ever)
    m_asm.OR(host_rd, host_ra, host_ra);
    m_asm.LI(REG_SCRATCH, 0);
    m_asm.STB(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(XER_CA_OFFSET));
    m_ca_known = true;
    m_ca_value = 0;
    return true;
  }

  m_asm.ADDI(host_rd, host_ra, simm);
  // CA = 1 if result < ra unsigned (carry out)
  m_asm.CMPLW(0, host_rd, host_ra);
  m_asm.MFCR(REG_SCRATCH);
  m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 0, 0, 0);  // keep CR0[LT] = bit 31
  m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 1, 31, 31); // shift bit 31→bit 0 for STB
  m_asm.STB(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(XER_CA_OFFSET));
  m_ca_known = false;
  return true;
}

bool JitPPC64::CompileADDIC_(UGeckoInstruction inst)
{
  CompileADDIC(inst);
  EmitCR0Update(gpr.R(inst.RD));
  return true;
}

bool JitPPC64::CompileMULLI(UGeckoInstruction inst)
{
  u32 rd = inst.RD, ra = inst.RA;
  s32 simm = static_cast<s32>(static_cast<s16>(inst.SIMM_16));
  m_asm.MULLI(gpr.W(rd), gpr.R(ra), simm);
  return true;
}

bool JitPPC64::CompileANDI_(UGeckoInstruction inst)
{
  u32 ra = inst.RA, rs = inst.RS;
  m_asm.ANDI_(gpr.W(ra), gpr.R(rs), inst.UIMM);
  EmitCR0Update(gpr.R(ra));
  return true;
}

bool JitPPC64::CompileANDIS_(UGeckoInstruction inst)
{
  u32 ra = inst.RA, rs = inst.RS;
  m_asm.ANDIS_(gpr.W(ra), gpr.R(rs), inst.UIMM);
  EmitCR0Update(gpr.R(ra));
  return true;
}

bool JitPPC64::CompileORI(UGeckoInstruction inst)
{
  u32 ra = inst.RA, rs = inst.RS;
  if (ra == rs && inst.UIMM == 0)
    return true;
  m_asm.ORI(gpr.W(ra), gpr.R(rs), inst.UIMM);
  return true;
}

bool JitPPC64::CompileORIS(UGeckoInstruction inst)
{
  u32 ra = inst.RA, rs = inst.RS;
  m_asm.ORIS(gpr.W(ra), gpr.R(rs), inst.UIMM);
  return true;
}

bool JitPPC64::CompileXORI(UGeckoInstruction inst)
{
  u32 ra = inst.RA, rs = inst.RS;
  m_asm.XORI(gpr.W(ra), gpr.R(rs), inst.UIMM);
  return true;
}

bool JitPPC64::CompileXORIS(UGeckoInstruction inst)
{
  u32 ra = inst.RA, rs = inst.RS;
  m_asm.XORIS(gpr.W(ra), gpr.R(rs), inst.UIMM);
  return true;
}

bool JitPPC64::CompileCMPI(UGeckoInstruction inst)
{
  u32 crfd = inst.CRFD, ra = inst.RA;
  s32 simm = static_cast<s32>(static_cast<s16>(inst.SIMM_16));
  u32 host_ra = gpr.R(ra);
  m_asm.EXTSW(REG_SCRATCH, host_ra);
  m_asm.CMPWI(crfd, REG_SCRATCH, simm);
  return true;
}

bool JitPPC64::CompileCMPLI(UGeckoInstruction inst)
{
  u32 crfd = inst.CRFD, ra = inst.RA;
  m_asm.CMPLWI(crfd, gpr.R(ra), inst.UIMM);
  return true;
}

// ===========================================================================
// Subfic (opcd=8)
// ===========================================================================

bool JitPPC64::CompileSubfic(UGeckoInstruction inst)
{
  u32 rd = inst.RD, ra = inst.RA;
  s32 simm = static_cast<s32>(static_cast<s16>(inst.SIMM_16));

  u32 host_ra = gpr.R(ra);
  u32 host_rd = gpr.W(rd);

  // rd = simm - ra
  m_asm.SUBFIC(host_rd, host_ra, simm);

  if (ra == 0)
  {
    // CA = 1 always (u32(simm) >= 0)
    m_asm.LI(REG_SCRATCH, 1);
    m_asm.STB(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(XER_CA_OFFSET));
    m_ca_known = true;
    m_ca_value = 1;
    return true;
  }

  // CA = 1 if u32(simm) >= u32(ra) unsigned (no borrow)
  m_asm.LI32(REG_SCRATCH, static_cast<u32>(static_cast<s32>(simm)));
  m_asm.CMPLW(0, REG_SCRATCH, host_ra);    // simm >= ra?
  // MFCR → keep GT (bit 30) → invert for CA
  m_asm.MFCR(REG_SCRATCH2);
  m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH2, 0, 1, 1);   // keep CR0[GT] = bit 30
  m_asm.XORI(REG_SCRATCH, REG_SCRATCH, 0x40000000);   // invert: CA = !GT
  m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 2, 31, 31);  // bit 30 → bit 0 for STB
  m_asm.STB(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(XER_CA_OFFSET));
  m_ca_known = false;
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
  case 266: // addx — regcache handles rd==ra/rd==rb aliasing automatically
    m_asm.ADD(gpr.W(rd), gpr.R(ra), gpr.R(rb));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 40:  // subfx
    m_asm.SUBF(gpr.W(rd), gpr.R(ra), gpr.R(rb));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 10:  // addcx
    {
      u32 host_ra = gpr.R(ra);
      u32 host_rb = gpr.R(rb);
      u32 host_rd = gpr.W(rd);

      // ra + 0: rd = ra, CA = 0
      if (rb == 0)
      {
        m_asm.OR(host_rd, host_ra, host_ra);
        m_asm.LI(REG_SCRATCH2, 0);
        m_asm.STB(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(XER_CA_OFFSET));
        m_ca_known = true;
        m_ca_value = 0;
      }
      else if (ra == 0)
      {
        // 0 + rb: rd = rb, CA = 0
        m_asm.OR(host_rd, host_rb, host_rb);
        m_asm.LI(REG_SCRATCH2, 0);
        m_asm.STB(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(XER_CA_OFFSET));
        m_ca_known = true;
        m_ca_value = 0;
      }
      else
      {
        m_asm.ADDC(host_rd, host_ra, host_rb);
        // CA = 1 if result < rb unsigned (carry out)
        m_asm.CMPLW(0, host_rd, host_rb);
        m_asm.MFCR(REG_SCRATCH2);
        m_asm.RLWINM(REG_SCRATCH2, REG_SCRATCH2, 0, 0, 0);  // keep CR0[LT] = bit 31
        m_asm.RLWINM(REG_SCRATCH2, REG_SCRATCH2, 1, 31, 31); // shift bit 31→bit 0 for STB
        m_asm.STB(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(XER_CA_OFFSET));
        m_ca_known = false;
      }
      if (rc) EmitCR0Update(host_rd);
    }
    return true;
  case 75:  // mulhwx
    m_asm.MULHW(gpr.W(rd), gpr.R(ra), gpr.R(rb));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 11:  // mulhwux
    m_asm.MULHWU(gpr.W(rd), gpr.R(ra), gpr.R(rb));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 235: // mullwx
    m_asm.MULLW(gpr.W(rd), gpr.R(ra), gpr.R(rb));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 491: // divwx
    m_asm.DIVW(gpr.W(rd), gpr.R(ra), gpr.R(rb));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 459: // divwux
    m_asm.DIVWU(gpr.W(rd), gpr.R(ra), gpr.R(rb));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 28:  // andx
    m_asm.AND(gpr.W(rd), gpr.R(ra), gpr.R(rb));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 60:  // andcx
    m_asm.ANDC(gpr.W(rd), gpr.R(ra), gpr.R(rb));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 444: // orx
    m_asm.OR(gpr.W(rd), gpr.R(ra), gpr.R(rb));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 412: // orcx
    m_asm.ORC(gpr.W(rd), gpr.R(ra), gpr.R(rb));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 316: // xorx
    m_asm.XOR(gpr.W(rd), gpr.R(ra), gpr.R(rb));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 476: // nandx
    m_asm.NAND(gpr.W(rd), gpr.R(ra), gpr.R(rb));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 124: // norx
    m_asm.NOR(gpr.W(rd), gpr.R(ra), gpr.R(rb));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 284: // eqvx
    m_asm.EQV(gpr.W(rd), gpr.R(ra), gpr.R(rb));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 24:  // slwx
    m_asm.SLW(gpr.W(rd), gpr.R(ra), gpr.R(rb));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 8:   // subfcx
    {
      u32 host_ra = gpr.R(ra);
      u32 host_rb = gpr.R(rb);
      u32 host_rd = gpr.W(rd);

      // subfcx rd, 0, rb: rd = rb - 0 = rb, CA = 1 (no borrow ever)
      if (ra == 0)
      {
        m_asm.OR(host_rd, host_rb, host_rb);
        m_asm.LI(REG_SCRATCH2, 1);
        m_asm.STB(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(XER_CA_OFFSET));
        m_ca_known = true;
        m_ca_value = 1;
      }
      else if (rb == 0)
      {
        // subfcx rd, ra, 0: rd = 0 - ra = NEG(ra), CA = 1 if ra == 0 else 0
        m_asm.NEG(host_rd, host_ra);
        // CA = 1 iff ra == 0 (unsigned comparison of 0 >= ra)
        m_asm.CNTLZW(REG_SCRATCH2, host_ra);
        m_asm.RLWINM(REG_SCRATCH2, REG_SCRATCH2, 5, 31, 31); // bit 26 → bit 0
        m_asm.STB(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(XER_CA_OFFSET));
        m_ca_known = false;  // depends on runtime ra value
      }
      else
      {
        m_asm.SUBFC(host_rd, host_ra, host_rb);
        // CA = 1 if rb >= u32(ra) (no borrow).
        m_asm.CMPLW(0, host_rd, host_rb);
        m_asm.MFCR(REG_SCRATCH2);
        m_asm.RLWINM(REG_SCRATCH2, REG_SCRATCH2, 0, 1, 1);  // keep CR0[GT] = bit 30
        m_asm.RLWINM(REG_SCRATCH2, REG_SCRATCH2, 2, 31, 31); // shift bit 30→bit 0 for STB
        m_asm.XORI(REG_SCRATCH2, REG_SCRATCH2, 1);           // invert: CA = !GT
        m_asm.STB(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(XER_CA_OFFSET));
        m_ca_known = false;
      }
      if (rc) EmitCR0Update(host_rd);
    }
    return true;
  case 136: // subfex — CA dependency, fallback
  case 138: // addex  — CA dependency, fallback
  case 200: // subfzex
  case 202: // addzex
  case 232: // subfmex
  case 234: // addmex
    return false;
  case 104: // negx
    m_asm.NEG(gpr.W(rd), gpr.R(ra));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 512: // mcrxr — move XER[SO,OV,CA] to CR0: CR0_SO=XER_SO, CR0_EQ=XER_OV, CR0_GT=XER_CA, CR0_LT=0
    {
      // SO at bit 0 of xer_so_ov → u32 bit 28 (CR0_SO)
      m_asm.LBZ(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(XER_SO_OV_OFFSET));
      m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 28, 28, 28);
      // OV at bit 1 of xer_so_ov → u32 bit 29 (CR0_EQ)
      m_asm.LBZ(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(XER_SO_OV_OFFSET));
      m_asm.RLWINM(REG_SCRATCH2, REG_SCRATCH2, 29, 29, 29);
      m_asm.OR(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2);
      // CA at bit 0 of xer_ca → u32 bit 30 (CR0_GT)
      m_asm.LBZ(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(XER_CA_OFFSET));
      m_asm.RLWINM(REG_SCRATCH2, REG_SCRATCH2, 30, 30, 30);
      m_asm.OR(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2);
      // u32 bit 31 (CR0_LT) stays 0
      m_asm.MTCRF(0x80, REG_SCRATCH);
    }
    return true;
  case 986: // extswx — sign-extend 32-bit word to 64-bit (nop on 32-bit Gekko)
    m_asm.EXTSW(gpr.W(rd), gpr.R(ra));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 536: // srwx
    m_asm.SRW(gpr.W(rd), gpr.R(ra), gpr.R(rb));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 792: // srawx
    m_asm.SRAW(gpr.W(rd), gpr.R(ra), gpr.R(rb));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 824: // srawix
    m_asm.SRAWI(gpr.W(rd), gpr.R(ra), rb);
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 0:   // cmpw
  {
    u32 crfd = inst.CRFD;
    u32 host_ra = gpr.R(ra);
    u32 host_rb = gpr.R(rb);
    m_asm.EXTSW(REG_SCRATCH, host_ra);
    m_asm.EXTSW(REG_SCRATCH2, host_rb);
    m_asm.CMPW(crfd, REG_SCRATCH, REG_SCRATCH2);
    return true;
  }
  case 32:  // cmplw
  {
    u32 crfd = inst.CRFD;
    m_asm.CMPLW(crfd, gpr.R(ra), gpr.R(rb));
    return true;
  }
  case 954: // extsbx
    m_asm.EXTSB(gpr.W(rd), gpr.R(ra));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 922: // extshx
    m_asm.EXTSH(gpr.W(rd), gpr.R(ra));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 26:  // cntlzwx
    m_asm.CNTLZW(gpr.W(rd), gpr.R(ra));
    if (rc) EmitCR0Update(gpr.R(rd));
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

  if (m_ca_known)
  {
    m_asm.LI(REG_SCRATCH, m_ca_value);
  }
  else
  {
    m_asm.LBZ(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(XER_CA_OFFSET));
  }

  switch (xo)
  {
  case 138: // adde rd, ra, rb  → rd = ra + rb + CA
    {
      u32 host_ra = gpr.R(ra);
      u32 host_rb = gpr.R(rb);
      u32 host_rd = gpr.W(rd);
      m_asm.ADD(3, host_ra, host_rb);
      m_asm.ADD(3, 3, REG_SCRATCH);
      m_asm.RLDICL(REG_SCRATCH, 3, 32, 63);
      m_asm.OR(host_rd, 3, 3);
      if (rc) EmitCR0Update(host_rd);
    }
    break;

  case 136: // subfe rd, ra, rb  → rd = rb + ~ra + CA
    {
      u32 host_ra = gpr.R(ra);
      u32 host_rb = gpr.R(rb);
      u32 host_rd = gpr.W(rd);
      m_asm.ADDI(3, 0, -1);
      m_asm.RLDICL(3, 3, 0, 32);
      m_asm.XOR(4, host_ra, 3);        // r4 = ~ra (32-bit)
      m_asm.ADD(3, host_rb, 4);
      m_asm.ADD(3, 3, REG_SCRATCH);
      m_asm.RLDICL(REG_SCRATCH, 3, 32, 63);
      m_asm.OR(host_rd, 3, 3);
      if (rc) EmitCR0Update(host_rd);
    }
    break;

  case 202: // addze rd, ra  → rd = ra + CA
    {
      u32 host_ra = gpr.R(ra);
      u32 host_rd = gpr.W(rd);
      m_asm.ADD(3, host_ra, REG_SCRATCH);
      m_asm.RLDICL(REG_SCRATCH, 3, 32, 63);
      m_asm.OR(host_rd, 3, 3);
      if (rc) EmitCR0Update(host_rd);
    }
    break;

  case 200: // subfze rd, ra  → rd = ~ra + CA
    {
      u32 host_ra = gpr.R(ra);
      u32 host_rd = gpr.W(rd);
      m_asm.ADDI(3, 0, -1);
      m_asm.RLDICL(3, 3, 0, 32);
      m_asm.XOR(4, host_ra, 3);        // r4 = ~ra (32-bit)
      m_asm.ADD(4, 4, REG_SCRATCH);
      m_asm.RLDICL(REG_SCRATCH, 4, 32, 63);
      m_asm.OR(host_rd, 4, 4);
      if (rc) EmitCR0Update(host_rd);
    }
    break;

  case 234: // addme rd, ra  → rd = ra + CA + 0xFFFFFFFF
    {
      u32 host_ra = gpr.R(ra);
      u32 host_rd = gpr.W(rd);
      m_asm.ADDI(3, 0, -1);
      m_asm.RLDICL(3, 3, 0, 32);       // r3 = 0x00000000FFFFFFFF
      m_asm.ADD(4, host_ra, REG_SCRATCH); // r4 = ra + CA
      m_asm.ADD(4, 4, 3);               // r4 = ra + CA + 0xFFFFFFFF
      m_asm.RLDICL(REG_SCRATCH, 4, 32, 63);
      m_asm.OR(host_rd, 4, 4);
      if (rc) EmitCR0Update(host_rd);
    }
    break;

  case 232: // subfme rd, ra  → rd = ~ra + CA + 0xFFFFFFFF
    {
      u32 host_ra = gpr.R(ra);
      u32 host_rd = gpr.W(rd);
      m_asm.ADDI(3, 0, -1);
      m_asm.RLDICL(3, 3, 0, 32);       // r3 = 0x00000000FFFFFFFF
      m_asm.ADDI(4, 0, -1);
      m_asm.RLDICL(4, 4, 0, 32);       // r4 = 0x00000000FFFFFFFF
      m_asm.XOR(5, host_ra, 3);         // r5 = ~ra (32-bit)
      m_asm.ADD(5, 5, REG_SCRATCH);    // r5 = ~ra + CA
      m_asm.ADD(5, 5, 4);              // r5 = ~ra + CA + 0xFFFFFFFF
      m_asm.RLDICL(REG_SCRATCH, 5, 32, 63);
      m_asm.OR(host_rd, 5, 5);
      if (rc) EmitCR0Update(host_rd);
    }
    break;

  default:
    return false;
  }

  // Store CA result
  m_asm.STB(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(XER_CA_OFFSET));
  // CA output depends on runtime operands — no longer constant
  m_ca_known = false;
  return true;
}
