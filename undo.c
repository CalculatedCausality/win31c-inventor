/*
 * UNDO.EXE - Remove what WALLGEN.EXE left behind: reset the desktop
 * wallpaper to none and delete CLAUDE.BMP from the Windows directory.
 */
#include <windows.h>

int PASCAL WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine,
                   int nCmdShow)
{
    char szPath[144];
    OFSTRUCT of;
    int len;

    (void)hInst; (void)hPrev; (void)lpCmdLine; (void)nCmdShow;

    WriteProfileString("Desktop", "TileWallpaper", "0");
    SystemParametersInfo(SPI_SETDESKWALLPAPER, 0, "(None)",
                         SPIF_UPDATEINIFILE | SPIF_SENDWININICHANGE);

    len = GetWindowsDirectory(szPath, sizeof(szPath) - 12);
    if (len > 0 && szPath[len - 1] != '\\')
        szPath[len++] = '\\';
    lstrcpy(szPath + len, "CLAUDE.BMP");

    if (OpenFile(szPath, &of, OF_DELETE) == HFILE_ERROR)
        MessageBox(NULL, "Wallpaper reset. CLAUDE.BMP was not found.",
                   "Undo", MB_OK | MB_ICONINFORMATION);
    else
        MessageBox(NULL, "Wallpaper reset and CLAUDE.BMP deleted.",
                   "Undo", MB_OK | MB_ICONINFORMATION);
    return 0;
}
