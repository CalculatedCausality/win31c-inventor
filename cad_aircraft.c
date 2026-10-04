#include "cad_aircraft.h"
#include <string.h>

static CadAircraft asset;
static CadModel import_stage;
static int initialized;
static CadStatus build_status;

#ifndef __WATCOMC__
typedef char AircraftFitsSegment[(sizeof(CadAircraft) < 65536UL) ? 1 : -1];
typedef char AircraftStageFitsSegment[(sizeof(CadModel) < 65536UL) ? 1 : -1];
#endif

/* Octagon, starting at roof, normal +X. Signed symmetric rounding. */
static const int ring_y[8] = {100,71,0,-71,-100,-71,0,71};
static const int ring_z[8] = {0,71,100,71,0,-71,-100,-71};
static int scaled(int radius, int fraction)
{
    long n = (long)radius * fraction;
    return (int)(n < 0L ? -((-n + 50L) / 100L) : (n + 50L) / 100L);
}

static void vertex(int x, int y, int z)
{
    CadExportVertex *v;
    if (build_status != CAD_OK) return;
    if (asset.vertex_count == CAD_AIRCRAFT_MAX_VERTICES) {
        build_status = CAD_CAPACITY; return;
    }
    if (x < -400 || x > 400 || y < -400 || y > 400 ||
        z < -400 || z > 400) { build_status = CAD_RANGE; return; }
    v = &asset.vertices[asset.vertex_count++];
    v->x = x; v->y = y; v->z = z;
}

static void edge(int a, int b)
{
    CadExportEdge *e;
    if (build_status != CAD_OK) return;
    if (asset.edge_count == CAD_AIRCRAFT_MAX_EDGES) {
        build_status = CAD_CAPACITY; return;
    }
    e = &asset.edges[asset.edge_count++];
    e->a = a; e->b = b; e->style = 0;
}

static void triangle(int a, int b, int c, int material, int reverse)
{
    CadAircraftTriangle *t;
    if (build_status != CAD_OK) return;
    if (asset.triangle_count == CAD_AIRCRAFT_MAX_TRIANGLES) {
        build_status = CAD_CAPACITY; return;
    }
    t = &asset.triangles[asset.triangle_count++];
    t->a = (unsigned short)a;
    t->b = (unsigned short)(reverse ? c : b);
    t->c = (unsigned short)(reverse ? b : c);
    t->material = (unsigned short)material;
}

/* Closed ring loft. Input ring normal must oppose sweep; reverse handles
 * reflected wings. Only quad boundaries enter the independent wireframe. */
static void loft(int base, int rings, int n, int material, int reverse)
{
    int s, k, a, b, c, d, m, first;
    CadAircraftComponent *part;
    if (build_status != CAD_OK) return;
    if (asset.component_count == CAD_AIRCRAFT_MAX_COMPONENTS) {
        build_status = CAD_CAPACITY; return;
    }
    first = asset.triangle_count;
    for (s = 0; s < rings; ++s) {
        for (k = 0; k < n; ++k) {
            a = base + s * n + k;
            b = base + s * n + (k + 1) % n;
            edge(a, b);
            if (s + 1 < rings) {
                c = a + n; d = b + n;
                edge(a, c);
                m = material;
                if (material == CAD_AIRCRAFT_BODY && s == 1 &&
                    (k == 0 || k == 7)) m = CAD_AIRCRAFT_CABIN_GLASS;
                triangle(a, c, d, m, reverse);
                triangle(a, d, b, m, reverse);
            }
        }
    }
    for (k = 1; k < n - 1; ++k) {
        triangle(base, base + k, base + k + 1, material, reverse);
        a = base + (rings - 1) * n;
        triangle(a, a + k + 1, a + k, material, reverse);
    }
    part = &asset.components[asset.component_count++];
    part->first_vertex = base; part->vertex_count = rings * n;
    part->first_triangle = first;
    part->triangle_count = asset.triangle_count - first;
    part->material = material;
}

static void octagon(int x, int cy, int z, int top, int bottom, int width)
{
    int k;
    for (k = 0; k < 8; ++k)
        vertex(x, cy + scaled(ring_y[k] >= 0 ? top : bottom, ring_y[k]),
               z + scaled(width, ring_z[k]));
}

/* x leading/trailing, y centre/thickness, z lateral station. */
static void wing_ring(int lead, int trail, int y, int half, int z)
{
    vertex(lead, y + half, z); vertex(trail, y + half, z);
    vertex(trail, y - half, z); vertex(lead, y - half, z);
}

/* Horizontal x/z ring with -Y normal; sweep upwards for fin/pylons. */
static void fin_ring(int lead, int trail, int y, int z, int half)
{
    vertex(lead, y, z + half); vertex(trail, y, z + half);
    vertex(trail, y, z - half); vertex(lead, y, z - half);
}

static void build(void)
{
    /* x, centre Y, roof radius, belly radius, half-width. Hump is in the
     * SAME rings as the main fuselage, fading aft of the upper deck. */
    static const int sections[10][5] = {
        {353,0,3,3,3}, {337,0,20,20,20}, {307,0,50,30,29},
        {245,0,55,31,31}, {185,0,53,31,31}, {120,0,33,31,31},
        {-90,0,31,31,31}, {-240,2,26,26,25},
        {-322,4,12,12,12}, {-354,2,3,3,3}
    };
    static const CadAircraftColor colors[CAD_AIRCRAFT_MATERIAL_COUNT] = {
        {230,234,240}, {176,185,197}, {201,208,219},
        {128,141,158}, {37,67,93}
    };
    int i, sign, base, outer, front, rear, cy, z, radius;
    memset(&asset, 0, sizeof(asset));
    build_status = CAD_OK;
    asset.units = CAD_UNITS_UNSPECIFIED; asset.up_axis = CAD_EXPORT_Y_UP;
    asset.ticks = 100;
    asset.scale_numerator = 1L; asset.scale_denominator = 100L;
    asset.reference_length_cm = 7066L; asset.reference_span_cm = 6444L;
    memcpy(asset.palette, colors, sizeof(colors));
    for (i = 0; i < 10; ++i)
        octagon(sections[i][0], sections[i][1], 0, sections[i][2],
                sections[i][3], sections[i][4]);
    loft(0, 10, 8, CAD_AIRCRAFT_BODY, 0);
    for (sign = -1; sign <= 1; sign += 2) {
        base = asset.vertex_count;
        wing_ring(105, -65, -8, 5, sign * 26);
        wing_ring(-5, -74, 0, 3, sign * 170);
        wing_ring(-116, -142, 14, 1, sign * 318);
        wing_ring(-125, -143, 42, 1, sign * 322);
        loft(base, 4, 4, CAD_AIRCRAFT_WING, sign > 0);
        base = asset.vertex_count;
        wing_ring(-236, -329, 9, 3, sign * 15);
        wing_ring(-313, -343, 22, 1, sign * 110);
        loft(base, 2, 4, CAD_AIRCRAFT_TAIL, sign > 0);
    }
    base = asset.vertex_count;
    fin_ring(-220, -330, 18, 0, 5);
    fin_ring(-264, -332, 87, 0, 3);
    fin_ring(-309, -336, 139, 0, 1);
    loft(base, 3, 4, CAD_AIRCRAFT_TAIL, 0);
    for (sign = -1; sign <= 1; sign += 2) {
        for (outer = 0; outer < 2; ++outer) {
            front = outer ? 7 : 104; rear = front - 47;
            cy = outer ? -21 : -29;
            z = sign * (outer ? 190 : 90); radius = outer ? 12 : 13;
            base = asset.vertex_count;
            octagon(front, cy, z, radius, radius, radius);
            octagon(rear, cy, z, radius - 2, radius - 2, radius - 2);
            loft(base, 2, 8, CAD_AIRCRAFT_ENGINE, 0);
            base = asset.vertex_count;
            fin_ring(outer ? -25 : 65, outer ? -55 : 40,
                     outer ? -12 : -20, z, 3);
            fin_ring(outer ? -25 : 65, outer ? -55 : 40,
                     outer ? 4 : -6, z, 3);
            loft(base, 2, 4, CAD_AIRCRAFT_ENGINE, 0);
        }
    }
    if (build_status == CAD_OK) build_status = cad_aircraft_validate(&asset);
}

CadStatus cad_aircraft_validate(const CadAircraft *a)
{
    int i, j, k, refs[3], covered_v, covered_t;
    const CadExportVertex *v, *p, *q, *r;
    const CadExportEdge *e;
    const CadAircraftTriangle *t;
    const CadAircraftComponent *c;
    long ux, uy, uz, vx, vy, vz;
    if (!a) return CAD_INVALID_ARGUMENT;
    if (a->vertex_count <= 0 || a->edge_count <= 0 || a->triangle_count <= 0 ||
        a->component_count <= 0) return CAD_INVALID_ARGUMENT;
    if (a->vertex_count > CAD_AIRCRAFT_MAX_VERTICES ||
        a->vertex_count > CAD_MAX_VERTICES ||
        a->edge_count > CAD_AIRCRAFT_MAX_EDGES || a->edge_count > CAD_MAX_EDGES ||
        a->triangle_count > CAD_AIRCRAFT_MAX_TRIANGLES ||
        a->component_count > CAD_AIRCRAFT_MAX_COMPONENTS) return CAD_CAPACITY;
    if (a->units != CAD_UNITS_UNSPECIFIED || a->up_axis != CAD_EXPORT_Y_UP ||
        a->ticks <= 0 || a->scale_numerator <= 0L ||
        a->scale_denominator <= 0L || a->reference_length_cm <= 0L ||
        a->reference_span_cm <= 0L) return CAD_INVALID_ARGUMENT;
    for (i = 0; i < a->vertex_count; ++i) {
        v = &a->vertices[i];
        if (v->x < -400 || v->x > 400 || v->y < -400 || v->y > 400 ||
            v->z < -400 || v->z > 400) return CAD_RANGE;
    }
    for (i = 0; i < a->edge_count; ++i) {
        e = &a->edges[i];
        if (e->a < 0 || e->a >= a->vertex_count || e->b < 0 ||
            e->b >= a->vertex_count || e->a == e->b || e->style != 0)
            return CAD_INVALID_ARGUMENT;
        for (j = 0; j < i; ++j)
            if ((e->a == a->edges[j].a && e->b == a->edges[j].b) ||
                (e->a == a->edges[j].b && e->b == a->edges[j].a))
                return CAD_INVALID_ARGUMENT;
    }
    for (i = 0; i < a->triangle_count; ++i) {
        t = &a->triangles[i];
        if (t->a >= a->vertex_count || t->b >= a->vertex_count ||
            t->c >= a->vertex_count || t->material >= CAD_AIRCRAFT_MATERIAL_COUNT)
            return CAD_INVALID_ARGUMENT;
        p = &a->vertices[t->a]; q = &a->vertices[t->b]; r = &a->vertices[t->c];
        ux = (long)q->x - p->x; uy = (long)q->y - p->y; uz = (long)q->z - p->z;
        vx = (long)r->x - p->x; vy = (long)r->y - p->y; vz = (long)r->z - p->z;
        if (uy * vz == uz * vy && uz * vx == ux * vz && ux * vy == uy * vx)
            return CAD_INVALID_ARGUMENT;
    }
    covered_v = covered_t = 0;
    for (i = 0; i < a->component_count; ++i) {
        c = &a->components[i];
        if (c->first_vertex != covered_v || c->first_triangle != covered_t ||
            c->vertex_count <= 0 || c->vertex_count > a->vertex_count - covered_v ||
            c->triangle_count <= 0 || c->triangle_count > a->triangle_count - covered_t ||
            c->material < 0 || c->material >= CAD_AIRCRAFT_MATERIAL_COUNT)
            return CAD_INVALID_ARGUMENT;
        covered_v += c->vertex_count; covered_t += c->triangle_count;
        for (j = c->first_triangle; j < covered_t; ++j) {
            refs[0] = a->triangles[j].a; refs[1] = a->triangles[j].b;
            refs[2] = a->triangles[j].c;
            for (k = 0; k < 3; ++k)
                if (refs[k] < c->first_vertex || refs[k] >= covered_v)
                    return CAD_INVALID_ARGUMENT;
        }
    }
    return covered_v == a->vertex_count && covered_t == a->triangle_count ?
           CAD_OK : CAD_INVALID_ARGUMENT;
}

const CadAircraft *cad_aircraft_get(void)
{
    if (!initialized) { build(); initialized = 1; }
    return build_status == CAD_OK ? &asset : NULL;
}

/* Commit full batches before the next operation, never exceeding 64 deltas. */
static CadStatus batch(void)
{
    CadStatus s;
    if (import_stage.staged_count == CAD_MAX_TRANSACTION_DELTAS) {
        s = cad_model_commit(&import_stage);
        if (s != CAD_OK) return s;
        return cad_model_begin(&import_stage);
    }
    return CAD_OK;
}

CadStatus cad_aircraft_load(CadModel *live)
{
    const CadAircraft *a;
    CadStatus s;
    CadId id;
    int i;
    if (!live) return CAD_INVALID_ARGUMENT;
    if (live->transaction_active) return CAD_TRANSACTION_ACTIVE;
    a = cad_aircraft_get();
    if (!a) return build_status;
    s = cad_aircraft_validate(a); if (s != CAD_OK) return s;
    s = cad_model_init(&import_stage); if (s != CAD_OK) return s;
    s = cad_model_begin(&import_stage); if (s != CAD_OK) return s;
    for (i = 0; i < a->vertex_count; ++i) {
        s = batch(); if (s != CAD_OK) return s;
        s = cad_model_add_vertex(&import_stage, a->vertices[i].x,
                                a->vertices[i].y, a->vertices[i].z, &id);
        if (s != CAD_OK) return s;
    }
    for (i = 0; i < a->edge_count; ++i) {
        s = batch(); if (s != CAD_OK) return s;
        s = cad_model_add_edge(&import_stage,
                              import_stage.vertices[a->edges[i].a].id,
                              import_stage.vertices[a->edges[i].b].id, &id);
        if (s != CAD_OK) return s;
    }
    s = cad_model_commit(&import_stage); if (s != CAD_OK) return s;
    memset(import_stage.history, 0, sizeof(import_stage.history));
    memset(import_stage.staged, 0, sizeof(import_stage.staged));
    memset(import_stage.lengths, 0, sizeof(import_stage.lengths));
    import_stage.history_count = import_stage.history_cursor = 0;
    import_stage.history_used = import_stage.staged_count = 0;
    memcpy(live, &import_stage, sizeof(*live));
    return CAD_OK;
}
