#include "../cad_script.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CadModel live;
static CadModel stage;
static CadModel saved;
static CadScriptResult r;
static unsigned long checks;
static char big[16384];

#define CHECK(e) do { ++checks; if (!(e)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #e); exit(1); \
} } while (0)
#define OK(e) CHECK((e) == CAD_OK)
#define SAVE() memcpy(&saved, &live, sizeof(live))
#define SAME() CHECK(memcmp(&saved, &live, sizeof(live)) == 0)

static CadScriptStatus run(const char *text)
{
    return cad_script_run(text, (unsigned int)strlen(text), &live, &stage,
                          0, 0, &r);
}

typedef struct Params { int calls; } Params;

static int lookup(void *context, const char *name, long *value)
{
    Params *p = (Params *)context;
    ++p->calls;
    if (strcmp(name, "len") == 0) { *value = 50; return 1; }
    if (strcmp(name, "A") == 0) { *value = 300; return 1; }
    if (strcmp(name, "huge") == 0) { *value = 2000000000L; return 1; }
    return 0;
}

static CadScriptStatus run_with(const char *text, void *context)
{
    return cad_script_run(text, (unsigned int)strlen(text), &live, &stage,
                          lookup, context, &r);
}

/* Failure must report status + line and leave live byte-identical. */
static void fails(const char *text, CadScriptStatus status,
                  unsigned long line)
{
    CadScriptStatus got;
    SAVE();
    got = run(text);
    if (got != status)
        fprintf(stderr, "status got %d want %d line %lu (%s) for: %.60s\n",
                (int)got, (int)status, r.line, r.message, text);
    CHECK(got == status);
    CHECK(r.status == status);
    if (r.line != line)
        fprintf(stderr, "line got %lu want %lu (%s)\n", r.line, line,
                r.message);
    CHECK(r.line == line);
    CHECK(r.message[0] != '\0');
    CHECK(strlen(r.message) < CAD_SCRIPT_MESSAGE_MAX);
    CHECK(r.history_added == 0);
    SAME();
}

static void reset(void)
{
    OK(cad_model_init(&live));
}

static int has_vertex(long x, long y, long z)
{
    int i;
    for (i = 0; i < live.vertex_count; ++i)
        if (live.vertices[i].x == x && live.vertices[i].y == y &&
            live.vertices[i].z == z) return 1;
    return 0;
}

static long eval(const char *e)
{
    long v = -12345;
    Params p;
    p.calls = 0;
    CHECK(cad_script_eval(e, lookup, &p, &v) == CAD_SCRIPT_OK);
    return v;
}

static void eval_fails(const char *e, CadScriptStatus status)
{
    long v = 777;
    Params p;
    p.calls = 0;
    CHECK(cad_script_eval(e, lookup, &p, &v) == status);
    CHECK(v == 777);
}

static void expressions(void)
{
    CHECK(eval("7") == 7);
    CHECK(eval("007") == 7);
    CHECK(eval("2+3*4") == 14);
    CHECK(eval("(2+3)*4") == 20);
    CHECK(eval("10-4-3") == 3);
    CHECK(eval("100/10/5") == 2);
    CHECK(eval("2*3+4*5") == 26);
    CHECK(eval("-3*-4") == 12);
    CHECK(eval("--5") == 5);
    CHECK(eval("+5-+2") == 3);
    CHECK(eval("-(2+3)*2") == -10);
    CHECK(eval("7/2") == 3);
    CHECK(eval("-7/2") == -3);
    CHECK(eval("7/-2") == -3);
    CHECK(eval("-7/-2") == 3);
    CHECK(eval("len*2+A") == 400);
    CHECK(eval("((((1))))") == 1);
    CHECK(eval("1000000000") == 1000000000L);
    CHECK(eval("-1000000000") == -1000000000L);
    CHECK(eval("500000000+500000000") == 1000000000L);
    eval_fails("", CAD_SCRIPT_EXPRESSION);
    eval_fails("1+", CAD_SCRIPT_EXPRESSION);
    eval_fails("(1+2", CAD_SCRIPT_EXPRESSION);
    eval_fails("1+2)", CAD_SCRIPT_EXPRESSION);
    eval_fails("1 2", CAD_SCRIPT_EXPRESSION);
    eval_fails("3x", CAD_SCRIPT_EXPRESSION);
    eval_fails("2**3", CAD_SCRIPT_EXPRESSION);
    eval_fails("1.5", CAD_SCRIPT_EXPRESSION);
    eval_fails("abcdefghijklmnop", CAD_SCRIPT_EXPRESSION);
    eval_fails("((((((((((((((((((((((((((1))))))))))))))))))))))))))",
               CAD_SCRIPT_EXPRESSION);
    eval_fails("nope", CAD_SCRIPT_UNKNOWN_NAME);
    eval_fails("1/0", CAD_SCRIPT_DIVIDE_BY_ZERO);
    eval_fails("5/(len-50)", CAD_SCRIPT_DIVIDE_BY_ZERO);
    eval_fails("1000000001", CAD_SCRIPT_OVERFLOW);
    eval_fails("99999999999999999999", CAD_SCRIPT_OVERFLOW);
    eval_fails("100000*100000", CAD_SCRIPT_OVERFLOW);
    eval_fails("600000000+600000000", CAD_SCRIPT_OVERFLOW);
    eval_fails("huge", CAD_SCRIPT_OVERFLOW);
    CHECK(cad_script_eval(0, 0, 0, 0) == CAD_SCRIPT_ARGUMENT);
}

static void commands(void)
{
    CadVertex v;
    CadId first;
    reset();
    CHECK(eval("(((((((((((((((1)))))))))))))))") == 1);
    /* Header-only and comment/blank handling. */
    CHECK(run("# leading comment\n\n   \t\nCADSCRIPT 1\n") == CAD_SCRIPT_OK);
    CHECK(r.commands == 0 && r.history_added == 0 && r.line == 0);
    CHECK(r.message[0] == '\0');
    CHECK(live.vertex_count == 0);

    CHECK(run("CADSCRIPT 1\nUNITS MM\n") == CAD_SCRIPT_OK);
    CHECK(live.units == CAD_UNITS_MM && r.history_added == 1);
    CHECK(run("CADSCRIPT 1\nUNITS MM\n") == CAD_SCRIPT_OK);
    CHECK(r.commands == 1 && r.history_added == 0);   /* net no-op */
    CHECK(run("CADSCRIPT 1\nUNITS NONE\n") == CAD_SCRIPT_OK);
    CHECK(live.units == CAD_UNITS_UNSPECIFIED && r.history_added == 1);

    reset();
    CHECK(run("CADSCRIPT 1\nLINE -10 0 0 10 20 30\n") == CAD_SCRIPT_OK);
    CHECK(live.vertex_count == 2 && live.edge_count == 1);
    CHECK(has_vertex(-10, 0, 0) && has_vertex(10, 20, 30));
    CHECK(r.history_added == 1 && !r.history_truncated);

    reset();
    CHECK(run("CADSCRIPT 1\nRECT 0 0 5 100 50\n") == CAD_SCRIPT_OK);
    CHECK(live.vertex_count == 4 && live.edge_count == 4);
    CHECK(has_vertex(100, 50, 5));

    reset();
    CHECK(run("CADSCRIPT 1\nBOX 1 2 3 10 20 30\n") == CAD_SCRIPT_OK);
    CHECK(live.vertex_count == 8 && live.edge_count == 12);
    CHECK(has_vertex(11, 22, 33) && has_vertex(1, 2, 3));

    reset();
    CHECK(run("CADSCRIPT 1\nEXTRUDE 40 0,0 100,0 100,50\n") == CAD_SCRIPT_OK);
    CHECK(live.vertex_count == 6 && live.edge_count == 9);
    CHECK(has_vertex(100, 50, 40));
    reset();
    CHECK(run("CADSCRIPT 1\nEXTRUDE 10 0,0 10,0 20,0 30,0 40,0 50,0 60,0 70,0"
              " 80,0 90,0 100,0 50,(-50)\n") == CAD_SCRIPT_OK);
    CHECK(live.vertex_count == 24);

    /* MOVEV is absolute, DELV removes incident edges, DELE removes edge. */
    reset();
    CHECK(run("CADSCRIPT 1\nLINE 0 0 0 10 0 0\n") == CAD_SCRIPT_OK);
    first = live.vertices[0].id;
    CHECK(first == 1);
    CHECK(run("CADSCRIPT 1\nMOVEV 1 5 6 7\nMOVEV 1 5 6 7\n") == CAD_SCRIPT_OK);
    OK(cad_model_get_vertex(&live, 1, &v));
    CHECK(v.x == 5 && v.y == 6 && v.z == 7);
    CHECK(r.history_added == 1 && r.commands == 2);
    CHECK(run("CADSCRIPT 1\nDELE 1\n") == CAD_SCRIPT_OK);
    CHECK(live.edge_count == 0 && live.vertex_count == 2);
    CHECK(run("CADSCRIPT 1\nLINE 0 0 0 1 1 1\nDELV 3\n") == CAD_SCRIPT_OK);
    CHECK(live.edge_count == 0 && live.vertex_count == 3);
    CHECK(r.history_added == 2);
    /* Group undo: undo history_added times restores the pre-script state. */
    SAVE();
    CHECK(run("CADSCRIPT 1\nBOX 0 0 0 5 5 5\nMOVEV 1 9 9 9\nDELV 2\n")
          == CAD_SCRIPT_OK);
    CHECK(r.history_added == 3);
    OK(cad_model_undo(&live));
    OK(cad_model_undo(&live));
    OK(cad_model_undo(&live));
    CHECK(live.vertex_count == saved.vertex_count);
    CHECK(!memcmp(live.vertices, saved.vertices,
                  saved.vertex_count * sizeof(CadVertex)));
    CHECK(live.edge_count == saved.edge_count);

    /* Model rejections are reported with the model status. */
    reset();
    fails("CADSCRIPT 1\nMOVEV 9 0 0 0\n", CAD_SCRIPT_MODEL, 2);
    CHECK(r.model_status == CAD_NOT_FOUND);
    fails("CADSCRIPT 1\nDELE 4\n", CAD_SCRIPT_MODEL, 2);
    fails("CADSCRIPT 1\nDELV 4\n", CAD_SCRIPT_MODEL, 2);
    fails("CADSCRIPT 1\nLINE 1 1 1 1 1 1\n", CAD_SCRIPT_MODEL, 2);
    CHECK(r.model_status == CAD_INVALID_ARGUMENT);
    fails("CADSCRIPT 1\nBOX 0 0 0 0 5 5\n", CAD_SCRIPT_MODEL, 2);
    fails("CADSCRIPT 1\nBOX 300 0 0 200 5 5\n", CAD_SCRIPT_MODEL, 2);
    CHECK(r.model_status == CAD_RANGE);
    fails("CADSCRIPT 1\nEXTRUDE 10 0,0 10,0 20,0\n", CAD_SCRIPT_MODEL, 2);
    fails("CADSCRIPT 1\nEXTRUDE 10 0,0 10,10 10,0 0,10\n",
          CAD_SCRIPT_MODEL, 2);
}

static void variables(void)
{
    Params p;
    char text[1024];
    int i;
    reset();
    p.calls = 0;
    /* Script variable A shadows the callback's A; len comes from callback. */
    CHECK(run_with("CADSCRIPT 1\nSET A 7\nSET B A*2\n"
                   "LINE A B 0 len A+len 1\n", &p) == CAD_SCRIPT_OK);
    CHECK(has_vertex(7, 14, 0) && has_vertex(50, 57, 1));
    CHECK(p.calls == 2);   /* only the two "len" lookups reach the callback */
    CHECK(r.commands == 3);
    reset();
    CHECK(run_with("CADSCRIPT 1\nLINE A 0 0 0 0 0\n", &p) == CAD_SCRIPT_OK);
    CHECK(has_vertex(300, 0, 0));
    SAVE();
    CHECK(run_with("CADSCRIPT 1\nLINE A+A 0 0 0 0 0\n", &p)
          == CAD_SCRIPT_RANGE);
    CHECK(r.line == 2);
    SAME();
    CHECK(run_with("CADSCRIPT 1\nLINE huge 0 0 0 0 0\n", &p)
          == CAD_SCRIPT_OVERFLOW);
    SAME();
    reset();
    CHECK(run("CADSCRIPT 1\nSET X 1\nSET X X+1\nSET X X*10\n"
              "LINE X 0 0 0 0 0\n") == CAD_SCRIPT_OK);
    CHECK(has_vertex(20, 0, 0));
    /* Without a callback unknown names fail. */
    fails("CADSCRIPT 1\nLINE len 0 0 0 0 0\n", CAD_SCRIPT_UNKNOWN_NAME, 2);
    fails("CADSCRIPT 1\nSET 9x 1\n", CAD_SCRIPT_EXPRESSION, 2);
    fails("CADSCRIPT 1\nSET abcdefghijklmnop 1\n", CAD_SCRIPT_EXPRESSION, 2);
    CHECK(run("CADSCRIPT 1\nSET abcdefghijklmno 4\nSET b abcdefghijklmno\n")
          == CAD_SCRIPT_OK);
    /* 16 variables fit, 17th fails; reassigning does not consume a slot. */
    strcpy(text, "CADSCRIPT 1\n");
    for (i = 0; i < 16; ++i)
        sprintf(text + strlen(text), "SET V%d %d\n", i, i);
    strcat(text, "SET V3 99\n");
    CHECK(run(text) == CAD_SCRIPT_OK);
    strcat(text, "SET W 1\n");
    fails(text, CAD_SCRIPT_TOO_MANY_VARIABLES, 19);
}

static void malformed(void)
{
    static const char nul[] = "CADSCRIPT 1\nLINE 0 0 0\0 1 1 1\n";
    reset();
    OK(cad_model_begin(&live));
    SAVE();
    CHECK(run("CADSCRIPT 1\n") == CAD_SCRIPT_ARGUMENT);
    SAME();
    OK(cad_model_cancel(&live));
    CHECK(cad_script_run("CADSCRIPT 1\n", 12, &live, &live, 0, 0, &r)
          == CAD_SCRIPT_ARGUMENT);
    CHECK(cad_script_run("CADSCRIPT 1\n", 12, 0, &stage, 0, 0, &r)
          == CAD_SCRIPT_ARGUMENT);
    CHECK(cad_script_run("x", 1, &live, &stage, 0, 0, 0)
          == CAD_SCRIPT_ARGUMENT);
    CHECK(cad_script_run(0, 0, &live, &stage, 0, 0, &r) == CAD_SCRIPT_HEADER);
    fails("", CAD_SCRIPT_HEADER, 0);
    fails("# only comments\n\n", CAD_SCRIPT_HEADER, 0);
    fails("CADSCRIPT 2\n", CAD_SCRIPT_HEADER, 1);
    fails("\n# c\nLINE 0 0 0 1 1 1\n", CAD_SCRIPT_HEADER, 3);
    fails("CADSCRIPT 1 x\n", CAD_SCRIPT_HEADER, 1);
    fails("cadscript 1\n", CAD_SCRIPT_HEADER, 1);
    fails("CADSCRIPT 1\nCADSCRIPT 1\n", CAD_SCRIPT_SYNTAX, 2);
    fails("CADSCRIPT 1\nline 0 0 0 1 1 1\n", CAD_SCRIPT_SYNTAX, 2);
    fails("CADSCRIPT 1\n\n# c\nLINE 0 0 0 1 1\n", CAD_SCRIPT_SYNTAX, 4);
    fails("CADSCRIPT 1\nLINE 0 0 0 1 1 1 1\n", CAD_SCRIPT_SYNTAX, 2);
    fails("CADSCRIPT 1\nLINE 0 0 0 1 1 1 # no inline\n",
          CAD_SCRIPT_SYNTAX, 2);
    fails("CADSCRIPT 1\nRECT 0 0 0 1\n", CAD_SCRIPT_SYNTAX, 2);
    fails("CADSCRIPT 1\nBOX 0 0 0 1 1\n", CAD_SCRIPT_SYNTAX, 2);
    fails("CADSCRIPT 1\nMOVEV 1 0 0\n", CAD_SCRIPT_SYNTAX, 2);
    fails("CADSCRIPT 1\nDELV\n", CAD_SCRIPT_SYNTAX, 2);
    fails("CADSCRIPT 1\nDELE 1 2\n", CAD_SCRIPT_SYNTAX, 2);
    fails("CADSCRIPT 1\nSET X\n", CAD_SCRIPT_SYNTAX, 2);
    fails("CADSCRIPT 1\nUNITS\n", CAD_SCRIPT_SYNTAX, 2);
    fails("CADSCRIPT 1\nUNITS IN\n", CAD_SCRIPT_SYNTAX, 2);
    fails("CADSCRIPT 1\nEXTRUDE 10 0,0 1,0\n", CAD_SCRIPT_SYNTAX, 2);
    fails("CADSCRIPT 1\nEXTRUDE 10 0,0 1,0 1,1 2,2 3,3 4,4 5,5 6,6 7,7 8,8"
          " 9,9 10,10 11,11\n", CAD_SCRIPT_SYNTAX, 2);
    fails("CADSCRIPT 1\nEXTRUDE 10 0,0 1,0 1\n", CAD_SCRIPT_SYNTAX, 2);
    fails("CADSCRIPT 1\nEXTRUDE 10 0,0 1,0 1,1,1\n", CAD_SCRIPT_SYNTAX, 2);
    fails("CADSCRIPT 1\nEXTRUDE 0 0,0 10,0 0,10\n", CAD_SCRIPT_RANGE, 2);
    fails("CADSCRIPT 1\nEXTRUDE 401 0,0 10,0 0,10\n", CAD_SCRIPT_RANGE, 2);
    fails("CADSCRIPT 1\nEXTRUDE 5 0,0 401,0 0,10\n", CAD_SCRIPT_RANGE, 2);
    fails("CADSCRIPT 1\nEXTRUDE 5 0,0 10,0 0,-401\n", CAD_SCRIPT_RANGE, 2);
    fails("CADSCRIPT 1\nEXTRUDE 5 0,0 10,0 ,1\n", CAD_SCRIPT_EXPRESSION, 2);
    fails("CADSCRIPT 1\nLINE 0 0 0 401 0 0\n", CAD_SCRIPT_RANGE, 2);
    fails("CADSCRIPT 1\nLINE 0 0 0 0 -401 0\n", CAD_SCRIPT_RANGE, 2);
    fails("CADSCRIPT 1\nMOVEV 0 0 0 0\n", CAD_SCRIPT_RANGE, 2);
    fails("CADSCRIPT 1\nDELV 65536\n", CAD_SCRIPT_RANGE, 2);
    fails("CADSCRIPT 1\nDELE -1\n", CAD_SCRIPT_RANGE, 2);
    fails("CADSCRIPT 1\nLINE 0 0 0 1 1 1/0\n", CAD_SCRIPT_DIVIDE_BY_ZERO, 2);
    fails("CADSCRIPT 1\nLINE 0 0 0 1 1 (1\n", CAD_SCRIPT_EXPRESSION, 2);
    fails("CADSCRIPT 1\n\tLINE\t0 0 0 1 1 q\n", CAD_SCRIPT_UNKNOWN_NAME, 2);
    /* Characters: bare CR, NUL, control and non-ASCII, even in comments. */
    fails("CADSCRIPT 1\r\nLINE 0 0 0 1 1 1\r\r\n", CAD_SCRIPT_CHARACTER, 2);
    fails("CADSCRIPT 1\n# a\rb\n", CAD_SCRIPT_CHARACTER, 2);
    fails("CADSCRIPT 1\n# caf\xe9\n", CAD_SCRIPT_CHARACTER, 2);
    fails("CADSCRIPT 1\n# bell\a\n", CAD_SCRIPT_CHARACTER, 2);
    fails("CADSCRIPT 1\nLINE 0 0 0 1 1 1\r", CAD_SCRIPT_CHARACTER, 2);
    SAVE();
    CHECK(cad_script_run(nul, sizeof(nul) - 1, &live, &stage, 0, 0, &r)
          == CAD_SCRIPT_CHARACTER);
    CHECK(r.line == 2);
    SAME();
}

static void crlf(void)
{
    reset();
    CHECK(run("# c\r\nCADSCRIPT 1\r\n\r\nSET W 20\r\nBOX 0 0 0 W W W\r\n"
              "LINE 0 0 0 5 5 5") == CAD_SCRIPT_OK);
    CHECK(live.vertex_count == 10 && r.commands == 3);
    CHECK(has_vertex(20, 20, 20) && has_vertex(5, 5, 5));
    fails("CADSCRIPT 1\r\nSET W 20\r\nBOX 0 0 0 W W Q\r\n",
          CAD_SCRIPT_UNKNOWN_NAME, 3);
}

static void atomic(void)
{
    char text[512];
    long v, e;
    reset();
    CHECK(run("CADSCRIPT 1\nUNITS MM\nBOX 0 0 0 10 10 10\n") == CAD_SCRIPT_OK);
    OK(cad_model_undo(&live));      /* leave a redo entry to preserve */
    /* Everything succeeds until the very last line. */
    /* Undo never rewinds allocators: the script's box gets IDs v..v+7. */
    v = live.next_vertex_id;
    e = live.next_edge_id;
    sprintf(text, "CADSCRIPT 1\nUNITS NONE\nSET S 30\nBOX 0 0 0 S S S\n"
            "RECT 1 1 1 5 5\nMOVEV %ld -5 -5 -5\nDELE %ld\nDELV %ld\n"
            "EXTRUDE 9 0,0 9,0 0,9\nLINE 0 0 0 1 1 bogus\n", v, e, v + 1);
    fails(text, CAD_SCRIPT_UNKNOWN_NAME, 10);
    sprintf(text, "CADSCRIPT 1\nBOX 0 0 0 10 10 10\nDELV %ld\nDELV %ld\n",
            v, v);
    fails(text, CAD_SCRIPT_MODEL, 4);
    CHECK(r.model_status == CAD_NOT_FOUND);
    /* Same prefix without the bad line succeeds (the failure was real). */
    sprintf(text, "CADSCRIPT 1\nUNITS NONE\nSET S 30\nBOX 0 0 0 S S S\n"
            "RECT 1 1 1 5 5\nMOVEV %ld -5 -5 -5\nDELE %ld\nDELV %ld\n"
            "EXTRUDE 9 0,0 9,0 0,9\n", v, e, v + 1);
    memcpy(&stage, &live, sizeof(live));
    CHECK(cad_script_run(text, (unsigned int)strlen(text), &stage, &saved,
                         0, 0, &r) == CAD_SCRIPT_OK);
    CHECK(r.history_added == 7);
    OK(cad_model_redo(&live));
    CHECK(live.vertex_count == 8);
}

static void capacity(void)
{
    unsigned int n, i;
    char line[CAD_SCRIPT_LINE_MAX + 8];
    reset();
    /* 160-byte line accepted (also with CRLF), 161 rejected. */
    memset(line, ' ', sizeof(line));
    memcpy(line, "LINE 0 0 0 1 1 1", 16);
    strcpy(big, "CADSCRIPT 1\n");
    n = (unsigned int)strlen(big);
    memcpy(big + n, line, CAD_SCRIPT_LINE_MAX);
    strcpy(big + n + CAD_SCRIPT_LINE_MAX, "\r\n");
    CHECK(run(big) == CAD_SCRIPT_OK);
    memcpy(big + n, line, CAD_SCRIPT_LINE_MAX + 1);
    strcpy(big + n + CAD_SCRIPT_LINE_MAX + 1, "\n");
    fails(big, CAD_SCRIPT_LINE_TOO_LONG, 2);
    big[0] = '#';
    memset(big + 1, 'x', 170);
    strcpy(big + 171, "\nCADSCRIPT 1\n");
    fails(big, CAD_SCRIPT_LINE_TOO_LONG, 1);

    /* 1024 statements accepted; 1025 rejected on line 1026. Comments and
     * blank lines do not count. */
    reset();
    strcpy(big, "CADSCRIPT 1\n# c\n\n");
    n = (unsigned int)strlen(big);
    for (i = 0; i < CAD_SCRIPT_COMMAND_MAX; ++i) {
        memcpy(big + n, "SET A 1\n", 8);
        n += 8;
    }
    big[n] = '\0';
    CHECK(run(big) == CAD_SCRIPT_OK);
    CHECK(r.commands == CAD_SCRIPT_COMMAND_MAX);
    strcpy(big + n, "SET A 2\n");
    fails(big, CAD_SCRIPT_TOO_MANY_COMMANDS, 1028);

    /* Model capacity: 33 boxes exceed 256 vertices; nothing is published. */
    strcpy(big, "CADSCRIPT 1\n");
    for (i = 0; i < 33; ++i) strcat(big, "BOX 0 0 0 1 1 1\n");
    fails(big, CAD_SCRIPT_MODEL, 34);
    CHECK(r.model_status == CAD_CAPACITY);

    /* History: more than 32 script commands cannot all stay undoable;
     * exactly 32 can (older history is evicted instead, no flag). */
    reset();
    CHECK(run("CADSCRIPT 1\nLINE 0 0 0 1 1 1\n") == CAD_SCRIPT_OK);
    strcpy(big, "CADSCRIPT 1\n");
    for (i = 0; i < 31; ++i) strcat(big, "UNITS MM\nUNITS NONE\n");
    CHECK(run(big) == CAD_SCRIPT_OK);
    CHECK(r.history_added == 62 && r.history_truncated);
    reset();
    CHECK(run("CADSCRIPT 1\nLINE 0 0 0 1 1 1\n") == CAD_SCRIPT_OK);
    strcpy(big, "CADSCRIPT 1\n");
    for (i = 0; i < 16; ++i) strcat(big, "MOVEV 1 1 0 0\nMOVEV 1 0 0 0\n");
    CHECK(run(big) == CAD_SCRIPT_OK);
    CHECK(r.history_added == 32 && !r.history_truncated);
    CHECK(live.history_cursor == 32);   /* prior LINE command was evicted */
    for (i = 0; i < 32; ++i) OK(cad_model_undo(&live));
    CHECK(cad_model_undo(&live) == CAD_NO_HISTORY);
    CHECK(live.vertex_count == 2 && live.vertices[0].x == 0);
    strcat(big, "MOVEV 1 1 0 0\n");
    CHECK(run(big) == CAD_SCRIPT_OK);
    CHECK(r.history_added == 33 && r.history_truncated);
}

static FILE *open_example(const char *dir, const char *name)
{
    char path[1024];
    FILE *f;
    sprintf(path, "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (!f) fprintf(stderr, "cannot open %s\n", path);
    CHECK(f != 0);
    return f;
}

static void bounds(void)
{
    int i;
    for (i = 0; i < live.vertex_count; ++i) {
        CHECK(live.vertices[i].x >= -400 && live.vertices[i].x <= 400);
        CHECK(live.vertices[i].y >= -400 && live.vertices[i].y <= 400);
        CHECK(live.vertices[i].z >= -400 && live.vertices[i].z <= 400);
    }
}

static void examples(const char *dir)
{
    FILE *f;
    size_t n;
    reset();
    f = open_example(dir, "BRACKET.CMD");
    CHECK(cad_script_run_file(f, &live, &stage, 0, 0, &r) == CAD_SCRIPT_OK);
    fclose(f);
    CHECK(live.units == CAD_UNITS_MM);
    CHECK(live.vertex_count == 12 + 8 && live.edge_count == 18 + 12);
    CHECK(has_vertex(120, 20, 60) && has_vertex(20, 120, 0));
    CHECK(has_vertex(60, 60, 40));         /* gusset far corner */
    CHECK(r.history_added == 3);
    bounds();

    reset();
    f = open_example(dir, "TABLE.CMD");
    n = fread(big, 1, sizeof(big), f);
    fclose(f);
    CHECK(n > 0 && n < sizeof(big));
    CHECK(cad_script_run(big, (unsigned int)n, &live, &stage, 0, 0, &r)
          == CAD_SCRIPT_OK);
    CHECK(live.vertex_count == 40 && live.edge_count == 60);
    CHECK(has_vertex(-150, -100, 160) && has_vertex(150, 100, 180));
    CHECK(has_vertex(130, 80, 0) && has_vertex(150, 100, 160));
    CHECK(r.history_added == 6);
    bounds();
}

static void file_api(void)
{
    FILE *f = tmpfile();
    unsigned int i;
    CHECK(f != 0);
    fputs("CADSCRIPT 1\r\nLINE 0 0 0 1 2 3\r\nLINE 0 0 0 3 2 1", f);
    rewind(f);
    reset();
    CHECK(cad_script_run_file(f, &live, &stage, 0, 0, &r) == CAD_SCRIPT_OK);
    CHECK(live.vertex_count == 4 && has_vertex(3, 2, 1));
    fclose(f);
    f = tmpfile();
    CHECK(f != 0);
    fputs("CADSCRIPT 1\nLINE 0 0 0 1 2 3\n", f);
    for (i = 0; i < CAD_SCRIPT_LINE_MAX + 1; ++i) fputc('#', f);
    fputs("\r\n", f);
    rewind(f);
    SAVE();
    CHECK(cad_script_run_file(f, &live, &stage, 0, 0, &r)
          == CAD_SCRIPT_LINE_TOO_LONG);
    CHECK(r.line == 3);
    SAME();
    fclose(f);
    f = tmpfile();
    CHECK(f != 0);
    for (i = 0; i < CAD_SCRIPT_LINE_MAX; ++i) fputc('#', f);
    fputs("\r\nCADSCRIPT 1\nMOVEV 1 7 7 7\n", f);
    rewind(f);
    CHECK(cad_script_run_file(f, &live, &stage, 0, 0, &r) == CAD_SCRIPT_OK);
    CHECK(has_vertex(7, 7, 7));
    fclose(f);
    f = tmpfile();
    CHECK(f != 0);
    rewind(f);
    CHECK(cad_script_run_file(f, &live, &stage, 0, 0, &r)
          == CAD_SCRIPT_HEADER);
    CHECK(cad_script_run_file(0, &live, &stage, 0, 0, &r)
          == CAD_SCRIPT_ARGUMENT);
    fclose(f);
}

int main(int argc, char **argv)
{
    char dir[1024];
    const char *slash;
    if (argc > 1) {
        sprintf(dir, "%.1000s", argv[1]);
    } else {
        /* Derive <root>/examples from this source file's path. */
        slash = strrchr(__FILE__, '/');
        if (slash)
            sprintf(dir, "%.*s/../examples", (int)(slash - __FILE__),
                    __FILE__);
        else
            strcpy(dir, "../examples");
    }
    expressions();
    commands();
    variables();
    malformed();
    crlf();
    atomic();
    capacity();
    examples(dir);
    file_api();
    CHECK(strcmp(cad_script_status_string(CAD_SCRIPT_MODEL), "model error")
          == 0);
    printf("script_test: %lu checks PASS\n", checks);
    return 0;
}
