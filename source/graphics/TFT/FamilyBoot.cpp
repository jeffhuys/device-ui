#if defined(FAMILY_UI) && defined(VIEW_320x240)

// The boot animation: from the splash straight to the family screen, so MUI never shows on the way.
//   hold      the splash as it is, so the hand-over from MUI's boot screen is seamless
//   glitch    the logo tears into horizontal slices that jitter, invert and throw off noise
//   shatter   the slices shoot off to the sides, trailing speed lines
//   pulse     a node in the middle sends out chamfered rings, over a status block whose bar follows
//             the radio's config and the message log. It loops until the node is ready
//   assemble  the slices fly back in from both sides and slam into the logo: a flash, a shake
//   wipe      a white 45-degree slash sweeps across and leaves the family screen behind it
// A node that still needs setup (no region, no family key) gets the same ending, with MUI behind it.
//
// One full-screen object draws a list of primitives that bootFrame() rebuilds every frame from the
// time since the start. Each primitive has a key; only those that changed since the previous frame
// are invalidated, so the panel gets small partial flushes and the frame rate stays up. The object
// also tells LVGL it covers the screen, so MUI underneath is not drawn at all until the wipe.

#include "graphics/view/TFT/FamilyScreen.h"
#include "graphics/view/TFT/TFTView_320x240.h"
#include "Arduino.h"
#include "family_strings.h"
#include "lvgl_private.h"
#include "ui.h"
#include "util/ILog.h"
#include <cmath>
#include <cstring>
#if defined(ARCH_ESP32)
#include "esp_heap_caps.h"
#endif

extern bool familyFilterBypass; // FamilyTheme.cpp: hand pixels to the panel without the grey curve

LV_IMAGE_DECLARE(family_logo_voxl);
LV_IMAGE_DECLARE(family_logo_network);
LV_FONT_DECLARE(family_font_16);

#define LV_COLOR_HEX(C)                                                                                                          \
    {                                                                                                                            \
        .blue = (C >> 0) & 0xff, .green = (C >> 8) & 0xff, .red = (C >> 16) & 0xff                                               \
    }

namespace
{
constexpr lv_color_t black = LV_COLOR_HEX(0x000000);
constexpr lv_color_t white = LV_COLOR_HEX(0xffffff);
constexpr int32_t W = 320, H = 240;

// where the splash (assets/VOXLnet.png) has its two parts; see LOGO_PARTS in scripts/gen-family-assets.py
constexpr int32_t c_voxlX = 30, c_voxlY = 73;
constexpr int32_t c_networkX = 100, c_networkY = 147;
constexpr int32_t c_logoX1 = 22, c_logoY1 = 66, c_logoX2 = 298, c_logoY2 = 186; // the logo with a margin

// timeline, in ms from the first frame
constexpr uint32_t c_hold = 100;
constexpr uint32_t c_glitchEnd = 520;
constexpr uint32_t c_shatter = 160, c_shatterStagger = 6; // per slice
constexpr uint32_t c_pulseStart = 580;
constexpr uint32_t c_ringFirst = 680, c_ringEvery = 300, c_ringLife = 1200;
constexpr uint32_t c_minOutro = 1400;   // the pulse shows at least until here: the first ring has found every house
constexpr uint32_t c_restoreWait = 400; // after config, wait this long for the message log at most; the count follows
constexpr uint32_t c_assemble = 380, c_assembleSlice = 260, c_assembleStagger = 9;
constexpr uint32_t c_flash = 70, c_shake = 100;
constexpr uint32_t c_wipe = 280;
constexpr uint32_t c_frameMs = 15;

constexpr int32_t c_nodeX = 160, c_nodeY = 98;
constexpr int32_t c_panelX1 = 26, c_panelX2 = 293, c_panelY = 170, c_panelH = 54;
constexpr int c_segments = 10;
constexpr int32_t c_band = 26; // the wipe's white slash, measured along a row

// the other houses, found by the pulse: a link grows out to each, and each answers every ring
struct House {
    int32_t x, y;
};
constexpr House houses[] = {{50, 40}, {274, 52}, {34, 136}, {252, 146}};
constexpr int c_houses = sizeof(houses) / sizeof(houses[0]);
uint32_t houseHit[c_houses]; // ms after a ring's spawn that its edge reaches the house

// a ring's half size, a in 0..1 of its life
int32_t ringHalf(float a)
{
    return 12 + (int32_t)((1 - (1 - a) * (1 - a)) * 210);
}

// ----- slices of the logo -----

struct Slice {
    const lv_image_dsc_t *img;
    int32_t x, y;      // where the image's top-left sits on the splash
    int32_t top, rows; // the rows of the image this slice shows
};

constexpr int c_voxlSlices = 10, c_networkSlices = 3, c_slices = c_voxlSlices + c_networkSlices;
Slice slices[c_slices];

void initSlices(void)
{
    for (int i = 0; i < c_voxlSlices; i++)
        slices[i] = {&family_logo_voxl, c_voxlX, c_voxlY, i * 7, 7};
    for (int i = 0; i < c_networkSlices; i++)
        slices[c_voxlSlices + i] = {&family_logo_network, c_networkX, c_networkY, i * 11, 11};
    for (int j = 0; j < c_houses; j++) {
        // a square ring reaches a point at its larger distance along x or y; invert ringHalf()
        float d = LV_MAX(LV_ABS(houses[j].x - c_nodeX), LV_ABS(houses[j].y - c_nodeY));
        houseHit[j] = (uint32_t)((1 - sqrtf(1 - (d - 12) / 210)) * c_ringLife);
    }
}

// ----- primitives -----

struct Prim {
    enum Kind : uint8_t { eRect, eTri, eLine, eImage, eText };
    uint16_t key; // 0: not tracked, the caller marks what changes
    Kind kind;
    lv_color_t color;
    lv_area_t bounds; // rect: the rect; image and text: their clip
    int32_t x, y;     // image: its top-left; line: its width in x
    lv_point_precise_t p[3];
    const void *src; // image or text
};

// in LVGL's pool (PSRAM on the T-Deck) while the animation runs, freed after it
constexpr int c_maxPrims = 160, c_maxDirty = 2 * c_maxPrims, c_maxInvalid = 24, c_maxMerge = 64;
// what one more area costs to render and flush, in pixels: two areas closer than this are joined
constexpr int64_t c_areaCost = 600;
Prim *prims = nullptr, *prevPrims = nullptr;
lv_area_t *dirtyAreas = nullptr;
int primCount = 0, prevPrimCount = 0;
int dirtyCount = 0;
bool dirtyAll = false;

Prim *addPrim(uint16_t key, Prim::Kind kind, lv_color_t color)
{
    if (primCount == c_maxPrims)
        return nullptr;
    Prim *p = &prims[primCount++];
    memset(p, 0, sizeof(*p)); // the frames are compared bytewise
    p->key = key;
    p->kind = kind;
    p->color = color;
    return p;
}

void markDirty(int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    x1 = LV_MAX(x1, 0);
    y1 = LV_MAX(y1, 0);
    x2 = LV_MIN(x2, W - 1);
    y2 = LV_MIN(y2, H - 1);
    if (x1 > x2 || y1 > y2)
        return;
    if (dirtyCount == c_maxDirty) {
        dirtyAll = true;
        return;
    }
    dirtyAreas[dirtyCount++] = {x1, y1, x2, y2};
}

void markDirty(const lv_area_t &a)
{
    markDirty(a.x1, a.y1, a.x2, a.y2);
}

void rect(uint16_t key, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t color)
{
    if (x1 > x2 || y1 > y2 || x2 < 0 || y2 < 0 || x1 >= W || y1 >= H)
        return;
    if (Prim *p = addPrim(key, Prim::eRect, color))
        p->bounds = {x1, y1, x2, y2};
}

void tri(uint16_t key, lv_point_precise_t a, lv_point_precise_t b, lv_point_precise_t c, lv_color_t color)
{
    if (Prim *p = addPrim(key, Prim::eTri, color)) {
        p->p[0] = a;
        p->p[1] = b;
        p->p[2] = c;
        p->bounds = {(int32_t)LV_MIN3(a.x, b.x, c.x) - 1, (int32_t)LV_MIN3(a.y, b.y, c.y) - 1,
                     (int32_t)LV_MAX3(a.x, b.x, c.x) + 1, (int32_t)LV_MAX3(a.y, b.y, c.y) + 1};
    }
}

void line(uint16_t key, lv_point_precise_t a, lv_point_precise_t b, int32_t width, lv_color_t color)
{
    if (Prim *p = addPrim(key, Prim::eLine, color)) {
        p->p[0] = a;
        p->p[1] = b;
        p->x = width;
        p->bounds = {(int32_t)LV_MIN(a.x, b.x) - width, (int32_t)LV_MIN(a.y, b.y) - width, (int32_t)LV_MAX(a.x, b.x) + width,
                     (int32_t)LV_MAX(a.y, b.y) + width};
    }
}

void image(uint16_t key, const lv_image_dsc_t *img, int32_t x, int32_t y, lv_area_t clip, lv_color_t color)
{
    lv_area_t a = {x, y, x + (int32_t)img->header.w - 1, y + (int32_t)img->header.h - 1};
    lv_area_t screen = {0, 0, W - 1, H - 1};
    if (!lv_area_intersect(&clip, &clip, &a) || !lv_area_intersect(&clip, &clip, &screen))
        return;
    if (Prim *p = addPrim(key, Prim::eImage, color)) {
        p->src = img;
        p->x = x;
        p->y = y;
        p->bounds = clip;
    }
}

void text(uint16_t key, const char *s, lv_area_t area, lv_color_t color)
{
    if (Prim *p = addPrim(key, Prim::eText, color)) {
        p->src = s;
        p->bounds = area;
    }
}

/**
 * A block with its bottom-right corner cut at 45 degrees, as on the family screen. Three primitives.
 */
void solidChamfer(uint16_t key, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t cut, lv_color_t color)
{
    const int32_t X2 = x2 + 1, Y2 = y2 + 1;
    rect(key, x1, y1, x2, Y2 - cut - 1, color);
    rect(key + 1, x1, Y2 - cut, X2 - cut - 1, y2, color);
    tri(key + 2, {X2 - cut, Y2 - cut}, {X2, Y2 - cut}, {X2 - cut, Y2}, color);
}

/**
 * A frame w wide with its bottom-right corner cut: four edges and a band of two triangles along the cut,
 * the band as wide as the edges (w * sqrt 2 along them). Six primitives.
 */
void frameChamfer(uint16_t key, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t w, int32_t cut, lv_color_t color)
{
    const int32_t X2 = x2 + 1, Y2 = y2 + 1, s = w * 3 / 2;
    rect(key, x1, y1, x2, y1 + w - 1, color);                // top
    rect(key + 1, x1, y1, x1 + w - 1, y2, color);            // left
    rect(key + 2, x1, Y2 - w, X2 - cut - 1, y2, color);      // bottom
    rect(key + 3, X2 - w, y1, x2, Y2 - cut - 1, color);      // right
    if (X2 > W || Y2 > H || X2 < 0 || Y2 < 0)
        return; // the corner is off the screen; its band alone would look like a stray triangle
    tri(key + 4, {X2 - cut - s, Y2}, {X2 - cut, Y2}, {X2, Y2 - cut - s}, color);
    tri(key + 5, {X2 - cut, Y2}, {X2, Y2 - cut}, {X2, Y2 - cut - s}, color);
}

/**
 * Slice i of the logo, moved by dx, dy; nothing left of clipX.
 */
void slice(uint16_t key, int i, int32_t dx, int32_t dy, lv_color_t color, int32_t clipX = INT32_MIN)
{
    const Slice &s = slices[i];
    lv_area_t clip = {s.x + dx, s.y + s.top + dy, s.x + dx + (int32_t)s.img->header.w - 1, s.y + s.top + s.rows - 1 + dy};
    clip.x1 = LV_MAX(clip.x1, clipX);
    image(key, s.img, s.x + dx, s.y + dy, clip, color);
}

/**
 * A slice in flight, moved by dx and heading right (dir > 0) or left, leaves a short line behind it
 * on its middle row.
 */
void speedLine(uint16_t key, int i, int32_t dx, int32_t dir)
{
    if (LV_ABS(dx) < 24)
        return;
    const Slice &s = slices[i];
    int32_t y = s.y + s.top + s.rows / 2;
    int32_t len = LV_MIN(LV_ABS(dx) / 2, 90);
    if (dir > 0)
        rect(key, s.x + dx - len, y, s.x + dx - 1, y, white);
    else
        rect(key, s.x + (int32_t)s.img->header.w + dx, y, s.x + (int32_t)s.img->header.w + dx + len - 1, y, white);
}

// ----- easing -----

float clamp01(float x)
{
    return x < 0 ? 0 : x > 1 ? 1 : x;
}

float easeInCubic(float x)
{
    return x * x * x;
}

float easeOutCubic(float x)
{
    x = 1 - x;
    return 1 - x * x * x;
}

float easeOutBack(float x)
{
    const float c1 = 1.70158f, c3 = c1 + 1;
    float y = x - 1;
    return 1 + c3 * y * y * y + c1 * y * y;
}

float easeInOutCubic(float x)
{
    return x < 0.5f ? 4 * x * x * x : 1 - (-2 * x + 2) * (-2 * x + 2) * (-2 * x + 2) / 2;
}

uint32_t hash(uint32_t a, uint32_t b)
{
    uint32_t h = a * 0x9e3779b1u ^ (b + 0x7f4a7c15u) * 0x85ebca77u;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    h *= 0x297a2d39u;
    h ^= h >> 15;
    return h;
}

/**
 * The glitch: each slice holds its offset (and whether it is inverted) until it rolls again. Every 70 ms
 * a part of the slices rolls a new offset or heals, so it reads as stepping, and only the slices that
 * rolled are redrawn. Sometimes the letters or the NETWORK block tear as one.
 */
int32_t glitchDx[c_slices];
bool glitchInverted[c_slices];
uint32_t glitchStep = UINT32_MAX;

void glitchTo(uint32_t t, float g)
{
    uint32_t step = t / 70;
    if (step == glitchStep)
        return;
    glitchStep = step;
    int32_t amp = 2 + (int32_t)(26 * g * g);
    uint32_t tear = hash(step, 777);
    bool partTear = tear % 5 == 0, letters = (tear >> 20) & 1;
    int32_t tearDx = (int32_t)((tear >> 8) % (2 * amp + 1)) - amp;
    for (int i = 0; i < c_slices; i++) {
        uint32_t r = hash(step, i);
        if (partTear && (i < c_voxlSlices) == letters) {
            glitchDx[i] = tearDx;
            glitchInverted[i] = false;
        } else if ((r & 0xff) < 70 + 60 * g) {
            bool heal = ((r >> 8) & 0xff) < 60;
            glitchDx[i] = heal ? 0 : (int32_t)((r >> 16) % (2 * amp + 1)) - amp;
            glitchInverted[i] = !heal && ((r >> 24) & 0xff) < 40 * g;
        }
    }
}

/**
 * Short white bars around the logo, n of them, new every 70 ms.
 */
void noise(uint32_t t, int n)
{
    for (int k = 0; k < n; k++) {
        uint32_t r = hash(t / 70, 200 + k);
        int32_t x = r % 300, y = 58 + (r >> 9) % 140;
        rect(300 + k, x, y, x + 6 + (int32_t)((r >> 17) % 50), y + 2 + (int32_t)((r >> 25) % 4), white);
    }
}

/**
 * A pulse around the node at half size h, drawn as a HUD's corner brackets: three corners, and the
 * bottom-right one cut, as a slash. Eight primitives, and only the corners redraw as it grows.
 */
void bracketRing(uint16_t key, int32_t h, int32_t w)
{
    const int32_t x1 = c_nodeX - h, y1 = c_nodeY - h, x2 = c_nodeX + h - 1, y2 = c_nodeY + h - 1;
    const int32_t len = LV_CLAMP(8, h / 3, 28);
    rect(key, x1, y1, x1 + len - 1, y1 + w - 1, white); // top-left
    rect(key + 1, x1, y1, x1 + w - 1, y1 + len - 1, white);
    rect(key + 2, x2 - len + 1, y1, x2, y1 + w - 1, white); // top-right
    rect(key + 3, x2 - w + 1, y1, x2, y1 + len - 1, white);
    rect(key + 4, x1, y2 - w + 1, x1 + len - 1, y2, white); // bottom-left
    rect(key + 5, x1, y2 - len + 1, x1 + w - 1, y2, white);
    const int32_t X2 = x2 + 1, Y2 = y2 + 1, s = w * 3 / 2;
    if (X2 - len - s > W || Y2 - len - s > H)
        return;
    tri(key + 6, {X2 - len - s, Y2}, {X2 - len, Y2}, {X2, Y2 - len - s}, white);
    tri(key + 7, {X2 - len, Y2}, {X2, Y2 - len}, {X2, Y2 - len - s}, white);
}

// ----- a draw buffer in internal RAM, while the animation runs -----

// MUI renders into one screen-sized buffer in PSRAM, which costs the animation most of its frame
// time. Internal RAM is several times faster; the animation borrows a strip of it and gives the
// display its own buffer back at the end. Rendering a large area just takes more strips.
lv_draw_buf_t fastBuf;
void *fastBufMem = nullptr;
lv_draw_buf_t *savedBuf1 = nullptr, *savedBuf2 = nullptr;
uint32_t savedStrideAuto = 0;

void useFastBuffer(lv_display_t *disp)
{
#if defined(ARCH_ESP32)
    if (disp->render_mode != LV_DISPLAY_RENDER_MODE_PARTIAL)
        return;
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t keepFree = 64 * 1024; // for the firmware, while it starts up
    const lv_color_format_t cf = lv_display_get_color_format(disp);
    const uint32_t stride = lv_draw_buf_width_to_stride(W, cf);
    const size_t freeBytes = heap_caps_get_free_size(caps), largest = heap_caps_get_largest_free_block(caps);
    uint32_t rows = 64;
    while (rows >= 16 && ((size_t)stride * rows + keepFree > freeBytes || (size_t)stride * rows + 64 > largest))
        rows -= 8;
    if (rows < 16) {
        ILOG_INFO("family boot: draw buffer stays in PSRAM (%u bytes internal free)", (unsigned)freeBytes);
        return;
    }
    const size_t size = (size_t)stride * rows;
    fastBufMem = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, size, caps);
    if (!fastBufMem)
        return;
    lv_draw_buf_init(&fastBuf, W, rows, cf, stride, fastBufMem, size);
    savedBuf1 = disp->buf_1;
    savedBuf2 = disp->buf_2;
    savedStrideAuto = disp->stride_is_auto;
    lv_display_set_draw_buffers(disp, &fastBuf, nullptr);
    ILOG_INFO("family boot: %u rows of draw buffer in internal RAM (%u bytes internal free)", (unsigned)rows,
              (unsigned)freeBytes);
#endif
}

void restoreBuffer(lv_display_t *disp)
{
#if defined(ARCH_ESP32)
    if (!fastBufMem)
        return;
    lv_display_set_draw_buffers(disp, savedBuf1, savedBuf2);
    disp->stride_is_auto = savedStrideAuto;
    heap_caps_free(fastBufMem);
    fastBufMem = nullptr;
#endif
}

// ----- state -----

bool started = false;    // the first frame on the loaded main screen has run
int32_t outroAt = -1;    // ms the outro started, -1 before
uint32_t configAt = 0;   // ms config was complete, 0 before
bool toFamily = false;   // the outro ends on the family screen, else on MUI (setup)
bool revealed = false;   // the family screen is up under the animation
float progress = 0;      // the status bar, 0..1
uint32_t lastT = 0;
int32_t wipePrevE = -H;  // the wipe's edge in the previous frame
uint32_t savedRefrPeriod = 0;
uint32_t family_boot_start = 0; // the boot animation's clock, for the stats

// what a frame costs, summed over the animation and logged at its end
struct Stats {
    uint32_t renders, lastRender, longestGap;
    uint32_t renderStartUs;
    uint64_t renderUs, flushUs, sceneUs, px;
    uint32_t flushes, flushStartUs, areasBefore, areasAfter, scenes;
    uint8_t perQuarter[48];   // frames in each 250 ms
    uint32_t renderUsQuarter[48]; // and the time spent rendering them
} stats;

/**
 * Tell LVGL the animation covers the screen, so nothing under it is drawn. Not during the wipe.
 */
void ui_event_boot_cover(lv_event_t *e)
{
    if (outroAt < 0 || lastT < (uint32_t)outroAt + c_assemble + c_flash + c_shake)
        lv_event_set_cover_res(e, LV_COVER_RES_COVER);
}

void ui_event_boot_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    const lv_area_t clip = layer->_clip_area;
    for (int i = 0; i < primCount; i++) {
        const Prim &p = prims[i];
        if (!lv_area_is_on(&p.bounds, &clip))
            continue;
        switch (p.kind) {
        case Prim::eRect: {
            lv_draw_rect_dsc_t d;
            lv_draw_rect_dsc_init(&d);
            d.bg_color = p.color;
            d.bg_opa = LV_OPA_COVER;
            d.radius = 0;
            d.border_width = 0;
            d.shadow_width = 0;
            lv_draw_rect(layer, &d, &p.bounds);
            break;
        }
        case Prim::eTri: {
            lv_draw_triangle_dsc_t d;
            lv_draw_triangle_dsc_init(&d);
            d.color = p.color;
            d.opa = LV_OPA_COVER;
            d.p[0] = p.p[0];
            d.p[1] = p.p[1];
            d.p[2] = p.p[2];
            lv_draw_triangle(layer, &d);
            break;
        }
        case Prim::eLine: {
            lv_draw_line_dsc_t d;
            lv_draw_line_dsc_init(&d);
            d.color = p.color;
            d.opa = LV_OPA_COVER;
            d.width = p.x;
            d.p1 = p.p[0];
            d.p2 = p.p[1];
            lv_draw_line(layer, &d);
            break;
        }
        case Prim::eImage: {
            lv_area_t c;
            if (!lv_area_intersect(&c, &clip, &p.bounds))
                break;
            const lv_image_dsc_t *img = (const lv_image_dsc_t *)p.src;
            lv_draw_image_dsc_t d;
            lv_draw_image_dsc_init(&d);
            d.src = img;
            d.recolor = p.color; // an A8 image is drawn in its recolor
            d.recolor_opa = LV_OPA_COVER;
            lv_area_t coords = {p.x, p.y, p.x + (int32_t)img->header.w - 1, p.y + (int32_t)img->header.h - 1};
            layer->_clip_area = c;
            lv_draw_image(layer, &d, &coords);
            layer->_clip_area = clip;
            break;
        }
        case Prim::eText: {
            lv_draw_label_dsc_t d;
            lv_draw_label_dsc_init(&d);
            d.font = &family_font_16;
            d.color = p.color;
            d.text = (const char *)p.src;
            lv_draw_label(layer, &d, &p.bounds);
            break;
        }
        }
    }
}

void ui_event_boot_display(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_RENDER_START: {
        uint32_t now = lv_tick_get();
        if (stats.renders++ && now - stats.lastRender > stats.longestGap)
            stats.longestGap = now - stats.lastRender;
        stats.lastRender = now;
        stats.renderStartUs = micros();
        break;
    }
    case LV_EVENT_RENDER_READY: {
        uint32_t us = micros() - stats.renderStartUs;
        stats.renderUs += us;
        uint32_t q = lv_tick_elaps(family_boot_start) / 250;
        if (q < sizeof(stats.perQuarter)) {
            stats.perQuarter[q]++;
            stats.renderUsQuarter[q] += us;
        }
        break;
    }
    case LV_EVENT_FLUSH_START:
        stats.flushStartUs = micros();
        stats.px += lv_area_get_size((const lv_area_t *)lv_event_get_param(e));
        stats.flushes++;
        break;
    case LV_EVENT_FLUSH_FINISH:
        stats.flushUs += micros() - stats.flushStartUs;
        break;
    default:
        break;
    }
}

/**
 * Mark what differs from the previous frame: changed, new and gone primitives, both where they were
 * and where they are. Untracked ones (key 0) were marked by whoever drew them.
 */
void diffFrames(void)
{
    for (int i = 0; i < primCount; i++) {
        const Prim &p = prims[i];
        if (!p.key)
            continue;
        const Prim *old = nullptr;
        for (int j = 0; j < prevPrimCount; j++) {
            if (prevPrims[j].key == p.key) {
                old = &prevPrims[j];
                break;
            }
        }
        if (!old) {
            markDirty(p.bounds);
        } else if (memcmp(old, &p, sizeof(Prim)) != 0) {
            // a moved edge mostly overlaps where it was: one area for both
            lv_area_t u = {LV_MIN(old->bounds.x1, p.bounds.x1), LV_MIN(old->bounds.y1, p.bounds.y1),
                           LV_MAX(old->bounds.x2, p.bounds.x2), LV_MAX(old->bounds.y2, p.bounds.y2)};
            if (lv_area_get_size(&u) <= 2 * (lv_area_get_size(&old->bounds) + lv_area_get_size(&p.bounds))) {
                markDirty(u);
            } else {
                markDirty(old->bounds);
                markDirty(p.bounds);
            }
        }
    }
    for (int j = 0; j < prevPrimCount; j++) {
        const Prim &old = prevPrims[j];
        if (!old.key)
            continue;
        bool kept = false;
        for (int i = 0; i < primCount && !kept; i++)
            kept = prims[i].key == old.key;
        if (!kept)
            markDirty(old.bounds);
    }
}

/**
 * Join the dirty areas: every pair whose join wastes fewer pixels than another area costs, then the
 * cheapest pairs until LVGL's invalidation buffer can take them.
 */
void mergeDirty(void)
{
    auto size = [](const lv_area_t &a) { return (int64_t)lv_area_get_width(&a) * lv_area_get_height(&a); };
    auto join = [](const lv_area_t &a, const lv_area_t &b) {
        return lv_area_t{LV_MIN(a.x1, b.x1), LV_MIN(a.y1, b.y1), LV_MAX(a.x2, b.x2), LV_MAX(a.y2, b.y2)};
    };
    if (dirtyCount > c_maxMerge) {
        // too many to pair up within a frame (a phase change): one box around all of them
        for (int i = 1; i < dirtyCount; i++)
            dirtyAreas[0] = join(dirtyAreas[0], dirtyAreas[i]);
        dirtyCount = 1;
        return;
    }
    for (;;) {
        int bi = -1, bj = -1;
        int64_t best = INT64_MAX;
        for (int i = 0; i < dirtyCount; i++) {
            for (int j = i + 1; j < dirtyCount; j++) {
                lv_area_t u = join(dirtyAreas[i], dirtyAreas[j]);
                int64_t waste = size(u) - size(dirtyAreas[i]) - size(dirtyAreas[j]);
                if (waste < best) {
                    best = waste;
                    bi = i;
                    bj = j;
                }
            }
        }
        if (bi < 0 || (best > c_areaCost && dirtyCount <= c_maxInvalid))
            break;
        dirtyAreas[bi] = join(dirtyAreas[bi], dirtyAreas[bj]);
        dirtyAreas[bj] = dirtyAreas[--dirtyCount];
    }
}

} // namespace

// ===== FamilyScreen =====

void FamilyScreen::startBoot(void)
{
    prims = (Prim *)lv_malloc(sizeof(Prim) * c_maxPrims);
    prevPrims = (Prim *)lv_malloc(sizeof(Prim) * c_maxPrims);
    dirtyAreas = (lv_area_t *)lv_malloc(sizeof(lv_area_t) * c_maxDirty);
    if (!prims || !prevPrims || !dirtyAreas) {
        ILOG_ERROR("family boot: no memory for the animation");
        lv_free(prims);
        lv_free(prevPrims);
        lv_free(dirtyAreas);
        prims = prevPrims = nullptr;
        dirtyAreas = nullptr;
        return;
    }
    initSlices();
    bootGroup = lv_group_create();
    bootObj = lv_obj_create(objects.main_screen);
    lv_obj_remove_style_all(bootObj);
    lv_obj_set_size(bootObj, W, H);
    lv_obj_set_pos(bootObj, 0, 0);
    lv_obj_clear_flag(bootObj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bootObj, LV_OBJ_FLAG_CLICKABLE); // swallow pointer input meant for MUI underneath
    lv_obj_add_event_cb(bootObj, ui_event_boot_draw, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(bootObj, ui_event_boot_cover, LV_EVENT_COVER_CHECK, nullptr);
    booting = true;
    bootFrame(); // the splash, before anything else gets drawn

    // refresh as often as the panel keeps up with, while the animation runs
    lv_display_t *disp = lv_display_get_default();
    if (lv_timer_t *refr = lv_display_get_refr_timer(disp)) {
        savedRefrPeriod = refr->period;
        lv_timer_set_period(refr, c_frameMs);
    }
    lv_display_add_event_cb(disp, ui_event_boot_display, LV_EVENT_ALL, nullptr);
    useFastBuffer(disp);
    familyFilterBypass = true; // only black and white on the panel until the wipe
    bootTimer = lv_timer_create(timer_boot, c_frameMs, nullptr);
    assignGroup();
    ILOG_INFO("family boot animation started");
}

void FamilyScreen::timer_boot(lv_timer_t *)
{
    if (family && family->booting)
        family->bootFrame();
}

void FamilyScreen::bootFrame(void)
{
    if (!started) {
        // the clock starts when the main screen is on the panel; until then MUI's boot screen shows
        if (!mainScreenActive && lv_screen_active() != objects.main_screen)
            return;
        started = true;
        bootStartTick = lv_tick_get();
        family_boot_start = bootStartTick;
    }
    if (screenSaverActive())
        return; // nothing to see
    const uint32_t t = lv_tick_elaps(bootStartTick);
    const uint32_t dt = t - lastT;
    lastT = t;

    // keep above whatever MUI adds to the main screen
    if (lv_obj_get_index(bootObj) != (int32_t)lv_obj_get_child_count(objects.main_screen) - 1)
        lv_obj_move_foreground(bootObj);

    // ----- when to end -----
    checkRestored();
    if (view->configComplete && !configAt)
        configAt = t ? t : 1;
    if (outroAt < 0 && configAt && t >= c_minOutro) {
        bool family = ready() && view->activeSettings == TFTView_320x240::eNone;
        if (!family || restored || t - configAt >= c_restoreWait) {
            outroAt = t;
            toFamily = family;
            ILOG_INFO("family boot: outro to %s at %u ms", toFamily ? "the family screen" : "MUI", (unsigned)t);
        }
    }
    const uint32_t never = 0x40000000; // far enough that adding a phase does not wrap
    const uint32_t assembleEnd = outroAt < 0 ? never : outroAt + c_assemble;
    const uint32_t wipeAt = assembleEnd + c_flash + c_shake;
    if (t >= wipeAt)
        familyFilterBypass = false; // the family screen or MUI comes into view, in their grey levels
    if (outroAt >= 0 && !revealed) {
        // under the animation, which covers it until the wipe
        revealed = true;
        if (toFamily && ready())
            show();
        lv_obj_move_foreground(bootObj);
    }
    if (t >= wipeAt + c_wipe) {
        endBoot();
        return;
    }

    // ----- progress -----
    float target = !configAt ? 0.08f + 0.62f * (1 - expf(-(float)(t > c_pulseStart ? t - c_pulseStart : 0) / 1400.0f))
                   : !restored ? 0.85f
                               : 1.0f;
    if (outroAt >= 0)
        target = 1.0f;
    progress += (target - progress) * clamp01(dt / 150.0f);

    // ----- the scene -----
    const uint32_t sceneStart = micros();
    memcpy(prevPrims, prims, sizeof(Prim) * primCount);
    prevPrimCount = primCount;
    primCount = 0;
    dirtyCount = 0;
    dirtyAll = false;
    const bool wiping = t >= wipeAt;

    if (!wiping)
        rect(0, 0, 0, W - 1, H - 1, black); // the ground; never changes, never marked

    // links to the houses, behind everything: each grows out when the first ring finds its house
    const bool pulsing = t >= c_ringFirst && (outroAt < 0 || t < (uint32_t)outroAt + 160);
    if (pulsing && outroAt < 0) {
        for (int j = 0; j < c_houses; j++) {
            uint32_t found = c_ringFirst + houseHit[j];
            if (t < found)
                continue;
            float g = easeOutCubic(clamp01((t - found) / 220.0f));
            line(700 + j, {c_nodeX, c_nodeY},
                 {c_nodeX + (int32_t)((houses[j].x - c_nodeX) * g), c_nodeY + (int32_t)((houses[j].y - c_nodeY) * g)}, 2, white);
        }
    }

    // rings
    if (t >= c_ringFirst && !wiping) {
        uint32_t k0 = t > c_ringFirst + c_ringLife ? (t - c_ringFirst - c_ringLife) / c_ringEvery : 0;
        for (uint32_t k = k0;; k++) {
            uint32_t spawn = c_ringFirst + k * c_ringEvery;
            if (spawn > t || (outroAt >= 0 && spawn >= (uint32_t)outroAt))
                break;
            float a = (t - spawn) / (float)c_ringLife;
            if (a >= 1)
                continue;
            int32_t h = ringHalf(a);
            int32_t w = a < 0.2f ? 4 : a < 0.45f ? 3 : 2;
            bracketRing(1000 + (k % 16) * 8, h, w);
        }
    }

    // the houses: a flash when a ring reaches one, then its answer, a small ring of its own
    if (pulsing) {
        uint32_t lastSpawn = outroAt < 0 ? UINT32_MAX : (uint32_t)outroAt - 1;
        float shrink = outroAt < 0 ? 1 : 1 - easeInCubic(clamp01((t - outroAt) / 160.0f));
        for (int j = 0; j < c_houses; j++) {
            uint32_t found = c_ringFirst + houseHit[j];
            if (t < found)
                continue;
            uint32_t k = (LV_MIN(t - houseHit[j], lastSpawn) - c_ringFirst) / c_ringEvery;
            uint32_t since = t - (c_ringFirst + k * c_ringEvery + houseHit[j]);
            const int32_t hx = houses[j].x, hy = houses[j].y;
            int32_t half = (int32_t)(7 * shrink + 0.5f);
            if (half <= 0)
                continue;
            if (since < 140)
                solidChamfer(720 + j * 8, hx - half, hy - half, hx + half - 1, hy + half - 1, half * 2 / 3, white);
            else {
                solidChamfer(800 + j * 4, hx - half, hy - half, hx + half - 1, hy + half - 1, half * 2 / 3, black); // the link ends at the box
                frameChamfer(720 + j * 8, hx - half, hy - half, hx + half - 1, hy + half - 1, 2, half * 2 / 3, white);
            }
            if (since < 380 && outroAt < 0) {
                int32_t r = 9 + (int32_t)(easeOutCubic(since / 380.0f) * 14);
                frameChamfer(760 + j * 8, hx - r, hy - r, hx + r - 1, hy + r - 1, since < 190 ? 2 : 1, r / 2, white);
            }
        }
    }

    // the node: pops up, kicks at every ring, shrinks away in the outro
    if (t >= c_pulseStart && (outroAt < 0 || t < (uint32_t)outroAt + 160)) {
        float size = 22 * easeOutBack(clamp01((t - c_pulseStart) / 180.0f));
        if (t >= c_ringFirst) {
            uint32_t k = (t - c_ringFirst) / c_ringEvery;
            uint32_t spawn = c_ringFirst + k * c_ringEvery;
            if ((outroAt < 0 || spawn < (uint32_t)outroAt) && t - spawn < 90)
                size += 8 * (1 - (t - spawn) / 90.0f);
        }
        if (outroAt >= 0)
            size *= 1 - easeInCubic(clamp01((t - outroAt) / 160.0f));
        int32_t half = (int32_t)(size / 2 + 0.5f);
        if (half > 0)
            solidChamfer(500, c_nodeX - half, c_nodeY - half, c_nodeX + half - 1, c_nodeY + half - 1, LV_MAX(2, half * 2 / 3), white);
    }

    // the status block: slams up from below, drops away in the outro
    if (t >= c_pulseStart + 80 && (outroAt < 0 || t < (uint32_t)outroAt + 240)) {
        int32_t dy = (int32_t)((1 - easeOutBack(clamp01((t - c_pulseStart - 80) / 260.0f))) * 80);
        if (outroAt >= 0)
            dy += (int32_t)(easeInCubic(clamp01((t - outroAt) / 240.0f)) * 90);
        const int32_t y1 = c_panelY + dy, y2 = y1 + c_panelH - 1;
        rect(600, c_panelX1, y1, c_panelX2, y2, black);
        frameChamfer(601, c_panelX1, y1, c_panelX2, y2, 3, 12, white);
        const char *status = !configAt ? FAMILY_STR_BOOT_CONNECTING : !restored ? FAMILY_STR_BOOT_LOADING : FAMILY_STR_BOOT_READY;
        if (outroAt >= 0)
            status = FAMILY_STR_BOOT_READY;
        text(610, status, {c_panelX1 + 16, y1 + 9, c_panelX2 - 16, y1 + 29}, white);
        float lit = progress * c_segments;
        for (int i = 0; i < c_segments; i++) {
            int32_t x = c_panelX1 + 16 + i * 24;
            int32_t sy = y1 + 34;
            if (i < (int)lit)
                rect(620 + i, x, sy, x + 19, sy + 9, white);
            else if (i == (int)lit && (t / 110) % 2 == 0)
                rect(620 + i, x, sy, x + 19, sy + 9, white);
            else
                rect(620 + i, x, sy + 7, x + 19, sy + 9, white);
        }
    }

    // the logo
    if (t < c_glitchEnd) {
        float g = t < c_hold ? 0 : (t - c_hold) / (float)(c_glitchEnd - c_hold);
        if (g > 0)
            glitchTo(t, g);
        for (int i = 0; i < c_slices; i++) {
            bool inverted = g > 0 && glitchInverted[i];
            int32_t dx = g > 0 ? glitchDx[i] : 0;
            if (inverted) {
                const Slice &s = slices[i];
                rect(100 + i, s.x + dx - 8, s.y + s.top, s.x + dx + (int32_t)s.img->header.w + 7, s.y + s.top + s.rows - 1, white);
            }
            slice(120 + i, i, dx, 0, inverted ? black : white);
        }
        if (g > 0) {
            noise(t, (int)(g * 5));
            int32_t y = c_logoY1 + (int32_t)((t - c_hold) * 7 / 20) % (c_logoY2 - c_logoY1); // scan line
            rect(290, 0, y, W - 1, y + 1, white);
        }
    } else if (outroAt < 0 || t < (uint32_t)outroAt) {
        // shatter: off to the sides, the letters' slices alternating, still glitching as they go
        glitchTo(t, 1);
        for (int i = 0; i < c_slices; i++) {
            float u = clamp01((float)((int32_t)(t - c_glitchEnd) - i * (int32_t)c_shatterStagger) / c_shatter);
            int32_t dir = (i & 1) ? 1 : -1;
            int32_t dx = dir * (int32_t)(easeInCubic(u) * 340) + (int32_t)(glitchDx[i] * (1 - u));
            if (LV_ABS(dx) >= 330)
                continue;
            slice(120 + i, i, dx, 0, white);
            speedLine(140 + i, i, dx, dir);
        }
        if (t < c_glitchEnd + 200)
            noise(t, (int)(5 * (1 - (t - c_glitchEnd) / 200.0f)));
    } else if (t < assembleEnd) {
        // assemble: back in from the side each slice left to, overshooting a little
        for (int i = 0; i < c_slices; i++) {
            float v = (float)((int32_t)(t - outroAt) - i * (int32_t)c_assembleStagger) / c_assembleSlice;
            if (v <= 0)
                continue;
            int32_t dir = (i & 1) ? 1 : -1;
            int32_t dx = dir * (int32_t)((1 - easeOutBack(clamp01(v))) * 340);
            slice(120 + i, i, dx, 0, white);
            speedLine(140 + i, i, dx, -dir);
        }
    } else if (!wiping) {
        // the slam: a flash of the inverted logo, then a shake that dies down
        uint32_t s = t - assembleEnd;
        bool flash = s < c_flash;
        int32_t dx = 0;
        if (!flash) {
            static const int8_t shake[] = {5, -3, 2, 0}; // a few steps, each a full redraw of the logo
            dx = shake[LV_MIN((s - c_flash) * 4 / c_shake, 3u)];
        }
        if (flash)
            rect(100, c_logoX1, c_logoY1, c_logoX2, c_logoY2, white);
        for (int i = 0; i < c_slices; i++)
            slice(120 + i, i, dx, 0, flash ? black : white);
    } else {
        // the wipe: the edge x(y) = e + H - y runs from the left to off the right; black and the logo
        // are right of it, a white band along it, the family screen (or MUI) left of it
        float w = clamp01((t - wipeAt) / (float)c_wipe);
        int32_t e = -H + (int32_t)(easeInOutCubic(w) * (W + H + c_band));
        const int32_t far = W + H + 2 * c_band;
        const int32_t x0 = e + H, xH = e;
        tri(0, {x0, 0}, {far, 0}, {xH, H}, black);
        tri(0, {far, 0}, {far, H}, {xH, H}, black);
        for (int i = 0; i < c_slices; i++) {
            const Slice &s = slices[i];
            slice(0, i, 0, 0, white, e + H - (s.y + s.top)); // its top row's edge; the band hides the rest
        }
        tri(0, {x0, 0}, {x0 + c_band, 0}, {xH + c_band, H}, white);
        tri(0, {x0, 0}, {xH + c_band, H}, {xH, H}, white);
        for (int32_t y0 = 0; y0 < H; y0 += 16) {
            int32_t y1 = LV_MIN(y0 + 15, H - 1);
            markDirty(wipePrevE + H - y1 - 1, y0, e + H - y0 + c_band + 1, y1);
        }
        wipePrevE = e;
    }

    diffFrames();
    stats.areasBefore += dirtyCount;
    if (dirtyAll) {
        lv_obj_invalidate(bootObj);
    } else {
        mergeDirty();
        for (int i = 0; i < dirtyCount; i++)
            lv_obj_invalidate_area(bootObj, &dirtyAreas[i]);
    }
    stats.areasAfter += dirtyAll ? 1 : dirtyCount;
    stats.scenes++;
    stats.sceneUs += micros() - sceneStart;
}

void FamilyScreen::endBoot(void)
{
    if (!booting)
        return;
    booting = false;
    bootEndTick = lv_tick_get() ? lv_tick_get() : 1;
    if (bootTimer) {
        lv_timer_delete(bootTimer);
        bootTimer = nullptr;
    }
    lv_display_t *disp = lv_display_get_default();
    lv_display_remove_event_cb_with_user_data(disp, ui_event_boot_display, nullptr);
    restoreBuffer(disp);
    familyFilterBypass = false;
    if (lv_timer_t *refr = lv_display_get_refr_timer(disp))
        lv_timer_set_period(refr, savedRefrPeriod ? savedRefrPeriod : LV_DEF_REFR_PERIOD);
    lv_obj_delete(bootObj);
    bootObj = nullptr;
    lv_free(prims);
    lv_free(prevPrims);
    lv_free(dirtyAreas);
    prims = prevPrims = nullptr;
    dirtyAreas = nullptr;
    primCount = prevPrimCount = 0;
    const uint32_t ms = stats.lastRender - bootStartTick, n = LV_MAX(stats.renders, 1u), k = LV_MAX(stats.scenes, 1u);
    ILOG_INFO("family boot animation: %u frames in %u ms (%u fps), longest gap %u ms", (unsigned)stats.renders, (unsigned)ms,
              (unsigned)(ms ? stats.renders * 1000 / ms : 0), (unsigned)stats.longestGap);
    ILOG_INFO("family boot animation, per frame: render %u us of which flush %u us, %u areas, %u px; scene %u us, %u -> %u dirty",
              (unsigned)(stats.renderUs / n), (unsigned)(stats.flushUs / n), (unsigned)(stats.flushes / n), (unsigned)(stats.px / n),
              (unsigned)(stats.sceneUs / k), (unsigned)(stats.areasBefore / k), (unsigned)(stats.areasAfter / k));
    char frames[48 * 4 + 1] = "", render[48 * 5 + 1] = "";
    for (uint32_t q = 0, f = 0, r = 0; q < sizeof(stats.perQuarter) && q * 250 <= ms; q++) {
        f += lv_snprintf(frames + f, sizeof(frames) - f, "%u ", (unsigned)stats.perQuarter[q]);
        r += lv_snprintf(render + r, sizeof(render) - r, "%u ", (unsigned)(stats.renderUsQuarter[q] / 1000));
    }
    ILOG_INFO("family boot animation, frames per 250 ms: %s", frames);
    ILOG_INFO("family boot animation, render ms per 250 ms: %s", render);
    assignGroup();
}

#endif
