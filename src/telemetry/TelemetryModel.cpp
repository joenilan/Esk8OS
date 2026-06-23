#include "telemetry/TelemetryModel.h"

float currentSpeedKmh = 0.0;
float currentSpeedMph = 0.0;
float currentVoltage = 42.0; // Overwritten at boot by BATTERY_MAX_V
int   currentBatteryPercent = 100;
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
float maxMotorAmpsSession = 0.0;        

int vescFault = 0;              
unsigned long lastVescOkMs = 0; 
bool vescLinkOk = true;         

unsigned long rideStartMs = 0;
float sessionTripStartKm = 0.0;
float tripDistanceKm = 0.0;
float totalDistanceKm = 0.0;
uint32_t tripMovingSec = 0;          // trip time = seconds spent rolling (NOT uptime)
uint32_t sessionMovingStartSec = 0;  // tripMovingSec at session start; AVG uses moving-time since here
unsigned long lastMovedMs = 0;       // millis() at the last rolling sample (0 = not yet moved this boot)
float estimatedRangeKm = 0.0;
float remainingRangeKm = 0.0;
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
