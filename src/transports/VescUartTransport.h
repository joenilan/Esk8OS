#pragma once
#include <stdint.h>

namespace Esk8OS {
namespace Transports {

    struct RawVescData {
        float rpm;
        float inpVoltage;
        float tempMotor;
        float tempMosfet;
        float avgInputCurrent;
        float avgMotorCurrent;
        float dutyCycleNow;
        float wattHours;
        float wattHoursCharged;
        int error;

        // --- remote input (decoded PPM from the master VESC) ---
        float ppmDecoded;   // -1..1 throttle: <0 brake, >0 accel
        float ppmPulseMs;   // last pulse length (ms); ~1-2 ms valid, ~0 = no signal
        bool  ppmConnected; // valid, present pulse

        // --- diagnostics ---
        uint8_t fwMajor;    // VESC firmware version
        uint8_t fwMinor;
        bool  slaveOnline;  // second motor answered over CAN this cycle
        float masterMotorAmps, slaveMotorAmps;     // per-motor motor current
        float masterTempMotor, slaveTempMotor;     // per-motor motor temp
        float masterTempMosfet, slaveTempMosfet;   // per-motor ESC temp
    };

    void beginVescUart();
    bool getLatestVescData(RawVescData* outData);
    void setVescPollPaused(bool paused);

}
}
