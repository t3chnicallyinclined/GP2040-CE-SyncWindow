#include "drivers/dreamcast/DreamcastDriver.h"
#include "gamepad.h"
#include "gamepad/GamepadState.h"
#include "pico/time.h"
#include "hardware/gpio.h"

// Dreamcast button masks (inverted: 0=pressed, 1=released)
#define DC_BTN_C      0x0001
#define DC_BTN_B      0x0002
#define DC_BTN_A      0x0004
#define DC_BTN_START  0x0008
#define DC_BTN_UP     0x0010
#define DC_BTN_DOWN   0x0020
#define DC_BTN_LEFT   0x0040
#define DC_BTN_RIGHT  0x0080
#define DC_BTN_Z      0x0100
#define DC_BTN_Y      0x0200
#define DC_BTN_X      0x0400

DreamcastDriver* DreamcastDriver::instance = nullptr;

DreamcastDriver::DreamcastDriver()
    : connected(false), lastPollUs(0), pollIntervalUs(16670),
      firstPoll(true), syncMode(DC_SYNC_ACCUMULATE), syncWindowMs(4),
      accumulatedButtons(0), accumulatedDpad(0),
      accumulatedLT(0), accumulatedRT(0),
      accumulatedJoyX(0x80), accumulatedJoyY(0x80),
      holdingResponse(false), holdStartUs(0), lastPressUs(0),
      prevButtons(0), prevDpad(0), pendingPoll(false), lastPort(0) {
    instance = this;
}

bool DreamcastDriver::init(uint pin_a, uint pin_b) {
    debugPinA = pin_a;
    debugPinB = pin_b;
    if (!bus.init(pin_a, pin_b)) return false;

    buildInfoPacket();
    buildControllerPacket();
    buildACKPacket();

    connected = true;
    return true;
}

void DreamcastDriver::buildInfoPacket() {
    memset(&infoPacket, 0, sizeof(infoPacket));

    infoPacket.bitPairsMinus1 = MapleBus::calcBitPairs(sizeof(infoPacket));
    infoPacket.header.command     = MAPLE_CMD_RESPOND_DEVICE_STATUS;
    infoPacket.header.destination = MAPLE_DC_ADDR;
    infoPacket.header.origin      = MAPLE_CTRL_ADDR;
    infoPacket.header.numWords    = sizeof(MapleDeviceInfo) / sizeof(uint32_t);

    // HKT-7300 arcade stick — 11 buttons
    // Store native values — sendPacket's bswap32 handles wire byte order
    infoPacket.info.func        = MAPLE_FUNC_CONTROLLER;
    infoPacket.info.funcData[0] = 0x000007FF;
    infoPacket.info.funcData[1] = 0;
    infoPacket.info.funcData[2] = 0;
    infoPacket.info.areaCode    = -1;
    infoPacket.info.connectorDirection = 0;

    strncpy(infoPacket.info.productName,
            "GP2040-CE NOBD Controller     ",
            sizeof(infoPacket.info.productName));
    strncpy(infoPacket.info.productLicense,
            "Open Source - github.com/OpenStickCommunity                  ",
            sizeof(infoPacket.info.productLicense));

    // Power values stored native — sendPacket's bswap32 handles wire byte order
    infoPacket.info.standbyPower = 430;
    infoPacket.info.maxPower     = 500;
}

void DreamcastDriver::buildControllerPacket() {
    memset(&controllerPacket, 0, sizeof(controllerPacket));

    controllerPacket.bitPairsMinus1 = MapleBus::calcBitPairs(sizeof(controllerPacket));
    controllerPacket.header.command     = MAPLE_CMD_RESPOND_DATA_XFER;
    controllerPacket.header.destination = MAPLE_DC_ADDR;
    controllerPacket.header.origin      = MAPLE_CTRL_ADDR;
    controllerPacket.header.numWords    = sizeof(MapleControllerCondition) / sizeof(uint32_t);

    controllerPacket.controller.condition    = MAPLE_FUNC_CONTROLLER;
    controllerPacket.controller.buttonsHi    = 0xFF;
    controllerPacket.controller.buttonsLo    = 0xFF;
    controllerPacket.controller.leftTrigger  = 0;
    controllerPacket.controller.rightTrigger = 0;
    controllerPacket.controller.joyX  = 0x80;
    controllerPacket.controller.joyY  = 0x80;
    controllerPacket.controller.joyX2 = 0x80;
    controllerPacket.controller.joyY2 = 0x80;
}

void DreamcastDriver::buildACKPacket() {
    memset(&ackPacket, 0, sizeof(ackPacket));

    ackPacket.bitPairsMinus1 = MapleBus::calcBitPairs(sizeof(ackPacket));
    ackPacket.header.command     = MAPLE_CMD_RESPOND_ACK;
    ackPacket.header.destination = MAPLE_DC_ADDR;
    ackPacket.header.origin      = MAPLE_CTRL_ADDR;
    ackPacket.header.numWords    = 0;
}

uint16_t DreamcastDriver::mapButtonsToDC(uint32_t gpButtons, uint8_t dpad) {
    uint16_t dc = 0xFFFF; // All released (inverted logic)

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

void DreamcastDriver::updateAccumulatedState(Gamepad* gamepad) {
    uint32_t currentButtons = gamepad->state.buttons;
    uint8_t currentDpad = gamepad->state.dpad;

    uint32_t newPresses = currentButtons & ~prevButtons;
    uint8_t newDpadPresses = currentDpad & ~prevDpad;

    if (newPresses || newDpadPresses) {
        lastPressUs = time_us_64();
    }

    accumulatedButtons |= newPresses;
    accumulatedButtons &= currentButtons;

    accumulatedDpad |= newDpadPresses;
    accumulatedDpad &= currentDpad;

    if (gamepad->state.lt > accumulatedLT) accumulatedLT = gamepad->state.lt;
    if (gamepad->state.rt > accumulatedRT) accumulatedRT = gamepad->state.rt;
    accumulatedJoyX = (uint8_t)(gamepad->state.lx >> 8);
    accumulatedJoyY = (uint8_t)(gamepad->state.ly >> 8);

    prevButtons = currentButtons;
    prevDpad = currentDpad;
}

void DreamcastDriver::sendControllerState(Gamepad* gamepad) {
    uint16_t dcButtons;
    uint8_t lt, rt, jx, jy;

    if (syncMode == DC_SYNC_OFF) {
        dcButtons = mapButtonsToDC(gamepad->state.buttons, gamepad->state.dpad);
        lt = gamepad->state.lt;
        rt = gamepad->state.rt;
        jx = (uint8_t)(gamepad->state.lx >> 8);
        jy = (uint8_t)(gamepad->state.ly >> 8);
    } else {
        dcButtons = mapButtonsToDC(accumulatedButtons, accumulatedDpad);
        lt = accumulatedLT;
        rt = accumulatedRT;
        jx = accumulatedJoyX;
        jy = accumulatedJoyY;

        accumulatedButtons = gamepad->state.buttons;
        accumulatedDpad = gamepad->state.dpad;
        accumulatedLT = gamepad->state.lt;
        accumulatedRT = gamepad->state.rt;
    }

    if (gamepad->state.buttons & GAMEPAD_MASK_L2) lt = 255;
    if (gamepad->state.buttons & GAMEPAD_MASK_R2) rt = 255;

    // Set port bits in header
    controllerPacket.header.origin = (MAPLE_CTRL_ADDR & MAPLE_PERIPH_MASK) | lastPort;
    controllerPacket.header.destination = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | lastPort;

    controllerPacket.controller.buttonsHi    = (dcButtons >> 8) & 0xFF;
    controllerPacket.controller.buttonsLo    = dcButtons & 0xFF;
    controllerPacket.controller.leftTrigger   = lt;
    controllerPacket.controller.rightTrigger  = rt;
    controllerPacket.controller.joyX          = jx;
    controllerPacket.controller.joyY          = jy;

    debugDcButtons = dcButtons;
    debugGpButtons = gamepad->state.buttons;
    debugGpDpad = gamepad->state.dpad;

    // Recalculate CRC after setting port bits and button data
    controllerPacket.crc = MapleBus::calcCRC(
        (uint32_t*)&controllerPacket.header,
        sizeof(controllerPacket) / sizeof(uint32_t) - 2);

    bus.sendPacket((uint32_t*)&controllerPacket,
                   sizeof(controllerPacket) / sizeof(uint32_t));
}

void DreamcastDriver::sendInfoResponse() {
    infoPacket.header.origin = (MAPLE_CTRL_ADDR & MAPLE_PERIPH_MASK) | lastPort;
    infoPacket.header.destination = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | lastPort;
    infoPacket.crc = MapleBus::calcCRC(
        (uint32_t*)&infoPacket.header,
        sizeof(infoPacket) / sizeof(uint32_t) - 2);
    bus.sendPacket((uint32_t*)&infoPacket,
                   sizeof(infoPacket) / sizeof(uint32_t));
}

void DreamcastDriver::sendACKResponse() {
    ackPacket.header.origin = (MAPLE_CTRL_ADDR & MAPLE_PERIPH_MASK) | lastPort;
    ackPacket.header.destination = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | lastPort;
    ackPacket.crc = MapleBus::calcCRC(
        (uint32_t*)&ackPacket.header,
        sizeof(ackPacket) / sizeof(uint32_t) - 2);
    bus.sendPacket((uint32_t*)&ackPacket,
                   sizeof(ackPacket) / sizeof(uint32_t));
}

void DreamcastDriver::waitTxFlushRx() {
    // Wait for full TX completion (DMA + PIO FIFO + wire) then flush RX echo
    bus.flushRx();
}

void DreamcastDriver::process(Gamepad* gamepad) {
    uint64_t nowUs = time_us_64();

    // Debug: read raw GPIO pin state
    debugPinState = (gpio_get(debugPinA) ? 1 : 0) | (gpio_get(debugPinB) ? 2 : 0);

    // Always accumulate button state between polls (modes 1 & 2)
    if (syncMode != DC_SYNC_OFF) {
        updateAccumulatedState(gamepad);
    }

    // Poll for decoded packets
    const uint8_t* packet = nullptr;
    uint rxLen = 0;

    debugFifoCount = bus.getFifoReadCount();
    debugSyncCount = bus.debugSyncCount;
    debugEndCount = bus.debugEndCount;
    debugMaxBytes = bus.debugMaxWritePos;
    debugLastXor = bus.debugLastXor;
    debugLastBitCnt = bus.debugLastBitCount;
    debugPollTrue = bus.debugPollTrue;
    debugXorFail = bus.debugXorFail;
    debugFlushCount = bus.debugFlushCount;
    if (bus.pollReceive(&packet, &rxLen) && rxLen >= sizeof(MaplePacketHeader)) {
        debugFifoCount = bus.getFifoReadCount();
        debugRxCount++;
        const MaplePacketHeader* header = (const MaplePacketHeader*)packet;
        debugLastRxCmd = header->command;

        // Extract port from sender address
        lastPort = header->origin & MAPLE_PORT_MASK;

        // Wait for DC to finish its end-of-packet tail sequence before responding.
        // The FIFO-empty completion can trigger before the DC releases the bus —
        // the tail has a momentary "both pins HIGH" that our bus-idle check catches.
        // MaplePad uses ~200µs natural delay from IRQ to main loop processing.
        busy_wait_us(200);

        switch (header->command) {
            case MAPLE_CMD_DEVICE_REQUEST:
            case MAPLE_CMD_ALL_STATUS_REQUEST:
                sendInfoResponse();
                waitTxFlushRx();
                debugTxCount++;
                break;

            case MAPLE_CMD_GET_CONDITION: {
                // Track VBlank timing
                if (!firstPoll) {
                    uint64_t interval = nowUs - lastPollUs;
                    pollIntervalUs = pollIntervalUs + (interval - pollIntervalUs) / 10;
                }
                firstPoll = false;
                lastPollUs = nowUs;

                // Handle sync mode
                if (syncMode == DC_SYNC_WINDOW && !holdingResponse) {
                    uint64_t windowUs = (uint64_t)syncWindowMs * 1000;
                    if (lastPressUs > 0 && (nowUs - lastPressUs) < windowUs) {
                        holdingResponse = true;
                        holdStartUs = nowUs;
                        pendingPoll = true;
                    } else {
                        sendControllerState(gamepad);
                        waitTxFlushRx();
                        debugTxCount++;
                    }
                } else if (syncMode != DC_SYNC_WINDOW) {
                    sendControllerState(gamepad);
                    waitTxFlushRx();
                    debugTxCount++;
                }
                break;
            }

            default:
                // Unknown command — ACK
                sendACKResponse();
                waitTxFlushRx();
                debugTxCount++;
                break;
        }
    }

    // Release held response (Sync Window mode)
    if (holdingResponse && pendingPoll) {
        uint64_t windowUs = (uint64_t)syncWindowMs * 1000;
        if ((nowUs - holdStartUs) >= windowUs) {
            sendControllerState(gamepad);
            waitTxFlushRx();
            debugTxCount++;
            holdingResponse = false;
            pendingPoll = false;
            lastPressUs = 0;
        }
    }
}
