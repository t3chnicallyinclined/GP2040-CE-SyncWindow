#include "drivers/dreamcast/DreamcastDriver.h"
#include "gamepad.h"
#include "gamepad/GamepadState.h"
#include "storagemanager.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "uart_rx.pio.h"
#include "net/w6100.h"
extern "C" {
#include "wizchip_conf.h"
#include "w6100.h"
}

// Driver instances indexed by PIO1 RX SM number (matches irqBusInstances in maple_bus.cpp).
static DreamcastDriver* irqDriverInstances[4] = {};

// ============================================================
// ISR Command Dispatch — handles ALL Maple Bus commands from ISR
// ============================================================

void __no_inline_not_in_flash_func(DreamcastDriver::isrCommandDispatch)(MapleBus* bus) {
    DreamcastDriver* drv = irqDriverInstances[bus->getRxSm()];
    if (!drv) return;

    const bool isP2 = (drv->p2Enabled && bus == &drv->busP2);

    uint32_t hdr = bus->rxDmaBuf[0];
    int8_t cmd = maple_hdr_command(hdr);
    uint8_t destAddr = maple_hdr_dest(hdr) & MAPLE_PERIPH_MASK;
    uint8_t port = maple_hdr_origin(hdr) & MAPLE_PORT_MASK;

    // Route VMU sub-peripheral commands (P1 only)
    if (!isP2 && !drv->disableVMU && destAddr == MAPLE_SUB0_ADDR) {
        if (!drv->vmuReady) return;
        drv->isrHandleVmuCommand(cmd, hdr, port, bus);
        return;
    }

    if (destAddr != MAPLE_CTRL_ADDR) return;

    // P2 has no VMU sub-peripheral
    uint8_t subPeriphBits = (!isP2 && !drv->disableVMU) ? MAPLE_SUB0_ADDR : 0;
    uint8_t origin = (MAPLE_CTRL_ADDR & MAPLE_PERIPH_MASK) | subPeriphBits | port;
    uint8_t dest = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | port;

    // Select the right buffers for P1 or P2
    uint32_t* ctrlBuf = isP2 ? drv->p2ControllerPacketBuf : drv->controllerPacketBuf;
    uint32_t* infoBuf = isP2 ? drv->p2InfoPacketBuf : drv->infoPacketBuf;
    uint32_t* extBuf  = isP2 ? drv->p2ExtInfoBuf : drv->extInfoBuf;
    uint32_t* ackBuf  = isP2 ? drv->p2AckPacketBuf : drv->ackPacketBuf;
    uint32_t* unkBuf  = isP2 ? drv->p2UnknownCmdBuf : drv->unknownCmdBuf;

    switch (cmd) {
        case MAPLE_CMD_GET_CONDITION: {
            // Fastest path — pre-computed lookup table.
            // If port is known and matches cached, use pre-computed W5 (CRC).
            // Otherwise compute CRC inline (~5 cycles).
            uint32_t w1 = maple_make_header(3, origin, dest, MAPLE_CMD_RESPOND_DATA_XFER);
            uint32_t w3 = isP2 ? drv->p2Cmd9ReadyW3 : drv->cmd9ReadyW3;

            ctrlBuf[1] = w1;
            ctrlBuf[3] = w3;

            {
                bool pk = isP2 ? drv->p2PortKnown : drv->portKnown;
                uint8_t cp = isP2 ? drv->p2CachedPort : drv->cachedPort;
                if (pk && port == cp) {
                    ctrlBuf[5] = isP2 ? drv->p2Cmd9ReadyW5 : drv->cmd9ReadyW5;
                } else {
                    uint32_t xorAll = w1 ^ ctrlBuf[2] ^ w3 ^ ctrlBuf[4];
                    uint8_t crc = (xorAll >> 24) ^ (xorAll >> 16) ^ (xorAll >> 8) ^ xorAll;
                    ctrlBuf[5] = (uint32_t)crc << 24;
                }
            }

            bus->isrSendFifo(ctrlBuf, 6);

            // Extract game state telemetry from extended CMD9 (P1 only).
            // ROM patch adds extra words after the function code:
            //   rxDmaBuf[0] = header (numWords=5 for 4 telemetry words)
            //   rxDmaBuf[1] = func code (0x01000000, standard)
            //   rxDmaBuf[2..5] = telemetry (health, timer, meter, match state)
            //   rxDmaBuf[6] = CRC
            // Reads happen AFTER response is queued — PIO is physically
            // transmitting while we read.  Zero added latency.
            if (!isP2) {
                uint8_t nw = maple_hdr_numWords(hdr);
                if (nw > 1) {
                    uint8_t extra = nw - 1;  // subtract function code word
                    if (extra > DreamcastDriver::TELEMETRY_MAX_WORDS)
                        extra = DreamcastDriver::TELEMETRY_MAX_WORDS;
                    for (uint8_t i = 0; i < extra; i++)
                        drv->telemetry[i] = bus->rxDmaBuf[2 + i];
                    drv->telemetryWordCount = extra;
                    drv->hasTelemetry = true;
                }
            }

            if (drv->enableDiagnostics) {
                drv->debugRxCount++;
                drv->debugCmd9Count++;
                drv->debugTxCount++;
                drv->debugTableHits++;
                drv->debugConsecutivePolls++;

                // Frame interval tracking
                uint32_t rxTs = bus->rxArrivalTimestamp;
                if (drv->prevCmd9Timestamp != 0) {
                    uint32_t interval = rxTs - drv->prevCmd9Timestamp;
                    drv->frameIntervalLast = interval;
                    if (interval < drv->frameIntervalMin) drv->frameIntervalMin = interval;
                    if (interval > drv->frameIntervalMax) drv->frameIntervalMax = interval;
                    drv->frameCount++;
                    if (interval > 20000) drv->droppedPollCount++;
                }
                drv->prevCmd9Timestamp = rxTs;

                // Button-to-poll latency
                if (drv->b2pPending) {
                    uint32_t latency = rxTs - drv->buttonChangeTimestamp;
                    drv->b2pLast = latency;
                    if (latency < drv->b2pMin) drv->b2pMin = latency;
                    if (latency > drv->b2pMax) drv->b2pMax = latency;
                    drv->b2pCount++;
                    drv->b2pPending = false;
                }
            }
            break;
        }

        case MAPLE_CMD_DEVICE_REQUEST: {
            if (isP2) {
                if (!drv->p2PortKnown || port != drv->p2CachedPort) {
                    drv->p2CachedPort = port;
                    drv->p2PortKnown = true;
                    drv->rebuildAllPacketsForPortP2(port);
                }
            } else {
                if (!drv->portKnown || port != drv->cachedPort) {
                    drv->cachedPort = port;
                    drv->portKnown = true;
                    drv->rebuildAllPacketsForPort(port);
                }
            }
            bus->isrSendDma(infoBuf, 31);
            if (!isP2) drv->vmuReady = true;
            if (drv->enableDiagnostics) {
                drv->debugRxCount++;
                drv->debugCmd1Count++;
                drv->debugTxCount++;
                if (drv->debugConsecutivePolls > drv->debugMaxConsecutivePolls)
                    drv->debugMaxConsecutivePolls = drv->debugConsecutivePolls;
                drv->debugConsecutivePolls = 0;
            }
            break;
        }

        case MAPLE_CMD_ALL_STATUS_REQUEST: {
            bus->isrSendDma(extBuf, 51);
            if (drv->enableDiagnostics) {
                drv->debugRxCount++;
                drv->debugTxCount++;
            }
            break;
        }

        case MAPLE_CMD_SET_CONDITION: {
            bus->isrSendFifo(ackBuf, 3);
            if (drv->enableDiagnostics) {
                drv->debugRxCount++;
                drv->debugTxCount++;

                uint32_t rxTs = bus->rxArrivalTimestamp;
                drv->cmd14Count++;

                // Extract vibrate payload (word 3 of packet: funcCode=w1, data=w2,w3...)
                // CMD14 payload: [header][funcCode][condition data]
                // For vibrate: condition data has motor values
                uint16_t payload = 0;
                if (bus->isrWordsReceived >= 3) {
                    // Word 2 (rxDmaBuf[2]) contains vibrate data
                    // Motor values are in the upper bytes (wire order)
                    uint32_t condWord = bus->rxDmaBuf[2];
                    payload = (uint16_t)(condWord >> 16);  // motor0 << 8 | motor1
                }
                drv->cmd14LastPayload = payload;

                // Interval from previous CMD14
                uint32_t interval = 0;
                if (drv->cmd14LastTimestamp != 0) {
                    interval = rxTs - drv->cmd14LastTimestamp;
                    drv->cmd14IntervalLast = interval;
                }
                drv->cmd14PrevTimestamp = drv->cmd14LastTimestamp;
                drv->cmd14LastTimestamp = rxTs;

                // Burst detection: CMD14s within 100ms of each other
                if (interval > 0 && interval < 100000) {
                    drv->cmd14BurstCount++;
                } else {
                    // New burst — save previous burst length if it was the longest
                    if (drv->cmd14BurstCount > drv->cmd14LongestBurst) {
                        drv->cmd14LongestBurst = drv->cmd14BurstCount;
                    }
                    drv->cmd14BurstCount = 1;
                    drv->cmd14BurstStart = rxTs;
                }

                // Log entry for vibrate fingerprinting
                uint8_t idx = drv->cmd14LogIdx;
                drv->cmd14Log[idx].timestamp = rxTs;
                drv->cmd14Log[idx].payload = payload;
                drv->cmd14Log[idx].intervalUs = (interval > 0xFFFF) ? 0xFFFF : (uint16_t)interval;
                drv->cmd14LogIdx = (idx + 1) % DreamcastDriver::CMD14_LOG_SIZE;
                if (drv->cmd14LogCount < DreamcastDriver::CMD14_LOG_SIZE) drv->cmd14LogCount++;
            }
            break;
        }

        case MAPLE_CMD_RESET_DEVICE:
        case MAPLE_CMD_SHUTDOWN_DEVICE: {
            bus->isrSendFifo(ackBuf, 3);
            if (drv->enableDiagnostics) {
                drv->debugRxCount++;
                drv->debugTxCount++;
            }
            break;
        }

        default: {
            bus->isrSendFifo(unkBuf, 3);
            if (drv->enableDiagnostics) {
                drv->debugRxCount++;
                drv->debugTxCount++;
            }
            break;
        }
    }
}

// ============================================================
// ISR VMU sub-dispatch
// ============================================================

void __no_inline_not_in_flash_func(DreamcastDriver::isrHandleVmuCommand)(
    int8_t cmd, uint32_t hdr, uint8_t port, MapleBus* bus) {
    // VMU commands are forwarded to DreamcastVMU for ISR handling.
    // For now, fall through to main loop via isrNvicDisabled — VMU commands
    // are infrequent (save/load screens only) and can tolerate main loop latency.
    // TODO: Move VMU command handling into ISR (Phase 3)
    bus->debugIsrFallthrough++;
    uint irqNum = (bus->getRxPio() == pio1) ? PIO1_IRQ_0 : PIO0_IRQ_0;
    irq_set_enabled(irqNum, false);
    bus->isrNvicDisabled = true;
    bus->isrTxFired = false;  // Don't set — main loop will handle via pollReceive
}

// ============================================================
// Helper: build device info string in wire order
// ============================================================

static void buildDevInfoString(uint32_t* dest, const char* str, uint maxLen) {
    uint8_t buf[64];
    memset(buf, ' ', maxLen);
    for (uint i = 0; i < maxLen && str[i]; i++) {
        buf[i] = str[i];
    }
    uint words = maxLen / 4;
    for (uint i = 0; i < words; i++) {
        uint32_t hostWord = buf[i*4] | ((uint32_t)buf[i*4+1] << 8)
                          | ((uint32_t)buf[i*4+2] << 16) | ((uint32_t)buf[i*4+3] << 24);
        dest[i] = __builtin_bswap32(hostWord);
    }
}

// ============================================================
// Constructor + Init
// ============================================================

DreamcastDriver::DreamcastDriver()
    : connected(false) {
}

bool DreamcastDriver::init(uint pin_a, uint pin_b) {
    if (!bus.init(pin_a, pin_b)) return false;

    const GamepadOptions& options = Storage::getInstance().getGamepadOptions();
    disableVMU = options.disableVMU;

    buildInfoPacket();
    buildExtInfoPacket();
    buildControllerPacket();
    buildACKPacket();
    buildResendPacket();
    buildUnknownCmdPacket();

    if (!disableVMU) {
        vmu.init();
    }

    buildGpioDcMap();
    buildCmd9LookupTable();

    // Always enable ISR dispatch — handles ALL commands from ISR.
    irqDriverInstances[bus.getRxSm()] = this;
    bus.enableIsrDispatch(isrCommandDispatch);

    connected = true;
    return true;
}

bool DreamcastDriver::initP2(uint pin_a, uint pin_b) {
    if (!busP2.init(pin_a, pin_b)) return false;

    buildInfoPacketP2();
    buildExtInfoPacketP2();
    buildControllerPacketP2();
    buildACKPacketP2();
    buildResendPacketP2();
    buildUnknownCmdPacketP2();

    // Set analog sticks to center
    p2ControllerPacketBuf[4] = ((uint32_t)0x80 << 24) | ((uint32_t)0x80 << 16)
                              | ((uint32_t)0x80 << 8) | 0x80;

    irqDriverInstances[busP2.getRxSm()] = this;
    busP2.enableIsrDispatch(isrCommandDispatch);

    p2Enabled = true;
    return true;
}

// ============================================================
// GPIO → DC button mapping
// ============================================================

void DreamcastDriver::buildGpioDcMap() {
    memset(gpioDcButtonMap, 0, sizeof(gpioDcButtonMap));
    triggerLTMask = 0;
    triggerRTMask = 0;
    buttonGpioMask = 0;

    Gamepad* gamepad = Storage::getInstance().GetGamepad();

    struct { GamepadButtonMapping* mapping; uint16_t dcMask; } buttonTable[] = {
        { gamepad->mapButtonB1, DC_BTN_A },
        { gamepad->mapButtonB2, DC_BTN_B },
        { gamepad->mapButtonB3, DC_BTN_X },
        { gamepad->mapButtonB4, DC_BTN_Y },
        { gamepad->mapButtonL1, DC_BTN_C },
        { gamepad->mapButtonR1, DC_BTN_Z },
        { gamepad->mapButtonS2, DC_BTN_START },
        { gamepad->mapDpadUp,    DC_BTN_UP },
        { gamepad->mapDpadDown,  DC_BTN_DOWN },
        { gamepad->mapDpadLeft,  DC_BTN_LEFT },
        { gamepad->mapDpadRight, DC_BTN_RIGHT },
    };

    for (auto& entry : buttonTable) {
        if (entry.mapping && entry.mapping->pinMask) {
            uint pin = __builtin_ctz(entry.mapping->pinMask);
            gpioDcButtonMap[pin] = entry.dcMask;
            buttonGpioMask |= (1u << pin);
        }
    }

    if (gamepad->mapButtonL2 && gamepad->mapButtonL2->pinMask) {
        triggerLTMask = gamepad->mapButtonL2->pinMask;
        buttonGpioMask |= triggerLTMask;
    }
    if (gamepad->mapButtonR2 && gamepad->mapButtonR2->pinMask) {
        triggerRTMask = gamepad->mapButtonR2->pinMask;
        buttonGpioMask |= triggerRTMask;
    }
}

// ============================================================
// CMD9 Lookup Table — pre-computed W3 (buttons) and W5 (CRC)
// ============================================================

void DreamcastDriver::buildCmd9LookupTable() {
    cmd9NumBits = 0;
    memset(cmd9GpioPins, 0, sizeof(cmd9GpioPins));

    for (uint pin = 0; pin < 30 && cmd9NumBits < 13; pin++) {
        if (buttonGpioMask & (1u << pin)) {
            cmd9GpioPins[cmd9NumBits] = pin;
            cmd9NumBits++;
        }
    }

    cmd9TableSize = 1u << cmd9NumBits;

    if (cmd9TableW3) delete[] cmd9TableW3;
    if (cmd9TableW5) delete[] cmd9TableW5;
    cmd9TableW3 = new uint32_t[cmd9TableSize];
    cmd9TableW5 = new uint32_t[cmd9TableSize];

    // Build W3 for every possible button combination.
    // W5 (CRC) built with default port (0x00). Rebuilt after CMD1 reveals real port.
    uint8_t subPeriphBits = disableVMU ? 0 : MAPLE_SUB0_ADDR;
    uint8_t origin = (MAPLE_CTRL_ADDR & MAPLE_PERIPH_MASK) | subPeriphBits;
    uint8_t dest   = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK);
    uint32_t w1 = maple_make_header(3, origin, dest, MAPLE_CMD_RESPOND_DATA_XFER);
    uint32_t w2 = maple_host_to_wire(MAPLE_FUNC_CONTROLLER);
    uint32_t w4 = ((uint32_t)0x80 << 24) | ((uint32_t)0x80 << 16)
                | ((uint32_t)0x80 << 8) | 0x80;

    for (uint32_t i = 0; i < cmd9TableSize; i++) {
        uint32_t fakeGpio = 0;
        for (uint8_t b = 0; b < cmd9NumBits; b++) {
            if (i & (1u << b)) {
                fakeGpio |= (1u << cmd9GpioPins[b]);
            }
        }

        uint8_t lt, rt;
        uint16_t dcButtons = mapRawGpioToDC(fakeGpio, &lt, &rt);

        uint32_t w3 = ((uint32_t)lt << 24) | ((uint32_t)rt << 16)
                    | ((uint32_t)((dcButtons >> 8) & 0xFF) << 8)
                    | (dcButtons & 0xFF);

        uint32_t xorAll = w1 ^ w2 ^ w3 ^ w4;
        uint8_t crc = (xorAll >> 24) ^ (xorAll >> 16) ^ (xorAll >> 8) ^ xorAll;

        cmd9TableW3[i] = w3;
        cmd9TableW5[i] = (uint32_t)crc << 24;
    }

    cmd9ReadyW3 = cmd9TableW3[0];
    cmd9ReadyW5 = cmd9TableW5[0];
}

void DreamcastDriver::rebuildCmd9LookupTableForPort() {
    // Rebuild W5 (CRC) with the real port address after CMD1.
    // W3 doesn't change — only the header word changes.
    uint8_t subPeriphBits = disableVMU ? 0 : MAPLE_SUB0_ADDR;
    uint8_t origin = (MAPLE_CTRL_ADDR & MAPLE_PERIPH_MASK) | subPeriphBits | cachedPort;
    uint8_t dest   = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | cachedPort;
    uint32_t w1 = maple_make_header(3, origin, dest, MAPLE_CMD_RESPOND_DATA_XFER);
    uint32_t w2 = controllerPacketBuf[2];  // funcCode — constant
    uint32_t w4 = controllerPacketBuf[4];  // analog — current value

    // Cache XOR constant for network input CRC computation
    cachedCrcXorConst = w1 ^ w2 ^ w4;

    for (uint32_t i = 0; i < cmd9TableSize; i++) {
        uint32_t w3 = cmd9TableW3[i];
        uint32_t xorAll = w1 ^ w2 ^ w3 ^ w4;
        uint8_t crc = (xorAll >> 24) ^ (xorAll >> 16) ^ (xorAll >> 8) ^ xorAll;
        cmd9TableW5[i] = (uint32_t)crc << 24;
    }

    // Update ready values
    uint32_t raw = ~gpio_get_all();
    uint32_t index = 0;
    for (uint8_t b = 0; b < cmd9NumBits; b++) {
        if (raw & (1u << cmd9GpioPins[b])) index |= (1u << b);
    }
    cmd9ReadyW3 = cmd9TableW3[index];
    cmd9ReadyW5 = cmd9TableW5[index];
}

void __no_inline_not_in_flash_func(DreamcastDriver::updateCmd9FromGpio)(uint32_t filteredGpio) {
    if (filteredGpio == lastFilteredGpio) return;
    lastFilteredGpio = filteredGpio;
    uint32_t index = 0;
    for (uint8_t b = 0; b < cmd9NumBits; b++) {
        if (filteredGpio & (1u << cmd9GpioPins[b])) {
            index |= (1u << b);
        }
    }
    cmd9ReadyW3 = cmd9TableW3[index];
    cmd9ReadyW5 = cmd9TableW5[index];

    // Timestamp for button-to-poll latency measurement
    if (enableDiagnostics) {
        buttonChangeTimestamp = time_us_32();
        b2pPending = true;
    }
}

void __no_inline_not_in_flash_func(DreamcastDriver::updateAnalogFromGamepad)(Gamepad* gamepad) {
    uint32_t lx32 = (uint32_t)gamepad->state.lx + 0x80;
    uint32_t ly32 = (uint32_t)gamepad->state.ly + 0x80;
    uint8_t jx = (uint8_t)(lx32 > 0xFFFF ? 0xFF : (lx32 >> 8));
    uint8_t jy = (uint8_t)(ly32 > 0xFFFF ? 0xFF : (ly32 >> 8));
    controllerPacketBuf[4] = ((uint32_t)0x80 << 24) | ((uint32_t)0x80 << 16)
                           | ((uint32_t)jy << 8) | jx;
}

// ============================================================
// Network input via PIO UART RX
// ============================================================

void DreamcastDriver::initUartRx(uint pin, uint baud) {
    uartRxPio = pio1;
    uartRxSmOffset = pio_add_program(uartRxPio, &uart_rx_program);
    uartRxSm = pio_claim_unused_sm(uartRxPio, true);
    uart_rx_program_init(uartRxPio, uartRxSm, uartRxSmOffset, pin, baud);
    uartFramePos = 0;
    uartRxInitialized = true;
}

void __no_inline_not_in_flash_func(DreamcastDriver::pollUartRx)() {
    if (!uartRxInitialized) return;

    // Debug: count raw FIFO level to verify PIO is receiving
    uint fifoLevel = pio_sm_get_rx_fifo_level(uartRxPio, uartRxSm);
    if (fifoLevel > 0 && enableDiagnostics) {
        netBadSync += 1000;  // Hacky marker: if we ever see bad>1000, FIFO has data
    }

    // Parse any new UART bytes from PIO FIFO
    while (!pio_sm_is_rx_fifo_empty(uartRxPio, uartRxSm)) {
        uint8_t byte = (uint8_t)(pio_sm_get(uartRxPio, uartRxSm) >> 24);
        if (uartFramePos == 0) {
            if (byte == 0xAA) {
                uartFramePos = 1;
            } else if (enableDiagnostics) {
                netBadSync++;
            }
            continue;
        }
        uartFrameBuf[uartFramePos - 1] = byte;
        uartFramePos++;
        if (uartFramePos == 5) {
            uint32_t w3 = ((uint32_t)uartFrameBuf[0] << 24) |
                          ((uint32_t)uartFrameBuf[1] << 16) |
                          ((uint32_t)uartFrameBuf[2] << 8)  |
                          (uint32_t)uartFrameBuf[3];
            lastNetW3 = w3;
            uint32_t now = timer_hw->timerawl;

            if (enableDiagnostics) {
                netFrameCount++;
                if (netFrameArrivalPrev != 0) {
                    uint32_t interval = now - netFrameArrivalPrev;
                    netIntervalLast = interval;
                    if (interval < netIntervalMin) netIntervalMin = interval;
                    if (interval > netIntervalMax) netIntervalMax = interval;
                }
                netFrameArrivalPrev = now;
            }

            lastNetTimestamp = now;
            hasNetState = true;
            uartFramePos = 0;
        }
    }

    // Re-apply latched network state every loop (like a held GPIO pin).
    // Timeout after 100ms of no UART frames = sender disconnected.
    if (hasNetState) {
        if (timer_hw->timerawl - lastNetTimestamp > 100000) {
            hasNetState = false;
        } else {
            updateCmd9FromNetwork(lastNetW3);
        }
    }
}

void __no_inline_not_in_flash_func(DreamcastDriver::updateCmd9FromNetwork)(uint32_t w3) {
    p2Cmd9ReadyW3 = w3;
    if (enableDiagnostics) {
        netApplyCount++;
        netLastW3 = w3;
    }
    if (p2PortKnown) {
        uint32_t xorAll = p2CachedCrcXorConst ^ w3;
        uint8_t crc = (xorAll >> 24) ^ (xorAll >> 16) ^ (xorAll >> 8) ^ xorAll;
        p2Cmd9ReadyW5 = (uint32_t)crc << 24;
    }
}

// ============================================================
// W6100 Ethernet transport
// ============================================================

void DreamcastDriver::initEthernet(uint pin_miso, uint pin_cs, uint pin_sclk, uint pin_mosi, uint pin_rst) {
    // Init W6100 — skip version check, just configure and go
    w6100_init(pin_miso, pin_cs, pin_sclk, pin_mosi, pin_rst);
    ethernetChipVersion = w6100_get_version();

    uint8_t mac[6] = {0x00, 0x08, 0xDC, 0xDC, 0x00, 0x01};
    uint8_t ip[4] = {192, 168, 1, 100};
    uint8_t subnet[4] = {255, 255, 255, 0};
    uint8_t gateway[4] = {192, 168, 1, 1};

    w6100_set_mac(mac);
    w6100_set_ip(ip, subnet, gateway);

    if (!w6100_udp_open(4977)) {
        return;
    }

    // MapleCast server IP/port — configured via web UI
    const GamepadOptions& opts = Storage::getInstance().getGamepadOptions();
    if (opts.maplecastEnabled) {
        serverIp[0] = opts.maplecastServerIP1;
        serverIp[1] = opts.maplecastServerIP2;
        serverIp[2] = opts.maplecastServerIP3;
        serverIp[3] = opts.maplecastServerIP4;
        serverPort = opts.maplecastServerPort;
    } else {
        serverIp[0] = 0; serverIp[1] = 0; serverIp[2] = 0; serverIp[3] = 0;
        serverPort = 0;
    }

    ethernetInitialized = true;
}

void __no_inline_not_in_flash_func(DreamcastDriver::sendLocalState)() {
    if (!ethernetInitialized) return;
    if (serverIp[0] == 0 && serverIp[1] == 0 && serverIp[2] == 0 && serverIp[3] == 0) return;

    // Send P1 physical button state to relay server
    uint32_t w3 = cmd9ReadyW3;
    uint8_t data[4];
    data[0] = (w3 >> 24) & 0xFF;
    data[1] = (w3 >> 16) & 0xFF;
    data[2] = (w3 >> 8) & 0xFF;
    data[3] = w3 & 0xFF;
    w6100_udp_send(data, 4, serverIp, serverPort);

    // Timeout netplay if server stops responding (2 seconds)
    if (netplayActive && (timer_hw->timerawl - lastNetTimestamp > 2000000)) {
        netplayActive = false;
        hasNetState = false;
    }
}

void DreamcastDriver::sendTelemetry() {
    if (!ethernetInitialized || !hasTelemetry) return;
    if (serverIp[0] == 0 && serverIp[1] == 0 && serverIp[2] == 0 && serverIp[3] == 0) return;

    // Forward game state telemetry from extended CMD9 to relay server.
    // Packet format: [0x54 'T'] [wordCount] [telemetry words...]
    // 0x54 = 'T' header byte — distinguishes from 4-byte button packets.
    uint8_t wordCount = telemetryWordCount;
    uint8_t buf[2 + TELEMETRY_MAX_WORDS * 4];
    buf[0] = 0x54;  // 'T' — telemetry packet marker
    buf[1] = wordCount;
    for (uint8_t i = 0; i < wordCount; i++) {
        uint32_t w = telemetry[i];
        buf[2 + i*4 + 0] = (w >> 24) & 0xFF;
        buf[2 + i*4 + 1] = (w >> 16) & 0xFF;
        buf[2 + i*4 + 2] = (w >> 8) & 0xFF;
        buf[2 + i*4 + 3] = w & 0xFF;
    }
    w6100_udp_send(buf, 2 + wordCount * 4, serverIp, serverPort);
    hasTelemetry = false;
    telemetryTxCount++;
}

void __no_inline_not_in_flash_func(DreamcastDriver::pollEthernet)() {
    if (!ethernetInitialized) return;

    // Receive merged state from relay server: 8 bytes = P1 W3 (4) + P2 W3 (4)
    // Or legacy 4-byte P2-only packets (direct mode without server)
    uint8_t data[8];
    uint8_t lastData[8] = {0};
    int lastLen = 0;
    bool gotPacket = false;

    // Drain all pending, keep latest
    while (true) {
        int len = w6100_udp_recv(data, 8, nullptr, nullptr);
        if (len <= 0) break;
        memcpy(lastData, data, len);
        lastLen = len;
        gotPacket = true;
        if (enableDiagnostics) netFrameCount++;
    }

    if (gotPacket) {
        if (lastLen >= 8) {
            // 8-byte merged packet from relay server: {P1_W3, P2_W3}
            // Server responding = netplay is active
            netplayActive = true;

            uint32_t p1_w3 = ((uint32_t)lastData[0] << 24) |
                             ((uint32_t)lastData[1] << 16) |
                             ((uint32_t)lastData[2] << 8)  |
                             (uint32_t)lastData[3];
            uint32_t p2_w3 = ((uint32_t)lastData[4] << 24) |
                             ((uint32_t)lastData[5] << 16) |
                             ((uint32_t)lastData[6] << 8)  |
                             (uint32_t)lastData[7];

            // Apply P1 from server (overrides local GPIO for sync)
            cmd9ReadyW3 = p1_w3;
            if (portKnown) {
                uint32_t xorAll = cachedCrcXorConst ^ p1_w3;
                uint8_t crc = (xorAll >> 24) ^ (xorAll >> 16) ^ (xorAll >> 8) ^ xorAll;
                cmd9ReadyW5 = (uint32_t)crc << 24;
            }

            // Apply P2 from server
            updateCmd9FromNetwork(p2_w3);

            lastNetW3 = p2_w3;
        } else if (lastLen >= 4) {
            // 4-byte legacy packet: P2 only (direct mode, no server)
            uint32_t w3 = ((uint32_t)lastData[0] << 24) |
                          ((uint32_t)lastData[1] << 16) |
                          ((uint32_t)lastData[2] << 8)  |
                          (uint32_t)lastData[3];
            updateCmd9FromNetwork(w3);
            lastNetW3 = w3;
        }

        lastNetTimestamp = timer_hw->timerawl;
        hasNetState = true;
    }

    // Re-apply latched state or timeout
    if (hasNetState) {
        if (timer_hw->timerawl - lastNetTimestamp > 100000) {
            hasNetState = false;
        } else if (!gotPacket) {
            updateCmd9FromNetwork(lastNetW3);
        }
    }
}

void __no_inline_not_in_flash_func(DreamcastDriver::pollNetwork)() {
    if (ethernetInitialized) {
        pollEthernet();
    } else {
        pollUartRx();
    }
}

uint16_t __no_inline_not_in_flash_func(DreamcastDriver::mapRawGpioToDC)(
    uint32_t rawGpio, uint8_t* outLT, uint8_t* outRT) {
    uint32_t pressed = rawGpio & buttonGpioMask;
    *outLT = (pressed & triggerLTMask) ? 255 : 0;
    *outRT = (pressed & triggerRTMask) ? 255 : 0;

    uint16_t dc = 0xFFFF;
    uint32_t btnPressed = pressed & ~(triggerLTMask | triggerRTMask);
    while (btnPressed) {
        uint pin = __builtin_ctz(btnPressed);
        dc &= ~gpioDcButtonMap[pin];
        btnPressed &= btnPressed - 1;
    }
    return dc;
}

uint16_t __no_inline_not_in_flash_func(DreamcastDriver::mapButtonsToDC)(uint32_t gpButtons, uint8_t dpad) {
    uint16_t dc = 0xFFFF;

    if (gpButtons & GAMEPAD_MASK_B1) dc &= ~DC_BTN_A;
    if (gpButtons & GAMEPAD_MASK_B2) dc &= ~DC_BTN_B;
    if (gpButtons & GAMEPAD_MASK_B3) dc &= ~DC_BTN_X;
    if (gpButtons & GAMEPAD_MASK_B4) dc &= ~DC_BTN_Y;
    if (gpButtons & GAMEPAD_MASK_L1) dc &= ~DC_BTN_C;
    if (gpButtons & GAMEPAD_MASK_R1) dc &= ~DC_BTN_Z;
    if (gpButtons & GAMEPAD_MASK_S2) dc &= ~DC_BTN_START;

    if (dpad & GAMEPAD_MASK_UP)    dc &= ~DC_BTN_UP;
    if (dpad & GAMEPAD_MASK_DOWN)  dc &= ~DC_BTN_DOWN;
    if (dpad & GAMEPAD_MASK_LEFT)  dc &= ~DC_BTN_LEFT;
    if (dpad & GAMEPAD_MASK_RIGHT) dc &= ~DC_BTN_RIGHT;

    return dc;
}

// ============================================================
// Packet Builders — called at init, rebuild after CMD1 for port
// ============================================================

void DreamcastDriver::buildInfoPacket() {
    memset(infoPacketBuf, 0, sizeof(infoPacketBuf));

    infoPacketBuf[0] = MapleBus::calcBitPairs(sizeof(infoPacketBuf));
    infoPacketBuf[1] = maple_make_header(
        28, MAPLE_CTRL_ADDR, MAPLE_DC_ADDR, MAPLE_CMD_RESPOND_DEVICE_STATUS
    );

    uint8_t infoBuf[112];
    memset(infoBuf, 0, sizeof(infoBuf));

    uint32_t funcHost = MAPLE_FUNC_CONTROLLER;
    infoBuf[0] = (funcHost >> 0) & 0xFF;
    infoBuf[1] = (funcHost >> 8) & 0xFF;
    infoBuf[2] = (funcHost >> 16) & 0xFF;
    infoBuf[3] = (funcHost >> 24) & 0xFF;

    uint32_t fd0 = 0x000F07FF;
    infoBuf[4] = (fd0 >> 0) & 0xFF;
    infoBuf[5] = (fd0 >> 8) & 0xFF;
    infoBuf[6] = (fd0 >> 16) & 0xFF;
    infoBuf[7] = (fd0 >> 24) & 0xFF;

    infoBuf[16] = 0xFF;
    infoBuf[17] = 0;

    const char* name = "GP2040-CE NOBD Controller";
    for (int i = 0; i < 30; i++) {
        infoBuf[18 + i] = (name[i] && i < (int)strlen(name)) ? name[i] : ' ';
    }

    const char* license = "Open Source - github.com/OpenStickCommunity";
    for (int i = 0; i < 60; i++) {
        infoBuf[48 + i] = (license[i] && i < (int)strlen(license)) ? license[i] : ' ';
    }

    uint16_t standby = 430;
    infoBuf[108] = standby & 0xFF;
    infoBuf[109] = (standby >> 8) & 0xFF;

    uint16_t maxPwr = 500;
    infoBuf[110] = maxPwr & 0xFF;
    infoBuf[111] = (maxPwr >> 8) & 0xFF;

    uint32_t* info = &infoPacketBuf[2];
    for (int i = 0; i < 28; i++) {
        uint32_t hostWord = infoBuf[i*4]
                          | ((uint32_t)infoBuf[i*4+1] << 8)
                          | ((uint32_t)infoBuf[i*4+2] << 16)
                          | ((uint32_t)infoBuf[i*4+3] << 24);
        info[i] = __builtin_bswap32(hostWord);
    }

    infoPacketBuf[30] = MapleBus::calcCRC(&infoPacketBuf[1], 29);
}

void DreamcastDriver::buildExtInfoPacket() {
    memset(extInfoBuf, 0, sizeof(extInfoBuf));
    extInfoBuf[0] = MapleBus::calcBitPairs(sizeof(extInfoBuf));
    extInfoBuf[1] = maple_make_header(
        48, MAPLE_CTRL_ADDR, MAPLE_DC_ADDR, MAPLE_CMD_RESPOND_ALL_STATUS
    );
    // Copy device info payload from infoPacketBuf
    memcpy(&extInfoBuf[2], &infoPacketBuf[2], 28 * sizeof(uint32_t));
    extInfoBuf[50] = MapleBus::calcCRC(&extInfoBuf[1], 49);
}

void DreamcastDriver::buildControllerPacket() {
    memset(controllerPacketBuf, 0, sizeof(controllerPacketBuf));

    controllerPacketBuf[0] = MapleBus::calcBitPairs(sizeof(controllerPacketBuf));
    controllerPacketBuf[1] = maple_make_header(
        3, MAPLE_CTRL_ADDR, MAPLE_DC_ADDR, MAPLE_CMD_RESPOND_DATA_XFER
    );

    controllerPacketBuf[2] = maple_host_to_wire(MAPLE_FUNC_CONTROLLER);
    controllerPacketBuf[3] = ((uint32_t)0 << 24) | ((uint32_t)0 << 16)
                           | ((uint32_t)0xFF << 8) | 0xFF;
    controllerPacketBuf[4] = ((uint32_t)0x80 << 24) | ((uint32_t)0x80 << 16)
                           | ((uint32_t)0x80 << 8) | 0x80;
}

void DreamcastDriver::buildACKPacket() {
    memset(ackPacketBuf, 0, sizeof(ackPacketBuf));
    ackPacketBuf[0] = MapleBus::calcBitPairs(sizeof(ackPacketBuf));
    ackPacketBuf[1] = maple_make_header(
        0, MAPLE_CTRL_ADDR, MAPLE_DC_ADDR, MAPLE_CMD_RESPOND_ACK
    );
    ackPacketBuf[2] = MapleBus::calcCRC(&ackPacketBuf[1], 1);
}

void DreamcastDriver::buildResendPacket() {
    memset(resendPacketBuf, 0, sizeof(resendPacketBuf));
    resendPacketBuf[0] = MapleBus::calcBitPairs(sizeof(resendPacketBuf));
    resendPacketBuf[1] = maple_make_header(
        0, MAPLE_CTRL_ADDR, MAPLE_DC_ADDR, MAPLE_CMD_RESPOND_SEND_AGAIN
    );
    resendPacketBuf[2] = MapleBus::calcCRC(&resendPacketBuf[1], 1);
}

void DreamcastDriver::buildUnknownCmdPacket() {
    memset(unknownCmdBuf, 0, sizeof(unknownCmdBuf));
    unknownCmdBuf[0] = MapleBus::calcBitPairs(sizeof(unknownCmdBuf));
    unknownCmdBuf[1] = maple_make_header(
        0, MAPLE_CTRL_ADDR, MAPLE_DC_ADDR, MAPLE_CMD_RESPOND_UNKNOWN_CMD
    );
    unknownCmdBuf[2] = MapleBus::calcCRC(&unknownCmdBuf[1], 1);
}

// ============================================================
// P2 Packet Builders — same format, targets p2 buffers, no VMU
// ============================================================

void DreamcastDriver::buildInfoPacketP2() {
    memset(p2InfoPacketBuf, 0, sizeof(p2InfoPacketBuf));
    // Copy P1 info packet as template (same device info)
    memcpy(p2InfoPacketBuf, infoPacketBuf, sizeof(p2InfoPacketBuf));
}

void DreamcastDriver::buildExtInfoPacketP2() {
    memset(p2ExtInfoBuf, 0, sizeof(p2ExtInfoBuf));
    memcpy(p2ExtInfoBuf, extInfoBuf, sizeof(p2ExtInfoBuf));
}

void DreamcastDriver::buildControllerPacketP2() {
    memset(p2ControllerPacketBuf, 0, sizeof(p2ControllerPacketBuf));
    memcpy(p2ControllerPacketBuf, controllerPacketBuf, sizeof(p2ControllerPacketBuf));
}

void DreamcastDriver::buildACKPacketP2() {
    memset(p2AckPacketBuf, 0, sizeof(p2AckPacketBuf));
    memcpy(p2AckPacketBuf, ackPacketBuf, sizeof(p2AckPacketBuf));
}

void DreamcastDriver::buildResendPacketP2() {
    memset(p2ResendPacketBuf, 0, sizeof(p2ResendPacketBuf));
    memcpy(p2ResendPacketBuf, resendPacketBuf, sizeof(p2ResendPacketBuf));
}

void DreamcastDriver::buildUnknownCmdPacketP2() {
    memset(p2UnknownCmdBuf, 0, sizeof(p2UnknownCmdBuf));
    memcpy(p2UnknownCmdBuf, unknownCmdBuf, sizeof(p2UnknownCmdBuf));
}

void DreamcastDriver::rebuildAllPacketsForPortP2(uint8_t port) {
    // P2 has no VMU — no sub-peripheral bits
    uint8_t origin = (MAPLE_CTRL_ADDR & MAPLE_PERIPH_MASK) | port;
    uint8_t dest   = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | port;

    p2InfoPacketBuf[1] = maple_make_header(28, origin, dest, MAPLE_CMD_RESPOND_DEVICE_STATUS);
    p2InfoPacketBuf[30] = MapleBus::calcCRC(&p2InfoPacketBuf[1], 29);

    p2ExtInfoBuf[1] = maple_make_header(48, origin, dest, MAPLE_CMD_RESPOND_ALL_STATUS);
    p2ExtInfoBuf[50] = MapleBus::calcCRC(&p2ExtInfoBuf[1], 49);

    p2AckPacketBuf[1] = maple_make_header(0, origin, dest, MAPLE_CMD_RESPOND_ACK);
    p2AckPacketBuf[2] = MapleBus::calcCRC(&p2AckPacketBuf[1], 1);

    p2ResendPacketBuf[1] = maple_make_header(0, origin, dest, MAPLE_CMD_RESPOND_SEND_AGAIN);
    p2ResendPacketBuf[2] = MapleBus::calcCRC(&p2ResendPacketBuf[1], 1);

    p2UnknownCmdBuf[1] = maple_make_header(0, origin, dest, MAPLE_CMD_RESPOND_UNKNOWN_CMD);
    p2UnknownCmdBuf[2] = MapleBus::calcCRC(&p2UnknownCmdBuf[1], 1);

    p2ControllerPacketBuf[1] = maple_make_header(3, origin, dest, MAPLE_CMD_RESPOND_DATA_XFER);

    // Cache CRC constant for network input
    uint32_t w1 = p2ControllerPacketBuf[1];
    uint32_t w2 = p2ControllerPacketBuf[2];
    uint32_t w4 = p2ControllerPacketBuf[4];
    p2CachedCrcXorConst = w1 ^ w2 ^ w4;

    // Compute initial W5 CRC for current W3 (all buttons released)
    uint32_t xorAll = p2CachedCrcXorConst ^ p2Cmd9ReadyW3;
    uint8_t crc = (xorAll >> 24) ^ (xorAll >> 16) ^ (xorAll >> 8) ^ xorAll;
    p2Cmd9ReadyW5 = (uint32_t)crc << 24;
}

// ============================================================
// Rebuild all static packets after CMD1 reveals the real port
// ============================================================

void DreamcastDriver::rebuildAllPacketsForPort(uint8_t port) {
    uint8_t subPeriphBits = disableVMU ? 0 : MAPLE_SUB0_ADDR;
    uint8_t origin = (MAPLE_CTRL_ADDR & MAPLE_PERIPH_MASK) | subPeriphBits | port;
    uint8_t dest   = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | port;

    // Info packet (CMD1 response)
    infoPacketBuf[1] = maple_make_header(28, origin, dest, MAPLE_CMD_RESPOND_DEVICE_STATUS);
    infoPacketBuf[30] = MapleBus::calcCRC(&infoPacketBuf[1], 29);

    // Extended info (CMD2 response)
    extInfoBuf[1] = maple_make_header(48, origin, dest, MAPLE_CMD_RESPOND_ALL_STATUS);
    extInfoBuf[50] = MapleBus::calcCRC(&extInfoBuf[1], 49);

    // ACK (CMD3/4/14 response)
    ackPacketBuf[1] = maple_make_header(0, origin, dest, MAPLE_CMD_RESPOND_ACK);
    ackPacketBuf[2] = MapleBus::calcCRC(&ackPacketBuf[1], 1);

    // Resend request
    uint8_t resendOrigin = (MAPLE_CTRL_ADDR & MAPLE_PERIPH_MASK) | port;
    resendPacketBuf[1] = maple_make_header(0, resendOrigin, dest, MAPLE_CMD_RESPOND_SEND_AGAIN);
    resendPacketBuf[2] = MapleBus::calcCRC(&resendPacketBuf[1], 1);

    // Unknown command response
    unknownCmdBuf[1] = maple_make_header(0, origin, dest, MAPLE_CMD_RESPOND_UNKNOWN_CMD);
    unknownCmdBuf[2] = MapleBus::calcCRC(&unknownCmdBuf[1], 1);

    // Controller state header (word 1 — used by ISR for CMD9)
    controllerPacketBuf[1] = maple_make_header(3, origin, dest, MAPLE_CMD_RESPOND_DATA_XFER);

    // Rebuild CMD9 lookup table CRC with real port
    rebuildCmd9LookupTableForPort();
}

// ============================================================
// Simplified process() — ISR handles all commands, main loop just cleans up
// ============================================================

void DreamcastDriver::processAux() {
}

void __no_inline_not_in_flash_func(DreamcastDriver::processP2)(Gamepad* gamepad) {
    if (!p2Enabled) return;

    // P2 ISR TX cleanup — same as P1 process() but for busP2, no VMU
    if (busP2.isrTxFired) {
        busP2.isrTxFired = false;
        busP2.finishIsrTx();
        return;
    }

    if (busP2.isrNvicDisabled) {
        // CRC fail on P2 — just restart RX
        const uint8_t* packet = nullptr;
        uint rxLen = 0;
        busP2.pollReceive(&packet, &rxLen);
        return;
    }
}

void __no_inline_not_in_flash_func(DreamcastDriver::waitTxFlushRx)() {
    bus.flushRx();
}

void __no_inline_not_in_flash_func(DreamcastDriver::process)(Gamepad* gamepad) {
    const bool diag = enableDiagnostics;
    bus.enableDiagnostics = diag;

    if (diag) debugXorFail = bus.debugXorFail;

    // ISR sent a response — wait for TX to complete and restart RX.
    if (bus.isrTxFired) {
        bus.isrTxFired = false;
        bus.finishIsrTx();
        if (diag) {
            uint32_t elapsed = bus.isrTxStartTimestamp - bus.rxArrivalTimestamp;
            respLast = elapsed;
            if (elapsed < respMin) respMin = elapsed;
            if (elapsed > respMax) respMax = elapsed;
            respCount++;
        }
        return;
    }

    // ISR couldn't handle the packet (VMU command, CRC fail) — main loop handles.
    if (bus.isrNvicDisabled) {
        const uint8_t* packet = nullptr;
        uint rxLen = 0;
        bool gotPacket = bus.pollReceive(&packet, &rxLen);

        if (!gotPacket && bus.wasLastRxCorrupt()) {
            // CRC fail on incoming — send RESEND
            bus.sendPacket(resendPacketBuf, 3);
            waitTxFlushRx();
            if (diag) debugResendCount++;
            return;
        }

        if (gotPacket && rxLen >= 4) {
            uint32_t hdrWord = ((const uint32_t*)packet)[0];
            uint8_t destAddr = maple_hdr_dest(hdrWord) & MAPLE_PERIPH_MASK;
            uint8_t port = maple_hdr_origin(hdrWord) & MAPLE_PORT_MASK;

            // VMU commands — route to existing handleCommand
            if (!disableVMU && destAddr == MAPLE_SUB0_ADDR) {
                if (vmuReady && vmu.handleCommand(packet, rxLen, port, bus)) {
                    waitTxFlushRx();
                }
                return;
            }
        }
    }
}
