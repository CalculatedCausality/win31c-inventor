/* CAD.EXE: generic Win16 vertex/edge editor. Compile EVERY linked module with
 * -ml -zt=1: the two models below must live in separate far data segments.
 * Part modeling (sketches, extrude/revolve feature tree) is driven from here
 * but its storage and dialogs live in cad_featureui.c: this module's far data
 * segments are nearly full. No curved-solid kernel or sample document. */
#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "cad_model.h"
#include "cad_create.h"
#include "cad_document.h"
#include "cad_export.h"
#include "cad_drawing.h"
#include "cad_aircraft.h"
#include "cad_meshview.h"
#include "cad_viewcube.h"
#include "cad_userparams.h"
#include "cad_vconstraints.h"
#include "cad_script.h"
#include "cad_parts.h"
#include "cad_featureui.h"
#include "cad_ui_ids.h"
#include "cad_chrome.h"

#define SIDE_WIDTH 154
/* = CHROME_RIBBON_HEIGHT (cad_chrome.h); the browser caption strip follows. */
#define RIBBON_HEIGHT 92
#define BROWSER_TITLE 22
#define CANVAS_LEFT (SIDE_WIDTH + 1)
#define STATUS_HEIGHT 24
#define GRIP 3
#define PICK 7
#define MAX_PROFILE 12
#define PATH_BYTES 128
#define G_NONE 0
#define G_GRIP 1
#define G_ORBIT 2
#define G_CUBE 3
/* Navigation bar modes (left-drag): pan and zoom. */
#define G_PAN 4
#define G_ZOOM 5
#define TIMER_CAPTURE 1
#define PICK_MACRO (-2)
/* Browser item data: vertex ID, 0x10000|edge ID, header, 0x80000000|constraint,
 * 0x20000000|component occurrence ID, 0x08000000|(profile+1)<<16|feature ID
 * (profile 0 = the feature row itself; test the feature bit before EDGE). */
#define BROWSER_FEATURE 0x08000000UL
#define BROWSER_EDGE 0x10000UL
#define BROWSER_COMPONENT 0x20000000UL
#define BROWSER_HEADER 0x40000000UL
#define BROWSER_CONSTRAINT 0x80000000UL

#define SECTION_TEXT (CAD_VC_TEXT_MAX + 1)
#define EXT_BYTES (CAD_DOC_EXT_MAX + 1)

static CadModel model;
static CadModel stage;
/* Load target / script rollback copy; never the live document. */
static CadModel spare;
/* Document state outside CadModel. Parameter and constraint records are not
 * part of model undo history: undo moves geometry only, after which
 * cad_vc_sync marks constraints the geometry no longer satisfies as Sick. */
static CadUserParams userParams;
static CadVConstraints constraints;
static CadVConstraints loadConstraints;
static CadVcMoves vcMoves;
static CadVcStatus vcFailure;
/* NEAR (DGROUP has room): -zt=1 packs this module's far BSS segments full,
 * and the window chrome globals pushed them to exactly 64 KiB. */
static char NEAR savedParamText[SECTION_TEXT], NEAR savedVcText[SECTION_TEXT];
static char NEAR sectionScratch[SECTION_TEXT];
static char extText[EXT_BYTES];
/* Placed components (Insert > Place Component). Only one live CadParts may
 * exist per process (cad_parts.c owns the part models, ~4 KiB here). Component
 * edits are document state but not model undo steps (like constraints). */
static CadParts parts;
static char savedPartsText[CAD_PARTS_TEXT_MAX + 1];
static CadAssemblyId selectedComponent;
/* Placement dialog in/out: offsets, yaw index 0..7; path shown read-only. */
static long placeValues[4];
static const char *placePath;
/* Undo group for one macro run: valid only while the history signature is
 * exactly the one recorded right after the run (kind 1) or its group undo
 * (kind 2). Any other edit, undo or redo invalidates it. */
static int groupKind;
static unsigned groupCount;
static unsigned long groupSignature;
static char fieldError[96];
/* Feature tree selection (browser): feature ID, profile index or -1. The
 * tree itself, sketch mode and feature undo live in cad_featureui.c. */
static unsigned selectedFeature;
static int NEAR selectedProfile = -1;
/* Export/drawing arrays: model entries followed by feature body edges;
 * materialSkipped body edges did not fit the 256/512 limits. */
static int materialVertices, materialEdges, materialSkipped;
static BOOL extOverflow;
/* Distance dialog in/out: axis 0..2, value, editing an existing record. */
static int distanceAxis;
static BOOL distanceEditing;
static long distanceValue;
/* C89 limits single string literals to 509 bytes; join at run time. */
static const char *const NEAR aboutText[] = {
    "Wireframe CAD: lines, XY rectangles, boxes, polygon extrusions,\n",
    "plus parametric part features (Part menu, Sketch / 3D Model tabs):\n",
    "Start 2D Sketch (XY/XZ/YZ), Rectangle/Circle/Polygon profiles,\n",
    "Finish Sketch, Extrude and Revolve; every value may use fx names.\n",
    "Features regenerate on each parameter change; the browser shows\n",
    "(!) errors, (blocked), (suppressed). Double-click to edit; Del removes.\n\n",
    "Select in browser; drag vertex grips. Right-drag orbits; the bar\n",
    "under the ViewCube adds Pan/Zoom/Orbit drag modes (Esc ends).\n",
    "Arrows: selected X/Z; PgUp/PgDn: Y; Tab: next vertex.\n",
    "Enter: coordinates; Esc: cancel drag / clear selection.\n\n",
    "Parameters (fx): Tools or Manage tab. Dialog fields accept\n",
    "expressions such as W/2+10 (mm/cm/in literals in mm documents).\n",
    "Constraints: click a vertex, Ctrl/Shift+click a second, then\n",
    "Edit > Constrain or the Constrain tab (Coincident, Align X/Y/Z,\n",
    "Distance, Fix). Moves keep constraints; undo can leave them Sick (red).\n",
    "Delete a constraint from the browser. Glyphs: C A D F.\n",
    "Run Macro: .CMD scripts (all-or-nothing; one Undo reverts them).\n",
    "Insert > Place Component references saved .CAD parts (blue);\n",
    "select one in the browser to move/ground/remove (Del).\n\n",
    "256 vertices / 512 edges; coordinates +/-400.\n",
    "Undo: 32 commands / 1024 deltas; operation: 64 deltas.\n",
    "31 parameters; 64 constraints over at most 40 vertices.\n",
    "16 components / 4 part files; offsets +/-2000, yaw 45 deg steps.\n",
    "24 features, 32 profiles; Part > Undo Last Feature Edit (4 steps).\n",
    "Parameter/constraint/component/feature edits are saved but are not\n",
    "model undo steps. DXF/OBJ/sheet: wireframe incl. feature body edges.\n",
    "No booleans, holes or curved kernel.",
    NULL
};
static char NEAR about[1700];
/* Exact saved geometry, independent of history eviction and load scratch.
 * Compare by ID, not dense order, and ignore allocator/history metadata. */
static CadVertex savedVertices[CAD_MAX_VERTICES];
static CadEdge savedEdges[CAD_MAX_EDGES];
static int savedVertexCount, savedEdgeCount;
static CadUnits savedUnits;
static CadExportVertex exportVertices[CAD_MAX_VERTICES];
static CadExportEdge exportEdges[CAD_MAX_EDGES];
static CadDrawingPoint drawingPoints[CAD_MAX_VERTICES];
static CadDrawingEdge drawingEdges[CAD_MAX_EDGES];

static HINSTANCE instance;
static HWND browser;
static FARPROC browserThunk, dialogThunk;
static WNDPROC oldBrowserProc;
static BOOL captureTimer;
static HPEN edgePen, gridPen, selectedPen, componentPen;
/* Sketch curves (blue), the active sketch (thick blue), dimmed geometry. */
static HPEN sketchPen, activePen, dimPen;
/* preBrush: pre-highlighted (hovered) vertex grip. */
static HBRUSH hotBrush, secondBrush, NEAR preBrush;
static char filePath[PATH_BYTES];
/* selected2: second vertex (Ctrl/Shift+click) for two-vertex constraints. */
static CadId selected, selectedEdge, selected2;
static CadVcId selectedConstraint;
static int width, height, canvasWidth, canvasHeight;
static int NEAR yaw = 32;
static int NEAR pitch = 24;
static BOOL sheetView, snap;
static BOOL NEAR showGrid = TRUE;
static double NEAR scale = 1.0;
static double centerX, centerY, centerZ;
static long NEAR focal = 1; /* Orthographic pixel scale * 1024; always positive. */
static int px[CAD_MAX_VERTICES], py[CAD_MAX_VERTICES];
static BOOL visible[CAD_MAX_VERTICES];
static int gesture, startMouseX, startMouseY, startYaw, startPitch;
static CadId dragTarget;
static CadVertex dragStart;
static double dragScale;
static long dragCos, dragSin, dragCP, dragSP;
static BOOL dragSnap;
static int dialogCommand;
static CadId propertyTarget;
static long dialogValues[6];
static CadSketchPoint profile[MAX_PROFILE];
static int profileCount, extrusionHeight;
static BOOL aircraftPresent, shaded;
static BOOL uiTestMode;
static int ribbonTab;
static CadCubeGeometry viewCube;
static int cubeHover, cubePending;
/* Window chrome (cad_chrome.c): hot/pressed ribbon item, hot navigation
 * button, navigation mode (ID_VIEW_PAN/ZOOM/ORBIT or 0), pre-highlighted
 * vertex index, sketch-mode state behind the automatic tab switch, and the
 * last viewport paint's fill state for status-only repaints. */
static HWND NEAR mainWindow;
static int NEAR ribbonHot, NEAR ribbonPressed, NEAR navHot, NEAR navMode;
static int NEAR hoverVertex = -1;
static BOOL NEAR sketchTabMode, NEAR lastFilled, NEAR lastFeatureFilled;
static double NEAR startScale, NEAR startCenterX, NEAR startCenterY, NEAR startCenterZ;
/* Scene cache (cad_chrome_scene_dc) holds the current scene; scene renders
 * are counted for /UITEST telemetry. */
static BOOL NEAR sceneValid;
static unsigned NEAR sceneRenders;
static HBRUSH aircraftBrushes[CAD_AIRCRAFT_MATERIAL_COUNT * 8];
static CadMeshViewVertex meshVertices[CAD_AIRCRAFT_MAX_VERTICES];
static CadMeshViewFace meshFaces[CAD_AIRCRAFT_MAX_TRIANGLES];

static const int NEAR quarter[65] = {
    0,25,50,75,100,125,150,175,200,224,249,273,297,321,345,369,
    392,415,438,460,483,505,526,548,569,590,610,630,650,669,688,707,
    724,742,759,775,792,807,822,836,851,865,878,891,903,915,926,936,
    946,955,964,972,980,987,994,1000,1005,1009,1013,1016,1019,1021,
    1023,1024,1024
};

static int ISin(int a)
{
    a &= 255;
    if (a < 64) return quarter[a];
    if (a < 128) return quarter[128-a];
    if (a < 192) return -quarter[a-128];
    return -quarter[256-a];
}
static int ICos(int a) { return ISin(a+64); }

static int VertexIndex(CadId id)
{
    int i;
    for (i=0; i<(int)model.vertex_count; ++i)
        if (model.vertices[i].id == id) return i;
    return -1;
}
/* Serialized section bodies (without headers), NUL-terminated. Both writers
 * are bounded to 4096 bytes by their modules, so failure is not expected. */
static unsigned ParamText(char *buffer)
{
    unsigned length = 0;
    if (cad_userparams_write_text(&userParams, buffer, SECTION_TEXT, &length) != CAD_UP_OK)
        length = 0;
    buffer[length] = 0;
    return length;
}
static unsigned VcText(char *buffer)
{
    unsigned length = 0;
    if (cad_vc_write_text(&constraints, buffer, SECTION_TEXT, &length) != CAD_VC_OK)
        length = 0;
    buffer[length] = 0;
    return length;
}
/* Component records; "" when none are placed (no [PARTS] section is
 * written then, so plain part files stay readable by older builds). */
static unsigned PartsText(char *buffer)
{
    unsigned length = 0;
    if (!cad_parts_count(&parts) ||
        cad_parts_write_text(&parts, buffer, CAD_PARTS_TEXT_MAX, &length) != CAD_PARTS_OK)
        length = 0;
    buffer[length] = 0;
    return length;
}
static void MarkSaved(void)
{
    ParamText(savedParamText);
    VcText(savedVcText);
    PartsText(savedPartsText);
    cad_fui_mark_saved();
    savedVertexCount = model.vertex_count;
    savedEdgeCount = model.edge_count;
    savedUnits = model.units;
    memcpy(savedVertices, model.vertices, savedVertexCount*sizeof(CadVertex));
    memcpy(savedEdges, model.edges, savedEdgeCount*sizeof(CadEdge));
}
static BOOL Dirty(void)
{
    int i;
    CadVertex v;
    CadEdge e;
    if (model.vertex_count != savedVertexCount ||
        model.edge_count != savedEdgeCount || model.units != savedUnits) return TRUE;
    for (i=0; i<savedVertexCount; ++i) {
        if (cad_model_get_vertex(&model, savedVertices[i].id, &v) != CAD_OK ||
            v.x != savedVertices[i].x || v.y != savedVertices[i].y ||
            v.z != savedVertices[i].z) return TRUE;
    }
    for (i=0; i<savedEdgeCount; ++i) {
        if (cad_model_get_edge(&model, savedEdges[i].id, &e) != CAD_OK ||
            e.a != savedEdges[i].a || e.b != savedEdges[i].b) return TRUE;
    }
    ParamText(sectionScratch);
    if (strcmp(sectionScratch, savedParamText)) return TRUE;
    VcText(sectionScratch);
    if (strcmp(sectionScratch, savedVcText)) return TRUE;
    PartsText(sectionScratch);
    if (strcmp(sectionScratch, savedPartsText)) return TRUE;
    return cad_fui_dirty();
}

/* Strict bounded ASCII integer parsing; strtol overflow is never invoked.
 * Whitespace at boundaries is accepted, fractional/trailing junk is not. */
static int ParseNumber(const char **text, long *value)
{
    const char *p = *text;
    long n = 0;
    int negative = 0, digits = 0;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '-' || *p == '+') { negative = *p == '-'; ++p; }
    while (*p >= '0' && *p <= '9') {
        n = n*10 + (*p-'0');
        if (n > 400) return 0;
        ++p; ++digits;
    }
    if (!digits) return 0;
    while (*p == ' ' || *p == '\t') ++p;
    *text = p;
    *value = negative ? -n : n;
    return 1;
}
static int ParseCoordinate(const char *text, long *value)
{
    return ParseNumber(&text, value) && !*text;
}
/* Dialog fields: plain integers, else a Parameters (fx) expression such as
 * "W/2 + 10" evaluated in document units and rounded to a model coordinate. */
static int ParseField(const char *text, long *value)
{
    CadUserParamError error;
    if (ParseCoordinate(text, value)) return 1;
    if (cad_userparams_eval_expression(&userParams, text, value, &error) == CAD_UP_OK) return 1;
    sprintf(fieldError, "%s (column %u).", error.message, error.offset + 1);
    return 0;
}
/* Component offsets: plain integers -2000..2000, else a dialog field
 * (expressions are limited to the model range -400..400). */
static int ParseOffset(const char *text, long *value)
{
    const char *p = text;
    long n = 0;
    int negative = 0, digits = 0;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '-' || *p == '+') { negative = *p == '-'; ++p; }
    while (*p >= '0' && *p <= '9') {
        n = n*10 + (*p-'0'); ++p; ++digits;
        if (n > CAD_PARTS_TRANSLATION) n = CAD_PARTS_TRANSLATION + 1;
    }
    while (*p == ' ' || *p == '\t') ++p;
    if (!digits || *p) return ParseField(text, value);
    if (n > CAD_PARTS_TRANSLATION) { strcpy(fieldError, "Offsets must be -2000..2000."); return 0; }
    *value = negative ? -n : n;
    return 1;
}
static int ParseProfile(const char *text, CadSketchPoint *out, int *count)
{
    CadSketchPoint candidate[MAX_PROFILE];
    int n = 0;
    long x,y;
    for (;;) {
        if (n == MAX_PROFILE || !ParseNumber(&text, &x) || *text++ != ',' ||
            !ParseNumber(&text, &y)) return 0;
        candidate[n].x = (int)x; candidate[n].y = (int)y; ++n;
        if (!*text) break;
        if (*text++ != ';' || !*text) return 0;
    }
    if (n < 3) return 0;
    memcpy(out, candidate, n*sizeof(CadSketchPoint));
    *count = n;
    return 1;
}
static long BoundedCoordinate(double n, BOOL useSnap)
{
    long value;
    if (n < -400.0) n = -400.0;
    if (n > 400.0) n = 400.0;
    value = (long)(n < 0 ? n-0.5 : n+0.5);
    if (useSnap) value = (value >= 0 ? value+5 : value-5)/10*10;
    return value;
}

static void Error(HWND hwnd, const char *text)
{
    MessageBox(hwnd, text, "CAD", MB_OK | MB_ICONEXCLAMATION);
}
static const char *VcMessage(CadVcStatus status)
{
    if (status == CAD_VC_CONFLICT)
        return "Constraint conflict: a fixed vertex or an inconsistent constraint prevents this. Nothing moved.";
    if (status == CAD_VC_RANGE)
        return "Constrained vertices would leave -400..400 (or distance out of range). Nothing moved.";
    return cad_vc_status_string(status);
}
static const char *PartsMessage(CadPartsStatus status)
{
    switch (status) {
    case CAD_PARTS_OK: return "OK";
    case CAD_PARTS_NOT_FOUND: return "Component not found.";
    case CAD_PARTS_FULL: return "At most 16 components can be placed.";
    case CAD_PARTS_CACHE_FULL: return "At most 4 different part files can be placed.";
    case CAD_PARTS_GROUNDED: return "The component is grounded; unground it first.";
    case CAD_PARTS_ID_EXHAUSTED: return "Component IDs are exhausted.";
    case CAD_PARTS_CAPACITY: return "Component records exceed 4096 bytes.";
    case CAD_PARTS_FORMAT: return "Malformed component record.";
    default: return "Invalid component path or placement (path 1..127 characters, no spaces; offsets -2000..2000).";
    }
}
/* A constraint failure is reported instead of the generic model status. */
static void ModelResult(HWND hwnd, CadStatus status)
{
    if (status != CAD_OK) Error(hwnd, vcFailure ? VcMessage(vcFailure) : cad_model_status_string(status));
    vcFailure = CAD_VC_OK;
}
/* Browser tree rows (owner-drawn by cad_chrome.c): "Part1" root with the
 * Origin folder, features and End of Part, then the Constraints, Components
 * and Wireframe folders. Folders and origin entries carry BROWSER_HEADER. */
#define ICON(name) (IDB_##name - IDB_ICON_FIRST)
#define ROW_ORIGIN 0x100UL
#define ROW_END_OF_PART 0x200UL
static int AddRow(const char *text, DWORD data, int level, int icon, int flags)
{
    int row = (int)SendMessage(browser, LB_ADDSTRING, 0, (LPARAM)(LPSTR)text);
    if (row < 0) return row;
    SendMessage(browser, LB_SETITEMDATA, row, (LPARAM)data);
    cad_chrome_tree_set(row, level, icon, flags);
    return row;
}
static BOOL AddFolder(const char *text, int folder, int level, int icon)
{
    int row = AddRow(text, BROWSER_HEADER | (DWORD)folder, level, icon, folder == CHROME_FOLDER_PART ? CHROME_TREE_BOLD : 0);
    cad_chrome_tree_set_folder(row, folder);
    return cad_chrome_folder_open(folder);
}
static void SyncBrowser(void)
{
    static const char *const NEAR originNames[7] = {
        "YZ Plane", "XZ Plane", "XY Plane", "X Axis", "Y Axis", "Z Axis", "Center Point"
    };
    int i, row, profileIndex, flags, icon;
    unsigned featureId, active = cad_fui_active_sketch();
    char text[100], line[260], *shown, *dot;
    const char *base;
    CadVertex *v;
    CadEdge *e;
    const CadVConstraint *c;
    CadFeatInfo info;
    if (!browser) return;
    SendMessage(browser, WM_SETREDRAW, FALSE, 0);
    SendMessage(browser, LB_RESETCONTENT, 0, 0);
    cad_chrome_tree_reset();
    /* Root: the document name without folder and extension, else Part1. */
    base = filePath;
    for (i = 0; filePath[i]; ++i) if (filePath[i] == '\\' || filePath[i] == ':' || filePath[i] == '/') base = filePath + i + 1;
    strncpy(text, *base ? base : "Part1", 40); text[40] = 0;
    dot = strchr(text, '.'); if (dot) *dot = 0;
    if (AddFolder(text, CHROME_FOLDER_PART, 0, ICON(B_PART))) {
        if (AddFolder("Origin", CHROME_FOLDER_ORIGIN, 1, ICON(B_ORIGIN)))
            for (i = 0; i < 7; ++i)
                AddRow(originNames[i], BROWSER_HEADER | (ROW_ORIGIN + i), 2,
                    i < 3 ? ICON(B_PLANE) : i < 6 ? ICON(B_AXIS) : ICON(B_POINT), 0);
        /* Features in tree order; health markers become icons/colours. */
        for (i = 0; cad_fui_row((unsigned)i, line, sizeof(line), &featureId, &profileIndex); ++i) {
            shown = line; flags = 0;
            while (*shown == ' ') ++shown;
            if (!strncmp(shown, "(!) ", 4)) { shown += 4; flags = CHROME_TREE_ERROR; }
            else if (!strncmp(shown, "(blocked) ", 10) || !strncmp(shown, "(suppressed) ", 13)) flags = CHROME_TREE_GREY;
            if (profileIndex < 0 && featureId == active) flags |= CHROME_TREE_BOLD;
            if (flags & CHROME_TREE_ERROR) icon = ICON(B_ERROR);
            else if (profileIndex >= 0) icon = ICON(B_PROFILE);
            else if (cad_features_get(cad_fui_features(), featureId, &info) != CAD_FEAT_OK) icon = ICON(B_SKETCH);
            else icon = info.type == CAD_FEAT_EXTRUDE ? ICON(B_EXTRUDE) : info.type == CAD_FEAT_REVOLVE ?
                ICON(B_REVOLVE) : ICON(B_SKETCH);
            row = AddRow(shown, BROWSER_FEATURE | ((DWORD)(profileIndex + 1) << 16) | featureId,
                profileIndex >= 0 ? 2 : 1, icon, flags);
            if (row >= 0 && selectedFeature == featureId && selectedProfile == profileIndex)
                SendMessage(browser, LB_SETCURSEL, row, 0);
        }
        AddRow("End of Part", BROWSER_HEADER | ROW_END_OF_PART, 1, ICON(B_EOP), 0);
    }
    if (constraints.count && AddFolder("Constraints", CHROME_FOLDER_CONSTRAINTS, 0, ICON(B_FOLDER))) {
        for (i=0; i<constraints.count; ++i) {
            c = &constraints.items[i];
            if (cad_vc_describe(c, text, sizeof(text)) != CAD_VC_OK) continue;
            row = AddRow(text, BROWSER_CONSTRAINT | c->id, 1,
                c->health == CAD_VC_SICK ? ICON(B_ERROR) : ICON(B_CONSTR),
                c->health == CAD_VC_SICK ? CHROME_TREE_ERROR : 0);
            if (row >= 0 && selectedConstraint == c->id) SendMessage(browser, LB_SETCURSEL, row, 0);
        }
    }
    if (cad_parts_count(&parts) && AddFolder("Components", CHROME_FOLDER_COMPONENTS, 0, ICON(B_FOLDER))) {
        for (i=0; i<(int)cad_parts_count(&parts); ++i) {
            CadAssemblyId id;
            CadPartsInfo info;
            if (cad_parts_at(&parts, (unsigned)i, &id) != CAD_PARTS_OK ||
                cad_parts_get(&parts, id, &info) != CAD_PARTS_OK) continue;
            sprintf(text, "%s%s%s", info.name, info.missing ? " (missing)" : "",
                info.grounded ? " (grounded)" : "");
            row = AddRow(text, BROWSER_COMPONENT | id, 1, info.missing ? ICON(B_ERROR) : ICON(B_COMP),
                info.missing ? CHROME_TREE_ERROR : 0);
            if (row >= 0 && selectedComponent == id) SendMessage(browser, LB_SETCURSEL, row, 0);
        }
    }
    /* Wireframe vertices/edges: up to 768 rows, so collapsed by default. */
    sprintf(text, "Wireframe (%u V, %u E)", model.vertex_count, model.edge_count);
    if ((model.vertex_count || model.edge_count) && AddFolder(text, CHROME_FOLDER_WIREFRAME, 0, ICON(B_FOLDER))) {
        for (i=0; i<(int)model.vertex_count; ++i) {
            v = &model.vertices[i];
            sprintf(text, "V%u  (%ld,%ld,%ld)%s", v->id, v->x, v->y, v->z,
                selected2 == v->id ? " [2nd]" : "");
            row = AddRow(text, (DWORD)v->id, 1, ICON(B_VERTEX), 0);
            if (row >= 0 && selected == v->id) SendMessage(browser, LB_SETCURSEL, row, 0);
        }
        for (i=0; i<(int)model.edge_count; ++i) {
            e = &model.edges[i];
            sprintf(text, "E%u  V%u - V%u", e->id, e->a, e->b);
            row = AddRow(text, BROWSER_EDGE | e->id, 1, ICON(B_EDGE), 0);
            if (row >= 0 && selectedEdge == e->id) SendMessage(browser, LB_SETCURSEL, row, 0);
        }
    }
    SendMessage(browser, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(browser, NULL, TRUE);
}
/* Expands/collapses the folder on a browser row, keeping the scroll position. */
static void ToggleFolder(int row, int open)
{
    int folder = cad_chrome_tree_folder(row), top;
    if (folder < 0) return;
    if (open < 0) open = !cad_chrome_folder_open(folder);
    if (open == cad_chrome_folder_open(folder)) return;
    top = (int)SendMessage(browser, LB_GETTOPINDEX, 0, 0);
    cad_chrome_folder_set(folder, open);
    SyncBrowser();
    SendMessage(browser, LB_SETCURSEL, row, 0);
    SendMessage(browser, LB_SETTOPINDEX, top, 0);
}
static BOOL AircraftTopology(void)
{
    const CadAircraft *asset;
    CadVertex vertex;
    CadEdge edge;
    int i;
    if (model.vertex_count < 236 || model.edge_count < 396) return FALSE;
    asset = cad_aircraft_get();
    if (!asset) return FALSE;
    for (i = 0; i < asset->vertex_count; ++i)
        if (cad_model_get_vertex(&model, (CadId)(i + 1), &vertex) != CAD_OK) return FALSE;
    for (i = 0; i < asset->edge_count; ++i) {
        if (cad_model_get_edge(&model, (CadId)(i + 1), &edge) != CAD_OK) return FALSE;
        if (edge.a != asset->edges[i].a + 1 || edge.b != asset->edges[i].b + 1) return FALSE;
    }
    return TRUE;
}

static void UpdateUI(HWND hwnd)
{
    char title[180];
    HMENU menu = GetMenu(hwnd);
    CadEdge e;
    CadVConstraint c;
    UINT two, component;
    CadPartsInfo info;
    c.kind = 0;
    if (selectedComponent && cad_parts_get(&parts, selectedComponent, &info) != CAD_PARTS_OK)
        selectedComponent = 0;
    if (selected && VertexIndex(selected) < 0) selected = 0;
    if (selectedEdge && cad_model_get_edge(&model, selectedEdge, &e) != CAD_OK)
        selectedEdge = 0;
    if (selected2 && (VertexIndex(selected2) < 0 || selected2 == selected)) selected2 = 0;
    if (selectedConstraint && cad_vc_get(&constraints, selectedConstraint, &c) != CAD_VC_OK)
        selectedConstraint = 0;
    aircraftPresent = AircraftTopology();
    if (!aircraftPresent && !cad_fui_output()->face_count) shaded = FALSE;
    sprintf(title, "CAD - %s%s%s", *filePath ? filePath : aircraftPresent ? "Boeing 747-400 demo" : "Untitled", Dirty() ? " *" : "", uiTestMode ? " [test]" : "");
    SetWindowText(hwnd, title);
    EnableMenuItem(menu, ID_EDIT_UNDO, MF_BYCOMMAND |
        (model.history_cursor ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, ID_EDIT_REDO, MF_BYCOMMAND |
        (model.history_cursor < model.history_count ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, ID_EDIT_DELETE, MF_BYCOMMAND |
        (selected || selectedEdge || selectedConstraint || selectedComponent || selectedFeature ? MF_ENABLED : MF_GRAYED));
    two = MF_BYCOMMAND | (selectedFeature ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(menu, ID_FEATURE_EDIT, two);
    EnableMenuItem(menu, ID_FEATURE_DELETE, two);
    two = MF_BYCOMMAND | (selectedFeature && selectedProfile < 0 ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(menu, ID_FEATURE_RENAME, two);
    EnableMenuItem(menu, ID_FEATURE_SUPPRESS, two);
    EnableMenuItem(menu, ID_FEATURE_UP, two);
    EnableMenuItem(menu, ID_FEATURE_DOWN, two);
    two = MF_BYCOMMAND | (cad_fui_active_sketch() ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(menu, ID_FEATURE_RECT, two);
    EnableMenuItem(menu, ID_FEATURE_CIRCLE, two);
    EnableMenuItem(menu, ID_FEATURE_POLY, two);
    EnableMenuItem(menu, ID_FEATURE_FINISH, two);
    EnableMenuItem(menu, ID_FEATURE_UNDO, MF_BYCOMMAND | (cad_fui_can_undo() ? MF_ENABLED : MF_GRAYED));
    component = MF_BYCOMMAND | (selectedComponent ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(menu, ID_INSERT_MOVE, component);
    EnableMenuItem(menu, ID_INSERT_GROUND, component);
    EnableMenuItem(menu, ID_INSERT_REMOVE, component);
    EnableMenuItem(menu, ID_INSERT_RELOAD, MF_BYCOMMAND |
        (cad_parts_count(&parts) ? MF_ENABLED : MF_GRAYED));
    two = MF_BYCOMMAND | (selected && selected2 ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(menu, ID_CONSTRAIN_COINCIDENT, two);
    EnableMenuItem(menu, ID_CONSTRAIN_ALIGN_X, two);
    EnableMenuItem(menu, ID_CONSTRAIN_ALIGN_Y, two);
    EnableMenuItem(menu, ID_CONSTRAIN_ALIGN_Z, two);
    EnableMenuItem(menu, ID_CONSTRAIN_DISTANCE, MF_BYCOMMAND |
        ((selected && selected2) || (selectedConstraint && c.kind == CAD_VC_DISTANCE) ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, ID_CONSTRAIN_FIX, MF_BYCOMMAND | (selected ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, ID_CONSTRAIN_DELETE, MF_BYCOMMAND | (selectedConstraint ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, ID_EDIT_PROPERTIES, MF_BYCOMMAND |
        (selected || selectedFeature ? MF_ENABLED : MF_GRAYED));
    CheckMenuItem(menu, ID_VIEW_DRAWING, MF_BYCOMMAND | (sheetView ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, ID_VIEW_GRID, MF_BYCOMMAND | (showGrid ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, ID_VIEW_WIREFRAME, MF_BYCOMMAND | (!shaded ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, ID_VIEW_SHADED, MF_BYCOMMAND | (shaded ? MF_CHECKED : MF_UNCHECKED));
    EnableMenuItem(menu, ID_VIEW_SHADED, MF_BYCOMMAND |
        (aircraftPresent || cad_fui_output()->face_count ? MF_ENABLED : MF_GRAYED));
    /* Entering sketch mode shows the Sketch tab; leaving it, 3D Model. */
    if ((cad_fui_active_sketch() != 0) != sketchTabMode) {
        sketchTabMode = cad_fui_active_sketch() != 0;
        ribbonTab = sketchTabMode ? CHROME_TAB_SKETCH : CHROME_TAB_MODEL;
    }
    for (two = 0; two < CHROME_TABS; ++two)
        CheckMenuItem(menu, ID_RIBBON_TAB0 + two, MF_BYCOMMAND | ((int)two == ribbonTab ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, ID_VIEW_ORBIT, MF_BYCOMMAND | (navMode == ID_VIEW_ORBIT ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, ID_VIEW_PAN, MF_BYCOMMAND | (navMode == ID_VIEW_PAN ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, ID_VIEW_ZOOM, MF_BYCOMMAND | (navMode == ID_VIEW_ZOOM ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, ID_TOOLS_SNAP, MF_BYCOMMAND | (snap ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, ID_TOOLS_UNSPECIFIED, MF_BYCOMMAND |
        (model.units == CAD_UNITS_UNSPECIFIED ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, ID_TOOLS_MM, MF_BYCOMMAND |
        (model.units == CAD_UNITS_MM ? MF_CHECKED : MF_UNCHECKED));
}
/* Geometry may have changed outside the constraint module (undo, redo, load,
 * delete, create, macro): drop records of vanished vertices, mark unmet ones
 * Sick and heal met ones. Never moves geometry. */
static void SyncConstraints(void)
{
    cad_vc_sync(&constraints, &model, NULL, NULL);
}
/* Associativity: features are re-evaluated after every document change
 * (feature edit, parameters, units, undo, load, new); <= 24 features, so
 * this is cheap. Drops a selection whose feature/profile vanished. */
static void SyncFeatures(void)
{
    CadFeatInfo info;
    cad_fui_regenerate();
    cad_fui_validate();
    if (!selectedFeature || cad_features_get(cad_fui_features(), selectedFeature, &info) != CAD_FEAT_OK) {
        selectedFeature = 0; selectedProfile = -1;
    } else if (selectedProfile >= 0 && (info.type != CAD_FEAT_SKETCH ||
        (unsigned)selectedProfile >= info.profile_count)) selectedProfile = -1;
}
/* The only way to request a scene render: geometry, camera, view mode,
 * selection or document changes. Hover feedback must never call this. */
static void ShowPending(HWND hwnd);
static void InvalidateScene(HWND hwnd)
{
    sceneValid = FALSE;
    InvalidateRect(hwnd, NULL, FALSE);
    ShowPending(hwnd);
}
static void Changed(HWND hwnd)
{
    hoverVertex = -1;
    SyncFeatures();
    SyncConstraints();
    UpdateUI(hwnd);
    SyncBrowser();
    InvalidateScene(hwnd);
}
static void EndGesture(HWND hwnd, BOOL accept)
{
    int old = gesture;
    CadStatus status = CAD_OK;
    gesture = G_NONE;
    if (old == G_GRIP) {
        if (accept) status = cad_model_commit(&model);
        else status = cad_model_cancel(&model);
    } else if (old == G_ORBIT && !accept) {
        yaw = startYaw; pitch = startPitch;
    } else if ((old == G_PAN || old == G_ZOOM) && !accept) {
        centerX = startCenterX; centerY = startCenterY; centerZ = startCenterZ;
        scale = startScale; focal = (long)(scale * 1024.0); if (focal < 1) focal = 1;
    }
    if (old && GetCapture() == hwnd) ReleaseCapture();
    if (old) Changed(hwnd);
    ModelResult(hwnd, status);
}
/* Every vertex move (grip, arrows, properties) goes through the constraint
 * solver and stages the whole solution into the caller's ACTIVE transaction.
 * On a constraint failure vcFailure explains the error for ModelResult. */
static CadStatus StageVertexMove(CadId id, long x, long y, long z)
{
    CadVcStatus vc = cad_vc_drag(&constraints, &model, id, x, y, z, &vcMoves);
    if (vc == CAD_VC_OK) vc = cad_vc_stage_moves(&model, &vcMoves);
    if (vc == CAD_VC_OK) return CAD_OK;
    vcFailure = vc;
    return vc == CAD_VC_RANGE ? CAD_RANGE : CAD_INVALID_ARGUMENT;
}
static CadStatus MoveSelected(long x, long y, long z)
{
    CadStatus status;
    if (!selected) return CAD_NOT_FOUND;
    status = cad_model_begin(&model);
    if (status != CAD_OK) return status;
    status = StageVertexMove(selected, x,y,z);
    if (status != CAD_OK) { cad_model_cancel(&model); return status; }
    return cad_model_commit(&model);
}

static void ToCamera(double x, double y, double z, double *u, double *v)
{
    double z1;
    x -= centerX; y -= centerY; z -= centerZ;
    *u = (x*ICos(yaw) + z*ISin(yaw))/1024.0;
    z1 = (-x*ISin(yaw) + z*ICos(yaw))/1024.0;
    *v = (y*ICos(pitch) - z1*ISin(pitch))/1024.0;
}
/* Grows the fit extent by one world point (the first point resets it). */
static void FitPoint(double x,double y,double z,double *extent,BOOL *first)
{
    double u,v;
    ToCamera(x,y,z,&u,&v);
    if (u<0) u=-u;
    if (v<0) v=-v;
    u=2*u/(canvasWidth>40 ? canvasWidth-40 : 1);
    v=2*v/(canvasHeight>40 ? canvasHeight-40 : 1);
    if (*first || u>*extent) *extent=u;
    if (v>*extent) *extent=v;
    *first=FALSE;
}
/* Zoom all: model vertices plus the world bounds of placed components and
 * of the regenerated feature geometry. */
static void Fit(void)
{
    int i,k;
    long lo[3],hi[3],flo[3],fhi[3];
    double minX,maxX,minY,maxY,minZ,maxZ,extent;
    BOOL components=cad_parts_bounds(&parts,lo,hi),features=cad_fui_bounds(flo,fhi),first=TRUE;
    if (!model.vertex_count && !components && !features) {
        centerX=centerY=centerZ=0;
        scale = canvasWidth>0 && canvasHeight>0 ?
            (double)min(canvasWidth,canvasHeight)/250.0 : 1.0;
    } else {
        if (model.vertex_count) {
            minX=maxX=model.vertices[0].x; minY=maxY=model.vertices[0].y;
            minZ=maxZ=model.vertices[0].z;
        } else if (components) {
            minX=maxX=lo[0]; minY=maxY=lo[1]; minZ=maxZ=lo[2];
        } else {
            minX=maxX=flo[0]; minY=maxY=flo[1]; minZ=maxZ=flo[2];
        }
        for (i=1; i<(int)model.vertex_count; ++i) {
            if (model.vertices[i].x<minX) minX=model.vertices[i].x;
            if (model.vertices[i].x>maxX) maxX=model.vertices[i].x;
            if (model.vertices[i].y<minY) minY=model.vertices[i].y;
            if (model.vertices[i].y>maxY) maxY=model.vertices[i].y;
            if (model.vertices[i].z<minZ) minZ=model.vertices[i].z;
            if (model.vertices[i].z>maxZ) maxZ=model.vertices[i].z;
        }
        if (components) {
            if (lo[0]<minX) minX=lo[0];
            if (hi[0]>maxX) maxX=hi[0];
            if (lo[1]<minY) minY=lo[1];
            if (hi[1]>maxY) maxY=hi[1];
            if (lo[2]<minZ) minZ=lo[2];
            if (hi[2]>maxZ) maxZ=hi[2];
        }
        if (features) {
            if (flo[0]<minX) minX=flo[0];
            if (fhi[0]>maxX) maxX=fhi[0];
            if (flo[1]<minY) minY=flo[1];
            if (fhi[1]>maxY) maxY=fhi[1];
            if (flo[2]<minZ) minZ=flo[2];
            if (fhi[2]>maxZ) maxZ=fhi[2];
        }
        centerX=(minX+maxX)/2; centerY=(minY+maxY)/2; centerZ=(minZ+maxZ)/2;
        extent=1;
        for (i=0; i<(int)model.vertex_count; ++i)
            FitPoint(model.vertices[i].x,model.vertices[i].y,model.vertices[i].z,&extent,&first);
        if (components) for (k=0; k<8; ++k)
            FitPoint(k&1 ? hi[0] : lo[0],k&2 ? hi[1] : lo[1],k&4 ? hi[2] : lo[2],&extent,&first);
        if (features) for (k=0; k<8; ++k)
            FitPoint(k&1 ? fhi[0] : flo[0],k&2 ? fhi[1] : flo[1],k&4 ? fhi[2] : flo[2],&extent,&first);
        scale=extent>0.0001 ? 1.0/extent : 1.0;
    }
    if (scale<0.001) scale=0.001;
    if (scale>8.0) scale=8.0;
    focal=(long)(scale*1024.0); if (focal<1) focal=1;
    scale=focal/1024.0;
}

static BOOL BuildViewCube(void)
{
    if (canvasWidth < 140 || canvasHeight < 140) return FALSE;
    return CadCubeBuild(yaw, pitch, canvasWidth - 132, 6, 126, &viewCube) != 0;
}

static void NavigateCube(HWND hwnd, int hit)
{
    int nextYaw, nextPitch;
    if (!cad_cube_orientation(hit, &nextYaw, &nextPitch)) return;
    yaw = nextYaw; pitch = nextPitch; sheetView = FALSE;
    Fit(); Changed(hwnd);
}
static void Project(void)
{
    int i;
    double u,v,x,y;
    for (i=0; i<(int)model.vertex_count; ++i) {
        visible[i]=FALSE;
        if (canvasWidth<=2*GRIP || canvasHeight<=2*GRIP || focal<1) continue;
        ToCamera(model.vertices[i].x,model.vertices[i].y,model.vertices[i].z,&u,&v);
        x=canvasWidth/2.0+u*scale; y=canvasHeight/2.0-v*scale;
        if (x<GRIP || x>=canvasWidth-GRIP || y<GRIP || y>=canvasHeight-GRIP) continue;
        px[i]=(int)x; py[i]=(int)y; visible[i]=TRUE;
    }
}
/* Liang-Barsky clipping in floating point BEFORE converting to Win16 int. */
static int ClipTest(double p, double q, double *lo, double *hi)
{
    double r;
    if (p==0) return q>=0;
    r=q/p;
    if (p<0) { if (r>*hi) return 0; if (r>*lo) *lo=r; }
    else { if (r<*lo) return 0; if (r<*hi) *hi=r; }
    return 1;
}
static void CanvasLine(HDC dc, double x1,double y1,double x2,double y2)
{
    double dx=x2-x1,dy=y2-y1,lo=0,hi=1;
    if (canvasWidth<2 || canvasHeight<2) return;
    if (!ClipTest(-dx,x1,&lo,&hi) || !ClipTest(dx,canvasWidth-1-x1,&lo,&hi) ||
        !ClipTest(-dy,y1,&lo,&hi) || !ClipTest(dy,canvasHeight-1-y1,&lo,&hi)) return;
    MoveTo(dc,(int)(x1+lo*dx),(int)(y1+lo*dy));
    LineTo(dc,(int)(x1+hi*dx),(int)(y1+hi*dy));
}
static void WorldLine(HDC dc, double x1,double y1,double z1,double x2,double y2,double z2)
{
    double u1,v1,u2,v2;
    ToCamera(x1,y1,z1,&u1,&v1); ToCamera(x2,y2,z2,&u2,&v2);
    CanvasLine(dc,canvasWidth/2.0+u1*scale,canvasHeight/2.0-v1*scale,
        canvasWidth/2.0+u2*scale,canvasHeight/2.0-v2*scale);
}
/* cad_parts_segments sink; component coordinates may reach +-2400, which
 * WorldLine/CanvasLine clip in floating point. context is an HDC *. */
static void ComponentSegment(void *context,long x1,long y1,long z1,long x2,long y2,long z2)
{
    WorldLine(*(HDC *)context,x1,y1,z1,x2,y2,z2);
}
static int Materialize(void)
{
    int i,a,b;
    for (i=0; i<(int)model.vertex_count; ++i) {
        exportVertices[i].x=(int)model.vertices[i].x;
        exportVertices[i].y=(int)model.vertices[i].y;
        exportVertices[i].z=(int)model.vertices[i].z;
        drawingPoints[i].x=model.vertices[i].x;
        drawingPoints[i].y=model.vertices[i].y;
        drawingPoints[i].z=model.vertices[i].z;
    }
    for (i=0; i<(int)model.edge_count; ++i) {
        a=VertexIndex(model.edges[i].a); b=VertexIndex(model.edges[i].b);
        if (a<0 || b<0) return 0;
        exportEdges[i].a=drawingEdges[i].a=a;
        exportEdges[i].b=drawingEdges[i].b=b;
        exportEdges[i].style=CAD_EXPORT_WALL;
    }
    /* Feature body edges follow, within the same 256/512 export limits. */
    materialVertices=model.vertex_count; materialEdges=model.edge_count;
    materialSkipped=cad_fui_materialize(exportVertices,drawingPoints,&materialVertices,CAD_MAX_VERTICES,
        exportEdges,drawingEdges,&materialEdges,CAD_MAX_EDGES);
    return 1;
}
typedef struct SheetContext { HDC dc; double scale,x,y; } SheetContext;
static int SheetLine(void *context,long x1,long y1,long x2,long y2,unsigned flags)
{
    SheetContext *c=(SheetContext *)context;
    (void)flags;
    CanvasLine(c->dc,c->x+x1*c->scale,c->y+y1*c->scale,
        c->x+x2*c->scale,c->y+y2*c->scale);
    return 1;
}
static int SheetText(void *context,long x,long y,const char *text,unsigned flags)
{
    SheetContext *c=(SheetContext *)context;
    (void)flags;
    TextOut(c->dc,(int)(c->x+x*c->scale),(int)(c->y+y*c->scale),text,strlen(text));
    return 1;
}
static void DrawSheet(HDC dc)
{
    CadDrawingModel drawing;
    CadDrawingSheet sheet;
    CadDrawingSink sink;
    SheetContext c;
    CadDrawingStatus status;
    char note[64];
    if (!Materialize()) { TextOut(dc,12,12,"Invalid edge endpoint",21); return; }
    if (!materialVertices) {
        TextOut(dc,12,12,"Empty drawing - create geometry first",37); return;
    }
    drawing.vertices=drawingPoints; drawing.vertex_count=materialVertices;
    drawing.edges=drawingEdges; drawing.edge_count=materialEdges;
    sheet.width=2600; sheet.height=2200; sheet.margin=80; sheet.gap=120;
    sheet.scale.numerator=1; sheet.scale.denominator=1;
    c.dc=dc; c.scale=(double)canvasWidth/2600.0;
    if ((double)canvasHeight/2200.0<c.scale) c.scale=(double)canvasHeight/2200.0;
    c.x=(canvasWidth-2600*c.scale)/2; c.y=(canvasHeight-2200*c.scale)/2;
    sink.context=&c; sink.line=SheetLine; sink.text=SheetText;
    status=CadDrawingRender(&drawing,&sheet,&sink,NULL);
    if (status!=CAD_DRAWING_OK) TextOut(dc,12,12,"Drawing sheet could not be laid out",34);
    else if (materialSkipped) {
        sprintf(note,"%d feature edges omitted (256/512 limit)",materialSkipped);
        TextOut(dc,12,12,note,strlen(note));
    }
}
typedef struct AircraftContext { HDC dc; BOOL flip; } AircraftContext;

static int AircraftTriangle(void *context, int x1, int y1, int x2, int y2,
                            int x3, int y3, unsigned short shadeIndex)
{
    AircraftContext *c = (AircraftContext *)context;
    POINT points[3];
    HBRUSH brush;
    if (shadeIndex >= CAD_AIRCRAFT_MATERIAL_COUNT * 8) return 0;
    if (c->flip) {
        x1 = max(0, min(canvasWidth - 1, canvasWidth - x1));
        x2 = max(0, min(canvasWidth - 1, canvasWidth - x2));
        x3 = max(0, min(canvasWidth - 1, canvasWidth - x3));
        y1 = max(0, min(canvasHeight - 1, canvasHeight - y1));
        y2 = max(0, min(canvasHeight - 1, canvasHeight - y2));
        y3 = max(0, min(canvasHeight - 1, canvasHeight - y3));
    }
    brush = aircraftBrushes[shadeIndex];
    SelectObject(c->dc, brush ? brush : (HBRUSH)GetStockObject(LTGRAY_BRUSH));
    points[0].x = x1; points[0].y = y1;
    points[1].x = x2; points[1].y = y2;
    points[2].x = x3; points[2].y = y3;
    return Polygon(c->dc, points, 3) != 0;
}

/* Orthographic mesh camera matching the wire view (aircraft and feature
 * faces). *flip reports an upside-down orbit mirrored back on screen. */
static void MeshCamera(CadMeshViewCamera *camera, CadMeshViewOrtho *ortho, BOOL *flip)
{
    memset(camera, 0, sizeof(*camera));
    *flip = FALSE;
    camera->yaw = yaw; camera->pitch = pitch > 128 ? pitch - 256 : pitch;
    /* The wire editor permits a full orbit; invert an upside-down view into
     * the equivalent supported pitch by adding a half-turn to yaw. */
    if (camera->pitch > 64) { camera->pitch = 128 - camera->pitch; camera->yaw = (yaw + 128) & 255; *flip = TRUE; }
    if (camera->pitch < -64) { camera->pitch = -128 - camera->pitch; camera->yaw = (yaw + 128) & 255; *flip = TRUE; }
    camera->width = canvasWidth; camera->height = canvasHeight;
    camera->cameraDistance = 900; camera->focal = 400; camera->backfaceCull = 1;
    ortho->centerX = centerX; ortho->centerY = centerY; ortho->centerZ = centerZ;
    ortho->pixelsPerUnit = scale;
}
static BOOL DrawFeatureFaces(HDC dc)
{
    CadMeshViewCamera camera;
    CadMeshViewOrtho ortho;
    BOOL flip;
    if (canvasWidth < 2 || canvasHeight < 2) return FALSE;
    MeshCamera(&camera, &ortho, &flip);
    return cad_fui_draw_shaded(dc, &camera, &ortho, flip);
}
static BOOL DrawAircraft(HDC dc)
{
    const CadAircraft *asset = cad_aircraft_get();
    CadMeshViewCamera camera;
    CadMeshViewOrtho ortho;
    CadMeshViewSink sink;
    CadMeshViewStatus result;
    CadVertex vertex;
    AircraftContext context;
    HGDIOBJ oldPen, oldBrush;
    int i, material, level, factor;
    if (!asset || canvasWidth < 2 || canvasHeight < 2) return FALSE;
    for (i = 0; i < asset->vertex_count; ++i) {
        if (cad_model_get_vertex(&model, (CadId)(i + 1), &vertex) != CAD_OK) return FALSE;
        meshVertices[i].x = (int)vertex.x; meshVertices[i].y = (int)vertex.y;
        meshVertices[i].z = (int)vertex.z;
    }
    for (i = 0; i < asset->triangle_count; ++i) {
        meshFaces[i].a = asset->triangles[i].a;
        meshFaces[i].b = asset->triangles[i].b;
        meshFaces[i].c = asset->triangles[i].c;
        meshFaces[i].material = asset->triangles[i].material;
    }
    for (material = 0; material < CAD_AIRCRAFT_MATERIAL_COUNT; ++material)
        for (level = 0; level < 8; ++level) if (!aircraftBrushes[material * 8 + level]) {
            factor = 45 + level * 7;
            aircraftBrushes[material * 8 + level] = CreateSolidBrush(RGB(
                (int)((long)asset->palette[material].red * factor / 100),
                (int)((long)asset->palette[material].green * factor / 100),
                (int)((long)asset->palette[material].blue * factor / 100)));
        }
    MeshCamera(&camera, &ortho, &context.flip);
    context.dc = dc; sink.context = &context; sink.triangle = AircraftTriangle;
    oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    oldBrush = SelectObject(dc, GetStockObject(LTGRAY_BRUSH));
    result = cad_meshview_render_orthographic(meshVertices, asset->vertex_count,
        meshFaces, asset->triangle_count, &camera, &sink, &ortho);
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen);
    return result == CAD_MESHVIEW_OK;
}

/* Ribbon (cad_chrome.c); command enable/check state mirrors the menu. */
static void DrawRibbon(HDC dc)
{
    cad_chrome_ribbon(dc, width, ribbonTab, mainWindow ? GetMenu(mainWindow) : NULL, ribbonHot, ribbonPressed);
}
static void TelemetryStrip(HDC dc, int y, DWORD value, const COLORREF *marker);
/* Third /UITEST strip (rows 5-6 of the status bar, under the other two; the
 * maximized client extends below the screen, so not the bottom): renders
 * mod 65536, bit 16 = scene cache allocated, bit 17 = a scene render is
 * pending (requested, not yet painted). Checksum constant 0x3e. */
static void RenderStrip(HDC dc)
{
    DWORD telemetry, checksum;
    COLORREF marker[4];
    telemetry = (DWORD)sceneRenders | ((DWORD)(cad_chrome_scene_cached() ? 1 : 0) << 16) |
        ((DWORD)(sceneValid ? 0 : 1) << 17);
    checksum = (telemetry ^ (telemetry >> 8) ^ (telemetry >> 16) ^ 0x3eUL) & 255;
    telemetry |= checksum << 24;
    marker[0] = RGB(255,255,0); marker[1] = RGB(0,255,255);
    marker[2] = RGB(255,0,255); marker[3] = RGB(255,255,0);
    TelemetryStrip(dc, height - STATUS_HEIGHT + 5, telemetry, marker);
}
/* Off-screen scene renders leave the screen unchanged for seconds: publish
 * "pending" at once so tests never mistake the old frame for the new one. */
static void ShowPending(HWND hwnd)
{
    HDC dc;
    if (!uiTestMode || width <= 90 || height <= 4 || !(dc = GetDC(hwnd))) return;
    RenderStrip(dc);
    ReleaseDC(hwnd, dc);
}
/* Status bar: prompt (hover help, navigation mode, sketch mode, selection)
 * at the left, document counts at the right; /UITEST strips on top. */
static void DrawStatus(HDC dc)
{
    char prompt[160], counts[160];
    const char *help = ribbonHot ? cad_chrome_help(ribbonHot) : navHot ? cad_chrome_help(navHot) : NULL;
    unsigned activeSketch = cad_fui_active_sketch();
    DWORD telemetry, checksum;
    COLORREF marker[4];
    CadVertex *v;
    if (height <= STATUS_HEIGHT) return;
    if (help) strcpy(prompt, help);
    else if (hoverVertex >= 0 && hoverVertex < (int)model.vertex_count) {
        v = &model.vertices[hoverVertex];
        sprintf(prompt, "V%u (%ld, %ld, %ld): drag to move; Ctrl+click picks a 2nd vertex", v->id, v->x, v->y, v->z);
    } else if (navMode) sprintf(prompt, "%s: drag in the view; Esc or the button ends it",
        navMode == ID_VIEW_PAN ? "Pan" : navMode == ID_VIEW_ZOOM ? "Zoom (drag up/down)" : "Orbit");
    else if (activeSketch) sprintf(prompt, "%.31s: add Rectangle/Circle/Polygon profiles, then Finish Sketch",
        cad_fui_name(activeSketch));
    else if (selectedFeature) sprintf(prompt, "%.31s selected: double-click edits, Del removes", cad_fui_name(selectedFeature));
    else if (selected) sprintf(prompt, "V%u selected%s: arrows/PgUp/PgDn nudge, Enter edits", selected,
        selected2 ? " (+2nd)" : "");
    else if (selectedEdge) sprintf(prompt, "E%u selected: Del removes", selectedEdge);
    else if (selectedConstraint) strcpy(prompt, "Constraint selected: Del removes, double-click edits a distance");
    else if (selectedComponent) strcpy(prompt, "Component selected: Move, Ground or Remove (Assemble tab)");
    else strcpy(prompt, "Ready");
    sprintf(counts, "%u vertices / %u edges | %s%s", model.vertex_count, model.edge_count,
        sheetView ? "drawing" : lastFilled ? "shaded" : "wireframe", snap ? " | snap" : "");
    if (model.units != CAD_UNITS_UNSPECIFIED)
        sprintf(counts + strlen(counts), " | %s", model.units == CAD_UNITS_MM ? "mm" : "in");
    if (cad_fui_count()) {
        sprintf(counts + strlen(counts), " | %u feature%s", cad_fui_count(), cad_fui_count() == 1 ? "" : "s");
        if (cad_fui_errors()) sprintf(counts + strlen(counts), " (%u failed)", cad_fui_errors());
    }
    if (cad_userparams_count(&userParams)) sprintf(counts + strlen(counts), " | fx %u", cad_userparams_count(&userParams));
    if (constraints.count) sprintf(counts + strlen(counts), " | %d constr", constraints.count);
    if (cad_parts_count(&parts)) sprintf(counts + strlen(counts), " | %u parts", cad_parts_count(&parts));
    cad_chrome_status(dc, height - STATUS_HEIGHT, width, STATUS_HEIGHT, prompt, counts);
    /* Debug-only display telemetry follows the real rendered state. It does
     * not drive commands or modify the document. Normal launches omit it. */
    if (uiTestMode && width > 90 && height > 4) {
        telemetry = (DWORD)model.vertex_count | ((DWORD)model.edge_count << 9) |
            ((DWORD)model.units << 19) | ((DWORD)(lastFilled ? 1 : 0) << 21) |
            ((DWORD)(Dirty() ? 1 : 0) << 22) |
            ((DWORD)(cad_parts_count(&parts) ? 1 : 0) << 23);
        checksum = (telemetry ^ (telemetry >> 8) ^ (telemetry >> 16) ^ 0xa7UL) & 255;
        telemetry |= checksum << 24;
        marker[0] = RGB(255,0,255); marker[1] = RGB(255,255,0);
        marker[2] = RGB(0,255,255); marker[3] = RGB(255,0,255);
        TelemetryStrip(dc, height-STATUS_HEIGHT + 1, telemetry, marker);
        /* Second strip (features, see cad_fui_telemetry): own marker order. */
        marker[0] = RGB(0,255,255); marker[1] = RGB(255,0,255);
        marker[2] = RGB(255,255,0); marker[3] = RGB(0,255,255);
        TelemetryStrip(dc, height-STATUS_HEIGHT + 3, cad_fui_telemetry(lastFeatureFilled), marker);
        RenderStrip(dc);
    }
}

static void DrawViewCube(HDC dc)
{
    POINT points[4];
    RECT r;
    HBRUSH face, hover;
    HPEN pen;
    HFONT font;
    HGDIOBJ oldPen, oldBrush, oldFont;
    DWORD extent;
    int i, j;
    if (!BuildViewCube()) return;
    face = CreateSolidBrush(RGB(230,236,245));
    hover = CreateSolidBrush(RGB(175,215,250));
    pen = CreatePen(PS_SOLID, 1, RGB(85,105,135));
    font = CreateFont(-8,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,ANSI_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,"MS Sans Serif");
    oldPen = SelectObject(dc, pen ? pen : (HPEN)GetStockObject(BLACK_PEN));
    oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    oldFont = SelectObject(dc, font ? font : (HFONT)GetStockObject(ANSI_VAR_FONT));
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(0,0,0));
    r.left = viewCube.outerOrbitRing.bounds.left;
    r.top = viewCube.outerOrbitRing.bounds.top;
    r.right = viewCube.outerOrbitRing.bounds.right + 1;
    r.bottom = viewCube.outerOrbitRing.bounds.bottom + 1;
    Ellipse(dc,r.left,r.top,r.right,r.bottom);
    for (i = 0; i < viewCube.quadCount; ++i) {
        CadCubeQuad *q = &viewCube.quads[i];
        for (j = 0; j < 4; ++j) { points[j].x = q->points[j].x; points[j].y = q->points[j].y; }
        SelectObject(dc, cubeHover == q->hit && hover ? hover : face ? face : (HBRUSH)GetStockObject(LTGRAY_BRUSH));
        Polygon(dc,points,4);
        extent = GetTextExtent(dc,q->label,strlen(q->label));
        TextOut(dc,q->labelCenter.x-(int)LOWORD(extent)/2,
            q->labelCenter.y-(int)HIWORD(extent)/2,q->label,strlen(q->label));
    }
    SelectObject(dc,oldPen); SelectObject(dc,oldBrush); SelectObject(dc,oldFont);
    if (font) DeleteObject(font);
    if (pen) DeleteObject(pen);
    if (face) DeleteObject(face);
    if (hover) DeleteObject(hover);
}

/* One letter per constraint end, stacked beside the vertex grip:
 * C coincident, A align, D distance, F fix; Sick records in red. Requires
 * Project() for the current view. */
static void DrawConstraintGlyphs(HDC dc)
{
    unsigned char stack[CAD_MAX_VERTICES];
    int i, end, index;
    const CadVConstraint *c;
    char letter[2];
    if (!constraints.count) return;
    memset(stack, 0, sizeof(stack));
    letter[1] = 0;
    for (i = 0; i < constraints.count; ++i) {
        c = &constraints.items[i];
        letter[0] = c->kind == CAD_VC_COINCIDENT ? 'C' : c->kind == CAD_VC_ALIGN ? 'A' :
            c->kind == CAD_VC_DISTANCE ? 'D' : 'F';
        SetTextColor(dc, c->health == CAD_VC_SICK ? RGB(220,0,0) : RGB(0,90,200));
        for (end = 0; end < 2; ++end) {
            index = VertexIndex(end ? c->b : c->a);
            if (index < 0 || !visible[index] || stack[index] >= 4) continue;
            TextOut(dc, px[index] + GRIP + 2 + stack[index] * 7, py[index] - GRIP - 11, letter, 1);
            ++stack[index];
        }
    }
    SetTextColor(dc, RGB(0,0,0));
}

/* Feature segments: body edges black, sketch curves blue (the active
 * sketch thick); in sketch mode everything else is dimmed. The selected
 * feature is drawn with the selection pen. */
static void DrawFeatures(HDC dc)
{
    const CadFeatOutput *o = cad_fui_output();
    const CadFeatSegment *s;
    unsigned i, active = cad_fui_active_sketch();
    HPEN pen;
    for (i = 0; i < o->segment_count; ++i) {
        s = &o->segments[i];
        if (s->feature_id == selectedFeature) pen = selectedPen;
        else if (active && s->feature_id != active) pen = dimPen;
        else if (s->kind == CAD_FEAT_SEG_SKETCH) pen = s->feature_id == active ? activePen : sketchPen;
        else pen = edgePen;
        SelectObject(dc, pen ? (HGDIOBJ)pen : GetStockObject(BLACK_PEN));
        WorldLine(dc, s->a[0], s->a[1], s->a[2], s->b[0], s->b[1], s->b[2]);
    }
    SelectObject(dc, edgePen ? (HGDIOBJ)edgePen : GetStockObject(BLACK_PEN));
}
/* /UITEST pixel strip: 4 marker pixels, then 32 data bits (white = 1),
 * each two rows high. */
static void TelemetryStrip(HDC dc, int y, DWORD value, const COLORREF *marker)
{
    int i;
    for (i = 0; i < 4; ++i) {
        SetPixel(dc, 6 + i * 2, y, marker[i]);
        SetPixel(dc, 6 + i * 2, y + 1, marker[i]);
    }
    for (i = 0; i < 32; ++i) {
        COLORREF color = value & (1UL << i) ? RGB(255,255,255) : RGB(0,0,0);
        SetPixel(dc, 14 + i * 2, y, color);
        SetPixel(dc, 14 + i * 2, y + 1, color);
    }
}
static const char NEAR emptyHint[]="Empty part - Start 2D Sketch or create wireframe";
/* Grip fill: selection colours win over the hover pre-highlight. */
static HGDIOBJ GripBrush(int index,BOOL hover)
{
    CadId id=model.vertices[index].id;
    return id==selected && hotBrush ? (HGDIOBJ)hotBrush : id==selected2 && secondBrush ? (HGDIOBJ)secondBrush :
        hover && preBrush ? (HGDIOBJ)preBrush : GetStockObject(WHITE_BRUSH);
}
/* The model scene in canvas coordinates (origin 0,0, clipped by the
 * caller): everything except the overlays (ViewCube, Home, navigation bar,
 * hovered grip), so hover feedback never needs a scene render. */
static void RenderScene(HDC dc)
{
    RECT r;
    int i,a,b;
    BOOL filled = FALSE, aircraftFilled = FALSE, featureFilled = FALSE;
    HGDIOBJ oldPen,oldBrush;
    HPEN wirePen;
    CadVertex *v,*w;
    unsigned activeSketch = cad_fui_active_sketch();
    ++sceneRenders;
    SetBkMode(dc,TRANSPARENT);
    oldPen=SelectObject(dc,edgePen ? (HGDIOBJ)edgePen : GetStockObject(BLACK_PEN));
    if (sheetView) {
        r.left=0; r.top=0; r.right=canvasWidth; r.bottom=canvasHeight;
        FillRect(dc,&r,(HBRUSH)GetStockObject(WHITE_BRUSH));
        DrawSheet(dc);
    } else {
        /* Inventor-style background: white above, banded towards silver. */
        cad_chrome_background(dc,canvasWidth,canvasHeight);
        if (showGrid) {
            SelectObject(dc,gridPen ? (HGDIOBJ)gridPen : GetStockObject(BLACK_PEN));
            for (i=-400; i<=400; i+=50) {
                WorldLine(dc,i,0,-400,i,0,400);
                WorldLine(dc,-400,0,i,400,0,i);
            }
            SelectObject(dc,edgePen ? (HGDIOBJ)edgePen : GetStockObject(BLACK_PEN));
        }
        if (shaded && aircraftPresent) aircraftFilled = DrawAircraft(dc);
        if (shaded) featureFilled = DrawFeatureFaces(dc);
        filled = aircraftFilled || featureFilled;
        wirePen = activeSketch && dimPen ? dimPen : edgePen;
        SelectObject(dc,wirePen ? (HGDIOBJ)wirePen : GetStockObject(BLACK_PEN));
        for (i=0; i<(int)model.edge_count; ++i) {
            if (aircraftFilled && model.edges[i].id <= 396 && model.edges[i].id != selectedEdge) continue;
            a=VertexIndex(model.edges[i].a); b=VertexIndex(model.edges[i].b);
            if (a<0 || b<0) continue;
            v=&model.vertices[a]; w=&model.vertices[b];
            if (selectedEdge==model.edges[i].id && selectedPen) SelectObject(dc,selectedPen);
            WorldLine(dc,v->x,v->y,v->z,w->x,w->y,w->z);
            SelectObject(dc,wirePen ? (HGDIOBJ)wirePen : GetStockObject(BLACK_PEN));
        }
        DrawFeatures(dc);
        /* Components: dark blue; the selected occurrence again, thick red. */
        if (cad_parts_count(&parts)) {
            SelectObject(dc,componentPen ? (HGDIOBJ)componentPen : GetStockObject(BLACK_PEN));
            cad_parts_segments(&parts,0,ComponentSegment,&dc);
            if (selectedComponent) {
                SelectObject(dc,selectedPen ? (HGDIOBJ)selectedPen : GetStockObject(BLACK_PEN));
                cad_parts_segments(&parts,selectedComponent,ComponentSegment,&dc);
            }
            SelectObject(dc,edgePen ? (HGDIOBJ)edgePen : GetStockObject(BLACK_PEN));
        }
        Project();
        oldBrush=SelectObject(dc,GetStockObject(WHITE_BRUSH));
        for (i=0; i<(int)model.vertex_count; ++i) if (visible[i]) {
            if (aircraftPresent && model.vertices[i].id != selected &&
                model.vertices[i].id != selected2) continue;
            SelectObject(dc,GripBrush(i,FALSE));
            Rectangle(dc,px[i]-GRIP,py[i]-GRIP,px[i]+GRIP+1,py[i]+GRIP+1);
        }
        SelectObject(dc,oldBrush);
        DrawConstraintGlyphs(dc);
        if (!model.vertex_count && !cad_parts_count(&parts) && !cad_fui_count()) {
            SetTextColor(dc,RGB(128,128,128));
            TextOut(dc,12,12,emptyHint,sizeof(emptyHint)-1);
            SetTextColor(dc,RGB(0,0,0));
        }
    }
    SelectObject(dc,oldPen);
    lastFilled=filled; lastFeatureFilled=featureFilled;
}
static void DrawGrip(HDC dc,int index,BOOL hover)
{
    HGDIOBJ oldPen,oldBrush;
    if (index<0 || index>=(int)model.vertex_count || !visible[index]) return;
    oldPen=SelectObject(dc,edgePen ? (HGDIOBJ)edgePen : GetStockObject(BLACK_PEN));
    oldBrush=SelectObject(dc,GripBrush(index,hover));
    Rectangle(dc,px[index]-GRIP,py[index]-GRIP,px[index]+GRIP+1,py[index]+GRIP+1);
    SelectObject(dc,oldBrush); SelectObject(dc,oldPen);
}
/* Overlays on top of the scene (canvas coordinates). */
static void DrawOverlays(HDC dc)
{
    if (sheetView) return;
    if (hoverVertex>=0) DrawGrip(dc,hoverVertex,TRUE);
    DrawViewCube(dc);
    cad_chrome_home(dc,canvasWidth,canvasHeight,navHot==ID_VIEW_HOME);
    cad_chrome_nav(dc,canvasWidth,canvasHeight,navHot,navMode);
}
/* Paints the window. The scene is rendered into the cache bitmap only when
 * InvalidateScene marked it stale; exposures (menus, dialogs, overlapping
 * windows) just blit it. Without GDI memory for the cache every paint
 * renders directly, as before. */
static void Render(HDC dc)
{
    RECT r;
    HDC cache;
    int saved,cacheSaved,recreated;
    DrawRibbon(dc);
    /* Splitter column between the browser and the viewport. */
    r.left=min(SIDE_WIDTH,width); r.top=RIBBON_HEIGHT; r.right=min(CANVAS_LEFT,width); r.bottom=height-STATUS_HEIGHT;
    FillRect(dc,&r,(HBRUSH)GetStockObject(GRAY_BRUSH));
    if (canvasWidth>0 && canvasHeight>0) {
        cache=cad_chrome_scene_dc(dc,canvasWidth,canvasHeight,&recreated);
        if (recreated) sceneValid=FALSE;
        saved=SaveDC(dc);
        SetViewportOrg(dc,CANVAS_LEFT,RIBBON_HEIGHT);
        IntersectClipRect(dc,0,0,canvasWidth,canvasHeight);
        if (cache) {
            if (!sceneValid) {
                cacheSaved=SaveDC(cache);
                IntersectClipRect(cache,0,0,canvasWidth,canvasHeight);
                RenderScene(cache);
                if (cacheSaved) RestoreDC(cache,cacheSaved);
            }
            BitBlt(dc,0,0,canvasWidth,canvasHeight,cache,0,0,SRCCOPY);
        } else RenderScene(dc);
        sceneValid=TRUE;
        DrawOverlays(dc);
        if (saved) RestoreDC(dc,saved);
        else { SetViewportOrg(dc,0,0); SelectClipRgn(dc,NULL); }
    }
    cad_chrome_browser_title(dc,RIBBON_HEIGHT,min(SIDE_WIDTH,width),BROWSER_TITLE);
    DrawStatus(dc);
}

static int HitTest(int x,int y)
{
    int i,best=-1;
    long dx,dy,distance,bestDistance=PICK*PICK+1;
    if (sheetView || x<0 || y<0 || x>=canvasWidth || y>=canvasHeight) return -1;
    Project();
    for (i=0; i<(int)model.vertex_count; ++i) if (visible[i]) {
        dx=(long)x-px[i]; dy=(long)y-py[i];
        if (dx < -PICK || dx > PICK || dy < -PICK || dy > PICK) continue;
        distance=dx*dx+dy*dy;
        if (distance<bestDistance) { best=i; bestDistance=distance; }
    }
    return best;
}
static void MouseMove(HWND hwnd,int x,int y)
{
    double du,dv,dx,dy,dz;
    CadStatus status;
    CadVcStatus vc;
    /* Hover (ViewCube, ribbon, grips) is ChromeHover's job: never a render. */
    if (!gesture) return;
    if (GetCapture()!=hwnd) { EndGesture(hwnd,FALSE); return; }
    if (gesture == G_CUBE) {
        if (abs(x-startMouseX)+abs(y-startMouseY) <= 3) return;
        gesture = G_ORBIT;
    }
    if (gesture==G_ORBIT) {
        yaw=(int)((startYaw+((long)x-startMouseX)/2)&255);
        pitch=(int)((startPitch+((long)y-startMouseY)/2)&255);
    } else if (gesture==G_PAN) {
        /* The press point stays under the cursor: move the view center by
         * the inverse camera rotation of the screen displacement. */
        if (scale<=0) { EndGesture(hwnd,FALSE); return; }
        du=((double)x-startMouseX)/scale; dv=((double)startMouseY-y)/scale;
        centerX=startCenterX-(du*ICos(yaw)/1024.0+dv*ISin(pitch)*ISin(yaw)/1048576.0);
        centerY=startCenterY-dv*ICos(pitch)/1024.0;
        centerZ=startCenterZ-(du*ISin(yaw)/1024.0-dv*ISin(pitch)*ICos(yaw)/1048576.0);
    } else if (gesture==G_ZOOM) {
        /* Drag up zooms in: 100 pixels doubles / halves the scale. */
        du=((double)startMouseY-y)/100.0;
        scale=startScale*(du>=0 ? 1.0+du : 1.0/(1.0-du));
        if (scale<0.001) scale=0.001;
        if (scale>8.0) scale=8.0;
        focal=(long)(scale*1024.0); if (focal<1) focal=1;
        scale=focal/1024.0;
    } else {
        if (dragScale<=0 || VertexIndex(dragTarget)<0) { EndGesture(hwnd,FALSE); return; }
        du=((double)x-startMouseX)/dragScale;
        dv=((double)startMouseY-y)/dragScale;
        /* Inverse rotation of a camera-plane displacement, frozen at press. */
        dx=du*dragCos/1024.0+dv*dragSP*dragSin/1048576.0;
        dy=dv*dragCP/1024.0;
        dz=du*dragSin/1024.0-dv*dragSP*dragCos/1048576.0;
        /* Model stage calls append deltas. Rebuild this tentative move from
         * the press state instead of consuming 64 deltas during a long drag.
         * No history command is published until the single release commit.
         * Returning to the press pixel restores exact unsnapped coordinates. */
        status=cad_model_cancel(&model);
        if (status==CAD_OK) status=cad_model_begin(&model);
        /* Constrained partners are re-solved from the press state too. A
         * group pushed past -400..400 simply stays at the press state; a
         * fixed vertex/group ends the drag with an explanation. */
        if (status==CAD_OK) status=StageVertexMove(dragTarget,
            x==startMouseX && y==startMouseY ? dragStart.x : BoundedCoordinate(dragStart.x+dx,dragSnap && dx!=0),
            x==startMouseX && y==startMouseY ? dragStart.y : BoundedCoordinate(dragStart.y+dy,dragSnap && dy!=0),
            x==startMouseX && y==startMouseY ? dragStart.z : BoundedCoordinate(dragStart.z+dz,dragSnap && dz!=0));
        if (status!=CAD_OK && vcFailure==CAD_VC_RANGE) { vcFailure=CAD_VC_OK; status=CAD_OK; }
        if (status!=CAD_OK) {
            vc=vcFailure; EndGesture(hwnd,FALSE); vcFailure=vc; ModelResult(hwnd,status); return;
        }
    }
    InvalidateScene(hwnd);
}

/* Initialized statics are NEAR (DGROUP) throughout this module: Watcom
 * -zt=1 packs far BSS to ~65000 bytes and then appends initialized far data
 * to that same segment, overflowing it (reported as 0xFFFF bytes; the guest
 * saw zeroed tables). Keep initialized data out of the far segments. */
static char NEAR cadFilter[]="CAD documents (*.CAD)\0*.CAD\0All files (*.*)\0*.*\0\0";
static char NEAR dxfFilter[]="DXF wireframe (*.DXF)\0*.DXF\0\0";
static char NEAR objFilter[]="OBJ wireframe (*.OBJ)\0*.OBJ\0\0";
static char NEAR cmdFilter[]="CAD macros (*.CMD)\0*.CMD\0All files (*.*)\0*.*\0\0";
static BOOL PickPath(HWND hwnd, BOOL save, int format, char *path)
{
    OPENFILENAME ofn;
    BOOL result;
    memset(&ofn,0,sizeof(ofn));
    ofn.lStructSize=sizeof(ofn); ofn.hwndOwner=hwnd; ofn.hInstance=instance;
    ofn.lpstrFilter=format==PICK_MACRO ? cmdFilter : format<0 ? cadFilter :
        format==CAD_EXPORT_DXF ? dxfFilter : objFilter;
    ofn.lpstrFile=path; ofn.nMaxFile=PATH_BYTES;
    ofn.lpstrDefExt=format==PICK_MACRO ? "CMD" : format<0 ? "CAD" : format==CAD_EXPORT_DXF ? "DXF" : "OBJ";
    ofn.Flags=OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!save) ofn.Flags|=OFN_FILEMUSTEXIST;
    else if (format<0) ofn.Flags|=OFN_OVERWRITEPROMPT;
    result=save ? GetSaveFileName(&ofn) : GetOpenFileName(&ofn);
    if (!result && CommDlgExtendedError()) Error(hwnd,"File dialog failed (path must fit 127 bytes).");
    return result;
}
/* CADG v2 extension text: "[PARAMS]\n" + cad_userparams text, then
 * "[CONSTRAINTS]\n" + cad_vconstraints text, then (only when components are
 * placed) "[PARTS]\n" + cad_parts text, then (only when features exist)
 * "[FEATURES]\n" + cad_features text. The first three are <= 4096 bytes
 * each; the feature tree (<= 8192) may not fit beside full sections: then
 * extOverflow is set and the document must not be saved. */
static unsigned BuildExtension(void)
{
    unsigned n;
    int overflow = 0;
    extOverflow = FALSE;
    strcpy(extText, "[PARAMS]\n"); n = strlen(extText);
    n += ParamText(extText + n);
    strcpy(extText + n, "[CONSTRAINTS]\n"); n += strlen(extText + n);
    n += VcText(extText + n);
    if (cad_parts_count(&parts)) {
        strcpy(extText + n, "[PARTS]\n"); n += strlen(extText + n);
        n += PartsText(extText + n);
    }
    if (cad_fui_count()) {
        if (n + 11 < EXT_BYTES) {
            strcpy(extText + n, "[FEATURES]\n"); n += 11;
            n += cad_fui_text(extText + n, EXT_BYTES - n, &overflow);
        } else overflow = 1;
        extOverflow = overflow != 0;
    }
    return n;
}
/* Parses all sections, then commits parameters and constraint records only
 * if every section is valid (fail-atomic; message explains a failure).
 * Policy: text before the first header, duplicate sections and UNKNOWN
 * sections are rejected, so a document written by a newer CAD.EXE is never
 * silently re-saved without its data. Missing sections mean empty (v1). */
/* Commit order: constraints parse into staging; parameters, then components
 * are read straight into the live tables (both modules are fail-atomic).
 * Only one live CadParts may exist (its part models are module statics), so
 * if [PARTS] fails after [PARAMS] succeeded, the previous parameter table is
 * restored from its own serialized text (exact round trip). A successful
 * [PARTS] read re-loads every referenced part file from disk. [FEATURES]
 * reads live after [PARAMS] (fail-atomic) and is restored from a backup if
 * [PARTS] then fails. */
static BOOL ApplyExtension(const char *text, unsigned length, CadUnits units, char *message)
{
    const char *body[4], *line, *end = text + length;
    unsigned bodyLength[4], n;
    int section = -1, i, seen[4], status;
    CadUserParamError error;
    CadUnits oldUnits = cad_userparams_units(&userParams);
    CadVcStatus vc;
    for (i = 0; i < 4; ++i) { body[i] = ""; bodyLength[i] = 0; seen[i] = 0; }
    for (line = text; line < end; line += n) {
        for (n = 0; line + n < end && line[n] != '\n'; ++n) {}
        if (line + n < end) ++n;
        if (*line != '[') {
            if (section < 0) { strcpy(message, "Document extension text precedes any section."); return FALSE; }
            bodyLength[section] += n;
            continue;
        }
        if (n == 9 && !strncmp(line, "[PARAMS]\n", 9)) i = 0;
        else if (n == 14 && !strncmp(line, "[CONSTRAINTS]\n", 14)) i = 1;
        else if (n == 8 && !strncmp(line, "[PARTS]\n", 8)) i = 2;
        else if (n == 11 && !strncmp(line, "[FEATURES]\n", 11)) i = 3;
        else {
            sprintf(message, "Unsupported document section %.40s", line);
            if (strchr(message, '\n')) *strchr(message, '\n') = 0;
            return FALSE;
        }
        if (seen[i]) { strcpy(message, "Duplicate document section."); return FALSE; }
        seen[i] = 1; section = i; body[i] = line + n;
    }
    vc = cad_vc_read_text(&loadConstraints, body[1], bodyLength[1], &n);
    if (vc != CAD_VC_OK) {
        sprintf(message, "[CONSTRAINTS] line %u: %s", n, cad_vc_status_string(vc)); return FALSE;
    }
    ParamText(sectionScratch);
    if (cad_userparams_read_text(&userParams, body[0], bodyLength[0], units, &error) != CAD_UP_OK) {
        sprintf(message, "[PARAMS] line %u: %s", error.line, error.message); return FALSE;
    }
    cad_fui_backup();
    if (!cad_fui_read(body[3], bodyLength[3], message)) {
        cad_userparams_read_text(&userParams, sectionScratch, strlen(sectionScratch), oldUnits, NULL);
        return FALSE;
    }
    n = 0;
    status = cad_parts_read_text(&parts, body[2], bodyLength[2], &n);
    if (status != CAD_PARTS_OK) {
        cad_userparams_read_text(&userParams, sectionScratch, strlen(sectionScratch), oldUnits, NULL);
        cad_fui_restore();
        sprintf(message, "[PARTS] line %u: %s", n, PartsMessage((CadPartsStatus)status)); return FALSE;
    }
    constraints = loadConstraints;
    return TRUE;
}
static void ResetSelection(void)
{
    selected = selectedEdge = selected2 = 0;
    selectedConstraint = 0;
    selectedComponent = 0;
    selectedFeature = 0; selectedProfile = -1;
    groupKind = 0;
}
/* New/demo documents start with no parameters, constraints, components or
 * features (and leave sketch mode). */
static void ResetDocumentState(void)
{
    cad_userparams_init(&userParams, model.units);
    cad_vc_init(&constraints);
    cad_parts_init(&parts);
    cad_fui_reset();
    ResetSelection();
}
static BOOL SaveDocument(HWND hwnd,BOOL saveAs)
{
    char path[PATH_BYTES];
    unsigned length;
    strcpy(path,filePath);
    length=BuildExtension();
    if (extOverflow) {
        Error(hwnd,"Parameters, constraints, components and features together exceed the 16 KiB document data limit. Remove some features or records. Nothing was saved.");
        return FALSE;
    }
    if ((saveAs || !*path) && !PickPath(hwnd,TRUE,-1,path)) return FALSE;
    if (cad_document_save_ex(path,&model,extText,length)!=0) {
        Error(hwnd,"Could not save CAD document. Document remains modified."); return FALSE;
    }
    strcpy(filePath,path); MarkSaved(); UpdateUI(hwnd); return TRUE;
}
static BOOL MayDiscard(HWND hwnd)
{
    int answer;
    if (!Dirty()) return TRUE;
    answer=MessageBox(hwnd,"Save changes to this CAD document?","CAD",
        MB_YESNOCANCEL | MB_ICONQUESTION);
    if (answer==IDCANCEL) return FALSE;
    return answer==IDYES ? SaveDocument(hwnd,FALSE) : answer==IDNO;
}

BOOL FAR PASCAL __export CoordinatesDlg(HWND dlg,UINT msg,WPARAM wp,LPARAM lp)
{
    char text[CAD_USERPARAM_EXPR_MAX+2];
    const char *labels[6];
    int i,n;
    long values[6];
    (void)lp;
    n=dialogCommand==ID_EDIT_PROPERTIES ? 3 : dialogCommand==ID_CREATE_RECTANGLE ? 5 : 6;
    if (msg==WM_INITDIALOG) {
        labels[0]="X"; labels[1]="Y"; labels[2]="Z";
        labels[3]="Width (+X)"; labels[4]="Height (+Y)"; labels[5]="Depth (+Z)";
        if (dialogCommand==ID_CREATE_LINE) {
            labels[0]="Start X"; labels[1]="Start Y"; labels[2]="Start Z";
            labels[3]="End X"; labels[4]="End Y"; labels[5]="End Z";
        }
        SetWindowText(dlg,dialogCommand==ID_CREATE_LINE ? "Create line" :
            dialogCommand==ID_CREATE_RECTANGLE ? "Create XY rectangle" :
            dialogCommand==ID_CREATE_BOX ? "Create box" : "Vertex properties");
        for (i=0; i<6; ++i) {
            SetDlgItemText(dlg,IDC_LABEL1+i,labels[i]);
            sprintf(text,"%ld",dialogValues[i]); SetDlgItemText(dlg,IDC_VALUE1+i,text);
            SendDlgItemMessage(dlg,IDC_VALUE1+i,EM_LIMITTEXT,CAD_USERPARAM_EXPR_MAX,0);
            if (i>=n) {
                ShowWindow(GetDlgItem(dlg,IDC_LABEL1+i),SW_HIDE);
                ShowWindow(GetDlgItem(dlg,IDC_VALUE1+i),SW_HIDE);
                EnableWindow(GetDlgItem(dlg,IDC_VALUE1+i),FALSE);
            }
        }
        return TRUE;
    }
    if (msg==WM_COMMAND && wp==IDCANCEL) { EndDialog(dlg,IDCANCEL); return TRUE; }
    if (msg==WM_COMMAND && wp==IDOK) {
        for (i=0; i<n; ++i) {
            GetDlgItemText(dlg,IDC_VALUE1+i,text,sizeof(text));
            if (!ParseField(text,&values[i])) {
                Error(dlg,fieldError); SetFocus(GetDlgItem(dlg,IDC_VALUE1+i)); return TRUE;
            }
            if (i>=3 && dialogCommand!=ID_CREATE_LINE && values[i]<=0) {
                Error(dlg,"Dimensions must be 1..400.");
                SetFocus(GetDlgItem(dlg,IDC_VALUE1+i)); return TRUE;
            }
        }
        if (dialogCommand==ID_CREATE_RECTANGLE || dialogCommand==ID_CREATE_BOX) {
            if (values[0]+values[3]>400 || values[1]+values[4]>400 ||
                (n==6 && values[2]+values[5]>400)) {
                Error(dlg,"Origin plus each dimension must stay within -400..400."); return TRUE;
            }
        }
        memcpy(dialogValues,values,n*sizeof(long)); EndDialog(dlg,IDOK); return TRUE;
    }
    return FALSE;
}
BOOL FAR PASCAL __export ExtrusionDlg(HWND dlg,UINT msg,WPARAM wp,LPARAM lp)
{
    char text[256];
    long value,area;
    CadSketchStatus status;
    (void)lp;
    if (msg==WM_INITDIALOG) {
        SendDlgItemMessage(dlg,IDC_PROFILE,EM_LIMITTEXT,250,0);
        SendDlgItemMessage(dlg,IDC_HEIGHT,EM_LIMITTEXT,CAD_USERPARAM_EXPR_MAX,0);
        SetDlgItemText(dlg,IDC_PROFILE,"-50,-50;50,-50;50,50;-50,50");
        SetDlgItemText(dlg,IDC_HEIGHT,"100"); return TRUE;
    }
    if (msg==WM_COMMAND && wp==IDCANCEL) { EndDialog(dlg,IDCANCEL); return TRUE; }
    if (msg==WM_COMMAND && wp==IDOK) {
        GetDlgItemText(dlg,IDC_PROFILE,text,sizeof(text));
        if (!ParseProfile(text,profile,&profileCount)) {
            Error(dlg,"Enter 3..12 XY points as x,y;x,y;... with coordinates -400..400."); return TRUE;
        }
        status=cad_sketch_validate(profile,profileCount,&area);
        if (status!=CAD_SKETCH_OK) {
            Error(dlg,"Polygon must be simple, nonzero-area, with no duplicate points or crossings."); return TRUE;
        }
        GetDlgItemText(dlg,IDC_HEIGHT,text,sizeof(text));
        if (!ParseField(text,&value)) { Error(dlg,fieldError); return TRUE; }
        if (value<1) { Error(dlg,"Extrusion height must be 1..400 along +Z."); return TRUE; }
        extrusionHeight=(int)value; EndDialog(dlg,IDOK); return TRUE;
    }
    return FALSE;
}
static int RunDialog(HWND hwnd,FARPROC proc,int id)
{
    int result;
    FARPROC thunk=MakeProcInstance(proc,instance);
    if (!thunk) { Error(hwnd,"Not enough memory for dialog callback."); return -1; }
    result=DialogBox(instance,MAKEINTRESOURCE(id),hwnd,(DLGPROC)thunk);
    FreeProcInstance(thunk);
    if (result==-1) Error(hwnd,"Could not create native dialog.");
    return result;
}

/* ---- Components (Insert > Place Component) ---- */
/* Yaw in degrees, a multiple of 45 (negative or >= 360 wrap) -> index 0..7. */
static int ParseYaw(const char *text, long *index)
{
    long degrees;
    if (!ParseOffset(text,&degrees) || degrees%45) {
        strcpy(fieldError,"Yaw must be a multiple of 45 degrees (0, 45, ... 315).");
        return 0;
    }
    *index=((degrees/45)%8+8)%8;
    return 1;
}
BOOL FAR PASCAL __export PlacementDlg(HWND dlg,UINT msg,WPARAM wp,LPARAM lp)
{
    char text[CAD_USERPARAM_EXPR_MAX+2];
    long values[4];
    int i;
    (void)lp;
    if (msg==WM_INITDIALOG) {
        SetDlgItemText(dlg,IDC_PLACE_PATH,placePath ? placePath : "");
        for (i=0; i<4; ++i) {
            sprintf(text,"%ld",i<3 ? placeValues[i] : placeValues[3]*45);
            SetDlgItemText(dlg,IDC_VALUE1+i,text);
            SendDlgItemMessage(dlg,IDC_VALUE1+i,EM_LIMITTEXT,CAD_USERPARAM_EXPR_MAX,0);
        }
        return TRUE;
    }
    if (msg==WM_COMMAND && wp==IDCANCEL) { EndDialog(dlg,IDCANCEL); return TRUE; }
    if (msg==WM_COMMAND && wp==IDOK) {
        for (i=0; i<4; ++i) {
            GetDlgItemText(dlg,IDC_VALUE1+i,text,sizeof(text));
            if (!(i<3 ? ParseOffset(text,&values[i]) : ParseYaw(text,&values[i]))) {
                Error(dlg,fieldError); SetFocus(GetDlgItem(dlg,IDC_VALUE1+i)); return TRUE;
            }
        }
        memcpy(placeValues,values,sizeof(values)); EndDialog(dlg,IDOK); return TRUE;
    }
    return FALSE;
}
/* Place, move, ground, remove and reload. None of these are model undo
 * steps; they change the document (dirty) like constraint edits. */
static void ComponentCommand(HWND hwnd,int command)
{
    char path[PATH_BYTES],text[220];
    CadPartsInfo info;
    CadAssemblyId id;
    CadPartsStatus status=CAD_PARTS_OK;
    unsigned i,missing=0;
    BOOL known=selectedComponent && cad_parts_get(&parts,selectedComponent,&info)==CAD_PARTS_OK;
    if (command!=ID_INSERT_PLACE && command!=ID_INSERT_RELOAD && !known) {
        Error(hwnd,"Select a component in the model browser first."); return;
    }
    if (command==ID_INSERT_PLACE) {
        path[0]=0;
        if (!PickPath(hwnd,FALSE,-1,path)) return;
        placePath=path;
        for (i=0; i<4; ++i) placeValues[i]=0;
        if (RunDialog(hwnd,(FARPROC)PlacementDlg,IDD_PLACEMENT)!=IDOK) return;
        status=cad_parts_place(&parts,path,placeValues[0],placeValues[1],placeValues[2],
            (int)placeValues[3],&id);
        if (status==CAD_PARTS_OK) {
            selected=selectedEdge=selected2=0; selectedConstraint=0; selectedComponent=id;
            if (cad_parts_get(&parts,id,&info)==CAD_PARTS_OK && info.missing)
                Error(hwnd,"The part file could not be read as a CAD document; it is placed as missing (no geometry). Use Reload Components after fixing it.");
            sheetView=FALSE; Fit();
        }
    } else if (command==ID_INSERT_MOVE) {
        placePath=info.path;
        placeValues[0]=info.tx; placeValues[1]=info.ty; placeValues[2]=info.tz; placeValues[3]=info.yaw;
        if (info.grounded) status=CAD_PARTS_GROUNDED;
        else if (RunDialog(hwnd,(FARPROC)PlacementDlg,IDD_PLACEMENT)!=IDOK) return;
        else status=cad_parts_move(&parts,info.id,placeValues[0],placeValues[1],placeValues[2],
            (int)placeValues[3]);
    } else if (command==ID_INSERT_GROUND) {
        status=cad_parts_ground(&parts,info.id,!info.grounded);
    } else if (command==ID_INSERT_REMOVE) {
        status=cad_parts_remove(&parts,info.id);
        if (status==CAD_PARTS_OK) selectedComponent=0;
    } else {
        status=cad_parts_reload(&parts);
        for (i=0; i<cad_parts_count(&parts); ++i)
            if (cad_parts_at(&parts,i,&id)==CAD_PARTS_OK && cad_parts_get(&parts,id,&info)==CAD_PARTS_OK &&
                info.missing) ++missing;
        if (missing) {
            sprintf(text,"%u component(s) reference part files that could not be read.",missing);
            Error(hwnd,text);
        }
    }
    if (status!=CAD_PARTS_OK) Error(hwnd,PartsMessage(status));
    Changed(hwnd);
}

/* ---- Parameters (fx) ---- */
static void ParamError(HWND dlg,const CadUserParamError *error)
{
    char text[200];
    CadUserParamInfo info;
    if (error->parameter_id && cad_userparams_get(&userParams,error->parameter_id,&info)==CAD_UP_OK)
        sprintf(text,"%s\n\nParameter involved: %s (column %u).",error->message,info.name,error->offset+1);
    else sprintf(text,"%s (column %u).",error->message,error->offset+1);
    Error(dlg,text);
}
static void FillParamList(HWND dlg,unsigned selectId)
{
    char text[CAD_USERPARAM_NAME_MAX+CAD_USERPARAM_EXPR_MAX+CAD_USERPARAM_DISPLAY_MAX+12];
    CadUserParamInfo info;
    unsigned i;
    int row;
    SendDlgItemMessage(dlg,IDC_PARAM_LIST,LB_RESETCONTENT,0,0);
    for (i=0; i<cad_userparams_count(&userParams); ++i) {
        if (cad_userparams_at(&userParams,i,&info)!=CAD_UP_OK) continue;
        sprintf(text,"%s = %s -> %s",info.name,info.expression,info.display);
        row=(int)SendDlgItemMessage(dlg,IDC_PARAM_LIST,LB_ADDSTRING,0,(LPARAM)(LPSTR)text);
        if (row<0) continue;
        SendDlgItemMessage(dlg,IDC_PARAM_LIST,LB_SETITEMDATA,row,(LPARAM)(DWORD)info.id);
        if (info.id==selectId) SendDlgItemMessage(dlg,IDC_PARAM_LIST,LB_SETCURSEL,row,0);
    }
    SetDlgItemText(dlg,IDC_PARAM_UNITS,cad_userparams_units(&userParams)==CAD_UNITS_MM ?
        "Units: mm (mm/cm/in literals allowed; Tools > Units)" :
        "Units: unspecified (plain numbers only; Tools > Units)");
}
static unsigned SelectedParam(HWND dlg)
{
    int row=(int)SendDlgItemMessage(dlg,IDC_PARAM_LIST,LB_GETCURSEL,0,0);
    if (row<0) return 0;
    return (unsigned)SendDlgItemMessage(dlg,IDC_PARAM_LIST,LB_GETITEMDATA,row,0);
}
BOOL FAR PASCAL __export ParametersDlg(HWND dlg,UINT msg,WPARAM wp,LPARAM lp)
{
    char name[CAD_USERPARAM_NAME_MAX+2], expression[CAD_USERPARAM_EXPR_MAX+2];
    CadUserParamError error;
    CadUserParamInfo info;
    CadUserParamStatus status;
    unsigned id;
    BOOL exprChanged,nameChanged;
    if (msg==WM_INITDIALOG) {
        SendDlgItemMessage(dlg,IDC_PARAM_NAME,EM_LIMITTEXT,CAD_USERPARAM_NAME_MAX,0);
        SendDlgItemMessage(dlg,IDC_PARAM_EXPR,EM_LIMITTEXT,CAD_USERPARAM_EXPR_MAX,0);
        FillParamList(dlg,0);
        SetFocus(GetDlgItem(dlg,IDC_PARAM_NAME));
        return FALSE;
    }
    if (msg!=WM_COMMAND) return FALSE;
    if (wp==IDCANCEL) { EndDialog(dlg,IDCANCEL); return TRUE; }
    if (wp==IDC_PARAM_LIST) {
        if (HIWORD(lp)==LBN_SELCHANGE && cad_userparams_get(&userParams,SelectedParam(dlg),&info)==CAD_UP_OK) {
            SetDlgItemText(dlg,IDC_PARAM_NAME,info.name);
            SetDlgItemText(dlg,IDC_PARAM_EXPR,info.expression);
        }
        return TRUE;
    }
    if (wp!=IDOK && wp!=IDC_PARAM_ADD && wp!=IDC_PARAM_MODIFY && wp!=IDC_PARAM_DELETE) return FALSE;
    GetDlgItemText(dlg,IDC_PARAM_NAME,name,sizeof(name));
    GetDlgItemText(dlg,IDC_PARAM_EXPR,expression,sizeof(expression));
    id=0;
    if (wp==IDOK || wp==IDC_PARAM_ADD) {
        status=cad_userparams_add(&userParams,name,expression,&id,&error);
    } else {
        id=SelectedParam(dlg);
        if (cad_userparams_get(&userParams,id,&info)!=CAD_UP_OK) {
            Error(dlg,"Select a parameter in the list first."); return TRUE;
        }
        if (wp==IDC_PARAM_DELETE) {
            status=cad_userparams_remove(&userParams,id,&error); id=0;
        } else {
            /* Expression first, then rename; undo the expression if the
             * rename is rejected so Modify is all-or-nothing. */
            strcpy(sectionScratch,info.expression);
            exprChanged=strcmp(expression,info.expression)!=0;
            nameChanged=strcmp(name,info.name)!=0;
            status=exprChanged ? cad_userparams_set_expression(&userParams,id,expression,&error) : CAD_UP_OK;
            if (status==CAD_UP_OK && nameChanged) {
                status=cad_userparams_rename(&userParams,id,name,&error);
                if (status!=CAD_UP_OK && exprChanged)
                    cad_userparams_set_expression(&userParams,id,sectionScratch,NULL);
            }
        }
    }
    if (status!=CAD_UP_OK) { ParamError(dlg,&error); return TRUE; }
    /* Associativity: features using the parameter resize right away. */
    cad_fui_regenerate();
    InvalidateScene(GetParent(dlg));
    FillParamList(dlg,id);
    if (wp!=IDC_PARAM_MODIFY) {
        SetDlgItemText(dlg,IDC_PARAM_NAME,""); SetDlgItemText(dlg,IDC_PARAM_EXPR,"");
    }
    SetFocus(GetDlgItem(dlg,IDC_PARAM_NAME));
    return TRUE;
}

static void CreateOrProperties(HWND hwnd,int command)
{
    CadVertex v;
    CadStatus status=CAD_OK;
    int result,i;
    FARPROC proc;
    dialogCommand=command;
    propertyTarget=selected;
    for (i=0; i<6; ++i) dialogValues[i]=i<3 ? 0 : 100;
    if (command==ID_EDIT_PROPERTIES) {
        if (cad_model_get_vertex(&model,propertyTarget,&v)!=CAD_OK) return;
        dialogValues[0]=v.x; dialogValues[1]=v.y; dialogValues[2]=v.z;
    }
    proc=command==ID_CREATE_EXTRUDE ? (FARPROC)ExtrusionDlg : (FARPROC)CoordinatesDlg;
    dialogThunk=MakeProcInstance(proc,instance);
    if (!dialogThunk) { Error(hwnd,"Not enough memory for dialog callback."); return; }
    result=DialogBox(instance,MAKEINTRESOURCE(command==ID_CREATE_EXTRUDE ?
        IDD_EXTRUSION : IDD_COORDINATES),hwnd,(DLGPROC)dialogThunk);
    FreeProcInstance(dialogThunk); dialogThunk=NULL;
    if (result==-1) { Error(hwnd,"Could not create native dialog."); return; }
    if (result!=IDOK) return;
    if (command==ID_CREATE_LINE) status=cad_create_line(&model,dialogValues[0],dialogValues[1],
        dialogValues[2],dialogValues[3],dialogValues[4],dialogValues[5]);
    else if (command==ID_CREATE_RECTANGLE) status=cad_create_rectangle(&model,dialogValues[0],
        dialogValues[1],dialogValues[2],dialogValues[3],dialogValues[4]);
    else if (command==ID_CREATE_BOX) status=cad_create_box(&model,dialogValues[0],dialogValues[1],
        dialogValues[2],dialogValues[3],dialogValues[4],dialogValues[5]);
    else if (command==ID_CREATE_EXTRUDE) status=cad_create_extrusion(&model,profile,profileCount,extrusionHeight);
    else {
        /* A modal dialog never changes its stable target; no array indices. */
        selected=propertyTarget;
        status=MoveSelected(dialogValues[0],dialogValues[1],dialogValues[2]);
    }
    ModelResult(hwnd,status);
    if (status==CAD_OK && command!=ID_EDIT_PROPERTIES) { sheetView=FALSE; Fit(); }
    Changed(hwnd);
}
/* ---- Part features (dialogs and storage in cad_featureui.c) ---- */
/* Sketch mode looks straight at the sketch plane (view presets). */
static void SketchView(int plane)
{
    yaw = plane == CAD_FEAT_YZ ? 64 : 0;
    pitch = plane == CAD_FEAT_XZ ? 64 : 0;
    sheetView = FALSE;
}
/* Feature edits are document state (dirty, [FEATURES]) but not model undo
 * steps; Part > Undo Last Feature Edit reverts up to four of them. */
static void FeatureCommand(HWND hwnd,int command)
{
    char message[CAD_FUI_MESSAGE];
    CadFuiDialog *d=&cad_fui_dialog;
    int kind=0,ok=1,id;
    unsigned sketch;
    message[0]=0;
    if ((command==ID_FEATURE_EDIT || command==ID_FEATURE_RENAME || command==ID_FEATURE_SUPPRESS ||
        command==ID_FEATURE_UP || command==ID_FEATURE_DOWN || command==ID_FEATURE_DELETE) && !selectedFeature) {
        Error(hwnd,"Select a feature in the model browser first."); return;
    }
    switch (command) {
    case ID_FEATURE_FINISH:
        if (cad_fui_active_sketch()) { yaw=32; pitch=24; sheetView=FALSE; }
        cad_fui_set_active_sketch(0); Fit(); Changed(hwnd); return;
    case ID_FEATURE_SUPPRESS: ok=cad_fui_toggle_suppress(selectedFeature,message); break;
    case ID_FEATURE_UP: case ID_FEATURE_DOWN:
        ok=cad_fui_move(selectedFeature,command==ID_FEATURE_UP,message); break;
    case ID_FEATURE_DELETE:
        ok=cad_fui_remove(selectedFeature,selectedProfile,message);
        if (ok) { selectedFeature=0; selectedProfile=-1; }
        break;
    case ID_FEATURE_UNDO: ok=cad_fui_undo(message); break;
    default:
        kind=command==ID_FEATURE_SKETCH ? CAD_FUI_SKETCH : command==ID_FEATURE_RECT ? CAD_FUI_RECT :
            command==ID_FEATURE_CIRCLE ? CAD_FUI_CIRCLE : command==ID_FEATURE_POLY ? CAD_FUI_POLY :
            command==ID_FEATURE_EXTRUDE ? CAD_FUI_EXTRUDE : command==ID_FEATURE_REVOLVE ? CAD_FUI_REVOLVE :
            command==ID_FEATURE_RENAME ? CAD_FUI_RENAME : 0;
        if (!cad_fui_prepare(kind,kind ? (kind==CAD_FUI_RENAME ? selectedFeature : 0) : selectedFeature,
                kind ? -1 : selectedProfile)) {
            Error(hwnd,d->message); return;
        }
        id=d->kind==CAD_FUI_SKETCH ? IDD_FEAT_SKETCH : d->kind==CAD_FUI_RENAME ? IDD_FEAT_RENAME :
            d->kind==CAD_FUI_EXTRUDE || d->kind==CAD_FUI_REVOLVE ? IDD_FEAT_SOLID : IDD_FEAT_PROFILE;
        /* The dialog applies (and validates by regenerating) on OK. */
        if (RunDialog(hwnd,(FARPROC)FeatureDlg,id)!=IDOK) { Changed(hwnd); return; }
        if (d->created) {
            selected=selectedEdge=selected2=0; selectedConstraint=0; selectedComponent=0;
            selectedFeature=d->created; selectedProfile=d->created_profile;
        }
        if (d->kind==CAD_FUI_SKETCH) {
            sketch=d->created ? d->created : d->target;
            cad_fui_set_active_sketch(sketch);
            SketchView(d->option);
        } else sheetView=FALSE;
        if (d->kind!=CAD_FUI_RENAME) Fit();
        break;
    }
    if (!ok) Error(hwnd,message);
    Changed(hwnd);
}
/* ---- Constraints ---- */
static long AxisDelta(int axis)
{
    CadVertex a,b;
    if (cad_model_get_vertex(&model,selected,&a)!=CAD_OK ||
        cad_model_get_vertex(&model,selected2,&b)!=CAD_OK) return 0;
    return axis==0 ? b.x-a.x : axis==1 ? b.y-a.y : b.z-a.z;
}
BOOL FAR PASCAL __export DistanceDlg(HWND dlg,UINT msg,WPARAM wp,LPARAM lp)
{
    char text[CAD_USERPARAM_EXPR_MAX+2];
    int i;
    (void)lp;
    if (msg==WM_INITDIALOG) {
        CheckRadioButton(dlg,IDC_AXIS_X,IDC_AXIS_Z,IDC_AXIS_X+distanceAxis);
        for (i=0; i<3; ++i) EnableWindow(GetDlgItem(dlg,IDC_AXIS_X+i),!distanceEditing);
        if (distanceEditing) SetDlgItemText(dlg,IDC_DIST_LABEL,"Edit distance value (axis is fixed):");
        SendDlgItemMessage(dlg,IDC_DIST_VALUE,EM_LIMITTEXT,CAD_USERPARAM_EXPR_MAX,0);
        sprintf(text,"%ld",distanceValue); SetDlgItemText(dlg,IDC_DIST_VALUE,text);
        SetFocus(GetDlgItem(dlg,IDC_DIST_VALUE));
        SendDlgItemMessage(dlg,IDC_DIST_VALUE,EM_SETSEL,0,MAKELPARAM(0,-1));
        return FALSE;
    }
    if (msg!=WM_COMMAND) return FALSE;
    if (wp>=IDC_AXIS_X && wp<=IDC_AXIS_Z && !distanceEditing) {
        /* Propose the current separation along the newly chosen axis. */
        sprintf(text,"%ld",AxisDelta(wp-IDC_AXIS_X)); SetDlgItemText(dlg,IDC_DIST_VALUE,text);
        return TRUE;
    }
    if (wp==IDCANCEL) { EndDialog(dlg,IDCANCEL); return TRUE; }
    if (wp==IDOK) {
        GetDlgItemText(dlg,IDC_DIST_VALUE,text,sizeof(text));
        if (!ParseField(text,&distanceValue)) {
            Error(dlg,fieldError); SetFocus(GetDlgItem(dlg,IDC_DIST_VALUE)); return TRUE;
        }
        for (i=0; i<3; ++i) if (IsDlgButtonChecked(dlg,IDC_AXIS_X+i)) distanceAxis=i;
        EndDialog(dlg,IDOK); return TRUE;
    }
    return FALSE;
}
/* Stages a solver move list as ONE undoable model command. */
static CadStatus CommitMoves(void)
{
    CadStatus status;
    CadVcStatus vc;
    if (!vcMoves.count) return CAD_OK;
    status=cad_model_begin(&model);
    if (status!=CAD_OK) return status;
    vc=cad_vc_stage_moves(&model,&vcMoves);
    if (vc!=CAD_VC_OK) { cad_model_cancel(&model); vcFailure=vc; return CAD_INVALID_ARGUMENT; }
    status=cad_model_commit(&model);
    if (status!=CAD_OK) cad_model_cancel(&model);
    return status;
}
/* Record + geometry change together, or neither. */
static void FinishConstraintEdit(HWND hwnd,CadVcStatus vc,CadVcId id)
{
    CadStatus status;
    if (vc!=CAD_VC_OK) { Error(hwnd,VcMessage(vc)); return; }
    status=CommitMoves();
    if (status!=CAD_OK) { constraints=loadConstraints; ModelResult(hwnd,status); }
    else if (id) { selectedConstraint=id; }
    Changed(hwnd);
}
static void AddConstraint(HWND hwnd,int kind,int axis,long offset)
{
    CadVcId id=0;
    CadVcStatus vc;
    if (!selected || (kind!=CAD_VC_FIX && !selected2)) {
        Error(hwnd,kind==CAD_VC_FIX ? "Select a vertex first." :
            "Select two vertices: click the first, then Ctrl+click (or Shift+click) the second.");
        return;
    }
    loadConstraints=constraints;
    vc=cad_vc_add(&constraints,&model,kind,selected,kind==CAD_VC_FIX ? 0 : selected2,
        axis,offset,&vcMoves,&id);
    FinishConstraintEdit(hwnd,vc,id);
}
static void DistanceCommand(HWND hwnd)
{
    CadVConstraint c;
    distanceEditing=selectedConstraint && cad_vc_get(&constraints,selectedConstraint,&c)==CAD_VC_OK &&
        c.kind==CAD_VC_DISTANCE && !(selected && selected2);
    if (!distanceEditing && !(selected && selected2)) {
        AddConstraint(hwnd,CAD_VC_DISTANCE,0,0); return; /* explains the selection */
    }
    if (distanceEditing) { distanceAxis=c.axis; distanceValue=c.offset; }
    else { distanceAxis=0; distanceValue=AxisDelta(0); }
    if (RunDialog(hwnd,(FARPROC)DistanceDlg,IDD_DISTANCE)!=IDOK) return;
    if (!distanceEditing) { AddConstraint(hwnd,CAD_VC_DISTANCE,distanceAxis,distanceValue); return; }
    loadConstraints=constraints;
    FinishConstraintEdit(hwnd,cad_vc_set_distance(&constraints,&model,c.id,distanceValue,&vcMoves),c.id);
}
static void DeleteSelected(HWND hwnd)
{
    CadStatus status;
    if (!selected && !selectedEdge && !selectedConstraint && !selectedComponent && selectedFeature) {
        FeatureCommand(hwnd,ID_FEATURE_DELETE); return;
    }
    if (!selected && !selectedEdge && !selectedConstraint && selectedComponent) {
        if (cad_parts_remove(&parts,selectedComponent)==CAD_PARTS_OK) selectedComponent=0;
        Changed(hwnd); return;
    }
    if (!selected && !selectedEdge && selectedConstraint) {
        cad_vc_remove(&constraints,selectedConstraint); selectedConstraint=0;
        Changed(hwnd); return;
    }
    if (!selected && !selectedEdge) return;
    status=cad_model_begin(&model);
    if (status!=CAD_OK) { ModelResult(hwnd,status); return; }
    status=selected ? cad_model_delete_vertex(&model,selected) : cad_model_delete_edge(&model,selectedEdge);
    if (status==CAD_OK) status=cad_model_commit(&model);
    else cad_model_cancel(&model);
    ModelResult(hwnd,status); Changed(hwnd);
}
/* ---- Undo groups and macros ---- */
static unsigned long HistorySignature(void)
{
    unsigned long sum=0x5a17UL;
    unsigned i;
    int j;
    sum=sum*31+model.history_count; sum=sum*31+model.history_cursor;
    sum=sum*31+model.history_used;
    for (i=0; i<model.history_count; ++i) sum=sum*31+model.lengths[i];
    for (i=0; i<model.history_used; ++i) {
        sum=sum*31+model.history[i].kind; sum=sum*31+model.history[i].id;
        for (j=0; j<6; ++j) sum=sum*31+(unsigned short)model.history[i].data[j];
    }
    return sum;
}
/* Parameters follow document units; refuse (FALSE) if they cannot. */
static BOOL FollowUnits(HWND hwnd)
{
    CadUserParamError error;
    char text[200];
    if (model.units==cad_userparams_units(&userParams)) return TRUE;
    if (cad_userparams_set_units(&userParams,model.units,&error)==CAD_UP_OK) return TRUE;
    sprintf(text,"Parameters cannot use the new document units: %s\nEdit them in Tools > Parameters first.",
        error.message);
    Error(hwnd,text);
    return FALSE;
}
/* One Undo after a macro undoes all of its history_added commands; Redo
 * after that redoes them all. Otherwise one command per step. */
static void UndoRedo(HWND hwnd,BOOL undo)
{
    unsigned steps=1,done=0;
    CadStatus status=CAD_OK;
    if (groupKind==(undo ? 1 : 2) && groupSignature==HistorySignature()) steps=groupCount;
    while (done<steps && status==CAD_OK) {
        status=undo ? cad_model_undo(&model) : cad_model_redo(&model);
        if (status==CAD_OK) ++done;
    }
    if (!FollowUnits(hwnd)) {
        for (; done; --done) {
            if (undo) cad_model_redo(&model);
            else cad_model_undo(&model);
        }
        groupKind=0; return;
    }
    if (steps>1 && done==steps) { groupKind=undo ? 2 : 1; groupSignature=HistorySignature(); }
    else groupKind=0;
    if (!done) ModelResult(hwnd,status);
}
/* Macro names resolve to script SET variables first, then to user
 * parameters: value ticks (1/100 unit) rounded half away from zero. */
static int ParamLookup(void *context,const char *name,long *value)
{
    CadUserParamInfo info;
    (void)context;
    if (cad_userparams_get(&userParams,cad_userparams_find(&userParams,name),&info)!=CAD_UP_OK) return 0;
    *value=info.ticks>=0 ? (info.ticks+50)/100 : -((50-info.ticks)/100);
    return 1;
}
static void RunMacro(HWND hwnd)
{
    char path[PATH_BYTES],text[240];
    FILE *file;
    CadScriptResult result;
    path[0]=0;
    if (!PickPath(hwnd,FALSE,PICK_MACRO,path)) return;
    file=fopen(path,"rb");
    if (!file) { Error(hwnd,"Could not open the macro file."); return; }
    spare=model;
    cad_script_run_file(file,&model,&stage,ParamLookup,NULL,&result);
    fclose(file);
    if (result.status!=CAD_SCRIPT_OK) {
        if (result.line) sprintf(text,"Macro failed at line %lu: %s\n(%s",result.line,result.message,
            cad_script_status_string(result.status));
        else sprintf(text,"Macro failed: %s\n(%s",result.message,cad_script_status_string(result.status));
        if (result.status==CAD_SCRIPT_MODEL)
            sprintf(text+strlen(text),": %s",cad_model_status_string(result.model_status));
        strcat(text,")\n\nThe document is unchanged.");
        Error(hwnd,text); return;
    }
    if (!FollowUnits(hwnd)) { model=spare; Changed(hwnd); return; }
    groupKind=0;
    if (result.history_added>1 && !result.history_truncated) {
        groupKind=1; groupCount=result.history_added; groupSignature=HistorySignature();
    }
    sheetView=FALSE; Fit(); Changed(hwnd);
    if (result.history_truncated)
        MessageBox(hwnd,"The macro ran, but it produced more than the 32-command undo history.\nUndo can only partially revert it.",
            "CAD",MB_OK | MB_ICONINFORMATION);
}

/* Chrome-only repaints (defined with the hover code below). */
static void RepaintChrome(HWND hwnd);
static void Command(HWND hwnd,int command)
{
    char path[PATH_BYTES],message[200];
    CadStatus status;
    CadUserParamError paramError;
    unsigned extLength;
    int format,result;
    EndGesture(hwnd,FALSE);
    switch (command) {
    case ID_FILE_NEW:
        if (!MayDiscard(hwnd)) return;
        cad_model_init(&model); ResetDocumentState(); filePath[0]=0;
        MarkSaved(); sheetView=FALSE; Fit(); break;
    case ID_FILE_OPEN:
        path[0]=0;
        if (!MayDiscard(hwnd) || !PickPath(hwnd,FALSE,-1,path)) return;
        /* Atomic: geometry loads into spare; sections parse into staging;
         * the live document changes only when everything is valid. */
        if (cad_document_load_ex(path,&spare,&stage,extText,EXT_BYTES,&extLength)!=0) {
            Error(hwnd,"Could not read CAD document. Current document is retained."); return;
        }
        if (!ApplyExtension(extText,extLength,spare.units,message)) {
            strcat(message,"\n\nCurrent document is retained."); Error(hwnd,message); return;
        }
        model=spare; ResetSelection();
        strcpy(filePath,path); SyncConstraints(); MarkSaved(); sheetView=FALSE; Fit(); break;
    case ID_FILE_SAVE: SaveDocument(hwnd,FALSE); break;
    case ID_FILE_SAVEAS: SaveDocument(hwnd,TRUE); break;
    case ID_FILE_EXIT: SendMessage(hwnd,WM_CLOSE,0,0); return;
    case ID_FILE_DXF: case ID_FILE_OBJ:
        format=command==ID_FILE_DXF ? CAD_EXPORT_DXF : CAD_EXPORT_OBJ;
        path[0]=0;
        if (!PickPath(hwnd,TRUE,format,path)) return;
        if (!Materialize()) { Error(hwnd,"Invalid edge endpoint."); return; }
        result=CadExportWrite(path,format,exportVertices,materialVertices,
            exportEdges,materialEdges,NULL);
        if (result==CAD_EXPORT_EXISTS) Error(hwnd,"Export requires a new filename; file already exists.");
        else if (result!=CAD_EXPORT_OK) Error(hwnd,"Export failed. Use a bounded DOS 8.3 path to a writable folder.");
        else if (materialSkipped) {
            sprintf(message,"Exported, but %d feature body edges did not fit the 256-vertex / 512-edge limit and were omitted.",
                materialSkipped);
            Error(hwnd,message);
        }
        break;
    case ID_DEMO_HOUSE:
        if (!MayDiscard(hwnd)) return;
        status=cad_create_house_demo(&model); ModelResult(hwnd,status);
        if (status!=CAD_OK) return;
        ResetDocumentState(); filePath[0]=0; sheetView=FALSE;
        /* Retain saved snapshot: an imported demo must prompt to save. */
        Fit(); break;
    case ID_DEMO_747:
        if (!MayDiscard(hwnd)) return;
        status = cad_aircraft_load(&model); ModelResult(hwnd, status);
        if (status != CAD_OK) return;
        ResetDocumentState(); filePath[0] = 0; sheetView = FALSE;
        shaded = TRUE; showGrid = FALSE; yaw = 32; pitch = 24;
        Fit(); break;
    case ID_EDIT_UNDO: UndoRedo(hwnd,TRUE); break;
    case ID_EDIT_REDO: UndoRedo(hwnd,FALSE); break;
    case ID_CONSTRAIN_COINCIDENT: AddConstraint(hwnd,CAD_VC_COINCIDENT,0,0); return;
    case ID_CONSTRAIN_ALIGN_X: case ID_CONSTRAIN_ALIGN_Y: case ID_CONSTRAIN_ALIGN_Z:
        AddConstraint(hwnd,CAD_VC_ALIGN,command-ID_CONSTRAIN_ALIGN_X,0); return;
    case ID_CONSTRAIN_DISTANCE: DistanceCommand(hwnd); return;
    case ID_CONSTRAIN_FIX: AddConstraint(hwnd,CAD_VC_FIX,0,0); return;
    case ID_CONSTRAIN_DELETE:
        if (selectedConstraint) { selected=selectedEdge=0; DeleteSelected(hwnd); }
        else Error(hwnd,"Select a constraint in the model browser first.");
        return;
    case ID_TOOLS_PARAMETERS:
        RunDialog(hwnd,(FARPROC)ParametersDlg,IDD_PARAMETERS); break;
    case ID_TOOLS_RUN_MACRO: RunMacro(hwnd); return;
    case ID_INSERT_PLACE: case ID_INSERT_MOVE: case ID_INSERT_GROUND:
    case ID_INSERT_REMOVE: case ID_INSERT_RELOAD:
        ComponentCommand(hwnd,command); return;
    case ID_EDIT_DELETE: DeleteSelected(hwnd); return;
    case ID_FEATURE_SKETCH: case ID_FEATURE_RECT: case ID_FEATURE_CIRCLE: case ID_FEATURE_POLY:
    case ID_FEATURE_FINISH: case ID_FEATURE_EXTRUDE: case ID_FEATURE_REVOLVE: case ID_FEATURE_EDIT:
    case ID_FEATURE_RENAME: case ID_FEATURE_SUPPRESS: case ID_FEATURE_UP: case ID_FEATURE_DOWN:
    case ID_FEATURE_DELETE: case ID_FEATURE_UNDO:
        FeatureCommand(hwnd,command); return;
    case ID_EDIT_PROPERTIES:
        if (selectedFeature && !selected) { FeatureCommand(hwnd,ID_FEATURE_EDIT); return; }
        CreateOrProperties(hwnd,command); return;
    case ID_CREATE_LINE: case ID_CREATE_RECTANGLE: case ID_CREATE_BOX: case ID_CREATE_EXTRUDE:
        CreateOrProperties(hwnd,command); return;
    /* Navigation bar modes: left-drag orbits / pans / zooms until toggled
     * off or Esc. Right-drag always orbits. */
    case ID_VIEW_ORBIT: case ID_VIEW_PAN: case ID_VIEW_ZOOM:
        navMode=navMode==command ? 0 : command;
        if (sheetView) { sheetView=FALSE; break; }
        UpdateUI(hwnd); RepaintChrome(hwnd); return;
    case ID_VIEW_HOME: yaw=32; pitch=24; sheetView=FALSE; Fit(); break;
    case ID_VIEW_FRONT: yaw=0; pitch=0; sheetView=FALSE; Fit(); break;
    case ID_VIEW_TOP: yaw=0; pitch=64; sheetView=FALSE; Fit(); break;
    case ID_VIEW_SIDE: yaw=64; pitch=0; sheetView=FALSE; Fit(); break;
    case ID_VIEW_ISO: yaw=32; pitch=24; sheetView=FALSE; Fit(); break;
    case ID_VIEW_DRAWING: sheetView=!sheetView; break;
    case ID_VIEW_GRID: showGrid=!showGrid; break;
    case ID_VIEW_FIT: Fit(); break;
    case ID_VIEW_WIREFRAME: shaded = FALSE; sheetView = FALSE; break;
    case ID_VIEW_SHADED:
        if (AircraftTopology() || cad_fui_output()->face_count) shaded = TRUE;
        sheetView = FALSE; break;
    /* Toggles and tabs that leave the scene alone repaint chrome only. */
    case ID_TOOLS_SNAP: snap=!snap; UpdateUI(hwnd); RepaintChrome(hwnd); return;
    case ID_RIBBON_TAB0: case ID_RIBBON_TAB0+1: case ID_RIBBON_TAB0+2:
    case ID_RIBBON_TAB0+3: case ID_RIBBON_TAB0+4: case ID_RIBBON_TAB5:
        ribbonTab=command-ID_RIBBON_TAB0; UpdateUI(hwnd); RepaintChrome(hwnd); return;
    case ID_TOOLS_TEST_STATUS: uiTestMode = TRUE; break;
    case ID_TOOLS_MM: case ID_TOOLS_UNSPECIFIED:
        /* Parameter units switch first (fail-atomic) so a table that uses
         * mm/cm/in literals blocks a switch to unspecified units. */
        if (cad_userparams_set_units(&userParams,command==ID_TOOLS_MM ? CAD_UNITS_MM :
                CAD_UNITS_UNSPECIFIED,&paramError)!=CAD_UP_OK) {
            sprintf(message,"Parameters cannot switch units: %s\nEdit them in Tools > Parameters first.",
                paramError.message);
            Error(hwnd,message); break;
        }
        status=cad_model_begin(&model);
        if (status==CAD_OK) {
            status=cad_model_set_units(&model,command==ID_TOOLS_MM ? CAD_UNITS_MM : CAD_UNITS_UNSPECIFIED);
            if (status==CAD_OK) status=cad_model_commit(&model);
            else cad_model_cancel(&model);
        }
        if (status!=CAD_OK) cad_userparams_set_units(&userParams,model.units,NULL);
        ModelResult(hwnd,status); break;
    case ID_HELP_ABOUT:
        about[0]=0;
        for (result=0; aboutText[result]; ++result) strcat(about,aboutText[result]);
        MessageBox(hwnd,about,"About CAD",MB_OK); return;
    default: return;
    }
    Changed(hwnd);
}
static BOOL Key(HWND hwnd,WPARAM key)
{
    CadVertex v;
    CadStatus status;
    long step=snap ? 10 : 1;
    int i;
    if (key==VK_ESCAPE) {
        if (gesture) EndGesture(hwnd,FALSE);
        else if (navMode) { navMode=0; UpdateUI(hwnd); RepaintChrome(hwnd); }
        else {
            selected=selectedEdge=selected2=0; selectedConstraint=0; selectedComponent=0;
            selectedFeature=0; selectedProfile=-1; Changed(hwnd);
        }
        return TRUE;
    }
    if (gesture) return TRUE;
    if (key==VK_TAB) {
        if (!model.vertex_count) return TRUE;
        i=VertexIndex(selected);
        if (GetKeyState(VK_SHIFT)<0) i=i<=0 ? model.vertex_count-1 : i-1;
        else i=(i+1)%model.vertex_count;
        selected=model.vertices[i].id; selectedEdge=0; selectedConstraint=0; selectedComponent=0;
        selectedFeature=0; selectedProfile=-1;
        Changed(hwnd); return TRUE;
    }
    if (key!=VK_LEFT && key!=VK_RIGHT && key!=VK_UP && key!=VK_DOWN &&
        key!=VK_PRIOR && key!=VK_NEXT) return FALSE;
    if (cad_model_get_vertex(&model,selected,&v)!=CAD_OK) return TRUE;
    if (key==VK_LEFT) v.x-=step;
    if (key==VK_RIGHT) v.x+=step;
    if (key==VK_UP) v.z-=step;
    if (key==VK_DOWN) v.z+=step;
    if (key==VK_PRIOR) v.y+=step;
    if (key==VK_NEXT) v.y-=step;
    status=MoveSelected(v.x,v.y,v.z); ModelResult(hwnd,status); Changed(hwnd); return TRUE;
}

/* Browser subclass: Esc/Tab/PgUp/PgDn go to the editor; a click on a +/-
 * expander and Right/Left (or +/-) on a folder row expand/collapse it. */
LRESULT FAR PASCAL __export BrowserProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
{
    int row;
    if (msg==WM_KEYDOWN && (wp==VK_ESCAPE || wp==VK_TAB || wp==VK_PRIOR || wp==VK_NEXT))
        return SendMessage(GetParent(hwnd),msg,wp,lp);
    if (msg==WM_KEYDOWN && (wp==VK_RIGHT || wp==VK_LEFT || wp==VK_ADD || wp==VK_SUBTRACT)) {
        row=(int)SendMessage(hwnd,LB_GETCURSEL,0,0);
        if (row>=0 && (cad_chrome_tree_flags(row) & CHROME_TREE_FOLDER)) {
            ToggleFolder(row,wp==VK_RIGHT || wp==VK_ADD); return 0;
        }
    }
    if (msg==WM_LBUTTONDOWN || msg==WM_LBUTTONDBLCLK) {
        row=(int)SendMessage(hwnd,LB_GETTOPINDEX,0,0)+(int)HIWORD(lp)/CHROME_ROW_HEIGHT;
        if (row<(int)SendMessage(hwnd,LB_GETCOUNT,0,0) && cad_chrome_tree_expander(row,(int)LOWORD(lp))) {
            ToggleFolder(row,-1); return 0;
        }
    }
    return CallWindowProc((FARPROC)oldBrowserProc,hwnd,msg,wp,lp);
}
/* ---- Hover feedback: never a scene render ----
 * Everything below paints straight into a window DC and touches only the
 * pixels that changed: ribbon buttons, the tab row, ViewCube faces (same
 * geometry, new fill: exact), the opaque navigation bar, the Home button and
 * grips (scene pixels restored from the cache bitmap), and the status bar. */
static int BeginCanvas(HDC dc)
{
    int saved=SaveDC(dc);
    SetViewportOrg(dc,CANVAS_LEFT,RIBBON_HEIGHT);
    IntersectClipRect(dc,0,0,canvasWidth,canvasHeight);
    return saved;
}
static void EndCanvas(HDC dc,int saved)
{
    if (saved) RestoreDC(dc,saved);
    else { SetViewportOrg(dc,0,0); SelectClipRgn(dc,NULL); }
}
/* Copies scene pixels (canvas coordinates) back from the cache. */
static BOOL RestoreScene(HDC dc,const RECT *r)
{
    HDC cache=sceneValid ? cad_chrome_scene_cached() : NULL;
    if (!cache) return FALSE;
    BitBlt(dc,r->left,r->top,r->right-r->left,r->bottom-r->top,cache,r->left,r->top,SRCCOPY);
    return TRUE;
}
static BOOL NearOverlay(int index)
{
    return index>=0 && index<(int)model.vertex_count && visible[index] &&
        px[index]+GRIP>=canvasWidth-180 && py[index]-GRIP<240;
}
static void PaintHover(HWND hwnd,int oldRibbon,int oldNav,int oldVertex,int oldCube)
{
    HDC dc=GetDC(hwnd);
    RECT r;
    int saved;
    BOOL overlays;
    if (!dc) return;
    if (oldRibbon!=ribbonHot)
        cad_chrome_ribbon_items(dc,width,ribbonTab,GetMenu(hwnd),ribbonHot,ribbonPressed,oldRibbon,ribbonHot);
    if (!sheetView && canvasWidth>0 && canvasHeight>0) {
        saved=BeginCanvas(dc);
        overlays=FALSE;
        if (oldVertex!=hoverVertex) {
            /* The old grip: cached scene pixels, else its normal (opaque) grip. */
            if (oldVertex>=0 && oldVertex<(int)model.vertex_count && visible[oldVertex]) {
                r.left=px[oldVertex]-GRIP; r.top=py[oldVertex]-GRIP;
                r.right=px[oldVertex]+GRIP+1; r.bottom=py[oldVertex]+GRIP+1;
                if (!RestoreScene(dc,&r)) DrawGrip(dc,oldVertex,FALSE);
            }
            DrawGrip(dc,hoverVertex,TRUE);
            overlays=NearOverlay(oldVertex) || NearOverlay(hoverVertex);
        }
        if (overlays || oldCube!=cubeHover) DrawViewCube(dc);
        if (overlays || ((oldNav==ID_VIEW_HOME) != (navHot==ID_VIEW_HOME))) {
            cad_chrome_home_rect(canvasWidth,canvasHeight,&r);
            if (!RestoreScene(dc,&r)) FillRect(dc,&r,(HBRUSH)GetStockObject(WHITE_BRUSH));
            cad_chrome_home(dc,canvasWidth,canvasHeight,navHot==ID_VIEW_HOME);
        }
        if (overlays || oldNav!=navHot) cad_chrome_nav(dc,canvasWidth,canvasHeight,navHot,navMode);
        EndCanvas(dc,saved);
    }
    DrawStatus(dc);
    ReleaseDC(hwnd,dc);
}
/* Mode/tab/toggle changes that leave the scene alone: ribbon, navigation
 * bar and status only. */
static void RepaintChrome(HWND hwnd)
{
    HDC dc=GetDC(hwnd);
    int saved;
    if (!dc) return;
    DrawRibbon(dc);
    if (!sheetView && canvasWidth>0 && canvasHeight>0) {
        saved=BeginCanvas(dc);
        cad_chrome_nav(dc,canvasWidth,canvasHeight,navHot,navMode);
        EndCanvas(dc,saved);
    }
    DrawStatus(dc);
    ReleaseDC(hwnd,dc);
}
/* Pressed-state changes: only the two ribbon items involved. */
static void RibbonItemsNow(HWND hwnd,int a,int b)
{
    HDC dc=GetDC(hwnd);
    if (!dc) return;
    cad_chrome_ribbon_items(dc,width,ribbonTab,GetMenu(hwnd),ribbonHot,ribbonPressed,a,b);
    DrawStatus(dc);
    ReleaseDC(hwnd,dc);
}
static void SetHover(HWND hwnd,int ribbon,int nav,int vertex,int cube)
{
    int oldRibbon=ribbonHot,oldNav=navHot,oldVertex=hoverVertex,oldCube=cubeHover;
    if (ribbon==ribbonHot && nav==navHot && vertex==hoverVertex && cube==cubeHover) return;
    ribbonHot=ribbon; navHot=nav; hoverVertex=vertex; cubeHover=cube;
    PaintHover(hwnd,oldRibbon,oldNav,oldVertex,oldCube);
}
/* HitTest against the projection of the last paint: no floating point per
 * mouse move. Stale for at most one frame after a change (Changed repaints). */
static int HoverTest(int x,int y)
{
    int i,best=-1;
    long dx,dy,distance,bestDistance=PICK*PICK+1;
    for (i=0; i<(int)model.vertex_count; ++i) if (visible[i]) {
        dx=(long)x-px[i]; dy=(long)y-py[i];
        if (dx < -PICK || dx > PICK || dy < -PICK || dy > PICK) continue;
        distance=dx*dx+dy*dy;
        if (distance<bestDistance) { best=i; bestDistance=distance; }
    }
    return best;
}
static void ChromeHover(HWND hwnd,int x,int y)
{
    int ribbon=0,nav=0,vertex=-1,cube=CAD_CUBE_NONE,cx=x-CANVAS_LEFT,cy=y-RIBBON_HEIGHT;
    if (y>=0 && y<RIBBON_HEIGHT) ribbon=cad_chrome_ribbon_hit(x,y,width,ribbonTab);
    else if (!sheetView && cx>=0 && cy>=0 && cx<canvasWidth && cy<canvasHeight) {
        if (BuildViewCube()) cube=cad_cube_hit(&viewCube,cx,cy);
        if (cube==CAD_CUBE_NONE) nav=cad_chrome_nav_hit(cx,cy,canvasWidth,canvasHeight);
        /* Pre-highlight vertex grips (the 747 demo draws no grips). */
        if (cube==CAD_CUBE_NONE && !nav && !aircraftPresent && !navMode) vertex=HoverTest(cx,cy);
    }
    if (ribbon && ribbon<CHROME_HIT_TAB && !cad_chrome_enabled(GetMenu(hwnd),ribbon)) ribbon=0;
    SetHover(hwnd,ribbon,nav,vertex,cube);
}
static void RepaintRibbon(HWND hwnd)
{
    HDC dc=GetDC(hwnd);
    if (!dc) return;
    DrawRibbon(dc); DrawStatus(dc);
    ReleaseDC(hwnd,dc);
}
/* What the viewport highlights: a browser selection change re-renders the
 * scene only if one of these changed (constraint picks, folder rows: chrome). */
typedef struct ShownSelection { CadId vertex, second, edge; CadAssemblyId component; unsigned feature; } ShownSelection;
static void TakeSelection(ShownSelection *s)
{
    s->vertex=selected; s->second=selected2; s->edge=selectedEdge;
    s->component=selectedComponent; s->feature=selectedFeature;
}
static void ShowSelection(HWND hwnd,const ShownSelection *before)
{
    UpdateUI(hwnd);
    if (before->vertex!=selected || before->second!=selected2 || before->edge!=selectedEdge ||
        before->component!=selectedComponent || before->feature!=selectedFeature) InvalidateScene(hwnd);
    else RepaintChrome(hwnd);
}
/* Inventor's File button opens the classic File menu as a popup. */
static void FileMenu(HWND hwnd)
{
    POINT point;
    HMENU menu=GetMenu(hwnd);
    if (!menu || !GetSubMenu(menu,0)) return;
    UpdateUI(hwnd);
    cad_chrome_file_anchor(&point);
    ClientToScreen(hwnd,&point);
    ribbonPressed=CHROME_HIT_FILE; RibbonItemsNow(hwnd,CHROME_HIT_FILE,0);
    TrackPopupMenu(GetSubMenu(menu,0),0,point.x,point.y,0,hwnd,NULL);
    ribbonPressed=0; ribbonHot=0; RibbonItemsNow(hwnd,CHROME_HIT_FILE,0);
}
LRESULT FAR PASCAL __export MainWndProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
{
    PAINTSTRUCT ps;
    HDC dc;
    int x=(int)LOWORD(lp),y=(int)HIWORD(lp),index,row;
    DWORD data;
    CadStatus status;
    ShownSelection shown;
    switch (msg) {
    case WM_CREATE:
        mainWindow=hwnd;
        cad_chrome_init(instance);
        browser=CreateWindow("LISTBOX","",WS_CHILD | WS_VISIBLE | WS_BORDER |
            WS_VSCROLL | WS_HSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
            LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
            0,RIBBON_HEIGHT+BROWSER_TITLE,SIDE_WIDTH,100,hwnd,(HMENU)IDC_BROWSER,instance,NULL);
        if (!browser) return -1;
        SendMessage(browser,WM_SETFONT,(WPARAM)GetStockObject(ANSI_VAR_FONT),0);
        SendMessage(browser,LB_SETHORIZONTALEXTENT,420,0);
        browserThunk=MakeProcInstance((FARPROC)BrowserProc,instance);
        if (browserThunk) {
            oldBrowserProc=(WNDPROC)GetWindowLong(browser,GWL_WNDPROC);
            SetWindowLong(browser,GWL_WNDPROC,(LONG)browserThunk);
        }
        edgePen=CreatePen(PS_SOLID,1,RGB(0,0,0));
        gridPen=CreatePen(PS_SOLID,1,RGB(215,215,215));
        selectedPen=CreatePen(PS_SOLID,2,RGB(192,0,0));
        componentPen=CreatePen(PS_SOLID,1,RGB(0,0,160));
        sketchPen=CreatePen(PS_SOLID,1,RGB(0,0,255));
        activePen=CreatePen(PS_SOLID,2,RGB(0,0,255));
        dimPen=CreatePen(PS_SOLID,1,RGB(170,170,170));
        hotBrush=CreateSolidBrush(RGB(255,64,64));
        secondBrush=CreateSolidBrush(RGB(255,170,0));
        preBrush=CreateSolidBrush(RGB(0,255,255));
        captureTimer=SetTimer(hwnd,TIMER_CAPTURE,80,NULL)!=0;
        Changed(hwnd); return 0;
    case WM_SIZE:
        EndGesture(hwnd,FALSE);
        width=max(0,x); height=max(0,y);
        canvasWidth=max(0,width-CANVAS_LEFT); canvasHeight=max(0,height-STATUS_HEIGHT-RIBBON_HEIGHT);
        MoveWindow(browser,0,RIBBON_HEIGHT+BROWSER_TITLE,min(width,SIDE_WIDTH),max(0,canvasHeight-BROWSER_TITLE),TRUE);
        Fit(); InvalidateScene(hwnd); return 0;
    case WM_PAINT:
        dc=BeginPaint(hwnd,&ps); Render(dc); EndPaint(hwnd,&ps); return 0;
    case WM_ERASEBKGND: return 1;
    /* Windows 3.0 leaves wParam unused here: identify the control by CtlID. */
    case WM_MEASUREITEM:
        if (((MEASUREITEMSTRUCT FAR *)lp)->CtlID==IDC_BROWSER) {
            ((MEASUREITEMSTRUCT FAR *)lp)->itemHeight=CHROME_ROW_HEIGHT; return TRUE;
        }
        break;
    case WM_DRAWITEM:
        if (((const DRAWITEMSTRUCT FAR *)lp)->CtlID==IDC_BROWSER) {
            cad_chrome_tree_draw((const DRAWITEMSTRUCT FAR *)lp); return TRUE;
        }
        break;
    case WM_KEYDOWN: if (Key(hwnd,wp)) return 0; break;
    case WM_LBUTTONDOWN:
        if (y >= 0 && y < RIBBON_HEIGHT && !gesture) {
            /* Ribbon buttons act on release over the same button. */
            row = cad_chrome_ribbon_hit(x, y, width, ribbonTab);
            if (row == CHROME_HIT_FILE) FileMenu(hwnd);
            else if (row >= CHROME_HIT_TAB && row < CHROME_HIT_TAB + CHROME_TABS) {
                ribbonTab = row - CHROME_HIT_TAB; UpdateUI(hwnd); RepaintRibbon(hwnd);
            } else if (row && cad_chrome_enabled(GetMenu(hwnd), row)) {
                index = ribbonHot;
                ribbonPressed = ribbonHot = row; SetCapture(hwnd); RibbonItemsNow(hwnd, index, row);
            }
            return 0;
        }
        x -= CANVAS_LEFT; y -= RIBBON_HEIGHT;
        if (gesture && GetCapture()!=hwnd) EndGesture(hwnd,FALSE);
        if (gesture || sheetView) return 0;
        row = cad_chrome_nav_hit(x, y, canvasWidth, canvasHeight);
        if (row) { SetFocus(hwnd); Command(hwnd, row); return 0; }
        if (BuildViewCube()) {
            row = cad_cube_hit(&viewCube,x,y);
            if (row != CAD_CUBE_NONE && captureTimer) {
                SetFocus(hwnd); cubePending = row;
                startMouseX=x; startMouseY=y; startYaw=yaw; startPitch=pitch;
                gesture = row == CAD_CUBE_ORBIT ? G_ORBIT : G_CUBE;
                SetCapture(hwnd);
                if (GetCapture()!=hwnd) EndGesture(hwnd,FALSE);
                return 0;
            }
        }
        /* Navigation bar modes take the left button over the whole view. */
        if (navMode && captureTimer && x>=0 && y>=0 && x<canvasWidth && y<canvasHeight) {
            SetFocus(hwnd); startMouseX=x; startMouseY=y; startYaw=yaw; startPitch=pitch;
            startCenterX=centerX; startCenterY=centerY; startCenterZ=centerZ; startScale=scale;
            gesture=navMode==ID_VIEW_PAN ? G_PAN : navMode==ID_VIEW_ZOOM ? G_ZOOM : G_ORBIT;
            SetCapture(hwnd);
            if (GetCapture()!=hwnd) EndGesture(hwnd,FALSE);
            return 0;
        }
        SetFocus(hwnd); index=HitTest(x,y); selectedEdge=0; selectedConstraint=0; selectedComponent=0;
        selectedFeature=0; selectedProfile=-1;
        /* Ctrl/Shift+click adds a second vertex for two-vertex constraints. */
        if ((wp & (MK_CONTROL | MK_SHIFT)) && index>=0 && selected &&
            model.vertices[index].id!=selected) {
            selected2=model.vertices[index].id; Changed(hwnd); return 0;
        }
        selected2=0;
        selected=index<0 ? 0 : model.vertices[index].id;
        Changed(hwnd);
        if (index<0 || !captureTimer || focal<1) return 0;
        dragTarget=selected; dragStart=model.vertices[index]; dragScale=scale;
        dragCos=ICos(yaw); dragSin=ISin(yaw); dragCP=ICos(pitch); dragSP=ISin(pitch);
        dragSnap=snap; startMouseX=x; startMouseY=y;
        status=cad_model_begin(&model);
        if (status!=CAD_OK) { ModelResult(hwnd,status); return 0; }
        gesture=G_GRIP; SetCapture(hwnd);
        if (GetCapture()!=hwnd) EndGesture(hwnd,FALSE);
        return 0;
    case WM_RBUTTONDOWN:
        x -= CANVAS_LEFT; y -= RIBBON_HEIGHT;
        if (gesture && GetCapture()!=hwnd) EndGesture(hwnd,FALSE);
        if (gesture || sheetView || !captureTimer || x<0 || y<0 ||
            x>=canvasWidth || y>=canvasHeight) return 0;
        SetFocus(hwnd); startMouseX=x; startMouseY=y; startYaw=yaw; startPitch=pitch;
        gesture=G_ORBIT; SetCapture(hwnd);
        if (GetCapture()!=hwnd) EndGesture(hwnd,FALSE);
        return 0;
    case WM_MOUSEMOVE:
        if (!gesture) ChromeHover(hwnd,x,y);
        MouseMove(hwnd,x-CANVAS_LEFT,y-RIBBON_HEIGHT); return 0;
    case WM_LBUTTONUP:
        if (ribbonPressed) {
            row = ribbonPressed; ribbonPressed = 0;
            if (GetCapture()==hwnd) ReleaseCapture();
            index = cad_chrome_ribbon_hit(x, y, width, ribbonTab);
            RibbonItemsNow(hwnd, row, 0);
            if (index == row) Command(hwnd, row);
            return 0;
        }
        if (gesture == G_CUBE) {
            if (GetCapture()!=hwnd) { EndGesture(hwnd,FALSE); return 0; }
            MouseMove(hwnd,x-CANVAS_LEFT,y-RIBBON_HEIGHT);
            if (gesture == G_CUBE) { row = cubePending; EndGesture(hwnd,TRUE); NavigateCube(hwnd,row); return 0; }
        }
        if (gesture==G_ORBIT || gesture==G_PAN || gesture==G_ZOOM) {
            MouseMove(hwnd,x-CANVAS_LEFT,y-RIBBON_HEIGHT); EndGesture(hwnd,TRUE); return 0;
        }
        if (gesture==G_GRIP) { MouseMove(hwnd,x-CANVAS_LEFT,y-RIBBON_HEIGHT); EndGesture(hwnd,TRUE); } return 0;
    case WM_RBUTTONUP:
        if (gesture==G_ORBIT) { MouseMove(hwnd,x-CANVAS_LEFT,y-RIBBON_HEIGHT); EndGesture(hwnd,TRUE); } return 0;
    case WM_TIMER:
        if (gesture && GetCapture()!=hwnd) EndGesture(hwnd,FALSE);
        if (ribbonPressed && GetCapture()!=hwnd) { row=ribbonPressed; ribbonPressed=0; RibbonItemsNow(hwnd,row,0); }
        /* No WM_MOUSELEAVE in Windows 3.1: drop hover once the cursor is
         * over another window (browser, dialog, desktop). */
        if (!gesture && !ribbonPressed && (ribbonHot || navHot || hoverVertex>=0 || cubeHover!=CAD_CUBE_NONE)) {
            POINT point;
            GetCursorPos(&point);
            if (WindowFromPoint(point)!=hwnd) SetHover(hwnd,0,0,-1,CAD_CUBE_NONE);
        }
        return 0;
    case WM_CANCELMODE: case WM_KILLFOCUS:
        if (ribbonPressed) {
            row=ribbonPressed; ribbonPressed=0;
            if (GetCapture()==hwnd) ReleaseCapture();
            RibbonItemsNow(hwnd,row,0);
        }
        EndGesture(hwnd,FALSE); return 0;
    case WM_ACTIVATE:
        if (wp==WA_INACTIVE) EndGesture(hwnd,FALSE); break;
    case WM_ACTIVATEAPP:
        if (!wp) EndGesture(hwnd,FALSE); break;
    case WM_INITMENU:
        EndGesture(hwnd,FALSE); UpdateUI(hwnd); return 0;
    case WM_COMMAND:
        if (wp==IDC_BROWSER) {
            EndGesture(hwnd,FALSE);
            TakeSelection(&shown);
            if (HIWORD(lp)==LBN_SELCHANGE || HIWORD(lp)==LBN_DBLCLK) {
                row=(int)SendMessage(browser,LB_GETCURSEL,0,0);
                if (row<0) return 0;
                data=(DWORD)SendMessage(browser,LB_GETITEMDATA,row,0);
                /* Folders and origin entries: double-click expands/collapses. */
                if (data&BROWSER_HEADER) {
                    if (HIWORD(lp)==LBN_DBLCLK && (cad_chrome_tree_flags(row)&CHROME_TREE_FOLDER))
                        ToggleFolder(row,-1);
                    return 0;
                }
                if ((data&BROWSER_FEATURE) && !(data&(BROWSER_COMPONENT|BROWSER_CONSTRAINT))) {
                    selected=selectedEdge=selected2=0; selectedConstraint=0; selectedComponent=0;
                    selectedFeature=(unsigned)(data&0xFFFFUL);
                    selectedProfile=(int)((data>>16)&0xFFUL)-1;
                    ShowSelection(hwnd,&shown);
                    if (HIWORD(lp)==LBN_DBLCLK) Command(hwnd,ID_FEATURE_EDIT);
                    return 0;
                }
                selectedFeature=0; selectedProfile=-1;
                if (data&BROWSER_COMPONENT) {
                    selected=selectedEdge=selected2=0; selectedConstraint=0;
                    selectedComponent=(CadAssemblyId)(data&~BROWSER_COMPONENT);
                    ShowSelection(hwnd,&shown);
                    if (HIWORD(lp)==LBN_DBLCLK) Command(hwnd,ID_INSERT_MOVE);
                    return 0;
                }
                selectedComponent=0;
                if (data&BROWSER_CONSTRAINT) {
                    selected=selectedEdge=selected2=0;
                    selectedConstraint=(CadVcId)(data&~BROWSER_CONSTRAINT);
                    ShowSelection(hwnd,&shown);
                    if (HIWORD(lp)==LBN_DBLCLK) Command(hwnd,ID_CONSTRAIN_DISTANCE);
                    return 0;
                }
                selectedConstraint=0;
                if (!(data&BROWSER_EDGE) && selected && (CadId)data!=selected &&
                    (GetKeyState(VK_CONTROL)<0 || GetKeyState(VK_SHIFT)<0)) {
                    selected2=(CadId)data; SyncBrowser();
                    ShowSelection(hwnd,&shown); return 0;
                }
                selected2=0;
                selected=(data&BROWSER_EDGE) ? 0 : (CadId)data;
                selectedEdge=(data&BROWSER_EDGE) ? (CadId)data : 0;
                ShowSelection(hwnd,&shown);
                if (HIWORD(lp)==LBN_DBLCLK && selected) Command(hwnd,ID_EDIT_PROPERTIES);
            }
            return 0;
        }
        Command(hwnd,wp); return 0;
    case WM_CLOSE:
        EndGesture(hwnd,FALSE); if (MayDiscard(hwnd)) DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        EndGesture(hwnd,FALSE);
        if (captureTimer) KillTimer(hwnd,TIMER_CAPTURE);
        if (browserThunk) {
            if (IsWindow(browser)) SetWindowLong(browser,GWL_WNDPROC,(LONG)oldBrowserProc);
            FreeProcInstance(browserThunk); browserThunk=NULL;
        }
        if (edgePen) DeleteObject(edgePen);
        if (gridPen) DeleteObject(gridPen);
        if (selectedPen) DeleteObject(selectedPen);
        if (componentPen) DeleteObject(componentPen);
        if (sketchPen) DeleteObject(sketchPen);
        if (activePen) DeleteObject(activePen);
        if (dimPen) DeleteObject(dimPen);
        cad_fui_free_brushes();
        cad_chrome_free();
        if (preBrush) DeleteObject(preBrush);
        if (hotBrush) DeleteObject(hotBrush);
        if (secondBrush) DeleteObject(secondBrush);
        for (index = 0; index < CAD_AIRCRAFT_MATERIAL_COUNT * 8; ++index)
            if (aircraftBrushes[index]) { DeleteObject(aircraftBrushes[index]); aircraftBrushes[index] = NULL; }
        PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hwnd,msg,wp,lp);
}
int PASCAL WinMain(HINSTANCE hInst,HINSTANCE hPrev,LPSTR commandLine,int show)
{
    WNDCLASS wc;
    HWND hwnd;
    MSG msg;
    HACCEL keys;
    int result;
    uiTestMode = strstr(commandLine, "/UITEST") != NULL;
    instance=hInst;
    cad_model_init(&model); cad_model_init(&stage); cad_model_init(&spare);
    cad_fui_init(&userParams); ResetDocumentState(); MarkSaved();
    if (!hPrev) {
        memset(&wc,0,sizeof(wc));
        wc.style=CS_HREDRAW | CS_VREDRAW; wc.lpfnWndProc=MainWndProc;
        wc.hInstance=hInst; wc.hIcon=LoadIcon(NULL,IDI_APPLICATION);
        wc.hCursor=LoadCursor(NULL,IDC_CROSS); wc.hbrBackground=NULL;
        wc.lpszMenuName=MAKEINTRESOURCE(IDR_CAD_MENU); wc.lpszClassName="GenericCAD";
        if (!RegisterClass(&wc)) return 1;
    }
    hwnd=CreateWindow("GenericCAD","CAD - Untitled",WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,760,520,NULL,NULL,hInst,NULL);
    if (!hwnd) return 1;
    keys=LoadAccelerators(hInst,MAKEINTRESOURCE(IDR_CAD_KEYS));
    ShowWindow(hwnd,show); UpdateWindow(hwnd);
    while ((result=GetMessage(&msg,NULL,0,0))>0) {
        if (!keys || !TranslateAccelerator(hwnd,keys,&msg)) {
            TranslateMessage(&msg); DispatchMessage(&msg);
        }
    }
    return result<0 ? 1 : msg.wParam;
}
