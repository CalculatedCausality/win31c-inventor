#include "../cad_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Caller-owned storage; no large automatic object on the Win16 stack. */
static CadModel model, before;
static unsigned long checks;

#define CHECK(expr) do { ++checks; if (!(expr)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); exit(1); \
} } while (0)
#define OK(expr) CHECK((expr) == CAD_OK)
#define SAVE() memcpy(&before, &model, sizeof(model))
#define UNCHANGED() CHECK(memcmp(&before, &model, sizeof(model)) == 0)

static CadId vertex(long x, long y, long z)
{
    CadId id = 0;
    OK(cad_model_add_vertex(&model, x, y, z, &id));
    CHECK(id != 0);
    return id;
}

static CadId edge(CadId a, CadId b)
{
    CadId id = 0;
    OK(cad_model_add_edge(&model, a, b, &id));
    CHECK(id != 0);
    return id;
}

static void position(CadId id, long x, long y, long z)
{
    CadVertex v;
    OK(cad_model_get_vertex(&model, id, &v));
    CHECK(v.id == id && v.x == x && v.y == y && v.z == z);
}

static void endpoints(CadId id, CadId a, CadId b)
{
    CadEdge e;
    OK(cad_model_get_edge(&model, id, &e));
    CHECK(e.id == id && e.a == a && e.b == b);
}

static void stable_references(void)
{
    CadId a, b, c, ab, bc, cc, d;
    CadVertex v;
    CadEdge e;
    OK(cad_model_init(&model));
    CHECK(model.units == CAD_UNITS_UNSPECIFIED);
    OK(cad_model_begin(&model));
    a = vertex(-400L, 400L, 0L); b = vertex(10L, 20L, 30L);
    c = vertex(40L, 50L, 60L);
    ab = edge(a, b); bc = edge(b, c); cc = edge(c, c);
    OK(cad_model_commit(&model));
    OK(cad_model_begin(&model));
    OK(cad_model_delete_vertex(&model, a));
    CHECK(model.vertices[0].id == c); /* dense deletion reorders */
    CHECK(model.vertex_count == 2 && model.edge_count == 2);
    CHECK(cad_model_get_vertex(&model, a, &v) == CAD_NOT_FOUND);
    CHECK(cad_model_get_edge(&model, ab, &e) == CAD_NOT_FOUND);
    endpoints(bc, b, c); endpoints(cc, c, c);
    OK(cad_model_move_vertex(&model, c, 100L, -100L, 200L));
    OK(cad_model_commit(&model));
    OK(cad_model_undo(&model));
    position(a, -400L, 400L, 0L); position(c, 40L, 50L, 60L);
    endpoints(ab, a, b); endpoints(bc, b, c); endpoints(cc, c, c);
    OK(cad_model_redo(&model));
    position(c, 100L, -100L, 200L); endpoints(bc, b, c);
    OK(cad_model_begin(&model));
    OK(cad_model_delete_vertex(&model, c));
    CHECK(model.edge_count == 0 && model.vertex_count == 1);
    OK(cad_model_cancel(&model));
    endpoints(bc, b, c); endpoints(cc, c, c);
    OK(cad_model_begin(&model));
    d = vertex(0L, 0L, 0L); CHECK(d > c);
    CHECK(cad_model_move_vertex(&model, a, 0L, 0L, 0L) == CAD_NOT_FOUND);
    OK(cad_model_cancel(&model));
    OK(cad_model_begin(&model));
    a = vertex(0L, 0L, 0L); CHECK(a > d);
    OK(cad_model_cancel(&model));
}

static void transactions(void)
{
    CadId a, b, e;
    unsigned short count;
    OK(cad_model_init(&model));
    OK(cad_model_begin(&model)); a = vertex(0L, 0L, 0L);
    b = vertex(1L, 2L, 3L); e = edge(a, b);
    OK(cad_model_commit(&model));
    OK(cad_model_begin(&model));
    OK(cad_model_move_vertex(&model, a, 10L, 20L, 30L));
    OK(cad_model_set_units(&model, CAD_UNITS_MM));
    OK(cad_model_delete_edge(&model, e));
    OK(cad_model_commit(&model));
    OK(cad_model_undo(&model));
    count = model.history_count;
    OK(cad_model_begin(&model)); OK(cad_model_commit(&model));
    CHECK(model.history_count == count && model.history_cursor == 1);
    OK(cad_model_begin(&model));
    OK(cad_model_move_vertex(&model, a, 9L, 9L, 9L));
    OK(cad_model_move_vertex(&model, a, 0L, 0L, 0L));
    OK(cad_model_set_units(&model, CAD_UNITS_INCHES));
    OK(cad_model_set_units(&model, CAD_UNITS_UNSPECIFIED));
    b = vertex(0L, 0L, 0L); edge(a, b);
    OK(cad_model_delete_vertex(&model, b));
    OK(cad_model_commit(&model));
    CHECK(model.history_count == count && model.history_cursor == 1);
    OK(cad_model_redo(&model)); position(a, 10L, 20L, 30L);
    CHECK(model.units == CAD_UNITS_MM && model.edge_count == 0);
    OK(cad_model_undo(&model));
    OK(cad_model_begin(&model));
    OK(cad_model_move_vertex(&model, a, 5L, 6L, 7L));
    OK(cad_model_cancel(&model));
    position(a, 0L, 0L, 0L); endpoints(e, a, 2);
    CHECK(model.history_count == count && model.history_cursor == 1);
    OK(cad_model_begin(&model));
    OK(cad_model_move_vertex(&model, a, -1L, -2L, -3L));
    OK(cad_model_commit(&model));
    CHECK(cad_model_redo(&model) == CAD_NO_HISTORY);
    OK(cad_model_undo(&model)); position(a, 0L, 0L, 0L);
    OK(cad_model_redo(&model)); position(a, -1L, -2L, -3L);
}

static void validation(void)
{
    CadId a, output = 123;
    CadVertex v;
    CadEdge e;
    OK(cad_model_init(&model)); SAVE();
    CHECK(cad_model_begin(NULL) == CAD_INVALID_ARGUMENT);
    CHECK(cad_model_init(NULL) == CAD_INVALID_ARGUMENT);
    CHECK(cad_model_add_vertex(&model, 0L, 0L, 0L, &output) == CAD_NO_TRANSACTION);
    CHECK(output == 123);
    CHECK(cad_model_cancel(&model) == CAD_NO_TRANSACTION);
    CHECK(cad_model_commit(&model) == CAD_NO_TRANSACTION);
    CHECK(cad_model_undo(&model) == CAD_NO_HISTORY);
    CHECK(cad_model_redo(&model) == CAD_NO_HISTORY); UNCHANGED();
    OK(cad_model_begin(&model)); SAVE();
    CHECK(cad_model_begin(&model) == CAD_TRANSACTION_ACTIVE);
    CHECK(cad_model_undo(&model) == CAD_TRANSACTION_ACTIVE);
    CHECK(cad_model_redo(&model) == CAD_TRANSACTION_ACTIVE);
    CHECK(cad_model_add_vertex(&model, LONG_MAX, 0L, 0L, &output) == CAD_RANGE);
    CHECK(cad_model_add_vertex(&model, 0L, LONG_MIN, 0L, &output) == CAD_RANGE);
    CHECK(cad_model_add_vertex(&model, 0L, 0L, 401L, &output) == CAD_RANGE);
    CHECK(cad_model_add_vertex(&model, -401L, 0L, 0L, &output) == CAD_RANGE);
    CHECK(cad_model_add_vertex(&model, 0L, 0L, 0L, NULL) == CAD_INVALID_ARGUMENT);
    CHECK(cad_model_set_units(&model, (CadUnits)77) == CAD_INVALID_ARGUMENT);
    CHECK(cad_model_add_edge(&model, 0, 0, &output) == CAD_NOT_FOUND);
    CHECK(output == 123); UNCHANGED();
    a = vertex(-400L, 400L, -400L); SAVE();
    CHECK(cad_model_move_vertex(&model, a, 401L, 0L, 0L) == CAD_RANGE);
    CHECK(cad_model_move_vertex(&model, 0, 0L, 0L, 0L) == CAD_NOT_FOUND);
    CHECK(cad_model_delete_vertex(&model, 0) == CAD_NOT_FOUND);
    CHECK(cad_model_delete_edge(&model, 0) == CAD_NOT_FOUND);
    CHECK(cad_model_get_vertex(&model, a, NULL) == CAD_INVALID_ARGUMENT);
    CHECK(cad_model_get_edge(&model, 0, NULL) == CAD_INVALID_ARGUMENT);
    memset(&v, 0, sizeof(v)); memset(&e, 0, sizeof(e));
    v.id = 42; e.id = 42;
    CHECK(cad_model_get_vertex(&model, 0, &v) == CAD_NOT_FOUND);
    CHECK(cad_model_get_edge(&model, 0, &e) == CAD_NOT_FOUND);
    CHECK(v.id == 42 && e.id == 42); UNCHANGED();
    OK(cad_model_cancel(&model));
    CHECK(strcmp(cad_model_status_string(CAD_TRANSACTION_FULL),
                 "transaction exceeds 64 deltas") == 0);
}

static void mixed_replay(void)
{
    CadId a, b, c, ab, bc, ac;
    CadVertex v;
    CadEdge e;
    unsigned short i;
    OK(cad_model_init(&model)); OK(cad_model_begin(&model));
    a = vertex(1L, 2L, 3L); b = vertex(4L, 5L, 6L); ab = edge(a, b);
    OK(cad_model_commit(&model)); OK(cad_model_begin(&model));
    c = vertex(7L, 8L, 9L); bc = edge(b, c); ac = edge(a, c);
    OK(cad_model_move_vertex(&model, c, -7L, -8L, -9L));
    OK(cad_model_move_vertex(&model, b, 10L, 11L, 12L));
    OK(cad_model_delete_vertex(&model, b));
    OK(cad_model_set_units(&model, CAD_UNITS_INCHES));
    SAVE();
    CHECK(cad_model_add_edge(&model, a, b, &bc) == CAD_NOT_FOUND);
    UNCHANGED();
    OK(cad_model_commit(&model));
    for (i = 0; i < 4; ++i) {
        OK(cad_model_undo(&model));
        CHECK(model.vertex_count == 2 && model.edge_count == 1);
        position(b, 4L, 5L, 6L); endpoints(ab, a, b);
        CHECK(cad_model_get_vertex(&model, c, &v) == CAD_NOT_FOUND);
        CHECK(model.units == CAD_UNITS_UNSPECIFIED);
        OK(cad_model_redo(&model));
        CHECK(model.vertex_count == 2 && model.edge_count == 1);
        position(c, -7L, -8L, -9L); endpoints(ac, a, c);
        CHECK(cad_model_get_vertex(&model, b, &v) == CAD_NOT_FOUND);
        CHECK(cad_model_get_edge(&model, ab, &e) == CAD_NOT_FOUND);
        CHECK(cad_model_get_edge(&model, bc, &e) == CAD_NOT_FOUND);
        CHECK(model.units == CAD_UNITS_INCHES);
    }
}

static void capacities(void)
{
    unsigned short i, j;
    CadId first = 0, last = 0, output = 99, a, b;
    OK(cad_model_init(&model));
    for (i = 0; i < 4; ++i) {
        OK(cad_model_begin(&model));
        for (j = 0; j < 64; ++j) {
            last = vertex((long)j, 0L, 0L);
            if (!first) first = last;
        }
        SAVE();
        CHECK(cad_model_add_vertex(&model, 0L, 0L, 0L, &output) == CAD_TRANSACTION_FULL);
        CHECK(output == 99); UNCHANGED();
        OK(cad_model_commit(&model));
    }
    CHECK(model.vertex_count == 256);
    OK(cad_model_begin(&model)); SAVE();
    CHECK(cad_model_add_vertex(&model, 0L, 0L, 0L, &output) == CAD_CAPACITY);
    UNCHANGED();
    OK(cad_model_delete_vertex(&model, first));
    output = vertex(0L, 0L, 0L); CHECK(output > last);
    OK(cad_model_commit(&model));
    OK(cad_model_undo(&model)); position(first, 0L, 0L, 0L);
    OK(cad_model_redo(&model)); position(output, 0L, 0L, 0L);
    for (i = 0; i < 8; ++i) {
        OK(cad_model_begin(&model));
        for (j = 0; j < 64; ++j) edge(last, output);
        OK(cad_model_commit(&model));
    }
    CHECK(model.edge_count == 512);
    OK(cad_model_begin(&model)); SAVE(); output = 99;
    CHECK(cad_model_add_edge(&model, last, last, &output) == CAD_CAPACITY);
    CHECK(output == 99);
    CHECK(cad_model_delete_vertex(&model, last) == CAD_TRANSACTION_FULL);
    UNCHANGED(); OK(cad_model_cancel(&model));

    /* Exactly 63 incident edges fit; 64 do not. Shared endpoints survive.
     * One self-edge is counted once, even though it has two incidences. */
    OK(cad_model_init(&model)); OK(cad_model_begin(&model));
    a = vertex(0L, 0L, 0L); b = vertex(1L, 1L, 1L);
    OK(cad_model_commit(&model)); OK(cad_model_begin(&model));
    for (i = 0; i < 63; ++i) edge(a, b);
    last = edge(a, a); OK(cad_model_commit(&model));
    OK(cad_model_begin(&model)); SAVE();
    CHECK(cad_model_delete_vertex(&model, a) == CAD_TRANSACTION_FULL);
    UNCHANGED(); OK(cad_model_cancel(&model));
    OK(cad_model_begin(&model)); OK(cad_model_delete_edge(&model, last));
    OK(cad_model_commit(&model)); OK(cad_model_begin(&model));
    OK(cad_model_move_vertex(&model, b, 2L, 2L, 2L)); SAVE();
    CHECK(cad_model_delete_vertex(&model, a) == CAD_TRANSACTION_FULL);
    UNCHANGED(); OK(cad_model_cancel(&model));
    OK(cad_model_begin(&model)); OK(cad_model_delete_vertex(&model, a));
    CHECK(model.staged_count == 64 && model.edge_count == 0);
    position(b, 1L, 1L, 1L); OK(cad_model_commit(&model));
    OK(cad_model_undo(&model));
    CHECK(model.edge_count == 63 && model.vertex_count == 2);
    position(b, 1L, 1L, 1L); OK(cad_model_redo(&model));
    CHECK(model.edge_count == 0 && model.vertex_count == 1);
}

static void history_limits(void)
{
    CadId a;
    unsigned short i, j, n;
    OK(cad_model_init(&model)); OK(cad_model_begin(&model));
    a = vertex(0L, 0L, 0L); OK(cad_model_commit(&model));
    for (i = 1; i <= 40; ++i) {
        OK(cad_model_begin(&model));
        OK(cad_model_move_vertex(&model, a, (long)i, 0L, 0L));
        OK(cad_model_commit(&model));
    }
    CHECK(model.history_count == 32);
    for (i = 0; i < 32; ++i) OK(cad_model_undo(&model));
    position(a, 8L, 0L, 0L);
    CHECK(cad_model_undo(&model) == CAD_NO_HISTORY);
    for (i = 0; i < 32; ++i) OK(cad_model_redo(&model));
    position(a, 40L, 0L, 0L);
    /* 64 moves per command exercise delta-pool eviction and compaction. */
    OK(cad_model_init(&model)); OK(cad_model_begin(&model));
    a = vertex(0L, 0L, 0L); OK(cad_model_commit(&model));
    for (i = 1; i <= 20; ++i) {
        OK(cad_model_begin(&model));
        for (j = 0; j < 63; ++j)
            OK(cad_model_move_vertex(&model, a, 100L + (long)(j % 2), 0L, 0L));
        OK(cad_model_move_vertex(&model, a, (long)i, 0L, 0L));
        OK(cad_model_commit(&model));
    }
    CHECK(model.history_used == 1024 && model.history_count == 16);
    for (i = 0; i < 16; ++i) OK(cad_model_undo(&model));
    position(a, 4L, 0L, 0L);
    CHECK(cad_model_undo(&model) == CAD_NO_HISTORY);
    for (i = 0; i < 8; ++i) OK(cad_model_redo(&model));
    position(a, 12L, 0L, 0L);
    OK(cad_model_begin(&model));
    OK(cad_model_move_vertex(&model, a, -400L, 0L, 0L));
    OK(cad_model_commit(&model));
    CHECK(model.history_count == 9 && model.history_used == 513);
    CHECK(cad_model_redo(&model) == CAD_NO_HISTORY);
    n = 0;
    while (cad_model_undo(&model) == CAD_OK) ++n;
    CHECK(n == 9); position(a, 4L, 0L, 0L);
}

static void id_limits(void)
{
    long i;
    CadId a = 0, b, e = 0, output = 99;
    OK(cad_model_init(&model));
    /* Allocators are exercised through the API, never modified by tests. */
    for (i = 1L; i <= 65534L; ++i) {
        OK(cad_model_begin(&model)); a = vertex(0L, 0L, 0L);
        CHECK((long)a == i); OK(cad_model_cancel(&model));
    }
    OK(cad_model_begin(&model)); b = vertex(0L, 0L, 0L);
    CHECK(b == 65535U); OK(cad_model_commit(&model));
    OK(cad_model_undo(&model)); OK(cad_model_redo(&model));
    OK(cad_model_begin(&model)); SAVE();
    CHECK(cad_model_add_vertex(&model, 0L, 0L, 0L, &output) == CAD_ID_EXHAUSTED);
    CHECK(output == 99); UNCHANGED(); OK(cad_model_cancel(&model));
    for (i = 1L; i <= 65535L; ++i) {
        OK(cad_model_begin(&model)); e = edge(b, b);
        CHECK((long)e == i); endpoints(e, b, b);
        if (i == 65535L) OK(cad_model_commit(&model));
        else OK(cad_model_cancel(&model));
    }
    OK(cad_model_undo(&model)); OK(cad_model_redo(&model));
    endpoints(e, b, b);
    OK(cad_model_begin(&model)); SAVE();
    CHECK(cad_model_add_edge(&model, b, b, &output) == CAD_ID_EXHAUSTED);
    CHECK(output == 99); UNCHANGED(); OK(cad_model_cancel(&model));
}

int main(void)
{
    volatile unsigned long delta_bytes = (unsigned long)sizeof(CadDelta);
    volatile unsigned long history_bytes = (unsigned long)(sizeof(model.history) +
        sizeof(model.staged) + sizeof(model.lengths) + 5 * sizeof(unsigned short));
    CHECK(delta_bytes == 16UL);
    CHECK(history_bytes < 32768UL);
    stable_references(); transactions(); validation(); mixed_replay(); capacities();
    history_limits(); id_limits();
    printf("model tests passed: %lu checks; history storage %lu bytes\n",
           checks, (unsigned long)(sizeof(model.history) +
           sizeof(model.staged) + sizeof(model.lengths) +
           5 * sizeof(unsigned short)));
    return 0;
}
