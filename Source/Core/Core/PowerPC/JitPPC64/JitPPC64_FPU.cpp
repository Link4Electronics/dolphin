#include "Core/PowerPC/JitPPC64/Jit.h"
#include "Core/PowerPC/PowerPC.h"

// Scratch FPR convention:
//   FPR1 = first operand (frA or frB for unary)
//   FPR2 = second operand (frB or frC for fmul)
//   FPR3 = third operand (frC for fmadd/fmsub/fnmsub/fnmadd, frB for fsel)
//   FPR0 = result, store back to guest

// Offset of ps[fr][pair] (pair=0=ps0, pair=1=ps1).
// sizeof(PairedSingle) = 16, sizeof(u64) = 8.
static u32 PS_OFF(u32 fr, u32 pair = 0) { return PS_OFFSET + fr * 16 + pair * 8; }

// ===========================================================================
// opcd 59: FPU single-precision arithmetic
// ===========================================================================

bool JitPPC64::CompileFPUSingle(UGeckoInstruction inst)
{
  u32 xo5 = inst.SUBOP5;
  u32 fd = inst.FD, fa = inst.FA, fb = inst.FB, fc = inst.FC;

  switch (xo5)
  {
  case 18: // fdivs fd, fa, fb
    m_asm.LFD(1, REG_PPC_BASE, static_cast<s32>(PS_OFF(fa)));
    m_asm.LFD(2, REG_PPC_BASE, static_cast<s32>(PS_OFF(fb)));
    m_asm.FDIVS(0, 1, 2);
    break;
  case 20: // fsubs fd, fa, fb
    m_asm.LFD(1, REG_PPC_BASE, static_cast<s32>(PS_OFF(fa)));
    m_asm.LFD(2, REG_PPC_BASE, static_cast<s32>(PS_OFF(fb)));
    m_asm.FSUBS(0, 1, 2);
    break;
  case 21: // fadds fd, fa, fb
    m_asm.LFD(1, REG_PPC_BASE, static_cast<s32>(PS_OFF(fa)));
    m_asm.LFD(2, REG_PPC_BASE, static_cast<s32>(PS_OFF(fb)));
    m_asm.FADDS(0, 1, 2);
    break;
  case 24: // fres fd, fb
    m_asm.LFD(2, REG_PPC_BASE, static_cast<s32>(PS_OFF(fb)));
    m_asm.FRES(0, 2);
    break;
  case 25: // fmuls fd, fa, fc
    m_asm.LFD(1, REG_PPC_BASE, static_cast<s32>(PS_OFF(fa)));
    m_asm.LFD(2, REG_PPC_BASE, static_cast<s32>(PS_OFF(fc)));
    m_asm.FMULS(0, 1, 2);
    break;
  case 28: // fmsubs fd, fa, fc, fb  → fd = fa*fc - fb
    m_asm.LFD(1, REG_PPC_BASE, static_cast<s32>(PS_OFF(fa)));
    m_asm.LFD(2, REG_PPC_BASE, static_cast<s32>(PS_OFF(fc)));
    m_asm.LFD(3, REG_PPC_BASE, static_cast<s32>(PS_OFF(fb)));
    m_asm.FMSUBS(0, 1, 3, 2);
    break;
  case 29: // fmadds fd, fa, fc, fb  → fd = fa*fc + fb
    m_asm.LFD(1, REG_PPC_BASE, static_cast<s32>(PS_OFF(fa)));
    m_asm.LFD(2, REG_PPC_BASE, static_cast<s32>(PS_OFF(fc)));
    m_asm.LFD(3, REG_PPC_BASE, static_cast<s32>(PS_OFF(fb)));
    m_asm.FMADDS(0, 1, 3, 2);
    break;
  case 30: // fnmsubs fd, fa, fc, fb  → fd = -(fa*fc - fb)
    m_asm.LFD(1, REG_PPC_BASE, static_cast<s32>(PS_OFF(fa)));
    m_asm.LFD(2, REG_PPC_BASE, static_cast<s32>(PS_OFF(fc)));
    m_asm.LFD(3, REG_PPC_BASE, static_cast<s32>(PS_OFF(fb)));
    m_asm.FNMSUBS(0, 1, 3, 2);
    break;
  case 31: // fnmadds fd, fa, fc, fb  → fd = -(fa*fc + fb)
    m_asm.LFD(1, REG_PPC_BASE, static_cast<s32>(PS_OFF(fa)));
    m_asm.LFD(2, REG_PPC_BASE, static_cast<s32>(PS_OFF(fc)));
    m_asm.LFD(3, REG_PPC_BASE, static_cast<s32>(PS_OFF(fb)));
    m_asm.FNMADDS(0, 1, 3, 2);
    break;
  default:
    return false;
  }

  m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFF(fd)));
  return true;
}

// ===========================================================================
// opcd 63: FPU double-precision arithmetic
// ===========================================================================

bool JitPPC64::CompileFPUDouble(UGeckoInstruction inst)
{
  u32 xo5  = inst.SUBOP5;
  u32 xo10 = inst.SUBOP10;
  u32 fd = inst.FD, fa = inst.FA, fb = inst.FB, fc = inst.FC;

  // A-form (subop5)
  if (xo5 != 0)
  {
    switch (xo5)
    {
    case 12: // frsp fd, fb
      m_asm.LFD(2, REG_PPC_BASE, static_cast<s32>(PS_OFF(fb)));
      m_asm.FRSP(0, 2);
      break;
    case 18: // fdiv fd, fa, fb
      m_asm.LFD(1, REG_PPC_BASE, static_cast<s32>(PS_OFF(fa)));
      m_asm.LFD(2, REG_PPC_BASE, static_cast<s32>(PS_OFF(fb)));
      m_asm.FDIV(0, 1, 2);
      break;
    case 20: // fsub fd, fa, fb
      m_asm.LFD(1, REG_PPC_BASE, static_cast<s32>(PS_OFF(fa)));
      m_asm.LFD(2, REG_PPC_BASE, static_cast<s32>(PS_OFF(fb)));
      m_asm.FSUB(0, 1, 2);
      break;
    case 21: // fadd fd, fa, fb
      m_asm.LFD(1, REG_PPC_BASE, static_cast<s32>(PS_OFF(fa)));
      m_asm.LFD(2, REG_PPC_BASE, static_cast<s32>(PS_OFF(fb)));
      m_asm.FADD(0, 1, 2);
      break;
    case 23: // fsel fd, fa, fb, fc  → fd = (fa >= 0) ? fc : fb
      m_asm.LFD(1, REG_PPC_BASE, static_cast<s32>(PS_OFF(fa)));
      m_asm.LFD(2, REG_PPC_BASE, static_cast<s32>(PS_OFF(fb)));
      m_asm.LFD(3, REG_PPC_BASE, static_cast<s32>(PS_OFF(fc)));
      m_asm.FSEL(0, 1, 2, 3);
      break;
    case 25: // fmul fd, fa, fc
      m_asm.LFD(1, REG_PPC_BASE, static_cast<s32>(PS_OFF(fa)));
      m_asm.LFD(2, REG_PPC_BASE, static_cast<s32>(PS_OFF(fc)));
      m_asm.FMUL(0, 1, 2);
      break;
    case 26: // frsqrte fd, fb
      m_asm.LFD(2, REG_PPC_BASE, static_cast<s32>(PS_OFF(fb)));
      m_asm.FRSQRTE(0, 2);
      break;
    case 28: // fmsub fd, fa, fc, fb  → fd = fa*fc - fb
      m_asm.LFD(1, REG_PPC_BASE, static_cast<s32>(PS_OFF(fa)));
      m_asm.LFD(2, REG_PPC_BASE, static_cast<s32>(PS_OFF(fc)));
      m_asm.LFD(3, REG_PPC_BASE, static_cast<s32>(PS_OFF(fb)));
      m_asm.FMSUB(0, 1, 3, 2);
      break;
    case 29: // fmadd fd, fa, fc, fb  → fd = fa*fc + fb
      m_asm.LFD(1, REG_PPC_BASE, static_cast<s32>(PS_OFF(fa)));
      m_asm.LFD(2, REG_PPC_BASE, static_cast<s32>(PS_OFF(fc)));
      m_asm.LFD(3, REG_PPC_BASE, static_cast<s32>(PS_OFF(fb)));
      m_asm.FMADD(0, 1, 3, 2);
      break;
    case 30: // fnmsub fd, fa, fc, fb  → fd = -(fa*fc - fb)
      m_asm.LFD(1, REG_PPC_BASE, static_cast<s32>(PS_OFF(fa)));
      m_asm.LFD(2, REG_PPC_BASE, static_cast<s32>(PS_OFF(fc)));
      m_asm.LFD(3, REG_PPC_BASE, static_cast<s32>(PS_OFF(fb)));
      m_asm.FNMSUB(0, 1, 3, 2);
      break;
    case 31: // fnmadd fd, fa, fc, fb  → fd = -(fa*fc + fb)
      m_asm.LFD(1, REG_PPC_BASE, static_cast<s32>(PS_OFF(fa)));
      m_asm.LFD(2, REG_PPC_BASE, static_cast<s32>(PS_OFF(fc)));
      m_asm.LFD(3, REG_PPC_BASE, static_cast<s32>(PS_OFF(fb)));
      m_asm.FNMADD(0, 1, 3, 2);
      break;
    default:
      return false;
    }
    m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFF(fd)));
    return true;
  }

  // X-form (subop10)

  // fmr/fneg/fabs/fnabs: fd = op(fb)
  if (xo10 == 72 || xo10 == 40 || xo10 == 264 || xo10 == 136)
  {
    m_asm.LFD(2, REG_PPC_BASE, static_cast<s32>(PS_OFF(fb)));
    if (xo10 == 72)    m_asm.FMR(0, 2);
    else if (xo10 == 40)   m_asm.FNEG(0, 2);
    else if (xo10 == 264)  m_asm.FABS(0, 2);
    else                   m_asm.FNABS(0, 2);
    m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFF(fd)));
    return true;
  }

  // fctiw/fctiwz
  if (xo10 == 14 || xo10 == 15)
  {
    m_asm.LFD(2, REG_PPC_BASE, static_cast<s32>(PS_OFF(fb)));
    if (xo10 == 14) m_asm.FCTIW(0, 2);
    else            m_asm.FCTIWZ(0, 2);
    m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFF(fd)));
    return true;
  }

  // fcmpu/fcmpo
  if (xo10 == 0 || xo10 == 32)
  {
    m_asm.LFD(1, REG_PPC_BASE, static_cast<s32>(PS_OFF(fa)));
    m_asm.LFD(2, REG_PPC_BASE, static_cast<s32>(PS_OFF(fb)));
    if (xo10 == 0) m_asm.FCMPU(inst.CRFD, 1, 2);
    else           m_asm.FCMPO(inst.CRFD, 1, 2);
    return true;
  }

  // FPSCR
  if (xo10 == 583) { m_asm.MFFS(0); m_asm.STFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFF(fd))); return true; }
  if (xo10 == 711) { m_asm.LFD(0, REG_PPC_BASE, static_cast<s32>(PS_OFF(fb))); m_asm.MTFSF(inst.CRM, 0); return true; }
  if (xo10 == 134) { m_asm.MTFSFI(inst.CRFD, (inst.hex >> 15) & 0xF); return true; }
  if (xo10 == 70)  { m_asm.MTFSB0(fb); return true; }
  if (xo10 == 38)  { m_asm.MTFSB1(fb); return true; }

  return false;
}
