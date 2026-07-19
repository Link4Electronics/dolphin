#include "Core/PowerPC/JitPPC64/Jit.h"
#include "Core/PowerPC/PowerPC.h"

// Dolphin's BitField<21,5> extracts the BO field (5 bits at PPC instruction
// bits 6-10) into a 5-bit integer value.  The Dolphin interpreter stores BO
// with the same bit positions as the PPC ISA:
//   bit 4 (value 16) = BO_DONT_CHECK_CONDITION — skip CR check
//   bit 3 (value  8) = BO_BRANCH_IF_TRUE       — CR polarity
//   bit 2 (value  4) = BO_DONT_DECREMENT_FLAG  — skip CTR decrement
//   bit 1 (value  2) = BO_BRANCH_IF_CTR_0      — CTR condition
//   bit 0 (value  1) = reserved
//
// Native PPC bc(BO, BI, BD) semantics:
//   if (!BO[4]) if (CR[BI] != BO[3]) skip  — CR check
//   if (!BO[2]) { CTR--; if ((CTR!=0)!=BO[1]) skip } — CTR check
//
// CR-only checks (no CTR): BO=12 (branch if true), BO=4 (branch if false)
// CTR-only checks (no CR): BO=16 (bdnz), BO=18 (bdz)
//
// CR0 is the only field updated by JIT-ted comparisons.  Non-CR0 fields in
// the native PPC970 CR are stale — EmitCRCheck loads from ppcState.cr which
// always holds the correct emulated value.

// ===========================================================================
// EmitCRCheck — check CR bit BI, set r10=1 if condition fails
//
// For CR0 (BI<4): uses native BC (fast path — CR0 is always up-to-date).
// For non-CR0: loads the field from ppcState.cr, extracts the correct bit
// (SO=bit59, EQ=low32==0, GT=s64>0, LT=bit62), compares via CMPLWI/CMPDI
// back to CR0, then branches on CR0 EQ/GT.
// ===========================================================================

void JitPPC64::EmitCRCheck(u32 bi, bool cr_true)
{
  const u32 field_idx = bi >> 2;

  // Fast path: CR0 (bi 0-3) — native BC is correct when m_cr0_native_valid
  if (field_idx == 0)
  {
    if (bi != 3 && m_cr0_native_valid)
    {
      // CR0[LT,GT,EQ] is correct in host CR from the last RC-bit CMPWI.
      // Native BC reads it directly.  CR0[SO] (bi=3) is NOT safe without
      // flush — defer to ppcState load below.
      m_asm.BC(cr_true ? 12u : 4u, bi, 8);
      m_asm.ADDI(10, 0, 1);
      return;
    }
    // Fall through to ppcState load for stale CR0 or bi=3 (SO)
  }

  // Load ppcState.cr.fields[field_idx]
  const s32 field_off = static_cast<s32>(CR_OFFSET + field_idx * 8);
  m_asm.LD(REG_SCRATCH2, REG_PPC_BASE, field_off);

  switch (bi & 3)
  {
  case 0: // LT — bit 62 of the field value
    m_asm.RLDICL(REG_SCRATCH2, REG_SCRATCH2, 2, 63);  // bit 62 → LSB
    m_asm.CMPLWI(0, REG_SCRATCH2, 0);
    if (cr_true)
      m_asm.BC(12, 1, 8);  // GT = 1 when r11 != 0 = LT set = condition met
    else
      m_asm.BC(12, 2, 8);  // EQ = 1 when r11 == 0 = LT clear = condition met
    break;

  case 1: // GT — (s64)field value > 0
    m_asm.CMPDI(0, REG_SCRATCH2, 0);
    if (cr_true)
      m_asm.BC(12, 1, 8);  // GT = 1 when val > 0 = condition met
    else
      m_asm.BC(4, 1, 8);   // skip if GT == 0 = condition (val <= 0) met
    break;

  case 2: // EQ — lower 32 bits of field value == 0
    m_asm.CLRLDI(REG_SCRATCH2, REG_SCRATCH2, 32);
    m_asm.CMPLWI(0, REG_SCRATCH2, 0);
    if (cr_true)
      m_asm.BC(12, 2, 8);  // EQ = 1 when val == 0 = EQ set = condition met
    else
      m_asm.BC(12, 1, 8);  // GT = 1 when val != 0 = EQ clear = condition met
    break;

  case 3: // SO — bit 59 of the field value
    m_asm.RLDICL(REG_SCRATCH2, REG_SCRATCH2, 5, 63);  // bit 59 → LSB
    m_asm.CMPLWI(0, REG_SCRATCH2, 0);
    if (cr_true)
      m_asm.BC(12, 1, 8);  // GT = 1 when r11 != 0 = SO set = condition met
    else
      m_asm.BC(12, 2, 8);  // EQ = 1 when r11 == 0 = SO clear = condition met
    break;
  }

  m_asm.ADDI(10, 0, 1);  // r10 = 1 (not-taken)
}

// ===========================================================================
// CompileB — unconditional branch (opcd=18)
// ===========================================================================

bool JitPPC64::CompileB(UGeckoInstruction inst)
{
  s32 li = static_cast<s32>(inst.LI << 8) >> 6;
  u32 target = inst.AA ? static_cast<u32>(li) : (js.compilerPC + 4) + li;

  EmitBranchCounter();

  if (inst.LK)
  {
    u32 lr_value = js.compilerPC + 4;
    m_asm.LI32(REG_SCRATCH, lr_value);
    m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(SPR_OFFSET + 4 * 8));
  }

  // When PPCAnalyst inlines the return (skipLRStack), pass bl=false to
  // skip the BLR stack push — the inlined BCLR won't need to pop it.
  const bool use_blr_push = inst.LK && !js.op->skipLRStack;
  WriteExit(target, use_blr_push, js.compilerPC + 4);
  return true;
}

// ===========================================================================
// CompileBC — conditional branch (opcd=16)
// ===========================================================================

bool JitPPC64::CompileBC(UGeckoInstruction inst)
{
  const u32 bo = inst.BO;
  const u32 bi = inst.BI;

  // BO bits (Dolphin interpreter convention — CR and CTR are SWAPPED vs PPC ISA):
  //   bit 4 (16) = skip_cr_check (1=don't check CR)
  //   bit 3 ( 8) = cr_true (1=branch if CR true)
  //   bit 2 ( 4) = skip_ctr_check (1=don't decrement CTR, don't check CTR)
  //   bit 1 ( 2) = ctr_eq_zero (1=branch if CTR==0, 0=branch if CTR!=0)
  //   bit 0 ( 1) = reserved
  const bool skip_cr_check = (bo & 16) != 0;       // bit 4
  const bool cr_true = (bo & 8) != 0;               // bit 3
  const bool skip_ctr_check = (bo & 4) != 0;        // bit 2
  const bool ctr_eq_zero = (bo & 2) != 0;           // bit 1

  EmitBranchCounter();

  gpr.Flush(js.op);
  fpr.Flush(js.op);
  FlushCarry();
  FlushCR0IfDirty();

  const s32 bd = static_cast<s32>(inst.BD << 16) >> 14;
  const u32 target = inst.AA ? static_cast<u32>(bd) : (js.compilerPC + 4) + bd;
  const u32 next_pc = js.compilerPC + 4;

  // Unconditional: both CR and CTR checks skipped
  if (skip_cr_check && skip_ctr_check)
  {
    if (inst.LK && !js.op->skipLRStack)
    {
      m_asm.LI32(REG_SCRATCH, next_pc);
      m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(SPR_OFFSET + 4 * 8));
    }
    WriteExit(target, inst.LK && !js.op->skipLRStack, next_pc);
    return true;
  }

  // Registers used:
  //   r10 = not-taken flag (0 = taken, non-zero = not-taken)
  //   r0  = REG_SCRATCH (final PC value, CR bit extraction scratch)
  //   r11 = REG_SCRATCH2 (temporary for CR field / CTR)

  // r10 = 0 (assume taken)
  m_asm.ADDI(10, 0, 0);

  // -----------------------------------------------------------------------
  // 1. CR check (if needed) — EmitCRCheck loads ppcState.cr for non-CR0
  //    fields where the native PPC970 CR is stale, and falls back to native
  //    BC for CR0 (which the JIT always keeps up-to-date).
  // -----------------------------------------------------------------------
  if (!skip_cr_check)
    EmitCRCheck(bi, cr_true);

  // -----------------------------------------------------------------------
  // 2. CTR check (if needed) — only if CR didn't already fail
  // -----------------------------------------------------------------------
  if (!skip_ctr_check)
  {
    // If r10 != 0 (already not-taken from CR check), skip the CTR decrement
    m_asm.CMPLWI(0, 10, 0);
    m_asm.BC(4, 2, 8);     // skip if EQ==0 (r10 != 0), skip CTR check
    m_asm.MFSPR(REG_SCRATCH2, 9);
    m_asm.ADDI(REG_SCRATCH2, REG_SCRATCH2, -1);
    m_asm.MTSPR(9, REG_SCRATCH2);
    m_asm.CMPLWI(0, REG_SCRATCH2, 0);
    if (ctr_eq_zero)
      m_asm.BC(12, 2, 8);   // BEQ: if EQ=1 (CTR==0 → condition met), skip ADDI
    else
      m_asm.BC(4, 2, 8);   // BNE: if EQ=0 (CTR!=0 → condition met), skip ADDI
    m_asm.ADDI(10, 0, 1);   // r10 = 1 (not-taken — CTR condition failed)
  }

  // -----------------------------------------------------------------------
  // 3. LR save (LK=1 case) — skip when return is inlined
  // -----------------------------------------------------------------------
  if (inst.LK && !js.op->skipLRStack)
  {
    m_asm.LI32(REG_SCRATCH2, next_pc);
    m_asm.STW(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(SPR_OFFSET + 4 * 8));
  }

  // -----------------------------------------------------------------------
  // 4. Final decision: r10 == 0 → branch taken (use target)
  //                    r10 != 0 → not taken (use next_pc)
  // -----------------------------------------------------------------------
  m_asm.LI32(REG_SCRATCH, next_pc);
  m_asm.CMPWI(0, 10, 0);
  m_asm.BC(4, 2, 8);       // skip if EQ==0 (r10 != 0 → keep next_pc)
  m_asm.LI32(REG_SCRATCH, target);

  m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PC_OFFSET));

  // Decrement downcount — must use r11 (REG_SCRATCH2), NOT r0 (REG_SCRATCH).
  // On PPC, addi rD, 0, SI means rD = 0 + SI (RA=0 → source is literal zero).
  // So addi r0, r0, -N produces -N, discarding the LWZ result.
  m_asm.LWZ(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(DOWNCOUNT_OFFSET));
  m_asm.ADDI(REG_SCRATCH2, REG_SCRATCH2, -static_cast<s32>(js.downcountAmount));
  m_asm.STW(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(DOWNCOUNT_OFFSET));

  // Jump to dispatcher_lite — record linkData so WriteLinkBlock can patch
  // this BRel to jump directly to the destination block once it's compiled.
  // Target is static (known at compile time), so linking is valid.
  JitBlock::LinkData linkData;
  linkData.exitAddress = target;
  linkData.linkStatus = false;
  linkData.call = false;
  linkData.exitPtrs = m_asm.Code() + m_asm.Size();
  m_asm.BRel(m_dispatcher_lite);
  js.curBlock->linkData.push_back(linkData);
  return true;
}

// ===========================================================================
// opcd 19 dispatcher
// ===========================================================================

bool JitPPC64::CompileOPCD19(UGeckoInstruction inst)
{
  u32 subop10 = inst.SUBOP10;
  switch (subop10)
  {
  case 0:   return CompileMCRF(inst);
  case 16:  return CompileBCLR(inst);
  case 18:  return CompileBCLR(inst);   // bclrl (bclr with LK=1)
  case 528: return CompileBCCTR(inst);
  case 530: return CompileBCCTR(inst);  // bcctrl (bcctr with LK=1)
  case 150: return true;  // isync — no-op
  default:
    if (subop10 >= 33 && subop10 <= 449)
      return CompileCRLogical(inst);
    return false;
  }
}

// ===========================================================================
// MCRF — move CR field (native PPC970 instruction)
// ===========================================================================

bool JitPPC64::CompileMCRF(UGeckoInstruction inst)
{
  // If reading from CR0 (CRFS==0), its host value must be flushed first.
  if (inst.CRFS == 0)
    FlushCR0IfDirty();

  m_asm.MCRF(inst.CRFD, inst.CRFS);

  // If writing to CR0 (CRFD==0), the copied value is correct in the host CR.
  if (inst.CRFD == 0)
    m_cr0_native_valid = true;

  return true;
}

// ===========================================================================
// CR logical ops — crand, cror, crxor, etc. (native PPC970 instructions)
// ===========================================================================

bool JitPPC64::CompileCRLogical(UGeckoInstruction inst)
{
  // Any operand in CR0 (bits 0-3) requires a flush first.
  const u32 bd = inst.CRBD, ba = inst.CRBA, bb = inst.CRBB;
  if (bd < 4 || ba < 4 || bb < 4)
    FlushCR0IfDirty();

  switch (inst.SUBOP10)
  {
  case 33:   m_asm.CRNOR(bd, ba, bb); break;
  case 129:  m_asm.CRANDC(bd, ba, bb); break;
  case 193:  m_asm.CRXOR(bd, ba, bb); break;
  case 225:  m_asm.CRNAND(bd, ba, bb); break;
  case 257:  m_asm.CRAND(bd, ba, bb); break;
  case 289:  m_asm.CREQV(bd, ba, bb); break;
  case 417:  m_asm.CRORC(bd, ba, bb); break;
  case 449:  m_asm.CROR(bd, ba, bb); break;
  default:   return false;
  }

  // If the result goes into CR0, it's now correct in the host CR.
  if (bd < 4)
    m_cr0_native_valid = true;

  return true;
}

// ===========================================================================
// BCLR / BCLRL — branch conditional to link register
// ===========================================================================

bool JitPPC64::CompileBCLR(UGeckoInstruction inst)
{
  const u32 bo = inst.BO;
  const u32 bi = inst.BI;

  const bool skip_cr_check = (bo & 16) != 0;       // bit 4
  const bool cr_true = (bo & 8) != 0;               // bit 3
  const bool skip_ctr_check = (bo & 4) != 0;        // bit 2
  const bool ctr_eq_zero = (bo & 2) != 0;           // bit 1

  EmitBranchCounter();

  gpr.Flush(js.op);
  fpr.Flush(js.op);
  FlushCarry();
  FlushCR0IfDirty();
  const u32 next_pc = js.compilerPC + 4;

  // Unconditional: both CR and CTR checks skipped
  if (skip_cr_check && skip_ctr_check)
  {
    if (inst.LK)
    {
      m_asm.LI32(REG_SCRATCH, next_pc);
      m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(SPR_OFFSET + 4 * 8));
    }

    if (js.op->skipLRStack)
    {
      // Inlined return: the preceding BL didn't push to the BLR stack.
      // Just read LR from SPR and dispatch to dispatcher_lite.
      m_asm.LWZ(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(SPR_OFFSET + 4 * 8));
      m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 0, 0, 29);
      m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PC_OFFSET));
      // Use r11 for downcount — PPC addi with RA=0 uses literal 0.
      m_asm.LWZ(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(DOWNCOUNT_OFFSET));
      m_asm.ADDI(REG_SCRATCH2, REG_SCRATCH2, -static_cast<s32>(js.downcountAmount));
      m_asm.STW(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(DOWNCOUNT_OFFSET));
      m_asm.BRel(m_dispatcher_lite);
    }
    else
    {
      WriteBLRExit();
    }
    return true;
  }

  // r10 = not-taken flag (0 = taken, non-zero = not-taken)
  m_asm.ADDI(10, 0, 0);

  // -----------------------------------------------------------------------
  // 1. CR check (if needed)
  // -----------------------------------------------------------------------
  if (!skip_cr_check)
    EmitCRCheck(bi, cr_true);

  // -----------------------------------------------------------------------
  // 2. CTR check (if needed) — only if CR didn't already fail
  // -----------------------------------------------------------------------
  if (!skip_ctr_check)
  {
    m_asm.CMPLWI(0, 10, 0);
    m_asm.BC(4, 2, 8);     // skip if EQ==0 (r10 != 0), skip CTR check
    m_asm.MFSPR(REG_SCRATCH2, 9);
    m_asm.ADDI(REG_SCRATCH2, REG_SCRATCH2, -1);
    m_asm.MTSPR(9, REG_SCRATCH2);
    m_asm.CMPLWI(0, REG_SCRATCH2, 0);
    if (ctr_eq_zero)
      m_asm.BC(12, 2, 8);   // BEQ: if CTR==0, skip ADDI
    else
      m_asm.BC(4, 2, 8);   // BNE: if CTR!=0, skip ADDI
    m_asm.ADDI(10, 0, 1);   // r10 = 1 (CTR condition failed)
  }

  // -----------------------------------------------------------------------
  // 3. LR save (LK=1 case) — skip when return is inlined
  // -----------------------------------------------------------------------
  if (inst.LK && !js.op->skipLRStack)
  {
    m_asm.LI32(REG_SCRATCH2, next_pc);
    m_asm.STW(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(SPR_OFFSET + 4 * 8));
  }

  // -----------------------------------------------------------------------
  // 4. Final: r10 == 0 → branch to LR; r10 != 0 → next_pc
  // -----------------------------------------------------------------------
  m_asm.LI32(REG_SCRATCH, next_pc);
  m_asm.CMPWI(0, 10, 0);
  m_asm.BC(4, 2, 8);       // skip if EQ==0 (r10 != 0), skip LR load
  m_asm.LWZ(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(SPR_OFFSET + 4 * 8));
  m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 0, 0, 29);

  m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PC_OFFSET));

  // Decrement downcount and jump to dispatcher_lite (dynamic target)
  // Use r11 (REG_SCRATCH2), not r0 — PPC addi with RA=0 uses literal 0.
  m_asm.LWZ(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(DOWNCOUNT_OFFSET));
  m_asm.ADDI(REG_SCRATCH2, REG_SCRATCH2, -static_cast<s32>(js.downcountAmount));
  m_asm.STW(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(DOWNCOUNT_OFFSET));
  m_asm.BRel(m_dispatcher_lite);
  return true;
}

// ===========================================================================
// BCCTR / BCCTRL — branch conditional to counter register
// ===========================================================================

bool JitPPC64::CompileBCCTR(UGeckoInstruction inst)
{
  const u32 bo = inst.BO;
  const u32 bi = inst.BI;

  const bool skip_cr_check = (bo & 16) != 0;       // bit 4
  const bool cr_true = (bo & 8) != 0;               // bit 3

  // bcctr never decrements CTR (the assert in the interpreter verifies this,
  // and BO_DONT_DECREMENT_FLAG / skip_ctr_check must always be set).

  EmitBranchCounter();

  gpr.Flush(js.op);
  fpr.Flush(js.op);
  FlushCarry();
  FlushCR0IfDirty();
  const u32 next_pc = js.compilerPC + 4;

  // LR save (LK=1) — skip when return is inlined
  if (inst.LK && !js.op->skipLRStack)
  {
    m_asm.LI32(REG_SCRATCH, next_pc);
    m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(SPR_OFFSET + 4 * 8));
  }

  // -----------------------------------------------------------------------
  // 1. CR check (if needed)
  // -----------------------------------------------------------------------
  if (!skip_cr_check)
  {
    // r10 = not-taken flag (0 = taken, 1 = not-taken)
    m_asm.ADDI(10, 0, 0);

    EmitCRCheck(bi, cr_true);

    // Fall through: if CR condition failed, r10=1 from EmitCRCheck
    // Select PC: if r10 == 0 (taken), use CTR; else use next_pc
    m_asm.LI32(REG_SCRATCH, next_pc);
    m_asm.CMPWI(0, 10, 0);
    m_asm.BC(4, 2, 8);       // skip if EQ==0 (r10 != 0), skip CTR read
    m_asm.MFSPR(REG_SCRATCH, 9);
    m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 0, 0, 29);

    m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PC_OFFSET));

    // Decrement downcount and jump to dispatcher_lite (dynamic target)
    // Use r11 (REG_SCRATCH2), not r0 — PPC addi with RA=0 uses literal 0.
    m_asm.LWZ(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(DOWNCOUNT_OFFSET));
    m_asm.ADDI(REG_SCRATCH2, REG_SCRATCH2, -static_cast<s32>(js.downcountAmount));
    m_asm.STW(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(DOWNCOUNT_OFFSET));
    m_asm.BRel(m_dispatcher_lite);
    return true;
  }

  // -----------------------------------------------------------------------
  // 2. No CR check — unconditional bcctr/bcctrl (reachable when skip_cr_check)
  // -----------------------------------------------------------------------
  m_asm.MFSPR(REG_SCRATCH, 9);
  m_asm.RLWINM(REG_SCRATCH, REG_SCRATCH, 0, 0, 29);
  m_asm.STW(REG_SCRATCH, REG_PPC_BASE, static_cast<s32>(PC_OFFSET));

  // Decrement downcount and jump to dispatcher_lite (dynamic target)
  // Use r11 (REG_SCRATCH2), not r0 — PPC addi with RA=0 uses literal 0.
  m_asm.LWZ(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(DOWNCOUNT_OFFSET));
  m_asm.ADDI(REG_SCRATCH2, REG_SCRATCH2, -static_cast<s32>(js.downcountAmount));
  m_asm.STW(REG_SCRATCH2, REG_PPC_BASE, static_cast<s32>(DOWNCOUNT_OFFSET));
  m_asm.BRel(m_dispatcher_lite);
  return true;
}
