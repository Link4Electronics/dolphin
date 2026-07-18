#include "Core/PowerPC/JitPPC64/JitPPC64_RegCache.h"
#include "Core/PowerPC/JitPPC64/Jit.h"
#include "Core/PowerPC/PPCAnalyst.h"

bool JitPPC64RegCache::HasGPR(u32 ppc_reg) const
{
  return m_known_mask & (1u << ppc_reg);
}

u32 JitPPC64RegCache::GetGPR(u32 ppc_reg) const
{
  return m_known_val[ppc_reg];
}

void JitPPC64RegCache::SetImmediate32(u32 ppc_reg, u32 val)
{
  m_known_val[ppc_reg] = val;
  m_known_mask |= (1u << ppc_reg);
}

u32 JitPPC64RegCache::R(u32 ppc_reg)
{
  // Check if already cached
  for (u32 i = 0; i < NUM_CACHED_REGS; ++i)
  {
    if (m_entries[i].ppc_reg == ppc_reg)
      return FIRST_CACHED_REG + i;
  }

  // Need to load into a host register
  u32 host_reg = FindFreeHostReg(ppc_reg);
  m_entries[host_reg - FIRST_CACHED_REG].ppc_reg = ppc_reg;
  m_entries[host_reg - FIRST_CACHED_REG].dirty = false;

  if (m_known_mask & (1u << ppc_reg))
  {
    // Known constant: emit LI instead of LWZ from ppcState
    u32 val = m_known_val[ppc_reg];
    if (val <= 0x7FFF)
      m_asm->ADDI(host_reg, 0, static_cast<s32>(val));
    else
      m_asm->LI32(host_reg, val);
  }
  else
  {
    // lwz host_reg, GPR_OFFSET + 4*ppc_reg(r12)
    m_asm->LWZ(host_reg, m_ppc_base, static_cast<s32>(GPR_OFFSET + 4 * ppc_reg));
  }
  return host_reg;
}

u32 JitPPC64RegCache::W(u32 ppc_reg)
{
  // Invalidate known-constant tracking — writing to the reg changes its value
  InvalidateKnown(ppc_reg);

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

void JitPPC64RegCache::Flush(const PPCAnalyst::CodeOp* op)
{
  if (!op)
  {
    Flush();
    return;
  }

  for (u32 i = 0; i < NUM_CACHED_REGS; ++i)
  {
    const u32 guest_reg = m_entries[i].ppc_reg;
    if (m_entries[i].dirty && guest_reg < 32)
    {
      // If the PPCAnalyst says the register is discardable (will be
      // overwritten before next use), skip the store.
      if (op->gprDiscardable[guest_reg])
      {
        m_entries[i].dirty = false;
        continue;
      }

      u32 host_reg = FIRST_CACHED_REG + i;
      m_asm->STW(host_reg, m_ppc_base,
                 static_cast<s32>(GPR_OFFSET + 4 * guest_reg));
      m_entries[i].dirty = false;
    }
  }
}

void JitPPC64RegCache::FlushRegister(u32 ppc_reg)
{
  for (u32 i = 0; i < NUM_CACHED_REGS; ++i)
  {
    if (m_entries[i].ppc_reg == ppc_reg && m_entries[i].dirty)
    {
      u32 host_reg = FIRST_CACHED_REG + i;
      m_asm->STW(host_reg, m_ppc_base,
                 static_cast<s32>(GPR_OFFSET + 4 * ppc_reg));
      m_entries[i].dirty = false;
      return;
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
  m_known_mask = 0;
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

// ===========================================================================
// JitPPC64FPRCache
// ===========================================================================
//
// Each host FPR caches a full 64-bit paired-single value:
//   [ps0:upper32][ps1:lower32]
//
// Load from ppcState.ps[guest_reg] via LFD (64 bits).
// Store back to ppcState.ps[guest_reg] via STFD (64 bits).
// sizeof(PairedSingle) = 16 bytes, so each guest FPR occupies 16 bytes.
// PS0 (the 64-bit value) is at offset 0 within each 16-byte slot.
// ===========================================================================

u32 JitPPC64FPRCache::R(u32 guest_reg)
{
  // Check if already cached
  for (u32 i = 0; i < NUM_CACHED_FPRS; ++i)
  {
    if (m_entries[i].guest_reg == guest_reg)
      return FIRST_CACHED_FPR + i;
  }

  // Need to load from ppcState into a host FPR
  u32 host_fpr = FindFreeHostFPR(guest_reg);
  m_entries[host_fpr - FIRST_CACHED_FPR].guest_reg = guest_reg;
  m_entries[host_fpr - FIRST_CACHED_FPR].dirty = false;
  m_entries[host_fpr - FIRST_CACHED_FPR].type = FPRType::ValidPair;

  // LFD host_fpr, PS_OFFSET + guest_reg * 16, REG_PPC_BASE
  m_asm->LFD(host_fpr, m_ppc_base,
             static_cast<s32>(PS_OFFSET + guest_reg * 16));
  return host_fpr;
}

u32 JitPPC64FPRCache::W(u32 guest_reg)
{
  // Check if already cached
  for (u32 i = 0; i < NUM_CACHED_FPRS; ++i)
  {
    if (m_entries[i].guest_reg == guest_reg)
    {
      m_entries[i].dirty = true;
      m_entries[i].type = FPRType::ValidPair;
      return FIRST_CACHED_FPR + i;
    }
  }

  u32 host_fpr = FindFreeHostFPR(guest_reg);
  m_entries[host_fpr - FIRST_CACHED_FPR].guest_reg = guest_reg;
  m_entries[host_fpr - FIRST_CACHED_FPR].dirty = true;
  m_entries[host_fpr - FIRST_CACHED_FPR].type = FPRType::ValidPair;
  return host_fpr;
}

void JitPPC64FPRCache::Flush()
{
  for (u32 i = 0; i < NUM_CACHED_FPRS; ++i)
  {
    if (m_entries[i].dirty && m_entries[i].guest_reg < 32)
    {
      u32 host_fpr = FIRST_CACHED_FPR + i;
      m_asm->STFD(host_fpr, m_ppc_base,
                  static_cast<s32>(PS_OFFSET + m_entries[i].guest_reg * 16));
      m_entries[i].dirty = false;
    }
  }
}

void JitPPC64FPRCache::Flush(const PPCAnalyst::CodeOp* op)
{
  if (!op)
  {
    Flush();
    return;
  }

  for (u32 i = 0; i < NUM_CACHED_FPRS; ++i)
  {
    const u32 guest_reg = m_entries[i].guest_reg;
    if (m_entries[i].dirty && guest_reg < 32)
    {
      // Skip if PPCAnalyst says this register is discardable
      if (op->fprDiscardable[guest_reg])
      {
        m_entries[i].dirty = false;
        continue;
      }

      u32 host_fpr = FIRST_CACHED_FPR + i;
      m_asm->STFD(host_fpr, m_ppc_base,
                  static_cast<s32>(PS_OFFSET + guest_reg * 16));
      m_entries[i].dirty = false;
    }
  }
}

void JitPPC64FPRCache::FlushRegister(u32 guest_reg)
{
  for (u32 i = 0; i < NUM_CACHED_FPRS; ++i)
  {
    if (m_entries[i].guest_reg == guest_reg && m_entries[i].dirty)
    {
      u32 host_fpr = FIRST_CACHED_FPR + i;
      m_asm->STFD(host_fpr, m_ppc_base,
                  static_cast<s32>(PS_OFFSET + guest_reg * 16));
      m_entries[i].dirty = false;
      return;
    }
  }
}

void JitPPC64FPRCache::Reset()
{
  for (auto& e : m_entries)
  {
    e.guest_reg = REG_INVALID;
    e.dirty = false;
    e.type = FPRType::Unknown;
  }
}

u32 JitPPC64FPRCache::FindFreeHostFPR(u32 guest_reg)
{
  // First pass: find an invalid (empty) slot
  for (u32 i = 0; i < NUM_CACHED_FPRS; ++i)
  {
    if (m_entries[i].guest_reg == REG_INVALID)
      return FIRST_CACHED_FPR + i;
  }

  // Second pass: find a non-dirty slot (can discard)
  for (u32 i = 0; i < NUM_CACHED_FPRS; ++i)
  {
    if (!m_entries[i].dirty)
    {
      m_entries[i].guest_reg = guest_reg;
      m_entries[i].type = FPRType::Unknown;
      return FIRST_CACHED_FPR + i;
    }
  }

  // Last resort: evict the first dirty slot (flush it)
  {
    u32 host_fpr = FIRST_CACHED_FPR;
    m_asm->STFD(host_fpr, m_ppc_base,
                static_cast<s32>(PS_OFFSET + m_entries[0].guest_reg * 16));
    m_entries[0].guest_reg = guest_reg;
    m_entries[0].dirty = true;
    m_entries[0].type = FPRType::Unknown;
    return host_fpr;
  }
}
