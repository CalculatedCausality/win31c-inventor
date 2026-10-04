/* Host:
 * cc -std=c89 -pedantic -Wall -Wextra -Werror cad_sketch.c \
 *    tests/sketch_test.c -o /tmp/opencode/sketch_test
 * No Win32/Win16 headers or test stubs needed. */
#include "../cad_sketch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static CadSketchMesh mesh, saved, changed, second;
static unsigned long checks;
static const CadSketchPoint rectangle[] = {{0,0},{100,0},{100,50},{0,50}};
static const CadSketchPoint elbow[] = {
    {0,0},{100,0},{100,40},{40,40},{40,100},{0,100}
};

static long xy_area(const CadSketchVertex *a, const CadSketchVertex *b,
                    const CadSketchVertex *c)
{
    return ((long)b->x - (long)a->x) * ((long)c->y - (long)a->y) -
           ((long)b->y - (long)a->y) * ((long)c->x - (long)a->x);
}

/* Independent tetrahedral determinant sum, rather than Z-divergence.
 * Used only for our small bounded extrusions (not arbitrary mesh inputs). */
static long tetra_volume6(const CadSketchMesh *m)
{
    int i;
    long sum = 0L;
    const CadSketchTriangle *t;
    const CadSketchVertex *a, *b, *c;
    for (i = 0; i < m->triangle_count; ++i) {
        t = &m->triangles[i];
        a = &m->vertices[t->a]; b = &m->vertices[t->b]; c = &m->vertices[t->c];
        sum += (long)a->x * ((long)b->y * c->z - (long)b->z * c->y) +
               (long)a->y * ((long)b->z * c->x - (long)b->x * c->z) +
               (long)a->z * ((long)b->x * c->y - (long)b->y * c->x);
    }
    return sum;
}

static void successful(const CadSketchPoint *p, int n, int h, long area2)
{
    int i, j, top = 0, bottom = 0, sides = 0, matches;
    long area = 0L, volume = -1L, top_area = 0L, bottom_area = 0L, cross;
    const CadSketchTriangle *t;
    const CadSketchVertex *a, *b, *c;
    CHECK(cad_sketch_validate(p, n, &area) == CAD_SKETCH_OK);
    CHECK(area == area2 || area == -area2);
    CHECK(cad_sketch_extrude(p, n, h, &mesh) == CAD_SKETCH_OK);
    CHECK(mesh.kind == CAD_SKETCH_CLOSED_MESH);
    CHECK(mesh.vertex_count == 2 * n);
    CHECK(mesh.triangle_count == 4 * n - 4);
    CHECK(cad_sketch_mesh_validate(&mesh) == CAD_SKETCH_OK);
    CHECK(cad_sketch_mesh_volume6(&mesh, &volume) == CAD_SKETCH_OK);
    CHECK(volume == area2 * (long)h * 3L);
    CHECK(tetra_volume6(&mesh) == volume);
    for (i = 0; i < n; ++i) {
        matches = 0;
        for (j = 0; j < n; ++j)
            if (mesh.vertices[i].x == p[j].x && mesh.vertices[i].y == p[j].y)
                ++matches;
        CHECK(matches == 1);
        CHECK(mesh.vertices[i].z == 0);
        CHECK(mesh.vertices[i + n].x == mesh.vertices[i].x);
        CHECK(mesh.vertices[i + n].y == mesh.vertices[i].y);
        CHECK(mesh.vertices[i + n].z == h);
    }
    for (i = 0; i < mesh.triangle_count; ++i) {
        t = &mesh.triangles[i];
        a = &mesh.vertices[t->a]; b = &mesh.vertices[t->b]; c = &mesh.vertices[t->c];
        cross = xy_area(a, b, c);
        if (a->z == h && b->z == h && c->z == h) {
            CHECK(cross > 0L); top_area += cross; ++top;
        } else if (a->z == 0 && b->z == 0 && c->z == 0) {
            CHECK(cross < 0L); bottom_area += cross; ++bottom;
        } else {
            CHECK(cross == 0L); ++sides;
        }
    }
    CHECK(top == n - 2 && bottom == n - 2 && sides == 2 * n);
    CHECK(top_area == area2 && bottom_area == -area2);
    ++checks;
}

static void rejected(const CadSketchPoint *p, int n, int h,
                     CadSketchStatus expected)
{
    memset(&mesh, 0xa5, sizeof(mesh));
    memcpy(&saved, &mesh, sizeof(mesh));
    CHECK(cad_sketch_extrude(p, n, h, &mesh) == expected);
    CHECK(memcmp(&saved, &mesh, sizeof(mesh)) == 0);
    ++checks;
}

static void profiles(void)
{
    const CadSketchPoint bowtie[] = {{0,0},{100,100},{0,100},{100,0}};
    const CadSketchPoint duplicate[] = {{0,0},{100,0},{100,100},{0,0},{0,100}};
    const CadSketchPoint zero_edge[] = {{0,0},{100,0},{100,0},{0,100}};
    const CadSketchPoint repeat_close[] = {{0,0},{100,0},{0,100},{0,0}};
    const CadSketchPoint straight[] = {{0,0},{50,0},{100,0},{100,100},{0,100}};
    const CadSketchPoint backtrack[] = {{0,0},{100,0},{50,0},{100,100},{0,100}};
    const CadSketchPoint touch[] = {{0,0},{100,0},{100,100},{50,0},{0,100}};
    const CadSketchPoint overlap[] = {
        {0,0},{100,0},{100,100},{20,100},{20,0},{80,0},{80,50},{0,50}
    };
    const CadSketchPoint flat[] = {{0,0},{50,0},{100,0}};
    const CadSketchPoint triangle[] = {{-400,-400},{400,-400},{0,400}};
    CadSketchPoint p[CAD_SKETCH_MAX_POINTS + 1];
    long area;
    int i, j, reverse, shift;
    successful(rectangle, 4, 25, 10000L);
    successful(elbow, 6, 30, 12800L);
    successful(straight, 5, 10, 20000L);
    successful(triangle, 3, 400, 640000L);
    for (reverse = 0; reverse < 2; ++reverse)
        for (shift = 0; shift < 6; ++shift) {
            for (i = 0; i < 6; ++i) {
                j = reverse ? (shift + 6 - i) % 6 : (shift + i) % 6;
                p[i].x = elbow[j].x - 250;
                p[i].y = elbow[j].y - 200;
            }
            successful(p, 6, 400, 12800L);
        }
    rejected(bowtie, 4, 10, CAD_SKETCH_INTERSECTION);
    rejected(duplicate, 5, 10, CAD_SKETCH_DUPLICATE);
    rejected(zero_edge, 4, 10, CAD_SKETCH_ZERO_EDGE);
    rejected(repeat_close, 4, 10, CAD_SKETCH_ZERO_EDGE);
    rejected(backtrack, 5, 10, CAD_SKETCH_INTERSECTION);
    rejected(touch, 5, 10, CAD_SKETCH_INTERSECTION);
    rejected(overlap, 8, 10, CAD_SKETCH_INTERSECTION);
    rejected(flat, 3, 10, CAD_SKETCH_INTERSECTION);
    rejected(rectangle, 4, 0, CAD_SKETCH_RANGE);
    rejected(rectangle, 4, -1, CAD_SKETCH_RANGE);
    rejected(rectangle, 4, INT_MIN, CAD_SKETCH_RANGE);
    rejected(rectangle, 4, 401, CAD_SKETCH_RANGE);
    rejected(rectangle, 4, INT_MAX, CAD_SKETCH_RANGE);
    rejected(rectangle, 2, 10, CAD_SKETCH_COMPLEXITY);
    rejected(rectangle, -1, 10, CAD_SKETCH_COMPLEXITY);
    /* Count rejection occurs before reading even one coordinate. */
    rejected(rectangle, 65, 10, CAD_SKETCH_COMPLEXITY);
    rejected(0, 4, 10, CAD_SKETCH_ARGUMENT);
    CHECK(cad_sketch_extrude(rectangle, 4, 10, 0) == CAD_SKETCH_ARGUMENT);
    for (i = 0; i < 4; ++i) p[i] = rectangle[i];
    p[0].x = 401; rejected(p, 4, 10, CAD_SKETCH_RANGE);
    p[0].x = -401; rejected(p, 4, 10, CAD_SKETCH_RANGE);
    p[0].x = INT_MIN; rejected(p, 4, 10, CAD_SKETCH_RANGE);
    p[0].x = INT_MAX; rejected(p, 4, 10, CAD_SKETCH_RANGE);
    p[0].x = 0; p[0].y = 401; rejected(p, 4, 10, CAD_SKETCH_RANGE);
    area = 123L;
    CHECK(cad_sketch_validate(bowtie, 4, &area) == CAD_SKETCH_INTERSECTION);
    CHECK(area == 123L);
    CHECK(cad_sketch_validate(rectangle, 4, 0) == CAD_SKETCH_OK);
    /* Exact capacity: 16 straight-through points on each square side. */
    for (i = 0; i < 16; ++i) {
        p[i].x = -400 + 50 * i; p[i].y = -400;
        p[i + 16].x = 400; p[i + 16].y = -400 + 50 * i;
        p[i + 32].x = 400 - 50 * i; p[i + 32].y = 400;
        p[i + 48].x = -400; p[i + 48].y = 400 - 50 * i;
    }
    successful(p, 64, 400, 1280000L);
    CHECK(mesh.vertex_count == CAD_SKETCH_MAX_VERTICES);
    CHECK(mesh.triangle_count == CAD_SKETCH_MAX_TRIANGLES);
    for (i = 0; i < 32; ++i) {
        CadSketchPoint temp;
        temp = p[i]; p[i] = p[63 - i]; p[63 - i] = temp;
    }
    successful(p, 64, 400, 1280000L);
}

static void bad_mesh(const CadSketchMesh *m, CadSketchStatus expected)
{
    long volume = 987654L;
    CHECK(cad_sketch_mesh_validate(m) == expected);
    CHECK(cad_sketch_mesh_volume6(m, &volume) == expected);
    CHECK(volume == 987654L);
    ++checks;
}

static void topology(void)
{
    int i;
    unsigned short swap;
    long volume;
    successful(rectangle, 4, 25, 10000L);
    saved = mesh;
    changed = saved; changed.kind = CAD_SKETCH_WIREFRAME;
    bad_mesh(&changed, CAD_SKETCH_NOT_CLOSED);
    changed = saved; changed.kind = CAD_SKETCH_EMPTY;
    bad_mesh(&changed, CAD_SKETCH_NOT_CLOSED);
    changed = saved; changed.vertex_count = 129;
    bad_mesh(&changed, CAD_SKETCH_COMPLEXITY);
    changed = saved; changed.triangle_count = 253;
    bad_mesh(&changed, CAD_SKETCH_COMPLEXITY);
    changed = saved; changed.vertices[0].z = -401;
    bad_mesh(&changed, CAD_SKETCH_RANGE);
    changed = saved; changed.triangles[0].a = 128;
    bad_mesh(&changed, CAD_SKETCH_TOPOLOGY);
    changed = saved; changed.triangles[0].a = changed.triangles[0].b;
    bad_mesh(&changed, CAD_SKETCH_DEGENERATE);
    changed = saved; --changed.triangle_count;
    bad_mesh(&changed, CAD_SKETCH_TOPOLOGY);
    changed = saved; changed.triangles[0] = changed.triangles[1];
    bad_mesh(&changed, CAD_SKETCH_TOPOLOGY);
    changed = saved;
    swap = changed.triangles[0].a;
    changed.triangles[0].a = changed.triangles[0].b;
    changed.triangles[0].b = swap;
    bad_mesh(&changed, CAD_SKETCH_TOPOLOGY);
    changed = saved; ++changed.vertex_count;
    changed.vertices[8] = changed.vertices[0];
    bad_mesh(&changed, CAD_SKETCH_TOPOLOGY);
    /* Every edge is still oppositely paired, but surface is disconnected. */
    changed = saved;
    for (i = 0; i < 8; ++i) {
        changed.vertices[i + 8] = saved.vertices[i];
        changed.vertices[i + 8].x += 200;
    }
    for (i = 0; i < 12; ++i) {
        changed.triangles[i + 12] = saved.triangles[i];
        changed.triangles[i + 12].a += 8;
        changed.triangles[i + 12].b += 8;
        changed.triangles[i + 12].c += 8;
    }
    changed.vertex_count = 16; changed.triangle_count = 24;
    bad_mesh(&changed, CAD_SKETCH_TOPOLOGY);
    /* Inward winding is combinatorially valid; volume is algebraic. */
    changed = saved;
    for (i = 0; i < changed.triangle_count; ++i) {
        swap = changed.triangles[i].a;
        changed.triangles[i].a = changed.triangles[i].b;
        changed.triangles[i].b = swap;
    }
    CHECK(cad_sketch_mesh_validate(&changed) == CAD_SKETCH_OK);
    CHECK(cad_sketch_mesh_volume6(&changed, &volume) == CAD_SKETCH_OK);
    CHECK(volume == -750000L);
    bad_mesh(0, CAD_SKETCH_ARGUMENT);
    CHECK(cad_sketch_mesh_volume6(&saved, 0) == CAD_SKETCH_ARGUMENT);
}

static void volume_bounds(void)
{
    const CadSketchPoint square[] = {{-400,-400},{400,-400},{400,400},{-400,400}};
    long volume;
    int i;
    successful(square, 4, 400, 1280000L);
    CHECK(cad_sketch_mesh_volume6(&mesh, &volume) == CAD_SKETCH_OK);
    CHECK(volume == 1536000000L);
    CHECK(volume / 6L == 256000000L);
    /* A general closed cube spanning -400..400 Z is outside volume6's
     * proven signed-32-bit domain, although every coordinate is legal. */
    changed = mesh;
    for (i = 0; i < 4; ++i) changed.vertices[i].z = -400;
    CHECK(cad_sketch_mesh_validate(&changed) == CAD_SKETCH_OK);
    volume = 77L;
    CHECK(cad_sketch_mesh_volume6(&changed, &volume) == CAD_SKETCH_RANGE);
    CHECK(volume == 77L);
    for (i = 0; i < changed.triangle_count; ++i) {
        unsigned short tmp;
        tmp = changed.triangles[i].a;
        changed.triangles[i].a = changed.triangles[i].b;
        changed.triangles[i].b = tmp;
    }
    CHECK(cad_sketch_mesh_volume6(&changed, &volume) == CAD_SKETCH_RANGE);
    CHECK(volume == 77L);
    ++checks;
}

/* An integer triangulated torus, then identification of distant vertices.
 * Edge incidence, orientation and global connectivity survive, but the
 * identified vertex has two link cycles: it must be rejected as pinched.
 * Torus here is only a topology fixture, not a general solid assertion. */
static void vertex_links(void)
{
    static const int cosine[6] = {100,50,-50,-100,-50,50};
    static const int sine[6] = {0,87,87,0,-87,-87};
    int i, j, v, a, b, c, d, radius, t = 0;
    memset(&second, 0, sizeof(second));
    second.kind = CAD_SKETCH_CLOSED_MESH;
    second.vertex_count = 36; second.triangle_count = 72;
    for (i = 0; i < 6; ++i)
        for (j = 0; j < 6; ++j) {
            v = 6 * i + j;
            radius = 100 + cosine[j] / 5;
            second.vertices[v].x = radius * cosine[i] / 100;
            second.vertices[v].y = radius * sine[i] / 100;
            second.vertices[v].z = sine[j] / 5;
            a = v; b = 6 * ((i + 1) % 6) + j;
            c = 6 * ((i + 1) % 6) + (j + 1) % 6;
            d = 6 * i + (j + 1) % 6;
            second.triangles[t].a = (unsigned short)a;
            second.triangles[t].b = (unsigned short)b;
            second.triangles[t++].c = (unsigned short)c;
            second.triangles[t].a = (unsigned short)a;
            second.triangles[t].b = (unsigned short)c;
            second.triangles[t++].c = (unsigned short)d;
        }
    CHECK(cad_sketch_mesh_validate(&second) == CAD_SKETCH_OK);
    changed = second;
    /* Remove vertex 21, identify it with 0, then pack vertex 35 into 21. */
    for (i = 0; i < 72; ++i) {
        CadSketchTriangle *f;
        f = &changed.triangles[i];
        if (f->a == 21) f->a = 0; else if (f->a == 35) f->a = 21;
        if (f->b == 21) f->b = 0; else if (f->b == 35) f->b = 21;
        if (f->c == 21) f->c = 0; else if (f->c == 35) f->c = 21;
    }
    changed.vertices[21] = changed.vertices[35];
    changed.vertex_count = 35;
    bad_mesh(&changed, CAD_SKETCH_TOPOLOGY);
}

int main(void)
{
    profiles();
    topology();
    volume_bounds();
    vertex_links();
    printf("sketch_test: %lu scenarios passed\n", checks);
    return 0;
}
