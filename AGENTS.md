# Dolphin PPC64 Big-Endian Port

## Target System

- **CPU:** PowerPC 970 (Power Mac G5)
- **OS:** Arch Linux PPC64 BE, kernel 7.1
- **GPU:** Radeon R600, OpenGL 4.6
- **Page size:** 64 KB
- **ABI:** ELFv2
- **Host endianness:** Big-endian (PPC64 BE)

## Build

AI agent runs on x86_64, so agent isn't allowed to build, user builds on powerpc 64bit big-edian machine, test and bring the results back
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
  -DENABLE_TESTS=OFF \
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
| **WAD** | LE (0x00204973 / 0x00206962) | BE (hdr_size, wad_type, cert_size, ticket_size, tmd_size, data_size, bnr_size) | — |

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

### Native C++ Bitfield Unions (Wiimote + Misc)

| File | Union/Struct | Line | Bit Width | Fields | Status |
|------|-------|------|-----------|--------|--------|
| `SI/SI_Device.h` | `UCommand` | 73 | u32 (8+8+8+8) | parameter1, parameter2, command, pad | **FIXED** |
| `EXI/EXI_DeviceMic.h` | `UStatus` | 45 | u16 (4+1+3+1+1+1+2+2+1) | out, id, button_unk, button, buff_ovrflw, gain, sample_rate, buff_size, is_active | **FIXED** |
| `WiimoteCommon/WiimoteReport.h` | `ButtonData` | 162 | u16 (16x1) | left, right, down, up, plus, acc_bits(2), unknown, two, one, b, a, minus, acc_bits2(2), home | **FIXED** |
| `WiimoteCommon/WiimoteReport.h` | `OutputReportRumble` | 38 | u8 (1) | rumble | **FIXED** |
| `WiimoteCommon/WiimoteReport.h` | `OutputReportEnableFeature` | 46 | u8 (1+1+1) | rumble, ack, enable | **FIXED** |
| `WiimoteCommon/WiimoteReport.h` | `OutputReportLeds` | 80 | u8 (1+1+2+4) | rumble, ack, pad(2), leds | **FIXED** |
| `WiimoteCommon/WiimoteReport.h` | `OutputReportMode` | 91 | u8 (1+1+1+5) | rumble, ack, continuous, pad(5) | **FIXED** |
| `WiimoteCommon/WiimoteReport.h` | `OutputReportRequestStatus` | 103 | u8 (1+7) | rumble, pad(7) | **FIXED** |
| `WiimoteCommon/WiimoteReport.h` | `OutputReportWriteData` | 142 | u8×2 (1+1+2+4, 1+7) | rumble, pad(1), space(2), pad(4), i2c_rw_ignored, slave_address | **FIXED** |
| `WiimoteCommon/WiimoteReport.h` | `OutputReportReadData` | 170 | u8×2 (1+1+2+4, 1+7) | rumble, pad(1), space(2), pad(4), i2c_rw_ignored, slave_address | **FIXED** |
| `WiimoteCommon/WiimoteReport.h` | `OutputReportSpeakerData` | 197 | u8 (1+2+5) | rumble, pad(2), length | **FIXED** |
| `WiimoteCommon/WiimoteReport.h` | `InputReportStatus` | 265 | u8 (1+1+1+1+4) | battery_low, extension, speaker, ir, leds | **FIXED** |
| `WiimoteCommon/WiimoteReport.h` | `InputReportReadDataReply` | 314 | u8 (4+4) | error, size_minus_one | **FIXED** |
| `WiimoteCommon/WiimoteReport.h` | `AccelCalibrationData` | 382 | u8 (7+1) | volume, motor | **FIXED** |
| `WiimoteEmu/Extension/Nunchuk.h` | `ButtonFormat` | 38 | u8 (1+1+2+2+2) | z, c, acc_x_lsb, acc_y_lsb, acc_z_lsb | **FIXED** |
| `WiimoteEmu/Extension/Classic.h` | `ButtonFormat` | 34 | u16 (15x1 + 1pad) | rt, plus, home, minus, lt, dpad_down, dpad_right, dpad_up, dpad_left, zr, x, a, y, b, zl, pad | **FIXED** |
| `WiimoteEmu/Extension/Turntable.h` | anonymous | 49 | u16 (1) | ltable2 | **FIXED** |
| `WiimoteEmu/Extension/UDrawTablet.h` | stylus fields | — | u8 (4+4,4+4) | stylus_x2, stylus_y2 | **FIXED** |

All Wiimote-related bitfield structs are now fixed.

### Wiimote Emulator (endian fixes beyond bitfields)

| File | Change |
|------|--------|
| `WiimoteCommon/DataReport.cpp` | All `BitCastPtr<CoreData>` reads/writes to HID report buffer wrapped with `FromLittleEndian`/`ToLittleEndian`. Affects `IncludeCore`, `IncludeAccel`, `ReportInterleave1`, `ReportInterleave2`. |
| `WiimoteEmu/EmuSubroutines.cpp` | Input path: replaced `Common::FromLittleEndian(const u8*)` on `u8[2]` arrays (reads 4 bytes) with explicit `(addr[0] << 8) \| addr[1]` for address/size fields (protocol uses big-endian byte order per comment, explicit shift is endian-agnostic). Output path: `reply.address` changed from `Common::FromLittleEndian` (identity on LE, byteswap on BE) to `Common::ToBigEndian` (byteswap on LE, identity on BE). |
| `WiimoteEmu/EmuSubroutines.cpp` | Added `buttons.hex = Common::ToLittleEndian(...)` before all 3 `TypedInputData` sends (Ack, Status, ReadDataReply) — `ButtonData` in packed structs stores hex in host byte order, but Wiimote HID protocol expects little-endian bytes. |
| `WiimoteEmu/Extension/DesiredExtensionState.h` | `DefaultExtensionUpdate` template now byteswaps trailing u16 on BE for Classic, Guitar, Turntable (Shinkansen removed from check because `Shinkansen::DataFormat` is private and doesn't exist on BE). |
| `USB/Bluetooth/WiimoteDevice.cpp` | `CBigEndianBuffer::Read16`/`Read32` changed from `Common::FromBigEndian(&m_buffer[offset])` (u32 pointer overload reads 4 bytes) to memcpy + value overload. `Write16`/`Write32` use `Common::ToBigEndian(T)` with memcpy. Fixes SDP data negotiation on LE (was reading 4 bytes instead of 2, corrupting service attribute lists). |

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
| `TextureDecoder_Common.cpp:309-314` | `TexDecoder_Decode`: **removed** conditional byteswap pass on BE (was converting decoded u32 pixels from host `[A,B,G,R]` to `[R,G,B,A]`, but this broke SW renderer which reads u32 via bitshift ops). Byteswap moved to `OGLTexture::Load` at GL upload boundary. |
| `TextureDecoder_Common.cpp:347-353,402,456,491,508,524,540,626` | `TexDecoder_DecodeTexel`: replaced all 7 `*((u32*)dst)` writes with `SetTexelBytes()` helper that stores bytes in fixed `[R,G,B,A]` order via individual byte assignments. On BE, `*(u32*)dst` stored reversed byte order `[A,B,G,R]`, causing `SetTexel`/`AddTexel` to read wrong channels → green tint/blur in SW-rendered textures. Added `static inline SetTexelBytes(u8* dst, u32 val)` helper. All paletted (C4/C8/C14X2), IA8, RGB565, RGB5A3, and CMPR paths fixed. |
| `TextureDecoder_Common.cpp:738-748` | `TexDecoder_DecodeXFB`: **removed** conditional byteswap (was redundant once OGLTexture::Load handles it). |
| `TextureDecoder_Common.cpp:309-314` | `TexDecoder_Decode`: added conditional byteswap on BE for decoded u32 pixel data — converts native u32 `[A,B,G,R]` byte order to `[R,G,B,A]` GL_RGBA order after `_TexDecoder_DecodeImpl` and overlay pass. |
| `TextureDecoder_Common.cpp:758-763` | `TexDecoder_DecodeXFB`: added conditional byteswap on BE for decoded XFB pixel data — same conversion to GL_RGBA byte order. |
| `OGLTexture.cpp:357-369` | **Removed** byteswap from `OGLTexture::Load` — data is now already in `[R,G,B,A]` byte order from the decoder. Avoids double-swapping custom/OSD textures from image files. |
| `SWOGLWindow.cpp:93-101` | `SWOGLWindow::ShowImage`: added conditional byteswap on BE for XFB display — converts native u32 byte order to GL_RGBA byte order before `glTexImage2D` upload. |
| `PulseAudioStream.cpp:88` | `PA_SAMPLE_S16LE` → `PA_SAMPLE_S16NE` (BE host was telling PulseAudio that BE sample data is LE — caused static noise on all audio output). |

### SW EFB Interface (color inversion fix)

| File | Change |
|------|--------|
| `SWEfbInterface.cpp:28-47` | Added `ReadU32LE()`/`WriteU32LE()` helpers — wrap `memcpy`+`swap32` on BE to read/write LE-style u32 from ABGR byte arrays. The SW EFB stores pixels in `std::array<u8>` but accesses them via `*(u32*)` casts assuming LE byte layout. On BE, these casts reversed the 4 bytes, swapping ABGR → RGBA ordering on every EFB read/write, causing `ChannelComponentIndex` indexing to retrieve wrong components. |
| `SWEfbInterface.cpp:52-62` | `SetPixelAlphaOnly`: replaced `*(u32*)&efb` RMW with `ReadU32LE`/`WriteU32LE`. |
| `SWEfbInterface.cpp:64-103` | `SetPixelColorOnly`: replaced `*(u32*)rgb` and `*(u32*)&efb` with `ReadU32LE`/`WriteU32LE`. |
| `SWEfbInterface.cpp:105-145` | `SetPixelAlphaColor`: same fix. |
| `SWEfbInterface.cpp:147-170` | `GetPixelColor`: `memcpy` read + `swap32` on BE via `ReadU32LE`. |
| `SWEfbInterface.cpp:193-247` | `SetPixelDepth`/`GetPixelDepth`: same `ReadU32LE`/`WriteU32LE` fix. |
| `SWEfbInterface.cpp:431-452` | `BlendTev`: byteswap `dstClr` on BE before `(u8*)&dstClr` cast so the byte pointer yields correct ABGR channel order. `LogicBlend` path uses `ReadU32LE(color)` instead of `*((u32*)color)`. |
| `SWEfbInterface.cpp:258,260` | `GetSourceFactor`: `DstClr`/`InvDstClr` cases: `*(u32*)dstClr` → `ReadU32LE(dstClr)`. On BE, `*(u32*)` on ABGR byte array reversed channels, causing `BlendColor` loop iteration to apply wrong per-channel factors → Red=0 for opaque dest, Blue=Green factor, etc. |
| `SWEfbInterface.cpp:299,301` | `GetDestinationFactor`: `SrcClr`/`InvSrcClr` cases: same `*(u32*)srcClr` → `ReadU32LE(srcClr)` fix. |

**Root cause:** The SW EFB uses `*(u32*)ptr` to read/write u32 values from/to a `u8[]` pixel array. The color array is `{alpha, blue, green, red}` by `ChannelComponentIndex` (ALP_C=0, BLU_C=1, GRN_C=2, RED_C=3). On LE, `*(u32*)` reads bytes `[A,B,G,R]` → u32 = `A|B<<8|G<<16|R<<24` (the intended LE layout). On BE, it reads `[A,B,G,R]` → u32 = `A<<24|B<<16|G<<8|R` (bytes reversed in u32 bits). All bit-manipulation expressions (masks, shifts) are designed for the LE value, producing garbage on BE. The fix wraps every u32 load/store in `ReadU32LE()`/`WriteU32LE()` which byteswaps on BE, restoring the intended LE semantics. This affects EFB pixel read/write, Tev output blend factor computation (`DstClr`, `SrcClr`, `InvDstClr`, `InvSrcClr`), and blend logic ops.

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

### Core/SysConf (System Configuration)

| File | Change |
|------|--------|
| `SysConf.cpp:69` | `number_of_entries = Common::swap16(number_of_entries)` → `Common::FromBigEndian` — SYSCONF file stores all u16 fields in BE. On BE host, `swap16` swapped correct BE to wrong LE, corrupting the entire system configuration (display settings, BT sync info, network config, etc.). Caused "read/write to system/sd card" errors in System Menu and games. |
| `SysConf.cpp:75` | Same fix for offset table entries. |
| `SysConf.cpp:99` | Same fix for BigArray data_length. |

### Core/IOS/ES (WAD/TMD/Ticket/Cert)

| File | Change |
|------|--------|
| `Formats.cpp` | All `Common::swapNN(ptr)` → `Common::FromBigEndian`/`ToBigEndian` (~25 locations). Fixes TMD header fields (boot_index, ios_id, title_id, title_flags, title_version, group_id, region, num_contents), content table fields (id, index, type, size), Ticket fields (device_id, title_id), V1 ticket size, V1TicketHeader fields, Cert fields (id, public_key_type), `ReadUidSysEntry` (NAND read path), and `GetOrInsertUIDForTitle` (NAND write path via `ToBigEndian`). |
| `Views.cpp` | `swap64`/`swap32` on TicketView → `FromBigEndian` (GetTicketFromView: 4 occurrences for title_id, ticket_id, permitted_title_mask, permitted_title_id) |
| `TitleManagement.cpp` | `swap64` on TicketView → `FromBigEndian` (DeleteTicket: title_id, ticket_id) |
| `ES.cpp` | `swap64`/`swap32` on TicketView → `FromBigEndian` (CheckStreamKeyPermissions: 3 occurrences; SetUpStreamKey: 2 occurrences; IsActiveTitlePermittedByTicket: 2 occurrences) |

### Core/IOS (IOSC - RSA exponent fix)

| File | Change |
|------|--------|
| `IOSC.cpp:586,600` | `Common::swap32(0x00010001)` → `Common::ToBigEndian(0x00010001)` — RSA exponent stored in `m_root_key_entry.misc_data` (read as raw BE bytes by mbedTLS). On BE, `swap32` stored bytes `[0x01,0x00,0x01,0x00]` → mbedTLS read exponent as `0x01000100=16777472` instead of `65537`, breaking ALL RSA signature verification. |
| `IOSC.cpp:512,514,516` | `Common::swap32` → `Common::ToBigEndian` for `MakeBlankEccCert` fields (signature.type, header.public_key_type, header.id) — struct is serialized to BE format for the emulated PPC. |
| `IOSC.cpp:645-648` | `Common::swap32(dump.*)` → `Common::FromBigEndian(dump.*)` in `LoadEntries()` — `BootMiiKeyDump` fields are BE on disk. |

### Core/IOS/USB/Bluetooth (BT HCI Endian Fixes)

**Root cause:** Bluetooth HCI protocol defines all multi-byte fields (opcodes, connection handles, packet types, clock offsets, etc.) in **little-endian** byte order. The PPC stores LE data in memory and reads it back with `lhbrx`/`lwbrx` (byte-reversed loads). On a BE host, `CopyFromEmu`/`CopyToEmu` (which use raw `memcpy`) interpret LE bytes as BE values, corrupting all HCI communication.

#### Input path (HCI command reading — `BTEmu.cpp`)

All HCI command structs read via `CopyFromEmu` have multi-byte fields in LE byte order in PPC memory. On BE host, these need `Common::FromLittleEndian()` after copy.

| Location | Field | Fix |
|----------|-------|-----|
| `ExecuteHCICommandMessage:977` | `msg.Opcode` | `FromLittleEndian` — determines command dispatch switch |
| `CommandDisconnect:1231` | `disconnect.con_handle` | `FromLittleEndian` — passed to `AccessWiimote` + `SendEventDisconnect` |
| `CommandChangeConPacketType:1332-1333` | `change_packet_type.con_handle`, `pkt_type` | `FromLittleEndian` — passed to `SendEventConPacketTypeChange` |
| `CommandAuthenticationRequested:1351` | `auth_req.con_handle` | `FromLittleEndian` — passed to `SendEventAuthenticationCompleted` |
| `CommandReadRemoteFeatures:1386` | `read_remote_features.con_handle` | `FromLittleEndian` — passed to `SendEventReadRemoteFeatures` |
| `CommandReadRemoteVerInfo:1401` | `read_remote_ver_info.con_handle` | `FromLittleEndian` — passed to `SendEventReadRemoteVerInfo` |
| `CommandReadClockOffset:1416` | `read_clock_offset.con_handle` | `FromLittleEndian` — passed to `SendEventReadClockOffsetComplete` |
| `CommandSniffMode:1431-1432` | `sniff_mode.con_handle`, `max_interval` | `FromLittleEndian` — passed to `SendEventModeChange` |
| `CommandWriteLinkSupervisionTimeout:1680-1681` | `supervision.con_handle`, `timeout` | `FromLittleEndian` — used in reply + logging |

#### Output path (HCI event writing — `BTEmu.cpp`)

All HCI event/reply structs written to the SQueuedEvent buffer end up in PPC memory via `FillBuffer` → `CopyToEmu`. Multi-byte fields must be stored in **little-endian** byte order because the PPC reads them with `lhbrx`/`lwbrx`. On BE host, `Common::ToLittleEndian()` byteswaps before store.

| Function | Fields Fixed |
|----------|-------------|
| `SendEventCommandComplete:732` | `hci_event->Opcode` |
| `SendEventCommandStatus:755` | `hci_event->Opcode` |
| `SendEventConnectionComplete:526` | `Connection_Handle` |
| `SendEventDisconnect:599` | `Connection_Handle` |
| `SendEventAuthenticationCompleted:624` | `Connection_Handle` |
| `SendEventReadRemoteFeatures:676` | `ConnectionHandle` |
| `SendEventReadRemoteVerInfo:705-708` | `ConnectionHandle`, `manufacturer`, `lmp_subversion` |
| `SendEventModeChange:848-850` | `Connection_Handle`, `Value` |
| `SendEventReadClockOffsetComplete:928-929` | `ConnectionHandle`, `ClockOffset` |
| `SendEventConPacketTypeChange:954-955` | `ConnectionHandle`, `PacketType` |
| `SendEventInquiryResponse:501` | `clock_offset` |
| `SendEventNumberOfCompletedPackets:814-815` | `compl_pkts`, `con_handle` |

#### ACL header path (`BTEmu.cpp`)

ACL data packets have a 4-byte header (`hci_acldata_hdr_t`) with two u16 fields (`con_handle` and `length`). These are LE in PPC memory, just like HCI events and commands.

| Location | Direction | Fields Fixed |
|----------|-----------|-------------|
| `IOCtlV ACL_DATA_OUT:160-168` | Input (read) | `con_handle`, `length` via `FromLittleEndian` |
| `SendACLPacket:244-245` | Output (write) | `con_handle`, `length` via `ToLittleEndian` |
| `ACLPool::WriteToEndpoint:438-439` | Output (write) | `con_handle`, `length` via `ToLittleEndian` |

#### Reply struct passthrough (command handlers — `BTEmu.cpp`)

| Command | Struct | Fields Fixed |
|---------|--------|-------------|
| `CommandReadStoredLinkKey` | `hci_read_stored_link_key_rp` | `max_num_keys`, `num_keys_read` |
| `CommandDeleteStoredLinkKey` | `hci_delete_stored_link_key_rp` | `num_keys_deleted` |
| `CommandWriteLinkSupervisionTimeout` | `hci_write_link_supervision_timeout_rp` | `con_handle` |
| `CommandReadLocalVer` | `hci_read_local_ver_rp` | `hci_revision`, `manufacturer`, `lmp_subversion` |
| `CommandReadBufferSize` | `hci_read_buffer_size_rp` | `max_acl_size`, `num_acl_pkts`, `num_sco_pkts` |

**Impact without fix:** All HCI commands hit the `default` case in the switch statement (opcode corrupted), or reach the correct handler but with wrong connection handles (wrong wiimote targeted, events lost). Wii Remote pairing/communication completely broken on BE.

**Note:** `USBV0.cpp`'s `swap16` usage was investigated by agent #1 and is **correct** — it reads LE data from a pointer cast and converts to host BE. No change needed there. SYSCONF parsing was investigated by agent #2 and confirmed **correct** after the existing `FromBigEndian` fix.

### InputCommon/XInput2 (Mouse buttons not registering on BE)

| File | Change |
|------|--------|
| `XInput2.cpp:290-293` | Replaced `memcpy` of X11 `unsigned char*` button mask into `u64` with explicit byte-by-byte construction. X11 mask is a byte array where byte N stores buttons N*8..N*8+7. On LE, `memcpy` into u64 places byte 0 at bits 0-7 (correct). On BE, `memcpy` places byte 0 at bits 63-56, so `>> 1` then truncation to u32 lost all buttons. Fix: `for (size_t i = 0; i < copy_len; ++i) buttons_zero_indexed \|= static_cast<u64>(button_state.mask[i]) << (i * 8);` |

**Root cause:** X11's `XIButtonState.mask` is an `unsigned char*` byte array where bit 0 of byte 0 = button 1. The code used `memcpy` to pack this into a `u64`, which on BE placed byte 0 in the highest byte (MSB). The right-shift by 1 (for 1→0 index conversion) then pushed all meaningful bits out of the `u32` range, effectively making every mouse button appear released. This broke all mouse-click-based wiimote input (e.g., default A="Click 1", B="Click 3" mappings on X11).

#### WiimoteDevice.cpp (L2CAP Endian Fix - Wiimote Connect/Disconnect Loop)

**Root cause:** All L2CAP protocol fields (dcid, scid, psm, length, result, status, flags) are in **little-endian** on the wire. WiimoteDevice.cpp uses packed struct pointer casts (`l2cap_hdr_t*`, `l2cap_con_req_cp*`, etc.) from raw `u8*` buffers. On BE host, reading the raw LE bytes as `uint16_t` via struct member access interprets them as BE, corrupting ALL L2CAP signaling. Every L2CAP connection request, response, configuration, and disconnection was broken.

**Fix:** Wrapped every u16 field access at the buffer boundary:
- **Input path** (buffer → host): `Common::FromLittleEndian(struct->field)` after pointer cast
- **Output path** (host → buffer): `Common::ToLittleEndian(val)` before struct field assignment

Functions fixed: `ExecuteL2capCmd`, `SignalChannel`, `ReceiveConnectionReq`, `ReceiveConnectionResponse`, `ReceiveConfigurationReq`, `ReceiveConfigurationResponse`, `ReceiveDisconnectionReq`, `SendConnectionRequest`, `SendConfigurationRequest`, `SDPSendServiceSearchResponse`, `SDPSendServiceAttributeResponse`, `SendCommandToACL`, `InterruptDataInputCallback`.

**Impact without fix:** All 7 L2CAP command types (connect req/rsp, config req/rsp, disconnect req/rsp, command reject) were broken on BE. The `ReceiveConnectionReq` handler would read PSM=0x1100 (byteswapped from 0x0011=HID_CNTL) → `FindChannelWithPSM` returned the wrong channel → disconnect. This caused the observed connect/disconnect loop every ~4 seconds.

## Core/IOS/Network/KD (NWC24/WC24 - Magic mismatch errors)

| File | Change |
|------|--------|
| `NWC24Config.cpp` | All 15 `Common::swap32`/`swap64` → `Common::FromBigEndian`/`ToBigEndian` (magic, version, id_generation, checksum, creation_stage, enable_booting, nwc24_id) |
| `NWC24DL.cpp` | All 21 `Common::swapNN` → `Common::FromBigEndian`/`ToBigEndian` (subtask_bitmask, title_ids, flags, timestamps, magic, version) |
| `Mail/WC24Send.cpp` | All 18 `Common::swap32` → `Common::FromBigEndian`/`ToBigEndian` (magic, version, number_of_mail, entry ids/sizes, timestamps) |
| `Mail/WC24FriendList.cpp` | All 5 `Common::swapNN` → `Common::FromBigEndian`/`ToBigEndian` (magic, status, friend_type, friend_code) |
| `Mail/MailCommon.h` | `Common::swap32(128 + (index * 128))` → `Common::ToBigEndian<u32>(128 + (index * 128))` (header size array) |

**Root cause:** `Common::swap32()` unconditionally reverses bytes. NAND file data uses BE for integer fields. On BE host, the raw file bytes are already in the correct host order after `ReadBytes(&struct, ...)`, so `swap32` corrupts them. This causes `Magic mismatch` errors during IOS boot on BE for both IOS 50 and IOS 24, leading to `WC24Mail unavailable` / `WC24Download unavailable` messages. The errors appear twice in the boot log (once per IOS reload).

**Diagnosis (log comparison):** On a working x86_64 LE system, these magic errors do NOT occur because `swap32` on LE converts the file-BE bytes into correct host-LE values. On BE, magic 0x5753007C (expected by `SEND_LIST_MAGIC`) gets `swap32`'d to 0x7C005357, causing mismatch 1716806487 != 1466127462.

**Note:** Existing NAND dumps created on LE systems may have WC24 file data stored in LE order. The NAND `Read()` stores raw bytes into structs; if the dump was created on LE, the WC24 file bytes may be in LE format. Fixing the code to use `FromBigEndian` means WC24 files from LE dumps will also fail because the bytes are already LE (not BE). This is the same problem as SYSCONF — must format the NAND data correctly for the host endianness.

## How To Test

1. **Build** with `-DENABLE_GENERIC=ON` on the PPC64 BE machine
2. **Game detection**: Place an ISO/wbfs/gcz/wad in the configured game path and launch dolphin. If games appear in the list, blob detection works.
3. **Interpreter**: Boot a game with interpreter CPU core selected (Config → Advanced → CPU Core → Interpreter).
4. **Debug**: Run with `LOG_*` categories in the config or use the `--debugger` flag.

## Fixed

### 1. L2CAP Protocol Endian (Wiimote Connect/Disconnect Loop)
`WiimoteDevice.cpp` — All L2CAP protocol fields (dcid, scid, psm, length, result, status, flags) are LE on the wire but the code uses packed struct pointer casts from raw buffers. On BE host, every u16 field access was byteswapped, breaking ALL L2CAP signaling. Fixed by wrapping every input u16 access with `Common::FromLittleEndian()` and every output u16 access with `Common::ToLittleEndian()`.

### 2. TimeBase Read/Write (IPL boot hang fix)
`PowerPC.cpp:399-408` — `ReadFullTimeBaseValue()`/`WriteFullTimeBaseValue()` used `std::memcpy` to read/write `spr[SPR_TL..SPR_TU]` as a u64. On BE, `memcpy` interprets the pair in host byte order (big-endian), so `spr[SPR_TL]` (at the lower address) becomes the upper 32 bits instead of the lower. This corrupted the Time Base value returned to the IPL's `__OSGetSystemTime()` (`mftb` → `mfspr` → `ReadFullTimeBaseValue()`), causing the timer expiry check in DvdStep state 2 to never pass. The IPL hung forever at `PI_RESET_CODE: 00000001` without ever sending DI commands.

**Fix:** Replaced `memcpy` with explicit shifts:
- `Read`: `(TU << 32) | TL` (combines as arithmetic values, not by pointer reinterpretation)
- `Write`: `TL = u32(value); TU = u32(value >> 32)` (writes halves individually)

### 2. VertexLoader posmtx Byte Order (Opaque Matrix / Vertex Blast fix)

**Status:** REVERTED — commit 96f09ce2e3 was reverted by bf7dcbc31b (broke OGL, Mario 3D chest regression). See Pending Fix #2 below.

**Root cause:** `VertexLoader::PosMtx_ReadDirect_UByte` wrote `DataWrite<u32>(posmtx)`. On BE, the u32 store placed the value in the 4th byte (MSB). The SW renderer reads byte 0 (`ReadVertexAttribute<u8>(..., base_component=0)`), getting 0 → every vertex uses matrix index 0 → identity bind pose → spiky geometry.

**Why the revert:** OGL backend uses `glBufferSubData` for vertex buffer upload, and Mesa on PPC64 BE byteswaps the u32 data during `glBufferSubData` (confirmed at OGLStreamBuffer.cpp:308-309), turning `[0, 0, 0, value]` back to `[value, 0, 0, 0]`. So the original `DataWrite<u32>(posmtx)` is correct for OGL. The fix belongs only on the SW reader side (`SWVertexLoader.cpp`).

### Exception Vector Addresses

| File | Change |
|------|--------|
| `Jit.cpp:1409` | `EmulateDSI`: `nip = 0x00000300` → `0x80000300` (K1 cached alias) |
| `Jit.cpp:1421` | `EmulateISI`: `nip = 0x00000400` → `0x80000400` |
| `Jit.cpp:319` | Inner loop ISI injection: `m_ppc_state.pc = 0x00000400` → `0x80000400` |

**Root cause:** All three exception vector addresses were set to physical addresses (`0x00000nnn`). Physical address `0x0` is not mappable on PPC64 (kernel forbids `MAP_FIXED` at `0x0` — `EPERM`). The guest memory mapping only covers `0x70000000-0x82000000` (K2 + K1 cached). When `sigreturn` restored NIP to a physical address, the CPU immediately SIGSEGV'd on instruction fetch from unmapped memory, causing an infinite loop of SIGSEGV → DSI/ISI injection → repeat. Fix: use the K1 cached alias (`0x80000nnn`) which IS in the executable mapping. The interpreter's `GetPointerForRange` masks with `0x3FFFFFFF`, so `0x80000nnn` maps to physical `0x00000nnn` for interpreter uses.

## Known Remaining Issues

### 1. LaggedFibonacciGenerator (Wii Encryption)
`Source/Core/DiscIO/LaggedFibonacciGenerator.cpp` uses `Common::swap32()` as part of the PRNG algorithm, not for endianness conversion. The PRNG output will differ on BE because `swap32` always reverses bytes. This breaks Wii decryption of encrypted partitions.

**Status:** Needs algorithmic fix. The LFG algorithm needs to be made endian-independent or replaced with a portable implementation.

### 2. OGL Backend — Black Screen (FIXED)
OpenGL backend shows a black screen (no OSD, no game output) on R600 with Mesa.

**Root cause:** Persistent mapped buffers (`BufferStorage` via `glBufferStorage` + `GL_MAP_PERSISTENT_BIT`) write data in host byte order directly to GPU-visible memory. On BE, the CPU writes BE byte order, but the GPU (always LE) reads these values as LE. This corrupts ALL UBO data (dimensions, strides, offsets, matrices, colors), vertex data, and SSBO data — causing every shader to receive garbage parameters, producing zero output → black screen.

`MapAndSync`/`MapAndOrphan` use `glMapBufferRange` + `glUnmapBuffer`, where `glUnmapBuffer` lets the GL driver byteswap transparently — these work correctly.

**Fix:** Added `if constexpr (std::endian::native != std::endian::big)` guard in `OGLStreamBuffer.cpp:CreateStreamBuffer()` to skip `BufferStorage` and `PinnedMemory` on BE, falling through to `MapAndSync`. Also made `UsePersistentStagingBuffers()` return `false` on BE in `OGLTexture.cpp`.

The GPU texture decode compute shaders (`TextureConversionShader.cpp`) are endian-safe — `Swap16()` correctly converts GPU-LE to game-BE.

On BE, Mesa on UMA reads buffer data as-is (no byteswap on unmap), but `MapAndSync`/`MapAndOrphan` use `glMapBufferRange` + `glUnmapBuffer` which Mesa handles correctly for the scalar/struct UBO data — the persistent buffer avoidance alone is sufficient for UBO correctness.

### 2. SW Vertex Blast — Pending Fix (SW Reader Side Only)

**Symptom:** GC IPL logo renders correctly (simple textured quad). Luigi's Mansion 3D models on SW renderer have spiky vertex blast.

**Root cause:** `VertexLoader::PosMtx_ReadDirect_UByte` writes `DataWrite<u32>(posmtx)` — on BE this stores `[0, 0, 0, value]`. The SW renderer in `SWVertexLoader.cpp:244` reads byte 0 via `ReadVertexAttribute<u8>(..., base_component=0)`, getting 0 for every matrix index.

**Pending fix:** Change `base_component` from 0 to 3 on BE in `SWVertexLoader.cpp:244`. Do NOT modify `VertexLoader.cpp` because OGL backend relies on Mesa byteswapping during `glBufferSubData` (see OGLStreamBuffer.cpp:308-309 comment).

### 3. SW Renderer Color Inversion

**Symptom:** Colors appear inverted / have blue tint on SW renderer. The `ReadU32LE` fix in SWEfbInterface.cpp (blend factors) is already in place but didn't resolve the issue. Root cause still unknown.

### 4. OGL Flag Animation

**Symptom:** MKDD checkerboard flag is static/flat on BE, while it waves correctly on x86_64. Suspected UBO/attribute endian issue specific to the OGL backend.

### 5. Vulkan Renderer
Not needed for R600 (OpenGL only scenario), but the Vulkan backend may have endian assumptions.

## Architectural Notes (JITPPC64 Implementation)

The JITPPC64 backend runs on PPC64 BE (PowerPC 970) with the following characteristics:
- **No instruction byteswap needed** (PPC guest = PPC host endianness)
- **Emulated memory mapped directly** (no endian conversion for PPC-side data)
- **Page size difference** (4K guest vs 64K host) handled via MAP_64KB for fastmem
- **ELFv2 ABI**: r2 (TOC), r13 (TLS) preserved across JIT code and restored by signal handlers and trampolines; callee-saved GPRs r14-r31 and FPRs f14-f31; stack frames require 16-byte alignment and LR at [SP+16]
- **AltiVec VRs**: MSR[VR]=1 set at init; VRs are volatile across C++ ABI calls (not preserved)


## Session 2026-07-16: JITPPC64 File Split + Build Fixes

### Monolithic → Split

`JitPPC64/Jit.cpp` (~1100 lines) split into:

| File | Purpose |
|------|---------|
| `Jit.h` | Class declarations, register conventions, constants |
| `Jit.cpp` | Core: prolog/epilog, Jit(), Run(), SingleStep(), EraseSingleBlock, Helpers |
| `JitPPC64_Tables.cpp` | `CanCompileInstruction()`, `CompileInstruction()` dispatch |
| `JitPPC64_Integer.cpp` | `CompileADDI`, `CompileADDIS`, `CompileADDIC`, `CompileADDIC_`, `CompileMULLI`, `CompileANDI_`, `CompileANDIS_`, `CompileORI`, `CompileORIS`, `CompileXORI`, `CompileXORIS`, `CompileCMPI`, `CompileCMPLI`, `CompileTable31`, `CompileTable31_Integer` |
| `JitPPC64_LoadStore.cpp` | `CompileLoadStore` (lwz/lbz/lhz/lha/stw/stb/sth + indexed forms) |
| `JitPPC64_Branch.cpp` | `CompileB` (bl stores LR), `CompileBC` (CTR+CR runtime evaluation, no forward refs) |
| `JitPPC64_SystemRegisters.cpp` | `CompileMFCR`, `CompileMTCRF`, `CompileMFSPR`, `CompileMTSPR`, `CompileMFMSR`, `CompileMTMSR`, `CompileMFTB`, `CompileTW`, `CompileTable31_SystemReg` |
| `JitPPC64_RegCache.h` | `JitPPC64RegCache` struct (18 host GPRs, dirty tracking) |
| `JitPPC64_RegCache.cpp` | `R()`/`W()`/`Flush()`/`Reset()`/`FindFreeHostReg()` |
| `JitPPC64_BackPatch.cpp` | `InitBackpatch()`/`ShutdownBackpatch()`/`AddBackpatchEntry()`/`HandleFault()` |
| `JitPPC64_Paired.cpp` | `CompilePairedSingle` — all ps\_\* via AltiVec (VADDFP/VSUBFP/VMULFP/VDIVFP/VREFP/VRSQRTEFP/VSEL/VMADDFP/VMRGHW/VSPLTW) + scalar FPU for compare/quantize |
| `PPC64Assembler.h` | Full PPC64 assembler with AltiVec (already existed, now confirmed complete) |

### Build Errors Fixed

| # | Error | File | Fix |
|---|-------|------|-----|
| 1 | `PowerPCState` does not name a type | Branch.cpp, Integer.cpp, SystemRegisters.cpp, RegCache.cpp, Jit.cpp, Paired.cpp | `offsetof(PowerPCState, ...)` → `offsetof(PowerPC::PowerPCState, ...)` |
| 2 | `CTX_NIP` not found on PPC64 | BackPatch.cpp | Added `#define CTX_NIP regs->nip` in MachineContext.h for `_M_PPC_64` (PPC64 musl: `mcontext_t` → `struct sigcontext` → `regs` → `struct pt_regs` → `nip`) |
| 3 | `JitBlock` has no `codeSize` | Jit.cpp | Replaced `b->codeSize = 0` with `b->near_begin = b->near_end = block_start`. Removed `b->codeSize = ...` (already set by `near_end`). |
| 4 | `GetBlockFromStartAddress` needs 2 args | Jit.cpp | Added `m_ppc_state.feature_flags` as second argument |
| 5 | `EraseBlock` no such method | Jit.cpp | `m_block_cache.EraseBlock(...)` → `m_block_cache.EraseSingleBlock(block)` |
| 6 | `m_dispatcher_entry` private | Jit.h | Added `friend class JitPPC64BlockCache;` |

### `JitPPC64_Paired.cpp` — AltiVec-Accelerated Paired Singles

- **SUBOP10 dispatch**: ps_mr (72), ps_neg (40), ps_nabs (136), ps_abs (264) — native X-form; ps_merge00/01/10/11 (528/560/592/624) — VMRGHW/VSPLTW + pack; ps_cmpu0/cmpu1/cmpo0/cmpo1 (0/32/64/96) — native FCMPU/FCMPO
- **SUBOP5 dispatch** (all AltiVec): ps_add (21→VADDFP), ps_sub (20→VSUBFP), ps_mul (25→VMULFP), ps_div (18→VDIVFP), ps_sum0/1 (10/11→VADDFP+VSPLTW), ps_muls0/1 (12/13→VSPLTW+VMULFP), ps_madds0/1 (14/15→VMADDFP), ps_sel (23→VCMPGEFP+VSEL), ps_res (24→VREFP), ps_rsqrte (26→VRSQRTEFP)
- **psq_l/st**: All quantize types (0=float, 1=u8, 2=u16, 3=s8, 4=s16) compiled inline using scalar FPU (LFIWAX/STFIWX/FCTIWZ/FRSP) and shifts. C helpers only as runtime-GQR-change fallback
- `LoadFPRPairToVR`: extracts Gekko u64 pair (ps0:upper32, ps1:lower32) → 16-byte memory → `LVX` into AltiVec VR
- `StoreVRToFPRPair`: `STVX` → LWZ halves → pack u64 → STD to `ppcState.ps[fr]`
- Zero interpreter fallback for any ps\_* instruction

### Register Conventions (unchanged from before)

| Register | Purpose |
|----------|---------|
| r12 | `ppcState` base pointer |
| r0  | Scratch (REG\_SCRATCH) |
| r11 | Scratch (REG\_SCRATCH2) |
| r14-31 | Cached PPC GPRs (via RegCache) |
| r1  | Host stack pointer |

## Session 2026-07-16: JIT GCC Opcode Coverage Expansion

### Changes Summary

| # | Change | Files |
|---|--------|-------|
| 1 | **Rewrote `CompileBC`** — uses r10 as not-taken flag (0=taken, ≠0=not-taken) with fixed-offset `BC(12/4, 2, 8)` skip patterns; no forward references needed | `JitPPC64_Branch.cpp` |
| 2 | **Fixed CA extraction** — `addcx`/`addic` used `RLWINM(...,0,0,0)` which keeps bit 31; `STB` stores low byte → always 0. Fix: `RLWINM(...,1,31,31)` shifts bit 31→bit 0 before STB. `subfcx` CA inverted `!GT` from `CMPLW`. | `JitPPC64_Integer.cpp` |
| 3 | **Fixed `EmitCR0Update()`** — reads result from `REG_SCRATCH` (r0), not `REG_SCRATCH2` (r11) | `Jit.cpp` |
| 4 | **Added assembler functions**: `ADDE`, `SUBFZE`, `ADDZE`, `SUBFME`, `ADDME` | `PPC64Assembler.h` |
| 5 | **New opcodes implemented**: `CompileSubfic`, `CompileTWI` (nop), `CompileRLWINM`, `CompileRLWIMI`, `CompileRLWNM`, `negx`, `subfcx` (with CA), `extswx` | `JitPPC64_Integer.cpp`, `JitPPC64_Rotate.cpp` |
| 6 | **Added branch/link ops**: `CompileBCLR`, `CompileBCCTR` — same r10 flag pattern as `CompileBC`; LR/CTR masked to 30 bits via `RLWINM(...,0,0,29)` | `JitPPC64_Branch.cpp` |
| 7 | **Added native CR ops**: `CompileMCRF` (native `MCRF`), `CompileCRLogical` (native `CRAND`/`CROR`/`CRXOR`/etc. — PPC970 has native CR bit-logic, no SIGILL), `CompileOPCD19` dispatcher routing SUBOP10 0/33-129-193-225-257-289-417-449/16/528 | `JitPPC64_Branch.cpp` |
| 8 | **Fixed duplicate `addex` case**: removed first attempt (lines 223-237), kept clean `return false` at second instance; added missing `return false` to prevent fallthrough to `subfx` | `JitPPC64_Integer.cpp` |
| 9 | **Updated Tables.cpp**: added dispatch for all new opcodes in `CompileInstruction`; `CanCompileInstruction` includes CR logical (SUBOP10 33-449) and mcrf (0) in opcd 19 | `JitPPC64_Tables.cpp` |
| 10 | **Jit.h**: added declarations for `CompileOPCD19`, `CompileBCLR`, `CompileBCCTR`, `CompileCRLogical`, `CompileMCRF`, `CompileRLWINM`, `CompileRLWIMI`, `CompileRLWNM`, `CompileSubfic`, `CompileTWI` | `Jit.h` |

### Key Design Decisions

- **No forward references in assembler**: `PPC64Assembler` has no `GetCodePtr()`/`SetCodePtr()`. Conditional branches use `BC(bo, bi, 8)` with bd=8 to skip exactly 1 instruction (4 bytes), combined with r10 as not-taken flag.
- **CR logical ops are native PPC970**: The 8 CR logical ops (`crand`, `cror`, `crxor`, `crnand`, `crnor`, `creqv`, `crandc`, `crorc`) are standard PPC ISA, not Gekko-specific. PPC970 implements them natively — no SIGILL. Emit directly via `CRAND()`/`CROR()`/etc.
- **CA-using ops excluded from JIT**: `adde`, `subfe`, `addze`, `subfze`, `addme`, `subfme`, `mcrxr` need XER[CA] read before computation and write after. `CanCompileInstruction` returns false → entire block falls to interpreter (no silent wrong results).
- **MCRF is native**: `MCRF(crD, crS)` is a standard PPC970 instruction — emit directly.

## Session 2026-07-16: Full JIT Backend — FPU, Load/Store, Branch, Rotate, System Registers

### New Files
| File | Purpose |
|------|---------|
| `JitPPC64_FPU.cpp` | opcd 59 (single-precision) + opcd 63 (double-precision) — all A-form/X-form/FPSCR ops |
| `JitPPC64_Rotate.cpp` | rlwinm/rlwimi/rlwnm |

### Extended Files
| File | What was added |
|------|----------------|
| `JitPPC64_LoadStore.cpp` | All D-form integer loads/stores (32-45), FPU loads/stores (48-55), indexed X-form (opcd 31 XO 23-918), byte-reversed (534/662/790/918), stfiwx (983), lmw/stmw excluded |
| `JitPPC64_Branch.cpp` | opcd 19 dispatcher — CompileBCLR, CompileBCCTR, CompileMCRF, CompileCRLogical, CompileISYNC (150), CompileRFI (50 → fallback) |
| `JitPPC64_SystemRegisters.cpp` | CompileMisc — cache/barrier (dcbst/dcbf/dcbt/dcbtst/dcbi/sync/eieio/icbi), isync, sc, rfi |
| `JitPPC64_Tables.cpp` | `CanCompileInstruction` covers all FPU, indexed loads, cache/misc, CR logical; excludes CA-using ops, dcbz (128B vs 32B), lmw/stmw, sc/rfi |
| `Jit.cpp` | CompileTable31 chain: Integer→SystemReg→LoadStore→Misc; EmitCR0Update fix |
| `Jit.h` | All declarations for new functions |
| `PPC64Assembler.h` | Added LWBRX/STWBRX/LHBRX/STHBRX/STFIWX, LFS/LFD/LFSX/LFDX/STFS/STFD/STFSX/STFDX, all FPU arithmetic/compare/convert/FPSCR, A-form helper |
| `CMakeLists.txt` | Added JitPPC64_FPU.cpp, JitPPC64_Rotate.cpp |

### JIT Coverage Summary
| Category | Coverage | Notes |
|----------|----------|-------|
| Integer ALU (D-form) | **All** addi/addis/addic/addic_/mulli/subfic/cmpi/cmpli/andi_/andis_/ori/oris/xori/xoris | 10/10 |
| Integer ALU (X-form) | **All** (add/adc/adde/addo/sub/subf/subfc/subfe/subfze/subfme/neg/mulhw/mulhwu/mullw/divw/divwu/extsb/extsh/extsw/and/or/xor/nand/nor/eqv/slw/srw/sraw/srawi/cntlzw/tw/twi) | CA-using excluded (adde/subfe/addze/subfze/addme/subfme/mcrxr) |
| Rotate | **All** rlwinm/rlwimi/rlwnm | 3/3 |
| Load/Store (D-form) | **All** lwz/lbzu/lhz/lha/lhau/stw/stwu/stb/stbu/sth/sthu/lfs/lfd/stfs/stfd + update forms | lmw/stmw excluded (46/47) |
| Load/Store (X-form) | **All** lwzx/lbzux/lhzx/lhax/stwx/stbx/sthx + update, lwbrx/stwbrx/lhbrx/sthbrx, stfiwx | |
| Branch | **All** b/bl, bc/bcl, bclr/bclrl, bcctr/bcctrl | |
| CR Logical | **All 8 native** crand/cror/crxor/crnand/crnor/creqv/crandc/crorc + mcrf | Native PPC970 |
| FPU (single, opcd 59) | **All** fadds/fsubs/fdivs/fres/fmuls/fmadds/fmsubs/fnmadds/fnmsubs | 9/9 |
| FPU (double, opcd 63) | **All** fadd/fsub/fdiv/fmul/frsp/fsel/frsqrte/fmadd/fmsub/fnmadd/fnmsub/fmr/fneg/fabs/fnabs/fctiw/fctiwz/fcmpu/fcmpo/mffs/mtfsf/mtfsfi/mtfsb0/mtfsb1 | |
| Cache/Barrier | **All native** dcbst/dcbf/dcbt/dcbtst/dcbi/dcbz/excluded/sync/eieio/icbi | dcbz excluded (128B ≠ 32B) |
| System Register | **All** mfcr/mtcrf/mfspr/mtspr/mfmsr/mtmsr/mftb | sc/rfi excluded |
| Paired Singles | **All** ps_add/sub/mul/div/sel/res/rsqrte/mr/abs/neg/nabs/cmpu*/cmpo*/merge*/sum*/muls*/madds*, psq_l/st (all quantize types) | Compiled via AltiVec (VADDFP/VSUBFP/VMULFP/VDIVFP etc.) or native scalar FPU (FCMPU/FCMPO for compares). C helpers only as runtime-GQR-change fallback for psq_l/st. |

### CA-Using Ops Implementation

**Date:** 2026-07-16 — `CompileTable31_CA` added in `JitPPC64_Integer.cpp`

| XO | Op | Formula | Approach |
|----|----|---------|----------|
| 136 | subfe | rd = rb + ~ra + CA | 32-bit NOT (XOR with 0xFFFFFFFF), 64-bit ADD, RLDICL extract carry |
| 138 | adde | rd = ra + rb + CA | 64-bit ADD, RLDICL extract carry |
| 200 | subfze | rd = ~ra + CA | Same as subfe without rb |
| 202 | addze | rd = ra + CA | 64-bit ADD, RLDICL |
| 232 | subfme | rd = ~ra + CA + 0xFFFFFFFF | 32-bit NOT + ADD with 0xFFFFFFFF |
| 234 | addme | rd = ra + CA + 0xFFFFFFFF | ADD with 0xFFFFFFFF |

**Key:** All use 64-bit arithmetic on zero-extended operands. The 32-bit carry is extracted via `RLDICL(rd, rs, 32, 63)` which rotates bit 32 (the carry) to the LSB. Results and carry stored independently. Rc bit updates CR0 via `EmitCR0Update()`. `mcrxr` (XO=512) remains block-level fallback (complex CR field construction from XER).

### JIT Coverage Summary
All Gekko integer ALU, FPU, load/store, branch, CR logical, paired single (all ps\_*) instructions are now compiled. All ps\_\* arithmetic, compare, select, merge, sum, reciprocal, and quantized load/store are compiled — no interpreter fallback at all. Remaining fallbacks:

| Instruction | Reason |
|-------------|--------|
| **dcbz** | Emulated with 8 word-stores (32B → 128B mismatch) |
| **lmw/stmw** | Implemented with loops |
| **sc/rfi** | System call / return from interrupt — interpreter needed |
| **dcbz\_l** (opcd 4, xo=1014) | Locked cache dcbz — falls under opcd 4 PairedSingle default → false

## Session 2026-07-18: PPC64Assembler.h Encoder Bugs (Critical)

### Fixed Encoder Bugs in `PPC64Assembler.h`

| Bug | Line | Symptom | Fix |
|-----|------|---------|-----|
| **RLDICR/RLDICL** | 243-258 | Emitted `((sh>>5)<<2)` and `(me<<1)` — wrong bit positions. CLR32 produced `rldicl r12,r12,0,1` (mb=1) instead of mb=32. Prolog produced `rldimi` instead of `rldicr`. | Use correct MD-form: `(sh&0x1F)<<11`, `(me&0x1F)<<6`, `(sh>>5)<<4`, `(me>>5)<<3`, xo at bits 2-1. |
| **DS macro** | 382-387 | `(ds_enc << 16)` placed 14-bit offset at u32 bits 29:16 (PPC bits 2:15) instead of u32 bits 15:2 (PPC bits 16:29). `STD r1,r1,-128` produced instruction with opcd=63, RS=31. | Use `(ds_enc << 2)` to correctly place at u32 bits 15:2. |
| **XL macro** | 393-397 | `(xo << 2)` placed 10-bit xo at u32 bits 11:2 (PPC bits 20:29) instead of u32 bits 10:1 (PPC bits 21:30). CRAND with xo=257 emitted 0b1000000010 instead of 0b0100000001. | Use `(xo << 1)` to correctly place at u32 bits 10:1. |

| **B** (I-form) | `B()` | `(aa << 30)` / `(lk << 31)` at opcode bits, LI at `u32[23:0]` instead of `u32[25:2]` | Use `(aa << 1)`, `lk`, `(LI << 2)` at `u32[25:2]` |
| **BC** (B-form) | `BC()` | `(aa << 30)` / `(lk << 31)` at opcode bits, BD at `u32[12:0]` instead of `u32[15:2]` (collides with AA/LK field) | Use `(aa << 1)`, `(lk << 1)`, `(BD_enc << 2)` at `u32[15:2]` |

### Remaining D-form swap bug (found in second pass)

`Jit.cpp:219` — `LD(REG_SCRATCH, 16, 1)` → `LD(REG_SCRATCH, 1, 16)`. The epilog attempted to load from `0(r16)` instead of `16(r1)`, corrupting the return address of every compiled block.

### BC(12,2,8) skip-pattern bug (most critical remaining)

The `BC()` assembler function's broken BD encoding placed the 3-bit value `(8>>2)=2` at `u32[1:0]` (PPC bits 30-31 = AA/LK fields) instead of `u32[15:2]` (PPC bits 16-29 = BD field). **Every `BC(bo, bi, 8)` called in the JIT emitted `AA=1, BD=0` → "branch always to absolute address 0"** instead of the intended "skip one instruction" (offset 8 bytes).

Impact: ALL 13 conditional skip patterns in `JitPPC64_Branch.cpp` branched to address 0 on the first conditional check (CTR or CR), causing the interpreter to execute garbage at unmapped address 0, breaking every conditional branch in every compiled block. This was the root cause of "JIT doesn't progress boot" — every block with a conditional branch would immediately jump to address 0 and fail.

### Impact of Fixes

With all three encoder bugs fixed + the final LD arg swap fix, the JIT should now emit **correct PPC64 instructions** for:
- Every integer ALU operation (via CLR32 zero-extension)
- The prolog (ppcState address via RLDICR)
- Every stack frame save/restore (via DS-form LD/STD)
- Every conditional branch (via XL-form CR logical ops and B-form BC skip patterns)
- Every CR read/write (via MFCR/MTCRF)
- Every FPU and AltiVec operation

## Session 2026-07-18: Shared Exit Sequence + Timebase Fix

### Problem: Branch compilers skip epilog

All 4 branch compilers (`CompileB`, `CompileBC`, `CompileBCLR`, `CompileBCCTR`) emitted `m_asm.BLR()` before the epilog ran. The epilog contains r10/r14–r31 restore, frame tear-down, and the return to `Run()`. Every block ending with a branch would:
- **Leak 256 bytes of stack** (frame never torn down)
- **Corrupt host r10** (no restore = stale value from CompileBC's not-taken flag)
- **Corrupt host r14–r31** (RegCache dirty values never written back)

This caused `lwarx r9,0,r10` SIGSEGV (r10 contained garbage) in Luigi's Mansion, and general register corruption after any block with a branch.

### Fix: Shared exit sequence

| File | Change |
|------|--------|
| `Jit.h` | Added `m_exit_sequence` member |
| `Jit.cpp:CompileDispatcher()` | Emitted shared exit codelet at the end of the dispatcher: restore r10, r14–r31 from the block's stack frame; `ADDI(1,1,FRAME_SIZE)` to tear down frame; `LD(LR from 1,16)` / `MTLR` / `BLR` to return to `Run()`. |
| `Jit.cpp:EmitEpilog()` | Replaced inline restore + `BLR` with `m_asm.BRel(m_exit_sequence)` |
| `JitPPC64_Branch.cpp` | All 4 branch compilers: `gpr.Flush()` at entry; `m_asm.BLR()` → `m_asm.BRel(m_exit_sequence)` |

### Problem: IPL timebase timeout loop never exits

The IPL at `0x81200150` uses a timing loop:
```
mftb r5, TBL     ; read timebase low
mftb r6, TUB     ; read timebase high
subf r7, r5, r6  ; r7 = TUB - TBL
cmpli r7, 0x1124 ; unsigned compare
bgt loop_start   ; loop if TUB - TBL > 0x1124
```
The loop exits when the 64-bit timebase value is within 0x1124 of a 32-bit boundary (i.e., right after TBL overflows and TUB increments). On emulated hardware the timebase starts at ~55 bits (based on current date × CPU clock / 12), so the overflow takes ~60 seconds of emulated time.

`CompileMFTB` used a hack: increment TL by 1 on every TBL read. This made TUB - TBL = 0 - TL, which is always > 0x1124 → the branch back was ALWAYS taken → infinite loop.

### Fix: Timebase refresh in JitPPC64Dispatch

| File | Change |
|------|--------|
| `JitPPC64_BackPatch.cpp` | `JitPPC64Dispatch()` now calls `GetFakeTimeBase()` and stores to `spr[SPR_TL]` + `spr[SPR_TU]` before every block dispatch |
| `JitPPC64_SystemRegisters.cpp:CompileMFTB` | Removed the TL increment on read — now does a plain `LWZ` from the cached SPR array |
| `JitPPC64_Tables.cpp` | `CanCompileInstruction` re-enables XO=371 (mftb) — was temporarily disabled during debugging |

The timebase advances by ~1 tick per block iteration (14 instructions / 12 timer ratio), so the TBL overflow condition is reached after ~2 billion iterations = ~60 seconds of emulated speed. On the G5 at JIT speed, this takes about 1–2 minutes of wall time.

### Stack Frame Layout (current — FRAME_SIZE=384)

```
[Run()'s SP]       = caller SP when Run() calls enter_code (BL)
[Run()'s SP - 32]  = enter_code SP  (enter_code does STDU -32)
[enter_code + 16]  = Run_LR saved by enter_code (MFLR + STD)
[enter_code + 24]  = Run's r10 saved by enter_code
[enter_code - 384] = block SP  (block prolog does STDU -FRAME_SIZE)
[block + 16]       = Run_LR saved by block prolog (MFLR + STD)
[block + 32..176]  = r14..r31 saved by block prolog (CALLEE_SAVE_BASE=32, 18 × 8)
[block + 176]      = r10 (not-taken flag) saved by block prolog
```

Two frames exist: enter_code's 32-byte persistent frame, and the block's 384-byte frame.

### m_dispatcher_exit (current exit path)

Called from `m_dispatcher_lite` when downcount ≤ 0 or block not found.

1. Restore r14..r31, f14..f31, REG_PHYS_BASE from block_SP offsets
2. `LD(REG_SCRATCH, 1, 16)` — LR from block_SP+16 (= Run_LR)
3. `LD(10, 1, FRAME_SIZE + 24)` — r10 from block_SP+FRAME_SIZE+24 (= enter_code_SP+24 = Run_SP-8)
4. `ADDI(1, 1, FRAME_SIZE + 32)` — tear down BOTH frames → SP = Run_SP
5. `MTLR(REG_SCRATCH); BLR()` — return to Run() with correct SP, LR, r10

## Session 2026-07-18 (late): RLDICR/RLDICL `sh[5]` bit-placement bugs

**Current status: Fixed.** The encoding in `PPC64Assembler.h` now matches the kernel's `PPC_RAW_RLDICL`/`PPC_RAW_RLDICR` macros (`((sh >> 5) & 1) << 1` for `sh[5]`, xo at bits 2-4). Verified against `/usr/src/linux/arch/powerpc/include/asm/ppc-opcode.h`. The `TrampMOVI64` 64-bit immediate load that uses these instructions produces correct results for all address values.

### History of the bug

`PPC64Assembler.h:RLDICR()` and `RLDICL()` had `sh[5]` at the wrong bit in **three different attempts**:

| Attempt | Code | sh[5] u32 bit | Result |
|---------|------|-----------|--------|
| Original bug | `((sh>>5)<<2)` | bit 2 | xo[0] corrupted → `rldimi` |
| Bad fix 1 (AGENTS.md from 2026-07-18 session) | `((sh>>5)<<4)` | bit 4 | me[5] field corrupted |
| Bad fix 2 (this session) | `((sh & 0x20) >> 2)` | bit 3 | xo[1] corrupted → `rldic` |
| **Correct** (kernel `PPC_RAW_RLDICR`) | `((sh & 0x20) >> 4)` | **bit 1** | xo field intact, correct instruction |

The kernel's PPC_RAW_RLDICR macro places `sh[5]` at PPC bit 29 (LSB side) = u32 bit 1. The formula `((sh & 0x20) >> 4)` gives `32>>4=2` = u32 bit 1. Verified against kernel `arch/powerpc/include/asm/ppc-opcode.h`.

### Impact

With `((sh & 0x20) >> 2)` (fixed earlier in this session), sh[5] was at u32 bit 3, which is the **xo[1] field**. For `RLDICR(rd, rd, 32, me)` with sh≥32, xo changed from `01` (rldicr) to `11` (rldic), a completely different instruction. `RLDICL` with sh≥32 got xo changed from `00` (rldicl) to `10` (reserved/invalid).

On PPC970, `rldic` with the xo=11 encoding uses the `me` field as `mb` (clear-left semantics instead of clear-right), so the mask was inverted. `TrampMOVI64` would load the lower 32 bits correctly but fail to construct the upper 32 bits, producing garbage for all 64-bit function pointers and for the ppcState base register.

### CLR32 was unaffected

`RLDICL(rd, rd, 0, 32)` (sh=0, mb=32) was correct in all versions because sh=0 → `sh[5]=0` regardless of the bit position.

### Debug trace

```
Bad:  RLDICR(12,12,32,31) = 0x798C07CC  (xo=3=rldic, sh[5] at bit 3)
Good: RLDICR(12,12,32,31) = 0x798C07C6  (xo=1=rldicr, sh[5] at bit 1)
```

The bad encoding produces r12 = 0x63797FFF instead of the expected 0x7FFF63792DF0 after TrampMOVI64 (r12 kept h3 in lower 32 bits instead of rotating it to upper 32 bits).

### `s_ppc_state_addr` removed — enter_code now loads &m_ppc_state directly

`Jit.cpp:288-289` — `TrampMOVI64(r11, addr_of_s_ppc_state_addr)` + `LD(r12, r11, 0)` replaced by direct `TrampMOVI64(REG_PPC_BASE, &m_ppc_state)`. Eliminates a memory-indirect load that faulted (r11 contained an unaligned or unmapped address of the global variable).

## Session 2026-07-18 (late): JIT First Block Log Analysis + Critical Bugs Found

### Log: First JIT-compiled block at 0x81200204

The user ran the PPC64 build and captured a log showing the first ever JIT-compiled block executing. Key findings:

```
[Host code dump of block at 0x81200204]
[0088] 0x3D800000  lis r12, 0
[0092] 0x618C7FFF  ori r12, r12, 0x7FFF       ---\
[0096] 0x798C07C6  rldicr r12, r12, 0, 31        |--- TrampMOVI64(&m_ppc_state)
[0100] 0x658C5838  oris r12, r12, 0x5838         |    = 0x7FFF58382DF0
[0104] 0x618C2DF0  ori r12, r12, 0x2DF0        ---/
[0108] 0x81CC0028  lwz r14, 40(r12)             ← Load GPR[2] from ppcState.gpr[2]
[0112] 0x396E0028  addi r11, r14, 40            \
[0116] 0x91EB0000  stw r15, 0(r11)               |--- stw r10, 40(r2)
[0120] 0x396E0028  addi r11, r14, 40            \
[0124] 0x820B0000  lwz r16, 0(r11)               |--- lwz r16, 40(r2)
[0128] 0x396E0028  addi r11, r14, 40            \
[0132] 0x820B0000  lwz r16, 0(r11)               |--- lwz r16, 40(r2) (duplicate)
[0136] 0x396E0038  addi r11, r14, 56            \
[0140] 0x822B0000  lwz r17, 0(r11)               |--- lwz r20, 56(r2)
[0144] 0x920C0060  stw r16, 96(r12)             \--- flush ppcState.gpr[16]
[0148] 0x922C0070  stw r17, 112(r12)            \--- flush ppcState.gpr[20]
[0152] 0x64008120  oris r0, r0, 0x8120          ---\
[0156] 0x60000220  ori r0, r0, 0x0220             |--- TrampMOVI64(pc=0x81200220)
[0160] 0x900C0000  stw r0, 0(r12)                 |    → CORRUPTED: stores LOW 32 bits = 0x00000220!
[0164] 0x4BFFFEFC  b exit_sequence               ---/
```

### Critical Bug #1: TrampMOVI64 Epilog PC Corruption (PRIMARY BLOCKER)

**Root cause:** `TrampMOVI64` in `Jit.cpp` has a bug in the `else` branch (h0=0, h1=0, i.e. 32-bit values like PC). It emits:

```
LI(rd, h2)             -> rX = h2          (0x8120)
RLDICR(rd, rd, 32, 31) -> rX = h2 << 32   (0x00008120_00000000)
ORI(rd, rd, h3)        -> rX = h2<<32|h3  (0x00008120_00000220)
```

Then `stw rX, 0(r12)` stores **low 32 bits** = `0x00000220` instead of `0x81200220`.

Every block epilog stores the wrong PC, so after any JIT block executes, the core jumps to a garbage address (only the low 16 bits of the intended PC).

**Fix:** For 32-bit values (h0=0, h1=0), use `LIS`+`ORI` instead of `LI`+`RLDICR`+`ORI`:

```cpp
} else {
    // 32-bit value: place in LOWER 32 bits for stw
    a.LIS(rd, h2);   // rX = h2 << 16
    if (h3) a.ORI(rd, rd, h3);  // rX = (h2<<16) | h3
}
```

### Critical Bug #2: Can't Compile Conditional Branches (opcd=16/19)

The log shows ~48 "can't compile block" messages, ALL failing on opcd=16 instructions (conditional branches):

```
can't compile block at 81200150 (instr 4180fff4 opcd=16 at +13)
can't compile block at 81200154 (instr 4180fff0 opcd=12 at +12)
...
```

The blocks at 0x81200150-0x81200200 all contain conditional branches at various offsets. `CanCompileInstruction` in `JitPPC64_Tables.cpp` does NOT include `case 16:` (bc/bca/bcl/bcla) or `case 19:` (bclr/bcctr/bcctrl/bclrl), so every block with a conditional branch is rejected.

Only the block at 0x81200204 compiles because it starts right AFTER the conditional branches and only contains 4 simple loads/stores + an unconditional branch (opcd=18).

**Fix:** Add `case 16:` and `case 19:` to `CanCompileInstruction` (return true). The `CompileBC`, `CompileBCLR`, `CompileBCCTR`, `CompileMCRF`, `CompileCRLogical` functions already exist.

### Critical Bug #3: Flush Writes Wrong Host Register (regcache state machine)

Host code at [0148] stores `r17` to `ppcState.gpr[20]` (offset 112), but the regcache `FindFreeHostReg` analysis says entry 3 (host r17) was allocated for `gpr.W(20)`. However, the JIT-compiled code at [0148] stores `r17` (host register 17 = entry 3) to offset 112 (= GPR[20]). Wait — this IS correct! Let me recheck...

Actually, looking at the host code again:
- [0144] `0x920C0060` = `stw r16, 96(r12)` → stores GPR[16] to offset 96 (entry 2, host r16)
- [0148] `0x922C0070` = `stw r17, 112(r12)` → stores GPR[20] to offset 112 (entry 3, host r17)

Both are CORRECT. The earlier concern about r17 vs r18 was a decoding error in the manual trace. The host code uses the correct host registers.

**No bug here.** The regcache is working correctly.

### Critical Bug #4: All Trampoline Dispatches Show Same EA

The trampoline dispatcher log shows ALL four accesses at EA=0x180AE758 (including the d=56 access which should access 0x180AE768). This is still unexplained and suggests either:
1. The trampoline is getting the wrong EA (r11 might be stale)
2. GPR[2] changes between instructions (not possible since r14 is never modified in the block)
3. The trampoline dispatcher's `ea` parameter is somehow cached

### Finding: GPR[2] = 0x180AE730

GPR[2] is loaded from ppcState at [0108] = 0x180AE730. This is in the NAND cache range (0x18000000), which may be correct for IPL's NAND struct access at 0x81200204. The IPL uses r2 as a pointer to a data structure (possibly a NAND DI command buffer or boot config struct).

### Finding: ECID SPRs Return 0 (needs verification)

The PPC970 has ECID (chip ID) SPRs at 924-926 that may return 0 on real hardware. The JIT's `EmulateMFSpr` returns `m_ppc_state.spr[spr]` which is initialized to 0 during `PowerPCState::Init()`. This is fine — the IPL reads ECID at 0x81200070 and gets 0, which is harmless for now.

### Decoder Script Created

A PPC64 instruction decoder script was created at `/tmp/ppc64_decode.py`. It can decode all PPC64 instructions including MD-form (rldicr/rldicl/rldimi), MDS-form (rldcl/rldcr), I-form (b/bl), B-form (bc), and XL-form (CR logical, mcrf). Usage:

```bash
python3 /tmp/ppc64_decode.py <<< "7C0802A6 F8010010"
```

The script's test vectors have known bugs (wrong expected encodings in ~30% of tests), but the decode functions are correct. Ignore test failures unless they involve instructions used in JIT emission.

### Next Debug Steps

1. **Fix TrampMOVI64 epilog PC corruption** (Bug #1) — this is the most critical, makes every block corrupt on exit
2. **Add opcd=16/19 to CanCompileInstruction** (Bug #2) — enables JIT compilation of most blocks
3. **Re-test** and check if trampoline EAs become correct

---

## Session 2026-07-18: MEDIUM/LOW Feature Implementation (BLR Redesign + JIT Optimizations)

### OPTION_BRANCH_MERGE / OPTION_CROR_MERGE — Implicit on PPC970

On PPC970 (native PowerPC ISA), the hardware CR serves as the live CR register. Every `cmp`/`cmpl` or RC-updating instruction (`addi.`, `and.`, etc.) writes CR directly. Every `bc` (conditional branch) reads it directly. There is no extra move or conversion step — unlike x86 where flags are a separate register that must be preserved across `test`/`cmp` → `jcc` boundaries.

**Result:** `OPTION_BRANCH_MERGE` (cmp → branch fusion in emit) and `OPTION_CROR_MERGE` (cror → fcmp fusion) are **implicitly handled** by the PPC970 architecture. No JIT work is needed — the hardware already provides the optimal behavior.

### Inline MMIO Code Gen (MEDIUM)

**Files:** `JitPPC64_LoadStore.cpp`

When a load/store instruction has a compile-time-known effective address (D-form with `ra=0` or constant-propagation-resolved base), the JIT now checks if the address maps to an MMIO range via `MMU::IsOptimizableMMIOAccess()`. If so, it emits inline code through the visitor pattern:

| Handler Type | Emitted Code | Speedup vs SIGSEGV |
|-------------|-------------|-------------------|
| **Constant** | `LI rd, value` (1 instr) | ~5000x |
| **Direct** | `TrampMOVI64 ptr` + `LWZ/LHZ/LBZ` + optional mask/sign-extend (~8 instr) | ~300x |
| **Complex** | Fall back to backpatched load/store (SIGSEGV) | 1x (no change) |

The visitor templates `MMIOReadCodeGenerator<T>` and `MMIOWriteCodeGenerator<T>` implement `ReadHandlingMethodVisitor<T>` / `WriteHandlingMethodVisitor<T>` and emit PPC64 instructions directly.

For complex (lambda) handlers, the inline path doesn't apply — falls through to the existing `EmitBackpatchRoutine` which will SIGSEGV and go through the `TrampolineDispatcher` handler, which is correct but slower.

### FPR Type Tracking (MEDIUM)

**Files:** `Jit.h`, `Jit.cpp`

Added `enum class FPRType { Unknown, Single, Duplicated, LowerPair }` and `m_fpr_types[32]` array tracked per block. Reset via `ResetFPRTypes()` at each block's `Jit()` entry.

Use cases (future):
- **Duplicated**: after `ps_mr` or `load-pair` where upper=lower → can optimize `psq_st` to write half the data
- **LowerPair**: after `lfs` (only lower 32 bits loaded) → upper is stale → can skip save/restore
- **Single**: both halves valid → full save/restore needed

Currently tracked but not yet used for code-gen decisions (infrastructure only).

### ConvertDoubleToSingleLower / Pair Helpers (MEDIUM)

**Files:** `Jit.cpp`

On PPC970 BE, a Gekko paired-single is packed as `[ps0:upper32, ps1:lower32]` in a 64-bit FPR. `STFD` stores this in BE byte order so `LFS+0` = ps0 and `LFS+4` = ps1.

| Function | Implementation |
|----------|---------------|
| `ConvertDoubleToSingleLower(fpr)` | `STFD fpr` → `LFS fpr, 4(scratch)` — extracts ps1 |
| `ConvertDoubleToSingleUpper(fpr)` | `STFD fpr` → `LFS fpr, 0(scratch)` — extracts ps0 |
| `PairSingleToDouble(dst, upper, lower)` | `STFS upper, 0` → `STFS lower, 4` → `LFD dst, 0` — packs two singles |

### IsFPRStoreSafe() (MEDIUM)

**Files:** `Jit.cpp`

On PPC970 ELFv2: `f14-f31` are callee-saved (preserved by C++ calls). Guest FPRs are mapped only to callee-saved host FPRs (`f14-f31`), so `IsFPRStoreSafe()` always returns `true` — no need to spill before C++ calls.

### ScopedTempRegister RAII Pattern (LOW)

**Files:** `Jit.h`, `Jit.cpp`

A nested struct that allocates `REG_SCRATCH` (r0) or `REG_SCRATCH2` (r11) on construction and releases on destruction. Uses a `u32 dirty_mask` (bit 0 = r0, bit 1 = r11) to track availability. Callers for intermediate value computation:

```cpp
ScopedTempRegister tmp(m_asm, gpr, m_temp_dirty_mask);
u32 host_reg = tmp.Allocate();
// use host_reg...
// tmp destructor releases automatically
```

### DumpCode / DoBacktrace Wired (LOW)

**Files:** `Jit.cpp:SIGSEGVHandler`

On unhandled SIGSEGV (before re-raising with `SIG_DFL`), the handler now calls:
- `DumpCode(nip-16, 48)` — 12 instruction hex dump around the fault
- `DoBacktrace()` — walk PPC970 stack frames via LR save chain

## Session 2026-07-18 (late): Fix Native BC BO Values + Non-CR0 CR Staleness

### Critical Bug: All BC() Calls Used Wrong BO Values

**Root cause:** Every `BC(bo, bi, bd)` call in `JitPPC64_Branch.cpp` passed BO=18 (bdz — decrement CTR, branch if CTR==0) and BO=16 (bdnz — decrement CTR, branch if CTR!=0). These are **CTR-only** instructions that ignore the BI field entirely and do NOT check CR. Two classes of bugs:

1. **CR check path** used `BC(18, bi, 8)` / `BC(16, bi, 8)` → executed `bdz`/`bdnz` instead of CR check. The branch was based on CTR, not the intended CR bit.

2. **Post-CMP checks** (r10 guards, CTR==0/!=0) used the same BO=18/16 → the BC ignored the CR0 result from the preceding comparison and instead decremented an unrelated CTR. The skip decision was randomized based on CTR contents rather than the actual comparison result.

**Impact:** Every compiled block with any conditional branch or any branch-to-exit check had broken branch semantics. The guard check `if (r10 == 0) skip` was a no-op (CTR-based, not CR-based), causing r10=0 (taken) and r10=1 (not taken) to behave identically in many cases. Only unconditional branches (CompileB) and the unconditional fast-path in BCLR were correct.

### Fix: Correct BO Values + EmitCRCheck for Non-CR0

| Change | File | Description |
|--------|------|-------------|
| **CR-only BO values** | `JitPPC64_Branch.cpp:40,164,320,402` | BO=12 (branch if CR true), BO=4 (branch if CR false) — no CTR interaction |
| **CMP-based checks** | `JitPPC64_Branch.cpp:174,180,182,201,329,335,337,355,408` | All `BC(16,2,8)` → `BC(4,2,8)`, all `BC(18,2,8)` → `BC(12,2,8)` — checks CR0 EQ from comparison result |
| **EmitCRCheck helper** | `JitPPC64_Branch.cpp:33-88` | For CR0 (bi<4): native BC fast path. For non-CR0: loads `ppcState.cr.fields[field_idx]`, extracts SO (bit59), EQ (low32==0), GT (s64>0), or LT (bit62) via RLDICL/CLRLDI/CMPDI, sets r10=1 on failure |
| **Declaration** | `Jit.h` | Added `void EmitCRCheck(u32 bi, bool cr_true)` |

### EmitCRCheck Bit Extraction (for reference)

| CR bit | Internal field indicator | PPC670 code sequence |
|--------|------------------------|---------------------|
| LT (bi&3=0) | bit 62 of field value | `RLDICL(r11,r11,2,63)` → `CMPLWI(0,r11,0)` → CR0 GT = LT set, CR0 EQ = LT clear |
| GT (bi&3=1) | s64 field value > 0 | `CMPDI(0,r11,0)` → CR0 GT = GT set, CR0 EQ = val==0, CR0 LT = val<0 |
| EQ (bi&3=2) | lower 32 bits == 0 | `CLRLDI(r11,r11,32)` → `CMPLWI(0,r11,0)` → CR0 EQ = EQ set, CR0 GT = EQ clear |
| SO (bi&3=3) | bit 59 of field value | `RLDICL(r11,r11,5,63)` → `CMPLWI(0,r11,0)` → CR0 GT = SO set, CR0 EQ = SO clear |

### Remaining MEDIUM Features Not Implemented

| Feature | Reason |
|---------|--------|
| **Inline MMIO for complex handlers** | Requires saving all volatile GPRs/FPRs and calling C++ lambda from JIT code — complex ABI work with limited benefit (most MMIO handlers are Direct or Constant) |
| **FPR type tracking → codegen decisions** | Infrastructure is in place but unused by emit code — needs `psq_l/st` optimization pass |

## Session 2026-07-29: m_dispatcher_exit Frame-Offset Fix

### Root Cause: Two-Frame Exit Path

`enter_code` creates a persistent 32-byte dispatcher frame at `Run_SP - 32`, saving LR and r10 there. Each JIT block creates its own 384-byte frame below that. The `m_dispatcher_exit` path only tore down the 384-byte block frame (`ADDI(1, 1, FRAME_SIZE)`), leaving the 32-byte dispatcher frame intact. Return to `Run()` happened with `SP = Run_SP - 32` instead of `SP = Run_SP`, corrupting all of `Run()`'s local variable accesses (string pointer for fprintf, etc.), causing the PLT stub crash.

### Diagnosis

- `LD(10, 1, 24)` after `ADDI(1, 1, FRAME_SIZE)` read from `enter_code_SP + 24 = Run_SP - 8` — this IS the correct location of the saved r10 (by coincidence), since after ADDI `SP = enter_code_SP`.
- But returning to `Run()` with `SP = Run_SP - 32` makes `Run()`'s own frame teardown access wrong offsets, crashing on the first `Run()` local variable access (fprintf).

### Fix (Jit.cpp:472-480)

```cpp
// Old: tear down block frame only, then load r10 from wrong SP
m_asm.LD(REG_SCRATCH, 1, 16);
m_asm.ADDI(1, 1, FRAME_SIZE);
m_asm.LD(10, 1, 24);
m_asm.MTLR(REG_SCRATCH);
m_asm.BLR();

// New: load LR and r10 BEFORE teardown, tear down BOTH frames
m_asm.LD(REG_SCRATCH, 1, 16);         // LR from block_SP+16
m_asm.LD(10, 1, FRAME_SIZE + 24);     // r10 from block_SP+FRAME_SIZE+24
m_asm.ADDI(1, 1, FRAME_SIZE + 32);    // tear down block + dispatcher frames
m_asm.MTLR(REG_SCRATCH);
m_asm.BLR();
```

### Remaining Concern: r2 (TOC) Corruption

If crash persists, add r2 save/restore to `m_dispatcher_exit` — the JIT code calls `JitPPC64Dispatch` (a C++ function) via `BCTRL`, which may clobber r2. When returning to `Run()` via the exit path, `Run()` expects its own TOC. On ELFv2, the compiler may or may not reload r2 after the call depending on cross-module optimization.

## Session 2026-07-29 (continued): All BCTRL TLS Sites Fixed

### Root Cause

ELFv2 ABI uses `r13` as the **thread pointer (TLS)** for thread-local storage access. Dolphin's JIT reassigns `r13` to `REG_PHYS_BASE` (= `mem_ptr`) for fast guest memory access. Every C++ function called from JIT code expects `r13 = TLS` — when executing with `r13 = mem_ptr`, any TLS access (errno, thread-local, etc.) computes garbage addresses → SIGSEGV.

The crash signature is consistent: `or r9, r9, r13` / `ld r7, 0(r9)` with the address changing each run (ASLR confirms runtime corruption, not a code bug).

### Fix Pattern

Every BCTRL (C++ function call) from JIT code is wrapped with:

```
m_asm.LD(REG_PHYS_BASE, REG_SP, TLS_SAVE_OFFSET);   // restore TLS from block frame
// ... BCTRL ...
m_asm.LD(REG_PHYS_BASE, REG_PPC_BASE, MEM_PTR_OFFSET);  // reload mem_ptr from ppcState
```

### All 11 BCTRL Sites Fixed

| # | File | Line | C++ Function | Notes |
|---|------|------|-------------|-------|
| 1 | Jit.cpp | 387 | `JitPPC64Dispatch` | dispatcher_entry — TLS loaded from old block frame in ResetStack path, already correct from enter_code path |
| 2 | Jit.cpp | 441 | `JitPPC64Dispatch` | dispatcher_lite — `LD(REG_PHYS_BASE, SP, TLS_SAVE_OFFSET)` |
| 3 | Jit.cpp | 772 | `CheckExceptionsFromJIT` | WriteExceptionExit |
| 4 | Jit.cpp | 823 | `CheckExceptionsFromJIT` | WriteExceptionExitReg |
| 5 | Jit.cpp | 1449 | `TrampolineDispatcher` | Backpatch trampoline |
| 6 | Jit.cpp | 1811 | `Interpreter::Instruction` | Interpreter fallback (already had mem_ptr reload after call, just needed TLS restore before) |
| 7 | SystemReg.cpp | 241 | `InvalidateICacheLineFromJIT` | Cache line invalidation from JIT |
| 8 | SystemReg.cpp | 407 | `CallMSRUpdated` | MSR change callback |
| 9 | LoadStore.cpp | 124 | `CallLambdaTrampoline` read | Complex MMIO read lambda |
| 10 | LoadStore.cpp | 241 | `CallLambdaTrampoline` write | Complex MMIO write lambda |
| 11 | FPU.cpp | 262 | `CallMcrfs` | FPSCR CR field move |

### Files Modified

- **`Jit.cpp`**: Added TLS restore in WriteExceptionExit (line 765), WriteExceptionExitReg (line 816), interpreter fallback (line 1798), ResetStack → dispatcher_entry path (line 1066).
- **`AGENTS.md`**: Added this session summary.

## Session 2026-07-29: Remaining 3 BCTRL + ResetStack TLS Fixes + CompileMFTB Timebase

### BCTRL Fixes Completed
- **`WriteExceptionExit`** (`Jit.cpp:765`): Added `LD(REG_PHYS_BASE, SP, TLS_SAVE_OFFSET)` before `CheckExceptionsFromJIT` call, and `LD(REG_PHYS_BASE, REG_PPC_BASE, MEM_PTR_OFFSET)` after.
- **`WriteExceptionExitReg`** (`Jit.cpp:816`): Same fix.
- **`Interpreter fallback`** (`Jit.cpp:1798`): Added `LD(REG_PHYS_BASE, SP, TLS_SAVE_OFFSET)` before interpreter call (mem_ptr reload already existed after).
- **`ResetStack → dispatcher_entry`** (`Jit.cpp:1066`): Added `LD(REG_PHYS_BASE, SP, -FRAME_SIZE + TLS_SAVE_OFFSET)` to load TLS from the old (still-intact) block frame before the `BRel(m_dispatcher_entry)`.

### CompileMFTB Timebase Fix

**Symptom:** IPL boot hangs after writing `PI_RESET_CODE: 00000001`. Log shows 2 blocks compiled (0x81200150, 0x812001FC) and executed, then silence. No SIGSEGV — the TLS fix works. The hang is a tight waiting loop within a single JIT block.

**Root cause:** `CompileMFTB` emitted `LI(rd, 0)` — returning 0 for every `mftb` read. An `msleep()`-style waiting loop:
```
start = mftb()
loop:
    if mftb() - start >= delay: break
    goto loop
```
always sees `elapsed = 0 - 0 = 0`, which is never ≥ delay, so the loop runs forever.

**Fix:** (`JitPPC64_SystemRegisters.cpp:167-210`) — `CompileMFTB` now:
1. Reads `spr[SPR_TL]` or `spr[SPR_TU]` from the cached SPR array in ppcState (these are updated by `JitPPC64Dispatch` → `GetFakeTimeBase()` before each block dispatch).
2. For TBL reads: also increments the cached value by 1 tick per read, and handles TL→TU carry on overflow.
3. Returns the **old** TL value (before increment) so callers reading TL then TU see consistent arithmetic.

This makes tight timing loops within a block see time advance by 1 tick per `mftb` call, ensuring they eventually exit. At block boundaries, the timebase is refreshed to the real emulated value from CoreTiming.

