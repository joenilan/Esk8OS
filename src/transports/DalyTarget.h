#pragma once
// Compatibility names for the existing Daly adapter and its tests.
#include "transports/BmsTarget.h"
namespace Esk8OS { namespace Transports {
using DalyTarget = BmsTarget;
using DalyTargetResult = BmsTargetResult;
inline bool normalizeDalyAddress(const char* input, char out[18]) {
    return normalizeBmsBleAddress(input, out);
}
constexpr bool dalyTargetChangeAllowed(bool dashboard, bool demo, float speed,
                                       bool remoteActive) {
    return bmsTargetChangeAllowed(dashboard, demo, speed, remoteActive);
}
} }
