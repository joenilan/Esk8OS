#pragma once

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
    };

    void beginVescUart();
    bool getLatestVescData(RawVescData* outData);
    void setVescPollPaused(bool paused);

}
}
