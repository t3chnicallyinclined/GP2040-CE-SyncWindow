---
name: VMU Expert Reference
description: Comprehensive Dreamcast VMU (Visual Memory Unit) technical reference — filesystem layout, FAT structure, system block format, file info, information fork, Maple FT1 storage protocol, byte order handling, flash access, media info encoding, and implementation lessons learned
type: reference
---

# VMU (Visual Memory Unit) — Expert Reference

Source: Sega VMU.pdf (778-page official specification), MaplePad reference implementation, real VMU binary dump analysis, and implementation experience.

---

## 1. Physical Memory Layout

VMU flash = 128KB total = 64KB × 2 banks.
Managed as **256 blocks × 512 bytes** (from the Maple Bus perspective).

```
Block  Purpose                    Real Address (Bank)
─────  ─────────────────────────  ──────────────────
0x00   Executable file start      Bank 0, 0x0000
 ...   (or data files)
0x7F   Executable file end        Bank 0, 0xFFFF
0x80   Data area continues        Bank 1, 0x0000
 ...
0xC7   Data area end (block 199)  Bank 1, 0x63FF
0xC8   Reserved area start (200)  Bank 1, 0x6400
 ...
0xF0   Reserved area end (240)    Bank 1, 0x7FFF (approx)
0xF1   File info end (block 241)  Bank 1, 0x7880 (approx)
 ...
0xFD   File info start (block 253) Bank 1, ...
0xFE   FAT block (254)            Bank 1, ...
0xFF   System block (255)         Bank 1, top of memory
```

**Key constants (from spec + our implementation):**
- `VMU_NUM_BLOCKS = 256`
- `VMU_BYTES_PER_BLOCK = 512`
- `VMU_MEMORY_SIZE = 131072` (128KB)
- `VMU_SYSTEM_BLOCK_NO = 255`
- `VMU_FAT_BLOCK_NO = 254`
- `VMU_FILE_INFO_BLOCK_NO = 253` (start of chain, descending)
- `VMU_NUM_FILE_INFO = 13` (blocks 253 down to 241)
- `VMU_NUM_SAVE_BLOCKS = 200` (blocks 0–199)
- `VMU_SAVE_AREA_BLOCK_NO = 31` (used in media info — represents the start block number from the DC's perspective for the "user-accessible" save region)
- Reserved blocks: 200–240 (41 blocks, used by system/BIOS)

---

## 2. System Block (Block 255)

The system block is write-protected except during formatting. Layout:

| Byte Offset | Size | Content |
|-------------|------|---------|
| 0x00–0x0F | 16 | Signature: all `0x55` (marks formatted VMU) |
| 0x10 | 1 | Custom use byte (real VMU: 0x01) |
| 0x11 | 1 | **Format version** (our addition: `VMU_FORMAT_VERSION`) |
| 0x12–0x2F | 30 | Unused / reserved |
| 0x30–0x37 | 8 | Date/time in BCD: century(1), year(1), month(1), day(1), hour(1), min(1), sec(1), weekday(1) |
| 0x38–0x3F | 8 | Reserved |
| 0x40–0x57 | 24 | **Media info** (6 × uint32_t at word offset 16) |
| 0x58–0x1FF | 424 | Unused |

### Media Info (6 words at system block word offset 16)

Stored in **native little-endian 16-bit** format on flash. Each uint32_t packs two LE uint16_t values:

| Word | Lower 16 bits | Upper 16 bits |
|------|---------------|---------------|
| 0 | totalBlocks-1 (0xFF=255) | partition (0) |
| 1 | systemBlockNo (0xFF=255) | fatBlockNo (0xFE=254) |
| 2 | numFatBlocks (1) | fileInfoBlockNo (0xFD=253) |
| 3 | numFileInfoBlocks (13) | reserved (0) |
| 4 | numSaveBlocks (200=0xC8) | saveAreaBlockNo (31=0x1F) |
| 5 | executionFile / icon shape (0x0080) | — |

**Real VMU dump confirmed** this layout at system block offset 0x40.

### Media Info Over Maple Bus (GET_MEMORY_INFO response)

When sent over the wire, media info uses **big-endian 16-bit** encoding per the Maple FT1 spec. Each uint32_t packs two BE uint16_t values (first field in upper 16 bits, second in lower 16 bits):

```c
#define BSWAP16(v) ((uint16_t)(((v) << 8) | (((v) >> 8) & 0xFF)))
#define MEDIA_HI(v) ((uint32_t)BSWAP16(v) << 16)
#define MEDIA_LO(v) ((uint32_t)BSWAP16(v))

out[0] = MEDIA_HI(totalBlocks-1) | MEDIA_LO(partition);
out[1] = MEDIA_HI(systemBlockNo) | MEDIA_LO(fatBlockNo);
// ... etc
```

This matches MaplePad's `U16_TO_UPPER_HALF_WORD` / `U16_TO_LOWER_HALF_WORD` macros.

---

## 3. FAT Block (Block 254)

256 entries × 16 bits = 512 bytes = exactly one block.

Each entry maps to a block number (entry[0] = block 0, entry[255] = block 255).

| Value | Meaning |
|-------|---------|
| 0xFFFC | Free block |
| 0xFFFA | System-reserved (FAT, system block, end-of-file-info-chain) |
| 0x0000–0x00FF | Chain pointer to next block |

**File info chain:** FAT[253]=252, FAT[252]=251, ..., FAT[242]=241, FAT[241]=0xFFFA (end).

**Data files:** Stored from block 199 (0xC7) downward toward 0. File's first block is in the file info entry; FAT chains to subsequent blocks.

**Executable files:** Start at block 0, grow upward. Must be contiguous.

**Real VMU dump analysis:** 236 free blocks (0xFFFC), 16 used (chained), 4 system-reserved (0xFFFA for blocks 241, 254, 255 + end markers).

---

## 4. File Info Area (Blocks 241–253)

13 blocks × 512 bytes = 6656 bytes. Each file entry = 32 bytes → max 200 files (matches save area block count).

### File Info Entry (32 bytes)

| Offset | Size | Field |
|--------|------|-------|
| 0x00 | 1 | File type: 0x00=none, 0x33=data, 0xCC=game (executable) |
| 0x01 | 1 | Copy protect: 0x00=ok, 0xFF=protected |
| 0x02 | 2 | First block number (LE uint16_t) |
| 0x04 | 12 | Filename (ASCII, uppercase, space-padded, max 12 chars) |
| 0x10 | 8 | Creation date/time (BCD: century, year, month, day, hour, min, sec, weekday) |
| 0x18 | 2 | File size in blocks (LE uint16_t) |
| 0x1A | 2 | Header offset in blocks (for executable files) |
| 0x1C | 4 | Reserved / unused |

**Characters allowed in filenames:** Uppercase A-Z, 0-9, underscore `_`. No lowercase, no hyphen `-`.

File info blocks are chained via FAT: 253→252→...→241→end (0xFFFA).

Empty entries have file type = 0x00.

---

## 5. Information Fork (File Header)

Files (other than ICONDATA_VMS) contain an **information fork** — metadata for the Dreamcast file manager UI.

### Information Fork Structure (from byte offset 0x000 of the file's data)

| Offset | Size | Field |
|--------|------|-------|
| 0x00 | 16 | VM comment (displayed on VMU LCD file browser) |
| 0x10 | 32 | GUI comment (displayed on DC file management screen, Shift-JIS supported) |
| 0x30 | 16 | Game name / sort key (unique per game for icon ordering) |
| 0x40 | 2 | Number of icons (1–3, LE uint16_t) |
| 0x42 | 2 | Animation speed (LE uint16_t, n/30 seconds per frame) |
| 0x44 | 2 | Visual type (0=none, 1=direct color, 2=256-color, 3=16-color) |
| 0x46 | 2 | CRC of data portion (LE uint16_t, auto-calculated by buMakeBackupFileImage) |
| 0x48 | 4 | Save data size in bytes (LE uint32_t, excludes info fork) |
| 0x4C | 20 | Reserved (all 0x00) |
| 0x60 | 32 | Icon palette (16 colors × 2 bytes, ARGB4444) |
| 0x80 | 512 | Icon #1 pattern data (32×32, 4bpp, 16 colors) |
| 0x280 | 512 | Icon #2 pattern data (if numIcons ≥ 2) |
| 0x480 | 512 | Icon #3 pattern data (if numIcons ≥ 3) |
| varies | varies | Visual comment data (72×56 graphic, format per visual type) |

**Icon pattern format:** 32×32 pixels, 4 bits per pixel (palette index). Upper nibble = right pixel, lower nibble = left pixel. 16 bytes per row × 32 rows = 512 bytes per icon.

**Visual comment types:**
- Type 0: None (0 bytes)
- Type 1: Direct color ARGB4444 (72×56×2 = 8064 bytes, ~16 blocks)
- Type 2: 256-color palette (512 byte palette + 4032 byte pattern = 4544 bytes, ~9 blocks)
- Type 3: 16-color palette (32 byte palette + 2016 byte pattern = 2048 bytes, ~4 blocks)

---

## 6. Volume Icon (ICONDATA_VMS)

Special file that sets the VMU's icon on the DC memory selection screen.

| Offset | Size | Field |
|--------|------|-------|
| 0x00 | 16 | VM comment |
| 0x10 | 4 | Monochrome icon data offset (usually 0x00000020, LE) |
| 0x14 | 4 | Color icon data offset (usually 0x000000A0, LE) |
| 0x18 | 8 | Reserved (all 0x00) |
| 0x20 | 128 | Monochrome icon pattern (32×32, 1bpp, MSB=left) |
| 0xA0 | 32 | Color icon palette (16 colors × 2 bytes, ARGB4444) |
| 0xC0 | 512 | Color icon pattern (32×32, 4bpp) |

No information fork — this file uses its own format.

---

## 7. Maple Bus Storage Protocol (FT1)

VMU declares function code `MAPLE_FUNC_MEMORY_CARD = 0x00000002`.

### Device Info (funcData[0] encoding)

```
Bits  Field              Our Value
────  ─────              ─────────
7     Removable          0
6     CRC needed         0
11-8  Reads per block    1
15-12 Writes per block   4
23-16 (bytes/32 - 1)     15 (= 512/32 - 1)
31-24 (partitions - 1)   0
Total: 0x000F4100
```

### Storage Commands

| Command | Value | Direction | Payload |
|---------|-------|-----------|---------|
| GET_MEDIA_INFO | 0x0A | DC→VMU | funcCode (1 word) |
| BLOCK_READ | 0x0B | DC→VMU | funcCode + location (2 words) |
| BLOCK_WRITE | 0x0C | DC→VMU | funcCode + location + data (2 + 32 words) |
| BLOCK_COMPLETE_WRITE | 0x0D | DC→VMU | funcCode + location (2 words) |

### Location Word Encoding

```
uint32_t location:
  bits 15-0:  block number (0–255)
  bits 23-16: phase (0 for reads; 0–3 for writes)
  bits 31-24: partition (always 0)
```

### Write Protocol (4 phases + commit)

A block write requires 5 Maple commands:
1. `BLOCK_WRITE` phase=0: 128 bytes (block offset 0x000–0x07F) → ACK
2. `BLOCK_WRITE` phase=1: 128 bytes (block offset 0x080–0x0FF) → ACK
3. `BLOCK_WRITE` phase=2: 128 bytes (block offset 0x100–0x17F) → ACK
4. `BLOCK_WRITE` phase=3: 128 bytes (block offset 0x180–0x1FF) → ACK
5. `BLOCK_COMPLETE_WRITE` phase=4: no data → ACK (triggers flash commit)

Each write phase sends 32 payload words (128 bytes). The device accumulates all 4 phases in a write buffer, then commits to flash on BLOCK_COMPLETE_WRITE.

**MaplePad note:** MaplePad calls BLOCK_COMPLETE_WRITE `COMMAND_GET_LAST_ERROR` (same command value 0x0D). MaplePad does NOT validate that all 4 phases were received (no writePhaseMask check). Our implementation does validate.

### Response Packets

| Response | Command Value | When |
|----------|---------------|------|
| RESPOND_DEVICE_STATUS | 0x05 | DEVICE_REQUEST (cmd 1) |
| RESPOND_DATA_XFER | 0x08 | GET_MEDIA_INFO, BLOCK_READ |
| RESPOND_ACK | 0x07 | BLOCK_WRITE, BLOCK_COMPLETE_WRITE, RESET, SHUTDOWN |
| RESPOND_FILE_ERROR | 0xFC | Bad funcCode, out-of-range block, etc. |

---

## 8. Byte Order — The Critical Detail

### The Double-DMA Bswap Identity

```
Our TX DMA bswap  +  DC RX DMA bswap  =  identity (cancels out)
```

Data stored as raw uint32_t in flash → sent via DMA (which bswaps) → DC receives via DMA (which bswaps back) → DC sees original uint32_t values.

### What This Means for Implementation

**BLOCK_READ:** `memcpy()` flash data into response packet. NO software bswap.

**BLOCK_WRITE:** `memcpy()` received payload into write buffer. NO software bswap.

**Round trip:** DC sends value V → our RX stores V in writeBuffer → flash stores V → BLOCK_READ memcpy's V into response → DMA+DMA cancels → DC reads V. Perfect.

### The Bug That Was Fixed (2026-03-23)

Previous code added `__builtin_bswap32()` per-word in `sendBlockReadResponse()`. This created a **triple swap** (software bswap + our TX DMA bswap + DC RX DMA bswap = one net swap = corrupted data). The DC saw byte-reversed FAT entries (e.g., `0xFCFF` instead of `0xFFFC` for free blocks), causing "error writing" in MvC2.

### MaplePad's Approach (Different but Equivalent)

MaplePad uses `flipWordBytes()` (bswap) on BOTH write and read paths, maintaining NETWORK byte order in flash:
- Write: `flipWordBytes(received_data)` → store
- Read: load → `flipWordBytes(stored_data)` → send

Two flips cancel = same result as our zero-flip approach. Both are correct.

### Protocol Field Byte Order — CONFIRMED BY HARDWARE TESTING (2026-03-24)

**CRITICAL: funcCode and location word have DIFFERENT byte orders in our decoded payload.**

Hardware test results (diagnostic `P` and `L` fields on OLED):

1. **funcCode (payload[0]):** Arrives in **HOST (LE) order**.
   - `P00000002` = `MAPLE_FUNC_MEMORY_CARD` (value 2). Direct comparison works, NO bswap needed.
   - This is confirmed: `payload[0] != MAPLE_FUNC_MEMORY_CARD` passes correctly.

2. **Location word (payload[1]):** Arrives with **byte-swapped uint16 halves**.
   - Block 255 → `L000000FF` → `locWord & 0xFFFF = 0x00FF = 255` ✓ (no bswap needed for this case)
   - Block 251 → `L0000FB00` → `locWord & 0xFFFF = 0xFB00 = 64256` ✗ (WRONG without bswap)
   - `__builtin_bswap16(0xFB00) = 0x00FB = 251` ✓ (correct with bswap)
   - **BUT** `__builtin_bswap16(0x00FF) = 0xFF00 = 65280` ✗ (WRONG with bswap)
   - **This is contradictory** — bswap16 fixes block 251 but breaks block 255.
   - Full raw locWord hex needed to understand the pattern. Investigation ongoing.

3. **Block data (BLOCK_WRITE payload[2..]):** Use `memcpy()` directly, no bswap. Round-trips correctly via double-DMA-bswap identity.

**What makes the DC recognize the card vs not:**
- ✅ Card IS recognized when: funcCode comparison passes AND block numbers extract correctly for system blocks (255, 254)
- ❌ Card NOT recognized when: `__builtin_bswap16` is applied to block numbers (breaks block 255: `bswap16(0x00FF) = 0xFF00 = 65280 > 255` → FILE_ERROR)
- ❌ Card NOT recognized when: `buildMediaInfo()` uses plain `(hi << 16) | lo` instead of BSWAP16 macro encoding for GET_MEMORY_INFO response

**Current status (2026-03-24):** Card is recognized, DC sees VMU, but reports "not enough empty blocks". Block extraction without bswap16 works for block 255 but produced `0xFB00` for block 251 in earlier test — raw locWord hex dump needed to resolve.

---

## 9. Flash Memory Access (VMU Internal)

From the spec, the real VMU's flash access works differently from our RP2040 emulation:

### Real VMU Flash

- **Page size:** 128 bytes (NOT 512 — the 512-byte "block" is a Maple Bus abstraction over 4 pages)
- **Bank switching:** Bank 0 (program, blocks 0x00–0x7F), Bank 1 (data, blocks 0x80–0xFF)
- **Clock requirement:** Must switch to RC oscillator (600 kHz) before flash write. Quartz (32 kHz) for normal operation.
- **Interrupts:** Must disable all interrupts during flash write
- **BIOS routines:**
  - `fm_prd_ex` (ORG 0x0120): Read 128 bytes from flash page
  - `fm_wrt_ex` (ORG 0x0100): Write 128 bytes to flash page. Returns ACC=0x00 (ok) or 0xFF (fail)
  - `fm_vrf_ex` (ORG 0x0110): Verify written data. Must be called immediately after `fm_wrt_ex` with same arguments
- **Address calculation:** `start_address = 0x80 × page_number` (pages 0–511 per bank)
- **Write completion detection:** Toggle-bit or data-polling methods (selectable via RAM bank-1 0x07C bit 0)

### Our RP2040 Emulation

- **Storage:** RP2040 onboard flash at offset `VMU_FLASH_OFFSET = 0x001D8000` (128KB before config area)
- **Read:** XIP memory-mapped (`XIP_BASE + VMU_FLASH_OFFSET + blockNum * 512`)
- **Write:** Read-modify-write of 4KB flash sectors:
  1. `memcpy()` 4KB sector from XIP to RAM buffer
  2. Overlay 512-byte block in RAM buffer
  3. `flash_range_erase()` + `flash_range_program()` with interrupts disabled
- **Timing:** ~45ms for sector erase+program (acceptable — real VMU has similar latency)
- **Write is done BEFORE sending ACK** — the DC blocks waiting for our response, so no commands are missed during the flash write

---

## 10. System Block Self-Correction

The DC sometimes writes `0` for the save area fields in the system block during formatting or saves. MaplePad includes protection for this, and so does our implementation:

```c
if (blockNum == VMU_SYSTEM_BLOCK_NO) {
    uint32_t saveWord = buf32[VMU_MEDIA_INFO_OFFSET + 4];
    if (saveWord == 0) {
        buf32[VMU_MEDIA_INFO_OFFSET + 4] =
            (uint32_t)VMU_NUM_SAVE_BLOCKS | ((uint32_t)VMU_SAVE_AREA_BLOCK_NO << 16);
    }
}
```

Without this, a zeroed-out save area info would prevent future saves (DC thinks no save blocks exist).

---

## 11. Format Version Tracking

Our addition (not in real VMU): `VMU_FORMAT_VERSION` byte at system block offset 17 (after the 0x55 signature + 1 unused byte).

- On boot, `init()` checks signature (16 × 0x55) and version byte
- Signature missing/corrupted → full format
- Version mismatch → full format (rebuilds FAT with correct values)
- Version match → no action needed

This prevents stale/corrupted filesystem data from persisting across firmware updates that change layout constants.

---

## 12. Key Differences: Our Implementation vs Real VMU

| Aspect | Real VMU | Our Emulation |
|--------|----------|---------------|
| Flash page size | 128 bytes | 512 bytes (block-level, 4KB sector erase) |
| Write granularity | 128-byte page | 4KB sector read-modify-write |
| Clock switching | RC oscillator required | N/A (RP2040 runs at 125 MHz) |
| Bank switching | Hardware (Bank 0/1) | Linear address space |
| Verify after write | `fm_vrf_ex` required | Not implemented (flash is reliable) |
| LCD sub-peripheral | Supported (FT2) | Not implemented |
| Timer sub-peripheral | Supported (FT3) | Not implemented |
| Standalone mode | Full OS with games | N/A (we're a controller) |
| File info validation | Full 32-byte entries | Zeroed blocks (DC/game writes the entries) |

---

## 13. External References

- **VMU.pdf** (778 pages): `VMU/VMU.pdf` in repo — official Sega VMU specification covering hardware, BIOS, file formats, and development tools
- **MaplePad reference:** `maplepad_ref/src/clientLib/DreamcastStorage.cpp` — known-good VMU emulation with matching constants
- **Real VMU binary dump:** `VMU/vmu_save_A1.bin` — 128KB dump confirming filesystem layout
- **Maple Bus wire protocol:** http://mc.pp.se/dc/maplewire.html
- **Maple Bus commands:** http://mc.pp.se/dc/maplebus.html
- **Web Archive VMU FAQ:** https://web.archive.org/web/20110714050713/http://rvmu.maushammer.com/faq.html
- **Deco Franken VMU files:** https://www.deco.franken.de/myfiles/myfiles.html
- **Marcus Comstedt VMU flashrom spec:** https://mc.pp.se/dc/vms/flashmem.html — Root block layout, FAT format, directory format, DCI/DCM byte-swap details

---

## 14. Lessons Learned

1. **Double-DMA bswap = identity.** Never add software bswap to block data — it creates a triple-swap that corrupts the round trip. Use `memcpy()` for both reads and writes.

2. **Media info has TWO encodings.** Flash storage uses native LE 16-bit pairs. Wire protocol uses BE 16-bit pairs (BSWAP16 + shift). Mixing them up = DC sees wrong filesystem geometry.

3. **System block self-correction is essential.** The DC can zero out save area fields during writes. Without protection, the VMU becomes permanently unable to save.

4. **Format version tracking prevents stale data.** Firmware updates that change FAT layout need to force re-format. A version byte is the simplest mechanism.

5. **Flash write timing is acceptable.** ~45ms for RP2040 sector erase+program is within the DC's tolerance. Real VMUs have similar latency. Writing BEFORE sending ACK ensures no commands are missed.

6. **BLOCK_COMPLETE_WRITE = GET_LAST_ERROR.** Same command value (0x0D), different names in different docs. MaplePad uses the "GET_LAST_ERROR" name.

7. **The 512-byte "block" is a Maple Bus abstraction.** The real VMU flash has 128-byte pages. Four pages = one block. The 4-phase write protocol (128 bytes per phase) directly corresponds to writing one page at a time.

8. **File info entries are 32 bytes.** 13 blocks × 512 bytes / 32 bytes = max 208 entries, but the spec limits to 200 (matching the 200 save area blocks).

9. **GET_MEMORY_INFO encoding is critical for card recognition.** The DC will NOT recognize the VMU at all if `buildMediaInfo()` uses plain `(hi << 16) | lo` encoding. It MUST use the BSWAP16 macro encoding: `MEDIA_HI(v) | MEDIA_LO(v)`. Confirmed by test: changing to plain encoding = DC only does 2 VMU exchanges and never recognizes card.

10. **`__builtin_bswap16` on block numbers BREAKS card recognition.** Block 255 (system block) arrives as `locWord & 0xFFFF = 0x00FF`. Applying `bswap16(0x00FF)` = `0xFF00` = 65280, which fails the `>= 256` range check → FILE_ERROR → DC can't read system block → card not recognized. Confirmed by test (2026-03-24).

11. **Only change ONE thing at a time when debugging byte order.** funcCode and location word have different byte order behaviors in our software decoder. Assumptions from one field don't transfer to the other. Always verify with hardware test diagnostics before and after each change.

12. **Flash safety confirmed.** VMU flash region (0x001D8000, 128KB) has no overlap with GP2040-CE config storage (0x001F8000, 32KB) or firmware code. Only two flash writers exist in the firmware: FlashPROM (config) and DreamcastVMU. They are completely independent.
