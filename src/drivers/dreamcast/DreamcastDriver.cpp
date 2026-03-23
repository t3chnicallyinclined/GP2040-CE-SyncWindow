#include "drivers/dreamcast/DreamcastDriver.h"
#include "gamepad.h"
#include "gamepad/GamepadState.h"
#include "storagemanager.h"
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

DreamcastDriver::DreamcastDriver()
    : connected(false), lastPort(0), lastProcessUs(0) {
}

bool DreamcastDriver::init(uint pin_a, uint pin_b) {
    if (!bus.init(pin_a, pin_b)) return false;

    // Load VMU config from storage
    disableVMU = Storage::getInstance().getGamepadOptions().disableVMU;

    buildInfoPacket();
    buildControllerPacket();
    buildACKPacket();

    // Initialize VMU sub-peripheral (formats flash if blank)
    if (!disableVMU) {
        vmu.init();
    }

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

    // Controller capability declaration (funcData[0] bits, native byte order):
    //   Bits 0-7:   C,B,A,Start,Up,Down,Left,Right (0xFF)
    //   Bits 8-10:  Z,Y,X buttons (0x700)
    //   Bits 16-19: R trigger, L trigger, Analog X, Analog Y (0xF0000)
    // Total: 0x000F07FF — matches standard controller with analog stick + triggers.
    // Store native values — sendPacket's bswap32 handles wire byte order.
    infoPacket.info.func        = MAPLE_FUNC_CONTROLLER;
    infoPacket.info.funcData[0] = 0x000F07FF;
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

void DreamcastDriver::sendControllerState(Gamepad* gamepad) {
    uint16_t dcButtons = mapButtonsToDC(gamepad->state.buttons, gamepad->state.dpad);
    uint8_t lt = gamepad->state.lt;
    uint8_t rt = gamepad->state.rt;
    // Convert 16-bit unsigned (0-65535, mid=32767) to 8-bit unsigned (0-255, mid=128)
    // Add 0x80 for rounding before shift, clamp to 255
    uint32_t lx32 = (uint32_t)gamepad->state.lx + 0x80;
    uint32_t ly32 = (uint32_t)gamepad->state.ly + 0x80;
    uint8_t jx = (uint8_t)(lx32 > 0xFFFF ? 0xFF : (lx32 >> 8));
    uint8_t jy = (uint8_t)(ly32 > 0xFFFF ? 0xFF : (ly32 >> 8));

    // Digital L2/R2 as fallback — only override if no analog trigger value
    if (lt == 0 && (gamepad->state.buttons & GAMEPAD_MASK_L2)) lt = 255;
    if (rt == 0 && (gamepad->state.buttons & GAMEPAD_MASK_R2)) rt = 255;

    // Set port bits in header. Origin includes MAPLE_SUB0_ADDR to announce VMU sub-peripheral
    // on EVERY response, not just device info. MaplePad does: senderAddr = mAddr | mAddrAugmenter.
    uint8_t subPeriphBits = disableVMU ? 0 : MAPLE_SUB0_ADDR;
    controllerPacket.header.origin = (MAPLE_CTRL_ADDR & MAPLE_PERIPH_MASK) | subPeriphBits | lastPort;
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
    // CMD 1 (DEVICE_REQUEST): respond with cmd 5 (DEVICE_INFO), 28 words
    uint8_t subPeriphBits = disableVMU ? 0 : MAPLE_SUB0_ADDR;
    infoPacket.header.command = MAPLE_CMD_RESPOND_DEVICE_STATUS;
    infoPacket.header.numWords = sizeof(MapleDeviceInfo) / sizeof(uint32_t);  // 28
    infoPacket.header.origin = (MAPLE_CTRL_ADDR & MAPLE_PERIPH_MASK) | subPeriphBits | lastPort;
    infoPacket.header.destination = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | lastPort;
    infoPacket.bitPairsMinus1 = MapleBus::calcBitPairs(sizeof(infoPacket));
    infoPacket.crc = MapleBus::calcCRC(
        (uint32_t*)&infoPacket.header,
        sizeof(infoPacket) / sizeof(uint32_t) - 2);
    bus.sendPacket((uint32_t*)&infoPacket,
                   sizeof(infoPacket) / sizeof(uint32_t));
}

void DreamcastDriver::sendExtInfoResponse() {
    // CMD 2 (ALL_STATUS_REQUEST): respond with cmd 6 (EXT_DEVICE_INFO), 48 words
    // MaplePad uses a 48-word payload: first 28 words are device info, rest are zero.
    // We build this on a static buffer to match MaplePad exactly.
    static struct __attribute__((aligned(4))) {
        uint32_t          bitPairsMinus1;
        MaplePacketHeader header;
        uint32_t          payload[48];  // 48 words = 192 bytes
        uint32_t          crc;
    } extInfoPacket;

    memset(&extInfoPacket, 0, sizeof(extInfoPacket));

    uint8_t subPeriphBits = disableVMU ? 0 : MAPLE_SUB0_ADDR;
    extInfoPacket.header.command     = MAPLE_CMD_RESPOND_ALL_STATUS;  // cmd 6
    extInfoPacket.header.numWords    = 48;
    extInfoPacket.header.origin      = (MAPLE_CTRL_ADDR & MAPLE_PERIPH_MASK) | subPeriphBits | lastPort;
    extInfoPacket.header.destination = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | lastPort;

    // Copy device info into first 28 words of payload
    memcpy(extInfoPacket.payload, &infoPacket.info, sizeof(MapleDeviceInfo));

    // Calculate bitPairsMinus1 for the full packet
    uint totalBytes = sizeof(uint32_t) + sizeof(MaplePacketHeader) + 48 * sizeof(uint32_t) + sizeof(uint32_t);
    extInfoPacket.bitPairsMinus1 = MapleBus::calcBitPairs(totalBytes);

    extInfoPacket.crc = MapleBus::calcCRC(
        (uint32_t*)&extInfoPacket.header,
        1 + 48);  // header (1 word) + payload (48 words)

    bus.sendPacket((uint32_t*)&extInfoPacket, totalBytes / sizeof(uint32_t));
}

void DreamcastDriver::sendACKResponse() {
    uint8_t subPeriphBits = disableVMU ? 0 : MAPLE_SUB0_ADDR;
    ackPacket.header.origin = (MAPLE_CTRL_ADDR & MAPLE_PERIPH_MASK) | subPeriphBits | lastPort;
    ackPacket.header.destination = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | lastPort;
    ackPacket.crc = MapleBus::calcCRC(
        (uint32_t*)&ackPacket.header,
        sizeof(ackPacket) / sizeof(uint32_t) - 2);
    bus.sendPacket((uint32_t*)&ackPacket,
                   sizeof(ackPacket) / sizeof(uint32_t));
}

void DreamcastDriver::sendUnknownCommandResponse() {
    // MaplePad responds to unrecognized commands with UNKNOWN_COMMAND (0xFD / -3)
    // Uses the same structure as ACK (header only, 0 payload words)
    static MapleACKPacket unknownPkt;
    memset(&unknownPkt, 0, sizeof(unknownPkt));

    unknownPkt.bitPairsMinus1 = MapleBus::calcBitPairs(sizeof(unknownPkt));
    unknownPkt.header.command     = MAPLE_CMD_RESPOND_UNKNOWN_CMD;  // -3 / 0xFD
    unknownPkt.header.numWords    = 0;
    uint8_t subPeriphBits = disableVMU ? 0 : MAPLE_SUB0_ADDR;
    unknownPkt.header.origin      = (MAPLE_CTRL_ADDR & MAPLE_PERIPH_MASK) | subPeriphBits | lastPort;
    unknownPkt.header.destination = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | lastPort;

    unknownPkt.crc = MapleBus::calcCRC(
        (uint32_t*)&unknownPkt.header,
        sizeof(unknownPkt) / sizeof(uint32_t) - 2);
    bus.sendPacket((uint32_t*)&unknownPkt,
                   sizeof(unknownPkt) / sizeof(uint32_t));
}

void DreamcastDriver::waitTxFlushRx() {
    // Wait for full TX completion (DMA + PIO FIFO + wire) then flush RX echo
    bus.flushRx();
}

void DreamcastDriver::process(Gamepad* gamepad) {
    uint64_t nowUs = time_us_64();

    // Loop timing diagnostic: track max time between process() calls
    if (lastProcessUs > 0) {
        uint32_t deltaUs = (uint32_t)(nowUs - lastProcessUs);
        if (deltaUs > debugLoopMaxUs) {
            debugLoopMaxUs = deltaUs;
        }
        // Reset max every ~1 second so it shows current worst-case, not all-time
        if ((nowUs - debugLoopLastResetUs) > 1000000) {
            debugLoopMaxUs = deltaUs;
            debugLoopLastResetUs = nowUs;
        }
    } else {
        debugLoopLastResetUs = nowUs;
    }
    lastProcessUs = nowUs;

    // Copy XOR fail counter from bus layer
    debugXorFail = bus.debugXorFail;

    // Poll for decoded packets
    const uint8_t* packet = nullptr;
    uint rxLen = 0;
    if (bus.pollReceive(&packet, &rxLen) && rxLen >= sizeof(MaplePacketHeader)) {
        const MaplePacketHeader* header = (const MaplePacketHeader*)packet;

        // Route by destination address (matches MaplePad's dispensePacket approach)
        uint8_t destAddr = header->destination & MAPLE_PERIPH_MASK;
        debugLastDest = destAddr;

        // Extract port from sender address
        lastPort = header->origin & MAPLE_PORT_MASK;

        // No busy_wait needed here — sendPacket() now includes a 10µs bus line
        // check that verifies both pins are idle before transmitting.

        // Route to VMU sub-peripheral if addressed to 0x01
        if (!disableVMU && destAddr == MAPLE_SUB0_ADDR) {
            if (vmu.handleCommand(packet, rxLen, lastPort, bus)) {
                waitTxFlushRx();
            }
            return;
        }

        // Ignore commands to other sub-peripheral addresses (0x02, etc.)
        if (destAddr != MAPLE_CTRL_ADDR) {
            debugFilteredCount++;
            return;
        }

        debugRxCount++;
        debugLastRxCmd = header->command;

        // Track command types for diagnostics
        switch (header->command) {
            case MAPLE_CMD_DEVICE_REQUEST:     debugCmd1Count++; break;
            case MAPLE_CMD_ALL_STATUS_REQUEST: debugCmd2Count++; break;
            case MAPLE_CMD_RESET_DEVICE:       debugCmd3Count++; break;
            case MAPLE_CMD_GET_CONDITION:       debugCmd9Count++; break;
            default:                           debugCmdOtherCount++; break;
        }

        switch (header->command) {
            case MAPLE_CMD_DEVICE_REQUEST:
                // CMD 1: respond with device info (cmd 5, 28 words)
                // Track how many consecutive CMD 9 polls happened before this re-probe
                if (debugConsecutivePolls > debugMaxConsecutivePolls) {
                    debugMaxConsecutivePolls = debugConsecutivePolls;
                }
                debugConsecutivePolls = 0;
                sendInfoResponse();
                waitTxFlushRx();
                debugTxCount++;
                break;

            case MAPLE_CMD_ALL_STATUS_REQUEST:
                // CMD 2: respond with extended device info (cmd 6, 48 words)
                sendExtInfoResponse();
                waitTxFlushRx();
                debugTxCount++;
                break;

            case MAPLE_CMD_GET_CONDITION:
                // CMD 9: respond with current controller state immediately
                debugConsecutivePolls++;
                sendControllerState(gamepad);
                waitTxFlushRx();
                debugTxCount++;
                break;

            case MAPLE_CMD_RESET_DEVICE:
            case MAPLE_CMD_SHUTDOWN_DEVICE:
                // DC sends RESET to recover from errors, SHUTDOWN before power-off.
                // Must ACK — if we don't respond, DC thinks we crashed and disconnects.
                sendACKResponse();
                waitTxFlushRx();
                debugTxCount++;
                break;

            case MAPLE_CMD_SET_CONDITION:
                // Rumble/vibration — ACK to keep DC happy even though we don't
                // drive haptic hardware yet. Without this, games that use rumble
                // get UNKNOWN_COMMAND errors every frame.
                sendACKResponse();
                waitTxFlushRx();
                debugTxCount++;
                break;

            default:
                // Unknown command — respond with UNKNOWN_COMMAND (matches MaplePad)
                sendUnknownCommandResponse();
                waitTxFlushRx();
                debugTxCount++;
                break;
        }
    }

    // Process deferred VMU flash writes
    if (!disableVMU) {
        vmu.processFlashWrite();
    }
}
