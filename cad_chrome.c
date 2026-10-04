/* CAD.EXE window chrome: see cad_chrome.h. Win16 GDI only; all tables and
 * GDI objects are owned here (cad.c's far data segments are nearly full).
 * Colours are chosen for the 16-colour VGA palette: solid fills use exact
 * palette colours; light tints are 8x8 ordered-dither pattern brushes of two
 * palette colours (no per-pixel work at paint time). */
#include <windows.h>
#include <string.h>
#include "cad_ui_ids.h"
#include "cad_chrome.h"

#define ICONS (IDB_ICON_LAST - IDB_ICON_FIRST + 1)
#define I(name) (IDB_##name - IDB_ICON_FIRST)
#define NO CHROME_NO_ICON

#define C_BLACK RGB(0,0,0)
#define C_WHITE RGB(255,255,255)
#define C_SILVER RGB(192,192,192)
#define C_GREY RGB(128,128,128)
#define C_NAVY RGB(0,0,128)
#define C_BLUE RGB(0,0,255)
#define C_CYAN RGB(0,255,255)
#define C_RED RGB(255,0,0)

/* Ribbon geometry (client rows). */
#define BODY_TOP (CHROME_TAB_HEIGHT + 1)
#define CAPTION_TOP 77
#define CAPTION_BOTTOM 90
#define LARGE_TOP 24
#define SMALL_TOP 25
#define SMALL_ROW 17
#define FILE_RIGHT 44
#define TAB_FIRST 50
#define QAT_WIDTH 22

/* ROP3 codes not named by windows.h. */
#define ROP_DSNA 0x00220326L    /* D & ~S */
#define ROP_DPO 0x00FA0089L     /* D | P */
#define ROP_PSDPXAX 0x00B8074AL /* S ? D : P */

static HINSTANCE chromeInstance;
static HBITMAP iconColor[ICONS], iconMask[ICONS];
static unsigned char iconState[ICONS]; /* 0 not loaded, 1 ready, 2 missing */
static unsigned char iconWidth[ICONS], iconHeight[ICONS];
static HDC memColor, memMask;
static HBITMAP fadeMask;
/* Dither level L: L/16 of the pixels in the "zero" colour (see PatternFill). */
#define LEVELS 9
static HBITMAP levelBitmap[LEVELS];
static HBRUSH levelBrush[LEVELS];
static HBRUSH checkerBrush;
static HBITMAP checkerBitmap;
/* MS Sans Serif 8 bold (browser root, File button); falls back to stock. */
static HFONT boldFont;

/* ---- tree rows ---- */
static unsigned char treeLevel[CHROME_TREE_ROWS], treeIcon[CHROME_TREE_ROWS];
static unsigned char treeFlags[CHROME_TREE_ROWS];
static signed char treeFolder[CHROME_TREE_ROWS];
static unsigned char folderOpen[CHROME_FOLDERS] = {1, 0, 1, 1, 0};

/* ---- ribbon tables ---- */
#define K_END 0
#define K_PANEL 'P'
#define K_LARGE 'L'
#define K_SMALL 'S'
typedef struct RibbonItem {
    char kind;
    unsigned char icon;
    int command;
    const char *label;
    const char *help;
} RibbonItem;

static const RibbonItem modelItems[] = {
    {K_PANEL, NO, 0, "Sketch", NULL},
    {K_LARGE, I(L_SKETCH), ID_FEATURE_SKETCH, "Start 2D Sketch", "Start 2D Sketch: pick the XY, XZ or YZ plane, then add profiles"},
    {K_PANEL, NO, 0, "Create", NULL},
    {K_LARGE, I(L_EXTRUDE), ID_FEATURE_EXTRUDE, "Extrude", "Extrude: give a closed sketch profile depth"},
    {K_LARGE, I(L_REVOLVE), ID_FEATURE_REVOLVE, "Revolve", "Revolve: sweep a sketch profile around an axis"},
    {K_LARGE, I(L_BOX), ID_CREATE_BOX, "Box", "Box: wireframe box from a corner and a size"},
    {K_PANEL, NO, 0, "Modify", NULL},
    {K_SMALL, I(S_PROPS), ID_EDIT_PROPERTIES, "Properties", "Properties: edit the selected vertex or feature (Enter)"},
    {K_SMALL, I(S_DELETE), ID_EDIT_DELETE, "Delete", "Delete the selection (Del)"},
    {K_SMALL, I(S_UNDO), ID_FEATURE_UNDO, "Undo Feature", "Undo the last feature edit (up to 4 steps)"},
    {K_PANEL, NO, 0, "Wireframe", NULL},
    {K_SMALL, I(S_LINE), ID_CREATE_LINE, "Line", "Line: wireframe line between two points"},
    {K_SMALL, I(S_RECT), ID_CREATE_RECTANGLE, "Rectangle", "Rectangle: wireframe rectangle in the XY plane"},
    {K_SMALL, I(S_POLY), ID_CREATE_EXTRUDE, "Prism", "Prism: extrude a polygon as wireframe"},
    {K_END, NO, 0, NULL, NULL}
};
static const RibbonItem sketchItems[] = {
    {K_PANEL, NO, 0, "Sketch", NULL},
    {K_LARGE, I(L_SKETCH), ID_FEATURE_SKETCH, "Start 2D Sketch", "Start 2D Sketch: pick the XY, XZ or YZ plane, then add profiles"},
    {K_PANEL, NO, 0, "Create", NULL},
    {K_SMALL, I(S_LINE), ID_CREATE_LINE, "Line", "Line: wireframe line between two points"},
    {K_SMALL, I(S_RECT), ID_FEATURE_RECT, "Rectangle", "Rectangle profile: center, width, height (sketch mode)"},
    {K_SMALL, I(S_CIRCLE), ID_FEATURE_CIRCLE, "Circle", "Circle profile: center and radius (sketch mode)"},
    {K_SMALL, I(S_POLY), ID_FEATURE_POLY, "Polygon", "Polygon profile: 3..16 points (sketch mode)"},
    {K_PANEL, NO, 0, "Format", NULL},
    {K_SMALL, I(S_UNITS), ID_TOOLS_SNAP, "Snap 10", "Snap: round dragged coordinates to 10 (F9)"},
    {K_SMALL, I(S_GRID), ID_VIEW_GRID, "Grid", "Grid: show the XZ ground grid"},
    {K_PANEL, NO, 0, "Exit", NULL},
    {K_LARGE, I(L_FINISH), ID_FEATURE_FINISH, "Finish Sketch", "Finish Sketch: leave sketch mode and return to 3D Model"},
    {K_END, NO, 0, NULL, NULL}
};
static const RibbonItem assembleItems[] = {
    {K_PANEL, NO, 0, "Component", NULL},
    {K_LARGE, I(L_PLACE), ID_INSERT_PLACE, "Place", "Place: reference a saved .CAD part as a component"},
    {K_SMALL, I(S_MOVE), ID_INSERT_MOVE, "Move", "Move: offset and rotate the selected component"},
    {K_SMALL, I(S_GROUND), ID_INSERT_GROUND, "Ground", "Ground / unground the selected component"},
    {K_SMALL, I(S_DELETE), ID_INSERT_REMOVE, "Remove", "Remove the selected component"},
    {K_SMALL, I(S_RELOAD), ID_INSERT_RELOAD, "Reload", "Reload all component part files"},
    {K_END, NO, 0, NULL, NULL}
};
static const RibbonItem constrainItems[] = {
    {K_PANEL, NO, 0, "Constrain", NULL},
    {K_SMALL, I(S_COINC), ID_CONSTRAIN_COINCIDENT, "Coincident", "Coincident: make two vertices meet"},
    {K_SMALL, I(S_ALIGN), ID_CONSTRAIN_ALIGN_X, "Align X", "Align X: equal X for two vertices"},
    {K_SMALL, I(S_ALIGN), ID_CONSTRAIN_ALIGN_Y, "Align Y", "Align Y: equal Y for two vertices"},
    {K_SMALL, I(S_ALIGN), ID_CONSTRAIN_ALIGN_Z, "Align Z", "Align Z: equal Z for two vertices"},
    {K_SMALL, I(S_DIST), ID_CONSTRAIN_DISTANCE, "Distance", "Distance: fixed offset between two vertices"},
    {K_SMALL, I(S_FIX), ID_CONSTRAIN_FIX, "Fix", "Fix: lock the selected vertex"},
    {K_PANEL, NO, 0, "Manage", NULL},
    {K_SMALL, I(S_DELETE), ID_CONSTRAIN_DELETE, "Delete", "Delete the constraint selected in the browser"},
    {K_END, NO, 0, NULL, NULL}
};
static const RibbonItem manageItems[] = {
    {K_PANEL, NO, 0, "Parameters", NULL},
    {K_LARGE, I(L_PARAMS), ID_TOOLS_PARAMETERS, "Parameters", "Parameters (fx): named values usable in every field"},
    {K_PANEL, NO, 0, "Macro", NULL},
    {K_LARGE, I(L_MACRO), ID_TOOLS_RUN_MACRO, "Run Macro", "Run Macro: replay a .CMD script as one undo step"},
    {K_PANEL, NO, 0, "Units", NULL},
    {K_SMALL, I(S_UNITS), ID_TOOLS_MM, "Millimetres", "Document units: millimetres (no rescaling)"},
    {K_SMALL, I(S_UNITS), ID_TOOLS_UNSPECIFIED, "Unspecified", "Document units: unspecified (no rescaling)"},
    {K_PANEL, NO, 0, "Export", NULL},
    {K_SMALL, I(S_DRAWING), ID_FILE_DXF, "DXF", "Export the wireframe as DXF"},
    {K_SMALL, I(S_DRAWING), ID_FILE_OBJ, "OBJ", "Export the wireframe as OBJ"},
    {K_END, NO, 0, NULL, NULL}
};
static const RibbonItem viewItems[] = {
    {K_PANEL, NO, 0, "Appearance", NULL},
    {K_SMALL, I(S_WIRE), ID_VIEW_WIREFRAME, "Wireframe", "Visual style: wireframe"},
    {K_SMALL, I(S_SHADED), ID_VIEW_SHADED, "Shaded", "Visual style: shaded faces (solids and the 747 demo)"},
    {K_SMALL, I(S_GRID), ID_VIEW_GRID, "Grid", "Grid: show the XZ ground grid"},
    {K_SMALL, I(S_DRAWING), ID_VIEW_DRAWING, "Drawing", "Drawing sheet: three orthographic views"},
    {K_PANEL, NO, 0, "Navigate", NULL},
    {K_LARGE, I(L_HOME), ID_VIEW_HOME, "Home", "Home view: isometric, zoom all"},
    {K_SMALL, I(S_FRONT), ID_VIEW_FRONT, "Front", "View the front (XY plane)"},
    {K_SMALL, I(S_TOP), ID_VIEW_TOP, "Top", "View the top (XZ plane)"},
    {K_SMALL, I(S_SIDE), ID_VIEW_SIDE, "Side", "View the side (YZ plane)"},
    {K_SMALL, I(S_ISO), ID_VIEW_ISO, "Isometric", "Isometric view"},
    {K_SMALL, I(S_FIT), ID_VIEW_FIT, "Zoom All", "Zoom all: fit the whole model"},
    {K_SMALL, I(S_ORBIT), ID_VIEW_ORBIT, "Orbit", "Orbit: left-drag rotates the view (Esc ends)"},
    {K_SMALL, I(S_PAN), ID_VIEW_PAN, "Pan", "Pan: left-drag moves the view (Esc ends)"},
    {K_SMALL, I(S_ZOOM), ID_VIEW_ZOOM, "Zoom", "Zoom: left-drag up/down zooms (Esc ends)"},
    {K_END, NO, 0, NULL, NULL}
};
static const RibbonItem *const tabItems[CHROME_TABS] = {
    modelItems, sketchItems, assembleItems, constrainItems, manageItems, viewItems
};
static const char *const tabNames[CHROME_TABS] = {
    "3D Model", "Sketch", "Assemble", "Constrain", "Manage", "View"
};
static const char *const tabHelp[CHROME_TABS] = {
    "3D Model tab: sketches, features and wireframe primitives",
    "Sketch tab: profiles for the active 2D sketch",
    "Assemble tab: place and position components",
    "Constrain tab: relationships between two vertices",
    "Manage tab: parameters, macros, units and export",
    "View tab: visual style and navigation"
};
/* Navigation bar (top to bottom) and the Home button. */
static const int navCommands[4] = {ID_VIEW_PAN, ID_VIEW_ZOOM, ID_VIEW_ORBIT, ID_VIEW_FIT};
static const unsigned char navIcons[4] = {I(S_PAN), I(S_ZOOM), I(S_ORBIT), I(S_FIT)};

#define MAX_ITEMS 24
static RECT itemRect[MAX_ITEMS];
static int layoutTab = -1, layoutWidth;
static int tabLeft[CHROME_TABS], tabRight[CHROME_TABS];

/* ---- GDI helpers ---- */
static HBITMAP PatternBitmap(const unsigned char *rows)
{
    WORD bits[8];
    int i;
    for (i = 0; i < 8; ++i) bits[i] = rows[i];
    return CreateBitmap(8, 8, 1, 1, (LPSTR)bits);
}
/* Mono pattern brushes draw 0 bits in the text colour, 1 bits in the
 * background colour of the destination DC. */
static void PatternFill(HDC dc, const RECT *r, HBRUSH brush, COLORREF zero, COLORREF one)
{
    COLORREF text, back;
    if (!brush) {
        HBRUSH solid = CreateSolidBrush(one);
        FillRect(dc, r, solid ? solid : (HBRUSH)GetStockObject(WHITE_BRUSH));
        if (solid) DeleteObject(solid);
        return;
    }
    text = SetTextColor(dc, zero); back = SetBkColor(dc, one);
    FillRect(dc, r, brush);
    SetTextColor(dc, text); SetBkColor(dc, back);
}
static void Solid(HDC dc, const RECT *r, COLORREF color)
{
    PatternFill(dc, r, levelBrush[0], color, color);
}
static void Frame(HDC dc, const RECT *r, COLORREF color)
{
    RECT e;
    e = *r; e.bottom = e.top + 1; Solid(dc, &e, color);
    e = *r; e.top = e.bottom - 1; Solid(dc, &e, color);
    e = *r; e.right = e.left + 1; Solid(dc, &e, color);
    e = *r; e.left = e.right - 1; Solid(dc, &e, color);
}
static void HLine(HDC dc, int x1, int x2, int y, COLORREF color)
{
    RECT r;
    r.left = x1; r.right = x2; r.top = y; r.bottom = y + 1;
    Solid(dc, &r, color);
}
static void VLine(HDC dc, int x, int y1, int y2, COLORREF color)
{
    RECT r;
    r.left = x; r.right = x + 1; r.top = y1; r.bottom = y2;
    Solid(dc, &r, color);
}
static int TextWidth(HDC dc, const char *text)
{
    return (int)LOWORD(GetTextExtent(dc, text, strlen(text)));
}

void cad_chrome_init(HINSTANCE instance)
{
    static const unsigned char bayer[4][4] = {
        {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}
    };
    static const unsigned char checker[8] = {0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA};
    unsigned char rows[8];
    int level, x, y;
    chromeInstance = instance;
    memColor = CreateCompatibleDC(NULL);
    memMask = CreateCompatibleDC(NULL);
    fadeMask = CreateBitmap(32, 32, 1, 1, NULL);
    for (level = 0; level < LEVELS; ++level) {
        /* Level L: L/16 of the pixels are 0 bits (drawn in the "zero" colour). */
        for (y = 0; y < 8; ++y) {
            rows[y] = 0;
            for (x = 0; x < 8; ++x)
                if (bayer[y & 3][x & 3] >= (unsigned char)level) rows[y] |= (unsigned char)(0x80 >> x);
        }
        levelBitmap[level] = PatternBitmap(rows);
        levelBrush[level] = levelBitmap[level] ? CreatePatternBrush(levelBitmap[level]) : NULL;
    }
    checkerBitmap = PatternBitmap(checker);
    checkerBrush = checkerBitmap ? CreatePatternBrush(checkerBitmap) : NULL;
    boldFont = CreateFont(-11, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, VARIABLE_PITCH | FF_SWISS, "MS Sans Serif");
}
static HGDIOBJ Bold(void)
{
    return boldFont ? (HGDIOBJ)boldFont : GetStockObject(ANSI_VAR_FONT);
}

void cad_chrome_free(void)
{
    int i;
    for (i = 0; i < ICONS; ++i) {
        if (iconColor[i]) DeleteObject(iconColor[i]);
        if (iconMask[i]) DeleteObject(iconMask[i]);
        iconColor[i] = iconMask[i] = NULL; iconState[i] = 0;
    }
    for (i = 0; i < LEVELS; ++i) {
        if (levelBrush[i]) DeleteObject(levelBrush[i]);
        if (levelBitmap[i]) DeleteObject(levelBitmap[i]);
        levelBrush[i] = NULL; levelBitmap[i] = NULL;
    }
    if (checkerBrush) DeleteObject(checkerBrush);
    if (checkerBitmap) DeleteObject(checkerBitmap);
    if (fadeMask) DeleteObject(fadeMask);
    if (boldFont) DeleteObject(boldFont);
    boldFont = NULL;
    if (memColor) DeleteDC(memColor);
    if (memMask) DeleteDC(memMask);
    cad_chrome_scene_free();
    checkerBrush = NULL; checkerBitmap = NULL; fadeMask = NULL; memColor = memMask = NULL;
}

/* Loads an icon once: mask = 1 where the bitmap is magenta; the colour
 * bitmap's transparent pixels are then cleared to black for SRCPAINT. */
static int EnsureIcon(int i)
{
    BITMAP bm;
    HGDIOBJ oldColor, oldMask;
    if (iconState[i]) return iconState[i] == 1;
    iconState[i] = 2;
    if (!memColor || !memMask) return 0;
    iconColor[i] = LoadBitmap(chromeInstance, MAKEINTRESOURCE(IDB_ICON_FIRST + i));
    if (!iconColor[i]) return 0;
    GetObject(iconColor[i], sizeof(bm), (LPSTR)&bm);
    if (bm.bmWidth < 1 || bm.bmWidth > 32 || bm.bmHeight < 1 || bm.bmHeight > 32 ||
        !(iconMask[i] = CreateBitmap(bm.bmWidth, bm.bmHeight, 1, 1, NULL))) {
        DeleteObject(iconColor[i]); iconColor[i] = NULL;
        return 0;
    }
    iconWidth[i] = (unsigned char)bm.bmWidth; iconHeight[i] = (unsigned char)bm.bmHeight;
    oldColor = SelectObject(memColor, iconColor[i]);
    oldMask = SelectObject(memMask, iconMask[i]);
    SetBkColor(memColor, RGB(255,0,255));
    BitBlt(memMask, 0, 0, bm.bmWidth, bm.bmHeight, memColor, 0, 0, SRCCOPY);
    SetTextColor(memColor, C_BLACK); SetBkColor(memColor, C_WHITE);
    BitBlt(memColor, 0, 0, bm.bmWidth, bm.bmHeight, memMask, 0, 0, ROP_DSNA);
    SelectObject(memColor, oldColor); SelectObject(memMask, oldMask);
    iconState[i] = 1;
    return 1;
}

static int IconSize(int icon)
{
    return icon >= I(L_SKETCH) && icon <= I(L_HOME) ? 32 : 16;
}

/* Disabled icons: every other opaque pixel is replaced by white (the
 * ribbon/browser background behind disabled items). */
void cad_chrome_icon(HDC dc, int icon, int x, int y, int disabled)
{
    int w, h;
    COLORREF text, back;
    HGDIOBJ oldColor, oldMask, oldBrush;
    RECT r;
    if (icon < 0 || icon >= ICONS || !EnsureIcon(icon)) {
        if (icon == NO) return;
        w = icon >= 0 && icon < ICONS ? IconSize(icon) : 16;
        r.left = x + 1; r.top = y + 1; r.right = x + w - 1; r.bottom = y + w - 1;
        Solid(dc, &r, C_SILVER);
        Frame(dc, &r, C_GREY);
        return;
    }
    w = iconWidth[icon]; h = iconHeight[icon];
    text = SetTextColor(dc, C_BLACK); back = SetBkColor(dc, C_WHITE);
    oldMask = SelectObject(memMask, iconMask[icon]);
    oldColor = SelectObject(memColor, iconColor[icon]);
    BitBlt(dc, x, y, w, h, memMask, 0, 0, SRCAND);
    BitBlt(dc, x, y, w, h, memColor, 0, 0, SRCPAINT);
    if (disabled && fadeMask && checkerBrush) {
        SelectObject(memColor, fadeMask);
        SetTextColor(memColor, C_BLACK); SetBkColor(memColor, C_WHITE);
        BitBlt(memColor, 0, 0, w, h, memMask, 0, 0, SRCCOPY);
        oldBrush = SelectObject(memColor, checkerBrush);
        PatBlt(memColor, 0, 0, w, h, ROP_DPO);
        SelectObject(memColor, oldBrush);
        oldBrush = SelectObject(dc, GetStockObject(WHITE_BRUSH));
        BitBlt(dc, x, y, w, h, memColor, 0, 0, ROP_PSDPXAX);
        SelectObject(dc, oldBrush);
    }
    SelectObject(memColor, oldColor); SelectObject(memMask, oldMask);
    SetTextColor(dc, text); SetBkColor(dc, back);
}

/* ---- ribbon ---- */
static int ItemCount(const RibbonItem *items)
{
    int n = 0;
    while (items[n].kind != K_END && n < MAX_ITEMS) ++n;
    return n;
}

/* Lays out tabs and the items of one tab (font metrics via a screen DC). */
static void Layout(int tab, int width)
{
    const RibbonItem *items = tabItems[tab];
    HDC dc;
    HGDIOBJ oldFont;
    int i, j, n, x, w, panel = -1, column = -1, rows = 0, columnWidth = 0;
    if (layoutTab == tab && layoutWidth == width) return;
    dc = GetDC(NULL);
    if (!dc) return;
    oldFont = SelectObject(dc, GetStockObject(ANSI_VAR_FONT));
    x = TAB_FIRST;
    for (i = 0; i < CHROME_TABS; ++i) {
        tabLeft[i] = x; x += TextWidth(dc, tabNames[i]) + 20; tabRight[i] = x; x += 2;
    }
    n = ItemCount(items);
    x = 4;
    for (i = 0; i <= n; ++i) {
        if (column >= 0 && (i == n || items[i].kind != K_SMALL || rows == 3)) {
            for (j = column; j < i; ++j) if (items[j].kind == K_SMALL) itemRect[j].right = itemRect[j].left + columnWidth;
            x += columnWidth + 3; column = -1; rows = 0; columnWidth = 0;
        }
        if (panel >= 0 && (i == n || items[i].kind == K_PANEL)) {
            w = TextWidth(dc, items[panel].label) + 12;
            if (x - itemRect[panel].left < w) x = itemRect[panel].left + w;
            itemRect[panel].right = x; itemRect[panel].top = BODY_TOP; itemRect[panel].bottom = CAPTION_BOTTOM;
            x += 4; panel = -1;
        }
        if (i == n) break;
        if (items[i].kind == K_PANEL) {
            panel = i; itemRect[i].left = x - 1; x += 2;
        } else if (items[i].kind == K_LARGE) {
            w = TextWidth(dc, items[i].label) + 8;
            if (w < 40) w = 40;
            itemRect[i].left = x; itemRect[i].right = x + w;
            itemRect[i].top = LARGE_TOP; itemRect[i].bottom = CAPTION_TOP - 1;
            x += w + 2;
        } else {
            if (column < 0) column = i;
            w = TextWidth(dc, items[i].label) + 26;
            if (w > columnWidth) columnWidth = w;
            itemRect[i].left = x; itemRect[i].right = x + w;
            itemRect[i].top = SMALL_TOP + rows * SMALL_ROW; itemRect[i].bottom = itemRect[i].top + SMALL_ROW;
            ++rows;
        }
    }
    SelectObject(dc, oldFont);
    ReleaseDC(NULL, dc);
    layoutTab = tab; layoutWidth = width;
}

int cad_chrome_enabled(HMENU menu, int command)
{
    UINT state;
    if (!menu) return 1;
    state = GetMenuState(menu, command, MF_BYCOMMAND);
    return state == (UINT)-1 || !(state & (MF_GRAYED | MF_DISABLED));
}
static int Checked(HMENU menu, int command)
{
    UINT state;
    if (!menu) return 0;
    state = GetMenuState(menu, command, MF_BYCOMMAND);
    return state != (UINT)-1 && (state & MF_CHECKED);
}

/* Hover: light blue tint, blue frame; pressed/checked: deeper tint. */
static void ButtonFace(HDC dc, const RECT *r, int hot, int pressed, int checked)
{
    if (pressed) {
        PatternFill(dc, r, levelBrush[6], C_CYAN, C_WHITE);
        Frame(dc, r, C_NAVY);
    } else if (hot) {
        PatternFill(dc, r, levelBrush[2], C_CYAN, C_WHITE);
        Frame(dc, r, C_BLUE);
    } else if (checked) {
        PatternFill(dc, r, levelBrush[2], C_CYAN, C_WHITE);
        Frame(dc, r, C_NAVY);
    }
}

static void DrawTabs(HDC dc, int width, int tab, HMENU menu, int hot, int pressed)
{
    RECT r;
    int i;
    r.left = 0; r.top = 0; r.right = width; r.bottom = CHROME_TAB_HEIGHT;
    Solid(dc, &r, C_SILVER);
    HLine(dc, 0, width, CHROME_TAB_HEIGHT, C_GREY);
    /* File button: Inventor's blue application menu. */
    r.left = 0; r.top = 1; r.right = FILE_RIGHT; r.bottom = CHROME_TAB_HEIGHT;
    Solid(dc, &r, hot == CHROME_HIT_FILE || pressed == CHROME_HIT_FILE ? C_BLUE : C_NAVY);
    SetTextColor(dc, C_WHITE);
    SelectObject(dc, Bold());
    DrawText(dc, "File", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, GetStockObject(ANSI_VAR_FONT));
    for (i = 0; i < CHROME_TABS; ++i) {
        r.left = tabLeft[i]; r.right = tabRight[i]; r.top = 3; r.bottom = CHROME_TAB_HEIGHT + 1;
        if (i == tab) {
            Solid(dc, &r, C_WHITE);
            VLine(dc, r.left, r.top, r.bottom, C_GREY);
            VLine(dc, r.right - 1, r.top, r.bottom, C_GREY);
            HLine(dc, r.left, r.right, r.top, C_GREY);
            HLine(dc, r.left + 1, r.right - 1, r.top + 1, C_BLUE);
        } else if (hot == CHROME_HIT_TAB + i) {
            r.bottom = CHROME_TAB_HEIGHT;
            PatternFill(dc, &r, levelBrush[3], C_CYAN, C_WHITE);
        }
        r.top = 4;
        SetTextColor(dc, i == tab ? C_NAVY : C_BLACK);
        DrawText(dc, tabNames[i], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    /* Quick access: Undo / Redo at the right end of the tab row. */
    for (i = 0; i < 2 && width > tabRight[CHROME_TABS - 1] + 2 * QAT_WIDTH + 8; ++i) {
        int command = i ? ID_EDIT_REDO : ID_EDIT_UNDO, enabled = cad_chrome_enabled(menu, command);
        r.left = width - (2 - i) * QAT_WIDTH - 4; r.right = r.left + QAT_WIDTH - 2;
        r.top = 1; r.bottom = CHROME_TAB_HEIGHT - 1;
        if (enabled && hot == command) {
            ButtonFace(dc, &r, 1, pressed == command, 0);
            cad_chrome_icon(dc, i ? I(S_REDO) : I(S_UNDO), r.left + 2, r.top + 2, 0);
        } else {
            cad_chrome_icon(dc, i ? I(S_REDO) : I(S_UNDO), r.left + 2, r.top + 2, !enabled);
        }
    }
}

/* One ribbon button; clear repaints its white body background first so
 * a hover/pressed change can be drawn in place. */
static void DrawItem(HDC dc, const RibbonItem *item, const RECT *rect, HMENU menu, int hot, int pressed, int clear)
{
    RECT r = *rect;
    int enabled = cad_chrome_enabled(menu, item->command), isHot, size;
    isHot = enabled && hot == item->command;
    if (clear) Solid(dc, &r, C_WHITE);
    ButtonFace(dc, &r, isHot, isHot && pressed == item->command, Checked(menu, item->command));
    SetTextColor(dc, enabled ? C_BLACK : C_GREY);
    size = IconSize(item->icon);
    if (item->kind == K_LARGE) {
        cad_chrome_icon(dc, item->icon, (r.left + r.right - size) / 2, r.top + 3, !enabled);
        r.top += 36;
        DrawText(dc, item->label, -1, &r, DT_CENTER | DT_TOP | DT_SINGLELINE);
    } else {
        cad_chrome_icon(dc, item->icon, r.left + 3, r.top, !enabled);
        r.left += 22;
        DrawText(dc, item->label, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}

void cad_chrome_ribbon(HDC dc, int width, int tab, HMENU menu, int hot, int pressed)
{
    const RibbonItem *items;
    RECT r;
    HGDIOBJ oldFont;
    int i, n, oldMode;
    if (tab < 0 || tab >= CHROME_TABS) tab = 0;
    Layout(tab, width);
    oldFont = SelectObject(dc, GetStockObject(ANSI_VAR_FONT));
    oldMode = SetBkMode(dc, TRANSPARENT);
    DrawTabs(dc, width, tab, menu, hot, pressed);
    r.left = 0; r.right = width; r.top = BODY_TOP; r.bottom = CAPTION_TOP;
    Solid(dc, &r, C_WHITE);
    r.top = CAPTION_TOP; r.bottom = CHROME_RIBBON_HEIGHT - 1;
    Solid(dc, &r, C_SILVER);
    HLine(dc, 0, width, CHROME_RIBBON_HEIGHT - 1, C_GREY);
    items = tabItems[tab];
    n = ItemCount(items);
    for (i = 0; i < n; ++i) {
        r = itemRect[i];
        if (items[i].kind == K_PANEL) {
            r.top = CAPTION_TOP; r.bottom = CAPTION_BOTTOM + 1;
            SetTextColor(dc, C_BLACK);
            DrawText(dc, items[i].label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            VLine(dc, itemRect[i].right + 1, BODY_TOP + 3, CAPTION_BOTTOM, C_SILVER);
            VLine(dc, itemRect[i].right + 1, CAPTION_TOP, CAPTION_BOTTOM + 1, C_GREY);
            continue;
        }
        DrawItem(dc, &items[i], &r, menu, hot, pressed, 0);
    }
    SetBkMode(dc, oldMode);
    SelectObject(dc, oldFont);
    SetTextColor(dc, C_BLACK);
}

void cad_chrome_ribbon_items(HDC dc, int width, int tab, HMENU menu, int hot, int pressed, int a, int b)
{
    const RibbonItem *items;
    HGDIOBJ oldFont;
    int i, k, n, oldMode, hit, tabRow = 0;
    if (tab < 0 || tab >= CHROME_TABS) tab = 0;
    Layout(tab, width);
    oldFont = SelectObject(dc, GetStockObject(ANSI_VAR_FONT));
    oldMode = SetBkMode(dc, TRANSPARENT);
    items = tabItems[tab];
    n = ItemCount(items);
    for (k = 0; k < 2; ++k) {
        hit = k ? b : a;
        if (!hit || (k && b == a)) continue;
        /* File, tabs and quick access share the cheap tab row. */
        if (hit >= CHROME_HIT_TAB || hit == ID_EDIT_UNDO || hit == ID_EDIT_REDO) { tabRow = 1; continue; }
        for (i = 0; i < n; ++i)
            if (items[i].kind != K_PANEL && items[i].command == hit)
                DrawItem(dc, &items[i], &itemRect[i], menu, hot, pressed, 1);
    }
    if (tabRow) DrawTabs(dc, width, tab, menu, hot, pressed);
    SetBkMode(dc, oldMode);
    SelectObject(dc, oldFont);
    SetTextColor(dc, C_BLACK);
}

int cad_chrome_ribbon_hit(int x, int y, int width, int tab)
{
    const RibbonItem *items;
    int i, n;
    if (tab < 0 || tab >= CHROME_TABS || x < 0 || x >= width || y < 0 || y >= CHROME_RIBBON_HEIGHT)
        return CHROME_HIT_NONE;
    Layout(tab, width);
    if (y < CHROME_TAB_HEIGHT) {
        if (x < FILE_RIGHT) return CHROME_HIT_FILE;
        for (i = 0; i < CHROME_TABS; ++i) if (x >= tabLeft[i] && x < tabRight[i]) return CHROME_HIT_TAB + i;
        if (width > tabRight[CHROME_TABS - 1] + 2 * QAT_WIDTH + 8) {
            if (x >= width - 2 * QAT_WIDTH - 4 && x < width - QAT_WIDTH - 6) return ID_EDIT_UNDO;
            if (x >= width - QAT_WIDTH - 4 && x < width - 6) return ID_EDIT_REDO;
        }
        return CHROME_HIT_NONE;
    }
    items = tabItems[tab];
    n = ItemCount(items);
    for (i = 0; i < n; ++i)
        if (items[i].kind != K_PANEL && x >= itemRect[i].left && x < itemRect[i].right &&
            y >= itemRect[i].top && y < itemRect[i].bottom) return items[i].command;
    return CHROME_HIT_NONE;
}

const char *cad_chrome_help(int hit)
{
    const RibbonItem *items;
    int t, i;
    if (hit == CHROME_HIT_FILE) return "File: new, open, save, export, exit";
    if (hit >= CHROME_HIT_TAB && hit < CHROME_HIT_TAB + CHROME_TABS) return tabHelp[hit - CHROME_HIT_TAB];
    switch (hit) {
    case ID_EDIT_UNDO: return "Undo the last model change (Ctrl+Z)";
    case ID_EDIT_REDO: return "Redo (Ctrl+Y)";
    case ID_VIEW_HOME: return "Home view: isometric, zoom all";
    case ID_VIEW_FIT: return "Zoom all: fit the whole model";
    }
    for (t = 0; t < CHROME_TABS; ++t)
        for (items = tabItems[t], i = 0; items[i].kind != K_END; ++i)
            if (items[i].kind != K_PANEL && items[i].command == hit) return items[i].help;
    return NULL;
}

void cad_chrome_file_anchor(POINT *point)
{
    point->x = 0; point->y = CHROME_TAB_HEIGHT;
}

/* ---- viewport ---- */
/* White above, then 8 bands darkening towards silver (ordered dither of
 * white and the palette's blue-grey silver). The top band stays pure white
 * so the ViewCube corner keeps a plain background. */
void cad_chrome_background(HDC dc, int width, int height)
{
    RECT r;
    int i, start;
    if (width <= 0 || height <= 0) return;
    start = height * 45 / 100;
    if (start < 150) start = height < 150 ? height / 2 : 150;
    r.left = 0; r.right = width; r.top = 0; r.bottom = start;
    FillRect(dc, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
    for (i = 0; i < 8; ++i) {
        r.top = start + (int)((long)(height - start) * i / 8);
        r.bottom = start + (int)((long)(height - start) * (i + 1) / 8);
        PatternFill(dc, &r, levelBrush[i + 1], C_SILVER, C_WHITE);
    }
}

static int NavShown(int width, int height)
{
    return width >= 200 && height >= 240;
}
static void HomeRect(int width, RECT *r)
{
    r->left = width - 176; r->top = 6; r->right = width - 142; r->bottom = 40;
}
static void NavRect(int width, int i, RECT *r)
{
    r->left = width - CHROME_NAV_WIDTH - 2; r->right = width - 6;
    r->top = 142 + i * 22; r->bottom = r->top + 21;
}

void cad_chrome_home_rect(int width, int height, RECT *r)
{
    (void)height;
    HomeRect(width, r);
}

void cad_chrome_home(HDC dc, int width, int height, int hot)
{
    RECT r;
    if (!NavShown(width, height)) return;
    HomeRect(width, &r);
    if (hot) ButtonFace(dc, &r, 1, 0, 0);
    cad_chrome_icon(dc, I(L_HOME), r.left + 1, r.top + 1, 0);
}

/* The bar is opaque, so redrawing it in place is exact. */
void cad_chrome_nav(HDC dc, int width, int height, int hot, int mode)
{
    RECT r;
    HGDIOBJ oldPen, oldBrush;
    HPEN pen;
    int i;
    if (!NavShown(width, height)) return;
    pen = CreatePen(PS_SOLID, 1, C_GREY);
    oldPen = SelectObject(dc, pen ? (HGDIOBJ)pen : GetStockObject(BLACK_PEN));
    oldBrush = SelectObject(dc, GetStockObject(WHITE_BRUSH));
    RoundRect(dc, width - CHROME_NAV_WIDTH - 4, 138, width - 4, 142 + 4 * 22 + 3, 8, 8);
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen);
    if (pen) DeleteObject(pen);
    for (i = 0; i < 4; ++i) {
        NavRect(width, i, &r);
        ButtonFace(dc, &r, hot == navCommands[i], 0, mode && mode == navCommands[i]);
        cad_chrome_icon(dc, navIcons[i], (r.left + r.right - 16) / 2, r.top + 2, 0);
        if (i == 2) HLine(dc, r.left + 2, r.right - 2, r.bottom + 1, C_SILVER);
    }
}

/* ---- scene cache ---- */
static HDC sceneDC;
static HBITMAP sceneBitmap;
static HGDIOBJ sceneOld;
static int sceneWidth, sceneHeight;

HDC cad_chrome_scene_dc(HDC screen, int width, int height, int *recreated)
{
    *recreated = 0;
    if (width <= 0 || height <= 0) return NULL;
    if (sceneBitmap && sceneWidth == width && sceneHeight == height) return sceneDC;
    cad_chrome_scene_free();
    *recreated = 1;
    sceneDC = CreateCompatibleDC(screen);
    if (!sceneDC) return NULL;
    sceneBitmap = CreateCompatibleBitmap(screen, width, height);
    if (!sceneBitmap) { DeleteDC(sceneDC); sceneDC = NULL; return NULL; }
    sceneOld = SelectObject(sceneDC, sceneBitmap);
    sceneWidth = width; sceneHeight = height;
    return sceneDC;
}

HDC cad_chrome_scene_cached(void)
{
    return sceneBitmap ? sceneDC : NULL;
}

void cad_chrome_scene_free(void)
{
    if (sceneDC && sceneOld) SelectObject(sceneDC, sceneOld);
    if (sceneBitmap) DeleteObject(sceneBitmap);
    if (sceneDC) DeleteDC(sceneDC);
    sceneDC = NULL; sceneBitmap = NULL; sceneOld = NULL; sceneWidth = sceneHeight = 0;
}

int cad_chrome_nav_hit(int x, int y, int width, int height)
{
    RECT r;
    int i;
    if (!NavShown(width, height)) return 0;
    HomeRect(width, &r);
    if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return ID_VIEW_HOME;
    for (i = 0; i < 4; ++i) {
        NavRect(width, i, &r);
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return navCommands[i];
    }
    return 0;
}

/* ---- model browser ---- */
void cad_chrome_tree_reset(void)
{
    memset(treeLevel, 0, sizeof(treeLevel));
    memset(treeIcon, NO, sizeof(treeIcon));
    memset(treeFlags, 0, sizeof(treeFlags));
    memset(treeFolder, -1, sizeof(treeFolder));
}
void cad_chrome_tree_set(int row, int level, int icon, int flags)
{
    if (row < 0 || row >= CHROME_TREE_ROWS) return;
    treeLevel[row] = (unsigned char)level; treeIcon[row] = (unsigned char)icon;
    treeFlags[row] = (unsigned char)flags;
}
int cad_chrome_tree_flags(int row)
{
    return row >= 0 && row < CHROME_TREE_ROWS ? treeFlags[row] : 0;
}
int cad_chrome_tree_folder(int row)
{
    return row >= 0 && row < CHROME_TREE_ROWS ? treeFolder[row] : -1;
}
void cad_chrome_tree_set_folder(int row, int folder)
{
    if (row < 0 || row >= CHROME_TREE_ROWS || folder < 0 || folder >= CHROME_FOLDERS) return;
    treeFolder[row] = (signed char)folder;
    treeFlags[row] |= CHROME_TREE_FOLDER;
    if (folderOpen[folder]) treeFlags[row] |= CHROME_TREE_OPEN;
    else treeFlags[row] &= ~CHROME_TREE_OPEN;
}
int cad_chrome_folder_open(int folder)
{
    return folder >= 0 && folder < CHROME_FOLDERS && folderOpen[folder];
}
void cad_chrome_folder_set(int folder, int open)
{
    if (folder >= 0 && folder < CHROME_FOLDERS) folderOpen[folder] = (unsigned char)(open != 0);
}
static int Indent(int row)
{
    return 3 + treeLevel[row] * 12;
}
int cad_chrome_tree_expander(int row, int x)
{
    int left;
    if (row < 0 || row >= CHROME_TREE_ROWS || !(treeFlags[row] & CHROME_TREE_FOLDER)) return 0;
    left = Indent(row);
    return x >= left - 2 && x < left + 11;
}

void cad_chrome_tree_draw(const DRAWITEMSTRUCT FAR *item)
{
    char text[260];
    RECT r = item->rcItem;
    HDC dc = item->hDC;
    HGDIOBJ oldFont;
    int row = (int)item->itemID, x, mid, selected, flags, oldMode;
    if (row < 0 || row >= CHROME_TREE_ROWS) {
        if (item->itemAction & ODA_FOCUS) DrawFocusRect(dc, &r);
        return;
    }
    if (item->itemAction == ODA_FOCUS) { DrawFocusRect(dc, &r); return; }
    text[0] = 0;
    SendMessage(item->hwndItem, LB_GETTEXT, row, (LPARAM)(LPSTR)text);
    selected = (item->itemState & ODS_SELECTED) != 0;
    flags = treeFlags[row];
    Solid(dc, &r, selected ? C_BLUE : C_WHITE);
    x = r.left + Indent(row);
    mid = r.top + CHROME_ROW_HEIGHT / 2;
    if (flags & CHROME_TREE_FOLDER) {
        RECT box;
        box.left = x; box.top = mid - 4; box.right = x + 9; box.bottom = mid + 5;
        Solid(dc, &box, C_WHITE);
        Frame(dc, &box, C_GREY);
        HLine(dc, x + 2, x + 7, mid, C_BLACK);
        if (!(flags & CHROME_TREE_OPEN)) VLine(dc, x + 4, mid - 2, mid + 3, C_BLACK);
    }
    x += 11;
    if (treeIcon[row] != NO) cad_chrome_icon(dc, treeIcon[row], x, r.top + 1, 0);
    x += 19;
    oldFont = SelectObject(dc, flags & CHROME_TREE_BOLD ? Bold() : GetStockObject(ANSI_VAR_FONT));
    oldMode = SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, selected ? C_WHITE : (flags & CHROME_TREE_ERROR) ? C_RED :
        (flags & CHROME_TREE_GREY) ? C_GREY : C_BLACK);
    TextOut(dc, x, r.top + 2, text, strlen(text));
    SetBkMode(dc, oldMode);
    SelectObject(dc, oldFont);
    SetTextColor(dc, C_BLACK);
    if (item->itemState & ODS_FOCUS) DrawFocusRect(dc, &r);
}

void cad_chrome_browser_title(HDC dc, int top, int width, int height)
{
    RECT r;
    HGDIOBJ oldFont;
    int oldMode;
    r.left = 0; r.right = width; r.top = top; r.bottom = top + height;
    Solid(dc, &r, C_SILVER);
    HLine(dc, 0, width, top + height - 1, C_GREY);
    oldFont = SelectObject(dc, GetStockObject(ANSI_VAR_FONT));
    oldMode = SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, C_BLACK);
    r.left = 6; r.bottom -= 1;
    DrawText(dc, "Model", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    /* Drop-down arrow after the title, as Inventor's browser selector. */
    {
        int x = 48, y = top + height / 2 - 1, i;
        for (i = 0; i < 4; ++i) HLine(dc, x + i, x + 7 - i, y + i, C_BLACK);
    }
    SetBkMode(dc, oldMode);
    SelectObject(dc, oldFont);
}

void cad_chrome_status(HDC dc, int top, int width, int height,
                       const char *prompt, const char *counts)
{
    RECT r;
    HGDIOBJ oldFont;
    int oldMode, countsWidth;
    r.left = 0; r.right = width; r.top = top; r.bottom = top + height;
    Solid(dc, &r, C_SILVER);
    HLine(dc, 0, width, top, C_GREY);
    HLine(dc, 0, width, top + 1, C_WHITE);
    oldFont = SelectObject(dc, GetStockObject(ANSI_VAR_FONT));
    oldMode = SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, C_BLACK);
    countsWidth = counts ? TextWidth(dc, counts) : 0;
    if (countsWidth > width / 2 + 40) countsWidth = width / 2 + 40;
    r.top = top + 5; r.bottom = top + height;
    r.right = width - 6; r.left = r.right - countsWidth;
    if (counts) DrawText(dc, counts, -1, &r, DT_RIGHT | DT_SINGLELINE);
    /* Sunken divider between the prompt and the counts. */
    VLine(dc, r.left - 6, top + 4, top + height - 2, C_GREY);
    VLine(dc, r.left - 5, top + 4, top + height - 2, C_WHITE);
    r.right = r.left - 10; r.left = 6;
    if (prompt) DrawText(dc, prompt, -1, &r, DT_LEFT | DT_SINGLELINE);
    SetBkMode(dc, oldMode);
    SelectObject(dc, oldFont);
}
