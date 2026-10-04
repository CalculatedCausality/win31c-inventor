/* Host: cc -std=c89 -pedantic -Wall -Wextra -Werror
 *       cad_assembly.c tests/assembly_test.c -o /tmp/opencode/assembly_test
 * No implementation inclusion, Windows stubs, heap, or floating point.
 */
#include "../cad_assembly.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define OK(x) CHECK((x) == CAD_ASSEMBLY_OK)

static CadAssembly a, saved;
static CadAssemblyTransform unit, t, u, v, worlds[CAD_ASSEMBLY_MAX];
static CadAssemblyTransform saved_worlds[CAD_ASSEMBLY_MAX];
static CadAssemblyOccurrence occurrence;
static CadAssemblyId root, child, sibling, ids[CAD_ASSEMBLY_MAX], result;

static int same(const CadAssemblyTransform *p, const CadAssemblyTransform *q)
{
    int i, j;
    for (i = 0; i < 3; ++i) {
        if (p->t[i] != q->t[i]) return 0;
        for (j = 0; j < 3; ++j) if (p->r[i][j] != q->r[i][j]) return 0;
    }
    return 1;
}

static void snapshot(void) { memcpy(&saved, &a, sizeof(a)); }
static void unchanged(void) { CHECK(!memcmp(&saved, &a, sizeof(a))); }
static void reset(void)
{
    CadAssemblyInit(&a);
    CadAssemblyIdentity(&unit);
    t = u = v = unit;
}

static void definitions_and_placement(void)
{
    int i;
    reset();
    t.t[0] = 100;
    OK(CadAssemblyAdd(&a, 7, 0, &t, &root));
    t.t[0] = -20;
    OK(CadAssemblyAdd(&a, 7, 0, &t, &sibling));
    CHECK(root != sibling);
    OK(CadAssemblyGet(&a, root, &occurrence));
    CHECK(occurrence.part_id == 7 && occurrence.parent_id == 0);
    OK(CadAssemblyGet(&a, sibling, &occurrence));
    CHECK(occurrence.part_id == 7);
    t.t[0] = 150;
    OK(CadAssemblySetLocal(&a, root, &t));
    OK(CadAssemblyGetWorld(&a, sibling, &u)); CHECK(u.t[0] == -20);
    OK(CadAssemblyGetWorld(&a, root, &u)); CHECK(u.t[0] == 150);
    /* Independent hand-calculated 90-degree parent composition. */
    t = unit; t.r[0][0] = t.r[1][1] = 0;
    t.r[0][1] = -1024; t.r[1][0] = 1024; t.t[0] = 100;
    OK(CadAssemblySetLocal(&a, root, &t));
    t = unit; t.t[0] = 10; t.t[1] = 20; t.t[2] = -3;
    OK(CadAssemblyAdd(&a, 9, root, &t, &child));
    OK(CadAssemblyGetWorld(&a, child, &u));
    CHECK(u.t[0] == 80 && u.t[1] == 10 && u.t[2] == -3);
    CHECK(u.r[0][1] == -1024 && u.r[1][0] == 1024);
    snapshot();
    for (i = 0; i < 1000; ++i) {
        OK(CadAssemblyGetWorld(&a, child, &v)); CHECK(same(&u, &v));
    }
    unchanged(); /* Evaluation never rewrites locals or IDs. */
    OK(CadAssemblyReparent(&a, child, sibling));
    OK(CadAssemblyGetWorld(&a, child, &u)); CHECK(u.t[0] == -10);
    OK(CadAssemblyGet(&a, child, &occurrence)); CHECK(same(&occurrence.local, &t));
    snapshot();
    CHECK(CadAssemblyRemove(&a, sibling) == CAD_ASSEMBLY_HAS_CHILDREN);
    unchanged();
    OK(CadAssemblyRemove(&a, root));
    OK(CadAssemblyGet(&a, sibling, &occurrence)); CHECK(occurrence.id == sibling);
    OK(CadAssemblyAdd(&a, 7, 0, &unit, &result)); CHECK(result > child);
    CHECK(CadAssemblyGet(&a, root, &occurrence) == CAD_ASSEMBLY_NOT_FOUND);
}

static void graph_and_atomicity(void)
{
    int i;
    reset();
    OK(CadAssemblyAdd(&a, 1, 0, &unit, &root));
    OK(CadAssemblyAdd(&a, 1, root, &unit, &child));
    snapshot();
    CHECK(CadAssemblyReparent(&a, root, child) == CAD_ASSEMBLY_CYCLE); unchanged();
    CHECK(CadAssemblyReparent(&a, child, child) == CAD_ASSEMBLY_CYCLE); unchanged();
    CHECK(CadAssemblyReparent(&a, child, 60000) == CAD_ASSEMBLY_NOT_FOUND); unchanged();
    result = 987;
    CHECK(CadAssemblyAdd(&a, 1, 60000, &unit, &result) == CAD_ASSEMBLY_NOT_FOUND);
    CHECK(result == 987); unchanged();
    CHECK(CadAssemblyAdd(&a, 0, 0, &unit, &result) == CAD_ASSEMBLY_ARGUMENT); unchanged();
    CHECK(CadAssemblyAdd(&a, 65536UL, 0, &unit, &result) == CAD_ASSEMBLY_ARGUMENT);
    unchanged();
    CHECK(CadAssemblySetLocal(&a, 60000, &unit) == CAD_ASSEMBLY_NOT_FOUND); unchanged();
    CHECK(CadAssemblyRemove(&a, 0) == CAD_ASSEMBLY_NOT_FOUND); unchanged();
    reset();
    result = 0;
    for (i = 0; i < CAD_ASSEMBLY_DEPTH; ++i) {
        OK(CadAssemblyAdd(&a, 1, result, &unit, &ids[i])); result = ids[i];
    }
    snapshot(); result = 777;
    CHECK(CadAssemblyAdd(&a, 1, ids[7], &unit, &result) == CAD_ASSEMBLY_TOO_DEEP);
    CHECK(result == 777); unchanged();
    OK(CadAssemblyAdd(&a, 1, 0, &unit, &sibling)); snapshot();
    CHECK(CadAssemblyReparent(&a, ids[0], sibling) == CAD_ASSEMBLY_TOO_DEEP);
    unchanged();
    reset();
    for (i = 0; i < CAD_ASSEMBLY_MAX; ++i)
        OK(CadAssemblyAdd(&a, 42, 0, &unit, &ids[i]));
    snapshot(); result = 777;
    CHECK(CadAssemblyAdd(&a, 42, 0, &unit, &result) == CAD_ASSEMBLY_FULL);
    CHECK(result == 777); unchanged();
    OK(CadAssemblyRemove(&a, ids[10]));
    OK(CadAssemblyAdd(&a, 42, 0, &unit, &result)); CHECK(result == 33);
    OK(CadAssemblyGet(&a, ids[31], &occurrence)); CHECK(occurrence.id == ids[31]);
    /* Exhaustion boundary without performing 65535 allocations. */
    reset(); a.next_id = CAD_ASSEMBLY_ID_MAX;
    OK(CadAssemblyAdd(&a, 1, 0, &unit, &root)); CHECK(root == 65535UL);
    OK(CadAssemblyRemove(&a, root)); snapshot(); result = 777;
    CHECK(CadAssemblyAdd(&a, 1, 0, &unit, &result) == CAD_ASSEMBLY_ID_EXHAUSTED);
    CHECK(result == 777); unchanged();
}

static void matrix_and_bounds(void)
{
    reset();
    OK(CadAssemblyValidateTransform(&unit));
    t.r[0][1] = 64; /* Shear. */
    CHECK(CadAssemblyValidateTransform(&t) == CAD_ASSEMBLY_TRANSFORM);
    t = unit; t.r[0][0] = 900; /* Scale. */
    CHECK(CadAssemblyValidateTransform(&t) == CAD_ASSEMBLY_TRANSFORM);
    t = unit; t.r[0][0] = -1024; /* Reflection. */
    CHECK(CadAssemblyValidateTransform(&t) == CAD_ASSEMBLY_TRANSFORM);
    t = unit; t.r[2][2] = INT_MIN;
    CHECK(CadAssemblyValidateTransform(&t) == CAD_ASSEMBLY_TRANSFORM);
    t = unit; t.t[0] = LONG_MIN;
    CHECK(CadAssemblyValidateTransform(&t) == CAD_ASSEMBLY_BOUNDS);
    t.t[0] = LONG_MAX;
    CHECK(CadAssemblyValidateTransform(&t) == CAD_ASSEMBLY_BOUNDS);
    t = unit; t.t[0] = 32767; t.t[1] = -32767;
    OK(CadAssemblyAdd(&a, 1, 0, &t, &root));
    snapshot(); u = unit; u.t[0] = 1; result = 777;
    CHECK(CadAssemblyAdd(&a, 1, root, &u, &result) == CAD_ASSEMBLY_BOUNDS);
    CHECK(result == 777); unchanged();
    u = unit; u.t[1] = -1;
    CHECK(CadAssemblyAdd(&a, 1, root, &u, &result) == CAD_ASSEMBLY_BOUNDS); unchanged();
    OK(CadAssemblyAdd(&a, 1, root, &unit, &child)); snapshot();
    t = unit; t.t[0] = 32768L;
    CHECK(CadAssemblySetLocal(&a, root, &t) == CAD_ASSEMBLY_BOUNDS); unchanged();
    t = unit; t.r[1][0] = 100;
    CHECK(CadAssemblySetLocal(&a, root, &t) == CAD_ASSEMBLY_TRANSFORM); unchanged();
    /* Ancestor change valid locally but descendant world overflows bounds. */
    t = unit; t.t[0] = -32767;
    OK(CadAssemblySetLocal(&a, root, &t));
    u = unit; u.t[0] = 32767;
    OK(CadAssemblySetLocal(&a, child, &u)); snapshot();
    t.t[0] = 1;
    CHECK(CadAssemblySetLocal(&a, root, &t) == CAD_ASSEMBLY_BOUNDS); unchanged();
    /* Compose supports output aliasing; failure leaves it untouched. */
    t = unit; u = unit; t.t[0] = 32767; u.t[0] = 1; v = t;
    CHECK(CadAssemblyCompose(&t, &u, &t) == CAD_ASSEMBLY_BOUNDS); CHECK(same(&t, &v));
    u.t[0] = -1; OK(CadAssemblyCompose(&t, &u, &t)); CHECK(t.t[0] == 32766);
    /* Symmetric half rounding, checked independently from the implementation. */
    t = unit; t.r[0][0] = t.r[1][1] = 724;
    t.r[0][1] = -724; t.r[1][0] = 724;
    u = unit; u.t[0] = 128;
    OK(CadAssemblyCompose(&t, &u, &v)); CHECK(v.t[0] == 91 && v.t[1] == 91);
    u.t[0] = -128;
    OK(CadAssemblyCompose(&t, &u, &v)); CHECK(v.t[0] == -91 && v.t[1] == -91);
    /* Near-rigid quantized matrices need not compose to a valid rigid result. */
    t = unit; t.r[0][1] = 3; OK(CadAssemblyValidateTransform(&t)); u = t; v = unit;
    CHECK(CadAssemblyCompose(&t, &u, &v) == CAD_ASSEMBLY_TRANSFORM);
    CHECK(same(&v, &unit));
}

static void grounding(void)
{
    reset();
    OK(CadAssemblyAdd(&a, 1, 0, &unit, &root));
    OK(CadAssemblyAdd(&a, 1, root, &unit, &child));
    OK(CadAssemblyAdd(&a, 1, 0, &unit, &sibling));
    OK(CadAssemblyGround(&a, child, 1)); snapshot();
    t.t[0] = 1;
    CHECK(CadAssemblySetLocal(&a, child, &t) == CAD_ASSEMBLY_GROUNDED); unchanged();
    CHECK(CadAssemblyReparent(&a, child, sibling) == CAD_ASSEMBLY_GROUNDED); unchanged();
    CHECK(CadAssemblyRemove(&a, child) == CAD_ASSEMBLY_GROUNDED); unchanged();
    CHECK(CadAssemblySetLocal(&a, root, &t) == CAD_ASSEMBLY_GROUNDED); unchanged();
    t = unit; t.r[0][0] = t.r[1][1] = -1024;
    CHECK(CadAssemblySetLocal(&a, root, &t) == CAD_ASSEMBLY_GROUNDED); unchanged();
    t = unit; t.t[1] = 30;
    OK(CadAssemblySetLocal(&a, sibling, &t)); snapshot();
    CHECK(CadAssemblyReparent(&a, root, sibling) == CAD_ASSEMBLY_GROUNDED); unchanged();
    CHECK(CadAssemblyGround(&a, child, 2) == CAD_ASSEMBLY_ARGUMENT); unchanged();
    OK(CadAssemblyGround(&a, child, 0));
    OK(CadAssemblySetLocal(&a, root, &t));
    OK(CadAssemblyGetWorld(&a, child, &u)); CHECK(u.t[1] == 30);
}

static void datums_and_hinge(void)
{
    int i, yaw;
    reset();
    t.t[0] = 100;
    OK(CadAssemblyAdd(&a, 1, 0, &t, &root));
    OK(CadAssemblyAdd(&a, 2, 0, &unit, &child));
    t = unit; u = unit; t.t[0] = 10; u.t[0] = 2;
    OK(CadAssemblyBindFixed(&a, child, root, &t, &u));
    OK(CadAssemblyGetWorld(&a, child, &v)); CHECK(v.t[0] == 108);
    snapshot();
    CHECK(CadAssemblySetLocal(&a, child, &unit) == CAD_ASSEMBLY_CONSTRAINED); unchanged();
    CHECK(CadAssemblyReparent(&a, child, 0) == CAD_ASSEMBLY_CONSTRAINED); unchanged();
    CHECK(CadAssemblySetYaw(&a, child, 1) == CAD_ASSEMBLY_CONSTRAINED); unchanged();
    CHECK(CadAssemblyBindFixed(&a, root, child, &t, &u) == CAD_ASSEMBLY_CYCLE); unchanged();
    CHECK(CadAssemblyBindFixed(&a, child, 60000, &t, &u) == CAD_ASSEMBLY_NOT_FOUND);
    unchanged();
    v = u; v.r[0][0] = -1024;
    CHECK(CadAssemblyBindFixed(&a, child, root, &t, &v) == CAD_ASSEMBLY_TRANSFORM);
    unchanged();
    OK(CadAssemblyBindHinge(&a, child, root, &t, &u, 2, 0, 4));
    OK(CadAssemblyGetWorld(&a, child, &v));
    CHECK(v.t[0] == 110 && v.t[1] == -2);
    CHECK(v.r[0][1] == -1024 && v.r[1][0] == 1024);
    snapshot();
    for (i = 0; i < 1000; ++i) {
        yaw = i % 5;
        OK(CadAssemblySetYaw(&a, child, yaw));
    }
    OK(CadAssemblySetYaw(&a, child, 2)); unchanged(); /* Absolute table, no drift. */
    CHECK(CadAssemblySetYaw(&a, child, -1) == CAD_ASSEMBLY_ARGUMENT); unchanged();
    CHECK(CadAssemblySetYaw(&a, child, 5) == CAD_ASSEMBLY_ARGUMENT); unchanged();
    CHECK(CadAssemblyBindHinge(&a, child, root, &t, &u, 0, 4, 0) == CAD_ASSEMBLY_ARGUMENT);
    unchanged();
    CHECK(CadAssemblyBindHinge(&a, child, root, &t, &u, 8, 0, 8) == CAD_ASSEMBLY_ARGUMENT);
    unchanged();
    OK(CadAssemblyGround(&a, child, 1)); snapshot();
    CHECK(CadAssemblySetYaw(&a, child, 3) == CAD_ASSEMBLY_GROUNDED); unchanged();
    CHECK(CadAssemblyClearConstraint(&a, child) == CAD_ASSEMBLY_GROUNDED); unchanged();
    CHECK(CadAssemblyBindFixed(&a, child, root, &t, &u) == CAD_ASSEMBLY_GROUNDED);
    unchanged();
    OK(CadAssemblyGround(&a, child, 0));
    OK(CadAssemblyClearConstraint(&a, child));
    OK(CadAssemblySetLocal(&a, child, &unit));
    /* Nonidentity child datum orientation: parent90 * inverse(child90)=I. */
    t = unit; t.r[0][0] = t.r[1][1] = 0;
    t.r[0][1] = -1024; t.r[1][0] = 1024; u = t;
    OK(CadAssemblyBindFixed(&a, child, 0, &t, &u));
    OK(CadAssemblyGetWorld(&a, child, &v)); CHECK(same(&v, &unit));
    OK(CadAssemblyBindHinge(&a, child, 0, &unit, &unit, 0, 0, 7));
    for (i = 0; i < CAD_ASSEMBLY_YAWS; ++i) {
        OK(CadAssemblySetYaw(&a, child, i));
        OK(CadAssemblyGetWorld(&a, child, &v)); OK(CadAssemblyValidateTransform(&v));
    }
    /* Failed inverse/frame translation must not partially install a binding. */
    t = unit; t.t[0] = 32767; u = unit; u.t[0] = -1; snapshot();
    CHECK(CadAssemblyBindFixed(&a, child, 0, &t, &u) == CAD_ASSEMBLY_BOUNDS); unchanged();
}

static void output_and_corruption(void)
{
    int i;
    reset();
    OK(CadAssemblyEvaluate(&a, worlds, CAD_ASSEMBLY_MAX));
    OK(CadAssemblyAdd(&a, 1, 0, &unit, &root));
    OK(CadAssemblyAdd(&a, 2, root, &unit, &child));
    memset(worlds, 0x5a, sizeof(worlds));
    memcpy(saved_worlds, worlds, sizeof(worlds));
    CHECK(CadAssemblyEvaluate(&a, worlds, 1) == CAD_ASSEMBLY_ARGUMENT);
    CHECK(!memcmp(worlds, saved_worlds, sizeof(worlds)));
    a.occurrence[1].parent_id = 60000;
    CHECK(CadAssemblyEvaluate(&a, worlds, CAD_ASSEMBLY_MAX) == CAD_ASSEMBLY_NOT_FOUND);
    CHECK(!memcmp(worlds, saved_worlds, sizeof(worlds)));
    a.occurrence[1].parent_id = root; a.occurrence[0].parent_id = child;
    CHECK(CadAssemblyEvaluate(&a, worlds, CAD_ASSEMBLY_MAX) == CAD_ASSEMBLY_CYCLE);
    CHECK(!memcmp(worlds, saved_worlds, sizeof(worlds)));
    a.occurrence[0].parent_id = 0; a.occurrence[1].local.t[0] = LONG_MAX;
    CHECK(CadAssemblyEvaluate(&a, worlds, CAD_ASSEMBLY_MAX) == CAD_ASSEMBLY_BOUNDS);
    CHECK(!memcmp(worlds, saved_worlds, sizeof(worlds)));
    a.occurrence[1].local = unit;
    OK(CadAssemblyEvaluate(&a, worlds, CAD_ASSEMBLY_MAX));
    for (i = 0; i < 2; ++i) CHECK(same(&worlds[i], &unit));
    CHECK(!memcmp(&worlds[2], &saved_worlds[2], (CAD_ASSEMBLY_MAX - 2) * sizeof(worlds[0])));
    u = t = unit;
    CHECK(CadAssemblyGetWorld(&a, 60000, &u) == CAD_ASSEMBLY_NOT_FOUND); CHECK(same(&t, &u));
    CHECK(CadAssemblyCompose(NULL, &unit, &u) == CAD_ASSEMBLY_ARGUMENT);
    CHECK(CadAssemblyValidateTransform(NULL) == CAD_ASSEMBLY_ARGUMENT);
    CHECK(CadAssemblyEvaluate(NULL, worlds, CAD_ASSEMBLY_MAX) == CAD_ASSEMBLY_ARGUMENT);
    CHECK(CadAssemblyAdd(NULL, 1, 0, &unit, &result) == CAD_ASSEMBLY_ARGUMENT);
    CHECK(CadAssemblyGround(NULL, 1, 1) == CAD_ASSEMBLY_ARGUMENT);
    CHECK(CadAssemblyRemove(NULL, 1) == CAD_ASSEMBLY_ARGUMENT);
}

static void deep_rotation_and_datum_grounding(void)
{
    int i;
    reset();
    /* Eight 45-degree locals are evaluated in root-to-leaf order each time.
     * Quantized orientation error is allowed, but cannot accumulate per call. */
    t.r[0][0] = t.r[1][1] = 724;
    t.r[0][1] = -724; t.r[1][0] = 724;
    result = 0;
    for (i = 0; i < CAD_ASSEMBLY_DEPTH; ++i) {
        OK(CadAssemblyAdd(&a, 1, result, &t, &ids[i])); result = ids[i];
    }
    OK(CadAssemblyGetWorld(&a, result, &u));
    CHECK(u.r[0][0] >= 1020 && u.r[0][0] <= 1024);
    CHECK(u.r[0][1] >= -4 && u.r[0][1] <= 4);
    snapshot();
    for (i = 0; i < 100; ++i) {
        OK(CadAssemblyGetWorld(&a, result, &v)); CHECK(same(&u, &v));
    }
    unchanged();
    reset();
    OK(CadAssemblyAdd(&a, 1, 0, &unit, &root));
    OK(CadAssemblyAdd(&a, 2, root, &unit, &child));
    t.t[0] = 10;
    OK(CadAssemblyBindHinge(&a, root, 0, &t, &unit, 0, 0, 7));
    t.t[0] = 500; /* Mutating source datum does not mutate the copied binding. */
    OK(CadAssemblySetYaw(&a, root, 2));
    OK(CadAssemblyGetWorld(&a, root, &u)); CHECK(u.t[0] == 10);
    OK(CadAssemblyGround(&a, child, 1)); snapshot();
    CHECK(CadAssemblySetYaw(&a, root, 4) == CAD_ASSEMBLY_GROUNDED); unchanged();
    CHECK(CadAssemblyBindFixed(&a, root, 0, &t, &unit) == CAD_ASSEMBLY_GROUNDED);
    unchanged();
    /* Rotated translation is checked after rotation, not just input axes. */
    reset();
    t = unit; t.r[0][0] = t.r[1][1] = 724;
    t.r[0][1] = -724; t.r[1][0] = 724;
    OK(CadAssemblyAdd(&a, 1, 0, &t, &root));
    u = unit; u.t[0] = u.t[1] = 32767; snapshot(); result = 777;
    CHECK(CadAssemblyAdd(&a, 2, root, &u, &result) == CAD_ASSEMBLY_BOUNDS);
    CHECK(result == 777); unchanged();
}

int main(void)
{
    definitions_and_placement();
    graph_and_atomicity();
    matrix_and_bounds();
    grounding();
    datums_and_hinge();
    output_and_corruption();
    deep_rotation_and_datum_grounding();
    puts("assembly tests passed");
    return 0;
}
