// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/JitPPC64/Jit.h"

#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"
#include "Common/Thread.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/MachineContext.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SystemTimers.h"
#include "Core/ConfigManager.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/PPCAnalyst.h"
#include "Core/PowerPC/PPCTables.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

// TrampolineDispatcher — defined in JitPPC64_BackPatch.cpp
extern "C" u64 TrampolineDispatcher(PowerPC::PowerPCState* state, u32 ea,
                                     u32 is_store, u32 access_size,
                                     u32 rd, u32 ra, u64 store_value);

// Real TLS pointer captured at JIT init.
// On ELFv2 PPC64, r13 = thread pointer (TLS).  JIT code overwrites r13
// with mem_ptr.  The SIGSEGV handler must restore real TLS before calling
// any C++ code that might access thread-local storage (errno, logging, etc.).
static u64 s_real_tls = 0;

// PPCState field offsets (computed at init from actual struct layout)
u32 PC_OFFSET = 0;
u32 GPR_OFFSET = 0;
u32 CR_OFFSET = 0;
u32 XER_CA_OFFSET = 0;
u32 XER_SO_OV_OFFSET = 0;
u32 RESERVE_OFFSET = 0;
u32 RESERVE_ADDR_OFFSET = 0;
u32 DOWNCOUNT_OFFSET = 0;
u32 SPR_OFFSET = 0;
u32 MSR_OFFSET = 0;
u32 PS_OFFSET = 0;
u32 MEM_PTR_OFFSET = 0;
u32 EXCEPTIONS_OFFSET = 0;
u32 STACK_PTR_OFFSET = 0;
u32 BLR_DEPTH_OFFSET = 0;
u32 FPSCR_OFFSET = 0;

// Signal handler for MMIO backpatching
JitPPC64* g_jit_ppc64_instance = nullptr;

// Set by dispatcher just before MTCTR(3)/BCTR — SIGSEGV handler uses this
// to compare the intended jump target against the actual fault address.
const u8* volatile g_last_block_entry = nullptr;
volatile u32 g_probe_pc_1 = 0;
volatile u32 g_probe_pc_2 = 0;
volatile u32 g_probe_pc_3 = 0;

// Fast-path store probes: written by generated JIT code (EmitStoreProbe)
// right before every store in the fastmem arena.  Used to diagnose heap
// corruption from a wrong-but-valid EA (e.g. 0x3FF0004F masks into the
// backed-RAM arena window).  After a crash/hang, read these to find the
// last store executed and where it was translated to.
volatile u64 g_stm_last_ea = 0;       // last probed store: guest EA
volatile u32 g_stm_ea12 = 0;          // (EA & 0xFFF) << 12 — page offset in high bits
volatile u64 g_stm_last_host = 0;     // host address the store was translated to
volatile u32 g_probe_block_addr = 0;  // start address of the JIT block doing the store

static struct sigaction s_old_sigsegv;

// code_region address for signal handler debug output
static const u8* s_code_region = nullptr;

static void InitOffsets(const PowerPC::PowerPCState& state)
{
  const auto base = reinterpret_cast<const char*>(&state);
  PC_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.pc) - base);
  GPR_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.gpr) - base);
  CR_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.cr) - base);
  XER_CA_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.xer_ca) - base);
  XER_SO_OV_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.xer_so_ov) - base);
  RESERVE_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.reserve) - base);
  RESERVE_ADDR_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.reserve_address) - base);
  DOWNCOUNT_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.downcount) - base);
  SPR_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.spr) - base);
  MSR_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.msr) - base);
  PS_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.ps) - base);
  MEM_PTR_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.mem_ptr) - base);
  EXCEPTIONS_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.Exceptions) - base);
  STACK_PTR_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.stored_stack_pointer) - base);
  BLR_DEPTH_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.blr_stack_depth) - base);
  FPSCR_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.fpscr.Hex) - base);
}

// Combined JIT code memory layout:
// [-- 32 MB main JIT code --][-- 4 MB trampoline area --]
//
// PPC `b` instruction can reach ±32 MB.  The trampoline page is at
// code_region + JIT_CODE_SIZE.  The distance from a fast-path instruction
// near the start of the code area to the end of the trampoline area must
// be < 32 MB.  JIT_CODE_SIZE = 16 MB gives 12 MB of margin.
static constexpr u32 JIT_CODE_SIZE = 16 * 1024 * 1024;
static constexpr u32 TRAMP_CODE_SIZE = 4 * 1024 * 1024;
static constexpr u32 COMBINED_SIZE = JIT_CODE_SIZE + TRAMP_CODE_SIZE;

static u8* AllocateCodeRegion(size_t size)
{
  void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (ptr == MAP_FAILED)
  {
    ERROR_LOG_FMT(POWERPC, "JITPPC64: failed to allocate {} bytes of RWX memory", size);
    return nullptr;
  }
  return static_cast<u8*>(ptr);
}

static void FreeCodeRegion(u8* ptr, size_t size)
{
  if (ptr)
    munmap(ptr, size);
}



// Stack frame for compiled blocks (ELFv2 PPC64 ABI)
// Stack frame layout (384 bytes):
//   0(r1)  : backchain
//   8(r1)  : saved CR (optional)
//  16(r1)  : LR save (caller's frame)
//  24(r1)  : TOC / r12 save (ppcState)
//  32(r1)..176(r1): callee-saved r14-r31 (18 × 8 = 144 bytes)
// 176(r1)      : saved r10 (clobbered by CompileBC as not-taken flag)
// 184(r1)      : saved guest EA for backpatch (EA_SAVE_OFFSET)
// 192(r1)      : PSQ EA save (PSQ_EA_SAVE_OFFSET)
// 200(r1)      : saved mem_ptr in r13 (PHYS_BASE_SAVE_OFFSET) — set after prolog overwrites r13
// 208(r1)..352(r1): callee-saved FPRs f14-f31 (18 × 8 = 144 bytes)
// 352(r1)      : saved real TLS (TLS_SAVE_OFFSET) — saved before prolog overwrites r13
static constexpr u32 FRAME_SIZE = 384;
static constexpr s32 CALLEE_SAVE_FPR_BASE = 208;

// ===========================================================================
// JitPPC64BlockCache
// ===========================================================================

void JitPPC64BlockCache::WriteLinkBlock(const JitBlock::LinkData& source, const JitBlock* dest)
{
  const Common::ScopedJITPageWriteAndNoExecute enable_jit_page_writes;
  u8* location = source.exitPtrs;

  if (dest)
  {
    s64 distance = static_cast<s64>(dest->normalEntry - location);
    if (distance >= -0x2000000LL && distance <= 0x1FFFFFFLL)
    {
      u32 li = (static_cast<u32>(distance >> 2)) & 0x00FFFFFF;
      u32 branch = (18u << 26) | (li << 2) | (source.call ? 1u : 0u);
      std::memcpy(location, &branch, sizeof(branch));
      __builtin___clear_cache(location, location + 4);
      return;
    }
  }

  // Fall back to dispatcher_lite
  auto* jit = static_cast<JitPPC64*>(&m_jit);
  if (jit->m_dispatcher_lite)
  {
    s64 dist = static_cast<s64>(jit->m_dispatcher_lite - location);
    if (dist >= -0x2000000LL && dist <= 0x1FFFFFFLL)
    {
      u32 li = (static_cast<u32>(dist >> 2)) & 0x00FFFFFF;
      u32 branch = (18u << 26) | (li << 2);
      std::memcpy(location, &branch, sizeof(branch));
      __builtin___clear_cache(location, location + 4);
      return;
    }
  }
  u32 blr = 0x4E800020;
  std::memcpy(location, &blr, sizeof(blr));
  __builtin___clear_cache(location, location + 4);
}

void JitPPC64BlockCache::WriteDestroyBlock(const JitBlock& block)
{
  const Common::ScopedJITPageWriteAndNoExecute enable_jit_page_writes;
  u32 trap = 0x7FE00008;
  std::memcpy(const_cast<u8*>(block.normalEntry), &trap, sizeof(trap));
}

// ===========================================================================
// JitPPC64
// ===========================================================================

JitPPC64::JitPPC64(Core::System& system) : JitBase(system) {}
JitPPC64::~JitPPC64() { Shutdown(); }

static void SIGSEGVHandler(int sig, siginfo_t* info, void* ucontext_arg)
{
  // On ELFv2 PPC64, r13 = TLS pointer.  JIT code overwrites r13 with
  // mem_ptr, so when a fast-path SIGSEGV fires, r13 = mem_ptr, not TLS.
  // Any C++ code in this handler that accesses thread-local storage
  // (errno via logging, fmt internals, etc.) will crash.
  //
  // Fix: save fault r13 (= mem_ptr), swap to real TLS atomically so no
  // C++ code runs with the wrong r13.  Restore mem_ptr to the ucontext
  // before returning (the kernel's sigreturn will restore mem_ptr into
  // r13 for the JIT code).
  u64 fault_r13;
  if (s_real_tls)
    asm volatile("mr %0, 13\n\t"
                 "mr 13, %1"
                 : "=r"(fault_r13)
                 : "r"(s_real_tls)
                 : "13");
  else
    asm volatile("mr %0, 13" : "=r"(fault_r13));

  auto* uc = static_cast<ucontext_t*>(ucontext_arg);
  auto* ctx = &uc->uc_mcontext;

  uintptr_t access_addr = reinterpret_cast<uintptr_t>(info->si_addr);

  if (g_jit_ppc64_instance && g_jit_ppc64_instance->HandleFault(access_addr, ctx))
    goto done;

  if (g_jit_ppc64_instance)
  {
    if (ctx->CTX_NIP)
      g_jit_ppc64_instance->DumpCode(reinterpret_cast<const u8*>(ctx->CTX_NIP) - 16, 48);
    fprintf(stderr, "JIT: g_last_block_entry = %p (intended jump target)\n",
            reinterpret_cast<const void*>(g_last_block_entry));
    fprintf(stderr, "JIT: fault NIP=0x%08llX addr=0x%08llX r1=0x%08llX\n",
            static_cast<unsigned long long>(ctx->CTX_NIP),
            static_cast<unsigned long long>(access_addr),
            static_cast<unsigned long long>(ctx->regs->gpr[1]));
    fprintf(stderr,
            "JIT: last store probe: EA=0x%08llX ea12=0x%08X host=0x%08llX block=0x%08X\n",
            static_cast<unsigned long long>(g_stm_last_ea), g_stm_ea12,
            static_cast<unsigned long long>(g_stm_last_host), g_probe_block_addr);
    g_jit_ppc64_instance->DoBacktrace();
  }

  // Not a JIT MMIO fault — restore default handler and re-raise.
  {
    struct sigaction sa_default = {};
    sa_default.sa_handler = SIG_DFL;
    sigaction(SIGSEGV, &sa_default, nullptr);
  }
  raise(SIGSEGV);

done:
  // Restore mem_ptr to the ucontext so the kernel's sigreturn sets
  // r13 = mem_ptr when control returns to the JIT code.
  if (s_real_tls)
    ctx->regs->gpr[13] = fault_r13;
}

void JitPPC64::Init()
{
  // Capture the real TLS pointer BEFORE any JIT code overwrites r13.
  // On ELFv2 PPC64, r13 = thread pointer.  The SIGSEGV handler needs
  // this to restore TLS before calling C++ code.
  asm volatile("mr %0, 13" : "=r"(s_real_tls));

  // Initialize the fastmem arena FIRST (so RefreshConfig can evaluate
  // jo.fastmem based on the real arena state), then read all config.
  InitFastmemArena();
  RefreshConfig();
  InitOffsets(m_ppc_state);

  // Allocate combined region
  m_code_region = AllocateCodeRegion(COMBINED_SIZE);
  if (!m_code_region)
    return;

  m_code_pos = m_code_region;
  m_code_end = m_code_region + JIT_CODE_SIZE;
  m_asm.SetBase(m_code_pos, JIT_CODE_SIZE);

  // Trampoline region starts right after main code
  m_tramp_region = m_code_region + JIT_CODE_SIZE;
  m_tramp_pos = m_tramp_region;
  m_tramp_end = m_tramp_region + TRAMP_CODE_SIZE;
  m_tramp_asm.SetBase(m_tramp_pos, TRAMP_CODE_SIZE);

  gpr.SetJit(*this, m_asm, REG_PPC_BASE);
  fpr.SetJit(*this, m_asm, REG_PPC_BASE);

  m_block_cache.Init();
  code_block.m_stats = &js.st;
  code_block.m_gpa = &js.gpa;
  code_block.m_fpa = &js.fpa;
  jo.optimizeGatherPipe = false;

  // Enable block linking (can be disabled via config)
  jo.enableBlocklink = true;
  if (SConfig::GetInstance().bJITNoBlockLinking)
    jo.enableBlocklink = false;

  // BLR optimization disabled on PPC64 — its guard-page calculation assumes
  // 4 KB pages, leaving only 64 KB of safe stack space on 64 KB page systems
  // (< 256 KB required by the check).  We also don't use the return-address
  // stack that this optimization enables.
  m_enable_blr_optimization = false;

  // Enable all analyzer optimizations for better block analysis
  analyzer.SetOption(PPCAnalyst::PPCAnalyzer::OPTION_CONDITIONAL_CONTINUE);
  analyzer.SetOption(PPCAnalyst::PPCAnalyzer::OPTION_BRANCH_MERGE);
  analyzer.SetOption(PPCAnalyst::PPCAnalyzer::OPTION_CROR_MERGE);
  analyzer.SetOption(PPCAnalyst::PPCAnalyzer::OPTION_CARRY_MERGE);
  analyzer.SetOption(PPCAnalyst::PPCAnalyzer::OPTION_BRANCH_FOLLOW);

  s_code_region = m_code_region;
  InitBackpatch();
  CompileDispatcher();

  NOTICE_LOG_FMT(POWERPC, "JITPPC64: initialized ppcState={} (code={}, tramp={}, combined={})",
                 fmt::ptr(&m_ppc_state),
                 fmt::ptr(m_code_region), fmt::ptr(m_tramp_region), COMBINED_SIZE);

  // Install SIGSEGV handler for MMIO backpatching
  g_jit_ppc64_instance = this;
  struct sigaction sa = {};
  sa.sa_sigaction = SIGSEGVHandler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, &s_old_sigsegv);
}

void JitPPC64::Shutdown()
{
  // Restore old SIGSEGV handler
  sigaction(SIGSEGV, &s_old_sigsegv, nullptr);
  g_jit_ppc64_instance = nullptr;

  ShutdownBackpatch();
  m_block_cache.Shutdown();
  m_fault_to_handler.clear();
  FreeCodeRegion(m_code_region, COMBINED_SIZE);
  m_code_region = nullptr;
  m_code_pos = nullptr;
  m_code_end = nullptr;
  m_tramp_region = nullptr;
  m_tramp_pos = nullptr;
  m_tramp_end = nullptr;
  m_enter_code = nullptr;
  m_dispatcher_entry = nullptr;
}

void JitPPC64::ClearCache()
{
  m_block_cache.Clear();
  m_code_pos = m_code_region;
  m_code_end = m_code_region + JIT_CODE_SIZE;
  m_asm.SetBase(m_code_pos, JIT_CODE_SIZE);
  m_tramp_pos = m_tramp_region;
  m_tramp_end = m_tramp_region + TRAMP_CODE_SIZE;
  m_tramp_asm.SetBase(m_tramp_pos, TRAMP_CODE_SIZE);
  m_fault_to_handler.clear();
  m_enter_code = nullptr;
  m_dispatcher_entry = nullptr;
  CompileDispatcher();
}

void JitPPC64::CompileDispatcher()
{
  m_asm.SetBase(m_code_pos, static_cast<size_t>(m_code_end - m_code_pos));

  // ── enter_code (called from Run()) ────────────────────────────────────
  // Sets r12 = &ppcState, then falls through to the dispatcher.
  // On PPC64, r12 is REG_PPC_BASE — the base pointer for all ppcState
  // loads/stores.  The block prolog also re-establishes it, but the
  // dispatcher itself needs r12 to read pc/downcount from ppcState.
  //
  // Mirror of Jit64's JitAsm.cpp: MOV(64, R(RPPCSTATE), Imm64(&ppc_state)).

  m_enter_code = m_code_pos;
  TrampMOVI64(m_asm, REG_PPC_BASE, reinterpret_cast<u64>(&m_ppc_state));

  // Save host SP to ppcState for ResetStack (BLR mispredict recovery).
  // We save BEFORE creating the dispatcher frame so that restoring it yields
  // the clean pre-JIT SP.
  m_asm.MR(REG_SCRATCH, REG_SP);
  m_asm.STD(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(STACK_PTR_OFFSET));

  // Save LR, r2 (TOC), and r10 to the dispatcher frame.  The frame persists
  // for the entire JIT chain — every dispatcher re-entry via block BRel
  // jumps to m_dispatcher_entry and does NOT overwrite these saves.
  // r2 must be restored before returning to Run() because JitPPC64Dispatch
  // (in a different compilation unit) clobbers it on ELFv2.
  m_asm.MFLR(REG_SCRATCH);
  m_asm.STDU(1, 1, -32);
  m_asm.STD(REG_SCRATCH, 1, 16);  // save LR at frame+16
  m_asm.STD(2, 1, 8);             // save r2 (TOC) at frame+8
  m_asm.STD(10, 1, 24);           // save r10 at frame+24

  // ── dispatcher entry (called from enter_code or block BRel) ──────────
  // Register state at entry:
  //   r12      = &ppcState  (set by enter_code or block prolog)
  //   r14-r31  = host callee-saved regs (set by last block's epilog
  //             when returning via exit path, or clobbered when chaining)
  //   r1       = dispatcher frame SP (enter_code_SP - 32)
  //   LR save  = Run_LR at frame+16  (set once by enter_code, never touched
  //             by dispatcher re-entries — block BRel jumps here without
  //             modifying frame+16)
  //   r10 save = Run's r10 at frame+24 (same reasoning)
  //
  // IMPORTANT: m_dispatcher_entry does NOT save LR or r10 — they were saved
  // once by enter_code above.  Block BRel branches directly to this point
  // without creating a new frame or clobbering the saved values.  The exit
  // path always loads LR/r10 from the original dispatcher frame, which
  // is correct because the frame is never re-created.
  //
  // Without this design, every block BRel → MFLR r14 would overwrite Run_LR
  // with the block's BRel return address, and the exit MTLR+BLR would return
  // to garbage instead of Run().

  m_dispatcher_entry = m_asm.Code() + m_asm.Size();

  m_asm.LWZ(11, REG_PPC_BASE, DOWNCOUNT_OFFSET);
  m_asm.CMPWI(0, 11, 0);

  // ble exit (branch if downcount ≤ 0) — placeholder
  const u8* ble_pos = m_asm.Code() + m_asm.Size();
  m_asm.BC(4, 1, 0);  // BO=4 (false), BI=1 (GT) → "branch if not GT" = ble

  // r3 = ppcState.pc → call JitPPC64Dispatch(pc)
  m_asm.LWZ(3, REG_PPC_BASE, PC_OFFSET);
  TrampMOVI64(m_asm, 12, reinterpret_cast<u64>(&JitPPC64Dispatch));
  m_asm.MTCTR(12);
  m_asm.BCTRL();

  // beq exit (branch if r3 == 0, i.e. block not found) — placeholder
  m_asm.CMPLDI(0, 3, 0);
  const u8* beq_pos = m_asm.Code() + m_asm.Size();
  m_asm.BC(12, 2, 0);  // BO=12 (true), BI=2 (EQ) → beq

  // ── Success path: jump to block entry ──────────────────────────────────
  // Keep the dispatcher frame intact (don't ADDI SP) so that the block
  // prolog can still read Run_LR and Jit.cpp's TOC from frame+16 and
  // frame+8 respectively.  SP remains at enter_code_SP - 32.
  m_asm.LD(10, 1, 24);           // restore r10 from dispatcher frame+24
  // PROBE: save intended block entry to global before jumping
  TrampMOVI64(m_asm, REG_SCRATCH2, reinterpret_cast<u64>(&g_last_block_entry));
  m_asm.STD(3, REG_SCRATCH2, 0);
  m_asm.MTCTR(3);                // block → CTR
  m_asm.BCTR();                  // jump to block (LR = BCTR return address)

  // ── Exit path: return to Run() ────────────────────────────────────────
  const u8* exit_pos = m_asm.Code() + m_asm.Size();
  // SP is still dispatcher_frame_SP = enter_code_SP - 32.
  // Load LR, r2 (TOC), and r10 from the dispatcher frame, then restore SP
  // to clean enter_code_SP (no block frames to unwind — block epilog
  // already restored).
  m_asm.LD(14, 1, 16);           // load LR from dispatcher frame+16
  m_asm.LD(2, 1, 8);             // restore r2 (TOC) from frame+8
  m_asm.LD(10, 1, 24);           // load r10 from dispatcher frame+24
  m_asm.LD(REG_SP, 1, 0);        // load backchain = original Run_SP from SP+0
  m_asm.MTLR(14);                // restore Run_LR
  m_asm.BLR();                   // return to Run()

  // ── dispatcher_lite (for block linking exits) ─────────────────────────
  // Called from JustWriteExit (BRel).  SP = block_SP = enter_code_SP - FRAME_SIZE.
  // r12 = &ppcState.  LR is saved by the block's prolog at block_SP + 16.
  // We load Run_LR from there and dispatch.  The block frame is kept intact
  // until we know whether we're exiting (→ m_dispatcher_exit, which restores
  // callee-saved regs from the frame) or continuing (tear down frame, jump).
  m_dispatcher_lite = m_asm.Code() + m_asm.Size();

  // PROBE: store PC to g_probe_pc_1 — confirms block exit reaches dispatcher_lite
  TrampMOVI64(m_asm, REG_SCRATCH2, reinterpret_cast<u64>(&g_probe_pc_1));
  m_asm.LWZ(REG_SCRATCH, REG_PPC_BASE, PC_OFFSET);
  m_asm.STW(REG_SCRATCH, REG_SCRATCH2, 0);

  m_asm.LD(14, 1, 16);   // r14 = Run_LR from block's prolog LR save

  // Frame is INTACT here — m_dispatcher_exit will restore callee-saved regs
  m_asm.LWZ(11, REG_PPC_BASE, DOWNCOUNT_OFFSET);
  m_asm.CMPWI(0, 11, 0);

  // ble → m_dispatcher_exit (branch if downcount ≤ 0) — placeholder
  const u8* ble_lite_pos = m_asm.Code() + m_asm.Size();
  m_asm.BC(4, 1, 0);

  // r3 = ppcState.pc → call JitPPC64Dispatch(pc)
  // Restore TLS before calling C++ — ELFv2 uses r13 as thread pointer.
  m_asm.LD(REG_PHYS_BASE, 1, TLS_SAVE_OFFSET);
  m_asm.LWZ(3, REG_PPC_BASE, PC_OFFSET);

  // PROBE: store dispatch PC to g_probe_pc_2 — confirms PC passed to JitPPC64Dispatch
  TrampMOVI64(m_asm, REG_SCRATCH2, reinterpret_cast<u64>(&g_probe_pc_2));
  m_asm.STW(3, REG_SCRATCH2, 0);

  TrampMOVI64(m_asm, 12, reinterpret_cast<u64>(&JitPPC64Dispatch));
  m_asm.MTCTR(12);
  m_asm.BCTRL();

  // beq → m_dispatcher_exit (block not found, frame still intact) — placeholder
  m_asm.CMPLDI(0, 3, 0);
  const u8* beq_lite_pos = m_asm.Code() + m_asm.Size();
  m_asm.BC(12, 2, 0);

  // Success: tear down frame and jump to next block
  m_asm.ADDI(1, 1, FRAME_SIZE);
  m_asm.MTLR(14);
  // PROBE: save intended block entry to global before jumping
  TrampMOVI64(m_asm, REG_SCRATCH2, reinterpret_cast<u64>(&g_last_block_entry));
  m_asm.STD(3, REG_SCRATCH2, 0);
  m_asm.MTCTR(3);
  m_asm.BCTR();

  // ── m_dispatcher_exit: restore callee-saved regs and return to Run() ──
  // Called from dispatcher_lite when downcount ≤ 0 or block not found.
  // r1 = block_SP = dispatcher_SP - FRAME_SIZE (frame intact — we haven't
  // torn it down yet).  The dispatcher frame is always above the block
  // frame (the entry success path no longer tears it down).
  // r12 = &ppcState.
  //
  // Run_LR and Jit.cpp's TOC are saved at block_SP+16 and block_SP+8
  // respectively (set by the block prolog from the dispatcher frame).
  // Run's r10 is at block_SP+FRAME_SIZE+24 (dispatcher frame+24).
  m_dispatcher_exit = m_asm.Code() + m_asm.Size();

  // PROBE: store PC to g_probe_pc_3 — confirms we're exiting via m_dispatcher_exit
  TrampMOVI64(m_asm, REG_SCRATCH2, reinterpret_cast<u64>(&g_probe_pc_3));
  m_asm.LWZ(REG_SCRATCH, REG_PPC_BASE, PC_OFFSET);
  m_asm.STW(REG_SCRATCH, REG_SCRATCH2, 0);

  m_asm.LD(REG_PHYS_BASE, 1, TLS_SAVE_OFFSET);       // restore real TLS for Run()
  for (u32 i = 14; i <= 31; ++i)
    m_asm.LD(i, 1, static_cast<s32>(CALLEE_SAVE_BASE + (i - 14) * 8));
  // Restore callee-saved FPRs
  for (u32 i = 14; i <= 31; ++i)
    m_asm.LFD(i, 1, static_cast<s32>(CALLEE_SAVE_FPR_BASE + (i - 14) * 8));
  // Load Run_LR and TOC from block frame saves (set by block prolog)
  m_asm.LD(REG_SCRATCH, 1, 16);
  m_asm.LD(2, 1, static_cast<s32>(R2_SAVE_OFFSET));
  // Load Run's r10 from dispatcher frame+24 (= block_SP+FRAME_SIZE+24)
  m_asm.LD(10, 1, static_cast<s32>(FRAME_SIZE + 24));
  // Tear down both block frame (FRAME_SIZE) and dispatcher frame (32)
  m_asm.ADDI(1, 1, FRAME_SIZE + 32);
  m_asm.MTLR(REG_SCRATCH);
  m_asm.BLR();

  // ── Patch placeholder branch offsets ───────────────────────────────────
  s32 ble_bd = static_cast<s32>(exit_pos - ble_pos);
  s32 beq_bd = static_cast<s32>(exit_pos - beq_pos);
  s32 ble_lite_bd = static_cast<s32>(m_dispatcher_exit - ble_lite_pos);
  s32 beq_lite_bd = static_cast<s32>(m_dispatcher_exit - beq_lite_pos);

  auto patch_bc = [](u8* pos, s32 bd, u32 bo, u32 bi) {
    u32 enc = (16u << 26) | ((bo & 0x1F) << 21) | ((bi & 0x1F) << 16) |
              (((bd >> 2) & 0x3FFF) << 2);
    std::memcpy(pos, &enc, sizeof(enc));
  };
  patch_bc(const_cast<u8*>(ble_pos), ble_bd, 4, 1);
  patch_bc(const_cast<u8*>(beq_pos), beq_bd, 12, 2);
  patch_bc(const_cast<u8*>(ble_lite_pos), ble_lite_bd, 4, 1);
  patch_bc(const_cast<u8*>(beq_lite_pos), beq_lite_bd, 12, 2);
  // Flush icache for all patched branch instructions
  __builtin___clear_cache(const_cast<u8*>(ble_pos), const_cast<u8*>(beq_lite_pos + 4));

  // Flush icache for dispatcher code — PPC970 has separate I-cache and D-cache
  __builtin___clear_cache(const_cast<u8*>(m_asm.Code()),
                           const_cast<u8*>(m_asm.Code() + m_asm.Size()));
  m_code_pos = const_cast<u8*>(m_asm.Code() + m_asm.Size());

  // Generate asm routines after the dispatcher code
  GenerateAsmRoutines();

  // Initialize branch watch counters base
  m_branch_counters_base = m_code_pos;
  m_branch_counters.clear();
  m_branch_counters_used = 0;
}

// ===========================================================================
// Prolog / Epilog
// ===========================================================================

void JitPPC64::EmitProlog()
{
  // Load Run_LR and Jit.cpp's TOC from the dispatcher frame (at SP+16 and
  // SP+8 respectively) BEFORE allocating the block frame.  The dispatcher
  // frame is always present because the entry success path no longer tears
  // it down.  dispatcher_lite's LD(14,1,16) later reads Run_LR from the
  // block's LR save.
  //
  // On ELFv2, JitPPC64Dispatch (a different compilation unit) clobbers r2,
  // so we must save Run()'s TOC here and restore it before returning.
  m_asm.LD(REG_SCRATCH, 1, 16);                  // r0 = dispatcher LR save = Run_LR
  m_asm.LD(REG_SCRATCH2, 1, 8);                  // r11 = dispatcher r2 save = Jit.cpp TOC
  // Probe the target page BEFORE the STDU — write 0 to the exact address
  // that STDU will use, forcing the kernel to commit that page.  Without
  // this, stdu r1,-FRAME_SIZE(r1) may cross a 64 KB page boundary onto
  // an uncommitted page, causing SIGSEGV on 64 KB page systems (PPC64).
  // Use r10 for the probe value — r0 holds LR and r11 holds TOC.
  // The exit path restores r10 from the dispatcher frame, not the block's
  // save slot, so clobbering r10 here is safe.
  m_asm.LI(10, 0);
  m_asm.STD(10, 1, -static_cast<s32>(FRAME_SIZE));
  m_asm.STDU(1, 1, -static_cast<s32>(FRAME_SIZE));
  m_asm.STD(REG_SCRATCH, 1, 16);                 // save Run_LR at block_SP+16
  m_asm.STD(REG_SCRATCH2, 1, static_cast<s32>(R2_SAVE_OFFSET));  // save TOC at block_SP+8

  // Save r10 (clobbered by CompileBC as not-taken flag) and r13 (real TLS)
  // Save r13 BEFORE overwriting with mem_ptr — ELFv2 ABI uses r13 as TLS.
  m_asm.STD(10, 1, static_cast<s32>(CALLEE_SAVE_BASE + (31 - 14 + 1) * 8));
  m_asm.STD(REG_PHYS_BASE, 1, TLS_SAVE_OFFSET);      // save real TLS
  // and callee-saved registers r14-r31 (used by GPR RegCache)
  for (u32 i = 14; i <= 31; ++i)
    m_asm.STD(i, 1, static_cast<s32>(CALLEE_SAVE_BASE + (i - 14) * 8));
  // and callee-saved FPRs f14-f31 (used by FPR RegCache)
  for (u32 i = 14; i <= 31; ++i)
    m_asm.STFD(i, 1, static_cast<s32>(CALLEE_SAVE_FPR_BASE + (i - 14) * 8));

  // Load REG_PPC_BASE with &ppcState (the emulated CPU state structure).
  TrampMOVI64(m_asm, REG_PPC_BASE, reinterpret_cast<u64>(&m_ppc_state));

  // Save REG_PPC_BASE (r12) at [SP+24] (TOC save slot).
  // This is done AFTER r12 is loaded with &ppcState so that the saved
  // value is the correct ppcState address.  The trampoline reloads r12
  // from this slot after frame tear-down instead of relying on its own
  // save (which may be corrupted by signal delivery or re-entrant fault
  // handling).
  m_asm.STD(REG_PPC_BASE, 1, 24);

  // Load REG_PHYS_BASE (r13) = mem_ptr from ppcState.
  // mem_ptr points to the host VA where guest physical 0 is mapped.
  // If null (fastmem arena unavailable), the fast path will fault and be
  // patched to the trampoline slow path automatically.
  m_asm.LD(REG_PHYS_BASE, REG_PPC_BASE, static_cast<s32>(MEM_PTR_OFFSET));
  // Save mem_ptr separately — C++ calls (backpatch, dispatcher) need TLS
  // restored before the call and mem_ptr restored after.
  m_asm.STD(REG_PHYS_BASE, 1, PHYS_BASE_SAVE_OFFSET);

  gpr.Reset();
  fpr.Reset();

  // Reset CA tracking — CA value is unknown at block entry
  m_ca_known = false;
  m_ca_value = 0;
  m_ca_in_r0 = false;
  m_ca_dirty = false;

  // Reset CR0 caching — native CR0 may be stale from previous block/interpreter
  m_cr0_native_valid = false;

  // Sync guest rounding mode to host FPSCR at block entry.
  // On PPC970 the guest and host share the same FPSCR, but the
  // interpreter may have changed RN since the last JIT block.
  UpdateRoundingMode();

  // JITPROBE disabled
}

void JitPPC64::EmitEpilog(u32 next_pc)
{
  gpr.Flush(js.op);
  fpr.Flush(js.op);

  // Use JustWriteExit to emit a linkable exit (branches to dispatcher_lite
  // or directly to the next block if already linked).
  JustWriteExit(next_pc, false, 0);
}

// ===========================================================================
// Block linking exits
//
// WriteExit — stores the destination PC, decrements downcount (subtracting
// the block's estimated cycle count), and emits a jump to the dispatcher.
// The jump target is recorded in linkData so that once the destination block
// is compiled, WriteLinkBlock patches it to jump directly to the block.
//
// JustWriteExit — lower-level: just emits the dispatcher jump with linkData,
// without downcount subtraction (caller handles that if needed).
//
// The emitted jump is a `b dispatcher` at the exitPtrs location.  After
// block linking, it gets patched to `b dest_normalEntry`.
// ===========================================================================

void JitPPC64::EmitDowncountAndBail()
{
  // Decrement downcount by the block's cycle estimate.  If the result is
  // ≤ 0, branch to m_dispatcher_exit (restores callee-saved regs, returns to
  // Run(), which advances CoreTiming and refills downcount).  This check MUST
  // be inline in the exit code BEFORE the linkable BRel: WriteLinkBlock
  // patches that BRel to jump directly to the destination block's
  // normalEntry, bypassing dispatcher_lite — without this inline check, a
  // linked loop back-edge never lets emulated time advance (the boot DMA
  // wait loop spun forever because the done bit was only set by a CoreTiming
  // event that never fired).
  m_asm.LWZ(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(DOWNCOUNT_OFFSET));
  m_asm.ADDI(REG_SCRATCH2, REG_SCRATCH2, -static_cast<s32>(js.downcountAmount));
  m_asm.STW(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(DOWNCOUNT_OFFSET));

  // if (downcount > 0) skip the bail; else branch to m_dispatcher_exit.
  // BC(12, 1, 8): branch on CR0[GT]; bd=8 skips the next (single) instruction.
  m_asm.CMPWI(0, REG_SCRATCH2, 0);
  m_asm.BC(12, 1, 8);
  m_asm.BRel(m_dispatcher_exit);
}

void JitPPC64::WriteExit(u32 destination, bool bl, u32 after)
{
  gpr.Flush(js.op);
  fpr.Flush(js.op);
  FlushCarry();
  FlushCR0IfDirty();

  // Store the destination PC FIRST: the downcount bail below returns to Run()
  // (which reads ppcState.pc to dispatch the next block), so PC must already
  // be correct when the bail is taken.  JustWriteExit stores it again — the
  // redundant store is harmless.
  m_asm.LI32(REG_SCRATCH, destination);
  m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PC_OFFSET));

  // Subtract block's estimated instruction count from downcount and bail to
  // Run() if it reaches zero — checked inline so linked exits can't bypass it.
  EmitDowncountAndBail();

  JustWriteExit(destination, bl, after);
}

void JitPPC64::JustWriteExit(u32 destination, bool bl, u32 after)
{
  auto* b = js.curBlock;
  JitBlock::LinkData linkData;
  linkData.exitAddress = destination;
  linkData.linkStatus = false;
  linkData.call = bl;

  const bool use_blr_opt = bl && m_enable_blr_optimization;

  if (use_blr_opt)
  {
    // =====================================================================
    // CALL path with BLR return-address stack push.
    //
    // IMPORTANT: REG_SCRATCH2 (r11) MUST hold host_ret_addr when pushed at
    // the STD below.  r11 cannot be clobbered between the ADDI and the push.
    //
    // Emits:
    //   [increment BLR depth counter]   ← r11 OK to clobber here
    //   [compute host_ret_addr via BL .+4 / MFLR / ADDI]  ← r11 = ret addr
    //   [construct guest_val = (feature_flags << 32) | after]
    //   PUSH {guest_val, host_ret_addr} onto host stack
    //   [store destination PC]
    //   B dispatcher_lite          ← exitPtrs = this instruction
    //   [return path]:             ← host_ret_addr points here
    //     [store after to PC]
    //     B dispatcher_lite
    //
    // WriteLinkBlock patches the B to a BL, setting LR = exitPtrs + 4 =
    // host_ret_addr.  WriteBLRExit pops the entry and fast-BCTR to
    // host_ret_addr on prediction match.
    // =====================================================================

    const u8* start_pos = m_asm.Code() + m_asm.Size();

    // --- Increment BLR stack depth counter (before host_ret_addr) ---
    // r11 is free here — the host_ret_addr computation below will set it.
    m_asm.LWZ(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(BLR_DEPTH_OFFSET));
    m_asm.ADDI(REG_SCRATCH2, REG_SCRATCH2, 1);
    m_asm.STW(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(BLR_DEPTH_OFFSET));

    // --- Compute host_ret_addr via BL .+4 / MFLR ---
    // BL .+4: LR = address right after this BL instruction = MFLR address
    m_asm.BLRel(start_pos + 4);
    m_asm.MFLR(REG_SCRATCH2);     // r11 = MFLR address = start_pos + 4

    // Emit ADDI placeholder; patched below once we know the offset
    const u8* addi_pos = m_asm.Code() + m_asm.Size();
    m_asm.Write32((14u << 26) | (REG_SCRATCH2 << 21) | (REG_SCRATCH2 << 16) | 0);

    // --- Construct guest_val = (feature_flags << 32) | after ---
    const u64 feature_flags = m_ppc_state.feature_flags;
    if (feature_flags == 0)
    {
      m_asm.LI32(REG_SCRATCH, after);
    }
    else
    {
      m_asm.LI32(REG_SCRATCH, static_cast<u32>(feature_flags));
      m_asm.RLDICL(REG_SCRATCH, REG_SCRATCH, 32, 0);   // ff << 32
      m_asm.ORIS(REG_SCRATCH, REG_SCRATCH, static_cast<u16>(after >> 16));
      m_asm.ORI(REG_SCRATCH, REG_SCRATCH, static_cast<u16>(after));
    }

    // --- Push BLR entry: 16 bytes on host stack ---
    m_asm.ADDI(REG_SP, REG_SP, -16);
    m_asm.STD(REG_SCRATCH, REG_SP, 0);   // SP+0 = guest_val
    m_asm.STD(REG_SCRATCH2, REG_SP, 8);  // SP+8 = host_ret_addr (r11 intact!)

    // --- Store destination PC ---
    m_asm.LI32(REG_SCRATCH, destination);
    m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PC_OFFSET));

    // --- Emit branch link slot ---
    linkData.exitPtrs = m_asm.Code() + m_asm.Size();
    m_asm.BRel(m_dispatcher_lite);

    // --- Return path (host_ret_addr = exitPtrs + 4) ---
    m_asm.LI32(REG_SCRATCH, after);
    m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PC_OFFSET));
    m_asm.BRel(m_dispatcher_lite);

    // --- Patch the ADDI placeholder to compute: r11 = r11 + offset_to_ret_path
    //     r11 after MFLR = start_pos + 4
    //     Desired: r11 = ret_path = exitPtrs + 4 = (start_pos + 4) + (exitPtrs - start_pos)
    //     offset = exitPtrs - start_pos
    s32 offset = static_cast<s32>(linkData.exitPtrs - start_pos);
    u32 patched_addi = (14u << 26) | (REG_SCRATCH2 << 21) | (REG_SCRATCH2 << 16) |
                       (offset & 0xFFFF);
    std::memcpy(const_cast<u8*>(addi_pos), &patched_addi, sizeof(patched_addi));

    b->linkData.push_back(linkData);
    return;
  }

  // Non-call or BLR optimization disabled: simple dispatcher exit
  m_asm.LI32(REG_SCRATCH, destination);
  m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PC_OFFSET));

  linkData.exitPtrs = m_asm.Code() + m_asm.Size();
  m_asm.BRel(m_dispatcher_lite);

  b->linkData.push_back(linkData);
}

// ===========================================================================
// WriteExceptionExit — exit block, check for pending exceptions
//
// Stores the destination PC, then calls CheckExceptionsFromJIT so that
// any pending exception (e.g. FP Program exception from the last
// instruction) gets handled.  After the call, dispatches to the block
// at ppcState.npc (which the exception handler may have modified).
//
// Clobbers: LR, r0, r11, r3-r10 (ABI call)
// Preserves: r12 (REG_PPC_BASE — saved/restored around the call)
// ===========================================================================

void JitPPC64::WriteExceptionExit(u32 destination)
{
  gpr.Flush(js.op);
  fpr.Flush(js.op);
  FlushCarry();
  FlushCR0IfDirty();

  // Store destination to pc (CheckExceptionsFromJIT reads pc)
  m_asm.LI32(REG_SCRATCH, destination);
  m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PC_OFFSET));

  // Also store to npc (so the normal-case dispatch finds the right address)
  m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PC_OFFSET + 4));

  // Save LR and r12 between block frame slots (safe: within 256B frame)
  m_asm.MFLR(REG_SCRATCH2);
  m_asm.STD(REG_SCRATCH2, REG_SP, 8);
  m_asm.STD(REG_PPC_BASE, REG_SP, 16);

  // Restore TLS before calling C++ — ELFv2 uses r13 as thread pointer.
  m_asm.LD(REG_PHYS_BASE, REG_SP, TLS_SAVE_OFFSET);

  // Call CheckExceptionsFromJIT(m_system.GetPowerPC())
  TrampMOVI64(m_asm, 3, reinterpret_cast<u64>(&m_system.GetPowerPC()));
  TrampMOVI64(m_asm, 12, reinterpret_cast<u64>(&PowerPC::CheckExceptionsFromJIT));
  m_asm.MTCTR(12);
  m_asm.BCTRL();

  // Restore r12 (REG_PPC_BASE — volatile across call)
  m_asm.LD(REG_PPC_BASE, REG_SP, 16);

  // Reload mem_ptr — TLS was restored before the call, block needs it back
  m_asm.LD(REG_PHYS_BASE, REG_PPC_BASE, static_cast<s32>(MEM_PTR_OFFSET));

  // Restore LR
  m_asm.LD(REG_SCRATCH2, REG_SP, 8);
  m_asm.MTLR(REG_SCRATCH2);

  // Load NPC (may have been modified by CheckExceptionsFromJIT) and dispatch
  m_asm.LWZ(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PC_OFFSET + 4));
  m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PC_OFFSET));

  // Decrement downcount with inline bail (matches Jit64 WriteExceptionExit)
  EmitDowncountAndBail();

  // Use JustWriteExit to emit the linkable exit (handles linkData internally)
  JustWriteExit(destination, false, 0);
  return;
}

// ===========================================================================
// WriteExceptionExitReg — exit block with runtime destination register
//
// Same as WriteExceptionExit but takes a host register (host_reg) containing
// the destination PC value instead of a compile-time constant.
// ===========================================================================

void JitPPC64::WriteExceptionExitReg(u32 host_reg)
{
  gpr.Flush(js.op);
  fpr.Flush(js.op);
  FlushCarry();
  FlushCR0IfDirty();

  // Store destination to pc and npc (value already in host_reg)
  m_asm.STW(host_reg, REG_PPC_BASE, static_cast<s32>(PC_OFFSET));
  m_asm.STW(host_reg, REG_PPC_BASE, static_cast<s32>(PC_OFFSET + 4));

  // Save LR and r12 between block frame slots
  m_asm.MFLR(REG_SCRATCH2);
  m_asm.STD(REG_SCRATCH2, REG_SP, 8);
  m_asm.STD(REG_PPC_BASE, REG_SP, 16);

  // Restore TLS before calling C++ — ELFv2 uses r13 as thread pointer.
  m_asm.LD(REG_PHYS_BASE, REG_SP, TLS_SAVE_OFFSET);

  // Call CheckExceptionsFromJIT(m_system.GetPowerPC())
  TrampMOVI64(m_asm, 3, reinterpret_cast<u64>(&m_system.GetPowerPC()));
  TrampMOVI64(m_asm, 12, reinterpret_cast<u64>(&PowerPC::CheckExceptionsFromJIT));
  m_asm.MTCTR(12);
  m_asm.BCTRL();

  // Restore r12 (REG_PPC_BASE — volatile across call)
  m_asm.LD(REG_PPC_BASE, REG_SP, 16);

  // Reload mem_ptr — TLS was restored before the call, block needs it back
  m_asm.LD(REG_PHYS_BASE, REG_PPC_BASE, static_cast<s32>(MEM_PTR_OFFSET));

  // Restore LR
  m_asm.LD(REG_SCRATCH2, REG_SP, 8);
  m_asm.MTLR(REG_SCRATCH2);

  // Load NPC (may have been modified by CheckExceptionsFromJIT) and dispatch
  m_asm.LWZ(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PC_OFFSET + 4));
  m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PC_OFFSET));

  // Decrement downcount with inline bail (matches Jit64 WriteExceptionExit)
  EmitDowncountAndBail();

  // Record link data — exit address unknown at compile time
  auto* b = js.curBlock;
  JitBlock::LinkData linkData;
  linkData.exitAddress = 0;
  linkData.linkStatus = false;
  linkData.call = false;
  linkData.exitPtrs = m_asm.Code() + m_asm.Size();
  m_asm.BRel(m_dispatcher_lite);
  b->linkData.push_back(linkData);
}

// ===========================================================================
// WriteConditionalExceptionExit — check a specific exception bit
//
// If the given exception bit is set in ppcState.Exceptions, flushes GPRs
// and exits via WriteExceptionExit.  Otherwise continues (no-op).
// The exception handler is inlined: a beq branches past the handler.
// ===========================================================================

void JitPPC64::WriteConditionalExceptionExit(int exception)
{
  const u32 bit = MathUtil::IntLog2(exception);

  // Load ppcState.Exceptions
  m_asm.LWZ(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(EXCEPTIONS_OFFSET));

  // Test the bit: andi. r0, r11, (1 << bit); sets CR0[EQ] if result == 0
  m_asm.ANDI_(REG_SCRATCH, REG_SCRATCH, 1u << bit);

  // beq skip — placeholder, patched after we know the handler end
  const u8* beq_pos = m_asm.Code() + m_asm.Size();
  m_asm.BC(12, 2, 0);  // BO=12 (branch if true), BI=2 (EQ) → beq

  // Handler: exception is pending — do the full exception exit
  WriteExceptionExit(js.compilerPC + 4);

  // Patch the beq to jump past the handler
  const u8* after_handler = m_asm.Code() + m_asm.Size();
  s32 beq_bd = static_cast<s32>(after_handler - beq_pos);
  u32 enc = (16u << 26) | ((12u & 0x1F) << 21) | ((2u & 0x1F) << 16) |
            (((beq_bd >> 2) & 0x3FFF) << 2);
  std::memcpy(const_cast<u8*>(beq_pos), &enc, sizeof(enc));
}

// ===========================================================================
// WriteIdleExit — exit block, set downcount=0 for idle detection
//
// On PPC, the idle loop is: mfspr DEC; cmpwi; bgt loop.  When DEC expires,
// a decrementer exception fires.  Setting downcount=0 forces Run() to exit
// the inner JIT loop and enter CoreTiming::Idle() — same as Jit64.
// ===========================================================================

void JitPPC64::WriteIdleExit()
{
  gpr.Flush(js.op);
  fpr.Flush(js.op);
  FlushCarry();
  FlushCR0IfDirty();

  // Set downcount to 0 (triggers idle detection in Run())
  m_asm.LI(REG_SCRATCH, 0);
  m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(DOWNCOUNT_OFFSET));

  // Read LR from SPR and dispatch (same as non-optimized BLR)
  m_asm.LWZ(REG_SCRATCH, REG_PPC_BASE,
            static_cast<s32>(SPR_OFFSET + 4 * 8));
  m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 0, 0, 29);
  m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PC_OFFSET));

  // downcount is 0 here — always exit to Run() (which enters CoreTiming's
  // idle detection).  Bail directly instead of dispatching: the exit must
  // NOT be linkable, because a linked jump would bypass the downcount=0
  // signal and spin forever.
  m_asm.BRel(m_dispatcher_exit);
}

// ===========================================================================
// WriteBLRExit — BLR return with return-address stack prediction
//
// Pops a {host_ret_addr, guest_val} entry from the BLR stack.  Reads the
// guest LR from ppcState.spr[SPR_LR] and compares with the popped value.
// If they match: decrement downcount and BCTR to host_ret_addr (skips
// dispatcher).  On mismatch: ResetStack (discard all BLR entries),
// decrement downcount, and fall back to dispatcher.
// ===========================================================================

void JitPPC64::WriteBLRExit()
{
  if (!m_enable_blr_optimization)
  {
    // No optimization: read LR from SPR and dispatch
    gpr.Flush(js.op);
    fpr.Flush(js.op);
    FlushCarry();
    FlushCR0IfDirty();
    m_asm.LWZ(REG_SCRATCH, REG_PPC_BASE,
              static_cast<s32>(SPR_OFFSET + 4 * 8));  // r0 = SPR_LR
    m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 0, 0, 29); // mask to 30 bits
    m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PC_OFFSET));
    EmitDowncountAndBail();
    auto* b = js.curBlock;
    JitBlock::LinkData linkData;
    linkData.exitAddress = js.compilerPC;
    linkData.linkStatus = false;
    linkData.call = false;
    linkData.exitPtrs = m_asm.Code() + m_asm.Size();
    m_asm.BRel(m_dispatcher_lite);
    b->linkData.push_back(linkData);
    return;
  }

  gpr.Flush(js.op);
  fpr.Flush(js.op);
  FlushCarry();
  FlushCR0IfDirty();

  // Check BLR stack depth: if 0, skip pop and go to slow path
  // Use r11 — PPC addi with RA=0 uses literal 0.
  m_asm.LWZ(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(BLR_DEPTH_OFFSET));
  m_asm.CMPWI(0, REG_SCRATCH2, 0);
  const u8* beq_empty_pos = m_asm.Code() + m_asm.Size();
  m_asm.BC(12, 2, 0);   // BEQ → patch to slow path later

  // Decrement counter
  m_asm.ADDI(REG_SCRATCH2, REG_SCRATCH2, -1);
  m_asm.STW(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(BLR_DEPTH_OFFSET));

  // Pop BLR entry: r11 = guest_val, r0 = host_ret_addr; SP += 16
  m_asm.LD(REG_SCRATCH2, REG_SP, 0);   // r11 = guest_val  (ff<<32 | ppc_pc)
  m_asm.LD(REG_SCRATCH, REG_SP, 8);    // r0  = host_ret_addr
  m_asm.ADDI(REG_SP, REG_SP, 16);

  // Save host_ret_addr in r10 (not-taken flag, preserved across flush)
  m_asm.MR(10, REG_SCRATCH);

  // Read guest LR from SPR, mask to 30 bits
  m_asm.LWZ(REG_SCRATCH, REG_PPC_BASE,
            static_cast<s32>(SPR_OFFSET + 4 * 8));
  m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 0, 0, 29);  // r0 = LR & 0xFFFFFFFC

  // Build expected value from LR: combine with feature_flags if needed
  const u64 feature_flags = m_ppc_state.feature_flags;
  if (feature_flags != 0)
  {
    // Need: (ff << 32) | LR
    // r0 has LR, r11 has popped guest_val
    // Use r11 as scratch, save guest_val to r10 temporarily
    m_asm.MR(REG_SCRATCH2, REG_SCRATCH);  // save LR to r11 (overwrites guest_val!)
    // Wait, r11 has guest_val. Save guest_val to r10 first.
    // Actually r10 already has host_ret, saved above.
    // Need another register. Use stack temporarily.
    // Simpler: reconstruct expected value from runtime LR + compile-time ff
    m_asm.LI32(REG_PHYS_BASE, static_cast<u32>(feature_flags));
    m_asm.RLDICL(REG_PHYS_BASE, REG_PHYS_BASE, 32, 0);   // r13 = ff << 32
    m_asm.OR(REG_PHYS_BASE, REG_PHYS_BASE, REG_SCRATCH); // r13 = (ff<<32) | LR
    // Now compare guest_val (saved to r10) with r13
    m_asm.CMPD(0, 10, REG_PHYS_BASE);
  }
  else
  {
    // Compare guest_val (r11) with LR (r0) as 32-bit values
    m_asm.CMPW(0, REG_SCRATCH2, REG_SCRATCH);
  }

  // Branch to slow path if NOT equal (mispredict)
  // BO=4 (branch if condition FALSE), BI=2 (EQ): branch if not equal
  const u8* bne_pos = m_asm.Code() + m_asm.Size();
  m_asm.BC(4, 2, 0);   // placeholder — patched after we know slow path size

  // ============================================
  // Fast path: prediction match — BCTR to host_ret_addr
  // ============================================

  // Downcount
  // Use r11 — PPC addi with RA=0 uses literal 0.
  m_asm.LWZ(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(DOWNCOUNT_OFFSET));
  m_asm.ADDI(REG_SCRATCH2, REG_SCRATCH2, -static_cast<s32>(js.downcountAmount));
  m_asm.STW(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(DOWNCOUNT_OFFSET));

  // Restore REG_PHYS_BASE (may have been clobbered by feature_flags path)
  if (feature_flags != 0)
  {
    m_asm.LD(REG_PHYS_BASE, REG_PPC_BASE, static_cast<s32>(MEM_PTR_OFFSET));
  }

  // Jump directly to the return path code (skips dispatcher)
  m_asm.MTCTR(10);        // CTR = host_ret_addr
  m_asm.BCTR();

  // ============================================
  // Slow path: mispredict — reset stack + dispatch
  // ============================================
  const u8* slow_path_start = m_asm.Code() + m_asm.Size();

  // Restore REG_PHYS_BASE (clobbered if feature_flags was used)
  if (feature_flags != 0)
  {
    m_asm.LD(REG_PHYS_BASE, REG_PPC_BASE, static_cast<s32>(MEM_PTR_OFFSET));
  }

  // Reset BLR stack: load saved SP from ppcState
  m_asm.LD(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(STACK_PTR_OFFSET));
  m_asm.MR(REG_SP, REG_SCRATCH);

  // Re-create dispatcher frame: load return_to_Run_addr from the old
  // dispatcher frame at SP-16 (preserved since enter_code created it —
  // nothing overwrites it because all block frames are below SP-32).
  m_asm.LD(REG_SCRATCH, REG_SP, -16);
  m_asm.STDU(REG_SP, REG_SP, -32);
  m_asm.STD(REG_SCRATCH, REG_SP, 16);
  m_asm.STD(10, REG_SP, 24);

  // Downcount
  // Use r11 — PPC addi with RA=0 uses literal 0.
  m_asm.LWZ(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(DOWNCOUNT_OFFSET));
  m_asm.ADDI(REG_SCRATCH2, REG_SCRATCH2, -static_cast<s32>(js.downcountAmount));
  m_asm.STW(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(DOWNCOUNT_OFFSET));

  // Read LR from SPR and dispatch
  m_asm.LWZ(REG_SCRATCH, REG_PPC_BASE,
            static_cast<s32>(SPR_OFFSET + 4 * 8));
  m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 0, 0, 29);
  m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PC_OFFSET));

  // Restore TLS from the old block frame (still on stack at SP - FRAME_SIZE)
  // before entering dispatcher_entry — r13 was mem_ptr when the error
  // occurred, but dispatcher_entry's BCTRL to JitPPC64Dispatch needs TLS.
  // Old block frame: top of block frame at SP - FRAME_SIZE (before ResetStack
  // moved SP), TLS saved at block_SP + TLS_SAVE_OFFSET.
  m_asm.LD(REG_PHYS_BASE, REG_SP, static_cast<s32>(-static_cast<s32>(FRAME_SIZE) +
                                                     static_cast<s32>(TLS_SAVE_OFFSET)));

  // Enter dispatcher_entry (re-enters the normal dispatch loop with
  // a valid dispatcher frame — no block frame exists after ResetStack,
  // so dispatcher_lite is not usable).
  // No linkData recorded — this exit is runtime-dynamic (depends on LR).
  m_asm.BRel(m_dispatcher_entry);

  // Patch both branches to the slow path:
  //   beq_empty_pos — taken when counter == 0 (stack empty)
  //   bne_pos       — taken when popped guest_val ≠ LR (mispredict)
  const auto patch_branch = [&](const u8* pos, u32 bo, u32 bi) {
    s32 bd = static_cast<s32>(slow_path_start - pos);
    u32 enc = (16u << 26) | ((bo & 0x1F) << 21) | ((bi & 0x1F) << 16) |
              (((bd >> 2) & 0x3FFF) << 2);
    std::memcpy(const_cast<u8*>(pos), &enc, sizeof(enc));
  };
  patch_branch(beq_empty_pos, 12, 2);  // BEQ: BO=12, BI=2
  patch_branch(bne_pos, 4, 2);          // BNE: BO=4, BI=2
}

// ===========================================================================
// FakeLKExit — push BLR entry without a terminal block exit
//
// Used when PPCAnalyst inlines a CALL (the return was found in the same
// block), the CALL is not the last instruction, and a subsequent BCLR in
// the same block needs a matching BLR stack entry.
//
// This is identical to the push portion of JustWriteExit(bl=true) but does
// NOT emit the terminal exit — the block continues after this function.
// ===========================================================================

void JitPPC64::FakeLKExit(u32 after)
{
  if (!m_enable_blr_optimization)
    return;

  const u8* start_pos = m_asm.Code() + m_asm.Size();

  // Compute host_ret_addr: BL .+4 / MFLR trick
  m_asm.BLRel(start_pos + 4);
  m_asm.MFLR(REG_SCRATCH2);

  // Emit ADDI placeholder (patched below)
  const u8* addi_pos = m_asm.Code() + m_asm.Size();
  m_asm.Write32((14u << 26) | (REG_SCRATCH2 << 21) | (REG_SCRATCH2 << 16) | 0);

  // Build guest_val
  const u64 feature_flags = m_ppc_state.feature_flags;
  if (feature_flags == 0)
  {
    m_asm.LI32(REG_SCRATCH, after);
  }
  else
  {
    m_asm.LI32(REG_SCRATCH, static_cast<u32>(feature_flags));
    m_asm.RLDICL(REG_SCRATCH, REG_SCRATCH, 32, 0);
    m_asm.ORIS(REG_SCRATCH, REG_SCRATCH, static_cast<u16>(after >> 16));
    m_asm.ORI(REG_SCRATCH, REG_SCRATCH, static_cast<u16>(after));
  }

  // Increment BLR stack depth counter (use r3 as scratch — not otherwise used here)
  m_asm.LWZ(3, REG_PPC_BASE, static_cast<s32>(BLR_DEPTH_OFFSET));
  m_asm.ADDI(3, 3, 1);
  m_asm.STW(3, REG_PPC_BASE, static_cast<s32>(BLR_DEPTH_OFFSET));

  // Push BLR entry
  m_asm.ADDI(REG_SP, REG_SP, -16);
  m_asm.STD(REG_SCRATCH, REG_SP, 0);
  m_asm.STD(REG_SCRATCH2, REG_SP, 8);

  // Emit a BL (with small offset) so LR gets set for the next compiler
  // that may need it.  This BL goes to the instruction right after it.
  const u8* after_push = m_asm.Code() + m_asm.Size();
  m_asm.BLRel(after_push + 8);  // skip 1 instruction (4B) = BL .+8
  m_asm.ADDI(REG_SCRATCH, REG_SCRATCH, 0);  // NOP-like (skipped by BL)

  // Patch the host_ret offset
  // We want r11 = address where execution continues after WriteBLRExit
  // That's `after_push + 8` (right after the BL .+8)
  // r11 currently = start_pos + 4 (from MFLR)
  // offset = (after_push + 8) - (start_pos + 4) = after_push + 4 - start_pos
  s32 offset = static_cast<s32>(after_push + 4 - start_pos);
  u32 patched_addi = (14u << 26) | (REG_SCRATCH2 << 21) | (REG_SCRATCH2 << 16) |
                     (offset & 0xFFFF);
  std::memcpy(const_cast<u8*>(addi_pos), &patched_addi, sizeof(patched_addi));
}

// ===========================================================================
// ResetStack — restore host SP to the value saved at JIT entry
//
// This effectively discards all BLR stack entries, preventing stack
// corruption on mispredict or JIT exit.
// ===========================================================================

void JitPPC64::ResetStack()
{
  if (!m_enable_blr_optimization)
    return;
  // Restore SP to saved value (unwinds all BLR entries)
  m_asm.LD(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(STACK_PTR_OFFSET));
  m_asm.MR(REG_SP, REG_SCRATCH);
  // Zero the depth counter
  m_asm.LI(REG_SCRATCH, 0);
  m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(BLR_DEPTH_OFFSET));
}

void JitPPC64::LoadGPR(u32 host_reg, u32 guest_reg)
{
  m_asm.LWZ(host_reg, REG_PPC_BASE, static_cast<s32>(GPR_OFFSET + 4 * guest_reg));
}

void JitPPC64::StoreGPR(u32 guest_reg, u32 host_reg)
{
  m_asm.STW(host_reg, REG_PPC_BASE, static_cast<s32>(GPR_OFFSET + 4 * guest_reg));
}

// ===========================================================================
// UpdateRoundingMode — sync guest FPSCR.RN to host FPSCR
//
// On PPC970 the guest and host share the same FPSCR, but the interpreter
// may have changed RN since the last block was compiled (via mtfsfi/mtfsf).
// We reload RN from ppcState.fpscr and write it to FPSCR field 0.
// Clobbers: r0 (REG_SCRATCH), r11 (REG_SCRATCH2)
// ===========================================================================

void JitPPC64::UpdateRoundingMode()
{
  // Load guest FPSCR.Hex from ppcState
  m_asm.LWZ(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(FPSCR_OFFSET));
  // Extract RN (bits 1-0 of Hex) and shift to bits 30-31 for RLWIMI
  m_asm.RLWINM(REG_SCRATCH2, REG_SCRATCH2, 30, 30, 31);
  // Read current hardware FPSCR
  m_asm.MFFS(REG_SCRATCH);
  // Insert RN into bits 30-31, preserving FEX/VXNI at bits 28-29
  m_asm.RLWIMI(REG_SCRATCH, REG_SCRATCH2, 0, 30, 31);
  // Write back field 0 (mask=0x01 = crfD=0)
  m_asm.MTFSF(0x01, REG_SCRATCH);
}

// ===========================================================================
// FlushCR0IfDirty — synchronize host CR0 with ppcState.cr[0]
//
// If native CR0 was set by CMPWI (m_cr0_native_valid), CR0[LT,GT,EQ] is
// correct but CR0[SO] may have a stale host value.  We read the host CR,
// fix CR0[SO] from the guest XER_SO, and write CR0 back.
//
// If CR0 was never modified by a JIT RC-bit op (not native valid), the
// host CR0 still has the value from the last block's epilog or interpreter,
// which matches ppcState.cr[0] — nothing to do.
//
// Clobbers: r0 (REG_SCRATCH), r11 (REG_SCRATCH2)
// ===========================================================================

void JitPPC64::FlushCR0IfDirty()
{
  if (!m_cr0_native_valid)
    return;

  m_asm.MFCR(REG_SCRATCH2);
  m_asm.LBZ(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(XER_SO_OV_OFFSET));
  m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 0, 30, 30);   // SO bit → u32 bit 30
  m_asm.RLWIMI(REG_SCRATCH2, REG_SCRATCH, 27, 3, 3);   // insert at CR0[SO] pos
  m_asm.MTCRF(0x01, REG_SCRATCH2);
  // After flush, native CR0 IS correct (including SO).  The next CMPWI
  // will overwrite CR0[LT,GT,EQ] but preserve SO — so we keep valid=true.
}

// ===========================================================================
// FlushCarry — if CA is dirty in REG_SCRATCH, store to ppcState.xer_ca
// Clobbers: nothing (REG_SCRATCH already holds CA if dirty)
// ===========================================================================

void JitPPC64::FlushCarry()
{
  if (m_ca_dirty)
  {
    m_asm.STB(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(XER_CA_OFFSET));
    m_ca_dirty = false;
    m_ca_in_r0 = false;  // ppcState now holds the value, not r0
  }
  else if (m_ca_known && !m_ca_in_r0)
  {
    // CA is a known constant that was never computed in a host register
    // but might still need storing if it was SET in this block.
    // Actually, if m_ca_known=true and !m_ca_dirty, the value was known
    // at block entry (reset to false in EmitProlog) or set by addic with
    // simm=0 followed by FlushCarry.  In either case, ppcState is already
    // correct.  Nothing to do.
  }
}

// ===========================================================================
// FlushAll — flush everything before a C call or block exit
// ===========================================================================

void JitPPC64::FlushAll()
{
  gpr.Flush();
  fpr.Flush();
  FlushCarry();
  FlushCR0IfDirty();
}

// ===========================================================================
// PrepareCall — flush guest state before calling C++ from JIT code
//
// Saves all dirty registers (GPR, FPR, carry, CR0) to ppcState so the
// C++ function finds a consistent guest state.  After the call, the
// compiler should reload r12 (REG_PPC_BASE) if the C function may have
// relocated ppcState.
// ===========================================================================

void JitPPC64::PrepareCall()
{
  gpr.Flush();
  fpr.Flush();
  FlushCarry();
  FlushCR0IfDirty();
}

// ===========================================================================
// CR0 update (for RC-bit instructions)
// Clobbers r0, r11
// ===========================================================================

void JitPPC64::EmitCR0Update(u32 host_reg)
{
  m_asm.EXTSW(REG_SCRATCH2, host_reg);
  m_asm.CMPWI(0, REG_SCRATCH2, 0);
  // CR0[LT,GT,EQ] now correct in host CR.  CR0[SO] is preserved from the
  // last flush.  The full MFCR+LBZ+RLWINM+RLWIMI+MTCRF sequence is deferred
  // to FlushCR0IfDirty() — needed only on mfcr/mtcrf/mcrf/bc(bi=3)/block exit.
  m_cr0_native_valid = true;
}

void JitPPC64::EmitSetXER_OV(u32 r_ov)
{
  // r_ov must have 0 or 1 in bit 0 (the OV value).
  // xer_so_ov byte format: bit 0 = OV, bit 1 = SO.
  m_asm.RLWINM(r_ov, r_ov, 0, 31, 31);            // keep only bit 0
  m_asm.LBZ(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(XER_SO_OV_OFFSET));
  m_asm.RLWINM(REG_SCRATCH2, REG_SCRATCH2, 0, 0, 30);  // clear OV bit (PPC bit 31), keep SO
  m_asm.OR(r_ov, r_ov, REG_SCRATCH2);
  m_asm.STB(r_ov, REG_PPC_BASE, static_cast<s32>(XER_SO_OV_OFFSET));
}

// ===========================================================================
// EmitStoreProbe — record store EA / host address / block to globals
//
// Emitted immediately before a fast-path store memory access, stores only.
// Precondition: r11 (REG_SCRATCH2) = host address; guest EA in EA_SAVE_OFFSET.
// Only REG_SCRATCH (r0) is clobbered; r11 is restored to the host address so
// the subsequent store instruction uses the correct pointer.
// ===========================================================================
void JitPPC64::EmitStoreProbe()
{
  // r0 = guest EA (reload from the frame slot saved by the caller)
  m_asm.LD(REG_SCRATCH, 1, EA_SAVE_OFFSET);

  // g_stm_last_ea = guest EA
  TrampMOVI64(m_asm, REG_SCRATCH2, reinterpret_cast<u64>(&g_stm_last_ea));
  m_asm.STD(REG_SCRATCH, REG_SCRATCH2, 0);

  // g_stm_ea12 = (EA & 0xFFF) << 12
  m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 12, 12, 23);
  TrampMOVI64(m_asm, REG_SCRATCH2, reinterpret_cast<u64>(&g_stm_ea12));
  m_asm.STW(REG_SCRATCH, REG_SCRATCH2, 0);

  // g_probe_block_addr = block start PC
  TrampMOVI64(m_asm, REG_SCRATCH2, reinterpret_cast<u64>(&g_probe_block_addr));
  m_asm.LI32(REG_SCRATCH, m_block_start);
  m_asm.STW(REG_SCRATCH, REG_SCRATCH2, 0);

  // g_stm_last_host = r11 (host address) — recompute r11 from saved EA first
  m_asm.LD(REG_SCRATCH, 1, EA_SAVE_OFFSET);
  m_asm.RLWINM(REG_SCRATCH2, REG_SCRATCH, 0, 2, 31);
  m_asm.ADD(REG_SCRATCH2, REG_SCRATCH2, REG_PHYS_BASE);
  TrampMOVI64(m_asm, REG_SCRATCH, reinterpret_cast<u64>(&g_stm_last_host));
  m_asm.STD(REG_SCRATCH2, REG_SCRATCH, 0);
}

// ===========================================================================
// EmitBackpatchRoutine — emit fast path + trampoline slow path
//
// Called from CompileLoadStore after the EA is in REG_SCRATCH2 (r11) and
// the data register is loaded/stored.  We emit:
//   1. The fast path instruction (LWZ/STW/LBZ/etc.) in main code
//   2. A corresponding slow path in the trampoline region that:
//      - Saves volatile registers
//      - Calls TrampolineDispatcher with (ppcState*, EA, instruction, next_jit)
//      - For loads: stores the returned value to data_reg
//      - Restores registers
//      - Branches back to the instruction after the fast path
//
// On fault, the fast path is patched with `b slow_path_entry`.
// ===========================================================================
void JitPPC64::EmitBackpatchRoutine(u32 access_size, u32 opcd, u32 rd,
                                     u32 ra, u32 data_reg, bool is_load,
                                     bool is_fpr)
{
  // 1. Record fast path position
  const u8* fast_start = m_asm.Code() + m_asm.Size();

  // Save original guest EA (r11) to block frame for slow path recovery
  m_asm.STD(REG_SCRATCH2, 1, EA_SAVE_OFFSET);

  if (jo.fastmem)
  {
    // Fast path: translate EA to host address via fastmem arena.
    // Guest addresses use K1 (0x80000000) and K2 (0xC0000000, 0xE0000000)
    // alias bits; mask to physical address with 0x3FFFFFFF (30-bit mask
    // covering MEM1/MEM2 up to 1 GB).  RLWINM clears the top 2 bits of
    // the 32-bit word, giving the equivalent of (EA & 0x3FFFFFFF).
    m_asm.RLWINM(REG_SCRATCH2, REG_SCRATCH2, 0, 2, 31);
    m_asm.ADD(REG_SCRATCH2, REG_SCRATCH2, REG_PHYS_BASE);

    // Diagnostic probe: record the store's EA / host address / block before
    // the access.  Only REG_SCRATCH (r0) is clobbered; r11 stays host address.
    // Skipped when the store value lives in REG_SCRATCH itself (r0 as data_reg)
    // — FPR stores (is_fpr) hold data in FPR0, so r0 is free and they are probed.
    if (!is_load && (is_fpr || data_reg != REG_SCRATCH))
      EmitStoreProbe();

    if (is_fpr)
    {
      if (is_load)
      {
        if (access_size == 32)
          m_asm.LFS(0, REG_SCRATCH2, 0);
        else
          m_asm.LFD(0, REG_SCRATCH2, 0);
      }
      else
      {
        if (access_size == 32)
          m_asm.STFS(0, REG_SCRATCH2, 0);
        else
          m_asm.STFD(0, REG_SCRATCH2, 0);
      }
    }
    else
    {
      switch (access_size)
      {
      case 8:
        if (is_load)  m_asm.LBZ(data_reg, REG_SCRATCH2, 0);
        else          m_asm.STB(data_reg, REG_SCRATCH2, 0);
        break;
      case 16:
        if (is_load)  m_asm.LHZ(data_reg, REG_SCRATCH2, 0);
        else          m_asm.STH(data_reg, REG_SCRATCH2, 0);
        break;
      case 32:
        if (is_load)  m_asm.LWZ(data_reg, REG_SCRATCH2, 0);
        else          m_asm.STW(data_reg, REG_SCRATCH2, 0);
        break;
      default:
        return;
      }
    }
  }
  else
  {
    // No fastmem arena — REG_PHYS_BASE points to the page table array,
    // not to emulated RAM.  Emit a placeholder branch (NOP) that will
    // be patched to branch to the trampoline slow path below.
    m_asm.NOP();
  }

  // Restore the original guest EA to REG_SCRATCH2 for update-form
  // instructions (ra != 0), so the caller's MR(gpr.W(ra), REG_SCRATCH2)
  // copies the correct EA.  This is needed by both the fast path (where
  // REG_SCRATCH2 holds the translated host address) and the slow path
  // (where the trampoline clobbers REG_SCRATCH2 with the return address).
  if (ra != 0)
    m_asm.LD(REG_SCRATCH2, 1, EA_SAVE_OFFSET);
  const u8* fast_end = m_asm.Code() + m_asm.Size();

  // 2. Emit slow path in trampoline region
  const u8* slow_entry = m_tramp_pos;
  m_tramp_asm.SetBase(m_tramp_pos, static_cast<size_t>(m_tramp_end - m_tramp_pos));

  // Save volatile registers (r0, r3-r10) + LR
  m_tramp_asm.STD(REG_SCRATCH, 1, 8);  // save store value (r0) at block frame + 8
  m_tramp_asm.MFLR(0);
  m_tramp_asm.STDU(REG_SCRATCH, 1, -128);
  m_tramp_asm.STD(0, 1, 120);   // save LR
  m_tramp_asm.LD(REG_SCRATCH, 1, 136);  // restore r0 from block frame + 8
  m_tramp_asm.STD(3, 1, 112);
  m_tramp_asm.STD(4, 1, 104);
  m_tramp_asm.STD(5, 1, 96);
  m_tramp_asm.STD(6, 1, 88);
  m_tramp_asm.STD(7, 1, 80);
  m_tramp_asm.STD(8, 1, 72);
  m_tramp_asm.STD(9, 1, 64);
  m_tramp_asm.STD(10, 1, 56);

  // Arguments to TrampolineDispatcher:
  //   r3 = ppcState* (from r12)
  //   r4 = EA         (from saved slot in block frame: block_SP + EA_SAVE_OFFSET)
  //   r5 = is_store   (0=load, 1=store)
  //   r6 = access_size (8/16/32)
  //   r7 = PPC register rd
  //   r8 = PPC register ra
  m_tramp_asm.MR(3, REG_PPC_BASE);
  m_tramp_asm.LD(4, 1, static_cast<s32>(128 + EA_SAVE_OFFSET));

  m_tramp_asm.LI(5, is_load ? 0 : 1);
  m_tramp_asm.LI(6, static_cast<s32>(access_size));
  m_tramp_asm.LI(7, static_cast<s32>(rd));
  m_tramp_asm.LI(8, static_cast<s32>(ra));

  // For stores: pass the actual value in r9
  if (!is_load)
  {
    if (is_fpr)
    {
      m_tramp_asm.STFD(0, 1, -8);
      m_tramp_asm.LD(9, 1, -8);
    }
    else
    {
      m_tramp_asm.MR(9, data_reg);
    }
  }

  m_tramp_asm.STD(REG_PPC_BASE, 1, 48);

  // Restore TLS before calling C++ — ELFv2 uses r13 as thread pointer.
  m_tramp_asm.LD(REG_PHYS_BASE, 1, 128 + TLS_SAVE_OFFSET);

  TrampMOVI64(m_tramp_asm, 12,
              reinterpret_cast<u64>(&TrampolineDispatcher));
  m_tramp_asm.MTCTR(12);
  m_tramp_asm.BCTRL();

  m_tramp_asm.LD(10, 1, 56);
  m_tramp_asm.LD(9, 1, 64);
  m_tramp_asm.LD(8, 1, 72);
  m_tramp_asm.LD(7, 1, 80);
  m_tramp_asm.LD(6, 1, 88);
  m_tramp_asm.LD(5, 1, 96);
  m_tramp_asm.LD(4, 1, 104);
  m_tramp_asm.LD(3, 1, 112);
  m_tramp_asm.LD(0, 1, 120);
  m_tramp_asm.MTLR(0);

  m_tramp_asm.ADDI(REG_SCRATCH, 1, 128);
  m_tramp_asm.MR(1, REG_SCRATCH);

  m_tramp_asm.LD(REG_PPC_BASE, 1, 24);
  m_tramp_asm.LD(REG_PHYS_BASE, 1, PHYS_BASE_SAVE_OFFSET);

  if (is_load)
  {
    if (is_fpr)
    {
      m_tramp_asm.LFD(0, REG_PPC_BASE,
                       static_cast<s32>(PS_OFFSET + rd * 16));
    }
    else
    {
      m_tramp_asm.LWZ(data_reg, REG_PPC_BASE,
                       static_cast<s32>(GPR_OFFSET + rd * 4));
    }
  }

  TrampMOVI64(m_tramp_asm, REG_SCRATCH2,
              reinterpret_cast<u64>(fast_end));
  m_tramp_asm.MTCTR(REG_SCRATCH2);
  m_tramp_asm.BCTR();

  u32 tramp_size = static_cast<u32>(m_tramp_asm.Size());
  m_tramp_pos += tramp_size;

  // 3. Patch the placeholder branch (no-fastmem case) or record the
  //    fast → slow mapping for the fault handler (fastmem case).
  if (!jo.fastmem)
  {
    // Patch the NOP at fast_start + 4 to an unconditional branch to slow_entry.
    u8* patch = const_cast<u8*>(fast_start) + 4;
    ptrdiff_t delta = slow_entry - patch;
    u32 li = (static_cast<u32>(delta >> 2)) & 0x00FFFFFF;
    *(u32*)patch = (18u << 26) | (li << 2);
    __builtin___clear_cache(patch, patch + 4);
  }
  else
  {
    FastmemArea area;
    area.fast_access_code = fast_start;
    area.slow_access_code = slow_entry;
    area.is_load = is_load;
    area.rd = rd;
    area.ra = ra;
    m_fault_to_handler[fast_end] = area;
  }

  // Flush trampoline icache
  __builtin___clear_cache(const_cast<u8*>(slow_entry),
                           const_cast<u8*>(slow_entry + tramp_size));
}

// ===========================================================================
// Jit() — compile a block
// ===========================================================================

void JitPPC64::Jit(u32 em_address)
{
  Jit(em_address, true);
}

void JitPPC64::Jit(u32 em_address, bool clear_cache_and_retry_on_failure)
{
  RefreshConfig();

  if (SConfig::GetInstance().bJITNoBlockCache)
    ClearCache();

  const u32 nextPC = analyzer.Analyze(em_address, &code_block, &m_code_buffer,
                                      static_cast<u32>(m_code_buffer.size()));

  if (code_block.m_memory_exception)
    return;

  fprintf(stderr, "JITPROBE: compiling block at 0x%08X (%u instr)\n",
          em_address, code_block.m_num_instructions);

  // Record block start for the store probe (g_probe_block_addr).
  m_block_start = em_address;

  // Per-instruction fallback: unhandled opcodes call FallBackToInterpreter
  // (no block-level reject gate).

  fprintf(stderr, "JITPROBE2: m_code_pos=%p m_code_end=%p\n", m_code_pos, m_code_end);
  size_t estimated_size = code_block.m_num_instructions * 128;
  if (m_code_pos + estimated_size > m_code_end)
  {
    fprintf(stderr, "JITPROBE2: clear_cache path\n");
    if (clear_cache_and_retry_on_failure)
    {
      ClearCache();
      Jit(em_address, false);
    }
    return;
  }

  fprintf(stderr, "JITPROBE2: AllocateBlock\n");
  JitBlock* b = m_block_cache.AllocateBlock(em_address);
  if (!b)
  {
    fprintf(stderr, "JITPROBE2: AllocateBlock returned NULL\n");
    return;
  }
  fprintf(stderr, "JITPROBE2: AllocateBlock OK\n");

  fprintf(stderr, "JITPROBE2: SetBase\n");
  u8* block_start = m_code_pos;
  m_asm.SetBase(m_code_pos, static_cast<size_t>(m_code_end - m_code_pos));

  b->normalEntry = block_start;
  b->near_begin = block_start;
  b->near_end = block_start;
  js.curBlock = b;

  fprintf(stderr, "JITPROBE2: EmitProlog\n");
  EmitProlog();

  fprintf(stderr, "JITPROBE2: After EmitProlog, code size so far=%zu\n", m_asm.Size());

  m_constant_propagation.Clear();
  ResetFPRTypes();

  // Initialize GQR tracking from block analysis.
  // GQR values known at compile time are set from the instruction stream;
  // m_gqr_known[i] is set/cleared by mtspr/gqrs in the compiler.
  for (auto& g : m_gqr_known)
    g = false;

  js.downcountAmount = 0;

  fprintf(stderr, "JITPROBE2: Starting instruction loop (%u instr)\n",
          code_block.m_num_instructions);

  for (u32 i = 0; i < code_block.m_num_instructions; ++i)
  {
    PPCAnalyst::CodeOp& op = m_code_buffer[i];
    js.compilerPC = op.address;
    js.op = &op;
    js.instructionsLeft = static_cast<int>(code_block.m_num_instructions - i - 1);
    js.downcountAmount += op.opinfo->num_cycles;

    if (op.skip)
      continue;

    // Constant propagation: try to evaluate at compile time
    const JitCommon::ConstantPropagationResult cp_result =
        m_constant_propagation.EvaluateInstruction(op.inst, op.opinfo->flags);

    if (cp_result.instruction_fully_executed)
    {
      // Instruction has no side effects — skip emitting any code
      m_constant_propagation.Apply(cp_result);
      i += js.skipInstructions;
      js.skipInstructions = 0;
      continue;
    }

    CompileInstruction(op);
    m_constant_propagation.Apply(cp_result);

    // Propagate known-constant values into the regcache.
    // If constant propagation resolved this instruction's output to a known
    // value, mark it in the regcache so R() uses LI instead of LWZ.
    if (cp_result.gpr >= 0)
      gpr.SetImmediate32(static_cast<u32>(cp_result.gpr), cp_result.gpr_value);

    i += js.skipInstructions;
    js.skipInstructions = 0;

    if (i % 10 == 0)
      fprintf(stderr, "JITPROBE2: compiled instr %u/%u\n", i, code_block.m_num_instructions);
  }

  fprintf(stderr, "JITPROBE2: EmitEpilog\n");
  EmitEpilog(nextPC);

  fprintf(stderr, "JITPROBE2: clear_cache\n");
  u8* block_end = m_code_pos + m_asm.Size();
  __builtin___clear_cache(block_start, block_end);

  b->near_end = block_end;

  fprintf(stderr, "JITPROBE2: FinalizeBlock\n");
  m_block_cache.FinalizeBlock(*b, jo.enableBlocklink, code_block, m_code_buffer);
  m_code_pos = block_end;

  fprintf(stderr, "JITPROBE2: block compiled OK\n");
}

// ===========================================================================
// Run / SingleStep
// ===========================================================================

void JitPPC64::Run()
{
  ProtectStack();

  // Stack probe: commit all stack pages before entering the JIT.
  //
  // The JIT block prolog does stdu r1,-FRAME_SIZE(r1) which writes to a page
  // below the current SP.  On 64 KB page systems, this write may land on a
  // page that the kernel has not yet committed (the stack grows one page at
  // a time, and the JIT's frame is much smaller than 64 KB — even a single
  // C++ function call may push SP past a page boundary).
  //
  // Worse, the SP at block entry may be ABOVE the SP captured here (e.g. if
  // a prior block's exit path restored SP to a value higher than Run()'s
  // frame).  The prolog's STD at r1-384 then writes to a page above the
  // probed region, causing SIGSEGV.
  //
  // Fix: probe EVERY 64 KB page in the stack region from stack_base to
  // stack_end (as reported by pthread_getattr_np).  This guarantees every
  // page is committed regardless of where SP ends up during JIT execution.
  //
  // Additionally, probe pages above the current SP up to stack_end + 128 KB
  // to catch the case where SP has been restored above Run()'s frame by
  // the m_dispatcher_exit path (ADDI +FRAME_SIZE+32).
  {
    auto [stack_addr, stack_size] = Common::GetCurrentThreadStack();
    const uintptr_t sbase = reinterpret_cast<uintptr_t>(stack_addr);
    const uintptr_t send = sbase + stack_size;
    uintptr_t sp;
    asm volatile("mr %0, 1" : "=r"(sp));

    NOTICE_LOG_FMT(POWERPC,
                   "JITPROBE: stack_base={:#018x} stack_end={:#018x} "
                   "guard={} r1={:#018x}",
                   sbase, send, fmt::ptr(m_stack_guard), sp);

    // Probe downward: from current SP down to stack_base, in 4 KB steps.
    // IMPORTANT: use 4 KB granularity, NOT 64 KB.  On PPC64 with 64 KB
    // pages, writing a single byte at a 64 KB boundary only commits the
    // first 4 KB subpage.  The block prolog's STD at r1-384 may land in
    // a different 4 KB subpage of the same 64 KB page, which is still
    // uncommitted → SIGSEGV.  Probing every 4 KB subpage guarantees all
    // subpages are committed regardless of access offset.
    //
    // The for-loop condition uses signed comparison to handle wraparound
    // after page underflows past 0.
    for (uintptr_t page = sp & ~static_cast<uintptr_t>(0xFFF);
         static_cast<intptr_t>(page) >= static_cast<intptr_t>(sbase);
         page -= 0x1000)
    {
      *reinterpret_cast<volatile u8*>(page) = 0;
    }

    // Probe upward: from current SP up to send, in 4 KB steps.
    // This covers the case where m_dispatcher_exit restores SP above
    // Run()'s frame.  Cap at send to avoid probing unmapped memory.
    for (uintptr_t page = (sp + 0x1000) & ~static_cast<uintptr_t>(0xFFF);
         page < send;
         page += 0x1000)
    {
      *reinterpret_cast<volatile u8*>(page) = 0;
    }
  }

  // Initialize timebase from CoreTiming before entering JIT.  Without this,
  // CompileMFTB's per-read increments are overwritten every dispatch by
  // JitPPC64Dispatch's GetFakeTimeBase call.  With GetFakeTimeBase removed
  // from the dispatch path, we set it here once per slice so the JIT sees
  // a starting timebase that advances across slices while CompileMFTB's
  // per-read increments accumulate within each slice.
  {
    const u64 tb = m_system.GetSystemTimers().GetFakeTimeBase();
    m_ppc_state.spr[SPR_TL] = static_cast<u32>(tb);
    m_ppc_state.spr[SPR_TU] = static_cast<u32>(tb >> 32);
  }

  auto& core_timing = m_system.GetCoreTiming();
  auto& cpu = m_system.GetCPU();

  while (cpu.GetState() == CPU::State::Running)
  {
    core_timing.Advance();

    // Refresh timebase after CoreTiming advance so JIT sees new base value
    {
      const u64 tb = m_system.GetSystemTimers().GetFakeTimeBase();
      m_ppc_state.spr[SPR_TL] = static_cast<u32>(tb);
      m_ppc_state.spr[SPR_TU] = static_cast<u32>(tb >> 32);
    }

    while (m_ppc_state.downcount > 0 && cpu.GetState() == CPU::State::Running)
    {
      if (bJITOff)
      {
        m_system.GetInterpreter().SingleStep();
        m_ppc_state.downcount -= 1;
        continue;
      }

      // Try to find or compile a block for the current PC
      if (!m_block_cache.GetBlockFromStartAddress(m_ppc_state.pc,
                                                     m_ppc_state.feature_flags))
      {
        if (m_failed_pcs.count(m_ppc_state.pc) == 0)
        {
          Jit(m_ppc_state.pc);
          if (!m_block_cache.GetBlockFromStartAddress(m_ppc_state.pc,
                                                        m_ppc_state.feature_flags))
          {
            m_failed_pcs.insert(m_ppc_state.pc);
            // Can't compile — fall back to interpreter
            m_system.GetInterpreter().SingleStep();
            m_ppc_state.downcount -= 1;
            continue;
          }
        }
        else
        {
          // Already known to fail — skip Jit() and go straight to interpreter
          m_system.GetInterpreter().SingleStep();
          m_ppc_state.downcount -= 1;
          continue;
        }
      }

      // Enter the JIT code via enter_code, which sets r12 = &ppcState and
      // falls through to the dispatcher.  The dispatcher chains blocks
      // internally (via JitPPC64Dispatch + block linking) until
      // downcount ≤ 0, then returns to Run().
      reinterpret_cast<void (*)()>(m_enter_code)();
    }
  }
  UnprotectStack();
}

void JitPPC64::SingleStep()
{
  ProtectStack();
  m_system.GetCoreTiming().Advance();
  m_system.GetInterpreter().SingleStep();
  m_ppc_state.downcount -= 1;
  UnprotectStack();
}

// ===========================================================================
// Overrides
// ===========================================================================

void JitPPC64::EraseSingleBlock(const JitBlock& block)
{
  m_block_cache.EraseSingleBlock(block);
}

std::vector<JitBase::MemoryStats> JitPPC64::GetMemoryStats() const
{
  const std::size_t near_free = static_cast<std::size_t>(m_code_end - m_code_pos);
  const std::size_t main_size = static_cast<std::size_t>(m_code_end - m_code_region);
  return {{"near", {near_free, 1.0 - static_cast<double>(near_free) / main_size}}};
}

std::size_t JitPPC64::DisassembleNearCode(const JitBlock& block, std::ostream& stream) const
{
  // Dump host PPC64 instructions of the compiled JIT block
  const u8* start = block.near_begin;
  const u8* end = block.near_end;
  std::size_t count = 0;
  for (const u8* p = start; p < end; p += 4)
  {
    u32 insn = 0;
    std::memcpy(&insn, p, sizeof(insn));
    stream << fmt::format("  [{:04x}] {:08x}\n",
                          static_cast<u32>(p - start), insn);
    ++count;
  }
  return count;
}

std::size_t JitPPC64::DisassembleFarCode(const JitBlock& block, std::ostream& stream) const
{
  // No far code section in JITPPC64 currently
  return 0;
}

bool JitPPC64::CompileTable31(UGeckoInstruction inst)
{
  if (CompileTable31_Integer(inst))
    return true;
  if (CompileTable31_SystemReg(inst))
    return true;
  if (CompileTable31_LoadStore(inst))
    return true;
  if (CompileTable31_CA(inst))
    return true;
  return CompileMisc(inst);
}

void JitPPC64::FallBackToInterpreter(UGeckoInstruction inst)
{
  // Flush everything before calling C++ (interpreters reads all guest state)
  FlushCarry();
  gpr.Flush();
  fpr.Flush();

  if (js.op->canEndBlock)
  {
    m_asm.LI32(REG_SCRATCH, js.compilerPC);
    m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PC_OFFSET));
    m_asm.LI32(REG_SCRATCH, js.compilerPC + 4);
    m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PC_OFFSET + 4));
  }

  const Interpreter::Instruction instr = Interpreter::GetInterpreterOp(inst);

  // Save LR + r12 across the C++ call
  m_asm.MFLR(REG_SCRATCH2);
  m_asm.STD(REG_SCRATCH2, REG_SP, 8);
  m_asm.STD(REG_PPC_BASE, REG_SP, 16);

  // Restore TLS before calling C++ — ELFv2 uses r13 as thread pointer.
  m_asm.LD(REG_PHYS_BASE, REG_SP, TLS_SAVE_OFFSET);

  // ELFv2 ABI: r3 = this, r4 = inst.hex, r12 = function pointer
  TrampMOVI64(m_asm, 3, reinterpret_cast<u64>(&m_system.GetInterpreter()));
  m_asm.LI32(4, inst.hex);
  TrampMOVI64(m_asm, 12, reinterpret_cast<u64>(instr));
  m_asm.MTCTR(12);
  m_asm.BCTRL();

  // Restore r12 and LR
  m_asm.LD(REG_PPC_BASE, REG_SP, 16);
  m_asm.LD(REG_SCRATCH2, REG_SP, 8);
  m_asm.MTLR(REG_SCRATCH2);

  // Reload r13 (REG_PHYS_BASE = mem_ptr) — clobbered by C++ ABI
  m_asm.LD(REG_PHYS_BASE, REG_PPC_BASE, static_cast<s32>(MEM_PTR_OFFSET));

  // Reset caches — interpreter may have changed any register
  gpr.Reset();
  fpr.Reset();
  m_constant_propagation.ClearGPRs(js.op->regsOut);

  // Exit if NPC changed (exception triggered)
  if (js.op->canEndBlock)
  {
    if (js.isLastInstruction)
    {
      WriteExceptionExit(js.compilerPC);
    }
    else
    {
      m_asm.LWZ(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PC_OFFSET + 4));
      m_asm.LI32(REG_SCRATCH2, js.compilerPC + 4);
      m_asm.CMPLW(0, REG_SCRATCH, REG_SCRATCH2);
      const u8* beq_pos = m_asm.Code() + m_asm.Size();
      m_asm.BC(12, 2, 0);  // beq skip
      WriteExceptionExit(js.compilerPC);
      const u8* after_handler = m_asm.Code() + m_asm.Size();
      const s32 beq_bd = static_cast<s32>(after_handler - beq_pos);
      u32 enc = (16u << 26) | ((12u & 0x1F) << 21) | ((2u & 0x1F) << 16) |
                (((beq_bd >> 2) & 0x3FFF) << 2);
      std::memcpy(const_cast<u8*>(beq_pos), &enc, sizeof(enc));
    }
  }
  else if (ShouldHandleFPExceptionForInstruction(js.op))
  {
    WriteConditionalExceptionExit(EXCEPTION_PROGRAM);
  }
}
void JitPPC64::DoNothing(UGeckoInstruction inst) {}
void JitPPC64::UnknownInstruction(UGeckoInstruction inst) {}

void JitPPC64::LoadCR(u32 host_reg)
{
  // Load ppcState.cr.Hex (u32 at offset 64 from CR_OFFSET, after u64 fields[8])
  // and write to the real PPC970 CR register so native BC instructions see
  // the correct CR values.
  m_asm.LWZ(host_reg, REG_PPC_BASE, static_cast<s32>(CR_OFFSET + 64));
  m_asm.MTCRF(0xFF, host_reg);
}

void JitPPC64::StoreCR(u32 host_reg)
{
  // Read the real PPC970 CR and store to ppcState.cr.Hex.
  // fields[] is NOT updated here — the EmitCRCheck non-CR0 path reads
  // fields[] which stays current from interpreter cr.Set() calls.
  // This matches the existing design (non-CR0 fields are rare).
  m_asm.MFCR(host_reg);
  m_asm.STW(host_reg, REG_PPC_BASE, static_cast<s32>(CR_OFFSET + 64));
}

void JitPPC64::EmitBackpatchSlot()
{
  // Emit a NOP placeholder that can be patched to a branch by HandleFault.
  // On JitPPC64 the backpatch system uses EmitBackpatchRoutine instead
  // (which emits both fast and slow paths and installs them in
  // m_fault_to_handler), so this is a no-op.
}

// ===========================================================================
// Debug helpers
// ===========================================================================

void JitPPC64::DumpCode(const u8* start, size_t size)
{
  fprintf(stderr, "JIT: code dump at %p (%zu bytes):\n",
          reinterpret_cast<const void*>(start), size);
  for (size_t i = 0; i < size; i += 4)
  {
    u32 hw = 0;
    if (i + 4 <= size)
      hw = *reinterpret_cast<const u32*>(start + i);
    fprintf(stderr, "  [%04zu] 0x%08X\n", i, hw);
  }
}

void JitPPC64::DoBacktrace()
{
  u32 lr = 0;
  u32 sp = 0;

  // Read LR from ppcState
  lr = m_ppc_state.spr[SPR_LR];
  sp = m_ppc_state.gpr[1];

  fprintf(stderr, "=== JIT Backtrace ===\n");
  fprintf(stderr, "  pc   = 0x%08X\n", m_ppc_state.pc);
  fprintf(stderr, "  lr   = 0x%08X\n", lr);
  fprintf(stderr, "  sp   = 0x%08X\n", sp);
  fprintf(stderr, "  host SP = %p\n", reinterpret_cast<const void*>(&sp));
  fprintf(stderr, "  downcount = %d\n", m_ppc_state.downcount);
  fprintf(stderr, "  Exceptions = 0x%08X\n",
          m_ppc_state.Exceptions);

  // Walk guest stack
  u32 fp = sp;
  int frame = 0;
  while (fp && fp < 0x83000000 && frame < 16)
  {
    u32 next_fp = 0;
    u32 saved_lr = 0;
    auto& mem = m_system.GetMemory();
    if (mem.GetPointerForRange(fp, 12) != nullptr)
    {
      next_fp = mem.Read_U32(fp);
      saved_lr = mem.Read_U32(fp + 8);
      fprintf(stderr, "  [frame %d] fp=0x%08X lr=0x%08X\n", frame, fp, saved_lr);
      fp = next_fp;
      frame++;
    }
    else
    {
      break;
    }
  }
}

// ===========================================================================
// FPR type tracking helpers
//
// On PPC970, Gekko paired-single values are stored in 64-bit FPRs as:
//   [ps0 : upper 32 bits][ps1 : lower 32 bits]
//
// stfd stores the 64-bit value in BE byte order:
//   [mem+0..3] = ps0, [mem+4..7] = ps1
//
// lfs loads a 32-bit float from a BE memory address:
//   lfs frD, 0(mem) → ps0
//   lfs frD, 4(mem) → ps1
// ===========================================================================

void JitPPC64::ConvertDoubleToSingleLower(u32 fr_idx)
{
  // Extract the lower 32 bits (ps1) of the FPR as a scalar single
  m_asm.STFD(fr_idx, REG_SCRATCH2, 0);
  m_asm.LFS(fr_idx, REG_SCRATCH2, 4);
}

void JitPPC64::ConvertDoubleToSingleUpper(u32 fr_idx)
{
  // Extract the upper 32 bits (ps0) of the FPR as a scalar single
  m_asm.STFD(fr_idx, REG_SCRATCH2, 0);
  m_asm.LFS(fr_idx, REG_SCRATCH2, 0);
}

void JitPPC64::PairSingleToDouble(u32 dst_fpr, u32 src_upper, u32 src_lower)
{
  // Pack two singles into one 64-bit FPR: [src_upper:ps0][src_lower:ps1]
  // 1. Store src_upper to stack[0..3]
  // 2. Store src_lower to stack[4..7]
  // 3. Load as 64-bit double back into dst_fpr
  m_asm.STFS(src_upper, REG_SCRATCH2, 0);
  m_asm.STFS(src_lower, REG_SCRATCH2, 4);
  m_asm.LFD(dst_fpr, REG_SCRATCH2, 0);
}

bool JitPPC64::IsFPRStoreSafe(u32 guest_fpr) const
{
  // On PPC970 ELFv2: f14-f31 are callee-saved (preserved by C++ functions).
  // f0-f13 are volatile (clobbered by function calls).
  //
  // We use the host FPRs as follows:
  //   f0      = scratch / pack/unpack temporary
  //   f14-f31 = mapped to guest FPRs when cached
  //
  // A store is NOT safe (must be spilled) if the guest FPR is tracked in a
  // volatile host FPR. Since we only map guest FPRs to f14-f31 (callee-saved),
  // the answer is always YES for any cached guest FPR.
  //
  // Uncached guest FPRs don't need storing (they're already in ppcState).
  return true;
}


// ===========================================================================
// Branch watch — emit profiling counter increments
// ===========================================================================
//
// When IsBranchWatchEnabled() is true, each branch site calls EmitBranchCounter
// which increments a 64-bit counter stored in the trampoline region.
// The counter address is loaded via LI32, followed by LD/ADDI/STD.
// ===========================================================================

void JitPPC64::EmitBranchCounter()
{
  if (!IsBranchWatchEnabled())
    return;
  if (!m_branch_counters_base)
    return;

  u64* counter_addr = reinterpret_cast<u64*>(m_branch_counters_base) +
                      m_branch_counters_used;

  // Load counter address (r0), load value (r11), increment, store back.
  m_asm.LI32(REG_SCRATCH, static_cast<u32>(reinterpret_cast<uintptr_t>(counter_addr)));
  m_asm.LD(REG_SCRATCH2, REG_SCRATCH, 0);
  m_asm.ADDI(REG_SCRATCH2, REG_SCRATCH2, 1);
  m_asm.STD(REG_SCRATCH2, REG_SCRATCH, 0);

  m_branch_counters_used++;
}

// ===========================================================================
// GenerateAsmRoutines — emit helper code sequences
// ===========================================================================
//
// These are fused instruction sequences for common operations, placed in the
// code region after the dispatcher.  They are called via BRel from JIT
// blocks for operations too complex or large to inline.
// ===========================================================================

void JitPPC64::GenerateAsmRoutines()
{
  // For now, only emit placeholders — the most common psq paths (type 0 = float)
  // are inlined in CompilePairedLoadStore.
  m_asm_routines.psq_float_load = nullptr;
  m_asm_routines.psq_float_store = nullptr;
  m_asm_routines.psq_u16_load = nullptr;
  m_asm_routines.psq_u16_store = nullptr;
  m_asm_routines.psq_s16_load = nullptr;
  m_asm_routines.psq_s16_store = nullptr;
}

// ===========================================================================
// ScopedTempRegister — RAII temp register management
// ===========================================================================

u32 JitPPC64::ScopedTempRegister::Allocate()
{
  // Try REG_SCRATCH (r0) first
  if (!(m_dirty_mask & 1))
  {
    m_dirty_mask |= 1;
    reg = 0;
    m_allocated = true;
    return reg;
  }
  // Try REG_SCRATCH2 (r11) second
  if (!(m_dirty_mask & 2))
  {
    m_dirty_mask |= 2;
    reg = 11;
    m_allocated = true;
    return reg;
  }
  // Both in use — return 0 (caller should handle this)
  return 0;
}

void JitPPC64::ScopedTempRegister::Release()
{
  if (reg == 0)
    m_dirty_mask &= ~1u;
  else if (reg == 11)
    m_dirty_mask &= ~2u;
  m_allocated = false;
}
