#include "Core/PowerPC/JitPPC64/Jit.h"

// ===========================================================================
// CanCompileInstruction — check if we can JIT this instruction
// ===========================================================================

bool CanCompileInstruction(UGeckoInstruction inst)
{
  u32 opcd = inst.OPCD;
  switch (opcd)
  {
  case 7:   // mulli
  case 10:  // cmpli
  case 11:  // cmpi
  case 12:  // addic
  case 13:  // addic.
  case 14:  // addi
  case 15:  // addis
  case 24:  // ori
  case 25:  // oris
  case 26:  // xori
  case 27:  // xoris
  case 28:  // andi.
  case 29:  // andis.
    return true;

  case 3:   // twi
  case 8:   // subfic
    return true;

  case 20:  // rlwimi
  case 21:  // rlwinm
  case 23:  // rlwnm
    return true;

  case 31:  // X-form (opcd=31)
  {
    u32 xo = inst.SUBOP10;
    switch (xo)
    {
    // Integer ALU (fully implemented)
    case 0:   case 4:   case 8:   case 10:  case 11:
    case 19:  case 24:  case 26:  case 28:  case 32:
    case 40:  case 60:  case 75:  case 83:  case 124:
    case 144: case 146: case 235: case 266: case 284:
    case 316: case 339: case 412: case 444:
    case 459: case 467: case 476: case 491:
    case 536: case 792: case 824: case 922: case 954:
    case 104: case 986:
      return true;
    // CA-using ops
    case 136: case 138: case 200: case 202:
    case 232: case 234:
      return true;

    // mftb — compiled via CompileMFTB. The timebase SPRs are refreshed by
    // JitPPC64Dispatch() before every block dispatch (reads CoreTiming's
    // GetFakeTimeBase() and writes to ppcState.spr[SPR_TL/SPR_TU]), so the
    // cached SPR load in CompileMFTB returns the correct emulated value.
    case 371:
      return true;

    // Integer indexed loads/stores
    case 23:  case 55:  case 87:  case 119:
    case 151: case 183: case 215: case 247:
    case 279: case 311: case 343: case 375:
    case 407: case 439:
      return true;

    // Byte-reversed loads/stores
    case 534: case 662: case 790: case 918:
      return true;

    // FPU indexed loads/stores
    case 535: case 567: case 599: case 631:
    case 663: case 695: case 727: case 759:
    case 983:
      return true;

    // Cache/misc (dcbz=1014 emulated with 8 word-stores)
    case 54:  case 86:  case 246: case 278:
    case 470: case 598: case 854: case 982:
    case 1014:
      return true;

    default:
      return false;
    }
  }

  // Integer D-form loads/stores + FPU D-form — Now enabled. The backpatch
  // system (SIGSEGV → EmitBackpatchRoutine → TrampolineDispatcher → HandleFault)
  // handles MMIO accesses correctly.  Before enabling, ensure that:
  //   (a) mcontext_t on the host provides the same register fields as
  //       pt_regs (verified: regs->nip, regs->gpr[] all work),
  //   (b) the trampoline is in the same RWX region as the block code so
  //       HandleFault can patch the fast path with a b-trampoline.
  case 32: case 33: case 34: case 35:
  case 36: case 37: case 38: case 39:
  case 40: case 41: case 42: case 43:
  case 44: case 45:
  case 46: case 47:  // lmw/stmw
  case 48: case 49: case 50: case 51:
  case 52: case 53: case 54: case 55:
    return true;

  case 18:  // b
    return true;
  case 16:  // bc
    return true;

  case 19:  // opcd 19
    return inst.SUBOP10 == 16  ||  // bclr
           inst.SUBOP10 == 528 ||  // bcctr
           inst.SUBOP10 == 0   ||  // mcrf
           inst.SUBOP10 == 150 ||  // isync
           (inst.SUBOP10 >= 33 && inst.SUBOP10 <= 449);  // CR logical

  // FPU (opcd 48-55 handled above)
  case 59:  // FPU single-precision
    return inst.SUBOP5 == 18 || inst.SUBOP5 == 20 || inst.SUBOP5 == 21 ||
           inst.SUBOP5 == 24 || inst.SUBOP5 == 25 ||
           inst.SUBOP5 == 28 || inst.SUBOP5 == 29 ||
           inst.SUBOP5 == 30 || inst.SUBOP5 == 31;
  case 63:  // FPU double-precision
  {
    u32 xo5 = inst.SUBOP5;
    u32 xo10 = inst.SUBOP10;
    if (xo5 != 0)
      return xo5 == 12 || xo5 == 18 || xo5 == 20 || xo5 == 21 ||
             xo5 == 23 || xo5 == 25 || xo5 == 26 ||
             xo5 == 28 || xo5 == 29 || xo5 == 30 || xo5 == 31;
    // X-form
    return xo10 == 0 || xo10 == 32 ||  // fcmpu/fcmpo
           xo10 == 14 || xo10 == 15 ||  // fctiw/fctiwz
           xo10 == 72 || xo10 == 40 || xo10 == 264 || xo10 == 136 ||  // fmr/fneg/fabs/fnabs
           xo10 == 583 || xo10 == 711 || xo10 == 134 ||  // mffs/mtfsf/mtfsfi
           xo10 == 70 || xo10 == 38;  // mtfsb0/mtfsb1
  }

  case 4:  // Paired Singles + psq_lx/stx
  {
    u32 subop6 = inst.SUBOP6;
    if (subop6 == 6 || subop6 == 7 || subop6 == 38 || subop6 == 39)
      return true;
    u32 xo10 = inst.SUBOP10;
    if (xo10 == 40 || xo10 == 72 || xo10 == 136 || xo10 == 264 ||
        xo10 == 528 || xo10 == 560 || xo10 == 592 || xo10 == 624)
      return true;
    u32 xo5 = inst.SUBOP5;
    return xo5 == 10 || xo5 == 11 || xo5 == 12 || xo5 == 13 ||
           xo5 == 14 || xo5 == 15 || xo5 == 18 || xo5 == 20 ||
           xo5 == 21 || xo5 == 23 || xo5 == 24 || xo5 == 25 ||
           xo5 == 26 || xo5 == 28 || xo5 == 29 || xo5 == 30 || xo5 == 31;
  }

  // psq_l/psq_lu/psq_st/psq_stu (QUANTIZE_FLOAT path only)
  case 56: case 57:
  case 60: case 61:
    return true;

  default:
    return false;
  }
}

// ===========================================================================
// CompileInstruction — dispatch table
// ===========================================================================

void JitPPC64::CompileInstruction(PPCAnalyst::CodeOp& op)
{
  const UGeckoInstruction inst = op.inst;
  const u32 opcd = inst.OPCD;

  switch (opcd)
  {
  case 3:   CompileTWI(inst); break;
  case 7:   CompileMULLI(inst); break;
  case 8:   CompileSubfic(inst); break;
  case 10:  CompileCMPLI(inst); break;
  case 11:  CompileCMPI(inst); break;
  case 12:  CompileADDIC(inst); break;
  case 13:  CompileADDIC_(inst); break;
  case 14:  CompileADDI(inst); break;
  case 15:  CompileADDIS(inst); break;
  case 17:  CompileSC(inst); break;
  case 20:  CompileRLWIMI(inst); break;
  case 21:  CompileRLWINM(inst); break;
  case 23:  CompileRLWNM(inst); break;
  case 24:  CompileORI(inst); break;
  case 25:  CompileORIS(inst); break;
  case 26:  CompileXORI(inst); break;
  case 27:  CompileXORIS(inst); break;
  case 28:  CompileANDI_(inst); break;
  case 29:  CompileANDIS_(inst); break;
  case 31:  CompileTable31(inst); break;

  // D-form loads/stores (integer + FPU)
  case 32: case 33: case 34: case 35:
  case 36: case 37: case 38: case 39:
  case 40: case 41: case 42: case 43:
  case 44: case 45:
  case 48: case 49: case 50: case 51:
  case 52: case 53: case 54: case 55:
    CompileLoadStore(inst); break;

  case 46:  CompileLMW(inst); break;
  case 47:  CompileSTMW(inst); break;

  case 4:
  {
    u32 subop6 = inst.SUBOP6;
    if (subop6 == 6 || subop6 == 7 || subop6 == 38 || subop6 == 39)
      CompilePairedLoadStore(inst);
    else
      CompilePairedSingle(inst);
    break;
  }
  case 16:  CompileBC(inst); break;
  case 18:  CompileB(inst); break;

  case 19:
  {
    u32 sub10 = inst.SUBOP10;
    if (sub10 == 150)
      CompileISYNC(inst);
    else if (sub10 == 50)
      CompileRFI(inst);
    else
      CompileOPCD19(inst);
    break;
  }

  case 56: case 57:
  case 60: case 61:
    CompilePairedLoadStore(inst); break;

  case 59:  CompileFPUSingle(inst); break;
  case 63:  CompileFPUDouble(inst); break;

  default:
    break;
  }

  PPCTables::CountInstructionCompile(op.opinfo, js.compilerPC);
}
