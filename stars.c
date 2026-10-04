/*
 * STARS.SCR - Starfield screen saver for Windows 3.1
 *
 * Written without SCRNSAVE.LIB: Control Panel runs the .SCR with "/s" to
 * start the saver and "/c" to configure it.  The module description must
 * begin with "SCRNSAVE :" for Control Panel to list it.
 */
#include <windows.h>
#include <stdlib.h>

#define NSTARS   90
#define DEPTH    1024
#define SPEED    24
#define TIMER_ID 1

typedef struct {
    int x, y, z;        /* position in space */
    int sx, sy, sz;     /* last plotted screen position/size, sx < 0 = none */
} STAR;

static STAR     stars[NSTARS];
static int      cxScreen, cyScreen;
static POINT    ptFirst;
static BOOL     fHaveFirst = FALSE;
static unsigned long seed;

static int Rand(int n)
{
    seed = seed * 1103515245UL + 12345UL;
    return (int)((seed >> 16) % (unsigned long)n);
}

static void ResetStar(STAR *s, BOOL anyDepth)
{
    s->x = Rand(2000) - 1000;
    s->y = Rand(2000) - 1000;
    s->z = anyDepth ? Rand(DEPTH - 1) + 1 : DEPTH;
}

static void StepStars(HDC hdc)
{
    HBRUSH hbrBlack = GetStockObject(BLACK_BRUSH);
    HBRUSH hbr;
    int i, sx, sy, sz;

    for (i = 0; i < NSTARS; i++) {
        STAR *s = &stars[i];

        if (s->sx >= 0) {
            SelectObject(hdc, hbrBlack);
            PatBlt(hdc, s->sx, s->sy, s->sz, s->sz, PATCOPY);
        }

        s->z -= SPEED;
        if (s->z <= 0)
            ResetStar(s, FALSE);

        sx = cxScreen / 2 + (int)((long)s->x * (cxScreen / 2) / s->z);
        sy = cyScreen / 2 + (int)((long)s->y * (cyScreen / 2) / s->z);
        if (sx < 0 || sy < 0 || sx >= cxScreen || sy >= cyScreen) {
            ResetStar(s, FALSE);
            s->sx = -1;
            continue;
        }

        if (s->z > DEPTH * 2 / 3) {
            hbr = GetStockObject(GRAY_BRUSH);
            sz = 1;
        } else if (s->z > DEPTH / 3) {
            hbr = GetStockObject(LTGRAY_BRUSH);
            sz = 2;
        } else {
            hbr = GetStockObject(WHITE_BRUSH);
            sz = 3;
        }
        SelectObject(hdc, hbr);
        PatBlt(hdc, sx, sy, sz, sz, PATCOPY);
        s->sx = sx;
        s->sy = sy;
        s->sz = sz;
    }
    SelectObject(hdc, hbrBlack);
}

LRESULT CALLBACK __export SaverWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                       LPARAM lParam)
{
    HDC hdc;
    int i;

    switch (msg) {
    case WM_CREATE:
        seed = GetTickCount();
        for (i = 0; i < NSTARS; i++) {
            ResetStar(&stars[i], TRUE);
            stars[i].sx = -1;
        }
        ShowCursor(FALSE);
        SetTimer(hwnd, TIMER_ID, 55, NULL);
        return 0;

    case WM_TIMER:
        hdc = GetDC(hwnd);
        StepStars(hdc);
        ReleaseDC(hwnd, hdc);
        return 0;

    case WM_MOUSEMOVE:
        if (!fHaveFirst) {
            ptFirst.x = LOWORD(lParam);
            ptFirst.y = HIWORD(lParam);
            fHaveFirst = TRUE;
        } else if (abs((int)LOWORD(lParam) - ptFirst.x) > 3 ||
                   abs((int)HIWORD(lParam) - ptFirst.y) > 3) {
            PostMessage(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        PostMessage(hwnd, WM_CLOSE, 0, 0);
        return 0;

    case WM_ACTIVATEAPP:
        if (!wParam)
            PostMessage(hwnd, WM_CLOSE, 0, 0);
        break;

    case WM_SYSCOMMAND:
        /* Swallow requests to start another saver while we run. */
        if ((wParam & 0xFFF0) == SC_SCREENSAVE)
            return 0;
        break;

    case WM_SETCURSOR:
        SetCursor(NULL);
        return TRUE;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ID);
        ShowCursor(TRUE);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static BOOL HasSwitch(LPSTR cmd, char sw)
{
    for (; *cmd; cmd++) {
        if ((*cmd == '/' || *cmd == '-') &&
            (cmd[1] == sw || cmd[1] == sw - 'a' + 'A'))
            return TRUE;
    }
    return FALSE;
}

int PASCAL WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine,
                   int nCmdShow)
{
    WNDCLASS wc;
    HWND hwnd;
    MSG msg;

    if (!HasSwitch(lpCmdLine, 's')) {
        MessageBox(NULL,
                   "Starfield screen saver\n\n"
                   "Written in C with Open Watcom.\n"
                   "There are no options to set.",
                   "Starfield", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    (void)nCmdShow;
    if (hPrev)
        return 0;

    wc.style = 0;
    wc.lpfnWndProc = SaverWndProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = hInst;
    wc.hIcon = NULL;
    wc.hCursor = NULL;
    wc.hbrBackground = GetStockObject(BLACK_BRUSH);
    wc.lpszMenuName = NULL;
    wc.lpszClassName = "ClaudeStarfield";
    if (!RegisterClass(&wc))
        return 0;

    cxScreen = GetSystemMetrics(SM_CXSCREEN);
    cyScreen = GetSystemMetrics(SM_CYSCREEN);
    hwnd = CreateWindowEx(WS_EX_TOPMOST, "ClaudeStarfield", "Starfield",
                          WS_POPUP | WS_VISIBLE, 0, 0, cxScreen, cyScreen,
                          NULL, NULL, hInst, NULL);
    if (!hwnd)
        return 0;
    SetFocus(hwnd);

    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return msg.wParam;
}
