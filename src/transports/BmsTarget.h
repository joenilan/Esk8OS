#pragma once

#include <stdint.h>
#include <string.h>

namespace Esk8OS { namespace Transports {

struct BmsTarget {
    char address[18] = {};
    uint32_t revision = 0;
};

// Empty explicitly forgets the target. Never turn malformed input into a clear.
inline bool normalizeBmsBleAddress(const char* input, char out[18]) {
    if (!input) return false;
    if (!input[0]) { out[0] = 0; return true; }
    char normalized[18];
    bool allZero = true, allFF = true;
    for (int i = 0; i < 17; ++i) {
        const char c = input[i];
        if (!c) return false;
        if (i % 3 == 2) {
            if (c != ':') return false;
            normalized[i] = ':';
        } else {
            const char lower = (c >= 'A' && c <= 'F') ? c + ('a' - 'A') : c;
            if (!((lower >= '0' && lower <= '9') || (lower >= 'a' && lower <= 'f')))
                return false;
            normalized[i] = lower;
            allZero &= lower == '0';
            allFF &= lower == 'f';
        }
    }
    if (input[17] || allZero || allFF) return false;
    normalized[17] = 0;
    memcpy(out, normalized, sizeof(normalized));
    return true;
}

enum class BmsTargetResult { Saved, Invalid, Busy, StorageError, NotBuilt };

constexpr bool bmsTargetChangeAllowed(bool dashboard, bool demo, float speedKmh,
                                       bool remoteActive) {
    // Comparisons also reject NaN/Inf and motion in reverse.
    return dashboard && !remoteActive &&
           (demo || (speedKmh >= -1.0f && speedKmh <= 1.0f));
}

} }
