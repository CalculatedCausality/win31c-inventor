/* CAD.EXE: generic Win16 vertex/edge editor. Compile EVERY linked module with
 * -ml -zt=1: the two models below must live in separate far data segments.
 * No feature tree, curved-solid kernel, or automatic sample document. */
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
#include "cad_ui_ids.h"

#define SIDE_WIDTH 154
#define RIBBON_HEIGHT 64
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
#define TIMER_CAPTURE 1

static CadModel model;
static CadModel stage;
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
static HPEN edgePen, gridPen, selectedPen;
static HBRUSH hotBrush;
static char filePath[PATH_BYTES];
static CadId selected, selectedEdge;
static int width, height, canvasWidth, canvasHeight;
static int yaw = 32, pitch = 24;
static BOOL sheetView, showGrid = TRUE, snap;
static double scale = 1.0, centerX, centerY, centerZ;
static long focal = 1; /* Orthographic pixel scale * 1024; always positive. */
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
static HBRUSH aircraftBrushes[CAD_AIRCRAFT_MATERIAL_COUNT * 8];
static CadMeshViewVertex meshVertices[CAD_AIRCRAFT_MAX_VERTICES];
static CadMeshViewFace meshFaces[CAD_AIRCRAFT_MAX_TRIANGLES];

static const int quarter[65] = {
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
static void MarkSaved(void)
{
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
    return FALSE;
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
static void ModelResult(HWND hwnd, CadStatus status)
{
    if (status != CAD_OK) Error(hwnd, cad_model_status_string(status));
}
static void SyncBrowser(void)
{
    int i, row;
    char text[100];
    CadVertex *v;
    CadEdge *e;
    if (!browser) return;
    SendMessage(browser, WM_SETREDRAW, FALSE, 0);
    SendMessage(browser, LB_RESETCONTENT, 0, 0);
    for (i=0; i<(int)model.vertex_count; ++i) {
        v = &model.vertices[i];
        sprintf(text, "V%u  (%ld,%ld,%ld)", v->id, v->x, v->y, v->z);
        row = (int)SendMessage(browser, LB_ADDSTRING, 0, (LPARAM)(LPSTR)text);
        if (row >= 0) {
            SendMessage(browser, LB_SETITEMDATA, row, (LPARAM)(DWORD)v->id);
            if (selected == v->id) SendMessage(browser, LB_SETCURSEL, row, 0);
        }
    }
    for (i=0; i<(int)model.edge_count; ++i) {
        e = &model.edges[i];
        sprintf(text, "E%u  V%u - V%u", e->id, e->a, e->b);
        row = (int)SendMessage(browser, LB_ADDSTRING, 0, (LPARAM)(LPSTR)text);
        if (row >= 0) {
            SendMessage(browser, LB_SETITEMDATA, row, (LPARAM)(0x10000UL | e->id));
            if (selectedEdge == e->id) SendMessage(browser, LB_SETCURSEL, row, 0);
        }
    }
    SendMessage(browser, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(browser, NULL, TRUE);
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
    if (selected && VertexIndex(selected) < 0) selected = 0;
    if (selectedEdge && cad_model_get_edge(&model, selectedEdge, &e) != CAD_OK)
        selectedEdge = 0;
    aircraftPresent = AircraftTopology();
    if (!aircraftPresent) shaded = FALSE;
    sprintf(title, "CAD - %s%s%s", *filePath ? filePath : aircraftPresent ? "Boeing 747-400 demo" : "Untitled", Dirty() ? " *" : "", uiTestMode ? " [test]" : "");
    SetWindowText(hwnd, title);
    EnableMenuItem(menu, ID_EDIT_UNDO, MF_BYCOMMAND |
        (model.history_cursor ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, ID_EDIT_REDO, MF_BYCOMMAND |
        (model.history_cursor < model.history_count ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, ID_EDIT_DELETE, MF_BYCOMMAND |
        (selected || selectedEdge ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, ID_EDIT_PROPERTIES, MF_BYCOMMAND |
        (selected ? MF_ENABLED : MF_GRAYED));
    CheckMenuItem(menu, ID_VIEW_DRAWING, MF_BYCOMMAND | (sheetView ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, ID_VIEW_GRID, MF_BYCOMMAND | (showGrid ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, ID_VIEW_WIREFRAME, MF_BYCOMMAND | (!shaded ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, ID_VIEW_SHADED, MF_BYCOMMAND | (shaded ? MF_CHECKED : MF_UNCHECKED));
    EnableMenuItem(menu, ID_VIEW_SHADED, MF_BYCOMMAND | (aircraftPresent ? MF_ENABLED : MF_GRAYED));
    CheckMenuItem(menu, ID_TOOLS_SNAP, MF_BYCOMMAND | (snap ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, ID_TOOLS_UNSPECIFIED, MF_BYCOMMAND |
        (model.units == CAD_UNITS_UNSPECIFIED ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, ID_TOOLS_MM, MF_BYCOMMAND |
        (model.units == CAD_UNITS_MM ? MF_CHECKED : MF_UNCHECKED));
}
static void Changed(HWND hwnd)
{
    UpdateUI(hwnd);
    SyncBrowser();
    InvalidateRect(hwnd, NULL, FALSE);
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
    }
    if (old && GetCapture() == hwnd) ReleaseCapture();
    if (old) Changed(hwnd);
    ModelResult(hwnd, status);
}
static CadStatus MoveSelected(long x, long y, long z)
{
    CadStatus status;
    if (!selected) return CAD_NOT_FOUND;
    status = cad_model_begin(&model);
    if (status != CAD_OK) return status;
    status = cad_model_move_vertex(&model, selected, x,y,z);
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
static void Fit(void)
{
    int i;
    double minX,maxX,minY,maxY,minZ,maxZ,u,v,extent;
    if (!model.vertex_count) {
        centerX=centerY=centerZ=0;
        scale = canvasWidth>0 && canvasHeight>0 ?
            (double)min(canvasWidth,canvasHeight)/250.0 : 1.0;
    } else {
        minX=maxX=model.vertices[0].x; minY=maxY=model.vertices[0].y;
        minZ=maxZ=model.vertices[0].z;
        for (i=1; i<(int)model.vertex_count; ++i) {
            if (model.vertices[i].x<minX) minX=model.vertices[i].x;
            if (model.vertices[i].x>maxX) maxX=model.vertices[i].x;
            if (model.vertices[i].y<minY) minY=model.vertices[i].y;
            if (model.vertices[i].y>maxY) maxY=model.vertices[i].y;
            if (model.vertices[i].z<minZ) minZ=model.vertices[i].z;
            if (model.vertices[i].z>maxZ) maxZ=model.vertices[i].z;
        }
        centerX=(minX+maxX)/2; centerY=(minY+maxY)/2; centerZ=(minZ+maxZ)/2;
        extent=1;
        for (i=0; i<(int)model.vertex_count; ++i) {
            ToCamera(model.vertices[i].x,model.vertices[i].y,model.vertices[i].z,&u,&v);
            if (u<0) u=-u;
            if (v<0) v=-v;
            u=2*u/(canvasWidth>40 ? canvasWidth-40 : 1);
            v=2*v/(canvasHeight>40 ? canvasHeight-40 : 1);
            if (i==0 || u>extent) extent=u;
            if (v>extent) extent=v;
        }
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
    if (!model.vertex_count) {
        TextOut(dc,12,12,"Empty drawing - create geometry first",37); return;
    }
    if (!Materialize()) { TextOut(dc,12,12,"Invalid edge endpoint",21); return; }
    drawing.vertices=drawingPoints; drawing.vertex_count=model.vertex_count;
    drawing.edges=drawingEdges; drawing.edge_count=model.edge_count;
    sheet.width=2600; sheet.height=2200; sheet.margin=80; sheet.gap=120;
    sheet.scale.numerator=1; sheet.scale.denominator=1;
    c.dc=dc; c.scale=(double)canvasWidth/2600.0;
    if ((double)canvasHeight/2200.0<c.scale) c.scale=(double)canvasHeight/2200.0;
    c.x=(canvasWidth-2600*c.scale)/2; c.y=(canvasHeight-2200*c.scale)/2;
    sink.context=&c; sink.line=SheetLine; sink.text=SheetText;
    status=CadDrawingRender(&drawing,&sheet,&sink,NULL);
    if (status!=CAD_DRAWING_OK) TextOut(dc,12,12,"Drawing sheet could not be laid out",34);
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
    memset(&camera, 0, sizeof(camera));
    context.flip = FALSE;
    camera.yaw = yaw; camera.pitch = pitch > 128 ? pitch - 256 : pitch;
    /* The wire editor permits a full orbit; invert an upside-down view into
     * the equivalent supported pitch by adding a half-turn to yaw. */
    if (camera.pitch > 64) { camera.pitch = 128 - camera.pitch; camera.yaw = (yaw + 128) & 255; context.flip = TRUE; }
    if (camera.pitch < -64) { camera.pitch = -128 - camera.pitch; camera.yaw = (yaw + 128) & 255; context.flip = TRUE; }
    camera.width = canvasWidth; camera.height = canvasHeight;
    camera.cameraDistance = 900; camera.focal = 400; camera.backfaceCull = 1;
    ortho.centerX = centerX; ortho.centerY = centerY; ortho.centerZ = centerZ;
    ortho.pixelsPerUnit = scale;
    context.dc = dc; sink.context = &context; sink.triangle = AircraftTriangle;
    oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    oldBrush = SelectObject(dc, GetStockObject(LTGRAY_BRUSH));
    result = cad_meshview_render_orthographic(meshVertices, asset->vertex_count,
        meshFaces, asset->triangle_count, &camera, &sink, &ortho);
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen);
    return result == CAD_MESHVIEW_OK;
}

typedef struct RibbonButton { const char *label; int command, span; } RibbonButton;
static const RibbonButton modelButtons[] = {
    {"Box",ID_CREATE_BOX,46}, {"Extrude",ID_CREATE_EXTRUDE,62},
    {"Properties",ID_EDIT_PROPERTIES,78}, {"Undo",ID_EDIT_UNDO,48},
    {"Redo",ID_EDIT_REDO,48}, {"Wireframe",ID_VIEW_WIREFRAME,76},
    {"Shaded",ID_VIEW_SHADED,60}, {"Fit",ID_VIEW_FIT,38}
};
static const RibbonButton sketchButtons[] = {
    {"Line",ID_CREATE_LINE,44}, {"Rectangle",ID_CREATE_RECTANGLE,76},
    {"Snap",ID_TOOLS_SNAP,48}, {"Properties",ID_EDIT_PROPERTIES,78},
    {"Undo",ID_EDIT_UNDO,48}, {"Redo",ID_EDIT_REDO,48}, {"Fit",ID_VIEW_FIT,38}
};
static const RibbonButton viewButtons[] = {
    {"Front",ID_VIEW_FRONT,48}, {"Top",ID_VIEW_TOP,40},
    {"Side",ID_VIEW_SIDE,42}, {"Isometric",ID_VIEW_ISO,76},
    {"Fit",ID_VIEW_FIT,38}, {"Orbit",ID_VIEW_ORBIT,46},
    {"Wireframe",ID_VIEW_WIREFRAME,76}, {"Shaded",ID_VIEW_SHADED,60}, {"Grid",ID_VIEW_GRID,42}
};

static const RibbonButton *RibbonItems(int *count)
{
    if (ribbonTab == 1) { *count = sizeof(sketchButtons)/sizeof(sketchButtons[0]); return sketchButtons; }
    if (ribbonTab == 2) { *count = sizeof(viewButtons)/sizeof(viewButtons[0]); return viewButtons; }
    *count = sizeof(modelButtons)/sizeof(modelButtons[0]); return modelButtons;
}

static int RibbonHit(int x, int y)
{
    const RibbonButton *items;
    int i, count, left = 8;
    if (y < 0 || y >= RIBBON_HEIGHT || x < 0 || x >= width) return 0;
    if (y < 23) {
        if (x >= 8 && x < 102) return 10000;
        if (x >= 108 && x < 184) return 10001;
        if (x >= 190 && x < 246) return 10002;
        return 0;
    }
    items = RibbonItems(&count);
    for (i = 0; i < count; ++i) {
        if (x >= left && x < left + items[i].span && y >= 27 && y < 56) return items[i].command;
        left += items[i].span + 5;
    }
    return 0;
}

static void DrawRibbon(HDC dc)
{
    static const char *tabs[] = {"3D Model", "Sketch", "View"};
    static const int tabLeft[] = {8, 108, 190}, tabRight[] = {102, 184, 246};
    const RibbonButton *items;
    RECT r;
    HBRUSH background, active;
    HGDIOBJ oldFont;
    int i, count, left = 8;
    background = CreateSolidBrush(RGB(225,232,240));
    active = CreateSolidBrush(RGB(255,255,255));
    oldFont = SelectObject(dc, GetStockObject(ANSI_VAR_FONT));
    r.left = r.top = 0; r.right = width; r.bottom = RIBBON_HEIGHT;
    FillRect(dc, &r, background ? background : (HBRUSH)GetStockObject(LTGRAY_BRUSH));
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(0,0,0));
    for (i = 0; i < 3; ++i) {
        r.left = tabLeft[i]; r.right = tabRight[i]; r.top = 2; r.bottom = 23;
        if (ribbonTab == i) FillRect(dc, &r, active ? active : (HBRUSH)GetStockObject(WHITE_BRUSH));
        DrawText(dc, tabs[i], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    items = RibbonItems(&count);
    for (i = 0; i < count; ++i) {
        r.left = left; r.right = left + items[i].span; r.top = 27; r.bottom = 56;
        FillRect(dc, &r, active ? active : (HBRUSH)GetStockObject(WHITE_BRUSH));
        FrameRect(dc, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
        DrawText(dc, items[i].label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        left += items[i].span + 5;
    }
    SelectObject(dc, oldFont);
    if (background) DeleteObject(background);
    if (active) DeleteObject(active);
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

static void Render(HDC dc)
{
    RECT r;
    int i,a,b,oldDC;
    BOOL filled = FALSE;
    HGDIOBJ oldPen,oldBrush;
    CadVertex *v,*w;
    char text[160];
    DWORD telemetry, checksum;
    COLORREF marker[4];
    r.left=0; r.top=0; r.right=width; r.bottom=height;
    FillRect(dc,&r,(HBRUSH)GetStockObject(WHITE_BRUSH));
    DrawRibbon(dc);
    oldDC=SaveDC(dc);
    SetViewportOrg(dc, CANVAS_LEFT, RIBBON_HEIGHT);
    IntersectClipRect(dc,0,0,canvasWidth,canvasHeight);
    SetBkMode(dc,TRANSPARENT);
    oldPen=SelectObject(dc,edgePen ? (HGDIOBJ)edgePen : GetStockObject(BLACK_PEN));
    if (sheetView) DrawSheet(dc);
    else {
        if (showGrid) {
            SelectObject(dc,gridPen ? (HGDIOBJ)gridPen : GetStockObject(BLACK_PEN));
            for (i=-400; i<=400; i+=50) {
                WorldLine(dc,i,0,-400,i,0,400);
                WorldLine(dc,-400,0,i,400,0,i);
            }
            SelectObject(dc,edgePen ? (HGDIOBJ)edgePen : GetStockObject(BLACK_PEN));
        }
        if (shaded && aircraftPresent) filled = DrawAircraft(dc);
        for (i=0; i<(int)model.edge_count; ++i) {
            if (filled && model.edges[i].id <= 396 && model.edges[i].id != selectedEdge) continue;
            a=VertexIndex(model.edges[i].a); b=VertexIndex(model.edges[i].b);
            if (a<0 || b<0) continue;
            v=&model.vertices[a]; w=&model.vertices[b];
            if (selectedEdge==model.edges[i].id && selectedPen) SelectObject(dc,selectedPen);
            WorldLine(dc,v->x,v->y,v->z,w->x,w->y,w->z);
            SelectObject(dc,edgePen ? (HGDIOBJ)edgePen : GetStockObject(BLACK_PEN));
        }
        Project();
        oldBrush=SelectObject(dc,GetStockObject(WHITE_BRUSH));
        for (i=0; i<(int)model.vertex_count; ++i) if (visible[i]) {
            if (aircraftPresent && model.vertices[i].id != selected) continue;
            SelectObject(dc,model.vertices[i].id==selected && hotBrush ?
                (HGDIOBJ)hotBrush : GetStockObject(WHITE_BRUSH));
            Rectangle(dc,px[i]-GRIP,py[i]-GRIP,px[i]+GRIP+1,py[i]+GRIP+1);
        }
        SelectObject(dc,oldBrush);
        if (!model.vertex_count) TextOut(dc,12,12,"Empty document - use Create",27);
        DrawViewCube(dc);
    }
    SelectObject(dc,oldPen);
    if (oldDC) RestoreDC(dc,oldDC);
    else { SetViewportOrg(dc,0,0); SelectClipRgn(dc,NULL); }
    r.left=0; r.top=RIBBON_HEIGHT; r.right=min(SIDE_WIDTH,width); r.bottom=RIBBON_HEIGHT+22;
    FillRect(dc,&r,(HBRUSH)GetStockObject(LTGRAY_BRUSH));
    TextOut(dc,4,RIBBON_HEIGHT+4,"Model browser",13);
    r.left=0; r.top=height-STATUS_HEIGHT; r.right=width; r.bottom=height;
    FillRect(dc,&r,(HBRUSH)GetStockObject(LTGRAY_BRUSH));
    sprintf(text,"%u vertices / %u edges | %s | %s%s | V%u E%u",
        model.vertex_count,model.edge_count,
        model.units==CAD_UNITS_MM ? "mm" : model.units==CAD_UNITS_INCHES ? "inches" : "unspecified",
        sheetView ? "drawing" : filled ? "shaded" : "wireframe",snap ? " / snap 10" : "",selected,selectedEdge);
    TextOut(dc,4,height-STATUS_HEIGHT+5,text,strlen(text));
    /* Debug-only display telemetry follows the real rendered state. It does
     * not drive commands or modify the document. Normal launches omit it. */
    if (uiTestMode && width > 90 && height > 4) {
        telemetry = (DWORD)model.vertex_count | ((DWORD)model.edge_count << 9) |
            ((DWORD)model.units << 19) | ((DWORD)(filled ? 1 : 0) << 21) |
            ((DWORD)(Dirty() ? 1 : 0) << 22);
        checksum = (telemetry ^ (telemetry >> 8) ^ (telemetry >> 16) ^ 0xa7UL) & 255;
        telemetry |= checksum << 24;
        marker[0] = RGB(255,0,255); marker[1] = RGB(255,255,0);
        marker[2] = RGB(0,255,255); marker[3] = RGB(255,0,255);
        for (i = 0; i < 4; ++i) {
            SetPixel(dc, 6 + i * 2, height-STATUS_HEIGHT + 1, marker[i]);
            SetPixel(dc, 6 + i * 2, height-STATUS_HEIGHT + 2, marker[i]);
        }
        for (i = 0; i < 32; ++i) {
            COLORREF color = telemetry & (1UL << i) ? RGB(255,255,255) : RGB(0,0,0);
            SetPixel(dc, 14 + i * 2, height-STATUS_HEIGHT + 1, color);
            SetPixel(dc, 14 + i * 2, height-STATUS_HEIGHT + 2, color);
        }
    }
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
    if (!gesture) {
        int hit = !sheetView && BuildViewCube() ? cad_cube_hit(&viewCube,x,y) : CAD_CUBE_NONE;
        if (cubeHover != hit) { cubeHover = hit; InvalidateRect(hwnd,NULL,FALSE); }
        return;
    }
    if (GetCapture()!=hwnd) { EndGesture(hwnd,FALSE); return; }
    if (gesture == G_CUBE) {
        if (abs(x-startMouseX)+abs(y-startMouseY) <= 3) return;
        gesture = G_ORBIT;
    }
    if (gesture==G_ORBIT) {
        yaw=(int)((startYaw+((long)x-startMouseX)/2)&255);
        pitch=(int)((startPitch+((long)y-startMouseY)/2)&255);
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
        if (status==CAD_OK) status=cad_model_move_vertex(&model,dragTarget,
            x==startMouseX && y==startMouseY ? dragStart.x : BoundedCoordinate(dragStart.x+dx,dragSnap && dx!=0),
            x==startMouseX && y==startMouseY ? dragStart.y : BoundedCoordinate(dragStart.y+dy,dragSnap && dy!=0),
            x==startMouseX && y==startMouseY ? dragStart.z : BoundedCoordinate(dragStart.z+dz,dragSnap && dz!=0));
        if (status!=CAD_OK) { EndGesture(hwnd,FALSE); ModelResult(hwnd,status); return; }
    }
    InvalidateRect(hwnd,NULL,FALSE);
}

static BOOL PickPath(HWND hwnd, BOOL save, int format, char *path)
{
    OPENFILENAME ofn;
    BOOL result;
    static char cadFilter[]="CAD documents (*.CAD)\0*.CAD\0All files (*.*)\0*.*\0\0";
    static char dxfFilter[]="DXF wireframe (*.DXF)\0*.DXF\0\0";
    static char objFilter[]="OBJ wireframe (*.OBJ)\0*.OBJ\0\0";
    memset(&ofn,0,sizeof(ofn));
    ofn.lStructSize=sizeof(ofn); ofn.hwndOwner=hwnd; ofn.hInstance=instance;
    ofn.lpstrFilter=format<0 ? cadFilter : format==CAD_EXPORT_DXF ? dxfFilter : objFilter;
    ofn.lpstrFile=path; ofn.nMaxFile=PATH_BYTES;
    ofn.lpstrDefExt=format<0 ? "CAD" : format==CAD_EXPORT_DXF ? "DXF" : "OBJ";
    ofn.Flags=OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!save) ofn.Flags|=OFN_FILEMUSTEXIST;
    else if (format<0) ofn.Flags|=OFN_OVERWRITEPROMPT;
    result=save ? GetSaveFileName(&ofn) : GetOpenFileName(&ofn);
    if (!result && CommDlgExtendedError()) Error(hwnd,"File dialog failed (path must fit 127 bytes).");
    return result;
}
static BOOL SaveDocument(HWND hwnd,BOOL saveAs)
{
    char path[PATH_BYTES];
    strcpy(path,filePath);
    if ((saveAs || !*path) && !PickPath(hwnd,TRUE,-1,path)) return FALSE;
    if (cad_document_save(path,&model)!=0) {
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
    char text[32];
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
            SendDlgItemMessage(dlg,IDC_VALUE1+i,EM_LIMITTEXT,16,0);
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
            if (!ParseCoordinate(text,&values[i]) ||
                (i>=3 && dialogCommand!=ID_CREATE_LINE && values[i]<=0)) {
                Error(dlg,"Enter an integer from -400 to 400; dimensions must be 1..400.");
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
        SendDlgItemMessage(dlg,IDC_HEIGHT,EM_LIMITTEXT,16,0);
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
        if (!ParseCoordinate(text,&value) || value<1) {
            Error(dlg,"Extrusion height must be an integer 1..400 along +Z."); return TRUE;
        }
        extrusionHeight=(int)value; EndDialog(dlg,IDOK); return TRUE;
    }
    return FALSE;
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
static void DeleteSelected(HWND hwnd)
{
    CadStatus status;
    if (!selected && !selectedEdge) return;
    status=cad_model_begin(&model);
    if (status!=CAD_OK) { ModelResult(hwnd,status); return; }
    status=selected ? cad_model_delete_vertex(&model,selected) : cad_model_delete_edge(&model,selectedEdge);
    if (status==CAD_OK) status=cad_model_commit(&model);
    else cad_model_cancel(&model);
    ModelResult(hwnd,status); Changed(hwnd);
}
static void Command(HWND hwnd,int command)
{
    char path[PATH_BYTES];
    CadStatus status;
    int format,result;
    EndGesture(hwnd,FALSE);
    switch (command) {
    case ID_FILE_NEW:
        if (!MayDiscard(hwnd)) return;
        cad_model_init(&model); selected=selectedEdge=0; filePath[0]=0;
        MarkSaved(); sheetView=FALSE; Fit(); break;
    case ID_FILE_OPEN:
        path[0]=0;
        if (!MayDiscard(hwnd) || !PickPath(hwnd,FALSE,-1,path)) return;
        if (cad_document_load(path,&model,&stage)!=0) {
            Error(hwnd,"Could not read CAD document. Current document is retained."); return;
        }
        strcpy(filePath,path); selected=selectedEdge=0; MarkSaved(); sheetView=FALSE; Fit(); break;
    case ID_FILE_SAVE: SaveDocument(hwnd,FALSE); break;
    case ID_FILE_SAVEAS: SaveDocument(hwnd,TRUE); break;
    case ID_FILE_EXIT: SendMessage(hwnd,WM_CLOSE,0,0); return;
    case ID_FILE_DXF: case ID_FILE_OBJ:
        format=command==ID_FILE_DXF ? CAD_EXPORT_DXF : CAD_EXPORT_OBJ;
        path[0]=0;
        if (!PickPath(hwnd,TRUE,format,path)) return;
        if (!Materialize()) { Error(hwnd,"Invalid edge endpoint."); return; }
        result=CadExportWrite(path,format,exportVertices,model.vertex_count,
            exportEdges,model.edge_count,NULL);
        if (result==CAD_EXPORT_EXISTS) Error(hwnd,"Export requires a new filename; file already exists.");
        else if (result!=CAD_EXPORT_OK) Error(hwnd,"Export failed. Use a bounded DOS 8.3 path to a writable folder.");
        break;
    case ID_DEMO_HOUSE:
        if (!MayDiscard(hwnd)) return;
        status=cad_create_house_demo(&model); ModelResult(hwnd,status);
        if (status!=CAD_OK) return;
        filePath[0]=0; selected=selectedEdge=0; sheetView=FALSE;
        /* Retain saved snapshot: an imported demo must prompt to save. */
        Fit(); break;
    case ID_DEMO_747:
        if (!MayDiscard(hwnd)) return;
        status = cad_aircraft_load(&model); ModelResult(hwnd, status);
        if (status != CAD_OK) return;
        filePath[0] = 0; selected = selectedEdge = 0; sheetView = FALSE;
        shaded = TRUE; showGrid = FALSE; yaw = 32; pitch = 24;
        Fit(); break;
    case ID_EDIT_UNDO: ModelResult(hwnd,cad_model_undo(&model)); break;
    case ID_EDIT_REDO: ModelResult(hwnd,cad_model_redo(&model)); break;
    case ID_EDIT_DELETE: DeleteSelected(hwnd); return;
    case ID_EDIT_PROPERTIES:
    case ID_CREATE_LINE: case ID_CREATE_RECTANGLE: case ID_CREATE_BOX: case ID_CREATE_EXTRUDE:
        CreateOrProperties(hwnd,command); return;
    case ID_VIEW_ORBIT: sheetView=FALSE; break;
    case ID_VIEW_FRONT: yaw=0; pitch=0; sheetView=FALSE; Fit(); break;
    case ID_VIEW_TOP: yaw=0; pitch=64; sheetView=FALSE; Fit(); break;
    case ID_VIEW_SIDE: yaw=64; pitch=0; sheetView=FALSE; Fit(); break;
    case ID_VIEW_ISO: yaw=32; pitch=24; sheetView=FALSE; Fit(); break;
    case ID_VIEW_DRAWING: sheetView=!sheetView; break;
    case ID_VIEW_GRID: showGrid=!showGrid; break;
    case ID_VIEW_FIT: Fit(); break;
    case ID_VIEW_WIREFRAME: shaded = FALSE; sheetView = FALSE; break;
    case ID_VIEW_SHADED: if (AircraftTopology()) shaded = TRUE; sheetView = FALSE; break;
    case ID_TOOLS_SNAP: snap=!snap; break;
    case ID_TOOLS_TEST_STATUS: uiTestMode = TRUE; break;
    case ID_TOOLS_MM: case ID_TOOLS_UNSPECIFIED:
        status=cad_model_begin(&model);
        if (status==CAD_OK) {
            status=cad_model_set_units(&model,command==ID_TOOLS_MM ? CAD_UNITS_MM : CAD_UNITS_UNSPECIFIED);
            if (status==CAD_OK) status=cad_model_commit(&model);
            else cad_model_cancel(&model);
        }
        ModelResult(hwnd,status); break;
    case ID_HELP_ABOUT:
        MessageBox(hwnd,"Wireframe CAD: lines, XY rectangles, boxes, polygon extrusions.\n\nSelect in browser; drag vertex grips. Right-drag orbits.\nArrows: selected X/Z; PgUp/PgDn: Y; Tab: next vertex.\nEnter: coordinates; Esc: cancel drag.\n\n256 vertices / 512 edges; coordinates +/-400.\nUndo: 32 commands / 1024 deltas; operation: 64 deltas.\nExtrude: 3..12 XY points, +Z height 1..400.\nUnits label without rescaling. DXF/OBJ: wireframe only.\nNo curved kernel, solid booleans or feature tree.",
            "About CAD",MB_OK); return;
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
        else { selected=selectedEdge=0; Changed(hwnd); }
        return TRUE;
    }
    if (gesture) return TRUE;
    if (key==VK_TAB) {
        if (!model.vertex_count) return TRUE;
        i=VertexIndex(selected);
        if (GetKeyState(VK_SHIFT)<0) i=i<=0 ? model.vertex_count-1 : i-1;
        else i=(i+1)%model.vertex_count;
        selected=model.vertices[i].id; selectedEdge=0; Changed(hwnd); return TRUE;
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

LRESULT FAR PASCAL __export BrowserProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
{
    if (msg==WM_KEYDOWN && (wp==VK_ESCAPE || wp==VK_TAB || wp==VK_PRIOR || wp==VK_NEXT))
        return SendMessage(GetParent(hwnd),msg,wp,lp);
    return CallWindowProc((FARPROC)oldBrowserProc,hwnd,msg,wp,lp);
}
LRESULT FAR PASCAL __export MainWndProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
{
    PAINTSTRUCT ps;
    HDC dc;
    int x=(int)LOWORD(lp),y=(int)HIWORD(lp),index,row;
    DWORD data;
    CadStatus status;
    switch (msg) {
    case WM_CREATE:
        browser=CreateWindow("LISTBOX","",WS_CHILD | WS_VISIBLE | WS_BORDER |
            WS_VSCROLL | WS_HSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            0,RIBBON_HEIGHT+22,SIDE_WIDTH,100,hwnd,(HMENU)IDC_BROWSER,instance,NULL);
        if (!browser) return -1;
        SendMessage(browser,LB_SETHORIZONTALEXTENT,240,0);
        browserThunk=MakeProcInstance((FARPROC)BrowserProc,instance);
        if (browserThunk) {
            oldBrowserProc=(WNDPROC)GetWindowLong(browser,GWL_WNDPROC);
            SetWindowLong(browser,GWL_WNDPROC,(LONG)browserThunk);
        }
        edgePen=CreatePen(PS_SOLID,1,RGB(0,0,0));
        gridPen=CreatePen(PS_SOLID,1,RGB(215,215,215));
        selectedPen=CreatePen(PS_SOLID,2,RGB(192,0,0));
        hotBrush=CreateSolidBrush(RGB(255,64,64));
        captureTimer=SetTimer(hwnd,TIMER_CAPTURE,80,NULL)!=0;
        Changed(hwnd); return 0;
    case WM_SIZE:
        EndGesture(hwnd,FALSE);
        width=max(0,x); height=max(0,y);
        canvasWidth=max(0,width-CANVAS_LEFT); canvasHeight=max(0,height-STATUS_HEIGHT-RIBBON_HEIGHT);
        MoveWindow(browser,0,RIBBON_HEIGHT+22,min(width,SIDE_WIDTH),max(0,canvasHeight-22),TRUE);
        Fit(); InvalidateRect(hwnd,NULL,TRUE); return 0;
    case WM_PAINT:
        dc=BeginPaint(hwnd,&ps); Render(dc); EndPaint(hwnd,&ps); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_KEYDOWN: if (Key(hwnd,wp)) return 0; break;
    case WM_LBUTTONDOWN:
        if (y >= 0 && y < RIBBON_HEIGHT && !gesture) {
            row = RibbonHit(x, y);
            if (row >= 10000) { ribbonTab = row - 10000; InvalidateRect(hwnd,NULL,FALSE); }
            else if (row) Command(hwnd, row);
            return 0;
        }
        x -= CANVAS_LEFT; y -= RIBBON_HEIGHT;
        if (gesture && GetCapture()!=hwnd) EndGesture(hwnd,FALSE);
        if (gesture || sheetView) return 0;
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
        SetFocus(hwnd); index=HitTest(x,y); selectedEdge=0;
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
    case WM_MOUSEMOVE: MouseMove(hwnd,x-CANVAS_LEFT,y-RIBBON_HEIGHT); return 0;
    case WM_LBUTTONUP:
        if (gesture == G_CUBE) {
            if (GetCapture()!=hwnd) { EndGesture(hwnd,FALSE); return 0; }
            MouseMove(hwnd,x-CANVAS_LEFT,y-RIBBON_HEIGHT);
            if (gesture == G_CUBE) { row = cubePending; EndGesture(hwnd,TRUE); NavigateCube(hwnd,row); return 0; }
        }
        if (gesture==G_ORBIT) { MouseMove(hwnd,x-CANVAS_LEFT,y-RIBBON_HEIGHT); EndGesture(hwnd,TRUE); return 0; }
        if (gesture==G_GRIP) { MouseMove(hwnd,x-CANVAS_LEFT,y-RIBBON_HEIGHT); EndGesture(hwnd,TRUE); } return 0;
    case WM_RBUTTONUP:
        if (gesture==G_ORBIT) { MouseMove(hwnd,x-CANVAS_LEFT,y-RIBBON_HEIGHT); EndGesture(hwnd,TRUE); } return 0;
    case WM_TIMER:
        if (gesture && GetCapture()!=hwnd) EndGesture(hwnd,FALSE); return 0;
    case WM_CANCELMODE: case WM_KILLFOCUS:
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
            if (HIWORD(lp)==LBN_SELCHANGE || HIWORD(lp)==LBN_DBLCLK) {
                row=(int)SendMessage(browser,LB_GETCURSEL,0,0);
                if (row<0) return 0;
                data=(DWORD)SendMessage(browser,LB_GETITEMDATA,row,0);
                selected=(data&0x10000UL) ? 0 : (CadId)data;
                selectedEdge=(data&0x10000UL) ? (CadId)data : 0;
                UpdateUI(hwnd); InvalidateRect(hwnd,NULL,FALSE);
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
        if (hotBrush) DeleteObject(hotBrush);
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
    cad_model_init(&model); cad_model_init(&stage); MarkSaved();
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
