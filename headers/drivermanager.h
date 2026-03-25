#ifndef _DRIVERMANAGER_H
#define _DRIVERMANAGER_H

#include "enums.pb.h"
#include "gpdriver.h"

class GPDriver;
class DreamcastDriver;
class DreamcastVMU;

class DriverManager {
public:
    DriverManager(DriverManager const&) = delete;
    void operator=(DriverManager const&)  = delete;
    static DriverManager& getInstance() {// Thread-safe storage ensures cross-thread talk
        static DriverManager instance; // Guaranteed to be destroyed. // Instantiated on first use.
        return instance;
    }
    GPDriver * getDriver() { return driver; }
    DreamcastDriver * getDCDriver() { return dcDriver; }
    // Returns a VMU instance regardless of current input mode.
    // In Dreamcast mode: returns the live driver's VMU.
    // In webconfig mode: returns a static standalone VMU (for flash access only).
    DreamcastVMU * getVMU();
    void setup(InputMode);
    InputMode getInputMode(){ return inputMode; }
    bool isConfigMode(){ return (inputMode == INPUT_MODE_CONFIG); }
private:
    DriverManager() {}
    GPDriver * driver;
    DreamcastDriver * dcDriver = nullptr;
    InputMode inputMode;
};

#endif