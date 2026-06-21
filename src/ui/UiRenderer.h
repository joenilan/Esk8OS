#pragma once
#include "esk8os.h"

extern LGFX tft;

namespace Esk8OS {
namespace UiRenderer {

    void begin();
    void pushCanvas();
    void pushCanvasFull();
    void markDirty(int y, int h);

}
}
