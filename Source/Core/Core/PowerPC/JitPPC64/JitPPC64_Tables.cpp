#include "Core/PowerPC/JitPPC64/Jit.h"

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
    FallBackToInterpreter(inst);
    break;
  }

  PPCTables::CountInstructionCompile(op.opinfo, js.compilerPC);
}
