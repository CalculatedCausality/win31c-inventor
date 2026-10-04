#include <string.h>
#include "cad_script.h"
#include "cad_create.h"

#define MAX_TOKENS 16
#define MAX_DEPTH 40   /* each '(' nests two levels */
#define LIMIT CAD_SCRIPT_VALUE_LIMIT

typedef struct Variable {
    char name[CAD_SCRIPT_NAME_MAX + 1];
    long value;
} Variable;

/* Static to keep the Win16 stack small; the module is not reentrant. */
static struct State {
    CadModel *stage;
    CadScriptLookup lookup;
    void *context;
    CadScriptResult *result;
    Variable vars[CAD_SCRIPT_VARIABLE_MAX];
    int var_count;
    int header_seen;
    unsigned long line_no;
    const char *cursor;          /* expression parser position */
    int depth;
    char line[CAD_SCRIPT_LINE_MAX + 2];
    char *tokens[MAX_TOKENS];
} st;

static CadScriptStatus fail(CadScriptStatus status, const char *message,
                            const char *detail)
{
    char *out = st.result->message;
    unsigned int n = 0;
    while (*message && n < CAD_SCRIPT_MESSAGE_MAX - 1) out[n++] = *message++;
    if (detail) {
        if (n < CAD_SCRIPT_MESSAGE_MAX - 1) out[n++] = ' ';
        while (*detail && n < CAD_SCRIPT_MESSAGE_MAX - 1) out[n++] = *detail++;
    }
    out[n] = '\0';
    st.result->status = status;
    st.result->line = st.line_no;
    return status;
}

static int is_alpha(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int is_digit(int c) { return c >= '0' && c <= '9'; }

/* ---- expressions: recursive descent over one NUL-terminated token ---- */

static CadScriptStatus parse_sum(long *value);

static CadScriptStatus lookup_name(const char *name, long *value)
{
    int i;
    long v;
    for (i = 0; i < st.var_count; ++i)
        if (strcmp(st.vars[i].name, name) == 0) {
            *value = st.vars[i].value;
            return CAD_SCRIPT_OK;
        }
    if (st.lookup && st.lookup(st.context, name, &v)) {
        if (v < -LIMIT || v > LIMIT)
            return fail(CAD_SCRIPT_OVERFLOW, "value too large:", name);
        *value = v;
        return CAD_SCRIPT_OK;
    }
    return fail(CAD_SCRIPT_UNKNOWN_NAME, "unknown name", name);
}

static CadScriptStatus parse_primary(long *value)
{
    char name[CAD_SCRIPT_NAME_MAX + 1];
    unsigned int n = 0;
    long v = 0;
    CadScriptStatus s;
    if (*st.cursor == '(') {
        ++st.cursor;
        s = parse_sum(value);
        if (s != CAD_SCRIPT_OK) return s;
        if (*st.cursor != ')')
            return fail(CAD_SCRIPT_EXPRESSION, "missing )", 0);
        ++st.cursor;
        return CAD_SCRIPT_OK;
    }
    if (is_digit(*st.cursor)) {
        while (is_digit(*st.cursor)) {
            v = v * 10 + (*st.cursor++ - '0');
            if (v > LIMIT) return fail(CAD_SCRIPT_OVERFLOW,
                                       "number too large", 0);
        }
        if (is_alpha(*st.cursor))
            return fail(CAD_SCRIPT_EXPRESSION, "bad number", 0);
        *value = v;
        return CAD_SCRIPT_OK;
    }
    if (is_alpha(*st.cursor)) {
        while (is_alpha(*st.cursor) || is_digit(*st.cursor)) {
            if (n == CAD_SCRIPT_NAME_MAX)
                return fail(CAD_SCRIPT_EXPRESSION, "name too long", 0);
            name[n++] = *st.cursor++;
        }
        name[n] = '\0';
        return lookup_name(name, value);
    }
    return fail(CAD_SCRIPT_EXPRESSION, "bad expression", 0);
}

static CadScriptStatus parse_unary(long *value)
{
    CadScriptStatus s;
    int negate = 0;
    if (++st.depth > MAX_DEPTH)
        return fail(CAD_SCRIPT_EXPRESSION, "expression too deep", 0);
    if (*st.cursor == '-' || *st.cursor == '+') {
        negate = *st.cursor++ == '-';
        s = parse_unary(value);
    } else {
        s = parse_primary(value);
    }
    --st.depth;
    if (s == CAD_SCRIPT_OK && negate) *value = -*value;
    return s;
}

static CadScriptStatus parse_product(long *value)
{
    long a = 0, b = 0, ma, mb, q;
    char op;
    CadScriptStatus s = parse_unary(&a);
    if (s != CAD_SCRIPT_OK) return s;
    while (*st.cursor == '*' || *st.cursor == '/') {
        op = *st.cursor++;
        s = parse_unary(&b);
        if (s != CAD_SCRIPT_OK) return s;
        /* Magnitudes keep sign handling portable (C89 '/' rounding of
         * negative operands is implementation-defined). |a|,|b| <= LIMIT. */
        ma = a < 0 ? -a : a;
        mb = b < 0 ? -b : b;
        if (op == '*') {
            if (ma != 0 && mb > LIMIT / ma)
                return fail(CAD_SCRIPT_OVERFLOW, "overflow", 0);
            q = ma * mb;
        } else {
            if (mb == 0)
                return fail(CAD_SCRIPT_DIVIDE_BY_ZERO, "division by zero", 0);
            q = ma / mb;
        }
        a = ((a < 0) != (b < 0)) ? -q : q;
    }
    *value = a;
    return CAD_SCRIPT_OK;
}

static CadScriptStatus parse_sum(long *value)
{
    long a = 0, b = 0;
    char op;
    CadScriptStatus s;
    if (++st.depth > MAX_DEPTH)
        return fail(CAD_SCRIPT_EXPRESSION, "expression too deep", 0);
    s = parse_product(&a);
    while (s == CAD_SCRIPT_OK && (*st.cursor == '+' || *st.cursor == '-')) {
        op = *st.cursor++;
        s = parse_product(&b);
        if (s != CAD_SCRIPT_OK) break;
        a = op == '+' ? a + b : a - b;   /* |a|,|b| <= 1e9: fits 32 bits */
        if (a < -LIMIT || a > LIMIT)
            s = fail(CAD_SCRIPT_OVERFLOW, "overflow", 0);
    }
    --st.depth;
    if (s == CAD_SCRIPT_OK) *value = a;
    return s;
}

static CadScriptStatus eval_token(const char *token, long *value)
{
    long v = 0;
    CadScriptStatus s;
    st.cursor = token;
    st.depth = 0;
    s = parse_sum(&v);
    if (s != CAD_SCRIPT_OK) return s;
    if (*st.cursor != '\0')
        return fail(CAD_SCRIPT_EXPRESSION, "unexpected text:", st.cursor);
    *value = v;
    return CAD_SCRIPT_OK;
}

/* ---- statements ---- */

static CadScriptStatus value_arg(int index, long *value)
{
    return eval_token(st.tokens[index], value);
}

static CadScriptStatus coord_arg(int index, long *value)
{
    CadScriptStatus s = value_arg(index, value);
    if (s == CAD_SCRIPT_OK &&
        (*value < CAD_COORD_MIN || *value > CAD_COORD_MAX))
        return fail(CAD_SCRIPT_RANGE, "coordinate outside -400..400:",
                    st.tokens[index]);
    return s;
}

static CadScriptStatus id_arg(int index, CadId *id)
{
    long v = 0;
    CadScriptStatus s = value_arg(index, &v);
    if (s != CAD_SCRIPT_OK) return s;
    if (v < 1 || v > 65535L)
        return fail(CAD_SCRIPT_RANGE, "ID outside 1..65535:", st.tokens[index]);
    *id = (CadId)v;
    return CAD_SCRIPT_OK;
}

static CadScriptStatus model_fail(CadStatus status)
{
    st.result->model_status = status;
    return fail(CAD_SCRIPT_MODEL, cad_model_status_string(status), 0);
}

/* One more undo command now sits on top of stage's history. */
static void count_command(void)
{
    ++st.result->history_added;
    if (st.result->history_added > st.stage->history_cursor)
        st.result->history_truncated = 1;
}

static CadScriptStatus created(CadStatus status)
{
    if (status != CAD_OK) return model_fail(status);
    count_command();   /* every successful primitive adds a vertex */
    return CAD_SCRIPT_OK;
}

/* Runs one single-operation transaction (op 0 move, 1 delv, 2 dele, 3 units). */
static CadScriptStatus edit(int op, CadId id, long x, long y, long z)
{
    CadModel *m = st.stage;
    CadStatus s = cad_model_begin(m);
    if (s != CAD_OK) return model_fail(s);
    if (op == 0) s = cad_model_move_vertex(m, id, x, y, z);
    else if (op == 1) s = cad_model_delete_vertex(m, id);
    else if (op == 2) s = cad_model_delete_edge(m, id);
    else s = cad_model_set_units(m, (CadUnits)x);
    if (s != CAD_OK) {
        cad_model_cancel(m);
        return model_fail(s);
    }
    /* Single-operation transactions stage a delta iff they change state. */
    if (m->staged_count == 0) {
        cad_model_cancel(m);
        return CAD_SCRIPT_OK;
    }
    s = cad_model_commit(m);
    if (s != CAD_OK) return model_fail(s);
    count_command();
    return CAD_SCRIPT_OK;
}

static int valid_name(const char *name)
{
    unsigned int n = 0;
    if (!is_alpha(*name)) return 0;
    while (name[n]) {
        if (!is_alpha(name[n]) && !is_digit(name[n])) return 0;
        if (++n > CAD_SCRIPT_NAME_MAX) return 0;
    }
    return 1;
}

static CadScriptStatus do_set(void)
{
    long v = 0;
    int i;
    CadScriptStatus s;
    if (!valid_name(st.tokens[1]))
        return fail(CAD_SCRIPT_EXPRESSION, "bad variable name:", st.tokens[1]);
    s = value_arg(2, &v);
    if (s != CAD_SCRIPT_OK) return s;
    for (i = 0; i < st.var_count; ++i)
        if (strcmp(st.vars[i].name, st.tokens[1]) == 0) break;
    if (i == st.var_count) {
        if (i == CAD_SCRIPT_VARIABLE_MAX)
            return fail(CAD_SCRIPT_TOO_MANY_VARIABLES,
                        "more than 16 variables", 0);
        strcpy(st.vars[i].name, st.tokens[1]);
        ++st.var_count;
    }
    st.vars[i].value = v;
    return CAD_SCRIPT_OK;
}

static CadScriptStatus do_extrude(int count)
{
    CadSketchPoint points[CAD_SCRIPT_EXTRUDE_MAX];
    long h = 0, v = 0;
    int i;
    char *comma;
    CadScriptStatus s;
    if (count < 2 + CAD_SCRIPT_EXTRUDE_MIN || count > 2 + CAD_SCRIPT_EXTRUDE_MAX)
        return fail(CAD_SCRIPT_SYNTAX, "EXTRUDE needs h and 3..12 x,y", 0);
    s = value_arg(1, &h);
    if (s != CAD_SCRIPT_OK) return s;
    if (h < 1 || h > CAD_COORD_MAX)
        return fail(CAD_SCRIPT_RANGE, "height outside 1..400:", st.tokens[1]);
    for (i = 2; i < count; ++i) {
        comma = strchr(st.tokens[i], ',');
        if (!comma || strchr(comma + 1, ','))
            return fail(CAD_SCRIPT_SYNTAX, "expected x,y:", st.tokens[i]);
        *comma = '\0';
        s = eval_token(st.tokens[i], &v);
        if (s == CAD_SCRIPT_OK && (v < CAD_COORD_MIN || v > CAD_COORD_MAX))
            s = fail(CAD_SCRIPT_RANGE, "coordinate outside -400..400:",
                     st.tokens[i]);
        if (s != CAD_SCRIPT_OK) return s;
        points[i - 2].x = (int)v;
        s = eval_token(comma + 1, &v);
        if (s == CAD_SCRIPT_OK && (v < CAD_COORD_MIN || v > CAD_COORD_MAX))
            s = fail(CAD_SCRIPT_RANGE, "coordinate outside -400..400:",
                     comma + 1);
        if (s != CAD_SCRIPT_OK) return s;
        points[i - 2].y = (int)v;
    }
    return created(cad_create_extrusion(st.stage, points, count - 2, (int)h));
}

static CadScriptStatus args(int first, int coords, int count, long *v)
{
    int i;
    CadScriptStatus s = CAD_SCRIPT_OK;
    for (i = first; i < count && s == CAD_SCRIPT_OK; ++i)
        s = i < first + coords ? coord_arg(i, &v[i - first])
                               : value_arg(i, &v[i - first]);
    return s;
}

static int want(int count, int expected)
{
    if (count == expected) return 1;
    fail(CAD_SCRIPT_SYNTAX, "wrong argument count for", st.tokens[0]);
    return 0;
}

static CadScriptStatus statement(int count)
{
    const char *op = st.tokens[0];
    long v[6];
    CadId id = 0;
    CadScriptStatus s;
    memset(v, 0, sizeof(v));
    if (strcmp(op, "SET") == 0) {
        if (!want(count, 3)) return st.result->status;
        return do_set();
    }
    if (strcmp(op, "LINE") == 0) {
        if (!want(count, 7)) return st.result->status;
        s = args(1, 6, count, v);
        return s ? s : created(cad_create_line(st.stage, v[0], v[1], v[2],
                                               v[3], v[4], v[5]));
    }
    if (strcmp(op, "RECT") == 0) {
        if (!want(count, 6)) return st.result->status;
        s = args(1, 3, count, v);
        return s ? s : created(cad_create_rectangle(st.stage, v[0], v[1],
                                                    v[2], v[3], v[4]));
    }
    if (strcmp(op, "BOX") == 0) {
        if (!want(count, 7)) return st.result->status;
        s = args(1, 3, count, v);
        return s ? s : created(cad_create_box(st.stage, v[0], v[1], v[2],
                                              v[3], v[4], v[5]));
    }
    if (strcmp(op, "EXTRUDE") == 0) return do_extrude(count);
    if (strcmp(op, "MOVEV") == 0) {
        if (!want(count, 5)) return st.result->status;
        s = id_arg(1, &id);
        if (s == CAD_SCRIPT_OK) s = args(2, 3, count, v);
        return s ? s : edit(0, id, v[0], v[1], v[2]);
    }
    if (strcmp(op, "DELV") == 0 || strcmp(op, "DELE") == 0) {
        if (!want(count, 2)) return st.result->status;
        s = id_arg(1, &id);
        return s ? s : edit(op[3] == 'V' ? 1 : 2, id, 0, 0, 0);
    }
    if (strcmp(op, "UNITS") == 0) {
        if (!want(count, 2)) return st.result->status;
        if (strcmp(st.tokens[1], "MM") == 0)
            return edit(3, 0, (long)CAD_UNITS_MM, 0, 0);
        if (strcmp(st.tokens[1], "NONE") == 0)
            return edit(3, 0, (long)CAD_UNITS_UNSPECIFIED, 0, 0);
        return fail(CAD_SCRIPT_SYNTAX, "UNITS takes MM or NONE", 0);
    }
    return fail(CAD_SCRIPT_SYNTAX, "unknown command", op);
}

/* text excludes the line terminator (LF, or CRLF). */
static CadScriptStatus feed_line(const char *text, unsigned int length)
{
    unsigned int i;
    int count = 0;
    char *p;
    unsigned char c;
    ++st.line_no;
    if (length > CAD_SCRIPT_LINE_MAX)
        return fail(CAD_SCRIPT_LINE_TOO_LONG, "line longer than 160 bytes", 0);
    for (i = 0; i < length; ++i) {
        c = (unsigned char)text[i];
        if ((c < 32 && c != '\t') || c > 126)
            return fail(CAD_SCRIPT_CHARACTER, "invalid character", 0);
        st.line[i] = (char)c;
    }
    st.line[length] = '\0';
    p = st.line;
    for (;;) {
        while (*p == ' ' || *p == '\t') *p++ = '\0';
        if (!*p) break;
        if (count == MAX_TOKENS)
            return fail(CAD_SCRIPT_SYNTAX, "too many tokens", 0);
        st.tokens[count++] = p;
        while (*p && *p != ' ' && *p != '\t') ++p;
    }
    if (count == 0 || st.tokens[0][0] == '#') return CAD_SCRIPT_OK;
    if (!st.header_seen) {
        if (count != 2 || strcmp(st.tokens[0], "CADSCRIPT") != 0 ||
            strcmp(st.tokens[1], "1") != 0)
            return fail(CAD_SCRIPT_HEADER, "expected CADSCRIPT 1", 0);
        st.header_seen = 1;
        return CAD_SCRIPT_OK;
    }
    if (st.result->commands == CAD_SCRIPT_COMMAND_MAX)
        return fail(CAD_SCRIPT_TOO_MANY_COMMANDS, "more than 1024 commands", 0);
    ++st.result->commands;
    return statement(count);
}

static CadScriptStatus start(CadModel *live, CadModel *stage,
                             CadScriptLookup lookup, void *context,
                             CadScriptResult *result)
{
    memset(result, 0, sizeof(*result));
    memset(&st, 0, sizeof(st));
    st.result = result;
    if (!live || !stage || live == stage)
        return fail(CAD_SCRIPT_ARGUMENT, "invalid models", 0);
    if (live->transaction_active)
        return fail(CAD_SCRIPT_ARGUMENT, "transaction active", 0);
    *stage = *live;
    st.stage = stage;
    st.lookup = lookup;
    st.context = context;
    return CAD_SCRIPT_OK;
}

static CadScriptStatus finish(CadScriptStatus s, CadModel *live)
{
    CadScriptResult *r = st.result;
    if (s == CAD_SCRIPT_OK && !st.header_seen) {
        st.line_no = 0;
        s = fail(CAD_SCRIPT_HEADER, "missing CADSCRIPT 1 header", 0);
    }
    if (s != CAD_SCRIPT_OK) {
        r->history_added = 0;
        r->history_truncated = 0;
        return s;
    }
    *live = *st.stage;
    r->status = CAD_SCRIPT_OK;
    r->line = 0;
    r->message[0] = '\0';
    return CAD_SCRIPT_OK;
}

CadScriptStatus cad_script_run(const char *text, unsigned int length,
                               CadModel *live, CadModel *stage,
                               CadScriptLookup lookup, void *context,
                               CadScriptResult *result)
{
    unsigned int begin = 0, end, n;
    CadScriptStatus s;
    if (!result) return CAD_SCRIPT_ARGUMENT;
    s = start(live, stage, lookup, context, result);
    if (s != CAD_SCRIPT_OK) return s;
    if (!text && length) return fail(CAD_SCRIPT_ARGUMENT, "no text", 0);
    while (s == CAD_SCRIPT_OK && begin < length) {
        end = begin;
        while (end < length && text[end] != '\n') ++end;
        n = end - begin;
        if (end < length && n > 0 && text[end - 1] == '\r') --n;
        s = feed_line(text + begin, n);
        begin = end + 1;
    }
    return finish(s, live);
}

CadScriptStatus cad_script_run_file(FILE *input, CadModel *live,
                                    CadModel *stage, CadScriptLookup lookup,
                                    void *context, CadScriptResult *result)
{
    /* Reads up to 161 bytes plus a possible CRLF CR before rejecting. */
    static char buffer[CAD_SCRIPT_LINE_MAX + 2];
    unsigned int n;
    int c, eof = 0;
    CadScriptStatus s;
    if (!result) return CAD_SCRIPT_ARGUMENT;
    s = start(live, stage, lookup, context, result);
    if (s != CAD_SCRIPT_OK) return s;
    if (!input) return fail(CAD_SCRIPT_ARGUMENT, "no file", 0);
    while (s == CAD_SCRIPT_OK && !eof) {
        n = 0;
        for (;;) {
            c = getc(input);
            if (c == EOF) { eof = 1; break; }
            if (c == '\n') break;
            if (n < sizeof(buffer)) buffer[n++] = (char)c;
        }
        if (ferror(input)) {
            s = fail(CAD_SCRIPT_IO, "read error", 0);
            break;
        }
        if (eof && n == 0) break;
        if (!eof && n > 0 && n <= CAD_SCRIPT_LINE_MAX + 1 &&
            buffer[n - 1] == '\r') --n;
        s = feed_line(buffer, n);
    }
    return finish(s, live);
}

CadScriptStatus cad_script_eval(const char *expression,
                                CadScriptLookup lookup, void *context,
                                long *value)
{
    CadScriptResult scratch;
    memset(&scratch, 0, sizeof(scratch));
    memset(&st, 0, sizeof(st));
    st.result = &scratch;
    if (!expression || !value) return CAD_SCRIPT_ARGUMENT;
    st.lookup = lookup;
    st.context = context;
    return eval_token(expression, value);
}

const char *cad_script_status_string(CadScriptStatus status)
{
    switch (status) {
    case CAD_SCRIPT_OK: return "ok";
    case CAD_SCRIPT_ARGUMENT: return "invalid argument";
    case CAD_SCRIPT_IO: return "read error";
    case CAD_SCRIPT_LINE_TOO_LONG: return "line too long";
    case CAD_SCRIPT_CHARACTER: return "invalid character";
    case CAD_SCRIPT_HEADER: return "missing or bad header";
    case CAD_SCRIPT_SYNTAX: return "syntax error";
    case CAD_SCRIPT_EXPRESSION: return "bad expression";
    case CAD_SCRIPT_UNKNOWN_NAME: return "unknown name";
    case CAD_SCRIPT_DIVIDE_BY_ZERO: return "division by zero";
    case CAD_SCRIPT_OVERFLOW: return "value too large";
    case CAD_SCRIPT_RANGE: return "value out of range";
    case CAD_SCRIPT_TOO_MANY_COMMANDS: return "too many commands";
    case CAD_SCRIPT_TOO_MANY_VARIABLES: return "too many variables";
    case CAD_SCRIPT_MODEL: return "model error";
    }
    return "unknown status";
}
