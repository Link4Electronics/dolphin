// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

struct GuestRegs;
struct NativeContext;

void InitAsm();

// Defined in JitAsm.S
extern "C" void JitPPC64EnterGuest(const GuestRegs* regs, NativeContext* ctx);
