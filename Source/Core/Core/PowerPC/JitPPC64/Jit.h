#pragma once

#include <array>
#include <map>
#include <unordered_set>
#include <vector>

#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/JitCommon/JitCache.h"
#include "Core/PowerPC/JitPPC64/JitPPC64_RegCache.h"
#include "Core/PowerPC/JitPPC64/PPC64Assembler.h"

// Emit a 64-bit immediate load into a PPC64 assembler.
// Used by EmitBackpatchRoutine and CompileMFTB to load function addresses.
inline void TrampMOVI64(PPC64Assembler& asm_, u32 rd, u64 imm)
{
  const auto h4 = static_cast<s32>((imm >> 48) & 0xFFFF);
  const auto h3 = static_cast<u32>((imm >> 32) & 0xFFFF);
  const auto h2 = static_cast<u32>((imm >> 16) & 0xFFFF);
  const auto lo = static_cast<u32>(imm & 0xFFFF);
  asm_.ADDIS(rd, 0, h4);
  asm_.RLDICL(rd, rd, 0, 32);
  asm_.ORI(rd, rd, h3);
  asm_.RLDICR(rd, rd, 32, 31);
  asm_.ORIS(rd, rd, h2);
  asm_.ORI(rd, rd, lo);
}

// Forward declaration — defined in JitPPC64_Tables.cpp
bool CanCompileInstruction(UGeckoInstruction inst);

// C dispatch function — defined in JitPPC64_BackPatch.cpp
extern "C" const u8* JitPPC64Dispatch(u32 pc);
extern "C" u64 TrampolineDispatcher(PowerPC::PowerPCState* state, u32 ea,
                                    u32 is_store, u32 access_size,
                                    u32 rd, u32 ra, u64 store_value);
extern "C" u64 JitPPC64RefreshTimebase(PowerPC::PowerPCState* state);

// Global JIT instance pointer (set during Init, used by asm dispatcher + signal handler)
class JitPPC64;
extern JitPPC64* g_jit_ppc64_instance;

// State offsets computed at runtime via InitOffsets() — avoids
// -Winvalid-offsetof on GCC (PowerPCState is non-standard-layout).
extern u32 PC_OFFSET;
extern u32 GPR_OFFSET;
extern u32 CR_OFFSET;
extern u32 XER_CA_OFFSET;
extern u32 XER_SO_OV_OFFSET;
extern u32 DOWNCOUNT_OFFSET;
extern u32 SPR_OFFSET;
extern u32 MSR_OFFSET;
extern u32 PS_OFFSET;

class JitPPC64BlockCache : public JitBaseBlockCache
{
public:
  explicit JitPPC64BlockCache(JitBase& jit) : JitBaseBlockCache(jit) {}
private:
  void WriteLinkBlock(const JitBlock::LinkData& source, const JitBlock* dest) override;
  void WriteDestroyBlock(const JitBlock& block) override;
};

class JitPPC64 : public JitBase
{
public:
  explicit JitPPC64(Core::System& system);
  ~JitPPC64() override;

  const char* GetName() const override { return "JITPPC64"; }

  void Init() override;
  void Shutdown() override;
  void ClearCache() override;
  void Run() override;
  void SingleStep() override;

  JitBaseBlockCache* GetBlockCache() override { return &m_block_cache; }
  void Jit(u32 em_address) override;
  void EraseSingleBlock(const JitBlock& block) override;
  std::vector<MemoryStats> GetMemoryStats() const override;
  std::size_t DisassembleNearCode(const JitBlock& block, std::ostream& stream) const override;
  std::size_t DisassembleFarCode(const JitBlock& block, std::ostream& stream) const override;
  const CommonAsmRoutinesBase* GetAsmRoutines() override { return nullptr; }
  bool HandleFault(uintptr_t access_address, SContext* ctx) override;

private:
  friend struct JitPPC64RegCache;
  friend class JitPPC64BlockCache;
  friend const u8* JitPPC64Dispatch(u32);

  // Internal Jit() with retry parameter
  void Jit(u32 em_address, bool clear_cache_and_retry_on_failure);

  // Prolog/epilog generation
  void EmitProlog();
  void EmitEpilog(u32 next_pc);
  void EmitBackpatchSlot();

  // Compile the dispatcher trampoline
  void CompileDispatcher();

  // Guest instruction dispatching
  void CompileInstruction(PPCAnalyst::CodeOp& op);

  // ---- Integer ALU (JitPPC64_Integer.cpp) ----
  bool CompileADDI(UGeckoInstruction inst);
  bool CompileADDIS(UGeckoInstruction inst);
  bool CompileADDIC(UGeckoInstruction inst);
  bool CompileADDIC_(UGeckoInstruction inst);
  bool CompileMULLI(UGeckoInstruction inst);
  bool CompileANDI_(UGeckoInstruction inst);
  bool CompileANDIS_(UGeckoInstruction inst);
  bool CompileORI(UGeckoInstruction inst);
  bool CompileORIS(UGeckoInstruction inst);
  bool CompileXORI(UGeckoInstruction inst);
  bool CompileXORIS(UGeckoInstruction inst);
  bool CompileCMPI(UGeckoInstruction inst);
  bool CompileCMPLI(UGeckoInstruction inst);

  // Table 31 dispatcher (Integer.cpp: calls Integer + SystemReg)
  bool CompileTable31(UGeckoInstruction inst);
  // Integer subset of Table 31 (Integer.cpp)
  bool CompileTable31_Integer(UGeckoInstruction inst);

  // ---- Load/Store (JitPPC64_LoadStore.cpp) ----
  bool CompileLoadStore(UGeckoInstruction inst);
  bool CompileLMW(UGeckoInstruction inst);
  bool CompileSTMW(UGeckoInstruction inst);

  // ---- Branch (JitPPC64_Branch.cpp) ----
  bool CompileB(UGeckoInstruction inst);
  bool CompileBC(UGeckoInstruction inst);

  // ---- System registers (JitPPC64_SystemRegisters.cpp) ----
  bool CompileMFCR(UGeckoInstruction inst);
  bool CompileMTCRF(UGeckoInstruction inst);
  bool CompileMFSPR(UGeckoInstruction inst);
  bool CompileMTSPR(UGeckoInstruction inst);
  bool CompileMFMSR(UGeckoInstruction inst);
  bool CompileMTMSR(UGeckoInstruction inst);
  bool CompileMFTB(UGeckoInstruction inst);
  bool CompileTW(UGeckoInstruction inst);
  bool CompileTable31_SystemReg(UGeckoInstruction inst);

  // ---- Rotate/Mask (JitPPC64_Rotate.cpp) ----
  bool CompileRLWINM(UGeckoInstruction inst);
  bool CompileRLWIMI(UGeckoInstruction inst);
  bool CompileRLWNM(UGeckoInstruction inst);

  // ---- Misc Integer (JitPPC64_Integer.cpp) ----
  bool CompileSubfic(UGeckoInstruction inst);
  bool CompileTWI(UGeckoInstruction inst);

  // ---- opcd 19 dispatcher (JitPPC64_Branch.cpp) ----
  bool CompileOPCD19(UGeckoInstruction inst);
  bool CompileBCLR(UGeckoInstruction inst);
  bool CompileBCCTR(UGeckoInstruction inst);
  bool CompileCRLogical(UGeckoInstruction inst);
  bool CompileMCRF(UGeckoInstruction inst);

  // ---- FPU (JitPPC64_FPU.cpp) ----
  bool CompileFPUSingle(UGeckoInstruction inst);
  bool CompileFPUDouble(UGeckoInstruction inst);

  // ---- Misc/opcd 17/19 (JitPPC64_SystemRegisters.cpp) ----
  bool CompileSC(UGeckoInstruction inst);
  bool CompileRFI(UGeckoInstruction inst);
  bool CompileISYNC(UGeckoInstruction inst);
  bool CompileMisc(UGeckoInstruction inst);

  // ---- Indexed Load/Store (JitPPC64_LoadStore.cpp) ----
  bool CompileTable31_LoadStore(UGeckoInstruction inst);

  // ---- CA-using ops (JitPPC64_Integer.cpp) ----
  bool CompileTable31_CA(UGeckoInstruction inst);

  // ---- Paired Singles (JitPPC64_Paired.cpp) ----
  bool CompilePairedSingle(UGeckoInstruction inst);
  bool CompilePairedLoadStore(UGeckoInstruction inst);

  // ---- Fastmem / Backpatch (JitPPC64_BackPatch.cpp) ----
  struct FastmemArea
  {
    const u8* fast_access_code;  // start of the fast path range
    const u8* slow_access_code;  // entry of the slow path in trampoline region
    bool is_load;                // true for loads, false for stores
    u32 rd;                      // PPC dest/src register field
    u32 ra;                      // PPC base register (for update forms)
  };
  void InitBackpatch();
  void ShutdownBackpatch();
  void AddBackpatchEntry(const u8* code_addr, u32 guest_pc, u32 guest_address,
                          u32 original_inst, u32 rd);

  // Emit both fast and slow paths for a load/store instruction.
  // slow_path is emitted in the trampoline region; on fault the fast path
  // is patched with a branch to it.
  void EmitBackpatchRoutine(u32 access_size, u32 opcd, u32 rd, u32 ra, u32 data_reg,
                              bool is_load, bool is_fpr = false);

  // ---- Helpers (Jit.cpp) ----
  void LoadGPR(u32 host_reg, u32 guest_reg);
  void StoreGPR(u32 guest_reg, u32 host_reg);
  void LoadCR(u32 host_reg);
  void StoreCR(u32 host_reg);
  void EmitCR0Update(u32 host_reg);
  void EmitCarryFromReg();

  void FallBackToInterpreter(UGeckoInstruction inst);
  void DoNothing(UGeckoInstruction inst);
  void UnknownInstruction(UGeckoInstruction inst);

  // ---- Members ----
  JitPPC64BlockCache m_block_cache{*this};
  PPC64Assembler m_asm;
  JitPPC64RegCache gpr;

  // Trampoline code region (adjacent to main code)
  u8* m_tramp_region = nullptr;
  u8* m_tramp_pos = nullptr;
  u8* m_tramp_end = nullptr;
  PPC64Assembler m_tramp_asm;

  // Fastmem fault → slow handler map (keyed by fast_access_end)
  // Stores the mapping from fast path range to slow path entry.
  // upper_bound(fault_pc) finds the range, then check pc >= fast_access_code.
  std::map<const u8*, FastmemArea> m_fault_to_handler;

  // PCs that failed JIT compilation — skip retries to avoid log spam.
  std::unordered_set<u32> m_failed_pcs;

  // Per-block JIT state
  u32 m_block_start = 0;
  u32 m_block_end = 0;
  bool m_is_in_block = false;

  // JIT code buffer
  u8* m_code_region = nullptr;
  u8* m_code_pos = nullptr;
  u8* m_code_end = nullptr;

  // enter_code entry point (called from Run(), sets r12 → falls through to dispatcher)
  const u8* m_enter_code = nullptr;

  // Dispatcher entry point (for block exit linking)
  const u8* m_dispatcher_entry = nullptr;

  // Shared exit sequence: restores host regs, tears down frame, BLR to Run().
  // Both the epilog and branch compilers branch here instead of inline BLR,
  // ensuring r10/r14-r31 are always restored and the frame is always torn down.
  const u8* m_exit_sequence = nullptr;

  // Register usage in compiled code:
  // r12 = ppcState pointer
  // r11 = scratch
  // r0  = scratch
  // r14-r31 = cached PPC GPRs (via RegCache)
  static constexpr u32 REG_PPC_BASE = 12;
  static constexpr u32 REG_SCRATCH = 0;
  static constexpr u32 REG_SCRATCH2 = 11;
};
