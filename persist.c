#include "persist.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#define CAD_COUNT 90
#define CAD_SIZE 194
#define PATH_SIZE 128

static unsigned char wire[CAD_SIZE];
static int staged[CAD_COUNT];
static char target[PATH_SIZE], temporary[PATH_SIZE], backup[PATH_SIZE];

static int upper(int c)
{
    return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

static int namechar(int c)
{
    c = upper(c);
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
}

static BOOL paths(LPCSTR path)
{
    unsigned int n, base, dot, i, stem, ext;
    char name[9];
    if (!path) return FALSE;
    for (n = 0; n < PATH_SIZE && path[n]; ++n) { }
    if (!n || n == PATH_SIZE) return FALSE;
    base = 0;
    for (i = 0; i < n; ++i)
        if (path[i] == '/' || path[i] == '\\' || path[i] == ':')
            base = i + 1;
    dot = n;
    for (i = base; i < n; ++i) {
        if (path[i] == '.' && dot == n) dot = i;
        else if (!namechar((unsigned char)path[i])) return FALSE;
    }
    stem = dot - base;
    ext = dot == n ? 0 : n - dot - 1;
    if (!stem || stem > 8 || ext > 3 || (dot != n && !ext))
        return FALSE;
    for (i = 0; i < stem; ++i) name[i] = (char)upper(path[base + i]);
    name[stem] = '\0';
    if (!strcmp(name, "CON") || !strcmp(name, "PRN") ||
        !strcmp(name, "AUX") || !strcmp(name, "NUL") ||
        !strcmp(name, "CLOCK$") ||
        (stem == 4 && (!strncmp(name, "COM", 3) ||
                       !strncmp(name, "LPT", 3)) &&
         name[3] >= '1' && name[3] <= '9')) return FALSE;
    if (ext == 3 &&
        ((upper(path[dot + 1]) == 'T' && upper(path[dot + 2]) == 'M' &&
          upper(path[dot + 3]) == 'P') ||
         (upper(path[dot + 1]) == 'B' && upper(path[dot + 2]) == 'A' &&
          upper(path[dot + 3]) == 'K'))) return FALSE;
    if (dot + 4 >= PATH_SIZE) return FALSE;
    /* LPCSTR may be far in Win16 small model; copy explicitly. */
    for (i = 0; i <= n; ++i) target[i] = path[i];
    memcpy(temporary, target, dot);
    memcpy(backup, target, dot);
    strcpy(temporary + dot, ".TMP");
    strcpy(backup + dot, ".BAK");
    return TRUE;
}

/* 0 absent, 1 regular file, -1 directory/other/error. */
static int state(const char *path)
{
    struct stat s;
    if (stat(path, &s) != 0) return errno == ENOENT ? 0 : -1;
#ifdef S_ISREG
    return S_ISREG(s.st_mode) ? 1 : -1;
#else
    return (s.st_mode & S_IFMT) == S_IFREG ? 1 : -1;
#endif
}

static void putword(unsigned int at, unsigned int value)
{
    wire[at] = (unsigned char)(value & 255U);
    wire[at + 1] = (unsigned char)((value >> 8) & 255U);
}

static unsigned int getword(unsigned int at)
{
    return (unsigned int)wire[at] | ((unsigned int)wire[at + 1] << 8);
}

static unsigned int checksum(void)
{
    unsigned int crc, i, bit;
    crc = 65535U;
    for (i = 0; i < CAD_SIZE; ++i) {
        if (i == 12 || i == 13) continue;
        crc ^= (unsigned int)wire[i] << 8;
        for (bit = 0; bit < 8; ++bit)
            crc = ((crc & 32768U) ? (crc << 1) ^ 0x1021U : crc << 1)
                  & 65535U;
    }
    return crc;
}

BOOL CadRead(LPCSTR path, int *xyz, WORD count)
{
    FILE *f;
    BOOL ok;
    unsigned int i, value;
    if (!xyz || count != CAD_COUNT || !paths(path) || state(target) != 1)
        return FALSE;
    f = fopen(target, "rb");
    if (!f) return FALSE;
    ok = fread(wire, 1, CAD_SIZE, f) == CAD_SIZE;
    if (ok) ok = fgetc(f) == EOF && !ferror(f);
    if (fclose(f) != 0) ok = FALSE;
    if (!ok || memcmp(wire, "CADH", 4) || getword(4) != 1 ||
        getword(6) != 1 || getword(8) != CAD_COUNT || getword(10) != 180 ||
        getword(12) != checksum()) return FALSE;
    for (i = 0; i < CAD_COUNT; ++i) {
        value = getword(14 + 2 * i);
        /* Decode without unsigned-to-signed overflow on 16-bit int. */
        if (value <= 400U) staged[i] = (int)value;
        else if (value >= 65136U) staged[i] = -(int)(65535U - value) - 1;
        else return FALSE;
    }
    for (i = 0; i < CAD_COUNT; ++i) xyz[i] = staged[i];
    return TRUE;
}

BOOL CadWrite(LPCSTR path, const int *xyz, WORD count)
{
    FILE *f;
    BOOL ok;
    int old;
    unsigned int i, value;
    if (!xyz || count != CAD_COUNT || !paths(path)) return FALSE;
    for (i = 0; i < CAD_COUNT; ++i)
        if (xyz[i] < -400 || xyz[i] > 400) return FALSE;
    old = state(target);
    if (old < 0 || state(temporary) != 0 || state(backup) != 0) return FALSE;
    memcpy(wire, "CADH", 4);
    putword(4, 1); putword(6, 1); putword(8, CAD_COUNT); putword(10, 180);
    for (i = 0; i < CAD_COUNT; ++i) {
        value = xyz[i] < 0 ? 65535U - (unsigned int)(-(xyz[i] + 1)) :
                            (unsigned int)xyz[i];
        putword(14 + 2 * i, value);
    }
    putword(12, checksum());
    f = fopen(temporary, "wb");
    if (!f) return FALSE;
    ok = fwrite(wire, 1, CAD_SIZE, f) == CAD_SIZE;
    if (fclose(f) != 0) ok = FALSE;
    if (!ok) {
        if (remove(temporary) != 0) return FALSE;
        return FALSE;
    }
    if (old && rename(target, backup) != 0) {
        if (remove(temporary) != 0) return FALSE;
        return FALSE;
    }
    if (rename(temporary, target) != 0) {
        if (old && rename(backup, target) != 0) return FALSE;
        return FALSE; /* Keep complete .TMP for inspection/recovery. */
    }
    if (old && remove(backup) != 0) return FALSE;
    return TRUE;
}
