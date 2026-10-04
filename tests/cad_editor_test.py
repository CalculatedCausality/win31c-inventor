#!/usr/bin/env python3
"""Exercise actual cad.c controller routines with real portable CAD modules.

Native dialogs/GDI are compile-checked separately with Watcom. Only OS calls
are shimmed here. Generated sources/binaries/documents stay in a temp dir.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "cad.c").read_text()


def function(name):
    match = re.search(r"static [^\n]+\b" + name + r"\([^;]*?\)\s*\{", source)
    assert match, name
    pos = source.index("{", match.start()) + 1
    depth = 1
    # These routines have no unmatched braces inside string literals.
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[match.start():pos] + "\n"


shim = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cad_model.h"
#include "cad_create.h"
#include "cad_document.h"
#include "cad_export.h"
#include "cad_drawing.h"
#include "cad_aircraft.h"
#include "cad_meshview.h"
#include "cad_ui_ids.h"
#include "cad_viewcube.h"
typedef int BOOL;
typedef int HWND;
typedef int HDC;
typedef int HINSTANCE;
typedef int FARPROC;
typedef int WNDPROC;
typedef int HPEN;
typedef int HBRUSH;
typedef unsigned int WPARAM;
#define TRUE 1
#define FALSE 0
#define max(a,b) ((a)>(b)?(a):(b))
#define min(a,b) ((a)<(b)?(a):(b))
#define MB_OK 0
#define MB_ICONEXCLAMATION 0
#define MB_YESNOCANCEL 0
#define MB_ICONQUESTION 0
#define IDYES 6
#define IDNO 7
#define IDCANCEL 2
#define WM_CLOSE 16
#define VK_ESCAPE 27
#define VK_TAB 9
#define VK_SHIFT 16
#define VK_LEFT 37
#define VK_RIGHT 39
#define VK_UP 38
#define VK_DOWN 40
#define VK_PRIOR 33
#define VK_NEXT 34
static HWND capture;
static int errors, changes, lines, mx, my, answer=IDNO, shift;
static int textCalls;
static int dialogOK=1;
static char chosenPath[128]="TEST.CAD";
static HWND GetCapture(void) { return capture; }
static void ReleaseCapture(void) { capture=0; }
static void InvalidateRect(HWND h,void *r,BOOL e) { (void)h;(void)r;(void)e; }
static int MessageBox(HWND h,const char *s,const char *t,int f)
{ (void)h;(void)t;(void)f; if (strncmp(s,"Save changes",12)) ++errors; return answer; }
static void Changed(HWND h) { (void)h; ++changes; }
static void UpdateUI(HWND h) { (void)h; }
static void SendMessage(HWND h,int m,int w,long l) { (void)h;(void)m;(void)w;(void)l; }
static int GetKeyState(int k) { (void)k; return shift; }
static BOOL PickPath(HWND h,BOOL s,int f,char *p)
{ (void)h;(void)s;(void)f; if (!dialogOK) return FALSE; strcpy(p,chosenPath); return TRUE; }
static void CreateOrProperties(HWND h,int c) { (void)h;(void)c; }
static void MoveTo(HDC h,int x,int y) { (void)h; mx=x;my=y; }
static void LineTo(HDC h,int x,int y);
static void TextOut(HDC h,int x,int y,const char *s,int length)
{ (void)h;(void)x;(void)y;assert(length==(int)strlen(s));++textCalls; }
'''

defines = source[source.index("#define SIDE_WIDTH"):source.index("static CadModel model")]
globals_ = source[source.index("static CadModel model"):source.index("static int ISin")]
names = ["ISin", "ICos", "VertexIndex", "MarkSaved", "Dirty", "ParseNumber",
         "ParseCoordinate", "ParseProfile", "BoundedCoordinate", "Error",
         "ModelResult", "EndGesture", "MoveSelected", "ToCamera", "Fit", "BuildViewCube",
         "Project", "ClipTest", "CanvasLine", "WorldLine", "Materialize",
         "SheetLine", "SheetText", "DrawSheet", "HitTest", "MouseMove", "SaveDocument", "MayDiscard", "DeleteSelected",
          "AircraftTopology", "Command", "Key"]

tests = r'''
static void LineTo(HDC h,int x,int y)
{
    (void)h;
    assert(mx>=0 && mx<canvasWidth && my>=0 && my<canvasHeight);
    assert(x>=0 && x<canvasWidth && y>=0 && y<canvasHeight);
    ++lines;
}
static void BeginGrip(CadId target)
{
    selected=target; dragTarget=target;
    assert(cad_model_get_vertex(&model,target,&dragStart)==CAD_OK);
    dragScale=scale; dragCos=ICos(yaw); dragSin=ISin(yaw);
    dragCP=ICos(pitch); dragSP=ISin(pitch); dragSnap=snap;
    startMouseX=100; startMouseY=100;
    assert(cad_model_begin(&model)==CAD_OK); gesture=G_GRIP; capture=1;
}
int main(void)
{
    CadVertex v,before,other;
    CadEdge e;
    CadId a,b;
    int i,j,n=77,history,oldLines;
    long value=99;
    CadSketchPoint points[12];
    FILE *file;
    assert(cad_model_init(&model)==CAD_OK); cad_model_init(&stage); MarkSaved();
    assert(!model.vertex_count && !model.edge_count && !Dirty());
    width=760;height=520;canvasWidth=586;canvasHeight=496; Fit(); Project();
    assert(focal>=1 && HitTest(10,10)==-1 && Materialize());
    DrawSheet(1);assert(textCalls==1 && !lines);
    assert(ParseCoordinate(" -400 ",&value) && value==-400);
    assert(ParseCoordinate("+400",&value) && value==400);
    assert(!ParseCoordinate("401",&value)); assert(!ParseCoordinate("-401",&value));
    assert(!ParseCoordinate("9999999999999999999999999",&value));
    assert(!ParseCoordinate("1.5",&value)); assert(!ParseCoordinate("4xyz",&value));
    assert(!ParseCoordinate("",&value)); assert(!ParseCoordinate("+",&value));
    assert(!ParseCoordinate("1 2",&value));
    assert(ParseProfile("-50,-50; 50,-50;50,50;-50,50",points,&n) && n==4);
    assert(!ParseProfile("0,0;1,1",points,&n));
    assert(!ParseProfile("0,0;1,1;2,2;",points,&n));
    assert(!ParseProfile("0,0;1,1;401,2",points,&n));
    assert(!ParseProfile("0,0;1,1;2,2garbage",points,&n));
    assert(!ParseProfile("0,0;1,0;2,0;3,0;4,0;5,0;6,0;7,0;8,0;9,0;10,0;11,0;12,0",points,&n));
    assert(cad_create_line(&model,3,7,11,100,70,50)==CAD_OK);
    DrawSheet(1);assert(textCalls>1 && lines>0);
    a=model.vertices[0].id;b=model.vertices[1].id;
    assert(Dirty()); MarkSaved(); assert(!Dirty());
    selected=a; assert(MoveSelected(4,7,11)==CAD_OK && Dirty());
    assert(cad_model_undo(&model)==CAD_OK && !Dirty());
    assert(cad_model_redo(&model)==CAD_OK && Dirty());
    assert(cad_model_undo(&model)==CAD_OK); MarkSaved();
    /* Frozen target/basis, hundreds of moves, <=one staged delta, one commit. */
    yaw=0; pitch=0; scale=1; focal=1024; snap=FALSE;
    cad_model_get_vertex(&model,a,&before); cad_model_get_vertex(&model,b,&other);
    history=model.history_cursor;
    BeginGrip(a); selected=b;
    for (i=0; i<500; ++i) {
        MouseMove(1,120+i%80,110+i%30);
        assert(model.staged_count<=1 && model.history_cursor==history);
    }
    cad_model_get_vertex(&model,b,&v); assert(v.x==other.x && v.y==other.y && v.z==other.z);
    yaw=64;pitch=32;snap=TRUE; MouseMove(1,150,110);
    cad_model_get_vertex(&model,a,&v); assert(v.x==before.x+50 && v.y==before.y-10 && v.z==before.z);
    EndGesture(1,TRUE); assert(model.history_cursor==history+1 && !model.transaction_active);
    assert(cad_model_undo(&model)==CAD_OK && !Dirty());
    yaw=0;pitch=0;snap=TRUE;
    BeginGrip(a);MouseMove(1,120,100);cad_model_get_vertex(&model,a,&v);
    assert(v.y==before.y && v.z==before.z);EndGesture(1,FALSE);assert(!Dirty());
    /* No motion and move-away-return preserve unsnapped press coordinates/redo. */
    for (i=0; i<256; ++i) {
        yaw=i;pitch=i;snap=TRUE; history=model.history_cursor;
        BeginGrip(a); MouseMove(1,100,100); EndGesture(1,TRUE);
        assert(model.history_cursor==history && !Dirty());
        BeginGrip(a); MouseMove(1,140,130); MouseMove(1,100,100); EndGesture(1,TRUE);
        assert(model.history_cursor==history && !Dirty());
    }
    yaw=0;pitch=0;snap=FALSE;
    BeginGrip(a); MouseMove(1,140,130); capture=2; MouseMove(1,150,140);
    assert(!gesture && capture==2 && !Dirty() && !model.transaction_active);
    BeginGrip(a); MouseMove(1,-32768,32767); EndGesture(1,TRUE);
    cad_model_get_vertex(&model,a,&v); assert(v.x==-400 && v.y==-400 && v.z==11);
    assert(cad_model_undo(&model)==CAD_OK && !Dirty());
    BeginGrip(a); MouseMove(1,160,100); EndGesture(1,FALSE); assert(!Dirty());
    gesture=G_ORBIT;startYaw=32;startPitch=24;capture=1;
    startMouseX=-32768;startMouseY=-32768; MouseMove(1,32767,32767);
    assert(yaw>=0 && yaw<256 && pitch>=0 && pitch<256);
    EndGesture(1,FALSE);assert(yaw==32 && pitch==24);
    /* Dense swap on deletion must not confuse browser IDs or export endpoints. */
    selected=a; selectedEdge=0; DeleteSelected(1); assert(VertexIndex(a)<0 && VertexIndex(b)==0);
    assert(cad_model_undo(&model)==CAD_OK && !Dirty());
    assert(cad_create_line(&model,-10,0,0,-20,0,0)==CAD_OK);
    assert(Materialize());
    DrawSheet(1);
    for (i=0; i<(int)model.edge_count; ++i) {
        e=model.edges[i];
        assert(model.vertices[exportEdges[i].a].id==e.a);
        assert(model.vertices[exportEdges[i].b].id==e.b);
    }
    /* Exact saved snapshot survives history ring eviction and array reorder. */
    selected=b; MarkSaved(); cad_model_get_vertex(&model,b,&before);
    for (i=1; i<=40; ++i) assert(MoveSelected(before.x+i,before.y,before.z)==CAD_OK);
    assert(model.history_count==32 && Dirty());
    assert(MoveSelected(before.x,before.y,before.z)==CAD_OK && !Dirty());
    assert(cad_model_undo(&model)==CAD_OK && Dirty());
    assert(cad_model_redo(&model)==CAD_OK && !Dirty());
    /* Numeric keyboard never changes unselected coordinates, and rejects range. */
    selected=0; history=model.history_cursor; assert(Key(1,VK_RIGHT)); assert(model.history_cursor==history);
    selected=b;snap=FALSE;cad_model_get_vertex(&model,b,&before);
    assert(Key(1,VK_RIGHT));cad_model_get_vertex(&model,b,&v);
    assert(v.x==before.x+1 && v.y==before.y && v.z==before.z);
    assert(Key(1,VK_PRIOR));cad_model_get_vertex(&model,b,&v);
    assert(v.x==before.x+1 && v.y==before.y+1 && v.z==before.z);
    assert(MoveSelected(400,v.y,v.z)==CAD_OK);history=model.history_cursor;
    assert(Key(1,VK_RIGHT));assert(model.history_cursor==history);cad_model_get_vertex(&model,b,&v);assert(v.x==400);
    assert(Key(1,VK_ESCAPE) && !selected && !selectedEdge);
    /* Clip huge offscreen endpoints and fit all orientations in tiny views. */
    canvasWidth=100;canvasHeight=80; lines=0;
    CanvasLine(1,-1e12,40,1e12,40); assert(lines==1);
    CanvasLine(1,-20,-20,-10,-10); assert(lines==1);
    CanvasLine(1,20,-1e12,20,1e12); assert(lines==2);
    for (i=0; i<256; ++i) {
        yaw=i;pitch=i; Fit();Project();assert(focal>=1 && focal<=8192);
        for (j=0; j<(int)model.vertex_count; ++j) if (visible[j]) {
            assert(px[j]>=GRIP && px[j]<canvasWidth-GRIP);
            assert(py[j]>=GRIP && py[j]<canvasHeight-GRIP);
            assert(HitTest(px[j],py[j])>=0);
        }
        WorldLine(1,-400,-400,-400,400,400,400);
    }
    oldLines=lines;canvasWidth=0;canvasHeight=0;Fit();Project();
    CanvasLine(1,0,0,100,100); assert(lines==oldLines);
    for (i=0;i<(int)model.vertex_count;++i) assert(!visible[i]);
    /* Native command routing: save cancellation/failure, optional demo discard,
       load fail-atomicity, open/save/undo dirty restoration, units, empty New. */
    filePath[0]=0; dialogOK=0; assert(!SaveDocument(1,FALSE) && !filePath[0]);
    dialogOK=1;strcpy(chosenPath,"MISSING/TEST.CAD"); assert(!SaveDocument(1,FALSE));
    strcpy(chosenPath,"TEST.CAD"); assert(SaveDocument(1,FALSE) && !Dirty());
    selected=b;assert(MoveSelected(399,v.y,v.z)==CAD_OK && Dirty());
    answer=IDCANCEL;Command(1,ID_DEMO_HOUSE);assert(Dirty() && VertexIndex(b)>=0);
    answer=IDYES; dialogOK=0;filePath[0]=0;Command(1,ID_DEMO_HOUSE);assert(Dirty() && VertexIndex(b)>=0);
    dialogOK=1; answer=IDNO;Command(1,ID_DEMO_HOUSE);
    assert(model.vertex_count==30 && model.edge_count==40 && !model.history_count && !*filePath && Dirty());
    Command(1,ID_FILE_OPEN);assert(!Dirty() && !strcmp(filePath,"TEST.CAD"));
    cad_model_get_vertex(&model,b,&before);
    file=fopen("BAD.CAD","wb");assert(file);fputs("bad",file);fclose(file);
    strcpy(chosenPath,"BAD.CAD");Command(1,ID_FILE_OPEN);
    assert(!Dirty() && !strcmp(filePath,"TEST.CAD") && cad_model_get_vertex(&model,b,&v)==CAD_OK && v.x==before.x);
    Command(1,ID_TOOLS_MM);assert(model.units==CAD_UNITS_MM && Dirty());
    Command(1,ID_EDIT_UNDO);assert(model.units==CAD_UNITS_UNSPECIFIED && !Dirty());
    Command(1,ID_FILE_NEW);assert(!model.vertex_count && !model.edge_count && !Dirty() && !*filePath);
    Command(1,ID_VIEW_DRAWING);assert(sheetView);Command(1,ID_VIEW_FRONT);assert(!sheetView && yaw==0 && pitch==0);
    Command(1,ID_FILE_NEW); Command(1,ID_DEMO_747);
    assert(model.vertex_count==236 && model.edge_count==396 && shaded);
    assert(AircraftTopology());
    Command(1,ID_VIEW_WIREFRAME); assert(!shaded);
    Command(1,ID_VIEW_SHADED); assert(shaded);
    assert(cad_model_begin(&model)==CAD_OK);
    assert(cad_model_delete_vertex(&model,1)==CAD_OK);
    assert(!AircraftTopology());
    assert(cad_model_cancel(&model)==CAD_OK && AircraftTopology());
    puts("cad editor: parsing, exact dirty tracking, 500-preview drag, cancel/capture, stable IDs, keyboard, clipping, file/demo commands passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="cad-editor-") as temp:
    temp = Path(temp)
    c = temp / "controller.c"
    sheet_type = source[source.index("typedef struct SheetContext"):source.index("static int SheetLine")]
    c.write_text(shim + defines + globals_ + sheet_type + "".join(function(n) for n in names) + tests)
    executable = temp / "controller"
    flags = ["-std=c89", "-pedantic", "-Wall", "-Wextra", "-Wno-unused-variable"]
    if os.environ.get("CAD_EDITOR_SANITIZE"):
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    subprocess.run(["cc", *flags, "-I", str(root), str(c),
                    *(str(root / f) for f in ["cad_model.c", "cad_create.c", "cad_sketch.c",
                                             "cad_document.c", "cad_export.c", "cad_drawing.c", "cad_aircraft.c", "cad_viewcube.c"]),
                    "-o", str(executable)], check=True)
    subprocess.run([str(executable)], cwd=temp, check=True)

# Startup is structurally separate from the optional demo command.
startup = source[source.index("int PASCAL WinMain"):]
assert "cad_model_init(&model)" in startup and "cad_create_house_demo" not in startup
assert re.search(r"case ID_DEMO_HOUSE:\s*if \(!MayDiscard\(hwnd\)\) return;", source)
resources = (root / "cad.rc").read_text()
for command in ["ID_FILE_NEW", "ID_FILE_OPEN", "ID_FILE_SAVE", "ID_FILE_SAVEAS",
                "ID_EDIT_UNDO", "ID_EDIT_REDO", "ID_CREATE_LINE", "ID_CREATE_RECTANGLE",
                "ID_CREATE_BOX", "ID_CREATE_EXTRUDE", "ID_DEMO_HOUSE"]:
    assert command in resources
print("cad editor: empty startup, guarded optional demo and native menu resource checks passed")
