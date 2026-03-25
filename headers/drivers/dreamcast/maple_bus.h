#pragma once

// Maple Bus transport layer for RP2040 Dreamcast controller/VMU emulation.
// Architecture based on MaplePad by mackieks (github.com/mackieks/MaplePad)
// and DreamPicoPort. All packet data is stored in WIRE (network) byte order —
// NO DMA bswap. Field extraction uses explicit __builtin_bswap32().

#include <stdint.h>
#include <string.h>
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"

// Forward declaration for callback type
class MapleBus;
typedef void (*MapleFastPathCallback)(uint32_t hdr, MapleBus* bus);

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

// Maple Bus function codes (logical/host values)
enum MapleFunction {
    MAPLE_FUNC_CONTROLLER  = 1,
    MAPLE_FUNC_MEMORY_CARD = 2,
    MAPLE_FUNC_LCD         = 4,
    MAPLE_FUNC_TIMER       = 8,
    MAPLE_FUNC_VIBRATION   = 256,
};

// ============================================================
// Wire-order header accessors (NO DMA bswap architecture)
//
// PIO maple_rx shifts bits LEFT, autopush at 32 bits.
// Maple Bus sends LSByte first. First byte received → bits [31:24].
// Without DMA bswap, RAM uint32_t = PIO FIFO value:
//   bits [31:24] = numWords  (first byte on wire)
//   bits [23:16] = origin    (sender address)
//   bits [15:8]  = destination (recipient address)
//   bits [7:0]   = command   (last byte on wire)
// ============================================================

static inline uint8_t maple_hdr_numWords(uint32_t w) { return (w >> 24) & 0xFF; }
static inline uint8_t maple_hdr_origin(uint32_t w)   { return (w >> 16) & 0xFF; }
static inline uint8_t maple_hdr_dest(uint32_t w)     { return (w >> 8) & 0xFF; }
static inline int8_t  maple_hdr_command(uint32_t w)   { return (int8_t)(w & 0xFF); }

static inline uint32_t maple_make_header(uint8_t nw, uint8_t orig, uint8_t dest, int8_t cmd) {
    return ((uint32_t)nw << 24) | ((uint32_t)orig << 16)
         | ((uint32_t)(uint8_t)dest << 8) | (uint8_t)cmd;
}

// Convert a logical (host-order) 32-bit value to wire order for TX payload
static inline uint32_t maple_host_to_wire(uint32_t v) { return __builtin_bswap32(v); }

// Convert a wire-order 32-bit value from RX payload to logical (host) order
static inline uint32_t maple_wire_to_host(uint32_t v) { return __builtin_bswap32(v); }

// TX buffer size (must handle VMU block read: bitPairs + header + func + loc + 128 data words + CRC)
#define MAPLE_TX_BUF_SIZE  140

// RX DMA buffer size in 32-bit words.
// Must handle largest RX packet: VMU block write = header(1) + func(1) + loc(1) + data(32) + CRC(1) = 36 words.
// Add margin for the `in null 24` padding at end-of-packet.
// Not a ring buffer — linear, reset per packet.
#define MAPLE_RX_DMA_BUF_WORDS  64

// Maple Bus transport layer
class MapleBus {
public:
    MapleBus();

    bool init(uint pin_a, uint pin_b);
    void sendPacket(const uint32_t* words, uint numWords);
    bool isTransmitting();

    // Poll for received packets. Call frequently from main loop.
    // Returns true if a complete valid packet is ready.
    // On true, outPacket/outLength point to the decoded packet data (excluding CRC).
    // All data is in wire (network) byte order.
    bool pollReceive(const uint8_t** outPacket, uint* outLength);

    // Flush RX FIFO and reset decoder (call after TX to discard echo)
    void flushRx();

    // CRC calculation (Maple Bus XOR-based).
    // Returns the CRC word ready for TX: CRC byte at bits 31:24 (wire order).
    static uint32_t calcCRC(const uint32_t* words, uint numWords);

    // Build the BitPairsMinus1 field for a packet of given byte size
    static uint32_t calcBitPairs(uint packetSize);

    PIO getRxPio() { return rxPio; }
    PIO getTxPio() { return txPio; }
    uint getRxSm() { return rxSm; }

    // Clear PIO RX state after ISR consumed the packet.
    void clearRxAfterFastPath();

    // Check if last pollReceive() detected a corrupt packet (CRC fail or numWords mismatch).
    // Caller should send REQUEST_RESEND (cmd -4) when this returns true.
    // Automatically clears the flag after reading.
    bool wasLastRxCorrupt() { bool v = lastRxWasCorrupt; lastRxWasCorrupt = false; return v; }

    // Set by ISR when a valid CMD 9 packet is received and response is pre-built.
    volatile bool cmd9PreBuilt = false;

    // Packet arrival timestamp — set when end-of-packet IRQ flag is first detected.
    // Timestamp when end-of-packet was detected (for diagnostics).
    volatile uint32_t rxArrivalTimestamp = 0;

    // Fast-path callback: called from ISR to capture GPIO and build response.
    // Must be __no_inline_not_in_flash_func. Set by DreamcastDriver::init().
    MapleFastPathCallback fastPathCallback = nullptr;

    // Enable/disable the PIO IRQ path
    void enableFastPath(MapleFastPathCallback callback);
    void disableFastPath();

    // Debug counters (only updated when enableDiagnostics == true)
    bool enableDiagnostics = false;
    uint32_t debugXorFail = 0;
    uint32_t debugPollTrue = 0;
    uint32_t debugTxTimeout = 0;
    uint32_t debugBusStuckCount = 0;
    uint32_t debugRxTimeout = 0;
    uint32_t debugEndIrqCount = 0;
    uint32_t debugNumWordsMismatch = 0;

    // RX DMA buffer — public for ISR read access.
    // maple_rx PIO produces 32 decoded data bits per FIFO word (= 4 payload bytes).
    // NO DMA bswap — data stays in wire (network) byte order as received by PIO.
    uint32_t rxDmaBuf[MAPLE_RX_DMA_BUF_WORDS] __attribute__((aligned(4)));
    uint32_t rxDmaInitCount;  // Initial DMA transfer count (for computing words received)
    uint rxDmaChannel;

private:
    PIO txPio;
    PIO rxPio;
    uint txSm;
    uint txSmOffset;  // TX program offset (needed for SM recovery on timeout)
    uint rxSm;
    uint rxSmOffset;  // RX program offset (needed for SM restart)
    uint txDmaChannel;
    uint pinA;
    uint pinB;

    bool initialized;
    bool fastPathEnabled = false;
    bool lastRxWasCorrupt = false;  // Set by pollReceive() on CRC/numWords failure

    // Validated packet buffer — data is copied here before returning to caller.
    // This prevents the DMA from overwriting the packet while the caller processes it.
    uint32_t rxPacketBuf[MAPLE_RX_DMA_BUF_WORDS] __attribute__((aligned(4)));

    // Timestamp for stale-data timeout
    uint64_t rxStartTimeUs;

    // Helper: restart RX SM and DMA for next packet
    void startRx();
};
