// ============================================================================
// EVEE Link receiver, hosted inside ESK8OS.
//
// The same wire contract as the standalone receiver in the evee_link repo
// (include/evee_link.h, pulled in as a pinned PlatformIO dependency) — this is
// just a second implementation of it, one that happens to already own the VESC
// UART and the telemetry model.
//
// Compiled out entirely unless -DEVEE_LINK_ENABLED=1. Your ride firmware is
// byte-for-byte unchanged unless you ask for this.
//
// Where it runs: tick() is called from vescPollTask, which is pinned to core 0
// and is the sole owner of the VESC UART. That is deliberate. The throttle must
// not share the UART with a concurrent poll (interleaved writes corrupt frames)
// and must not share a core with the renderer (a redraw must not stretch a
// control tick). Both fall out for free by living in that one task.
// ============================================================================
#pragma once
#include <stdint.h>

namespace Esk8OS { namespace Transports { class VescProtocol; } }

namespace Esk8OS {
namespace RemoteLink {

#if EVEE_LINK_ENABLED

void begin();

// Called every control tick (EVEE_CONTROL_MS) from vescPollTask. Runs the arming
// state machine and writes the throttle. Returns true while ARMED, which the
// caller uses to know it must keep its own telemetry poll short — see the note
// in VescUartTransport.cpp.
bool tick(Esk8OS::Transports::VescProtocol& proto);

bool armed();
const char* stateName();

// Button actions, consumed by App::loop() on the UI core.
//
// tick() runs on core 0 in the throttle loop; the renderer runs on core 1. A page
// change or a trip reset mutates UI state and repaints, so it must NOT be called
// from the throttle task — it would race the renderer, and it would put a screen
// repaint on the critical path of a 100 Hz control loop. So tick() only latches a
// request, and the UI thread picks it up.
//
// take*() returns true once per press and clears the latch.
bool takePageNext();
bool takeTripReset();

// True while the link is armed OR could arm. The WiFi AP and the VESC-Tool
// bridge both refuse to start while this is set: the AP would drag the shared
// radio off EVEE_CHANNEL, and the bridge would hand the UART to VESC Tool.
// Neither is survivable for a live throttle.
bool blocksRadioAndUart();

#else

// Compiled-out stubs so callers need no #ifdefs.
inline void begin() {}
inline bool armed() { return false; }
inline bool blocksRadioAndUart() { return false; }
inline const char* stateName() { return "OFF"; }
inline bool takePageNext() { return false; }
inline bool takeTripReset() { return false; }

#endif

}  // namespace RemoteLink
}  // namespace Esk8OS
