#pragma once

#include <stdint.h>
#include "drivers/dreamcast/maple_bus.h"
#include "drivers/dreamcast/DreamcastVMU.h"
#include "hardware/gpio.h"

class Gamepad;

// DC button masks (inverted: 0=pressed, 1=released)
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
    virtual void processAux();  // Called from Core 1 — executes pending VMU flash writes
    bool isConnected() { return connected; }

    // Master diagnostics flag — when false, zero debug overhead in hot path.
    // Toggled by S1 (Select) on the OLED display.
    bool enableDiagnostics = false;

    bool zeroLatencyMode = false;

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

    // Zero Latency: GPIO pin → DC button mapping (built at init time)
    // Index = GPIO pin number, value = DC button mask bit to CLEAR when pressed
    uint16_t gpioDcButtonMap[30];  // NUM_BANK0_GPIOS = 30
    uint32_t triggerLTMask;       // Bitmask: which GPIO pin(s) map to LT
    uint32_t triggerRTMask;       // Bitmask: which GPIO pin(s) map to RT
    uint32_t buttonGpioMask;      // Bitmask of all GPIO pins that are buttons
    void buildGpioDcMap();        // Build the mapping table from current pin config

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
    uint16_t mapRawGpioToDC(uint32_t rawGpio, uint8_t* outLT, uint8_t* outRT);
};
