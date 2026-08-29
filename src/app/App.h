#pragma once
#include <stdint.h>

namespace Esk8OS {
namespace App {
    enum class TripResetReason : uint8_t {
        Physical,
        CompanionBle,
        Console,
        Remote,
        AutoParked,
    };

    void setup();
    void loop();
    // Zero current ride/trip metrics, persist them, and write an origin-specific
    // black-box event so a future log never leaves a reset ambiguous.
    void resetTrip(TripResetReason reason);
    void pageRel(int dir);  // step prev/next page in the board's PAGE_ORDER (BLE PAGE_NEXT/PREV)
}
}
