#pragma once

#include <stdint.h>
#include "drivers/dreamcast/maple_bus.h"
#include "drivers/dreamcast/DreamcastVMU.h"
#include "hardware/gpio.h"

class Gamepad;

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

class DreamcastDriver {
public:
    DreamcastDriver();

    bool init(uint pin_a, uint pin_b);
    void process(Gamepad* gamepad);
    virtual void processAux();
    bool isConnected() { return connected; }

    bool enableDiagnostics = false;
    bool zeroLatencyMode = false;
    void setFastPath(bool enable);

    // Cached analog values for ISR access (ZL mode)
    volatile uint8_t cachedJX = 0x80;
    volatile uint8_t cachedJY = 0x80;

    // Debug counters (only updated when enableDiagnostics == true)
    uint32_t debugRxCount = 0;
    uint32_t debugTxCount = 0;
    uint32_t debugXorFail = 0;
    uint32_t debugCmd1Count = 0;
    uint32_t debugCmd9Count = 0;
    uint32_t debugConsecutivePolls = 0;
    uint32_t debugMaxConsecutivePolls = 0;
    uint32_t debugResendCount = 0;

    // Response timing (only updated when enableDiagnostics == true)
    // Measures from packet arrival (rxArrivalTimestamp) to sendPacket()
    volatile uint32_t respLast = 0;       // Last response time in µs
    volatile uint32_t respMin = 0xFFFFFFFF;
    volatile uint32_t respMax = 0;
    volatile uint32_t respCount = 0;

    bool disableVMU = false;
    bool vmuReady = false;

    DreamcastVMU vmu;

    // Zero Latency: GPIO pin → DC button mapping (built at init time)
    uint16_t gpioDcButtonMap[30];
    uint32_t triggerLTMask;
    uint32_t triggerRTMask;
    uint32_t buttonGpioMask;
    void buildGpioDcMap();

    MapleBus bus;
    uint32_t controllerPacketBuf[6];

    // Public for ISR fast-path callback access
    uint16_t mapRawGpioToDC(uint32_t rawGpio, uint8_t* outLT, uint8_t* outRT);

private:
    uint32_t infoPacketBuf[31];
    uint32_t ackPacketBuf[3];
    uint32_t resendPacketBuf[3];

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
