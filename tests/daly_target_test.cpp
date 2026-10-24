#include "transports/DalyTarget.h"
#include <assert.h>
#include <limits>

using namespace Esk8OS::Transports;

int main() {
    char out[18] = "c2:11:22:33:44:55";
    assert(normalizeDalyAddress("A1:B2:C3:D4:E5:F6", out));
    assert(!strcmp(out, "a1:b2:c3:d4:e5:f6"));
    const char* invalid[] = {"a", "a1:b2:c3:d4:e5:f", "a1:b2:c3:d4:e5:f6x",
        "a1-b2-c3-d4-e5-f6", "gg:b2:c3:d4:e5:f6", "00:00:00:00:00:00",
        "ff:ff:ff:ff:ff:ff", " a1:b2:c3:d4:e5:f6", nullptr};
    for (auto address : invalid) {
        assert(!normalizeDalyAddress(address, out));
        assert(!strcmp(out, "a1:b2:c3:d4:e5:f6"));
    }
    assert(normalizeDalyAddress("", out) && !out[0]);
    assert(dalyTargetChangeAllowed(true, false, 0.0f, false));
    assert(dalyTargetChangeAllowed(true, false, 1.0f, false));
    assert(!dalyTargetChangeAllowed(true, false, 1.01f, false));
    assert(!dalyTargetChangeAllowed(true, false, -1.01f, false));
    assert(!dalyTargetChangeAllowed(false, true, 0.0f, false));
    assert(!dalyTargetChangeAllowed(true, true, 0.0f, true));
    assert(dalyTargetChangeAllowed(true, true, 30.0f, false));
    assert(!dalyTargetChangeAllowed(true, false, std::numeric_limits<float>::infinity(), false));
    assert(!dalyTargetChangeAllowed(true, false, std::numeric_limits<float>::quiet_NaN(), false));
}
