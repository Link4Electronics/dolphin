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

**Root cause:** All three exception vector addresses were set to physical addresses (`0x00000nnn`). Physical address `0x0` is not mappable on PPC64 (kernel forbids `MAP_FIXED` at `0x0` — `EPERM`). The NCE executable mapping only covers `0x70000000-0x82000000` (K2 + K1 cached). When `sigreturn` restored NIP to a physical address, the CPU immediately SIGSEGV'd on instruction fetch from unmapped memory, causing an infinite loop of SIGSEGV → DSI/ISI injection → repeat. Fix: use the K1 cached alias (`0x80000nnn`) which IS in the NCE mapping. The interpreter's `GetPointerForRange` masks with `0x3FFFFFFF`, so `0x80000nnn` maps to physical `0x00000nnn` for interpreter uses.

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

## Native Code Execution (NCE) for PPC64

### Current Status

Initial NCE stub skeleton is in `Source/Core/Core/PowerPC/JitPPC64/`. The NCE option appears as a CPU engine choice in the Qt GUI. Currently the trampoline is being debugged — the IPL first instruction is reached via the SHM-backed NCE mapping but crashes at MMIO access.

### Architecture

- **`JitPPC64`** class extends `JitBase` and implements `CPUCoreBase`
- Reuses existing `JitCache` / `BlockCache` / `PPCAnalyst` infrastructure
- Instruction selection tables mirror Jit64/JitArm64 structure
- Signal handlers for SIGILL (Paired Singles, supervisor SPRs), SIGSEGV (MMIO), SIGALRM (timer), and SIGTRAP (debug traps)

### Signal Handlers

| Signal | Handler | Purpose |
|--------|---------|---------|
| SIGSEGV | `HandleSIGSEGV` | MMIO access, slowmem fallback, DSI emulation |
| SIGILL | `HandleSIGILL` | Supervisor SPRs, Paired Singles, MSR[SF]=0 |
| SIGALRM | `HandleSIGALRM` | Periodic downcount estimation (replaces interpreter loop) |
| SIGTRAP | `HandleSIGTRAP` | Debug trap (for testing trampoline entry) |

All handlers restore host r2 (TOC) and r13 (TLS) before any C++ member access.

### NativeContext Layout (must match JitAsm.S)

| Offset | Field | Size | Description |
|--------|-------|------|-------------|
| 0 | `host_r2` | 8 | Host TOC pointer |
| 8 | `host_r13` | 8 | Host TLS pointer |
| 16 | `host_r1` | 8 | Host stack pointer |
| 24 | `return_addr` | 8 | Host LR (return address) |
| 32 | `host_cr` | 8 | Host CR |
| 40-176 | `host_gpr14_31` | 144 | Host callee-saved GPRs (18 × 8) |

Total: 184 bytes, aligned to 16 bytes.

### GuestRegs Layout (must match JitAsm.S)

| Offset | Field | Size | Description |
|--------|-------|------|-------------|
| 0-127 | `gpr[32]` | 128 | Guest GPRs (each 4 bytes) |
| 128 | `pc` | 4 | Guest PC (NIP) |
| 132 | `cr` | 4 | Guest CR |
| 136 | `lr` | 4 | Guest LR |
| 140 | `ctr` | 4 | Guest CTR |
| 144 | `xer` | 4 | Guest XER |

### Critical Bug: .S File .text Not Executable on This System

The PPC64 BE system (Arch Linux, kernel 7.1) **prevents execution from anonymous `MAP_PRIVATE` pages** and may also place separate `.text` sections from `.S` files in non-executable memory. This is likely due to a kernel security feature (`PaX MPROTECT`, `CONFIG_STRICT_MEMORY_RWX`, or similar).

**Evidence from testing:**
- `&JitPPC64EnterGuest` returns a valid address → reading via `*(volatile u32*)` gives correct value (0x7FE00008 = `trap`)
- Calling via function pointer → SIGSEGV (core dumped), no signal handler output
- `mprotect(page, PROT_READ|PROT_EXEC)` returns 0 but still SIGSEGV
- `mmap(MAP_PRIVATE|MAP_ANONYMOUS, PROT_READ|PROT_WRITE|PROT_EXEC)` returns a valid page → writing `trap; blr` and calling it → SIGSEGV
- The SHM-backed `MAP_SHARED` NCE mapping (created via `shm_open` + `mmap` with MAP_SHARED) **IS executable** — C function pointer test to 0x81200150 succeeded

**Solution:** All NCE trampoline code must execute from the SHM-backed NCE mapping, not from `.text` sections of `.S` files. Either:
1. Copy trampoline to the NCE mapping at init time and jump there, OR
2. Use `asm()` at file scope in `.cpp` files (same `.text` as C++ code, proven executable)

### Current NCE Test Flow (as of Jul 2026)

1. `Run()` installs signal handlers, maps SHM-backed NCE memory, starts ALRM timer
2. Inner loop: `FillGuestRegsForEntry`, check PC range, set `0x80003FF8 = &local_ctx`, copy trampoline to `0x80010000`, call `JitPPC64EnterGuest` via function pointer
3. Trampoline saves host context (r2, r13, r1, LR, CR, r14-r31) to stack-local `NativeContext`, restores guest GPRs from `GuestRegs`, `bctr` to guest PC
4. Guest executes natively in `0x80000000–0x82000000` range:
   - `sc` → SIGILL → safety handler (nip+=4) — **skips syscall, breaks guest state**
   - Supervisor SPR (mtspr/mfspr) → SIGILL → `HandleSIGILL` → `EmulateMFSpr`/`EmulateMTSpr`
   - MMIO access → SIGSEGV → `HandleSIGSEGV` → MMIO read/write or slowmem
   - ISI at unmapped address → SIGSEGV → `HandleSIGSEGV` → instruction-fetch shortcut → `EmulateDSI`
5. ALRM fires every 2ms → `HandleSIGALRM` decrements downcount by 128. When downcount ≤ 0 and `0x80003FF8` points to a valid NativeContext (non-null, non-sentinel): save guest state, restore host regs, sigreturn to Run()
6. Back in Run(): null-out `0x80003FF8`, copy back `m_native_ctx = local_ctx`, set sentinel. Continue inner while.
7. If guest PC is outside `0x80000000–0x82000000`: check via `GetPointerForRange` — if valid, interpret one instruction; if invalid, inject ISI (set SRR0=PC, SRR1=MSR, clear MSR, PC=0x400)

### Key NCE Fixes

| Fix | File | Description |
|-----|------|-------------|
| `.S` → `asm()` in `.cpp` | `Jit.cpp:74-94` | `.S` .text not executable on PaX kernel; use file-scope `asm()` in `.cpp` |
| `oris` base register | `Jit.cpp:78-79` | `li r12,0` before `oris r12,r12,0x8000` (PPC `oris` does NOT zero upper bits) |
| No `sc` in asm entry | `Jit.cpp:87-94` | `sc` clobbers r3-r8 (kernel syscall ABI); signal args would be lost |
| Sentinel return_addr | `Jit.cpp:275` | `m_native_ctx.return_addr = 0xFFFFFFFFFFFFFFFFULL` prevents ALRM from exiting during setup |
| Null-out after enter | `Jit.cpp:288` | `*reinterpret_cast<...>(0x80003FF8) = nullptr` BEFORE copy-back closes ALRM race window |
| Null-check in asm entries | `Jit.cpp:99-100,109-110` | `cmpdi r12,0; beq 1f` skips r2/r13 restore when 0x80003FF8 is null (prevents double fault) |
| PC range fallback | `Jit.cpp:257-268` | Check PC outside NCE range before trampoline; if invalid address → ISI injection |
| `_exit(1)` instead of EmulateDSI | `Jit.cpp:762` | Host-code SIGSEGV → `_exit(1)` instead of setting nip to 0x300 (guest DSI vector) |
| Duplicate fprintf removed | `Jit.cpp:262-263` | Accidental double `fprintf` of "NCE: enter" message |
| FPR save/restore for psq_l/st | `Jit.cpp:930-967,2042-2060` | `SaveFPRsFromContext`/`RestoreFPRToContext` — copies all 32 FPRs between ucontext and `m_ppc_state.ps[]`, splitting/packing Gekko two-float FPR format. Used in P0 else-branch for psq_l/st. |
| CR/FPSCR save/restore in EmulatePairedSingle | `Jit.cpp:2495-2515` | `EmulatePairedSingle` now also saves/restores CR (`regs->ccr`) and FPSCR (`fp_regs[32]`) from ucontext before `FallBackToInterpreter`. |
| BAT SPR array bounds fix | `Jit.cpp:2263-2276,2358-2375` | Both Gekko (528-543) and PPC970 (560-575) BAT SPR ranges now compute IBAT/DBAT indices independently for `[0..3]` instead of `[4..11]` (off-by-4 overflow bug). |
| mfmsr/mtmsr P0 fix | `Jit.cpp:1965-2010` | Added `mfmsr` (xo=83) and `mtmsr` (xo=146) to `IsP0Instruction()` — they're valid PPC970 supervisor instructions (no SIGILL), now patched to `trap` → emulated via interpreter. |
| PC advance after interpreter | `Jit.cpp:660-715` | Pre-entry `FallBackToInterpreter` paths (P0 check + PC-range check) now set `npc=pc+4` before and `pc=npc` after, matching `SingleStepInner` dispatch. Without this, `mfmsr` in P0 check looped forever. |
| ps\_\* → AltiVec trampoline | `Jit.cpp:323-540,621-680` | `GeneratePsTrampolines()` at `0x7E000000` for ps\_add/sub/mul/mr: `stfd`→`lvx`→AltiVec→`stvx`→`lfd`→`b return`. `PatchAllP0` patches ps\_\* with `b trampoline` (AA=1) instead of `trap`. `MSR[VR]=1` set at `Run()` init. |

### Known Blockers

| Blocker | Cause | Workaround |
|---------|-------|------------|
| **sc not emulated (FIXED)** | SIGILL now routed to `HandleSIGILL` (was `nce_safety_handler` which skipped). `HandleSIGILL`'s existing `opcd==17` handler calls interpreter's `sc` handler → sets `EXCEPTION_SYSCALL` → `CheckExceptions` → `SRR0=pc+4`, `SRR1=msr`, `msr&=~0x04EF36`, `pc=0xC00`. |
| **K2 not mapped (FIXED)** | `InitNCEGuestMapping` now maps `0x70000000-0x7FFFFFFF` (256MB, SHM offset 0=RWX). Inner loop range expanded from `0x80000000-0x82000000` to `0x70000000-0x82000000`. |
| **Interrupt vectors** | Vectors at `0x00000000–0x00000FFF` — can't mmap at 0 on some kernels; run through interpreter fallback. On systems where `0x00000000` mmap succeeds, vectors are in the NCE physical mapping but outside the inner-loop range (pc < 0x70000000) so they still run through interpreter. |
| **`trap` instructions in trampoline** | Two `trap` in `JitAsm.S` (entry + before `bctr`). No longer needed — removed. With `SIGILL` now routed to `HandleSIGILL`, `trap` + unknown opcd → exit to `Run()` instead of `nip+=4`, breaking first entry. |

### Files

- `Source/Core/Core/PowerPC/JitPPC64/Jit.cpp` — asm entries, all signal handlers, `Run()` inner loop, `Init()`
- `Source/Core/Core/PowerPC/JitPPC64/JitAsm.S` — trampoline definition (`JitPPC64EnterGuest`)
- `Source/Core/Core/PowerPC/JitPPC64/Jit.h` — struct/class declarations
- `Source/Core/Core/HW/Memmap.cpp` — `InitNCEGuestMapping()`, `ShutdownNCEGuestMapping()`

### Build Config

The `_M_PPC_64` define is set when `_ARCH_64` and `CMAKE_SYSTEM_PROCESSOR` matches `powerpc64|ppc64|ppc64le`. This activates the `JitPPC64/` source files and the NCE CPU engine option.

## Trap-and-Emulate Coverage (Gekko vs PPC970 ISA Gaps)

The PPC970 (G5) lacks many Gekko/Broadway-specific instructions. NCE must trap them via SIGILL or SIGSEGV and emulate them. Below is the current coverage.

### Instructions that DON'T SIGILL (silently wrong results — P0)

These are the most dangerous. They execute as different instructions on PPC970.

| Instruction | Gekko | PPC970 behavior | NCE impact |
|-------------|-------|-----------------|------------|
| **`dcbz`** (31, xo=1014) | Zero 32 bytes | Zero **128 bytes** — corrupts adjacent 96 bytes | Silent memory corruption. Cannot trap via SIGILL. Fix: page-protection trick or interpreter fallback for dcbz-heavy code. |
| **`psq_l`/`psq_lu`** (opc 56/57) | Paired-single quantized load | Executes as `lq`/`lq_u` (Load Quadword, 16 bytes) | Wrong register values, 16-byte read instead of 8. No SIGILL. |
| **`psq_st`/`psq_stu`** (opc 60/61) | Paired-single quantized store | Executes as `stq`/`stq_u` (Store Quadword, 16 bytes) | Wrong register values, 16-byte write → adjacent memory corruption. No SIGILL. |
| **`mftb`** (31, xo=371) | Returns Dolphin's fake timebase | Returns real PPC970 timebase | Wrong frequency, breaks timing-dependent code. No SIGILL. |
| **`mfspr PVR`** (SPR 287) | Returns `0x00007000` | Returns real PPC970 PVR (e.g. `0x0039xx`) | Game reads wrong CPU version. No SIGILL (user-readable on all PPC). |
| **`mfspr TL/TU`** (SPR 268/269) | Dolphin's emulated timebase | Real PPC970 timebase | Same timing issue as `mftb`. |
| **`mfmsr`** (31, xo=83) | Returns emulated Gekko MSR | Returns real PPC970 MSR | Wrong MSR bits (DR, IR, EE, etc.). Game reads real HW values, corrupting exception handling and MMU state. No SIGILL (valid supervisor instr). |
| **`mtmsr`** (31, xo=146) | Writes emulated Gekko MSR | Writes real PPC970 MSR | Dangerous: can disable interrupts (EE), change endianness (LE), or modify host MMU bits. No SIGILL (valid supervisor instr). |
| **`ps_*` arithmetic** (opc 4) | Paired-single FP ops | Executes as AltiVec Vector ops (opcd 4 is AltiVec primary) | Wrong results (different operation mapping). E.g. `ps_add` → `vaddfp` (correct!) but `ps_abs` → some other AltiVec op. Some have NO equivalent (ps_div). |
| **`ps_mr`** (opc 4, xo=528) | Copy frB → frD | Some AltiVec op | Wrong register copy. |

**Current fix for P0 items:** All P0 instructions are patched in RAM before NCE entry. Non-ps\_\* P0 instructions get `trap`→SIGILL→interpreter fallback. ps\_\* arithmetic (add/sub/mul/mr) get `b trampoline`→AltiVec native execution (avoids signal). Other ps\_\* (psq_l/st, ps_div, ps_abs/neg/nabs, ps_sel, etc.) get `trap`→SIGILL→interpreter fallback with FPR save/restore.

### Instructions that SIGILL (P1 — currently handled)

These trap via SIGILL and are emulated. Updated coverage in `HandleSIGILL`, `EmulateMFSpr`, `EmulateMTSpr`:

| Instruction | Opcode/XO | Handler | Status |
|-------------|-----------|---------|--------|
| All `ps_*` paired singles | opc 4 | `EmulatePairedSingle` → `SaveFPRsFromContext` + `FallBackToInterpreter` + `RestoreFPRToContext`, also saves/restores CR and FPSCR | **OK** — FPR/CR/FPSCR state at ucontext boundary now correct |
| `psq_l`/`psq_lu`/`psq_st`/`psq_stu` | opc 56/57/60/61 | P0 handler else-branch: `SaveFPRsFromContext` before interpreter, `RestoreFPRToContext(FD)` after for loads | **OK** — FPR state at ucontext boundary now correct |
| `dcbz_l` (locked cache dcbz) | opc 4, xo=1014 | Falls under `IsPairedSingleOpcd(4)` → `FallBackToInterpreter` | **OK** (but mis-categorized) |
| `mfspr`/`mtspr` (any SPR) | 31, xo=339/467 | `EmulateMFSpr`/`EmulateMTSpr` | **OK** — all Gekko SPRs now covered |
| `mfmsr`/`mtmsr` | 31, xo=83/146 | `EmulateMFMSR`/`EmulateMTMSR` | **OK** — modifies m_guest.msr only, not real PPC970 MSR |
| `mfsr`/`mtsr` | 31, xo=595/210 | Segment reg read/write | **OK** |
| `rfi` | 19, xo=50 | `EmulateRFI` | **OK** |
| `sc` (syscall) | opc 17 | `FallBackToInterpreter` | **OK** |
| `dcbi` | 31, xo=470 | Calls `m_mmu.InvalidateDCacheLine(ea)` | **FIXED** (was no-op, now properly invalidates) |
| `icbi` | 31, xo=982 | `FallBackToInterpreter` | **OK** |
| `tlbie` | 31, xo=306 | `FallBackToInterpreter` | **OK** |
| `tlbsync` | 31, xo=566 | `FallBackToInterpreter` | **OK** |
| `mtsrin`/`mfsrin` | 31, xo=242/659 | `FallBackToInterpreter` | **OK** |
| `eciwx`/`ecowx` | 31, xo=310/438 | `FallBackToInterpreter` (if unimplemented on PPC970 → SIGILL) | **OK** |
| Paired single FP (opc 59/63) | — | NOT handled (don't SIGILL on PPC970). Native PPC970 FPU handles them | **OK** — standard PPC FP, correct native execution |
| `mfdcr`/`mtdcr` | 31, xo=166/454 | `FallBackToInterpreter` (generic SIGILL handler) | **OK** — DCRA removed in ISA 2.01, always SIGILLs; interpreter returns 1 for "ready" |

### SPR Coverage Added (2026-07-14)

| SPR | Number | EmulateMFSpr | EmulateMTSpr | Notes |
|-----|--------|-------------|-------------|-------|
| GQR0-7 | 912-919 | Returns `m_ppc_state.spr[spr]` | Stores to `m_ppc_state.spr[spr]` | Graphics quantization, needed for psq_* interpreter fallback |
| WPAR | 921 | Returns `m_ppc_state.spr[spr]` with BNE bit from GPFifo | Stores + calls `ResetGatherPipe()` | Write gather pipe (GP FIFO communication) |
| DMAU | 922 | Returns `m_ppc_state.spr[SPR_DMAU]` | — (written via DMAL trigger) | DMA address |
| DMAL | 923 | Returns `m_ppc_state.spr[SPR_DMAL]` | Full DMA emulation via `m_mmu.DMA_MemoryToLC`/`DMA_LCToMemory` | Locked cache DMA trigger |
| ECID_U/M/L | 924-926 | Returns `m_ppc_state.spr[spr]` (set during Init) | — (read-only) | Chip ID |
| UPMC1/USIA/UPMC2 | 937-939 | Returns PMC1/SIA/PMC2 | — (read-only aliases) | User-mode perf counter aliases |
| UPMC3/UPMC4 | 941-942 | Returns PMC3/PMC4 | — (read-only aliases) | User-mode perf counter aliases |
| PMC1-4 | 953-954, 957-958 | Returns PMC value | — | Performance monitor counters |
| SIA | 955 | Returns SIA | — | Sampled Instruction Address |
| IABR | 1010 | Returns `m_ppc_state.spr[SPR_IABR] & ~1` | — | Instruction breakpoint (TE bit clears on read) |
| DABR | 1013 | Returns `m_ppc_state.spr[SPR_DABR]` | — | Data breakpoint |
| ICTC | 1019 | Returns `m_ppc_state.spr[SPR_ICTC]` | Stores to `m_ppc_state.spr[SPR_ICTC]` | Instruction cache timing control |
| THRM1-3 | 1020-1022 | Returns thermal value | Stores value (simplified — no thermal interrupt emulation) | Thermal monitoring |
| SDR1 | 25 | — (handled by default fallback) | Stores + calls `m_mmu.SDRUpdated()` | Page table base |
| EAR | 282 | — (handled by default fallback) | Stores to `m_ppc_state.spr[SPR_EAR]` | External Access Register |
| MMCR0/MMCR1 | 952/956 | Returns `m_guest.mmcr0`/`mmcr1` (existing) | Stores + calls `PowerPC::MMCRUpdated(m_ppc_state)` | Performance monitor control |

### Commit History

| Date | File(s) | Change |
|------|---------|--------|
| 2026-07-15a | `Jit.cpp`, `Jit.h` | **FPR save/restore**: Added `SaveFPRsFromContext` (copy all 32 FPRs from ucontext to `m_ppc_state.ps[]`, splitting Gekko packed 64-bit FPR into two doubles) and `RestoreFPRToContext` (pack one FPR back). Used in P0 psq_l/st path and `EmulatePairedSingle`. Added CR/FPSCR save/restore to `EmulatePairedSingle`. |
| 2026-07-15b | `Jit.cpp` | **BAT SPR off-by-4 fix**: Both `EmulateMFSpr` and `EmulateMTSpr` computed IBAT/DBAT array indices incorrectly using `idx` directly (DBAT) or `4+idx` (PPC970 range), overflowing the `[8]` arrays. Fixed: compute independent `idx` for IBAT vs DBAT, staying in `[0..3]`. |
| 2026-07-15c | `Jit.cpp` | **mfdcr/mtdcr handler (REMOVED)**: Added XO=166/454 handlers for DCRA polling, but the stuck instruction was `mfmsr`, not `mfdcr`. Removed in 2026-07-15d. |
| 2026-07-15d | `Jit.cpp` | **mfmsr/mtmsr P0 fix + PC advance fix**: Added `mfmsr` (xo=83) and `mtmsr` (xo=146) to `IsP0Instruction()` — these are valid PPC970 supervisor instructions that don't SIGILL, returning the real PPC970 MSR instead of the emulated Gekko MSR. Also fixed pre-entry `FallBackToInterpreter` paths (P0 check + PC-range check) to properly advance `m_ppc_state.pc` after interpreter execution: `npc=pc+4` before, `pc=npc` after — matching `Interpreter::SingleStepInner` dispatch. Without this, `mfmsr` in the P0 check path would loop forever because the interpreter handler doesn't modify PC. Removed dead mfdcr/mtdcr SIGILL chain handlers. |
| 2026-07-15e | `Jit.cpp`, `Jit.h` | **ps\_\* → AltiVec trampoline prototype**: Generate native AltiVec trampolines in the NCE K2 mapping at `0x7E000000` for ps\_add/sub/mul/mr. Each 64-byte trampoline does `stfd` FPRs→scratch → `lvx`→VRs → `vaddfp`/`vsubfp`/`vmulfp`/`vor` → `stvx`→`lfd` back → `b addr+4`. ps\_\* sites are patched with `b trampoline_addr` (absolute, AA=1) instead of `trap`, avoiding the ~2µs SIGILL round-trip. `PatchAllP0`/`UnpatchAllP0` handle both branch-patched (ps\_\*) and trap-patched (other P0) instructions with the same restore-from-map mechanism. `MSR[VR]=1` is set once at `Run()` init via `mtmsrd` so AltiVec instructions execute without a Vector Unavailable exception. |
| 2026-07-14a | `Source/Core/Core/PowerPC/JitPPC64/Jit.cpp` | Added all missing Gekko SPRs to `EmulateMFSpr`/`EmulateMTSpr` (GQR0-7, WPAR, DMAU, DMAL, ECID, UPMC, PMC, SIA, IABR, DABR, ICTC, THRM, SDR1, EAR, MMCR). Fixed `dcbi` to call `InvalidateDCacheLine`. Added MSR[DR]/MSR[IR] logging in `HandleSIGALRM`. Added `#include Core/HW/GPFifo.h` for WPAR/BNE access. |
| 2026-07-14b | `Source/Core/Core/PowerPC/JitPPC64/Jit.cpp` | **MSR[DR]/MSR[IR] hypothesis refuted**: added logging in `HandleSIGALRM` — always DR/IR=0/0. Replaced `_exit(1)` with `EmulateDSI` in the unhandled-fault path in `HandleSIGSEGV`. **Pre-check fault address before `SlowmemDataAccess`**: if the fault address is outside valid guest memory (RAM or EXRAM), inject DSI directly instead of calling `SlowmemDataAccess` (which silently drops invalid accesses and corrupts guest state). Added `valid_addr` helper that checks both RAM and EXRAM ranges via 30-bit mask. |
| 2026-07-14c | `Source/Core/Core/PowerPC/JitPPC64/Jit.cpp`, `Jit.h` | **IPL vector address fix**: DSI/ISI vectors at `0x80000300`/`0x80000400` are zeroed (no IPL code at SHM offset 0x300/0x400). Changed to `0x81200200`/`0x81200300` (IPL file offsets 0x300/0x400 → physical 0x01200200/0x01200300). |
| 2026-07-14d | `Source/Core/Core/PowerPC/JitPPC64/Jit.cpp`, `Jit.h` | **IPL vector injection is fundamentally broken on real PPC970**: `mfspr SRR0` reads the **real** PPC970 SPR, not our emulated `m_guest.srr0`. Linux SIGSEGV delivery does NOT save SRR0/SRR1 in the ucontext (`pt_regs`). The IPL handler uses stale/garbage return address → crash. **Fix**: replaced ALL IPL vector injection with `ExitNCEFromSignal()` — restores host registers from NativeContext and returns to `Run()` loop, which handles the fault via interpreter fallback. Changed: Run() loop ISI injection → `FallBackToInterpreter`; SIGSEGV instruction-fetch path → `ExitNCEFromSignal`; all 3 `EmulateDSI` call sites → `ExitNCEFromSignal`. Added `ExitNCEFromSignal()` helper method in `JitPPC64`. |
| 2026-07-14e | `Jit.cpp` | **ExitNCEFromSignal re-entry fix**: Moved `m_ppc_state.pc` from unconditionally `pc_val` to `skip_instruction ? (pc_val + 4) : pc_val`. Data faults (skip=true) now advance past the faulting instruction to prevent infinite re-entry loop. Instruction-fetch faults (skip=false) keep pc for interpreter fallback. |
| 2026-07-14f | `Jit.cpp` | **MMIO handler opcode coverage**: Added all missing D-form load/store opcodes to the MMIO handler in `HandleSIGSEGV`: lwzu(33), lbzu(35), lhzu(41), lhau(43), stwu(37), stbu(39), sthu(45), lmw(46), stmw(47). Update-form opcodes do MMIO access + RA write-back. Multiple opcodes (lmw/stmw) access consecutive words starting from D-form EA. **Temporary stack fix**: In `Run()` loop, if `m_ppc_state.gpr[1] == 0`, set to `0x80100000` before NCE entry to prevent infinite loop from `stwu` with r1=0 accessing `0xFFFFFFF0`. |
| 2026-07-14g | `Memmap.cpp`, `Jit.cpp` | **K2 uncached mapping** at 0xB0000000-0xBFFFFFFF (256MB, SHM offset 0). Guest code can branch into K2 uncached territory (e.g., 0xB7240470). Added mapping, range checks, shutdown unmap, and MAPS filter. |
| 2026-07-14h | `Jit.cpp` | **EXRAM-aware range checks**: Both Run() loop guards now handle K1 block 1 (0x90000000) and K1 uncached block 1 (0xD0000000) by checking against `exram_size` instead of `ram_size`. **Interpreter fallback safety**: Before calling `Memory::Read_U32(m_ppc_state.pc)` in the interpreter fallback path, validates the address against RAM/EXRAM ranges to prevent Unknown Pointer crash when NCE exits at a non-mapped K1 block 1 address (e.g., Luigi's Mansion at 0x97A10470). |

## NCE vs JIT Performance Analysis

### Trap-and-Emulate vs AltiVec JIT for Missing Instructions

PPC970 (G5) lacks Gekko-specific instructions (Paired Singles, dcbz with 32-byte zero, mftb, mfspr PVR/TL/TU). Two approaches exist for handling them:

| Approach | Per-instruction cost | Dev time | Performance vs CachedInterpreter |
|----------|---------------------|----------|----------------------------------|
| **Trap** | SIGILL → kernel → C++ handler → sigreturn: ~1-5μs = ~2000-10000 native cycles | Low (extend existing patching) | ~20-50x faster (trap rate <1% of instrs) |
| **AltiVec trampoline (prototype)** | Inline `b trampoline` → AltiVec → `b return`: ~50 native cycles, no signal | Moderate (existing P0 patching infra) | ~100-200x faster (ps_* only) |
| **Full AltiVec JIT translator** | Native ALU execution via vaddfp/vmulfp/etc + manual quantize for psq | High (~weeks) | ~70-100x faster (near 1:1 hw speed) |

**Trap approach** works well because real GC/Wii games spend well under 1% of instructions on non-native opcodes. Pure compute (ALU, loads/stores, branches) runs at full PPC970 speed with 0 overhead. The signal round-trip cost only hits when the guest actually executes one of the ~dozen trapped instruction types.

**AltiVec trampoline** (current prototype): ps\_add/sub/mul/mr are patched with `b trampoline_addr` instead of `trap`. The trampoline executes native AltiVec and branches back (no signal). ~50 cycles vs ~5000 for a SIGILL. Covers the most common ps\_\* instructions. psq\_l/st, ps\_div, ps\_abs/neg/nabs, ps\_sel, ps\_cmp, ps\_merge, ps\_sum, ps\_muls, ps\_res/rsqrte still use trap+interpreter.

**Full AltiVec translator** would eliminate ALL remaining traps by mapping Gekko Paired Singles directly to PPC970 AltiVec:
- `ps_add`/`ps_mul`/`ps_sub`/`ps_div` → `vaddfp`/`vmulfp`/`vsubfp`/`vdivfp`
- `ps_sel` → `vsel`
- `psq_l`/`psq_st` → AltiVec load/store with manual quantize (integer shift + pack/unpack)
- `ps_mr`/`ps_abs`/`ps_neg`/`ps_nabs` → `vand`/`vandc`/`vor` with sign-bit masks
- `ps_cmp` → `vcmpeqfp`/`vcmpgtfp` + CR field construction
- `ps_res`/`ps_rsqrte` → `vrefp`/`vrsqrtefp`
- `ps_merge00/01/10/11` → `vmrghw`/`vmrglw` permutations
- `ps_sum0/1` → `vaddfp` with permute
- `ps_muls0/1` → `vspltw` + `vmulfp`

The translator would need:
- **GPR mapping**: No translation needed (PPC32 guest on PPC64 host — 32-bit GPRs in low halves of 64-bit GPRs)
- **VR mapping 1:1**: Gekko has 32 Gekko VRs = 16 pairs of singles; map directly to PPC970's 32 AltiVec VRs (each VR holds 2 singles = 1 pair)
- **CR field emulation**: Paired-single FP compares update CR1 with FPCC; needs manual CR field merging
- **FPSCR emulation**: Gekko FPSCR differs from PPC970; VX/FPCC exceptions need software handling
- **psq_l/st quantize**: Read/write GQRs from emulated memory; shift/round/truncate; optimized with AltiVec bit ops

**Recommendation**: Start with the trap approach (working now), extend patching to all P0 instructions. Add AltiVec translator later for games that hit traps frequently. The trap approach already gives ~80% of potential performance.

### NCE Speed Estimate

| Component | CachedInterpreter | NCE (trap) | NCE + AltiVec |
|-----------|------------------|------------|---------------|
| ALU/Load/Store/Branch | ~30 cycles/instr | ~1 cycle | ~1 cycle |
| Paired Single (ps\_add/sub/mul/mr) | ~50 cycles | ~5000 cycles (SIGILL) | ~50 cycles (AltiVec trampoline) |
| Paired Single (other ps\_\*) | ~50 cycles | ~5000 cycles (SIGILL) | ~5000 cycles (SIGILL, unavoidable) |
| dcbz | ~30 cycles | ~5000 cycles (SIGILL) | ~2 cycles (4-instr: dcbz + 3 subi/stw) |
| mftb/TL/TU | ~20 cycles | ~5000 cycles (SIGILL) | ~1 cycle (native mftb + adjust) |
| MMIO | ~40 cycles | ~5000 cycles (SIGSEGV) | ~5000 cycles (SIGSEGV, unavoidable) |
| Supervisor SPR | ~40 cycles | ~5000 cycles (SIGILL) | ~5000 cycles (SIGILL, unavoidable) |

For a typical game with <100 PS instructions per frame: trap approach loses ~0.5ms to PS signal overhead per frame — negligible against 16.6ms frame budget. For PS-heavy scenes (SMG starbit effects, MKDD particle rendering), the loss could reach 2-3ms — still playable.

## Commit Strategy

Each functional area (Swap.h, Blob readers, Volume code, Boot code) should be committed separately for clarity. Use descriptive messages prefixed with `[PPC64-BE]`.
