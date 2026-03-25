#pragma once

#include <stdint.h>
#include "drivers/dreamcast/maple_bus.h"
#include "drivers/dreamcast/DreamcastVMU.h"

class Gamepad;

class DreamcastDriver {
public:
    DreamcastDriver();

    bool init(uint pin_a, uint pin_b);
    void process(Gamepad* gamepad);
    virtual void processAux();  // Called from Core 1 — executes pending VMU flash writes
    bool isConnected() { return connected; }

    // Master diagnostics flag — when false, zero debug overhead in hot path.
    // Toggled by S1 (Select) on the OLED display.
    bool enableDiagnostics = false;

    // Debug counters (only updated when enableDiagnostics == true)
    uint32_t debugRxCount = 0;
    uint32_t debugTxCount = 0;
    uint32_t debugXorFail = 0;
    uint32_t debugCmd1Count = 0;
    uint32_t debugCmd9Count = 0;
    uint32_t debugConsecutivePolls = 0;
    uint32_t debugMaxConsecutivePolls = 0;
    uint32_t debugResendCount = 0;

    // VMU disable flag — loaded from config in init()
    bool disableVMU = false;

    // VMU ready gate — don't process VMU commands until we've confirmed
    // the RX decoder is aligned via a successful controller DEVICE_REQUEST.
    bool vmuReady = false;

    // VMU sub-peripheral (public for debug display and webconfig access)
    DreamcastVMU vmu;

private:
    MapleBus bus;

    // Pre-built packet buffers (wire/network byte order, uint32_t arrays)
    uint32_t infoPacketBuf[31];       // bitpairs + header + 28 device info words + CRC
    uint32_t controllerPacketBuf[6];  // bitpairs + header + funcCode + 2 condition words + CRC
    uint32_t ackPacketBuf[3];         // bitpairs + header + CRC
    uint32_t resendPacketBuf[3];      // bitpairs + header + CRC

    bool connected;
    uint8_t lastPort;

    void buildInfoPacket();
    void buildControllerPacket();
    void buildACKPacket();
    void buildResendPacket();

    void sendControllerState(Gamepad* gamepad);
    void sendInfoResponse();
    void sendExtInfoResponse();
    void sendACKResponse();
    void sendResendRequest();
    void sendUnknownCommandResponse();
    void waitTxFlushRx();
    uint16_t mapButtonsToDC(uint32_t gpButtons, uint8_t dpad);
};
