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

    // Debug counters
    uint32_t debugRxCount = 0;
    uint32_t debugTxCount = 0;
    uint32_t debugXorFail = 0;
    uint32_t debugCmd1Count = 0;
    uint32_t debugCmd9Count = 0;
    uint32_t debugConsecutivePolls = 0;
    uint32_t debugMaxConsecutivePolls = 0;
    uint32_t debugResendCount = 0;
    uint32_t debugTableHits = 0;

    // Response timing
    volatile uint32_t respLast = 0;
    volatile uint32_t respMin = 0xFFFFFFFF;
    volatile uint32_t respMax = 0;
    volatile uint32_t respCount = 0;

    bool disableVMU = false;
    bool vmuReady = false;

    DreamcastVMU vmu;

    // GPIO pin → DC button mapping (built at init time)
    uint16_t gpioDcButtonMap[30];
    uint32_t triggerLTMask;
    uint32_t triggerRTMask;
    uint32_t buttonGpioMask;
    void buildGpioDcMap();

    // Pre-computed CMD9 response lookup table.
    // Index = compressed GPIO button state (sized to actual button count at init).
    // W3 = buttons+triggers, W5 = CRC (pre-computed with cached port + constant W4).
    uint32_t* cmd9TableW3 = nullptr;
    uint32_t* cmd9TableW5 = nullptr;        // Restored: CRC pre-computed per button state
    uint32_t  cmd9TableSize = 0;
    uint8_t   cmd9GpioPins[13];
    uint8_t   cmd9NumBits = 0;
    volatile uint32_t cmd9ReadyW3;
    volatile uint32_t cmd9ReadyW5;          // Pre-computed CRC for current button state
    uint32_t lastFilteredGpio = 0xFFFFFFFF; // Cache for early-return in updateCmd9FromGpio
    void buildCmd9LookupTable();
    void rebuildCmd9LookupTableForPort();   // Rebuild with real port after CMD1
    void updateCmd9FromGpio(uint32_t filteredGpio);
    void updateAnalogFromGamepad(Gamepad* gamepad);

    // Cached port from CMD1 — never changes after initial handshake.
    // Used to pre-compute all packet headers and CRCs.
    uint8_t cachedPort = 0;
    bool portKnown = false;

    // Pre-built packet buffers — all public for ISR access.
    // Headers and CRCs are pre-computed with cachedPort after first CMD1.
    MapleBus bus;
    uint32_t controllerPacketBuf[6];
    uint32_t infoPacketBuf[31];
    uint32_t extInfoBuf[51];
    uint32_t ackPacketBuf[3];
    uint32_t resendPacketBuf[3];
    uint32_t unknownCmdBuf[3];

    // Public for ISR callback access
    uint16_t mapRawGpioToDC(uint32_t rawGpio, uint8_t* outLT, uint8_t* outRT);

    // Rebuild all static packet headers+CRCs after port is known from CMD1.
    void rebuildAllPacketsForPort(uint8_t port);

    // ISR command dispatch — handles ALL Maple Bus commands from ISR context.
    // Registered via bus.enableIsrDispatch() at init.
    static void __no_inline_not_in_flash_func(isrCommandDispatch)(MapleBus* bus);

    // ISR VMU sub-dispatch
    void isrHandleVmuCommand(int8_t cmd, uint32_t hdr, uint8_t port, MapleBus* bus);

private:
    bool connected;

    void buildInfoPacket();
    void buildExtInfoPacket();
    void buildControllerPacket();
    void buildACKPacket();
    void buildResendPacket();
    void buildUnknownCmdPacket();

    void waitTxFlushRx();
    uint16_t mapButtonsToDC(uint32_t gpButtons, uint8_t dpad);
};
