#include "drivers/dreamcast/DreamcastDriver.h"
#include "gamepad.h"
#include "gamepad/GamepadState.h"
#include "storagemanager.h"
#include "pico/time.h"
#include "hardware/gpio.h"

// Dreamcast controller driver — wire-order architecture (NO DMA bswap).
// Based on MaplePad by mackieks (github.com/mackieks/MaplePad).

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

// Helper: build a device info string in wire order.
// Maple Bus sends LSByte first per word. PIO sends bits 31:24 first.
// So the first character of the string must end up at bits [31:24] of the first word.
// We write chars naturally to a byte buffer, then bswap32 each 4-byte chunk.
static void buildDevInfoString(uint32_t* dest, const char* str, uint maxLen) {
    uint8_t buf[64];
    memset(buf, ' ', maxLen);  // Pad with spaces (matching real VMU/controller)
    for (uint i = 0; i < maxLen && str[i]; i++) {
        buf[i] = str[i];
    }
    // Pack into uint32_t words and bswap to wire order
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

    // Load config from storage
    const GamepadOptions& options = Storage::getInstance().getGamepadOptions();
    disableVMU = options.disableVMU;
    // dcSyncMode removed — raw passthrough is always used (matches real DC controller)

    buildInfoPacket();
    buildControllerPacket();
    buildACKPacket();
    buildResendPacket();

    // Initialize VMU sub-peripheral (formats flash if blank)
    if (!disableVMU) {
        vmu.init();
    }

    connected = true;
    return true;
}

void DreamcastDriver::buildInfoPacket() {
    // Device info response: 28 payload words (112 bytes)
    // Layout: [0]=bitPairs, [1]=header, [2..29]=deviceInfo, [30]=CRC
    memset(infoPacketBuf, 0, sizeof(infoPacketBuf));

    infoPacketBuf[0] = MapleBus::calcBitPairs(sizeof(infoPacketBuf));
    infoPacketBuf[1] = maple_make_header(
        28,  // numWords = 28 (device info payload)
        MAPLE_CTRL_ADDR,
        MAPLE_DC_ADDR,
        MAPLE_CMD_RESPOND_DEVICE_STATUS
    );

    // Build 28 words of device info payload in wire order.
    // Strategy: construct each field in host order, then bswap32 to wire.
    uint32_t* info = &infoPacketBuf[2];  // payload starts at word 2

    // Word 0: function code
    info[0] = maple_host_to_wire(MAPLE_FUNC_CONTROLLER);

    // Words 1-3: funcData
    // Controller capability (funcData[0]):
    //   Bits 0-7:   C,B,A,Start,Up,Down,Left,Right (0xFF)
    //   Bits 8-10:  Z,Y,X buttons (0x700)
    //   Bits 16-19: R trigger, L trigger, Analog X, Analog Y (0xF0000)
    // Total: 0x000F07FF
    info[1] = maple_host_to_wire(0x000F07FF);
    info[2] = 0;  // funcData[1] = 0
    info[3] = 0;  // funcData[2] = 0

    // Word 4: areaCode + connectorDirection (packed as bytes)
    // areaCode = 0xFF (all regions), connDir = 0
    // In host order: areaCode at MSByte (MaplePad puts regionCode << 24)
    info[4] = maple_host_to_wire((uint32_t)0xFF << 24);

    // Words 5-11: productName (30 bytes padded to 32 = 8 words, last 2 bytes are zero)
    buildDevInfoString(&info[5], "GP2040-CE NOBD Controller", 30);
    // Zero-pad the remaining 2 bytes in word 12 (words 5+7=12, but 30/4=7.5 words)
    // Actually 30 bytes = 7 full words + 2 extra bytes. Handle in buildDevInfoString.
    // Let's just use 32 bytes (8 words) with the last 2 bytes as spaces.
    buildDevInfoString(&info[5], "GP2040-CE NOBD Controller     ", 32);

    // Words 13-27: productLicense (60 bytes padded to 60 = 15 words)
    buildDevInfoString(&info[13], "Open Source - github.com/OpenStickCommunity              ", 60);

    // Word 28 (index 26 from info base... wait, let me recount.
    // info[0]=func, [1-3]=funcData, [4]=area/conn, [5..12]=name(32 bytes=8 words),
    // [13..27]=license(60 bytes=15 words)
    // Total so far: 1+3+1+8+15 = 28 words. That's exactly right.
    // But wait — the original MapleDeviceInfo has:
    //   func(4) + funcData(12) + areaCode(1) + connDir(1) + name(30) + license(60) + power(4) = 112 bytes = 28 words
    // So area+conn is 2 bytes, not 4. Let me recalculate.
    //
    // Actually in the original packed struct:
    //   func: 4 bytes (1 word)
    //   funcData[3]: 12 bytes (3 words)
    //   areaCode: 1 byte
    //   connectorDirection: 1 byte
    //   productName: 30 bytes
    //   productLicense: 60 bytes
    //   standbyPower: 2 bytes
    //   maxPower: 2 bytes
    //   Total: 4+12+1+1+30+60+2+2 = 112 bytes = 28 words
    //
    // The challenge is that areaCode+connDir+productName = 1+1+30 = 32 bytes = 8 words.
    // These cross a word boundary. Let me build the entire payload as a byte buffer
    // then bswap32 each word — much cleaner.

    // Start over with byte-level construction
    uint8_t infoBuf[112];
    memset(infoBuf, 0, sizeof(infoBuf));

    // func (4 bytes, offset 0) — big-endian wire value
    uint32_t funcHost = MAPLE_FUNC_CONTROLLER;
    infoBuf[0] = (funcHost >> 0) & 0xFF;
    infoBuf[1] = (funcHost >> 8) & 0xFF;
    infoBuf[2] = (funcHost >> 16) & 0xFF;
    infoBuf[3] = (funcHost >> 24) & 0xFF;

    // funcData[0] (4 bytes, offset 4)
    uint32_t fd0 = 0x000F07FF;
    infoBuf[4] = (fd0 >> 0) & 0xFF;
    infoBuf[5] = (fd0 >> 8) & 0xFF;
    infoBuf[6] = (fd0 >> 16) & 0xFF;
    infoBuf[7] = (fd0 >> 24) & 0xFF;

    // funcData[1], funcData[2] (offsets 8-15) — already zero

    // areaCode (1 byte, offset 16)
    infoBuf[16] = 0xFF;  // All regions

    // connectorDirection (1 byte, offset 17)
    infoBuf[17] = 0;

    // productName (30 bytes, offset 18)
    const char* name = "GP2040-CE NOBD Controller";
    for (int i = 0; i < 30; i++) {
        infoBuf[18 + i] = (name[i] && i < (int)strlen(name)) ? name[i] : ' ';
    }

    // productLicense (60 bytes, offset 48)
    const char* license = "Open Source - github.com/OpenStickCommunity";
    for (int i = 0; i < 60; i++) {
        infoBuf[48 + i] = (license[i] && i < (int)strlen(license)) ? license[i] : ' ';
    }

    // standbyPower (2 bytes LE, offset 108)
    uint16_t standby = 430;
    infoBuf[108] = standby & 0xFF;
    infoBuf[109] = (standby >> 8) & 0xFF;

    // maxPower (2 bytes LE, offset 110)
    uint16_t maxPwr = 500;
    infoBuf[110] = maxPwr & 0xFF;
    infoBuf[111] = (maxPwr >> 8) & 0xFF;

    // Now convert to wire order: bswap32 each 4-byte word.
    // On wire, Maple Bus sends LSByte first per word. PIO puts first-received byte at bits[31:24].
    // So for each host-order uint32 at infoBuf, the LSByte (byte[0]) should end up at bits[31:24].
    // That means: wire_word = bswap32(host_word).
    for (int i = 0; i < 28; i++) {
        uint32_t hostWord = infoBuf[i*4]
                          | ((uint32_t)infoBuf[i*4+1] << 8)
                          | ((uint32_t)infoBuf[i*4+2] << 16)
                          | ((uint32_t)infoBuf[i*4+3] << 24);
        info[i] = __builtin_bswap32(hostWord);
    }
}

void DreamcastDriver::buildControllerPacket() {
    // Controller condition response: 3 payload words (funcCode + 2 condition words)
    // Layout: [0]=bitPairs, [1]=header, [2]=funcCode, [3]=triggers+buttons, [4]=analogs, [5]=CRC
    memset(controllerPacketBuf, 0, sizeof(controllerPacketBuf));

    controllerPacketBuf[0] = MapleBus::calcBitPairs(sizeof(controllerPacketBuf));
    controllerPacketBuf[1] = maple_make_header(
        3,  // numWords = 3
        MAPLE_CTRL_ADDR,
        MAPLE_DC_ADDR,
        MAPLE_CMD_RESPOND_DATA_XFER
    );

    // Payload word 0: function code in wire order
    controllerPacketBuf[2] = maple_host_to_wire(MAPLE_FUNC_CONTROLLER);

    // Payload word 1: triggers + buttons (all released defaults)
    // Wire order: leftTrig at bits[31:24], rightTrig at [23:16], buttonsHi at [15:8], buttonsLo at [7:0]
    controllerPacketBuf[3] = ((uint32_t)0 << 24) | ((uint32_t)0 << 16)
                           | ((uint32_t)0xFF << 8) | 0xFF;

    // Payload word 2: analogs (centered defaults)
    // Wire order: joyY2 at [31:24], joyX2 at [23:16], joyY at [15:8], joyX at [7:0]
    controllerPacketBuf[4] = ((uint32_t)0x80 << 24) | ((uint32_t)0x80 << 16)
                           | ((uint32_t)0x80 << 8) | 0x80;
}

void DreamcastDriver::buildACKPacket() {
    // ACK response: header only, 0 payload words
    // Layout: [0]=bitPairs, [1]=header, [2]=CRC
    memset(ackPacketBuf, 0, sizeof(ackPacketBuf));

    ackPacketBuf[0] = MapleBus::calcBitPairs(sizeof(ackPacketBuf));
    ackPacketBuf[1] = maple_make_header(
        0,  // numWords = 0
        MAPLE_CTRL_ADDR,
        MAPLE_DC_ADDR,
        MAPLE_CMD_RESPOND_ACK
    );
}

void DreamcastDriver::buildResendPacket() {
    // REQUEST_RESEND: header only, 0 payload words
    memset(resendPacketBuf, 0, sizeof(resendPacketBuf));

    resendPacketBuf[0] = MapleBus::calcBitPairs(sizeof(resendPacketBuf));
    resendPacketBuf[1] = maple_make_header(
        0,
        MAPLE_CTRL_ADDR,
        MAPLE_DC_ADDR,
        MAPLE_CMD_RESPOND_SEND_AGAIN
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

void __no_inline_not_in_flash_func(DreamcastDriver::sendControllerState)(Gamepad* gamepad) {
    uint32_t buttons = gamepad->state.buttons;
    uint8_t  dpad    = gamepad->state.dpad;
    uint8_t  lt      = gamepad->state.lt;
    uint8_t  rt      = gamepad->state.rt;
    if (lt == 0 && (buttons & GAMEPAD_MASK_L2)) lt = 255;
    if (rt == 0 && (buttons & GAMEPAD_MASK_R2)) rt = 255;

    uint16_t dcButtons = mapButtonsToDC(buttons, dpad);

    // Convert 16-bit unsigned (0-65535, mid=32767) to 8-bit unsigned (0-255, mid=128)
    // Add 0x80 for rounding before shift, clamp to 255
    uint32_t lx32 = (uint32_t)gamepad->state.lx + 0x80;
    uint32_t ly32 = (uint32_t)gamepad->state.ly + 0x80;
    uint8_t jx = (uint8_t)(lx32 > 0xFFFF ? 0xFF : (lx32 >> 8));
    uint8_t jy = (uint8_t)(ly32 > 0xFFFF ? 0xFF : (ly32 >> 8));

    // Set port bits in header. Origin includes MAPLE_SUB0_ADDR to announce VMU sub-peripheral
    // on EVERY response, not just device info. MaplePad does: senderAddr = mAddr | mAddrAugmenter.
    uint8_t subPeriphBits = disableVMU ? 0 : MAPLE_SUB0_ADDR;
    uint8_t origin = (MAPLE_CTRL_ADDR & MAPLE_PERIPH_MASK) | subPeriphBits | lastPort;
    uint8_t dest = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | lastPort;

    controllerPacketBuf[1] = maple_make_header(3, origin, dest, MAPLE_CMD_RESPOND_DATA_XFER);

    // Payload word 1: triggers + buttons (wire order)
    // bits[31:24]=leftTrig, [23:16]=rightTrig, [15:8]=buttonsHi, [7:0]=buttonsLo
    controllerPacketBuf[3] = ((uint32_t)lt << 24) | ((uint32_t)rt << 16)
                           | ((uint32_t)((dcButtons >> 8) & 0xFF) << 8)
                           | (dcButtons & 0xFF);

    // Payload word 2: analogs (wire order)
    // bits[31:24]=joyY2, [23:16]=joyX2, [15:8]=joyY, [7:0]=joyX
    controllerPacketBuf[4] = ((uint32_t)0x80 << 24) | ((uint32_t)0x80 << 16)
                           | ((uint32_t)jy << 8) | jx;

    // Recalculate CRC over header + 3 payload words
    controllerPacketBuf[5] = MapleBus::calcCRC(&controllerPacketBuf[1], 4);

    bus.sendPacket(controllerPacketBuf, 6);
}

void __no_inline_not_in_flash_func(DreamcastDriver::sendInfoResponse)() {
    // CMD 1 (DEVICE_REQUEST): respond with cmd 5 (DEVICE_INFO), 28 words
    uint8_t subPeriphBits = disableVMU ? 0 : MAPLE_SUB0_ADDR;
    uint8_t origin = (MAPLE_CTRL_ADDR & MAPLE_PERIPH_MASK) | subPeriphBits | lastPort;
    uint8_t dest = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | lastPort;

    infoPacketBuf[1] = maple_make_header(28, origin, dest, MAPLE_CMD_RESPOND_DEVICE_STATUS);
    infoPacketBuf[30] = MapleBus::calcCRC(&infoPacketBuf[1], 29);  // header + 28 payload words
    bus.sendPacket(infoPacketBuf, 31);
}

void __no_inline_not_in_flash_func(DreamcastDriver::sendExtInfoResponse)() {
    // CMD 2 (ALL_STATUS_REQUEST): respond with cmd 6 (EXT_DEVICE_INFO), 48 words
    // MaplePad uses a 48-word payload: first 28 words are device info, rest are zero.
    static uint32_t extInfoBuf[51];  // bitpairs + header + 48 payload + CRC
    memset(extInfoBuf, 0, sizeof(extInfoBuf));

    uint8_t subPeriphBits = disableVMU ? 0 : MAPLE_SUB0_ADDR;
    uint8_t origin = (MAPLE_CTRL_ADDR & MAPLE_PERIPH_MASK) | subPeriphBits | lastPort;
    uint8_t dest = (MAPLE_DC_ADDR & MAPLE_PERIPH_MASK) | lastPort;

    extInfoBuf[0] = MapleBus::calcBitPairs(sizeof(extInfoBuf));
    extInfoBuf[1] = maple_make_header(48, origin, dest, MAPLE_CMD_RESPOND_ALL_STATUS);

    // Copy device info into first 28 words of payload (already in wire order)
    memcpy(&extInfoBuf[2], &infoPacketBuf[2], 28 * sizeof(uint32_t));

    extInfoBuf[50] = MapleBus::calcCRC(&extInfoBuf[1], 49);  // header + 48 payload words
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
    // MaplePad responds to unrecognized commands with UNKNOWN_COMMAND (0xFD / -3)
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
    // Flash writes are handled directly in GP2040Aux::run() via vmu.doFlashWriteFromCore1()
}

void __no_inline_not_in_flash_func(DreamcastDriver::waitTxFlushRx)() {
    // Wait for full TX completion (DMA + PIO FIFO + wire) then flush RX echo
    bus.flushRx();
}

void __no_inline_not_in_flash_func(DreamcastDriver::process)(Gamepad* gamepad) {
    const bool diag = enableDiagnostics;
    bus.enableDiagnostics = diag;

    if (diag) debugXorFail = bus.debugXorFail;

    // Poll for decoded packets (all data in wire/network byte order)
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

        // Route to VMU sub-peripheral
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
                if (diag) { debugCmd9Count++; debugConsecutivePolls++; }
                sendControllerState(gamepad);
                waitTxFlushRx();
                if (diag) debugTxCount++;
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
