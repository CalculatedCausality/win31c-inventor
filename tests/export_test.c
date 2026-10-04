/* Strict host: cc -std=c89 -pedantic -Wall -Wextra -Werror
 * tests/export_test.c -o /tmp/opencode/export_test
 * Run in an empty scratch directory. Implementation inclusion is only for
 * deterministic I/O fault injection, with no production test hooks.
 */
#ifndef __WATCOMC__
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <limits.h>

static int write_fail, close_fail, stream_fail, remove_fail, error_fail, writes;
static int injected_print(FILE *f, const char *fmt, ...)
{
    va_list args;
    int rc;
    ++writes;
    if (write_fail && writes == write_fail) {
        (void)fputs("partial", f);
        return -1;
    }
    va_start(args, fmt);
    rc = vfprintf(f, fmt, args);
    va_end(args);
    return rc;
}
static int injected_close(FILE *f)
{
    int rc;
    rc = fclose(f);
    return close_fail ? EOF : rc;
}
static FILE *injected_fdopen(int fd, const char *mode);
static int injected_error(FILE *f)
{
    return error_fail || ferror(f);
}
static int injected_remove(const char *path)
{
    if (remove_fail) { errno = EACCES; return -1; }
    return remove(path);
}
#define fprintf injected_print
#define fclose injected_close
#define fdopen injected_fdopen
#define remove injected_remove
#ifdef ferror
#undef ferror
#endif
#define ferror injected_error
#include "../cad_export.c"
#undef fprintf
#undef fclose
#undef fdopen
#undef remove
#undef ferror
static FILE *injected_fdopen(int fd, const char *mode)
{
    if (stream_fail) { errno = ENOMEM; return NULL; }
    return fdopen(fd, mode);
}

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static CadExportVertex vertices[257];
static CadExportEdge edges[513];
static const char * const expected_names[5] = {
    "wall", "roof", "door", "window", "chimney"
};

static void expect_xyz(int i, int axis, int x, int y, int z)
{
    CHECK(x == vertices[i].x);
    CHECK(y == (axis ? -vertices[i].z : vertices[i].y));
    CHECK(z == (axis ? vertices[i].y : vertices[i].z));
}

static void line_read(FILE *f, char *s, size_t n)
{
    size_t len;
    CHECK(fgets(s, (int)n, f) != NULL);
    len = strlen(s);
    CHECK(len && s[len - 1] == '\n');
    s[--len] = '\0';
    if (len && s[len - 1] == '\r') s[len - 1] = '\0';
}

static void parse_dxf(const char *path, int ne, int axis, int scale)
{
    FILE *f;
    char code[32], value[160], layer[32];
    int tag, count, in_line, fields, coords[6], layers, units, scaled, eof;
    int version, j, slot;
    f = fopen(path, "rb"); CHECK(f != NULL);
    count = in_line = fields = layers = units = scaled = eof = version = 0;
    layer[0] = '\0';
    while (fgets(code, sizeof(code), f)) {
        CHECK(sscanf(code, "%d", &tag) == 1);
        line_read(f, value, sizeof(value));
        if (tag == 0 && in_line) {
            CHECK(count < ne && fields == 63);
            CHECK(!strcmp(layer, expected_names[edges[count].style]));
            expect_xyz(edges[count].a, axis, coords[0], coords[1], coords[2]);
            expect_xyz(edges[count].b, axis, coords[3], coords[4], coords[5]);
            ++count; in_line = 0;
        }
        if (tag == 0 && !strcmp(value, "LINE")) {
            in_line = 1; fields = 0; layer[0] = '\0';
        } else if (in_line) {
            if (tag == 8) {
                CHECK(strlen(value) < sizeof(layer)); strcpy(layer, value);
            } else {
                slot = -1;
                if (tag == 10) slot = 0;
                if (tag == 20) slot = 1;
                if (tag == 30) slot = 2;
                if (tag == 11) slot = 3;
                if (tag == 21) slot = 4;
                if (tag == 31) slot = 5;
                CHECK(slot >= 0 && !(fields & (1 << slot)));
                CHECK(sscanf(value, "%d", &coords[slot]) == 1);
                fields |= 1 << slot;
            }
        }
        if (tag == 2) {
            for (j = 0; j < 5; ++j)
                if (!strcmp(value, expected_names[j])) layers |= 1 << j;
        }
        if (tag == 1 && !strcmp(value, "AC1009")) version = 1;
        if (tag == 999 && !strcmp(value, "Units: unspecified model units"))
            units = 1;
        if (tag == 999 && !strcmp(value,
            "Scale metadata only: 2/3 (coordinates unchanged)")) scaled = 1;
        if (tag == 0 && !strcmp(value, "EOF")) eof = 1;
    }
    CHECK(!ferror(f) && fclose(f) == 0);
    CHECK(count == ne && !in_line && layers == 31 && units && version && eof);
    CHECK(scaled == scale);
}

static void parse_obj(const char *path, int nv, int ne, int axis, int scale)
{
    FILE *f;
    char s[180], group[32];
    int vc, ec, x, y, z, a, b, units, scaled;
    f = fopen(path, "rb"); CHECK(f != NULL);
    vc = ec = units = scaled = 0; group[0] = '\0';
    while (fgets(s, sizeof(s), f)) {
        if (!strncmp(s, "v ", 2)) {
            CHECK(vc < nv && sscanf(s, "v %d %d %d", &x, &y, &z) == 3);
            expect_xyz(vc++, axis, x, y, z);
        } else if (!strncmp(s, "g ", 2)) {
            CHECK(sscanf(s, "g %31s", group) == 1);
        } else if (!strncmp(s, "l ", 2)) {
            CHECK(ec < ne && sscanf(s, "l %d %d", &a, &b) == 2);
            CHECK(a == edges[ec].a + 1 && b == edges[ec].b + 1);
            CHECK(a >= 1 && a <= nv && b >= 1 && b <= nv);
            CHECK(!strcmp(group, expected_names[edges[ec].style]));
            ++ec;
        } else {
            CHECK(s[0] == '#');
            if (!strcmp(s, "# Units: unspecified model units\n")) units = 1;
            if (!strcmp(s, "# Scale metadata only: 2/3 (coordinates unchanged)\n"))
                scaled = 1;
        }
    }
    CHECK(!ferror(f) && fclose(f) == 0);
    CHECK(vc == nv && ec == ne && units && scaled == scale);
}

static int output(int format, int nv, int ne, const CadExportOptions *o)
{
    return CadExportWrite("TEST.OUT", format, vertices, nv, edges, ne, o);
}

static void absent(void)
{
    struct stat st;
    CHECK(stat("TEST.OUT", &st) != 0 && errno == ENOENT);
}

static void sentinel(void)
{
    FILE *f;
    char s[32];
    f = fopen("TEST.OUT", "rb"); CHECK(f != NULL);
    CHECK(fgets(s, sizeof(s), f) && !strcmp(s, "do not touch\n"));
    CHECK(fgetc(f) == EOF && !ferror(f) && fclose(f) == 0);
}

int main(void)
{
    int i, axis, format, n, rc;
    FILE *f;
    CadExportOptions o;
    char longpath[129];
    absent();
    for (i = 0; i < 257; ++i) {
        vertices[i].x = i % 2 ? 400 : -400;
        vertices[i].y = i - 128; vertices[i].z = 300 - i;
    }
    vertices[2].y = vertices[2].z = 400;
    vertices[3].y = vertices[3].z = -400;
    for (i = 0; i < 513; ++i) {
        edges[i].a = i % 30; edges[i].b = (i + 7) % 30;
        edges[i].style = i % 5;
    }
    for (axis = 0; axis < 2; ++axis)
        for (format = 0; format < 2; ++format) {
            o.axis = axis; o.scale_numerator = 2; o.scale_denominator = 3;
            CHECK(output(format, 30, 40, &o) == CAD_EXPORT_OK);
            if (format == CAD_EXPORT_DXF) parse_dxf("TEST.OUT", 40, axis, 1);
            else parse_obj("TEST.OUT", 30, 40, axis, 1);
            CHECK(remove("TEST.OUT") == 0);
        }
    for (format = 0; format < 2; ++format)
        for (n = 0; n < 3; ++n) {
            rc = n == 0 ? 0 : n == 1 ? 7 : 512;
            CHECK(output(format, n == 2 ? 256 : 30, rc, NULL) == CAD_EXPORT_OK);
            if (format == CAD_EXPORT_DXF) parse_dxf("TEST.OUT", rc, 0, 0);
            else parse_obj("TEST.OUT", n == 2 ? 256 : 30, rc, 0, 0);
            CHECK(remove("TEST.OUT") == 0);
        }
    edges[0].a = edges[0].b = 255; edges[0].style = 4;
    CHECK(output(CAD_EXPORT_OBJ, 256, 1, NULL) == CAD_EXPORT_OK);
    parse_obj("TEST.OUT", 256, 1, 0, 0); CHECK(remove("TEST.OUT") == 0);
    edges[0].a = 0; edges[0].b = 7; edges[0].style = 0;
    CHECK(CadExportWrite("TEST.OUT", CAD_EXPORT_OBJ, NULL, 0, NULL, 0, NULL)
          == CAD_EXPORT_OK);
    parse_obj("TEST.OUT", 0, 0, 0, 0);
    CHECK(remove("TEST.OUT") == 0);
    o.axis = 0; o.scale_numerator = o.scale_denominator = 0;
    CHECK(CadExportWrite("TEST.OUT", CAD_EXPORT_DXF, NULL, 0, NULL, 0, &o)
          == CAD_EXPORT_OK);
    parse_dxf("TEST.OUT", 0, 0, 0); CHECK(remove("TEST.OUT") == 0);
    f = fopen("TEST.OUT", "wb"); CHECK(f != NULL);
    CHECK(fputs("do not touch\n", f) >= 0 && fclose(f) == 0);
    CHECK(output(0, 30, 40, NULL) == CAD_EXPORT_EXISTS); sentinel();
    CHECK(output(2, 30, 40, NULL) == CAD_EXPORT_INVALID); sentinel();
    CHECK(output(0, -1, 40, NULL) == CAD_EXPORT_INVALID);
    CHECK(output(0, 257, 40, NULL) == CAD_EXPORT_INVALID);
    CHECK(output(0, 30, -1, NULL) == CAD_EXPORT_INVALID);
    CHECK(output(0, 30, 513, NULL) == CAD_EXPORT_INVALID);
    CHECK(output(0, INT_MAX, 40, NULL) == CAD_EXPORT_INVALID);
    CHECK(output(0, 30, INT_MAX, NULL) == CAD_EXPORT_INVALID);
    CHECK(CadExportWrite("TEST.OUT", 0, NULL, 30, edges, 40, NULL) == CAD_EXPORT_INVALID);
    CHECK(CadExportWrite("TEST.OUT", 0, vertices, 30, NULL, 40, NULL) == CAD_EXPORT_INVALID);
    for (i = 0; i < 6; ++i) {
        if (i == 0) vertices[0].x = 401;
        if (i == 1) vertices[0].y = -401;
        if (i == 2) vertices[0].z = INT_MIN;
        if (i == 3) edges[39].a = -1;
        if (i == 4) edges[39].b = 30;
        if (i == 5) edges[39].style = 5;
        CHECK(output(0, 30, 40, NULL) == CAD_EXPORT_INVALID); sentinel();
        vertices[0].x = -400; vertices[0].y = -128; vertices[0].z = 300;
        edges[39].a = 9; edges[39].b = 16; edges[39].style = 4;
    }
    edges[39].style = -1;
    CHECK(output(0, 30, 40, NULL) == CAD_EXPORT_INVALID); sentinel();
    edges[39].style = 4;
    o.axis = 2; o.scale_numerator = o.scale_denominator = 0;
    CHECK(output(0, 30, 40, &o) == CAD_EXPORT_INVALID);
    o.axis = 0; o.scale_numerator = 1;
    CHECK(output(0, 30, 40, &o) == CAD_EXPORT_INVALID);
    o.scale_numerator = 0; o.scale_denominator = 1;
    CHECK(output(0, 30, 40, &o) == CAD_EXPORT_INVALID);
    o.scale_numerator = -1;
    CHECK(output(0, 30, 40, &o) == CAD_EXPORT_INVALID); sentinel();
    CHECK(remove("TEST.OUT") == 0);
    CHECK(output(0, 257, 0, NULL) == CAD_EXPORT_INVALID); absent();
    CHECK(CadExportWrite(NULL, 0, vertices, 30, edges, 40, NULL) == CAD_EXPORT_INVALID);
    CHECK(CadExportWrite("", 0, vertices, 30, edges, 40, NULL) == CAD_EXPORT_INVALID);
    CHECK(CadExportWrite("NUL.OBJ", 0, vertices, 30, edges, 40, NULL) == CAD_EXPORT_INVALID);
    CHECK(CadExportWrite("COM1", 0, vertices, 30, edges, 40, NULL) == CAD_EXPORT_INVALID);
    CHECK(CadExportWrite("TOOLONG99.OBJ", 0, vertices, 30, edges, 40, NULL) == CAD_EXPORT_INVALID);
    memset(longpath, 'a', 128); longpath[128] = '\0';
    CHECK(CadExportWrite(longpath, 0, vertices, 30, edges, 40, NULL) == CAD_EXPORT_INVALID);
    CHECK(CadExportWrite("MISSING/TEST.OBJ", 0, vertices, 30, edges, 40, NULL) == CAD_EXPORT_IO);
#ifndef __WATCOMC__
    /* O_EXCL must also refuse directories and dangling/ordinary symlinks. */
    CHECK(mkdir("TEST.OUT", 0700) == 0);
    CHECK(output(0, 30, 40, NULL) != CAD_EXPORT_OK);
    CHECK(rmdir("TEST.OUT") == 0);
    CHECK(symlink("NOFILE", "TEST.OUT") == 0);
    CHECK(output(0, 30, 40, NULL) == CAD_EXPORT_EXISTS);
    CHECK(unlink("TEST.OUT") == 0);
    CHECK(symlink("/dev/full", "TEST.OUT") == 0);
    CHECK(output(0, 30, 40, NULL) == CAD_EXPORT_EXISTS);
    CHECK(unlink("TEST.OUT") == 0);
#endif
    for (format = 0; format < 2; ++format) {
        writes = 0; CHECK(output(format, 30, 40, NULL) == CAD_EXPORT_OK);
        n = writes; CHECK(remove("TEST.OUT") == 0);
        for (i = 1; i <= n; ++i) {
            writes = 0; write_fail = i;
            CHECK(output(format, 30, 40, NULL) == CAD_EXPORT_IO); absent();
        }
        write_fail = 0; close_fail = 1;
        CHECK(output(format, 30, 40, NULL) == CAD_EXPORT_IO); absent();
        close_fail = 0;
        error_fail = 1;
        CHECK(output(format, 30, 40, NULL) == CAD_EXPORT_IO); absent();
        error_fail = 0;
    }
    stream_fail = 1;
    CHECK(output(0, 30, 40, NULL) == CAD_EXPORT_IO); absent(); stream_fail = 0;
    writes = 0; write_fail = 2; remove_fail = 1;
    CHECK(output(0, 30, 40, NULL) == CAD_EXPORT_CLEANUP);
    write_fail = remove_fail = 0; CHECK(remove("TEST.OUT") == 0);
    stream_fail = remove_fail = 1;
    CHECK(output(0, 30, 40, NULL) == CAD_EXPORT_CLEANUP);
    stream_fail = remove_fail = 0; CHECK(remove("TEST.OUT") == 0);
    puts("export tests passed (DXF/OBJ, bounds, axes, metadata, collisions, I/O)");
    return 0;
}
