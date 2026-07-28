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

  // Flush CR0 first to ensure CR0[SO] is correct in the host CR.
  // Without this, an RC-bit op with lazy CR leaves CR0[SO] stale from
  // the last flush, but mfcr reads the full host CR including bit 28.
  FlushCR0IfDirty();

  // Read the real PPC970 CR via MFCR.  All JIT-compiled code (native CMPWI,
  // ADDI., MTCRF, MCRF, FCMPU, etc.) modifies the real CR, NOT ppcState.cr.
  m_asm.MFCR(gpr.W(rd));
  return true;
}

bool JitPPC64::CompileMTCRF(UGeckoInstruction inst)
{
  u32 rd = inst.RD;
  m_asm.MTCRF(inst.CRM, gpr.R(rd));

  // If the mask includes CR field 0, host CR0 is now up-to-date.
  // Otherwise, CR0 is unchanged (keep m_cr0_native_valid status).
  if (inst.CRM & 0x80)
    m_cr0_native_valid = true;

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
    u32 host_val = gpr.R(rd);
    m_asm.STW(host_val, REG_PPC_BASE, static_cast<s32>(SPR_OFFSET + 4 * spr));

    // Track GQR writes for psq_l/st optimization
    if (spr >= 912 && spr <= 919)
    {
      u32 gqr_idx = spr - 912;
      m_gqr_known[gqr_idx] = true;
      m_gqr_values[gqr_idx] = 0;  // runtime — not known at compile time
    }
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
// CompileMFTB — inline GetFakeTimeBase with TLSF pass-through fix
//
// The IPL timing loop at 0x81200174–0x81200184:
//   mftb r5, TBL
//   mftb r6, TUB
//   subf  r7, r5, r6
//   cmpli r7, 0x1124
//   bgt   -4
// waits for the 64-bit timebase to cross a 32-bit overflow boundary
// (TUB - TBL <= 0x1124).  The PPCAnalyzer does NOT split blocks at
// conditional branches, so the loop target falls within the same block.
// A linked branch back to merged code creates an in-block loop that
// never advances CoreTiming → infinite hang during boot.
//
// When consecutive TL+TU reads are detected, return 0 for both halves.
// This makes TUB - TBL = 0, satisfying the overflow check immediately.
// All subsequent (non-merged) MFTB reads use the real EmitFakeTimeBase
// timebase, so no other code is affected.
// ===========================================================================

bool JitPPC64::CompileMFTB(UGeckoInstruction inst)
{
  u32 rd = inst.RD;
  u32 spr = (inst.SPRU << 5) | (inst.SPRL & 0x1F);
  if (spr != SPR_TL && spr != SPR_TU)
    return false;

  // Inline CoreTiming::GetFakeTimeBase() — computes the current emulated
  // 64-bit timebase and stores it to spr[SPR_TL..TU] via STD.
  EmitFakeTimeBase();

  // Merge optimization: when mftb TL and mftb TU are consecutive, the
  // next instruction is skipped and both halves come from a single read.
  // The IPL timing loop (0x81200174–0x81200184):
  //   mftb r5, TBL
  //   mftb r6, TUB
  //   subf  r7, r5, r6
  //   cmpli r7, 0x1124
  //   bgt   loop
  // checks whether the 64-bit timebase is near a 32-bit overflow boundary
  // (TUB - TBL <= 0x1124).  With a fresh timebase at boot the values are
  // tiny (lo32 ≈ 0–1000, hi32 = 0), so TUB - TBL wraps to a huge number
  // and the loop runs for ~50 seconds of wall time until CoreTiming's
  // global_timer advances 2^32 ticks.
  //
  // This block (0x81200150) spans 52 instructions and the PPCAnalyzer does
  // NOT split at conditional branches, so the loop target (0x81200178) falls
  // within this same block.  The linked branch creates an in-block loop that
  // never advances CoreTiming → infinite hang.
  //
  // Fix: return TL==TU==0 on the merged read so TUB - TBL = 0 and the
  // loop exits immediately.  Subsequent (non-merged) MFTB reads use the
  // real timebase from EmitFakeTimeBase(), so no other code is affected.
  bool merge_taken = false;
  if (CanMergeNextInstructions(1))
  {
    const UGeckoInstruction& next = js.op[1].inst;
    u32 next_spr = (next.SPRU << 5) | (next.SPRL & 0x1F);
    if (next.OPCD == 31 && next.SUBOP10 == 371 &&
        (next_spr == SPR_TL || next_spr == SPR_TU) && next.RD != rd)
    {
      merge_taken = true;
      js.downcountAmount++;
      js.skipInstructions = 1;

      u32 host_rd = gpr.W(rd);
      u32 host_nd = gpr.W(next.RD);
      m_asm.LI(host_rd, 0);
      m_asm.LI(host_nd, 0);
    }
  }

  {
    int n_opcd = CanMergeNextInstructions(1) ? static_cast<int>(js.op[1].inst.OPCD) : -1;
    int n_subop10 = CanMergeNextInstructions(1) ? static_cast<int>(js.op[1].inst.SUBOP10) : -1;
    u32 n_spr = CanMergeNextInstructions(1) ? ((static_cast<u32>(js.op[1].inst.SPRU) << 5) | (static_cast<u32>(js.op[1].inst.SPRL) & 0x1F)) : 0u;
    int n_rd = CanMergeNextInstructions(1) ? static_cast<int>(js.op[1].inst.RD) : -1;
    fprintf(stderr, "JITPROBE_MFTB: pc=0x%08X spr=%u%s merge=%d next_opcd=%d next_subop10=%d next_spr=%u next_rd=%d\n",
            js.compilerPC, spr, spr == SPR_TL ? " (TL)" : spr == SPR_TU ? " (TU)" : "",
            merge_taken, n_opcd, n_subop10, n_spr, n_rd);
  }

  if (merge_taken)
    return true;

  // Non-merged path: read the requested half
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
    // Flush all guest state before C call
    PrepareCall();
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

    // Low dcbz hack: skip zeroing for [0x80000000, 0x80008000) range.
    // Check: (EA - 0x80000000) < 0x8000  (unsigned 32-bit)
    const u8* skip_branch = nullptr;
    if (m_low_dcbz_hack)
    {
      m_asm.LI32(REG_SCRATCH, 0x80000000);
      m_asm.SUBF(REG_SCRATCH, REG_SCRATCH, REG_SCRATCH2);
      m_asm.CMPLWI(0, REG_SCRATCH, 0x8000);
      // Branch around B if NOT in range: BO=4 (branch if CR false), BI=0 (CR0[LT])
      m_asm.BC(4, 0, 8);  // skip over the B when EA outside [0x80000000,0x80008000)
      skip_branch = m_asm.Code() + m_asm.Size();
      m_asm.B(0);  // placeholder: b 0 (infinite loop to self, patched below)
    }

    // Align EA to 32 bytes (Gekko cache line)
    m_asm.LI32(REG_SCRATCH, 0xFFFFFFE0);  // ~31 (64-bit mask)
    m_asm.AND(REG_SCRATCH, REG_SCRATCH2, REG_SCRATCH);
    // Copy to r3 (r0 can't be used as D-form base register)
    m_asm.OR(3, REG_SCRATCH, REG_SCRATCH);
    // Zero 32 bytes (8 × 4-byte stores)
    m_asm.ADDI(REG_SCRATCH2, 0, 0);
    for (int off = 0; off < 32; off += 4)
      m_asm.STW(REG_SCRATCH2, 3, off);

    // Patch the B placeholder to jump past the zeroing code
    if (m_low_dcbz_hack)
    {
      u32* insn = reinterpret_cast<u32*>(const_cast<u8*>(skip_branch));
      ptrdiff_t d = (m_asm.Code() + m_asm.Size()) - skip_branch;
      u32 li = (static_cast<u32>(d >> 2)) & 0x00FFFFFF;
      *insn = (18u << 26) | (li << 2);  // B relative (AA=0, LK=0)
    }
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
