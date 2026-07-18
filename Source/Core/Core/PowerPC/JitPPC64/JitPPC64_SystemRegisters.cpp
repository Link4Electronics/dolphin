#include "Core/PowerPC/JitPPC64/Jit.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/CoreTiming.h"

// ===========================================================================
// System register compilers
//
// All GPR access uses regcache (gpr.R/gpr.W) to stay consistent with
// cached values from the integer/load-store compilers.
// ===========================================================================

bool JitPPC64::CompileMFCR(UGeckoInstruction inst)
{
  u32 rd = inst.RD;
  // Read the real PPC970 CR via MFCR.  All JIT-compiled code (native CMPWI,
  // ADDI., MTCRF, MCRF, FCMPU, etc.) modifies the real CR, NOT ppcState.cr.
  m_asm.MFCR(gpr.W(rd));
  return true;
}

bool JitPPC64::CompileMTCRF(UGeckoInstruction inst)
{
  u32 rd = inst.RD;
  m_asm.MTCRF(inst.CRM, gpr.R(rd));
  return true;
}

bool JitPPC64::CompileMFSPR(UGeckoInstruction inst)
{
  u32 rd = inst.RD;
  u32 spr = (inst.SPRU << 5) | (inst.SPRL & 0x1F);
  if (spr < 1024)
  {
    m_asm.LWZ(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(SPR_OFFSET + 4 * spr));
    m_asm.MR(gpr.W(rd), REG_SCRATCH);
    return true;
  }
  return false;
}

bool JitPPC64::CompileMTSPR(UGeckoInstruction inst)
{
  u32 rd = inst.RD;
  u32 spr = (inst.SPRU << 5) | (inst.SPRL & 0x1F);
  if (spr < 1024)
  {
    m_asm.STW(gpr.R(rd), REG_PPC_BASE, static_cast<s32>(SPR_OFFSET + 4 * spr));
    return true;
  }
  return false;
}

bool JitPPC64::CompileMFMSR(UGeckoInstruction inst)
{
  u32 rd = inst.RD;
  m_asm.LWZ(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(MSR_OFFSET));
  m_asm.MR(gpr.W(rd), REG_SCRATCH);
  return true;
}

bool JitPPC64::CompileMTMSR(UGeckoInstruction inst)
{
  u32 rd = inst.RD;
  m_asm.STW(gpr.R(rd), REG_PPC_BASE, static_cast<s32>(MSR_OFFSET));
  return true;
}

// ===========================================================================
// EmitFakeTimeBase — inline CoreTiming::GetFakeTimeBase()
//
// Computes the 64-bit emulated timebase and stores it in REG_SCRATCH (r0)
// and to spr[SPR_TL..SPR_TU] (as a 64-bit store covering both 32-bit slots).
// All register temps use r3/r4 (volatile, outside regcache) plus f0/f1.
// ===========================================================================

void JitPPC64::EmitFakeTimeBase()
{
  auto& ctg = m_system.GetCoreTiming().GetGlobals();
  const s32 TEMP_OFF = 248;

  // r11 = &ctg
  TrampMOVI64(m_asm, REG_SCRATCH2, reinterpret_cast<u64>(&ctg));

  // scaled_downcount = (double)downcount * last_OC_factor_inverted;  [r3 = s64]
  m_asm.LWZ(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(DOWNCOUNT_OFFSET));
  m_asm.STD(REG_SCRATCH, REG_SP, TEMP_OFF);
  m_asm.LFD(0, REG_SP, TEMP_OFF);
  m_asm.FCFID(0, 0);
  m_asm.LFS(1, REG_SCRATCH2,
            static_cast<s32>(offsetof(CoreTiming::Globals, last_OC_factor_inverted)));
  m_asm.FMUL(0, 0, 1);
  m_asm.FCTIDZ(0, 0);
  m_asm.STFD(0, REG_SP, TEMP_OFF);
  m_asm.LD(REG_SCRATCH, REG_SP, TEMP_OFF);
  m_asm.MR(3, REG_SCRATCH);                     // r3 = scaled_downcount

  // r4 = global_timer
  m_asm.LD(4, REG_SCRATCH2,
           static_cast<s32>(offsetof(CoreTiming::Globals, global_timer)));
  // r0 = slice_length (zero-extended int)
  m_asm.LWZ(REG_SCRATCH, REG_SCRATCH2,
            static_cast<s32>(offsetof(CoreTiming::Globals, slice_length)));
  m_asm.SUBF(REG_SCRATCH, 3, REG_SCRATCH);     // r0 = slice_length - scaled_downcount
  m_asm.ADD(REG_SCRATCH, REG_SCRATCH, 4);        // r0 += global_timer
  // r4 = fake_TB_start_ticks
  m_asm.LD(4, REG_SCRATCH2,
           static_cast<s32>(offsetof(CoreTiming::Globals, fake_TB_start_ticks)));
  m_asm.SUBF(REG_SCRATCH, 4, REG_SCRATCH);      // r0 = cycles = r0 - fake_TB_start_ticks
  m_asm.MR(3, REG_SCRATCH);                      // r3 = cycles (preserve)

  // r4 = fake_TB_start_value (load before overwriting r11 with magic constant)
  m_asm.LD(4, REG_SCRATCH2,
           static_cast<s32>(offsetof(CoreTiming::Globals, fake_TB_start_value)));

  // tb = (cycles * 0xAAAAAAAAAAAAAAAB) >> 67 + fake_TB_start_value
  TrampMOVI64(m_asm, REG_SCRATCH2, 0xAAAAAAAAAAAAAAABULL);
  m_asm.MULHDU(REG_SCRATCH, 3, REG_SCRATCH2);   // r0 = high(cycles * magic)
  m_asm.RLDICL(REG_SCRATCH, REG_SCRATCH, 61, 3); // r0 >>= 3
  m_asm.ADD(REG_SCRATCH, REG_SCRATCH, 4);          // r0 += fake_TB_start_value

  // Store 64-bit timebase to spr[SPR_TL] (overwrites both TL and TU)
  m_asm.STD(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(SPR_OFFSET + 4 * SPR_TL));
}

// ===========================================================================
// CompileMFTB — inline GetFakeTimeBase with merge optimization
//
// The IPL timing loop reads both TBL and TUB in consecutive instructions:
//   mftb r5, TBL
//   mftb r6, TUB
//   subf  r7, r5, r6
//   cmpli r7, 0x1124
//   bgt   -4
// Both reads must return the 64-bit timebase, not stale cached values.
// Merge optimization: when mftb TL and mftb TU are adjacent, compute
// the 64-bit timebase once and extract both halves, skipping the second
// instruction (js.skipInstructions=1).
// ===========================================================================

bool JitPPC64::CompileMFTB(UGeckoInstruction inst)
{
  u32 rd = inst.RD;
  u32 spr = (inst.SPRU << 5) | (inst.SPRL & 0x1F);
  if (spr != SPR_TL && spr != SPR_TU)
    return false;

  // Read the pre-computed timebase from spr[] (refreshed by the dispatcher
  // via GetFakeTimeBase() before every block dispatch).  This matches the
  // CachedInterpreter behavior where mftb reads from the SPR array, ensuring
  // the timebase advances at the real emulated rate between dispatches.
  //
  // No inline recomputation needed — EmitFakeTimeBase() would freeze the
  // timebase at the value corresponding to the current downcount, advancing
  // by only ~0.36 ticks per dispatch (far too slow for timebase loops that
  // wait for TBL overflow).
  m_asm.LWZ(REG_SCRATCH, REG_PPC_BASE,
            static_cast<s32>(SPR_OFFSET + 4 * spr));
  m_asm.MR(gpr.W(rd), REG_SCRATCH);
  return true;
}

bool JitPPC64::CompileTW(UGeckoInstruction inst)
{
  return true;
}

// ===========================================================================
// Cache / barrier / misc compiler (opcd=31, various XO)
// ===========================================================================
// These emit native PPC970 instructions where the semantics match Gekko.

bool JitPPC64::CompileMisc(UGeckoInstruction inst)
{
  u32 xo = inst.SUBOP10;

  switch (xo)
  {
  case 54:  // dcbst — must invalidate JIT block cache (self-modifying code)
  case 86:  // dcbf
  case 470: // dcbi
  {
    // Flush register cache before C call (r12 will be clobbered)
    gpr.Flush();
    fpr.Flush();
    // Compute EA into r3
    if (inst.RA == 0)
      m_asm.MR(3, gpr.R(inst.RB));
    else
      m_asm.ADD(3, gpr.R(inst.RA), gpr.R(inst.RB));
    // Move EA to r4 (second arg)
    m_asm.MR(4, 3);
    // r3 = &jit_interface
    TrampMOVI64(m_asm, 3, reinterpret_cast<u64>(&m_system.GetJitInterface()));
    TrampMOVI64(m_asm, 12,
                reinterpret_cast<u64>(&JitInterface::InvalidateICacheLineFromJIT));
    m_asm.MTCTR(12);
    m_asm.BCTRL();
    // Reload r12 (REG_PPC_BASE) from block prolog save at [SP+24]
    m_asm.LD(REG_PPC_BASE, 1, 24);
    return true;
  }
  case 246: // dcbtst
    if (inst.RA == 0)
      m_asm.DCBTST(0, gpr.R(inst.RB));
    else
    {
      m_asm.ADD(REG_SCRATCH2, gpr.R(inst.RA), gpr.R(inst.RB));
      m_asm.DCBTST(REG_SCRATCH2, 0);
    }
    return true;
  case 278: // dcbt
    if (inst.RA == 0)
      m_asm.DCBT(0, gpr.R(inst.RB));
    else
    {
      m_asm.ADD(REG_SCRATCH2, gpr.R(inst.RA), gpr.R(inst.RB));
      m_asm.DCBT(REG_SCRATCH2, 0);
    }
    return true;
  case 598: // sync
    m_asm.SYNC();
    return true;
  case 854: // eieio
    m_asm.EIEIO();
    return true;
  case 982: // icbi — too rare to JIT; falls to interpreter which calls
            // JitInterface::InvalidateICacheLine via the icbi handler
    return false;
  case 1014: // dcbz — PPC970 zeros 128B, not 32B → emulate with 8 word-stores
  {
    // EA = (RA ? GPR[RA] : 0) + GPR[RB]
    if (inst.RA == 0)
      m_asm.MR(REG_SCRATCH2, gpr.R(inst.RB));
    else
    {
      m_asm.ADD(REG_SCRATCH2, gpr.R(inst.RA), gpr.R(inst.RB));
    }
    // Align EA to 32 bytes (Gekko cache line)
    m_asm.RLDICR(REG_SCRATCH, REG_SCRATCH2, 0, 58);
    // Copy to r3 (r0 can't be used as D-form base register)
    m_asm.OR(3, REG_SCRATCH, REG_SCRATCH);
    // Zero 32 bytes (8 × 4-byte stores)
    m_asm.ADDI(REG_SCRATCH2, 0, 0);
    for (int off = 0; off < 32; off += 4)
      m_asm.STW(REG_SCRATCH2, 3, off);
    return true;
  }
  default:
    return false;
  }
}

// ===========================================================================
// isync (opcd=19, SUBOP10=150)
// ===========================================================================

bool JitPPC64::CompileISYNC(UGeckoInstruction inst)
{
  m_asm.ISYNC();
  return true;
}

// ===========================================================================
// sc (opcd=17) — syscall → must deliver EXCEPTION_SYSCALL via interpreter
// ===========================================================================

bool JitPPC64::CompileSC(UGeckoInstruction inst)
{
  return false;
}

// ===========================================================================
// rfi (opcd=19, SUBOP10=50) — return from interrupt → interpreter fallback
// ===========================================================================

bool JitPPC64::CompileRFI(UGeckoInstruction inst)
{
  return false;
}

// ===========================================================================
// Table 31 system register dispatch
// ===========================================================================

bool JitPPC64::CompileTable31_SystemReg(UGeckoInstruction inst)
{
  u32 xo = inst.SUBOP10;

  switch (xo)
  {
  case 19:  // mfcr
    return CompileMFCR(inst);
  case 144: // mtcrf
    return CompileMTCRF(inst);
  case 339: // mfspr
    return CompileMFSPR(inst);
  case 467: // mtspr
    return CompileMTSPR(inst);
  case 83:  // mfmsr
    return CompileMFMSR(inst);
  case 146: // mtmsr
    return CompileMTMSR(inst);
  case 371: // mftb
    return CompileMFTB(inst);
  case 4:   // tw
    return CompileTW(inst);
  default:
    return false;
  }
}
