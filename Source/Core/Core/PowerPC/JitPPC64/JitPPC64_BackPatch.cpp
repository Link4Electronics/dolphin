#include "Core/PowerPC/JitPPC64/Jit.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/PowerPC/PowerPC.h"
#include "Common/Logging/Log.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

// ===========================================================================
// Old backpatch table (kept for backward compat with NCE)
// ===========================================================================

struct BackPatchEntry
{
  const u8* code_addr;
  u32 guest_pc;
  u32 guest_address;
  u32 original_inst;
  u32 rd;
};

static std::vector<BackPatchEntry> s_backpatch_entries;

void JitPPC64::InitBackpatch()
{
  s_backpatch_entries.clear();
  s_backpatch_entries.reserve(4096);
  m_fault_to_handler.clear();
}

void JitPPC64::ShutdownBackpatch()
{
  s_backpatch_entries.clear();
}

void JitPPC64::AddBackpatchEntry(const u8* code_addr, u32 guest_pc, u32 guest_address,
                                  u32 original_inst, u32 rd)
{
  s_backpatch_entries.push_back({code_addr, guest_pc, guest_address, original_inst, rd});
}

// ===========================================================================
// TrampolineDispatcher — called from generated trampoline code
//
// The trampoline calls dispatcher directly with the actual register values;
// the dispatcher does NOT rely on ppcState for the store data value
// (avoids stale regcache issue).  Returns loaded value in r3 (r3+r4 for 64-bit).
//
// Signature (ELFv2 ABI, args in r3-r10):
//   r3 = PowerPCState*
//   r4 = EA (effective address)
//   r5 = is_store (0=load, 1=store)
//   r6 = access_size (8, 16, 32, or 64)
//   r7 = rd (PPC dest/src register number)
//   r8 = ra (PPC base register number, for update forms)
//   r9 = store_value (for stores — the actual value from the host register)
//
// access_size indicates the access type:
//   8, 16, 32 → integer GPR access (uses state->gpr[rd])
//   64        → FPU double access (uses state->ps[rd].ps0)
//
// For loads: reads from memory at EA, stores result to state->gpr[rd] (or
//            state->ps[rd] for FPU) and for update forms stores EA to
//            state->gpr[ra]; returns the value.
// For stores: writes store_value to memory at EA and for update forms
//             stores EA to state->gpr[ra]; returns 0.
// ===========================================================================

extern "C" u64 TrampolineDispatcher(PowerPC::PowerPCState* state, u32 ea,
                                     u32 is_store, u32 access_size,
                                     u32 rd, u32 ra, u64 store_value)
{
  auto& memory = Core::System::GetInstance().GetMemory();

  if (is_store)
  {
    switch (access_size)
    {
    case 8:  memory.Write_U8(static_cast<u8>(store_value), ea);  break;
    case 16: memory.Write_U16(static_cast<u16>(store_value), ea); break;
    case 32: memory.Write_U32(static_cast<u32>(store_value), ea); break;
    case 64: memory.Write_U64(store_value, ea);                   break;
    default: break;
    }
    if (ra != 0)
      state->gpr[ra] = ea;
    return 0;
  }
  else
  {
    u64 result = 0;
    switch (access_size)
    {
    case 8:
      result = memory.Read_U8(ea);
      break;
    case 16:
      result = memory.Read_U16(ea);
      break;
    case 32:
      result = memory.Read_U32(ea);
      break;
    case 64:
      result = memory.Read_U64(ea);
      state->ps[rd].ps0 = result;
      break;
    default:
      break;
    }
    if (access_size < 64)
      state->gpr[rd] = static_cast<u32>(result);
    if (ra != 0)
      state->gpr[ra] = ea;
    return result;
  }
}

// ===========================================================================
// JitPPC64Dispatch — asm-friendly block lookup
//
// Called from the generated dispatcher code in CompileDispatcher().
// Signature (ELFv2 ABI, r3 = first arg):
//   u32 pc — the current guest PC (from ppcState after last block executed)
// Returns: normalEntry of the compiled block, or nullptr if Jit() fails.
//
// Uses g_jit_ppc64_instance (set during JitPPC64::Init()) to find or compile
// the block for the given PC.
// ===========================================================================
extern "C" const u8* JitPPC64Dispatch(u32 pc)
{
  auto* jit = g_jit_ppc64_instance;
  if (!jit)
    return nullptr;

  // Update the emulated timebase from CoreTiming before each block dispatch.
  // This ensures CompileMFTB's cached SPR reads return fresh values, and that
  // the timebase advances between loop iterations (even within the same slice,
  // because downcount decreases between dispatches).
  {
    const u64 tb = jit->m_system.GetSystemTimers().GetFakeTimeBase();
    TL(jit->m_ppc_state) = static_cast<u32>(tb);
    TU(jit->m_ppc_state) = static_cast<u32>(tb >> 32);
  }

  JitBlock* block =
      jit->GetBlockCache()->GetBlockFromStartAddress(pc, jit->m_ppc_state.feature_flags);
  if (!block)
  {
    jit->Jit(pc);
    block =
        jit->GetBlockCache()->GetBlockFromStartAddress(pc, jit->m_ppc_state.feature_flags);
  }
  if (block)
  {
    jit->m_ppc_state.downcount -= static_cast<s32>(block->originalSize);
    return block->normalEntry;
  }
  return nullptr;
}

// ===========================================================================
// HandleFault — catches SIGSEGV from fast path, patches to trampoline
//
// On entry: ctx->CTX_NIP points to the faulting instruction.
// On success: patches the fast path range with `b slow_entry`, redirects
//   CTX_NIP to the patched range start, returns true.
// On failure (no matching fast path): falls back to old backpatch table
//   which simply advances CTX_NIP past the instruction.
// ===========================================================================

bool JitPPC64::HandleFault(uintptr_t access_address, SContext* ctx)
{
  const u8* fault_pc = reinterpret_cast<const u8*>(ctx->CTX_NIP);

  // ---- Try trampoline-based handler ----
  auto it = m_fault_to_handler.upper_bound(fault_pc);
  if (it != m_fault_to_handler.end())
  {
    const FastmemArea& area = it->second;
    if (fault_pc >= area.fast_access_code)
    {
      const u32 fast_size = static_cast<u32>(
        reinterpret_cast<const u8*>(it->first) - area.fast_access_code);

      const Common::ScopedJITPageWriteAndNoExecute enable_jit_page_writes;

      // Compute relative branch: b slow_access_code
      ptrdiff_t dist = area.slow_access_code - area.fast_access_code;
      u32 li = (static_cast<u32>(dist >> 2)) & 0x00FFFFFF;
      u32 branch = (18u << 26) | li;

      // Overwrite the entire fast path range
      std::memcpy(const_cast<u8*>(area.fast_access_code), &branch, sizeof(branch));
      const u32 nop = 0x60000000;
      for (u32 i = sizeof(branch); i < fast_size; i += sizeof(nop))
        std::memcpy(const_cast<u8*>(area.fast_access_code + i), &nop, sizeof(nop));

      // Flush icache on PPC64
      __builtin___clear_cache(const_cast<u8*>(area.fast_access_code),
                               const_cast<u8*>(area.fast_access_code + fast_size));

      // Redirect execution to the start of the patched range
      ctx->CTX_NIP = reinterpret_cast<std::uintptr_t>(area.fast_access_code);

      WARN_LOG_FMT(POWERPC, "HandleFault: patched fast path at {} to trampoline at {}",
                   fmt::ptr(area.fast_access_code), fmt::ptr(area.slow_access_code));

      m_fault_to_handler.erase(it);
      return true;
    }
  }

  // ---- Fallback: old backpatch table ----
  auto bp_it = std::find_if(s_backpatch_entries.begin(), s_backpatch_entries.end(),
                            [fault_pc](const BackPatchEntry& e) {
                              return e.code_addr == fault_pc;
                            });

  if (bp_it == s_backpatch_entries.end())
    return false;

  // Advance NIP past the faulting instruction so it re-faults next time.
  ctx->CTX_NIP += 4;

  WARN_LOG_FMT(POWERPC, "HandleFault (legacy): advanced NIP past PC {:08x}",
               bp_it->guest_pc);

  return true;
}
