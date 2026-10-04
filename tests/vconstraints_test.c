/* Host (run from repository root):
 * cc -std=c89 -pedantic -Wall -Wextra -Werror cad_vconstraints.c \
 *    cad_constraints.c cad_model.c tests/vconstraints_test.c \
 *    -o /tmp/opencode/vconstraints_test && /tmp/opencode/vconstraints_test
 */
#include "../cad_vconstraints.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define STATUS(call, expected) CHECK((call) == (expected))
/* Failure must leave the constraint set and the move output untouched. */
#define FAIL(call, expected) do { \
    memcpy(&vc_before, &vc, sizeof(vc)); \
    memcpy(&moves_before, &moves, sizeof(moves)); \
    STATUS(call, expected); \
    CHECK(memcmp(&vc_before, &vc, sizeof(vc)) == 0); \
    CHECK(memcmp(&moves_before, &moves, sizeof(moves)) == 0); \
} while (0)

static CadModel model;
static CadVConstraints vc, vc_before, vc2;
static CadVcMoves moves, moves_before;
static CadId v[48];
static char text[CAD_VC_TEXT_MAX + 1], text2[CAD_VC_TEXT_MAX + 1];

static CadId vertex(long x, long y, long z)
{
    CadId id;
    STATUS(cad_model_begin(&model), CAD_OK);
    STATUS(cad_model_add_vertex(&model, x, y, z, &id), CAD_OK);
    STATUS(cad_model_commit(&model), CAD_OK);
    return id;
}

static void reset(void)
{
    STATUS(cad_model_init(&model), CAD_OK);
    STATUS(cad_vc_init(&vc), CAD_VC_OK);
    memset(&moves, 0, sizeof(moves));
}

static void at(CadId id, long x, long y, long z)
{
    CadVertex p;
    STATUS(cad_model_get_vertex(&model, id, &p), CAD_OK);
    CHECK(p.x == x && p.y == y && p.z == z);
}

/* Apply the move list in exactly one model transaction. */
static void apply(void)
{
    unsigned short before;
    before = model.history_count;
    STATUS(cad_model_begin(&model), CAD_OK);
    STATUS(cad_vc_stage_moves(&model, &moves), CAD_VC_OK);
    CHECK(model.staged_count <= CAD_MAX_TRANSACTION_DELTAS);
    STATUS(cad_model_commit(&model), CAD_OK);
    if (moves.count > 0)
        CHECK(model.history_count == before + 1 ||
              model.history_count == CAD_MAX_HISTORY_COMMANDS);
}

static void describe(CadVcId id, const char *expected)
{
    CadVConstraint c;
    char buf[CAD_VC_DESCRIBE_MAX];
    STATUS(cad_vc_get(&vc, id, &c), CAD_VC_OK);
    STATUS(cad_vc_describe(&c, buf, sizeof(buf)), CAD_VC_OK);
    if (strcmp(buf, expected) != 0) {
        fprintf(stderr, "describe: got '%s' want '%s'\n", buf, expected);
        exit(1);
    }
}

static int health(CadVcId id)
{
    CadVConstraint c;
    STATUS(cad_vc_get(&vc, id, &c), CAD_VC_OK);
    return c.health;
}

static void test_kinds(void)
{
    CadVcId id, d, f;
    CadVConstraint c;
    char small[8];
    reset();
    v[1] = vertex(0, 0, 0);
    v[2] = vertex(10, 20, 30);
    v[3] = vertex(5, 7, 9);
    v[4] = vertex(50, 60, 70);
    v[5] = vertex(1, 2, 3);

    STATUS(cad_vc_add(&vc, &model, CAD_VC_COINCIDENT, v[1], v[2], 0, 0L,
                      &moves, &id), CAD_VC_OK);
    CHECK(id == 1UL && moves.count == 1 && moves.items[0].id == v[2]);
    apply();
    at(v[2], 0, 0, 0);
    describe(id, "Coincident V1-V2");

    STATUS(cad_vc_add(&vc, &model, CAD_VC_ALIGN, v[3], v[4], 1, 0L,
                      &moves, &id), CAD_VC_OK);
    CHECK(moves.count == 1);
    apply();
    at(v[4], 50, 7, 70);
    describe(id, "Align Y V3-V4");

    STATUS(cad_vc_add(&vc, &model, CAD_VC_DISTANCE, v[1], v[5], 0, 50L,
                      &moves, &d), CAD_VC_OK);
    apply();
    at(v[5], 50, 2, 3);
    describe(d, "Distance X V1-V5 = 50");

    /* Already satisfied: no moves. */
    STATUS(cad_vc_add(&vc, &model, CAD_VC_ALIGN, v[1], v[2], 2, 0L,
                      &moves, &id), CAD_VC_OK);
    CHECK(moves.count == 0);
    STATUS(cad_vc_remove(&vc, id), CAD_VC_OK);
    STATUS(cad_vc_remove(&vc, id), CAD_VC_NOT_FOUND);

    STATUS(cad_vc_add(&vc, &model, CAD_VC_FIX, v[1], 0, 0, 0L, &moves, &f),
           CAD_VC_OK);
    CHECK(moves.count == 0 && f == 5UL);
    describe(f, "Fix V1");
    STATUS(cad_vc_get(&vc, f, &c), CAD_VC_OK);
    CHECK(c.fix[0] == 0L && c.fix[1] == 0L && c.fix[2] == 0L);

    /* Fixed vertex and its group reject drags. */
    FAIL(cad_vc_drag(&vc, &model, v[1], 1, 0, 0, &moves), CAD_VC_CONFLICT);
    FAIL(cad_vc_drag(&vc, &model, v[2], 0, 0, 5, &moves), CAD_VC_CONFLICT);
    FAIL(cad_vc_drag(&vc, &model, v[5], 60, 2, 3, &moves), CAD_VC_CONFLICT);
    STATUS(cad_vc_drag(&vc, &model, v[1], 0, 0, 0, &moves), CAD_VC_OK);
    CHECK(moves.count == 1 && moves.items[0].id == v[1]);
    /* v5 is only X-constrained, Y/Z stay free. */
    STATUS(cad_vc_drag(&vc, &model, v[5], 50, 80, -90, &moves), CAD_VC_OK);
    CHECK(moves.count == 1);
    apply();
    at(v[5], 50, 80, -90);

    /* Distance edit with b free moves b; out of range rejected. */
    STATUS(cad_vc_set_distance(&vc, &model, d, -25L, &moves), CAD_VC_OK);
    CHECK(moves.count == 1 && moves.items[0].id == v[5]);
    apply();
    at(v[5], -25, 80, -90);
    describe(d, "Distance X V1-V5 = -25");
    FAIL(cad_vc_set_distance(&vc, &model, d, 801L, &moves), CAD_VC_RANGE);
    FAIL(cad_vc_set_distance(&vc, &model, d, 500L, &moves), CAD_VC_RANGE);
    FAIL(cad_vc_set_distance(&vc, &model, f, 5L, &moves), CAD_VC_INVALID);
    FAIL(cad_vc_set_distance(&vc, &model, 999UL, 5L, &moves), CAD_VC_NOT_FOUND);

    /* Distance edit with b fixed moves a instead. */
    STATUS(cad_vc_remove(&vc, f), CAD_VC_OK);
    STATUS(cad_vc_add(&vc, &model, CAD_VC_FIX, v[5], 0, 0, 0L, &moves, &f),
           CAD_VC_OK);
    STATUS(cad_vc_set_distance(&vc, &model, d, 40L, &moves), CAD_VC_OK);
    CHECK(moves.count == 2);   /* v1 and its coincident partner v2. */
    apply();
    at(v[1], -65, 0, 0);
    at(v[2], -65, 0, 0);

    /* Argument validation. */
    FAIL(cad_vc_add(&vc, &model, CAD_VC_COINCIDENT, v[1], v[1], 0, 0L,
                    &moves, NULL), CAD_VC_INVALID);
    FAIL(cad_vc_add(&vc, &model, CAD_VC_ALIGN, v[1], v[3], 3, 0L,
                    &moves, NULL), CAD_VC_INVALID);
    FAIL(cad_vc_add(&vc, &model, 9, v[1], v[3], 0, 0L, &moves, NULL),
         CAD_VC_INVALID);
    FAIL(cad_vc_add(&vc, &model, CAD_VC_ALIGN, v[1], 999, 0, 0L,
                    &moves, NULL), CAD_VC_NOT_FOUND);
    FAIL(cad_vc_add(&vc, &model, CAD_VC_FIX, 999, 0, 0, 0L, &moves, NULL),
         CAD_VC_NOT_FOUND);
    FAIL(cad_vc_add(&vc, &model, CAD_VC_DISTANCE, v[1], v[3], 0, -801L,
                    &moves, NULL), CAD_VC_RANGE);
    FAIL(cad_vc_drag(&vc, &model, v[3], 401, 0, 0, &moves), CAD_VC_RANGE);
    FAIL(cad_vc_drag(&vc, &model, 999, 0, 0, 0, &moves), CAD_VC_NOT_FOUND);
    STATUS(cad_vc_get(&vc, d, &c), CAD_VC_OK);
    STATUS(cad_vc_describe(&c, small, sizeof(small)), CAD_VC_BUFFER);
    CHECK(cad_vc_status_string(CAD_VC_CONFLICT)[0] != '\0');
}

static void test_chain_and_conflict(void)
{
    CadVcId id;
    CadVcAnalysis a;
    reset();
    v[1] = vertex(0, 0, 0);
    v[2] = vertex(1, 1, 1);
    v[3] = vertex(30, 40, 50);
    v[4] = vertex(-30, -40, -50);
    v[5] = vertex(100, 100, 100);
    /* a=b coincident, c.y=b.y, c.x=b.x+20, d.y=c.y+5. */
    STATUS(cad_vc_add(&vc, &model, CAD_VC_COINCIDENT, v[1], v[2], 0, 0L,
                      &moves, NULL), CAD_VC_OK);
    apply();
    STATUS(cad_vc_add(&vc, &model, CAD_VC_ALIGN, v[2], v[3], 1, 0L,
                      &moves, NULL), CAD_VC_OK);
    apply();
    STATUS(cad_vc_add(&vc, &model, CAD_VC_DISTANCE, v[2], v[3], 0, 20L,
                      &moves, NULL), CAD_VC_OK);
    apply();
    STATUS(cad_vc_add(&vc, &model, CAD_VC_DISTANCE, v[3], v[4], 1, 5L,
                      &moves, NULL), CAD_VC_OK);
    apply();
    at(v[3], 20, 0, 50);
    at(v[4], -30, 5, -50);

    STATUS(cad_vc_analyze(&vc, &model, &a), CAD_VC_OK);
    CHECK(a.records == 4 && a.healthy == 4 && a.sick == 0);
    CHECK(a.vertices == 4 && a.equations == 6 && a.independent == 6);
    CHECK(a.redundant == 0 && a.free_dof == 6);

    /* Drag propagates through the whole chain. */
    STATUS(cad_vc_drag(&vc, &model, v[1], 10, 10, 10, &moves), CAD_VC_OK);
    CHECK(moves.count == 4 && moves.items[0].id == v[1]);
    apply();
    at(v[1], 10, 10, 10);
    at(v[2], 10, 10, 10);
    at(v[3], 30, 10, 50);
    at(v[4], -30, 15, -50);
    /* Unconstrained vertex passes through. */
    STATUS(cad_vc_drag(&vc, &model, v[5], -1, -2, -3, &moves), CAD_VC_OK);
    CHECK(moves.count == 1 && moves.items[0].id == v[5] &&
          moves.items[0].x == -1L && moves.items[0].z == -3L);
    /* Drag whose propagation leaves the domain. */
    FAIL(cad_vc_drag(&vc, &model, v[1], 0, 398, 0, &moves), CAD_VC_RANGE);

    /* Conflicts are atomic: same group, inconsistent offset. */
    FAIL(cad_vc_add(&vc, &model, CAD_VC_DISTANCE, v[1], v[2], 0, 10L,
                    &moves, NULL), CAD_VC_CONFLICT);
    FAIL(cad_vc_add(&vc, &model, CAD_VC_DISTANCE, v[1], v[4], 1, 0L,
                    &moves, NULL), CAD_VC_CONFLICT);
    /* Redundant but consistent is accepted without moves. */
    STATUS(cad_vc_add(&vc, &model, CAD_VC_DISTANCE, v[1], v[4], 1, 5L,
                      &moves, &id), CAD_VC_OK);
    CHECK(moves.count == 0);
    STATUS(cad_vc_analyze(&vc, &model, &a), CAD_VC_OK);
    CHECK(a.redundant == 1 && a.free_dof == 6);
    STATUS(cad_vc_remove(&vc, id), CAD_VC_OK);

    /* Two fixed vertices cannot be made coincident. */
    STATUS(cad_vc_add(&vc, &model, CAD_VC_FIX, v[1], 0, 0, 0L, &moves, NULL),
           CAD_VC_OK);
    STATUS(cad_vc_add(&vc, &model, CAD_VC_FIX, v[5], 0, 0, 0L, &moves, NULL),
           CAD_VC_OK);
    /* X would solve (v4.x is free) but Y cannot: no partial result leaks. */
    FAIL(cad_vc_add(&vc, &model, CAD_VC_COINCIDENT, v[4], v[5], 0, 0L,
                    &moves, NULL), CAD_VC_CONFLICT);
    /* v3 is fully tied to fixed v1: neither side can move. */
    FAIL(cad_vc_add(&vc, &model, CAD_VC_COINCIDENT, v[5], v[3], 0, 0L,
                    &moves, NULL), CAD_VC_CONFLICT);
    /* b fixed -> a's group moves instead. */
    v[6] = vertex(7, 8, 9);
    STATUS(cad_vc_add(&vc, &model, CAD_VC_COINCIDENT, v[6], v[5], 0, 0L,
                      &moves, NULL), CAD_VC_OK);
    CHECK(moves.count == 1 && moves.items[0].id == v[6]);
    apply();
    at(v[6], 100, 100, 100);
}

static void test_sync(void)
{
    CadVcId c1, c2, c3;
    CadVcAnalysis an;
    int removed, sick;
    char buf[CAD_VC_DESCRIBE_MAX];
    CadVConstraint c;
    reset();
    v[1] = vertex(0, 0, 0);
    v[2] = vertex(0, 0, 0);
    v[3] = vertex(10, 0, 0);
    v[4] = vertex(0, 5, 0);
    STATUS(cad_vc_add(&vc, &model, CAD_VC_COINCIDENT, v[1], v[2], 0, 0L,
                      &moves, &c1), CAD_VC_OK);
    STATUS(cad_vc_add(&vc, &model, CAD_VC_DISTANCE, v[1], v[3], 0, 10L,
                      &moves, &c2), CAD_VC_OK);
    STATUS(cad_vc_add(&vc, &model, CAD_VC_ALIGN, v[1], v[4], 0, 0L,
                      &moves, &c3), CAD_VC_OK);
    CHECK(moves.count == 0);

    /* Geometry edited outside the solver: solving refuses until sync. */
    STATUS(cad_model_begin(&model), CAD_OK);
    STATUS(cad_model_move_vertex(&model, v[3], 30, 0, 0), CAD_OK);
    STATUS(cad_model_commit(&model), CAD_OK);
    FAIL(cad_vc_drag(&vc, &model, v[1], 1, 1, 1, &moves), CAD_VC_STALE);
    STATUS(cad_vc_sync(&vc, &model, &removed, &sick), CAD_VC_OK);
    CHECK(removed == 0 && sick == 1);
    CHECK(health(c2) == CAD_VC_SICK && health(c1) == CAD_VC_HEALTHY);
    describe(c2, "Distance X V1-V3 = 10 (Sick)");
    /* Sick constraint does not drive geometry. */
    STATUS(cad_vc_drag(&vc, &model, v[1], 1, 1, 1, &moves), CAD_VC_OK);
    CHECK(moves.count == 3);   /* v1, v2, v4 (x) */
    STATUS(cad_vc_drag(&vc, &model, v[3], 1, 1, 1, &moves), CAD_VC_OK);
    CHECK(moves.count == 1);
    /* Undo heals it. */
    STATUS(cad_model_undo(&model), CAD_OK);
    STATUS(cad_vc_sync(&vc, &model, &removed, &sick), CAD_VC_OK);
    CHECK(removed == 0 && sick == 0 && health(c2) == CAD_VC_HEALTHY);
    /* Redo makes it sick again; editing the dimension re-solves it. */
    STATUS(cad_model_redo(&model), CAD_OK);
    STATUS(cad_vc_sync(&vc, &model, NULL, NULL), CAD_VC_OK);
    CHECK(health(c2) == CAD_VC_SICK);
    STATUS(cad_vc_set_distance(&vc, &model, c2, 30L, &moves), CAD_VC_OK);
    CHECK(moves.count == 0 && health(c2) == CAD_VC_HEALTHY);

    /* Vertex deletion removes dependent constraints. */
    STATUS(cad_model_begin(&model), CAD_OK);
    STATUS(cad_model_delete_vertex(&model, v[2]), CAD_OK);
    STATUS(cad_model_commit(&model), CAD_OK);
    FAIL(cad_vc_analyze(&vc, &model, &an), CAD_VC_STALE);
    STATUS(cad_vc_sync(&vc, &model, &removed, &sick), CAD_VC_OK);
    CHECK(removed == 1 && sick == 0 && vc.count == 2);
    STATUS(cad_vc_get(&vc, c1, &c), CAD_VC_NOT_FOUND);
    STATUS(cad_vc_at(&vc, 0, &c), CAD_VC_OK);
    CHECK(c.id == c2);
    STATUS(cad_vc_at(&vc, 1, &c), CAD_VC_OK);
    CHECK(c.id == c3);
    STATUS(cad_vc_at(&vc, 2, &c), CAD_VC_NOT_FOUND);
    STATUS(cad_vc_describe(&c, buf, sizeof(buf)), CAD_VC_OK);
    CHECK(strcmp(buf, "Align X V1-V4") == 0);
}

static void test_limit(void)
{
    int i;
    CadVcId id;
    CadVcAnalysis a;
    reset();
    for (i = 0; i < 41; ++i) v[i] = vertex((long)i * 5L, (long)i, 0);
    for (i = 0; i + 1 < 40; ++i) {
        STATUS(cad_vc_add(&vc, &model, CAD_VC_ALIGN, v[i], v[i + 1], 0, 0L,
                          &moves, NULL), CAD_VC_OK);
        apply();
    }
    for (i = 0; i < 40; ++i) at(v[i], 0, (long)i, 0);
    FAIL(cad_vc_add(&vc, &model, CAD_VC_ALIGN, v[39], v[40], 0, 0L,
                    &moves, NULL), CAD_VC_CAPACITY);
    STATUS(cad_vc_analyze(&vc, &model, &a), CAD_VC_OK);
    CHECK(a.vertices == 40 && a.independent == 39 && a.free_dof == 81);
    /* A 40-vertex drag fits one 64-delta transaction. */
    STATUS(cad_vc_drag(&vc, &model, v[0], 100, 0, 0, &moves), CAD_VC_OK);
    CHECK(moves.count == 40);
    apply();
    for (i = 0; i < 40; ++i) at(v[i], 100, (long)i, 0);
    /* Staging is atomic when the transaction lacks room. */
    STATUS(cad_vc_drag(&vc, &model, v[0], 90, 0, 0, &moves), CAD_VC_OK);
    STATUS(cad_model_begin(&model), CAD_OK);
    for (i = 0; i < 30; ++i)
        STATUS(cad_model_move_vertex(&model, v[40], (long)i, 0, 0), CAD_OK);
    STATUS(cad_vc_stage_moves(&model, &moves), CAD_VC_CAPACITY);
    CHECK(model.staged_count == 30);
    STATUS(cad_model_cancel(&model), CAD_OK);
    STATUS(cad_vc_stage_moves(&model, &moves), CAD_VC_INVALID);

    /* A loaded 41st vertex that the geometry satisfies is still SICK. */
    STATUS(cad_model_begin(&model), CAD_OK);
    STATUS(cad_model_move_vertex(&model, v[40], 100, 0, 0), CAD_OK);
    STATUS(cad_model_commit(&model), CAD_OK);
    {
        unsigned len, line;
        int removed, sick;
        char extra[40];
        STATUS(cad_vc_write_text(&vc, text, sizeof(text), &len), CAD_VC_OK);
        sprintf(extra, "C %lu ALIGN %u %u X\n", vc.next_id,
                (unsigned)v[39], (unsigned)v[40]);
        sprintf(text, "N %lu\n", vc.next_id + 1UL);
        STATUS(cad_vc_write_text(&vc, text2, sizeof(text2), &len), CAD_VC_OK);
        strcat(text, strchr(text2, '\n') + 1);
        strcat(text, extra);
        STATUS(cad_vc_read_text(&vc, text, (unsigned)strlen(text), &line),
               CAD_VC_OK);
        STATUS(cad_vc_sync(&vc, &model, &removed, &sick), CAD_VC_OK);
        CHECK(removed == 0 && sick == 1 && vc.count == 40);
        CHECK(vc.items[39].health == CAD_VC_SICK);
        STATUS(cad_vc_analyze(&vc, &model, &a), CAD_VC_OK);
        CHECK(a.vertices == 40 && a.sick == 1);
    }

    /* Record capacity: 64 records. */
    reset();
    v[0] = vertex(0, 0, 0);
    v[1] = vertex(0, 0, 0);
    for (i = 0; i < CAD_VC_MAX_RECORDS; ++i)
        STATUS(cad_vc_add(&vc, &model, CAD_VC_ALIGN, v[0], v[1], i % 3, 0L,
                          &moves, &id), CAD_VC_OK);
    FAIL(cad_vc_add(&vc, &model, CAD_VC_ALIGN, v[0], v[1], 0, 0L,
                    &moves, NULL), CAD_VC_CAPACITY);
}

static void roundtrip(void)
{
    unsigned len, len2, line;
    STATUS(cad_vc_write_text(&vc, text, sizeof(text), &len), CAD_VC_OK);
    CHECK(strlen(text) == len && len <= CAD_VC_TEXT_MAX);
    memset(&vc2, 0x5a, sizeof(vc2));
    STATUS(cad_vc_read_text(&vc2, text, len, &line), CAD_VC_OK);
    STATUS(cad_vc_write_text(&vc2, text2, sizeof(text2), &len2), CAD_VC_OK);
    CHECK(len == len2 && strcmp(text, text2) == 0);
    CHECK(vc2.count == vc.count && vc2.next_id == vc.next_id);
}

static void bad(const char *s, CadVcStatus expected, unsigned want_line)
{
    unsigned line;
    memcpy(&vc_before, &vc, sizeof(vc));
    line = 77U;
    STATUS(cad_vc_read_text(&vc, s, (unsigned)strlen(s), &line), expected);
    CHECK(memcmp(&vc_before, &vc, sizeof(vc)) == 0);
    if (line != want_line) {
        fprintf(stderr, "bad '%s': line %u want %u\n", s, line, want_line);
        exit(1);
    }
}

static void test_text(void)
{
    static const char sample[] =
        "N 9\n"
        "C 1 COINCIDENT 3 7\n"
        "C 2 ALIGN 1 2 Y\n"
        "C 4 DISTANCE 1 2 X -50\n"
        "C 8 FIX 4 -400 0 400\n";
    unsigned len, line;
    char tiny[10];
    int removed, sick;
    reset();
    v[1] = vertex(0, 0, 0);
    v[2] = vertex(5, 6, 7);
    v[3] = vertex(-1, 2, -3);
    STATUS(cad_vc_add(&vc, &model, CAD_VC_COINCIDENT, v[1], v[2], 0, 0L,
                      &moves, NULL), CAD_VC_OK);
    apply();
    STATUS(cad_vc_add(&vc, &model, CAD_VC_DISTANCE, v[1], v[3], 2, -300L,
                      &moves, NULL), CAD_VC_OK);
    apply();
    STATUS(cad_vc_add(&vc, &model, CAD_VC_ALIGN, v[2], v[3], 1, 0L,
                      &moves, NULL), CAD_VC_OK);
    apply();
    STATUS(cad_vc_add(&vc, &model, CAD_VC_FIX, v[3], 0, 0, 0L, &moves, NULL),
           CAD_VC_OK);
    roundtrip();
    CHECK(strcmp(text, "N 5\nC 1 COINCIDENT 1 2\nC 2 DISTANCE 1 3 Z -300\n"
                       "C 3 ALIGN 2 3 Y\nC 4 FIX 3 -1 0 -300\n") == 0);
    CHECK(memcmp(vc2.items, vc.items, sizeof(vc.items)) == 0);
    /* Loaded state syncs cleanly against the same model. */
    STATUS(cad_vc_sync(&vc2, &model, &removed, &sick), CAD_VC_OK);
    CHECK(removed == 0 && sick == 0);

    STATUS(cad_vc_read_text(&vc, sample, (unsigned)strlen(sample), &line),
           CAD_VC_OK);
    CHECK(vc.count == 4 && vc.next_id == 9UL);
    describe(4UL, "Distance X V1-V2 = -50");
    describe(1UL, "Coincident V3-V7");
    roundtrip();
    CHECK(strcmp(text, sample) == 0);

    /* Empty text and exhausted ID space. */
    STATUS(cad_vc_read_text(&vc2, "", 0U, &line), CAD_VC_OK);
    CHECK(vc2.count == 0 && vc2.next_id == 1UL);
    {
        const char *full = "N 0\nC 2147483647 FIX 1 0 0 0\n";
        STATUS(cad_vc_read_text(&vc2, full, (unsigned)strlen(full), &line),
               CAD_VC_OK);
        CHECK(vc2.next_id == 0UL && vc2.count == 1);
        STATUS(cad_vc_add(&vc2, &model, CAD_VC_FIX, v[1], 0, 0, 0L, &moves,
                          NULL), CAD_VC_CAPACITY);
    }
    STATUS(cad_vc_init(&vc2), CAD_VC_OK);

    FAIL(cad_vc_write_text(&vc, tiny, sizeof(tiny), &len), CAD_VC_BUFFER);

    bad("C 1 COINCIDENT 1 2\n", CAD_VC_PARSE, 1U);
    bad("N 5\nN 5\n", CAD_VC_PARSE, 2U);
    bad("N 5\nC 1 COINCIDENT 1 2", CAD_VC_PARSE, 2U);
    bad("N 5\nC 1 COINCIDENT 1 2\r\n", CAD_VC_PARSE, 2U);
    bad("N 5\nC 1  COINCIDENT 1 2\n", CAD_VC_PARSE, 2U);
    bad("N 5\nC 1 COINCIDENT 1 2 \n", CAD_VC_PARSE, 2U);
    bad("N 5\n C 1 COINCIDENT 1 2\n", CAD_VC_PARSE, 2U);
    bad("N 5\nC 1\tCOINCIDENT 1 2\n", CAD_VC_PARSE, 2U);
    bad("N 5\nC 1 COINCIDENT 1 1\n", CAD_VC_PARSE, 2U);
    bad("N 5\nC 1 COINCIDENT 0 1\n", CAD_VC_PARSE, 2U);
    bad("N 5\nC 1 COINCIDENT 1 65536\n", CAD_VC_PARSE, 2U);
    bad("N 5\nC 1 COINCIDENT 1 2 X\n", CAD_VC_PARSE, 2U);
    bad("N 5\nC 1 PERPENDICULAR 1 2\n", CAD_VC_PARSE, 2U);
    bad("N 5\nC 1 ALIGN 1 2 W\n", CAD_VC_PARSE, 2U);
    bad("N 5\nC 1 ALIGN 1 2 x\n", CAD_VC_PARSE, 2U);
    bad("N 5\nC 1 DISTANCE 1 2 X 801\n", CAD_VC_PARSE, 2U);
    bad("N 5\nC 1 DISTANCE 1 2 X -0\n", CAD_VC_PARSE, 2U);
    bad("N 5\nC 1 DISTANCE 1 2 X 050\n", CAD_VC_PARSE, 2U);
    bad("N 5\nC 1 DISTANCE 1 2 X +5\n", CAD_VC_PARSE, 2U);
    bad("N 5\nC 1 DISTANCE 1 2 X\n", CAD_VC_PARSE, 2U);
    bad("N 5\nC 1 FIX 1 0 0 401\n", CAD_VC_PARSE, 2U);
    bad("N 5\nC 1 FIX 1 0 0\n", CAD_VC_PARSE, 2U);
    bad("N 5\nC 0 FIX 1 0 0 0\n", CAD_VC_PARSE, 2U);
    bad("N 5\nC 01 FIX 1 0 0 0\n", CAD_VC_PARSE, 2U);
    bad("N 5\nC 2 FIX 1 0 0 0\nC 2 FIX 1 0 0 0\n", CAD_VC_PARSE, 3U);
    bad("N 5\nC 3 FIX 1 0 0 0\nC 2 FIX 1 0 0 0\n", CAD_VC_PARSE, 3U);
    bad("N 5\nC 5 FIX 1 0 0 0\n", CAD_VC_PARSE, 2U);
    bad("N 2147483648\n", CAD_VC_PARSE, 1U);
    bad("N 5\n\n", CAD_VC_PARSE, 2U);
    bad("N 5\nX 1\n", CAD_VC_PARSE, 2U);
    bad("N 5\nC 1 FIX 1 0 0 0 0 0 0\n", CAD_VC_PARSE, 2U);
    STATUS(cad_vc_read_text(&vc, NULL, 3U, &line), CAD_VC_INVALID);
}

static void test_text_capacity(void)
{
    char big[CAD_VC_TEXT_MAX * 2];
    unsigned len, i;
    CadVcId id;
    reset();
    v[1] = vertex(-400, -400, -400);
    v[2] = vertex(-400, -400, -400);
    for (i = 0; i < CAD_VC_MAX_RECORDS; ++i)
        STATUS(cad_vc_add(&vc, &model, i % 2 ? CAD_VC_FIX : CAD_VC_DISTANCE,
                          v[1], i % 2 ? 0 : v[2], 0, 0L, &moves, &id),
               CAD_VC_OK);
    roundtrip();
    CHECK(vc2.count == CAD_VC_MAX_RECORDS);
    /* 65th record rejected with its line number. */
    {
        const char *extra = "C 99 FIX 1 0 0 0\n";
        const char *body;
        unsigned line;
        body = strchr(text, '\n') + 1;
        strcpy(big, "N 100\n");
        strcat(big, body);
        strcat(big, extra);
        len = (unsigned)strlen(big);
        memcpy(&vc_before, &vc, sizeof(vc));
        STATUS(cad_vc_read_text(&vc, big, len, &line), CAD_VC_CAPACITY);
        CHECK(line == CAD_VC_MAX_RECORDS + 2U);
        CHECK(memcmp(&vc_before, &vc, sizeof(vc)) == 0);
        big[len - strlen(extra)] = '\0';
        STATUS(cad_vc_read_text(&vc2, big, (unsigned)strlen(big), &line),
               CAD_VC_OK);
        CHECK(vc2.next_id == 100UL && vc2.count == CAD_VC_MAX_RECORDS);
    }
}

int main(void)
{
    test_kinds();
    test_chain_and_conflict();
    test_sync();
    test_limit();
    test_text();
    test_text_capacity();
    printf("vconstraints_test: PASS\n");
    return 0;
}
