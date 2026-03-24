#pragma once

// Dreamcast VMU (Visual Memory Unit) sub-peripheral emulation.
// Wire-order architecture (NO DMA bswap) — based on MaplePad by mackieks.

#include <stdint.h>
#include <string.h>
#include "drivers/dreamcast/maple_bus.h"
#include "hardware/flash.h"

class Gamepad;

// VMU command log — records every command received and response sent
// Stored in RAM, ring buffer, for diagnostic purposes
#define VMU_LOG_MAX_ENTRIES  64

struct VmuLogEntry {
    uint32_t timestamp_us;    // microseconds since boot (wraps at ~71 min)
    uint8_t  cmd;             // Maple command received (0x01, 0x0A, 0x0B, 0x0C, 0x0D, etc.)
    uint8_t  response;        // Response sent (0x05, 0x07, 0x08, 0xFC)
    uint16_t blockNum;        // Extracted block number (or 0xFFFF if N/A)
    uint32_t funcCode;        // Raw payload[0] (funcCode, wire order)
    uint32_t locWord;         // Raw payload[1] (location word, wire order, or 0 if N/A)
    uint8_t  phase;           // Write phase (0-3 for BLOCK_WRITE, 4 for COMPLETE, 0xFF if N/A)
    uint8_t  payloadWords;    // Number of payload words received
    uint16_t rawBlockLo16;    // Raw locWord & 0xFFFF before any extraction
    uint8_t  rawBytes[12];   // 4 header bytes + 8 payload bytes (for alignment debugging)
};

// VMU storage constants
#define VMU_BYTES_PER_BLOCK     512
#define VMU_WORDS_PER_BLOCK     (VMU_BYTES_PER_BLOCK / 4)
#define VMU_NUM_BLOCKS          256
#define VMU_MEMORY_SIZE         (VMU_NUM_BLOCKS * VMU_BYTES_PER_BLOCK)  // 128KB
#define VMU_WRITES_PER_BLOCK    4
#define VMU_BYTES_PER_WRITE     (VMU_BYTES_PER_BLOCK / VMU_WRITES_PER_BLOCK)  // 128 bytes
#define VMU_WORDS_PER_WRITE     (VMU_BYTES_PER_WRITE / 4)

// VMU filesystem layout
#define VMU_SYSTEM_BLOCK_NO     255
#define VMU_NUM_SYSTEM_BLOCKS   1
#define VMU_FAT_BLOCK_NO        254
#define VMU_NUM_FAT_BLOCKS      1
#define VMU_FILE_INFO_BLOCK_NO  253
#define VMU_NUM_FILE_INFO       13
#define VMU_SAVE_AREA_BLOCK_NO  31
#define VMU_NUM_SAVE_BLOCKS     200

// VMU flash location: 128KB before the config area (0x101F8000)
// Config is at flash offset 0x001F8000 (XIP address 0x101F8000)
// VMU goes at flash offset 0x001D8000 (XIP address 0x101D8000)
#define VMU_FLASH_OFFSET        0x001D8000

// Media info word offset within system block (in 32-bit words)
#define VMU_MEDIA_INFO_OFFSET   16

// Format version byte stored at system block byte offset 17.
// Incremented when filesystem layout or byte order changes, forcing re-format.
#define VMU_FORMAT_VERSION      10

class DreamcastVMU {
public:
    DreamcastVMU();

    // Initialize VMU — format flash if blank
    void init();

    // Handle a Maple Bus command addressed to the VMU sub-peripheral (0x01).
    // packet points to the full decoded packet (header + payload) in WIRE order.
    // rxLen is the total length in bytes (excluding CRC, already stripped by pollReceive).
    // Returns true if a response was sent.
    bool handleCommand(const uint8_t* packet, uint rxLen, uint8_t port, MapleBus& bus);

    // Debug counters
    uint32_t debugVmuRxCount = 0;
    uint32_t debugVmuTxCount = 0;
    uint32_t debugVmuWriteCount = 0;
    int8_t   debugVmuLastCmd = -1;     // Last command received by VMU handler
    uint32_t debugVmuLastError = 0;    // Last FILE_ERROR code sent (0 = none)
    uint32_t debugVmuLastLocWord = 0;  // Raw location word from last BLOCK_READ/WRITE (wire order)
    uint32_t debugVmuLastFuncCode = 0; // Raw funcCode from last command with payload (wire order)
    uint32_t debugVmuRawPayload0 = 0;  // First payload word (raw wire order)
    uint32_t debugVmuReadOkCount = 0;  // Successful BLOCK_READ responses sent
    uint32_t debugVmuReadErrCount = 0; // BLOCK_READ FILE_ERROR responses sent
    uint16_t debugVmuLastBlockOk = 0xFFFF;  // Last block number that was read successfully
    uint16_t debugVmuLastBlockErr = 0xFFFF; // Last block number that failed

    // Command log ring buffer
    VmuLogEntry cmdLog[VMU_LOG_MAX_ENTRIES];
    uint16_t cmdLogWriteIdx = 0;   // Next write position
    uint16_t cmdLogCount = 0;      // Total entries written (saturates at 0xFFFF)
    void logCommand(uint8_t cmd, uint8_t response, uint32_t funcCode,
                    uint32_t locWord, uint16_t blockNum, uint8_t phase,
                    uint8_t payloadWords, const uint8_t* rawPayload = nullptr,
                    uint rawPayloadLen = 0);

private:
    // Pre-built packet buffers (wire order, uint32_t arrays)
    uint32_t vmuInfoBuf[31];    // bitpairs + header + 28 device info words + CRC
    uint32_t vmuAckBuf[3];      // bitpairs + header + CRC

    // Write buffer — accumulates one block across 4 write phases
    uint8_t writeBuffer[VMU_BYTES_PER_BLOCK];

    // Flash write target block number (set before calling doFlashWrite)
    uint16_t flashWriteBlockNum;

    // Write phase tracking — validates all 4 phases target the same block
    uint16_t currentWriteBlock;   // Block number being accumulated (0xFFFF = none)
    uint8_t  writePhaseMask;      // Bitmask of received phases (0x0F = all 4)

    // Sector buffer for read-modify-write of 4KB flash sectors
    uint8_t sectorBuffer[FLASH_SECTOR_SIZE] __attribute__((aligned(4)));

    void buildVMUInfoPacket();
    void buildVMUAckPacket();

    // Read a block from XIP flash (returns pointer to memory-mapped flash)
    const uint8_t* readBlock(uint16_t blockNum);

    // Perform flash erase+program for the block in writeBuffer
    void doFlashWrite();

    // Format empty VMU with filesystem structures (wire-order flash)
    void format();

    // Check if VMU flash area is blank/corrupted (needs full format)
    bool needsFormat();

    // Check if signature is valid but version byte is stale
    bool needsVersionUpdate();

    // Patch just the version byte in-place (single sector write)
    void patchVersionByte();

    // Build media info words (6 words) for GET_MEDIA_INFO response
    void buildMediaInfoForWire(uint32_t* out);

    // Build media info words (6 words) in wire order for system block flash storage
    static void buildMediaInfoForFlash(uint32_t* out);

    // Response senders
    void sendInfoResponse(uint8_t port, MapleBus& bus);
    void sendMemoryInfoResponse(uint8_t port, MapleBus& bus);
    void sendBlockReadResponse(uint16_t blockNum, uint32_t locWord, uint8_t port, MapleBus& bus);
    void sendAckResponse(uint8_t port, MapleBus& bus);
    void sendFileErrorResponse(uint32_t errorCode, uint8_t port, MapleBus& bus);
};
