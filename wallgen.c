/*
 * WALLGEN.EXE - Kaleidoscope wallpaper generator for Windows 3.1
 *
 * Builds a random 64x64 tile with 8-fold mirror symmetry (so it tiles
 * seamlessly) using only the 16 standard VGA colours, saves it as
 * CLAUDE.BMP in the Windows directory and makes it the tiled desktop
 * wallpaper.
 */
#include <windows.h>

#define TILE      64
#define ROWBYTES  (TILE / 2)            /* 4 bits per pixel */
#define BITSSIZE  (ROWBYTES * TILE)
#define PREVIEW   2                     /* preview scale */

#define IDC_NEW   101
#define IDC_EXIT  102

typedef struct {
    BITMAPINFOHEADER hdr;
    RGBQUAD          pal[16];
} BMI16;

/* Standard VGA palette, in the order Windows uses for 16-colour DIBs. */
static const BYTE vga[16][3] = {
    {0x00,0x00,0x00}, {0x80,0x00,0x00}, {0x00,0x80,0x00}, {0x80,0x80,0x00},
    {0x00,0x00,0x80}, {0x80,0x00,0x80}, {0x00,0x80,0x80}, {0xC0,0xC0,0xC0},
    {0x80,0x80,0x80}, {0xFF,0x00,0x00}, {0x00,0xFF,0x00}, {0xFF,0xFF,0x00},
    {0x00,0x00,0xFF}, {0xFF,0x00,0xFF}, {0x00,0xFF,0xFF}, {0xFF,0xFF,0xFF}
};

static BMI16         bmi;
static BYTE          bits[BITSSIZE];
static char          szPath[144];
static unsigned long seed;

static int Rand(int n)
{
    seed = seed * 1103515245UL + 12345UL;
    return (int)((seed >> 16) % (unsigned long)n);
}

static void InitBitmapInfo(void)
{
    int i;

    bmi.hdr.biSize = sizeof(BITMAPINFOHEADER);
    bmi.hdr.biWidth = TILE;
    bmi.hdr.biHeight = TILE;
    bmi.hdr.biPlanes = 1;
    bmi.hdr.biBitCount = 4;
    bmi.hdr.biCompression = BI_RGB;
    bmi.hdr.biSizeImage = BITSSIZE;
    bmi.hdr.biXPelsPerMeter = 0;
    bmi.hdr.biYPelsPerMeter = 0;
    bmi.hdr.biClrUsed = 16;
    bmi.hdr.biClrImportant = 16;
    for (i = 0; i < 16; i++) {
        bmi.pal[i].rgbRed = vga[i][0];
        bmi.pal[i].rgbGreen = vga[i][1];
        bmi.pal[i].rgbBlue = vga[i][2];
        bmi.pal[i].rgbReserved = 0;
    }
}

static void SetPixel4(int x, int y, int c)
{
    BYTE *p = &bits[y * ROWBYTES + x / 2];

    if (x & 1)
        *p = (BYTE)((*p & 0xF0) | c);
    else
        *p = (BYTE)((*p & 0x0F) | (c << 4));
}

static void GenerateTile(void)
{
    int colors[6];
    int ncolors, i, j, x, y, u, v, t;
    long a, b, c, d, shift;
    long f;

    ncolors = 3 + Rand(4);
    for (i = 0; i < ncolors; i++) {
    again:
        colors[i] = Rand(16);
        for (j = 0; j < i; j++)
            if (colors[j] == colors[i])
                goto again;
    }

    a = Rand(9) + 1;
    b = Rand(9) + 1;
    c = Rand(17) - 8;
    d = Rand(64);
    shift = 3 + Rand(5);

    for (y = 0; y < TILE; y++) {
        for (x = 0; x < TILE; x++) {
            /* Fold into one octant: mirror both axes, then the diagonal. */
            u = x < TILE / 2 ? x : TILE - 1 - x;
            v = y < TILE / 2 ? y : TILE - 1 - y;
            if (u > v) {
                t = u; u = v; v = t;
            }
            f = (a * u * u + b * v * v + c * u * v + d * (u + v)) >> shift;
            if (f < 0)
                f = -f;
            SetPixel4(x, y, colors[(int)(f % ncolors)]);
        }
    }
}

static BOOL WriteBmp(void)
{
    static BYTE fh[14];
    DWORD offBits = sizeof(fh) + sizeof(BMI16);
    DWORD size = offBits + BITSSIZE;
    HFILE hf;
    BOOL ok;

    fh[0] = 'B';
    fh[1] = 'M';
    fh[2] = (BYTE)size;
    fh[3] = (BYTE)(size >> 8);
    fh[4] = (BYTE)(size >> 16);
    fh[5] = (BYTE)(size >> 24);
    fh[10] = (BYTE)offBits;
    fh[11] = (BYTE)(offBits >> 8);
    fh[12] = (BYTE)(offBits >> 16);
    fh[13] = (BYTE)(offBits >> 24);

    hf = _lcreat(szPath, 0);
    if (hf == HFILE_ERROR)
        return FALSE;
    ok = _lwrite(hf, (LPCSTR)fh, sizeof(fh)) == sizeof(fh) &&
         _lwrite(hf, (LPCSTR)&bmi, sizeof(bmi)) == sizeof(bmi) &&
         _lwrite(hf, (LPCSTR)bits, BITSSIZE) == BITSSIZE;
    _lclose(hf);
    return ok;
}

static void ApplyWallpaper(HWND hwnd)
{
    if (!WriteBmp()) {
        MessageBox(hwnd, "Could not write the wallpaper file.", "Wallpaper",
                   MB_OK | MB_ICONEXCLAMATION);
        return;
    }
    WriteProfileString("Desktop", "TileWallpaper", "1");
    SystemParametersInfo(SPI_SETDESKWALLPAPER, 0, szPath,
                         SPIF_UPDATEINIFILE | SPIF_SENDWININICHANGE);
}

static void NewWallpaper(HWND hwnd)
{
    GenerateTile();
    InvalidateRect(hwnd, NULL, FALSE);
    ApplyWallpaper(hwnd);
}

LRESULT CALLBACK __export MainWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                      LPARAM lParam)
{
    PAINTSTRUCT ps;
    HINSTANCE hInst;
    int x, y;

    switch (msg) {
    case WM_CREATE:
        /* Don't take hInstance from the CREATESTRUCT: Watcom hoists the
         * far-pointer load of lParam above the switch, and loading a
         * non-pointer lParam into ES faults in protected mode. */
        hInst = (HINSTANCE)GetWindowWord(hwnd, GWW_HINSTANCE);
        CreateWindow("BUTTON", "&New", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                     BS_DEFPUSHBUTTON, 10, 2 * TILE * PREVIEW + 20, 120, 28,
                     hwnd, (HMENU)IDC_NEW, hInst, NULL);
        CreateWindow("BUTTON", "E&xit", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                     BS_PUSHBUTTON, 2 * TILE * PREVIEW - 110,
                     2 * TILE * PREVIEW + 20, 120, 28,
                     hwnd, (HMENU)IDC_EXIT, hInst, NULL);
        return 0;

    case WM_SETFOCUS:
        SetFocus(GetDlgItem(hwnd, IDC_NEW));
        return 0;

    case WM_COMMAND:
        if (wParam == IDC_NEW)
            NewWallpaper(hwnd);
        else if (wParam == IDC_EXIT)
            DestroyWindow(hwnd);
        return 0;

    case WM_PAINT:
        BeginPaint(hwnd, &ps);
        /* 2x2 tiles so you can see how it repeats. */
        for (y = 0; y < 2; y++)
            for (x = 0; x < 2; x++)
                StretchDIBits(ps.hdc, 10 + x * TILE * PREVIEW,
                              10 + y * TILE * PREVIEW, TILE * PREVIEW,
                              TILE * PREVIEW, 0, 0, TILE, TILE, bits,
                              (LPBITMAPINFO)&bmi, DIB_RGB_COLORS, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;

    case WM_DESTROY:
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
    RECT rc;
    int len;

    (void)lpCmdLine;
    seed = GetTickCount();
    InitBitmapInfo();

    len = GetWindowsDirectory(szPath, sizeof(szPath) - 12);
    if (len > 0 && szPath[len - 1] != '\\')
        szPath[len++] = '\\';
    lstrcpy(szPath + len, "CLAUDE.BMP");

    if (!hPrev) {
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = MainWndProc;
        wc.cbClsExtra = 0;
        wc.cbWndExtra = 0;
        wc.hInstance = hInst;
        wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = GetStockObject(LTGRAY_BRUSH);
        wc.lpszMenuName = NULL;
        wc.lpszClassName = "ClaudeWallGen";
        if (!RegisterClass(&wc))
            return 0;
    }

    rc.left = 0;
    rc.top = 0;
    rc.right = 2 * TILE * PREVIEW + 20;
    rc.bottom = 2 * TILE * PREVIEW + 58;
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    hwnd = CreateWindow("ClaudeWallGen", "Kaleidoscope Wallpaper",
                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                        WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
                        rc.right - rc.left, rc.bottom - rc.top,
                        NULL, NULL, hInst, NULL);
    if (!hwnd)
        return 0;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    NewWallpaper(hwnd);

    while (GetMessage(&msg, NULL, 0, 0)) {
        if (!IsDialogMessage(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    return msg.wParam;
}
