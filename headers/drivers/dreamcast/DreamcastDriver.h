#pragma once

#include <stdint.h>
#include "drivers/dreamcast/maple_bus.h"

class Gamepad;

// DC Sync modes
enum DCSyncMode {
    DC_SYNC_OFF        = 0,
    DC_SYNC_ACCUMULATE = 1,
    DC_SYNC_WINDOW     = 2,
};

class DreamcastDriver {
public:
    DreamcastDriver();

    bool init(uint pin_a, uint pin_b);
    void process(Gamepad* gamepad);
    bool isConnected() { return connected; }

    void setSyncMode(uint8_t mode) { syncMode = mode; }
    void setSyncWindow(uint8_t ms) { syncWindowMs = ms; }

    // Debug counters (visible on OLED)
    uint32_t debugRxCount = 0;    // Packets successfully decoded
    uint32_t debugTxCount = 0;    // Response packets sent
    uint32_t debugFifoCount = 0;  // FIFO words read (any PIO activity)
    uint8_t  debugPinState = 0;   // Raw GPIO state: bit0=pinA, bit1=pinB
    uint8_t  debugPinA = 0;       // GPIO pin A number
    uint8_t  debugPinB = 0;       // GPIO pin B number
    uint32_t debugSyncCount = 0;  // Sync sequences detected
    uint32_t debugEndCount = 0;   // End-of-packet attempts
    uint16_t debugMaxBytes = 0;   // Max bytes decoded in any attempt
    uint8_t  debugLastXor = 0;    // Last XOR value at end attempt
    uint8_t  debugLastBitCnt = 0; // bitCount at last end attempt
    uint32_t debugPollTrue = 0;   // Times pollReceive returned true
    uint32_t debugXorFail = 0;    // Times XOR/condition check failed
    uint32_t debugFlushCount = 0; // FIFO words flushed after TX (0 = TX not producing output)
    int8_t   debugLastRxCmd = 0;  // Last command code received from DC
    uint16_t debugDcButtons = 0xFFFF; // Last DC button state sent (0=pressed)
    uint32_t debugGpButtons = 0;     // Last raw gamepad button state
    uint8_t  debugGpDpad = 0;        // Last raw gamepad dpad state

    // Static instance pointer for display access
    static DreamcastDriver* instance;

private:
    MapleBus bus;

    MapleInfoPacket       infoPacket;
    MapleControllerPacket controllerPacket;
    MapleACKPacket        ackPacket;

    bool connected;
    uint8_t lastPort;

    uint64_t lastPollUs;
    uint64_t pollIntervalUs;
    bool     firstPoll;

    uint8_t  syncMode;
    uint8_t  syncWindowMs;
    uint32_t accumulatedButtons;
    uint8_t  accumulatedDpad;
    uint8_t  accumulatedLT;
    uint8_t  accumulatedRT;
    uint8_t  accumulatedJoyX;
    uint8_t  accumulatedJoyY;
    bool     holdingResponse;
    bool     pendingPoll;
    uint64_t holdStartUs;
    uint64_t lastPressUs;
    uint32_t prevButtons;
    uint8_t  prevDpad;

    void buildInfoPacket();
    void buildControllerPacket();
    void buildACKPacket();

    void sendControllerState(Gamepad* gamepad);
    void sendInfoResponse();
    void sendACKResponse();
    void waitTxFlushRx();
    void updateAccumulatedState(Gamepad* gamepad);
    uint16_t mapButtonsToDC(uint32_t gpButtons, uint8_t dpad);
};
