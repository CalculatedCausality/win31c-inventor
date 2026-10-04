/*
 * HOUSE.EXE - Win3.1 wireframe house editor with AutoCAD-style grips.
 *
 * The house starts stationary. Space toggles spin. While stopped, every
 * vertex shows a grip: blue = cold, magenta = hover, red = hot (selected).
 * Drag a hot grip to move the vertex in the screen plane; every edge that
 * shares the vertex stretches with it.
 *
 *   Space        spin on/off           Right-drag   orbit
 *   Tab/Shift+Tab select next/prev     Arrows       move X / Z (orbit if none)
 *   PgUp/PgDn    move Y                F9           snap to 10 units
 *   Esc          clear selection       R            reset the model
 *   Ctrl+Z/Y     undo/redo             Menus        files and preset views
 *
 * No 80287 is assumed: transforms use 10-bit fixed point and a sine table;
 * clipping interpolation uses the compiler's software floating point.
 */
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <commdlg.h>
#include "persist.h"
#include "cad_export.h"
#include "cad_drawing.h"
#include "cad_macro.h"

#define TIMER_ID   1
#define DIST       600      /* camera distance from the origin */
#define GRIP       3        /* grip half-size in pixels */
#define PICK       6        /* pick radius in pixels */
#define STATUS_H   20
#define SNAP       10
#define LIMIT      400      /* keep vertices within +/- this */
#define NONE       (-1)
#define NEAR_Z     50
#define HISTORY    32
#define ID_OPEN    101
#define ID_SAVE    102
#define ID_SAVEAS  103
#define ID_EXIT    104
#define ID_DXF     105
#define ID_OBJ     106
#define ID_SHEET   306
#define ID_UNDO    201
#define ID_REDO    202
#define ID_RESET   203
#define ID_SPIN    301
#define ID_ISO     302
#define ID_FRONT   303
#define ID_TOP     304
#define ID_SIDE    305
#define ID_SNAP    401
#define ID_MACRO   402

typedef struct { int x, y, z; } VEC;
typedef struct { BYTE a, b, pen; } EDGE;

enum { P_WALL, P_ROOF, P_DOOR, P_WINDOW, P_CHIMNEY, P_HILITE, NPENS };

/* sin(0..90 degrees) * 1024, 64 steps per quarter turn. */
static const int quarter[65] = {
       0,   25,   50,   75,  100,  125,  150,  175,  200,  224,  249,  273,
     297,  321,  345,  369,  392,  415,  438,  460,  483,  505,  526,  548,
     569,  590,  610,  630,  650,  669,  688,  706,  724,  742,  759,  775,
     792,  807,  822,  837,  851,  865,  878,  891,  903,  915,  926,  936,
     946,  955,  964,  972,  980,  987,  993,  999, 1004, 1009, 1013, 1016,
    1019, 1021, 1023, 1024, 1024
};

/* y is up, +z faces the viewer at yaw 0. */
static const VEC houseInit[] = {
    /* 0-7: walls */
    {-100, -70,  70}, { 100, -70,  70}, { 100, -70, -70}, {-100, -70, -70},
    {-100,  30,  70}, { 100,  30,  70}, { 100,  30, -70}, {-100,  30, -70},
    /* 8-9: roof ridge */
    {-100, 100,   0}, { 100, 100,   0},
    /* 10-13: door */
    { -15, -70,  70}, { -15,   0,  70}, {  15,   0,  70}, {  15, -70,  70},
    /* 14-21: front windows */
    { -75, -30,  70}, { -35, -30,  70}, { -35,  10,  70}, { -75,  10,  70},
    {  35, -30,  70}, {  75, -30,  70}, {  75,  10,  70}, {  35,  10,  70},
    /* 22-29: chimney, sitting on the back roof slope */
    {  40,  55, -45}, {  65,  55, -45}, {  65,  75, -25}, {  40,  75, -25},
    {  40, 120, -45}, {  65, 120, -45}, {  65, 120, -25}, {  40, 120, -25}
};
#define NVERT (sizeof(houseInit) / sizeof(houseInit[0]))

static const EDGE edges[] = {
    {0,1,P_WALL}, {1,2,P_WALL}, {2,3,P_WALL}, {3,0,P_WALL},
    {4,5,P_WALL}, {5,6,P_WALL}, {6,7,P_WALL}, {7,4,P_WALL},
    {0,4,P_WALL}, {1,5,P_WALL}, {2,6,P_WALL}, {3,7,P_WALL},
    {8,9,P_ROOF}, {4,8,P_ROOF}, {7,8,P_ROOF}, {5,9,P_ROOF}, {6,9,P_ROOF},
    {10,11,P_DOOR}, {11,12,P_DOOR}, {12,13,P_DOOR},
    {14,15,P_WINDOW}, {15,16,P_WINDOW}, {16,17,P_WINDOW}, {17,14,P_WINDOW},
    {18,19,P_WINDOW}, {19,20,P_WINDOW}, {20,21,P_WINDOW}, {21,18,P_WINDOW},
    {22,23,P_CHIMNEY}, {23,24,P_CHIMNEY}, {24,25,P_CHIMNEY},
    {25,22,P_CHIMNEY}, {26,27,P_CHIMNEY}, {27,28,P_CHIMNEY},
    {28,29,P_CHIMNEY}, {29,26,P_CHIMNEY}, {22,26,P_CHIMNEY},
    {23,27,P_CHIMNEY}, {24,28,P_CHIMNEY}, {25,29,P_CHIMNEY}
};
#define NEDGES (sizeof(edges) / sizeof(edges[0]))

static const COLORREF penColors[NPENS] = {
    RGB(255, 255, 255), RGB(255, 0, 0), RGB(255, 255, 0),
    RGB(0, 255, 255), RGB(192, 192, 192), RGB(0, 255, 0)
};

static VEC      verts[NVERT];
static int      sx[NVERT], sy[NVERT];   /* last projected positions */
static int      yaw = 32, pitch = 20;   /* 256 units per full turn */
static BOOL     fSpin = FALSE, fSnap = FALSE;
static BOOL     fSheet = FALSE;
static int      sel = NONE, hover = NONE;
static int      cxClient, cyClient, midX, midY;
static long     focal;
static HBITMAP  hbmBack;
static HPEN     pens[NPENS];
static HBRUSH   hbrCold, hbrHover, hbrHot, hbrStatus;

static BOOL     valid[NVERT], viewport;
static long     camX[NVERT], camY[NVERT], camZ[NVERT];
static VEC      history[HISTORY + 1][NVERT];
static DWORD    stateIds[HISTORY + 1], nextId = 1, savedId;
static int      histPos, histLast;
static char     filePath[260];
static HINSTANCE instance;
static BOOL     timerOK;

/* Exactly one gesture owns capture; its target and basis never change. */
enum { G_NONE, G_GRIP, G_ORBIT };
static int      gesture, dragTarget;
static VEC      dragStart;
static int      mx0, my0, yaw0, pitch0;
static long     dragDepth, dragFocal, dragCos, dragSin, dragCP, dragSP;
static BOOL     dragSnap;

static void Reset(void);

static void InitHistory(void)
{
    histPos = histLast = 0;
    memcpy(history[0], verts, sizeof(verts));
    stateIds[0] = nextId++;
    savedId = stateIds[0];
}

static BOOL Dirty(void)
{
    return stateIds[histPos] != savedId;
}

static void Commit(void)
{
    if (!memcmp(history[histPos], verts, sizeof(verts))) return;
    if (histPos == HISTORY) {
        memmove(history[0], history[1], HISTORY * sizeof(verts));
        memmove(stateIds, stateIds + 1, HISTORY * sizeof(DWORD));
        --histPos;
    }
    ++histPos;
    histLast = histPos;
    memcpy(history[histPos], verts, sizeof(verts));
    stateIds[histPos] = nextId++;
}

static void EndGesture(HWND hwnd, BOOL accept)
{
    int old = gesture;
    gesture = G_NONE;
    if (old == G_GRIP) {
        if (accept) Commit();
        else verts[dragTarget] = dragStart;
    } else if (old == G_ORBIT && !accept) {
        yaw = yaw0;
        pitch = pitch0;
    }
    if (old != G_NONE && GetCapture() == hwnd) ReleaseCapture();
    if (old != G_NONE) InvalidateRect(hwnd, NULL, FALSE);
}

static int ISin(int a)
{
    a &= 255;
    if (a < 64)  return quarter[a];
    if (a < 128) return quarter[128 - a];
    if (a < 192) return -quarter[a - 128];
    return -quarter[256 - a];
}

static int ICos(int a)
{
    return ISin(a + 64);
}

static void ToCamera(const VEC *v, long *cx, long *cy, long *cz)
{
    long cyw = ICos(yaw), syw = ISin(yaw);
    long cp = ICos(pitch), sp = ISin(pitch);
    long x1 = ((long)v->x * cyw + (long)v->z * syw) >> 10;
    long z1 = (-(long)v->x * syw + (long)v->z * cyw) >> 10;

    *cx = x1;
    *cy = ((long)v->y * cp - z1 * sp) >> 10;
    *cz = ((long)v->y * sp + z1 * cp) >> 10;
}

static int Clamp(long n)
{
    if (n > LIMIT)  return LIMIT;
    if (n < -LIMIT) return -LIMIT;
    return (int)n;
}

static int SnapTo(int n)
{
    if (!fSnap)
        return n;
    return (n >= 0 ? n + SNAP / 2 : n - SNAP / 2) / SNAP * SNAP;
}

static void Project(void)
{
    long cx, cy, cz, depth, x, y;
    int i;

    for (i = 0; i < (int)NVERT; i++) {
        ToCamera(&verts[i], &cx, &cy, &cz);
        camX[i] = cx; camY[i] = cy; camZ[i] = cz;
        depth = DIST - cz;
        valid[i] = FALSE;
        if (!viewport || depth < NEAR_Z) continue;
        x = midX + cx * focal / depth;
        y = midY - cy * focal / depth;
        if (x < GRIP || x >= (long)cxClient - GRIP ||
            y < GRIP || y >= (long)cyClient - STATUS_H - GRIP) continue;
        sx[i] = (int)x; sy[i] = (int)y;
        valid[i] = TRUE;
    }
}

static HPEN Pen(int n)
{
    return pens[n] ? pens[n] : GetStockObject(WHITE_PEN);
}

static HBRUSH Brush(HBRUSH b)
{
    return b ? b : GetStockObject(LTGRAY_BRUSH);
}

static int OutCode(long x, long y)
{
    return (x < 0 ? 1 : x >= cxClient ? 2 : 0) |
           (y < 0 ? 4 : y >= cyClient - STATUS_H ? 8 : 0);
}

/* Interpolation uses software floating point to avoid 32-bit products
 * overflowing at the viewport boundary. GDI receives clipped ints only. */
static long Interp(long a, long b, long n, long d)
{
    return a + (long)((double)(b - a) * (double)n / (double)d);
}

static void DrawEdge(HDC hdc, int a, int b)
{
    long ax = camX[a], ay = camY[a], ad = DIST - camZ[a];
    long bx = camX[b], by = camY[b], bd = DIST - camZ[b];
    long x, y, x0, y0, x1, y1;
    int c0, c1, c, tries;

    if (!viewport || (ad < NEAR_Z && bd < NEAR_Z)) return;
    if (ad < NEAR_Z) {
        ax = Interp(ax, bx, NEAR_Z - ad, bd - ad);
        ay = Interp(ay, by, NEAR_Z - ad, bd - ad); ad = NEAR_Z;
    } else if (bd < NEAR_Z) {
        bx = Interp(bx, ax, NEAR_Z - bd, ad - bd);
        by = Interp(by, ay, NEAR_Z - bd, ad - bd); bd = NEAR_Z;
    }
    x0 = midX + ax * focal / ad; y0 = midY - ay * focal / ad;
    x1 = midX + bx * focal / bd; y1 = midY - by * focal / bd;
    for (tries = 0; tries < 8; ++tries) {
        c0 = OutCode(x0, y0); c1 = OutCode(x1, y1);
        if (!(c0 | c1)) {
            MoveTo(hdc, (int)x0, (int)y0);
            LineTo(hdc, (int)x1, (int)y1); return;
        }
        if (c0 & c1) return;
        c = c0 ? c0 : c1;
        if (c & 12) {
            y = c & 4 ? 0 : cyClient - STATUS_H - 1;
            x = Interp(x0, x1, y - y0, y1 - y0);
        } else {
            x = c & 1 ? 0 : cxClient - 1;
            y = Interp(y0, y1, x - x0, x1 - x0);
        }
        if (c0) { x0 = x; y0 = y; }
        else { x1 = x; y1 = y; }
    }
}

static void DrawUcsIcon(HDC hdc)
{
    static const VEC axes[3] = { {24, 0, 0}, {0, 24, 0}, {0, 0, 24} };
    static const char names[3] = { 'X', 'Y', 'Z' };
    int ox = 24, oy = cyClient - STATUS_H - 24, i, x, y;
    long cx, cy, cz;

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(192, 192, 192));
    for (i = 0; i < 3; i++) {
        ToCamera(&axes[i], &cx, &cy, &cz);
        x = ox + (int)cx;
        y = oy - (int)cy;
        SelectObject(hdc, Pen(i == 0 ? P_ROOF : i == 1 ? P_HILITE :
                                P_WINDOW));
        MoveTo(hdc, ox, oy);
        LineTo(hdc, x, y);
        TextOut(hdc, x + (x >= ox ? 2 : -8), y - 6, &names[i], 1);
    }
}

static void DrawStatus(HDC hdc)
{
    char buf[100];
    RECT rc;
    int n;

    /* wsprintf takes far pointers for %s; small-model literals are near. */

    rc.left = 0;
    rc.top = max(0, cyClient - STATUS_H);
    rc.right = cxClient;
    rc.bottom = cyClient;
    FillRect(hdc, &rc, Brush(hbrStatus));
    SelectObject(hdc, GetStockObject(BLACK_PEN));
    MoveTo(hdc, 0, rc.top);
    LineTo(hdc, cxClient, rc.top);

    if (sel != NONE)
        n = wsprintf(buf, "Vertex %d   X: %d   Y: %d   Z: %d   %s",
                     sel, verts[sel].x, verts[sel].y, verts[sel].z,
                     (LPSTR)(fSnap ? "SNAP" : ""));
    else if (fSpin)
        n = wsprintf(buf, "Click or Space to stop and edit   %s",
                     (LPSTR)(fSnap ? "SNAP" : ""));
    else
        n = wsprintf(buf, "Pick a grip (or Tab)   F9 snap   R reset   %s",
                     (LPSTR)(fSnap ? "SNAP" : ""));

    SelectObject(hdc, GetStockObject(ANSI_VAR_FONT));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(0, 0, 0));
    TextOut(hdc, 6, rc.top + 4, buf, n);
}

typedef struct { HDC hdc; int width, height, left, top; } SHEET_CONTEXT;

static int SheetLine(void *context, long x1, long y1, long x2, long y2,
                     unsigned int flags)
{
    SHEET_CONTEXT *c = (SHEET_CONTEXT *)context;
    (void)flags;
    MoveTo(c->hdc, c->left + (int)(x1 * c->width / 600),
                   c->top + (int)(y1 * c->height / 480));
    LineTo(c->hdc, c->left + (int)(x2 * c->width / 600),
                   c->top + (int)(y2 * c->height / 480));
    return 1;
}

static int SheetText(void *context, long x, long y, const char *text,
                     unsigned int flags)
{
    SHEET_CONTEXT *c = (SHEET_CONTEXT *)context;
    if (flags & CAD_DRAWING_TITLE) {
        if (text[0] != 'O') return 1;
        text = "WIREFRAME / THIRD ANGLE / MODEL UNITS UNSPECIFIED";
    }
    TextOut(c->hdc, c->left + (int)(x * c->width / 600),
                    c->top + (int)(y * c->height / 480),
                    text, lstrlen(text));
    return 1;
}

static void DrawSheet(HDC hdc)
{
    CadDrawingPoint points[NVERT];
    CadDrawingEdge lines[NEDGES];
    CadDrawingModel model;
    CadDrawingSheet sheet;
    CadDrawingSink sink;
    SHEET_CONTEXT context;
    HFONT font, oldFont;
    RECT rc;
    int i;
    rc.left = rc.top = 0; rc.right = cxClient; rc.bottom = cyClient;
    FillRect(hdc, &rc, GetStockObject(WHITE_BRUSH));
    SelectObject(hdc, GetStockObject(BLACK_PEN));
    SelectObject(hdc, GetStockObject(ANSI_VAR_FONT));
    SetBkMode(hdc, TRANSPARENT); SetTextColor(hdc, RGB(0, 0, 0));
    for (i = 0; i < (int)NVERT; ++i) {
        points[i].x = verts[i].x; points[i].y = verts[i].y;
        points[i].z = verts[i].z;
    }
    for (i = 0; i < (int)NEDGES; ++i) {
        lines[i].a = edges[i].a; lines[i].b = edges[i].b;
    }
    model.vertices = points; model.vertex_count = NVERT;
    model.edges = lines; model.edge_count = NEDGES;
    sheet.width = 600; sheet.height = 480; sheet.margin = 16;
    sheet.gap = 32; sheet.scale.numerator = 1; sheet.scale.denominator = 1;
    context.width = (int)min((long)cxClient, (long)max(0, cyClient - STATUS_H) * 600 / 480);
    context.height = (int)((long)context.width * 480 / 600);
    context.left = (cxClient - context.width) / 2; context.top = 0;
    font = CreateFont(-9, 0, 0, 0,
        FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial");
    oldFont = font ? SelectObject(hdc, font) : NULL;
    context.hdc = hdc; sink.context = &context;
    sink.line = SheetLine; sink.text = SheetText;
    if (CadDrawingRender(&model, &sheet, &sink, NULL) != CAD_DRAWING_OK)
        TextOut(hdc, 10, 10, "Model does not fit sheet", 23);
    if (font) { if (oldFont) SelectObject(hdc, oldFont); DeleteObject(font); }
    DrawStatus(hdc);
}

static void Render(HDC hdc)
{
    RECT rc;
    HBRUSH hbr;
    int i, a, b;

    if (fSheet) { DrawSheet(hdc); return; }

    rc.left = 0;
    rc.top = 0;
    rc.right = cxClient;
    rc.bottom = cyClient;
    FillRect(hdc, &rc, GetStockObject(BLACK_BRUSH));

    Project();
    for (i = 0; i < (int)NEDGES; i++) {
        a = edges[i].a;
        b = edges[i].b;
        SelectObject(hdc, Pen((a == sel || b == sel) ? P_HILITE
                                                       : edges[i].pen));
        DrawEdge(hdc, a, b);
    }

    if (!fSpin) {
        SelectObject(hdc, GetStockObject(WHITE_PEN));
        for (i = 0; i < (int)NVERT; i++) {
            if (!valid[i]) continue;
            hbr = i == sel ? hbrHot : i == hover ? hbrHover : hbrCold;
            SelectObject(hdc, Brush(hbr));
            Rectangle(hdc, sx[i] - GRIP, sy[i] - GRIP,
                      sx[i] + GRIP + 1, sy[i] + GRIP + 1);
        }
    }

    if (viewport) DrawUcsIcon(hdc);
    DrawStatus(hdc);
}

static int HitTest(int x, int y)
{
    int i, best = NONE;
    long d, dx, dy, bestD = PICK + 1;

    Project();
    if (!viewport || x < 0 || x >= cxClient ||
        y < 0 || y >= cyClient - STATUS_H) return NONE;
    for (i = 0; i < (int)NVERT; i++) {
        if (!valid[i]) continue;
        dx = labs((long)sx[i] - x); dy = labs((long)sy[i] - y);
        d = max(dx, dy);
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

static void StopSpin(void)
{
    fSpin = FALSE;
    Project();
}

static void Nudge(int dx, int dy, int dz)
{
    int step = fSnap ? SNAP : 1;
    VEC *v = &verts[sel];

    v->x = SnapTo(Clamp((long)v->x + dx * step));
    v->y = SnapTo(Clamp((long)v->y + dy * step));
    v->z = SnapTo(Clamp((long)v->z + dz * step));
}

static void Reset(void)
{
    int i;

    for (i = 0; i < (int)NVERT; i++)
        verts[i] = houseInit[i];
    sel = hover = NONE;
}

static BOOL OnKey(HWND hwnd, WPARAM key)
{
    BOOL shift = GetKeyState(VK_SHIFT) < 0;

    if (GetKeyState(VK_CONTROL) < 0 && (key == 'Z' || key == 'Y')) {
        SendMessage(hwnd, WM_COMMAND, key == 'Z' ? ID_UNDO : ID_REDO, 0L);
        return TRUE;
    }
    if (fSheet && key != VK_ESCAPE) return FALSE;
    if (fSheet) fSheet = FALSE;
    EndGesture(hwnd, FALSE);
    switch (key) {
    case VK_SPACE:
        if (timerOK) fSpin = !fSpin;
        if (fSpin)
            sel = hover = NONE;
        break;
    case VK_TAB:
        StopSpin();
        if (sel == NONE)
            sel = shift ? NVERT - 1 : 0;
        else
            sel = (sel + (shift ? NVERT - 1 : 1)) % NVERT;
        break;
    case VK_ESCAPE:
        sel = NONE;
        break;
    case VK_F9:
        fSnap = !fSnap;
        break;
    case 'R':
        SendMessage(hwnd, WM_COMMAND, ID_RESET, 0L);
        break;
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
        if (sel == NONE) {
            if (key == VK_LEFT)  yaw -= 4;
            if (key == VK_RIGHT) yaw += 4;
            if (key == VK_UP)    pitch = min(pitch + 4, 56);
            if (key == VK_DOWN)  pitch = max(pitch - 4, -56);
        } else {
            Nudge(key == VK_LEFT ? -1 : key == VK_RIGHT ? 1 : 0, 0,
                  key == VK_UP ? -1 : key == VK_DOWN ? 1 : 0);
        }
        break;
    case VK_PRIOR:
    case VK_NEXT:
        if (sel == NONE)
            return FALSE;
        Nudge(0, key == VK_PRIOR ? 1 : -1, 0);
        break;
    default:
        return FALSE;
    }
    yaw &= 255;
    Commit();
    InvalidateRect(hwnd, NULL, FALSE);
    return TRUE;
}

static void OnMouseMove(HWND hwnd, int x, int y)
{
    long dx, dy, wx, wy, wz;
    VEC v;
    int h;

    if (gesture != G_NONE && GetCapture() != hwnd) {
        EndGesture(hwnd, FALSE); return;
    }
    if (gesture == G_GRIP) {
        dx = ((long)x - mx0) * dragDepth / dragFocal;
        dy = -((long)y - my0) * dragDepth / dragFocal;
        /* Bound before fixed-point products, including out-of-client input. */
        dx = max(-16384L, min(16384L, dx));
        dy = max(-16384L, min(16384L, dy));
        wy = dy * dragCP / 1024;
        wz = -dy * dragSP / 1024;
        wx = (dx * dragCos - wz * dragSin) / 1024;
        wz = (dx * dragSin + wz * dragCos) / 1024;
        v.x = Clamp((long)dragStart.x + wx);
        v.y = Clamp((long)dragStart.y + wy);
        v.z = Clamp((long)dragStart.z + wz);
        if (dragSnap && (x != mx0 || y != my0)) {
            v.x = (v.x >= 0 ? v.x + 5 : v.x - 5) / 10 * 10;
            v.y = (v.y >= 0 ? v.y + 5 : v.y - 5) / 10 * 10;
            v.z = (v.z >= 0 ? v.z + 5 : v.z - 5) / 10 * 10;
        }
        if (x == mx0 && y == my0) v = dragStart;
        verts[dragTarget] = v;
        InvalidateRect(hwnd, NULL, FALSE);
    } else if (gesture == G_ORBIT) {
        yaw = (int)((yaw0 + ((long)x - mx0) / 2) & 255);
        pitch = (int)max(-64L, min(64L, pitch0 + ((long)y - my0) / 2));
        InvalidateRect(hwnd, NULL, FALSE);
    } else if (!fSpin) {
        h = HitTest(x, y);
        if (h != hover) {
            hover = h;
            InvalidateRect(hwnd, NULL, FALSE);
        }
    }
}

static BOOL ChoosePath(HWND hwnd, BOOL save, char *path)
{
    OPENFILENAME ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.hInstance = instance;
    ofn.lpstrFilter = "House CAD (*.H31)\0*.H31\0All files (*.*)\0*.*\0\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = path;
    ofn.nMaxFile = 260;
    ofn.lpstrDefExt = "H31";
    ofn.Flags = OFN_HIDEREADONLY | OFN_PATHMUSTEXIST |
                (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (save) return GetSaveFileName(&ofn);
    return GetOpenFileName(&ofn);
}

static BOOL SaveFile(HWND hwnd, BOOL saveAs)
{
    char path[260];
    int xyz[NVERT * 3], i;
    lstrcpy(path, filePath[0] ? filePath : "C:\\HOUSE.H31");
    if ((saveAs || !filePath[0]) && !ChoosePath(hwnd, TRUE, path)) return FALSE;
    for (i = 0; i < (int)NVERT; ++i) {
        xyz[i * 3] = verts[i].x;
        xyz[i * 3 + 1] = verts[i].y;
        xyz[i * 3 + 2] = verts[i].z;
    }
    if (!CadWrite(path, xyz, (WORD)(NVERT * 3))) {
        MessageBox(hwnd, "Could not save the house.", "House", MB_OK | MB_ICONEXCLAMATION);
        return FALSE;
    }
    lstrcpy(filePath, path);
    savedId = stateIds[histPos];
    return TRUE;
}

static BOOL MayDiscard(HWND hwnd)
{
    int answer;
    if (!Dirty()) return TRUE;
    answer = MessageBox(hwnd, "Save changes?\nYes: Save   No: Discard   Cancel: Keep editing",
                        "House", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (answer == IDCANCEL) return FALSE;
    if (answer == IDYES) return SaveFile(hwnd, FALSE);
    return answer == IDNO;
}

static void ExportFile(HWND hwnd, int format)
{
    CadExportVertex points[NVERT];
    CadExportEdge lines[NEDGES];
    OPENFILENAME ofn;
    char path[128];
    int i, result;
    memset(&ofn, 0, sizeof(ofn));
    lstrcpy(path, format == CAD_EXPORT_DXF ? "C:\\HOUSE.DXF" : "C:\\HOUSE.OBJ");
    ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = hwnd;
    ofn.hInstance = instance; ofn.lpstrFile = path; ofn.nMaxFile = sizeof(path);
    ofn.lpstrFilter = format == CAD_EXPORT_DXF ?
        "DXF wireframe (*.DXF)\0*.DXF\0\0" : "OBJ wireframe (*.OBJ)\0*.OBJ\0\0";
    ofn.lpstrDefExt = format == CAD_EXPORT_DXF ? "DXF" : "OBJ";
    ofn.Flags = OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
    if (!GetSaveFileName(&ofn)) return;
    for (i = 0; i < (int)NVERT; ++i) {
        points[i].x = verts[i].x; points[i].y = verts[i].y; points[i].z = verts[i].z;
    }
    for (i = 0; i < (int)NEDGES; ++i) {
        lines[i].a = edges[i].a; lines[i].b = edges[i].b; lines[i].style = edges[i].pen;
    }
    result = CadExportWrite(path, format, points, NVERT, lines, NEDGES, NULL);
    MessageBox(hwnd, result == CAD_EXPORT_OK ? "Wireframe exported in unspecified model units." :
        "Export failed. Use a new DOS 8.3 filename; existing files are not overwritten.",
        "Wireframe export", MB_OK | (result == CAD_EXPORT_OK ? MB_ICONINFORMATION : MB_ICONEXCLAMATION));
}

static void RunMacro(HWND hwnd)
{
    static CadMacroProgram program;
    CadMacroError error;
    CadMacroStatus result;
    OPENFILENAME ofn;
    FILE *input;
    char path[128], message[100];
    int xyz[NVERT * 3], i, closed;
    memset(&ofn, 0, sizeof(ofn));
    lstrcpy(path, "A:\\RAISE.CMD");
    ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = hwnd;
    ofn.hInstance = instance; ofn.lpstrFile = path; ofn.nMaxFile = sizeof(path);
    ofn.lpstrFilter = "CAD macros (*.CMD)\0*.CMD\0All files\0*.*\0\0";
    ofn.Flags = OFN_HIDEREADONLY | OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    if (!GetOpenFileName(&ofn)) return;
    input = fopen(path, "rb");
    if (!input) {
        MessageBox(hwnd, "Could not open macro file.", "Macro", MB_OK | MB_ICONEXCLAMATION);
        return;
    }
    result = CadMacroRead(input, &program, &error);
    closed = fclose(input);
    if (closed != 0 && result == CAD_MACRO_OK) result = CAD_MACRO_IO;
    for (i = 0; i < (int)NVERT; ++i) {
        xyz[i * 3] = verts[i].x; xyz[i * 3 + 1] = verts[i].y;
        xyz[i * 3 + 2] = verts[i].z;
    }
    if (result == CAD_MACRO_OK)
        result = CadMacroExecute(&program, xyz, NVERT * 3, NULL, NULL, &error);
    if (result != CAD_MACRO_OK) {
        wsprintf(message, "Macro failed: status %d, line %lu. Model unchanged.",
                 (int)result, error.line);
        MessageBox(hwnd, message, "Macro", MB_OK | MB_ICONEXCLAMATION);
        return;
    }
    for (i = 0; i < (int)NVERT; ++i) {
        verts[i].x = xyz[i * 3]; verts[i].y = xyz[i * 3 + 1];
        verts[i].z = xyz[i * 3 + 2];
    }
    sel = hover = NONE; fSheet = FALSE; StopSpin(); Commit();
}

static void LoadHouse(HWND hwnd)
{
    char path[260];
    int xyz[NVERT * 3], i;
    path[0] = '\0';
    if (!ChoosePath(hwnd, FALSE, path)) return;
    if (!MayDiscard(hwnd)) return;
    if (!CadRead(path, xyz, (WORD)(NVERT * 3))) goto bad;
    for (i = 0; i < (int)(NVERT * 3); ++i)
        if (xyz[i] < -LIMIT || xyz[i] > LIMIT) goto bad;
    for (i = 0; i < (int)NVERT; ++i) {
        verts[i].x = xyz[i * 3];
        verts[i].y = xyz[i * 3 + 1];
        verts[i].z = xyz[i * 3 + 2];
    }
    lstrcpy(filePath, path);
    sel = hover = NONE;
    StopSpin();
    InitHistory();
    return;
bad:
    MessageBox(hwnd, "Could not open the house.", "House", MB_OK | MB_ICONEXCLAMATION);
}

static void UpdateUI(HWND hwnd)
{
    HMENU menu = GetMenu(hwnd);
    char title[300];
    wsprintf(title, "House - %s%s", (LPSTR)(filePath[0] ? filePath : "Untitled"),
             (LPSTR)(Dirty() ? " *" : ""));
    SetWindowText(hwnd, title);
    EnableMenuItem(menu, ID_UNDO, MF_BYCOMMAND | (histPos ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, ID_REDO, MF_BYCOMMAND | (histPos < histLast ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, ID_SPIN, MF_BYCOMMAND | (timerOK ? MF_ENABLED : MF_GRAYED));
    CheckMenuItem(menu, ID_SPIN, MF_BYCOMMAND | (fSpin ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, ID_SNAP, MF_BYCOMMAND | (fSnap ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, ID_SHEET, MF_BYCOMMAND | (fSheet ? MF_CHECKED : MF_UNCHECKED));
}

static void Command(HWND hwnd, UINT id)
{
    EndGesture(hwnd, FALSE);
    switch (id) {
    case ID_OPEN: LoadHouse(hwnd); break;
    case ID_SAVE: SaveFile(hwnd, FALSE); break;
    case ID_SAVEAS: SaveFile(hwnd, TRUE); break;
    case ID_DXF: ExportFile(hwnd, CAD_EXPORT_DXF); break;
    case ID_OBJ: ExportFile(hwnd, CAD_EXPORT_OBJ); break;
    case ID_MACRO: RunMacro(hwnd); break;
    case ID_EXIT: SendMessage(hwnd, WM_CLOSE, 0, 0L); return;
    case ID_UNDO:
    case ID_REDO:
        if (id == ID_UNDO && histPos) --histPos;
        if (id == ID_REDO && histPos < histLast) ++histPos;
        memcpy(verts, history[histPos], sizeof(verts));
        sel = hover = NONE; StopSpin(); break;
    case ID_RESET:
        if (MayDiscard(hwnd)) { Reset(); StopSpin(); Commit(); }
        break;
    case ID_SHEET: fSheet = !fSheet; StopSpin(); sel = hover = NONE; break;
    case ID_SPIN:
        fSheet = FALSE;
        if (timerOK) fSpin = !fSpin;
        sel = hover = NONE; break;
    case ID_ISO: fSheet = FALSE; yaw = 32; pitch = 20; StopSpin(); break;
    case ID_FRONT: fSheet = FALSE; yaw = 0; pitch = 0; StopSpin(); break;
    case ID_TOP: fSheet = FALSE; yaw = 0; pitch = 64; StopSpin(); break;
    case ID_SIDE: fSheet = FALSE; yaw = 64; pitch = 0; StopSpin(); break;
    case ID_SNAP: fSnap = !fSnap; break;
    }
    UpdateUI(hwnd);
    InvalidateRect(hwnd, NULL, FALSE);
}

static HMENU BuildMenu(void)
{
    HMENU bar = CreateMenu(), pop = NULL;
    if (!bar) return NULL;
    pop = CreatePopupMenu();
    if (!pop) goto fail;
    if (!AppendMenu(pop, MF_STRING, ID_OPEN, "&Open...") ||
        !AppendMenu(pop, MF_STRING, ID_SAVE, "&Save") ||
        !AppendMenu(pop, MF_STRING, ID_SAVEAS, "Save &As...") ||
        !AppendMenu(pop, MF_STRING, ID_DXF, "Export &DXF...") ||
        !AppendMenu(pop, MF_STRING, ID_OBJ, "Export O&BJ...") ||
        !AppendMenu(pop, MF_SEPARATOR, 0, NULL) ||
        !AppendMenu(pop, MF_STRING, ID_EXIT, "E&xit") ||
        !AppendMenu(bar, MF_POPUP, (UINT)pop, "&File")) goto fail;
    pop = CreatePopupMenu();
    if (!pop) goto fail;
    if (!AppendMenu(pop, MF_STRING, ID_UNDO, "&Undo\tCtrl+Z") ||
        !AppendMenu(pop, MF_STRING, ID_REDO, "&Redo\tCtrl+Y") ||
        !AppendMenu(pop, MF_STRING, ID_RESET, "&Reset") ||
        !AppendMenu(bar, MF_POPUP, (UINT)pop, "&Edit")) goto fail;
    pop = CreatePopupMenu();
    if (!pop) goto fail;
    if (!AppendMenu(pop, MF_STRING, ID_SPIN, "&Spin\tSpace") ||
        !AppendMenu(pop, MF_STRING, ID_ISO, "&Isometric") ||
        !AppendMenu(pop, MF_STRING, ID_FRONT, "&Front") ||
        !AppendMenu(pop, MF_STRING, ID_TOP, "&Top") ||
        !AppendMenu(pop, MF_STRING, ID_SHEET, "Drawing s&heet") ||
        !AppendMenu(pop, MF_STRING, ID_SIDE, "Si&de") ||
        !AppendMenu(bar, MF_POPUP, (UINT)pop, "&View")) goto fail;
    pop = CreatePopupMenu();
    if (!pop) goto fail;
    if (!AppendMenu(pop, MF_STRING, ID_SNAP, "&Snap\tF9") ||
        !AppendMenu(pop, MF_STRING, ID_MACRO, "Run &Macro...") ||
        !AppendMenu(bar, MF_POPUP, (UINT)pop, "&Tools")) goto fail;
    return bar;
fail:
    if (pop) DestroyMenu(pop);
    DestroyMenu(bar);
    return NULL;
}

static void FreeGraphics(void)
{
    int i;
    for (i = 0; i < NPENS; ++i) {
        if (pens[i]) DeleteObject(pens[i]);
        pens[i] = NULL;
    }
    if (hbrCold) DeleteObject(hbrCold);
    if (hbrHover) DeleteObject(hbrHover);
    if (hbrHot) DeleteObject(hbrHot);
    if (hbrStatus) DeleteObject(hbrStatus);
    if (hbmBack) DeleteObject(hbmBack);
    hbrCold = hbrHover = hbrHot = hbrStatus = NULL;
    hbmBack = NULL;
}

LRESULT CALLBACK __export MainWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                      LPARAM lParam)
{
    PAINTSTRUCT ps;
    HDC hdc, hdcMem;
    HBITMAP hbmOld;
    HMENU menu;
    int x = (int)(short)LOWORD(lParam);
    int y = (int)(short)HIWORD(lParam);
    int i;

    switch (msg) {
    case WM_CREATE:
        menu = BuildMenu();
        if (!menu) return -1;
        if (!SetMenu(hwnd, menu)) { DestroyMenu(menu); return -1; }
        for (i = 0; i < NPENS; i++)
            pens[i] = CreatePen(PS_SOLID, 1, penColors[i]);
        hbrCold = CreateSolidBrush(RGB(0, 0, 255));
        hbrHover = CreateSolidBrush(RGB(255, 0, 255));
        hbrHot = CreateSolidBrush(RGB(255, 0, 0));
        hbrStatus = CreateSolidBrush(RGB(192, 192, 192));
        timerOK = SetTimer(hwnd, TIMER_ID, 55, NULL) != 0;
        UpdateUI(hwnd);
        return 0;

    case WM_SIZE:
        EndGesture(hwnd, FALSE);
        hover = NONE;
        cxClient = max(0, x);
        cyClient = max(0, y);
        viewport = cxClient > 2 * GRIP && cyClient > STATUS_H + 2 * GRIP;
        midX = cxClient / 2;
        midY = max(0, cyClient - STATUS_H) / 2;
        focal = max(1L, min(8192L, (long)min(cxClient,
                    max(0, cyClient - STATUS_H)) * 12 / 7));
        if (hbmBack)
            DeleteObject(hbmBack);
        hbmBack = NULL;
        hdc = GetDC(hwnd);
        if (hdc) {
            if (cxClient && cyClient)
                hbmBack = CreateCompatibleBitmap(hdc, cxClient, cyClient);
            ReleaseDC(hwnd, hdc);
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_TIMER:
        /* Win16 has no WM_CAPTURECHANGED. Catch silent capture theft too. */
        if (gesture != G_NONE && GetCapture() != hwnd) EndGesture(hwnd, FALSE);
        if (fSpin && !IsIconic(hwnd)) {
            yaw = (yaw + 2) & 255;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        hdc = BeginPaint(hwnd, &ps);
        UpdateUI(hwnd);
        hdcMem = hbmBack ? CreateCompatibleDC(hdc) : NULL;
        hbmOld = hdcMem ? SelectObject(hdcMem, hbmBack) : NULL;
        if (hdcMem && hbmOld) {
            Render(hdcMem);
            if (!BitBlt(hdc, 0, 0, cxClient, cyClient, hdcMem, 0, 0, SRCCOPY))
                Render(hdc);
            SelectObject(hdcMem, hbmOld);
        } else Render(hdc);
        if (hdcMem) DeleteDC(hdcMem);
        EndPaint(hwnd, &ps);
        return 0;

    case WM_KEYDOWN:
        if (OnKey(hwnd, wParam))
            return 0;
        break;

    case WM_LBUTTONDOWN:
        if (fSheet) return 0;
        if (gesture != G_NONE && GetCapture() != hwnd) EndGesture(hwnd, FALSE);
        if (gesture != G_NONE) return 0;
        if (fSpin)
            StopSpin();
        sel = HitTest(x, y);
        if (sel != NONE) {
            dragTarget = sel;
            dragStart = verts[sel];
            dragDepth = DIST - camZ[sel];
            dragFocal = focal;
            dragCos = ICos(yaw); dragSin = ISin(yaw);
            dragCP = ICos(pitch); dragSP = ISin(pitch);
            dragSnap = fSnap;
            mx0 = x;
            my0 = y;
            gesture = G_GRIP;
            SetCapture(hwnd);
            if (GetCapture() != hwnd) EndGesture(hwnd, FALSE);
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_RBUTTONDOWN:
        if (fSheet) return 0;
        if (gesture != G_NONE && GetCapture() != hwnd) EndGesture(hwnd, FALSE);
        if (gesture != G_NONE || !viewport) return 0;
        StopSpin();
        mx0 = x;
        my0 = y;
        yaw0 = yaw;
        pitch0 = pitch;
        gesture = G_ORBIT;
        SetCapture(hwnd);
        if (GetCapture() != hwnd) EndGesture(hwnd, FALSE);
        return 0;

    case WM_MOUSEMOVE:
        OnMouseMove(hwnd, x, y);
        return 0;

    case WM_LBUTTONUP:
        if (gesture == G_GRIP) {
            OnMouseMove(hwnd, x, y);
            EndGesture(hwnd, TRUE);
        }
        return 0;
    case WM_RBUTTONUP:
        if (gesture == G_ORBIT) {
            OnMouseMove(hwnd, x, y);
            EndGesture(hwnd, TRUE);
        }
        return 0;

    case WM_CANCELMODE:
    case WM_KILLFOCUS:
        EndGesture(hwnd, FALSE);
        return 0;
    case WM_ACTIVATE:
        if (wParam == WA_INACTIVE) EndGesture(hwnd, FALSE);
        break;
    case WM_ACTIVATEAPP:
        if (!wParam) EndGesture(hwnd, FALSE);
        break;
    case WM_INITMENU:
        EndGesture(hwnd, FALSE);
        UpdateUI(hwnd);
        return 0;
    case WM_COMMAND:
        Command(hwnd, wParam);
        return 0;
    case WM_CLOSE:
        EndGesture(hwnd, FALSE);
        if (MayDiscard(hwnd)) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        EndGesture(hwnd, FALSE);
        KillTimer(hwnd, TIMER_ID);
        FreeGraphics();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int PASCAL WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine,
                   int nCmdShow)
{
    WNDCLASS wc;
    HWND hwnd;
    MSG msg;
    int result;

    (void)lpCmdLine;
    instance = hInst;
    Reset();
    InitHistory();

    if (!hPrev) {
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = MainWndProc;
        wc.cbClsExtra = 0;
        wc.cbWndExtra = 0;
        wc.hInstance = hInst;
        wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
        wc.hCursor = LoadCursor(NULL, IDC_CROSS);
        wc.hbrBackground = NULL;
        wc.lpszMenuName = NULL;
        wc.lpszClassName = "ClaudeHouse";
        if (!RegisterClass(&wc))
            return 0;
    }

    hwnd = CreateWindow("ClaudeHouse", "Wireframe House",
                        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                        500, 420, NULL, NULL, hInst, NULL);
    if (!hwnd) {
        FreeGraphics();
        return 0;
    }
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    while ((result = GetMessage(&msg, NULL, 0, 0)) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return result < 0 ? 1 : msg.wParam;
}
