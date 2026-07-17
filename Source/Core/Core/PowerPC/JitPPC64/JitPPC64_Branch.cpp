#include "Core/PowerPC/JitPPC64/Jit.h"
#include "Core/PowerPC/PowerPC.h"

// ===========================================================================
// Branch compilers
// ===========================================================================

bool JitPPC64::CompileB(UGeckoInstruction inst)
{
  gpr.Flush();  // flush dirty guest GPRs before exit
  s32 li = static_cast<s32>(inst.LI << 8) >> 6;
  u32 target = inst.AA ? static_cast<u32>(li) : js.compilerPC + li;

  if (inst.LK)
  {
    u32 lr_value = js.compilerPC + 4;
    m_asm.LI32(REG_SCRATCH, lr_value);
    m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(SPR_OFFSET + 4 * 8));
  }

  m_asm.LI32(REG_SCRATCH, target);
  m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PC_OFFSET));
  m_asm.BRel(m_exit_sequence);
  return true;
}

bool JitPPC64::CompileBC(UGeckoInstruction inst)
{
  gpr.Flush();  // flush dirty guest GPRs before exit
  const u32 bo = inst.BO;
  const u32 bi = inst.BI;
  const s32 bd = static_cast<s32>(inst.BD << 16) >> 14;
  const u32 target = inst.AA ? static_cast<u32>(bd) : js.compilerPC + bd;
  const u32 next_pc = js.compilerPC + 4;

  // Dolphin BitField<21,5> extracts BO bits in reverse order from PPC ISA:
  //   bo[4]=PPC BO[0] (CTR decrement flag: 0=decrement, 1=skip)
  //   bo[3]=PPC BO[1] (CTR condition: 0=!=0, 1===0)
  //   bo[2]=PPC BO[2] (CR check flag: 0=skip, 1=do check)
  //   bo[1]=PPC BO[3] (CR condition: 0=CR[BI]==0, 1=CR[BI]==1)
  //   bo[0]=PPC BO[4] (reserved)
  const bool true_false = (bo >> 1) & 1;         // PPC BO[3]
  const bool do_cr_check = (bo >> 2) & 1;         // PPC BO[2]
  const bool skip_ctr_check = (bo >> 4) & 1;      // PPC BO[0]

  if (!do_cr_check && skip_ctr_check)
  {
    if (inst.LK)
    {
m_asm.LI32(REG_SCRATCH, next_pc);
      m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(SPR_OFFSET + 4 * 8));
    }
    m_asm.LI32(REG_SCRATCH, target);
      m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PC_OFFSET));
    m_asm.BRel(m_exit_sequence);
    return true;
  }

  // r10 = not-taken flag (0 = taken, non-zero = not-taken)
  m_asm.ADDI(10, 0, 0);

  if (!skip_ctr_check)
  {
    const bool ctr_eq_zero = (bo >> 3) & 1;      // PPC BO[1]
    m_asm.MFSPR(REG_SCRATCH2, 9);
    m_asm.ADDI(REG_SCRATCH2, REG_SCRATCH2, -1);
    m_asm.MTSPR(9, REG_SCRATCH2);
    m_asm.CMPLWI(0, REG_SCRATCH2, 0);
    // BO=8/10: check CR[2] only, no CTR decrement (already done above via ADDI)
    if (ctr_eq_zero)
      m_asm.BC(10, 2, 8);    // BO=10: branch if eq=1 (CTR==0) → skip r10=1
    else
      m_asm.BC(8, 2, 8);     // BO=8:  branch if eq=0 (CTR!=0) → skip r10=1
    m_asm.ADDI(10, 0, 1);
  }

  if (do_cr_check)
  {
    m_asm.LWZ(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(CR_OFFSET));
    m_asm.RLWINM(REG_SCRATCH2, REG_SCRATCH, bi, 31, 31);
    m_asm.CMPWI(0, REG_SCRATCH2, 0);
    // BO=8/10: check CR[2] only, no CTR decrement
    if (true_false)
      m_asm.BC(8, 2, 8);     // BO=8:  branch if eq=0 (CR[BI]==1) → skip r10=1
    else
      m_asm.BC(10, 2, 8);    // BO=10: branch if eq=1 (CR[BI]==0) → skip r10=1
    m_asm.ADDI(10, 0, 1);
  }

  if (inst.LK)
  {
      m_asm.LI32(REG_SCRATCH2, next_pc);
      m_asm.STW(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(SPR_OFFSET + 4 * 8));
    }

    m_asm.LI32(REG_SCRATCH, next_pc);
    m_asm.CMPWI(0, 10, 0);
    m_asm.BC(8, 2, 8);         // BO=8:  branch if eq=0 (r10!=0 → not-taken → keep next_pc)
    m_asm.LI32(REG_SCRATCH, target);

  m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PC_OFFSET));
  m_asm.BRel(m_exit_sequence);
  return true;
}

// ===========================================================================
// opcd 19 dispatcher
// ===========================================================================

bool JitPPC64::CompileOPCD19(UGeckoInstruction inst)
{
  u32 subop10 = inst.SUBOP10;
  switch (subop10)
  {
  case 0:   return CompileMCRF(inst);
  case 16:  return CompileBCLR(inst);
  case 528: return CompileBCCTR(inst);
  default:
    if (subop10 >= 33 && subop10 <= 449)
      return CompileCRLogical(inst);
    return false;
  }
}

// ===========================================================================
// MCRF — move CR field (native PPC970 instruction)
// ===========================================================================

bool JitPPC64::CompileMCRF(UGeckoInstruction inst)
{
  m_asm.MCRF(inst.CRFD, inst.CRFS);
  return true;
}

// ===========================================================================
// CR logical ops — crand, cror, crxor, etc. (native PPC970 instructions)
// ===========================================================================

bool JitPPC64::CompileCRLogical(UGeckoInstruction inst)
{
  switch (inst.SUBOP10)
  {
  case 33:   m_asm.CRNOR(inst.CRBD, inst.CRBA, inst.CRBB); break;
  case 129:  m_asm.CRANDC(inst.CRBD, inst.CRBA, inst.CRBB); break;
  case 193:  m_asm.CRXOR(inst.CRBD, inst.CRBA, inst.CRBB); break;
  case 225:  m_asm.CRNAND(inst.CRBD, inst.CRBA, inst.CRBB); break;
  case 257:  m_asm.CRAND(inst.CRBD, inst.CRBA, inst.CRBB); break;
  case 289:  m_asm.CREQV(inst.CRBD, inst.CRBA, inst.CRBB); break;
  case 417:  m_asm.CRORC(inst.CRBD, inst.CRBA, inst.CRBB); break;
  case 449:  m_asm.CROR(inst.CRBD, inst.CRBA, inst.CRBB); break;
  default:   return false;
  }
  return true;
}

// ===========================================================================
// BCLR / BCLRL — branch conditional to link register
// ===========================================================================

bool JitPPC64::CompileBCLR(UGeckoInstruction inst)
{
  gpr.Flush();  // flush dirty guest GPRs before exit
  u32 bo = inst.BO;
  u32 bi = inst.BI;
  u32 next_pc = js.compilerPC + 4;

  const bool true_false = (bo >> 1) & 1;         // PPC BO[3]
  const bool do_cr_check = (bo >> 2) & 1;         // PPC BO[2]
  const bool skip_ctr_check = (bo >> 4) & 1;      // PPC BO[0]

  if (!do_cr_check && skip_ctr_check)
  {
    if (inst.LK)
    {
      m_asm.LI32(REG_SCRATCH, next_pc);
      m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(SPR_OFFSET + 4 * 8));
    }
    m_asm.LWZ(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(SPR_OFFSET + 4 * 8));
    m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 0, 0, 29);
    m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PC_OFFSET));
    m_asm.BRel(m_exit_sequence);
    return true;
  }

  // r10 = not-taken flag (0 = taken, non-zero = not-taken)
  m_asm.ADDI(10, 0, 0);

  if (!skip_ctr_check)
  {
    const bool ctr_eq_zero = (bo >> 3) & 1;      // PPC BO[1]
    m_asm.MFSPR(REG_SCRATCH2, 9);
    m_asm.ADDI(REG_SCRATCH2, REG_SCRATCH2, -1);
    m_asm.MTSPR(9, REG_SCRATCH2);
    m_asm.CMPLWI(0, REG_SCRATCH2, 0);
    if (ctr_eq_zero)
      m_asm.BC(10, 2, 8);    // BO=10: branch if eq=1 (CTR==0 → skip r10=1)
    else
      m_asm.BC(8, 2, 8);     // BO=8:  branch if eq=0 (CTR!=0 → skip r10=1)
    m_asm.ADDI(10, 0, 1);
  }

  if (do_cr_check)
  {
    m_asm.LWZ(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(CR_OFFSET));
    m_asm.RLWINM(REG_SCRATCH2, REG_SCRATCH, bi, 31, 31);
    m_asm.CMPWI(0, REG_SCRATCH2, 0);
    if (true_false)
      m_asm.BC(8, 2, 8);     // BO=8:  branch if eq=0 (CR[BI]==1 → skip r10=1)
    else
      m_asm.BC(10, 2, 8);    // BO=10: branch if eq=1 (CR[BI]==0 → skip r10=1)
    m_asm.ADDI(10, 0, 1);
  }

  if (inst.LK)
  {
    m_asm.LI32(REG_SCRATCH2, next_pc);
    m_asm.STW(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(SPR_OFFSET + 4 * 8));
  }

  m_asm.LI32(REG_SCRATCH, next_pc);
  m_asm.CMPWI(0, 10, 0);
  m_asm.BC(8, 2, 8);         // BO=8:  branch if eq=0 (r10!=0 → not-taken → keep next_pc)
  m_asm.LWZ(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(SPR_OFFSET + 4 * 8));
  m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 0, 0, 29);

  m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PC_OFFSET));
  m_asm.BRel(m_exit_sequence);
  return true;
}

// ===========================================================================
// BCCTR / BCCTRL — branch conditional to counter register
// ===========================================================================

bool JitPPC64::CompileBCCTR(UGeckoInstruction inst)
{
  gpr.Flush();  // flush dirty guest GPRs before exit
  u32 bo = inst.BO;
  u32 bi = inst.BI;
  u32 next_pc = js.compilerPC + 4;

  const bool true_false = (bo >> 1) & 1;         // PPC BO[3]
  const bool do_cr_check = (bo >> 2) & 1;         // PPC BO[2]

  if (!do_cr_check)
  {
    if (inst.LK)
    {
      m_asm.LI32(REG_SCRATCH, next_pc);
      m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(SPR_OFFSET + 4 * 8));
    }
    m_asm.MFSPR(REG_SCRATCH, 9);
    m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 0, 0, 29);
    m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PC_OFFSET));
    m_asm.BRel(m_exit_sequence);
    return true;
  }

  m_asm.ADDI(10, 0, 0);

  m_asm.LWZ(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(CR_OFFSET));
  m_asm.RLWINM(REG_SCRATCH2, REG_SCRATCH, bi, 31, 31);
  m_asm.CMPWI(0, REG_SCRATCH2, 0);
  if (true_false)
    m_asm.BC(8, 2, 8);     // BO=8:  branch if eq=0 (CR[BI]==1 → skip r10=1)
  else
    m_asm.BC(10, 2, 8);    // BO=10: branch if eq=1 (CR[BI]==0 → skip r10=1)
  m_asm.ADDI(10, 0, 1);

  // Unconditional branch to CTR (no CR or CTR check)
  if (inst.LK)
  {
    m_asm.LI32(REG_SCRATCH, next_pc);
    m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(SPR_OFFSET + 4 * 8));
  }

  m_asm.LI32(REG_SCRATCH, next_pc);
  m_asm.CMPWI(0, 10, 0);
  m_asm.BC(8, 2, 8);         // BO=8:  branch if eq=0 (r10!=0 → not-taken → keep next_pc)
  m_asm.MFSPR(REG_SCRATCH, 9);
  m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 0, 0, 29);
  m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PC_OFFSET));
  m_asm.BRel(m_exit_sequence);
  return true;
}
