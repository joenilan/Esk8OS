#include "esk8os.h"
#include "ui/UiRenderer.h"

uint16_t COL_BG;        
uint16_t COL_BORDER;    
uint16_t COL_DIM;       
uint16_t COL_LABEL;     
uint16_t COL_WHITE;     
uint16_t COL_GREEN;     
uint16_t COL_RED;       
uint16_t COL_ACCENT;    
uint16_t COL_YELLOW;    
uint16_t COL_ORANGE;    

const Theme THEMES[] = {
    { "CAM",      {26,26,26},   {68,68,68},   {136,136,136}, {170,170,170}, {255,255,255},   {0,200,100},   {255,51,51},   {185,80,215},  {255,205,0},   {255,128,0}  },
    { "EMBER",    {20,16,14},   {72,58,48},   {150,122,100}, {192,162,138}, {255,246,236},   {120,200,90},  {255,60,48},   {255,140,40},  {255,205,0},   {255,120,0}  },
    { "ICE",      {18,22,26},   {54,66,74},   {120,140,150}, {162,182,192}, {240,248,255},   {0,210,150},   {255,70,70},   {0,200,230},   {255,210,90},  {255,140,40} },
    { "LIGHT",    {236,236,240},{176,176,182},{120,120,126}, {74,74,80},    {24,24,28},      {0,150,72},    {210,32,32},   {150,44,190},  {190,140,0},   {214,96,0}   },
    { "CYBER",    {10,8,18},    {62,30,84},   {124,92,162},  {186,142,224}, {228,230,255},   {0,255,180},   {255,42,120},  {255,44,204},  {255,238,60},  {255,120,200}},
    { "SYNTHWAVE",{22,12,34},   {70,40,90},   {150,110,170}, {210,150,200}, {250,240,255},   {60,240,200},  {255,80,110},  {255,90,170},  {255,210,100}, {255,150,90} },
    { "MONO",     {16,16,16},   {64,64,64},   {130,130,130}, {180,180,180}, {245,245,245},   {200,200,200}, {235,235,235}, {255,255,255}, {210,210,210}, {225,225,225}},
    { "FOREST",   {14,22,16},   {48,70,52},   {110,140,116}, {160,190,164}, {236,246,238},   {90,220,120},  {240,90,70},   {120,210,110}, {220,200,90},  {235,150,70} },
};
const int THEME_COUNT = sizeof(THEMES) / sizeof(THEMES[0]);

void applyTheme(int idx) {
    if (idx < 0 || idx >= THEME_COUNT) idx = 0;
    const Theme& t = THEMES[idx];
    COL_BG     = tft.color565(t.bg.r,     t.bg.g,     t.bg.b);
    COL_BORDER = tft.color565(t.border.r, t.border.g, t.border.b);
    COL_DIM    = tft.color565(t.dim.r,    t.dim.g,    t.dim.b);
    COL_LABEL  = tft.color565(t.label.r,  t.label.g,  t.label.b);
    COL_WHITE  = tft.color565(t.white.r,  t.white.g,  t.white.b);
    COL_GREEN  = tft.color565(t.green.r,  t.green.g,  t.green.b);
    COL_RED    = tft.color565(t.red.r,    t.red.g,    t.red.b);
    COL_ACCENT = tft.color565(t.accent.r, t.accent.g, t.accent.b);
    COL_YELLOW = tft.color565(t.yellow.r, t.yellow.g, t.yellow.b);
    COL_ORANGE = tft.color565(t.orange.r, t.orange.g, t.orange.b);
}
