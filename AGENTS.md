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

### Core/IOS/Network/KD (NWC24/WC24 - Magic mismatch errors)

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

### 1. TimeBase Read/Write (IPL boot hang fix)
`PowerPC.cpp:399-408` — `ReadFullTimeBaseValue()`/`WriteFullTimeBaseValue()` used `std::memcpy` to read/write `spr[SPR_TL..SPR_TU]` as a u64. On BE, `memcpy` interprets the pair in host byte order (big-endian), so `spr[SPR_TL]` (at the lower address) becomes the upper 32 bits instead of the lower. This corrupted the Time Base value returned to the IPL's `__OSGetSystemTime()` (`mftb` → `mfspr` → `ReadFullTimeBaseValue()`), causing the timer expiry check in DvdStep state 2 to never pass. The IPL hung forever at `PI_RESET_CODE: 00000001` without ever sending DI commands.

**Fix:** Replaced `memcpy` with explicit shifts:
- `Read`: `(TU << 32) | TL` (combines as arithmetic values, not by pointer reinterpretation)
- `Write`: `TL = u32(value); TU = u32(value >> 32)` (writes halves individually)

### 2. VertexLoader posmtx Byte Order (Opaque Matrix / Vertex Blast fix)

**Status:** REVERTED — commit 96f09ce2e3 was reverted by bf7dcbc31b (broke OGL, Mario 3D chest regression). See Pending Fix #2 below.

**Root cause:** `VertexLoader::PosMtx_ReadDirect_UByte` wrote `DataWrite<u32>(posmtx)`. On BE, the u32 store placed the value in the 4th byte (MSB). The SW renderer reads byte 0 (`ReadVertexAttribute<u8>(..., base_component=0)`), getting 0 → every vertex uses matrix index 0 → identity bind pose → spiky geometry.

**Why the revert:** OGL backend uses `glBufferSubData` for vertex buffer upload, and Mesa on PPC64 BE byteswaps the u32 data during `glBufferSubData` (confirmed at OGLStreamBuffer.cpp:308-309), turning `[0, 0, 0, value]` back to `[value, 0, 0, 0]`. So the original `DataWrite<u32>(posmtx)` is correct for OGL. The fix belongs only on the SW reader side (`SWVertexLoader.cpp`).

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

## Architectural Notes for Future JIT Port

When implementing a native PPC64 JIT on PPC64 BE:
- No instruction byteswap needed (PPC guest = PPC host endianness)
- Emulated memory can be mapped directly (no endian conversion)
- Page size difference (4K vs 64K) needs consideration for fastmem
- ELFv2 ABI differences from ELFv1

## Commit Strategy

Each functional area (Swap.h, Blob readers, Volume code, Boot code) should be committed separately for clarity. Use descriptive messages prefixed with `[PPC64-BE]`.
