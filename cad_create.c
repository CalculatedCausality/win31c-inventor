#include "cad_create.h"
#include <string.h>

typedef struct CreatePoint { long x, y, z; } CreatePoint;
typedef struct CreateEdge { unsigned short a, b; } CreateEdge;

/* Separate globals allow Watcom -ml -zt=1 to place large objects in their
 * own far data segments. Never put a model or mesh on the Win16 stack. */
static CreatePoint points[30];
static CreateEdge links[40];
static CadId ids[30];
static CadSketchMesh mesh;
static CadModel demo;

static const CreatePoint house_points[30] = {
    {-100,-70,70},{100,-70,70},{100,-70,-70},{-100,-70,-70},
    {-100,30,70},{100,30,70},{100,30,-70},{-100,30,-70},
    {-100,100,0},{100,100,0},
    {-15,-70,70},{-15,0,70},{15,0,70},{15,-70,70},
    {-75,-30,70},{-35,-30,70},{-35,10,70},{-75,10,70},
    {35,-30,70},{75,-30,70},{75,10,70},{35,10,70},
    {40,55,-45},{65,55,-45},{65,75,-25},{40,75,-25},
    {40,120,-45},{65,120,-45},{65,120,-25},{40,120,-25}
};
static const CreateEdge house_links[40] = {
    {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},
    {0,4},{1,5},{2,6},{3,7},{8,9},{4,8},{7,8},{5,9},{6,9},
    {10,11},{11,12},{12,13},
    {14,15},{15,16},{16,17},{17,14},
    {18,19},{19,20},{20,21},{21,18},
    {22,23},{23,24},{24,25},{25,22},{26,27},{27,28},
    {28,29},{29,26},{22,26},{23,27},{24,28},{25,29}
};

static int coordinate(long v)
{
    /* These bounds also reject values outside signed 32-bit on LP64 hosts. */
    return v >= CAD_COORD_MIN && v <= CAD_COORD_MAX;
}

static CadStatus available(const CadModel *m, int nv, int ne)
{
    if (!m) return CAD_INVALID_ARGUMENT;
    if (m->transaction_active) return CAD_TRANSACTION_ACTIVE;
    if (nv > CAD_MAX_VERTICES - (int)m->vertex_count ||
        ne > CAD_MAX_EDGES - (int)m->edge_count) return CAD_CAPACITY;
    /* Check before subtraction; no host-long overflow, even for bad counters. */
    if (m->next_vertex_id < 1L || m->next_vertex_id > 65535L ||
        m->next_edge_id < 1L || m->next_edge_id > 65535L)
        return CAD_ID_EXHAUSTED;
    if ((long)nv > 65536L - m->next_vertex_id ||
        (long)ne > 65536L - m->next_edge_id) return CAD_ID_EXHAUSTED;
    return CAD_OK;
}

static CadStatus validate_graph(int nv, int ne)
{
    int i;
    const CreatePoint *a, *b;
    for (i = 0; i < nv; ++i)
        if (!coordinate(points[i].x) || !coordinate(points[i].y) ||
            !coordinate(points[i].z)) return CAD_RANGE;
    for (i = 0; i < ne; ++i) {
        if (links[i].a >= nv || links[i].b >= nv)
            return CAD_INVALID_ARGUMENT;
        a = &points[links[i].a]; b = &points[links[i].b];
        if (a->x == b->x && a->y == b->y && a->z == b->z)
            return CAD_INVALID_ARGUMENT;
    }
    return CAD_OK;
}

static CadStatus finish(CadModel *m, CadStatus s)
{
    if (s == CAD_OK) s = cad_model_commit(m);
    if (s != CAD_OK && m->transaction_active) cad_model_cancel(m);
    return s;
}

static CadStatus append(CadModel *m, int nv, int ne)
{
    CadStatus s;
    CadId edge_id;
    int i;
    s = validate_graph(nv, ne); if (s != CAD_OK) return s;
    s = available(m, nv, ne); if (s != CAD_OK) return s;
    if (nv + ne > CAD_MAX_TRANSACTION_DELTAS) return CAD_TRANSACTION_FULL;
    s = cad_model_begin(m); if (s != CAD_OK) return s;
    for (i = 0; i < nv && s == CAD_OK; ++i)
        s = cad_model_add_vertex(m, points[i].x, points[i].y,
                                 points[i].z, &ids[i]);
    for (i = 0; i < ne && s == CAD_OK; ++i)
        s = cad_model_add_edge(m, ids[links[i].a], ids[links[i].b], &edge_id);
    return finish(m, s);
}

static void point(int i, long x, long y, long z)
{
    points[i].x = x; points[i].y = y; points[i].z = z;
}

static void link(int i, int a, int b)
{
    links[i].a = (unsigned short)a; links[i].b = (unsigned short)b;
}

CadStatus cad_create_line(CadModel *m, long x1, long y1, long z1,
                          long x2, long y2, long z2)
{
    if (!m) return CAD_INVALID_ARGUMENT;
    if (m->transaction_active) return CAD_TRANSACTION_ACTIVE;
    point(0, x1, y1, z1); point(1, x2, y2, z2); link(0, 0, 1);
    return append(m, 2, 1);
}

static CadStatus extent(long base, long size, long *end)
{
    if (!coordinate(base) || size <= 0L)
        return CAD_RANGE;
#if LONG_MAX > 2147483647L
    if (size > 2147483647L) return CAD_RANGE;
#endif
    /* Compare before addition: CAD_COORD_MAX-base is safely in 0..800. */
    if (size > CAD_COORD_MAX - base) return CAD_RANGE;
    *end = base + size;
    return CAD_OK;
}

static CadStatus rectangle(long x, long y, long z, long w, long h)
{
    long xx, yy;
    int i;
    CadStatus s;
    if (!coordinate(z)) return CAD_RANGE;
    s = extent(x, w, &xx); if (s != CAD_OK) return s;
    s = extent(y, h, &yy); if (s != CAD_OK) return s;
    point(0, x, y, z); point(1, xx, y, z);
    point(2, xx, yy, z); point(3, x, yy, z);
    for (i = 0; i < 4; ++i) link(i, i, (i + 1) % 4);
    return CAD_OK;
}

CadStatus cad_create_rectangle(CadModel *m, long x, long y, long z,
                               long width, long height)
{
    CadStatus s;
    if (!m) return CAD_INVALID_ARGUMENT;
    if (m->transaction_active) return CAD_TRANSACTION_ACTIVE;
    s = rectangle(x, y, z, width, height);
    if (s != CAD_OK) return s;
    return append(m, 4, 4);
}

CadStatus cad_create_box(CadModel *m, long x, long y, long z,
                         long width, long height, long depth)
{
    CadStatus s;
    long zz;
    int i;
    if (!m) return CAD_INVALID_ARGUMENT;
    if (m->transaction_active) return CAD_TRANSACTION_ACTIVE;
    s = extent(z, depth, &zz); if (s != CAD_OK) return s;
    s = rectangle(x, y, z, width, height); if (s != CAD_OK) return s;
    for (i = 0; i < 4; ++i) {
        point(i + 4, points[i].x, points[i].y, zz);
        link(i + 4, i + 4, (i + 1) % 4 + 4);
        link(i + 8, i, i + 4);
    }
    return append(m, 8, 12);
}

static CadStatus sketch_status(CadSketchStatus s)
{
    if (s == CAD_SKETCH_OK) return CAD_OK;
    if (s == CAD_SKETCH_RANGE) return CAD_RANGE;
    return CAD_INVALID_ARGUMENT;
}

CadStatus cad_create_extrusion(CadModel *m, const CadSketchPoint *p,
                               int n, int height)
{
    CadSketchStatus s;
    int i;
    if (!m || !p || n < 3 || n > 12) return CAD_INVALID_ARGUMENT;
    if (m->transaction_active) return CAD_TRANSACTION_ACTIVE;
    s = cad_sketch_extrude(p, n, height, &mesh);
    if (s != CAD_SKETCH_OK) return sketch_status(s);
    s = cad_sketch_mesh_validate(&mesh);
    if (s != CAD_SKETCH_OK) return sketch_status(s);
    for (i = 0; i < 2 * n; ++i)
        point(i, (long)mesh.vertices[i].x, (long)mesh.vertices[i].y,
              (long)mesh.vertices[i].z);
    for (i = 0; i < n; ++i) {
        link(i, i, (i + 1) % n);
        link(i + n, i + n, (i + 1) % n + n);
        link(i + 2 * n, i, i + n);
    }
    return append(m, 2 * n, 3 * n);
}

CadStatus cad_create_house_demo(CadModel *m)
{
    CadStatus s;
    CadId edge_id;
    int i;
    if (!m) return CAD_INVALID_ARGUMENT;
    if (m->transaction_active) return CAD_TRANSACTION_ACTIVE;
    memcpy(points, house_points, sizeof(house_points));
    memcpy(links, house_links, sizeof(house_links));
    s = validate_graph(30, 40); if (s != CAD_OK) return s;
    s = cad_model_init(&demo); if (s != CAD_OK) return s;
    s = cad_model_begin(&demo); if (s != CAD_OK) return s;
    for (i = 0; i < 30 && s == CAD_OK; ++i)
        s = cad_model_add_vertex(&demo, points[i].x, points[i].y,
                                 points[i].z, &ids[i]);
    s = finish(&demo, s); if (s != CAD_OK) return s;
    s = cad_model_begin(&demo); if (s != CAD_OK) return s;
    for (i = 0; i < 40 && s == CAD_OK; ++i)
        s = cad_model_add_edge(&demo, ids[links[i].a], ids[links[i].b], &edge_id);
    s = finish(&demo, s); if (s != CAD_OK) return s;
    /* Import baseline, not an undoable creation command. */
    memset(demo.history, 0, sizeof(demo.history));
    memset(demo.staged, 0, sizeof(demo.staged));
    memset(demo.lengths, 0, sizeof(demo.lengths));
    demo.history_count = demo.history_cursor = demo.history_used = 0;
    memcpy(m, &demo, sizeof(demo));
    return CAD_OK;
}
