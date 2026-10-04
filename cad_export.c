#ifndef __WATCOMC__
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif
#include "cad_export.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef __WATCOMC__
#include <io.h>
#define CAD_OPEN_MODE (S_IREAD | S_IWRITE)
#define CAD_OPEN_FLAGS (O_WRONLY | O_CREAT | O_EXCL | O_BINARY)
#else
#include <unistd.h>
#define CAD_OPEN_MODE 0666
#define CAD_OPEN_FLAGS (O_WRONLY | O_CREAT | O_EXCL)
#endif

static const char * const cad_names[5] = {
    "wall", "roof", "door", "window", "chimney"
};

static int cad_upper(int c)
{
    return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

static int cad_namechar(int c)
{
    c = cad_upper(c);
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
}

static int cad_path_valid(const char *path)
{
    int n, base, dot, i, stem, ext;
    char name[9];
    if (!path) return 0;
    for (n = 0; n <= CAD_EXPORT_MAX_PATH && path[n]; ++n) { }
    if (!n || n > CAD_EXPORT_MAX_PATH) return 0;
    base = 0;
    for (i = 0; i < n; ++i)
        if (path[i] == '/' || path[i] == '\\' || path[i] == ':')
            base = i + 1;
    dot = n;
    for (i = base; i < n; ++i) {
        if (path[i] == '.' && dot == n) dot = i;
        else if (!cad_namechar((unsigned char)path[i])) return 0;
    }
    stem = dot - base;
    ext = dot == n ? 0 : n - dot - 1;
    if (stem < 1 || stem > 8 || ext > 3 || (dot != n && !ext)) return 0;
    for (i = 0; i < stem; ++i) name[i] = (char)cad_upper(path[base + i]);
    name[stem] = '\0';
    if (!strcmp(name, "CON") || !strcmp(name, "PRN") ||
        !strcmp(name, "AUX") || !strcmp(name, "NUL") ||
        (stem == 4 && (!strncmp(name, "COM", 3) ||
                       !strncmp(name, "LPT", 3)) &&
         name[3] >= '1' && name[3] <= '9')) return 0;
    return 1;
}

static int cad_model_valid(const CadExportVertex *v, int nv,
                           const CadExportEdge *e, int ne,
                           const CadExportOptions *o)
{
    int i;
    if (nv < 0 || nv > CAD_EXPORT_MAX_VERTICES ||
        ne < 0 || ne > CAD_EXPORT_MAX_EDGES ||
        (nv && !v) || (ne && !e)) return 0;
    if (o && ((o->axis != CAD_EXPORT_Y_UP && o->axis != CAD_EXPORT_Z_UP) ||
        !((o->scale_numerator == 0 && o->scale_denominator == 0) ||
          (o->scale_numerator > 0 && o->scale_denominator > 0)))) return 0;
    for (i = 0; i < nv; ++i)
        if (v[i].x < -400 || v[i].x > 400 ||
            v[i].y < -400 || v[i].y > 400 ||
            v[i].z < -400 || v[i].z > 400) return 0;
    for (i = 0; i < ne; ++i)
        if (e[i].a < 0 || e[i].a >= nv || e[i].b < 0 || e[i].b >= nv ||
            e[i].style < CAD_EXPORT_WALL || e[i].style > CAD_EXPORT_CHIMNEY)
            return 0;
    return 1;
}

static void cad_map(const CadExportVertex *v, int axis, int *x, int *y, int *z)
{
    *x = v->x;
    *y = axis == CAD_EXPORT_Z_UP ? -v->z : v->y;
    *z = axis == CAD_EXPORT_Z_UP ? v->y : v->z;
}

static int cad_emit(FILE *f, int format, const CadExportVertex *v, int nv,
                    const CadExportEdge *e, int ne, const CadExportOptions *o)
{
    int i, axis, x, y, z, bx, by, bz, last_style;
    const char *comment;
    axis = o ? o->axis : CAD_EXPORT_Y_UP;
    comment = format == CAD_EXPORT_DXF ? "999\n" : "# ";
    if (fprintf(f, "%sUnits: unspecified model units\n%sAxis: %s\n",
                comment, comment, axis == CAD_EXPORT_Z_UP ?
                "Z-up (x,-z,y)" : "Y-up (x,y,z)") < 0) return 0;
    if (o && o->scale_numerator &&
        fprintf(f, "%sScale metadata only: %ld/%ld (coordinates unchanged)\n",
                comment, o->scale_numerator, o->scale_denominator) < 0)
        return 0;
    if (format == CAD_EXPORT_DXF) {
        if (fprintf(f, "0\nSECTION\n2\nHEADER\n9\n$ACADVER\n1\nAC1009\n"
                       "0\nENDSEC\n0\nSECTION\n2\nTABLES\n0\nTABLE\n"
                       "2\nLAYER\n70\n5\n") < 0) return 0;
        for (i = 0; i < 5; ++i)
            if (fprintf(f, "0\nLAYER\n2\n%s\n70\n0\n62\n%d\n6\nCONTINUOUS\n",
                        cad_names[i], i + 1) < 0) return 0;
        if (fprintf(f, "0\nENDTAB\n0\nENDSEC\n0\nSECTION\n2\nENTITIES\n") < 0)
            return 0;
        for (i = 0; i < ne; ++i) {
            cad_map(v + e[i].a, axis, &x, &y, &z);
            cad_map(v + e[i].b, axis, &bx, &by, &bz);
            if (fprintf(f, "0\nLINE\n8\n%s\n10\n%d\n20\n%d\n30\n%d\n"
                           "11\n%d\n21\n%d\n31\n%d\n",
                        cad_names[e[i].style], x, y, z, bx, by, bz) < 0)
                return 0;
        }
        return fprintf(f, "0\nENDSEC\n0\nEOF\n") >= 0;
    }
    for (i = 0; i < nv; ++i) {
        cad_map(v + i, axis, &x, &y, &z);
        if (fprintf(f, "v %d %d %d\n", x, y, z) < 0) return 0;
    }
    last_style = -1;
    for (i = 0; i < ne; ++i) {
        if (e[i].style != last_style) {
            if (fprintf(f, "g %s\n", cad_names[e[i].style]) < 0) return 0;
            last_style = e[i].style;
        }
        if (fprintf(f, "l %d %d\n", e[i].a + 1, e[i].b + 1) < 0) return 0;
    }
    return 1;
}

int CadExportWrite(const char *path, int format,
                   const CadExportVertex *vertices, int vertex_count,
                   const CadExportEdge *edges, int edge_count,
                   const CadExportOptions *options)
{
    int fd, ok, close_result;
    FILE *f;
    if (!cad_path_valid(path) ||
        (format != CAD_EXPORT_DXF && format != CAD_EXPORT_OBJ) ||
        !cad_model_valid(vertices, vertex_count, edges, edge_count, options))
        return CAD_EXPORT_INVALID;
    fd = open(path, CAD_OPEN_FLAGS, CAD_OPEN_MODE);
    if (fd < 0) return errno == EEXIST ? CAD_EXPORT_EXISTS : CAD_EXPORT_IO;
    f = fdopen(fd, "wb");
    if (!f) {
        close_result = close(fd);
        if (remove(path) != 0) return CAD_EXPORT_CLEANUP;
        (void)close_result; /* Already an I/O failure, regardless of close. */
        return CAD_EXPORT_IO;
    }
    ok = cad_emit(f, format, vertices, vertex_count, edges, edge_count, options);
    if (ferror(f)) ok = 0;
    if (fclose(f) != 0) ok = 0;
    if (ok) return CAD_EXPORT_OK;
    return remove(path) == 0 ? CAD_EXPORT_IO : CAD_EXPORT_CLEANUP;
}
