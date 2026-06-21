#pragma once

namespace Esk8OS {
namespace App {
    void setup();
    void loop();
    void resetTrip();   // zero the session/trip metrics + repaint (long-press L, or BLE TRIP_RESET)
}
}
