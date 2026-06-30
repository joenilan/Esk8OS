#pragma once

namespace Esk8OS {
namespace App {
    void setup();
    void loop();
    void resetTrip();   // zero current ride/trip metrics + repaint (long-press L, or BLE TRIP_RESET)
}
}
