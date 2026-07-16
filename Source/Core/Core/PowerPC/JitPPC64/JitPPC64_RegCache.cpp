#include "Core/PowerPC/JitPPC64/JitPPC64_RegCache.h"
#include "Core/PowerPC/JitPPC64/Jit.h"

u32 JitPPC64RegCache::R(u32 ppc_reg)
{
  // Check if already cached
  for (u32 i = 0; i < NUM_CACHED_REGS; ++i)
  {
    if (m_entries[i].ppc_reg == ppc_reg)
      return FIRST_CACHED_REG + i;
  }

  // Need to load from ppcState
  u32 host_reg = FindFreeHostReg(ppc_reg);
  m_entries[host_reg - FIRST_CACHED_REG].ppc_reg = ppc_reg;
  m_entries[host_reg - FIRST_CACHED_REG].dirty = false;

  // lwz host_reg, GPR_OFFSET + 4*ppc_reg(r12)
  m_asm->LWZ(host_reg, m_ppc_base, static_cast<s32>(GPR_OFFSET + 4 * ppc_reg));
  return host_reg;
}

u32 JitPPC64RegCache::W(u32 ppc_reg)
{
  // Check if already cached
  for (u32 i = 0; i < NUM_CACHED_REGS; ++i)
  {
    if (m_entries[i].ppc_reg == ppc_reg)
    {
      m_entries[i].dirty = true;
      return FIRST_CACHED_REG + i;
    }
  }

  u32 host_reg = FindFreeHostReg(ppc_reg);
  m_entries[host_reg - FIRST_CACHED_REG].ppc_reg = ppc_reg;
  m_entries[host_reg - FIRST_CACHED_REG].dirty = true;
  return host_reg;
}

void JitPPC64RegCache::Flush()
{
  for (u32 i = 0; i < NUM_CACHED_REGS; ++i)
  {
    if (m_entries[i].dirty && m_entries[i].ppc_reg < 32)
    {
      u32 host_reg = FIRST_CACHED_REG + i;
      m_asm->STW(host_reg, m_ppc_base,
                 static_cast<s32>(GPR_OFFSET + 4 * m_entries[i].ppc_reg));
      m_entries[i].dirty = false;
    }
  }
}

void JitPPC64RegCache::Reset()
{
  for (auto& e : m_entries)
  {
    e.ppc_reg = REG_INVALID;
    e.dirty = false;
  }
}

u32 JitPPC64RegCache::FindFreeHostReg(u32 ppc_reg)
{
  // First pass: find an invalid (empty) slot
  for (u32 i = 0; i < NUM_CACHED_REGS; ++i)
  {
    if (m_entries[i].ppc_reg == REG_INVALID)
      return FIRST_CACHED_REG + i;
  }

  // Second pass: find a non-dirty slot (can discard)
  for (u32 i = 0; i < NUM_CACHED_REGS; ++i)
  {
    if (!m_entries[i].dirty)
    {
      m_entries[i].ppc_reg = ppc_reg;
      return FIRST_CACHED_REG + i;
    }
  }

  // Last resort: evict the first dirty slot (flush it)
  {
    u32 host_reg = FIRST_CACHED_REG;
    m_asm->STW(host_reg, m_ppc_base,
               static_cast<s32>(GPR_OFFSET + 4 * m_entries[0].ppc_reg));
    m_entries[0].ppc_reg = ppc_reg;
    m_entries[0].dirty = true;
    return host_reg;
  }
}
