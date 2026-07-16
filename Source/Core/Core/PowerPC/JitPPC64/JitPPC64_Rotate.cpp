#include "Core/PowerPC/JitPPC64/Jit.h"

bool JitPPC64::CompileRLWINM(UGeckoInstruction inst)
{
  u32 ra = inst.RA, rs = inst.RS;
  LoadGPR(REG_SCRATCH, rs);
  m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, inst.SH, inst.MB, inst.ME);
  if (inst.Rc) EmitCR0Update();
  StoreGPR(ra, REG_SCRATCH);
  return true;
}

bool JitPPC64::CompileRLWIMI(UGeckoInstruction inst)
{
  u32 ra = inst.RA, rs = inst.RS;
  LoadGPR(REG_SCRATCH, ra);
  LoadGPR(REG_SCRATCH2, rs);
  m_asm.RLWIMI(REG_SCRATCH, REG_SCRATCH2, inst.SH, inst.MB, inst.ME);
  if (inst.Rc) EmitCR0Update();
  StoreGPR(ra, REG_SCRATCH);
  return true;
}

bool JitPPC64::CompileRLWNM(UGeckoInstruction inst)
{
  u32 ra = inst.RA, rs = inst.RS;
  u32 rb = inst.RB;
  LoadGPR(REG_SCRATCH, rs);
  LoadGPR(REG_SCRATCH2, rb);
  m_asm.RLWNM(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2, inst.MB, inst.ME);
  if (inst.Rc) EmitCR0Update();
  StoreGPR(ra, REG_SCRATCH);
  return true;
}
