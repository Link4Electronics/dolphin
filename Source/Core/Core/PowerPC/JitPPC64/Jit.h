// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <csignal>
#include <ctime>
#include <unordered_map>
#include <vector>

#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/JitCommon/JitCache.h"

class JitPPC64BlockCache : public JitBaseBlockCache
{
public:
  explicit JitPPC64BlockCache(JitBase& jit) : JitBaseBlockCache(jit) {}

private:
  void WriteLinkBlock(const JitBlock::LinkData& source, const JitBlock* dest) override {}
  void WriteDestroyBlock(const JitBlock& block) override {}
};

// Guest register state passed to the assembly trampoline for restoration.
// All fields are the 32-bit Gekko register values (upper 32 bits on PPC64 are zero).
struct GuestRegs
{
  u32 gpr[32];  // 0..127
  u32 pc;       // 128
  u32 cr;       // 132
  u32 lr;       // 136
  u32 ctr;      // 140
  u32 xer;      // 144
};

// Host context saved by the trampoline before jumping to guest code.
// Signal handlers use this to restore the host environment when
// redirecting back to the Run() loop.
struct alignas(16) NativeContext
{
  u64 host_r2;          // 0   TOC pointer
  u64 host_r13;         // 8   TLS pointer
  u64 host_r1;          // 16  stack pointer
  u64 return_addr;      // 24  where to continue in Run()
  u64 host_cr;          // 32  condition register (all 8 fields)
  u64 host_gpr14_31[18]; // 40..176  callee-saved GPRs
  u32 guest_ctr;        // 184 saved guest CTR (before trampoline overwrites it)
  u32 overwrite_ctr;    // 188 the value the trampoline overwrote CTR with (guest PC)
  u32 padding;          // 192 pad to 8-byte boundary
};
// Total: 196 bytes

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
  bool HandleFault(uintptr_t access_address, SContext* ctx) override { return false; }

private:
  // Guest CPU state (SPRs that can't run natively)
  struct PPC64CPUGuestState
  {
    u32 msr = 0;
    u32 srr0 = 0;
    u32 srr1 = 0;
    u32 hid0 = 0;
    u32 hid1 = 0;
    u32 hid2 = 0;
    u32 hid4 = 0;
    u32 l2cr = 0;
    u32 mmcr0 = 0;
    u32 mmcr1 = 0;
    u32 sprg[4] = {};
    u32 ibatu[8] = {};
    u32 ibatl[8] = {};
    u32 dbatu[8] = {};
    u32 dbatl[8] = {};
    u32 segment_regs[16] = {};
    u32 dsisr = 0;
    u32 dar = 0;
    u32 decrementer = 0;
    u32 tbl = 0;
    u32 tbu = 0;
  };

  void FallBackToInterpreter(UGeckoInstruction inst);
  void DoNothing(UGeckoInstruction inst);
  void UnknownInstruction(UGeckoInstruction inst);

  void InstallSignalHandlers();
  void RemoveSignalHandlers();

public:
  void HandleSIGSEGV(int sig, siginfo_t* info, void* uctx);
  void HandleSIGILL(int sig, siginfo_t* info, void* uctx);
  void HandleSIGALRM(int sig, siginfo_t* info, void* uctx);
  void HandleSIGTRAP(int sig, siginfo_t* info, void* uctx);

private:

  // Save guest register state from ucontext to ppcState (exit native mode)
  void SaveGuestRegsFromContext(void* uctx);
  // Restore host callee-saved registers in ucontext (before sigreturn to Run)
  void RestoreHostRegsInContext(void* uctx);
  // Copy FPRs from real PPC970 (ucontext) into m_ppc_state.ps[] — needed before
  // FallBackToInterpreter for psq_l/st or ps_* arithmetic, because native
  // execution left the real values in the PPC970 FPRs, not in m_ppc_state.
  void SaveFPRsFromContext(void* uctx);
  // Copy a single modified FPR back from m_ppc_state.ps[] to the ucontext
  // after the interpreter wrote a new value there (psq_l destination, ps_* FD).
  void RestoreFPRToContext(void* uctx, u32 fpr_index);
  // Copy ppcState GPRs/PC/CR/etc. to a GuestRegs struct for the trampoline
  void FillGuestRegsForEntry(GuestRegs& regs);

  // Sync m_guest SPR state from ppc_state before first native execution
  void SyncGuestState();

  // Setup a periodic timer for downcount checks
  void StartNativeTimer();
  void StopNativeTimer();
  static u64 EstimateDowncount();

  // Make guest memory executable for native execution
  void MakeGuestMemoryExecutable();
  void MakeGuestMemoryNonExecutable();

  // SPR emulation (called from SIGILL handler)
  u32 EmulateMFSpr(u32 spr);
  void EmulateMTSpr(u32 spr, u32 val);
  bool ExitNCEFromSignal(void* uctx, u32 pc_val, bool skip_instruction);
  void EmulateDSI(void* uctx);
  void EmulateISI(void* uctx);
  u32 EmulateRFI();
  void EmulateMTMSR(u32 val);
  u32 EmulateMFMSR();

  // Paired Singles emulation (called from SIGILL handler)
  void EmulatePairedSingle(u32 instr, void* uctx);

  // Slowmem fallback for non-MMIO data faults (called from SIGSEGV handler)
  bool SlowmemDataAccess(void* uctx, u32 instr, u32 pc_val, u32 fault_addr, u32 dsisr_val);

  // MMIO dispatch (called from SIGSEGV handler)
  u32 MMIORead(u32 addr, int width);
  void MMIOWrite(u32 addr, u32 val, int width);

  // dcbz → trap patching (trap-and-emulate for PPC970 128-byte dcbz fix)
  void ScanDCBZ();
  void PatchAllDCBZ();
  void UnpatchAllDCBZ();

  // P0 → trap patching (trap-and-emulate for silently-wrong PPC970 instrs)
  void ScanP0();
  void PatchAllP0();
  void UnpatchAllP0();

  JitPPC64BlockCache m_block_cache{*this};

  // NCE state
  PPC64CPUGuestState m_guest{};
  NativeContext m_native_ctx{};
  bool m_signals_installed = false;
  bool m_memory_executable = false;
  bool m_nce_mapping_done = false;
  stack_t m_alt_stack{};
  struct sigaction m_old_sigsegv{};
  struct sigaction m_old_sigill{};
  struct sigaction m_old_sigbus{};
  struct sigaction m_old_sigalrm{};
  struct sigaction m_old_sigtrap{};
  timer_t m_native_timer{};
  u64 m_host_sda[2] = {};

  // Estimated cycles consumed per native execution block
  static constexpr u32 CYCLES_PER_BLOCK = 128;

  // dcbz → trap patch state
  std::unordered_map<u32, u32> m_patched_dcbz;  // NCE addr → original instruction
  bool m_dcbz_patches_applied = false;
  bool m_dcbz_needs_rescan = false;

  // P0 → trap patch state
  std::unordered_map<u32, u32> m_patched_p0;
  bool m_p0_patches_applied = false;

  // DCR (Device Control Register) bus state — written by mtdcr, read by mfdcr
  // Initialized in constructor to 0xFFFFFFFF (all bits set = ready).
  static constexpr u32 DCR_INIT = 0xFFFFFFFFu;
  std::array<u32, 1024> m_dcr;

  // ps_* → AltiVec trampoline state (fast path — avoids SIGILL round-trip)
  static constexpr u32 PS_ALTIVEC_BASE = 0x81F00000u;
  static constexpr u32 PS_ALTIVEC_SIZE = 0x00080000u;  // 512 KB (8192 trampolines × 64 B)
  static constexpr u32 PS_TRAMP_STRIDE = 64;            // 16 instructions per trampoline
  static constexpr u32 PS_SCRATCH_ADDR = 0x81FFF000u;   // scratch space (16-byte aligned, K1)

  std::unordered_map<u32, u32> m_ps_trampoline_map;  // guest_addr → trampoline_addr
  u32 m_ps_trampoline_next = PS_ALTIVEC_BASE;

  void GeneratePsTrampolines();
  static bool IsPsArithInstruction(u32 instr);

public:
  // Pointer to the active instance (for signal handlers / asm bridge fcns)
  static thread_local JitPPC64* s_active_instance;
};

// Assembly trampoline (defined in JitAsm.S)
extern "C" void JitPPC64EnterGuest(const GuestRegs* regs, NativeContext* ctx);
extern "C" void JitPPC64EnterGuest_end(); // end label for size calculation
