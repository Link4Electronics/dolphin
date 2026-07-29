#include "Core/PowerPC/JitPPC64/Jit.h"

bool JitPPC64::CompileRLWINM(UGeckoInstruction inst)
{
  u32 ra = inst.RA, rs = inst.RS;
  u32 host_rs = gpr.R(rs);
  u32 host_ra = gpr.W(ra);
  m_asm.RLWINM(host_ra, host_rs, inst.SH, inst.MB, inst.ME);
  if (inst.Rc) EmitCR0Update(host_ra);
  return true;
}

bool JitPPC64::CompileRLWIMI(UGeckoInstruction inst)
{
  u32 ra = inst.RA, rs = inst.RS;
  u32 host_rs = gpr.R(rs);
  u32 host_ra = gpr.W(ra);
  m_asm.RLWIMI(host_ra, host_rs, inst.SH, inst.MB, inst.ME);
  if (inst.Rc) EmitCR0Update(host_ra);
  return true;
}

bool JitPPC64::CompileRLWNM(UGeckoInstruction inst)
{
  u32 ra = inst.RA, rs = inst.RS;
  u32 rb = inst.RB;
  u32 host_rs = gpr.R(rs);
  u32 host_rb = gpr.R(rb);
  u32 host_ra = gpr.W(ra);
  m_asm.RLWNM(host_ra, host_rs, host_rb, inst.MB, inst.ME);
  if (inst.Rc) EmitCR0Update(host_ra);
  return true;
}
