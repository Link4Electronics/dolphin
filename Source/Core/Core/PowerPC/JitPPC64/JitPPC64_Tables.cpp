#include "Core/PowerPC/JitPPC64/Jit.h"

// ===========================================================================
// CompileInstruction — dispatch table
// ===========================================================================

void JitPPC64::CompileInstruction(PPCAnalyst::CodeOp& op)
{
  const UGeckoInstruction inst = op.inst;
  const u32 opcd = inst.OPCD;

  if (bJITOff)
  {
    FallBackToInterpreter(inst);
    PPCTables::CountInstructionCompile(op.opinfo, js.compilerPC);
    return;
  }

  switch (opcd)
  {
  case 3:
    if (bJITIntegerOff) { FallBackToInterpreter(inst); break; }
    CompileTWI(inst); break;
  case 7:
    if (bJITIntegerOff) { FallBackToInterpreter(inst); break; }
    CompileMULLI(inst); break;
  case 8:
    if (bJITIntegerOff) { FallBackToInterpreter(inst); break; }
    CompileSubfic(inst); break;
  case 10:
    if (bJITIntegerOff) { FallBackToInterpreter(inst); break; }
    CompileCMPLI(inst); break;
  case 11:
    if (bJITIntegerOff) { FallBackToInterpreter(inst); break; }
    CompileCMPI(inst); break;
  case 12:
    if (bJITIntegerOff) { FallBackToInterpreter(inst); break; }
    CompileADDIC(inst); break;
  case 13:
    if (bJITIntegerOff) { FallBackToInterpreter(inst); break; }
    CompileADDIC_(inst); break;
  case 14:
    if (bJITIntegerOff) { FallBackToInterpreter(inst); break; }
    CompileADDI(inst); break;
  case 15:
    if (bJITIntegerOff) { FallBackToInterpreter(inst); break; }
    CompileADDIS(inst); break;
  case 17:  CompileSC(inst); break;
  case 20:
    if (bJITIntegerOff) { FallBackToInterpreter(inst); break; }
    CompileRLWIMI(inst); break;
  case 21:
    if (bJITIntegerOff) { FallBackToInterpreter(inst); break; }
    CompileRLWINM(inst); break;
  case 23:
    if (bJITIntegerOff) { FallBackToInterpreter(inst); break; }
    CompileRLWNM(inst); break;
  case 24:
    if (bJITIntegerOff) { FallBackToInterpreter(inst); break; }
    CompileORI(inst); break;
  case 25:
    if (bJITIntegerOff) { FallBackToInterpreter(inst); break; }
    CompileORIS(inst); break;
  case 26:
    if (bJITIntegerOff) { FallBackToInterpreter(inst); break; }
    CompileXORI(inst); break;
  case 27:
    if (bJITIntegerOff) { FallBackToInterpreter(inst); break; }
    CompileXORIS(inst); break;
  case 28:
    if (bJITIntegerOff) { FallBackToInterpreter(inst); break; }
    CompileANDI_(inst); break;
  case 29:
    if (bJITIntegerOff) { FallBackToInterpreter(inst); break; }
    CompileANDIS_(inst); break;
  case 31:
    if (!CompileTable31(inst))
      FallBackToInterpreter(inst);
    break;

  // D-form loads/stores (integer + FPU)
  case 32: case 33: case 34: case 35:
  case 36: case 37: case 38: case 39:
  case 40: case 41: case 42: case 43:
  case 44: case 45:
  case 48: case 49: case 50: case 51:
  case 52: case 53: case 54: case 55:
    if (bJITLoadStoreOff) { FallBackToInterpreter(inst); break; }
    CompileLoadStore(inst); break;

  case 46:
    if (bJITLoadStoreOff) { FallBackToInterpreter(inst); break; }
    CompileLMW(inst); break;
  case 47:
    if (bJITLoadStoreOff) { FallBackToInterpreter(inst); break; }
    CompileSTMW(inst); break;

  case 4:
  {
    if (bJITPairedOff) { FallBackToInterpreter(inst); break; }
    u32 subop6 = inst.SUBOP6;
    if (subop6 == 6 || subop6 == 7 || subop6 == 38 || subop6 == 39)
      CompilePairedLoadStore(inst);
    else
      CompilePairedSingle(inst);
    break;
  }
  case 16:
    if (bJITBranchOff) { FallBackToInterpreter(inst); break; }
    CompileBC(inst); break;
  case 18:
    if (bJITBranchOff) { FallBackToInterpreter(inst); break; }
    CompileB(inst); break;

  case 19:
  {
    u32 sub10 = inst.SUBOP10;
    if (sub10 == 150)
      CompileISYNC(inst);
    else if (sub10 == 50)
      CompileRFI(inst);
    else if (bJITBranchOff) { FallBackToInterpreter(inst); break; }
    else
      CompileOPCD19(inst);
    break;
  }

  case 56: case 57:
  case 60: case 61:
    if (bJITPairedOff) { FallBackToInterpreter(inst); break; }
    CompilePairedLoadStore(inst); break;

  case 59:
    if (bJITFloatingPointOff) { FallBackToInterpreter(inst); break; }
    CompileFPUSingle(inst); break;
  case 63:
    if (bJITFloatingPointOff) { FallBackToInterpreter(inst); break; }
    CompileFPUDouble(inst); break;

  default:
    FallBackToInterpreter(inst);
    break;
  }

  PPCTables::CountInstructionCompile(op.opinfo, js.compilerPC);
}
