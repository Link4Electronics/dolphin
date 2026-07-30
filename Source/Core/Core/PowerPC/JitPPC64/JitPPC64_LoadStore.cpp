#include "Core/PowerPC/JitPPC64/Jit.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#include <functional>

static u32 PS_OFFSET_FR(u32 fr, u32 pair)
{
  return PS_OFFSET + fr * 16 + pair * 8;
}

// Trampoline for calling complex MMIO lambdas from JIT code.
// On PPC64 ELFv2, arguments go in r3-r10:
//   r3 = const std::function* f
//   r4-r10 = args (Core::System& → pointer in r4, u32 in r5, T in r6 for writes)
// Returns T in r3.
template <typename T, typename... Args>
static T CallLambdaTrampoline(const std::function<T(Args...)>* f, Args... args)
{
  return (*f)(args...);
}

// PPC64 ELFv2: r14-r31 and f14-f31 are callee-saved (preserved by calls).
// r0, r3-r10, r12, f0-f13 are volatile (clobbered by calls).
// CR and LR are volatile — must save/restore around calls to C++ code.

// ===========================================================================
// Inline MMIO helpers
//
// When the effective address is known at compile time and falls within the
// MMIO range (0x0C000000-0x0DFFFFFF), we emit code that reads/writes the
// MMIO handler directly instead of going through a SIGSEGV round-trip.
//
// Three handler types exist:
//   Constant  — handler returns a fixed value → LI
//   Direct    — handler reads/writes a host memory pointer → LDR/STR
//   Complex   — handler calls a C++ lambda → fall back to normal backpatch
// ===========================================================================

template <typename T>
class MMIOReadCodeGenerator : public MMIO::ReadHandlingMethodVisitor<T>
{
public:
  MMIOReadCodeGenerator(Core::System* system, PPC64Assembler* asm_, u32 host_reg,
                        u32 address, bool sign_extend)
      : m_system(system), m_asm(asm_), m_host_reg(host_reg),
        m_address(address), m_sign_extend(sign_extend), m_inline_ok(true) {}

  void VisitConstant(T value) override
  {
    m_asm->LI(m_host_reg, static_cast<u32>(value));
  }

  void VisitDirect(const T* addr, u32 mask) override
  {
    // Load the 64-bit host pointer into REG_SCRATCH2 (r11)
    TrampMOVI64(*m_asm, JitPPC64::REG_SCRATCH2, reinterpret_cast<u64>(addr));
    switch (sizeof(T))
    {
    case 1:
      m_asm->LBZ(m_host_reg, JitPPC64::REG_SCRATCH2, 0);
      break;
    case 2:
      m_asm->LHZ(m_host_reg, JitPPC64::REG_SCRATCH2, 0);
      break;
    case 4:
      m_asm->LWZ(m_host_reg, JitPPC64::REG_SCRATCH2, 0);
      break;
    }

    // Apply mask if not all-ones (avoids CR-clobbering andi./andis.)
    const u32 all_ones = (1ULL << (8 * sizeof(T))) - 1;
    const u32 imask = mask;
    if ((all_ones & imask) != all_ones)
    {
      // Build host mask in REG_SCRATCH (r0) using ADDIS/ORI, then AND
      m_asm->ADDIS(JitPPC64::REG_SCRATCH, 0, (imask >> 16) & 0xFFFF);
      m_asm->ORI(JitPPC64::REG_SCRATCH, JitPPC64::REG_SCRATCH, imask & 0xFFFF);
      m_asm->AND(m_host_reg, m_host_reg, JitPPC64::REG_SCRATCH);
    }

    // Sign-extend if needed
    if (m_sign_extend && sizeof(T) < 4)
    {
      if (sizeof(T) == 1)
        m_asm->EXTSB(m_host_reg, m_host_reg);
      else
        m_asm->EXTSH(m_host_reg, m_host_reg);
    }
  }

  // Complex (lambda): save r10, r13, CR, r12; call CallLambdaTrampoline; restore
  void VisitComplex(const std::function<T(Core::System&, u32)>* lambda) override
  {
    // Load real TLS from block frame before saving r13 — ELFv2 uses r13 as
    // thread pointer; we need TLS for any C++ call that might access TLS.
    m_asm->LD(JitPPC64::REG_PHYS_BASE, 1, JitPPC64::TLS_SAVE_OFFSET);
    // Save r10 and r13 (now = TLS) BEFORE sub-frame
    m_asm->STD(10, 1, -32);
    m_asm->STD(JitPPC64::REG_PHYS_BASE, 1, -24);
    // Save CR (via r0) and r12 to sub-frame
    m_asm->MFCR(JitPPC64::REG_SCRATCH);
    m_asm->STD(JitPPC64::REG_SCRATCH, 1, -16);
    m_asm->STD(JitPPC64::REG_PPC_BASE, 1, -8);
    m_asm->STDU(1, 1, -48);

    // r3 = lambda pointer
    TrampMOVI64(*m_asm, 3, reinterpret_cast<u64>(lambda));
    // r4 = &system
    TrampMOVI64(*m_asm, 4, reinterpret_cast<u64>(m_system));
    // r5 = address
    if (m_address <= 0x7FFF)
      m_asm->ADDI(5, 0, static_cast<s32>(m_address));
    else
      m_asm->LI32(5, m_address);

    // r12 = trampoline address
    using TrampRead = T (*)(const std::function<T(Core::System&, u32)>*, Core::System&, u32);
    TrampMOVI64(*m_asm, 12, reinterpret_cast<u64>(static_cast<TrampRead>(&CallLambdaTrampoline<T, Core::System&, u32>)));
    m_asm->MTCTR(12);
    m_asm->BCTRL();

    // Move result (r3) to destination host GPR
    m_asm->MR(m_host_reg, 3);

    // Restore r12 first (r0 must be free for checkpoint), then CR, r13, r10
    m_asm->LD(JitPPC64::REG_PPC_BASE, 1, 40); // r12 saved at old SP-8 = frame SP+40
    m_asm->LD(JitPPC64::REG_SCRATCH, 1, 32);  // CR saved at old SP-16 = frame SP+32
    m_asm->LD(JitPPC64::REG_PHYS_BASE, 1, 24); // r13 restored TLS from sub-frame save
    m_asm->LD(10, 1, 16);                      // r10 saved at old SP-32 = frame SP+16
    m_asm->ADDI(1, 1, 48);                     // tear down sub-frame
    m_asm->MTCRF(0xFF, JitPPC64::REG_SCRATCH); // restore all CR fields

    // Reload mem_ptr from block frame — TLS was restored before call but now
    // we need the physical memory base for fast-path access.
    m_asm->LD(JitPPC64::REG_PHYS_BASE, 1, JitPPC64::PHYS_BASE_SAVE_OFFSET);

    // Reload LR from block prolog's save at SP+16
    m_asm->LD(JitPPC64::REG_SCRATCH, 1, 16);
    m_asm->MTLR(JitPPC64::REG_SCRATCH);

    // Sign-extend if needed (for byte/half-word loads)
    if (m_sign_extend && sizeof(T) < 4)
    {
      if (sizeof(T) == 1)
        m_asm->EXTSB(m_host_reg, m_host_reg);
      else
        m_asm->EXTSH(m_host_reg, m_host_reg);
    }

    m_inline_ok = true;
  }

  bool HandledInline() const { return m_inline_ok; }

private:
  Core::System* m_system;
  PPC64Assembler* m_asm;
  u32 m_host_reg;
  u32 m_address;
  bool m_sign_extend;
  bool m_inline_ok;
};

template <typename T>
class MMIOWriteCodeGenerator : public MMIO::WriteHandlingMethodVisitor<T>
{
public:
  MMIOWriteCodeGenerator(Core::System* system, PPC64Assembler* asm_, u32 src_reg,
                         u32 address)
      : m_system(system), m_asm(asm_), m_src_reg(src_reg),
        m_address(address), m_inline_ok(true) {}

  void VisitNop() override
  {
    // Nothing to emit — the write is ignored
  }

  void VisitDirect(T* addr, u32 mask) override
  {
    TrampMOVI64(*m_asm, JitPPC64::REG_SCRATCH2, reinterpret_cast<u64>(addr));
    const u32 all_ones = (1ULL << (8 * sizeof(T))) - 1;
    const u32 imask = mask;
    if ((all_ones & imask) != all_ones)
    {
      m_asm->ADDIS(JitPPC64::REG_SCRATCH, 0, (imask >> 16) & 0xFFFF);
      m_asm->ORI(JitPPC64::REG_SCRATCH, JitPPC64::REG_SCRATCH, imask & 0xFFFF);
      m_asm->AND(JitPPC64::REG_SCRATCH, m_src_reg, JitPPC64::REG_SCRATCH);
      switch (sizeof(T))
      {
      case 1: m_asm->STB(JitPPC64::REG_SCRATCH, JitPPC64::REG_SCRATCH2, 0); break;
      case 2: m_asm->STH(JitPPC64::REG_SCRATCH, JitPPC64::REG_SCRATCH2, 0); break;
      case 4: m_asm->STW(JitPPC64::REG_SCRATCH, JitPPC64::REG_SCRATCH2, 0); break;
      }
    }
    else
    {
      switch (sizeof(T))
      {
      case 1: m_asm->STB(m_src_reg, JitPPC64::REG_SCRATCH2, 0); break;
      case 2: m_asm->STH(m_src_reg, JitPPC64::REG_SCRATCH2, 0); break;
      case 4: m_asm->STW(m_src_reg, JitPPC64::REG_SCRATCH2, 0); break;
      }
    }
  }

  // Complex (lambda): save r10, r13, CR, r12; call CallLambdaTrampoline; restore
  void VisitComplex(const std::function<void(Core::System&, u32, T)>* lambda) override
  {
    // Load real TLS from block frame before saving r13 — ELFv2 uses r13 as
    // thread pointer; we need TLS for any C++ call that might access TLS.
    m_asm->LD(JitPPC64::REG_PHYS_BASE, 1, JitPPC64::TLS_SAVE_OFFSET);
    // Save r10 and r13 (now = TLS) BEFORE sub-frame
    m_asm->STD(10, 1, -32);
    m_asm->STD(JitPPC64::REG_PHYS_BASE, 1, -24);
    // Save CR (via r0) and r12 to sub-frame
    m_asm->MFCR(JitPPC64::REG_SCRATCH);
    m_asm->STD(JitPPC64::REG_SCRATCH, 1, -16);
    m_asm->STD(JitPPC64::REG_PPC_BASE, 1, -8);
    m_asm->STDU(1, 1, -48);

    // r3 = lambda pointer
    TrampMOVI64(*m_asm, 3, reinterpret_cast<u64>(lambda));
    // r4 = &system
    TrampMOVI64(*m_asm, 4, reinterpret_cast<u64>(m_system));
    // r5 = address
    if (m_address <= 0x7FFF)
      m_asm->ADDI(5, 0, static_cast<s32>(m_address));
    else
      m_asm->LI32(5, m_address);
    // r6 = value
    m_asm->MR(6, m_src_reg);

    // r12 = trampoline address
    using TrampWrite = void (*)(const std::function<void(Core::System&, u32, T)>*, Core::System&, u32, T);
    TrampMOVI64(*m_asm, 12, reinterpret_cast<u64>(static_cast<TrampWrite>(&CallLambdaTrampoline<void, Core::System&, u32, T>)));
    m_asm->MTCTR(12);
    m_asm->BCTRL();

    // Restore r12 first (r0 must be free for checkpoint), then CR, r13, r10
    m_asm->LD(JitPPC64::REG_PPC_BASE, 1, 40); // r12 saved at old SP-8 = frame SP+40
    m_asm->LD(JitPPC64::REG_SCRATCH, 1, 32);  // CR saved at old SP-16 = frame SP+32
    m_asm->LD(JitPPC64::REG_PHYS_BASE, 1, 24); // r13 restored TLS from sub-frame save
    m_asm->LD(10, 1, 16);                      // r10 saved at old SP-32 = frame SP+16
    m_asm->ADDI(1, 1, 48);                     // tear down sub-frame
    m_asm->MTCRF(0xFF, JitPPC64::REG_SCRATCH); // restore all CR fields

    // Reload mem_ptr from block frame — TLS was restored before call but now
    // we need the physical memory base for fast-path access.
    m_asm->LD(JitPPC64::REG_PHYS_BASE, 1, JitPPC64::PHYS_BASE_SAVE_OFFSET);

    // Reload LR from block prolog's save at SP+16
    m_asm->LD(JitPPC64::REG_SCRATCH, 1, 16);
    m_asm->MTLR(JitPPC64::REG_SCRATCH);

    m_inline_ok = true;
  }

  bool HandledInline() const { return m_inline_ok; }

private:
  Core::System* m_system;
  PPC64Assembler* m_asm;
  u32 m_src_reg;
  u32 m_address;
  bool m_inline_ok;
};

// Try to emit inline MMIO load code for a compile-time-known address.
// Returns true if inline code was emitted (no backpatch needed).
static bool TryInlineMMIOLoad(Core::System& system, PPC64Assembler& asm_,
                              u32 host_reg, u32 physical_addr, int access_size,
                              bool sign_extend)
{
  MMIO::Mapping* mmio = system.GetMemory().GetMMIOMapping();
  if (!mmio)
    return false;

  switch (access_size)
  {
  case 8:
  {
    MMIOReadCodeGenerator<u8> gen(&system, &asm_, host_reg, physical_addr, sign_extend);
    mmio->GetHandlerForRead<u8>(physical_addr).Visit(gen);
    return gen.HandledInline();
  }
  case 16:
  {
    MMIOReadCodeGenerator<u16> gen(&system, &asm_, host_reg, physical_addr, sign_extend);
    mmio->GetHandlerForRead<u16>(physical_addr).Visit(gen);
    return gen.HandledInline();
  }
  case 32:
  {
    MMIOReadCodeGenerator<u32> gen(&system, &asm_, host_reg, physical_addr, sign_extend);
    mmio->GetHandlerForRead<u32>(physical_addr).Visit(gen);
    return gen.HandledInline();
  }
  default:
    return false;
  }
}

// Try to emit inline MMIO store code for a compile-time-known address.
// Returns true if inline code was emitted (no backpatch needed).
static bool TryInlineMMIOStore(Core::System& system, PPC64Assembler& asm_,
                               u32 src_reg, u32 physical_addr, int access_size)
{
  MMIO::Mapping* mmio = system.GetMemory().GetMMIOMapping();
  if (!mmio)
    return false;

  switch (access_size)
  {
  case 8:
  {
    MMIOWriteCodeGenerator<u8> gen(&system, &asm_, src_reg, physical_addr);
    mmio->GetHandlerForWrite<u8>(physical_addr).Visit(gen);
    return gen.HandledInline();
  }
  case 16:
  {
    MMIOWriteCodeGenerator<u16> gen(&system, &asm_, src_reg, physical_addr);
    mmio->GetHandlerForWrite<u16>(physical_addr).Visit(gen);
    return gen.HandledInline();
  }
  case 32:
  {
    MMIOWriteCodeGenerator<u32> gen(&system, &asm_, src_reg, physical_addr);
    mmio->GetHandlerForWrite<u32>(physical_addr).Visit(gen);
    return gen.HandledInline();
  }
  default:
    return false;
  }
}

// ===========================================================================
// D-form Load/Store (opcd 32-55)
//
// All use regcache for base/data registers: gpr.R(ra) returns a host
// register with the cached PPC value; gpr.W(rd) allocates a write register.
// This avoids dependency on r12 (ppcState pointer) being correct at runtime.
// ===========================================================================

bool JitPPC64::CompileLoadStore(UGeckoInstruction inst)
{
  u32 opcd = inst.OPCD;
  u32 rd = inst.RD, ra = inst.RA;
  s32 d = static_cast<s32>(static_cast<s16>(inst.SIMM_16));

  // Compute EA into REG_SCRATCH2 (r11) and track whether it's compile-time known
  u32 known_ea = 0;
  bool is_known_ea = false;
  if (ra == 0)
  {
    known_ea = static_cast<u32>(d);
    is_known_ea = true;
    m_asm.ADDI(REG_SCRATCH2, 0, d);
  }
  else if (m_constant_propagation.HasGPR(ra))
  {
    known_ea = m_constant_propagation.GetGPR(ra) + d;
    is_known_ea = true;
    m_asm.LI32(REG_SCRATCH2, static_cast<s32>(known_ea));
  }
  else
  {
    m_asm.ADDI(REG_SCRATCH2, gpr.R(ra), d);
  }

  // Helper: check if known_ea is MMIO and emit inline code; returns true if handled
  auto try_mmio_read = [&](int access_size, bool sign_extend) -> bool
  {
    if (!is_known_ea)
      return false;
    u32 phys = m_system.GetMMU().IsOptimizableMMIOAccess(known_ea, access_size);
    if (!phys)
      return false;
    u32 host_rd = gpr.W(rd);
    return TryInlineMMIOLoad(m_system, m_asm, host_rd, phys, access_size, sign_extend);
  };
  auto try_mmio_store = [&](int access_size) -> bool
  {
    if (!is_known_ea)
      return false;
    u32 phys = m_system.GetMMU().IsOptimizableMMIOAccess(known_ea, access_size);
    if (!phys)
      return false;
    return TryInlineMMIOStore(m_system, m_asm, gpr.R(rd), phys, access_size);
  };

  switch (opcd)
  {
  // Integer loads
  case 32: // lwz
    {
      if (try_mmio_read(32, false))
        return true;
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(32, opcd, rd, 0, host_rd, true);
    }
    return true;
  case 33: // lwzu
    {
      if (try_mmio_read(32, false))
        return true;
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(32, opcd, rd, ra, host_rd, true);
      m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    }
    return true;
  case 34: // lbz
    {
      if (try_mmio_read(8, false))
        return true;
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(8, opcd, rd, 0, host_rd, true);
    }
    return true;
  case 35: // lbzu
    {
      if (try_mmio_read(8, false))
        return true;
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(8, opcd, rd, ra, host_rd, true);
      m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    }
    return true;
  case 40: // lhz
    {
      if (try_mmio_read(16, false))
        return true;
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(16, opcd, rd, 0, host_rd, true);
    }
    return true;
  case 41: // lhzu
    {
      if (try_mmio_read(16, false))
        return true;
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(16, opcd, rd, ra, host_rd, true);
      m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    }
    return true;
  case 42: // lha (signed halfword)
    {
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(16, opcd, rd, 0, host_rd, true);
      m_asm.EXTSH(host_rd, host_rd);
    }
    return true;
  case 43: // lhau
    {
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(16, opcd, rd, ra, host_rd, true);
      m_asm.EXTSH(host_rd, host_rd);
      m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    }
    return true;

  // Integer stores
  case 36: // stw
    if (try_mmio_store(32))
      return true;
    EmitBackpatchRoutine(32, opcd, rd, 0, gpr.R(rd), false);
    return true;
  case 37: // stwu
    if (try_mmio_store(32))
      return true;
    EmitBackpatchRoutine(32, opcd, rd, ra, gpr.R(rd), false);
    m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    return true;
  case 38: // stb
    if (try_mmio_store(8))
      return true;
    EmitBackpatchRoutine(8, opcd, rd, 0, gpr.R(rd), false);
    return true;
  case 39: // stbu
    if (try_mmio_store(8))
      return true;
    EmitBackpatchRoutine(8, opcd, rd, ra, gpr.R(rd), false);
    m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    return true;
  case 44: // sth
    if (try_mmio_store(16))
      return true;
    EmitBackpatchRoutine(16, opcd, rd, 0, gpr.R(rd), false);
    return true;
  case 45: // sthu
    if (try_mmio_store(16))
      return true;
    EmitBackpatchRoutine(16, opcd, rd, ra, gpr.R(rd), false);
    m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    return true;

  // FPU loads (D-form)
  case 48: // lfs
    EmitBackpatchRoutine(32, opcd, rd, 0, 0, true, true);
    m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
    return true;
  case 50: // lfd
    EmitBackpatchRoutine(64, opcd, rd, 0, 0, true, true);
    m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
    return true;
  case 49: // lfsu
    EmitBackpatchRoutine(32, opcd, rd, ra, 0, true, true);
    m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
    m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    return true;
  case 51: // lfdu
    EmitBackpatchRoutine(64, opcd, rd, ra, 0, true, true);
    m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
    m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    return true;

  // FPU stores (D-form)
  case 52: // stfs
    m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
    EmitBackpatchRoutine(32, opcd, rd, 0, 0, false, true);
    return true;
  case 53: // stfsu
    m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
    EmitBackpatchRoutine(32, opcd, rd, ra, 0, false, true);
    m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    return true;
  case 54: // stfd
    m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
    EmitBackpatchRoutine(64, opcd, rd, 0, 0, false, true);
    return true;
  case 55: // stfdu
    m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
    EmitBackpatchRoutine(64, opcd, rd, ra, 0, false, true);
    m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    return true;

  default:
    return false;
  }
}

// ===========================================================================
// Indexed Load/Store (opcd=31, specific XO values)
// ===========================================================================
// EA = (ra ? GPR[ra] : 0) + GPR[rb]

bool JitPPC64::CompileTable31_LoadStore(UGeckoInstruction inst)
{
  u32 xo = inst.SUBOP10;
  u32 rd = inst.RD, ra = inst.RA, rb = inst.RB;

  // Compute EA into REG_SCRATCH2 (r11) and check for compile-time-known address
  u32 known_ea = 0;
  bool is_known_ea = false;
  if (ra == 0)
  {
    if (m_constant_propagation.HasGPR(rb))
    {
      known_ea = m_constant_propagation.GetGPR(rb);
      is_known_ea = true;
      m_asm.LI32(REG_SCRATCH2, static_cast<s32>(known_ea));
    }
    else
    {
      m_asm.MR(REG_SCRATCH2, gpr.R(rb));
    }
  }
  else
  {
    if (m_constant_propagation.HasGPR(ra) && m_constant_propagation.HasGPR(rb))
    {
      known_ea = m_constant_propagation.GetGPR(ra) + m_constant_propagation.GetGPR(rb);
      is_known_ea = true;
      m_asm.LI32(REG_SCRATCH2, static_cast<s32>(known_ea));
    }
    else
    {
      m_asm.ADD(REG_SCRATCH2, gpr.R(ra), gpr.R(rb));
    }
  }

  // Helper: check if known_ea is MMIO and emit inline code; returns true if handled
  auto try_mmio_read = [&](int access_size) -> bool
  {
    if (!is_known_ea)
      return false;
    u32 phys = m_system.GetMMU().IsOptimizableMMIOAccess(known_ea, access_size);
    if (!phys)
      return false;
    u32 host_rd = gpr.W(rd);
    return TryInlineMMIOLoad(m_system, m_asm, host_rd, phys, access_size, false);
  };
  auto try_mmio_store = [&](int access_size) -> bool
  {
    if (!is_known_ea)
      return false;
    u32 phys = m_system.GetMMU().IsOptimizableMMIOAccess(known_ea, access_size);
    if (!phys)
      return false;
    return TryInlineMMIOStore(m_system, m_asm, gpr.R(rd), phys, access_size);
  };

  switch (xo)
  {
  // Integer indexed loads
  case 23:   // lwzx
    {
      if (try_mmio_read(32))
        return true;
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(32, inst.hex, rd, 0, host_rd, true);
    }
    return true;
  case 55:   // lwzux
    {
      if (try_mmio_read(32))
        return true;
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(32, inst.hex, rd, ra, host_rd, true);
      m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    }
    return true;
  case 87:   // lbzx
    {
      if (try_mmio_read(8))
        return true;
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(8, inst.hex, rd, 0, host_rd, true);
    }
    return true;
  case 119:  // lbzux
    {
      if (try_mmio_read(8))
        return true;
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(8, inst.hex, rd, ra, host_rd, true);
      m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    }
    return true;
  case 279:  // lhzx
    {
      if (try_mmio_read(16))
        return true;
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(16, inst.hex, rd, 0, host_rd, true);
    }
    return true;
  case 311:  // lhzux
    {
      if (try_mmio_read(16))
        return true;
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(16, inst.hex, rd, ra, host_rd, true);
      m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    }
    return true;
  case 343:  // lhax
    {
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(16, inst.hex, rd, 0, host_rd, true);
      m_asm.EXTSH(host_rd, host_rd);
    }
    return true;
  case 375:  // lhaux
    {
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(16, inst.hex, rd, ra, host_rd, true);
      m_asm.EXTSH(host_rd, host_rd);
      m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    }
    return true;

  // Integer indexed stores
  case 151:  // stwx
    if (try_mmio_store(32))
      return true;
    EmitBackpatchRoutine(32, inst.hex, rd, 0, gpr.R(rd), false);
    return true;
  case 183:  // stwux
    if (try_mmio_store(32))
      return true;
    EmitBackpatchRoutine(32, inst.hex, rd, ra, gpr.R(rd), false);
    m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    return true;
  case 215:  // stbx
    if (try_mmio_store(8))
      return true;
    EmitBackpatchRoutine(8, inst.hex, rd, 0, gpr.R(rd), false);
    return true;
  case 247:  // stbux
    if (try_mmio_store(8))
      return true;
    EmitBackpatchRoutine(8, inst.hex, rd, ra, gpr.R(rd), false);
    m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    return true;
  case 407:  // sthx
    if (try_mmio_store(16))
      return true;
    EmitBackpatchRoutine(16, inst.hex, rd, 0, gpr.R(rd), false);
    return true;
  case 439:  // sthux
    if (try_mmio_store(16))
      return true;
    EmitBackpatchRoutine(16, inst.hex, rd, ra, gpr.R(rd), false);
    m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    return true;

  // FPU indexed loads
  case 535:  // lfsx
    EmitBackpatchRoutine(32, inst.hex, rd, 0, 0, true, true);
    m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
    return true;
  case 567:  // lfsux
    EmitBackpatchRoutine(32, inst.hex, rd, ra, 0, true, true);
    m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
    m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    return true;
  case 599:  // lfdx
    EmitBackpatchRoutine(64, inst.hex, rd, 0, 0, true, true);
    m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
    return true;
  case 631:  // lfdux
    EmitBackpatchRoutine(64, inst.hex, rd, ra, 0, true, true);
    m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
    m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    return true;

  // FPU indexed stores
  case 663:  // stfsx
    m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
    EmitBackpatchRoutine(32, inst.hex, rd, 0, 0, false, true);
    return true;
  case 695:  // stfsux
    m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
    EmitBackpatchRoutine(32, inst.hex, rd, ra, 0, false, true);
    m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    return true;
  case 727:  // stfdx
    m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
    EmitBackpatchRoutine(64, inst.hex, rd, 0, 0, false, true);
    return true;
  case 759:  // stfdux
    m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
    EmitBackpatchRoutine(64, inst.hex, rd, ra, 0, false, true);
    m_asm.MR(gpr.W(ra), REG_SCRATCH2);
    return true;

  // Byte-reversed loads/stores
  case 534:  // lwbrx
    {
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(32, inst.hex, rd, 0, host_rd, true);
      m_asm.STW(host_rd, 1, -8);
      m_asm.ADDI(REG_SCRATCH, 0, -8);
      m_asm.LWBRX(host_rd, 1, REG_SCRATCH);
    }
    return true;
  case 662:  // stwbrx
    {
      u32 host_rs = gpr.R(rd);
      m_asm.STW(host_rs, 1, -8);
      m_asm.ADDI(REG_SCRATCH, 0, -8);
      m_asm.LWBRX(REG_SCRATCH, 1, REG_SCRATCH);
      EmitBackpatchRoutine(32, inst.hex, rd, 0, REG_SCRATCH, false);
    }
    return true;
  case 790:  // lhbrx
    {
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(16, inst.hex, rd, 0, host_rd, true);
      m_asm.STH(host_rd, 1, -4);
      m_asm.ADDI(REG_SCRATCH, 0, -4);
      m_asm.LHBRX(host_rd, 1, REG_SCRATCH);
    }
    return true;
  case 918:  // sthbrx
    {
      u32 host_rs = gpr.R(rd);
      m_asm.STH(host_rs, 1, -4);
      m_asm.ADDI(REG_SCRATCH, 0, -4);
      m_asm.LHBRX(REG_SCRATCH, 1, REG_SCRATCH);
      EmitBackpatchRoutine(16, inst.hex, rd, 0, REG_SCRATCH, false);
    }
    return true;

  // lwarx — load word and reserve (always succeeds, single-core)
  case 20:  // lwarx
    {
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(32, inst.hex, rd, 0, host_rd, true);
      m_asm.STW(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(RESERVE_ADDR_OFFSET));
      m_asm.LI(REG_SCRATCH, 1);
      m_asm.STB(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(RESERVE_OFFSET));
    }
    return true;

  // stwcx. — store word conditional (always succeeds, single-core)
  case 150: // stwcx.
    {
      gpr.Flush(js.op);
      EmitBackpatchRoutine(32, inst.hex, rd, 0, gpr.R(rd), false);
      // Set CR0 = {LT=0, GT=0, EQ=1, SO=XER_SO}
      m_asm.LBZ(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(XER_SO_OV_OFFSET));
      m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 27, 2, 3);  // SO → u32 bit 28
      m_asm.ORIS(REG_SCRATCH, REG_SCRATCH, 0x2000);       // set u32 bit 29 = EQ=1
      m_asm.MTCRF(0x01, REG_SCRATCH);                      // update CR field 0 only
      m_asm.ADDI(REG_SCRATCH, 0, 0);
      m_asm.STB(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(RESERVE_OFFSET));
    }
    return true;

  // stfiwx — store FPR as integer word
  case 983:  // stfiwx
    {
      if (!jo.fastmem)
        return false;
      const u8* addr = m_asm.Code() + m_asm.Size();
      m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFFSET_FR(rd, 0)));
      m_asm.STD(REG_SCRATCH2, 1, EA_SAVE_OFFSET);
      m_asm.RLDICL(REG_SCRATCH2, REG_SCRATCH2, 0, 32);
      m_asm.ADD(REG_SCRATCH2, REG_SCRATCH2, REG_PHYS_BASE);
      m_asm.STFIWX(0, REG_SCRATCH2, 0);
      AddBackpatchEntry(addr, m_ppc_state.pc, 0, inst.hex, rd);
      return true;
    }

  // eciwx — External Control In Word Indexed (XO=310)
  // On modern hardware treated as regular 32-bit load with backpatch.
  // The EAR check is handled at the interpreter/MMIO level.
  case 310: // eciwx
    {
      u32 host_rd = gpr.W(rd);
      EmitBackpatchRoutine(32, inst.hex, rd, 0, host_rd, true);
    }
    return true;

  // ecowx — External Control Out Word Indexed (XO=438)
  // Same treatment as eciwx but for stores.
  case 438: // ecowx
    EmitBackpatchRoutine(32, inst.hex, rd, 0, gpr.R(rd), false);
    return true;

  default:
    return false;
  }
}

// ===========================================================================
// lmw/stmw — multi-word load/store (opcd 46/47)
// ===========================================================================

bool JitPPC64::CompileLMW(UGeckoInstruction inst)
{
  if (!jo.fastmem)
    return false;
  u32 rt = inst.RD;
  u32 ra = inst.RA;
  s32 d = static_cast<s32>(static_cast<s16>(inst.SIMM_16));

  // Compute guest EA into REG_SCRATCH2, then translate to host address
  if (ra == 0)
    m_asm.ADDI(REG_SCRATCH2, 0, d);
  else if (m_constant_propagation.HasGPR(ra))
    m_asm.LI32(REG_SCRATCH2, m_constant_propagation.GetGPR(ra) + d);
  else
    m_asm.ADDI(REG_SCRATCH2, gpr.R(ra), d);

  m_asm.STD(REG_SCRATCH2, 1, EA_SAVE_OFFSET);
  m_asm.RLDICL(REG_SCRATCH2, REG_SCRATCH2, 0, 32);
  m_asm.ADD(REG_SCRATCH2, REG_SCRATCH2, REG_PHYS_BASE);

  for (u32 r = rt; r <= 31; ++r)
  {
    m_asm.LWZ(gpr.W(r), REG_SCRATCH2, 0);
    if (r < 31)
      m_asm.ADDI(REG_SCRATCH2, REG_SCRATCH2, 4);
  }
  return true;
}

bool JitPPC64::CompileSTMW(UGeckoInstruction inst)
{
  if (!jo.fastmem)
    return false;
  u32 rs = inst.RS;
  u32 ra = inst.RA;
  s32 d = static_cast<s32>(static_cast<s16>(inst.SIMM_16));

  // Compute guest EA into REG_SCRATCH2, then translate to host address
  if (ra == 0)
    m_asm.ADDI(REG_SCRATCH2, 0, d);
  else if (m_constant_propagation.HasGPR(ra))
    m_asm.LI32(REG_SCRATCH2, m_constant_propagation.GetGPR(ra) + d);
  else
    m_asm.ADDI(REG_SCRATCH2, gpr.R(ra), d);

  m_asm.STD(REG_SCRATCH2, 1, EA_SAVE_OFFSET);
  m_asm.RLDICL(REG_SCRATCH2, REG_SCRATCH2, 0, 32);
  m_asm.ADD(REG_SCRATCH2, REG_SCRATCH2, REG_PHYS_BASE);

  for (u32 r = rs; r <= 31; ++r)
  {
    m_asm.STW(gpr.R(r), REG_SCRATCH2, 0);
    if (r < 31)
      m_asm.ADDI(REG_SCRATCH2, REG_SCRATCH2, 4);
  }
  return true;
}
