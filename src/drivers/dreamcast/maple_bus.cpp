#include "drivers/dreamcast/maple_bus.h"
#include "maple.pio.h"
#include "hardware/gpio.h"
#include "pico/time.h"

MapleBus::MapleBus()
    : txPio(pio0), rxPio(pio1), txSm(0), txDmaChannel(0),
      pinA(0), pinB(0), initialized(false),
      rxState(MAPLE_RX_IDLE), syncCount(0), bitCount(0),
      currentByte(0), xorCheck(0), writePos(0), packetComplete(false), fifoReadCount(0) {
    memset(rxDecodedBuf, 0, sizeof(rxDecodedBuf));
}

bool MapleBus::init(uint pin_a, uint pin_b) {
    if (initialized) return true;
    if (pin_b != pin_a + 1) return false; // Pins must be consecutive

    pinA = pin_a;
    pinB = pin_b;

    // TX: Use PIO0, claim a state machine
    // SM is enabled by init and stalls at `pull` (both pins HIGH = idle)
    txPio = pio0;
    txSm = pio_claim_unused_sm(txPio, true);
    uint offset = pio_add_program(txPio, &maple_tx_program);
    maple_tx_program_init(txPio, txSm, offset, pinA, pinB, 3.0f);

    // TX DMA channel
    txDmaChannel = dma_claim_unused_channel(true);
    dma_channel_config txConfig = dma_channel_get_default_config(txDmaChannel);
    channel_config_set_read_increment(&txConfig, true);
    channel_config_set_write_increment(&txConfig, false);
    channel_config_set_transfer_data_size(&txConfig, DMA_SIZE_32);
    channel_config_set_dreq(&txConfig, pio_get_dreq(txPio, txSm, true));
    dma_channel_configure(txDmaChannel, &txConfig,
                          &txPio->txf[txSm], // Destination: PIO TX FIFO
                          NULL,               // Source: set at send time
                          0,                  // Count: set at send time
                          false);             // Don't start yet

    gpio_pull_up(pinA);
    gpio_pull_up(pinB);

    // RX: Use PIO1, 3 state machines
    rxPio = pio1;
    uint rxOffsets[3] = {
        pio_add_program(rxPio, &maple_rx_triple1_program),
        pio_add_program(rxPio, &maple_rx_triple2_program),
        pio_add_program(rxPio, &maple_rx_triple3_program)
    };
    maple_rx_triple_program_init(rxPio, rxOffsets, pinA, pinB, 3.0f);

    // Enable RX state machines
    pio_sm_set_enabled(rxPio, 1, true);
    pio_sm_set_enabled(rxPio, 2, true);
    pio_sm_set_enabled(rxPio, 0, true);

    initialized = true;
    return true;
}

void MapleBus::sendPacket(const uint32_t* words, uint numWords) {
    // Byte-swap each DATA word for PIO MSB-first transmission.
    // Structs use wire byte order (offset 0 = first byte on wire).
    // PIO sends bit 31 (MSB) first. On LE ARM, MSB = offset 3.
    // bswap32 reverses bytes so offset 0 → MSB → first on wire.
    //
    // Word 0 (bitPairsMinus1) is NOT swapped — it's a PIO loop counter value,
    // consumed by `out x,32`, not transmitted as wire data.
    static uint32_t txBuf[64];
    uint count = (numWords <= 64) ? numWords : 64;
    txBuf[0] = words[0]; // bitPairsMinus1: value for PIO, not byte data
    for (uint i = 1; i < count; i++) {
        txBuf[i] = __builtin_bswap32(words[i]);
    }

    // TX SM is stalled at `pull` with both pins HIGH (idle state).
    // Just fire DMA — SM will auto-start when data arrives in TX FIFO.
    dma_channel_set_read_addr(txDmaChannel, txBuf, false);
    dma_channel_set_trans_count(txDmaChannel, count, true);
}

bool MapleBus::isTransmitting() {
    return dma_channel_is_busy(txDmaChannel);
}

void MapleBus::flushRx() {
    // Wait for DMA to finish feeding the PIO TX FIFO
    while (dma_channel_is_busy(txDmaChannel)) { tight_loop_contents(); }
    // Wait for PIO TX FIFO to drain (PIO consuming remaining words)
    while (!pio_sm_is_tx_fifo_empty(txPio, txSm)) { tight_loop_contents(); }
    // Wait for PIO to finish clocking out last bits + tail sequence
    busy_wait_us(100);

    // SM returns to `pull` stall with both pins HIGH after tail sequence.
    // No need to stop/restart SM.

    // Drain and discard all RX FIFO data (echo from our TX)
    debugFlushCount = 0;
    while (!pio_sm_is_rx_fifo_empty(rxPio, 0)) {
        (void)pio_sm_get(rxPio, 0);
        debugFlushCount++;
    }
    // Reset decoder state machine
    rxState = MAPLE_RX_IDLE;
    writePos = 0;
    bitCount = 0;
    currentByte = 0;
    xorCheck = 0;
    packetComplete = false;
}

// ============================================================
// RX Decoder
//
// The PIO RX triple state machines work together:
// - SM1 watches pinA edges, SM2 watches pinB edges → both fire IRQ 7
// - SM0 samples both pins (2 bits) on each IRQ 7, autopushes at 32 bits
// - Each FIFO word = 4 bytes = 16 x 2-bit pin samples, packed MSB-first
// - FIFO is joined to 8 entries deep (128 samples capacity)
//
// Maple Bus wire protocol (mc.pp.se/dc/maplewire.html):
//   Sync: 0b10, then 4x(0b00,0b10), then 0b11
//   Data: alternating phase A (pinA=1→both=bit) and phase B (pinB=1→both=bit)
//   Each byte is 8 bits MSB first, each bit = 2 transitions (clock + data)
//   End: detected when expected clock transition doesn't match data pattern
//   CRC: XOR of all bytes; XOR including CRC byte == 0 means valid
// ============================================================

void MapleBus::decodeSample(uint8_t pins) {
    // pins: bit0 = pinA, bit1 = pinB
    switch (rxState) {
        case MAPLE_RX_IDLE:
            // Wait for 0b10 (pinA low, pinB high) — start of sync
            if (pins == 0b10) {
                syncCount = 0;
                rxState = MAPLE_RX_SYNC_WAIT_LOW;
            }
            break;

        case MAPLE_RX_SYNC_WAIT_LOW:
            // Expect 0b00 (both low) for sync pulse
            if (pins == 0b00) {
                syncCount++;
                rxState = MAPLE_RX_SYNC_WAIT_HIGH;
            } else if (pins == 0b11 && syncCount >= 4) {
                // End of sync — start data phase
                debugSyncCount++;
                rxState = MAPLE_RX_DATA_CLOCK_A;
                bitCount = 0;
                currentByte = 0;
                xorCheck = 0;
                writePos = 0;
                packetComplete = false;
            } else {
                rxState = MAPLE_RX_IDLE;
            }
            break;

        case MAPLE_RX_SYNC_WAIT_HIGH:
            // Expect 0b10 (pinB high, pinA low) — sync pulse return
            if (pins == 0b10) {
                rxState = MAPLE_RX_SYNC_WAIT_LOW;
            } else if (pins == 0b11 && syncCount >= 4) {
                // End of sync early
                debugSyncCount++;
                rxState = MAPLE_RX_DATA_CLOCK_A;
                bitCount = 0;
                currentByte = 0;
                xorCheck = 0;
                writePos = 0;
                packetComplete = false;
            } else {
                rxState = MAPLE_RX_IDLE;
            }
            break;

        case MAPLE_RX_DATA_CLOCK_A:
            // Expect 0b01 (pinA high, pinB low) — phase A clock
            if (pins == 0b01) {
                rxState = MAPLE_RX_DATA_BIT_A;
            } else {
                // Not a clock edge — might be end of packet
                if (writePos > debugMaxWritePos) debugMaxWritePos = writePos;
                debugEndCount++;
                debugLastXor = xorCheck;
                debugLastBitCount = bitCount;
                if (writePos >= 5 && xorCheck == 0) {
                    packetComplete = true;
                } else if (writePos > 0) {
                    debugXorFail++;
                }
                rxState = MAPLE_RX_IDLE;
            }
            break;

        case MAPLE_RX_DATA_BIT_A:
            // Read data bit: 0b11 = 1, 0b00 = 0
            if (pins == 0b11) {
                currentByte |= (1 << (7 - bitCount));
                bitCount++;
            } else if (pins == 0b00) {
                bitCount++;
            } else {
                rxState = MAPLE_RX_IDLE;
                break;
            }

            if (bitCount >= 8) {
                if (writePos < MAPLE_RX_BUF_SIZE) {
                    rxDecodedBuf[writePos++] = currentByte;
                    xorCheck ^= currentByte;
                }
                currentByte = 0;
                bitCount = 0;
                rxState = MAPLE_RX_DATA_CLOCK_A;
            } else {
                rxState = MAPLE_RX_DATA_CLOCK_B;
            }
            break;

        case MAPLE_RX_DATA_CLOCK_B:
            // Expect 0b10 (pinB high, pinA low) — phase B clock
            if (pins == 0b10) {
                rxState = MAPLE_RX_DATA_BIT_B;
            } else {
                if (writePos > debugMaxWritePos) debugMaxWritePos = writePos;
                debugEndCount++;
                debugLastXor = xorCheck;
                debugLastBitCount = bitCount;
                if (writePos >= 5 && xorCheck == 0) {
                    packetComplete = true;
                } else if (writePos > 0) {
                    debugXorFail++;
                }
                rxState = MAPLE_RX_IDLE;
            }
            break;

        case MAPLE_RX_DATA_BIT_B:
            // Read data bit: 0b11 = 1, 0b00 = 0
            if (pins == 0b11) {
                currentByte |= (1 << (7 - bitCount));
                bitCount++;
            } else if (pins == 0b00) {
                bitCount++;
            } else {
                rxState = MAPLE_RX_IDLE;
                break;
            }

            if (bitCount >= 8) {
                if (writePos < MAPLE_RX_BUF_SIZE) {
                    rxDecodedBuf[writePos++] = currentByte;
                    xorCheck ^= currentByte;
                }
                currentByte = 0;
                bitCount = 0;
                rxState = MAPLE_RX_DATA_CLOCK_A;
            } else {
                rxState = MAPLE_RX_DATA_CLOCK_A;
            }
            break;
    }
}

bool MapleBus::pollReceive(const uint8_t** outPacket, uint* outLength) {
    // Drain all available data from PIO RX FIFO and decode
    while (!pio_sm_is_rx_fifo_empty(rxPio, 0)) {
        uint32_t fifoWord = pio_sm_get(rxPio, 0);
        fifoReadCount++;

        // Each 32-bit FIFO word contains 4 packed bytes (autopush at 32 bits)
        // Each byte contains 4 x 2-bit pin samples (shift-left, MSB first)
        for (int byteIdx = 3; byteIdx >= 0; byteIdx--) {
            uint8_t fifoByte = (fifoWord >> (byteIdx * 8)) & 0xFF;

            for (int sampleIdx = 3; sampleIdx >= 0; sampleIdx--) {
                uint8_t pins = (fifoByte >> (sampleIdx * 2)) & 0x03;
                decodeSample(pins);

                if (packetComplete) {
                    *outPacket = rxDecodedBuf;
                    *outLength = (writePos > 0) ? writePos - 1 : 0;
                    packetComplete = false;
                    debugPollTrue++;
                    return true;
                }
            }
        }
    }

    // FIFO empty — with 32-bit autopush, up to 15 samples can be stuck in ISR.
    // If we have a valid packet (enough bytes + XOR passes), accept it.
    if (writePos >= 5 && xorCheck == 0) {
        *outPacket = rxDecodedBuf;
        *outLength = writePos - 1; // exclude CRC byte
        rxState = MAPLE_RX_IDLE;
        writePos = 0;
        bitCount = 0;
        currentByte = 0;
        xorCheck = 0;
        debugPollTrue++;
        return true;
    }

    *outPacket = nullptr;
    *outLength = 0;
    return false;
}

uint32_t MapleBus::calcCRC(const uint32_t* words, uint numWords) {
    // Maple Bus CRC: XOR all 32-bit words, then fold all 4 bytes to 8-bit CRC.
    // Result stored at bits 7-0 (LSByte in struct). sendPacket's bswap32 moves it
    // to bits 31-24 (MSByte), which PIO sends first on wire — matching protocol spec.
    // Reference: DreamPicoPort crc8(), dreamcast.wiki/Maple_bus
    uint32_t lrc = 0;
    for (uint i = 0; i < numWords; i++) {
        lrc ^= words[i];
    }
    // Fold 32-bit XOR to 8-bit CRC byte
    uint8_t crc = (lrc >> 24) ^ (lrc >> 16) ^ (lrc >> 8) ^ lrc;
    return (uint32_t)crc;
}

uint32_t MapleBus::calcBitPairs(uint packetSize) {
    // BitPairsMinus1: PIO data loop consumes 2 bits per iteration.
    // Wire data = header (4 bytes) + payload + CRC (1 byte).
    // packetSize includes bitPairsMinus1 (4 bytes) + header + payload + CRC word (4 bytes).
    // But CRC is only 1 byte on wire, not 4. So wire bytes = packetSize - 4 (prefix) - 3 (unused CRC padding).
    // Bit pairs = (packetSize - 7) * 4. Minus 1 for PIO loop counter.
    // Reference: DreamPicoPort formula ((numPayloadWords+1)*4+1)*8 produces same results.
    return (packetSize - 7) * 4 - 1;
}
