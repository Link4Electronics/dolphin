#pragma once

#include <array>
#include <unordered_set>
#include <vector>

#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/JitCommon/JitCache.h"
#include "Core/PowerPC/JitCommon/ConstantPropagation.h"
#include "Core/PowerPC/JitPPC64/JitPPC64_RegCache.h"
#include "Core/PowerPC/JitPPC64/PPC64Assembler.h"

// Emit a 64-bit immediate load into a PPC64 assembler.
// Builds upper 32 bits, rotates by 32 (swap halves), builds lower 32 bits.
// This is safe because RLDICL by 32 is exactly half the register: the rotate
// acts as a clean upper/lower swap with no wrap-around corruption.
// NOTE: must NOT use ORI(rd, 0, hw) — logical ops do NOT treat RA=0 as zero.
// Always zero via LI (ADDI ra=0 → zero) then ORI with rd as source.
inline void TrampMOVI64(PPC64Assembler& asm_, u32 rd, u64 imm)
{
  if (imm == 0)
  {
    asm_.LI(rd, 0);   // ADDI rd, 0, 0 = 0 (ADDI treats RA=0 as the value zero)
    return;
  }

  u32 hi = static_cast<u32>(imm >> 32);
  u32 lo = static_cast<u32>(imm & 0xFFFFFFFF);

  asm_.LI(rd, 0);
  if (hi)
  {
    asm_.ORIS(rd, rd, hi >> 16);
    asm_.ORI(rd, rd, hi & 0xFFFF);
  }
  asm_.RLDICL(rd, rd, 32, 0);
  if (lo)
  {
    asm_.ORIS(rd, rd, lo >> 16);
    asm_.ORI(rd, rd, lo & 0xFFFF);
  }
}

// C dispatch function — defined in JitPPC64_BackPatch.cpp
extern "C" const u8* JitPPC64Dispatch(u32 pc);
extern "C" u64 TrampolineDispatcher(PowerPC::PowerPCState* state, u32 ea,
                                    u32 is_store, u32 access_size,
                                    u32 rd, u32 ra, u64 store_value);
extern "C" u64 JitPPC64RefreshTimebase(PowerPC::PowerPCState* state);

// psq_l/st C helpers for integer quantize types (U8/U16/S8/S16).
// Called from JIT-emitted code; takes the MMU pointer for memory access.
extern "C" void JitPPC64PsqLoad(PowerPC::PowerPCState* state, PowerPC::MMU* mmu,
                                u32 ea, u32 gqr_idx, u32 fr, u32 w);
extern "C" void JitPPC64PsqStore(PowerPC::PowerPCState* state, PowerPC::MMU* mmu,
                                 u32 ea, u32 gqr_idx, u32 rs, u32 w);

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
extern u32 RESERVE_OFFSET;
extern u32 RESERVE_ADDR_OFFSET;
extern u32 DOWNCOUNT_OFFSET;
extern u32 SPR_OFFSET;
extern u32 MSR_OFFSET;
extern u32 PS_OFFSET;
extern u32 MEM_PTR_OFFSET;
extern u32 EXCEPTIONS_OFFSET;
extern u32 STACK_PTR_OFFSET;
extern u32 BLR_DEPTH_OFFSET;
extern u32 FPSCR_OFFSET;

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
  void EmitCRCheck(u32 bi, bool cr_true);
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

  // ---- Block linking exits ----
  void WriteExit(u32 destination, bool bl, u32 after);
  void JustWriteExit(u32 destination, bool bl = false, u32 after = 0);
  void WriteExceptionExit(u32 destination);
  void WriteConditionalExceptionExit(int exception);
  void WriteIdleExit();
  void WriteBLRExit();
  void FakeLKExit(u32 after);
  void ResetStack();

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
  void EmitFakeTimeBase();
  void UpdateRoundingMode();

  // ---- CR0 lazy caching helpers ----
  void FlushCR0IfDirty();
  void FlushAll();

  // ---- Carry caching helpers ----
  void FlushCarry();

  // ---- Pre-call flush (gpr+fpr+carry+CR) ----
  void PrepareCall();



public:
  // Debug helpers
  void DumpCode(const u8* start, size_t size);
  void DoBacktrace();

private:

  void FallBackToInterpreter(UGeckoInstruction inst);
  void DoNothing(UGeckoInstruction inst);
  void UnknownInstruction(UGeckoInstruction inst);

  // ---- Members ----
  JitPPC64BlockCache m_block_cache{*this};
  PPC64Assembler m_asm;
  JitPPC64RegCache gpr;
  JitPPC64FPRCache fpr;

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

  // Constant propagation (tracks known GPR values between instructions)
  JitCommon::ConstantPropagation m_constant_propagation;

  // FPR type tracking: tracks the state of each paired-single register.
  // On PPC970, an FPR holds two 32-bit singles packed in a 64-bit double.
  enum class FPRType : u8
  {
    Unknown,      // No tracking info
    Single,       // Both halves valid, distinct values
    Duplicated,   // Both halves identical (result of ps_mr, load-pair, etc.)
    LowerPair,    // Only lower half valid; upper is stale
  };
  FPRType m_fpr_types[32] = {};

  // Reset FPR type tracking at block entry
  void ResetFPRTypes() { for (auto& t : m_fpr_types) t = FPRType::Unknown; }

  // Convert between Gekko paired-single (packed in 64-bit FPR) and scalar single.
  // On PPC970 BE:
  //   FPR = [ps0:upper32, ps1:lower32] in host 64-bit register
  //   STFD stores [ps0, ps1] in BE byte order → LFS from +0 gives ps0, +4 gives ps1
  void ConvertDoubleToSingleLower(u32 fr_idx);
  void ConvertDoubleToSingleUpper(u32 fr_idx);
  // Pack two singles (src_upper → ps0, src_lower → ps1) into one FPR
  void PairSingleToDouble(u32 dst_fpr, u32 src_upper, u32 src_lower);

  // Check if storing an FPR before a C++ call is safe (i.e., the FPR is not
  // in a volatile/caller-saved host FPR). On PPC970 ELFv2, f14-f31 are
  // callee-saved. We use f0 for scratch, so we need to save FPRs guest-regs
  // 0-13 before calling C++ code; f14-f31 are preserved by the callee.
  bool IsFPRStoreSafe(u32 guest_fpr) const;

  // Scoped RAII temp register: allocates REG_SCRATCH or REG_SCRATCH2 on
  // construction and frees on destruction. Useful for intermediate values
  // during code generation without risking register conflicts.
  struct ScopedTempRegister
  {
    explicit ScopedTempRegister(PPC64Assembler& asm_, JitPPC64RegCache& gpr,
                                u32& dirty_mask)
        : m_asm(asm_), m_gpr(gpr), m_dirty_mask(dirty_mask) {}
    ~ScopedTempRegister()
    {
      if (m_allocated)
        Release();
    }
    // Allocate: picks the first free scratch register
    u32 Allocate();
    // Release the allocated register
    void Release();

    u32 reg = 0;
    bool m_allocated = false;

  private:
    PPC64Assembler& m_asm;
    JitPPC64RegCache& m_gpr;
    u32& m_dirty_mask;
  };

  // Carry (CA/XER) tracking within a block.
  // State machine:
  //   m_ca_known=true  → compile-time known value in m_ca_value
  //   m_ca_in_r0=true  → runtime value in REG_SCRATCH (r0), not yet stored
  //   neither          → must LBZ from ppcState on first use
  // m_ca_dirty=true when REG_SCRATCH differs from ppcState.xer_ca.
  bool m_ca_known = false;
  u32 m_ca_value = 0;
  bool m_ca_in_r0 = false;
  bool m_ca_dirty = false;

  // CR0 lazy caching.
  // After an RC-bit CMPWI, host CR0[LT,GT,EQ] is correct; CR0[SO] may be
  // stale (from entry or last flush).  Native BC for CR0 (bi!=3) works
  // without flushing.  mfcr/mtcrf/mcrf/bc_bi=3 require flush first.
  bool m_cr0_native_valid = false;

  // GQR tracking — most games use static GQR values known at compile time.
  // When m_gqr_known[reg] is true, psq_l/st can use direct quantize code.
  bool m_gqr_known[8] = {};
  u32 m_gqr_values[8] = {};

  // Asm routines (pre-generated helper sequences for quantized loads/stores,
  // FRES/FRSQRTE optimization, etc.).
  struct AsmRoutines
  {
    // For psq_l type 0 (float): converts GQR-format float to native single
    // For type 2 (u16): called when inline U16 quantize is too large
    // For type 3 (s16): same
    // These are placeholders; only the most common type (float=0) has inline code.
    const u8* psq_float_load = nullptr;
    const u8* psq_float_store = nullptr;
    const u8* psq_u16_load = nullptr;
    const u8* psq_u16_store = nullptr;
    const u8* psq_s16_load = nullptr;
    const u8* psq_s16_store = nullptr;
  };
  AsmRoutines m_asm_routines;
  void GenerateAsmRoutines();

  // Branch watch (profiling counters)
  // When enabled, each branch site increments a counter.
  // Counters are stored in the trampoline region after the last slow path.
  std::vector<u64*> m_branch_counters;
  u8* m_branch_counters_base = nullptr;
  u32 m_branch_counters_used = 0;
  // Emit a branch counter increment at the current position (if watch enabled)
  void EmitBranchCounter();

  // Branch watch uses IsBranchWatchEnabled() from JitBase.

  // JIT code buffer
  u8* m_code_region = nullptr;
  u8* m_code_pos = nullptr;
  u8* m_code_end = nullptr;

  // enter_code entry point (called from Run(), sets r12 → falls through to dispatcher)
  const u8* m_enter_code = nullptr;

  // Dispatcher entry point (for block exit linking)
  const u8* m_dispatcher_entry = nullptr;

  // Dispatcher lite for block linking fallback; SP = block_SP on entry
  const u8* m_dispatcher_lite = nullptr;

  // Dispatcher exit: restores callee-saved host regs from block frame,
  // tears down the frame, and returns to Run().
  // Called from m_dispatcher_lite when downcount ≤ 0 or block not found.
  const u8* m_dispatcher_exit = nullptr;

public:
  // Register usage in compiled code:
  // r12 = ppcState pointer
  // r11 = scratch / EA computation
  // r0  = scratch
  // r13 = physical base pointer (mem_ptr = host va of guest physical 0) — callee-saved
  // r14-r31 = cached PPC GPRs (via RegCache)
  static constexpr u32 REG_SP = 1;
  static constexpr u32 REG_PPC_BASE = 12;
  static constexpr u32 REG_SCRATCH = 0;
  static constexpr u32 REG_SCRATCH2 = 11;
  static constexpr u32 REG_PHYS_BASE = 13;

private:

  // Stack frame layout offsets (from block SP)
  static constexpr s32 R2_SAVE_OFFSET = 8;          // saved r2 (TOC) — free slot
  static constexpr s32 CALLEE_SAVE_BASE = 32;     // r14-r31 saves start here
  // NOTE: order matters — do NOT rearrange these without updating the block
  // frame layout in Jit.cpp.  r10 saves at 176; EA/r13/PSQ follow; FPRs after.
  static constexpr s32 EA_SAVE_OFFSET = 184;       // saved guest EA for backpatch
  static constexpr s32 PSQ_EA_SAVE_OFFSET = 192;   // saved EA for psq integer helper call
  static constexpr s32 PHYS_BASE_SAVE_OFFSET = 200; // saved r13 (phys base)
};
