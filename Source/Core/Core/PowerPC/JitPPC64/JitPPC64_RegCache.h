#pragma once

#include "Common/CommonTypes.h"
#include "Core/PowerPC/JitPPC64/PPC64Assembler.h"

class JitPPC64;

struct JitPPC64RegCache
{
  static constexpr u32 NUM_CACHED_REGS = 18;
  static constexpr u32 FIRST_CACHED_REG = 14;
  static constexpr u32 REG_INVALID = 32;

  struct Entry
  {
    u32 ppc_reg = REG_INVALID;
    bool dirty = false;
  };

  Entry m_entries[NUM_CACHED_REGS];
  JitPPC64* m_jit = nullptr;
  PPC64Assembler* m_asm = nullptr;
  u32 m_ppc_base = 12;

  JitPPC64RegCache() = default;

  void SetJit(JitPPC64& jit, PPC64Assembler& asm_, u32 ppc_base_reg)
  {
    m_jit = &jit;
    m_asm = &asm_;
    m_ppc_base = ppc_base_reg;
  }

  // Load a PPC GPR into a host register, returns host register number
  u32 R(u32 ppc_reg);

  // Allocate a host register for writing a PPC GPR value
  u32 W(u32 ppc_reg);

  // Flush all dirty registers back to ppcState
  void Flush();

  // Flush a single dirty register back to ppcState (no-op if not cached or not dirty)
  void FlushRegister(u32 ppc_reg);

  // Mark all entries invalid (block entry)
  void Reset();

private:
  // Find a host register for eviction, flush if dirty
  u32 FindFreeHostReg(u32 ppc_reg);
};
