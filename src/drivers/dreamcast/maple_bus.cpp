#include "drivers/dreamcast/maple_bus.h"
#include "maple.pio.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "pico/time.h"

MapleBus::MapleBus()
    : txPio(pio0), rxPio(pio1), txSm(0), txSmOffset(0), txDmaChannel(0), rxDmaChannel(0),
      pinA(0), pinB(0), initialized(false), rxDmaReadPos(0),
      rxState(MAPLE_RX_IDLE), syncCount(0), bitCount(0),
      currentByte(0), xorCheck(0), writePos(0), packetComplete(false), fifoReadCount(0),
      lastSampleTimeUs(0) {
    memset(rxDecodedBuf, 0, sizeof(rxDecodedBuf));
    memset(rxDmaBuf, 0, sizeof(rxDmaBuf));
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
    txSmOffset = offset;
    // TX clkdiv for 480ns/bit (Maple Bus spec, ~2 Mbps).
    // maple_tx data loop = 27 PIO cycles per bit.
    // clkdiv = target_ns_per_bit * sys_freq_MHz / (cycles_per_bit * 1000)
    uint32_t sysHz = clock_get_hz(clk_sys);
    float txClkDiv = (480.0f * (sysHz / 1e6f)) / (27.0f * 1000.0f);
    maple_tx_program_init(txPio, txSm, offset, pinA, pinB, txClkDiv);

    // TX DMA channel
    txDmaChannel = dma_claim_unused_channel(true);
    dma_channel_config txConfig = dma_channel_get_default_config(txDmaChannel);
    channel_config_set_read_increment(&txConfig, true);
    channel_config_set_write_increment(&txConfig, false);
    channel_config_set_transfer_data_size(&txConfig, DMA_SIZE_32);
    channel_config_set_dreq(&txConfig, pio_get_dreq(txPio, txSm, true));
    // DMA byte swap: reverses bytes in each 32-bit word during transfer.
    // This converts LE struct layout to BE wire order for PIO MSB-first TX.
    // Replaces the CPU bswap32 loop in sendPacket() and correctly handles
    // mixed-size fields (e.g., two uint16_t in one word).
    channel_config_set_bswap(&txConfig, true);
    dma_channel_configure(txDmaChannel, &txConfig,
                          &txPio->txf[txSm], // Destination: PIO TX FIFO
                          NULL,               // Source: set at send time
                          0,                  // Count: set at send time
                          false);             // Don't start yet

    gpio_pull_up(pinA);
    gpio_pull_up(pinB);

    // RX: Use PIO1, 3 state machines
    // clkdiv=1.0 for maximum sampling speed (full 125 MHz).
    // At clkdiv=3.0, SM0 responds to IRQ7 ~72ns late, risking missed transitions.
    // Reference: "Sample as fast as possible" at clkdiv=1.0.
    rxPio = pio1;
    rxSmOffsets[0] = pio_add_program(rxPio, &maple_rx_triple1_program);
    rxSmOffsets[1] = pio_add_program(rxPio, &maple_rx_triple2_program);
    rxSmOffsets[2] = pio_add_program(rxPio, &maple_rx_triple3_program);
    maple_rx_triple_program_init(rxPio, rxSmOffsets, pinA, pinB, 1.0f);

    // RX DMA: continuously drain SM0's RX FIFO into a ring buffer.
    // This prevents FIFO overflow when the main loop has latency spikes
    // (display updates, USB processing, etc.). Without DMA, a 9-byte
    // Maple command (~154 samples) can overflow the 128-sample FIFO.
    rxDmaChannel = dma_claim_unused_channel(true);
    dma_channel_config rxDmaConfig = dma_channel_get_default_config(rxDmaChannel);
    channel_config_set_read_increment(&rxDmaConfig, false);   // Fixed read: PIO FIFO
    channel_config_set_write_increment(&rxDmaConfig, true);    // Incrementing write: RAM buffer
    channel_config_set_transfer_data_size(&rxDmaConfig, DMA_SIZE_32);
    channel_config_set_dreq(&rxDmaConfig, pio_get_dreq(rxPio, 0, false));  // DREQ from SM0 RX
    channel_config_set_ring(&rxDmaConfig, true, MAPLE_RX_DMA_RING_BITS);   // Write address wraps
    channel_config_set_chain_to(&rxDmaConfig, rxDmaChannel);              // Self-chain for infinite operation
    dma_channel_configure(rxDmaChannel, &rxDmaConfig,
                          rxDmaBuf,          // Dest: ring buffer in RAM
                          &rxPio->rxf[0],    // Source: PIO1 SM0 RX FIFO
                          0xFFFFFFFF,         // Transfer count: run ~indefinitely
                          false);             // Don't start yet — SMs not enabled

    rxDmaReadPos = 0;

    // Enable RX state machines, then start RX DMA
    pio_sm_set_enabled(rxPio, 1, true);
    pio_sm_set_enabled(rxPio, 2, true);
    pio_sm_set_enabled(rxPio, 0, true);
    dma_channel_start(rxDmaChannel);

    initialized = true;
    return true;
}

void MapleBus::sendPacket(const uint32_t* words, uint numWords) {
    // DMA byte swap is enabled on the TX channel (channel_config_set_bswap),
    // so every 32-bit word gets its bytes reversed during DMA transfer.
    // This converts LE struct layout to BE wire order for PIO MSB-first TX.
    //
    // Word 0 (bitPairsMinus1) is a PIO loop counter consumed by `out x,32`,
    // NOT wire data. DMA bswap would reverse it incorrectly, so we pre-reverse
    // it here so the double-reversal cancels out and PIO gets the correct value.
    static uint32_t txBuf[MAPLE_TX_BUF_SIZE];
    uint count = (numWords <= MAPLE_TX_BUF_SIZE) ? numWords : MAPLE_TX_BUF_SIZE;
    txBuf[0] = __builtin_bswap32(words[0]); // Pre-reverse: DMA bswap will undo this
    memcpy(&txBuf[1], &words[1], (count - 1) * sizeof(uint32_t));

    // Disable RX state machines before transmitting to prevent echo capture.
    // The RX PIO shares the same bus pins and would decode our own TX as
    // incoming data, creating false packets that confuse the protocol.
    pio_sm_set_enabled(rxPio, 0, false);
    pio_sm_set_enabled(rxPio, 1, false);
    pio_sm_set_enabled(rxPio, 2, false);

    // Bus line check: verify both pins are HIGH (idle) for 10µs before driving.
    // If the DC hasn't finished its end-of-packet tail, we'd cause bus contention.
    // Outer 1ms timeout prevents infinite loop if bus is stuck low (hardware fault).
    {
        uint64_t startUs = time_us_64();
        uint64_t outerDeadline = startUs + 1000; // 1ms max wait
        bool busIdle = false;
        while (time_us_64() < outerDeadline) {
            if (!gpio_get(pinA) || !gpio_get(pinB)) {
                startUs = time_us_64();  // Reset 10µs window
            } else if ((time_us_64() - startUs) >= 10) {
                busIdle = true;
                break;
            }
        }
        if (!busIdle) {
            debugBusStuckCount++;
            // Re-enable RX SMs since we're aborting TX
            pio_sm_set_enabled(rxPio, 1, true);
            pio_sm_set_enabled(rxPio, 2, true);
            pio_sm_set_enabled(rxPio, 0, true);
            return;
        }
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
    // Wait for TX DMA to finish feeding the PIO TX FIFO (5ms timeout).
    // Longest packet: VMU block read = 130 words = 520 bytes = ~2ms at 480ns/bit.
    uint64_t deadline = time_us_64() + 5000;
    while (dma_channel_is_busy(txDmaChannel)) {
        if (time_us_64() >= deadline) {
            dma_channel_abort(txDmaChannel);
            debugTxTimeout++;
            break;
        }
        tight_loop_contents();
    }

    // Clear TXSTALL sticky bit, then wait for PIO to finish ALL data + tail sequence.
    // TXSTALL gets set when the TX SM stalls at `pull` with empty FIFO — meaning
    // all bit pairs have been clocked out AND the tail sequence is complete.
    txPio->fdebug = (1u << (PIO_FDEBUG_TXSTALL_LSB + txSm));
    deadline = time_us_64() + 5000;
    while (!(txPio->fdebug & (1u << (PIO_FDEBUG_TXSTALL_LSB + txSm)))) {
        if (time_us_64() >= deadline) {
            // TX SM is stuck — force-restart it to recover
            pio_sm_set_enabled(txPio, txSm, false);
            pio_sm_clear_fifos(txPio, txSm);
            pio_sm_restart(txPio, txSm);
            pio_sm_exec(txPio, txSm, pio_encode_jmp(txSmOffset));
            pio_sm_set_enabled(txPio, txSm, true);
            debugTxTimeout++;
            break;
        }
        tight_loop_contents();
    }

    // TX is fully complete (or recovered from timeout).
    // RX SMs have been disabled since sendPacket() — no echo was captured.

    // Reset decoder state machine
    rxState = MAPLE_RX_IDLE;
    writePos = 0;
    bitCount = 0;
    currentByte = 0;
    xorCheck = 0;
    packetComplete = false;

    // Skip any stale data in the RX DMA ring buffer.
    // (There shouldn't be any since RX SMs were disabled, but be safe.)
    uint dmaWriteIdx = ((uintptr_t)dma_hw->ch[rxDmaChannel].write_addr
                        - (uintptr_t)rxDmaBuf) / sizeof(uint32_t);
    rxDmaReadPos = dmaWriteIdx % MAPLE_RX_DMA_BUF_WORDS;

    // Properly restart all 3 RX state machines (not just re-enable).
    // When a PIO SM is disabled and re-enabled, it resumes from the exact
    // instruction where it was frozen. SM1/SM2 (edge watchers) may be frozen
    // at `irq 7` which fires immediately on re-enable, injecting spurious
    // samples. SM0 may be at `in pins, 2`, sampling garbage.
    // Reference: DreamPicoPort calls prestart() on every SM transition:
    //   pio_sm_clear_fifos, pio_sm_restart, pio_sm_clkdiv_restart, jmp to start
    for (int sm = 0; sm < 3; sm++) {
        pio_sm_clear_fifos(rxPio, sm);
        pio_sm_restart(rxPio, sm);
        pio_sm_clkdiv_restart(rxPio, sm);
        pio_sm_exec(rxPio, sm, pio_encode_jmp(rxSmOffsets[sm]));
    }
    // Clear IRQ 7 flag to prevent stale edge triggers
    pio_interrupt_clear(rxPio, 7);

    // Re-enable all RX state machines.
    // Enable edge watchers (SM1, SM2) first, then sampler (SM0) — ensures
    // SM0 doesn't sample before edge watchers are ready.
    pio_sm_set_enabled(rxPio, 1, true);
    pio_sm_set_enabled(rxPio, 2, true);
    pio_sm_set_enabled(rxPio, 0, true);
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
    // Read from the RX DMA ring buffer (DMA continuously drains PIO FIFO → RAM).
    // This prevents FIFO overflow even when the main loop is slow.
    uint dmaWriteIdx = ((uintptr_t)dma_hw->ch[rxDmaChannel].write_addr
                        - (uintptr_t)rxDmaBuf) / sizeof(uint32_t);
    dmaWriteIdx %= MAPLE_RX_DMA_BUF_WORDS;

    bool hadData = (rxDmaReadPos != dmaWriteIdx);
    if (hadData) lastSampleTimeUs = time_us_64();

    while (rxDmaReadPos != dmaWriteIdx) {
        uint32_t fifoWord = rxDmaBuf[rxDmaReadPos];
        rxDmaReadPos = (rxDmaReadPos + 1) % MAPLE_RX_DMA_BUF_WORDS;
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

    // DMA buffer drained — with 32-bit autopush, up to 15 samples can be stuck in ISR.
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

    // Stale-data timeout: if decoder is mid-packet but no new samples arrived
    // for >2ms, the packet was truncated (noise, partial TX). Reset to IDLE.
    if (!hadData && rxState != MAPLE_RX_IDLE && lastSampleTimeUs > 0) {
        if ((time_us_64() - lastSampleTimeUs) > 2000) {
            rxState = MAPLE_RX_IDLE;
            writePos = 0;
            bitCount = 0;
            currentByte = 0;
            xorCheck = 0;
        }
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
