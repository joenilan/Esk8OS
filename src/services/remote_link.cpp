#include "remote_link.h"

#if EVEE_LINK_ENABLED

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <string.h>

#include <evee_link.h>          // the contract — evee_link repo, pinned in platformio.ini
#include "../esk8os.h"
#include "../transports/VescProtocol.h"

namespace Esk8OS {
namespace RemoteLink {

// ---------------------------------------------------------------------------
// Configuration. EVEE_REMOTE_MAC must be the handheld's MAC — an all-zero MAC
// means unencrypted broadcast, which works for bench bring-up but CANNOT ARM.
// Set it with -DEVEE_REMOTE_MAC="{0x34,0x85,...}" or edit the default here.
// ---------------------------------------------------------------------------
#ifndef EVEE_REMOTE_MAC
#define EVEE_REMOTE_MAC {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
#endif

// Full-scale current at throttle == +/-1000. Starts LOW on purpose. Raise only
// after the arming and the failsafe have been watched working, wheels off the
// ground.
#ifndef EVEE_MAX_MOTOR_CURRENT_A
#define EVEE_MAX_MOTOR_CURRENT_A 20.0f
#endif
#ifndef EVEE_MAX_BRAKE_CURRENT_A
#define EVEE_MAX_BRAKE_CURRENT_A 20.0f
#endif

// EVEE_LINK_LIVE=0 (the default) means the throttle is decided, logged, and then
// NOT sent. Everything runs — the link, the arming, the failsafe, the current
// scaling — with the motor untouched. That is how this gets tested on the real
// board before it is ever allowed to move.
#ifndef EVEE_LINK_LIVE
#define EVEE_LINK_LIVE 0
#endif

static const uint8_t kRemoteMac[6] = EVEE_REMOTE_MAC;

// ---------------------------------------------------------------------------
// Link state. Written by the ESP-NOW callback (WiFi task, core 0), read by
// tick() (vescPollTask, core 0). onRecv writes all four control fields together;
// tick() takes a coherent snapshot of the same four under rxMux, so a poll that
// lands mid-update can never pair a NEW throttle with OLD flags (which would act
// on a fresh KILL/FAULT one tick late). The spinlock disables preemption on the
// core for the few-instruction copy, so the callback and the reader can't
// interleave. (F-1)
// ---------------------------------------------------------------------------
static portMUX_TYPE      rxMux       = portMUX_INITIALIZER_UNLOCKED;
static volatile int16_t  rxThrottle  = 0;
static volatile uint8_t  rxFlags     = 0;
static volatile uint8_t  rxButtons   = 0;
static volatile uint32_t rxLastMs    = 0;
static volatile bool     rxRestarted = false;

static uint32_t gPeerBoot    = 0;
static uint32_t gPeerLastSeq = 0;
static bool     gPeerSeen    = false;
static bool     gSecure      = false;

static EveeLinkState gState        = EVEE_STATE_BOOT;
static uint32_t      gNeutralSince = 0;
static uint32_t      gStatusAt     = 0;

// Button edge/long-press state. The wire carries LEVELS (a held button appears in
// dozens of consecutive packets, so a dropped packet cannot lose a press the way
// a one-shot edge could), so the receiver does the edge detection.
static uint8_t  gLastButtons = 0;
static uint32_t gTripHeldSince = 0;
static bool     gTripFired = false;

// Latched for the UI thread. tick() runs on core 0; the renderer is on core 1.
static volatile bool gPageNextPending = false;
static volatile bool gTripResetPending = false;

bool takePageNext() {
    if (!gPageNextPending) return false;
    gPageNextPending = false;
    return true;
}

bool takeTripReset() {
    if (!gTripResetPending) return false;
    gTripResetPending = false;
    return true;
}

static void handleButtons(uint8_t buttons, uint32_t now) {
    const uint8_t pressed = (uint8_t)(buttons & ~gLastButtons);   // rising edges

    if (pressed & EVEE_BTN_PAGE) gPageNextPending = true;

    // Trip reset is destructive — it throws away the ride's numbers — so it needs
    // a deliberate hold, not a brush. Fires once per hold, not repeatedly.
    if (buttons & EVEE_BTN_TRIP) {
        if (gTripHeldSince == 0) { gTripHeldSince = now; gTripFired = false; }
        if (!gTripFired && (now - gTripHeldSince) >= EVEE_BTN_LONG_MS) {
            gTripResetPending = true;
            gTripFired = true;
            Serial.println("[evee] trip reset (long press)");
        }
    } else {
        gTripHeldSince = 0;
        gTripFired = false;
    }

    gLastButtons = buttons;
}

const char* stateName() {
    switch (gState) {
        case EVEE_STATE_BOOT:     return "BOOT";
        case EVEE_STATE_DISARMED: return "DISARMED";
        case EVEE_STATE_ARMED:    return "ARMED";
        case EVEE_STATE_FAILSAFE: return "FAILSAFE";
        case EVEE_STATE_FAULT:    return "FAULT";
    }
    return "?";
}

bool armed() { return gState == EVEE_STATE_ARMED; }

// The AP and the VESC-Tool bridge must stay down while a throttle could go live.
// Not just while ARMED: a rider mid-arming-run must not have the radio yanked
// out from under them either.
bool blocksRadioAndUart() {
    return gState == EVEE_STATE_ARMED
        || gState == EVEE_STATE_FAILSAFE
        || gNeutralSince != 0;
}

static void setState(EveeLinkState s, const char* why) {
    if (s == gState) return;
    const char* from = stateName();
    gState = s;
    Serial.printf("[evee] %s -> %s (%s)\n", from, stateName(), why);
    gNeutralSince = 0;   // any transition restarts the arming run from scratch
}

// ---------------------------------------------------------------------------
// Frame validation — the contract's rules, verbatim. See docs in evee_link.h.
// ---------------------------------------------------------------------------
static bool validate(const uint8_t* data, int len, bool& restarted) {
    restarted = false;
    if (len < (int)sizeof(EveeHeader)) return false;

    EveeHeader h;
    memcpy(&h, data, sizeof(h));

    if (h.magic   != EVEE_MAGIC)             return false;
    if (h.version != EVEE_LINK_VERSION)      return false;
    if (h.type    != EVEE_PKT_CONTROL)       return false;
    if (len != eveePacketSize(h.type))       return false;

    const bool firstContact = !gPeerSeen;
    if (firstContact || h.boot != gPeerBoot) {
        // The remote power-cycled. Adopt its counter — otherwise its seq restarts
        // at 1, ours stays high, and we reject every packet as a replay forever.
        // And disarm: a remote that rebooted mid-ride must not resume into a
        // throttle the rider is still holding.
        restarted   = !firstContact;
        gPeerSeen   = true;
        gPeerBoot   = h.boot;
        gPeerLastSeq = h.seq;
        return true;
    }

    // Strictly increasing. A stale throttle is worse than a missing one: the
    // failsafe timer catches "missing" and nothing catches "old".
    if (h.seq <= gPeerLastSeq) return false;
    gPeerLastSeq = h.seq;
    return true;
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
static void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    (void)info;
#else
static void onRecv(const uint8_t* mac, const uint8_t* data, int len) {
    (void)mac;
#endif
    bool restarted = false;
    if (!validate(data, len, restarted)) return;

    EveeControl c;
    memcpy(&c, data, sizeof(c));

    // Publish the whole control packet as one unit (F-1).
    portENTER_CRITICAL(&rxMux);
    rxThrottle = eveeClampThrottle(c.throttle);   // never trust the sender's range
    rxFlags    = c.flags;
    rxButtons  = c.buttons;
    rxLastMs   = millis();
    if (restarted) rxRestarted = true;
    portEXIT_CRITICAL(&rxMux);
}

// ---------------------------------------------------------------------------
void begin() {
    Serial.println("[evee] EVEE Link receiver mode");
    Serial.printf("[evee] this node: %s\n", WiFi.macAddress().c_str());
#if EVEE_LINK_LIVE
    Serial.println("[evee] THROTTLE OUTPUT IS LIVE — wheels off the ground.");
#else
    Serial.println("[evee] dry run: throttle decided and logged, never sent.");
#endif

    // ESK8OS may bring WiFi up later for the export AP. That AP is refused while
    // blocksRadioAndUart() — it would drag this shared radio off EVEE_CHANNEL and
    // the link would die under a live throttle.
    if (WiFi.getMode() == WIFI_MODE_NULL) WiFi.mode(WIFI_STA);

    esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
    esp_wifi_set_channel(EVEE_CHANNEL, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) {
        Serial.println("[evee] ESP-NOW init failed — receiver mode is DOWN");
        return;
    }
    esp_now_register_recv_cb(onRecv);

    esp_now_peer_info_t peer = {};
    peer.channel = EVEE_CHANNEL;
    peer.ifidx   = WIFI_IF_STA;

    bool zero = true;
    for (int i = 0; i < 6; i++) if (kRemoteMac[i]) zero = false;

    if (zero) {
        memset(peer.peer_addr, 0xFF, 6);   // broadcast: cannot be encrypted
        peer.encrypt = false;
        gSecure = false;
    } else {
        memcpy(peer.peer_addr, kRemoteMac, 6);
        esp_now_set_pmk((const uint8_t*)EVEE_PMK);
        memcpy(peer.lmk, EVEE_LMK, 16);
        peer.encrypt = true;
        gSecure = true;
    }
    esp_now_add_peer(&peer);

    Serial.printf("[evee] link: %s\n",
        gSecure ? "encrypted unicast" : "BROADCAST (unencrypted — cannot arm)");

    gState = EVEE_STATE_DISARMED;
}

// ---------------------------------------------------------------------------
// Write the throttle. Coast means zero current, never brake current: an
// automatic hard brake at speed throws the rider off the board.
// ---------------------------------------------------------------------------
// Slew state. Only acceleration is limited (see the note in evee_link.h), so
// this tracks the last commanded ACCEL current and nothing else.
static float gLastAccelAmps = 0.0f;

// The single place a motor command leaves this firmware. Live or dry, everything
// funnels through here — one send path is one path to audit.
static void emit(Transports::VescProtocol& proto, float amps, bool brake, const char* note) {
#if EVEE_LINK_LIVE
    (void)note;
    if (brake) proto.setCurrentBrake(amps);
    else       proto.setCurrent(amps);
#else
    (void)proto;
    // Log on change only — 100 Hz would drown the console.
    static float lastAmps  = -999.0f;
    static bool  lastBrake = false;
    if (fabsf(amps - lastAmps) > 0.05f || brake != lastBrake) {
        lastAmps = amps; lastBrake = brake;
        Serial.printf("[evee] would send %s %.2f A %s\n",
                      brake ? "BRAKE" : "CURRENT", amps, note ? note : "");
    }
#endif
}

static void writeThrottle(Transports::VescProtocol& proto, int16_t rawThr) {
    // The wire carries raw linear position; the curve is ours to apply. Tuning
    // the feel here means never reflashing a sealed handheld to do it.
    const int16_t thr = eveeApplyExpo(rawThr, EVEE_EXPO_PCT_DEFAULT);

    float amps  = 0.0f;
    bool  brake = false;

    if (!eveeIsNeutral(thr)) {
        if (thr > 0) {
            amps = ((float)thr / (float)EVEE_THROTTLE_MAX) * EVEE_MAX_MOTOR_CURRENT_A;

            // Slew-limit the rise only. Falling off the throttle is always allowed
            // to happen instantly — the rider asking for LESS power must never be
            // made to wait.
            const float step = EVEE_ACCEL_SLEW_A_PER_S * (EVEE_CONTROL_MS / 1000.0f);
            if (amps > gLastAccelAmps + step) amps = gLastAccelAmps + step;
        } else {
            // Brake is never slew-limited. A mushy, delayed brake is a safety
            // problem, not a comfort feature.
            amps  = ((float)(-thr) / (float)(-EVEE_THROTTLE_MIN)) * EVEE_MAX_BRAKE_CURRENT_A;
            brake = true;
        }
    }

    gLastAccelAmps = brake ? 0.0f : amps;

    char note[40];
    snprintf(note, sizeof(note), "(raw %+d -> %+d)", (int)rawThr, (int)thr);
    emit(proto, amps, brake, note);
}

// Immediate zero. Bypasses the slew limiter on purpose: a failsafe coast must
// drop to zero NOW, not ramp down over a tenth of a second.
static void coast(Transports::VescProtocol& proto) {
    gLastAccelAmps = 0.0f;
    emit(proto, 0.0f, false, "(coast)");
}

static void sendStatus() {
    EveeStatus s = {};
    s.hdr.magic   = EVEE_MAGIC;
    s.hdr.version = EVEE_LINK_VERSION;
    s.hdr.type    = EVEE_PKT_STATUS;
    s.hdr.boot    = 0;                 // we are not the safety-critical sender
    static uint32_t seq = 0;
    s.hdr.seq = ++seq;

    // Straight out of the telemetry model this firmware already maintains — the
    // whole reason the receiver belongs in ESK8OS rather than on its own chip.
    s.state            = (uint8_t)gState;
    s.boardBattPct     = (uint8_t)constrain(currentBatteryPercent, 0, 100);
    s.vescFault        = (uint8_t)vescFault;
    s.speed_kmh_x10    = (int16_t)(currentSpeedKmh * 10.0f);
    s.voltage_x10      = (uint16_t)(currentVoltage * 10.0f);
    s.motorCurrent_x10 = (int16_t)(currentMotorAmps * 10.0f);
    s.motorTemp_c_x10  = (int16_t)(currentMotorTemp * 10.0f);
    s.escTemp_c_x10    = (int16_t)(currentEscTemp * 10.0f);
    s.tripMeters       = 0;

    uint8_t dest[6];
    bool zero = true;
    for (int i = 0; i < 6; i++) if (kRemoteMac[i]) zero = false;
    if (zero) memset(dest, 0xFF, 6); else memcpy(dest, kRemoteMac, 6);

    esp_now_send(dest, (const uint8_t*)&s, sizeof(s));
}

// ---------------------------------------------------------------------------
bool tick(Transports::VescProtocol& proto) {
    const uint32_t now = millis();

    // One coherent snapshot of everything onRecv publishes, so throttle/flags/
    // buttons/timestamp are all from the SAME packet (F-1).
    int16_t  thr;
    uint8_t  flags;
    uint8_t  buttons;
    uint32_t lastMs;
    bool     restarted;
    portENTER_CRITICAL(&rxMux);
    thr       = rxThrottle;
    flags     = rxFlags;
    buttons   = rxButtons;
    lastMs    = rxLastMs;
    restarted = rxRestarted;
    rxRestarted = false;
    portEXIT_CRITICAL(&rxMux);

    const bool linkUp = lastMs != 0 && (now - lastMs) < EVEE_FAILSAFE_MS;

    // Buttons only count while the link is actually up — a stale mask from a
    // remote that vanished must not keep firing page changes.
    if (linkUp) handleButtons(buttons, now);
    else        gLastButtons = 0;

    if (restarted) {
        if (gState == EVEE_STATE_ARMED) setState(EVEE_STATE_DISARMED, "remote rebooted");
    }

    if (flags & EVEE_FLAG_KILL) {
        setState(EVEE_STATE_DISARMED, "kill switch");
    } else if (flags & EVEE_FLAG_THROTTLE_FAULT) {
        // The remote's own sensor reads out of band — its hall or its wiring has
        // failed. Not something to ride through, and not something the rider can
        // clear by wiggling the trigger: it takes the full arming run again.
        setState(EVEE_STATE_FAULT, "remote reports throttle fault");
    }

    switch (gState) {
        case EVEE_STATE_ARMED:
            if (!linkUp) {
                setState(EVEE_STATE_FAILSAFE, "link lost");
                coast(proto);
            } else {
                writeThrottle(proto, thr);
            }
            break;

        case EVEE_STATE_FAILSAFE:
            // Actively command zero rather than going quiet. Going quiet also
            // works — the VESC's own timeout catches it — but that takes up to
            // EVEE_VESC_TIMEOUT_MS and we can be at zero right now.
            coast(proto);
            if (linkUp) setState(EVEE_STATE_DISARMED, "link back — re-arm required");
            break;

        case EVEE_STATE_FAULT:
            coast(proto);
            if (linkUp && !(flags & EVEE_FLAG_THROTTLE_FAULT))
                setState(EVEE_STATE_DISARMED, "throttle fault cleared");
            break;

        case EVEE_STATE_BOOT:
        case EVEE_STATE_DISARMED:
        default:
            // Arming. Every one of these must hold CONTINUOUSLY for
            // EVEE_ARM_NEUTRAL_MS: link up, link encrypted, rider asking, throttle
            // neutral. This is what makes a stuck-open throttle at power-on
            // impossible, and what stops a link recovery from resuming into a
            // throttle that is still held down. There is no faster path in.
            if (linkUp && gSecure &&
                (flags & EVEE_FLAG_ARM_REQUEST) && eveeIsNeutral(thr)) {
                if (gNeutralSince == 0) gNeutralSince = now;
                if (now - gNeutralSince >= EVEE_ARM_NEUTRAL_MS)
                    setState(EVEE_STATE_ARMED, "neutral run complete");
            } else {
                gNeutralSince = 0;
            }
            break;
    }

    if (now - gStatusAt >= EVEE_STATUS_MS) {
        gStatusAt = now;
        sendStatus();
    }

    return gState == EVEE_STATE_ARMED;
}

}  // namespace RemoteLink
}  // namespace Esk8OS

#endif  // EVEE_LINK_ENABLED
