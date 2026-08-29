#pragma once

#include <stdint.h>

namespace Esk8OS {
namespace App {

static constexpr uint32_t TRIP_PARK_RESET_MS =
    6UL * 60UL * 60UL * 1000UL;

// millis() wraps, so elapsed-time checks intentionally use unsigned
// subtraction. The signed half-range guard is equally important: it rejects a
// timestamp captured just before telemetry updated lastMovedMs. Without that
// guard, a 1 ms "future" lastMovedMs looks like roughly 49 days parked.
constexpr bool tripParkResetDue(uint32_t nowMs,
                                uint32_t lastMovedMs,
                                float tripKm,
                                bool demoMode) {
    // Arduino's toolchain builds as C++11, where constexpr bodies must be one
    // return expression.
    return !demoMode && lastMovedMs != 0 && tripKm > 0.0f &&
           static_cast<int32_t>(nowMs - lastMovedMs) >= 0 &&
           (nowMs - lastMovedMs) > TRIP_PARK_RESET_MS;
}

static_assert(!tripParkResetDue(1000, 1001, 1.0f, false),
              "a newer lastMovedMs must not underflow into an auto-reset");
static_assert(!tripParkResetDue(TRIP_PARK_RESET_MS, 1, 1.0f, false),
              "the parked threshold is strictly greater-than");
static_assert(tripParkResetDue(TRIP_PARK_RESET_MS + 2, 1, 1.0f, false),
              "a real six-hour parked interval must reset");
static_assert(!tripParkResetDue(0x00000100u, 0xFFFFFF00u, 1.0f, false),
              "a normal millis rollover must remain a short interval");
static_assert(!tripParkResetDue(TRIP_PARK_RESET_MS + 2, 1, 0.0f, false),
              "an empty trip must not reset");
static_assert(!tripParkResetDue(TRIP_PARK_RESET_MS + 2, 1, 1.0f, true),
              "demo trips must not auto-reset");

}  // namespace App
}  // namespace Esk8OS
