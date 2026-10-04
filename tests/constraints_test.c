/* Host (run from repository root):
 * cc -std=c89 -pedantic -Wall -Wextra -Werror cad_constraints.c \
 *    tests/constraints_test.c -o /tmp/opencode/constraints_test
 * /tmp/opencode/constraints_test
 * No Windows stubs, implementation inclusion, allocation, or test hooks.
 */
#include "../cad_constraints.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define STATUS(call, expected) CHECK((call) == (expected))
#define FAIL(call, expected) do { \
    memcpy(&before, &model, sizeof(model)); \
    STATUS(call, expected); \
    CHECK(memcmp(&before, &model, sizeof(model)) == 0); \
} while (0)

static CadConstraints model, before;
static int initial[CAD_CONSTRAINT_MAX_VARIABLES + 1];
static int candidate[CAD_CONSTRAINT_MAX_VARIABLES];
static CadConstraintId ids[CAD_CONSTRAINT_MAX_CONSTRAINTS];

static void reset(int n)
{
    memset(initial, 0, sizeof(initial));
    STATUS(CadConstraintsInit(&model, initial, n), CAD_CONSTRAINT_OK);
}

static void analysis(int independent, int redundant, int free_dof)
{
    CadConstraintAnalysis a;
    int i;
    long left;
    const CadConstraint *e;
    memcpy(&before, &model, sizeof(model));
    STATUS(CadConstraintsAnalyze(&model, &a), CAD_CONSTRAINT_OK);
    CHECK(memcmp(&before, &model, sizeof(model)) == 0);
    CHECK(a.variable_count == model.variable_count);
    CHECK(a.constraint_count == model.constraint_count);
    CHECK(a.independent == independent);
    CHECK(a.redundant == redundant);
    CHECK(a.free_root_dof == free_dof);
    CHECK(a.independent + a.redundant == model.constraint_count);
    CHECK(a.independent + a.free_root_dof == model.variable_count);
    for (i = 0; i < model.variable_count; ++i)
        CHECK(model.values[i] >= -400 && model.values[i] <= 400);
    for (i = 0; i < model.constraint_count; ++i) {
        e = &model.constraints[i];
        left = e->a == CAD_CONSTRAINT_FIXED ? 0L : (long)model.values[e->a];
        CHECK((long)model.values[e->b] - left == e->offset);
        CHECK(e->id != 0UL);
    }
}

static void range(int variable, long minimum, long maximum)
{
    long low, high;
    low = 12345L;
    high = 67890L;
    memcpy(&before, &model, sizeof(model));
    STATUS(CadConstraintsRange(&model, variable, &low, &high), CAD_CONSTRAINT_OK);
    CHECK(low == minimum && high == maximum);
    CHECK(memcmp(&before, &model, sizeof(model)) == 0);
}

static void test_xyz(void)
{
    CadConstraintId id;
    int axis, i;
    reset(12); /* Four XYZ vertices; coincidence is three scalar equations. */
    for (axis = 0; axis < 3; ++axis)
        STATUS(CadConstraintsAddOffset(&model, axis, 3 + axis, 0L, &id),
               CAD_CONSTRAINT_OK);
    analysis(3, 0, 9);
    STATUS(CadConstraintsDrag(&model, 3, 80L), CAD_CONSTRAINT_OK);
    STATUS(CadConstraintsDrag(&model, 4, -25L), CAD_CONSTRAINT_OK);
    STATUS(CadConstraintsDrag(&model, 5, 7L), CAD_CONSTRAINT_OK);
    CHECK(model.values[0] == 80 && model.values[1] == -25 && model.values[2] == 7);
    for (i = 0; i < 3; ++i) CHECK(model.values[i] == model.values[3 + i]);
    for (i = 6; i < 12; ++i) CHECK(model.values[i] == 0);
    /* Axis dimensions are signed offsets, not Euclidean distances. */
    STATUS(CadConstraintsAddOffset(&model, 3, 6, 100L, NULL), CAD_CONSTRAINT_OK);
    STATUS(CadConstraintsAddOffset(&model, 4, 7, -50L, NULL), CAD_CONSTRAINT_OK);
    STATUS(CadConstraintsAddOffset(&model, 5, 8, 20L, NULL), CAD_CONSTRAINT_OK);
    CHECK(model.values[6] == 180 && model.values[7] == -75 && model.values[8] == 27);
    STATUS(CadConstraintsDrag(&model, 6, 200L), CAD_CONSTRAINT_OK);
    CHECK(model.values[0] == 100 && model.values[3] == 100);
    CHECK(model.values[7] == -75 && model.values[8] == 27);
    analysis(6, 0, 6);
}

static void test_anchors(void)
{
    CadConstraintId link, anchor, redundant, other, out;
    reset(4);
    STATUS(CadConstraintsAddOffset(&model, 0, 1, -25L, &link), CAD_CONSTRAINT_OK);
    STATUS(CadConstraintsAddAnchor(&model, 1, 100L, &anchor), CAD_CONSTRAINT_OK);
    CHECK(model.values[0] == 125 && model.values[1] == 100);
    range(0, 125L, 125L);
    range(1, 100L, 100L);
    analysis(2, 0, 2);
    FAIL(CadConstraintsDrag(&model, 0, 124L), CAD_CONSTRAINT_CONFLICT);
    memcpy(&before, &model, sizeof(model));
    STATUS(CadConstraintsDrag(&model, 1, 100L), CAD_CONSTRAINT_OK);
    CHECK(memcmp(&before, &model, sizeof(model)) == 0);
    STATUS(CadConstraintsAddAnchor(&model, 0, 125L, &redundant),
           CAD_CONSTRAINT_REDUNDANT);
    out = 999UL;
    FAIL(CadConstraintsAddAnchor(&model, 0, 126L, &out), CAD_CONSTRAINT_CONFLICT);
    CHECK(out == 999UL);
    FAIL(CadConstraintsAddOffset(&model, 1, 0, 24L, &out), CAD_CONSTRAINT_CONFLICT);
    CHECK(out == 999UL);
    STATUS(CadConstraintsAddAnchor(&model, 2, -200L, &other), CAD_CONSTRAINT_OK);
    /* Separate anchored groups share the zero node; both remain exact. */
    STATUS(CadConstraintsDrag(&model, 2, -200L), CAD_CONSTRAINT_OK);
    CHECK(model.values[0] == 125 && model.values[1] == 100);
    FAIL(CadConstraintsAddOffset(&model, 0, 2, -324L, NULL), CAD_CONSTRAINT_CONFLICT);
    STATUS(CadConstraintsAddOffset(&model, 0, 2, -325L, &out),
           CAD_CONSTRAINT_REDUNDANT);
    analysis(3, 2, 1);
    STATUS(CadConstraintsRemove(&model, anchor), CAD_CONSTRAINT_OK);
    FAIL(CadConstraintsDrag(&model, 0, 126L), CAD_CONSTRAINT_CONFLICT);
    STATUS(CadConstraintsRemove(&model, redundant), CAD_CONSTRAINT_OK);
    /* Redundant cross-link and the other anchor still fix this group. */
    FAIL(CadConstraintsDrag(&model, 0, 126L), CAD_CONSTRAINT_CONFLICT);
    STATUS(CadConstraintsRemove(&model, out), CAD_CONSTRAINT_OK);
    analysis(2, 0, 2);
    STATUS(CadConstraintsDrag(&model, 1, 101L), CAD_CONSTRAINT_OK);
    CHECK(model.values[0] == 126 && model.values[2] == -200);
    STATUS(CadConstraintsRemove(&model, link), CAD_CONSTRAINT_OK);
    STATUS(CadConstraintsRemove(&model, other), CAD_CONSTRAINT_OK);
    analysis(0, 0, 4);
}

static void test_ids_and_removal(void)
{
    CadConstraint e;
    CadConstraintId a, b, c, d;
    reset(3);
    STATUS(CadConstraintsAddOffset(&model, 0, 1, 10L, &a), CAD_CONSTRAINT_OK);
    STATUS(CadConstraintsAddOffset(&model, 1, 2, 20L, &b), CAD_CONSTRAINT_OK);
    STATUS(CadConstraintsAddOffset(&model, 0, 2, 30L, &c), CAD_CONSTRAINT_REDUNDANT);
    CHECK(a == 1UL && b == 2UL && c == 3UL);
    analysis(2, 1, 1);
    STATUS(CadConstraintsRemove(&model, a), CAD_CONSTRAINT_OK);
    CHECK(model.constraints[0].id == b && model.constraints[1].id == c);
    analysis(2, 0, 1); /* The former redundant edge is now independent. */
    STATUS(CadConstraintsGet(&model, c, &e), CAD_CONSTRAINT_OK);
    CHECK(e.a == 0 && e.b == 2 && e.offset == 30L && e.id == c);
    FAIL(CadConstraintsRemove(&model, a), CAD_CONSTRAINT_NOT_FOUND);
    FAIL(CadConstraintsRemove(&model, 0UL), CAD_CONSTRAINT_NOT_FOUND);
    FAIL(CadConstraintsRemove(&model, 9999UL), CAD_CONSTRAINT_NOT_FOUND);
    e.id = 123UL;
    STATUS(CadConstraintsGet(&model, a, &e), CAD_CONSTRAINT_NOT_FOUND);
    CHECK(e.id == 123UL);
    STATUS(CadConstraintsRemove(&model, b), CAD_CONSTRAINT_OK);
    analysis(1, 0, 2);
    STATUS(CadConstraintsDrag(&model, 1, -300L), CAD_CONSTRAINT_OK);
    CHECK(model.values[0] == 0 && model.values[2] == 30);
    STATUS(CadConstraintsAddOffset(&model, 2, 1, -10L, &d), CAD_CONSTRAINT_OK);
    CHECK(d == 4UL); /* Array position 1, explicit ID 4. */
    CHECK(model.values[1] == 20);
    STATUS(CadConstraintsRemove(&model, c), CAD_CONSTRAINT_OK);
    range(0, -400L, 400L);
    analysis(1, 0, 2);
}

static void test_range_and_checked32(void)
{
    CadConstraintId out;
    reset(3);
    STATUS(CadConstraintsDrag(&model, 0, 390L), CAD_CONSTRAINT_OK);
    STATUS(CadConstraintsAddOffset(&model, 0, 1, 100L, NULL), CAD_CONSTRAINT_OK);
    /* Clamp the root to 300, not each coordinate to 400. */
    CHECK(model.values[0] == 300 && model.values[1] == 400);
    STATUS(CadConstraintsAddOffset(&model, 0, 2, -100L, NULL), CAD_CONSTRAINT_OK);
    range(0, -300L, 300L);
    range(1, -200L, 400L);
    range(2, -400L, 200L);
    FAIL(CadConstraintsDrag(&model, 0, 301L), CAD_CONSTRAINT_RANGE);
    FAIL(CadConstraintsDrag(&model, 2, 201L), CAD_CONSTRAINT_RANGE);
    STATUS(CadConstraintsDrag(&model, 2, -400L), CAD_CONSTRAINT_OK);
    CHECK(model.values[0] == -300 && model.values[1] == -200);
    STATUS(CadConstraintsDrag(&model, 1, 400L), CAD_CONSTRAINT_OK);
    CHECK(model.values[0] == 300 && model.values[2] == 200);
    out = 77UL;
    FAIL(CadConstraintsAddAnchor(&model, 0, 301L, &out), CAD_CONSTRAINT_RANGE);
    CHECK(out == 77UL);
    FAIL(CadConstraintsDrag(&model, 0, 401L), CAD_CONSTRAINT_RANGE);
    FAIL(CadConstraintsDrag(&model, 0, -401L), CAD_CONSTRAINT_RANGE);
    reset(2);
    FAIL(CadConstraintsAddOffset(&model, 0, 1, 801L, &out), CAD_CONSTRAINT_RANGE);
    FAIL(CadConstraintsAddOffset(&model, 0, 1, -801L, &out), CAD_CONSTRAINT_RANGE);
    FAIL(CadConstraintsAddAnchor(&model, 0, 401L, &out), CAD_CONSTRAINT_RANGE);
    FAIL(CadConstraintsAddAnchor(&model, 0, -401L, &out), CAD_CONSTRAINT_RANGE);
    FAIL(CadConstraintsAddOffset(&model, 0, 1, 2147483647L, &out),
         CAD_CONSTRAINT_RANGE);
    FAIL(CadConstraintsAddOffset(&model, 0, 1, (-2147483647L - 1L), &out),
         CAD_CONSTRAINT_RANGE);
    FAIL(CadConstraintsAddAnchor(&model, 0, (-2147483647L - 1L), &out),
         CAD_CONSTRAINT_RANGE);
#if LONG_MAX > 2147483647L
    FAIL(CadConstraintsAddOffset(&model, 0, 1, 2147483647L + 1L, &out),
         CAD_CONSTRAINT_RANGE);
    FAIL(CadConstraintsDrag(&model, 0, -2147483647L - 2L), CAD_CONSTRAINT_RANGE);
#endif
    CHECK(out == 77UL);
    STATUS(CadConstraintsAddOffset(&model, 0, 1, 800L, NULL), CAD_CONSTRAINT_OK);
    CHECK(model.values[0] == -400 && model.values[1] == 400);
    range(0, -400L, -400L);
    range(1, 400L, 400L);
    analysis(1, 0, 1); /* Algebraically free; bounds leave one integer position. */
    FAIL(CadConstraintsDrag(&model, 0, -399L), CAD_CONSTRAINT_RANGE);
    reset(2);
    STATUS(CadConstraintsAddOffset(&model, 0, 1, -800L, NULL), CAD_CONSTRAINT_OK);
    CHECK(model.values[0] == 400 && model.values[1] == -400);
    reset(3);
    STATUS(CadConstraintsAddOffset(&model, 0, 1, 400L, NULL), CAD_CONSTRAINT_OK);
    FAIL(CadConstraintsAddOffset(&model, 1, 2, 401L, NULL), CAD_CONSTRAINT_RANGE);
    /* Anchored +/-400 boundaries and a consistent link across them. */
    reset(2);
    STATUS(CadConstraintsAddAnchor(&model, 0, -400L, NULL), CAD_CONSTRAINT_OK);
    STATUS(CadConstraintsAddAnchor(&model, 1, 400L, NULL), CAD_CONSTRAINT_OK);
    STATUS(CadConstraintsAddOffset(&model, 0, 1, 800L, NULL),
           CAD_CONSTRAINT_REDUNDANT);
    analysis(2, 1, 0);
}

static void test_edit(void)
{
    int i;
    reset(4);
    STATUS(CadConstraintsAddOffset(&model, 0, 1, 50L, NULL), CAD_CONSTRAINT_OK);
    STATUS(CadConstraintsAddAnchor(&model, 2, -10L, NULL), CAD_CONSTRAINT_OK);
    candidate[0] = 10; candidate[1] = 60;
    candidate[2] = -10; candidate[3] = -400;
    STATUS(CadConstraintsEdit(&model, candidate, 4), CAD_CONSTRAINT_OK);
    for (i = 0; i < 4; ++i) CHECK(model.values[i] == candidate[i]);
    memcpy(&before, &model, sizeof(model));
    STATUS(CadConstraintsEdit(&model, model.values, 4), CAD_CONSTRAINT_OK);
    CHECK(memcmp(&before, &model, sizeof(model)) == 0);
    candidate[0] = 20; candidate[1] = 71; candidate[3] = 400;
    FAIL(CadConstraintsEdit(&model, candidate, 4), CAD_CONSTRAINT_CONFLICT);
    candidate[1] = 70; candidate[2] = -9;
    FAIL(CadConstraintsEdit(&model, candidate, 4), CAD_CONSTRAINT_CONFLICT);
    candidate[2] = -10; candidate[3] = 401;
    FAIL(CadConstraintsEdit(&model, candidate, 4), CAD_CONSTRAINT_RANGE);
    candidate[3] = -401;
    FAIL(CadConstraintsEdit(&model, candidate, 4), CAD_CONSTRAINT_RANGE);
    candidate[3] = 400;
    FAIL(CadConstraintsEdit(&model, candidate, 3), CAD_CONSTRAINT_INVALID);
    FAIL(CadConstraintsEdit(&model, NULL, 4), CAD_CONSTRAINT_INVALID);
    STATUS(CadConstraintsEdit(&model, candidate, 4), CAD_CONSTRAINT_OK);
    analysis(2, 0, 2);
    /* Alias-safe initialization stages values before clearing storage. */
    STATUS(CadConstraintsInit(&model, model.values, 4), CAD_CONSTRAINT_OK);
    for (i = 0; i < 4; ++i) CHECK(model.values[i] == candidate[i]);
    analysis(0, 0, 4);
}

static void test_zero_invalid_and_noop(void)
{
    CadConstraintId out, self;
    CadConstraintAnalysis a, saved;
    long low, high;
    reset(0);
    analysis(0, 0, 0);
    STATUS(CadConstraintsEdit(&model, NULL, 0), CAD_CONSTRAINT_OK);
    STATUS(CadConstraintsInit(&model, NULL, 0), CAD_CONSTRAINT_OK);
    FAIL(CadConstraintsAddAnchor(&model, 0, 0L, NULL), CAD_CONSTRAINT_INVALID);
    FAIL(CadConstraintsDrag(&model, 0, 0L), CAD_CONSTRAINT_INVALID);
    reset(2);
    out = 456UL;
    FAIL(CadConstraintsAddOffset(&model, -1, 0, 0L, &out), CAD_CONSTRAINT_INVALID);
    FAIL(CadConstraintsAddOffset(&model, 0, -1, 0L, &out), CAD_CONSTRAINT_INVALID);
    FAIL(CadConstraintsAddOffset(&model, 2, 0, 0L, &out), CAD_CONSTRAINT_INVALID);
    FAIL(CadConstraintsAddOffset(&model, 0, 2, 0L, &out), CAD_CONSTRAINT_INVALID);
    CHECK(out == 456UL);
    STATUS(CadConstraintsAddOffset(&model, 0, 0, 0L, &self),
           CAD_CONSTRAINT_REDUNDANT);
    analysis(0, 1, 2);
    FAIL(CadConstraintsAddOffset(&model, 0, 0, 1L, &out), CAD_CONSTRAINT_CONFLICT);
    FAIL(CadConstraintsAddOffset(&model, 0, 0, -1L, &out), CAD_CONSTRAINT_CONFLICT);
    STATUS(CadConstraintsRemove(&model, self), CAD_CONSTRAINT_OK);
    memcpy(&before, &model, sizeof(model));
    STATUS(CadConstraintsDrag(&model, 0, 0L), CAD_CONSTRAINT_OK);
    CHECK(memcmp(&before, &model, sizeof(model)) == 0);
    FAIL(CadConstraintsInit(&model, NULL, 2), CAD_CONSTRAINT_INVALID);
    FAIL(CadConstraintsInit(&model, initial, -1), CAD_CONSTRAINT_INVALID);
    FAIL(CadConstraintsInit(&model, initial, 129), CAD_CONSTRAINT_CAPACITY);
    initial[1] = 401;
    FAIL(CadConstraintsInit(&model, initial, 2), CAD_CONSTRAINT_RANGE);
    low = 123L; high = 456L;
    STATUS(CadConstraintsRange(&model, 2, &low, &high), CAD_CONSTRAINT_INVALID);
    CHECK(low == 123L && high == 456L);
    STATUS(CadConstraintsRange(&model, 0, &low, &low), CAD_CONSTRAINT_INVALID);
    CHECK(low == 123L);
    STATUS(CadConstraintsRange(&model, 0, NULL, &high), CAD_CONSTRAINT_INVALID);
    STATUS(CadConstraintsGet(&model, 1UL, NULL), CAD_CONSTRAINT_INVALID);
    memset(&a, 0x5a, sizeof(a)); memcpy(&saved, &a, sizeof(a));
    STATUS(CadConstraintsAnalyze(NULL, &a), CAD_CONSTRAINT_INVALID);
    CHECK(memcmp(&a, &saved, sizeof(a)) == 0);
    STATUS(CadConstraintsAnalyze(&model, NULL), CAD_CONSTRAINT_INVALID);
    STATUS(CadConstraintsInit(NULL, initial, 2), CAD_CONSTRAINT_INVALID);
    STATUS(CadConstraintsAddOffset(NULL, 0, 1, 0L, NULL), CAD_CONSTRAINT_INVALID);
    STATUS(CadConstraintsAddAnchor(NULL, 0, 0L, NULL), CAD_CONSTRAINT_INVALID);
    STATUS(CadConstraintsRemove(NULL, 1UL), CAD_CONSTRAINT_INVALID);
    STATUS(CadConstraintsDrag(NULL, 0, 0L), CAD_CONSTRAINT_INVALID);
    STATUS(CadConstraintsEdit(NULL, initial, 2), CAD_CONSTRAINT_INVALID);
}

static void test_capacity_and_long_chain(void)
{
    CadConstraintId out;
    int i;
    long offset;
    reset(128);
    analysis(0, 0, 128);
    for (i = 0; i < 127; ++i) {
        offset = (i % 2) == 0 ? 6L : -6L;
        STATUS(CadConstraintsAddOffset(&model, i, i + 1, offset, &ids[i]),
               CAD_CONSTRAINT_OK);
    }
    analysis(127, 0, 1);
    range(0, -400L, 394L);
    range(127, -394L, 400L);
    STATUS(CadConstraintsDrag(&model, 127, 400L), CAD_CONSTRAINT_OK);
    for (i = 0; i < 128; ++i)
        CHECK(model.values[i] == ((i % 2) == 0 ? 394 : 400));
    FAIL(CadConstraintsDrag(&model, 0, 395L), CAD_CONSTRAINT_RANGE);
    STATUS(CadConstraintsAddAnchor(&model, 127, 400L, &ids[127]), CAD_CONSTRAINT_OK);
    analysis(128, 0, 0);
    out = 88UL;
    FAIL(CadConstraintsAddOffset(&model, 0, 0, 0L, &out), CAD_CONSTRAINT_CAPACITY);
    CHECK(out == 88UL);
    STATUS(CadConstraintsRemove(&model, ids[63]), CAD_CONSTRAINT_OK);
    analysis(127, 0, 1);
    STATUS(CadConstraintsDrag(&model, 0, -400L), CAD_CONSTRAINT_OK);
    CHECK(model.values[63] == -394 && model.values[64] == 394);
    STATUS(CadConstraintsAddOffset(&model, 0, 0, 0L, &out),
           CAD_CONSTRAINT_REDUNDANT);
    CHECK(out == 129UL);
    analysis(127, 1, 1);
    reset(1);
    for (i = 0; i < 128; ++i)
        STATUS(CadConstraintsAddOffset(&model, 0, 0, 0L, &ids[i]),
               CAD_CONSTRAINT_REDUNDANT);
    analysis(0, 128, 1);
    FAIL(CadConstraintsAddAnchor(&model, 0, 0L, NULL), CAD_CONSTRAINT_CAPACITY);
    STATUS(CadConstraintsRemove(&model, ids[0]), CAD_CONSTRAINT_OK);
    STATUS(CadConstraintsAddAnchor(&model, 0, 400L, &out), CAD_CONSTRAINT_OK);
    CHECK(out == 129UL);
    analysis(1, 127, 0);
    /* Synthetic valid near-exhaustion fixture avoids billions of API calls. */
    reset(1);
    model.next_id = CAD_CONSTRAINT_ID_MAX;
    STATUS(CadConstraintsAddAnchor(&model, 0, 0L, &out), CAD_CONSTRAINT_OK);
    CHECK(out == CAD_CONSTRAINT_ID_MAX && model.next_id == 0UL);
    STATUS(CadConstraintsRemove(&model, out), CAD_CONSTRAINT_OK);
    FAIL(CadConstraintsAddAnchor(&model, 0, 0L, NULL), CAD_CONSTRAINT_CAPACITY);
}

static void test_balanced_union_and_vertices(void)
{
    int i, step, count;
    long offset;
    reset(120); /* Maximum forty XYZ vertices. */
    /* Balanced merges exercise both rank directions and non-root endpoints.
     * Known solution value[i] = i - 60, using arbitrary endpoint orientation.
     */
    for (step = 1; step < 120; step *= 2) {
        for (i = 0; i + step < 120; i += 2 * step) {
            offset = (long)step;
            if ((i / (2 * step)) % 2 == 0)
                STATUS(CadConstraintsAddOffset(&model, i, i + step, offset, NULL),
                       CAD_CONSTRAINT_OK);
            else
                STATUS(CadConstraintsAddOffset(&model, i + step, i, -offset, NULL),
                       CAD_CONSTRAINT_OK);
        }
    }
    analysis(119, 0, 1);
    STATUS(CadConstraintsDrag(&model, 119, 59L), CAD_CONSTRAINT_OK);
    for (i = 0; i < 120; ++i) CHECK(model.values[i] == i - 60);
    range(0, -400L, 281L);
    range(119, -281L, 400L);
    for (i = 0; i < 8; ++i)
        STATUS(CadConstraintsAddOffset(&model, 119 - i, i,
                                      (long)(2 * i - 119), NULL),
               CAD_CONSTRAINT_REDUNDANT);
    count = model.constraint_count;
    CHECK(count == 127);
    FAIL(CadConstraintsAddOffset(&model, 119, 0, -118L, NULL),
         CAD_CONSTRAINT_CONFLICT);
    STATUS(CadConstraintsAddAnchor(&model, 57, -3L, NULL), CAD_CONSTRAINT_OK);
    analysis(120, 8, 0);
    FAIL(CadConstraintsDrag(&model, 0, -59L), CAD_CONSTRAINT_CONFLICT);
}

int main(void)
{
    test_xyz();
    test_anchors();
    test_ids_and_removal();
    test_range_and_checked32();
    test_edit();
    test_zero_invalid_and_noop();
    test_capacity_and_long_chain();
    test_balanced_union_and_vertices();
    puts("constraints_test: all tests passed");
    return 0;
}
