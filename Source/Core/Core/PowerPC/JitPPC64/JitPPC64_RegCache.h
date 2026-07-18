#pragma once

#include "Common/CommonTypes.h"
#include "Core/PowerPC/JitPPC64/PPC64Assembler.h"

namespace PPCAnalyst {
struct CodeOp;
}  // namespace PPCAnalyst

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

  // Track known-constant GPR values for constant propagation integration.
  // m_known_val[ppc_reg] is valid when m_known_mask has bit ppc_reg set.
  u32 m_known_val[32]{};
  u32 m_known_mask = 0;

  JitPPC64RegCache() = default;

  void SetJit(JitPPC64& jit, PPC64Assembler& asm_, u32 ppc_base_reg)
  {
    m_jit = &jit;
    m_asm = &asm_;
    m_ppc_base = ppc_base_reg;
  }

  // Query constant propagation for a known value
  bool HasGPR(u32 ppc_reg) const;
  u32 GetGPR(u32 ppc_reg) const;

  // Mark a GPR as having a known immediate value (constant propagation result)
  void SetImmediate32(u32 ppc_reg, u32 val);

  // Load a PPC GPR into a host register, returns host register number.
  // Uses LI for known-constant values instead of LWZ from ppcState.
  u32 R(u32 ppc_reg);

  // Allocate a host register for writing a PPC GPR value
  u32 W(u32 ppc_reg);

  // Flush all dirty registers back to ppcState
  void Flush();

  // Flush with discardable guidance — skips registers marked discardable by PPCAnalyst
  void Flush(const PPCAnalyst::CodeOp* op);

  // Flush a single dirty register back to ppcState (no-op if not cached or not dirty)
  void FlushRegister(u32 ppc_reg);

  // Mark all entries invalid (block entry)
  void Reset();

  // Invalidate known-constant tracking for a register
  void InvalidateKnown(u32 ppc_reg) { m_known_mask &= ~(1u << ppc_reg); }

private:
  // Find a host register for eviction, flush if dirty
  u32 FindFreeHostReg(u32 ppc_reg);
};

// FPR cache: caches 32 guest FPRs (each holding a paired-single as
// [ps0:upper32, ps1:lower32] in a 64-bit host FPR) in host FPRs f14-f31.
//
// On PPC970 ELFv2, f14-f31 are callee-saved — preserved across C++ calls.
// We load/store full 64-bit values via LFD/STFD.
//
// FPR type tracking uses the same FPRType enum from Jit.h to track
// whether upper/lower halves are valid or duplicated.
struct JitPPC64FPRCache
{
  static constexpr u32 NUM_CACHED_FPRS = 18;
  static constexpr u32 FIRST_CACHED_FPR = 14;
  static constexpr u32 REG_INVALID = 32;

  struct FPRType
  {
    // On PPC970, a guest FPR holds ps0 (upper 32 bits) + ps1 (lower 32 bits).
    // The type tracks what's valid without re-loading from ppcState.
    enum Type : u8
    {
      Unknown,      // No tracking info — must load both halves
      ValidPair,    // Both ps0 and ps1 are valid (loaded or written)
      Duplicated,   // ps0 == ps1 (result of ps_mr, single-load, etc.)
      LowerOnly,    // Only ps0 valid; ps1 is stale
    };
  };

  struct Entry
  {
    u32 guest_reg = REG_INVALID;
    bool dirty = false;
    FPRType::Type type = FPRType::Unknown;
  };

  Entry m_entries[NUM_CACHED_FPRS];
  JitPPC64* m_jit = nullptr;
  PPC64Assembler* m_asm = nullptr;
  u32 m_ppc_base = 12;

  JitPPC64FPRCache() = default;

  void SetJit(JitPPC64& jit, PPC64Assembler& asm_, u32 ppc_base_reg)
  {
    m_jit = &jit;
    m_asm = &asm_;
    m_ppc_base = ppc_base_reg;
  }

  // Load a guest FPR into a host FPR, returns host FPR number (14-31).
  // Loads 64-bit from ppcState.ps[guest_reg] via LFD.
  u32 R(u32 guest_reg);

  // Allocate a host FPR for writing, returns host FPR number (14-31).
  // Marks the entry dirty for later Flush().
  u32 W(u32 guest_reg);

  // Write all dirty FPRs back to ppcState via STFD.
  void Flush();

  // Flush with discardable guidance from PPCAnalyst.
  void Flush(const PPCAnalyst::CodeOp* op);

  // Flush a single dirty FPR back to ppcState (no-op if not cached or clean).
  void FlushRegister(u32 guest_reg);

  // Mark all entries invalid (called at block entry).
  void Reset();

private:
  u32 FindFreeHostFPR(u32 guest_reg);
};
