#!/usr/bin/env python3
"""Run actual house.c editor routines with a small host-side Win16 shim.

This exercises model/gesture/clipping logic, not native Windows dialogs/GDI.
The generated C and executable live only in a temporary directory.
"""
from pathlib import Path
import re
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "house.c").read_text()


def function(name):
    match = re.search(r"static [^\n]+\b" + name + r"\([^;]*?\)\s*\{", source)
    assert match, name
    start = match.start()
    pos = source.index("{", match.start())
    depth = 1
    end = pos + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


shim = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef int BOOL;
typedef int HWND;
typedef int HDC;
typedef int HBITMAP;
typedef int HPEN;
typedef int HBRUSH;
typedef int HINSTANCE;
typedef uint32_t DWORD;
typedef unsigned short WORD;
#define TRUE 1
#define FALSE 0
#define max(a,b) ((a) > (b) ? (a) : (b))
#define min(a,b) ((a) < (b) ? (a) : (b))
#define DIST 600
#define GRIP 3
#define PICK 6
#define STATUS_H 20
#define SNAP 10
#define LIMIT 400
#define NONE (-1)
#define NEAR_Z 50
#define HISTORY 32
#define NPENS 6
#define MB_OK 0
#define MB_ICONEXCLAMATION 0
typedef struct { int x, y, z; } VEC;
static HWND capture;
static int lines, moveX, moveY;
static BOOL chooseOK = TRUE, readOK = TRUE, writeOK = TRUE, discardOK = TRUE;
static int readMode;
static HWND GetCapture(void) { return capture; }
static void ReleaseCapture(void) { capture = 0; }
static void InvalidateRect(HWND h, void *r, BOOL erase)
{ (void)h; (void)r; (void)erase; }
static void MoveTo(HDC h, int x, int y)
{ (void)h; moveX = x; moveY = y; }
static void LineTo(HDC h, int x, int y);
static void lstrcpy(char *to, const char *from) { strcpy(to, from); }
static int MessageBox(HWND h, const char *s, const char *t, int f)
{ (void)h; (void)s; (void)t; (void)f; return 0; }
static BOOL ChoosePath(HWND h, BOOL save, char *p)
{ (void)h; (void)save; strcpy(p, "C:\\TEST.H31"); return chooseOK; }
static BOOL MayDiscard(HWND h) { (void)h; return discardOK; }
static BOOL CadWrite(const char *p, const int *xyz, WORD count)
{ (void)p; assert(count == 90); assert(xyz[0] >= -400); return writeOK; }
static BOOL CadRead(const char *p, int *xyz, WORD count)
{
    int i;
    (void)p; assert(count == 90);
    for (i = 0; i < 90; ++i) xyz[i] = readMode == 1 ? 401 : 12;
    return readOK;
}
'''

constants = source[source.index("static const int quarter"):source.index("static const EDGE edges")]
globals_ = source[source.index("static VEC      verts"):source.index("static int ISin")]
names = ["InitHistory", "Dirty", "Commit", "EndGesture", "ISin", "ICos",
         "ToCamera", "Clamp", "SnapTo", "Project", "OutCode", "Interp",
         "DrawEdge", "HitTest", "StopSpin", "Nudge", "Reset", "OnMouseMove",
         "SaveFile", "LoadHouse"]
# The globals section already contains the first four definitions.
functions = "".join(function(name) for name in names[4:])

tests = r'''
static void LineTo(HDC h, int x, int y)
{
    (void)h;
    assert(moveX >= 0 && moveX < cxClient);
    assert(moveY >= 0 && moveY < cyClient - STATUS_H);
    assert(x >= 0 && x < cxClient);
    assert(y >= 0 && y < cyClient - STATUS_H);
    ++lines;
}
static void BeginGrip(void)
{
    dragTarget = 0; sel = 0; dragStart = verts[0];
    mx0 = 100; my0 = 100;
    dragDepth = 530; dragFocal = 600;
    dragCos = ICos(yaw); dragSin = ISin(yaw);
    dragCP = ICos(pitch); dragSP = ISin(pitch);
    dragSnap = fSnap; gesture = G_GRIP; capture = 1;
}
int main(void)
{
    int i, j, before, a, b;
    VEC original, moved, staged[NVERT];
    DWORD id;
    Reset(); InitHistory();
    assert(!fSpin && !Dirty());
    Commit(); assert(histPos == 0);
    for (i = 1; i <= 40; ++i) { verts[0].x = i; Commit(); }
    assert(histPos == 32 && histLast == 32);
    assert(history[0][0].x == 8 && history[32][0].x == 40);
    savedId = stateIds[histPos]; assert(!Dirty());
    --histPos; memcpy(verts, history[histPos], sizeof(verts)); assert(Dirty());
    ++histPos; memcpy(verts, history[histPos], sizeof(verts)); assert(!Dirty());
    id = stateIds[histPos];
    --histPos; memcpy(verts, history[histPos], sizeof(verts));
    verts[0].x = 77; Commit(); assert(histPos == histLast);
    assert(stateIds[histPos] != id && Dirty());

    Reset(); InitHistory(); original = verts[0];
    fSnap = TRUE; BeginGrip();
    OnMouseMove(1, 100, 100); EndGesture(1, TRUE);
    assert(!memcmp(&verts[0], &original, sizeof(original)) && histPos == 0);
    BeginGrip();
    for (i = 0; i < 10; ++i) OnMouseMove(1, 130 + i, 105);
    OnMouseMove(1, 100, 100); EndGesture(1, TRUE);
    assert(!memcmp(&verts[0], &original, sizeof(original)) && histPos == 0);
    fSnap = FALSE; BeginGrip();
    sel = 1; yaw = 128; pitch = -20;
    OnMouseMove(1, 150, 110); assert(verts[1].x == houseInit[1].x);
    assert(verts[0].x != original.x);
    moved = verts[0]; yaw = 0; pitch = 64; fSnap = TRUE;
    OnMouseMove(1, 150, 110);
    assert(!memcmp(&moved, &verts[0], sizeof(moved)));
    EndGesture(1, FALSE); assert(verts[0].x == original.x && histPos == 0);
    BeginGrip(); OnMouseMove(1, 150, 110); EndGesture(1, TRUE);
    assert(histPos == 1 && Dirty());
    before = histPos; BeginGrip(); OnMouseMove(1, 140, 120);
    capture = 2; OnMouseMove(1, 180, 130);
    assert(gesture == G_NONE && histPos == before && capture == 2);
    assert(!memcmp(verts, history[histPos], sizeof(verts)));
    BeginGrip(); OnMouseMove(1, -32768, 32767); EndGesture(1, TRUE);
    assert(abs(verts[0].x) <= 400 && abs(verts[0].y) <= 400 && abs(verts[0].z) <= 400);
    gesture = G_ORBIT; capture = 1; yaw0 = 32; pitch0 = 20;
    mx0 = -32768; my0 = -32768;
    OnMouseMove(1, 32767, 32767);
    assert(yaw >= 0 && yaw < 256 && pitch == 64);
    EndGesture(1, FALSE); assert(yaw == 32 && pitch == 20);
    for (j = 0; j < 256; ++j) {
        yaw = j; pitch = j % 129 - 64; fSnap = j & 1;
        original = verts[0]; before = histPos;
        BeginGrip(); OnMouseMove(1, 100, 100); EndGesture(1, TRUE);
        assert(!memcmp(&original, &verts[0], sizeof(original)) && histPos == before);
    }

    Reset(); InitHistory();
    sel = 0; Nudge(1, 0, 0); Commit(); assert(histPos == 1);
    writeOK = FALSE; assert(!SaveFile(1, FALSE) && Dirty() && !filePath[0]);
    writeOK = TRUE; chooseOK = FALSE; assert(!SaveFile(1, FALSE) && Dirty());
    chooseOK = TRUE; assert(SaveFile(1, FALSE) && !Dirty());
    assert(!strcmp(filePath, "C:\\TEST.H31"));
    memcpy(staged, verts, sizeof(verts)); before = histPos; id = savedId;
    readOK = FALSE; LoadHouse(1);
    assert(!memcmp(staged, verts, sizeof(verts)) && histPos == before && savedId == id);
    readOK = TRUE; readMode = 1; LoadHouse(1);
    assert(!memcmp(staged, verts, sizeof(verts)) && histPos == before);
    readMode = 0; discardOK = FALSE; LoadHouse(1);
    assert(!memcmp(staged, verts, sizeof(verts)));
    discardOK = TRUE; LoadHouse(1);
    assert(verts[0].x == 12 && histPos == 0 && histLast == 0 && !Dirty());

    cxClient = 500; cyClient = 420; midX = 250; midY = 200; focal = 600;
    viewport = TRUE; Reset(); yaw = 0; pitch = 0; Project();
    assert(valid[0] && HitTest(sx[0], sy[0]) == 0);
    assert(HitTest(250, 419) == NONE);
    viewport = FALSE; Project();
    for (i = 0; i < (int)NVERT; ++i) assert(!valid[i]);
    assert(HitTest(0, 0) == NONE);
    viewport = TRUE;
    camX[0] = 0; camY[0] = 0; camZ[0] = 580;
    camX[1] = 20; camY[1] = 20; camZ[1] = 0;
    lines = 0; DrawEdge(1, 0, 1); assert(lines == 1);
    camZ[1] = 590; lines = 0; DrawEdge(1, 0, 1); assert(lines == 0);
    camZ[0] = camZ[1] = 0;
    camX[0] = -400; camX[1] = 400; camY[0] = camY[1] = 0;
    focal = 8192; lines = 0; DrawEdge(1, 0, 1); assert(lines == 1);
    for (j = 0; j < 256; ++j) {
        yaw = j; pitch = j % 129 - 64;
        for (i = 0; i < (int)NVERT; ++i) {
            verts[i].x = i & 1 ? 400 : -400;
            verts[i].y = i & 2 ? 400 : -400;
            verts[i].z = i & 4 ? 400 : -400;
        }
        Project();
        for (a = 0; a < (int)NVERT; ++a)
            for (b = a + 1; b < (int)NVERT; ++b) DrawEdge(1, a, b);
    }
    puts("editor invariants: PASS (history, gestures, staging, clipping)");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="house-editor-") as directory:
    path = Path(directory)
    (path / "editor.c").write_text(shim + constants + globals_ + functions + tests)
    subprocess.run(["cc", "-std=c89", "-pedantic-errors", "-fsanitize=undefined",
                    "-fno-sanitize-recover=all", "-g", str(path / "editor.c"),
                    "-o", str(path / "editor")], check=True)
    subprocess.run([str(path / "editor")], check=True)
