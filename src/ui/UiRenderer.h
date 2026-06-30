#pragma once
#include "esk8os.h"

#if ESK8OS_DISPLAY_TFT
extern LGFX tft;
#endif

namespace Esk8OS {
namespace UiRenderer {

    void begin();
    void showBootSplash(uint8_t progressPct, const char* status);
    void pushCanvas();
    void pushCanvasFull();
    void markDirty(int y, int h);
    void renderMiniFrame(int alertState);
    void renderTftScreensaver();
    void applyOledInvert();
    void applyDisplayBrightness();
    void previewOledScreensaver(unsigned long durationMs);

}
}
