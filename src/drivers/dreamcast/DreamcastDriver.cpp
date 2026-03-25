#include "drivers/dreamcast/DreamcastDriver.h"
#include "gamepad.h"
#include "gamepad/GamepadState.h"
#include "storagemanager.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"

// ============================================================
// Level 3.5: ISR GPIO capture callback
// Called from mapleRxIrqHandler when a valid CMD 9 is received.
// Reads GPIO at the freshest moment and pre-builds the response.
// Does NOT send — main loop handles TX for sequential safety.
// ZERO overhead when diagnostics are off (no timer reads).
// ============================================================
static DreamcastDriver* irqDriverInstance = nullptr;

static void __no_inline_not_in_flash_func(cmd9GpioCapture)(uint32_t hdr, MapleBus* bus) {
    DreamcastDriver* drv = irqDriverInstance;
    if (!drv) return;

    uint32_t freshGpio = ~gpio_get_all();
    uint8_t lt, rt;
    uint16_t dcButtons = drv->mapRawGpioToDC(freshGpio, &lt, &rt);

    uint8_t jx = drv->cachedJX;
    uint8_t jy = drv->cachedJY;

    uint8_t subPeriphBits = drv->disableVMU ? 0 : MAPLE_SUB0_ADDR;
    uint8_t lastPort = maple_hdr_origin(hdr) & MAPLE_PORT_MASK;
    uint8_t origin = (MAPLE_CTRL_ADDR & MAPLE_PERIPH_MASK) | subPeriphBits | lastPort;
    uint8_t dest = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | lastPort;

    drv->controllerPacketBuf[1] = maple_make_header(3, origin, dest, MAPLE_CMD_RESPOND_DATA_XFER);
    drv->controllerPacketBuf[3] = ((uint32_t)lt << 24) | ((uint32_t)rt << 16)
                                | ((uint32_t)((dcButtons >> 8) & 0xFF) << 8)
                                | (dcButtons & 0xFF);
    drv->controllerPacketBuf[4] = ((uint32_t)0x80 << 24) | ((uint32_t)0x80 << 16)
                                | ((uint32_t)jy << 8) | jx;
    drv->controllerPacketBuf[5] = MapleBus::calcCRC(&drv->controllerPacketBuf[1], 4);
}

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

DreamcastDriver::DreamcastDriver()
    : connected(false), lastPort(0) {
}

bool DreamcastDriver::init(uint pin_a, uint pin_b) {
    if (!bus.init(pin_a, pin_b)) return false;

    const GamepadOptions& options = Storage::getInstance().getGamepadOptions();
    disableVMU = options.disableVMU;

    buildInfoPacket();
    buildControllerPacket();
    buildACKPacket();
    buildResendPacket();

    if (!disableVMU) {
        vmu.init();
    }

    buildGpioDcMap();

    irqDriverInstance = this;
    if (zeroLatencyMode) {
        bus.enableFastPath(cmd9GpioCapture);
    }

    connected = true;
    return true;
}

void DreamcastDriver::setFastPath(bool enable) {
    // Reset timing stats on mode change
    respLast = 0; respMin = 0xFFFFFFFF; respMax = 0; respCount = 0;

    if (enable) {
        irqDriverInstance = this;
        bus.enableFastPath(cmd9GpioCapture);
    } else {
        bus.disableFastPath();
    }
}

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

void DreamcastDriver::buildInfoPacket() {
    memset(infoPacketBuf, 0, sizeof(infoPacketBuf));

    infoPacketBuf[0] = MapleBus::calcBitPairs(sizeof(infoPacketBuf));
    infoPacketBuf[1] = maple_make_header(
        28, MAPLE_CTRL_ADDR, MAPLE_DC_ADDR, MAPLE_CMD_RESPOND_DEVICE_STATUS
    );

    uint32_t* info = &infoPacketBuf[2];

    info[0] = maple_host_to_wire(MAPLE_FUNC_CONTROLLER);
    info[1] = maple_host_to_wire(0x000F07FF);
    info[2] = 0;
    info[3] = 0;
    info[4] = maple_host_to_wire((uint32_t)0xFF << 24);

    buildDevInfoString(&info[5], "GP2040-CE NOBD Controller", 30);
    buildDevInfoString(&info[5], "GP2040-CE NOBD Controller     ", 32);
    buildDevInfoString(&info[13], "Open Source - github.com/OpenStickCommunity              ", 60);

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

    for (int i = 0; i < 28; i++) {
        uint32_t hostWord = infoBuf[i*4]
                          | ((uint32_t)infoBuf[i*4+1] << 8)
                          | ((uint32_t)infoBuf[i*4+2] << 16)
                          | ((uint32_t)infoBuf[i*4+3] << 24);
        info[i] = __builtin_bswap32(hostWord);
    }
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
}

void DreamcastDriver::buildResendPacket() {
    memset(resendPacketBuf, 0, sizeof(resendPacketBuf));

    resendPacketBuf[0] = MapleBus::calcBitPairs(sizeof(resendPacketBuf));
    resendPacketBuf[1] = maple_make_header(
        0, MAPLE_CTRL_ADDR, MAPLE_DC_ADDR, MAPLE_CMD_RESPOND_SEND_AGAIN
    );
}

void __no_inline_not_in_flash_func(DreamcastDriver::sendResendRequest)() {
    uint8_t origin = (MAPLE_CTRL_ADDR & MAPLE_PERIPH_MASK) | lastPort;
    uint8_t dest = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | lastPort;
    resendPacketBuf[1] = maple_make_header(0, origin, dest, MAPLE_CMD_RESPOND_SEND_AGAIN);
    resendPacketBuf[2] = MapleBus::calcCRC(&resendPacketBuf[1], 1);
    bus.sendPacket(resendPacketBuf, 3);
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

void __no_inline_not_in_flash_func(DreamcastDriver::sendControllerState)(Gamepad* gamepad) {
    uint16_t dcButtons;
    uint8_t lt, rt, jx, jy;

    if (zeroLatencyMode) {
        uint32_t freshGpio = ~gpio_get_all();
        dcButtons = mapRawGpioToDC(freshGpio, &lt, &rt);

        uint32_t lx32 = (uint32_t)gamepad->state.lx + 0x80;
        uint32_t ly32 = (uint32_t)gamepad->state.ly + 0x80;
        jx = (uint8_t)(lx32 > 0xFFFF ? 0xFF : (lx32 >> 8));
        jy = (uint8_t)(ly32 > 0xFFFF ? 0xFF : (ly32 >> 8));
    } else {
        uint32_t buttons = gamepad->state.buttons;
        uint8_t  dpad    = gamepad->state.dpad;
        lt = gamepad->state.lt;
        rt = gamepad->state.rt;
        if (lt == 0 && (buttons & GAMEPAD_MASK_L2)) lt = 255;
        if (rt == 0 && (buttons & GAMEPAD_MASK_R2)) rt = 255;

        dcButtons = mapButtonsToDC(buttons, dpad);

        uint32_t lx32 = (uint32_t)gamepad->state.lx + 0x80;
        uint32_t ly32 = (uint32_t)gamepad->state.ly + 0x80;
        jx = (uint8_t)(lx32 > 0xFFFF ? 0xFF : (lx32 >> 8));
        jy = (uint8_t)(ly32 > 0xFFFF ? 0xFF : (ly32 >> 8));
    }

    uint8_t subPeriphBits = disableVMU ? 0 : MAPLE_SUB0_ADDR;
    uint8_t origin = (MAPLE_CTRL_ADDR & MAPLE_PERIPH_MASK) | subPeriphBits | lastPort;
    uint8_t dest = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | lastPort;

    controllerPacketBuf[1] = maple_make_header(3, origin, dest, MAPLE_CMD_RESPOND_DATA_XFER);

    controllerPacketBuf[3] = ((uint32_t)lt << 24) | ((uint32_t)rt << 16)
                           | ((uint32_t)((dcButtons >> 8) & 0xFF) << 8)
                           | (dcButtons & 0xFF);

    controllerPacketBuf[4] = ((uint32_t)0x80 << 24) | ((uint32_t)0x80 << 16)
                           | ((uint32_t)jy << 8) | jx;

    controllerPacketBuf[5] = MapleBus::calcCRC(&controllerPacketBuf[1], 4);

    bus.sendPacket(controllerPacketBuf, 6);
}

void __no_inline_not_in_flash_func(DreamcastDriver::sendInfoResponse)() {
    uint8_t subPeriphBits = disableVMU ? 0 : MAPLE_SUB0_ADDR;
    uint8_t origin = (MAPLE_CTRL_ADDR & MAPLE_PERIPH_MASK) | subPeriphBits | lastPort;
    uint8_t dest = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | lastPort;

    infoPacketBuf[1] = maple_make_header(28, origin, dest, MAPLE_CMD_RESPOND_DEVICE_STATUS);
    infoPacketBuf[30] = MapleBus::calcCRC(&infoPacketBuf[1], 29);
    bus.sendPacket(infoPacketBuf, 31);
}

void __no_inline_not_in_flash_func(DreamcastDriver::sendExtInfoResponse)() {
    static uint32_t extInfoBuf[51];
    memset(extInfoBuf, 0, sizeof(extInfoBuf));

    uint8_t subPeriphBits = disableVMU ? 0 : MAPLE_SUB0_ADDR;
    uint8_t origin = (MAPLE_CTRL_ADDR & MAPLE_PERIPH_MASK) | subPeriphBits | lastPort;
    uint8_t dest = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | lastPort;

    extInfoBuf[0] = MapleBus::calcBitPairs(sizeof(extInfoBuf));
    extInfoBuf[1] = maple_make_header(48, origin, dest, MAPLE_CMD_RESPOND_ALL_STATUS);

    memcpy(&extInfoBuf[2], &infoPacketBuf[2], 28 * sizeof(uint32_t));

    extInfoBuf[50] = MapleBus::calcCRC(&extInfoBuf[1], 49);
    bus.sendPacket(extInfoBuf, 51);
}

void __no_inline_not_in_flash_func(DreamcastDriver::sendACKResponse)() {
    uint8_t subPeriphBits = disableVMU ? 0 : MAPLE_SUB0_ADDR;
    uint8_t origin = (MAPLE_CTRL_ADDR & MAPLE_PERIPH_MASK) | subPeriphBits | lastPort;
    uint8_t dest = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | lastPort;

    ackPacketBuf[1] = maple_make_header(0, origin, dest, MAPLE_CMD_RESPOND_ACK);
    ackPacketBuf[2] = MapleBus::calcCRC(&ackPacketBuf[1], 1);
    bus.sendPacket(ackPacketBuf, 3);
}

void __no_inline_not_in_flash_func(DreamcastDriver::sendUnknownCommandResponse)() {
    static uint32_t unknownBuf[3];

    unknownBuf[0] = MapleBus::calcBitPairs(sizeof(unknownBuf));
    uint8_t subPeriphBits = disableVMU ? 0 : MAPLE_SUB0_ADDR;
    uint8_t origin = (MAPLE_CTRL_ADDR & MAPLE_PERIPH_MASK) | subPeriphBits | lastPort;
    uint8_t dest = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | lastPort;

    unknownBuf[1] = maple_make_header(0, origin, dest, MAPLE_CMD_RESPOND_UNKNOWN_CMD);
    unknownBuf[2] = MapleBus::calcCRC(&unknownBuf[1], 1);
    bus.sendPacket(unknownBuf, 3);
}

void DreamcastDriver::processAux() {
}

void __no_inline_not_in_flash_func(DreamcastDriver::waitTxFlushRx)() {
    bus.flushRx();
}

void __no_inline_not_in_flash_func(DreamcastDriver::process)(Gamepad* gamepad) {
    const bool diag = enableDiagnostics;
    bus.enableDiagnostics = diag;

    if (zeroLatencyMode) {
        uint32_t lx32 = (uint32_t)gamepad->state.lx + 0x80;
        uint32_t ly32 = (uint32_t)gamepad->state.ly + 0x80;
        cachedJX = (uint8_t)(lx32 > 0xFFFF ? 0xFF : (lx32 >> 8));
        cachedJY = (uint8_t)(ly32 > 0xFFFF ? 0xFF : (ly32 >> 8));
    }

    if (diag) debugXorFail = bus.debugXorFail;

    if (bus.cmd9PreBuilt) {
        bus.cmd9PreBuilt = false;
        bus.clearRxAfterFastPath();
        bus.sendPacket(controllerPacketBuf, 6);
        if (diag) {
            uint32_t elapsed = timer_hw->timerawl - bus.rxArrivalTimestamp;
            respLast = elapsed;
            if (elapsed < respMin) respMin = elapsed;
            if (elapsed > respMax) respMax = elapsed;
            respCount++;
            debugCmd9Count++; debugTxCount++;
        }
        waitTxFlushRx();
        return;
    }

    // In ZL mode, CMD 9 is handled by the ISR fast path.
    // Only fall through to pollReceive for non-CMD-9 packets (VMU, device info).
    // If cmd9PreBuilt was false and we're in ZL mode, the ISR hasn't fired yet —
    // don't race it with pollReceive. Just return and let the ISR handle it next time.
    // Exception: we still need pollReceive for CMD 1 (device info) at connection time,
    // so only skip once vmuReady is true (meaning we've already connected).
    if (zeroLatencyMode && vmuReady) {
        return;
    }

    const uint8_t* packet = nullptr;
    uint rxLen = 0;
    bool gotPacket = bus.pollReceive(&packet, &rxLen);

    if (!gotPacket && bus.wasLastRxCorrupt()) {
        sendResendRequest();
        waitTxFlushRx();
        if (diag) debugResendCount++;
        return;
    }

    if (gotPacket && rxLen >= 4) {

        uint32_t hdrWord = ((const uint32_t*)packet)[0];
        int8_t cmd = maple_hdr_command(hdrWord);
        uint8_t destAddr = maple_hdr_dest(hdrWord) & MAPLE_PERIPH_MASK;
        lastPort = maple_hdr_origin(hdrWord) & MAPLE_PORT_MASK;

        if (!disableVMU && destAddr == MAPLE_SUB0_ADDR) {
            if (!vmuReady) return;
            if (vmu.handleCommand(packet, rxLen, lastPort, bus)) {
                waitTxFlushRx();
            }
            return;
        }

        if (destAddr != MAPLE_CTRL_ADDR) return;

        if (diag) debugRxCount++;

        switch (cmd) {
            case MAPLE_CMD_DEVICE_REQUEST:
                if (diag) {
                    debugCmd1Count++;
                    if (debugConsecutivePolls > debugMaxConsecutivePolls)
                        debugMaxConsecutivePolls = debugConsecutivePolls;
                    debugConsecutivePolls = 0;
                }
                sendInfoResponse();
                waitTxFlushRx();
                if (diag) debugTxCount++;
                vmuReady = true;
                break;

            case MAPLE_CMD_ALL_STATUS_REQUEST:
                sendExtInfoResponse();
                waitTxFlushRx();
                if (diag) debugTxCount++;
                break;

            case MAPLE_CMD_GET_CONDITION:
                sendControllerState(gamepad);
                if (diag) {
                    uint32_t elapsed = timer_hw->timerawl - bus.rxArrivalTimestamp;
                    respLast = elapsed;
                    if (elapsed < respMin) respMin = elapsed;
                    if (elapsed > respMax) respMax = elapsed;
                    respCount++;
                    debugCmd9Count++; debugConsecutivePolls++;
                    debugTxCount++;
                }
                waitTxFlushRx();
                break;

            case MAPLE_CMD_RESET_DEVICE:
            case MAPLE_CMD_SHUTDOWN_DEVICE:
                sendACKResponse();
                waitTxFlushRx();
                if (diag) debugTxCount++;
                break;

            case MAPLE_CMD_SET_CONDITION:
                sendACKResponse();
                waitTxFlushRx();
                if (diag) debugTxCount++;
                break;

            default:
                sendUnknownCommandResponse();
                waitTxFlushRx();
                if (diag) debugTxCount++;
                break;
        }
    }
}
