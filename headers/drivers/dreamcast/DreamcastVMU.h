#pragma once

#include <stdint.h>
#include <string.h>
#include "drivers/dreamcast/maple_bus.h"
#include "hardware/flash.h"

class Gamepad;

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
#define VMU_SAVE_AREA_BLOCK_NO  0
#define VMU_NUM_SAVE_BLOCKS     200

// VMU flash location: 128KB before the config area (0x101F8000)
// Config is at flash offset 0x001F8000 (XIP address 0x101F8000)
// VMU goes at flash offset 0x001D8000 (XIP address 0x101D8000)
#define VMU_FLASH_OFFSET        0x001D8000

// Media info word offset within system block (in 32-bit words)
#define VMU_MEDIA_INFO_OFFSET   16

class DreamcastVMU {
public:
    DreamcastVMU();

    // Initialize VMU — format flash if blank
    void init();

    // Handle a Maple Bus command addressed to the VMU sub-peripheral (0x01).
    // packet points to the full decoded packet (header + payload).
    // rxLen is the total length in bytes (excluding CRC, already stripped by pollReceive).
    // Returns true if a response was sent.
    bool handleCommand(const uint8_t* packet, uint rxLen, uint8_t port, MapleBus& bus);

    // Perform deferred flash write if pending. Call from main loop.
    void processFlashWrite();

    // Debug counters
    uint32_t debugVmuRxCount = 0;
    uint32_t debugVmuTxCount = 0;
    uint32_t debugVmuWriteCount = 0;

private:
    // Pre-built info packet for DEVICE_REQUEST responses
    MapleInfoPacket vmuInfoPacket;

    // Pre-built ACK packet
    MapleACKPacket vmuAckPacket;

    // Write buffer — accumulates one block across 4 write phases
    uint8_t writeBuffer[VMU_BYTES_PER_BLOCK];

    // Flash write state
    bool flashWritePending;
    uint16_t flashWriteBlockNum;

    // Sector buffer for read-modify-write of 4KB flash sectors
    uint8_t sectorBuffer[FLASH_SECTOR_SIZE] __attribute__((aligned(4)));

    void buildVMUInfoPacket();
    void buildVMUAckPacket();

    // Read a block from XIP flash (returns pointer to memory-mapped flash)
    const uint8_t* readBlock(uint16_t blockNum);

    // Write a block to flash (deferred via processFlashWrite)
    void scheduleBlockWrite(uint16_t blockNum);

    // Actually perform the flash erase+program
    void doFlashWrite();

    // Format empty VMU with filesystem structures
    void format();

    // Check if VMU flash area is blank (all 0xFF)
    bool needsFormat();

    // Build media info words (6 words) in native byte order
    void buildMediaInfo(uint32_t* out);

    // Response senders
    void sendInfoResponse(uint8_t port, MapleBus& bus);
    void sendMemoryInfoResponse(uint8_t port, MapleBus& bus);
    void sendBlockReadResponse(uint16_t blockNum, uint8_t port, MapleBus& bus);
    void sendAckResponse(uint8_t port, MapleBus& bus);
    void sendFileErrorResponse(uint32_t errorCode, uint8_t port, MapleBus& bus);
};
