#include "../cad_create.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CadModel model;
static CadModel saved;
static CadSketchMesh solid;
static unsigned long checks;

#define CHECK(e) do { ++checks; if (!(e)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #e); exit(1); \
} } while (0)
#define OK(e) CHECK((e) == CAD_OK)
#define SAVE() memcpy(&saved, &model, sizeof(model))
#define SAME() CHECK(memcmp(&saved, &model, sizeof(model)) == 0)
#define FAIL(e, s) do { SAVE(); CHECK((e) == (s)); SAME(); } while (0)

static void position(int i, long x, long y, long z)
{
    CHECK(model.vertices[i].x == x);
    CHECK(model.vertices[i].y == y);
    CHECK(model.vertices[i].z == z);
}

static void endpoints(int e, int a, int b)
{
    CHECK(model.edges[e].a == model.vertices[a].id);
    CHECK(model.edges[e].b == model.vertices[b].id);
}

static void graph(void)
{
    CadVertex a, b;
    int i;
    for (i = 0; i < model.edge_count; ++i) {
        OK(cad_model_get_vertex(&model, model.edges[i].a, &a));
        OK(cad_model_get_vertex(&model, model.edges[i].b, &b));
        CHECK(a.x != b.x || a.y != b.y || a.z != b.z);
    }
}

static void geometry_equal(void)
{
    CHECK(model.vertex_count == saved.vertex_count);
    CHECK(model.edge_count == saved.edge_count);
    CHECK(model.units == saved.units);
    CHECK(!memcmp(model.vertices, saved.vertices,
                  (size_t)model.vertex_count * sizeof(CadVertex)));
    CHECK(!memcmp(model.edges, saved.edges,
                  (size_t)model.edge_count * sizeof(CadEdge)));
}

static void primitives(void)
{
    CadId first, second;
    int i;
    OK(cad_model_init(&model));
    CHECK(!model.vertex_count && !model.edge_count);
    OK(cad_create_box(&model, -100L, -90L, -80L, 20L, 30L, 40L));
    CHECK(model.vertex_count == 8 && model.edge_count == 12);
    CHECK(model.history_count == 1 && model.lengths[0] == 20);
    first = model.vertices[0].id;
    position(0, -100L, -90L, -80L); position(2, -80L, -60L, -80L);
    position(6, -80L, -60L, -40L);
    SAVE();
    OK(cad_create_box(&model, 100L, 110L, 120L, 40L, 50L, 60L));
    CHECK(model.vertex_count == 16 && model.edge_count == 24);
    CHECK(model.history_count == 2 && model.lengths[1] == 20);
    second = model.vertices[8].id; CHECK(second > first);
    position(0, -100L, -90L, -80L); position(14, 140L, 160L, 180L);
    for (i = 0; i < 12; ++i) {
        CHECK(model.edges[i].a < second && model.edges[i].b < second);
        CHECK(model.edges[i + 12].a >= second && model.edges[i + 12].b >= second);
    }
    graph();
    OK(cad_model_undo(&model)); geometry_equal();
    CHECK(model.next_vertex_id == 17L && model.next_edge_id == 25L);
    OK(cad_model_redo(&model)); CHECK(model.vertices[8].id == second);
    SAVE();
    OK(cad_create_line(&model, -400L, 400L, -400L, 400L, -400L, 400L));
    CHECK(model.vertex_count == 18 && model.edge_count == 25);
    position(16, -400L, 400L, -400L); position(17, 400L, -400L, 400L);
    endpoints(24, 16, 17);
    CHECK(model.lengths[2] == 3);
    OK(cad_model_undo(&model)); geometry_equal(); OK(cad_model_redo(&model));
    SAVE();
    OK(cad_create_rectangle(&model, -400L, -400L, 400L, 800L, 800L));
    CHECK(model.vertex_count == 22 && model.edge_count == 29);
    CHECK(model.lengths[3] == 8);
    position(18, -400L, -400L, 400L); position(20, 400L, 400L, 400L);
    for (i = 0; i < 4; ++i) endpoints(25 + i, 18 + i, 18 + (i + 1) % 4);
    graph(); OK(cad_model_undo(&model)); geometry_equal();
    OK(cad_model_redo(&model));
    OK(cad_model_init(&model));
    OK(cad_create_box(&model, -400L, -400L, -400L, 800L, 800L, 800L));
    position(6, 400L, 400L, 400L); graph();
    OK(cad_model_undo(&model)); CHECK(!model.vertex_count && !model.edge_count);
}

static const CadSketchPoint concave[6] = {
    {0,0},{100,0},{100,40},{40,40},{40,100},{0,100}
};
static const CadSketchPoint twelve[12] = {
    {0,0},{10,0},{20,0},{30,0},{30,10},{30,20},
    {30,30},{20,30},{10,30},{0,30},{0,20},{0,10}
};

static void extrusion(const CadSketchPoint *p, int n, int h)
{
    int i, j, base, edge;
    long volume;
    CHECK(cad_sketch_extrude(p, n, h, &solid) == CAD_SKETCH_OK);
    CHECK(cad_sketch_mesh_validate(&solid) == CAD_SKETCH_OK);
    CHECK(solid.kind == CAD_SKETCH_CLOSED_MESH);
    CHECK(cad_sketch_mesh_volume6(&solid, &volume) == CAD_SKETCH_OK);
    CHECK(volume > 0L);
    base = model.vertex_count; edge = model.edge_count;
    SAVE();
    OK(cad_create_extrusion(&model, p, n, h));
    CHECK(model.vertex_count == base + 2 * n);
    CHECK(model.edge_count == edge + 3 * n);
    CHECK(model.lengths[model.history_cursor - 1] == 5 * n);
    for (i = 0; i < n; ++i) {
        j = (i + 1) % n;
        position(base + i, solid.vertices[i].x, solid.vertices[i].y, 0L);
        position(base + i + n, solid.vertices[i].x, solid.vertices[i].y, h);
        endpoints(edge + i, base + i, base + j);
        endpoints(edge + i + n, base + i + n, base + j + n);
        endpoints(edge + i + 2 * n, base + i, base + i + n);
    }
    graph(); OK(cad_model_undo(&model)); geometry_equal();
    OK(cad_model_redo(&model));
}

static void profiles(void)
{
    CadSketchPoint cw[6];
    const CadSketchPoint triangle[3] = {{-400,-400},{400,-400},{0,400}};
    int i;
    OK(cad_model_init(&model));
    OK(cad_create_line(&model, -10L, -10L, -10L, -9L, -9L, -9L));
    extrusion(concave, 6, 400);
    for (i = 0; i < 6; ++i) cw[i] = concave[5 - i];
    extrusion(cw, 6, 1);
    extrusion(twelve, 12, 10);
    extrusion(triangle, 3, 20);
}

static void invalid_input(void)
{
    const CadSketchPoint crossing[4] = {{0,0},{100,100},{0,100},{100,0}};
    const CadSketchPoint duplicate[4] = {{0,0},{100,0},{100,0},{0,100}};
    const CadSketchPoint flat[3] = {{0,0},{1,1},{2,2}};
    const CadSketchPoint overlap[4] = {{0,0},{100,0},{50,0},{0,100}};
    const CadSketchPoint range[3] = {{0,0},{401,0},{0,100}};
    OK(cad_model_init(&model));
    OK(cad_create_box(&model, 0L, 0L, 0L, 10L, 20L, 30L));
    OK(cad_model_undo(&model)); /* Failures must also preserve redo. */
    FAIL(cad_create_line(&model, 1L, 2L, 3L, 1L, 2L, 3L), CAD_INVALID_ARGUMENT);
    FAIL(cad_create_line(&model, LONG_MIN, 0L, 0L, LONG_MAX, 0L, 0L), CAD_RANGE);
    FAIL(cad_create_rectangle(&model, 0L, 0L, 0L, 0L, 1L), CAD_RANGE);
    FAIL(cad_create_rectangle(&model, 0L, 0L, 0L, 1L, -1L), CAD_RANGE);
    FAIL(cad_create_rectangle(&model, LONG_MAX, 0L, 0L, LONG_MAX, 1L), CAD_RANGE);
    FAIL(cad_create_rectangle(&model, -400L, 0L, 0L, LONG_MAX, 1L), CAD_RANGE);
    FAIL(cad_create_rectangle(&model, 400L, 0L, 0L, 1L, 1L), CAD_RANGE);
    FAIL(cad_create_rectangle(&model, 0L, 0L, 401L, 1L, 1L), CAD_RANGE);
    FAIL(cad_create_box(&model, 0L, 0L, 0L, 1L, 1L, 0L), CAD_RANGE);
    FAIL(cad_create_box(&model, 0L, 0L, 400L, 1L, 1L, 1L), CAD_RANGE);
    FAIL(cad_create_box(&model, 0L, 0L, LONG_MIN, 1L, 1L, LONG_MAX), CAD_RANGE);
#if LONG_MAX > 2147483647L
    FAIL(cad_create_rectangle(&model, 0L, 0L, 0L, 2147483647L + 1L, 1L), CAD_RANGE);
    FAIL(cad_create_line(&model, -2147483647L - 2L, 0L, 0L, 0L, 0L, 0L), CAD_RANGE);
#endif
    FAIL(cad_create_extrusion(&model, concave, 2, 10), CAD_INVALID_ARGUMENT);
    FAIL(cad_create_extrusion(&model, concave, 13, 10), CAD_INVALID_ARGUMENT);
    FAIL(cad_create_extrusion(&model, concave, INT_MAX, 10), CAD_INVALID_ARGUMENT);
    FAIL(cad_create_extrusion(&model, NULL, 6, 10), CAD_INVALID_ARGUMENT);
    FAIL(cad_create_extrusion(&model, concave, 6, 0), CAD_RANGE);
    FAIL(cad_create_extrusion(&model, concave, 6, 401), CAD_RANGE);
    FAIL(cad_create_extrusion(&model, crossing, 4, 10), CAD_INVALID_ARGUMENT);
    FAIL(cad_create_extrusion(&model, duplicate, 4, 10), CAD_INVALID_ARGUMENT);
    FAIL(cad_create_extrusion(&model, flat, 3, 10), CAD_INVALID_ARGUMENT);
    FAIL(cad_create_extrusion(&model, overlap, 4, 10), CAD_INVALID_ARGUMENT);
    FAIL(cad_create_extrusion(&model, range, 3, 10), CAD_RANGE);
    CHECK(cad_create_line(NULL, 0L, 0L, 0L, 1L, 1L, 1L) == CAD_INVALID_ARGUMENT);
    CHECK(cad_create_rectangle(NULL, 0L, 0L, 0L, 1L, 1L) == CAD_INVALID_ARGUMENT);
    CHECK(cad_create_box(NULL, 0L, 0L, 0L, 1L, 1L, 1L) == CAD_INVALID_ARGUMENT);
    CHECK(cad_create_extrusion(NULL, concave, 6, 10) == CAD_INVALID_ARGUMENT);
    CHECK(cad_create_house_demo(NULL) == CAD_INVALID_ARGUMENT);
    OK(cad_model_redo(&model)); CHECK(model.vertex_count == 8);
    OK(cad_model_begin(&model));
    FAIL(cad_create_line(&model, 0L, 0L, 0L, 1L, 1L, 1L), CAD_TRANSACTION_ACTIVE);
    FAIL(cad_create_rectangle(&model, 0L, 0L, 0L, 1L, 1L), CAD_TRANSACTION_ACTIVE);
    FAIL(cad_create_box(&model, 0L, 0L, 0L, 1L, 1L, 1L), CAD_TRANSACTION_ACTIVE);
    FAIL(cad_create_extrusion(&model, concave, 6, 10), CAD_TRANSACTION_ACTIVE);
    FAIL(cad_create_house_demo(&model), CAD_TRANSACTION_ACTIVE);
    OK(cad_model_cancel(&model));
}

static void capacity(void)
{
    CadId id;
    int i;
    OK(cad_model_init(&model));
    for (i = 0; i < 255; ++i) {
        if (i % 64 == 0) OK(cad_model_begin(&model));
        OK(cad_model_add_vertex(&model, (long)i, 0L, 0L, &id));
        if (i % 64 == 63 || i == 254) OK(cad_model_commit(&model));
    }
    FAIL(cad_create_line(&model, 0L, 1L, 0L, 1L, 1L, 0L), CAD_CAPACITY);
    FAIL(cad_create_rectangle(&model, 0L, 0L, 0L, 1L, 1L), CAD_CAPACITY);
    FAIL(cad_create_box(&model, 0L, 0L, 0L, 1L, 1L, 1L), CAD_CAPACITY);
    FAIL(cad_create_extrusion(&model, concave, 6, 10), CAD_CAPACITY);
    OK(cad_model_init(&model));
    OK(cad_create_line(&model, 0L, 0L, 0L, 1L, 1L, 1L));
    for (i = 1; i < 512; ++i) {
        if ((i - 1) % 64 == 0) OK(cad_model_begin(&model));
        OK(cad_model_add_edge(&model, 1, 2, &id));
        if ((i - 1) % 64 == 63 || i == 511) OK(cad_model_commit(&model));
    }
    FAIL(cad_create_line(&model, 0L, 0L, 0L, 1L, 1L, 1L), CAD_CAPACITY);
    FAIL(cad_create_box(&model, 0L, 0L, 0L, 1L, 1L, 1L), CAD_CAPACITY);
    FAIL(cad_create_extrusion(&model, concave, 6, 10), CAD_CAPACITY);
    OK(cad_model_init(&model));
    /* Place otherwise initialized counters at namespace boundaries. */
    model.next_vertex_id = 65535L;
    FAIL(cad_create_line(&model, 0L, 0L, 0L, 1L, 1L, 1L), CAD_ID_EXHAUSTED);
    model.next_vertex_id = 65534L; model.next_edge_id = 65535L;
    OK(cad_create_line(&model, 0L, 0L, 0L, 1L, 1L, 1L));
    CHECK(model.vertices[1].id == 65535U && model.edges[0].id == 65535U);
    CHECK(model.next_vertex_id == 65536L && model.next_edge_id == 65536L);
    OK(cad_model_undo(&model));
    FAIL(cad_create_line(&model, 0L, 0L, 0L, 1L, 1L, 1L), CAD_ID_EXHAUSTED);
    OK(cad_model_init(&model)); model.next_edge_id = 65525L;
    FAIL(cad_create_box(&model, 0L, 0L, 0L, 1L, 1L, 1L), CAD_ID_EXHAUSTED);
    model.next_edge_id = 65536L;
    FAIL(cad_create_line(&model, 0L, 0L, 0L, 1L, 1L, 1L), CAD_ID_EXHAUSTED);
    model.next_edge_id = 65501L;
    FAIL(cad_create_extrusion(&model, twelve, 12, 10), CAD_ID_EXHAUSTED);
    model.next_edge_id = 65500L; model.next_vertex_id = 65512L;
    OK(cad_create_extrusion(&model, twelve, 12, 10));
    CHECK(model.next_vertex_id == 65536L && model.next_edge_id == 65536L);
    graph();
}

static void house(void)
{
    static const short vertices[30][3] = {
        {-100,-70,70},{100,-70,70},{100,-70,-70},{-100,-70,-70},
        {-100,30,70},{100,30,70},{100,30,-70},{-100,30,-70},
        {-100,100,0},{100,100,0},
        {-15,-70,70},{-15,0,70},{15,0,70},{15,-70,70},
        {-75,-30,70},{-35,-30,70},{-35,10,70},{-75,10,70},
        {35,-30,70},{75,-30,70},{75,10,70},{35,10,70},
        {40,55,-45},{65,55,-45},{65,75,-25},{40,75,-25},
        {40,120,-45},{65,120,-45},{65,120,-25},{40,120,-25}
    };
    static const unsigned short edges[40][2] = {
        {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},
        {0,4},{1,5},{2,6},{3,7},{8,9},{4,8},{7,8},{5,9},{6,9},
        {10,11},{11,12},{12,13},{14,15},{15,16},{16,17},{17,14},
        {18,19},{19,20},{20,21},{21,18},{22,23},{23,24},{24,25},
        {25,22},{26,27},{27,28},{28,29},{29,26},{22,26},
        {23,27},{24,28},{25,29}
    };
    int i;
    OK(cad_model_init(&model));
    OK(cad_create_box(&model, 0L, 0L, 0L, 10L, 10L, 10L));
    OK(cad_model_begin(&model)); OK(cad_model_set_units(&model, CAD_UNITS_MM));
    OK(cad_model_commit(&model)); OK(cad_model_undo(&model));
    model.next_vertex_id = model.next_edge_id = 65536L;
    OK(cad_create_house_demo(&model));
    CHECK(model.vertex_count == 30 && model.edge_count == 40);
    CHECK(model.units == CAD_UNITS_UNSPECIFIED);
    CHECK(!model.transaction_active && !model.staged_count);
    CHECK(!model.history_count && !model.history_cursor && !model.history_used);
    CHECK(model.next_vertex_id == 31L && model.next_edge_id == 41L);
    for (i = 0; i < 30; ++i)
        position(i, vertices[i][0], vertices[i][1], vertices[i][2]);
    for (i = 0; i < 40; ++i) endpoints(i, edges[i][0], edges[i][1]);
    graph();
    CHECK(cad_model_undo(&model) == CAD_NO_HISTORY);
    CHECK(cad_model_redo(&model) == CAD_NO_HISTORY);
    SAVE(); OK(cad_create_line(&model, 0L, 0L, 0L, 1L, 1L, 1L));
    OK(cad_model_undo(&model)); geometry_equal();
    OK(cad_create_house_demo(&model)); SAME();
}

int main(void)
{
    primitives(); profiles(); invalid_input(); capacity(); house();
    printf("create: %lu checks PASS\n", checks);
    return 0;
}
