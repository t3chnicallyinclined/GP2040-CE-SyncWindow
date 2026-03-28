#include "drivers/dreamcast/DreamcastDriver.h"
#include "gamepad.h"
#include "gamepad/GamepadState.h"
#include "storagemanager.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"

static DreamcastDriver* irqDriverInstance = nullptr;

// ============================================================
// ISR Command Dispatch — handles ALL Maple Bus commands from ISR
// ============================================================

void __no_inline_not_in_flash_func(DreamcastDriver::isrCommandDispatch)(MapleBus* bus) {
    DreamcastDriver* drv = irqDriverInstance;
    if (!drv) return;

    uint32_t hdr = bus->rxDmaBuf[0];
    int8_t cmd = maple_hdr_command(hdr);
    uint8_t destAddr = maple_hdr_dest(hdr) & MAPLE_PERIPH_MASK;
    uint8_t port = maple_hdr_origin(hdr) & MAPLE_PORT_MASK;

    // Route VMU sub-peripheral commands
    if (!drv->disableVMU && destAddr == MAPLE_SUB0_ADDR) {
        if (!drv->vmuReady) return;  // No response — DC will retry
        drv->isrHandleVmuCommand(cmd, hdr, port, bus);
        return;
    }

    // Only handle controller-addressed packets
    if (destAddr != MAPLE_CTRL_ADDR) return;

    // Compute origin/dest with sub-peripheral bits for response header
    uint8_t subPeriphBits = drv->disableVMU ? 0 : MAPLE_SUB0_ADDR;
    uint8_t origin = (MAPLE_CTRL_ADDR & MAPLE_PERIPH_MASK) | subPeriphBits | port;
    uint8_t dest = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | port;

    switch (cmd) {
        case MAPLE_CMD_GET_CONDITION: {
            // Fastest path — pre-computed lookup table.
            // If port is known and matches cached, use pre-computed W5 (CRC).
            // Otherwise compute CRC inline (~5 cycles).
            uint32_t w1 = maple_make_header(3, origin, dest, MAPLE_CMD_RESPOND_DATA_XFER);
            uint32_t w3 = drv->cmd9ReadyW3;

            drv->controllerPacketBuf[1] = w1;
            drv->controllerPacketBuf[3] = w3;

            if (drv->portKnown && port == drv->cachedPort) {
                // Use pre-computed CRC from lookup table
                drv->controllerPacketBuf[5] = drv->cmd9ReadyW5;
            } else {
                // Compute CRC inline — port changed or not yet cached
                uint32_t xorAll = w1 ^ drv->controllerPacketBuf[2] ^ w3 ^ drv->controllerPacketBuf[4];
                uint8_t crc = (xorAll >> 24) ^ (xorAll >> 16) ^ (xorAll >> 8) ^ xorAll;
                drv->controllerPacketBuf[5] = (uint32_t)crc << 24;
            }

            bus->isrSendFifo(drv->controllerPacketBuf, 6);
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
            // Cache port on first CMD1 — used to pre-compute all packet CRCs
            if (!drv->portKnown || port != drv->cachedPort) {
                drv->cachedPort = port;
                drv->portKnown = true;
                drv->rebuildAllPacketsForPort(port);
            }
            // infoPacketBuf is pre-built with correct port+CRC
            bus->isrSendDma(drv->infoPacketBuf, 31);
            drv->vmuReady = true;
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
            // extInfoBuf pre-built with correct port+CRC
            bus->isrSendDma(drv->extInfoBuf, 51);
            if (drv->enableDiagnostics) {
                drv->debugRxCount++;
                drv->debugTxCount++;
            }
            break;
        }

        case MAPLE_CMD_SET_CONDITION: {
            // Vibrate command — ACK and track
            bus->isrSendFifo(drv->ackPacketBuf, 3);
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
            // ackPacketBuf pre-built with correct port+CRC
            bus->isrSendFifo(drv->ackPacketBuf, 3);
            if (drv->enableDiagnostics) {
                drv->debugRxCount++;
                drv->debugTxCount++;
            }
            break;
        }

        default: {
            // unknownCmdBuf pre-built with correct port+CRC
            bus->isrSendFifo(drv->unknownCmdBuf, 3);
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
    // No STD/ZL mode distinction for DC.
    irqDriverInstance = this;
    bus.enableIsrDispatch(isrCommandDispatch);

    connected = true;
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
