#include "telemetry/TelemetryModel.h"

float currentSpeedKmh = 0.0;
float currentSpeedMph = 0.0;
float currentVoltage = 0.0; // 0 until a real VESC read — no fake full-charge before telemetry
int   currentBatteryPercent = 0;
float currentMotorTemp = 0.0;
float currentBatteryTemp = 0.0;
float currentEscTemp = 0.0;
float currentAmps = 0.0;        
float currentMotorAmps = 0.0;   
float currentDuty = 0.0;        
float currentWatts = 0.0;
float peakWatts = 0.0;          
float currentWattHours = 0.0;   
float currentWhRegen = 0.0;     
float maxSpeedKmh = 0.0;        
float avgSpeedKmh = 0.0;        
float maxWattsSession = 0.0;            
float minVoltageSession = 42.0; 
float minVoltageUnderLoadSession = 42.0;
float maxMotorAmpsSession = 0.0;
float maxBatteryAmpsSession = 0.0;
float loadedCellVoltage = 4.2;
uint32_t homeVoltageSecondsSession = 0;
uint32_t limpVoltageSecondsSession = 0;
int rangeAlertState = 0;
int sagEventsSession = 0;

int vescFault = 0;
unsigned long lastVescOkMs = 0;
bool vescLinkOk = false;
bool telemetryLive = false;

// --- remote input + diagnostics (from the VESC; see VescUartTransport) ---
float gPpmDecoded = 0;          // -1..1 throttle (brake..accel)
float gPpmPulseMs = 0;          // last PPM pulse length, ms
bool  gPpmConnected = false;    // valid remote signal present
uint8_t gVescFwMajor = 0, gVescFwMinor = 0;
bool  gSlaveOnline = false;     // second motor responding over CAN
float gMasterMotorAmps = 0, gSlaveMotorAmps = 0;
float gMasterMotorTemp = 0, gSlaveMotorTemp = 0;
float gMasterEscTemp = 0, gSlaveEscTemp = 0;
int   gLastFault = 0;           // most recent non-zero fault (latched until cleared)
// Modern-protocol extras (COMM_GET_VALUES_SETUP_SELECTIVE; zero on legacy ESCs)
bool  gVescModernProto = false; // aggregated setup-values path active
uint8_t gVescNumVescs = 0;      // controllers the master sees on CAN
uint8_t gSlaveCanId = 0;        // auto-detected slave CAN id (0 = none found)
float gVescSpeedKmh = 0;        // ESC-computed speed (its own gearing config)
int   gVescBattPct = 0;         // ESC's own battery estimate, 0-100
float gVescWhLeft = 0;          // Wh remaining per the ESC battery config
float gVescOdoKm = 0;           // ESC persistent odometer
char  gVescHwName[17] = {0};    // ESC hardware name from the FW handshake
// --- adaptive battery calibration (learned on real rides; NVS-persisted) ---
float gPackROhm = 0.045f;       // seed = a healthy 10s6p (cells ~0.037 + wiring); self-corrects
                                // from real current steps, so a specific pack's R is learned,
                                // never baked in. (Was 0.11 = an old pre-rewire 18650 pack.)
float gTypicalRideAmps = 15.0f; // typical battery draw while rolling
float gLearnedPackWh = 0;       // measured deliverable pack Wh (0 = not learned yet)
float gLearnedWhPerKm = 0;      // cross-ride Wh/km EMA (0 = not learned yet)

unsigned long rideStartMs = 0;
float sessionTripStartKm = 0.0;
float tripDistanceKm = 0.0;
float totalDistanceKm = 0.0;
uint32_t tripMovingSec = 0;          // trip time = seconds spent rolling (NOT uptime)
uint32_t sessionMovingStartSec = 0;  // tripMovingSec at session start; AVG uses moving-time since here
unsigned long lastMovedMs = 0;       // millis() at the last rolling sample (0 = not yet moved this boot)
float estimatedRangeKm = 0.0;
float remainingRangeKm = 0.0;
float estimatedLimpRangeKm = 0.0;
float remainingLimpRangeKm = 0.0;
float avgWhPerKm = 0.0;
float rideStartVescWh = 0.0;
float rideStartVescWhRegen = 0.0;
bool  rideEnergyBaselineSet = false;
bool  rangeEstimateReady = false;

int motorHealthPct = 90;
int batteryHealthPct = 85;
int escHealthPct = 75;

TelemetrySample history[HIST_N];
int histHead = 0;
int histCount = 0;
