#include "Core/PowerPC/JitPPC64/Jit.h"

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
  asm_.RLDICR(REG_SCRATCH2, REG_SCRATCH2, 32, 31);
  asm_.OR(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH);
  asm_.STD(REG_SCRATCH2, REG_PPC_BASE, PS_OFFSET + 16 * fr);
}

// Load sign mask {0x80000000}×4 into VR vsign using scratch REG_SCRATCH
static void LoadSignMask(PPC64Assembler& asm_, u32 vsign)
{
  asm_.ADDI(REG_SCRATCH, 0, 0x8000);
  asm_.RLDICR(REG_SCRATCH, REG_SCRATCH, 16, 47);
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
// CompilePairedLoadStore — psq_l/psq_lu/psq_st/psq_stu + indexed forms
//   QUANTIZE_FLOAT path only (type != 0 → block-level interpreter fallback)
// ===========================================================================

bool JitPPC64::CompilePairedLoadStore(UGeckoInstruction inst)
{
  u32 opcd = inst.OPCD;
  bool indexed = (opcd == 4);
  bool is_load = (opcd == 56 || opcd == 57 ||
                  (opcd == 4 && (inst.SUBOP6 == 6 || inst.SUBOP6 == 38)));
  bool update = (opcd == 57 || opcd == 61 ||
                 (opcd == 4 && (inst.SUBOP6 == 38 || inst.SUBOP6 == 39)));
  u32 w = indexed ? inst.Wx : inst.W;
  u32 fr = inst.RD;

  // ---- EA computation ----
  if (indexed)
  {
    // X-form: EA = (RA ? GPR[RA] : 0) + GPR[RB]
    if (inst.RA == 0)
    {
      LoadGPR(REG_SCRATCH2, inst.RB);
    }
    else
    {
      LoadGPR(REG_SCRATCH, inst.RA);
      LoadGPR(REG_SCRATCH2, inst.RB);
      m_asm.ADD(REG_SCRATCH2, REG_SCRATCH, REG_SCRATCH2);
    }
  }
  else
  {
    // D-form: EA = (RA ? GPR[RA] : 0) + SIMM_12
    s32 simm = inst.SIMM_12;
    if (inst.RA == 0)
    {
      m_asm.ADDI(REG_SCRATCH2, 0, simm);
    }
    else
    {
      LoadGPR(REG_SCRATCH, inst.RA);
      m_asm.ADDI(REG_SCRATCH2, REG_SCRATCH, simm);
    }
  }

  // ---- QUANTIZE_FLOAT path ----
  if (is_load)
  {
    if (w)
    {
      // Single: load one float, PS1 = 1.0
      m_asm.LFS(0, REG_SCRATCH2, 0);
      m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET + 16 * fr));
      // ps1 = double(1.0) = 0x3FF0000000000000
      m_asm.ADDI(REG_SCRATCH, 0, 0x3FF0);
      m_asm.RLDICR(REG_SCRATCH, REG_SCRATCH, 48, 15);
      m_asm.STD(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PS_OFFSET + 16 * fr + 8));
    }
    else
    {
      // Pair: load two consecutive floats
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
      // Single: store PS0 only
      m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET + 16 * fr));
      m_asm.FRSP(0, 0);
      m_asm.STFS(0, REG_SCRATCH2, 0);
    }
    else
    {
      // Pair: store PS0 and PS1
      m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET + 16 * fr));
      m_asm.FRSP(0, 0);
      m_asm.STFS(0, REG_SCRATCH2, 0);
      m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET + 16 * fr + 8));
      m_asm.FRSP(0, 0);
      m_asm.STFS(0, REG_SCRATCH2, 4);
    }
  }

  // ---- Update form: GPR[RA] = EA ----
  if (update)
    StoreGPR(inst.RA, REG_SCRATCH2);

  return true;
}
