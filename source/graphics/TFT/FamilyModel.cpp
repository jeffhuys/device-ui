#if defined(FAMILY_UI) && defined(VIEW_320x240)

// A slowly turning wireframe of the T-Deck, for the first page of the welcome (FamilyScreen.cpp).
//
// The model is the booklet's device drawing (booklet.html, "02 Het apparaat") in its own units, about
// a millimetre each: the body as an outline with cut corners (the house style's 45 degrees) extruded
// to a slab, the screen with the family home screen's three rows on it, the trackball between its
// chevrons, the keyboard's keys, the switch, reset button and USB-C port on the edges.
//
// Every frame rotates the edges (a turn about the vertical axis, a fixed tilt so the top edge shows),
// projects them with a little perspective and draws them as 1-pixel lines, white on black, straight
// into an RGB565 buffer that LVGL shows as a canvas. Our own lines and not LVGL's: LVGL anti-aliases,
// and the black and white panel filter would turn the grey edge pixels into a ragged line.
//
// Hidden lines: the body is convex, so a face is visible when it faces the camera. A body edge is
// solid when one of its two faces is visible and dotted when neither is; the screen, keys and the
// other details are drawn only while their face is visible.

#include "graphics/view/TFT/FamilyScreen.h"
#include "lvgl.h"
#include <cmath>
#include <cstring>

namespace {

struct V3 {
    float x, y, z;
};

// faces: front, back, and the eight sides of the outline (side i runs from outline point i to i + 1)
constexpr int c_front = 0, c_back = 1, c_side0 = 2, c_faces = 10;
constexpr uint16_t bit(int face)
{
    return (uint16_t)(1u << face);
}

enum Kind : uint8_t { eBody, eDetail };

struct Edge {
    V3 a, b;
    uint16_t faces; // the faces this edge lies on
    Kind kind;
};

constexpr int c_maxEdges = 320;
Edge edges[c_maxEdges];
int edgeCount = 0;
V3 faceNormal[c_faces], faceCentre[c_faces];

// model units: booklet drawing x 1..79, y 1..117 -> X -39..39, Y 58..-58 (up), Z -9..9 (front at +9)
constexpr float c_halfW = 39, c_halfH = 58, c_halfD = 9, c_cut = 5;

float X(float sx)
{
    return sx - 40;
}
float Y(float sy)
{
    return 59 - sy;
}

void add(V3 a, V3 b, uint16_t faces, Kind kind)
{
    if (edgeCount < c_maxEdges)
        edges[edgeCount++] = {a, b, faces, kind};
}

// a rectangle on the front face, from booklet coordinates; inset shrinks it on every side
void frontRect(float sx, float sy, float w, float h, float inset = 0, float z = c_halfD)
{
    float x0 = X(sx + inset), x1 = X(sx + w - inset), y0 = Y(sy + inset), y1 = Y(sy + h - inset);
    add({x0, y0, z}, {x1, y0, z}, bit(c_front), eDetail);
    add({x1, y0, z}, {x1, y1, z}, bit(c_front), eDetail);
    add({x1, y1, z}, {x0, y1, z}, bit(c_front), eDetail);
    add({x0, y1, z}, {x0, y0, z}, bit(c_front), eDetail);
}

void octagon(float cx, float cy, float r, float z)
{
    for (int i = 0; i < 8; i++) {
        float a0 = (i + 0.5f) * (float)M_PI / 4, a1 = (i + 1.5f) * (float)M_PI / 4;
        add({cx + r * cosf(a0), cy + r * sinf(a0), z}, {cx + r * cosf(a1), cy + r * sinf(a1), z}, bit(c_front), eDetail);
    }
}

void build(void)
{
    if (edgeCount)
        return;
    // the outline, clockwise from the top left, with 45 degree cuts at the corners
    const float w = c_halfW, h = c_halfH, c = c_cut, d = c_halfD;
    const V3 outline[8] = {{-w + c, h, 0}, {w - c, h, 0}, {w, h - c, 0}, {w, -h + c, 0},
                           {w - c, -h, 0}, {-w + c, -h, 0}, {-w, -h + c, 0}, {-w, h - c, 0}};
    faceNormal[c_front] = {0, 0, 1};
    faceCentre[c_front] = {0, 0, d};
    faceNormal[c_back] = {0, 0, -1};
    faceCentre[c_back] = {0, 0, -d};
    for (int i = 0; i < 8; i++) {
        const V3 &p = outline[i], &q = outline[(i + 1) % 8];
        float dx = q.x - p.x, dy = q.y - p.y, len = sqrtf(dx * dx + dy * dy);
        faceNormal[c_side0 + i] = {-dy / len, dx / len, 0}; // outward for a clockwise outline
        faceCentre[c_side0 + i] = {(p.x + q.x) / 2, (p.y + q.y) / 2, 0};
        int prev = c_side0 + (i + 7) % 8, side = c_side0 + i;
        add({p.x, p.y, d}, {q.x, q.y, d}, bit(c_front) | bit(side), eBody);
        add({p.x, p.y, -d}, {q.x, q.y, -d}, bit(c_back) | bit(side), eBody);
        add({p.x, p.y, d}, {p.x, p.y, -d}, bit(prev) | bit(side), eBody);
    }

    // screen: the bezel, the display, and on it the family home screen's three rows (the first with its badge)
    frontRect(4, 4, 72, 56);
    frontRect(6, 6.5f, 68, 51);
    const float rowTop[3] = {7.2f, 21.7f, 36.1f};
    for (float top : rowTop)
        frontRect(7.3f, 6.5f + top, 65.4f, 13.2f);
    frontRect(62, 6.5f + 9.4f, 8, 8.8f);

    // the band with the trackball: chevrons pointing in, the ball standing a little proud
    const float chevron[3] = {10, 15, 20};
    for (float cx : chevron) {
        add({X(cx), Y(64), d}, {X(cx + 3), Y(67), d}, bit(c_front), eDetail);
        add({X(cx + 3), Y(67), d}, {X(cx), Y(70), d}, bit(c_front), eDetail);
        add({X(80 - cx), Y(64), d}, {X(77 - cx), Y(67), d}, bit(c_front), eDetail);
        add({X(77 - cx), Y(67), d}, {X(80 - cx), Y(70), d}, bit(c_front), eDetail);
    }
    octagon(X(40), Y(67), 5, d);
    octagon(X(40), Y(67), 3, d + 2);

    // keyboard: the well, three rows of ten keys, the bottom row around the space bar
    frontRect(4, 76, 72, 37);
    for (int row = 0; row < 3; row++)
        for (int k = 0; k < 10; k++)
            frontRect(5.6f + 6.9f * k, 78 + 8.4f * row, 6.2f, 7, 0.6f);
    const float bottomX[5] = {12, 20.2f, 27.2f, 51.6f, 59.8f}, bottomW[5] = {7.4f, 6.2f, 23.6f, 7.4f, 7.4f};
    for (int k = 0; k < 5; k++)
        frontRect(bottomX[k], 103.2f, bottomW[k], 7, 0.6f);

    // the edges: switch on the right side (side 2), reset on the left (side 6), USB-C on the bottom (side 4)
    const int right = c_side0 + 2, left = c_side0 + 6, bottom = c_side0 + 4;
    auto sideRect = [](V3 a, V3 b, V3 cc, V3 dd, int face) {
        add(a, b, bit(face), eDetail);
        add(b, cc, bit(face), eDetail);
        add(cc, dd, bit(face), eDetail);
        add(dd, a, bit(face), eDetail);
    };
    sideRect({w, Y(54), 2}, {w, Y(54), -2}, {w, Y(62), -2}, {w, Y(62), 2}, right);
    sideRect({-w, Y(44), 1.5f}, {-w, Y(44), -1.5f}, {-w, Y(49), -1.5f}, {-w, Y(49), 1.5f}, left);
    sideRect({X(34.5f), -h, 1.6f}, {X(45.5f), -h, 1.6f}, {X(45.5f), -h, -1.6f}, {X(34.5f), -h, -1.6f}, bottom);
}

struct View {
    float cy, sy, cp, sp; // cos and sin of the turn (yaw) and the tilt (pitch)
};

V3 rotate(const View &v, const V3 &p)
{
    float x1 = p.x * v.cy + p.z * v.sy;
    float z1 = -p.x * v.sy + p.z * v.cy;
    float y2 = p.y * v.cp - z1 * v.sp;
    float z2 = p.y * v.sp + z1 * v.cp;
    return {x1, y2, z2};
}

// the pixel buffer and the box of what this frame drew
struct Target {
    uint16_t *buf;
    int w, h;
    int x0, y0, x1, y1;
};

void plot(Target &t, int x, int y)
{
    if ((unsigned)x >= (unsigned)t.w || (unsigned)y >= (unsigned)t.h)
        return;
    t.buf[y * t.w + x] = 0xffff;
    if (x < t.x0)
        t.x0 = x;
    if (x > t.x1)
        t.x1 = x;
    if (y < t.y0)
        t.y0 = y;
    if (y > t.y1)
        t.y1 = y;
}

// Bresenham; dotted lines draw one pixel in three
void line(Target &t, int x0, int y0, int x1, int y1, bool dotted)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int step = 0;; step++) {
        if (!dotted || step % 3 == 0)
            plot(t, x0, y0);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

} // namespace

/**
 * Draw the model at `ms` into a w x h RGB565 buffer (cleared here). Returns the box of the pixels it
 * drew, in buffer coordinates, through x0..y1 (x1 < x0 if nothing).
 */
void familyModelDraw(uint16_t *buf, int w, int h, uint32_t ms, int *x0, int *y0, int *x1, int *y1)
{
    build();
    memset(buf, 0, (size_t)w * h * sizeof(uint16_t));
    Target t{buf, w, h, w, h, -1, -1};

    // one turn in 16 s, right to left as seen from the front (the user's choice; the first version turned the other way)
    const float turn = -(float)(ms % 16000) / 16000.0f * 2 * (float)M_PI;
    const float tilt = 0.32f + 0.05f * sinf(2 * turn);                   // about 18 degrees, two breaths a turn
    View v{cosf(turn), sinf(turn), cosf(tilt), sinf(tilt)};

    // a camera 300 units in front, the model scaled to fill the height
    const float camera = 300, scale = (h - 6) / (2 * c_halfH + 16), cx = w / 2.0f, cy = h / 2.0f;
    bool visible[c_faces];
    for (int f = 0; f < c_faces; f++) {
        V3 n = rotate(v, faceNormal[f]), c = rotate(v, faceCentre[f]);
        visible[f] = n.x * -c.x + n.y * -c.y + n.z * (camera - c.z) > 0;
    }
    for (int i = 0; i < edgeCount; i++) {
        const Edge &e = edges[i];
        bool seen = false;
        for (int f = 0; f < c_faces; f++)
            if ((e.faces & bit(f)) && visible[f])
                seen = true;
        if (!seen && e.kind == eDetail)
            continue;
        V3 a = rotate(v, e.a), b = rotate(v, e.b);
        float sa = scale * camera / (camera - a.z), sb = scale * camera / (camera - b.z);
        line(t, (int)lroundf(cx + a.x * sa), (int)lroundf(cy - a.y * sa), (int)lroundf(cx + b.x * sb),
             (int)lroundf(cy - b.y * sb), !seen);
    }
    *x0 = t.x0;
    *y0 = t.y0;
    *x1 = t.x1;
    *y1 = t.y1;
}

#endif
