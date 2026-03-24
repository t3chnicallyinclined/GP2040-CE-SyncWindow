#pragma once

#include <stdint.h>
#include "drivers/dreamcast/maple_bus.h"
#include "drivers/dreamcast/DreamcastVMU.h"

// DC Sync Modes — how inputs are timed relative to the Dreamcast's 60Hz polling.
// Currently always DC_SYNC_OFF (raw passthrough, matches real DC controller behavior).
// NOBD sync window handles press grouping at the GPIO level if needed.
// Accumulate mode is preserved for potential future use cases (e.g., USB-host-based
// input sources with lower update rates than direct GPIO).
#define DC_SYNC_OFF        0  // Raw snapshot at poll time (active)
#define DC_SYNC_ACCUMULATE 1  // Latch all presses between polls (reserved for future use)

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

    // Loop timing diagnostic (max us between process() calls, resets every ~1s)
    uint32_t debugLoopMaxUs = 0;
    uint32_t debugLoopLastResetUs = 0;

    // Consecutive CMD 9 polls before DC re-probes with CMD 1
    uint32_t debugConsecutivePolls = 0;
    uint32_t debugMaxConsecutivePolls = 0;

    // REQUEST_RESEND counter — how many times we asked DC to retransmit
    uint32_t debugResendCount = 0;

    // VMU disable flag — loaded from config in init()
    bool disableVMU = false;

    // VMU ready gate — don't process VMU commands until we've confirmed
    // the RX decoder is aligned via a successful controller DEVICE_REQUEST.
    // Prevents byte-shifted payloads from hot-plug mid-frame captures.
    bool vmuReady = false;

    // VMU sub-peripheral (public for debug display access)
    DreamcastVMU vmu;

private:
    MapleBus bus;

    // Pre-built packet buffers (wire/network byte order, uint32_t arrays)
    // Layout: [0]=bitPairsMinus1, [1]=header, [2..N-2]=payload, [N-1]=CRC
    uint32_t infoPacketBuf[31];       // bitpairs + header + 28 device info words + CRC
    uint32_t controllerPacketBuf[6];  // bitpairs + header + funcCode + 2 condition words + CRC
    uint32_t ackPacketBuf[3];         // bitpairs + header + CRC
    uint32_t resendPacketBuf[3];      // bitpairs + header + CRC

    bool connected;
    uint8_t lastPort;

    uint64_t lastProcessUs;  // Timestamp of last process() call (for loop timing)

    // Accumulate mode state — reserved for future use.
    // Currently disabled (dcSyncMode always DC_SYNC_OFF). See comment at top of file.
    uint32_t dcSyncMode = DC_SYNC_OFF;
    uint32_t accumButtons = 0;
    uint8_t  accumDpad = 0;
    uint8_t  accumLt = 0;
    uint8_t  accumRt = 0;

    void buildInfoPacket();
    void buildControllerPacket();
    void buildACKPacket();
    void buildResendPacket();

    void accumulate(Gamepad* gamepad);
    void sendControllerState(Gamepad* gamepad);
    void sendInfoResponse();
    void sendExtInfoResponse();
    void sendACKResponse();
    void sendResendRequest();
    void sendUnknownCommandResponse();
    void waitTxFlushRx();
    uint16_t mapButtonsToDC(uint32_t gpButtons, uint8_t dpad);
};
