/* Host: cc -std=c89 -pedantic -Wall -Wextra -Werror cad_parts.c
 *       cad_assembly.c cad_document.c cad_model.c cad_create.c cad_sketch.c
 *       tests/parts_test.c -o /tmp/opencode/parts_test
 * Run in a scratch directory: it creates and removes PT*.CAD files there.
 * Win16: wcc -q -bt=windows -zW -ml -zt=1 -2 -ox -w4 -we tests/parts_test.c
 */
#include "../cad_parts.h"
#include "../cad_document.h"
#include "../cad_create.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define OK(x) CHECK((x) == CAD_PARTS_OK)

static CadParts p, q, saved;
static CadModel model;
static CadPartsInfo info;
static char text[CAD_PARTS_TEXT_MAX], text2[CAD_PARTS_TEXT_MAX];
static char big[CAD_PARTS_TEXT_MAX];

static const char *files[] = {
    "PTBOX.CAD", "PTLINE.CAD", "PTNONE.CAD", "PTC.CAD", "PTD.CAD"
};

typedef struct Seg { long v[6]; } Seg;
typedef struct Collect { unsigned n; Seg seg[64]; } Collect;
static Collect col;

static void collect(void *ctx, long x1, long y1, long z1,
                    long x2, long y2, long z2)
{
    Collect *c = (Collect *)ctx;
    if (c->n < 64) {
        Seg *s = &c->seg[c->n];
        s->v[0] = x1; s->v[1] = y1; s->v[2] = z1;
        s->v[3] = x2; s->v[4] = y2; s->v[5] = z2;
    }
    ++c->n;
}

static void cleanup(void)
{
    unsigned i;
    for (i = 0; i < sizeof(files) / sizeof(files[0]); ++i) remove(files[i]);
}

static void save_box(const char *path)
{
    CHECK(cad_model_init(&model) == CAD_OK);
    CHECK(cad_create_box(&model, 0, 0, 0, 10, 20, 30) == CAD_OK);
    remove(path);
    CHECK(cad_document_save(path, &model) == CAD_DOC_OK);
}

static void save_line(const char *path)
{
    CHECK(cad_model_init(&model) == CAD_OK);
    CHECK(cad_create_line(&model, 0, 0, 0, 100, 5, 40) == CAD_OK);
    remove(path);
    CHECK(cad_document_save(path, &model) == CAD_DOC_OK);
}

static unsigned used_slots(const CadParts *x)
{
    unsigned i, n = 0;
    for (i = 0; i < CAD_PARTS_CACHE; ++i) if (x->slot[i].path[0]) ++n;
    return n;
}

static void snapshot(void) { memcpy(&saved, &p, sizeof(p)); }
static void unchanged(void) { CHECK(!memcmp(&saved, &p, sizeof(p))); }

static long dist(long a, long b) { return a > b ? a - b : b - a; }

static void place_and_names(void)
{
    CadAssemblyId a1, a2, b1, bad = 99;
    long mn[3], mx[3];

    cad_parts_init(&p);
    CHECK(cad_parts_count(&p) == 0);
    CHECK(!cad_parts_bounds(&p, mn, mx));
    OK(cad_parts_place(&p, "PTBOX.CAD", 0, 0, 0, 0, &a1));
    OK(cad_parts_place(&p, "PTLINE.CAD", 100, 0, 0, 0, &b1));
    OK(cad_parts_place(&p, "PTBOX.CAD", -50, 10, 5, 0, &a2));
    CHECK(cad_parts_count(&p) == 3 && used_slots(&p) == 2);
    CHECK(a1 == 1 && b1 == 2 && a2 == 3);
    OK(cad_parts_get(&p, a1, &info));
    CHECK(!strcmp(info.name, "PTBOX:1") && !info.missing);
    CHECK(info.vertex_count == 8 && info.edge_count == 12);
    OK(cad_parts_get(&p, a2, &info));
    CHECK(!strcmp(info.name, "PTBOX:2") && !strcmp(info.path, "PTBOX.CAD"));
    CHECK(info.tx == -50 && info.ty == 10 && info.tz == 5 && info.yaw == 0);
    OK(cad_parts_get(&p, b1, &info));
    CHECK(!strcmp(info.name, "PTLINE:1"));
    CHECK(info.vertex_count == 2 && info.edge_count == 1);
    OK(cad_parts_at(&p, 2, &bad));
    CHECK(bad == a2);
    CHECK(cad_parts_at(&p, 3, &bad) == CAD_PARTS_NOT_FOUND && bad == a2);
    CHECK(cad_parts_get(&p, 42, &info) == CAD_PARTS_NOT_FOUND);

    col.n = 0;
    OK(cad_parts_segments(&p, 0, collect, &col));
    CHECK(col.n == 25);
    col.n = 0;
    OK(cad_parts_segments(&p, b1, collect, &col));
    CHECK(col.n == 1);
    CHECK(col.seg[0].v[0] == 100 && col.seg[0].v[3] == 200 &&
          col.seg[0].v[4] == 5 && col.seg[0].v[5] == 40);
    CHECK(cad_parts_segments(&p, 77, collect, &col) == CAD_PARTS_NOT_FOUND);
    CHECK(cad_parts_segments(&p, 0, NULL, &col) == CAD_PARTS_ARGUMENT);
    CHECK(cad_parts_bounds(&p, mn, mx));
    CHECK(mn[0] == -50 && mn[1] == 0 && mn[2] == 0);
    CHECK(mx[0] == 200 && mx[1] == 30 && mx[2] == 40);

    /* Removing the first box renumbers the derived instance names. */
    OK(cad_parts_remove(&p, a1));
    OK(cad_parts_get(&p, a2, &info));
    CHECK(!strcmp(info.name, "PTBOX:1"));
    CHECK(used_slots(&p) == 2);
    OK(cad_parts_remove(&p, a2));
    CHECK(used_slots(&p) == 1 && cad_parts_count(&p) == 1);
    CHECK(cad_parts_remove(&p, a2) == CAD_PARTS_NOT_FOUND);

    /* Argument rejection is atomic. */
    snapshot();
    CHECK(cad_parts_place(&p, "PT BOX.CAD", 0, 0, 0, 0, &bad) ==
          CAD_PARTS_ARGUMENT);
    CHECK(cad_parts_place(&p, "", 0, 0, 0, 0, &bad) == CAD_PARTS_ARGUMENT);
    CHECK(cad_parts_place(&p, "PTBOX.CAD", 2001, 0, 0, 0, &bad) ==
          CAD_PARTS_ARGUMENT);
    CHECK(cad_parts_place(&p, "PTBOX.CAD", 0, 0, -2001, 0, &bad) ==
          CAD_PARTS_ARGUMENT);
    CHECK(cad_parts_place(&p, "PTBOX.CAD", 0, 0, 0, 8, &bad) ==
          CAD_PARTS_ARGUMENT);
    CHECK(cad_parts_place(&p, "PTBOX.CAD", 0, 0, 0, -1, &bad) ==
          CAD_PARTS_ARGUMENT);
    CHECK(cad_parts_move(&p, b1, 0, 0, 0, 9) == CAD_PARTS_ARGUMENT);
    CHECK(cad_parts_move(&p, 55, 0, 0, 0, 0) == CAD_PARTS_NOT_FOUND);
    {
        char longpath[140];
        memset(longpath, 'A', 128); longpath[128] = 0;
        CHECK(cad_parts_place(&p, longpath, 0, 0, 0, 0, &bad) ==
              CAD_PARTS_ARGUMENT);
    }
    unchanged();
    CHECK(bad == a2);
}

static void check_yaw(int yaw, long ex, long ez, int exact)
{
    CadAssemblyId id;
    cad_parts_init(&p);
    OK(cad_parts_place(&p, "PTLINE.CAD", 1000, -5, 7, yaw, &id));
    col.n = 0;
    OK(cad_parts_segments(&p, id, collect, &col));
    CHECK(col.n == 1);
    CHECK(col.seg[0].v[0] == 1000 && col.seg[0].v[1] == -5 &&
          col.seg[0].v[2] == 7);
    CHECK(col.seg[0].v[4] == 0);
    if (exact) {
        CHECK(col.seg[0].v[3] == 1000 + ex && col.seg[0].v[5] == 7 + ez);
    } else {
        CHECK(dist(col.seg[0].v[3], 1000 + ex) <= 1);
        CHECK(dist(col.seg[0].v[5], 7 + ez) <= 1);
    }
}

static void transforms(void)
{
    /* Line (0,0,0)-(100,5,40); yaw 2 maps +X to -Z. */
    check_yaw(0, 100, 40, 1);
    check_yaw(2, 40, -100, 1);
    check_yaw(4, -100, -40, 1);
    check_yaw(6, -40, 100, 1);
    /* 45-degree steps: 140/sqrt2 = 98.99, 60/sqrt2 = 42.43. */
    check_yaw(1, 99, -42, 0);
    check_yaw(3, -42, -99, 0);
    check_yaw(5, -99, 42, 0);
    check_yaw(7, 42, 99, 0);
}

static void grounding_missing_and_limits(void)
{
    CadAssemblyId g, m, c, id, ids[CAD_PARTS_MAX];
    unsigned i;

    cad_parts_init(&p);
    OK(cad_parts_place(&p, "PTBOX.CAD", 1, 2, 3, 0, &g));
    OK(cad_parts_ground(&p, g, 1));
    snapshot();
    CHECK(cad_parts_move(&p, g, 5, 5, 5, 1) == CAD_PARTS_GROUNDED);
    unchanged();
    OK(cad_parts_get(&p, g, &info));
    CHECK(info.grounded && info.tx == 1 && info.yaw == 0);
    OK(cad_parts_ground(&p, g, 0));
    OK(cad_parts_move(&p, g, 5, 6, 7, 3));
    OK(cad_parts_get(&p, g, &info));
    CHECK(!info.grounded && info.tx == 5 && info.ty == 6 && info.tz == 7 &&
          info.yaw == 3);
    CHECK(cad_parts_ground(&p, 99, 1) == CAD_PARTS_NOT_FOUND);

    /* Missing part file: placeholder with no geometry, fixed by reload. */
    remove("PTNONE.CAD");
    OK(cad_parts_place(&p, "PTNONE.CAD", 0, 0, 0, 0, &m));
    OK(cad_parts_get(&p, m, &info));
    CHECK(info.missing && info.vertex_count == 0 && info.edge_count == 0);
    CHECK(!strcmp(info.name, "PTNONE:1"));
    col.n = 0;
    OK(cad_parts_segments(&p, m, collect, &col));
    CHECK(col.n == 0);
    save_line("PTNONE.CAD");
    OK(cad_parts_reload(&p));
    OK(cad_parts_get(&p, m, &info));
    CHECK(!info.missing && info.edge_count == 1);
    remove("PTNONE.CAD");
    OK(cad_parts_reload(&p));
    OK(cad_parts_get(&p, m, &info));
    CHECK(info.missing);

    /* Cache: BOX, NONE, LINE, C use all four slots; D is one too many. */
    OK(cad_parts_place(&p, "PTLINE.CAD", 0, 0, 0, 0, &id));
    OK(cad_parts_place(&p, "PTC.CAD", 0, 0, 0, 0, &c));
    CHECK(used_slots(&p) == 4);
    snapshot();
    CHECK(cad_parts_place(&p, "PTD.CAD", 0, 0, 0, 0, &id) ==
          CAD_PARTS_CACHE_FULL);
    unchanged();
    OK(cad_parts_place(&p, "PTBOX.CAD", 0, 0, 0, 0, &id)); /* cached */
    OK(cad_parts_remove(&p, c));
    OK(cad_parts_place(&p, "PTD.CAD", 0, 0, 0, 0, &id));
    CHECK(used_slots(&p) == 4);

    /* Grounded occurrences can still be removed. */
    OK(cad_parts_ground(&p, g, 1));
    OK(cad_parts_remove(&p, g));

    /* Occurrence limit. */
    cad_parts_init(&p);
    for (i = 0; i < CAD_PARTS_MAX; ++i)
        OK(cad_parts_place(&p, "PTBOX.CAD", (long)i * 20L, 0, 0,
                           (int)(i % 8), &ids[i]));
    OK(cad_parts_get(&p, ids[15], &info));
    CHECK(!strcmp(info.name, "PTBOX:16"));
    snapshot();
    CHECK(cad_parts_place(&p, "PTBOX.CAD", 0, 0, 0, 0, &id) ==
          CAD_PARTS_FULL);
    unchanged();
}

static void read_ok(CadParts *x, const char *s)
{
    unsigned line = 77;
    OK(cad_parts_read_text(x, s, (unsigned)strlen(s), &line));
    CHECK(line == 0);
}

static void serialization(void)
{
    CadAssemblyId a, b, c, id;
    unsigned n, n2 = 0, line;
    static const char expect[] =
        "N 5\n"
        "O 2 3 1 -2000 0 2000 PTLINE.CAD\n"
        "O 3 0 0 7 -8 9 PTNONE.CAD\n"
        "O 4 6 0 0 0 0 PTBOX.CAD\n";

    remove("PTNONE.CAD");
    cad_parts_init(&p);
    OK(cad_parts_place(&p, "PTBOX.CAD", 1, 1, 1, 0, &a));
    OK(cad_parts_place(&p, "PTLINE.CAD", -2000, 0, 2000, 3, &b));
    OK(cad_parts_place(&p, "PTNONE.CAD", 7, -8, 9, 0, &c));
    OK(cad_parts_place(&p, "PTBOX.CAD", 0, 0, 0, 6, &id));
    OK(cad_parts_ground(&p, b, 1));
    OK(cad_parts_remove(&p, a));
    OK(cad_parts_write_text(&p, text, sizeof(text), &n));
    CHECK(n == strlen(expect) && !memcmp(text, expect, n));
    n2 = 5;
    CHECK(cad_parts_write_text(&p, text2, n - 1, &n2) == CAD_PARTS_CAPACITY);
    CHECK(n2 == 5);

    col.n = 0;
    OK(cad_parts_segments(&p, 0, collect, &col));
    CHECK(col.n == 13);
    cad_parts_init(&q);
    text[n] = 0;
    read_ok(&q, text);
    OK(cad_parts_write_text(&q, text2, sizeof(text2), &n2));
    CHECK(n2 == n && !memcmp(text, text2, n));
    OK(cad_parts_get(&q, id, &info));
    CHECK(!strcmp(info.name, "PTBOX:1") && info.yaw == 6 && !info.missing);
    CHECK(info.vertex_count == 8);
    OK(cad_parts_get(&q, c, &info));
    CHECK(info.missing && info.tx == 7 && info.ty == -8 && info.tz == 9);
    OK(cad_parts_get(&q, b, &info));
    CHECK(info.grounded && info.yaw == 3 && info.tx == -2000);
    CHECK(cad_parts_move(&q, b, 0, 0, 0, 0) == CAD_PARTS_GROUNDED);
    CHECK(used_slots(&q) == 3);
    /* IDs continue from N, never recycled. */
    OK(cad_parts_place(&q, "PTBOX.CAD", 0, 0, 0, 0, &id));
    CHECK(id == 5);
    /* Same geometry as the original (part models are process-wide, so
     * only the most recently read CadParts is queried). */
    col.n = 0;
    OK(cad_parts_segments(&q, 0, collect, &col));
    CHECK(col.n == 13 + 12);

    /* Empty text is the initial state. */
    line = 9;
    OK(cad_parts_read_text(&q, "", 0, &line));
    CHECK(cad_parts_count(&q) == 0 && line == 0);
    OK(cad_parts_write_text(&q, text2, sizeof(text2), &n2));
    CHECK(n2 == 4 && !memcmp(text2, "N 1\n", 4));
}

static void reject(const char *s, unsigned expect_line, int expect_status)
{
    unsigned line = 1000;
    snapshot();
    CHECK(cad_parts_read_text(&p, s, (unsigned)strlen(s), &line) ==
          expect_status);
    if (line != expect_line)
        fprintf(stderr, "reject %s: line %u\n", s, line);
    CHECK(line == expect_line);
    unchanged();
}

static void malformed(void)
{
    unsigned i, n;
    CadAssemblyId id;

    cad_parts_init(&p);
    OK(cad_parts_place(&p, "PTBOX.CAD", 3, 4, 5, 1, &id));
    reject("N 2", 1, CAD_PARTS_FORMAT);
    reject("O 1 0 0 0 0 0 PTBOX.CAD\n", 1, CAD_PARTS_FORMAT);
    reject("N 0\n", 1, CAD_PARTS_FORMAT);
    reject("N 02\n", 1, CAD_PARTS_FORMAT);
    reject("N 65537\n", 1, CAD_PARTS_FORMAT);
    reject("N  2\n", 1, CAD_PARTS_FORMAT);
    reject("N 2 \n", 1, CAD_PARTS_FORMAT);
    reject("N 2\r\n", 1, CAD_PARTS_FORMAT);
    reject("N 2\nN 2\n", 2, CAD_PARTS_FORMAT);
    reject("N 2\nX\n", 2, CAD_PARTS_FORMAT);
    reject("N 2\n\n", 2, CAD_PARTS_FORMAT);
    reject("N 2\nO 2 0 0 0 0 0 PTBOX.CAD\n", 2, CAD_PARTS_FORMAT);
    reject("N 3\nO 0 0 0 0 0 0 PTBOX.CAD\n", 2, CAD_PARTS_FORMAT);
    reject("N 3\nO 1 8 0 0 0 0 PTBOX.CAD\n", 2, CAD_PARTS_FORMAT);
    reject("N 3\nO 1 0 2 0 0 0 PTBOX.CAD\n", 2, CAD_PARTS_FORMAT);
    reject("N 3\nO 1 0 0 2001 0 0 PTBOX.CAD\n", 2, CAD_PARTS_FORMAT);
    reject("N 3\nO 1 0 0 -0 0 0 PTBOX.CAD\n", 2, CAD_PARTS_FORMAT);
    reject("N 3\nO 1 0 0 +1 0 0 PTBOX.CAD\n", 2, CAD_PARTS_FORMAT);
    reject("N 3\nO 1 0 0 0 0 0\n", 2, CAD_PARTS_FORMAT);
    reject("N 3\nO 1 0 0 0 0 0 \n", 2, CAD_PARTS_FORMAT);
    reject("N 3\nO 1 0 0 0 0 0 PT BOX.CAD\n", 2, CAD_PARTS_FORMAT);
    reject("N 3\nO 1 0 0 0 0  0 PTBOX.CAD\n", 2, CAD_PARTS_FORMAT);
    reject("N 3\nO 1 0 0 0 0 0 PTBOX.CAD\nO 1 0 0 0 0 0 PTBOX.CAD\n", 3,
           CAD_PARTS_FORMAT);
    reject("N 9\nO 1 0 0 0 0 0 A.CAD\nO 2 0 0 0 0 0 B.CAD\n"
           "O 3 0 0 0 0 0 A.CAD\nO 4 0 0 0 0 0 C.CAD\n"
           "O 5 0 0 0 0 0 D.CAD\nO 6 0 0 0 0 0 E.CAD\n", 7,
           CAD_PARTS_CACHE_FULL);
    reject("N 3\nO 1 0 0 0 0 0 PTBOX.CAD\nO 2 0 0 0 0 0 PTBOX.CAD", 3,
           CAD_PARTS_FORMAT);
    {
        char path[140];
        memset(path, 0, sizeof(path));
        sprintf(big, "N 2\nO 1 0 0 0 0 0 ");
        memset(path, 'A', 128);
        strcat(big, path);
        strcat(big, "\n");
        reject(big, 2, CAD_PARTS_FORMAT);
        path[127] = 0; /* 127 bytes is fine */
        sprintf(big, "N 2\nO 1 0 0 0 0 0 %s\n", path);
        cad_parts_init(&q);
        read_ok(&q, big);
        OK(cad_parts_get(&q, 1, &info));
        CHECK(info.missing && strlen(info.path) == 127);
        CHECK(!strcmp(info.name, "AAAAAAAAAAAA:1"));
    }
    /* 17 occurrences. */
    n = (unsigned)sprintf(big, "N 100\n");
    for (i = 1; i <= 17; ++i)
        n += (unsigned)sprintf(big + n, "O %u 0 0 0 0 0 PTBOX.CAD\n", i);
    reject(big, 18, CAD_PARTS_FULL);
    CHECK(cad_parts_read_text(&p, NULL, 1, NULL) == CAD_PARTS_ARGUMENT);
    unchanged();
}

int main(void)
{
    cleanup();
    save_box("PTBOX.CAD");
    save_line("PTLINE.CAD");
    place_and_names();
    transforms();
    grounding_missing_and_limits();
    serialization();
    malformed();
    cleanup();
    printf("parts_test: PASS\n");
    return 0;
}
