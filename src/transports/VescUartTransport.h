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

        // --- modern-protocol extras (COMM_GET_VALUES_SETUP_SELECTIVE; zeroed
        // when the ESC firmware predates it and the legacy path is active) ---
        bool  modernProto;    // aggregated setup-values path in use
        float vescSpeedKmh;   // ESC-computed speed (its own gearing config)
        int   vescBatteryPct; // ESC's battery estimate, 0-100
        float vescWhLeft;     // Wh remaining per the ESC battery config
        float vescOdometerKm; // ESC's persistent odometer
        uint8_t numVescs;     // controllers the master sees on the CAN bus
        uint8_t slaveCanId;   // auto-detected slave id (0 = none found)
        char  hwName[17];     // ESC hardware name from the FW handshake
    };

    void beginVescUart();
    bool getLatestVescData(RawVescData* outData);
    void setVescPollPaused(bool paused);

    // Last dataset the poll task published, without consuming it and without
    // the plausibility gating telemetry applies — so `diag` can show what the
    // ESC actually reports even when it's in logic-standby (vin ~3.6V) and
    // the rider-facing telemetry is deliberately masked. False until the
    // first publish since boot.
    bool peekVescData(RawVescData* outData);

    // Transport/wire-level state for the `diag` console command — lets a dead
    // link be diagnosed as "ESC silent" vs "talking but frames rejected" vs
    // "reads fine but voltage-gated" without a logic analyzer.
    struct VescLinkDebug {
        uint8_t path;           // 0 probing, 1 modern, 2 legacy
        uint8_t slaveId;        // detected slave CAN id (0 = none yet)
        bool searchDone;        // slave sweep finished with no hit
        int scanIdx;            // sweep progress
        uint32_t publishes;     // datasets handed to telemetry since boot
        float lastVin;          // last raw input voltage read (pre any gating)
        uint32_t txFrames, replies, rxBytes, crcErrors, timeouts;
    };
    void getVescLinkDebug(VescLinkDebug* out);

    // Raw COMM_GET_MCCONF capture (handoff step 1: bytes only, no parsing).
    // The poll task reads it once per boot as soon as the VESC link is up and
    // saves the hex dump to /mcconf.hex, so the bytes can be pulled off the
    // board later over USB with the pack off — no live probing required.
    // status: 0 = waiting for link, 1 = captured, 2 = gave up (link up, no
    // valid reply). Power-cycle the board to capture again.
    struct McconfCapture { uint8_t status; int len; uint8_t attempts; };
    void getMcconfCaptureState(McconfCapture* out);

    // ESC-side ride statistics (COMM_GET_STATS, FW 6+): the VESC's OWN
    // accumulated averages/maxima since ESC power-on — independent of this
    // firmware's math, so they cross-check it. Polled every ~2 s on the modern
    // path; false until the first successful read (old FW / link never up).
    struct VescRideStats {
        bool  valid;
        float speedAvgKmh, speedMaxKmh;
        float powerAvgW,  powerMaxW;
        float currentAvgA, currentMaxA;
        float tempMosAvgC, tempMosMaxC;
        float tempMotorAvgC, tempMotorMaxC;
        float rideTimeS;
    };
    bool getVescRideStats(VescRideStats* out);

}
}
