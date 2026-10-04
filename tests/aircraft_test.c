/* Host: cc -std=c89 -pedantic -Wall -Wextra -Werror cad_aircraft.c
 *       cad_model.c tests/aircraft_test.c -o /tmp/opencode/aircraft_test
 * Win16: compile all three with -bt=windows -zW -ml -zt=1 -w4 -we.
 * Large objects are static, never on the Win16 stack. */
#include "../cad_aircraft.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CadModel model, saved;
static CadAircraft broken;
static unsigned long checks;
static int mirrored[CAD_AIRCRAFT_MAX_VERTICES];
static int seen[CAD_AIRCRAFT_MAX_VERTICES];

#define CHECK(e) do { ++checks; if (!(e)) { \
    fprintf(stderr, "aircraft line %d: %s\n", __LINE__, #e); exit(1); \
} } while (0)
#define OK(e) CHECK((e) == CAD_OK)
#define SAVE() memcpy(&saved, &model, sizeof(model))
#define SAME() CHECK(!memcmp(&saved, &model, sizeof(model)))

static long absolute(long n) { return n < 0L ? -n : n; }

static int face_vertex(const CadAircraftTriangle *t, int k)
{
    return k == 0 ? t->a : (k == 1 ? t->b : t->c);
}

static int surface_edge(const CadAircraft *a, int from, int to)
{
    int i, k, x, y;
    for (i = 0; i < a->triangle_count; ++i) {
        for (k = 0; k < 3; ++k) {
            x = face_vertex(&a->triangles[i], k);
            y = face_vertex(&a->triangles[i], (k + 1) % 3);
            if ((from == x && to == y) || (from == y && to == x)) return 1;
        }
    }
    return 0;
}

static void geometry(const CadAircraft *a)
{
    int i, j, k, found, glass = 0;
    int xmin = 400, xmax = -400, zmin = 400, zmax = -400;
    int material_use[CAD_AIRCRAFT_MATERIAL_COUNT];
    const CadExportVertex *p, *q, *r;
    const CadAircraftTriangle *t;
    long ux, uy, uz, vx, vy, vz;
    memset(material_use, 0, sizeof(material_use));
    OK(cad_aircraft_validate(a));
    CHECK(a == cad_aircraft_get());
    CHECK(a->vertex_count == 236 && a->vertex_count <= 240);
    CHECK(a->edge_count == 396 && a->edge_count <= 512);
    CHECK(a->triangle_count == 416 && a->triangle_count <= 450);
    CHECK(a->component_count == 14);
#ifndef __WATCOMC__
    CHECK(sizeof(*a) < 65536UL && sizeof(model) < 65536UL);
#endif
    CHECK(a->units == CAD_UNITS_UNSPECIFIED && a->up_axis == CAD_EXPORT_Y_UP);
    CHECK(a->ticks == 100 && a->scale_numerator == 1L &&
          a->scale_denominator == 100L);
    CHECK(a->reference_length_cm == 7066L && a->reference_span_cm == 6444L);
    for (i = 0; i < a->vertex_count; ++i) {
        p = &a->vertices[i];
        CHECK(p->x >= -400 && p->x <= 400 && p->y >= -400 &&
              p->y <= 400 && p->z >= -400 && p->z <= 400);
        if (p->x < xmin) xmin = p->x;
        if (p->x > xmax) xmax = p->x;
        if (p->z < zmin) zmin = p->z;
        if (p->z > zmax) zmax = p->z;
        found = -1;
        for (j = 0; j < a->vertex_count; ++j)
            if (p->x == a->vertices[j].x && p->y == a->vertices[j].y &&
                p->z == -a->vertices[j].z) { found = j; break; }
        CHECK(found >= 0); mirrored[i] = found;
        found = 0;
        for (j = 0; j < a->triangle_count; ++j)
            for (k = 0; k < 3; ++k)
                if (face_vertex(&a->triangles[j], k) == i) found = 1;
        CHECK(found);
        found = 0;
        for (j = 0; j < a->edge_count; ++j)
            if (a->edges[j].a == i || a->edges[j].b == i) found = 1;
        CHECK(found);
    }
    CHECK(xmax == 353 && xmin == -354 && zmin == -322 && zmax == 322);
    CHECK(xmax - xmin > zmax - zmin);
    CHECK(absolute((long)(xmax - xmin) * 10L - a->reference_length_cm) <= 5L);
    CHECK(absolute((long)(zmax - zmin) * 10L - a->reference_span_cm) <= 5L);
    /* Ratio error < 0.2%, using cross-products, no floating point. */
    CHECK(absolute((long)(xmax - xmin) * a->reference_span_cm -
                   (long)(zmax - zmin) * a->reference_length_cm) * 500L <
          (long)(zmax - zmin) * a->reference_length_cm);
    CHECK(a->vertices[3 * 8].y == 55); /* Extended upper-deck roof. */
    CHECK(a->vertices[6 * 8].y == 31); /* Lower ordinary aft roof. */
    for (i = 0; i < a->edge_count; ++i) {
        int x = a->edges[i].a, y = a->edges[i].b;
        CHECK(x >= 0 && x < a->vertex_count && y >= 0 && y < a->vertex_count);
        CHECK(x != y && a->edges[i].style == 0);
        CHECK(surface_edge(a, x, y));
        for (j = 0; j < i; ++j)
            CHECK(!((x == a->edges[j].a && y == a->edges[j].b) ||
                    (x == a->edges[j].b && y == a->edges[j].a)));
        found = 0;
        for (j = 0; j < a->edge_count; ++j)
            if ((mirrored[x] == a->edges[j].a && mirrored[y] == a->edges[j].b) ||
                (mirrored[x] == a->edges[j].b && mirrored[y] == a->edges[j].a))
                found = 1;
        CHECK(found);
    }
    for (i = 0; i < a->triangle_count; ++i) {
        t = &a->triangles[i];
        CHECK(t->a < a->vertex_count && t->b < a->vertex_count &&
              t->c < a->vertex_count && t->material < CAD_AIRCRAFT_MATERIAL_COUNT);
        ++material_use[t->material];
        p = &a->vertices[t->a]; q = &a->vertices[t->b]; r = &a->vertices[t->c];
        ux = (long)q->x - p->x; uy = (long)q->y - p->y; uz = (long)q->z - p->z;
        vx = (long)r->x - p->x; vy = (long)r->y - p->y; vz = (long)r->z - p->z;
        CHECK(uy * vz != uz * vy || uz * vx != ux * vz || ux * vy != uy * vx);
        if (t->material == CAD_AIRCRAFT_CABIN_GLASS) {
            ++glass;
            CHECK(p->x >= 307 && q->x >= 307 && r->x >= 307);
            CHECK(p->y >= 0 && q->y >= 0 && r->y >= 0);
        }
    }
    CHECK(glass == 4);
    for (i = 0; i < CAD_AIRCRAFT_MATERIAL_COUNT; ++i) CHECK(material_use[i] > 0);
}

static void closed_components(const CadAircraft *a)
{
    int i, j, k, n, end, from, to, t, e, directed, reverse, changed;
    int bodies = 0, wings = 0, tails = 0, engines = 0, nacelles = 0;
    const CadAircraftComponent *c;
    const CadExportVertex *p, *q, *r;
    long volume;
    for (i = 0; i < a->component_count; ++i) {
        c = &a->components[i]; end = c->first_triangle + c->triangle_count;
        if (c->material == CAD_AIRCRAFT_BODY) ++bodies;
        if (c->material == CAD_AIRCRAFT_WING) {
            ++wings;
            CHECK(c->vertex_count == 16);
            CHECK(a->vertices[c->first_vertex].x >
                  a->vertices[c->first_vertex + 8].x); /* Sweep aft. */
            CHECK(a->vertices[c->first_vertex + 12].y >
                  a->vertices[c->first_vertex + 8].y + 20); /* Winglet. */
            CHECK(a->vertices[c->first_vertex].x -
                  a->vertices[c->first_vertex + 1].x >
                  a->vertices[c->first_vertex + 8].x -
                  a->vertices[c->first_vertex + 9].x); /* Taper. */
        }
        if (c->material == CAD_AIRCRAFT_TAIL) ++tails;
        if (c->material == CAD_AIRCRAFT_ENGINE) {
            ++engines;
            if (c->vertex_count == 16) {
                ++nacelles;
                CHECK(a->vertices[c->first_vertex].y < 0);
                CHECK(a->vertices[c->first_vertex].x >
                      a->vertices[c->first_vertex + 8].x);
            }
        }
        CHECK(c->triangle_count == 2 * c->vertex_count - 4); /* Euler sphere. */
        volume = 0L;
        for (j = c->first_triangle; j < end; ++j) {
            p = &a->vertices[a->triangles[j].a];
            q = &a->vertices[a->triangles[j].b];
            r = &a->vertices[a->triangles[j].c];
            volume += (long)p->x * ((long)q->y * r->z - (long)q->z * r->y) +
                      (long)p->y * ((long)q->z * r->x - (long)q->x * r->z) +
                      (long)p->z * ((long)q->x * r->y - (long)q->y * r->x);
            for (k = 0; k < 3; ++k) {
                from = face_vertex(&a->triangles[j], k);
                to = face_vertex(&a->triangles[j], (k + 1) % 3);
                directed = reverse = 0;
                for (t = c->first_triangle; t < end; ++t) {
                    for (e = 0; e < 3; ++e) {
                        int x = face_vertex(&a->triangles[t], e);
                        int y = face_vertex(&a->triangles[t], (e + 1) % 3);
                        if (from == x && to == y) ++directed;
                        if (from == y && to == x) ++reverse;
                    }
                }
                CHECK(directed == 1 && reverse == 1); /* Manifold and winding. */
            }
        }
        CHECK(volume > 0L); /* Consistency above + positive volume => outward. */
        memset(seen, 0, sizeof(seen)); seen[c->first_vertex] = 1;
        do {
            changed = 0;
            for (j = 0; j < a->edge_count; ++j) {
                from = a->edges[j].a; to = a->edges[j].b;
                if (from < c->first_vertex || from >= c->first_vertex + c->vertex_count)
                    continue;
                CHECK(to >= c->first_vertex && to < c->first_vertex + c->vertex_count);
                if (seen[from] != seen[to]) { seen[from] = seen[to] = 1; changed = 1; }
            }
        } while (changed);
        n = 0;
        for (j = c->first_vertex; j < c->first_vertex + c->vertex_count; ++j)
            n += seen[j];
        CHECK(n == c->vertex_count);
    }
    CHECK(bodies == 1 && wings == 2 && tails == 3 && engines == 8 && nacelles == 4);
}

static void invalid_assets(const CadAircraft *a)
{
    CHECK(cad_aircraft_validate(NULL) == CAD_INVALID_ARGUMENT);
    memcpy(&broken, a, sizeof(broken)); broken.vertex_count = 241;
    CHECK(cad_aircraft_validate(&broken) == CAD_CAPACITY);
    memcpy(&broken, a, sizeof(broken)); broken.edge_count = 513;
    CHECK(cad_aircraft_validate(&broken) == CAD_CAPACITY);
    memcpy(&broken, a, sizeof(broken)); broken.triangle_count = 513;
    CHECK(cad_aircraft_validate(&broken) == CAD_CAPACITY);
    memcpy(&broken, a, sizeof(broken)); broken.vertex_count = -1;
    CHECK(cad_aircraft_validate(&broken) == CAD_INVALID_ARGUMENT);
    memcpy(&broken, a, sizeof(broken)); broken.vertices[0].x = 401;
    CHECK(cad_aircraft_validate(&broken) == CAD_RANGE);
    memcpy(&broken, a, sizeof(broken)); broken.edges[0].a = -1;
    CHECK(cad_aircraft_validate(&broken) == CAD_INVALID_ARGUMENT);
    memcpy(&broken, a, sizeof(broken)); broken.edges[0].b = a->vertex_count;
    CHECK(cad_aircraft_validate(&broken) == CAD_INVALID_ARGUMENT);
    memcpy(&broken, a, sizeof(broken)); broken.edges[0].style = 1;
    CHECK(cad_aircraft_validate(&broken) == CAD_INVALID_ARGUMENT);
    memcpy(&broken, a, sizeof(broken));
    broken.edges[1].a = broken.edges[0].b; broken.edges[1].b = broken.edges[0].a;
    CHECK(cad_aircraft_validate(&broken) == CAD_INVALID_ARGUMENT);
    memcpy(&broken, a, sizeof(broken)); broken.triangles[0].a = 65535U;
    CHECK(cad_aircraft_validate(&broken) == CAD_INVALID_ARGUMENT);
    memcpy(&broken, a, sizeof(broken)); broken.triangles[0].c = broken.triangles[0].a;
    CHECK(cad_aircraft_validate(&broken) == CAD_INVALID_ARGUMENT);
    memcpy(&broken, a, sizeof(broken)); broken.triangles[0].material = 5;
    CHECK(cad_aircraft_validate(&broken) == CAD_INVALID_ARGUMENT);
    memcpy(&broken, a, sizeof(broken)); broken.components[0].vertex_count = 32767;
    CHECK(cad_aircraft_validate(&broken) == CAD_INVALID_ARGUMENT);
    memcpy(&broken, a, sizeof(broken)); broken.components[1].first_vertex = 0;
    CHECK(cad_aircraft_validate(&broken) == CAD_INVALID_ARGUMENT);
    memcpy(&broken, a, sizeof(broken)); broken.triangles[0].a = 80;
    CHECK(cad_aircraft_validate(&broken) == CAD_INVALID_ARGUMENT);
    memcpy(&broken, a, sizeof(broken)); broken.scale_denominator = 0L;
    CHECK(cad_aircraft_validate(&broken) == CAD_INVALID_ARGUMENT);
    memcpy(&broken, a, sizeof(broken)); broken.units = CAD_UNITS_MM;
    CHECK(cad_aircraft_validate(&broken) == CAD_INVALID_ARGUMENT);
    OK(cad_aircraft_validate(a)); /* Validator never changes the singleton. */
}

static void loaded(const CadAircraft *a)
{
    int i;
    CHECK(model.vertex_count == a->vertex_count && model.edge_count == a->edge_count);
    CHECK(model.next_vertex_id == (long)a->vertex_count + 1L);
    CHECK(model.next_edge_id == (long)a->edge_count + 1L);
    CHECK(model.units == CAD_UNITS_UNSPECIFIED);
    CHECK(!model.transaction_active && !model.staged_count && !model.history_count &&
          !model.history_cursor && !model.history_used);
    for (i = 0; i < a->vertex_count; ++i) {
        CHECK(model.vertices[i].id == i + 1);
        CHECK(model.vertices[i].x == a->vertices[i].x &&
              model.vertices[i].y == a->vertices[i].y &&
              model.vertices[i].z == a->vertices[i].z);
    }
    for (i = 0; i < a->edge_count; ++i) {
        CHECK(model.edges[i].id == i + 1);
        CHECK(model.edges[i].a == model.vertices[a->edges[i].a].id &&
              model.edges[i].b == model.vertices[a->edges[i].b].id);
    }
    CHECK(cad_model_undo(&model) == CAD_NO_HISTORY);
    CHECK(cad_model_redo(&model) == CAD_NO_HISTORY);
}

static void loader(const CadAircraft *a)
{
    CadId id, other, edge_id;
    int i;
    CHECK(cad_aircraft_load(NULL) == CAD_INVALID_ARGUMENT);
    OK(cad_model_init(&model));
    CHECK(model.vertex_count == 0); /* Accessing asset did not load a document. */
    OK(cad_aircraft_load(&model)); loaded(a); SAVE();
    OK(cad_aircraft_load(&model)); SAME();
    OK(cad_model_begin(&model));
    OK(cad_model_move_vertex(&model, model.vertices[0].id, 351L, 0L, 0L));
    SAVE();
    CHECK(cad_aircraft_load(&model) == CAD_TRANSACTION_ACTIVE); SAME();
    OK(cad_model_cancel(&model));
    OK(cad_model_begin(&model));
    OK(cad_model_move_vertex(&model, model.vertices[0].id, 351L, 0L, 0L));
    OK(cad_model_commit(&model)); OK(cad_model_undo(&model));
    CHECK(model.history_cursor == 0 && model.history_count == 1);
    OK(cad_aircraft_load(&model)); loaded(a);
    /* Full old graph and exhausted namespaces must still allow File/Demo. */
    OK(cad_model_init(&model));
    for (i = 0; i < CAD_MAX_VERTICES; ++i) {
        if (i % 64 == 0) OK(cad_model_begin(&model));
        OK(cad_model_add_vertex(&model, i, 0L, 0L, &id));
        if (i % 64 == 63) OK(cad_model_commit(&model));
    }
    for (i = 0; i < CAD_MAX_EDGES; ++i) {
        if (i % 64 == 0) OK(cad_model_begin(&model));
        OK(cad_model_add_edge(&model, 1, 2, &id));
        if (i % 64 == 63) OK(cad_model_commit(&model));
    }
    OK(cad_model_begin(&model)); OK(cad_model_set_units(&model, CAD_UNITS_INCHES));
    OK(cad_model_commit(&model));
    model.next_vertex_id = model.next_edge_id = 65536L;
    OK(cad_aircraft_load(&model)); loaded(a);
    /* Ordinary edits and undo/redo remain usable after history-free import. */
    SAVE(); OK(cad_model_begin(&model));
    OK(cad_model_add_vertex(&model, 0L, 0L, 0L, &id));
    OK(cad_model_add_vertex(&model, 1L, 1L, 1L, &other));
    OK(cad_model_add_edge(&model, id, other, &edge_id));
    CHECK(id == a->vertex_count + 1 && other == id + 1 && edge_id == a->edge_count + 1);
    OK(cad_model_commit(&model)); OK(cad_model_undo(&model));
    CHECK(model.vertex_count == saved.vertex_count && model.edge_count == saved.edge_count);
    CHECK(!memcmp(model.vertices, saved.vertices, (size_t)saved.vertex_count * sizeof(CadVertex)));
    CHECK(!memcmp(model.edges, saved.edges, (size_t)saved.edge_count * sizeof(CadEdge)));
    OK(cad_model_redo(&model)); CHECK(model.vertex_count == a->vertex_count + 2);
    OK(cad_aircraft_load(&model)); SAME();
}

int main(void)
{
    const CadAircraft *a = cad_aircraft_get();
    CHECK(a != NULL);
    geometry(a); closed_components(a); invalid_assets(a); loader(a);
    printf("aircraft: %lu checks PASS (%d vertices, %d edges, %d triangles)\n",
           checks, a->vertex_count, a->edge_count, a->triangle_count);
    return 0;
}
