# Dolphin PPC64 Big-Endian Port

## Target System

- **CPU:** PowerPC 970 (Power Mac G5)
- **OS:** Arch Linux PPC64 BE, kernel 7.1
- **GPU:** Radeon R600, OpenGL 4.6
- **Page size:** 64 KB
- **ABI:** ELFv2
- **Host endianness:** Big-endian (PPC64 BE)

## Build

User is the one who always build, but the build cmake setup used is this
```bash
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_GENERIC=OFF \
  -DENABLE_QT=OFF \
  -DENABLE_VULKAN=OFF \
  -DENABLE_HEADLESS=OFF \
  -DENABLE_NOGUI=OFF \
  -DENABLE_CUBEB=OFF \
  -DUSE_DISCORD_PRESENCE=OFF \
  -DENABLE_AUTOUPDATE=OFF \
  -DUSE_RETRO_ACHIEVEMENTS=OFF \
  -DUSE_SYSTEM_FMT=OFF \
  -DUSE_MGBA=OFF
cmake --build build -j$(nproc)
```

`-DUSE_MGBA=OFF` skips GBA controller emulation (libmgba). The bundled `Externals/mGBA` requires the system mgba library or builds its own; on PPC64 the system mgba version may have API mismatches or fail to build, so disable it for the headless/no-GUI port target.

## Endianness Architecture

### Core Principle

Emulated PPC memory is stored in **host byte order**. On a BE host, this is naturally PPC-compatible (BE). On LE hosts, byteswapping is required at every memory access.

### New Endian-Aware Functions (in `Common/Swap.h`)

| Function | LE host | BE host | Purpose |
|----------|---------|---------|---------|
| `Common::FromBigEndian(T)` (template) | byteswap | identity | Convert BE data → host arithmetic type |
| `Common::FromLittleEndian(T)` (template) | identity | byteswap | Convert LE data → host arithmetic type |
| `Common::ToBigEndian(T)` (template) | byteswap | identity | Convert host → BE (for output) |
| `Common::ToLittleEndian(T)` (template) | identity | byteswap | Convert host → LE (for output) |
| `Common::FromBigEndian(const u8*)` (returns u32) | byteswap | identity | Read BE u32 from byte pointer |
| `Common::FromLittleEndian(const u8*)` (returns u32) | identity | byteswap | Read LE u32 from byte pointer |

**Note:** Only u32 pointer overloads exist (the only width needed by callers). For u16/u64 from pointers, read via memcpy then call value overload.

### Key Design Decision

Existing `Common::swap32()`, `Common::swap64()`, `Common::swap16()` perform **unconditional** byteswaps (always reverse bytes). These are still used for algorithmic byte-reversal (e.g., LaggedFibonacciGenerator). 

The new conditional functions use `std::endian::native` (C++20) to only swap when host ≠ target endianness.

### File Format Endianness by Format

| Format | Magic | Integer fields | Strings |
|--------|-------|---------------|---------|
| **Plain ISO** (GC/Wii) | BE (WII_DISC_MAGIC, GAMECUBE_DISC_MAGIC) | BE | Raw bytes |
| **CISO** | LE ("CISO") | LE (block_size) | — |
| **GCZ** | LE (0xB10BC001) | LE (all header fields) | — |
| **WBFS** | LE ("WBFS") | BE (hd_sector_count), BE (wlba_table) | — |
| **TGC** | LE (0xA2380FAE) | BE (fst_real_offset, etc.) | — |
| **WIA/RVZ** | LE ("WIA\x1", "RVZ\x1") | BE (version, sizes, offsets) | — |
| **NFS** | LE ("EGGS") | BE (lba_range_count, blocks) | — |
| **WAD** | ? (0x00204973 / 0x00206962) | ? | — |

## Modified Files

### `Source/Core/Common/Swap.h`
- Added `#include <bit>` for `std::endian`
- Made `FromBigEndian()` conditional (swap on LE only, identity on BE)
- Added `FromLittleEndian(T)` (identity on LE, swap on BE)
- Added `ToBigEndian(T)` (swap on LE, identity on BE)
- Added `ToLittleEndian(T)` (identity on LE, swap on BE)
- Added `FromBigEndian(const u8*)` overloads for u16/u32/u64
- Added `FromLittleEndian(const u8*)` overloads for u16/u32/u64

### DiscIO/Blob Readers

| File | Change |
|------|--------|
| `Blob.cpp:217` | Added `FromLittleEndian(magic)` after reading blob magic |
| `CISOBlob.cpp:22` | `header.block_size` → `FromLittleEndian` |
| `CISOBlob.cpp:32` | `header.magic` → `FromLittleEndian` |
| `CompressedBlob.cpp:41-60` | All 6 header fields + block pointers + hashes byteswapped |
| `CompressedBlob.cpp:389` | `IsGCZBlob` magic → `FromLittleEndian` |
| `CompressedBlob.cpp:377-379` | `ConvertToGCZ` write path: header + offsets + hashes → `ToLittleEndian` |
| `CompressedBlob.h:4` | Updated warning comment |
| `WbfsBlob.cpp:44` | `wlba_table` `swap16` → `FromBigEndian` |
| `WbfsBlob.cpp:98-101` | Magic → `FromLittleEndian`, hd_sector_count → `FromBigEndian` |
| `TGCBlob.cpp:17-22` | `SubtractBE32` uses `FromBigEndian`/`ToBigEndian` |
| `TGCBlob.cpp:52-53` | Magic → `FromLittleEndian` |
| `TGCBlob.cpp:67-94` | All `swap32(m_header.*)` → `FromBigEndian`, `swap32(m_fst.data())` → `FromBigEndian`, `swap32(old_offset + shift)` → `ToBigEndian` |
| `TGCBlob.cpp:107-121` | Remaining `swap32` → `FromBigEndian`/`ToBigEndian` |
| `WIABlob.cpp:100-101` | Magic → `FromLittleEndian` |
| `WIABlob.cpp` | `FromBigEndian` (was `swap32`) replaceAll for all header/entry field accesses |
| `WIABlob.cpp` | All `swap16` → `FromBigEndian`/`ToBigEndian` (hash exception reads/writes) |
| `WIABlob.h:54-63` | Inline `swap64`/`swap32` → `FromBigEndian` |
| `NFSBlob.cpp:139` | Magic → `FromLittleEndian` |
| `NFSBlob.cpp:54-64` | `GetLBARanges` uses `FromBigEndian` |
| `NFSBlob.cpp:242` | `DecryptBlock` IV `swap64` → `ToBigEndian` |
| `NANDImporter.cpp:283` | `swap16(ptr)` → `memcpy` + `FromBigEndian` |

### DiscIO/Volumes

| File | Change |
|------|--------|
| `Volume.h:52` | `ReadSwapped` uses `FromBigEndian` (now conditionally correct) |
| `VolumeGC.cpp:50` | Triforce magic → `FromLittleEndian` |
| `VolumeGC.cpp:95` | `region_flags & 0xFF` → `reinterpret_cast<u8*>(...)[0]` |
| `VolumeGC.cpp:212-219` | Banner magic extracted via `FromLittleEndian` |
| `VolumeVerifier.cpp:548-552` | `swap32(disc_header.data())` → `FromBigEndian` |
| `VolumeVerifier.cpp:1329` | CRC32 byte swap → `ToBigEndian` |
| `FileSystemGCWii.cpp:81` | `swap32(m_fst + ...)` → `FromBigEndian` |
| `DirectoryBlob.cpp:208` | Partition type from name → `FromBigEndian` |
| `DirectoryBlob.cpp:972-973` | Disc type detection → `FromBigEndian` |
| `DirectoryBlob.cpp:1028` | Apploader size → `FromBigEndian` |

### Core/HW

| File | Change |
|------|--------|
| `ProcessorInterface.cpp:138-139` | Added 32-bit handler for `PI_FLIPPER_UNK` (IPL bootstrap write) |
| `MMIO.h:84-101` | `LowPart`/`HighPart` → endian-aware (`if constexpr` checks `std::endian::native`). Fixes CP FIFO and DSP ARAM DMA register access on BE where `(u16*)ptr` points to the high half, not low. |
| `AudioInterface.h:66-83` | `AICR` bitfield union: reversed member order on BE via `#if __BYTE_ORDER__`. GCC on BE allocates bitfields MSB-first — `PSTAT`, `AISFR`, `AIINTMSK`, etc. were at wrong bit positions, causing AI register reads to return garbage. Swiss reads AI control → wrong value → computes invalid address `0x0C004028`. |
| `AudioInterface.h:87-96` | `AIVR` bitfield union: reversed `left`/`right` volume fields on BE. CPU writes volume via MMIO → wrong channel volumes read back. |
| `EXI/EXI_Channel.h:63-87` | `UEXI_STATUS` bitfield union: reversed member order on BE. EXI status register (device detection, chip select, clock) returned wrong data → Swiss computes invalid EXI address `0x0C00688C`. |
| `EXI/EXI_Channel.h:90-101` | `UEXI_CONTROL` bitfield union: reversed `TSTART`/`DMA`/`RW`/`TLEN` on BE. EXI transfer control register returns wrong bits. |
| `DSP.h:132-140` | `UARAMCount` bitfield union: reversed `count`/`dir` on BE. ARAM DMA count/direction register accessed via HighPart/LowPart — bitfield order now matches register layout. |
| `DSP.h:143-151` | `UAudioDMAControl` bitfield union: reversed `NumBlocks`/`Enable` on BE. Audio DMA block count/control register corrected. |
| `DSP.h:179-188` | `ARAM_Info` bitfield union: reversed `size`/`unk` on BE. ARAM info register corrected. |
| `DSP.h:45-56` | Added `HostToGekko16()` — a constexpr 16-bit bit-reversal function, identity on LE. |
| `DSP.h:82` | `UDSPControl` constructor now calls `HostToGekko16(hex)` to store Gekko-order values in host-bitfield order. |
| `DSP.h:83` | Added `GekkoHex()` method that reverses back to Gekko order for CPU consumption. |
| `DSP.cpp:254-256` | Read handler: convert `m_dsp_control.Hex` + emulator value from host to Gekko order before returning to CPU. Mask also converted via `HostToGekko16(DSP_CONTROL_MASK)`. |
| `DSP.cpp:262-266` | Write handler: convert `val` and `DSP_CONTROL_MASK` to host-order before combining with emulator result (which already returns host-order). |
| `DSP.cpp:382-384` | `UpdateInterrupts`: read Hex via `GekkoHex()` before using with Gekko-order INT_* masks. |
| `DSP.cpp:399-403` | `GenerateDSPInterrupt`: convert to Gekko, OR with Gekko masks, convert back to host-order. |
| `DSPHLE.cpp:201` | Log: display `m_dsp_control.GekkoHex()` instead of raw Hex. |
| `VideoInterface.h:90-510` | All 16 VI union types: reversed bitfield member order on BE via `#if __BYTE_ORDER__`. GCC on BE allocates bitfields MSB-first, so the first field gets the highest bit — the exact reverse of the Gekko hardware register layout (LSB-first). With the `{Hi, Lo}` union swap (also applied), the bitfield → {Hi,Lo} mapping was broken, causing `Preset()` timing values and MMIO reads to return wrong register data. Games like Swiss read VI timings → wrong code path → invalid MMIO reads at AI+0x28, EXI+0x8C. |

**Root cause (DSP):** GCC on BE allocates C bitfields MSB-first, but the `UDSPControl` struct was designed for LSB-first (= Gekko hardware). Fixed via `HostToGekko16()` bit-reversal at all Gekko→host boundary crossings. `DSP_CONTROL_MASK = 0x0C07` is in Gekko order and correctly wrapped in `HostToGekko16()` at both usage sites — the mask is NOT broken on BE.

**Root cause (VI):** Same GCC MSB-first bitfield issue. The `{Hi, Lo}` union swap (commit `c7cb79f01a`) correctly fixes MMIO pointer semantics, but the bitfield struct order must also be reversed on BE to maintain the same physical bit layout as LE. Without this, `Preset()` stores timing values at wrong bit positions → game reads wrong VI register values → wrong display timing detection → hangs/crashes.

**Root cause (AI):** Same GCC MSB-first bitfield issue. The `AICR` bitfields (`PSTAT`, `AISFR`, `AIINTMSK`, `AIINT`, `AIINTVLD`, `SCRESET`, `AIDFR`) are allocated at wrong positions on BE, causing the AI control register read to return garbage. Swiss reads `AI_CONTROL` → wrong value → computes invalid address `0x0C004028` → invalid MMIO read.

**Root cause (EXI):** Same GCC MSB-first bitfield issue. The `UEXI_STATUS` and `UEXI_CONTROL` bitfields are allocated at wrong positions on BE, causing EXI status and control register reads to return garbage. Swiss reads `EXI_STATUS` → wrong chip select / device detection → computes invalid address `0x0C00688C` → invalid MMIO read.

### Remaining Native C++ Bitfield Unions

| File | Union | Line | Bit Width | Fields | Status |
|------|-------|------|-----------|--------|--------|
| `SI/SI_Device.h` | `UCommand` | 73 | u32 (8+8+8+8) | parameter1, parameter2, command, pad | **FIXED** |
| `EXI/EXI_DeviceMic.h` | `UStatus` | 45 | u16 (4+1+3+1+1+1+2+2+1) | out, id, button_unk, button, buff_ovrflw, gain, sample_rate, buff_size, is_active | **FIXED** |
| `WiimoteCommon/WiimoteReport.h` | `ButtonData` | 162 | u16 (16x1) | left, right, down, up, plus, acc_bits(2), unknown, two, one, b, a, minus, acc_bits2(2), home | **UNFIXED** |
| `WiimoteEmu/Extension/Nunchuk.h` | `ButtonFormat` | 38 | u8 (1+1+2+2+2) | z, c, acc_x_lsb, acc_y_lsb, acc_z_lsb | **UNFIXED** |
| `WiimoteEmu/Extension/Classic.h` | `ButtonFormat` | 34 | u16 (15x1 + 1pad) | rt, plus, home, minus, lt, dpad_down, dpad_right, dpad_up, dpad_left, zr, x, a, y, b, zl, pad | **UNFIXED** |
| `WiimoteEmu/Extension/Turntable.h` | anonymous | 49 | u16 (1) | ltable2 | **UNFIXED** |

Wiimote unions are not needed for GC-only use (Swiss on a GameCube).

### MemoryInterface

| File | Change |
|------|--------|
| `MemoryInterface.cpp:107-112` | Added `Constant<u16>(0)` / `Nop<u16>()` handlers at MI gap addresses 0x026-0x030. Silences invalid MMIO errors when software probes undocumented register space between MI_PROT_ADDR_HI and MI_TIMER0_HI. |

### Common / VideoCommon

| File | Change |
|------|--------|
| `ColorUtil.cpp:42-45` | `Decode5A3` output: added `if constexpr` endian-aware byte order — produces `[B,G,R,A]` memory bytes on both hosts for `.rgbSwapped()` compatibility. |
| `ColorUtil.cpp:58` | `Decode5A3Image`: `Common::swap16(src[ix])` → `Common::FromBigEndian(src[ix])` — fix for raw BE byte buffer cast to `u16*`. |
| `ColorUtil.cpp:78` | `DecodeCI8Image`: same `swap16` → `FromBigEndian` fix for palette lookups. |
| `TextureDecoder_Common.cpp:309-314` | `TexDecoder_Decode`: added conditional byteswap pass on BE — converts decoded u32 pixels from host byte order `[A,B,G,R]` to OpenGL `[R,G,B,A]` byte order. |

### SI / EXI

| File | Change |
|------|--------|
| `SI/SI.cpp:331-341` | SI buffer u32 MMIO handlers: `Common::swap32` → `Common::FromBigEndian` (read) / `Common::ToBigEndian` (write). Unconditional `swap32` reversed bytes again on BE, corrupting command writes and response reads to/from the SI buffer. This caused the IPL/OS to read `0x00000009` (no device) instead of `0x09000000` (GC controller) from the CMD_STATUS response → SI init timed out after ~678ms. |
| `SI/SI.cpp:347-357` | SI buffer u16 MMIO handlers: same `Common::swap16` → `Common::FromBigEndian`/`ToBigEndian` fix. |
| `SI/SI_Device.h:73-85` | `UCommand` union: reversed bitfield member order on BE via `#if __BYTE_ORDER__`. `parameter1`, `parameter2`, `command` were at wrong bit positions on BE when constructed from `USIChannelOut`. |
| `EXI/EXI_DeviceMic.h:45-61` | `UStatus` union: reversed bitfield member order on BE via `#if __BYTE_ORDER__`. All 9 status fields (`out`, `id`, `button_unk`, `button`, `buff_ovrflw`, `gain`, `sample_rate`, `buff_size`, `is_active`) were at wrong bit positions on BE. |

### Core/Boot

| File | Change |
|------|--------|
| `DolReader.cpp:42-44` | DOL header byteswap → `FromBigEndian` |
| `DolReader.cpp:46-47` | HID4 pattern → `FromBigEndian` (conditional) |
| `DolReader.cpp:106` | Ancast magic → `FromBigEndian` |
| `DolReader.cpp:160-209` | All ancast field reads → `FromBigEndian` |
| `ElfReader.cpp:19-25` | `bswap()` functions → `FromBigEndian` |
| `ElfReader.cpp:198-206` | Symbol table reads → `FromBigEndian` |
| `ElfReader.cpp:239-240` | HID4 pattern → `FromBigEndian` |

## How To Test

1. **Build** with `-DENABLE_GENERIC=ON` on the PPC64 BE machine
2. **Game detection**: Place an ISO/wbfs/gcz in the configured game path and launch dolphin. If games appear in the list, blob detection works.
3. **Interpreter**: Boot a game with interpreter CPU core selected (Config → Advanced → CPU Core → Interpreter).
4. **Debug**: Run with `LOG_*` categories in the config or use the `--debugger` flag.

## Fixed

### 1. TimeBase Read/Write (IPL boot hang fix)
`PowerPC.cpp:399-408` — `ReadFullTimeBaseValue()`/`WriteFullTimeBaseValue()` used `std::memcpy` to read/write `spr[SPR_TL..SPR_TU]` as a u64. On BE, `memcpy` interprets the pair in host byte order (big-endian), so `spr[SPR_TL]` (at the lower address) becomes the upper 32 bits instead of the lower. This corrupted the Time Base value returned to the IPL's `__OSGetSystemTime()` (`mftb` → `mfspr` → `ReadFullTimeBaseValue()`), causing the timer expiry check in DvdStep state 2 to never pass. The IPL hung forever at `PI_RESET_CODE: 00000001` without ever sending DI commands.

**Fix:** Replaced `memcpy` with explicit shifts:
- `Read`: `(TU << 32) | TL` (combines as arithmetic values, not by pointer reinterpretation)
- `Write`: `TL = u32(value); TU = u32(value >> 32)` (writes halves individually)

## Known Remaining Issues

### 1. LaggedFibonacciGenerator (Wii Encryption)
`Source/Core/DiscIO/LaggedFibonacciGenerator.cpp` uses `Common::swap32()` as part of the PRNG algorithm, not for endianness conversion. The PRNG output will differ on BE because `swap32` always reverses bytes. This breaks Wii decryption of encrypted partitions.

**Status:** Needs algorithmic fix. The LFG algorithm needs to be made endian-independent or replaced with a portable implementation.

### 2. Vulkan Renderer
Not needed for R600 (OpenGL only scenario), but the Vulkan backend may have endian assumptions.

## Architectural Notes for Future JIT Port

When implementing a native PPC64 JIT on PPC64 BE:
- No instruction byteswap needed (PPC guest = PPC host endianness)
- Emulated memory can be mapped directly (no endian conversion)
- Page size difference (4K vs 64K) needs consideration for fastmem
- ELFv2 ABI differences from ELFv1

## Commit Strategy

Each functional area (Swap.h, Blob readers, Volume code, Boot code) should be committed separately for clarity. Use descriptive messages prefixed with `[PPC64-BE]`.
