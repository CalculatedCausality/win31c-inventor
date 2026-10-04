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


def position(name):
    match = re.search(r"static [^\n]+\b" + name + r"\([^;]*?\)\s*\{", source)
    assert match, name
    return match.start()


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
#include "cad_userparams.h"
#include "cad_vconstraints.h"
#include "cad_script.h"
#include "cad_parts.h"
#include "cad_featureui.h"
typedef int BOOL;
#define NEAR
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
#define MB_ICONINFORMATION 0
#define IDYES 6
#define IDNO 7
#define IDOK 1
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
static char lastMessage[512];
static int dialogResult=1;
static int ParametersDlg, DistanceDlg, PlacementDlg, FeatureDlg;
/* Optional per-dialog hook: emulates the user editing dialog in/out globals. */
static void (*dialogHook)(int id);
#define IDD_PARAMETERS_SHIM 0
static HWND GetCapture(void) { return capture; }
static void ReleaseCapture(void) { capture=0; }
static void InvalidateRect(HWND h,void *r,BOOL e) { (void)h;(void)r;(void)e; }
static int MessageBox(HWND h,const char *s,const char *t,int f)
{ (void)h;(void)t;(void)f; if (strncmp(s,"Save changes",12)) { ++errors; strncpy(lastMessage,s,511); } return answer; }
static void SyncConstraints(void);
static void SyncFeatures(void);
static void Changed(HWND h) { (void)h; ++changes; SyncFeatures(); SyncConstraints(); }
/* Native modal dialogs: the test presets their in/out globals. */
static int RunDialog(HWND h,FARPROC p,int id) { (void)h;(void)p; if (dialogHook) dialogHook(id); return dialogResult; }
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
          "AircraftTopology", "ParamText", "VcText", "ParseField", "VcMessage",
          "SyncConstraints", "StageVertexMove", "BuildExtension", "ApplyExtension",
          "ResetSelection", "ResetDocumentState", "AxisDelta", "CommitMoves",
          "FinishConstraintEdit", "AddConstraint", "DistanceCommand", "HistorySignature",
          "FollowUnits", "UndoRedo", "ParamLookup", "RunMacro", "Command", "Key",
          "PartsText", "PartsMessage", "FitPoint", "ParseOffset", "ParseYaw",
          "ComponentSegment", "ComponentCommand", "SyncFeatures", "SketchView",
          "FeatureCommand"]

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
static CadVertex At(CadId id)
{
    CadVertex v;
    assert(cad_model_get_vertex(&model,id,&v)==CAD_OK);
    return v;
}
static void WriteText(const char *path,const char *text)
{
    FILE *file=fopen(path,"wb");
    assert(file); fputs(text,file); assert(fclose(file)==0);
}
static void integration_tests(void)
{
    CadVertex v;
    CadId a,b,c,d;
    CadVConstraint record;
    long value;
    int history,count,oldErrors,vertices,edges;
    answer=IDNO; dialogOK=1; capture=0;
    Command(1,ID_FILE_NEW); assert(!model.vertex_count && !Dirty());
    assert(!cad_userparams_count(&userParams) && !constraints.count);
    /* Parameters: dialog fields take expressions; table changes are dirty. */
    assert(cad_userparams_add(&userParams,"W","120",NULL,NULL)==CAD_UP_OK && Dirty());
    assert(ParseField("W",&value) && value==120);
    assert(ParseField(" W/2 + 10 ",&value) && value==70);
    assert(ParseField("-400",&value) && value==-400);
    assert(!ParseField("Nope+1",&value) && fieldError[0]);
    assert(!ParseField("W*4",&value) && strstr(fieldError,"column"));
    assert(!ParseField("10 mm",&value)); /* unspecified documents: no literals */
    /* Units: parameters follow Tools > Units; mm literals block unspecified. */
    Command(1,ID_TOOLS_MM); assert(model.units==CAD_UNITS_MM && cad_userparams_units(&userParams)==CAD_UNITS_MM);
    assert(cad_userparams_add(&userParams,"T","1 cm",NULL,NULL)==CAD_UP_OK);
    assert(ParseField("T*2",&value) && value==20);
    oldErrors=errors; Command(1,ID_TOOLS_UNSPECIFIED);
    assert(errors==oldErrors+1 && model.units==CAD_UNITS_MM && cad_userparams_units(&userParams)==CAD_UNITS_MM);
    history=model.history_cursor; oldErrors=errors; Command(1,ID_EDIT_UNDO); /* undo of units refused too */
    assert(errors==oldErrors+1 && model.units==CAD_UNITS_MM && model.history_cursor==history);
    assert(cad_userparams_remove(&userParams,cad_userparams_find(&userParams,"T"),NULL)==CAD_UP_OK);
    Command(1,ID_EDIT_UNDO); assert(model.units==CAD_UNITS_UNSPECIFIED && cad_userparams_units(&userParams)==CAD_UNITS_UNSPECIFIED);
    /* Constraints: two lines, align X moves the second vertex in one command. */
    assert(cad_create_line(&model,0,0,0,50,0,0)==CAD_OK);
    assert(cad_create_line(&model,10,40,0,90,40,0)==CAD_OK);
    a=model.vertices[0].id; b=model.vertices[1].id; c=model.vertices[2].id; d=model.vertices[3].id;
    selected=a; selected2=c; history=model.history_cursor;
    AddConstraint(1,CAD_VC_ALIGN,0,0);
    assert(constraints.count==1 && selectedConstraint && model.history_cursor==history+1);
    assert(At(c).x==At(a).x && At(c).y==40);
    /* Arrow nudge and Properties move the partner in the same transaction. */
    selected=a; selected2=0; history=model.history_cursor;
    assert(Key(1,VK_RIGHT)); assert(model.history_cursor==history+1 && At(a).x==1 && At(c).x==1);
    assert(MoveSelected(-30,0,0)==CAD_OK && At(c).x==-30 && At(c).y==40);
    /* Grip drag: one commit, partner follows during preview. */
    yaw=0; pitch=0; scale=1; focal=1024; snap=FALSE; history=model.history_cursor;
    BeginGrip(a); MouseMove(1,120,100); assert(At(c).x==-10 && model.history_cursor==history);
    EndGesture(1,TRUE); assert(model.history_cursor==history+1 && At(c).x==-10);
    /* Fix: a fixed group refuses to move and explains why. */
    selected=c; AddConstraint(1,CAD_VC_FIX,0,0); assert(constraints.count==2);
    selected=a; history=model.history_cursor; oldErrors=errors;
    assert(MoveSelected(5,0,0)!=CAD_OK); ModelResult(1,CAD_RANGE);
    assert(model.history_cursor==history && At(a).x==-10 && errors==oldErrors+1 && strstr(lastMessage,"fixed"));
    BeginGrip(a); MouseMove(1,140,100); assert(!gesture && At(a).x==-10 && !model.transaction_active);
    /* Distance between b and d (dialog preset), then edit via the browser record. */
    selected=b; selected2=d; selectedConstraint=0; distanceAxis=1; dialogResult=IDOK;
    DistanceCommand(1); /* proposes current Y delta (40) */
    assert(constraints.count==3 && cad_vc_get(&constraints,selectedConstraint,&record)==CAD_VC_OK);
    assert(record.kind==CAD_VC_DISTANCE && record.axis==0 && record.offset==At(d).x-At(b).x);
    selected=selected2=0; DistanceCommand(1); /* editing keeps value */
    assert(cad_vc_get(&constraints,record.id,&record)==CAD_VC_OK && record.offset==At(d).x-At(b).x);
    /* Undo of geometry leaves records; sync marks unmet ones Sick, redo heals. */
    selectedConstraint=0; selected=b; history=model.history_cursor;
    assert(MoveSelected(60,0,0)==CAD_OK && At(d).x-At(b).x==record.offset);
    Command(1,ID_EDIT_UNDO); Command(1,ID_EDIT_UNDO);
    count=0; for (value=0; value<constraints.count; ++value) count+=constraints.items[value].health==CAD_VC_SICK;
    assert(constraints.count==3 && count>=1);
    Command(1,ID_EDIT_REDO); Command(1,ID_EDIT_REDO);
    count=0; for (value=0; value<constraints.count; ++value) count+=constraints.items[value].health==CAD_VC_SICK;
    assert(count==0);
    /* Save/load round trip includes [PARAMS] and [CONSTRAINTS]; dirty tracks them. */
    strcpy(chosenPath,"CONS.CAD"); filePath[0]=0; assert(SaveDocument(1,FALSE) && !Dirty());
    selectedConstraint=constraints.items[0].id; selected=selectedEdge=0;
    DeleteSelected(1); assert(constraints.count==2 && Dirty());
    vertices=model.vertex_count; edges=model.edge_count;
    Command(1,ID_FILE_NEW); assert(!constraints.count && !cad_userparams_count(&userParams));
    Command(1,ID_FILE_OPEN);
    assert(model.vertex_count==vertices && model.edge_count==edges && constraints.count==3);
    assert(cad_userparams_count(&userParams)==1 && !Dirty() && !strcmp(filePath,"CONS.CAD"));
    /* Deleting a constrained vertex drops its records (sync after delete). */
    selected=c; selectedEdge=0; DeleteSelected(1); assert(constraints.count==1);
    Command(1,ID_EDIT_UNDO); assert(VertexIndex(c)>=0 && constraints.count==1);
    /* Unknown/malformed sections are rejected atomically. */
    assert(cad_document_save_ex("BADEXT.CAD",&model,"[PARAMS]\nP Q 5\n[PARTS]\nX\n",strlen("[PARAMS]\nP Q 5\n[PARTS]\nX\n"))==CAD_DOC_OK);
    strcpy(chosenPath,"BADEXT.CAD"); answer=IDNO; oldErrors=errors; count=constraints.count;
    Command(1,ID_FILE_OPEN);
    assert(errors>oldErrors && strstr(lastMessage,"PARTS") && !strcmp(filePath,"CONS.CAD"));
    assert(constraints.count==count && !cad_userparams_find(&userParams,"Q"));
    assert(cad_document_save_ex("BADEXT.CAD",&model,"[PARAMS]\nP Q W+\n",strlen("[PARAMS]\nP Q W+\n"))==CAD_DOC_OK);
    oldErrors=errors; Command(1,ID_FILE_OPEN); assert(errors>oldErrors && !cad_userparams_find(&userParams,"Q"));
    assert(cad_document_save_ex("BADEXT.CAD",&model,"[CONSTRAINTS]\nN 1\n[CONSTRAINTS]\nN 1\n",strlen("[CONSTRAINTS]\nN 1\n[CONSTRAINTS]\nN 1\n"))==CAD_DOC_OK);
    oldErrors=errors; Command(1,ID_FILE_OPEN); assert(errors>oldErrors && strstr(lastMessage,"Duplicate"));
    /* Plain v1-style (no extension) documents load with empty tables. */
    assert(cad_document_save("PLAIN.CAD",&model)==CAD_DOC_OK);
    strcpy(chosenPath,"PLAIN.CAD"); answer=IDNO; Command(1,ID_FILE_OPEN);
    assert(!constraints.count && !cad_userparams_count(&userParams) && !Dirty());
    /* Macros: all-or-nothing, parameters resolve, one Undo reverts all. */
    Command(1,ID_FILE_NEW);
    assert(cad_userparams_add(&userParams,"PW","25.4",NULL,NULL)==CAD_UP_OK);
    WriteText("PBOX.CMD","CADSCRIPT 1\nBOX 0 0 0 PW PW PW*2\nLINE 0 0 0 1 1 1\n");
    strcpy(chosenPath,"PBOX.CMD"); history=model.history_cursor; RunMacro(1);
    assert(model.vertex_count==10 && model.edge_count==13 && model.history_cursor==history+2);
    for (value=0; value<(long)model.vertex_count; ++value) {
        v=model.vertices[value]; assert(v.x==0 || v.x==25 || v.x==1);
        assert(v.z==0 || v.z==50 || v.z==1);
    }
    Command(1,ID_EDIT_UNDO); assert(!model.vertex_count && model.history_cursor==history);
    Command(1,ID_EDIT_REDO); assert(model.vertex_count==10 && model.edge_count==13);
    Command(1,ID_EDIT_UNDO); assert(!model.vertex_count);
    WriteText("BAD.CMD","CADSCRIPT 1\nBOX 0 0 0 10 10 10\nBOX 0 0 0 0 10 10\n");
    strcpy(chosenPath,"BAD.CMD"); oldErrors=errors; RunMacro(1);
    assert(errors==oldErrors+1 && strstr(lastMessage,"line 3") && !model.vertex_count);
    WriteText("UNK.CMD","CADSCRIPT 1\nBOX 0 0 0 NOPE 10 10\n");
    strcpy(chosenPath,"UNK.CMD"); oldErrors=errors; RunMacro(1);
    assert(errors==oldErrors+1 && strstr(lastMessage,"line 2") && !model.vertex_count);
    assert(cad_create_line(&model,0,0,0,5,5,5)==CAD_OK);
    vertices=model.vertex_count; edges=model.edge_count;
    strcpy(chosenPath,TABLE_PATH); RunMacro(1);
    assert(model.vertex_count==vertices+40 && model.edge_count==edges+60 && model.units==CAD_UNITS_MM);
    assert(cad_userparams_units(&userParams)==CAD_UNITS_MM);
    Command(1,ID_EDIT_UNDO);
    assert(model.vertex_count==vertices && model.edge_count==edges && model.units==CAD_UNITS_UNSPECIFIED);
    assert(cad_userparams_units(&userParams)==CAD_UNITS_UNSPECIFIED);
    Command(1,ID_EDIT_REDO); assert(model.vertex_count==vertices+40);
    /* Any later edit invalidates the group: Undo is one command again. */
    Command(1,ID_EDIT_UNDO); Command(1,ID_EDIT_REDO);
    selected=model.vertices[0].id; assert(MoveSelected(7,7,7)==CAD_OK);
    Command(1,ID_EDIT_UNDO); assert(model.vertex_count==vertices+40);
    Command(1,ID_EDIT_UNDO); assert(model.vertex_count==vertices+32);
}
static long hookPlace[4];
static void PlaceHook(int id)
{
    if (id==IDD_PLACEMENT) memcpy(placeValues,hookPlace,sizeof(hookPlace));
}
static CadPartsInfo Component(unsigned index)
{
    CadAssemblyId id;
    CadPartsInfo info;
    assert(cad_parts_at(&parts,index,&id)==CAD_PARTS_OK);
    assert(cad_parts_get(&parts,id,&info)==CAD_PARTS_OK);
    return info;
}
static void Place(const char *path,long x,long y,long z,long yawIndex)
{
    strcpy(chosenPath,path);
    hookPlace[0]=x; hookPlace[1]=y; hookPlace[2]=z; hookPlace[3]=yawIndex;
    dialogResult=IDOK; dialogHook=PlaceHook;
    Command(1,ID_INSERT_PLACE);
    dialogHook=NULL;
}
static void parts_tests(void)
{
    CadPartsInfo info;
    long value;
    int history,oldErrors,oldLines;
    unsigned count;
    HDC dc=1;
    answer=IDNO; dialogOK=1; capture=0; dialogResult=IDOK;
    /* Offsets: integers +-2000, expressions in the model range; yaw degrees. */
    assert(ParseOffset("2000",&value) && value==2000);
    assert(ParseOffset(" -2000 ",&value) && value==-2000);
    assert(!ParseOffset("2001",&value) && strstr(fieldError,"2000"));
    assert(!ParseOffset("99999999999999999999",&value));
    assert(ParseYaw("90",&value) && value==2);
    assert(ParseYaw("-45",&value) && value==7);
    assert(ParseYaw("360",&value) && value==0);
    assert(!ParseYaw("30",&value) && strstr(fieldError,"45"));
    /* A part: one box saved as PART.CAD. */
    Command(1,ID_FILE_NEW);
    assert(cad_create_box(&model,0,0,0,100,100,100)==CAD_OK);
    strcpy(chosenPath,"PART.CAD"); filePath[0]=0; assert(SaveDocument(1,FALSE) && !Dirty());
    Command(1,ID_FILE_NEW); assert(!cad_parts_count(&parts) && !Dirty());
    /* Place at X=200, yaw 90: not a model undo step, but the document is dirty. */
    assert(cad_userparams_add(&userParams,"OFF","20",NULL,NULL)==CAD_UP_OK);
    MarkSaved();
    assert(cad_create_line(&model,0,0,0,10,0,0)==CAD_OK);
    history=model.history_cursor; MarkSaved();
    Place("PART.CAD",200,0,0,2);
    assert(cad_parts_count(&parts)==1 && selectedComponent && Dirty());
    assert(model.history_cursor==history && model.vertex_count==2);
    info=Component(0);
    assert(!strcmp(info.name,"PART:1") && !info.missing && info.vertex_count==8 && info.edge_count==12);
    assert(info.tx==200 && info.yaw==2 && !info.grounded && !strcmp(info.path,"PART.CAD"));
    /* Zoom all covers the line and the rotated box (x 200..300, z -100..0). */
    canvasWidth=586; canvasHeight=496; yaw=32; pitch=24; Fit();
    assert(centerX==150 && centerY==50 && centerZ==-50 && focal>=1);
    /* Component edges are clipped in floating point even far off screen. */
    lines=0; assert(cad_parts_segments(&parts,0,ComponentSegment,&dc)==CAD_PARTS_OK && lines>0);
    Place("PART.CAD",2000,-2000,2000,1);
    info=Component(1); assert(cad_parts_count(&parts)==2 && !strcmp(info.name,"PART:2"));
    scale=8; focal=8192; oldLines=lines;
    assert(cad_parts_segments(&parts,0,ComponentSegment,&dc)==CAD_PARTS_OK && lines>=oldLines);
    Fit(); assert(centerX>1000 && focal>=1);
    /* Undo moves geometry only; components stay. */
    Command(1,ID_EDIT_UNDO); assert(!model.vertex_count && cad_parts_count(&parts)==2);
    Command(1,ID_EDIT_REDO); assert(model.vertex_count==2);
    /* Remove via Del on a selected component row. */
    selectedComponent=Component(1).id; selected=selectedEdge=0; selectedConstraint=0;
    Command(1,ID_EDIT_DELETE); assert(cad_parts_count(&parts)==1 && !selectedComponent);
    Command(1,ID_EDIT_UNDO); assert(cad_parts_count(&parts)==1);
    Command(1,ID_EDIT_REDO);
    /* Ground blocks Move; unground, then Move with an expression offset. */
    selectedComponent=Component(0).id;
    Command(1,ID_INSERT_GROUND); assert(Component(0).grounded);
    oldErrors=errors; hookPlace[0]=1; dialogHook=PlaceHook;
    Command(1,ID_INSERT_MOVE);
    assert(errors==oldErrors+1 && strstr(lastMessage,"grounded") && Component(0).tx==200);
    Command(1,ID_INSERT_GROUND); assert(!Component(0).grounded);
    assert(ParseOffset("OFF*2+10",&value) && value==50);
    hookPlace[0]=value; hookPlace[1]=10; hookPlace[2]=-20; hookPlace[3]=1;
    Command(1,ID_INSERT_MOVE); dialogHook=NULL;
    info=Component(0); assert(info.tx==50 && info.ty==10 && info.tz==-20 && info.yaw==1);
    /* Without a selection the component commands explain themselves. */
    selectedComponent=0; oldErrors=errors; Command(1,ID_INSERT_REMOVE);
    assert(errors==oldErrors+1 && cad_parts_count(&parts)==1);
    /* Save writes [PARTS]; dirty tracks ground/move; reopen restores all. */
    strcpy(chosenPath,"ASM.CAD"); filePath[0]=0; assert(SaveDocument(1,FALSE) && !Dirty());
    selectedComponent=Component(0).id; Command(1,ID_INSERT_GROUND); assert(Dirty());
    Command(1,ID_INSERT_GROUND); assert(!Dirty());
    Command(1,ID_FILE_NEW); assert(!cad_parts_count(&parts) && !selectedComponent);
    strcpy(chosenPath,"ASM.CAD"); Command(1,ID_FILE_OPEN);
    assert(cad_parts_count(&parts)==1 && !Dirty() && !strcmp(filePath,"ASM.CAD"));
    info=Component(0);
    assert(info.tx==50 && info.ty==10 && info.tz==-20 && info.yaw==1 && info.vertex_count==8);
    assert(model.vertex_count==2 && cad_userparams_find(&userParams,"OFF"));
    /* A [PARTS] failure after a valid [PARAMS] keeps the whole document,
     * including the previous parameter table. */
    assert(cad_document_save_ex("BADPARTS.CAD",&model,"[PARAMS]\nP Q 5\n[PARTS]\nN 1\nO 1 0 0 0 0 0 X\n",
        strlen("[PARAMS]\nP Q 5\n[PARTS]\nN 1\nO 1 0 0 0 0 0 X\n"))==CAD_DOC_OK);
    strcpy(chosenPath,"BADPARTS.CAD"); oldErrors=errors; Command(1,ID_FILE_OPEN);
    assert(errors==oldErrors+1 && strstr(lastMessage,"[PARTS] line 2") && !strcmp(filePath,"ASM.CAD"));
    assert(cad_userparams_find(&userParams,"OFF") && !cad_userparams_find(&userParams,"Q"));
    assert(cad_parts_count(&parts)==1 && Component(0).tx==50 && !Dirty());
    /* Missing part files: placed without geometry, reload retries. */
    remove("LATER.CAD");
    oldErrors=errors; Place("LATER.CAD",0,0,0,0);
    assert(errors==oldErrors+1 && cad_parts_count(&parts)==2 && Component(1).missing);
    count=0; assert(cad_parts_segments(&parts,Component(1).id,ComponentSegment,&dc)==CAD_PARTS_OK);
    oldErrors=errors; Command(1,ID_INSERT_RELOAD); assert(errors==oldErrors+1 && Component(1).missing);
    assert(cad_document_save("LATER.CAD",&model)==CAD_DOC_OK);
    oldErrors=errors; Command(1,ID_INSERT_RELOAD);
    assert(errors==oldErrors && !Component(1).missing && Component(1).vertex_count==2);
    /* A cancelled placement dialog changes nothing. */
    dialogResult=IDCANCEL; strcpy(chosenPath,"PART.CAD"); dialogHook=PlaceHook;
    Command(1,ID_INSERT_PLACE); dialogHook=NULL; dialogResult=IDOK;
    assert(cad_parts_count(&parts)==2);
    /* Demo and New reset components. */
    Command(1,ID_DEMO_HOUSE); assert(!cad_parts_count(&parts) && model.vertex_count==30);
    (void)count;
}
/* ---- part features: the hook plays the native FeatureDlg (fill fields,
 * press OK -> cad_fui_apply; on a rejected OK the user cancels). ---- */
static const char *hookFields[4];
static const char *hookPoints;
static int hookOption=-1, hookApplied;
static const char *hookParam, *hookExpr;
static void FeatureHook(int id)
{
    int i;
    if (id==IDD_PARAMETERS) {
        assert(cad_userparams_set_expression_named(&userParams,hookParam,hookExpr,NULL)==CAD_UP_OK);
        return;
    }
    assert(id==(cad_fui_dialog.kind==CAD_FUI_SKETCH ? IDD_FEAT_SKETCH : cad_fui_dialog.kind==CAD_FUI_RENAME ?
        IDD_FEAT_RENAME : cad_fui_dialog.kind>=CAD_FUI_EXTRUDE ? IDD_FEAT_SOLID : IDD_FEAT_PROFILE));
    for (i=0; i<4; ++i) if (hookFields[i]) strcpy(cad_fui_dialog.fields[i],hookFields[i]);
    if (hookPoints) strcpy(cad_fui_dialog.points,hookPoints);
    if (hookOption>=0) cad_fui_dialog.option=hookOption;
    hookApplied=cad_fui_apply();
    if (!hookApplied) { ++errors; strncpy(lastMessage,cad_fui_dialog.message,511); }
    dialogResult=hookApplied ? IDOK : IDCANCEL;
}
static int Feature(int command,const char *a,const char *b,const char *c,const char *d,const char *points,int option)
{
    hookFields[0]=a; hookFields[1]=b; hookFields[2]=c; hookFields[3]=d; hookPoints=points; hookOption=option;
    hookApplied=-1; dialogHook=FeatureHook; Command(1,command); dialogHook=NULL; dialogResult=IDOK;
    return hookApplied;
}
static void SetParam(const char *name,const char *expr)
{
    hookParam=name; hookExpr=expr; dialogHook=FeatureHook;
    Command(1,ID_TOOLS_PARAMETERS); dialogHook=NULL;
}
static void Row(unsigned row,char *text)
{
    unsigned id; int profile;
    assert(cad_fui_row(row,text,260,&id,&profile));
}
static long ExtentX(void)
{
    long lo[3],hi[3];
    assert(cad_fui_bounds(lo,hi));
    return hi[0]-lo[0];
}
static long TopZ(void)
{
    long lo[3],hi[3];
    assert(cad_fui_bounds(lo,hi));
    return hi[2];
}
static void feature_tests(void)
{
    char text[260];
    unsigned sketch,extrude,sketch2;
    unsigned long t;
    int oldErrors,count;
    long lo[3],hi[3];
    answer=IDNO; dialogOK=1; capture=0; dialogResult=IDOK;
    Command(1,ID_FILE_NEW); assert(!cad_fui_count() && !Dirty());
    assert(cad_userparams_add(&userParams,"W","100",NULL,NULL)==CAD_UP_OK);
    /* Profiles need sketch mode. */
    oldErrors=errors; assert(Feature(ID_FEATURE_RECT,"0","0","W","60",NULL,-1)==-1);
    assert(errors==oldErrors+1 && strstr(lastMessage,"Start 2D Sketch") && !cad_fui_count());
    /* Start 2D Sketch (XY): sketch mode, view normal to the plane. */
    yaw=32; pitch=24;
    assert(Feature(ID_FEATURE_SKETCH,"0",NULL,NULL,NULL,NULL,CAD_FEAT_XY)==1);
    sketch=selectedFeature; assert(sketch && cad_fui_active_sketch()==sketch && yaw==0 && pitch==0);
    assert(!strcmp(cad_fui_name(sketch),"Sketch1") && Dirty());
    assert(Feature(ID_FEATURE_RECT,"0","0","W","60",NULL,-1)==1);
    assert(selectedFeature==sketch && selectedProfile==0);
    Command(1,ID_FEATURE_FINISH); assert(!cad_fui_active_sketch() && yaw==32 && pitch==24);
    /* Extrude 40 (default sketch = last sketch, profile ALL). */
    assert(Feature(ID_FEATURE_EXTRUDE,"ALL","40",NULL,NULL,NULL,CAD_FEAT_POSITIVE)==1);
    extrude=selectedFeature; assert(cad_fui_type(extrude)==CAD_FEAT_EXTRUDE && cad_fui_count()==2);
    assert(!cad_fui_errors() && cad_fui_output()->face_count>0 && ExtentX()==100 && TopZ()==40);
    assert(cad_fui_bounds(lo,hi) && lo[0]==-50 && lo[1]==-30 && hi[1]==30 && lo[2]==0);
    Row(0,text); assert(!strcmp(text,"Sketch1 (XY, 1 profile)"));
    Row(1,text); assert(!strcmp(text,"  Rectangle 1"));
    Row(2,text); assert(!strcmp(text,"Extrusion1 (Sketch1, 40)"));
    assert(!cad_fui_row(3,text,260,&sketch2,&count));
    /* Telemetry word 2: count, error count, X extent, bounds, checksum. */
    t=cad_fui_telemetry(1);
    assert((t&31)==2 && ((t>>5)&31)==0 && !(t&(1UL<<10)) && (t&(1UL<<11)) && ((t>>12)&1023)==100 && (t&(1UL<<23)));
    assert((t>>24)==((t^(t>>8)^(t>>16)^0x5c)&255));
    /* Zoom all covers the body even without model vertices. */
    canvasWidth=586; canvasHeight=496; Fit(); assert(centerX==0 && centerY==0 && centerZ==20);
    /* Shaded mode is available for feature faces (not only the 747). */
    Command(1,ID_VIEW_SHADED); assert(shaded); Command(1,ID_VIEW_WIREFRAME); assert(!shaded);
    /* Associativity: changing W in Parameters resizes the extrusion. */
    SetParam("W","200"); assert(ExtentX()==200 && !cad_fui_errors());
    /* Error and blocked states: W too large for the sketch range. */
    SetParam("W","1000");
    assert(cad_fui_errors()==2 && !cad_fui_bounds(lo,hi));
    Row(0,text); assert(!strncmp(text,"(!) Sketch1 (XY, 1 profile) - profile 1: ",41));
    Row(1,text); assert(!strcmp(text,"  (!) Rectangle 1"));
    Row(2,text); assert(!strcmp(text,"(blocked) Extrusion1 (Sketch1, 40)"));
    t=cad_fui_telemetry(0); assert(((t>>5)&31)==2 && !(t&(1UL<<23)));
    SetParam("W","200"); assert(!cad_fui_errors() && ExtentX()==200);
    /* Suppress / unsuppress. */
    selectedFeature=extrude; selectedProfile=-1;
    Command(1,ID_FEATURE_SUPPRESS); Row(2,text);
    assert(!strcmp(text,"(suppressed) Extrusion1 (Sketch1, 40)") && !cad_fui_output()->face_count);
    Command(1,ID_FEATURE_SUPPRESS); Row(2,text); assert(!strstr(text,"suppressed") && cad_fui_output()->face_count);
    /* Delete a sketch in use: refused with an explanation (Del key path). */
    selectedFeature=sketch; selectedProfile=-1; oldErrors=errors;
    Command(1,ID_EDIT_DELETE); assert(errors==oldErrors+1 && strstr(lastMessage,"Extrusion1") && cad_fui_count()==2);
    /* Deleting the only profile (used through ALL) leaves the extrusion in
     * error; feature undo brings it back. */
    selectedProfile=0; oldErrors=errors;
    Command(1,ID_EDIT_DELETE); assert(errors==oldErrors && cad_fui_errors()==1 && selectedFeature==0);
    Row(1,text); assert(!strncmp(text,"(!) Extrusion1",14) && strstr(text,"no profiles"));
    Command(1,ID_FEATURE_UNDO); assert(!cad_fui_errors() && ExtentX()==200);
    /* Edit (double-click / Enter) prefills; distance W/2 -> 100. */
    selectedFeature=extrude; selectedProfile=-1;
    assert(Feature(ID_EDIT_PROPERTIES,NULL,"W/2",NULL,NULL,NULL,-1)==1 && TopZ()==100);
    assert(!strcmp(cad_fui_dialog.fields[0],"ALL") && cad_fui_dialog.sketch==sketch);
    /* A new error is rejected and the tree is unchanged. */
    oldErrors=errors; assert(Feature(ID_FEATURE_EDIT,NULL,"500",NULL,NULL,NULL,-1)==0);
    assert(errors==oldErrors+1 && strstr(lastMessage,"distance") && TopZ()==100);
    oldErrors=errors; assert(Feature(ID_FEATURE_EDIT,"7","40",NULL,NULL,NULL,-1)==0);
    assert(errors==oldErrors+1 && strstr(lastMessage,"Profile") && TopZ()==100);
    /* Feature undo reverts the last kept edit (distance back to 40). */
    Command(1,ID_FEATURE_UNDO); assert(TopZ()==40);
    /* Profile edit prefills the rectangle; reorder respects dependencies. */
    selectedFeature=sketch; selectedProfile=0;
    assert(Feature(ID_FEATURE_EDIT,NULL,NULL,"W/4",NULL,NULL,-1)==1 && ExtentX()==50);
    assert(!strcmp(cad_fui_dialog.fields[3],"60"));
    selectedFeature=extrude; selectedProfile=-1; oldErrors=errors;
    Command(1,ID_FEATURE_UP); assert(errors==oldErrors+1 && strstr(lastMessage,"reorder"));
    /* Rename, then dirty follows the exact feature text. */
    assert(Feature(ID_FEATURE_RENAME,"Body",NULL,NULL,NULL,NULL,-1)==1 && !strcmp(cad_fui_name(extrude),"Body"));
    /* Second sketch on XZ with a polygon; revolve a ring about V. */
    assert(Feature(ID_FEATURE_SKETCH,"10",NULL,NULL,NULL,NULL,CAD_FEAT_XZ)==1);
    sketch2=selectedFeature; assert(pitch==64 && cad_fui_active_sketch()==sketch2);
    assert(Feature(ID_FEATURE_POLY,NULL,NULL,NULL,NULL,"120,0; W/2+100 ,0;120,30",-1)==1);
    oldErrors=errors; assert(Feature(ID_FEATURE_POLY,NULL,NULL,NULL,NULL,"1,2;3",-1)==0);
    assert(errors==oldErrors+1 && strstr(lastMessage,"Points"));
    assert(Feature(ID_FEATURE_CIRCLE,"300","0","20","12",NULL,-1)==1);
    Command(1,ID_FEATURE_FINISH);
    assert(Feature(ID_FEATURE_REVOLVE,"1","12",NULL,NULL,NULL,CAD_FEAT_AXIS_V)==1);
    assert(cad_fui_count()==4 && !cad_fui_errors());
    oldErrors=errors; assert(Feature(ID_FEATURE_REVOLVE,"ALL","30",NULL,NULL,NULL,0)==0);
    assert(errors==oldErrors+1 && strstr(lastMessage,"Segments") && cad_fui_count()==4);
    /* Export/drawing materialize feature body edges (not sketch curves). */
    assert(Materialize() && materialVertices>8 && materialEdges>12 && !materialSkipped);
    lines=0; DrawSheet(1); assert(lines>0);
    /* Save / New / reopen round trip with [FEATURES]; dirty tracking. */
    strcpy(chosenPath,"FEAT.CAD"); filePath[0]=0; assert(SaveDocument(1,FALSE) && !Dirty());
    assert(strstr(extText,"[FEATURES]\nN ") && strstr(extText,"\"Body\""));
    selectedFeature=extrude; Command(1,ID_FEATURE_SUPPRESS); assert(Dirty());
    Command(1,ID_FEATURE_SUPPRESS); assert(!Dirty());
    count=(int)cad_fui_output()->segment_count;
    Command(1,ID_FILE_NEW); assert(!cad_fui_count() && !cad_fui_output()->segment_count && !Dirty());
    Command(1,ID_FILE_OPEN);
    assert(cad_fui_count()==4 && (int)cad_fui_output()->segment_count==count && !Dirty());
    assert(ExtentX()>=50);
    /* Undo ring and sketch mode do not survive a load. */
    assert(!cad_fui_can_undo() && !cad_fui_active_sketch());
    /* Associativity after reopen: W drives both sketches. */
    SetParam("W","100"); assert(Dirty());
    SetParam("W","200"); assert(!Dirty());
    /* Atomic load: a bad [FEATURES] keeps the whole current document. */
    assert(cad_document_save_ex("BADFEAT.CAD",&model,"[PARAMS]\nP Q 5\n[FEATURES]\nN 1\nF 1 BOGUS\n",
        strlen("[PARAMS]\nP Q 5\n[FEATURES]\nN 1\nF 1 BOGUS\n"))==CAD_DOC_OK);
    strcpy(chosenPath,"BADFEAT.CAD"); oldErrors=errors; Command(1,ID_FILE_OPEN);
    assert(errors==oldErrors+1 && strstr(lastMessage,"[FEATURES] line 2") && !strcmp(filePath,"FEAT.CAD"));
    assert(cad_fui_count()==4 && !cad_userparams_find(&userParams,"Q") && cad_userparams_find(&userParams,"W") && !Dirty());
    /* A [PARTS] failure after valid [FEATURES] restores the previous tree. */
    assert(cad_document_save_ex("BADFEAT.CAD",&model,"[FEATURES]\nN 1\n[PARTS]\nN 1\nO 1 0 0 0 0 0 X\n",
        strlen("[FEATURES]\nN 1\n[PARTS]\nN 1\nO 1 0 0 0 0 0 X\n"))==CAD_DOC_OK);
    oldErrors=errors; Command(1,ID_FILE_OPEN);
    assert(errors==oldErrors+1 && strstr(lastMessage,"[PARTS]") && cad_fui_count()==4 && !Dirty());
    /* Model undo moves geometry only; features stay. */
    assert(cad_create_line(&model,0,0,0,10,0,0)==CAD_OK);
    Command(1,ID_EDIT_UNDO); assert(!model.vertex_count && cad_fui_count()==4);
    /* Delete the revolve, then its sketch; feature undo restores it. */
    selectedFeature=cad_fui_features()->items[3].id; selectedProfile=-1;
    Command(1,ID_EDIT_DELETE); assert(cad_fui_count()==3 && !selectedFeature);
    selectedFeature=sketch2; Command(1,ID_FEATURE_DELETE); assert(cad_fui_count()==2);
    Command(1,ID_FEATURE_UNDO); assert(cad_fui_count()==3);
    /* New and demos clear the tree. */
    Command(1,ID_DEMO_HOUSE); assert(!cad_fui_count() && model.vertex_count==30);
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
    assert(cad_model_init(&model)==CAD_OK); cad_model_init(&stage);
    cad_fui_init(&userParams); ResetDocumentState(); MarkSaved();
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
    integration_tests();
    parts_tests();
    feature_tests();
    puts("cad editor: parsing, exact dirty tracking, 500-preview drag, cancel/capture, stable IDs, keyboard, clipping, file/demo commands, components, part features passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="cad-editor-") as temp:
    temp = Path(temp)
    c = temp / "controller.c"
    sheet_type = source[source.index("typedef struct SheetContext"):source.index("static int SheetLine")]
    (temp / "TABLE.CMD").write_bytes((root / "examples" / "TABLE.CMD").read_bytes())
    c.write_text('#define TABLE_PATH "TABLE.CMD"\n' + shim + defines + globals_ + sheet_type +
                 "".join(function(n) for n in sorted(names, key=position)) + tests)
    executable = temp / "controller"
    flags = ["-std=c89", "-pedantic", "-Wall", "-Wextra", "-Wno-unused-variable"]
    if os.environ.get("CAD_EDITOR_SANITIZE"):
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    subprocess.run(["cc", *flags, "-I", str(root), str(c),
                    *(str(root / f) for f in ["cad_model.c", "cad_create.c", "cad_sketch.c",
                                             "cad_document.c", "cad_export.c", "cad_drawing.c", "cad_aircraft.c", "cad_viewcube.c",
                                             "cad_params.c", "cad_userparams.c", "cad_constraints.c",
                                             "cad_vconstraints.c", "cad_script.c",
                                             "cad_assembly.c", "cad_parts.c",
                                             "cad_features.c", "cad_featureui.c"]),
                    "-o", str(executable)], check=True)
    subprocess.run([str(executable)], cwd=temp, check=True)

# Startup is structurally separate from the optional demo command.
startup = source[source.index("int PASCAL WinMain"):]
assert "cad_model_init(&model)" in startup and "cad_create_house_demo" not in startup
assert re.search(r"case ID_DEMO_HOUSE:\s*if \(!MayDiscard\(hwnd\)\) return;", source)
resources = (root / "cad.rc").read_text()
for command in ["ID_FILE_NEW", "ID_FILE_OPEN", "ID_FILE_SAVE", "ID_FILE_SAVEAS",
                "ID_EDIT_UNDO", "ID_EDIT_REDO", "ID_CREATE_LINE", "ID_CREATE_RECTANGLE",
                "ID_CREATE_BOX", "ID_CREATE_EXTRUDE", "ID_DEMO_HOUSE", "ID_INSERT_PLACE",
                "ID_INSERT_MOVE", "ID_INSERT_GROUND", "ID_INSERT_REMOVE", "ID_INSERT_RELOAD",
                "IDD_PLACEMENT"]:
    assert command in resources
print("cad editor: empty startup, guarded optional demo and native menu resource checks passed")
