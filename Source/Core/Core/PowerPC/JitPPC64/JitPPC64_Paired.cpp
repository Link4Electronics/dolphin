#include <algorithm>
#include <cmath>

#include "Core/PowerPC/JitPPC64/Jit.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"

// Gekko paired singles: ppcState.ps[i] stores two 32-bit floats in a u64:
//   upper 32 bits = ps0, lower 32 bits = ps1
// AltiVec VR = 128 bits = 4x f32.
//
// VR allocation within this file:
//   VR0 = primary operand / result
//   VR1 = secondary operand
//   VR2 = sign mask (for abs/neg/nabs) or tertiary operand
//   VR3 = scratch (zero register for ps_sel)


// Register constants (mirror JitPPC64 class constants — free functions need their own)
static constexpr u32 REG_PPC_BASE = 12;
static constexpr u32 REG_SCRATCH = 0;
static constexpr u32 REG_SCRATCH2 = 11;

static constexpr u32 VR0 = 0;
static constexpr u32 VR1 = 1;
static constexpr u32 VR2 = 2;
static constexpr u32 VR3 = 3;
static constexpr s32 SCRATCH_OFF = -80;

// Load Gekko FPR pair fr into VR vd (via stack scratch space)
static void LoadFPRPairToVR(PPC64Assembler& asm_, u32 vd, u32 fr)
{
  asm_.ADDI(3, 1, SCRATCH_OFF);
  asm_.LD(REG_SCRATCH2, REG_PPC_BASE, PS_OFFSET + 16 * fr);
  asm_.STD(REG_SCRATCH2, 3, 0);
  asm_.LWZ(REG_SCRATCH2, 3, 0);
  asm_.LWZ(REG_SCRATCH, 3, 4);
  asm_.STW(REG_SCRATCH2, 3, 0);
  asm_.STW(REG_SCRATCH, 3, 4);
  asm_.ADDI(REG_SCRATCH, 0, 0);
  asm_.STW(REG_SCRATCH, 3, 8);
  asm_.STW(REG_SCRATCH, 3, 12);
  asm_.ADDI(REG_SCRATCH, 1, SCRATCH_OFF);
  asm_.LVX(vd, 0, REG_SCRATCH);
}

// Store host VR vs back to Gekko FPR pair fr
static void StoreVRToFPRPair(PPC64Assembler& asm_, u32 fr, u32 vs)
{
  asm_.ADDI(3, 1, SCRATCH_OFF);
  asm_.STVX(vs, 0, 3);
  asm_.LWZ(REG_SCRATCH2, 3, 0);
  asm_.LWZ(REG_SCRATCH, 3, 4);
  asm_.RLDICL(REG_SCRATCH2, REG_SCRATCH2, 32, 0);   // rotate left 32, MB=0 → MASK(0,63)=all 1s
  asm_.OR(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH);
  asm_.STD(REG_SCRATCH2, REG_PPC_BASE, PS_OFFSET + 16 * fr);
}

// Load sign mask {0x80000000}×4 into VR vsign using scratch REG_SCRATCH
static void LoadSignMask(PPC64Assembler& asm_, u32 vsign)
{
  // ADDIS sign-extends 0x8000: r = 0xFFFF_FFFF_8000_0000, then STW stores low 32 = 0x80000000
  asm_.ADDIS(REG_SCRATCH, 0, 0x8000);
  asm_.STW(REG_SCRATCH, 1, SCRATCH_OFF);
  asm_.STW(REG_SCRATCH, 1, SCRATCH_OFF + 4);
  asm_.STW(REG_SCRATCH, 1, SCRATCH_OFF + 8);
  asm_.STW(REG_SCRATCH, 1, SCRATCH_OFF + 12);
  asm_.ADDI(REG_SCRATCH, 1, SCRATCH_OFF);
  asm_.LVX(vsign, 0, REG_SCRATCH);
}

// ===========================================================================
// CompilePairedSingle — compile opcd 4 Paired Single instructions
// ===========================================================================

bool JitPPC64::CompilePairedSingle(UGeckoInstruction inst)
{
  // ---- SUBOP10 dispatch (s_table4 entries) ----
  u32 xo10 = inst.SUBOP10;
  switch (xo10)
  {
  case 0:   // ps_cmpu0: unordered compare of ps0 → crfD
    m_asm.LFS(1, REG_PPC_BASE, static_cast<s32>(PS_OFFSET + 16 * (inst.FA)));
    m_asm.LFS(2, REG_PPC_BASE, static_cast<s32>(PS_OFFSET + 16 * (inst.FB)));
    m_asm.FCMPU(inst.CRFD, 1, 2);
    return true;
  case 32:  // ps_cmpo0: ordered compare of ps0 → crfD
    m_asm.LFS(1, REG_PPC_BASE, static_cast<s32>(PS_OFFSET + 16 * (inst.FA)));
    m_asm.LFS(2, REG_PPC_BASE, static_cast<s32>(PS_OFFSET + 16 * (inst.FB)));
    m_asm.FCMPO(inst.CRFD, 1, 2);
    return true;
  case 64:  // ps_cmpu1: unordered compare of ps1 → crfD
    m_asm.LFS(1, REG_PPC_BASE, static_cast<s32>(PS_OFFSET + 16 * (inst.FA) + 4));
    m_asm.LFS(2, REG_PPC_BASE, static_cast<s32>(PS_OFFSET + 16 * (inst.FB) + 4));
    m_asm.FCMPU(inst.CRFD, 1, 2);
    return true;
  case 96:  // ps_cmpo1: ordered compare of ps1 → crfD
    m_asm.LFS(1, REG_PPC_BASE, static_cast<s32>(PS_OFFSET + 16 * (inst.FA) + 4));
    m_asm.LFS(2, REG_PPC_BASE, static_cast<s32>(PS_OFFSET + 16 * (inst.FB) + 4));
    m_asm.FCMPO(inst.CRFD, 1, 2);
    return true;

  case 72:  // ps_mr: frD = frB (identity copy)
    LoadFPRPairToVR(m_asm, VR0, inst.FB);
    StoreVRToFPRPair(m_asm, inst.FD, VR0);
    return true;

  case 40:  // ps_neg: frD = −frB (flip sign bit)
    LoadFPRPairToVR(m_asm, VR0, inst.FB);
    LoadSignMask(m_asm, VR2);
    m_asm.VXOR(VR0, VR0, VR2);
    StoreVRToFPRPair(m_asm, inst.FD, VR0);
    return true;

  case 136: // ps_nabs: frD = −|frB| (set sign bit)
    LoadFPRPairToVR(m_asm, VR0, inst.FB);
    LoadSignMask(m_asm, VR2);
    m_asm.VOR(VR0, VR0, VR2);
    StoreVRToFPRPair(m_asm, inst.FD, VR0);
    return true;

  case 264: // ps_abs: frD = |frB| (clear sign bit via AND with complement)
    LoadFPRPairToVR(m_asm, VR0, inst.FB);
    LoadSignMask(m_asm, VR2);
    m_asm.VANDC(VR0, VR0, VR2);
    StoreVRToFPRPair(m_asm, inst.FD, VR0);
    return true;

  case 528: // ps_merge00: {frA[0], frB[0]}
    LoadFPRPairToVR(m_asm, VR0, inst.FA);
    LoadFPRPairToVR(m_asm, VR1, inst.FB);
    m_asm.VMRGHW(VR0, VR0, VR1);
    StoreVRToFPRPair(m_asm, inst.FD, VR0);
    return true;

  case 560: // ps_merge01: {frA[0], frB[1]}
    LoadFPRPairToVR(m_asm, VR0, inst.FA);
    LoadFPRPairToVR(m_asm, VR1, inst.FB);
    m_asm.VSPLTW(VR1, VR1, 1);
    m_asm.VMRGHW(VR0, VR0, VR1);
    StoreVRToFPRPair(m_asm, inst.FD, VR0);
    return true;

  case 592: // ps_merge10: {frA[1], frB[0]}
    LoadFPRPairToVR(m_asm, VR0, inst.FA);
    LoadFPRPairToVR(m_asm, VR1, inst.FB);
    m_asm.VSPLTW(VR0, VR0, 1);
    m_asm.VMRGHW(VR0, VR0, VR1);
    StoreVRToFPRPair(m_asm, inst.FD, VR0);
    return true;

  case 624: // ps_merge11: {frA[1], frB[1]}
    LoadFPRPairToVR(m_asm, VR0, inst.FA);
    LoadFPRPairToVR(m_asm, VR1, inst.FB);
    m_asm.VSPLTW(VR0, VR0, 1);
    m_asm.VSPLTW(VR1, VR1, 1);
    m_asm.VMRGHW(VR0, VR0, VR1);
    StoreVRToFPRPair(m_asm, inst.FD, VR0);
    return true;

  default:
    break;
  }

  // ---- SUBOP5 dispatch (s_table4_2 entries) ----
  u32 xo5 = inst.SUBOP5;
  switch (xo5)
  {
  // 2-operand arithmetic (frA = a, frB = b, frD = result)
  case 21:  // ps_add
    LoadFPRPairToVR(m_asm, VR0, inst.FA);
    LoadFPRPairToVR(m_asm, VR1, inst.FB);
    m_asm.VADDFP(VR0, VR0, VR1);
    StoreVRToFPRPair(m_asm, inst.FD, VR0);
    return true;

  case 20:  // ps_sub
    LoadFPRPairToVR(m_asm, VR0, inst.FA);
    LoadFPRPairToVR(m_asm, VR1, inst.FB);
    m_asm.VSUBFP(VR0, VR0, VR1);
    StoreVRToFPRPair(m_asm, inst.FD, VR0);
    return true;

  case 25:  // ps_mul
    LoadFPRPairToVR(m_asm, VR0, inst.FA);
    LoadFPRPairToVR(m_asm, VR1, inst.FB);
    m_asm.VMULFP(VR0, VR0, VR1);
    StoreVRToFPRPair(m_asm, inst.FD, VR0);
    return true;

  case 18:  // ps_div
    LoadFPRPairToVR(m_asm, VR0, inst.FA);
    LoadFPRPairToVR(m_asm, VR1, inst.FB);
    m_asm.VDIVFP(VR0, VR0, VR1);
    StoreVRToFPRPair(m_asm, inst.FD, VR0);
    return true;

  // Sum (broadcast element sum to both slots)
  case 10:  // ps_sum0: {frA[0]+frB[0], frA[0]+frB[0]}
    LoadFPRPairToVR(m_asm, VR0, inst.FA);
    LoadFPRPairToVR(m_asm, VR1, inst.FB);
    m_asm.VADDFP(VR0, VR0, VR1);
    m_asm.VSPLTW(VR0, VR0, 0);
    StoreVRToFPRPair(m_asm, inst.FD, VR0);
    return true;

  case 11:  // ps_sum1: {frA[1]+frB[1], frA[1]+frB[1]}
    LoadFPRPairToVR(m_asm, VR0, inst.FA);
    LoadFPRPairToVR(m_asm, VR1, inst.FB);
    m_asm.VADDFP(VR0, VR0, VR1);
    m_asm.VSPLTW(VR0, VR0, 1);
    StoreVRToFPRPair(m_asm, inst.FD, VR0);
    return true;

  // Scalar broadcast (frD = frC[0/1] * frA)
  case 12:  // ps_muls0: frD = frC[0] * frA
    LoadFPRPairToVR(m_asm, VR0, inst.FC);
    m_asm.VSPLTW(VR0, VR0, 0);
    LoadFPRPairToVR(m_asm, VR1, inst.FA);
    m_asm.VMULFP(VR0, VR0, VR1);
    StoreVRToFPRPair(m_asm, inst.FD, VR0);
    return true;

  case 13:  // ps_muls1: frD = frC[1] * frA
    LoadFPRPairToVR(m_asm, VR0, inst.FC);
    m_asm.VSPLTW(VR0, VR0, 1);
    LoadFPRPairToVR(m_asm, VR1, inst.FA);
    m_asm.VMULFP(VR0, VR0, VR1);
    StoreVRToFPRPair(m_asm, inst.FD, VR0);
    return true;

  // Scalar FMA (frD = frC[0/1] * frA + frB)
  case 14:  // ps_madds0
    LoadFPRPairToVR(m_asm, VR0, inst.FC);
    m_asm.VSPLTW(VR0, VR0, 0);
    LoadFPRPairToVR(m_asm, VR1, inst.FA);
    LoadFPRPairToVR(m_asm, VR2, inst.FB);
    m_asm.VMADDFP(VR0, VR0, VR1, VR2);
    StoreVRToFPRPair(m_asm, inst.FD, VR0);
    return true;

  case 15:  // ps_madds1
    LoadFPRPairToVR(m_asm, VR0, inst.FC);
    m_asm.VSPLTW(VR0, VR0, 1);
    LoadFPRPairToVR(m_asm, VR1, inst.FA);
    LoadFPRPairToVR(m_asm, VR2, inst.FB);
    m_asm.VMADDFP(VR0, VR0, VR1, VR2);
    StoreVRToFPRPair(m_asm, inst.FD, VR0);
    return true;

  // 3-operand FMA (frD = frA * frC ± frB)
  case 29:  // ps_madd: frD = frA * frC + frB
    LoadFPRPairToVR(m_asm, VR0, inst.FA);
    LoadFPRPairToVR(m_asm, VR1, inst.FB);
    LoadFPRPairToVR(m_asm, VR2, inst.FC);
    m_asm.VMADDFP(VR0, VR0, VR2, VR1);
    StoreVRToFPRPair(m_asm, inst.FD, VR0);
    return true;

  case 28:  // ps_msub: frD = frA * frC − frB
    LoadFPRPairToVR(m_asm, VR0, inst.FA);
    LoadFPRPairToVR(m_asm, VR1, inst.FB);
    LoadFPRPairToVR(m_asm, VR2, inst.FC);
    m_asm.VMSUBFP(VR0, VR0, VR2, VR1);
    StoreVRToFPRPair(m_asm, inst.FD, VR0);
    return true;

  case 31:  // ps_nmadd: frD = −(frA * frC + frB)
    LoadFPRPairToVR(m_asm, VR0, inst.FA);
    LoadFPRPairToVR(m_asm, VR1, inst.FB);
    LoadFPRPairToVR(m_asm, VR2, inst.FC);
    m_asm.VNMADDFP(VR0, VR0, VR2, VR1);
    StoreVRToFPRPair(m_asm, inst.FD, VR0);
    return true;

  case 30:  // ps_nmsub: frD = −(frA * frC − frB)
    LoadFPRPairToVR(m_asm, VR0, inst.FA);
    LoadFPRPairToVR(m_asm, VR1, inst.FB);
    LoadFPRPairToVR(m_asm, VR2, inst.FC);
    m_asm.VNMSUBFP(VR0, VR0, VR2, VR1);
    StoreVRToFPRPair(m_asm, inst.FD, VR0);
    return true;

  // ps_sel: frD[i] = frA[i] >= 0.0 ? frB[i] : frC[i]
  case 23:
  {
    LoadFPRPairToVR(m_asm, VR0, inst.FA);
    LoadFPRPairToVR(m_asm, VR1, inst.FB);
    LoadFPRPairToVR(m_asm, VR2, inst.FC);
    m_asm.VXOR(VR3, VR3, VR3);           // zero VR3
    m_asm.VCMPGEFP(VR3, VR0, VR3);       // VR3[i] = all-ones if frA[i] >= 0
    m_asm.VSEL(VR0, VR3, VR1, VR2);      // VR0 = (VR3 & VR1) | (~VR3 & VR2)
    StoreVRToFPRPair(m_asm, inst.FD, VR0);
    return true;
  }

  case 24:  // ps_res: reciprocal estimate (1/x)
    LoadFPRPairToVR(m_asm, VR0, inst.FB);
    m_asm.VREFP(VR0, VR0);
    StoreVRToFPRPair(m_asm, inst.FD, VR0);
    return true;

  case 26:  // ps_rsqrte: reciprocal sqrt estimate (1/sqrt(x))
    LoadFPRPairToVR(m_asm, VR0, inst.FB);
    m_asm.VRSQRTEFP(VR0, VR0);
    StoreVRToFPRPair(m_asm, inst.FD, VR0);
    return true;

  default:
    return false;
  }
}

// ===========================================================================
// Helper: load 1-2 bytes from (ea_reg + offset), sign-extend if needed,
// convert to single-precision float via LFIWAX + FRSP, result in dst_fpr.
// Clobbers REG_SCRATCH (r0).
// ===========================================================================
static void LoadIntToFPR(PPC64Assembler& asm_, u32 dst_fpr, u32 ea_reg, u32 offset, u32 type)
{
  switch (type)
  {
  case QUANTIZE_U8:
    asm_.LBZ(REG_SCRATCH, ea_reg, offset);
    break;
  case QUANTIZE_S8:
    asm_.LBZ(REG_SCRATCH, ea_reg, offset);
    asm_.EXTSB(REG_SCRATCH, REG_SCRATCH);
    break;
  case QUANTIZE_U16:
    asm_.LHZ(REG_SCRATCH, ea_reg, offset);
    break;
  case QUANTIZE_S16:
    asm_.LHZ(REG_SCRATCH, ea_reg, offset);
    asm_.EXTSH(REG_SCRATCH, REG_SCRATCH);
    break;
  }
  asm_.STW(REG_SCRATCH, 1, SCRATCH_OFF);
  asm_.ADDI(REG_SCRATCH, 1, SCRATCH_OFF);
  asm_.LFIWAX(dst_fpr, 0, REG_SCRATCH);
  asm_.FRSP(dst_fpr, dst_fpr);
}

// ===========================================================================
// Helper: store s32 from src_fpr (already FCTIWZ'd) to (ea_reg + offset) as
// 1 byte (U8/S8) or 2 bytes (U16/S16).  Clobbers REG_SCRATCH (r0).
// ===========================================================================
static void StoreIntFromFPR(PPC64Assembler& asm_, u32 src_fpr, u32 ea_reg, u32 offset, u32 type)
{
  asm_.ADDI(REG_SCRATCH, 1, SCRATCH_OFF);
  asm_.STFIWX(src_fpr, 0, REG_SCRATCH);
  asm_.LWZ(REG_SCRATCH, 1, SCRATCH_OFF);
  const u32 w = (type == QUANTIZE_U8 || type == QUANTIZE_S8) ? 1 : 2;
  if (w == 1)
    asm_.STB(REG_SCRATCH, ea_reg, offset);
  else
    asm_.STH(REG_SCRATCH, ea_reg, offset);
}

// ===========================================================================
// CompilePairedLoadStore — psq_l/psq_lu/psq_st/psq_stu + indexed forms
//   Float (type=0) and integer (type 1-4) quantize paths, both inline.
//   Uses scalar FPU for integer paths (LFIWAX/FCTIWZ/STFIWX) since PPC970
//   AltiVec has no int↔float conversion instructions.
// ===========================================================================

bool JitPPC64::CompilePairedLoadStore(UGeckoInstruction inst)
{
  if (!jo.fastmem)
    return false;
  u32 opcd = inst.OPCD;
  bool indexed = (opcd == 4);
  bool is_load = (opcd == 56 || opcd == 57 ||
                  (opcd == 4 && (inst.SUBOP6 == 6 || inst.SUBOP6 == 38)));
  bool update = (opcd == 57 || opcd == 61 ||
                 (opcd == 4 && (inst.SUBOP6 == 38 || inst.SUBOP6 == 39)));
  u32 w = indexed ? inst.Wx : inst.W;
  u32 fr = inst.RD;

  // GQR layout: bits 0:2 = st_type, bits 8:13 = st_scale, bits 16:18 = ld_type,
  // bits 24:29 = ld_scale.
  // D-form uses inst.I (bits 12:13, GQR 0-3), X-form uses inst.Ix (bits 7:9, GQR 0-7).
  u32 gqr = indexed ? inst.Ix : inst.I;
  u32 gqr_val = m_ppc_state.spr[SPR_GQR0 + gqr];
  u32 type = is_load ? ((gqr_val >> 16) & 0x7) : ((gqr_val >> 0) & 0x7);
  u32 scale = is_load ? ((gqr_val >> 24) & 0x3F) : ((gqr_val >> 8) & 0x3F);

  // For integer quantize types 1-4, emit inline AltiVec or scalar conversion.
  // The GQR value is read at JIT COMPILE TIME; if it changes, the block will be
  // re-compiled (block invalidation).  Types 5-7 (illegal) fall to interpreter.
  if (type != QUANTIZE_FLOAT && type > QUANTIZE_S16)
    return false;

  // ---- EA computation (regcache) ----
  if (indexed)
  {
    // X-form: EA = (RA ? GPR[RA] : 0) + GPR[RB]
    if (inst.RA == 0)
      m_asm.MR(REG_SCRATCH2, gpr.R(inst.RB));
    else
      m_asm.ADD(REG_SCRATCH2, gpr.R(inst.RA), gpr.R(inst.RB));
  }
  else
  {
    // D-form: EA = (RA ? GPR[RA] : 0) + SIMM_12
    s32 simm = inst.SIMM_12;
    if (inst.RA == 0)
      m_asm.ADDI(REG_SCRATCH2, 0, simm);
    else if (m_constant_propagation.HasGPR(inst.RA))
      m_asm.LI32(REG_SCRATCH2, m_constant_propagation.GetGPR(inst.RA) + simm);
    else
      m_asm.ADDI(REG_SCRATCH2, gpr.R(inst.RA), simm);
  }

  // Save guest EA for update-form RA write-back, then translate to host address
  if (update)
    m_asm.STD(REG_SCRATCH2, 1, EA_SAVE_OFFSET);
  m_asm.RLDICL(REG_SCRATCH2, REG_SCRATCH2, 0, 32);
  m_asm.ADD(REG_SCRATCH2, REG_SCRATCH2, REG_PHYS_BASE);

  if (type == QUANTIZE_FLOAT)
  {
    // ---- QUANTIZE_FLOAT path (AltiVec / inline FPR) ----
    if (is_load)
    {
      if (w)
      {
        // Single: load one float, PS1 = 1.0
        m_asm.LFS(0, REG_SCRATCH2, 0);
        m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET + 16 * fr));
        m_asm.ADDI(REG_SCRATCH, 0, 0x3FF0);
        m_asm.RLDICL(REG_SCRATCH, REG_SCRATCH, 48, 0);   // rotate left 48, MB=0 → MASK(0,63)=all 1s
        m_asm.STD(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PS_OFFSET + 16 * fr + 8));
      }
      else
      {
        m_asm.LFS(0, REG_SCRATCH2, 0);
        m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET + 16 * fr));
        m_asm.LFS(0, REG_SCRATCH2, 4);
        m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET + 16 * fr + 8));
      }
    }
    else
    {
      if (w)
      {
        m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET + 16 * fr));
        m_asm.FRSP(0, 0);
        m_asm.STFS(0, REG_SCRATCH2, 0);
      }
      else
      {
        m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET + 16 * fr));
        m_asm.FRSP(0, 0);
        m_asm.STFS(0, REG_SCRATCH2, 0);
        m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET + 16 * fr + 8));
        m_asm.FRSP(0, 0);
        m_asm.STFS(0, REG_SCRATCH2, 4);
      }
    }
  }
  else
  {
    // ---- Integer quantize path (scalar FPU only — PPC970 has no AltiVec int↔float) ----
    const float scale_val = is_load ? m_dequantizeTableS[scale] : m_quantizeTableS[scale];
    u32 scale_bits;
    std::memcpy(&scale_bits, &scale_val, 4);

    // Load scale factor as single-precision into f2
    m_asm.LI32(REG_SCRATCH, scale_bits);
    m_asm.STW(REG_SCRATCH, 1, SCRATCH_OFF);
    m_asm.LFS(2, 1, SCRATCH_OFF);

    if (is_load)
    {
      if (w)
      {
        // ---- Single load: int → float ----
        LoadIntToFPR(m_asm, 0, REG_SCRATCH2, 0, type);
        m_asm.FMUL(0, 0, 2);

        // Store ps0 as double, ps1 = double(1.0)
        m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET + 16 * fr));
        m_asm.LI(REG_SCRATCH, 0x3FF0);
        m_asm.RLDICL(REG_SCRATCH, REG_SCRATCH, 48, 0);
        m_asm.STD(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PS_OFFSET + 16 * fr + 8));
      }
      else
      {
        // ---- Pair load: int → float (scalar, 2 elements) ----
        LoadIntToFPR(m_asm, 0, REG_SCRATCH2, 0, type);
        m_asm.FMUL(0, 0, 2);
        m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET + 16 * fr));

        const u32 elem_bytes = (type == QUANTIZE_U8 || type == QUANTIZE_S8) ? 1 : 2;
        LoadIntToFPR(m_asm, 1, REG_SCRATCH2, elem_bytes, type);
        m_asm.FMUL(1, 1, 2);
        m_asm.STFD(1, REG_PPC_BASE, static_cast<s32>(PS_OFFSET + 16 * fr + 8));
      }
    }
    else
    {
      // Scale is the INVERSE for store path (quantize)
      // Reload scale factor (was loaded as dequantize above if is_load was false... no,
      // we loaded scale_val from quantizeTableS for store path, so it's correct)
      // Actually, the scale loading above is common. No re-load needed.

      if (w)
      {
        // ---- Single store: float → int ----
        m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET + 16 * fr));
        m_asm.FRSP(0, 0);
        m_asm.FMUL(0, 0, 2);
        m_asm.FCTIWZ(0, 0);
        StoreIntFromFPR(m_asm, 0, REG_SCRATCH2, 0, type);
      }
      else
      {
        // ---- Pair store: float → int (scalar, 2 elements) ----
        m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET + 16 * fr));
        m_asm.FRSP(0, 0);
        m_asm.FMUL(0, 0, 2);
        m_asm.FCTIWZ(0, 0);
        StoreIntFromFPR(m_asm, 0, REG_SCRATCH2, 0, type);

        m_asm.LFD(1, REG_PPC_BASE, static_cast<s32>(PS_OFFSET + 16 * fr + 8));
        m_asm.FRSP(1, 1);
        m_asm.FMUL(1, 1, 2);
        m_asm.FCTIWZ(1, 1);
        const u32 elem_bytes = (type == QUANTIZE_U8 || type == QUANTIZE_S8) ? 1 : 2;
        StoreIntFromFPR(m_asm, 1, REG_SCRATCH2, elem_bytes, type);
      }
    }
  }

  // ---- Update form: GPR[RA] = EA (reload from stack, REG_SCRATCH2 is now physical addr) ----
  if (update)
  {
    m_asm.LD(gpr.W(inst.RA), 1, EA_SAVE_OFFSET);
  }

  return true;
}

// ===========================================================================
// C helper functions for integer quantized psq_l/st (types 1-4)
//
// Called from JIT-emitted code when the GQR quantize type is not FLOAT.
// The GQR is read at RUNTIME from ppcState, so GQR changes between JIT
// compilation and execution are handled correctly.
//
// These replicate the logic in Interpreter_LoadStorePaired.cpp's
// Helper_Dequantize / Helper_Quantize but use the JIT-safe MMU API.
// ===========================================================================

static void LoadDequantizePair(PowerPC::MMU& mmu, float& ps0, float& ps1,
                                u32 addr, int scale, int type, bool single)
{
  switch (type)
  {
  case QUANTIZE_U8:
    if (single)
    {
      ps0 = float(PowerPC::ReadFromJit<u8>(mmu, addr)) * m_dequantizeTableS[scale];
      ps1 = 1.0f;
    }
    else
    {
      const u8 v0 = PowerPC::ReadFromJit<u8>(mmu, addr);
      const u8 v1 = PowerPC::ReadFromJit<u8>(mmu, addr + 1);
      ps0 = float(v0) * m_dequantizeTableS[scale];
      ps1 = float(v1) * m_dequantizeTableS[scale];
    }
    break;
  case QUANTIZE_U16:
    if (single)
    {
      const auto v = PowerPC::ReadFromJit<u16>(mmu, addr);
      ps0 = float(v) * m_dequantizeTableS[scale];
      ps1 = 1.0f;
    }
    else
    {
      const auto v0 = PowerPC::ReadFromJit<u16>(mmu, addr);
      const auto v1 = PowerPC::ReadFromJit<u16>(mmu, addr + 2);
      ps0 = float(v0) * m_dequantizeTableS[scale];
      ps1 = float(v1) * m_dequantizeTableS[scale];
    }
    break;
  case QUANTIZE_S8:
    if (single)
    {
      ps0 = float(s8(PowerPC::ReadFromJit<u8>(mmu, addr))) * m_dequantizeTableS[scale];
      ps1 = 1.0f;
    }
    else
    {
      const s8 v0 = s8(PowerPC::ReadFromJit<u8>(mmu, addr));
      const s8 v1 = s8(PowerPC::ReadFromJit<u8>(mmu, addr + 1));
      ps0 = float(v0) * m_dequantizeTableS[scale];
      ps1 = float(v1) * m_dequantizeTableS[scale];
    }
    break;
  case QUANTIZE_S16:
    if (single)
    {
      const auto v = s16(PowerPC::ReadFromJit<u16>(mmu, addr));
      ps0 = float(v) * m_dequantizeTableS[scale];
      ps1 = 1.0f;
    }
    else
    {
      const s16 v0 = s16(PowerPC::ReadFromJit<u16>(mmu, addr));
      const s16 v1 = s16(PowerPC::ReadFromJit<u16>(mmu, addr + 2));
      ps0 = float(v0) * m_dequantizeTableS[scale];
      ps1 = float(v1) * m_dequantizeTableS[scale];
    }
    break;
  default:
    ps0 = 0.0f;
    ps1 = 1.0f;
    break;
  }
}

static void QuantizeStorePair(PowerPC::MMU& mmu, float ps0, float ps1,
                               u32 addr, int scale, int type, bool single)
{
  const float inv_scale = m_quantizeTableS[scale];
  switch (type)
  {
  case QUANTIZE_U8:
    if (single)
    {
      const u8 v = static_cast<u8>(std::clamp(std::lround(ps0 * inv_scale), 0L, 255L));
      PowerPC::WriteFromJit<u8>(mmu, v, addr);
    }
    else
    {
      const u8 v0 = static_cast<u8>(std::clamp(std::lround(ps0 * inv_scale), 0L, 255L));
      const u8 v1 = static_cast<u8>(std::clamp(std::lround(ps1 * inv_scale), 0L, 255L));
      PowerPC::WriteFromJit<u8>(mmu, v0, addr);
      PowerPC::WriteFromJit<u8>(mmu, v1, addr + 1);
    }
    break;
  case QUANTIZE_U16:
    if (single)
    {
      const u16 v = static_cast<u16>(std::clamp(std::lround(ps0 * inv_scale), 0L, 65535L));
      PowerPC::WriteFromJit<u16>(mmu, v, addr);
    }
    else
    {
      const u16 v0 = static_cast<u16>(std::clamp(std::lround(ps0 * inv_scale), 0L, 65535L));
      const u16 v1 = static_cast<u16>(std::clamp(std::lround(ps1 * inv_scale), 0L, 65535L));
      PowerPC::WriteFromJit<u16>(mmu, v0, addr);
      PowerPC::WriteFromJit<u16>(mmu, v1, addr + 2);
    }
    break;
  case QUANTIZE_S8:
    if (single)
    {
      const s8 v = static_cast<s8>(std::clamp(std::lround(ps0 * inv_scale), -128L, 127L));
      PowerPC::WriteFromJit<u8>(mmu, static_cast<u8>(v), addr);
    }
    else
    {
      const s8 v0 = static_cast<s8>(std::clamp(std::lround(ps0 * inv_scale), -128L, 127L));
      const s8 v1 = static_cast<s8>(std::clamp(std::lround(ps1 * inv_scale), -128L, 127L));
      PowerPC::WriteFromJit<u8>(mmu, static_cast<u8>(v0), addr);
      PowerPC::WriteFromJit<u8>(mmu, static_cast<u8>(v1), addr + 1);
    }
    break;
  case QUANTIZE_S16:
    if (single)
    {
      const s16 v = static_cast<s16>(std::clamp(std::lround(ps0 * inv_scale), -32768L, 32767L));
      PowerPC::WriteFromJit<u16>(mmu, static_cast<u16>(v), addr);
    }
    else
    {
      const s16 v0 = static_cast<s16>(std::clamp(std::lround(ps0 * inv_scale), -32768L, 32767L));
      const s16 v1 = static_cast<s16>(std::clamp(std::lround(ps1 * inv_scale), -32768L, 32767L));
      PowerPC::WriteFromJit<u16>(mmu, static_cast<u16>(v0), addr);
      PowerPC::WriteFromJit<u16>(mmu, static_cast<u16>(v1), addr + 2);
    }
    break;
  default:
    break;
  }
}

extern "C" void JitPPC64PsqLoad(PowerPC::PowerPCState* state, PowerPC::MMU* mmu,
                                u32 ea, u32 gqr_idx, u32 fr, u32 w)
{
  const UGQR gqr(state->spr[SPR_GQR0 + gqr_idx]);
  float ps0, ps1;
  LoadDequantizePair(*mmu, ps0, ps1, ea, gqr.ld_scale, gqr.ld_type, w != 0);
  state->ps[fr].SetBoth(static_cast<double>(ps0), static_cast<double>(ps1));
}

extern "C" void JitPPC64PsqStore(PowerPC::PowerPCState* state, PowerPC::MMU* mmu,
                                 u32 ea, u32 gqr_idx, u32 rs, u32 w)
{
  const UGQR gqr(state->spr[SPR_GQR0 + gqr_idx]);
  const float ps0 = static_cast<float>(state->ps[rs].PS0AsDouble());
  const float ps1 = static_cast<float>(state->ps[rs].PS1AsDouble());
  QuantizeStorePair(*mmu, ps0, ps1, ea, gqr.st_scale, gqr.st_type, w != 0);
}
