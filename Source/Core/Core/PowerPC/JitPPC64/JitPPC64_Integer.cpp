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
    m_ca_known = true;
    m_ca_value = 0;
    m_ca_in_r0 = true;
    m_ca_dirty = true;
    return true;
  }

  m_asm.ADDI(host_rd, host_ra, simm);
  // CA = 1 if result < ra unsigned (carry out)
  m_asm.CMPLW(0, host_rd, host_ra);
  m_asm.MFCR(REG_SCRATCH);
  m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 0, 0, 0);  // keep CR0[LT] = bit 31
  m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 1, 31, 31); // shift bit 31→bit 0
  m_ca_in_r0 = true;
  m_ca_dirty = true;
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
  if (crfd == 0) m_cr0_native_valid = true;
  return true;
}

bool JitPPC64::CompileCMPLI(UGeckoInstruction inst)
{
  u32 crfd = inst.CRFD, ra = inst.RA;
  m_asm.CMPLWI(crfd, gpr.R(ra), inst.UIMM);
  if (crfd == 0) m_cr0_native_valid = true;
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
    m_ca_known = true;
    m_ca_value = 1;
    m_ca_in_r0 = true;
    m_ca_dirty = true;
    return true;
  }

  // CA = 1 if u32(simm) >= u32(ra) unsigned (no borrow)
  m_asm.LI32(REG_SCRATCH, static_cast<u32>(static_cast<s32>(simm)));
  m_asm.CMPLW(0, REG_SCRATCH, host_ra);    // simm >= ra?
  // MFCR → keep GT (bit 30) → invert for CA
  m_asm.MFCR(REG_SCRATCH2);
  m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH2, 0, 1, 1);   // keep CR0[GT] = bit 30
  m_asm.XORI(REG_SCRATCH, REG_SCRATCH, 0x40000000);   // invert: CA = !GT
  m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 2, 31, 31);  // bit 30 → bit 0
  m_ca_in_r0 = true;
  m_ca_dirty = true;
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
// OE helpers — compute signed overflow flags
//
// These standalone helpers (not JitPPC64 members) use host register constants
// that shadow the JitPPC64 static constexpr members.
// ===========================================================================

namespace
{
constexpr u32 REG_SCRATCH = 0;
constexpr u32 REG_SCRATCH2 = 11;
constexpr u32 REG_SP = 1;
}  // namespace

// All helpers save s_a to a stack slot, compute the needed term, then
// reload and combine after the arithmetic op. This avoids the register
// aliasing problem (ra/rb may equal rd, which gets clobbered by the op).
// ===========================================================================

// ADD-family overflow: ~(sa ^ sb) & (sa ^ sr) — use for add, addc, adde, addze, addme
// Emits before the arithmetic op: saves s_a and ~(sa^sb) to stack
// Caller must emit the arithmetic op after this, then call EmitAddOverflowAfter()
static void EmitAddOverflowBefore(PPC64Assembler& a, u32 stack_off,
                                   u32 host_ra, u32 host_rb)
{
  // s_a = sign bit of ra
  a.RLWINM(REG_SCRATCH, host_ra, 1, 31, 31);   // r0 = s_a
  a.STB(REG_SCRATCH, REG_SP, stack_off);          // stack[0] = s_a
  // s_b = sign bit of rb
  a.RLWINM(REG_SCRATCH2, host_rb, 1, 31, 31);   // r11 = s_b
  a.XOR(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2);  // r0 = s_a ^ s_b
  a.XORI(REG_SCRATCH, REG_SCRATCH, 1);            // r0 = ~(s_a ^ s_b) & 1
  a.STB(REG_SCRATCH, REG_SP, stack_off + 1);      // stack[1] = not_xor_ab
}

static void EmitAddOverflowAfter(PPC64Assembler& a, u32 stack_off,
                                  u32 host_rd)
{
  // s_r = sign bit of result
  a.RLWINM(REG_SCRATCH2, host_rd, 1, 31, 31);    // r11 = s_r
  // reload s_a
  a.LBZ(REG_SCRATCH, REG_SP, stack_off);           // r0 = s_a
  a.XOR(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2);   // r0 = s_a ^ s_r
  // reload ~(s_a ^ s_b) and combine
  a.LBZ(REG_SCRATCH2, REG_SP, stack_off + 1);      // r11 = not_xor_ab
  a.AND(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2);   // r0 = overflow bit
}

// SUBF-family overflow: (sa ^ sb) & (sb ^ sr) — use for subf, subfc, subfe, subfze, subfme
static void EmitSubfOverflowBefore(PPC64Assembler& a, u32 stack_off,
                                    u32 host_ra, u32 host_rb)
{
  a.RLWINM(REG_SCRATCH, host_ra, 1, 31, 31);    // r0 = s_a
  a.RLWINM(REG_SCRATCH2, host_rb, 1, 31, 31);   // r11 = s_b
  a.XOR(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2);  // r0 = s_a ^ s_b
  a.STB(REG_SCRATCH, REG_SP, stack_off);           // stack[0] = sa_xor_sb
  a.STB(REG_SCRATCH2, REG_SP, stack_off + 1);      // stack[1] = s_b
}

static void EmitSubfOverflowAfter(PPC64Assembler& a, u32 stack_off,
                                   u32 host_rd)
{
  a.RLWINM(REG_SCRATCH2, host_rd, 1, 31, 31);    // r11 = s_r
  a.LBZ(REG_SCRATCH, REG_SP, stack_off + 1);       // r0 = s_b
  a.XOR(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH);  // r11 = s_b ^ s_r
  a.LBZ(REG_SCRATCH, REG_SP, stack_off);            // r0 = sa_xor_sb
  a.AND(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2);   // r0 = overflow bit
}

// ===========================================================================
// Table 31 (opcd=31) — integer ALU subset
//
// Dispatches on 9-bit xo (inst.SUBOP10 & 0x1FF) and checks inst.OE separately.
// The OE bit (PPC bit 21) is part of the full SUBOP10 field, so OE=1 variants
// have xo10 = (OE << 9) | xo9.  We strip OE by masking off the top bit.
// ===========================================================================

bool JitPPC64::CompileTable31_Integer(UGeckoInstruction inst)
{
  u32 xo9 = inst.SUBOP10 & 0x1FF;  // 9-bit xo, excludes OE
  u32 rd = inst.RD, ra = inst.RA, rb = inst.RB;
  bool rc = inst.Rc;
  bool oe = inst.OE;

  switch (xo9)
  {
  case 266: // addx / addox
    {
      u32 host_rd = gpr.W(rd);
      u32 host_ra = gpr.R(ra);
      u32 host_rb = gpr.R(rb);
      if (oe)
      {
        EmitAddOverflowBefore(m_asm, R2_SAVE_OFFSET, host_ra, host_rb);
        m_asm.ADD(host_rd, host_ra, host_rb, rc, true);
        EmitAddOverflowAfter(m_asm, R2_SAVE_OFFSET, host_rd);
        EmitSetXER_OV(REG_SCRATCH);
      }
      else
      {
        m_asm.ADD(host_rd, host_ra, host_rb, rc);
      }
      if (rc) EmitCR0Update(host_rd);
    }
    return true;
  case 40:  // subfx / subfox
    {
      u32 host_rd = gpr.W(rd);
      u32 host_ra = gpr.R(ra);
      u32 host_rb = gpr.R(rb);
      if (oe)
      {
        EmitSubfOverflowBefore(m_asm, R2_SAVE_OFFSET, host_ra, host_rb);
        m_asm.SUBF(host_rd, host_ra, host_rb, rc, true);
        EmitSubfOverflowAfter(m_asm, R2_SAVE_OFFSET, host_rd);
        EmitSetXER_OV(REG_SCRATCH);
      }
      else
      {
        m_asm.SUBF(host_rd, host_ra, host_rb, rc);
      }
      if (rc) EmitCR0Update(host_rd);
    }
    return true;
  case 10:  // addcx / addcox
    {
      u32 host_ra = gpr.R(ra);
      u32 host_rb = gpr.R(rb);
      u32 host_rd = gpr.W(rd);

      // ra + 0: rd = ra, CA = 0
      if (rb == 0)
      {
        m_asm.OR(host_rd, host_ra, host_ra);
        m_asm.LI(REG_SCRATCH2, 0);
        m_ca_known = true;
        m_ca_value = 0;
        m_ca_in_r0 = true;
        m_ca_dirty = true;
        if (oe)
        {
          m_asm.LI(REG_SCRATCH, 0);  // OV = 0 (no overflow when adding 0)
          EmitSetXER_OV(REG_SCRATCH);
        }
      }
      else if (ra == 0)
      {
        m_asm.OR(host_rd, host_rb, host_rb);
        m_asm.LI(REG_SCRATCH2, 0);
        m_ca_known = true;
        m_ca_value = 0;
        m_ca_in_r0 = true;
        m_ca_dirty = true;
        if (oe)
        {
          m_asm.LI(REG_SCRATCH, 0);  // OV = 0 (no overflow when adding 0)
          EmitSetXER_OV(REG_SCRATCH);
        }
      }
      else
      {
        if (oe)
          EmitAddOverflowBefore(m_asm, R2_SAVE_OFFSET, host_ra, host_rb);
        m_asm.ADDC(host_rd, host_ra, host_rb, rc, oe);
        if (oe)
        {
          EmitAddOverflowAfter(m_asm, R2_SAVE_OFFSET, host_rd);
          EmitSetXER_OV(REG_SCRATCH);
        }
        // CA = 1 if result < rb unsigned (carry out)
        m_asm.CMPLW(0, host_rd, host_rb);
        m_asm.MFCR(REG_SCRATCH2);
        m_asm.RLWINM(REG_SCRATCH2, REG_SCRATCH2, 0, 0, 0);  // keep CR0[LT] = bit 31
        m_asm.RLWINM(REG_SCRATCH2, REG_SCRATCH2, 1, 31, 31); // shift bit 31→bit 0
        m_ca_in_r0 = true;
        m_ca_dirty = true;
        m_ca_known = false;
      }
      if (rc) EmitCR0Update(host_rd);
    }
    return true;
  case 8:   // subfcx / subfcox
    {
      u32 host_ra = gpr.R(ra);
      u32 host_rb = gpr.R(rb);
      u32 host_rd = gpr.W(rd);

      // subfcx rd, 0, rb: rd = rb - 0 = rb, CA = 1 (no borrow ever)
      if (ra == 0)
      {
        m_asm.OR(host_rd, host_rb, host_rb);
        m_asm.LI(REG_SCRATCH2, 1);
        m_ca_known = true;
        m_ca_value = 1;
        m_ca_in_r0 = true;
        m_ca_dirty = true;
        if (oe) { m_asm.LI(REG_SCRATCH, 0); EmitSetXER_OV(REG_SCRATCH); }
      }
      else if (rb == 0)
      {
        m_asm.NEG(host_rd, host_ra);
        if (oe)
        {
          // NEG overflow: ra == 0x80000000
          // For subfc rd, ra, 0: rd = -ra, overflow same as NEG
          m_asm.LI32(REG_SCRATCH, 0x80000000);
          m_asm.XOR(REG_SCRATCH, REG_SCRATCH, host_ra);
          m_asm.CNTLZW(REG_SCRATCH, REG_SCRATCH);
          m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 5, 31, 31); // !!(ra == 0x80000000)
          EmitSetXER_OV(REG_SCRATCH);
        }
        // CA = 1 iff ra == 0 (unsigned comparison of 0 >= ra)
        m_asm.CNTLZW(REG_SCRATCH2, host_ra);
        m_asm.RLWINM(REG_SCRATCH2, REG_SCRATCH2, 5, 31, 31);
        m_ca_in_r0 = true;
        m_ca_dirty = true;
        m_ca_known = false;
      }
      else
      {
        if (oe)
          EmitSubfOverflowBefore(m_asm, R2_SAVE_OFFSET, host_ra, host_rb);
        m_asm.SUBFC(host_rd, host_ra, host_rb, rc, oe);
        if (oe)
        {
          EmitSubfOverflowAfter(m_asm, R2_SAVE_OFFSET, host_rd);
          EmitSetXER_OV(REG_SCRATCH);
        }
        // CA = 1 if rb >= u32(ra) (no borrow).
        m_asm.CMPLW(0, host_rd, host_rb);
        m_asm.MFCR(REG_SCRATCH2);
        m_asm.RLWINM(REG_SCRATCH2, REG_SCRATCH2, 0, 1, 1);  // keep CR0[GT] = bit 30
        m_asm.RLWINM(REG_SCRATCH2, REG_SCRATCH2, 2, 31, 31);
        m_asm.XORI(REG_SCRATCH2, REG_SCRATCH2, 1);           // invert: CA = !GT
        m_ca_in_r0 = true;
        m_ca_dirty = true;
        m_ca_known = false;
      }
      if (rc) EmitCR0Update(host_rd);
    }
    return true;
  case 75:  // mulhwx (no OE)
    m_asm.MULHW(gpr.W(rd), gpr.R(ra), gpr.R(rb));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 11:  // mulhwux (no OE)
    m_asm.MULHWU(gpr.W(rd), gpr.R(ra), gpr.R(rb));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 235: // mullwx / mullwox
    {
      u32 host_rd = gpr.W(rd);
      u32 host_ra = gpr.R(ra);
      u32 host_rb = gpr.R(rb);
      if (oe)
      {
        // overflow = (mulhw result != sra(low_result, 31))
        // Compute MULHW first, MULLW second (preserves host_ra/rb for MULHW)
        m_asm.MULHW(REG_SCRATCH, host_ra, host_rb);     // r0 = high product
        m_asm.MULLW(host_rd, host_ra, host_rb, rc, true);
        m_asm.SRAWI(REG_SCRATCH2, host_rd, 31);           // r11 = sign-extend of low
        m_asm.XOR(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2); // r0 = high != sign_ext? (1 if overflow)
        m_asm.CNTLZW(REG_SCRATCH, REG_SCRATCH);
        m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 5, 31, 31); // normalize to 0/1
        EmitSetXER_OV(REG_SCRATCH);
      }
      else
      {
        m_asm.MULLW(host_rd, host_ra, host_rb, rc);
      }
      if (rc) EmitCR0Update(host_rd);
    }
    return true;
  case 491: // divwx / divwox
    {
      u32 host_rd = gpr.W(rd);
      u32 host_ra = gpr.R(ra);
      u32 host_rb = gpr.R(rb);
      if (oe)
      {
        // Overflow = (rb == 0) || (ra == INT_MIN && rb == -1)
        // Check rb == 0:
        m_asm.CNTLZW(REG_SCRATCH, host_rb);
        m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 5, 31, 31); // r0 = (rb==0)
        // Check ra == INT_MIN && rb == -1:
        m_asm.LI32(REG_SCRATCH2, 0x80000000);
        m_asm.XOR(REG_SCRATCH2, REG_SCRATCH2, host_ra);      // r11 = ra ^ INT_MIN
        m_asm.CNTLZW(REG_SCRATCH2, REG_SCRATCH2);
        m_asm.RLWINM(REG_SCRATCH2, REG_SCRATCH2, 5, 31, 31); // r11 = (ra == INT_MIN)
        m_asm.LI32(REG_SCRATCH, 0xFFFFFFFF);
        m_asm.XOR(REG_SCRATCH, REG_SCRATCH, host_rb);        // r0 = rb ^ -1
        m_asm.CNTLZW(REG_SCRATCH, REG_SCRATCH);
        m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 5, 31, 31);   // r0 = (rb == -1)
        m_asm.AND(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH);   // r11 = (ra==INT_MIN && rb==-1)
        m_asm.OR(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2);    // r0 = overflow
        m_asm.DIVW(host_rd, host_ra, host_rb, rc, true);
        EmitSetXER_OV(REG_SCRATCH);
      }
      else
      {
        m_asm.DIVW(host_rd, host_ra, host_rb, rc);
      }
      if (rc) EmitCR0Update(host_rd);
    }
    return true;
  case 459: // divwux / divwuox
    {
      u32 host_rd = gpr.W(rd);
      u32 host_ra = gpr.R(ra);
      u32 host_rb = gpr.R(rb);
      if (oe)
      {
        // Overflow = (rb == 0)
        m_asm.CNTLZW(REG_SCRATCH, host_rb);
        m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 5, 31, 31);   // r0 = (rb==0)
        m_asm.DIVWU(host_rd, host_ra, host_rb, rc, true);
        EmitSetXER_OV(REG_SCRATCH);
      }
      else
      {
        m_asm.DIVWU(host_rd, host_ra, host_rb, rc);
      }
      if (rc) EmitCR0Update(host_rd);
    }
    return true;
  case 136: // subfex / subfeox — handled in CompileTable31_CA
  case 138: // addex  / addeox
  case 200: // subfzex / subfzeox
  case 202: // addzex / addzeox
  case 232: // subfmex / subfmeox
  case 234: // addmex / addmeox
    return false;
  case 28:  // andx (no OE)
    m_asm.AND(gpr.W(rd), gpr.R(ra), gpr.R(rb));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 60:  // andcx (no OE)
    m_asm.ANDC(gpr.W(rd), gpr.R(ra), gpr.R(rb));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 444: // orx (no OE)
    m_asm.OR(gpr.W(rd), gpr.R(ra), gpr.R(rb));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 412: // orcx (no OE)
    m_asm.ORC(gpr.W(rd), gpr.R(ra), gpr.R(rb));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 316: // xorx (no OE)
    m_asm.XOR(gpr.W(rd), gpr.R(ra), gpr.R(rb));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 476: // nandx (no OE)
    m_asm.NAND(gpr.W(rd), gpr.R(ra), gpr.R(rb));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 124: // norx (no OE)
    m_asm.NOR(gpr.W(rd), gpr.R(ra), gpr.R(rb));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 284: // eqvx (no OE)
    m_asm.EQV(gpr.W(rd), gpr.R(ra), gpr.R(rb));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 24:  // slwx (no OE)
    m_asm.SLW(gpr.W(rd), gpr.R(ra), gpr.R(rb));
    if (rc) EmitCR0Update(gpr.R(rd));
    return true;
  case 104: // negx / negox
    {
      u32 host_rd = gpr.W(rd);
      u32 host_ra = gpr.R(ra);
      if (oe)
      {
        // overflow = (ra == 0x80000000)
        m_asm.LI32(REG_SCRATCH, 0x80000000);
        m_asm.XOR(REG_SCRATCH, REG_SCRATCH, host_ra);
        m_asm.CNTLZW(REG_SCRATCH, REG_SCRATCH);
        m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 5, 31, 31); // bit 26 → bit 0
        m_asm.NEG(host_rd, host_ra, rc, true);
        EmitSetXER_OV(REG_SCRATCH);
      }
      else
      {
        m_asm.NEG(host_rd, host_ra, rc);
      }
      if (rc) EmitCR0Update(host_rd);
    }
    return true;
  case 512: // mcrxr
    {
      // mcrxr crD: XER[SO,OV,CA] → CR[D][LT,GT,EQ], CR[D][SO]=0
      FlushCR0IfDirty();
      FlushCarry();

      // SO (xer_so_ov bit 1 = PPC bit 30 after LBZ) → CR0_LT (= src bit 28 = PPC bit 3)
      m_asm.LBZ(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(XER_SO_OV_OFFSET));
      m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 27, 3, 3);
      // OV (xer_so_ov bit 0 = PPC bit 31 after LBZ) → CR0_GT (= src bit 29 = PPC bit 2)
      m_asm.LBZ(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(XER_SO_OV_OFFSET));
      m_asm.RLWINM(REG_SCRATCH2, REG_SCRATCH2, 29, 2, 2);
      m_asm.OR(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2);
      // CA (xer_ca bit 0 = PPC bit 31 after LBZ) → CR0_EQ (= src bit 30 = PPC bit 1)
      m_asm.LBZ(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(XER_CA_OFFSET));
      m_asm.RLWINM(REG_SCRATCH2, REG_SCRATCH2, 30, 1, 1);
      m_asm.OR(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2);
      // CR0_SO (src bit 31 = PPC bit 0) stays 0
      m_asm.MTCRF(0x01, REG_SCRATCH);
      m_cr0_native_valid = true;
      // Clear SO, OV, CA in XER
      m_asm.LI(REG_SCRATCH, 0);
      m_asm.STB(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(XER_SO_OV_OFFSET));
      m_asm.STB(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(XER_CA_OFFSET));
      m_ca_known = true;
      m_ca_value = 0;
      m_ca_in_r0 = false;
      m_ca_dirty = false;
    }
    return true;
  case 986: // extswx
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
    if (crfd == 0) m_cr0_native_valid = true;
    return true;
  }
  case 32:  // cmplw
  {
    u32 crfd = inst.CRFD;
    m_asm.CMPLW(crfd, gpr.R(ra), gpr.R(rb));
    if (crfd == 0) m_cr0_native_valid = true;
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
  u32 xo9 = inst.SUBOP10 & 0x1FF;  // 9-bit xo excludes OE
  u32 rd = inst.RD, ra = inst.RA, rb = inst.RB;
  bool rc = inst.Rc;
  bool oe = inst.OE;

  // Conventions:
  //   REG_SCRATCH (r0) = CA byte after LBZ, then used for carry extraction
  //   REG_SCRATCH2 (r11) = used for OE overflow temp
  //   r3-r6 = scratch for intermediates
  //   r4-r5 = OE extraction temps

  if (m_ca_known)
  {
    m_asm.LI(REG_SCRATCH, m_ca_value);
  }
  else if (m_ca_in_r0)
  {
    // CA already in REG_SCRATCH from a previous op — nothing to do
  }
  else
  {
    m_asm.LBZ(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(XER_CA_OFFSET));
  }

  switch (xo9)
  {
  case 138: // adde / addeox  → rd = ra + rb + CA
    {
      u32 host_ra = gpr.R(ra);
      u32 host_rb = gpr.R(rb);
      u32 host_rd = gpr.W(rd);
      m_asm.ADD(3, host_ra, host_rb);
      m_asm.ADD(3, 3, REG_SCRATCH);    // r3 = 64-bit zero-extended result

      if (oe)
      {
        // OV = a31 ^ b31 ^ bit31 ^ bit32
        m_asm.RLDICL(4, 3, 33, 63);    // r4 = bit31 (sign of result)
        m_asm.RLDICL(5, 3, 32, 63);    // r5 = bit32 (carry)
        m_asm.XOR(REG_SCRATCH2, 4, 5); // r11 = bit31 ^ bit32
        m_asm.RLWINM(4, host_ra, 1, 31, 31); // r4 = a31
        m_asm.RLWINM(5, host_rb, 1, 31, 31); // r5 = b31
        m_asm.XOR(REG_SCRATCH, 4, 5);       // r0 = a31 ^ b31
        m_asm.XOR(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2); // r0 = OV
        EmitSetXER_OV(REG_SCRATCH);
        m_asm.RLDICL(REG_SCRATCH, 3, 32, 63); // reload CA
      }
      else
      {
        m_asm.RLDICL(REG_SCRATCH, 3, 32, 63); // CA
      }

      m_asm.OR(host_rd, 3, 3);
      if (rc) EmitCR0Update(host_rd);
    }
    break;

  case 136: // subfe / subfeox  → rd = rb + ~ra + CA
    {
      u32 host_ra = gpr.R(ra);
      u32 host_rb = gpr.R(rb);
      u32 host_rd = gpr.W(rd);
      m_asm.ADDI(3, 0, -1);
      m_asm.RLDICL(3, 3, 0, 32);
      m_asm.XOR(4, host_ra, 3);        // r4 = ~ra (32-bit)
      m_asm.ADD(3, host_rb, 4);
      m_asm.ADD(3, 3, REG_SCRATCH);    // r3 = 64-bit result

      if (oe)
      {
        // OV = a31 ^ b31 ^ bit31 ^ bit32
        // a31 = rb[31], b31 = ~ra[31] = 1 ^ ra[31]
        m_asm.RLDICL(4, 3, 33, 63);    // r4 = bit31
        m_asm.RLDICL(5, 3, 32, 63);    // r5 = bit32
        m_asm.XOR(REG_SCRATCH2, 4, 5); // r11 = bit31 ^ bit32
        m_asm.RLWINM(4, host_rb, 1, 31, 31); // r4 = a31 = rb[31]
        m_asm.RLWINM(5, host_ra, 1, 31, 31); // r5 = ra[31]
        m_asm.XORI(5, 5, 1);                 // r5 = ~ra[31] = b31
        m_asm.XOR(REG_SCRATCH, 4, 5);       // r0 = a31 ^ b31
        m_asm.XOR(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2); // r0 = OV
        EmitSetXER_OV(REG_SCRATCH);
        m_asm.RLDICL(REG_SCRATCH, 3, 32, 63); // reload CA
      }
      else
      {
        m_asm.RLDICL(REG_SCRATCH, 3, 32, 63); // CA
      }

      m_asm.OR(host_rd, 3, 3);
      if (rc) EmitCR0Update(host_rd);
    }
    break;

  case 202: // addze / addzeox  → rd = ra + CA
    {
      u32 host_ra = gpr.R(ra);
      u32 host_rd = gpr.W(rd);
      m_asm.ADD(3, host_ra, REG_SCRATCH); // r3 = 64-bit result

      if (oe)
      {
        // OV = a31 ^ 0 ^ bit31 ^ bit32 = a31 ^ bit31 ^ bit32
        m_asm.RLDICL(4, 3, 33, 63);    // r4 = bit31
        m_asm.RLDICL(5, 3, 32, 63);    // r5 = bit32
        m_asm.XOR(REG_SCRATCH2, 4, 5); // r11 = bit31 ^ bit32
        m_asm.RLWINM(4, host_ra, 1, 31, 31); // r4 = a31
        m_asm.XOR(REG_SCRATCH, 4, REG_SCRATCH2); // r0 = a31 ^ bit31 ^ bit32 = OV
        EmitSetXER_OV(REG_SCRATCH);
        m_asm.RLDICL(REG_SCRATCH, 3, 32, 63); // reload CA
      }
      else
      {
        m_asm.RLDICL(REG_SCRATCH, 3, 32, 63); // CA
      }

      m_asm.OR(host_rd, 3, 3);
      if (rc) EmitCR0Update(host_rd);
    }
    break;

  case 200: // subfze / subfzeox  → rd = ~ra + CA
    {
      u32 host_ra = gpr.R(ra);
      u32 host_rd = gpr.W(rd);
      m_asm.ADDI(3, 0, -1);
      m_asm.RLDICL(3, 3, 0, 32);
      m_asm.XOR(4, host_ra, 3);        // r4 = ~ra (32-bit)
      m_asm.ADD(4, 4, REG_SCRATCH);    // r4 = 64-bit result

      if (oe)
      {
        // OV = a31 ^ 0 ^ bit31 ^ bit32 = a31 ^ bit31 ^ bit32
        // a31 = ~ra[31] = 1 ^ ra[31]
        m_asm.RLDICL(3, 4, 33, 63);    // r3 = bit31
        m_asm.RLDICL(5, 4, 32, 63);    // r5 = bit32
        m_asm.XOR(REG_SCRATCH2, 3, 5); // r11 = bit31 ^ bit32
        m_asm.RLWINM(3, host_ra, 1, 31, 31); // r3 = ra[31]
        m_asm.XORI(3, 3, 1);                 // r3 = ~ra[31] = a31
        m_asm.XOR(REG_SCRATCH, 3, REG_SCRATCH2); // r0 = a31 ^ bit31 ^ bit32 = OV
        EmitSetXER_OV(REG_SCRATCH);
        m_asm.RLDICL(REG_SCRATCH, 4, 32, 63); // reload CA from r4
      }
      else
      {
        m_asm.RLDICL(REG_SCRATCH, 4, 32, 63); // CA
      }

      m_asm.OR(host_rd, 4, 4);
      if (rc) EmitCR0Update(host_rd);
    }
    break;

  case 234: // addme / addmeox  → rd = ra + CA + 0xFFFFFFFF
    {
      u32 host_ra = gpr.R(ra);
      u32 host_rd = gpr.W(rd);
      m_asm.ADDI(3, 0, -1);
      m_asm.RLDICL(3, 3, 0, 32);       // r3 = 0x00000000FFFFFFFF
      m_asm.ADD(4, host_ra, REG_SCRATCH); // r4 = ra + CA
      m_asm.ADD(4, 4, 3);               // r4 = ra + CA + 0xFFFFFFFF (64-bit result)

      if (oe)
      {
        // OV = a31 ^ 1 ^ bit31 ^ bit32 = ra[31] ^ 1 ^ bit31 ^ bit32
        m_asm.RLDICL(3, 4, 33, 63);    // r3 = bit31
        m_asm.RLDICL(5, 4, 32, 63);    // r5 = bit32
        m_asm.XOR(REG_SCRATCH2, 3, 5); // r11 = bit31 ^ bit32
        m_asm.RLWINM(3, host_ra, 1, 31, 31); // r3 = ra[31]
        m_asm.XORI(REG_SCRATCH, 3, 1);       // r0 = ra[31] ^ 1
        m_asm.XOR(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2); // r0 = OV
        EmitSetXER_OV(REG_SCRATCH);
        m_asm.RLDICL(REG_SCRATCH, 4, 32, 63); // reload CA from r4
      }
      else
      {
        m_asm.RLDICL(REG_SCRATCH, 4, 32, 63); // CA
      }

      m_asm.OR(host_rd, 4, 4);
      if (rc) EmitCR0Update(host_rd);
    }
    break;

  case 232: // subfme / subfmeox  → rd = ~ra + CA + 0xFFFFFFFF
    {
      u32 host_ra = gpr.R(ra);
      u32 host_rd = gpr.W(rd);
      m_asm.ADDI(3, 0, -1);
      m_asm.RLDICL(3, 3, 0, 32);       // r3 = 0x00000000FFFFFFFF
      m_asm.ADDI(4, 0, -1);
      m_asm.RLDICL(4, 4, 0, 32);       // r4 = 0x00000000FFFFFFFF
      m_asm.XOR(5, host_ra, 3);         // r5 = ~ra (32-bit)
      m_asm.ADD(5, 5, REG_SCRATCH);    // r5 = ~ra + CA
      m_asm.ADD(5, 5, 4);              // r5 = ~ra + CA + 0xFFFFFFFF (64-bit result)

      if (oe)
      {
        // OV = a31 ^ b31 ^ bit31 ^ bit32
        // a31 = ~ra[31] = 1 ^ ra[31], b31 = 1
        // Simplified: OV = ra[31] ^ bit31 ^ bit32  (since 1^1=0)
        m_asm.RLDICL(3, 5, 33, 63);    // r3 = bit31
        m_asm.RLDICL(4, 5, 32, 63);    // r4 = bit32
        m_asm.XOR(REG_SCRATCH2, 3, 4); // r11 = bit31 ^ bit32
        m_asm.RLWINM(3, host_ra, 1, 31, 31); // r3 = ra[31]
        m_asm.XOR(REG_SCRATCH, 3, REG_SCRATCH2); // r0 = ra[31] ^ bit31 ^ bit32 = OV
        EmitSetXER_OV(REG_SCRATCH);
        m_asm.RLDICL(REG_SCRATCH, 5, 32, 63); // reload CA from r5
      }
      else
      {
        m_asm.RLDICL(REG_SCRATCH, 5, 32, 63); // CA
      }

      m_asm.OR(host_rd, 5, 5);
      if (rc) EmitCR0Update(host_rd);
    }
    break;

  default:
    return false;
  }

  // CA is now in REG_SCRATCH; defer store to block exit or next CA read.
  m_ca_in_r0 = true;
  m_ca_dirty = true;
  m_ca_known = false;
  return true;
}
