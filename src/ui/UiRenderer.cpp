#include "ui/UiRenderer.h"
#include "board/BoardLilyGoTDisplayS3.h"

LGFX tft;
LGFX_Sprite canvas(&tft);
lgfx::LovyanGFX* GFX = &tft;

bool gUseCanvas = false;
uint16_t gFps = 0;            
unsigned long gLastPushUs = 0; 

int X0 = 0; 
bool gRedrawAll = true;
int gCanvasH = 320;
bool gCanvasPsram = false;

struct DirtyBand { int16_t y0, y1; };
const int MAX_DIRTY = 10;
DirtyBand gDirty[MAX_DIRTY];
int gDirtyN = 0;

unsigned long gToastUntil = 0;
char gToastMsg[20] = "";

namespace Esk8OS {
namespace UiRenderer {

void begin() {
    tft.init();
    tft.setRotation(DISPLAY_ROTATION);
    tft.setBrightness(0);
    tft.fillScreen(0);

    X0 = (tft.width() - UI_W) / 2;
    if (X0 < 0) X0 = 0;

    #ifndef WOKWI_SIMULATION
    canvas.setColorDepth(16);
    canvas.setPsram(false);
    void* cbuf = canvas.createSprite(tft.width(), tft.height());
    if (cbuf == nullptr) {
        canvas.setPsram(true);
        cbuf = canvas.createSprite(tft.width(), tft.height());
        gCanvasPsram = true;
    }
    if (cbuf != nullptr) {
        GFX = &canvas;
        gUseCanvas = true;
        gCanvasH = canvas.height();
    }
    #endif
}

void markDirty(int y, int h) {
    if (!gUseCanvas) return;
    int y0 = y < 0 ? 0 : y;
    int y1 = y + h;
    if (y1 > gCanvasH) y1 = gCanvasH;
    if (y1 <= y0) return;
    for (int i = 0; i < gDirtyN; i++) {
        if (y0 <= gDirty[i].y1 && y1 >= gDirty[i].y0) {
            if (y0 < gDirty[i].y0) gDirty[i].y0 = y0;
            if (y1 > gDirty[i].y1) gDirty[i].y1 = y1;
            return;
        }
    }
    if (gDirtyN < MAX_DIRTY) { gDirty[gDirtyN].y0 = y0; gDirty[gDirtyN].y1 = y1; gDirtyN++; }
    else { gDirty[0].y0 = 0; gDirty[0].y1 = gCanvasH; gDirtyN = 1; }
}

void pushCanvas() {
    if (!gUseCanvas || gDirtyN == 0) return;
    unsigned long t0 = micros();
    for (int i = 0; i < gDirtyN; i++) {
        int y0 = gDirty[i].y0;
        int h  = gDirty[i].y1 - y0;
        tft.setClipRect(0, y0, tft.width(), h);
        canvas.pushSprite(0, 0);
    }
    tft.clearClipRect();
    gLastPushUs = micros() - t0;
    gDirtyN = 0;

    static unsigned long fpsWindow = 0;
    static uint16_t fpsCount = 0;
    fpsCount++;
    unsigned long now = millis();
    if (now - fpsWindow >= 1000) { gFps = fpsCount; fpsCount = 0; fpsWindow = now; }
}

void pushCanvasFull() {
    markDirty(0, gCanvasH);
    pushCanvas();
}

}
}

// These are global wrappers to satisfy esk8os.h definitions for now, since 
// the rest of the app calls them. They redirect to the module.
void markDirty(int y, int h) { Esk8OS::UiRenderer::markDirty(y, h); }
void pushCanvasFull() { Esk8OS::UiRenderer::pushCanvasFull(); }
