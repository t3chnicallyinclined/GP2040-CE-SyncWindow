#include "drivers/dreamcast/DreamcastVMU.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/time.h"

// XIP base address for reading VMU data from flash
#define VMU_XIP_BASE  (XIP_BASE + VMU_FLASH_OFFSET)

DreamcastVMU::DreamcastVMU()
    : flashWritePending(false), flashWriteBlockNum(0),
      currentWriteBlock(0xFFFF), writePhaseMask(0) {
    memset(writeBuffer, 0, sizeof(writeBuffer));
    memset(sectorBuffer, 0, sizeof(sectorBuffer));
}

void DreamcastVMU::init() {
    buildVMUInfoPacket();
    buildVMUAckPacket();

    if (needsFormat()) {
        format();
    }
}

bool DreamcastVMU::needsFormat() {
    // Check system block for the 0x55 signature (first 16 bytes must all be 0x55).
    // If it's blank (0xFF) or corrupted (anything else), we need to format.
    const uint8_t* systemBlock = (const uint8_t*)(VMU_XIP_BASE + VMU_SYSTEM_BLOCK_NO * VMU_BYTES_PER_BLOCK);
    for (int i = 0; i < 16; i++) {
        if (systemBlock[i] != 0x55) return true;
    }
    // Check format version — forces re-format when filesystem layout constants change
    // (e.g., save_area_start, block count, etc.) or after a bug fix that changes on-flash data.
    if (systemBlock[17] != VMU_FORMAT_VERSION) return true;
    return false;
}

void DreamcastVMU::buildVMUInfoPacket() {
    memset(&vmuInfoPacket, 0, sizeof(vmuInfoPacket));

    vmuInfoPacket.bitPairsMinus1 = MapleBus::calcBitPairs(sizeof(vmuInfoPacket));
    vmuInfoPacket.header.command     = MAPLE_CMD_RESPOND_DEVICE_STATUS;
    vmuInfoPacket.header.destination = MAPLE_DC_ADDR;
    vmuInfoPacket.header.origin      = MAPLE_SUB0_ADDR;
    vmuInfoPacket.header.numWords    = sizeof(MapleDeviceInfo) / sizeof(uint32_t);

    // VMU declares storage function only (no LCD or timer — not needed for saves)
    vmuInfoPacket.info.func        = MAPLE_FUNC_MEMORY_CARD;

    // Storage function definition (matches MaplePad):
    //   bits 7:   removable = 0
    //   bits 6:   CRC needed = 0
    //   bits 11-8:  reads per block = 1
    //   bits 15-12: writes per block = 4
    //   bits 23-16: (bytes_per_block/32 - 1) = 15
    //   bits 31-24: (partitions - 1) = 0
    // Total: 0x000F4100
    vmuInfoPacket.info.funcData[0] = 0x000F4100;
    vmuInfoPacket.info.funcData[1] = 0;
    vmuInfoPacket.info.funcData[2] = 0;
    vmuInfoPacket.info.areaCode    = -1;  // 0xFF = all regions
    vmuInfoPacket.info.connectorDirection = 0;

    strncpy(vmuInfoPacket.info.productName,
            "Visual Memory                 ",
            sizeof(vmuInfoPacket.info.productName));
    strncpy(vmuInfoPacket.info.productLicense,
            "Produced By or Under License From SEGA ENTERPRISES,LTD.    ",
            sizeof(vmuInfoPacket.info.productLicense));

    vmuInfoPacket.info.standbyPower = 124;  // 12.4mA * 10
    vmuInfoPacket.info.maxPower     = 130;  // 13.0mA * 10
}

void DreamcastVMU::buildVMUAckPacket() {
    memset(&vmuAckPacket, 0, sizeof(vmuAckPacket));

    vmuAckPacket.bitPairsMinus1 = MapleBus::calcBitPairs(sizeof(vmuAckPacket));
    vmuAckPacket.header.command     = MAPLE_CMD_RESPOND_ACK;
    vmuAckPacket.header.destination = MAPLE_DC_ADDR;
    vmuAckPacket.header.origin      = MAPLE_SUB0_ADDR;
    vmuAckPacket.header.numWords    = 0;
}

const uint8_t* DreamcastVMU::readBlock(uint16_t blockNum) {
    if (blockNum >= VMU_NUM_BLOCKS) return nullptr;
    return (const uint8_t*)(VMU_XIP_BASE + blockNum * VMU_BYTES_PER_BLOCK);
}

void DreamcastVMU::buildMediaInfo(uint32_t* out) {
    // 6 words of media info for GET_MEMORY_INFO response.
    // Each word packs two 16-bit big-endian values (Maple Bus protocol format).
    // This matches DreamPicoPort's U16_TO_UPPER/LOWER_HALF_WORD encoding.
    // After sendPacket bswap32, the DC receives the values correctly.
    #define BSWAP16(v) ((uint16_t)(((v) << 8) | (((v) >> 8) & 0xFF)))
    #define MEDIA_HI(v) ((uint32_t)BSWAP16(v) << 16)
    #define MEDIA_LO(v) ((uint32_t)BSWAP16(v))

    out[0] = MEDIA_HI(VMU_NUM_BLOCKS - 1) | MEDIA_LO(0);
    out[1] = MEDIA_HI(VMU_SYSTEM_BLOCK_NO) | MEDIA_LO(VMU_FAT_BLOCK_NO);
    out[2] = MEDIA_HI(VMU_NUM_FAT_BLOCKS)  | MEDIA_LO(VMU_FILE_INFO_BLOCK_NO);
    out[3] = MEDIA_HI(VMU_NUM_FILE_INFO)   | MEDIA_LO(0);
    out[4] = MEDIA_HI(VMU_NUM_SAVE_BLOCKS) | MEDIA_LO(VMU_SAVE_AREA_BLOCK_NO);
    out[5] = 0x00008000;  // execution file (icon shape)

    #undef BSWAP16
    #undef MEDIA_HI
    #undef MEDIA_LO
}

// Build media info in native LE 16-bit format for the root/system block.
// This is the "flipped" version of buildMediaInfo — what a real VMU stores on flash.
static void buildMediaInfoNative(uint32_t* out) {
    out[0] = (uint32_t)(VMU_NUM_BLOCKS - 1)  | (0 << 16);
    out[1] = (uint32_t)VMU_SYSTEM_BLOCK_NO    | ((uint32_t)VMU_FAT_BLOCK_NO << 16);
    out[2] = (uint32_t)VMU_NUM_FAT_BLOCKS     | ((uint32_t)VMU_FILE_INFO_BLOCK_NO << 16);
    out[3] = (uint32_t)VMU_NUM_FILE_INFO      | (0 << 16);
    out[4] = (uint32_t)VMU_NUM_SAVE_BLOCKS    | ((uint32_t)VMU_SAVE_AREA_BLOCK_NO << 16);
    out[5] = 0x00800000;  // execution file (byte-swapped for flash storage)
}

void DreamcastVMU::format() {
    // Format creates the minimum VMU filesystem: system block, FAT, and empty file info.
    // All writes go through flash sector erase+program.

    // We need to write to the last few blocks (system=255, FAT=254, file info=241-253).
    // These are in the last portion of the 128KB VMU flash area.

    // Build a temporary block buffer for formatting
    uint8_t blockBuf[VMU_BYTES_PER_BLOCK];
    uint32_t* block32 = (uint32_t*)blockBuf;

    //
    // System Block (block 255)
    //
    memset(blockBuf, 0, VMU_BYTES_PER_BLOCK);
    // Signature: first 16 bytes all 0x55 (marks formatted VMU)
    memset(blockBuf, 0x55, 16);
    // Format version at byte 17 — triggers re-format when constants change
    blockBuf[17] = VMU_FORMAT_VERSION;
    // Date/time markers at byte offset 0x030 (word 12)
    // BCD format: century, year, month, day, hour, min, sec, weekday
    // Stored in native byte order (first byte at lowest address)
    block32[12] = __builtin_bswap32(0x19990909);  // 1999-09-09
    block32[13] = __builtin_bswap32(0x00001000);  // 00:00:10.00
    // Media info at word offset 16 — native LE 16-bit format (like a real VMU root block)
    buildMediaInfoNative(&block32[VMU_MEDIA_INFO_OFFSET]);

    // Write system block to flash
    flashWriteBlockNum = VMU_SYSTEM_BLOCK_NO;
    memcpy(writeBuffer, blockBuf, VMU_BYTES_PER_BLOCK);
    doFlashWrite();

    //
    // FAT Block (block 254)
    //
    // FAT has 256 entries (one per block), each 16 bits.
    // 256 * 2 = 512 bytes = one block.
    // Unallocated blocks: 0xFFFC
    // System/FAT blocks: 0xFFFA (system-reserved)
    // File info chain: linked list
    //
    // Initialize all entries to 0xFFFC (free)
    uint16_t* fat16 = (uint16_t*)blockBuf;
    for (int i = 0; i < 256; i++) {
        fat16[i] = 0xFFFC;
    }

    // Mark system block (255) as system-reserved
    fat16[VMU_SYSTEM_BLOCK_NO] = 0xFFFA;

    // Mark FAT block (254) as system-reserved
    fat16[VMU_FAT_BLOCK_NO] = 0xFFFA;

    // Mark file info blocks (241-253) as a chain
    // File info is a linked list: 253 → 252 → 251 → ... → 241 → 0xFFFA (end)
    for (int i = VMU_FILE_INFO_BLOCK_NO; i > VMU_FILE_INFO_BLOCK_NO - VMU_NUM_FILE_INFO + 1; i--) {
        fat16[i] = i - 1;
    }
    fat16[VMU_FILE_INFO_BLOCK_NO - VMU_NUM_FILE_INFO + 1] = 0xFFFA;  // End of chain

    // Write FAT block to flash
    flashWriteBlockNum = VMU_FAT_BLOCK_NO;
    memcpy(writeBuffer, blockBuf, VMU_BYTES_PER_BLOCK);
    doFlashWrite();

    //
    // File Info Blocks (blocks 241-253) — all zeros (no files)
    //
    memset(blockBuf, 0, VMU_BYTES_PER_BLOCK);
    for (int i = VMU_FILE_INFO_BLOCK_NO; i >= VMU_FILE_INFO_BLOCK_NO - VMU_NUM_FILE_INFO + 1; i--) {
        flashWriteBlockNum = i;
        memcpy(writeBuffer, blockBuf, VMU_BYTES_PER_BLOCK);
        doFlashWrite();
    }

    flashWritePending = false;
}

void DreamcastVMU::scheduleBlockWrite(uint16_t blockNum) {
    flashWriteBlockNum = blockNum;
    flashWritePending = true;
}

void DreamcastVMU::doFlashWrite() {
    // Read-modify-write: read the full 4KB sector, modify the target block, write back.
    // VMU blocks are 512 bytes, flash sectors are 4KB (8 blocks per sector).
    uint32_t blockFlashOffset = VMU_FLASH_OFFSET + flashWriteBlockNum * VMU_BYTES_PER_BLOCK;
    uint32_t sectorStart = blockFlashOffset & ~(FLASH_SECTOR_SIZE - 1);  // Align to sector
    uint32_t offsetInSector = blockFlashOffset - sectorStart;

    // Read current sector from XIP
    memcpy(sectorBuffer, (const uint8_t*)(XIP_BASE + sectorStart), FLASH_SECTOR_SIZE);

    // Merge the new block data
    memcpy(sectorBuffer + offsetInSector, writeBuffer, VMU_BYTES_PER_BLOCK);

    // Erase and reprogram the sector with interrupts disabled
    uint32_t interrupts = save_and_disable_interrupts();
    flash_range_erase(sectorStart, FLASH_SECTOR_SIZE);
    flash_range_program(sectorStart, sectorBuffer, FLASH_SECTOR_SIZE);
    restore_interrupts(interrupts);

    flashWritePending = false;
}

void DreamcastVMU::processFlashWrite() {
    if (flashWritePending) {
        doFlashWrite();
    }
}

// ============================================================
// Response packet builders
// ============================================================

void DreamcastVMU::sendInfoResponse(uint8_t port, MapleBus& bus) {
    vmuInfoPacket.header.origin = (MAPLE_SUB0_ADDR & MAPLE_PERIPH_MASK) | port;
    vmuInfoPacket.header.destination = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | port;
    vmuInfoPacket.crc = MapleBus::calcCRC(
        (uint32_t*)&vmuInfoPacket.header,
        sizeof(vmuInfoPacket) / sizeof(uint32_t) - 2);
    bus.sendPacket((uint32_t*)&vmuInfoPacket,
                   sizeof(vmuInfoPacket) / sizeof(uint32_t));
}

void DreamcastVMU::sendAckResponse(uint8_t port, MapleBus& bus) {
    vmuAckPacket.header.origin = (MAPLE_SUB0_ADDR & MAPLE_PERIPH_MASK) | port;
    vmuAckPacket.header.destination = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | port;
    vmuAckPacket.crc = MapleBus::calcCRC(
        (uint32_t*)&vmuAckPacket.header,
        sizeof(vmuAckPacket) / sizeof(uint32_t) - 2);
    bus.sendPacket((uint32_t*)&vmuAckPacket,
                   sizeof(vmuAckPacket) / sizeof(uint32_t));
}

void DreamcastVMU::sendMemoryInfoResponse(uint8_t port, MapleBus& bus) {
    static MapleMemoryInfoPacket pkt;
    memset(&pkt, 0, sizeof(pkt));

    pkt.bitPairsMinus1 = MapleBus::calcBitPairs(sizeof(pkt));
    pkt.header.command     = MAPLE_CMD_RESPOND_DATA_XFER;
    pkt.header.destination = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | port;
    pkt.header.origin      = (MAPLE_SUB0_ADDR & MAPLE_PERIPH_MASK) | port;
    pkt.header.numWords    = sizeof(MapleMemoryInfo) / sizeof(uint32_t);

    pkt.memInfo.funcCode = MAPLE_FUNC_MEMORY_CARD;
    buildMediaInfo(pkt.memInfo.mediaInfo);

    pkt.crc = MapleBus::calcCRC(
        (uint32_t*)&pkt.header,
        sizeof(pkt) / sizeof(uint32_t) - 2);
    bus.sendPacket((uint32_t*)&pkt, sizeof(pkt) / sizeof(uint32_t));
}

void DreamcastVMU::sendBlockReadResponse(uint16_t blockNum, uint8_t port, MapleBus& bus) {
    static MapleBlockReadPacket pkt;

    pkt.bitPairsMinus1 = MapleBus::calcBitPairs(sizeof(pkt));
    pkt.header.command     = MAPLE_CMD_RESPOND_DATA_XFER;
    pkt.header.destination = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | port;
    pkt.header.origin      = (MAPLE_SUB0_ADDR & MAPLE_PERIPH_MASK) | port;
    pkt.header.numWords    = sizeof(MapleBlockReadData) / sizeof(uint32_t);

    pkt.blockRead.funcCode = MAPLE_FUNC_MEMORY_CARD;
    // Location word: partition=0, phase=0, blockNum in lower 16 bits
    pkt.blockRead.location = (uint32_t)blockNum;

    // Copy block data from XIP flash
    const uint8_t* blockData = readBlock(blockNum);
    if (blockData) {
        memcpy(pkt.blockRead.data, blockData, VMU_BYTES_PER_BLOCK);
    } else {
        memset(pkt.blockRead.data, 0, VMU_BYTES_PER_BLOCK);
    }

    pkt.crc = MapleBus::calcCRC(
        (uint32_t*)&pkt.header,
        sizeof(pkt) / sizeof(uint32_t) - 2);
    bus.sendPacket((uint32_t*)&pkt, sizeof(pkt) / sizeof(uint32_t));
}

void DreamcastVMU::sendFileErrorResponse(uint32_t errorCode, uint8_t port, MapleBus& bus) {
    static MapleFileErrorPacket pkt;
    memset(&pkt, 0, sizeof(pkt));

    pkt.bitPairsMinus1 = MapleBus::calcBitPairs(sizeof(pkt));
    pkt.header.command     = MAPLE_CMD_RESPOND_FILE_ERROR;
    pkt.header.destination = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | port;
    pkt.header.origin      = (MAPLE_SUB0_ADDR & MAPLE_PERIPH_MASK) | port;
    pkt.header.numWords    = sizeof(MapleFileError) / sizeof(uint32_t);

    pkt.fileError.errorCode = errorCode;

    pkt.crc = MapleBus::calcCRC(
        (uint32_t*)&pkt.header,
        sizeof(pkt) / sizeof(uint32_t) - 2);
    bus.sendPacket((uint32_t*)&pkt, sizeof(pkt) / sizeof(uint32_t));
}

// ============================================================
// Main command handler
// ============================================================

bool DreamcastVMU::handleCommand(const uint8_t* packet, uint rxLen, uint8_t port, MapleBus& bus) {
    const MaplePacketHeader* header = (const MaplePacketHeader*)packet;

    debugVmuRxCount++;

    // Payload starts after the 4-byte header.
    // IMPORTANT: The software RX decoder stores bytes in wire (big-endian) order.
    // When cast to uint32_t* on this little-endian platform, each word is byte-reversed
    // relative to the DC's original value. Use __builtin_bswap32() when reading funcCode
    // and location words. Do NOT bswap raw block data (BLOCK_WRITE payload) — it must
    // stay in wire byte order so the BLOCK_READ round-trip preserves the original bytes.
    const uint32_t* payload = (const uint32_t*)(packet + sizeof(MaplePacketHeader));
    uint payloadWords = (rxLen - sizeof(MaplePacketHeader)) / sizeof(uint32_t);

    switch (header->command) {
        case MAPLE_CMD_DEVICE_REQUEST:
        case MAPLE_CMD_ALL_STATUS_REQUEST:
            sendInfoResponse(port, bus);
            debugVmuTxCount++;
            return true;

        case MAPLE_CMD_RESET_DEVICE:
        case MAPLE_CMD_SHUTDOWN_DEVICE:
            sendAckResponse(port, bus);
            debugVmuTxCount++;
            return true;

        case MAPLE_CMD_GET_MEDIA_INFO: {
            // Payload word 0 must be MAPLE_FUNC_MEMORY_CARD (bswap from wire order)
            if (payloadWords < 1 || __builtin_bswap32(payload[0]) != MAPLE_FUNC_MEMORY_CARD) {
                sendFileErrorResponse(0x00000001, port, bus);  // Function code not supported
                debugVmuTxCount++;
                return true;
            }
            sendMemoryInfoResponse(port, bus);
            debugVmuTxCount++;
            return true;
        }

        case MAPLE_CMD_BLOCK_READ: {
            if (payloadWords < 2 || __builtin_bswap32(payload[0]) != MAPLE_FUNC_MEMORY_CARD) {
                sendFileErrorResponse(0x00000001, port, bus);
                debugVmuTxCount++;
                return true;
            }
            uint32_t locWord = __builtin_bswap32(payload[1]);
            uint16_t blockNum = locWord & 0xFFFF;
            uint8_t phase = (locWord >> 16) & 0xFF;

            if (blockNum >= VMU_NUM_BLOCKS || phase != 0) {
                sendFileErrorResponse(0x00000010, port, bus);  // Out of range
                debugVmuTxCount++;
                return true;
            }

            sendBlockReadResponse(blockNum, port, bus);
            debugVmuTxCount++;
            return true;
        }

        case MAPLE_CMD_BLOCK_WRITE: {
            if (payloadWords < 2 || __builtin_bswap32(payload[0]) != MAPLE_FUNC_MEMORY_CARD) {
                sendFileErrorResponse(0x00000001, port, bus);
                debugVmuTxCount++;
                return true;
            }
            uint32_t locWord = __builtin_bswap32(payload[1]);
            uint16_t blockNum = locWord & 0xFFFF;
            uint8_t phase = (locWord >> 16) & 0xFF;

            if (blockNum >= VMU_NUM_BLOCKS || phase >= VMU_WRITES_PER_BLOCK) {
                sendFileErrorResponse(0x00000010, port, bus);
                debugVmuTxCount++;
                return true;
            }

            // Expected data: 32 words (128 bytes) for this phase
            uint expectedDataWords = VMU_WORDS_PER_WRITE;
            if (payloadWords < 2 + expectedDataWords) {
                sendFileErrorResponse(0x00000010, port, bus);
                debugVmuTxCount++;
                return true;
            }

            // Validate block consistency across write phases
            if (phase == 0) {
                // Starting new block — reset tracking
                currentWriteBlock = blockNum;
                writePhaseMask = 0;
                memset(writeBuffer, 0, sizeof(writeBuffer));
            } else if (blockNum != currentWriteBlock) {
                // Block mismatch — reject and reset
                currentWriteBlock = 0xFFFF;
                writePhaseMask = 0;
                sendFileErrorResponse(0x00000010, port, bus);
                debugVmuTxCount++;
                return true;
            }

            // Copy write data into the write buffer at the correct phase offset
            uint32_t byteOffset = phase * VMU_BYTES_PER_WRITE;
            memcpy(writeBuffer + byteOffset, &payload[2], VMU_BYTES_PER_WRITE);
            writePhaseMask |= (1u << phase);

            sendAckResponse(port, bus);
            debugVmuTxCount++;
            debugVmuWriteCount++;
            return true;
        }

        case MAPLE_CMD_BLOCK_COMPLETE_WRITE: {
            // Commit the accumulated block write to flash
            if (payloadWords < 2 || __builtin_bswap32(payload[0]) != MAPLE_FUNC_MEMORY_CARD) {
                sendFileErrorResponse(0x00000001, port, bus);
                debugVmuTxCount++;
                return true;
            }
            uint32_t locWord = __builtin_bswap32(payload[1]);
            uint16_t blockNum = locWord & 0xFFFF;
            uint8_t phase = (locWord >> 16) & 0xFF;

            if (blockNum >= VMU_NUM_BLOCKS || phase != VMU_WRITES_PER_BLOCK) {
                sendFileErrorResponse(0x00000010, port, bus);
                debugVmuTxCount++;
                return true;
            }

            // Validate: all 4 phases received for this block
            if (blockNum != currentWriteBlock || writePhaseMask != 0x0F) {
                sendFileErrorResponse(0x00000010, port, bus);
                debugVmuTxCount++;
                currentWriteBlock = 0xFFFF;
                writePhaseMask = 0;
                return true;
            }

            // Schedule the flash write (will be performed in processFlashWrite)
            scheduleBlockWrite(blockNum);
            currentWriteBlock = 0xFFFF;
            writePhaseMask = 0;

            sendAckResponse(port, bus);
            debugVmuTxCount++;
            return true;
        }

        default:
            // Unknown command — don't respond
            return false;
    }
}
