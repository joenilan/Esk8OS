#include "transports/BmsDriver.h"
#include "transports/DalyBms.h"

namespace Esk8OS { namespace Transports {

const BmsDriver* activeBmsDriver() {
#if ESK8OS_BMS_DALY
    static const BmsDriver daly = {
        "daly", "Daly Smart BMS (UART K BLE)", beginDalyBms, getDalyBmsData,
        loadDalyTarget, getDalyTarget, setDalyTarget
    };
    return &daly;
#else
    return nullptr;
#endif
}

void beginBms() {
    if (const auto* driver = activeBmsDriver()) driver->begin();
}
bool getBmsData(BmsData* out) {
    if (const auto* driver = activeBmsDriver()) return driver->read(out);
    *out = BmsData{};
    return false;
}
void loadBmsTarget() {
    if (const auto* driver = activeBmsDriver()) driver->loadTarget();
}
BmsTarget getBmsTarget() {
    if (const auto* driver = activeBmsDriver()) return driver->target();
    return BmsTarget{};
}
BmsTargetResult setBmsTarget(const char* address) {
    if (const auto* driver = activeBmsDriver()) return driver->setTarget(address);
    return BmsTargetResult::NotBuilt;
}

} }
