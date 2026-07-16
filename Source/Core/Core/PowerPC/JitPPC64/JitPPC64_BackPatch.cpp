#include "Core/PowerPC/JitPPC64/Jit.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

// ===========================================================================
// BackPatch infrastructure
//
// When a compiled load/store instruction accesses an unmapped address
// (typically MMIO), SIGSEGV fires. We:
//   1. Look up the instruction in our backpatch table
//   2. Rewrite the load/store to call the PPC MMIO/slow path
//   3. Resume execution with the rewritten instruction
//
// For now, backpatching rewrites the instruction to a trap followed by
// a slow-path call. The trap causes the CPU to exit the compiled block
// and return to the Run() loop, which handles the access via the
// interpreter.
// ===========================================================================

struct BackPatchEntry
{
  const u8* code_addr;     // address of compiled instruction
  u32 guest_pc;            // PPC PC
  u32 guest_address;       // accessed address (if known at compile time)
  u32 opcd;                // PPC opcode
  u32 rd;                  // dest/src register
};

static std::vector<BackPatchEntry> s_backpatch_entries;
static bool s_backpatch_initialized = false;

void JitPPC64::InitBackpatch()
{
  s_backpatch_entries.clear();
  s_backpatch_entries.reserve(4096);
  s_backpatch_initialized = true;
}

void JitPPC64::ShutdownBackpatch()
{
  s_backpatch_entries.clear();
  s_backpatch_initialized = false;
}

void JitPPC64::AddBackpatchEntry(const u8* code_addr, u32 guest_pc, u32 guest_address, u32 opcd,
                                  u32 rd)
{
  s_backpatch_entries.push_back({code_addr, guest_pc, guest_address, opcd, rd});
}

bool JitPPC64::HandleFault(uintptr_t access_address, SContext* ctx)
{
  // Find the backpatch entry for this code address
  auto it = std::find_if(s_backpatch_entries.begin(), s_backpatch_entries.end(),
                         [ctx](const BackPatchEntry& e) {
                           return e.code_addr ==
                                  reinterpret_cast<const u8*>(ctx->CTX_NIP);
                         });

  if (it == s_backpatch_entries.end())
    return false;

  ERROR_LOG_FMT(POWERPC, "BackPatch: MMIO access at PC {:08x}, addr {:08x}, opcd {} rd{}",
                it->guest_pc, it->guest_address, it->opcd, it->rd);

  // Rewrite to a trap instruction — the Run() interpreter fallback
  // will handle this access correctly.
  const Common::ScopedJITPageWriteAndNoExecute enable_jit_page_writes;
  u32 trap = 0x7FE00008;
  std::memcpy(const_cast<u8*>(it->code_addr), &trap, sizeof(trap));

  // Return true to indicate the fault was handled
  return true;
}
