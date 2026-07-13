// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// PPC64 ELFv2 assembly trampolines for Native Code Execution
// Target: PPC64 big-endian, 64 KB pages, ELFv2

// Guest register context layout (must match GuestRegs in Jit.h)
#define GREGS_GPR   0       // u32 gpr[32] at offset 0
#define GREGS_PC    128     // u32 pc
#define GREGS_CR    132     // u32 cr
#define GREGS_LR    136     // u32 lr
#define GREGS_CTR   140     // u32 ctr
#define GREGS_XER   144     // u32 xer

// Native context layout (must match NativeContext in Jit.h)
#define NCTX_R2     0
#define NCTX_R13    8
#define NCTX_R1     16
#define NCTX_RETURN 24
#define NCTX_CR     32
#define NCTX_GPR14  40
#define NCTX_GPR15  48
#define NCTX_GPR16  56
#define NCTX_GPR17  64
#define NCTX_GPR18  72
#define NCTX_GPR19  80
#define NCTX_GPR20  88
#define NCTX_GPR21  96
#define NCTX_GPR22  104
#define NCTX_GPR23  112
#define NCTX_GPR24  120
#define NCTX_GPR25  128
#define NCTX_GPR26  136
#define NCTX_GPR27  144
#define NCTX_GPR28  152
#define NCTX_GPR29  160
#define NCTX_GPR30  168
#define NCTX_GPR31  176
#define NCTX_GUEST_CTR  184
#define NCTX_OVERWRITE_CTR 188

    .section .text, "ax", @progbits

// ---------------------------------------------------------------------------
// JitPPC64EnterGuest
//
// Called from JitPPC64::Run() to enter native guest execution.
//
// Arguments (ELFv2 ABI):
//   r3 = pointer to GuestRegs (guest register state to restore)
//   r4 = pointer to NativeContext (host context save area)
// ---------------------------------------------------------------------------

    .globl JitPPC64EnterGuest
    .type JitPPC64EnterGuest, @function

JitPPC64EnterGuest:
    // ---- Phase 0: Diagnostics ----
    // Write stage=1 to 0x100003FEC to confirm trampoline entry.
    // Use r12 as temp, the address fits in oris+addi.
    li      %r12,   0
    oris    %r12,   %r12,   0x1000
    sldi    %r12,   %r12,   4
    addi    %r12,   %r12,   0x3FEC
    li      %r11,   1
    stw     %r11,   0(%r12)

    // ---- Phase 1: Save host context ----
    // These use r3 (GuestRegs*) and r4 (NativeContext*) before they get
    // overwritten by guest register values.

    // Save host R1 first (before pushing our own frame)
    std     %r1,    NCTX_R1(%r4)

    // Push host stack frame for saved critical values
    // Frame: 32 bytes header + 32 bytes for 4 saved values + 32 bytes scratch = 96
    stdu    %r1,    -96(%r1)

    // Save NativeContext* and GuestRegs* to host stack for Phase 3 use
    // (after all GPRs are overwritten by guest values).
    std     %r4,    80(%r1)      // save NativeContext*
    std     %r3,    72(%r1)      // save GuestRegs*

    // Save critical GuestRegs values to host stack while r3,r4 are valid.
    // We'll reload these at the end after all GPRs are overwritten.
    // Saved values (using R1-relative addressing, since R1 stays host):
    //   32(%r1)  = guest r0     (saved from GuestRegs.gpr[0])
    //   40(%r1)  = guest r1     (saved from GuestRegs.gpr[1])
    //   48(%r1)  = guest r12    (saved from GuestRegs.gpr[12])
    //   56(%r1)  = guest PC     (saved from GuestRegs.pc)

    lwz     %r0,    GREGS_GPR(%r3)       // guest r0
    stw     %r0,    32(%r1)
    lwz     %r0,    GREGS_GPR + 4(%r3)   // guest r1
    stw     %r0,    40(%r1)
    lwz     %r0,    GREGS_GPR + 48(%r3)  // guest r12 (12 * 4 = 48)
    stw     %r0,    48(%r1)
    lwz     %r0,    GREGS_PC(%r3)        // guest PC
    stw     %r0,    56(%r1)

    // Save remaining host context using r4 (NativeContext*)
    std     %r2,    NCTX_R2(%r4)
    std     %r13,   NCTX_R13(%r4)

    mflr    %r0
    std     %r0,    NCTX_RETURN(%r4)

    mfcr    %r0
    std     %r0,    NCTX_CR(%r4)

    std     %r14,   NCTX_GPR14(%r4)
    std     %r15,   NCTX_GPR15(%r4)
    std     %r16,   NCTX_GPR16(%r4)
    std     %r17,   NCTX_GPR17(%r4)
    std     %r18,   NCTX_GPR18(%r4)
    std     %r19,   NCTX_GPR19(%r4)
    std     %r20,   NCTX_GPR20(%r4)
    std     %r21,   NCTX_GPR21(%r4)
    std     %r22,   NCTX_GPR22(%r4)
    std     %r23,   NCTX_GPR23(%r4)
    std     %r24,   NCTX_GPR24(%r4)
    std     %r25,   NCTX_GPR25(%r4)
    std     %r26,   NCTX_GPR26(%r4)
    std     %r27,   NCTX_GPR27(%r4)
    std     %r28,   NCTX_GPR28(%r4)
    std     %r29,   NCTX_GPR29(%r4)
    std     %r30,   NCTX_GPR30(%r4)
    std     %r31,   NCTX_GPR31(%r4)

    // Diagnostic: stage=2 (host context saved)
    li      %r11,   0
    oris    %r11,   %r11,   0x1000
    sldi    %r11,   %r11,   4
    addi    %r11,   %r11,   0x3FEC
    li      %r12,   2
    stw     %r12,   0(%r11)

    // ---- Phase 2: Restore guest register state from GuestRegs (r3) ----
    // r12 will hold the GuestRegs base pointer during restores.
    // After restores, r12 will be overwritten with values from the stack
    // (at the very end), so we DON'T try to restore guest r12 here.
    mr      %r12,   %r3

    // Restore CR, LR, CTR, XER from GuestRegs
    lwz     %r0,    GREGS_CR(%r12)
    mtcr    %r0

    lwz     %r0,    GREGS_LR(%r12)
    mtlr    %r0

    lwz     %r0,    GREGS_CTR(%r12)
    mtctr   %r0
    // Save original guest CTR to NativeContext before the overwrite in Phase 3
    // (r4 is still the NativeContext* at this point)
    stw     %r0,    NCTX_GUEST_CTR(%r4)

    lwz     %r0,    GREGS_XER(%r12)
    mtxer   %r0

    // Restore GPRs 0..31 (explicitly skip r1, r12 which are handled last).
    // r12 remains as GuestRegs base pointer throughout.
    lwz     %r0,   GREGS_GPR +  0(%r12)
    // r1  — restored at end from stack
    lwz     %r2,   GREGS_GPR +  8(%r12)
    lwz     %r3,   GREGS_GPR + 12(%r12)
    lwz     %r4,   GREGS_GPR + 16(%r12)
    lwz     %r5,   GREGS_GPR + 20(%r12)
    lwz     %r6,   GREGS_GPR + 24(%r12)
    lwz     %r7,   GREGS_GPR + 28(%r12)
    lwz     %r8,   GREGS_GPR + 32(%r12)
    lwz     %r9,   GREGS_GPR + 36(%r12)
    lwz     %r10,  GREGS_GPR + 40(%r12)
    lwz     %r11,  GREGS_GPR + 44(%r12)
    // r12 — restored at end from stack (still holds GuestRegs base for now)
    lwz     %r13,  GREGS_GPR + 52(%r12)
    lwz     %r14,  GREGS_GPR + 56(%r12)
    lwz     %r15,  GREGS_GPR + 60(%r12)
    lwz     %r16,  GREGS_GPR + 64(%r12)
    lwz     %r17,  GREGS_GPR + 68(%r12)
    lwz     %r18,  GREGS_GPR + 72(%r12)
    lwz     %r19,  GREGS_GPR + 76(%r12)
    lwz     %r20,  GREGS_GPR + 80(%r12)
    lwz     %r21,  GREGS_GPR + 84(%r12)
    lwz     %r22,  GREGS_GPR + 88(%r12)
    lwz     %r23,  GREGS_GPR + 92(%r12)
    lwz     %r24,  GREGS_GPR + 96(%r12)
    lwz     %r25,  GREGS_GPR + 100(%r12)
    lwz     %r26,  GREGS_GPR + 104(%r12)
    lwz     %r27,  GREGS_GPR + 108(%r12)
    lwz     %r28,  GREGS_GPR + 112(%r12)
    lwz     %r29,  GREGS_GPR + 116(%r12)
    lwz     %r30,  GREGS_GPR + 120(%r12)
    lwz     %r31,  GREGS_GPR + 124(%r12)

    // ---- Phase 3: Restore final guest values from host stack ----
    // At this point:
    //   r1  = host stack (pushed frame with saved values)
    //   r0  = guest r0
    //   r2..r11,r13..r31 = guest values
    //   r12 = GuestRegs base pointer (NOT guest r12)
    //
    // On PPC64, load instructions read their base register BEFORE writing
    // the destination register.  This allows loading guest r1 from the
    // host stack without needing a separate base pointer.

    // Reload saved values from host stack frame:
    //   [r1+32] = guest r0
    //   [r1+40] = guest r1
    //   [r1+48] = guest r12
    //   [r1+56] = guest PC
    //   [r1+72] = saved GuestRegs*
    //   [r1+80] = saved NativeContext*

    lwz     %r0,    32(%r1)           // r0 = guest r0
    lwz     %r12,   56(%r1)           // r12 = guest PC (temp)

    // Save guest r11 to stack before using it as a temp register
    std     %r11,   64(%r1)           // save guest r11

    mtctr   %r12                       // CTR = guest PC (for bctr)

    // Save the overwrite CTR value (guest PC) to NativeContext so that
    // SaveGuestRegsFromContext can detect when CTR was corrupted.
    ld      %r11,   80(%r1)           // r11 = NativeContext* (from saved slot)
    stw     %r12,   NCTX_OVERWRITE_CTR(%r11)

    // Write NativeContext* to NCE_SLOT_ADDR (0x100003FF8) so SIGALRM
    // can find it when the guest is running.  Done here (after all host
    // state is saved but before guest state is live) to close the race
    // between the C++ code setting the slot and bctr — previously the
    // ALRM handler could fire after the C++ Set but before bctr, saving
    // the trampoline's partial ucontext as "guest state".
    // r12 holds guest PC temp — clobbering it is safe (restored below).
    li      %r12,   0
    oris    %r12,   %r12,   0x1000
    sldi    %r12,   %r12,   4
    addi    %r12,   %r12,   0x3FF8
    std     %r11,   0(%r12)           // NCE_SLOT = NativeContext*

    // Diagnostic: stage=3 (about to bctr to guest)
    li      %r12,   0
    oris    %r12,   %r12,   0x1000
    sldi    %r12,   %r12,   4
    addi    %r12,   %r12,   0x3FEC
    li      %r11,   3
    stw     %r11,   0(%r12)

    // Restore guest r11
    ld      %r11,   64(%r1)           // r11 = guest r11

    lwz     %r12,   48(%r1)           // r12 = guest r12
    lwz     %r1,    40(%r1)           // r1 = guest r1 (base read before dest write!)

    // ---- Phase 4: Jump to guest code ----
    bctr                                // branch to guest PC (CTR)

    .size JitPPC64EnterGuest, . - JitPPC64EnterGuest

    .globl JitPPC64EnterGuest_end
    .type JitPPC64EnterGuest_end, @function
JitPPC64EnterGuest_end:
