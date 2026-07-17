#include "Core/PowerPC/JitPPC64/Jit.h"

bool JitPPC64::CompileRLWINM(UGeckoInstruction inst)
{
  u32 ra = inst.RA, rs = inst.RS;
  u32 host_ra = gpr.W(ra);
  m_asm.RLWINM(host_ra, gpr.R(rs), inst.SH, inst.MB, inst.ME);
  if (inst.Rc) EmitCR0Update(host_ra);
  return true;
}

bool JitPPC64::CompileRLWIMI(UGeckoInstruction inst)
{
  u32 ra = inst.RA, rs = inst.RS;
  u32 host_ra = gpr.W(ra);
  m_asm.RLWIMI(host_ra, gpr.R(rs), inst.SH, inst.MB, inst.ME);
  if (inst.Rc) EmitCR0Update(host_ra);
  return true;
}

bool JitPPC64::CompileRLWNM(UGeckoInstruction inst)
{
  u32 ra = inst.RA, rs = inst.RS;
  u32 rb = inst.RB;
  u32 host_ra = gpr.W(ra);
  m_asm.RLWNM(host_ra, gpr.R(rs), gpr.R(rb), inst.MB, inst.ME);
  if (inst.Rc) EmitCR0Update(host_ra);
  return true;
}
