#include "drivers/dreamcast/DreamcastVMU.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/time.h"

// Dreamcast VMU sub-peripheral — wire-order architecture (NO DMA bswap).
// Based on MaplePad by mackieks (github.com/mackieks/MaplePad).
// All packet data and flash storage is in wire (network) byte order.
// Field extraction uses __builtin_bswap32() to convert wire → host.

// XIP base address for reading VMU data from flash
#define VMU_XIP_BASE  (XIP_BASE + VMU_FLASH_OFFSET)

// Wire-order function code constants
#define MAPLE_FUNC_MEMORY_CARD_WIRE  maple_host_to_wire(MAPLE_FUNC_MEMORY_CARD)

DreamcastVMU::DreamcastVMU()
    : flashWriteBlockNum(0),
      currentWriteBlock(0xFFFF), writePhaseMask(0) {
    memset(writeBuffer, 0, sizeof(writeBuffer));
    memset(sectorBuffer, 0, sizeof(sectorBuffer));
    memset(cmdLog, 0, sizeof(cmdLog));
}

void DreamcastVMU::logCommand(uint8_t cmd, uint8_t response, uint32_t funcCode,
                               uint32_t locWord, uint16_t blockNum, uint8_t phase,
                               uint8_t pWords, const uint8_t* rawPayload,
                               uint rawPayloadLen) {
    VmuLogEntry& e = cmdLog[cmdLogWriteIdx];
    e.timestamp_us = (uint32_t)time_us_64();
    e.cmd = cmd;
    e.response = response;
    e.funcCode = funcCode;
    e.locWord = locWord;
    e.blockNum = blockNum;
    e.phase = phase;
    e.payloadWords = pWords;
    e.rawBlockLo16 = locWord & 0xFFFF;
    // Capture 4 header bytes + first 8 payload bytes for alignment debugging.
    // rawPayload points to payload start (packet + 4), so header is at rawPayload - 4.
    memset(e.rawBytes, 0, sizeof(e.rawBytes));
    if (rawPayload) {
        const uint8_t* hdrStart = rawPayload - 4;  // header is 1 uint32_t = 4 bytes
        memcpy(e.rawBytes, hdrStart, 4); // header bytes
        if (rawPayloadLen > 0) {
            uint copyLen = rawPayloadLen > 8 ? 8 : rawPayloadLen;
            memcpy(e.rawBytes + 4, rawPayload, copyLen); // payload bytes
        }
    }
    cmdLogWriteIdx = (cmdLogWriteIdx + 1) % VMU_LOG_MAX_ENTRIES;
    if (cmdLogCount < 0xFFFF) cmdLogCount++;
}

void DreamcastVMU::init() {
    buildVMUInfoPacket();
    buildVMUAckPacket();

    if (needsFormat() || needsVersionUpdate()) {
        format();
    }
}

bool DreamcastVMU::needsFormat() {
    // Check system block for the 0x55 signature (first 16 bytes must all be 0x55).
    // 0x55 is byte-palindromic: bswap32(0x55555555) = 0x55555555, so byte-level
    // checks work regardless of wire/host order.
    const uint8_t* systemBlock = (const uint8_t*)(VMU_XIP_BASE + VMU_SYSTEM_BLOCK_NO * VMU_BYTES_PER_BLOCK);
    for (int i = 0; i < 16; i++) {
        if (systemBlock[i] != 0x55) return true;
    }
    return false;
}

bool DreamcastVMU::needsVersionUpdate() {
    // Version byte is stored in system block word 4.
    // In HOST order: version at bits [15:8] (byte offset 17).
    // In wire-order flash (after bswap32): version at bits [23:16].
    // Read as uint32_t, bswap32 to HOST, extract bits [15:8].
    const uint32_t* sysBlock32 = (const uint32_t*)(VMU_XIP_BASE + VMU_SYSTEM_BLOCK_NO * VMU_BYTES_PER_BLOCK);
    uint32_t word4host = __builtin_bswap32(sysBlock32[4]);
    uint8_t version = (word4host >> 8) & 0xFF;
    return version != VMU_FORMAT_VERSION;
}

void DreamcastVMU::patchVersionByte() {
    // Update version byte AND media info in system block without full re-format.
    flashWriteBlockNum = VMU_SYSTEM_BLOCK_NO;
    const uint8_t* block = readBlock(VMU_SYSTEM_BLOCK_NO);
    memcpy(writeBuffer, block, VMU_BYTES_PER_BLOCK);

    // writeBuffer is in wire order. Convert word 4 to HOST, set version, convert back.
    uint32_t* buf32 = (uint32_t*)writeBuffer;
    uint32_t word4host = __builtin_bswap32(buf32[4]);
    word4host = (word4host & ~0x0000FF00) | ((uint32_t)VMU_FORMAT_VERSION << 8);
    buf32[4] = __builtin_bswap32(word4host);

    // Also rewrite the media info section with current constants (wire order)
    buildMediaInfoForFlash(&buf32[VMU_MEDIA_INFO_OFFSET]);

    doFlashWrite();
}

void DreamcastVMU::buildVMUInfoPacket() {
    // VMU device info response: 28 payload words
    // Layout: [0]=bitPairs, [1]=header, [2..29]=deviceInfo, [30]=CRC
    memset(vmuInfoBuf, 0, sizeof(vmuInfoBuf));

    vmuInfoBuf[0] = MapleBus::calcBitPairs(sizeof(vmuInfoBuf));
    vmuInfoBuf[1] = maple_make_header(
        28,
        MAPLE_SUB0_ADDR,
        MAPLE_DC_ADDR,
        MAPLE_CMD_RESPOND_DEVICE_STATUS
    );

    // Build 28 words of device info as a byte buffer in HOST order, then bswap32 each word.
    uint8_t infoBuf[112];
    memset(infoBuf, 0, sizeof(infoBuf));

    // func (4 bytes, offset 0) — VMU declares storage function only
    uint32_t func = MAPLE_FUNC_MEMORY_CARD;
    infoBuf[0] = (func >> 0) & 0xFF;
    infoBuf[1] = (func >> 8) & 0xFF;
    infoBuf[2] = (func >> 16) & 0xFF;
    infoBuf[3] = (func >> 24) & 0xFF;

    // funcData[0] (4 bytes, offset 4) — storage function definition
    //   bits 7:   removable = 0
    //   bits 6:   CRC needed = 0
    //   bits 11-8:  reads per block = 1
    //   bits 15-12: writes per block = 4
    //   bits 23-16: (bytes_per_block/32 - 1) = 15
    //   bits 31-24: (partitions - 1) = 0
    // Total: 0x000F4100
    uint32_t fd0 = 0x000F4100;
    infoBuf[4] = (fd0 >> 0) & 0xFF;
    infoBuf[5] = (fd0 >> 8) & 0xFF;
    infoBuf[6] = (fd0 >> 16) & 0xFF;
    infoBuf[7] = (fd0 >> 24) & 0xFF;

    // funcData[1], funcData[2] (offsets 8-15) — zero

    // areaCode (1 byte, offset 16)
    infoBuf[16] = 0xFF;  // All regions

    // connectorDirection (1 byte, offset 17)
    infoBuf[17] = 0;

    // productName (30 bytes, offset 18)
    const char* name = "Visual Memory";
    for (int i = 0; i < 30; i++) {
        infoBuf[18 + i] = (name[i] && i < (int)strlen(name)) ? name[i] : ' ';
    }

    // productLicense (60 bytes, offset 48)
    const char* license = "Produced By or Under License From SEGA ENTERPRISES,LTD.";
    for (int i = 0; i < 60; i++) {
        infoBuf[48 + i] = (license[i] && i < (int)strlen(license)) ? license[i] : ' ';
    }

    // standbyPower (2 bytes LE, offset 108)
    uint16_t standby = 124;  // 12.4mA * 10
    infoBuf[108] = standby & 0xFF;
    infoBuf[109] = (standby >> 8) & 0xFF;

    // maxPower (2 bytes LE, offset 110)
    uint16_t maxPwr = 130;  // 13.0mA * 10
    infoBuf[110] = maxPwr & 0xFF;
    infoBuf[111] = (maxPwr >> 8) & 0xFF;

    // Convert to wire order: bswap32 each 4-byte word
    uint32_t* info = &vmuInfoBuf[2];
    for (int i = 0; i < 28; i++) {
        uint32_t hostWord = infoBuf[i*4]
                          | ((uint32_t)infoBuf[i*4+1] << 8)
                          | ((uint32_t)infoBuf[i*4+2] << 16)
                          | ((uint32_t)infoBuf[i*4+3] << 24);
        info[i] = __builtin_bswap32(hostWord);
    }
}

void DreamcastVMU::buildVMUAckPacket() {
    memset(vmuAckBuf, 0, sizeof(vmuAckBuf));

    vmuAckBuf[0] = MapleBus::calcBitPairs(sizeof(vmuAckBuf));
    vmuAckBuf[1] = maple_make_header(
        0,
        MAPLE_SUB0_ADDR,
        MAPLE_DC_ADDR,
        MAPLE_CMD_RESPOND_ACK
    );
}

const uint8_t* DreamcastVMU::readBlock(uint16_t blockNum) {
    if (blockNum >= VMU_NUM_BLOCKS) return nullptr;
    return (const uint8_t*)(VMU_XIP_BASE + blockNum * VMU_BYTES_PER_BLOCK);
}

// Build 6 media info words in wire order for GET_MEDIA_INFO response payload.
// Each word packs two 16-bit LE values. The DC interprets them after its own
// DMA bswap, so we need them in the same wire format as MaplePad.
void DreamcastVMU::buildMediaInfoForWire(uint32_t* out) {
    // Build in HOST order first (two LE uint16 values per uint32):
    //   hostWord = (upper16 << 16) | lower16
    // Then bswap32 to wire order.
    uint32_t host[6];
    host[0] = ((uint32_t)(VMU_NUM_BLOCKS - 1) << 16) | 0;           // totalBlocks-1, partition
    host[1] = ((uint32_t)VMU_SYSTEM_BLOCK_NO << 16)  | VMU_FAT_BLOCK_NO;
    host[2] = ((uint32_t)VMU_NUM_FAT_BLOCKS << 16)   | VMU_FILE_INFO_BLOCK_NO;
    host[3] = ((uint32_t)VMU_NUM_FILE_INFO << 16)     | 0;
    host[4] = ((uint32_t)VMU_NUM_SAVE_BLOCKS << 16)   | VMU_SAVE_AREA_BLOCK_NO;
    host[5] = 0x00008000;  // execution file (icon shape)

    for (int i = 0; i < 6; i++) {
        out[i] = __builtin_bswap32(host[i]);
    }
}

// Build 6 media info words in wire order for system block flash storage.
// Same encoding as buildMediaInfoForWire — flash stores wire-order data
// which gets sent to DC via memcpy (zero conversion).
void DreamcastVMU::buildMediaInfoForFlash(uint32_t* out) {
    // For flash, we need the media info in the format that the DC will see
    // when it reads the system block. The DC reads raw wire-order data and
    // interprets each uint32 as two LE uint16 values (after its own bswap).
    //
    // Build as two LE uint16 values packed per uint32 in HOST order,
    // then bswap32 to wire order for flash storage.
    uint32_t host[6];
    host[0] = (uint32_t)(VMU_NUM_BLOCKS - 1)  | (0 << 16);
    host[1] = (uint32_t)VMU_SYSTEM_BLOCK_NO    | ((uint32_t)VMU_FAT_BLOCK_NO << 16);
    host[2] = (uint32_t)VMU_NUM_FAT_BLOCKS     | ((uint32_t)VMU_FILE_INFO_BLOCK_NO << 16);
    host[3] = (uint32_t)VMU_NUM_FILE_INFO      | (0 << 16);
    host[4] = (uint32_t)VMU_NUM_SAVE_BLOCKS    | ((uint32_t)VMU_SAVE_AREA_BLOCK_NO << 16);
    host[5] = 0x00800000;  // execution file

    for (int i = 0; i < 6; i++) {
        out[i] = __builtin_bswap32(host[i]);
    }
}

void DreamcastVMU::format() {
    // Format creates the minimum VMU filesystem: system block, FAT, and empty file info.
    // All data stored in wire (network) byte order — matches MaplePad's approach.
    // When the DC reads blocks via BLOCK_READ, the wire-order data goes directly to
    // the TX buffer (memcpy, zero conversion). DC's own DMA bswap converts to HOST.

    uint8_t blockBuf[VMU_BYTES_PER_BLOCK];
    uint32_t* block32 = (uint32_t*)blockBuf;

    //
    // System Block (block 255)
    //
    // Build in HOST order first, then bswap32 every word to wire order.
    memset(blockBuf, 0, VMU_BYTES_PER_BLOCK);

    // Signature: first 16 bytes all 0x55 (marks formatted VMU)
    // 0x55 is byte-palindromic — bswap32(0x55555555) = 0x55555555
    memset(blockBuf, 0x55, 16);

    // Format version at HOST byte offset 17 (word 4, bits [15:8])
    // We set it in the HOST-order buffer; bswap32 moves it to bits [23:16] in wire order.
    blockBuf[17] = VMU_FORMAT_VERSION;

    // Date/time markers at byte offset 0x030 (word 12)
    // BCD format: century, year, month, day, hour, min, sec, weekday
    block32[12] = 0x19990909;
    block32[13] = 0x00001000;

    // Media info at word offset 16 — build in HOST LE 16-bit format
    // (two LE uint16 values per uint32)
    uint32_t* mediaSlot = &block32[VMU_MEDIA_INFO_OFFSET];
    mediaSlot[0] = (uint32_t)(VMU_NUM_BLOCKS - 1)  | (0 << 16);
    mediaSlot[1] = (uint32_t)VMU_SYSTEM_BLOCK_NO    | ((uint32_t)VMU_FAT_BLOCK_NO << 16);
    mediaSlot[2] = (uint32_t)VMU_NUM_FAT_BLOCKS     | ((uint32_t)VMU_FILE_INFO_BLOCK_NO << 16);
    mediaSlot[3] = (uint32_t)VMU_NUM_FILE_INFO      | (0 << 16);
    mediaSlot[4] = (uint32_t)VMU_NUM_SAVE_BLOCKS    | ((uint32_t)VMU_SAVE_AREA_BLOCK_NO << 16);
    mediaSlot[5] = 0x00800000;  // execution file

    // Convert entire system block from HOST to wire order
    for (int i = 0; i < VMU_WORDS_PER_BLOCK; i++) {
        block32[i] = __builtin_bswap32(block32[i]);
    }

    // Write system block to flash
    flashWriteBlockNum = VMU_SYSTEM_BLOCK_NO;
    memcpy(writeBuffer, blockBuf, VMU_BYTES_PER_BLOCK);
    doFlashWrite();

    //
    // FAT Block (block 254)
    //
    // FAT has 256 entries (one per block), each 16 bits LE.
    // 256 * 2 = 512 bytes = one block.
    // Build in HOST order (native LE uint16 array), then bswap32 each uint32 word.
    uint16_t fat16[256];
    for (int i = 0; i < 256; i++) {
        fat16[i] = 0xFFFC;  // Free
    }

    // Mark system block (255) as system-reserved
    fat16[VMU_SYSTEM_BLOCK_NO] = 0xFFFA;

    // Mark FAT block (254) as system-reserved
    fat16[VMU_FAT_BLOCK_NO] = 0xFFFA;

    // Mark file info blocks (241-253) as a chain
    for (int i = VMU_FILE_INFO_BLOCK_NO; i > VMU_FILE_INFO_BLOCK_NO - VMU_NUM_FILE_INFO + 1; i--) {
        fat16[i] = i - 1;
    }
    fat16[VMU_FILE_INFO_BLOCK_NO - VMU_NUM_FILE_INFO + 1] = 0xFFFA;  // End of chain

    // Pack into uint32 words (HOST order: two LE uint16 per word), then bswap32
    for (int i = 0; i < 128; i++) {
        uint32_t hostWord = (uint32_t)fat16[2*i] | ((uint32_t)fat16[2*i+1] << 16);
        block32[i] = __builtin_bswap32(hostWord);
    }

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
}


// ============================================================
// Response packet builders (wire order)
// ============================================================

void DreamcastVMU::sendInfoResponse(uint8_t port, MapleBus& bus) {
    uint8_t origin = (MAPLE_SUB0_ADDR & MAPLE_PERIPH_MASK) | port;
    uint8_t dest = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | port;

    vmuInfoBuf[1] = maple_make_header(28, origin, dest, MAPLE_CMD_RESPOND_DEVICE_STATUS);
    vmuInfoBuf[30] = MapleBus::calcCRC(&vmuInfoBuf[1], 29);  // header + 28 payload words
    bus.sendPacket(vmuInfoBuf, 31);
}

void DreamcastVMU::sendAckResponse(uint8_t port, MapleBus& bus) {
    uint8_t origin = (MAPLE_SUB0_ADDR & MAPLE_PERIPH_MASK) | port;
    uint8_t dest = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | port;

    vmuAckBuf[1] = maple_make_header(0, origin, dest, MAPLE_CMD_RESPOND_ACK);
    vmuAckBuf[2] = MapleBus::calcCRC(&vmuAckBuf[1], 1);
    bus.sendPacket(vmuAckBuf, 3);
}

void DreamcastVMU::sendMemoryInfoResponse(uint8_t port, MapleBus& bus) {
    // GET_MEDIA_INFO response: funcCode + 6 media info words = 7 payload words
    static uint32_t pkt[10];  // bitpairs + header + 7 payload + CRC
    memset(pkt, 0, sizeof(pkt));

    uint8_t origin = (MAPLE_SUB0_ADDR & MAPLE_PERIPH_MASK) | port;
    uint8_t dest = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | port;

    pkt[0] = MapleBus::calcBitPairs(sizeof(pkt));
    pkt[1] = maple_make_header(7, origin, dest, MAPLE_CMD_RESPOND_DATA_XFER);
    pkt[2] = MAPLE_FUNC_MEMORY_CARD_WIRE;  // funcCode in wire order
    buildMediaInfoForWire(&pkt[3]);          // 6 media info words in wire order

    pkt[9] = MapleBus::calcCRC(&pkt[1], 8);  // header + 7 payload words
    bus.sendPacket(pkt, 10);
}

void DreamcastVMU::sendBlockReadResponse(uint16_t blockNum, uint32_t locWord, uint8_t port, MapleBus& bus) {
    // BLOCK_READ response: funcCode + locWord + 128 data words = 130 payload words
    static uint32_t pkt[134];  // bitpairs + header + 130 payload + CRC

    uint8_t origin = (MAPLE_SUB0_ADDR & MAPLE_PERIPH_MASK) | port;
    uint8_t dest = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | port;

    pkt[0] = MapleBus::calcBitPairs(sizeof(pkt));
    pkt[1] = maple_make_header(130, origin, dest, MAPLE_CMD_RESPOND_DATA_XFER);
    pkt[2] = MAPLE_FUNC_MEMORY_CARD_WIRE;  // funcCode in wire order
    // Echo the raw location word exactly as received from the DC (wire order).
    pkt[3] = locWord;

    // Copy block data directly from flash — both are wire order, zero conversion.
    const uint8_t* blockData = readBlock(blockNum);
    if (blockData) {
        memcpy(&pkt[4], blockData, VMU_BYTES_PER_BLOCK);
    } else {
        memset(&pkt[4], 0, VMU_BYTES_PER_BLOCK);
    }

    pkt[133] = MapleBus::calcCRC(&pkt[1], 132);  // header + 130 payload words
    bus.sendPacket(pkt, 134);
}

void DreamcastVMU::sendFileErrorResponse(uint32_t errorCode, uint8_t port, MapleBus& bus) {
    static uint32_t pkt[4];  // bitpairs + header + errorCode + CRC
    memset(pkt, 0, sizeof(pkt));

    uint8_t origin = (MAPLE_SUB0_ADDR & MAPLE_PERIPH_MASK) | port;
    uint8_t dest = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | port;

    pkt[0] = MapleBus::calcBitPairs(sizeof(pkt));
    pkt[1] = maple_make_header(1, origin, dest, MAPLE_CMD_RESPOND_FILE_ERROR);
    pkt[2] = maple_host_to_wire(errorCode);

    debugVmuLastError = errorCode;

    pkt[3] = MapleBus::calcCRC(&pkt[1], 2);  // header + 1 payload word
    bus.sendPacket(pkt, 4);
}

// ============================================================
// Main command handler (wire-order packet parsing)
// ============================================================

bool DreamcastVMU::handleCommand(const uint8_t* packet, uint rxLen, uint8_t port, MapleBus& bus) {
    // Parse header from wire-order word
    uint32_t hdrWord = ((const uint32_t*)packet)[0];
    int8_t cmd = maple_hdr_command(hdrWord);

    debugVmuRxCount++;
    debugVmuLastCmd = cmd;

    // Payload starts after the 4-byte header word.
    // All payload words are in wire (network) byte order.
    const uint8_t* rawPayload = packet + 4;
    uint rawPayloadLen = (rxLen > 4) ? rxLen - 4 : 0;
    const uint32_t* payload = (const uint32_t*)rawPayload;
    uint payloadWords = rawPayloadLen / sizeof(uint32_t);

    switch (cmd) {
        case MAPLE_CMD_DEVICE_REQUEST:
        case MAPLE_CMD_ALL_STATUS_REQUEST:
            logCommand(cmd, MAPLE_CMD_RESPOND_DEVICE_STATUS, 0, 0, 0xFFFF, 0xFF, payloadWords);
            sendInfoResponse(port, bus);
            debugVmuTxCount++;
            return true;

        case MAPLE_CMD_RESET_DEVICE:
        case MAPLE_CMD_SHUTDOWN_DEVICE:
            currentWriteBlock = 0xFFFF;
            writePhaseMask = 0;
            logCommand(cmd, MAPLE_CMD_RESPOND_ACK, 0, 0, 0xFFFF, 0xFF, payloadWords);
            sendAckResponse(port, bus);
            debugVmuTxCount++;
            return true;

        case MAPLE_CMD_GET_MEDIA_INFO: {
            uint32_t fc = (payloadWords >= 1) ? payload[0] : 0;
            if (payloadWords >= 1) debugVmuLastFuncCode = fc;
            if (payloadWords < 1 || fc != MAPLE_FUNC_MEMORY_CARD_WIRE) {
                logCommand(0x0A, MAPLE_CMD_RESPOND_FILE_ERROR, fc, 0, 0xFFFF, 0xFF, payloadWords, rawPayload, rawPayloadLen);
                sendFileErrorResponse(0x00000001, port, bus);
                debugVmuTxCount++;
                return true;
            }
            logCommand(0x0A, MAPLE_CMD_RESPOND_DATA_XFER, fc, 0, 0xFFFF, 0xFF, payloadWords, rawPayload, rawPayloadLen);
            sendMemoryInfoResponse(port, bus);
            debugVmuTxCount++;
            return true;
        }

        case MAPLE_CMD_BLOCK_READ: {
            uint32_t fc = (payloadWords >= 1) ? payload[0] : 0;
            uint32_t lw = (payloadWords >= 2) ? payload[1] : 0;
            if (payloadWords >= 1) {
                debugVmuLastFuncCode = fc;
                debugVmuRawPayload0 = fc;
            }
            if (payloadWords >= 2) debugVmuLastLocWord = lw;
            if (payloadWords < 2 || fc != MAPLE_FUNC_MEMORY_CARD_WIRE) {
                logCommand(0x0B, MAPLE_CMD_RESPOND_FILE_ERROR, fc, lw, 0xFFFF, 0xFF, payloadWords, rawPayload, rawPayloadLen);
                sendFileErrorResponse(0x00000001, port, bus);
                debugVmuTxCount++;
                return true;
            }

            // Extract block and phase from location word.
            // Wire → host via bswap32, then extract fields from HOST-order value.
            // Location word spec: (partition << 24) | (phase << 16) | block
            // In HOST order: block at bits [15:0], phase at bits [23:16].
            uint32_t lwHost = maple_wire_to_host(lw);
            uint16_t blockNum = lwHost & 0xFFFF;
            uint8_t phase = (lwHost >> 16) & 0xFF;

            if (blockNum >= VMU_NUM_BLOCKS || phase != 0) {
                debugVmuReadErrCount++;
                debugVmuLastBlockErr = blockNum;
                logCommand(0x0B, MAPLE_CMD_RESPOND_FILE_ERROR, fc, lw, blockNum, phase, payloadWords, rawPayload, rawPayloadLen);
                sendFileErrorResponse(0x00000010, port, bus);
                debugVmuTxCount++;
                return true;
            }

            debugVmuReadOkCount++;
            debugVmuLastBlockOk = blockNum;
            logCommand(0x0B, MAPLE_CMD_RESPOND_DATA_XFER, fc, lw, blockNum, phase, payloadWords, rawPayload, rawPayloadLen);
            // Echo raw wire-order location word — the DC expects its own value back unchanged.
            sendBlockReadResponse(blockNum, lw, port, bus);
            debugVmuTxCount++;
            return true;
        }

        case MAPLE_CMD_BLOCK_WRITE: {
            uint32_t fc = (payloadWords >= 1) ? payload[0] : 0;
            uint32_t lw = (payloadWords >= 2) ? payload[1] : 0;
            if (payloadWords >= 1) debugVmuLastFuncCode = fc;
            if (payloadWords >= 2) debugVmuLastLocWord = lw;
            if (payloadWords < 2 || fc != MAPLE_FUNC_MEMORY_CARD_WIRE) {
                logCommand(0x0C, MAPLE_CMD_RESPOND_FILE_ERROR, fc, lw, 0xFFFF, 0xFF, payloadWords, rawPayload, rawPayloadLen);
                sendFileErrorResponse(0x00000001, port, bus);
                debugVmuTxCount++;
                return true;
            }

            // Extract block and phase from location word (wire → host)
            uint32_t lwHost = maple_wire_to_host(lw);
            uint16_t blockNum = lwHost & 0xFFFF;
            uint8_t phase = (lwHost >> 16) & 0xFF;

            if (blockNum >= VMU_NUM_BLOCKS || phase >= VMU_WRITES_PER_BLOCK) {
                logCommand(0x0C, MAPLE_CMD_RESPOND_FILE_ERROR, fc, lw, blockNum, phase, payloadWords, rawPayload, rawPayloadLen);
                sendFileErrorResponse(0x00000010, port, bus);
                debugVmuTxCount++;
                return true;
            }

            uint expectedDataWords = VMU_WORDS_PER_WRITE;
            if (payloadWords < 2 + expectedDataWords) {
                logCommand(0x0C, MAPLE_CMD_RESPOND_FILE_ERROR, fc, lw, blockNum, phase, payloadWords, rawPayload, rawPayloadLen);
                sendFileErrorResponse(0x00000010, port, bus);
                debugVmuTxCount++;
                return true;
            }

            if (phase == 0) {
                currentWriteBlock = blockNum;
                writePhaseMask = 0;
                memset(writeBuffer, 0, sizeof(writeBuffer));
            } else if (blockNum != currentWriteBlock) {
                currentWriteBlock = 0xFFFF;
                writePhaseMask = 0;
                logCommand(0x0C, MAPLE_CMD_RESPOND_FILE_ERROR, fc, lw, blockNum, phase, payloadWords, rawPayload, rawPayloadLen);
                sendFileErrorResponse(0x00000010, port, bus);
                debugVmuTxCount++;
                return true;
            }

            // Copy data from RX payload to write buffer.
            // Both are wire order — pure memcpy, zero conversion.
            uint32_t byteOffset = phase * VMU_BYTES_PER_WRITE;
            memcpy(writeBuffer + byteOffset, &payload[2], VMU_BYTES_PER_WRITE);
            writePhaseMask |= (1u << phase);

            logCommand(0x0C, MAPLE_CMD_RESPOND_ACK, fc, lw, blockNum, phase, payloadWords, rawPayload, rawPayloadLen);
            sendAckResponse(port, bus);
            debugVmuTxCount++;
            debugVmuWriteCount++;
            return true;
        }

        case MAPLE_CMD_BLOCK_COMPLETE_WRITE: {
            uint32_t fc = (payloadWords >= 1) ? payload[0] : 0;
            uint32_t lw = (payloadWords >= 2) ? payload[1] : 0;
            if (payloadWords >= 1) debugVmuLastFuncCode = fc;
            if (payloadWords >= 2) debugVmuLastLocWord = lw;
            if (payloadWords < 2 || fc != MAPLE_FUNC_MEMORY_CARD_WIRE) {
                logCommand(0x0D, MAPLE_CMD_RESPOND_FILE_ERROR, fc, lw, 0xFFFF, 0xFF, payloadWords, rawPayload, rawPayloadLen);
                sendFileErrorResponse(0x00000001, port, bus);
                debugVmuTxCount++;
                return true;
            }

            // Extract block and phase from location word (wire → host)
            uint32_t lwHost = maple_wire_to_host(lw);
            uint16_t blockNum = lwHost & 0xFFFF;
            uint8_t phase = (lwHost >> 16) & 0xFF;

            if (blockNum >= VMU_NUM_BLOCKS || phase != VMU_WRITES_PER_BLOCK) {
                logCommand(0x0D, MAPLE_CMD_RESPOND_FILE_ERROR, fc, lw, blockNum, phase, payloadWords, rawPayload, rawPayloadLen);
                sendFileErrorResponse(0x00000010, port, bus);
                debugVmuTxCount++;
                return true;
            }

            if (blockNum != currentWriteBlock || writePhaseMask != 0x0F) {
                logCommand(0x0D, MAPLE_CMD_RESPOND_FILE_ERROR, fc, lw, blockNum, phase, payloadWords, rawPayload, rawPayloadLen);
                sendFileErrorResponse(0x00000010, port, bus);
                debugVmuTxCount++;
                currentWriteBlock = 0xFFFF;
                writePhaseMask = 0;
                return true;
            }

            // System block override: if DC writes system block with zero save area
            // media info, fill it with our default values (in wire order).
            if (blockNum == VMU_SYSTEM_BLOCK_NO) {
                uint32_t* buf32 = (uint32_t*)writeBuffer;
                uint32_t saveWord = buf32[VMU_MEDIA_INFO_OFFSET + 4];
                if (saveWord == 0) {
                    // Build in HOST, bswap32 to wire order
                    uint32_t hostVal = (uint32_t)VMU_NUM_SAVE_BLOCKS | ((uint32_t)VMU_SAVE_AREA_BLOCK_NO << 16);
                    buf32[VMU_MEDIA_INFO_OFFSET + 4] = __builtin_bswap32(hostVal);
                }
            }

            flashWriteBlockNum = blockNum;
            currentWriteBlock = 0xFFFF;
            writePhaseMask = 0;
            doFlashWrite();

            logCommand(0x0D, MAPLE_CMD_RESPOND_ACK, fc, lw, blockNum, phase, payloadWords, rawPayload, rawPayloadLen);
            sendAckResponse(port, bus);
            debugVmuTxCount++;
            return true;
        }

        default:
            // Unknown command — don't respond
            return false;
    }
}
