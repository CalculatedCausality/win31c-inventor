#include "../cad_params.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static CadParams table;
static CadValue value(long ticks, CadType type, CadUnit unit)
{
    CadValue v;
    v.ticks = ticks; v.type = type; v.unit = unit; return v;
}
static unsigned seed(const char *name, long ticks, CadType type, CadUnit unit)
{
    unsigned id = 0;
    assert(cad_params_add(&table, name, value(ticks, type, unit), &id) == CAD_OK);
    return id;
}
static void source(unsigned id, const char *text)
{
    assert(cad_params_set_expression(&table, id, text) == CAD_OK);
}
static long ticks(unsigned id) { return cad_params_get(&table, id)->value.ticks; }
static void expect(unsigned id, const char *text, CadStatus status, long result)
{
    CadError e;
    CadStatus actual;
    long original = ticks(id);
    source(id, text);
    actual = cad_params_evaluate(&table, &e);
    if (actual != status)
        fprintf(stderr, "%s: expected %s, got %s (id %u, offset %u)\n",
                text, cad_status_string(status), cad_status_string(actual),
                e.parameter_id, e.offset);
    assert(actual == status);
    assert(e.status == status);
    if (status == CAD_OK) assert(ticks(id) == result);
    else { assert(ticks(id) == original); assert(e.parameter_id != 0); }
}
static void table_tests(void)
{
    unsigned i, id, unchanged = 99;
    char name[32], expr[130];
    CadParameter original;
    cad_params_init(&table);
    id = seed("Stable", 100, CAD_SCALAR, CAD_UNIT_NONE);
    assert(id == 1 && cad_params_find(&table, "sTaBlE") == id);
    assert(cad_params_add(&table, "STABLE", value(0, CAD_SCALAR, CAD_UNIT_NONE),
                          &unchanged) == CAD_ERR_DUPLICATE);
    assert(unchanged == 99);
    assert(cad_params_add(&table, "1bad", value(0, CAD_SCALAR, CAD_UNIT_NONE),
                          0) == CAD_ERR_NAME);
    assert(cad_params_add(&table, "bad", value(0, CAD_LENGTH, CAD_UNIT_NONE),
                          0) == CAD_ERR_ARGUMENT);
    for (i = 1; i < CAD_PARAM_MAX; ++i) {
        sprintf(name, "P%u", i);
        assert(seed(name, 0, CAD_SCALAR, CAD_UNIT_NONE) == i + 1);
    }
    assert(cad_params_add(&table, "Full", value(0, CAD_SCALAR, CAD_UNIT_NONE),
                          0) == CAD_ERR_FULL);
    assert(cad_params_find(&table, "stable") == 1);
    assert(cad_params_get(&table, 0) == 0 && cad_params_get(&table, 33) == 0);
    original = table.params[0];
    memset(expr, ' ', 129); expr[129] = 0;
    assert(cad_params_set_expression(&table, id, expr) == CAD_ERR_EXPRESSION);
    assert(memcmp(&original, &table.params[0], sizeof(original)) == 0);
    expr[127] = '1'; expr[128] = 0;
    source(id, expr); assert(cad_params_evaluate(&table, 0) == CAD_OK);
    assert(ticks(id) == 100);
    assert(cad_params_set_value(&table, id, value(100, CAD_LENGTH, CAD_UNIT_MM))
           == CAD_ERR_DIMENSION);
#if LONG_MAX > 2147483647L
    assert(cad_params_set_value(&table, id,
           value(CAD_LONG_MAX + 1L, CAD_SCALAR, CAD_UNIT_NONE)) == CAD_ERR_ARGUMENT);
    assert(cad_params_set_value(&table, id,
           value(CAD_LONG_MIN - 1L, CAD_SCALAR, CAD_UNIT_NONE)) == CAD_ERR_ARGUMENT);
#endif
}
static void expression_tests(void)
{
    unsigned s, l, legacy;
    char nested[40];
    unsigned i;
    cad_params_init(&table);
    s = seed("S", 0, CAD_SCALAR, CAD_UNIT_NONE);
    l = seed("L", 100, CAD_LENGTH, CAD_UNIT_MM);
    legacy = seed("Old", 100, CAD_LENGTH, CAD_UNIT_LEGACY);
    expect(s, "-2 + 3 * (4 - 1) / +2", CAD_OK, 250);
    expect(s, "--2", CAD_OK, 200);
    expect(s, "-1 / 3", CAD_OK, -33);
    expect(l, "1in + 2cm + .50mm", CAD_OK, 4590);
    expect(l, "0.05IN", CAD_OK, 127);
    expect(l, "0.01in", CAD_ERR_UNITS, 0);
    expect(l, "2 * (3cm / 2)", CAD_OK, 3000);
    expect(s, "l / 1mm", CAD_OK, 3000);
    assert(cad_params_set_value(&table, s, value(0, CAD_SCALAR, CAD_UNIT_NONE))
           == CAD_OK);
    expect(l, "3mm + 1", CAD_ERR_DIMENSION, 0);
    expect(l, "3mm * 2mm", CAD_ERR_DIMENSION, 0);
    expect(l, "2 / 1mm", CAD_ERR_DIMENSION, 0);
    expect(l, "3mm / 0", CAD_ERR_DIV_ZERO, 0);
    expect(l, "Old + 1mm", CAD_ERR_UNITS, 0);
    expect(l, "1mm", CAD_OK, 100);
    expect(legacy, "Old * 2", CAD_ERR_CYCLE, 0);
    assert(cad_params_set_value(&table, legacy,
           value(100, CAD_LENGTH, CAD_UNIT_LEGACY)) == CAD_OK);
    expect(l, "Old * 2", CAD_OK, 200);
    assert(table.params[l - 1].value.unit == CAD_UNIT_LEGACY);
    expect(l, "200", CAD_ERR_DIMENSION, 0);
    expect(l, "3ft", CAD_ERR_UNITS, 0);
    expect(l, "Missing + 1mm", CAD_ERR_NOT_FOUND, 0);
    expect(l, "1.001mm", CAD_ERR_EXPRESSION, 0);
    expect(l, "1mm garbage", CAD_ERR_EXPRESSION, 0);
    expect(l, "()", CAD_ERR_EXPRESSION, 0);
    expect(l, "(1mm", CAD_ERR_EXPRESSION, 0);
    expect(l, "1mm", CAD_OK, 100);
    for (i = 0; i < 16; ++i) nested[i] = '(';
    nested[16] = '1';
    for (i = 17; i < 33; ++i) nested[i] = ')';
    nested[33] = 0;
    expect(s, nested, CAD_OK, 100);
    for (i = 0; i < 17; ++i) nested[i] = '(';
    nested[17] = '1';
    for (i = 18; i < 35; ++i) nested[i] = ')';
    nested[35] = 0;
    expect(s, nested, CAD_ERR_NESTING, 0);
    expect(s, "-----------------1", CAD_ERR_NESTING, 0);
    expect(s, "21474836.47", CAD_OK, CAD_LONG_MAX);
    expect(s, "21474836.47 * 1", CAD_OK, CAD_LONG_MAX);
    expect(s, "21474836.47 / 1", CAD_OK, CAD_LONG_MAX);
    expect(s, "21474836.47 * .5", CAD_OK, 1073741823L);
    expect(s, "21474836.47 + .01", CAD_ERR_OVERFLOW, 0);
    expect(s, "21474836.48", CAD_ERR_OVERFLOW, 0);
    expect(s, "-21474836.48", CAD_OK, CAD_LONG_MIN);
    expect(s, "-21474836.48 / 1", CAD_OK, CAD_LONG_MIN);
    expect(s, "-21474836.48 * .5", CAD_OK, -1073741824L);
    expect(s, "-21474836.48 * .01", CAD_OK, -21474836L);
    expect(s, "-21474836.48 - .01", CAD_ERR_OVERFLOW, 0);
    expect(s, "-(-21474836.48)", CAD_ERR_OVERFLOW, 0);
    expect(s, "-21474836.48 / -1", CAD_ERR_OVERFLOW, 0);
    expect(s, "21474836.47 * 2", CAD_ERR_OVERFLOW, 0);
    expect(s, "21474836.47 / .01", CAD_ERR_OVERFLOW, 0);
    expect(s, "0 * -21474836.48", CAD_OK, 0);
    expect(s, "21474836.47 - -21474836.48", CAD_ERR_OVERFLOW, 0);
    expect(s, "-21474836.48 - -21474836.48", CAD_OK, 0);
    expect(s, "-21474836.48 / -21474836.48", CAD_OK, 100);
    expect(s, "1 / -21474836.48", CAD_OK, 0);
    expect(l, "21474836.47cm", CAD_ERR_OVERFLOW, 0);
    expect(l, "845466.00in", CAD_OK, 2147483640L);
    expect(l, "845466.05in", CAD_ERR_OVERFLOW, 0);
}
static void dependency_tests(void)
{
    unsigned a, b, c, i, j, len;
    CadValue before[CAD_PARAM_MAX];
    CadError e;
    char name[32], text[80];
    cad_params_init(&table);
    a = seed("A", 100, CAD_SCALAR, CAD_UNIT_NONE);
    b = seed("B", 200, CAD_SCALAR, CAD_UNIT_NONE);
    c = seed("C", 300, CAD_SCALAR, CAD_UNIT_NONE);
    source(a, "b + 1"); source(b, "C * 2");
    assert(cad_params_evaluate(&table, &e) == CAD_OK);
    assert(ticks(a) == 700 && ticks(b) == 600);
    assert(cad_params_set_value(&table, c, value(500, CAD_SCALAR, CAD_UNIT_NONE))
           == CAD_OK);
    assert(ticks(c) == 300);
    assert(cad_params_evaluate(&table, 0) == CAD_OK);
    assert(ticks(a) == 1100 && ticks(b) == 1000 && ticks(c) == 500);
    for (i = 0; i < table.count; ++i) before[i] = table.params[i].value;
    source(a, "42"); source(c, "B + 1");
    assert(cad_params_evaluate(&table, &e) == CAD_ERR_CYCLE);
    assert(e.parameter_id == c && e.offset > 0);
    for (i = 0; i < table.count; ++i)
        assert(memcmp(&before[i], &table.params[i].value, sizeof(CadValue)) == 0);
    source(c, "unknown");
    assert(cad_params_evaluate(&table, &e) == CAD_ERR_NOT_FOUND);
    assert(e.parameter_id == c);
    for (i = 0; i < table.count; ++i)
        assert(memcmp(&before[i], &table.params[i].value, sizeof(CadValue)) == 0);
    cad_params_init(&table);
    for (i = 0; i < CAD_PARAM_MAX; ++i) {
        sprintf(name, "P%u", i); seed(name, 100, CAD_SCALAR, CAD_UNIT_NONE);
        if (i) { sprintf(text, "P%u + 1", i - 1); source(i + 1, text); }
    }
    assert(cad_params_evaluate(&table, 0) == CAD_OK);
    assert(ticks(32) == 3200);
    /* Reverse order forces the full dependency recursion bound. */
    for (i = 0; i < 31; ++i) {
        sprintf(text, "P%u + 1", i + 1); source(i + 1, text);
    }
    source(32, "1");
    assert(cad_params_evaluate(&table, 0) == CAD_OK && ticks(1) == 3200);
    /* Maximum independent expression nesting at every dependency level. */
    for (i = 0; i < 31; ++i) {
        for (j = 0; j < 16; ++j) text[j] = '(';
        sprintf(text + 16, "P%u + 1", i + 1);
        len = (unsigned)strlen(text);
        for (j = 0; j < 16; ++j) text[len + j] = ')';
        text[len + 16] = 0; source(i + 1, text);
    }
    assert(cad_params_evaluate(&table, 0) == CAD_OK && ticks(1) == 3200);
    source(32, "P0");
    assert(cad_params_evaluate(&table, 0) == CAD_ERR_CYCLE);
}
static void arithmetic_oracle_tests(void)
{
    unsigned a_id, b_id, result;
    long a, b, aa, bb, expected;
    cad_params_init(&table);
    a_id = seed("A", 0, CAD_SCALAR, CAD_UNIT_NONE);
    b_id = seed("B", 0, CAD_SCALAR, CAD_UNIT_NONE);
    result = seed("Result", 0, CAD_SCALAR, CAD_UNIT_NONE);
    /* Independent small-product oracle fits even a true signed32 host. */
    for (a = -1000; a <= 1000; a += 37) {
        for (b = -1000; b <= 1000; b += 91) {
            aa = a < 0 ? -a : a; bb = b < 0 ? -b : b;
            assert(cad_params_set_value(&table, a_id,
                   value(a, CAD_SCALAR, CAD_UNIT_NONE)) == CAD_OK);
            assert(cad_params_set_value(&table, b_id,
                   value(b, CAD_SCALAR, CAD_UNIT_NONE)) == CAD_OK);
            source(result, "A * B");
            assert(cad_params_evaluate(&table, 0) == CAD_OK);
            expected = aa * bb / 100L;
            if ((a < 0) != (b < 0)) expected = -expected;
            assert(ticks(result) == expected);
            if (b) {
                source(result, "A / B");
                assert(cad_params_evaluate(&table, 0) == CAD_OK);
                expected = aa * 100L / bb;
                if ((a < 0) != (b < 0)) expected = -expected;
                assert(ticks(result) == expected);
            }
        }
    }
}
static void house_tests(void)
{
    static const CadXYZ expected[30] = {
        {-100,-70,70},{100,-70,70},{100,-70,-70},{-100,-70,-70},
        {-100,30,70},{100,30,70},{100,30,-70},{-100,30,-70},
        {-100,100,0},{100,100,0},
        {-15,-70,70},{-15,0,70},{15,0,70},{15,-70,70},
        {-75,-30,70},{-35,-30,70},{-35,10,70},{-75,10,70},
        {35,-30,70},{75,-30,70},{75,10,70},{35,10,70},
        {40,55,-45},{65,55,-45},{65,75,-25},{40,75,-25},
        {40,120,-45},{65,120,-45},{65,120,-25},{40,120,-25}
    };
    CadXYZ out[30], before[30];
    CadValue values[CAD_PARAM_MAX];
    CadModelScale scale;
    CadError e;
    unsigned i, width, rise;
    scale.unit = CAD_UNIT_MM; scale.ticks_per_model_unit = 100;
    cad_house_defaults(&table);
    assert(cad_house_generate(&table, scale, out, &e) == CAD_OK);
    assert(memcmp(out, expected, sizeof(out)) == 0);
    width = cad_params_find(&table, "Width"); rise = cad_params_find(&table, "RoofRise");
    source(width, "30cm"); source(rise, "WallHeight * .8");
    source(cad_params_find(&table, "DoorWidth"), "Width / 10");
    assert(cad_house_generate(&table, scale, out, &e) == CAD_OK);
    assert(out[0].x == -150 && out[1].x == 150 && out[8].y == 110);
    assert(out[10].x == -15 && out[12].x == 15);
    assert(out[22].y == 58 && out[24].y == 81 && out[26].y == 126);
    memcpy(before, out, sizeof(out));
    for (i = 0; i < table.count; ++i) values[i] = table.params[i].value;
    source(width, "40cm"); source(rise, "500mm");
    assert(cad_house_generate(&table, scale, out, &e) == CAD_ERR_GEOMETRY);
    assert(memcmp(before, out, sizeof(out)) == 0);
    for (i = 0; i < table.count; ++i)
        assert(memcmp(&values[i], &table.params[i].value, sizeof(CadValue)) == 0);
    source(rise, "Width * Depth");
    assert(cad_house_generate(&table, scale, out, &e) == CAD_ERR_DIMENSION);
    assert(memcmp(before, out, sizeof(out)) == 0);
    cad_house_defaults(&table);
    source(cad_params_find(&table, "ChimneyBack"), "100mm");
    assert(cad_house_generate(&table, scale, out, &e) == CAD_ERR_GEOMETRY);
    cad_house_defaults(&table);
    scale.ticks_per_model_unit = 0;
    assert(cad_house_generate(&table, scale, out, &e) == CAD_ERR_ARGUMENT);
    scale.ticks_per_model_unit = 100; scale.unit = CAD_UNIT_NONE;
    assert(cad_house_generate(&table, scale, out, &e) == CAD_ERR_ARGUMENT);
    scale.unit = CAD_UNIT_LEGACY;
    assert(cad_house_generate(&table, scale, out, &e) == CAD_ERR_UNITS);
    for (i = 0; i < table.count; ++i) {
        CadValue v = table.params[i].literal;
        v.unit = CAD_UNIT_LEGACY;
        assert(cad_params_set_value(&table, i + 1, v) == CAD_OK);
    }
    assert(cad_house_generate(&table, scale, out, &e) == CAD_OK);
    assert(memcmp(out, expected, sizeof(out)) == 0);
    scale.unit = CAD_UNIT_MM;
    assert(cad_house_generate(&table, scale, out, &e) == CAD_ERR_UNITS);
    cad_house_defaults(&table);
    /* Explicit 2 mm/model-unit changes coordinates, not source values. */
    scale.ticks_per_model_unit = 200;
    assert(cad_house_generate(&table, scale, out, &e) == CAD_OK);
    assert(out[0].x == -50 && out[26].y == 60 && ticks(width) == 20000);
    source(cad_params_find(&table, "BaseY"), "21474836.47mm");
    assert(cad_house_generate(&table, scale, out, &e) == CAD_ERR_OVERFLOW);
}
int main(void)
{
    table_tests(); expression_tests(); dependency_tests();
    arithmetic_oracle_tests(); house_tests();
    puts("params_test: all tests passed");
    return 0;
}
