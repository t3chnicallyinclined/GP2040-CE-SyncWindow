#include "drivers/dreamcast/maple_bus.h"
#include "maple.pio.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "pico/time.h"

// Maple Bus transport layer — wire-order architecture (NO DMA bswap).
// Based on MaplePad by mackieks (github.com/mackieks/MaplePad).

static MapleBus* irqBusInstance = nullptr;

static void __no_inline_not_in_flash_func(mapleRxIrqHandler)() {
    MapleBus* bus = irqBusInstance;
    if (!bus || !bus->fastPathCallback) return;

    if (bus->enableDiagnostics) bus->rxArrivalTimestamp = timer_hw->timerawl;

    // Validate packet: CMD 9 is 3 words (header + funcCode + CRC)
    uint32_t remaining = dma_channel_hw_addr(bus->rxDmaChannel)->transfer_count;
    uint32_t wordsReceived = bus->rxDmaInitCount - remaining;

    if (wordsReceived >= 3) {
        uint32_t hdr = bus->rxDmaBuf[0];
        int8_t cmd = maple_hdr_command(hdr);

        if (cmd == MAPLE_CMD_GET_CONDITION) {
            // CRC validation: XOR header + payload, fold to 8-bit, compare
            uint32_t xorAll = bus->rxDmaBuf[0] ^ bus->rxDmaBuf[1];
            uint8_t crc = (xorAll >> 24) ^ (xorAll >> 16) ^ (xorAll >> 8) ^ xorAll;
            uint8_t expected = (bus->rxDmaBuf[2] >> 24) & 0xFF;

            if (crc == expected) {
                // CRC valid — build response packet via callback.
                bus->fastPathCallback(hdr, bus);
                bus->cmd9PreBuilt = true;
                // Disable RX SM + clear PIO IRQ to prevent re-entry.
                // Main loop will restart RX via clearRxAfterFastPath + flushRx.
                pio_sm_set_enabled(bus->getRxPio(), bus->getRxSm(), false);
                pio_interrupt_clear(bus->getRxPio(), bus->getRxSm());
                return;
            }
        }
    }

    // Non-CMD9 or bad CRC: disable the NVIC interrupt to prevent infinite re-entry.
    // Leave the PIO IRQ flag SET so pollReceive() can detect end-of-packet.
    // startRx() will re-enable the NVIC interrupt after the main loop processes it.
    uint irqNum = (bus->getRxPio() == pio1) ? PIO1_IRQ_0 : PIO0_IRQ_0;
    irq_set_enabled(irqNum, false);
    bus->isrNvicDisabled = true;
}

MapleBus::MapleBus()
    : txPio(pio0), rxPio(pio1), txSm(0), txSmOffset(0), rxSm(0), rxSmOffset(0),
      txDmaChannel(0), rxDmaChannel(0),
      pinA(0), pinB(0), initialized(false), rxDmaInitCount(0), rxStartTimeUs(0) {
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

    // TX DMA channel — NO bswap. All data is already in wire (network) order.
    txDmaChannel = dma_claim_unused_channel(true);
    dma_channel_config txConfig = dma_channel_get_default_config(txDmaChannel);
    channel_config_set_read_increment(&txConfig, true);
    channel_config_set_write_increment(&txConfig, false);
    channel_config_set_transfer_data_size(&txConfig, DMA_SIZE_32);
    channel_config_set_dreq(&txConfig, pio_get_dreq(txPio, txSm, true));
    channel_config_set_bswap(&txConfig, false);  // Wire order — no byte swap
    dma_channel_configure(txDmaChannel, &txConfig,
                          &txPio->txf[txSm], // Destination: PIO TX FIFO
                          NULL,               // Source: set at send time
                          0,                  // Count: set at send time
                          false);             // Don't start yet

    gpio_pull_up(pinA);
    gpio_pull_up(pinB);

    // RX: Use PIO1, single state machine (maple_rx — ported from MaplePad/DreamPicoPort).
    // This SM does full Maple Bus protocol decode in hardware:
    // - Detects sync sequence (4 B-toggles while A is low)
    // - Samples data bits at correct clock phases
    // - Signals end-of-packet via IRQ
    // Each FIFO word = 32 decoded data bits = 4 bytes of Maple payload.
    rxPio = pio1;
    rxSmOffset = pio_add_program(rxPio, &maple_rx_program);
    rxSm = pio_claim_unused_sm(rxPio, true);
    maple_rx_program_init(rxPio, rxSm, rxSmOffset, pinA, pinB, 1.0f);

    // RX DMA: linear buffer, NO bswap.
    // maple_rx produces data in wire (network) byte order (MSByte at bits 31:24).
    // Data stays in wire order in RAM — field extraction uses __builtin_bswap32().
    rxDmaChannel = dma_claim_unused_channel(true);
    dma_channel_config rxDmaConfig = dma_channel_get_default_config(rxDmaChannel);
    channel_config_set_read_increment(&rxDmaConfig, false);    // Fixed read: PIO FIFO
    channel_config_set_write_increment(&rxDmaConfig, true);     // Incrementing write: RAM buffer
    channel_config_set_transfer_data_size(&rxDmaConfig, DMA_SIZE_32);
    channel_config_set_dreq(&rxDmaConfig, pio_get_dreq(rxPio, rxSm, false));  // DREQ from RX SM
    channel_config_set_bswap(&rxDmaConfig, false);  // Wire order — no byte swap
    dma_channel_configure(rxDmaChannel, &rxDmaConfig,
                          rxDmaBuf,              // Dest: linear buffer
                          &rxPio->rxf[rxSm],     // Source: PIO RX FIFO
                          MAPLE_RX_DMA_BUF_WORDS, // Transfer count
                          false);                 // Don't start yet

    // Start RX for first packet
    startRx();

    initialized = true;
    return true;
}

void __no_inline_not_in_flash_func(MapleBus::startRx)() {
    // Restart the RX state machine from the beginning of the program.
    // This ensures clean state — no stale data from previous packet.
    pio_sm_set_enabled(rxPio, rxSm, false);
    pio_sm_clear_fifos(rxPio, rxSm);
    pio_sm_restart(rxPio, rxSm);
    pio_sm_clkdiv_restart(rxPio, rxSm);
    pio_sm_exec(rxPio, rxSm, pio_encode_jmp(rxSmOffset));

    // Clear end-of-packet IRQ flag (SM-relative IRQ 0)
    pio_interrupt_clear(rxPio, rxSm);

    // Reset DMA to beginning of buffer
    dma_channel_abort(rxDmaChannel);
    rxDmaInitCount = MAPLE_RX_DMA_BUF_WORDS;
    dma_channel_set_write_addr(rxDmaChannel, rxDmaBuf, false);
    dma_channel_set_trans_count(rxDmaChannel, rxDmaInitCount, false);
    dma_channel_start(rxDmaChannel);

    // Enable SM — it will wait for sync sequence
    pio_sm_set_enabled(rxPio, rxSm, true);

    // Re-enable NVIC interrupt if the ISR disabled it for a non-CMD9 packet.
    if (isrNvicDisabled) {
        isrNvicDisabled = false;
        uint irqNum = (rxPio == pio1) ? PIO1_IRQ_0 : PIO0_IRQ_0;
        irq_set_enabled(irqNum, true);
    }

    rxStartTimeUs = time_us_64();
}

void __no_inline_not_in_flash_func(MapleBus::sendPacket)(const uint32_t* words, uint numWords) {
    // All data is in wire (network) order. No DMA bswap, no pre-reversal needed.
    // Word 0 (bitPairsMinus1) is a PIO loop counter — stored as a plain integer.
    // PIO `out x,32` loads the FIFO word directly into X. Without bswap,
    // the value reaches PIO unchanged.
    static uint32_t txBuf[MAPLE_TX_BUF_SIZE];
    uint count = (numWords <= MAPLE_TX_BUF_SIZE) ? numWords : MAPLE_TX_BUF_SIZE;
    memcpy(txBuf, words, count * sizeof(uint32_t));

    // Disable RX state machine before transmitting to prevent echo capture.
    pio_sm_set_enabled(rxPio, rxSm, false);

    // Bus line check: verify both pins are HIGH (idle) for 10us before driving.
    {
        uint64_t startUs = time_us_64();
        uint64_t outerDeadline = startUs + 1000; // 1ms max wait
        bool busIdle = false;
        while (time_us_64() < outerDeadline) {
            if (!gpio_get(pinA) || !gpio_get(pinB)) {
                startUs = time_us_64();  // Reset 10us window
            } else if ((time_us_64() - startUs) >= 10) {
                busIdle = true;
                break;
            }
        }
        if (!busIdle) {
            if (enableDiagnostics) debugBusStuckCount++;
            // Re-enable RX SM since we're aborting TX
            startRx();
            return;
        }
    }

    // TX SM is stalled at `pull` with both pins HIGH (idle state).
    // Just fire DMA — SM will auto-start when data arrives in TX FIFO.
    dma_channel_set_read_addr(txDmaChannel, txBuf, false);
    dma_channel_set_trans_count(txDmaChannel, count, true);
}

bool __no_inline_not_in_flash_func(MapleBus::isTransmitting)() {
    return dma_channel_is_busy(txDmaChannel);
}

void __no_inline_not_in_flash_func(MapleBus::flushRx)() {
    // Wait for TX DMA to finish feeding the PIO TX FIFO (5ms timeout).
    // Longest packet: VMU block read = 130 words = 520 bytes = ~2ms at 480ns/bit.
    uint64_t deadline = time_us_64() + 5000;
    while (dma_channel_is_busy(txDmaChannel)) {
        if (time_us_64() >= deadline) {
            dma_channel_abort(txDmaChannel);
            if (enableDiagnostics) debugTxTimeout++;
            break;
        }
        tight_loop_contents();
    }

    // Clear TXSTALL sticky bit, then wait for PIO to finish ALL data + tail sequence.
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
            if (enableDiagnostics) debugTxTimeout++;
            break;
        }
        tight_loop_contents();
    }

    // TX is fully complete. Restart RX for next incoming packet.
    startRx();
}

// ============================================================
// RX Packet Reception
//
// The maple_rx PIO program (ported from DreamPicoPort) does full
// Maple Bus protocol decode in hardware. Each FIFO word contains
// 32 decoded data bits = 4 bytes. NO DMA bswap — data stays in
// wire (network) byte order. Field extraction uses bswap32().
//
// End-of-packet: PIO sets IRQ flag and stalls. We poll the flag
// in pollReceive(), validate CRC, and return the packet data.
// ============================================================

bool __no_inline_not_in_flash_func(MapleBus::pollReceive)(const uint8_t** outPacket, uint* outLength) {
    // Check if the PIO has signaled end-of-packet via IRQ.
    // maple_rx uses `irq wait 0 rel` which sets the SM-relative IRQ flag.
    // The flag index is rxSm (SM-relative IRQ 0 maps to PIO IRQ rxSm).
    bool endOfPacket = (rxPio->irq & (1u << rxSm)) != 0;

    if (endOfPacket) {
        if (enableDiagnostics) {
            rxArrivalTimestamp = timer_hw->timerawl;
            debugEndIrqCount++;
        }

        // Wait up to 1ms for DMA to fully drain the RX FIFO.
        // Critical: we must NOT read transfer_count while DMA is still active —
        // that creates a race where we get a stale word count and then manually
        // drain FIFO words into buffer positions DMA may also write to.
        // MaplePad uses this same 1ms wait (MapleBus.cpp:440-442).
        {
            uint64_t drainDeadline = time_us_64() + 1000;
            while (!pio_sm_is_rx_fifo_empty(rxPio, rxSm)
                   && time_us_64() < drainDeadline) {
                tight_loop_contents();
            }
        }

        // Now DMA has had time to transfer everything. Read the final count.
        uint32_t remaining = dma_channel_hw_addr(rxDmaChannel)->transfer_count;
        uint32_t wordsReceived = rxDmaInitCount - remaining;

        // Need at least 2 words (header + CRC) for a valid packet
        if (wordsReceived >= 2) {
            // Extract numWords from wire-order header word.
            // Wire order: numWords at bits [31:24] (first byte on wire).
            uint8_t numWords = maple_hdr_numWords(rxDmaBuf[0]);

            // CRC byte: PIO `in null 24` pads lower 24 bits with zeros,
            // CRC byte is at bits [31:24] of the last word (wire order).
            uint8_t expectedCrc = (rxDmaBuf[wordsReceived - 1] >> 24) & 0xFF;

            // Flexible length check (matching MaplePad):
            // numWords = number of payload words declared in header.
            // Actual data words (excl CRC) = wordsReceived - 1.
            // Header itself is 1 word, so payload words = wordsReceived - 2.
            // Allow MORE words than declared (VMU extended info can have extras),
            // but reject if fewer than declared.
            if (numWords <= (wordsReceived - 2)) {
                // Compute CRC over all words except the CRC word itself.
                // XOR all data words, fold to 8-bit, compare against expected.
                // CRC computation is byte-order independent (XOR is commutative).
                uint32_t xorAll = 0;
                for (uint32_t i = 0; i < wordsReceived - 1; i++) {
                    xorAll ^= rxDmaBuf[i];
                }
                uint8_t crc = (xorAll >> 24) ^ (xorAll >> 16) ^ (xorAll >> 8) ^ xorAll;

                if (crc == expectedCrc) {
                    // Valid packet. Copy to separate buffer before restarting RX.
                    // Critical: if we return a pointer to rxDmaBuf and then clear the IRQ,
                    // the SM unblocks from `irq wait 0 rel`, wraps to wait_start, and
                    // a new incoming packet could overwrite rxDmaBuf while caller reads it.
                    uint32_t dataWords = wordsReceived - 1; // exclude CRC word
                    memcpy(rxPacketBuf, rxDmaBuf, dataWords * sizeof(uint32_t));
                    *outPacket = (const uint8_t*)rxPacketBuf;
                    *outLength = dataWords * sizeof(uint32_t);
                    if (enableDiagnostics) debugPollTrue++;

                    // Stop the SM so it can't capture new data into the DMA buffer.
                    // Caller will call flushRx() after sending response, which restarts RX.
                    pio_sm_set_enabled(rxPio, rxSm, false);
                    pio_interrupt_clear(rxPio, rxSm);
                    return true;
                } else {
                    if (enableDiagnostics) debugXorFail++;
                    lastRxWasCorrupt = true;
                }
            } else {
                // Not enough words received vs what header declared
                if (enableDiagnostics) debugNumWordsMismatch++;
                lastRxWasCorrupt = true;
            }
        }

        // Invalid packet (bad CRC or too short) — restart for next packet
        startRx();
    }

    // Timeout: if we've been waiting too long with no end-of-packet, restart.
    // This handles truncated packets (bus noise, partial TX from DC).
    if (rxStartTimeUs > 0 && (time_us_64() - rxStartTimeUs) > 5000) {
        // Only timeout if DMA has received some data (indicating a partial packet)
        uint32_t remaining = dma_channel_hw_addr(rxDmaChannel)->transfer_count;
        if (remaining < rxDmaInitCount) {
            if (enableDiagnostics) debugRxTimeout++;
            startRx();
        } else {
            // No data at all — just reset the timer to avoid repeated checks
            rxStartTimeUs = time_us_64();
        }
    }

    *outPacket = nullptr;
    *outLength = 0;
    return false;
}

uint32_t __no_inline_not_in_flash_func(MapleBus::calcCRC)(const uint32_t* words, uint numWords) {
    // Maple Bus CRC: XOR all 32-bit words, then fold all 4 bytes to 8-bit CRC.
    // CRC byte placed at bits [31:24] (wire order) — PIO sends MSBit first,
    // so this byte goes on wire first, matching the Maple Bus protocol spec.
    uint32_t lrc = 0;
    for (uint i = 0; i < numWords; i++) {
        lrc ^= words[i];
    }
    // Fold 32-bit XOR to 8-bit CRC byte
    uint8_t crc = (lrc >> 24) ^ (lrc >> 16) ^ (lrc >> 8) ^ lrc;
    return (uint32_t)crc << 24;  // CRC at bits [31:24] for wire TX
}

uint32_t __no_inline_not_in_flash_func(MapleBus::calcBitPairs)(uint packetSize) {
    // BitPairsMinus1: PIO data loop consumes 2 bits per iteration.
    // Wire data = header (4 bytes) + payload + CRC (1 byte).
    // packetSize includes bitPairsMinus1 (4 bytes) + header + payload + CRC word (4 bytes).
    // But CRC is only 1 byte on wire, not 4. So wire bytes = packetSize - 4 (prefix) - 3 (unused CRC padding).
    // Bit pairs = (packetSize - 7) * 4. Minus 1 for PIO loop counter.
    return (packetSize - 7) * 4 - 1;
}

void MapleBus::enableFastPath(MapleFastPathCallback callback) {
    if (fastPathEnabled || !initialized) return;

    fastPathCallback = callback;
    irqBusInstance = this;
    cmd9PreBuilt = false;

    // Enable PIO IRQ for end-of-packet notification.
    // maple_rx uses `irq wait 0 rel` — SM-relative IRQ 0 maps to PIO IRQ rxSm.
    uint irqNum = (rxPio == pio1) ? PIO1_IRQ_0 : PIO0_IRQ_0;
    pio_set_irq0_source_enabled(rxPio, (pio_interrupt_source)(pis_interrupt0 + rxSm), true);
    irq_set_exclusive_handler(irqNum, mapleRxIrqHandler);
    irq_set_priority(irqNum, 0);  // Highest priority
    irq_set_enabled(irqNum, true);

    fastPathEnabled = true;
}

void MapleBus::disableFastPath() {
    if (!fastPathEnabled) return;

    uint irqNum = (rxPio == pio1) ? PIO1_IRQ_0 : PIO0_IRQ_0;
    irq_set_enabled(irqNum, false);
    pio_set_irq0_source_enabled(rxPio, (pio_interrupt_source)(pis_interrupt0 + rxSm), false);

    fastPathCallback = nullptr;
    irqBusInstance = nullptr;
    cmd9PreBuilt = false;
    fastPathEnabled = false;
}

void __no_inline_not_in_flash_func(MapleBus::clearRxAfterFastPath)() {
    // After ISR consumed the packet, PIO RX SM is stalled at `irq wait 0 rel`.
    // Disable SM and clear IRQ so sendPacket() → flushRx() → startRx() can restart cleanly.
    pio_sm_set_enabled(rxPio, rxSm, false);
    pio_interrupt_clear(rxPio, rxSm);
}

