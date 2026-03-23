#pragma once

#include <stdint.h>
#include <string.h>
#include "hardware/pio.h"
#include "hardware/dma.h"

// Maple Bus addressing
#define MAPLE_DC_ADDR        0x00
#define MAPLE_CTRL_ADDR      0x20
#define MAPLE_SUB0_ADDR      0x01
#define MAPLE_SUB1_ADDR      0x02
#define MAPLE_PORT_MASK      0xC0
#define MAPLE_PERIPH_MASK    (~MAPLE_PORT_MASK)

// Maple Bus commands
enum MapleCommand {
    MAPLE_CMD_DEVICE_REQUEST        = 1,
    MAPLE_CMD_ALL_STATUS_REQUEST    = 2,
    MAPLE_CMD_RESET_DEVICE          = 3,
    MAPLE_CMD_SHUTDOWN_DEVICE       = 4,
    MAPLE_CMD_RESPOND_DEVICE_STATUS = 5,
    MAPLE_CMD_RESPOND_ALL_STATUS    = 6,
    MAPLE_CMD_RESPOND_ACK           = 7,
    MAPLE_CMD_RESPOND_DATA_XFER     = 8,
    MAPLE_CMD_GET_CONDITION         = 9,
    MAPLE_CMD_GET_MEDIA_INFO        = 10,
    MAPLE_CMD_BLOCK_READ            = 11,
    MAPLE_CMD_BLOCK_WRITE           = 12,
    MAPLE_CMD_BLOCK_COMPLETE_WRITE  = 13,
    MAPLE_CMD_SET_CONDITION         = 14,
    MAPLE_CMD_RESPOND_FILE_ERROR    = -5,
    MAPLE_CMD_RESPOND_SEND_AGAIN    = -4,
    MAPLE_CMD_RESPOND_UNKNOWN_CMD   = -3,
    MAPLE_CMD_RESPOND_FUNC_UNSUP    = -2,
    MAPLE_CMD_NO_RESPONSE           = -1,
};

// Maple Bus function codes
enum MapleFunction {
    MAPLE_FUNC_CONTROLLER  = 1,
    MAPLE_FUNC_MEMORY_CARD = 2,
    MAPLE_FUNC_LCD         = 4,
    MAPLE_FUNC_TIMER       = 8,
    MAPLE_FUNC_VIBRATION   = 256,
};

// Packet header (4 bytes) — field order matches wire byte order.
// Maple Bus sends MSB-first. The header word on wire is:
//   byte 0 (first): numWords   (bits 31-24 of the 32-bit word)
//   byte 1:         sender     (bits 23-16) — "origin" in our naming
//   byte 2:         recipient  (bits 15-8)  — "destination" in our naming
//   byte 3 (last):  command    (bits 7-0)
// RX decoder puts wire bytes into rxDecodedBuf[] sequentially,
// so this struct maps correctly when cast from the decoded buffer.
// TX uses bswap32 in sendPacket to convert this byte order to
// the correct bit positions for PIO MSB-first output.
typedef struct __attribute__((packed)) {
    uint8_t  numWords;
    uint8_t  origin;
    uint8_t  destination;
    int8_t   command;
} MaplePacketHeader;

// Device info payload
typedef struct __attribute__((packed)) {
    uint32_t func;           // Big endian
    uint32_t funcData[3];    // Big endian
    int8_t   areaCode;
    uint8_t  connectorDirection;
    char     productName[30];
    char     productLicense[60];
    uint16_t standbyPower;
    uint16_t maxPower;
} MapleDeviceInfo;

// Controller condition payload
// Byte order matches DreamPicoPort's controller_condition_t (HOST byte order).
// sendPacket bswap32 converts to NETWORK order for PIO MSB-first transmission.
// On wire (after bswap + PIO): offset 0 goes first, matching Maple Bus LSByte-first.
typedef struct __attribute__((packed)) {
    uint32_t condition;      // Function code (native value — sendPacket bswap handles wire order)
    uint8_t  leftTrigger;    // 0=released, 255=fully pressed
    uint8_t  rightTrigger;   // 0=released, 255=fully pressed
    uint8_t  buttonsHi;      // Buttons high byte (Z,Y,X,D,UpB,DownB,LeftB,RightB) (inverted: 0=pressed)
    uint8_t  buttonsLo;      // Buttons low byte (C,B,A,Start,Up,Down,Left,Right) (inverted: 0=pressed)
    uint8_t  joyY2;          // Right analog Y (0=up, 128=center, 255=down)
    uint8_t  joyX2;          // Right analog X (0=left, 128=center, 255=right)
    uint8_t  joyY;           // Left analog Y (0=up, 128=center, 255=down)
    uint8_t  joyX;           // Left analog X (0=left, 128=center, 255=right)
} MapleControllerCondition;

// VMU memory info payload (7 words: func code + 6 media info words)
typedef struct __attribute__((packed)) {
    uint32_t funcCode;
    uint32_t mediaInfo[6];
} MapleMemoryInfo;

// VMU block read payload (func code + location + 128 data words = 130 words)
typedef struct __attribute__((packed)) {
    uint32_t funcCode;
    uint32_t location;
    uint32_t data[128];  // 512 bytes = one VMU block
} MapleBlockReadData;

// VMU file error payload (1 word: error code)
typedef struct __attribute__((packed)) {
    uint32_t errorCode;
} MapleFileError;

// Pre-built packet structures (with BitPairsMinus1 prefix and CRC suffix)
typedef struct __attribute__((aligned(4))) {
    uint32_t           bitPairsMinus1;
    MaplePacketHeader  header;
    uint32_t           crc;
} MapleACKPacket;

typedef struct __attribute__((aligned(4))) {
    uint32_t           bitPairsMinus1;
    MaplePacketHeader  header;
    MapleDeviceInfo    info;
    uint32_t           crc;
} MapleInfoPacket;

typedef struct __attribute__((aligned(4))) {
    uint32_t                  bitPairsMinus1;
    MaplePacketHeader         header;
    MapleControllerCondition  controller;
    uint32_t                  crc;
} MapleControllerPacket;

typedef struct __attribute__((aligned(4))) {
    uint32_t          bitPairsMinus1;
    MaplePacketHeader header;
    MapleMemoryInfo   memInfo;
    uint32_t          crc;
} MapleMemoryInfoPacket;

typedef struct __attribute__((aligned(4))) {
    uint32_t           bitPairsMinus1;
    MaplePacketHeader  header;
    MapleBlockReadData blockRead;
    uint32_t           crc;
} MapleBlockReadPacket;

typedef struct __attribute__((aligned(4))) {
    uint32_t          bitPairsMinus1;
    MaplePacketHeader header;
    MapleFileError    fileError;
    uint32_t          crc;
} MapleFileErrorPacket;

// RX decoder states
enum MapleRxState {
    MAPLE_RX_IDLE,
    MAPLE_RX_SYNC_WAIT_LOW,
    MAPLE_RX_SYNC_WAIT_HIGH,
    MAPLE_RX_DATA_CLOCK_A,
    MAPLE_RX_DATA_BIT_A,
    MAPLE_RX_DATA_CLOCK_B,
    MAPLE_RX_DATA_BIT_B,
};

// RX receive buffer size (must handle VMU block write: header + func + loc + 32 data words + CRC)
#define MAPLE_RX_BUF_SIZE  256

// TX buffer size (must handle VMU block read: bitPairs + header + func + loc + 128 data words + CRC)
#define MAPLE_TX_BUF_SIZE  140

// RX DMA ring buffer size (must be power of 2).
// 256 words = 4096 samples = ~2ms of buffering at Maple Bus rate.
// Prevents FIFO overflow when main loop has latency spikes (display updates, etc.)
#define MAPLE_RX_DMA_BUF_WORDS  256
#define MAPLE_RX_DMA_BUF_BYTES  (MAPLE_RX_DMA_BUF_WORDS * 4)  // 1024
#define MAPLE_RX_DMA_RING_BITS  10  // log2(1024) = 10

// Maple Bus transport layer
class MapleBus {
public:
    MapleBus();

    bool init(uint pin_a, uint pin_b);
    void sendPacket(const uint32_t* words, uint numWords);
    bool isTransmitting();

    // Poll for received packets. Call frequently from main loop.
    // Drains PIO FIFO, decodes samples, returns true if a complete valid packet is ready.
    // On true, outPacket/outLength point to the decoded packet data (excluding CRC).
    bool pollReceive(const uint8_t** outPacket, uint* outLength);

    // Flush RX FIFO and reset decoder (call after TX to discard echo)
    void flushRx();

    // CRC calculation (Maple Bus XOR-based)
    static uint32_t calcCRC(const uint32_t* words, uint numWords);

    // Build the BitPairsMinus1 field for a packet of given byte size
    static uint32_t calcBitPairs(uint packetSize);

    PIO getRxPio() { return rxPio; }
    PIO getTxPio() { return txPio; }
    uint32_t getFifoReadCount() { return fifoReadCount; }

    // Debug: decoder state tracking
    uint32_t debugSyncCount = 0;     // Times sync detected (entered data phase)
    uint32_t debugEndCount = 0;      // Times end-of-packet detected
    uint32_t debugXorFail = 0;       // Times XOR check failed at end
    uint16_t debugMaxWritePos = 0;   // Max bytes decoded in any packet attempt
    uint8_t  debugLastXor = 0;       // Last XOR value at end attempt
    uint8_t  debugLastBitCount = 0;  // bitCount at last end-of-packet attempt
    uint32_t debugPollTrue = 0;      // Times pollReceive returned true
    uint32_t debugFlushCount = 0;    // FIFO words drained after last TX (echo proof)
    uint32_t debugTxTimeout = 0;     // Times flushRx() timed out waiting for TX completion
    uint32_t debugBusStuckCount = 0; // Times sendPacket() aborted due to bus stuck low

private:
    PIO txPio;
    PIO rxPio;
    uint txSm;
    uint txSmOffset;  // TX program offset (needed for SM recovery on timeout)
    uint txDmaChannel;
    uint rxDmaChannel;
    uint pinA;
    uint pinB;

    bool initialized;

    // RX SM program offsets (needed for proper restart after TX)
    uint rxSmOffsets[3];

    // RX DMA ring buffer — continuously drains PIO FIFO to prevent overflow.
    // Must be aligned to buffer size for DMA ring mode.
    uint32_t rxDmaBuf[MAPLE_RX_DMA_BUF_WORDS] __attribute__((aligned(MAPLE_RX_DMA_BUF_BYTES)));
    volatile uint rxDmaReadPos;  // Our read index into the ring buffer

    // RX decoder state
    MapleRxState rxState;
    int syncCount;
    int bitCount;
    uint8_t currentByte;
    uint8_t xorCheck;
    uint writePos;
    uint8_t rxDecodedBuf[MAPLE_RX_BUF_SIZE] __attribute__((aligned(4)));
    bool packetComplete;
    uint32_t fifoReadCount;
    uint64_t lastSampleTimeUs;  // Timestamp of last DMA data processed (for stale-data timeout)

    // Process one 2-bit pin sample through the decoder state machine
    void decodeSample(uint8_t pins);
};
