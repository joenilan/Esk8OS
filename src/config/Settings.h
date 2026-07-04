#pragma once
#include "transports/VescUartTransport.h"

namespace Esk8OS {
namespace Settings {
    void begin();

    // VESC-read base tier. Called by telemetry when a live mcconf parse lands:
    // re-derives every value the rider hasn't explicitly overridden and caches
    // the base in NVS so bench sessions (pack off) keep the real numbers.
    void applyVescBase(const Transports::VescBaseConfig& b);

    // Cached-or-live base config; false until a FW-matched mcconf has ever
    // parsed on this board.
    bool vescBase(Transports::VescBaseConfig* out);

    // Provenance of a value for the cfg display: "rider" (explicit NVS
    // override), "vesc" (base tier), or "default" (neutral generic).
    // vescProvides=false for values the ESC can't supply (e.g. Wh/mi).
    const char* sourceTag(const char* nvsKey, bool vescProvides = true);

    // Delete a rider override so the value falls back to the VESC base (or
    // the generic default). Whitelisted keys only; false = unknown key.
    bool removeOverride(const char* nvsKey);
}
}
