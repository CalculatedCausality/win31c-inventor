#include "cad_sketch.h"
#include <limits.h>
#include <string.h>

#if INT_MAX < 32767 || LONG_MAX < 2147483647L
#error cad_sketch requires at least 16-bit int and 32-bit long
#endif

#define CAD_LONG_MAX 2147483647L
#define CAD_LONG_MIN (-2147483647L - 1L)
#define CAD_EDGE_MAX (3 * CAD_SKETCH_MAX_TRIANGLES)

typedef struct CadEdge {
    unsigned short a, b, first, second;
} CadEdge;

/* Under 64K even with 32-bit int/enums; no large automatic objects. */
static CadSketchMesh work_mesh;
static CadSketchPoint work_points[CAD_SKETCH_MAX_POINTS];
static unsigned short ring[CAD_SKETCH_MAX_POINTS];
static CadEdge edges[CAD_EDGE_MAX];
static unsigned char seen[CAD_SKETCH_MAX_TRIANGLES];
static unsigned short queue[CAD_SKETCH_MAX_TRIANGLES];
typedef char CadScratchFits64K[((unsigned long)sizeof(work_mesh) + sizeof(work_points) +
    sizeof(ring) + sizeof(edges) + sizeof(seen) + sizeof(queue) < 65536UL) ? 1 : -1];

static int in_range(int value)
{
    return value >= -CAD_SKETCH_LIMIT && value <= CAD_SKETCH_LIMIT;
}

static long orient(const CadSketchPoint *a, const CadSketchPoint *b,
                   const CadSketchPoint *c)
{
    /* Widen BEFORE subtraction: each difference <=800, determinant <=1280000. */
    long ax = (long)b->x - (long)a->x;
    long ay = (long)b->y - (long)a->y;
    long bx = (long)c->x - (long)a->x;
    long by = (long)c->y - (long)a->y;
    return ax * by - ay * bx;
}

static int between(int a, int b, int x)
{
    return (a <= x && x <= b) || (b <= x && x <= a);
}

static int on_segment(const CadSketchPoint *a, const CadSketchPoint *b,
                      const CadSketchPoint *p)
{
    return orient(a, b, p) == 0L && between(a->x, b->x, p->x) &&
           between(a->y, b->y, p->y);
}

static int opposite(long a, long b)
{
    return (a < 0L && b > 0L) || (a > 0L && b < 0L);
}

static int intersects(const CadSketchPoint *a, const CadSketchPoint *b,
                      const CadSketchPoint *c, const CadSketchPoint *d)
{
    long ab_c = orient(a, b, c), ab_d = orient(a, b, d);
    long cd_a = orient(c, d, a), cd_b = orient(c, d, b);
    return (opposite(ab_c, ab_d) && opposite(cd_a, cd_b)) ||
           on_segment(a, b, c) || on_segment(a, b, d) ||
           on_segment(c, d, a) || on_segment(c, d, b);
}

CadSketchStatus cad_sketch_validate(const CadSketchPoint *p, int n, long *area2)
{
    int i, j, next, prev;
    long area = 0L;
    if (p == 0) return CAD_SKETCH_ARGUMENT;
    if (n < 3 || n > CAD_SKETCH_MAX_POINTS) return CAD_SKETCH_COMPLEXITY;
    for (i = 0; i < n; ++i)
        if (!in_range(p[i].x) || !in_range(p[i].y)) return CAD_SKETCH_RANGE;
    for (i = 0; i < n; ++i) {
        next = (i + 1) % n;
        if (p[i].x == p[next].x && p[i].y == p[next].y)
            return CAD_SKETCH_ZERO_EDGE;
    }
    for (i = 0; i < n; ++i)
        for (j = i + 1; j < n; ++j)
            if (p[i].x == p[j].x && p[i].y == p[j].y)
                return CAD_SKETCH_DUPLICATE;
    for (i = 0; i < n; ++i) {
        prev = (i + n - 1) % n;
        next = (i + 1) % n;
        if (orient(&p[prev], &p[i], &p[next]) == 0L &&
            !on_segment(&p[prev], &p[next], &p[i]))
            return CAD_SKETCH_INTERSECTION;
        for (j = i + 1; j < n; ++j) {
            if (j == next || (j + 1) % n == i) continue;
            if (intersects(&p[i], &p[next], &p[j], &p[(j + 1) % n]))
                return CAD_SKETCH_INTERSECTION;
        }
        /* At most 64 terms of magnitude 320000: safe even before simplicity. */
        area += (long)p[i].x * (long)p[next].y -
                (long)p[i].y * (long)p[next].x;
    }
    if (area == 0L) return CAD_SKETCH_ZERO_AREA;
    if (area2 != 0) *area2 = area;
    return CAD_SKETCH_OK;
}

static void triangle(int a, int b, int c)
{
    CadSketchTriangle *t = &work_mesh.triangles[work_mesh.triangle_count++];
    t->a = (unsigned short)a;
    t->b = (unsigned short)b;
    t->c = (unsigned short)c;
}

static int inside_triangle(int a, int b, int c, int p)
{
    return orient(&work_points[a], &work_points[b], &work_points[p]) >= 0L &&
           orient(&work_points[b], &work_points[c], &work_points[p]) >= 0L &&
           orient(&work_points[c], &work_points[a], &work_points[p]) >= 0L;
}

CadSketchStatus cad_sketch_extrude(const CadSketchPoint *p, int n, int height,
                                 CadSketchMesh *out)
{
    CadSketchStatus status;
    long area;
    int i, j, m, a, b, c, ear, blocked, next;
    if (out == 0) return CAD_SKETCH_ARGUMENT;
    if (height <= 0 || height > CAD_SKETCH_LIMIT) return CAD_SKETCH_RANGE;
    status = cad_sketch_validate(p, n, &area);
    if (status != CAD_SKETCH_OK) return status;
    memset(&work_mesh, 0, sizeof(work_mesh));
    work_mesh.kind = CAD_SKETCH_CLOSED_MESH;
    work_mesh.vertex_count = (unsigned short)(2 * n);
    for (i = 0; i < n; ++i) {
        work_points[i] = p[area > 0L ? i : n - 1 - i];
        work_mesh.vertices[i].x = work_points[i].x;
        work_mesh.vertices[i].y = work_points[i].y;
        work_mesh.vertices[i + n] = work_mesh.vertices[i];
        work_mesh.vertices[i + n].z = height;
        ring[i] = (unsigned short)i;
    }
    m = n;
    while (m > 3) {
        ear = -1;
        for (i = 0; i < m; ++i) {
            a = ring[(i + m - 1) % m]; b = ring[i]; c = ring[(i + 1) % m];
            if (orient(&work_points[a], &work_points[b], &work_points[c]) <= 0L)
                continue;
            blocked = 0;
            for (j = 0; j < m; ++j) {
                if (ring[j] == a || ring[j] == b || ring[j] == c) continue;
                if (inside_triangle(a, b, c, ring[j])) { blocked = 1; break; }
            }
            if (!blocked) { ear = i; break; }
        }
        if (ear < 0) return CAD_SKETCH_TRIANGULATION;
        triangle(c, b, a);
        triangle(a + n, b + n, c + n);
        for (j = ear; j < m - 1; ++j) ring[j] = ring[j + 1];
        --m;
    }
    a = ring[0]; b = ring[1]; c = ring[2];
    if (orient(&work_points[a], &work_points[b], &work_points[c]) <= 0L)
        return CAD_SKETCH_TRIANGULATION;
    triangle(c, b, a);
    triangle(a + n, b + n, c + n);
    for (i = 0; i < n; ++i) {
        next = (i + 1) % n;
        triangle(i, next, next + n);
        triangle(i, next + n, i + n);
    }
    status = cad_sketch_mesh_validate(&work_mesh);
    if (status != CAD_SKETCH_OK) return status;
    *out = work_mesh;
    return CAD_SKETCH_OK;
}

static int has_vertex(const CadSketchTriangle *t, int v)
{
    return t->a == v || t->b == v || t->c == v;
}

/* Traverse all faces, or the face fan around one vertex. Edge-paired closed
 * triangles have degree two in each vertex link; connected means one cycle. */
static int connected(const CadSketchMesh *mesh, int edge_count, int vertex)
{
    int i, start = -1, expected = 0, head = 0, tail = 0, face, other;
    for (i = 0; i < mesh->triangle_count; ++i) {
        seen[i] = 0;
        if (vertex < 0 || has_vertex(&mesh->triangles[i], vertex)) {
            start = i;
            ++expected;
        }
    }
    if (start < 0) return 0;
    seen[start] = 1;
    queue[tail++] = (unsigned short)start;
    while (head < tail) {
        face = queue[head++];
        for (i = 0; i < edge_count; ++i) {
            if (vertex >= 0 && edges[i].a != vertex && edges[i].b != vertex)
                continue;
            other = -1;
            if (edges[i].first == face) other = edges[i].second;
            else if (edges[i].second == face) other = edges[i].first;
            if (other >= 0 && !seen[other]) {
                seen[other] = 1;
                queue[tail++] = (unsigned short)other;
            }
        }
    }
    return tail == expected;
}

CadSketchStatus cad_sketch_mesh_validate(const CadSketchMesh *mesh)
{
    int i, j, k, e, edge_count = 0;
    unsigned short ids[3], a, b;
    const CadSketchVertex *v, *w, *u;
    const CadSketchTriangle *t;
    long ax, ay, az, bx, by, bz;
    if (mesh == 0) return CAD_SKETCH_ARGUMENT;
    if (mesh->kind != CAD_SKETCH_CLOSED_MESH) return CAD_SKETCH_NOT_CLOSED;
    if (mesh->vertex_count < 4 || mesh->vertex_count > CAD_SKETCH_MAX_VERTICES ||
        mesh->triangle_count < 4 || mesh->triangle_count > CAD_SKETCH_MAX_TRIANGLES)
        return CAD_SKETCH_COMPLEXITY;
    for (i = 0; i < mesh->vertex_count; ++i) {
        v = &mesh->vertices[i];
        if (!in_range(v->x) || !in_range(v->y) || !in_range(v->z))
            return CAD_SKETCH_RANGE;
    }
    for (i = 0; i < mesh->triangle_count; ++i) {
        t = &mesh->triangles[i];
        ids[0] = t->a; ids[1] = t->b; ids[2] = t->c;
        for (j = 0; j < 3; ++j)
            if (ids[j] >= mesh->vertex_count) return CAD_SKETCH_TOPOLOGY;
        v = &mesh->vertices[t->a]; w = &mesh->vertices[t->b]; u = &mesh->vertices[t->c];
        ax = (long)w->x - (long)v->x; ay = (long)w->y - (long)v->y;
        az = (long)w->z - (long)v->z; bx = (long)u->x - (long)v->x;
        by = (long)u->y - (long)v->y; bz = (long)u->z - (long)v->z;
        if (ax * by - ay * bx == 0L && ay * bz - az * by == 0L &&
            az * bx - ax * bz == 0L) return CAD_SKETCH_DEGENERATE;
        for (j = 0; j < 3; ++j) {
            a = ids[j]; b = ids[(j + 1) % 3];
            e = -1;
            for (k = 0; k < edge_count; ++k)
                if ((edges[k].a == a && edges[k].b == b) ||
                    (edges[k].a == b && edges[k].b == a)) { e = k; break; }
            if (e >= 0) {
                if (edges[e].second != CAD_SKETCH_MAX_TRIANGLES ||
                    edges[e].a != b || edges[e].b != a)
                    return CAD_SKETCH_TOPOLOGY;
                edges[e].second = (unsigned short)i;
            } else {
                if (edge_count >= CAD_EDGE_MAX) return CAD_SKETCH_COMPLEXITY;
                edges[edge_count].a = a; edges[edge_count].b = b;
                edges[edge_count].first = (unsigned short)i;
                edges[edge_count].second = CAD_SKETCH_MAX_TRIANGLES;
                ++edge_count;
            }
        }
    }
    for (i = 0; i < edge_count; ++i)
        if (edges[i].second == CAD_SKETCH_MAX_TRIANGLES) return CAD_SKETCH_TOPOLOGY;
    if (!connected(mesh, edge_count, -1)) return CAD_SKETCH_TOPOLOGY;
    for (i = 0; i < mesh->vertex_count; ++i)
        if (!connected(mesh, edge_count, i)) return CAD_SKETCH_TOPOLOGY;
    return CAD_SKETCH_OK;
}

CadSketchStatus cad_sketch_mesh_volume6(const CadSketchMesh *mesh, long *out)
{
    CadSketchStatus status;
    const CadSketchVertex *a, *b, *c;
    const CadSketchTriangle *t;
    long sum = 0L, area, zsum, term;
    int i;
    if (out == 0) return CAD_SKETCH_ARGUMENT;
    status = cad_sketch_mesh_validate(mesh);
    if (status != CAD_SKETCH_OK) return status;
    for (i = 0; i < mesh->triangle_count; ++i) {
        t = &mesh->triangles[i];
        a = &mesh->vertices[t->a]; b = &mesh->vertices[t->b]; c = &mesh->vertices[t->c];
        area = ((long)b->x - (long)a->x) * ((long)c->y - (long)a->y) -
               ((long)b->y - (long)a->y) * ((long)c->x - (long)a->x);
        zsum = (long)a->z + (long)b->z + (long)c->z;
        /* |area| <=1280000, |zsum| <=1200: product <=1536000000. */
        term = area * zsum;
        if ((term > 0L && sum > CAD_LONG_MAX - term) ||
            (term < 0L && sum < CAD_LONG_MIN - term)) return CAD_SKETCH_RANGE;
        sum += term;
    }
    *out = sum;
    return CAD_SKETCH_OK;
}
