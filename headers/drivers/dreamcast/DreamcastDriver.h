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
    bool isConnected() { return connected; }

    // Debug counters (visible on OLED)
    uint32_t debugRxCount = 0;    // Valid packets processed (addressed to us)
    uint32_t debugTxCount = 0;    // Response packets sent
    uint32_t debugFilteredCount = 0; // Packets filtered (wrong address)
    uint32_t debugXorFail = 0;    // Times XOR/condition check failed
    int8_t   debugLastRxCmd = 0;  // Last command code received from DC
    uint8_t  debugLastDest = 0;   // Last destination address (for debugging filtering)
    uint16_t debugDcButtons = 0xFFFF; // Last DC button state sent (0=pressed)
    uint32_t debugGpButtons = 0;     // Last raw gamepad button state
    uint8_t  debugGpDpad = 0;        // Last raw gamepad dpad state

    // Per-command counters (which commands is DC sending?)
    uint32_t debugCmd1Count = 0;  // DEVICE_REQUEST
    uint32_t debugCmd2Count = 0;  // ALL_STATUS_REQUEST
    uint32_t debugCmd3Count = 0;  // RESET_DEVICE
    uint32_t debugCmd9Count = 0;  // GET_CONDITION
    uint32_t debugCmdOtherCount = 0; // Any other command

    // Loop timing diagnostic (max µs between process() calls, resets every ~1s)
    uint32_t debugLoopMaxUs = 0;
    uint32_t debugLoopLastResetUs = 0;

    // Consecutive CMD 9 polls before DC re-probes with CMD 1
    uint32_t debugConsecutivePolls = 0;
    uint32_t debugMaxConsecutivePolls = 0;

    // VMU disable flag — set true to test without VMU sub-peripheral
    bool disableVMU = true;  // DEFAULT TRUE for diagnostic build

    // VMU sub-peripheral (public for debug display access)
    DreamcastVMU vmu;

    // Static instance pointer for display access
    static DreamcastDriver* instance;

private:
    MapleBus bus;

    MapleInfoPacket       infoPacket;
    MapleControllerPacket controllerPacket;
    MapleACKPacket        ackPacket;

    bool connected;
    uint8_t lastPort;

    uint64_t lastProcessUs;  // Timestamp of last process() call (for loop timing)

    void buildInfoPacket();
    void buildControllerPacket();
    void buildACKPacket();

    void sendControllerState(Gamepad* gamepad);
    void sendInfoResponse();
    void sendExtInfoResponse();
    void sendACKResponse();
    void sendUnknownCommandResponse();
    void waitTxFlushRx();
    uint16_t mapButtonsToDC(uint32_t gpButtons, uint8_t dpad);
};
