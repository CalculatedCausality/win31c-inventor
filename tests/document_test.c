/* Strict host: cc -std=c89 -pedantic -Wall -Wextra -Werror
 * tests/document_test.c cad_model.c -o /tmp/opencode/document_test
 * Run in an empty scratch directory. Include implementation solely for
 * deterministic I/O faults; production exposes no injection hooks.
 * Win16 module: wcc -q -bt=windows -zW -ml -zt=1 -2 -ox -w4 -we
 * -fo=/tmp/opencode/cad_document.obj cad_document.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include "../cad_document.h"

#if defined(__unix__) || defined(__APPLE__)
extern int symlink(const char *, const char *);
#endif

static int close_fail, write_fail, flush_fail, read_fail, error_fail;
static int rename_calls, rename_fail, rollback_fail, remove_fail;
static int test_close(FILE *f)
{
    int rc = fclose(f);
    if (close_fail) { close_fail = 0; return EOF; }
    return rc;
}
static size_t test_write(const void *p, size_t size, size_t n, FILE *f)
{
    if (write_fail) {
        write_fail = 0;
        return fwrite(p, size, n / 2, f);
    }
    return fwrite(p, size, n, f);
}
static size_t test_read(void *p, size_t size, size_t n, FILE *f)
{
    if (read_fail) { read_fail = 0; error_fail = 1; return 0; }
    return fread(p, size, n, f);
}
static int test_error(FILE *f)
{
    if (error_fail) { error_fail = 0; return 1; }
    return ferror(f);
}
static int test_flush(FILE *f)
{
    int rc = fflush(f);
    if (flush_fail) { flush_fail = 0; return EOF; }
    return rc;
}
static int test_rename(const char *from, const char *to)
{
    ++rename_calls;
    if (rename_calls == rename_fail || (rollback_fail && rename_calls == 3)) {
        errno = EACCES; return -1;
    }
    return rename(from, to);
}
static int test_remove(const char *path)
{
    if (remove_fail && !strcmp(path, "GRAPH.BAK")) {
        errno = EACCES; return -1;
    }
    return remove(path);
}
#define fclose test_close
#define fwrite test_write
#define fread test_read
#ifdef ferror
#undef ferror
#endif
#define ferror test_error
#define fflush test_flush
#define rename test_rename
#define remove test_remove
#include "../cad_document.c"
#undef fclose
#undef fwrite
#undef fread
#undef ferror
#undef fflush
#undef rename
#undef remove

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

/* Caller allocated, never on the Win16 stack. */
static CadModel input, live, stage, before;
static unsigned char good[CAD_DOC_WIRE_MAX], bad[CAD_DOC_WIRE_MAX + 1];
static size_t good_size;

static void put16(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 255UL);
    p[1] = (unsigned char)((v >> 8) & 255UL);
}
/* Independent bit-at-a-time reference, no production helpers. */
static void seal(unsigned char *p, size_t n)
{
    unsigned long crc = 65535UL;
    size_t i;
    int bit, feedback;
    for (i = 0; i < n - 2; ++i) {
        for (bit = 7; bit >= 0; --bit) {
            feedback = (int)((crc >> 15) & 1UL) ^ ((p[i] >> bit) & 1);
            crc = (crc << 1) & 65535UL;
            if (feedback) crc ^= 0x1021UL;
        }
    }
    put16(p + n - 2, crc);
}
static void bytes_save(const char *path, const unsigned char *p, size_t n)
{
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL); CHECK(fwrite(p, 1, n, f) == n);
    CHECK(fclose(f) == 0);
}
static size_t bytes_load(const char *path, unsigned char *p)
{
    FILE *f = fopen(path, "rb");
    size_t n;
    CHECK(f != NULL); n = fread(p, 1, CAD_DOC_WIRE_MAX, f);
    CHECK(fgetc(f) == EOF && !ferror(f)); CHECK(fclose(f) == 0);
    return n;
}
static void same_geometry(const CadModel *a, const CadModel *b)
{
    unsigned int i;
    CHECK(a->vertex_count == b->vertex_count && a->edge_count == b->edge_count);
    CHECK(a->units == b->units && a->next_vertex_id == b->next_vertex_id &&
          a->next_edge_id == b->next_edge_id);
    for (i = 0; i < a->vertex_count; ++i) {
        CHECK(a->vertices[i].id == b->vertices[i].id);
        CHECK(a->vertices[i].x == b->vertices[i].x &&
              a->vertices[i].y == b->vertices[i].y &&
              a->vertices[i].z == b->vertices[i].z);
    }
    for (i = 0; i < a->edge_count; ++i)
        CHECK(a->edges[i].id == b->edges[i].id &&
              a->edges[i].a == b->edges[i].a && a->edges[i].b == b->edges[i].b);
}
static void rejected(int status, size_t n, int reseal)
{
    if (reseal) seal(bad, n);
    bytes_save("BAD.CAD", bad, n);
    memcpy(&before, &live, sizeof(live));
    CHECK(cad_document_load("BAD.CAD", &live, &stage) == status);
    CHECK(!memcmp(&before, &live, sizeof(live)));
}
static void still_old(void)
{
    size_t n = bytes_load("GRAPH.CAD", bad);
    CHECK(n == good_size && !memcmp(bad, good, n));
}
static void roundtrip(void)
{
    FILE *f = tmpfile();
    CHECK(f != NULL);
    CHECK(cad_document_write(f, &input) == CAD_DOC_OK);
    rewind(f);
    memset(&live, 0xa5, sizeof(live)); memset(&stage, 0x5a, sizeof(stage));
    CHECK(cad_document_read(f, &live, &stage) == CAD_DOC_OK);
    CHECK(fclose(f) == 0); same_geometry(&input, &live);
    CHECK(!live.history_count && !live.history_cursor && !live.history_used &&
          !live.staged_count && !live.transaction_active);
    CHECK(cad_model_undo(&live) == CAD_NO_HISTORY);
    CHECK(cad_model_redo(&live) == CAD_NO_HISTORY);
}
static int make_directory(const char *path)
{
#ifdef __WATCOMC__
    return mkdir(path);
#else
    return mkdir(path, 0700);
#endif
}
static void arbitrary_graph(void)
{
    CadId a, b, c, d, edge;
    CHECK(cad_model_init(&input) == CAD_OK);
    CHECK(cad_model_begin(&input) == CAD_OK);
    CHECK(cad_model_add_vertex(&input, -400, 400, 0, &a) == CAD_OK);
    CHECK(cad_model_add_vertex(&input, 123, -17, 9, &b) == CAD_OK);
    CHECK(cad_model_add_vertex(&input, 0, -400, 400, &c) == CAD_OK);
    CHECK(cad_model_add_vertex(&input, 5, 6, 7, &d) == CAD_OK);
    CHECK(cad_model_add_edge(&input, a, b, &edge) == CAD_OK);
    CHECK(cad_model_add_edge(&input, b, c, &edge) == CAD_OK);
    CHECK(cad_model_add_edge(&input, c, a, &edge) == CAD_OK);
    CHECK(cad_model_add_edge(&input, a, b, &edge) == CAD_OK);
    CHECK(cad_model_add_edge(&input, d, d, &edge) == CAD_OK);
    CHECK(cad_model_set_units(&input, CAD_UNITS_INCHES) == CAD_OK);
    CHECK(cad_model_commit(&input) == CAD_OK);
    /* Non-dense, >32767 IDs and independent namespaces. */
    input.vertices[0].id = 65535U; input.vertices[1].id = 500U;
    input.vertices[2].id = 40000U; input.vertices[3].id = 42U;
    input.edges[0].id = 65535U; input.edges[0].a = 65535U; input.edges[0].b = 500U;
    input.edges[1].a = 500U; input.edges[1].b = 40000U;
    input.edges[2].a = 40000U; input.edges[2].b = 65535U;
    input.edges[3].a = 65535U; input.edges[3].b = 500U;
    input.edges[4].a = 42U; input.edges[4].b = 42U;
    input.next_vertex_id = input.next_edge_id = 65536L;
}

int main(void)
{
    unsigned int i;
    size_t n;
    FILE *f;
    CadId id;
    char path[140];
    CHECK(doc_state("GRAPH.CAD") == 0 && doc_state("GRAPH.TMP") == 0 &&
          doc_state("GRAPH.BAK") == 0 && doc_state("BAD.CAD") == 0);
    arbitrary_graph(); roundtrip();
    CHECK(cad_document_save("GRAPH.CAD", &input) == CAD_DOC_OK);
    good_size = bytes_load("GRAPH.CAD", good);
    CHECK(good_size == 24 + 4 * 8 + 5 * 6 + 2);
    CHECK(!memcmp(good, "CADG\001\000\002\000\004\000\005\000", 12));
    CHECK(good[12] == 0 && good[13] == 0 && good[14] == 1 && good[15] == 0);
    CHECK(good[24] == 255 && good[25] == 255 &&
          good[26] == 0x70 && good[27] == 0xfe &&
          good[28] == 0x90 && good[29] == 1);
    memcpy(bad, good, good_size); seal(bad, good_size);
    CHECK(!memcmp(bad, good, good_size));
    /* Every truncation, trailing bytes, wrong CRC, magic and version. */
    for (n = 0; n < good_size; ++n) {
        memcpy(bad, good, good_size); rejected(CAD_DOC_FORMAT, n, 0);
    }
    memcpy(bad, good, good_size); bad[good_size] = 0;
    rejected(CAD_DOC_FORMAT, good_size + 1, 0);
    memcpy(bad, good, good_size); bad[26] ^= 1;
    rejected(CAD_DOC_FORMAT, good_size, 0);
    memcpy(bad, good, good_size); bad[0] = 'X';
    rejected(CAD_DOC_FORMAT, good_size, 1);
    memcpy(bad, good, good_size); put16(bad + 4, 2);
    rejected(CAD_DOC_FORMAT, good_size, 1);
    /* Valid CRC cannot conceal semantic/length failures. */
    memcpy(bad, good, good_size); put16(bad + 6, 3);
    rejected(CAD_DOC_RANGE, good_size, 1);
    memcpy(bad, good, good_size); put16(bad + 8, 257);
    rejected(CAD_DOC_RANGE, good_size, 1);
    memcpy(bad, good, good_size); put16(bad + 10, 513);
    rejected(CAD_DOC_RANGE, good_size, 1);
    memcpy(bad, good, good_size); bad[20]++;
    rejected(CAD_DOC_FORMAT, good_size, 1);
    memcpy(bad, good, good_size); memset(bad + 12, 0, 4);
    rejected(CAD_DOC_RANGE, good_size, 1);
    memcpy(bad, good, good_size); bad[12] = 1;
    rejected(CAD_DOC_RANGE, good_size, 1);
    memcpy(bad, good, good_size); put16(bad + 12, 65535); bad[14] = 0;
    rejected(CAD_DOC_RANGE, good_size, 1);
    memcpy(bad, good, good_size); put16(bad + 16, 65535); bad[18] = 0;
    rejected(CAD_DOC_RANGE, good_size, 1);
    memcpy(bad, good, good_size); put16(bad + 24, 0);
    rejected(CAD_DOC_FORMAT, good_size, 1);
    memcpy(bad, good, good_size); put16(bad + 32, 65535);
    rejected(CAD_DOC_FORMAT, good_size, 1);
    memcpy(bad, good, good_size); put16(bad + 62, 65535);
    rejected(CAD_DOC_FORMAT, good_size, 1);
    memcpy(bad, good, good_size); put16(bad + 58, 999);
    rejected(CAD_DOC_FORMAT, good_size, 1);
    for (i = 0; i < 2; ++i) {
        memcpy(bad, good, good_size); put16(bad + 26, i ? 65135UL : 401UL);
        rejected(CAD_DOC_RANGE, good_size, 1);
    }
    memcpy(&before, &live, sizeof(live)); close_fail = 1;
    CHECK(cad_document_load("GRAPH.CAD", &live, &stage) == CAD_DOC_IO);
    CHECK(!memcmp(&before, &live, sizeof(live)));
    read_fail = 1;
    CHECK(cad_document_load("GRAPH.CAD", &live, &stage) == CAD_DOC_IO);
    CHECK(!memcmp(&before, &live, sizeof(live)));
    CHECK(cad_document_read(NULL, &live, &stage) == CAD_DOC_ARG);
    f = tmpfile(); CHECK(f != NULL);
    CHECK(cad_document_read(f, &live, &live) == CAD_DOC_ARG);
    CHECK(cad_document_read(f, NULL, &stage) == CAD_DOC_ARG);
    CHECK(cad_document_write(f, NULL) == CAD_DOC_ARG);
    input.vertices[0].x = 401;
    CHECK(cad_document_write(f, &input) == CAD_DOC_RANGE && ftell(f) == 0);
    input.vertices[0].x = -400; input.transaction_active = 1;
    CHECK(cad_document_write(f, &input) == CAD_DOC_ARG && ftell(f) == 0);
    input.transaction_active = 0; CHECK(fclose(f) == 0);
    CHECK(cad_document_write(NULL, &input) == CAD_DOC_ARG);
    CHECK(cad_document_load(NULL, &live, &stage) == CAD_DOC_ARG);
    CHECK(cad_document_load("GRAPH.CAD", &live, &live) == CAD_DOC_ARG);
    CHECK(cad_document_load("MISSING.CAD", &live, &stage) == CAD_DOC_IO);
    CHECK(cad_document_save(NULL, &input) == CAD_DOC_ARG);
    CHECK(cad_document_save("GRAPH.CAD", NULL) == CAD_DOC_ARG);
    CHECK(cad_document_save("CON.CAD", &input) == CAD_DOC_ARG);
    CHECK(cad_document_save("LPT1.CAD", &input) == CAD_DOC_ARG);
    CHECK(cad_document_save("a..CAD", &input) == CAD_DOC_ARG);
    CHECK(cad_document_save("", &input) == CAD_DOC_ARG);
    CHECK(cad_document_save("TOOLONG99.CAD", &input) == CAD_DOC_ARG);
    CHECK(cad_document_save("GRAPH.tmp", &input) == CAD_DOC_COLLISION);
    CHECK(cad_document_save("GRAPH.bAk", &input) == CAD_DOC_COLLISION);
    memset(path, 'X', sizeof(path)); path[139] = 0;
    CHECK(cad_document_save(path, &input) == CAD_DOC_ARG);
    memset(path, 'X', 125); path[125] = '/'; path[126] = 'A'; path[127] = 0;
    CHECK(cad_document_save(path, &input) == CAD_DOC_ARG);
    bytes_save("GRAPH.TMP", good, good_size);
    CHECK(cad_document_save("GRAPH.CAD", &input) == CAD_DOC_COLLISION);
    CHECK(bytes_load("GRAPH.TMP", bad) == good_size && !memcmp(good, bad, good_size));
    CHECK(remove("GRAPH.TMP") == 0);
    bytes_save("GRAPH.BAK", good, good_size);
    CHECK(cad_document_save("GRAPH.CAD", &input) == CAD_DOC_COLLISION); still_old();
    CHECK(bytes_load("GRAPH.BAK", bad) == good_size && !memcmp(good, bad, good_size));
    CHECK(remove("GRAPH.BAK") == 0);
    CHECK(make_directory("GRAPH.TMP") == 0);
    CHECK(cad_document_save("GRAPH.CAD", &input) == CAD_DOC_COLLISION);
    CHECK(remove("GRAPH.TMP") == 0);
    CHECK(make_directory("GRAPH.BAK") == 0);
    CHECK(cad_document_save("GRAPH.CAD", &input) == CAD_DOC_COLLISION);
    CHECK(remove("GRAPH.BAK") == 0);
    CHECK(make_directory("DIR.CAD") == 0);
    CHECK(cad_document_save("DIR.CAD", &input) == CAD_DOC_COLLISION);
    CHECK(cad_document_load("DIR.CAD", &live, &stage) == CAD_DOC_IO);
    CHECK(remove("DIR.CAD") == 0);
#if defined(__unix__) || defined(__APPLE__)
    /* A dangling link must never be followed by fopen("wb"). */
    CHECK(symlink("VICTIM.CAD", "GRAPH.TMP") == 0);
    CHECK(cad_document_save("GRAPH.CAD", &input) == CAD_DOC_COLLISION);
    CHECK(doc_state("VICTIM.CAD") == 0);
    CHECK(remove("GRAPH.TMP") == 0);
    CHECK(symlink("GRAPH.CAD", "LINK.CAD") == 0);
    CHECK(cad_document_save("LINK.CAD", &input) == CAD_DOC_COLLISION);
    still_old(); CHECK(remove("LINK.CAD") == 0);
#endif
    input.vertices[0].x = 100;
    close_fail = 1;
    CHECK(cad_document_save("GRAPH.CAD", &input) == CAD_DOC_IO); still_old();
    CHECK(doc_state("GRAPH.TMP") == 0);
    write_fail = 1;
    CHECK(cad_document_save("GRAPH.CAD", &input) == CAD_DOC_IO); still_old();
    CHECK(doc_state("GRAPH.TMP") == 0);
    flush_fail = 1;
    CHECK(cad_document_save("GRAPH.CAD", &input) == CAD_DOC_IO); still_old();
    CHECK(doc_state("GRAPH.TMP") == 0);
    rename_calls = 0; rename_fail = 1;
    CHECK(cad_document_save("GRAPH.CAD", &input) == CAD_DOC_IO); still_old();
    CHECK(doc_state("GRAPH.TMP") == 0 && doc_state("GRAPH.BAK") == 0);
    rename_calls = 0; rename_fail = 2;
    CHECK(cad_document_save("GRAPH.CAD", &input) == CAD_DOC_IO); still_old();
    CHECK(doc_state("GRAPH.TMP") == 1 && doc_state("GRAPH.BAK") == 0);
    CHECK(cad_document_load("GRAPH.CAD", &live, &stage) == CAD_DOC_OK);
    CHECK(live.vertices[0].x == -400);
    CHECK(remove("GRAPH.TMP") == 0);
    rename_calls = 0; rollback_fail = 1;
    CHECK(cad_document_save("GRAPH.CAD", &input) == CAD_DOC_IO);
    CHECK(doc_state("GRAPH.CAD") == 0 && doc_state("GRAPH.TMP") == 1 &&
          doc_state("GRAPH.BAK") == 1);
    CHECK(bytes_load("GRAPH.BAK", bad) == good_size && !memcmp(good, bad, good_size));
    CHECK(rename("GRAPH.BAK", "GRAPH.CAD") == 0);
    CHECK(remove("GRAPH.TMP") == 0);
    rename_fail = rollback_fail = 0; remove_fail = 1;
    CHECK(cad_document_save("GRAPH.CAD", &input) == CAD_DOC_IO);
    CHECK(bytes_load("GRAPH.BAK", bad) == good_size && !memcmp(good, bad, good_size));
    CHECK(cad_document_load("GRAPH.CAD", &live, &stage) == CAD_DOC_OK);
    CHECK(live.vertices[0].x == 100);
    CHECK(cad_document_save("GRAPH.CAD", &input) == CAD_DOC_COLLISION);
    remove_fail = 0; CHECK(remove("GRAPH.BAK") == 0);
    rename_calls = 0; rename_fail = 1;
    CHECK(cad_document_save("FIRST.CAD", &input) == CAD_DOC_IO);
    CHECK(doc_state("FIRST.CAD") == 0 && doc_state("FIRST.TMP") == 1);
    CHECK(remove("FIRST.TMP") == 0); rename_fail = 0;
    /* Empty, single line, maximum capacities, and allocator preservation. */
    CHECK(cad_model_init(&input) == CAD_OK); roundtrip();
    input.next_vertex_id = 65536L; input.next_edge_id = 77L;
    input.units = CAD_UNITS_MM; roundtrip();
    CHECK(cad_model_init(&input) == CAD_OK);
    input.vertex_count = 2; input.edge_count = 1;
    input.vertices[0].id = 9; input.vertices[1].id = 20;
    input.edges[0].id = 300; input.edges[0].a = 9; input.edges[0].b = 20;
    input.next_vertex_id = 100L; input.next_edge_id = 301L;
    roundtrip(); CHECK(cad_model_begin(&live) == CAD_OK);
    CHECK(cad_model_add_vertex(&live, 1, 2, 3, &id) == CAD_OK && id == 100);
    CHECK(cad_model_cancel(&live) == CAD_OK);
    CHECK(cad_model_init(&input) == CAD_OK);
    input.vertex_count = CAD_MAX_VERTICES; input.edge_count = CAD_MAX_EDGES;
    for (i = 0; i < CAD_MAX_VERTICES; ++i) {
        input.vertices[i].id = (CadId)(i + 1);
        input.vertices[i].x = i % 2 ? 400L : -400L;
    }
    for (i = 0; i < CAD_MAX_EDGES; ++i) {
        input.edges[i].id = (CadId)(i + 1);
        input.edges[i].a = (CadId)(i % CAD_MAX_VERTICES + 1);
        input.edges[i].b = (CadId)((i + 1) % CAD_MAX_VERTICES + 1);
    }
    input.next_vertex_id = 257L; input.next_edge_id = 513L;
    roundtrip(); CHECK(cad_document_save("GRAPH.CAD", &input) == CAD_DOC_OK);
    CHECK(bytes_load("GRAPH.CAD", bad) == CAD_DOC_WIRE_MAX);
    CHECK(cad_document_load("GRAPH.CAD", &live, &stage) == CAD_DOC_OK);
    same_geometry(&input, &live);
    CHECK(doc_state("GRAPH.TMP") == 0 && doc_state("GRAPH.BAK") == 0);
    CHECK(remove("GRAPH.CAD") == 0); CHECK(remove("BAD.CAD") == 0);
    puts("document tests passed (graph, wire, atomic publication, recovery)");
    return 0;
}
