#pragma once

namespace Esk8OS {
namespace App {
    void setup();
    void loop();
    void resetTrip();   // zero current ride/trip metrics + repaint (long-press L, or BLE TRIP_RESET)
    void pageRel(int dir);  // step prev/next page in the board's PAGE_ORDER (BLE PAGE_NEXT/PREV)
}
}
