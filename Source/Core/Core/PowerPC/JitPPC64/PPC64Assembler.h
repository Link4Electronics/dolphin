#pragma once

#include <cstdint>
#include <cstring>
#include <cstddef>

class PPC64Assembler
{
public:
  PPC64Assembler() = default;
  explicit PPC64Assembler(u8* buf, size_t cap) : m_code(buf), m_capacity(cap) {}
  void SetBase(u8* buf, size_t cap) { m_code = buf; m_capacity = cap; m_pos = 0; }

  u8* Code() const { return m_code; }
  size_t Size() const { return m_pos; }
  size_t Capacity() const { return m_capacity; }
  size_t Remaining() const { return m_capacity - m_pos; }

  void Write32(u32 insn)
  {
    if (m_pos + 4 <= m_capacity)
      std::memcpy(m_code + m_pos, &insn, 4);
    m_pos += 4;
  }

  // -- Arithmetic (opcd=31, various XO) --
  void ADD(u32 rd, u32 ra, u32 rb, bool rc = false) { Write32(X(31, rd, ra, rb, 266, rc)); }
  void ADDO(u32 rd, u32 ra, u32 rb) { Write32(X(31, rd, ra, rb, 266, 1)); }
  void ADDC(u32 rd, u32 ra, u32 rb, bool rc = false) { Write32(X(31, rd, ra, rb, 10, rc)); }
  void ADDE(u32 rd, u32 ra, u32 rb, bool rc = false) { Write32(X(31, rd, ra, rb, 138, rc)); }
  void ADDIC(u32 rd, u32 ra, s32 sim) { Write32(D(12, rd, ra, sim)); }
  void ADDIC_(u32 rd, u32 ra, s32 sim) { Write32(D(13, rd, ra, sim)); }
  void ADDIS(u32 rd, u32 ra, s32 sim) { Write32(D(15, rd, ra, sim)); }
  void ADDI(u32 rd, u32 ra, s32 sim) { Write32(D(14, rd, ra, sim)); }
  void LI(u32 rd, s32 sim) { ADDI(rd, 0, sim); }
  // Load 32-bit zero-extended value into rd using ORIS/ORI (no sign extension).
  // Always 2 instructions, works for all 32-bit values even when ADDI/LIS would
  // sign-extend (e.g. 0xFF00 becomes 0xFFFFFFFFFF00 with ADDI on PPC64).
  void LI32(u32 rd, u32 value)
  {
    ADDIS(rd, 0, static_cast<s32>(value >> 16));
    ORI(rd, rd, value & 0xFFFF);
  }
  void SUBF(u32 rd, u32 ra, u32 rb, bool rc = false) { Write32(X(31, rd, ra, rb, 40, rc)); }
  void SUBFC(u32 rd, u32 ra, u32 rb, bool rc = false) { Write32(X(31, rd, ra, rb, 8, rc)); }
  void SUBFE(u32 rd, u32 ra, u32 rb, bool rc = false) { Write32(X(31, rd, ra, rb, 136, rc)); }
  void SUBFZE(u32 rd, u32 ra, bool rc = false) { Write32(X(31, rd, 0, ra, 200, rc)); }
  void ADDZE(u32 rd, u32 ra, bool rc = false) { Write32(X(31, rd, 0, ra, 202, rc)); }
  void SUBFME(u32 rd, u32 ra, bool rc = false) { Write32(X(31, rd, 0, ra, 232, rc)); }
  void ADDME(u32 rd, u32 ra, bool rc = false) { Write32(X(31, rd, 0, ra, 234, rc)); }
  void SUBFIC(u32 rd, u32 ra, s32 sim) { Write32(D(8, rd, ra, sim)); }
  void NEG(u32 rd, u32 ra, bool rc = false) { Write32(X(31, rd, 0, ra, 104, rc)); }
  void MULLI(u32 rd, u32 ra, s32 sim) { Write32(D(7, rd, ra, sim)); }
  void MULLW(u32 rd, u32 ra, u32 rb, bool rc = false) { Write32(X(31, rd, ra, rb, 235, rc)); }
  void MULHW(u32 rd, u32 ra, u32 rb, bool rc = false) { Write32(X(31, rd, ra, rb, 75, rc)); }
  void MULHWU(u32 rd, u32 ra, u32 rb, bool rc = false) { Write32(X(31, rd, ra, rb, 11, rc)); }
  void MULHDU(u32 rd, u32 ra, u32 rb) { Write32(X(31, rd, ra, rb, 9, false)); }
  void DIVW(u32 rd, u32 ra, u32 rb, bool rc = false) { Write32(X(31, rd, ra, rb, 491, rc)); }
  void DIVWU(u32 rd, u32 ra, u32 rb, bool rc = false) { Write32(X(31, rd, ra, rb, 459, rc)); }

  // -- Logical (opcd=31) --
  void AND(u32 rd, u32 ra, u32 rb, bool rc = false) { Write32(X(31, rd, ra, rb, 28, rc)); }
  void ANDC(u32 rd, u32 ra, u32 rb, bool rc = false) { Write32(X(31, rd, ra, rb, 60, rc)); }
  void ANDI_(u32 rd, u32 ra, u32 ui) { Write32(D(28, rd, ra, ui)); }
  void ANDIS_(u32 rd, u32 ra, u32 ui) { Write32(D(29, rd, ra, ui)); }
  void OR(u32 rd, u32 ra, u32 rb, bool rc = false) { Write32(X(31, rd, ra, rb, 444, rc)); }
  void ORC(u32 rd, u32 ra, u32 rb, bool rc = false) { Write32(X(31, rd, ra, rb, 412, rc)); }
  void ORI(u32 rd, u32 ra, u32 ui) { Write32(D(24, rd, ra, ui)); }
  void ORIS(u32 rd, u32 ra, u32 ui) { Write32(D(25, rd, ra, ui)); }
  void XOR(u32 rd, u32 ra, u32 rb, bool rc = false) { Write32(X(31, rd, ra, rb, 316, rc)); }
  void XORI(u32 rd, u32 ra, u32 ui) { Write32(D(26, rd, ra, ui)); }
  void XORIS(u32 rd, u32 ra, u32 ui) { Write32(D(27, rd, ra, ui)); }
  void NAND(u32 rd, u32 ra, u32 rb, bool rc = false) { Write32(X(31, rd, ra, rb, 476, rc)); }
  void NOR(u32 rd, u32 ra, u32 rb, bool rc = false) { Write32(X(31, rd, ra, rb, 124, rc)); }
  void EQV(u32 rd, u32 ra, u32 rb, bool rc = false) { Write32(X(31, rd, ra, rb, 284, rc)); }
  void EXTSB(u32 rd, u32 ra, bool rc = false) { Write32(X(31, rd, 0, ra, 954, rc)); }
  void EXTSH(u32 rd, u32 ra, bool rc = false) { Write32(X(31, rd, 0, ra, 922, rc)); }
  void EXTSW(u32 rd, u32 ra, bool rc = false) { Write32(X(31, rd, 0, ra, 986, rc)); }
  void CNTLZW(u32 rd, u32 ra, bool rc = false) { Write32(X(31, rd, 0, ra, 26, rc)); }

  // -- Shift / Rotate (M-form) --
  void SLW(u32 rd, u32 ra, u32 rb, bool rc = false) { Write32(X(31, rd, ra, rb, 24, rc)); }
  void SRW(u32 rd, u32 ra, u32 rb, bool rc = false) { Write32(X(31, rd, ra, rb, 536, rc)); }
  void SRAW(u32 rd, u32 ra, u32 rb, bool rc = false) { Write32(X(31, rd, ra, rb, 792, rc)); }
  void SRAWI(u32 rd, u32 ra, u32 sh, bool rc = false) { Write32(X(31, rd, ra, sh, 824, rc)); }

  // rlwinm RA, RS, SH, MB, ME [, RC]
  void RLWINM(u32 ra, u32 rs, u32 sh, u32 mb, u32 me, bool rc = false)
  {
    Write32((21u << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1) | (rc ? 1 : 0));
  }
  void RLWNM(u32 ra, u32 rs, u32 rb, u32 mb, u32 me, bool rc = false)
  {
    Write32((23u << 26) | (rs << 21) | (ra << 16) | (rb << 11) | (mb << 6) | (me << 1) | (rc ? 1 : 0));
  }
  void RLWIMI(u32 ra, u32 rs, u32 sh, u32 mb, u32 me, bool rc = false)
  {
    Write32((20u << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1) | (rc ? 1 : 0));
  }

  // -- Compare (opcd=31 X-form with crfD at bits 25:23, L at bit 22) --
  // PPC CMP X-form: RA at arch bits 10-14 = u32 bits 21:17, RB at arch bits 15-19 = u32 bits 16:12
  // (different from standard X-form because crfD+L take 4 bits instead of rt's 5 bits)
  void CMPW(u32 crf, u32 ra, u32 rb) { Write32((31u << 26) | (crf << 23) | (0u << 22) | (ra << 17) | (rb << 12) | (0 << 1)); }
  void CMPLW(u32 crf, u32 ra, u32 rb) { Write32((31u << 26) | (crf << 23) | (0u << 22) | (ra << 17) | (rb << 12) | (32 << 1)); }
  void CMPD(u32 crf, u32 ra, u32 rb) { Write32((31u << 26) | (crf << 23) | (1u << 22) | (ra << 17) | (rb << 12) | (0 << 1)); }
  void CMPLD(u32 crf, u32 ra, u32 rb) { Write32((31u << 26) | (crf << 23) | (1u << 22) | (ra << 17) | (rb << 12) | (32 << 1)); }
  // CMPI/CMPLI (opcd 10/11): RA at arch bits 10-14 = u32 bits 21:17, SIMM at u32 15:0
  // crfD+L takes 4 bits (arch 6-9), shifting RA 1 bit up vs standard D-form
  void CMPWI(u32 crf, u32 ra, s32 sim) { Write32((11u << 26) | (crf << 23) | (0u << 22) | (ra << 17) | (sim & 0xFFFF)); }
  void CMPLWI(u32 crf, u32 ra, u32 ui) { Write32((10u << 26) | (crf << 23) | (0u << 22) | (ra << 17) | (ui & 0xFFFF)); }
  // CMPDI/CMPLDI (L=1 for 64-bit doubleword compare): same RA/SIMM layout with L=1
  void CMPDI(u32 crf, u32 ra, s32 sim) { Write32((11u << 26) | (crf << 23) | (1u << 22) | (ra << 17) | (sim & 0xFFFF)); }
  void CMPLDI(u32 crf, u32 ra, u32 ui) { Write32((10u << 26) | (crf << 23) | (1u << 22) | (ra << 17) | (ui & 0xFFFF)); }

  // -- Load/Store (D-form) --
  void LWZ(u32 rt, u32 ra, s32 d) { Write32(D(32, rt, ra, d)); }
  void LWZU(u32 rt, u32 ra, s32 d) { Write32(D(33, rt, ra, d)); }
  void LHA(u32 rt, u32 ra, s32 d) { Write32(D(42, rt, ra, d)); }
  void LHAU(u32 rt, u32 ra, s32 d) { Write32(D(43, rt, ra, d)); }
  void LHZ(u32 rt, u32 ra, s32 d) { Write32(D(40, rt, ra, d)); }
  void LHZU(u32 rt, u32 ra, s32 d) { Write32(D(41, rt, ra, d)); }
  void LBZ(u32 rt, u32 ra, s32 d) { Write32(D(34, rt, ra, d)); }
  void LBZU(u32 rt, u32 ra, s32 d) { Write32(D(35, rt, ra, d)); }
  void STW(u32 rs, u32 ra, s32 d) { Write32(D(36, rs, ra, d)); }
  void STWU(u32 rs, u32 ra, s32 d) { Write32(D(37, rs, ra, d)); }
  void STH(u32 rs, u32 ra, s32 d) { Write32(D(44, rs, ra, d)); }
  void STHU(u32 rs, u32 ra, s32 d) { Write32(D(45, rs, ra, d)); }
  void STB(u32 rs, u32 ra, s32 d) { Write32(D(38, rs, ra, d)); }
  void STBU(u32 rs, u32 ra, s32 d) { Write32(D(39, rs, ra, d)); }
  void LMW(u32 rt, u32 ra, s32 d) { Write32(D(46, rt, ra, d)); }
  void STMW(u32 rs, u32 ra, s32 d) { Write32(D(47, rs, ra, d)); }

  // Load/Store indexed (X-form, opcd=31)
  void LWZX(u32 rt, u32 ra, u32 rb) { Write32(X(31, rt, ra, rb, 23, false)); }
  void LWZUX(u32 rt, u32 ra, u32 rb) { Write32(X(31, rt, ra, rb, 55, false)); }
  void LHZX(u32 rt, u32 ra, u32 rb) { Write32(X(31, rt, ra, rb, 279, false)); }
  void LHZUX(u32 rt, u32 ra, u32 rb) { Write32(X(31, rt, ra, rb, 311, false)); }
  void LHAX(u32 rt, u32 ra, u32 rb) { Write32(X(31, rt, ra, rb, 343, false)); }
  void LHAUX(u32 rt, u32 ra, u32 rb) { Write32(X(31, rt, ra, rb, 375, false)); }
  void LBZX(u32 rt, u32 ra, u32 rb) { Write32(X(31, rt, ra, rb, 87, false)); }
  void LBZUX(u32 rt, u32 ra, u32 rb) { Write32(X(31, rt, ra, rb, 119, false)); }
  void STWX(u32 rs, u32 ra, u32 rb) { Write32(X(31, rs, ra, rb, 151, false)); }
  void STWUX(u32 rs, u32 ra, u32 rb) { Write32(X(31, rs, ra, rb, 183, false)); }
  void STHX(u32 rs, u32 ra, u32 rb) { Write32(X(31, rs, ra, rb, 407, false)); }
  void STHUX(u32 rs, u32 ra, u32 rb) { Write32(X(31, rs, ra, rb, 439, false)); }
  void STBX(u32 rs, u32 ra, u32 rb) { Write32(X(31, rs, ra, rb, 215, false)); }
  void STBUX(u32 rs, u32 ra, u32 rb) { Write32(X(31, rs, ra, rb, 247, false)); }

  void NOP() { Write32(0x60000000); }
  // blr = bclr 20, 0: BO=20 (always branch), BI=0 (unused), xo=16, LK=0
  void BLR() { Write32((19u << 26) | (20u << 21) | (16u << 1)); }
  void MFLR(u32 rd) { MFSPR(rd, 8); }
  void MTLR(u32 rs) { MTSPR(8, rs); }

  // -- Branch (I-form / B-form) --
  // I-form: |18|LI(24b)|AA|LK| — LI at PPC 6-29=u32 25-2, AA/LK at PPC 30-31=u32 1-0
  // B — relative branch (AA=0) with 24-bit displacement
  // target_u32 is the absolute address; we encode it as AA=1 (within 256MB).
  void B(u32 target, bool aa = false, bool lk = false)
  {
    Write32((18u << 26) | (((target >> 2) & 0x00FFFFFF) << 2) | (aa ? 1u << 1 : 0u) | (lk ? 1u : 0u));
  }
  void BL(u32 target, bool aa = false) { B(target, aa, true); }
  void BA(u32 target) { B(target, true, false); }

  // Branch relative to a pointer (within ±32MB from current position)
  // LI is at PPC bits 6-29 = u32 bits [25:2]; we compute d in bytes, shift
  // right by 2 to get word offset, and shift left by 2 to place at [25:2].
  void BRel(const u8* target)
  {
    ptrdiff_t d = target - (m_code + m_pos);
    u32 li = (static_cast<u32>(d >> 2)) & 0x00FFFFFF;
    u32 instr = (18u << 26) | (li << 2);
    fprintf(stderr, "JITPROBE: BRel m_code=%p m_pos=%zu target=%p d=%ld li=0x%06X instr=0x%08X\n",
            (void*)m_code, m_pos, (void*)target, (long)d, li, instr);
    Write32(instr);
  }
  void BLRel(const u8* target)
  {
    ptrdiff_t d = target - (m_code + m_pos);
    u32 li = (static_cast<u32>(d >> 2)) & 0x00FFFFFF;
    Write32((18u << 26) | (li << 2) | 1u);
  }

  // bc BO, BI, BD [, AA, LK]
  // B-form: |16|BO|BI|BD(14b)|AA|LK| — BD at PPC 16-29=u32 15-2, AA/LK at PPC 30-31=u32 1-0
  void BC(u32 bo, u32 bi, s32 bd, bool aa = false, bool lk = false)
  {
    Write32((16u << 26) | ((bo & 0x1F) << 21) | ((bi & 0x1F) << 16) | (((bd >> 2) & 0x3FFF) << 2) |
            (aa ? 1u << 1 : 0u) | (lk ? 1u : 0u));
  }

  // bclr BO, BI [, LK]
  void BCLR(u32 bo, u32 bi, bool lk = false)
  {
    Write32((19u << 26) | ((bo & 0x1F) << 21) | ((bi & 0x1F) << 16) | (16u << 1) | (lk ? 1 : 0));
  }

  // bcctr BO, BI [, LK]
  void BCCTR(u32 bo, u32 bi, bool lk = false)
  {
    Write32((19u << 26) | ((bo & 0x1F) << 21) | ((bi & 0x1F) << 16) | (528u << 1) | (lk ? 1 : 0));
  }

  // -- Condition register (crfD-variant, opcd=19) --
  // MCRF is XL-form: BT=crfD||00, BA=crfS||00, BB=00000, xo=0, LK=0
  // BT at PPC 6-10 = u32 25-21: crfD<<23 fills bits 25-23, bits 22-21=0
  // BA at PPC 11-15 = u32 20-16: crfS<<18 fills bits 20-18, bits 17-16=0
  void MCRF(u32 crfd, u32 crfs)
  {
    Write32((19u << 26) | (crfd << 23) | (crfs << 18) | (0u << 1));
  }
  void CRAND(u32 bt, u32 ba, u32 bb) { Write32(XL(19, bt, ba, bb, 257)); }
  void CRNAND(u32 bt, u32 ba, u32 bb) { Write32(XL(19, bt, ba, bb, 225)); }
  void CROR(u32 bt, u32 ba, u32 bb) { Write32(XL(19, bt, ba, bb, 449)); }
  void CRNOR(u32 bt, u32 ba, u32 bb) { Write32(XL(19, bt, ba, bb, 33)); }
  void CRXOR(u32 bt, u32 ba, u32 bb) { Write32(XL(19, bt, ba, bb, 193)); }
  void CREQV(u32 bt, u32 ba, u32 bb) { Write32(XL(19, bt, ba, bb, 289)); }
  void CRANDC(u32 bt, u32 ba, u32 bb) { Write32(XL(19, bt, ba, bb, 129)); }
  void CRORC(u32 bt, u32 ba, u32 bb) { Write32(XL(19, bt, ba, bb, 417)); }

  // mfcr
  void MFCR(u32 rd) { Write32((31u << 26) | (rd << 21) | (19u << 1)); }
  // mtcrf mask, rs
  void MTCRF(u32 mask, u32 rs) { Write32((31u << 26) | (rs << 21) | (mask << 12) | (144u << 1)); }
  // mcrxr crf
  void MCRXR(u32 crf) { Write32((31u << 26) | (crf << 23) | (512u << 1)); }

  // -- System (opcd=31) --
  // mfspr rd, spr
  void MFSPR(u32 rd, u32 spr)
  {
    Write32((31u << 26) | (rd << 21) | ((spr & 0x1F) << 16) | (((spr >> 5) & 0x1F) << 11) | (339u << 1));
  }
  // mtspr spr, rs
  void MTSPR(u32 spr, u32 rs)
  {
    Write32((31u << 26) | (rs << 21) | ((spr & 0x1F) << 16) | (((spr >> 5) & 0x1F) << 11) | (467u << 1));
  }
  // mfmsr
  void MFMSR(u32 rd) { Write32((31u << 26) | (rd << 21) | (83u << 1)); }
  // mtmsr rs
  void MTMSR(u32 rs) { Write32((31u << 26) | (rs << 21) | (146u << 1)); }
  // sc
  void SC() { Write32((17u << 26) | (0x1u << 21) | (0x1u << 1)); }
  // rfi
  void RFI() { Write32((19u << 26) | (50u << 1)); }

  // -- Trap (opcd=31) --
  void TRAP() { Write32((31u << 26) | (0x1Fu << 21) | (0x1Fu << 16) | (0x1Fu << 11) | (4u << 1)); }

  // Byte-reversed load/store (opcd=31)
  void LWBRX(u32 rt, u32 ra, u32 rb) { Write32(X(31, rt, ra, rb, 534, false)); }
  void STWBRX(u32 rs, u32 ra, u32 rb) { Write32(X(31, rs, ra, rb, 662, false)); }
  void LHBRX(u32 rt, u32 ra, u32 rb) { Write32(X(31, rt, ra, rb, 790, false)); }
  void STHBRX(u32 rs, u32 ra, u32 rb) { Write32(X(31, rs, ra, rb, 918, false)); }
  // stfiwx — store FPR as integer word
  void STFIWX(u32 frs, u32 ra, u32 rb) { Write32(X(31, frs, ra, rb, 983, false)); }
  // lfiwax — load integer word as floating-point (s32 → double)
  void LFIWAX(u32 frD, u32 ra, u32 rb) { Write32(X(31, frD, ra, rb, 887, false)); }

  // -- Cache / Misc (opcd=31) --
  void DCBF(u32 ra, u32 rb) { Write32(X(31, 0, ra, rb, 86, false)); }
  void DCBI(u32 ra, u32 rb) { Write32(X(31, 0, ra, rb, 470, false)); }
  void DCBST(u32 ra, u32 rb) { Write32(X(31, 0, ra, rb, 54, false)); }
  void DCBT(u32 ra, u32 rb) { Write32(X(31, 0, ra, rb, 278, false)); }
  void DCBTST(u32 ra, u32 rb) { Write32(X(31, 0, ra, rb, 246, false)); }
  void DCBZ(u32 ra, u32 rb) { Write32(X(31, 0, ra, rb, 1014, false)); }
  void ICBI(u32 ra, u32 rb) { Write32(X(31, 0, ra, rb, 982, false)); }
  void SYNC() { Write32((31u << 26) | (598u << 1)); }
  void EIEIO() { Write32((31u << 26) | (854u << 1)); }
  void ISYNC() { Write32((19u << 26) | (150u << 1)); }

  // PPC64-specific: 64-bit ops (for host-side code)
  void ADDI64(u32 rd, u32 ra, s32 sim) { Write32(D(14, rd, ra, sim)); }
  void ADDIS64(u32 rd, u32 ra, s32 sim) { Write32(D(15, rd, ra, sim)); }
  void ORIS64(u32 rd, u32 ra, u32 ui) { Write32(D(25, rd, ra, ui)); }

  // Load/store doubleword
  void LD(u32 rt, u32 ra, s32 ds) { Write32(DS(58, rt, ra, ds, 0)); }
  void STD(u32 rs, u32 ra, s32 ds) { Write32(DS(62, rs, ra, ds, 0)); }
  void LDU(u32 rt, u32 ra, s32 ds) { Write32(DS(58, rt, ra, ds, 1)); }
  void STDU(u32 rs, u32 ra, s32 ds) { Write32(DS(62, rs, ra, ds, 1)); }
  void LDX(u32 rt, u32 ra, u32 rb) { Write32(X(31, rt, ra, rb, 21, false)); }
  void LDUX(u32 rt, u32 ra, u32 rb) { Write32(X(31, rt, ra, rb, 53, false)); }
  void STDX(u32 rs, u32 ra, u32 rb) { Write32(X(31, rs, ra, rb, 149, false)); }
  void STDUX(u32 rs, u32 ra, u32 rb) { Write32(X(31, rs, ra, rb, 181, false)); }

  // rldicr RA, RS, SH, ME — rotate left double immediate and clear right
  // MD-form: opcd=30, xo=1 at PPC bits 27-30 (= u32 bits 4:1 = 0b0001)
  //   u31-u26: opcd=30, u25-21: RS, u20-16: RA,
  //   u15-u11: sh[4:0], u10: sh[5],
  //   u9-u5: me[4:0], u4: me[5],
  //   u4-u1: xo (4-bit, 0001=rldicr, 0000=rldicl), u0: Rc
  void RLDICR(u32 ra, u32 rs, u32 sh, u32 me, bool rc = false)
  {
    Write32((30u << 26) | ((rs & 0x1F) << 21) | ((ra & 0x1F) << 16) |
            ((sh & 0x1F) << 11) | (((sh >> 5) & 1) << 10) |
            ((me & 0x1F) << 5) | (((me >> 5) & 1) << 4) |
             (1u << 1) | (rc ? 1u : 0u));
  }

  // rldicl RA, RS, SH, MB — clear left bits (e.g., rldicl rd, rs, 0, 32 = zero-extend 32-bit)
  // MD-form: opcd=30, xo=0 at PPC bits 27-30 (= u32 bits 4:1 = 0b0000)
  void RLDICL(u32 ra, u32 rs, u32 sh, u32 mb, bool rc = false)
  {
    Write32((30u << 26) | ((rs & 0x1F) << 21) | ((ra & 0x1F) << 16) |
            ((sh & 0x1F) << 11) | (((sh >> 5) & 1) << 10) |
            ((mb & 0x1F) << 5) | (((mb >> 5) & 1) << 4) |
            (0u << 2) | (rc ? 1u : 0u));
  }

  // Zero-extend 32-bit: rldicl rd, rs, 0, 32  (clear upper 32 bits)
  void CLRLDI(u32 ra, u32 rs, u32 mb) { RLDICL(ra, rs, 0, mb); }
  void CLR32(u32 rd, u32 rs) { RLDICL(rd, rs, 0, 32); }

  // -- Branch / SPR moves --
  // mtspr 9, rs  — move to CTR (SPR=9)
  void MTCTR(u32 rs)
  {
    Write32((31u << 26) | ((rs & 0x1F) << 21) | (9u << 16) | (467u << 1));
  }
  // mfspr rd, 9 — move from CTR (SPR=9)
  void MFCTR(u32 rd)
  {
    Write32((31u << 26) | ((rd & 0x1F) << 21) | (9u << 16) | (339u << 1));
  }
  // bctrl / bctr: bcctr with BO=20 (always branch), BI=0, xo=528, LK=1/0
  void BCTRL() { Write32((19u << 26) | (20u << 21) | (528u << 1) | 1u); }
  void BCTR()  { Write32((19u << 26) | (20u << 21) | (528u << 1)); }

  // -- Move Register (OR rS, rS, rT → rA = rS) --
  void MR(u32 ra, u32 rs) { OR(ra, rs, rs); }

  // Load 64-bit immediate into rd (6 instructions, no sign-extension issues).
  // Uses existing assembler methods: lis + clrldi + ori + sldi + oris + ori
  void MOVI64(u32 rd, u64 imm)
  {
    const auto hi = static_cast<s32>((imm >> 48) & 0xFFFF);
    const auto h3 = static_cast<u32>((imm >> 32) & 0xFFFF);
    const auto h2 = static_cast<u32>((imm >> 16) & 0xFFFF);
    const auto lo = static_cast<u32>(imm & 0xFFFF);
    ADDIS(rd, 0, hi);
    RLDICL(rd, rd, 0, 32);
    ORI(rd, rd, h3);
    RLDICR(rd, rd, 32, 31);
    ORIS(rd, rd, h2);
    ORI(rd, rd, lo);
  }
  void VADDFP(u32 vd, u32 va, u32 vb) { Write32(AV_VA(4, vd, va, vb, 26)); }
  void VSUBFP(u32 vd, u32 va, u32 vb) { Write32(AV_VA(4, vd, va, vb, 28)); }
  void VMULFP(u32 vd, u32 va, u32 vb) { Write32(AV_VA(4, vd, va, vb, 52)); }
  void VDIVFP(u32 vd, u32 va, u32 vb) { Write32(AV_VA(4, vd, va, vb, 27)); }
  void VREFP(u32 vd, u32 vb) { Write32(AV_VA(4, vd, 0, vb, 1291)); }
  void VRSQRTEFP(u32 vd, u32 vb) { Write32(AV_VA(4, vd, 0, vb, 1299)); }
  void VAND(u32 vd, u32 va, u32 vb) { Write32(AV_VA(4, vd, va, vb, 1028)); }
  void VANDC(u32 vd, u32 va, u32 vb) { Write32(AV_VA(4, vd, va, vb, 1092)); }
  void VOR(u32 vd, u32 va, u32 vb) { Write32(AV_VA(4, vd, va, vb, 1156)); }
  void VXOR(u32 vd, u32 va, u32 vb) { Write32(AV_VA(4, vd, va, vb, 1220)); }
  void VNOR(u32 vd, u32 va, u32 vb) { Write32(AV_VA(4, vd, va, vb, 1284)); }
  // AltiVec 4-register VA form (vD, vA, vB, vC, 5-bit xo at PPC bits 26-30)
  // xo10 is the full 10-bit extended opcode; only lower 5 bits are used at bits 26-30.
  // VMADDFP: xo (5-bit) = 30,  10-bit extended = (vC<<5) | 30
  // VMSUBFP: xo (5-bit) = 31
  // VNMADDFP: xo (5-bit) = 23
  // VNMSUBFP: xo (5-bit) = 15
  // VSEL:     xo (5-bit) = 10
  void VMADDFP(u32 vd, u32 va, u32 vb, u32 vc) { Write32(AV_VA4(4, vd, va, vb, vc, 30)); }
  void VMSUBFP(u32 vd, u32 va, u32 vb, u32 vc) { Write32(AV_VA4(4, vd, va, vb, vc, 31)); }
  void VNMADDFP(u32 vd, u32 va, u32 vb, u32 vc) { Write32(AV_VA4(4, vd, va, vb, vc, 23)); }
  void VNMSUBFP(u32 vd, u32 va, u32 vb, u32 vc) { Write32(AV_VA4(4, vd, va, vb, vc, 15)); }
  void VSEL(u32 vd, u32 va, u32 vb, u32 vc) { Write32(AV_VA4(4, vd, va, vb, vc, 10)); }
  void VMRGHW(u32 vd, u32 va, u32 vb) { Write32(AV_VA(4, vd, va, vb, 1032)); }
  void VMRGLW(u32 vd, u32 va, u32 vb) { Write32(AV_VA(4, vd, va, vb, 1096)); }
  void VSPLTW(u32 vd, u32 vs, u32 ui) { Write32(AV_VA(4, vd, vs, ui, 396)); }
  void VSPLTB(u32 vd, u32 vs, u32 ui) { Write32(AV_VA(4, vd, vs, ui, 524)); }
  void VSPLTH(u32 vd, u32 vs, u32 ui) { Write32(AV_VA(4, vd, vs, ui, 588)); }
  void VCMPEQFP(u32 vd, u32 va, u32 vb) { Write32(AV_VA(4, vd, va, vb, 198)); }
  void VCMPGTFP(u32 vd, u32 va, u32 vb) { Write32(AV_VA(4, vd, va, vb, 710)); }
  void VCMPGEFP(u32 vd, u32 va, u32 vb) { Write32(AV_VA(4, vd, va, vb, 454)); }

  // AltiVec load/store
  void LVX(u32 vd, u32 ra, u32 rb) { Write32(AV_X(4, vd, ra, rb, 103)); }
  void STVX(u32 vs, u32 ra, u32 rb) { Write32(AV_X(4, vs, ra, rb, 231)); }
  void LVSR(u32 vd, u32 ra, u32 rb) { Write32(AV_X(4, vd, ra, rb, 39)); }
  void LVSL(u32 vd, u32 ra, u32 rb) { Write32(AV_X(4, vd, ra, rb, 7)); }

  // AltiVec FPR ↔ VR move via memory
  // stfd frS, D(ra)  — store FPR (64-bit double) to memory
  void STFD(u32 frs, u32 ra, s32 d) { Write32(D(54, frs, ra, d)); }
  // lfd frD, D(ra)   — load FPR (64-bit double) from memory
  void LFD(u32 frd, u32 ra, s32 d) { Write32(D(50, frd, ra, d)); }
  // stfs frS, D(ra)  — store FPR (32-bit float) to memory
  void STFS(u32 frs, u32 ra, s32 d) { Write32(D(52, frs, ra, d)); }
  // lfs frD, D(ra)   — load FPR (32-bit float) from memory
  void LFS(u32 frd, u32 ra, s32 d) { Write32(D(48, frd, ra, d)); }

  // FPU indexed loads/stores (X-form, opcd=31)
  void LFSX(u32 frd, u32 ra, u32 rb) { Write32(X(31, frd, ra, rb, 535, false)); }
  void LFDX(u32 frd, u32 ra, u32 rb) { Write32(X(31, frd, ra, rb, 599, false)); }
  void STFSX(u32 frs, u32 ra, u32 rb) { Write32(X(31, frs, ra, rb, 663, false)); }
  void STFDX(u32 frs, u32 ra, u32 rb) { Write32(X(31, frs, ra, rb, 727, false)); }

  // FPU arithmetic — A-form (opcd 59 = single, 63 = double)
  // Helper: A-form = opcd | frD << 21 | frA << 16 | frB << 11 | frC << 6 | xo5 << 1 | Rc
  // For 3-operand: frC = 0
  void FADDS(u32 frd, u32 fra, u32 frb) { Write32(A(59, frd, fra, frb, 0, 21, false)); }
  void FSUBS(u32 frd, u32 fra, u32 frb) { Write32(A(59, frd, fra, frb, 0, 20, false)); }
  void FDIVS(u32 frd, u32 fra, u32 frb) { Write32(A(59, frd, fra, frb, 0, 18, false)); }
  void FRES(u32 frd, u32 frb)           { Write32(A(59, frd, 0, frb, 0, 24, false)); }
  void FMULS(u32 frd, u32 fra, u32 frc) { Write32(A(59, frd, fra, 0, frc, 25, false)); }
  void FMADDS(u32 frd, u32 fra, u32 frb, u32 frc) { Write32(A(59, frd, fra, frb, frc, 29, false)); }
  void FMSUBS(u32 frd, u32 fra, u32 frb, u32 frc) { Write32(A(59, frd, fra, frb, frc, 28, false)); }
  void FNMADDS(u32 frd, u32 fra, u32 frb, u32 frc) { Write32(A(59, frd, fra, frb, frc, 31, false)); }
  void FNMSUBS(u32 frd, u32 fra, u32 frb, u32 frc) { Write32(A(59, frd, fra, frb, frc, 30, false)); }

  void FADD(u32 frd, u32 fra, u32 frb)  { Write32(A(63, frd, fra, frb, 0, 21, false)); }
  void FSUB(u32 frd, u32 fra, u32 frb)  { Write32(A(63, frd, fra, frb, 0, 20, false)); }
  void FMUL(u32 frd, u32 fra, u32 frc)  { Write32(A(63, frd, fra, 0, frc, 25, false)); }
  void FDIV(u32 frd, u32 fra, u32 frb)  { Write32(A(63, frd, fra, frb, 0, 18, false)); }
  void FRSP(u32 frd, u32 frb)           { Write32(A(63, frd, 0, frb, 0, 12, false)); }
  void FSEL(u32 frd, u32 fra, u32 frb, u32 frc) { Write32(A(63, frd, fra, frb, frc, 23, false)); }
  void FRSQRTE(u32 frd, u32 frb)        { Write32(A(63, frd, 0, frb, 0, 26, false)); }
  void FMADD(u32 frd, u32 fra, u32 frb, u32 frc) { Write32(A(63, frd, fra, frb, frc, 29, false)); }
  void FMSUB(u32 frd, u32 fra, u32 frb, u32 frc) { Write32(A(63, frd, fra, frb, frc, 28, false)); }
  void FNMADD(u32 frd, u32 fra, u32 frb, u32 frc) { Write32(A(63, frd, fra, frb, frc, 31, false)); }
  void FNMSUB(u32 frd, u32 fra, u32 frb, u32 frc) { Write32(A(63, frd, fra, frb, frc, 30, false)); }

  // FPU X-form compare (opcd=63): frA at arch bits 10-14 = u32 21:17, frB at arch 15-19 = u32 16:12
  // (same off-by-1 shift as integer compares: crfD+L take 4 bits vs rt's 5 bits)
  void FCMPU(u32 crf, u32 fra, u32 frb)
  {
    Write32((63u << 26) | ((crf & 0x7) << 23) | ((fra & 0x1F) << 17) | ((frb & 0x1F) << 12) | (0u << 1));
  }
  void FCMPO(u32 crf, u32 fra, u32 frb)
  {
    Write32((63u << 26) | ((crf & 0x7) << 23) | ((fra & 0x1F) << 17) | ((frb & 0x1F) << 12) | (32u << 1));
  }
  void FMR(u32 frd, u32 frb)   { Write32(X(63, frd, 0, frb, 72, false)); }
  void FNEG(u32 frd, u32 frb)  { Write32(X(63, frd, 0, frb, 40, false)); }
  void FABS(u32 frd, u32 frb)  { Write32(X(63, frd, 0, frb, 264, false)); }
  void FNABS(u32 frd, u32 frb) { Write32(X(63, frd, 0, frb, 136, false)); }
  void FCTIW(u32 frd, u32 frb)  { Write32(X(63, frd, 0, frb, 14, false)); }
  void FCTIWZ(u32 frd, u32 frb) { Write32(X(63, frd, 0, frb, 15, false)); }
  void FCFID(u32 frd, u32 frb)  { Write32(X(63, frd, 0, frb, 846, false)); }
  void FCTIDZ(u32 frd, u32 frb) { Write32(X(63, frd, 0, frb, 815, false)); }

  // FPSCR access (X-form, opcd=63)
  void MFFS(u32 frd)           { Write32(X(63, frd, 0, 0, 583, false)); }
  void MTFSF(u32 mask, u32 frb)
  {
    Write32((63u << 26) | ((frb & 0x1F) << 21) | ((mask & 0xFF) << 17) | (711u << 1));
  }
  void MTFSFI(u32 crfd, u32 imm)
  {
    Write32((63u << 26) | ((crfd & 0x7) << 23) | ((imm & 0xF) << 17) | (134u << 1));
  }
  void MTFSB0(u32 bit) { Write32(X(63, 0, 0, bit, 70, false)); }
  void MTFSB1(u32 bit) { Write32(X(63, 0, 0, bit, 38, false)); }
  void MCRFS(u32 crfd, u32 crfs)
  {
    Write32((63u << 26) | ((crfd & 0x7) << 23) | ((crfs & 0x7) << 18) | (64u << 1));
  }

private:
  static u32 D(u32 opcd, u32 rt, u32 ra, s32 d)
  {
    return (opcd << 26) | ((rt & 0x1F) << 21) | ((ra & 0x1F) << 16) | (static_cast<u32>(d) & 0xFFFF);
  }
  // DS-form: |opcd|RT|RA|ds[13:0]|xo| — ds at PPC 16-29 = u32 15-2
  static u32 DS(u32 opcd, u32 rt, u32 ra, s32 ds, u32 xo)
  {
    u32 ds_enc = (static_cast<u32>(ds >> 2)) & 0x3FFF;
    return (opcd << 26) | ((rt & 0x1F) << 21) | ((ra & 0x1F) << 16) |
           (ds_enc << 2) | (xo & 0x3);
  }
  static u32 X(u32 opcd, u32 rt, u32 ra, u32 rb, u32 xo, bool rc)
  {
    return (opcd << 26) | ((rt & 0x1F) << 21) | ((ra & 0x1F) << 16) |
           ((rb & 0x1F) << 11) | (xo << 1) | (rc ? 1u : 0u);
  }
  // XL-form: |opcd|BT|BA|BB|xo[9:0]|LK| — xo at PPC 21-30 = u32 10-1
  static u32 XL(u32 opcd, u32 bt, u32 ba, u32 bb, u32 xo)
  {
    return (opcd << 26) | ((bt & 0x1F) << 21) | ((ba & 0x1F) << 16) |
           ((bb & 0x1F) << 11) | (xo << 1);
  }
  // VA form: 3 registers + 10-bit xo at PPC bits 21-30 (u32 bits 1-10)
  // Shifted left 1 to leave u32 bit 0 = Rc = 0
  static u32 AV_VA(u32 opcd, u32 vd, u32 va, u32 vb, u32 xo)
  {
    return (opcd << 26) | ((vd & 0x1F) << 21) | ((va & 0x1F) << 16) |
           ((vb & 0x1F) << 11) | ((xo & 0x3FF) << 1);
  }
  // VA4 form: 4 registers + 5-bit xo at bits 26-30 (lower 5 of the 10-bit xo value)
  static u32 AV_VA4(u32 opcd, u32 vd, u32 va, u32 vb, u32 vc, u32 xo10)
  {
    return (opcd << 26) | ((vd & 0x1F) << 21) | ((va & 0x1F) << 16) |
           ((vb & 0x1F) << 11) | ((vc & 0x1F) << 6) | ((xo10 & 0x1F) << 1);
  }
  // X form for AltiVec: 3 registers + 10-bit xo at PPC bits 21-30
  static u32 AV_X(u32 opcd, u32 vd, u32 ra, u32 rb, u32 xo)
  {
    return (opcd << 26) | ((vd & 0x1F) << 21) | ((ra & 0x1F) << 16) |
           ((rb & 0x1F) << 11) | ((xo & 0x3FF) << 1);
  }

  // A form for FPU: opcd | frD << 21 | frA << 16 | frB << 11 | frC << 6 | xo5 << 1 | Rc
  static u32 A(u32 opcd, u32 frd, u32 fra, u32 frb, u32 frc, u32 xo5, bool rc)
  {
    return (opcd << 26) | ((frd & 0x1F) << 21) | ((fra & 0x1F) << 16) |
           ((frb & 0x1F) << 11) | ((frc & 0x1F) << 6) | ((xo5 & 0x1F) << 1) | (rc ? 1u : 0u);
  }

  u8* m_code = nullptr;
  size_t m_capacity = 0;
  size_t m_pos = 0;
};
