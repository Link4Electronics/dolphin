// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/JitPPC64/Jit.h"

#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/mman.h>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/ConfigManager.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/PPCAnalyst.h"
#include "Core/PowerPC/PPCTables.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

// PPCState field offsets (computed at init from actual struct layout)
u32 PC_OFFSET = 0;
u32 GPR_OFFSET = 0;
u32 CR_OFFSET = 0;
u32 XER_CA_OFFSET = 0;
u32 XER_SO_OV_OFFSET = 0;
u32 DOWNCOUNT_OFFSET = 0;
u32 SPR_OFFSET = 0;
u32 MSR_OFFSET = 0;
u32 PS_OFFSET = 0;

static void InitOffsets(const PowerPC::PowerPCState& state)
{
  const auto base = reinterpret_cast<const char*>(&state);
  PC_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.pc) - base);
  GPR_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.gpr) - base);
  CR_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.cr) - base);
  XER_CA_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.xer_ca) - base);
  XER_SO_OV_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.xer_so_ov) - base);
  DOWNCOUNT_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.downcount) - base);
  SPR_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.spr) - base);
  MSR_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.msr) - base);
  PS_OFFSET = static_cast<u32>(reinterpret_cast<const char*>(&state.ps) - base);
}

// JIT code memory: 32 MB of RWX
static constexpr u32 JIT_CODE_SIZE = 32 * 1024 * 1024;

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
static constexpr u32 FRAME_SIZE = 64;

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
      u32 branch = (18u << 26) | li | (source.call ? 1u << 31 : 0);
      std::memcpy(location, &branch, sizeof(branch));
    }
  }
  else
  {
    auto* jit = static_cast<JitPPC64*>(&m_jit);
    if (jit->m_dispatcher_entry)
    {
      s64 dist = static_cast<s64>(jit->m_dispatcher_entry - location);
      if (dist >= -0x2000000LL && dist <= 0x1FFFFFFLL)
      {
        u32 li = (static_cast<u32>(dist >> 2)) & 0x00FFFFFF;
        u32 branch = (18u << 26) | li;
        std::memcpy(location, &branch, sizeof(branch));
        return;
      }
    }
    u32 blr = 0x4E800020;
    std::memcpy(location, &blr, sizeof(blr));
  }
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

void JitPPC64::Init()
{
  RefreshConfig();
  InitOffsets(m_ppc_state);

  m_code_region = AllocateCodeRegion(JIT_CODE_SIZE);
  m_code_pos = m_code_region;
  m_code_end = m_code_region + JIT_CODE_SIZE;
  m_asm.SetBase(m_code_pos, JIT_CODE_SIZE);

  gpr.SetJit(*this, m_asm, REG_PPC_BASE);

  m_block_cache.Init();
  code_block.m_stats = &js.st;
  code_block.m_gpa = &js.gpa;
  code_block.m_fpa = &js.fpa;
  jo.fastmem_arena = false;
  jo.optimizeGatherPipe = false;

  InitBackpatch();
  CompileDispatcher();

  NOTICE_LOG_FMT(POWERPC, "JITPPC64: initialized (code={}, size={})",
                 fmt::ptr(m_code_region), JIT_CODE_SIZE);
}

void JitPPC64::Shutdown()
{
  ShutdownBackpatch();
  m_block_cache.Shutdown();
  FreeCodeRegion(m_code_region, JIT_CODE_SIZE);
  m_code_region = nullptr;
  m_code_pos = nullptr;
  m_code_end = nullptr;
  m_dispatcher_entry = nullptr;
}

void JitPPC64::ClearCache()
{
  m_block_cache.Clear();
  m_code_pos = m_code_region;
  m_code_end = m_code_region + JIT_CODE_SIZE;
  m_asm.SetBase(m_code_pos, JIT_CODE_SIZE);
  m_dispatcher_entry = nullptr;
  CompileDispatcher();
}

void JitPPC64::CompileDispatcher()
{
  m_asm.SetBase(m_code_pos, static_cast<size_t>(m_code_end - m_code_pos));
  m_dispatcher_entry = m_code_pos;
  m_asm.BLR();
  m_code_pos = m_code_pos + m_asm.Size();
}

// ===========================================================================
// Prolog / Epilog
// ===========================================================================

void JitPPC64::EmitProlog()
{
  m_asm.MFLR(REG_SCRATCH);
  m_asm.STD(REG_SCRATCH, 1, 16);
  m_asm.STDU(1, 1, -static_cast<s32>(FRAME_SIZE));

  u64 addr = reinterpret_cast<u64>(&m_ppc_state);
  if (addr > 0xFFFFFFFFULL)
  {
    u32 hi = static_cast<u32>(addr >> 32);
    u32 lo = static_cast<u32>(addr & 0xFFFFFFFF);
    // Load upper 32 bits (hi) into lower 32 of r12 using ADDIS (sign-extends but
    // RLDICR below will clear the upper 64-bit half, so it's harmless).
    m_asm.ADDIS(REG_PPC_BASE, 0, static_cast<s32>(hi >> 16));
    m_asm.ORI(REG_PPC_BASE, REG_PPC_BASE, hi & 0xFFFF);
    // Shift hi to the upper 32 bits; lower 32 are now zero.
    m_asm.RLDICR(REG_PPC_BASE, REG_PPC_BASE, 32, 31);
    // Add lo using ORIS/ORI — never sign-extends, so works even when
    // lo>>16 or lo&0xFFFF has bit 15 set (which ADDI/ADDIS would sign-extend).
    m_asm.ORIS(REG_PPC_BASE, REG_PPC_BASE, static_cast<u32>(lo >> 16));
    m_asm.ORI(REG_PPC_BASE, REG_PPC_BASE, lo & 0xFFFF);
  }
  else
  {
    m_asm.ORIS(REG_PPC_BASE, 0, static_cast<u32>(addr >> 16));
    m_asm.ORI(REG_PPC_BASE, REG_PPC_BASE, static_cast<u32>(addr & 0xFFFF));
  }

  gpr.Reset();
}

void JitPPC64::EmitEpilog(u32 next_pc)
{
  gpr.Flush();

  m_asm.ADDI(REG_SCRATCH, 0, static_cast<s32>(next_pc));
  m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PC_OFFSET));

  m_asm.ADDI(1, 1, FRAME_SIZE);
  m_asm.LD(REG_SCRATCH, 1, 16);
  m_asm.MTLR(REG_SCRATCH);
  m_asm.BLR();
}

// ===========================================================================
// Load/store helpers
// ===========================================================================

void JitPPC64::LoadGPR(u32 host_reg, u32 guest_reg)
{
  m_asm.LWZ(host_reg, REG_PPC_BASE, static_cast<s32>(GPR_OFFSET + 4 * guest_reg));
}

void JitPPC64::StoreGPR(u32 guest_reg, u32 host_reg)
{
  m_asm.STW(host_reg, REG_PPC_BASE, static_cast<s32>(GPR_OFFSET + 4 * guest_reg));
}

// ===========================================================================
// CR0 update (for RC-bit instructions)
// Clobbers r0, r11
// ===========================================================================

void JitPPC64::EmitCR0Update()
{
  m_asm.EXTSW(REG_SCRATCH2, REG_SCRATCH);
  m_asm.CMPWI(0, REG_SCRATCH2, 0);
  m_asm.MFCR(REG_SCRATCH2);
  m_asm.LBZ(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(XER_SO_OV_OFFSET));
  m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 0, 30, 30);
  m_asm.RLWIMI(REG_SCRATCH2, REG_SCRATCH, 27, 3, 3);
  m_asm.MTCRF(0x80, REG_SCRATCH2);
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
  if (SConfig::GetInstance().bJITNoBlockCache)
    ClearCache();

  const u32 nextPC = analyzer.Analyze(em_address, &code_block, &m_code_buffer,
                                      static_cast<u32>(m_code_buffer.size()));

  if (code_block.m_memory_exception)
    return;

  // Only compile blocks where ALL instructions can be JITted
  for (u32 i = 0; i < code_block.m_num_instructions; ++i)
  {
    if (m_code_buffer[i].skip)
      continue;
    if (!CanCompileInstruction(m_code_buffer[i].inst))
    {
      NOTICE_LOG_FMT(POWERPC, "JITPPC64: can't compile block at {:08x} (instr {:08x} opcd={} at +{})",
                     em_address, m_code_buffer[i].inst.hex, m_code_buffer[i].inst.OPCD, i);
      return;
    }
  }

  size_t estimated_size = code_block.m_num_instructions * 64;
  if (m_code_pos + estimated_size > m_code_end)
  {
    if (clear_cache_and_retry_on_failure)
    {
      ClearCache();
      Jit(em_address, false);
    }
    return;
  }

  JitBlock* b = m_block_cache.AllocateBlock(em_address);
  if (!b)
    return;

  u8* block_start = m_code_pos;
  m_asm.SetBase(m_code_pos, static_cast<size_t>(m_code_end - m_code_pos));

  b->normalEntry = block_start;
  b->near_begin = block_start;
  b->near_end = block_start;

  EmitProlog();

  for (u32 i = 0; i < code_block.m_num_instructions; ++i)
  {
    PPCAnalyst::CodeOp& op = m_code_buffer[i];
    js.compilerPC = op.address;
    js.op = &op;

    if (op.skip)
      continue;

    CompileInstruction(op);
    i += js.skipInstructions;
    js.skipInstructions = 0;
  }

  EmitEpilog(nextPC);

  u8* block_end = m_code_pos + m_asm.Size();
  __builtin___clear_cache(block_start, block_end);

  b->near_end = block_end;

  m_block_cache.FinalizeBlock(*b, jo.enableBlocklink, code_block, m_code_buffer);
  m_code_pos = block_end;

  fprintf(stderr, "JITPROBE: compiled block at 0x%08X, %u instrs, size=%zu bytes, next=0x%08X\n",
          em_address, code_block.m_num_instructions, m_asm.Size(), nextPC);
}

// ===========================================================================
// Run / SingleStep
// ===========================================================================

void JitPPC64::Run()
{
  auto& core_timing = m_system.GetCoreTiming();
  auto& cpu = m_system.GetCPU();
  auto& interpreter = m_system.GetInterpreter();
  u64 probe_count = 0;

  while (cpu.GetState() == CPU::State::Running)
  {
    core_timing.Advance();

    while (m_ppc_state.downcount > 0 && cpu.GetState() == CPU::State::Running)
    {
      JitBlock* block = m_block_cache.GetBlockFromStartAddress(m_ppc_state.pc, m_ppc_state.feature_flags);
      if (!block)
      {
        Jit(m_ppc_state.pc);
        block = m_block_cache.GetBlockFromStartAddress(m_ppc_state.pc, m_ppc_state.feature_flags);
      }

      if (block)
      {
        auto func = reinterpret_cast<void (*)()>(block->normalEntry);
        u32 pc_before = m_ppc_state.pc;
        func();
        u32 pc_after = m_ppc_state.pc;
        m_ppc_state.downcount -= static_cast<int>(block->originalSize);
        if (++probe_count % 1000 == 1)
          fprintf(stderr, "JITPROBE: pc 0x%08X -> 0x%08X (blocks=%llu, dc=%d)\n",
                  pc_before, pc_after,
                  (unsigned long long)probe_count, m_ppc_state.downcount);
      }
      else
      {
        u32 pc_before = m_ppc_state.pc;
        interpreter.SingleStep();
        m_ppc_state.downcount -= 1;
        if (++probe_count % 1000 == 1)
          fprintf(stderr, "JITPROBE: INTERP pc 0x%08X -> 0x%08X (blocks=%llu, dc=%d)\n",
                  pc_before, m_ppc_state.pc,
                  (unsigned long long)probe_count, m_ppc_state.downcount);
      }
    }
  }
}

void JitPPC64::SingleStep()
{
  m_system.GetCoreTiming().Advance();
  m_system.GetInterpreter().SingleStep();
  m_ppc_state.downcount -= 1;
}

// ===========================================================================
// Overrides
// ===========================================================================

void JitPPC64::EraseSingleBlock(const JitBlock& block)
{
  m_block_cache.EraseSingleBlock(block);
}

std::vector<JitBase::MemoryStats> JitPPC64::GetMemoryStats() const { return {}; }
std::size_t JitPPC64::DisassembleNearCode(const JitBlock& block, std::ostream& stream) const { return 0; }
std::size_t JitPPC64::DisassembleFarCode(const JitBlock& block, std::ostream& stream) const { return 0; }

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

void JitPPC64::FallBackToInterpreter(UGeckoInstruction inst) {}
void JitPPC64::DoNothing(UGeckoInstruction inst) {}
void JitPPC64::UnknownInstruction(UGeckoInstruction inst) {}

// Stubs
void JitPPC64::LoadCR(u32 host_reg) {}
void JitPPC64::StoreCR(u32 host_reg) {}
void JitPPC64::EmitCarryFromReg() {}
void JitPPC64::EmitBackpatchSlot() {}
