#include "../cad_userparams.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static CadUserParams table, other;
static char text[CAD_USERPARAM_TEXT_MAX + 64], before[CAD_USERPARAM_TEXT_MAX + 64];

static unsigned add(const char *name, const char *expr)
{
    unsigned id = 0;
    CadUserParamError e;
    CadUserParamStatus s = cad_userparams_add(&table, name, expr, &id, &e);
    if (s != CAD_UP_OK)
        fprintf(stderr, "add %s=%s: %s (id %u, offset %u)\n", name, expr,
                e.message, e.parameter_id, e.offset);
    assert(s == CAD_UP_OK && e.status == CAD_UP_OK && id);
    return id;
}
static const CadUserParamInfo *info(unsigned id)
{
    static CadUserParamInfo i;
    assert(cad_userparams_get(&table, id, &i) == CAD_UP_OK);
    return &i;
}
static void expect_display(unsigned id, const char *display)
{
    if (strcmp(info(id)->display, display))
        fprintf(stderr, "display %s != %s\n", info(id)->display, display);
    assert(!strcmp(info(id)->display, display));
}
static unsigned snapshot(CadUserParams *t, char *buffer)
{
    unsigned n;
    assert(cad_userparams_write_text(t, buffer, sizeof(text), &n) == CAD_UP_OK);
    return n;
}
/* The table text and every evaluated value are unchanged. */
static CadUserParams saved;
static void save(void) { saved = table; snapshot(&table, before); }
static void unchanged(void)
{
    snapshot(&table, text);
    assert(!strcmp(text, before));
    assert(!memcmp(&saved.live, &table.live, sizeof(saved.live)));
    assert(saved.units == table.units);
}
static long eval(const char *expr, CadUserParamStatus status, unsigned offset)
{
    long v = 12345L;
    CadUserParamError e;
    CadUserParamStatus s = cad_userparams_eval_expression(&table, expr, &v, &e);
    if (s != status)
        fprintf(stderr, "eval %s: expected %s got %s (offset %u)\n", expr,
                cad_userparams_status_string(status), e.message, e.offset);
    assert(s == status && e.status == status && e.message);
    if (s != CAD_UP_OK) {
        assert(v == 12345L && e.parameter_id == 0);
        if (offset != 999U) assert(e.offset == offset);
    }
    return v;
}

static void editing(void)
{
    unsigned width, depth, ratio, half, id;
    CadUserParamError e;
    CadUserParamInfo i;
    cad_userparams_init(&table, CAD_UNITS_MM);
    assert(cad_userparams_count(&table) == 0);
    width = add("Width", "200 mm");
    depth = add("Depth", "Width/2");
    ratio = add("Ratio", "0.15");
    half = add("Half", "Width * Ratio");
    assert(cad_userparams_count(&table) == 4);
    assert(cad_userparams_find(&table, "wIDTH") == width);
    assert(cad_userparams_find(&table, "Nope") == 0);
    expect_display(width, "200 mm");
    expect_display(depth, "100 mm");
    expect_display(ratio, "0.15");
    expect_display(half, "30 mm");
    assert(info(half)->type == CAD_UP_LENGTH && info(half)->ticks == 3000L);
    assert(info(ratio)->type == CAD_UP_SCALAR && info(ratio)->ticks == 15L);
    assert(cad_userparams_at(&table, 2, &i) == CAD_UP_OK && i.id == ratio);
    assert(!strcmp(i.name, "Ratio") && !strcmp(i.expression, "0.15"));
    assert(cad_userparams_at(&table, 4, &i) == CAD_UP_NOT_FOUND);

    /* Edits propagate through dependents. */
    assert(cad_userparams_set_expression(&table, width, "30cm", &e) == CAD_UP_OK);
    expect_display(depth, "150 mm");
    expect_display(half, "45 mm");
    assert(cad_userparams_set_expression_named(&table, "ratio", "-0.5", &e) == CAD_UP_OK);
    expect_display(half, "-150 mm");
    expect_display(ratio, "-0.5");
    assert(cad_userparams_set_expression_named(&table, "zz", "1", &e) == CAD_UP_NOT_FOUND);
    /* A dependent's type may change with its source. */
    assert(cad_userparams_set_expression(&table, ratio, "1mm", &e) == CAD_UP_DIMENSION);
    assert(e.parameter_id == half);

    /* Failures are atomic and identify the parameter and offset. */
    save();
    assert(cad_userparams_set_expression(&table, width, "300 +", &e) == CAD_UP_EXPRESSION);
    assert(e.parameter_id == width && e.offset == 5);
    unchanged();
    assert(cad_userparams_set_expression(&table, width, "Depth*2", &e) == CAD_UP_CYCLE);
    assert(e.parameter_id != 0);
    unchanged();
    assert(cad_userparams_add(&table, "Loop", "Loop+1", &id, &e) == CAD_UP_CYCLE);
    unchanged();
    assert(cad_userparams_add(&table, "Bad", "Width + 1", 0, &e) == CAD_UP_DIMENSION);
    assert(e.parameter_id == 5);       /* the ID it would have received */
    unchanged();
    assert(cad_userparams_add(&table, "Bad", "Width*Width", 0, &e) == CAD_UP_DIMENSION);
    assert(cad_userparams_add(&table, "Bad", "Wdth*2", 0, &e) == CAD_UP_NOT_FOUND);
    assert(e.offset == 0 && e.parameter_id == 5);
    assert(cad_userparams_add(&table, "Bad", "2*Wdth", 0, &e) == CAD_UP_NOT_FOUND);
    assert(e.offset == 2);
    assert(cad_userparams_add(&table, "Bad", "1/0", 0, &e) == CAD_UP_DIV_ZERO);
    assert(cad_userparams_add(&table, "Bad", "21474836.47*2", 0, &e) == CAD_UP_OVERFLOW);
    assert(cad_userparams_add(&table, "Bad", "((((((((((((((((((1))))))))))))))))))",
                              0, &e) == CAD_UP_NESTING);
    assert(cad_userparams_add(&table, "Bad", "1.234", 0, &e) == CAD_UP_EXPRESSION);
    assert(cad_userparams_add(&table, "Bad", "2 furlong", 0, &e) == CAD_UP_UNITS);
    assert(cad_userparams_add(&table, "Bad", "0.01in", 0, &e) == CAD_UP_UNITS);
    assert(cad_userparams_add(&table, "Bad", "", 0, &e) == CAD_UP_EXPRESSION);
    assert(cad_userparams_add(&table, "Bad", "1\t+1", 0, &e) == CAD_UP_EXPRESSION);
    assert(e.offset == 1);
    assert(cad_userparams_add(&table, "1Bad", "1", 0, &e) == CAD_UP_NAME);
    assert(cad_userparams_add(&table, "B d", "1", 0, &e) == CAD_UP_NAME);
    assert(cad_userparams_add(&table, "", "1", 0, &e) == CAD_UP_NAME);
    assert(cad_userparams_add(&table, "DEPTH", "1", 0, &e) == CAD_UP_DUPLICATE);
    assert(e.parameter_id == depth);
    assert(cad_userparams_add(&table, 0, "1", 0, &e) == CAD_UP_ARGUMENT);
    assert(cad_userparams_set_expression(&table, 99, "1", &e) == CAD_UP_NOT_FOUND);
    unchanged();
    assert(cad_userparams_evaluate(&table, &e) == CAD_UP_OK);
    unchanged();

    /* Rename rewrites references, including different-case spellings. */
    assert(cad_userparams_set_expression(&table, half, "WIDTH * ratio", &e) == CAD_UP_OK);
    assert(cad_userparams_rename(&table, width, "Breite", &e) == CAD_UP_OK);
    assert(!strcmp(info(depth)->expression, "Breite/2"));
    assert(!strcmp(info(half)->expression, "Breite * ratio"));
    assert(cad_userparams_find(&table, "Width") == 0);
    assert(cad_userparams_find(&table, "breite") == width);
    expect_display(half, "-150 mm");
    assert(cad_userparams_rename(&table, width, "BREITE", &e) == CAD_UP_OK);
    assert(!strcmp(info(depth)->expression, "BREITE/2"));
    save();
    assert(cad_userparams_rename(&table, width, "Ratio", &e) == CAD_UP_DUPLICATE);
    assert(e.parameter_id == ratio);
    assert(cad_userparams_rename(&table, width, "9x", &e) == CAD_UP_NAME);
    assert(cad_userparams_rename(&table, 77, "Ok", &e) == CAD_UP_NOT_FOUND);
    unchanged();

    /* References and guarded removal. */
    assert(cad_userparams_references(&table, "ratio") == half);
    assert(cad_userparams_references(&table, "breite") == depth);
    assert(cad_userparams_references(&table, "Half") == 0);
    assert(cad_userparams_remove(&table, ratio, &e) == CAD_UP_IN_USE);
    assert(e.parameter_id == half && e.message);
    unchanged();
    assert(cad_userparams_remove(&table, half, &e) == CAD_UP_OK);
    assert(cad_userparams_remove(&table, ratio, &e) == CAD_UP_OK);
    assert(cad_userparams_count(&table) == 2);
    assert(cad_userparams_get(&table, half, &i) == CAD_UP_NOT_FOUND);
    id = add("Half", "BREITE/2");
    assert(id == 5);                   /* IDs are never reused */
    assert(cad_userparams_at(&table, 2, &i) == CAD_UP_OK && i.id == id);
    assert(cad_userparams_remove(&table, half, &e) == CAD_UP_NOT_FOUND);
}

static void unit_suffix_rename(void)
{
    unsigned mm, use;
    CadUserParamError e;
    cad_userparams_init(&table, CAD_UNITS_MM);
    mm = add("mm", "5");
    use = add("Use", "2 mm + mm*1mm");
    expect_display(use, "7 mm");
    assert(cad_userparams_references(&table, "mm") == use);
    assert(cad_userparams_rename(&table, mm, "Five", &e) == CAD_UP_OK);
    assert(!strcmp(info(use)->expression, "2 mm + Five*1mm"));
    expect_display(use, "7 mm");
    /* Type inference across a chain. */
    add("A", "2");
    add("B", "A*3mm");
    add("C", "B/A");
    expect_display(add("D", "B/C"), "2");
    expect_display(cad_userparams_find(&table, "C"), "3 mm");
}

static void unit_modes(void)
{
    unsigned w;
    CadUserParamError e;
    /* Unspecified: bare numbers are model units, physical literals rejected. */
    cad_userparams_init(&table, CAD_UNITS_UNSPECIFIED);
    w = add("W", "200");
    expect_display(w, "200");
    assert(info(w)->type == CAD_UP_SCALAR);
    save();
    assert(cad_userparams_add(&table, "L", "2 mm", 0, &e) == CAD_UP_UNITS);
    assert(e.offset == 2 && e.parameter_id == 2);
    assert(cad_userparams_add(&table, "L", "W + 1cm", 0, &e) == CAD_UP_UNITS);
    assert(e.offset == 5);
    unchanged();
    assert(eval("W/2 + 10", CAD_UP_OK, 0) == 110L);
    assert(eval("w", CAD_UP_OK, 0) == 200L);
    assert(eval("-W*2", CAD_UP_OK, 0) == -400L);
    eval("W*3", CAD_UP_RANGE, 0);
    eval("1cm", CAD_UP_UNITS, 1);
    eval("2 * 3 in", CAD_UP_UNITS, 6);
    assert(eval("2.5", CAD_UP_OK, 0) == 3L);
    assert(eval("-2.5", CAD_UP_OK, 0) == -3L);
    assert(eval("2.49", CAD_UP_OK, 0) == 2L);
    assert(eval("-0.5", CAD_UP_OK, 0) == -1L);
    assert(eval("-0.49", CAD_UP_OK, 0) == 0L);
    assert(eval("400.49", CAD_UP_OK, 0) == 400L);
    eval("400.5", CAD_UP_RANGE, 0);
    eval("-400.5", CAD_UP_RANGE, 0);
    eval("W/0", CAD_UP_DIV_ZERO, 999U);
    eval("Nope+1", CAD_UP_NOT_FOUND, 0);
    eval("1 + Nope", CAD_UP_NOT_FOUND, 4);
    eval("W +", CAD_UP_EXPRESSION, 999U);
    eval("", CAD_UP_EXPRESSION, 0);
    eval("21474836.47*2", CAD_UP_OVERFLOW, 999U);
    unchanged();
    /* A parameter name that collides with the internal reserved one. */
    add("_00", "1");
    add("_01", "_00 + 1");
    assert(eval("_00 + _01", CAD_UP_OK, 0) == 3L);
    /* Switching unit modes. */
    assert(cad_userparams_set_units(&table, CAD_UNITS_INCHES, &e) == CAD_UP_UNITS);
    assert(cad_userparams_units(&table) == CAD_UNITS_UNSPECIFIED);
    assert(cad_userparams_set_units(&table, CAD_UNITS_MM, &e) == CAD_UP_OK);
    assert(info(w)->type == CAD_UP_SCALAR);
    add("Len", "W * 1mm");
    save();
    assert(cad_userparams_set_units(&table, CAD_UNITS_UNSPECIFIED, &e) == CAD_UP_UNITS);
    assert(e.parameter_id == cad_userparams_find(&table, "Len") && e.offset == 5);
    unchanged();

    /* mm: 1 model unit = 1 mm, cm/in converted, scalars also accepted. */
    cad_userparams_init(&table, CAD_UNITS_MM);
    w = add("Width", "200 mm");
    assert(eval("Width/2 + 10 mm", CAD_UP_OK, 0) == 110L);
    eval("Width/2 + 10", CAD_UP_DIMENSION, 999U);
    assert(eval("5", CAD_UP_OK, 0) == 5L);
    assert(eval("1in", CAD_UP_OK, 0) == 25L);
    assert(eval("0.5in", CAD_UP_OK, 0) == 13L);
    assert(eval("-0.5 in", CAD_UP_OK, 0) == -13L);
    assert(eval("1.5cm", CAD_UP_OK, 0) == 15L);
    assert(eval("Width*2", CAD_UP_OK, 0) == 400L);
    assert(eval("Width*2 + 0.49mm", CAD_UP_OK, 0) == 400L);
    eval("Width*2 + 0.5mm", CAD_UP_RANGE, 0);
    eval("0.01in", CAD_UP_UNITS, 999U);
    assert(eval("Width/Width", CAD_UP_OK, 0) == 1L);
    cad_userparams_init(&table, CAD_UNITS_INCHES);
    assert(cad_userparams_add(&table, "X", "1", 0, &e) == CAD_UP_UNITS);
    eval("1", CAD_UP_UNITS, 999U);
}

static void expect_read(const char *src, CadUnits units, CadUserParamStatus status,
                        unsigned line)
{
    CadUserParamError e;
    CadUserParamStatus s;
    save();
    s = cad_userparams_read_text(&table, src, (unsigned)strlen(src), units, &e);
    if (s != status || e.line != line)
        fprintf(stderr, "read [%s]: expected %s line %u, got %s line %u\n", src,
                cad_userparams_status_string(status), line, e.message, e.line);
    assert(s == status && e.status == status && e.line == line);
    unchanged();
}

static void serialization(void)
{
    unsigned n, m;
    CadUserParamError e;
    CadUserParamInfo a, b;
    unsigned i;
    cad_userparams_init(&table, CAD_UNITS_MM);
    add("Width", "200 mm");
    add("Depth", " Width / 2 ");
    add("k", "0.15");
    add("Area", "(Depth+1cm)*k ");
    n = snapshot(&table, before);
    assert(!strcmp(before, "P Width 200 mm\nP Depth  Width / 2 \nP k 0.15\n"
                           "P Area (Depth+1cm)*k \n"));
    assert(n == strlen(before));
    cad_userparams_init(&other, CAD_UNITS_UNSPECIFIED);
    assert(cad_userparams_read_text(&other, before, n, CAD_UNITS_MM, &e) == CAD_UP_OK);
    assert(cad_userparams_units(&other) == CAD_UNITS_MM);
    m = snapshot(&other, text);
    assert(m == n && !memcmp(text, before, n));
    for (i = 0; i < 4; ++i) {
        assert(cad_userparams_at(&table, i, &a) == CAD_UP_OK);
        assert(cad_userparams_at(&other, i, &b) == CAD_UP_OK);
        assert(a.id == b.id && a.ticks == b.ticks && a.type == b.type);
        assert(!strcmp(a.display, b.display) && !strcmp(a.expression, b.expression));
    }
    /* Exact-size buffer works without NUL; a smaller one reports the need. */
    assert(cad_userparams_write_text(&table, text, n, &m) == CAD_UP_OK && m == n);
    assert(cad_userparams_write_text(&table, text, n - 1, &m) == CAD_UP_CAPACITY);
    assert(m == n);
    /* Empty table and empty text. */
    cad_userparams_init(&other, CAD_UNITS_MM);
    assert(cad_userparams_write_text(&other, text, 0, &m) == CAD_UP_OK && m == 0);
    assert(cad_userparams_read_text(&table, "", 0, CAD_UNITS_MM, &e) == CAD_UP_OK);
    assert(cad_userparams_count(&table) == 0);
    assert(cad_userparams_read_text(&table, before, n, CAD_UNITS_MM, &e) == CAD_UP_OK);

    /* Malformed text is rejected with its line; the table is unchanged. */
    expect_read("X a 1\n", CAD_UNITS_MM, CAD_UP_FORMAT, 1);
    expect_read("P a 1\nP b\n", CAD_UNITS_MM, CAD_UP_FORMAT, 2);
    expect_read("P a 1\nP b \n", CAD_UNITS_MM, CAD_UP_EXPRESSION, 2);
    expect_read("P a 1", CAD_UNITS_MM, CAD_UP_FORMAT, 1);
    expect_read("P a 1\n\n", CAD_UNITS_MM, CAD_UP_FORMAT, 2);
    expect_read("P a 1\r\n", CAD_UNITS_MM, CAD_UP_FORMAT, 1);
    expect_read("p a 1\n", CAD_UNITS_MM, CAD_UP_FORMAT, 1);
    expect_read("P a 1\nP A 2\n", CAD_UNITS_MM, CAD_UP_DUPLICATE, 2);
    expect_read("P a 1\nP 1a 2\n", CAD_UNITS_MM, CAD_UP_NAME, 2);
    expect_read("P a 1\nP  2\n", CAD_UNITS_MM, CAD_UP_NAME, 2);
    expect_read("P abcdefghijabcdefghijabcdefghijkl 2\n", CAD_UNITS_MM, CAD_UP_NAME, 1);
    expect_read("P a 1\nP b a+\n", CAD_UNITS_MM, CAD_UP_EXPRESSION, 2);
    expect_read("P a b\nP b 1\nP c b*a\nP d c+d\n", CAD_UNITS_MM, CAD_UP_CYCLE, 4);
    expect_read("P a 1\nP b 2mm\n", CAD_UNITS_UNSPECIFIED, CAD_UP_UNITS, 2);
    expect_read("P a 1\nP b q\n", CAD_UNITS_MM, CAD_UP_NOT_FOUND, 2);
    expect_read("P a 1\n", CAD_UNITS_INCHES, CAD_UP_UNITS, 0);
    {
        CadUserParamStatus s;
        save();
        s = cad_userparams_read_text(&table, "P a 1\0\n", 7, CAD_UNITS_MM, &e);
        assert(s == CAD_UP_FORMAT && e.line == 1 && e.offset == 5);
        unchanged();
        s = cad_userparams_read_text(&table, before, CAD_USERPARAM_TEXT_MAX + 1,
                                     CAD_UNITS_MM, &e);
        assert(s == CAD_UP_CAPACITY);
        unchanged();
    }
}

static void capacity(void)
{
    char name[40], expr[200];
    unsigned i, n;
    CadUserParamError e;
    /* Parameter count. */
    cad_userparams_init(&table, CAD_UNITS_MM);
    for (i = 0; i < CAD_USERPARAM_MAX; ++i) {
        sprintf(name, "P%u", i);
        if (i) sprintf(expr, "P%u + 1", i - 1);
        else strcpy(expr, "1");
        add(name, expr);
    }
    expect_display(cad_userparams_find(&table, "P30"), "31");
    save();
    assert(cad_userparams_add(&table, "Extra", "1", 0, &e) == CAD_UP_FULL);
    /* A full table can still evaluate one-off expressions. */
    assert(eval("P30 + P0", CAD_UP_OK, 0) == 32L);
    unchanged();
    n = snapshot(&table, text);
    cad_userparams_init(&other, CAD_UNITS_MM);
    assert(cad_userparams_read_text(&other, text, n, CAD_UNITS_MM, &e) == CAD_UP_OK);
    strcat(text, "P Extra 1\n");
    assert(cad_userparams_read_text(&other, text, n + 10, CAD_UNITS_MM, &e) == CAD_UP_FULL);
    assert(e.line == 32);

    /* Text size limit: 25 maximal lines fit (4075 bytes), 26 do not. */
    cad_userparams_init(&table, CAD_UNITS_MM);
    strcpy(expr, "10");
    for (i = 0; i < 63; ++i) strcat(expr, "+0");
    assert(strlen(expr) == CAD_USERPARAM_EXPR_MAX);
    for (i = 0; i < 25; ++i) {
        sprintf(name, "N%030u", i);
        assert(strlen(name) == CAD_USERPARAM_NAME_MAX);
        add(name, expr);
    }
    n = snapshot(&table, text);
    assert(n == 25U * 163U);
    save();
    sprintf(name, "N%030u", 99U);
    assert(cad_userparams_add(&table, name, expr, 0, &e) == CAD_UP_CAPACITY);
    unchanged();
    expr[CAD_USERPARAM_EXPR_MAX] = '0'; expr[CAD_USERPARAM_EXPR_MAX + 1] = 0;
    assert(cad_userparams_add(&table, "S", expr, 0, &e) == CAD_UP_EXPRESSION);
    assert(e.offset == CAD_USERPARAM_EXPR_MAX);
    strcat(name, "9");
    assert(cad_userparams_add(&table, name, "1", 0, &e) == CAD_UP_NAME);
    unchanged();

    /* Rename that would overflow a referencing expression. */
    cad_userparams_init(&table, CAD_UNITS_MM);
    add("a", "1");
    strcpy(expr, "a");
    for (i = 0; i < 63; ++i) strcat(expr, "+a");
    add("b", expr);
    expect_display(2, "64");
    save();
    assert(cad_userparams_rename(&table, 1, "ab", &e) == CAD_UP_CAPACITY);
    assert(e.parameter_id == 2);
    unchanged();

    /* ID exhaustion (white-box: skip 65534 add/remove cycles). */
    cad_userparams_init(&table, CAD_UNITS_MM);
    table.live.next_id = 65535U;
    assert(add("Last", "1") == 65535U);
    assert(cad_userparams_add(&table, "More", "1", 0, &e) == CAD_UP_FULL);
}

int main(void)
{
    editing();
    unit_suffix_rename();
    unit_modes();
    serialization();
    capacity();
    printf("cad_userparams tests passed\n");
    return 0;
}
