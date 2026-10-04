/* The standalone params and model APIs use the same enum names; bridge the
 * params spellings exactly as cad_workspace.h does. */
#define CadStatus CadParamStatus
#define CAD_OK CAD_PARAM_OK
#include "cad_params.h"
#undef CAD_OK
#undef CadStatus
#include "cad_userparams.h"
#include <string.h>

#ifdef __WATCOMC__
#pragma disable_message(124)
#endif
typedef char CadUserScratchFits[
    (sizeof(CadParams) <= CAD_USERPARAM_SCRATCH_BYTES) ? 1 : -1];
typedef char CadUserSlotReserved[(CAD_USERPARAM_MAX < CAD_PARAM_MAX) ? 1 : -1];
typedef char CadUserNameLimit[(CAD_USERPARAM_NAME_MAX == CAD_NAME_MAX) ? 1 : -1];
typedef char CadUserExprLimit[(CAD_USERPARAM_EXPR_MAX == CAD_EXPR_MAX) ? 1 : -1];
typedef char CadUserStatusOrder[
    ((int)CAD_UP_DIV_ZERO == (int)CAD_ERR_DIV_ZERO &&
     (int)CAD_UP_DIMENSION == (int)CAD_ERR_DIMENSION) ? 1 : -1];
#ifdef __WATCOMC__
#pragma enable_message(124)
#endif

#define ID_LIMIT 65535U

static int alpha(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
static int digit(int c) { return c >= '0' && c <= '9'; }
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c; }
static int printable(int c) { return c >= 0x20 && c <= 0x7E; }
/* Case-insensitive compare of a NUL-terminated name and a counted span. */
static int same_span(const char *name, const char *s, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; ++i)
        if (!name[i] || lower(name[i]) != lower(s[i])) return 0;
    return !name[n];
}
static int same_name(const char *a, const char *b)
{
    return same_span(a, b, (unsigned)strlen(b));
}
static int bounded(const char *s, unsigned max)
{
    unsigned n;
    if (!s) return 0;
    for (n = 0; n <= max; ++n) if (!s[n]) return 1;
    return 0;
}
static CadUserParamStatus check_name(const char *s)
{
    unsigned i;
    if (!s) return CAD_UP_ARGUMENT;
    if (!bounded(s, CAD_USERPARAM_NAME_MAX) || !alpha(s[0])) return CAD_UP_NAME;
    for (i = 1; s[i]; ++i) if (!alpha(s[i]) && !digit(s[i])) return CAD_UP_NAME;
    return CAD_UP_OK;
}
static CadUserParamStatus check_expression(const char *s, unsigned *offset)
{
    unsigned i;
    *offset = 0;
    if (!s) return CAD_UP_ARGUMENT;
    for (i = 0; s[i]; ++i) {
        if (i == CAD_USERPARAM_EXPR_MAX || !printable(s[i])) {
            *offset = i; return CAD_UP_EXPRESSION;
        }
    }
    return i ? CAD_UP_OK : CAD_UP_EXPRESSION;
}

/* Token walker consistent with the cad_params grammar. Returns 1 for a
 * parameter reference, 2 for a unit suffix after a numeric literal, 0 at the
 * end. *start and *len describe the token; *pos advances past it. */
static int next_token(const char *s, unsigned *pos, unsigned *start, unsigned *len)
{
    unsigned p = *pos, q;
    while (s[p]) {
        if (digit(s[p]) || s[p] == '.') {
            while (digit(s[p])) ++p;
            if (s[p] == '.') { ++p; while (digit(s[p])) ++p; }
            q = p;
            while (s[q] == ' ') ++q;
            if (alpha(s[q])) {
                *start = q;
                while (alpha(s[q])) ++q;
                *len = q - *start; *pos = q;
                return 2;
            }
        } else if (alpha(s[p])) {
            *start = p;
            while (alpha(s[p]) || digit(s[p])) ++p;
            *len = p - *start; *pos = p;
            return 1;
        } else ++p;
    }
    *pos = p;
    return 0;
}
static int uses_name(const char *expression, const char *name)
{
    unsigned pos = 0, start, len;
    int kind;
    while ((kind = next_token(expression, &pos, &start, &len)) != 0)
        if (kind == 1 && same_span(name, expression + start, len)) return 1;
    return 0;
}
/* Returns the offset of the first unit suffix, or -1 if none. */
static long first_unit(const char *expression)
{
    unsigned pos = 0, start, len;
    int kind;
    while ((kind = next_token(expression, &pos, &start, &len)) != 0)
        if (kind == 2) return (long)start;
    return -1L;
}

static void report(CadUserParamError *e, CadUserParamStatus s, unsigned id,
                   unsigned offset, unsigned line)
{
    if (!e) return;
    e->status = s; e->parameter_id = id; e->offset = offset; e->line = line;
    e->message = cad_userparams_status_string(s);
}
static int index_of(const CadUserParamSet *set, unsigned id, unsigned *index)
{
    unsigned i;
    if (!id) return 0;
    for (i = 0; i < set->count; ++i)
        if (set->entries[i].id == id) { *index = i; return 1; }
    return 0;
}
static int find_index(const CadUserParamSet *set, const char *name, unsigned *index)
{
    unsigned i;
    if (!bounded(name, CAD_USERPARAM_NAME_MAX)) return 0;
    for (i = 0; i < set->count; ++i)
        if (same_name(set->entries[i].name, name)) { *index = i; return 1; }
    return 0;
}
static unsigned long text_size(const CadUserParamSet *set)
{
    unsigned long n = 0;
    unsigned i;
    for (i = 0; i < set->count; ++i)
        n += 4UL + strlen(set->entries[i].name) + strlen(set->entries[i].expression);
    return n;
}

/* Evaluates the candidate set (extra == 0) or, against the committed values
 * of the candidate set used as literals, one extra expression. Types are
 * inferred by guessing LENGTH and falling back to SCALAR when cad_params
 * reports a dimension error for that parameter: dependencies always finish
 * before their users, so each parameter flips at most once. On failure
 * *bad is the 0-based failing index (count for the extra expression). */
static CadUserParamStatus solve(CadUserParams *t, CadUserParamSet *set,
                                const char *extra, CadValue *result,
                                unsigned *bad, unsigned *offset)
{
    CadParams *p = (CadParams *)(void *)t->scratch.bytes;
    unsigned char length_guess[CAD_PARAM_MAX];
    unsigned i, n = set->count + (extra ? 1U : 0U), attempt, pid;
    char name[4];
    CadValue v;
    CadError err;
    CadParamStatus s;
    long unit;
    *bad = set->count; *offset = 0;   /* count: table or extra-level */
    if (t->units != CAD_UNITS_UNSPECIFIED && t->units != CAD_UNITS_MM)
        return CAD_UP_UNITS;
    /* Unitless documents: point at the offending unit suffix up front.
     * Committed literals (extra mode) are already known to be scalars. */
    for (i = extra ? set->count : 0;
         t->units == CAD_UNITS_UNSPECIFIED && i < n; ++i) {
        unit = first_unit(extra ? extra : set->entries[i].expression);
        if (unit >= 0) { *bad = i; *offset = (unsigned)unit; return CAD_UP_UNITS; }
    }
    /* A reserved name absent from the table and from the extra text. */
    strcpy(name, "_00");
    if (extra) {
        for (i = 0; i < 100; ++i) {
            name[1] = (char)('0' + i / 10); name[2] = (char)('0' + i % 10);
            if (!find_index(set, name, &pid) && !uses_name(extra, name)) break;
        }
    }
    for (i = 0; i < n; ++i)
        length_guess[i] = (unsigned char)(t->units == CAD_UNITS_MM);
    for (attempt = 0; attempt <= n; ++attempt) {
        cad_params_init(p);
        for (i = 0; i < n; ++i) {
            const char *nm = i < set->count ? set->entries[i].name : name;
            if (extra && i < set->count) {
                v.ticks = set->entries[i].ticks;
                v.type = set->entries[i].type == CAD_UP_LENGTH ? CAD_LENGTH : CAD_SCALAR;
            } else {
                v.ticks = 0;
                v.type = length_guess[i] ? CAD_LENGTH : CAD_SCALAR;
            }
            v.unit = v.type == CAD_LENGTH ? CAD_UNIT_MM : CAD_UNIT_NONE;
            s = cad_params_add(p, nm, v, 0);
            if (s == CAD_PARAM_OK && (!extra || i == set->count))
                s = cad_params_set_expression(p, i + 1,
                    i < set->count ? set->entries[i].expression : extra);
            if (s != CAD_PARAM_OK) { *bad = i; return (CadUserParamStatus)(int)s; }
        }
        s = cad_params_evaluate(p, &err);
        if (s == CAD_PARAM_OK) break;
        pid = err.parameter_id;
        if (pid && pid <= n && s == CAD_ERR_DIMENSION && length_guess[pid - 1]) {
            length_guess[pid - 1] = 0;
            continue;
        }
        *bad = pid && pid <= n ? pid - 1 : set->count; *offset = err.offset;
        if (s == CAD_ERR_NOT_FOUND && *bad <= set->count) {
            /* cad_params reports the end of the unknown name; point at its start. */
            const char *src = *bad < set->count && !extra ?
                set->entries[*bad].expression : extra;
            while (src && *offset && (alpha(src[*offset - 1]) || digit(src[*offset - 1])))
                --*offset;
        }
        return (CadUserParamStatus)(int)s;
    }
    if (s != CAD_PARAM_OK) return CAD_UP_DIMENSION; /* unreachable */
    for (i = 0; i < n; ++i) {
        v = p->params[i].value;
        if (t->units == CAD_UNITS_UNSPECIFIED && v.type == CAD_LENGTH) {
            *bad = i; return CAD_UP_UNITS;
        }
        if (extra && i == set->count) *result = v;
        else if (!extra) {
            set->entries[i].ticks = v.ticks;
            set->entries[i].type = v.type == CAD_LENGTH ? CAD_UP_LENGTH : CAD_UP_SCALAR;
        }
    }
    return CAD_UP_OK;
}
/* Evaluates t->candidate and publishes it on success. */
static CadUserParamStatus commit(CadUserParams *t, CadUserParamError *e)
{
    unsigned bad, offset;
    CadUserParamStatus s;
    if (text_size(&t->candidate) > CAD_USERPARAM_TEXT_MAX) {
        report(e, CAD_UP_CAPACITY, 0, 0, 0); return CAD_UP_CAPACITY;
    }
    s = solve(t, &t->candidate, 0, 0, &bad, &offset);
    if (s != CAD_UP_OK) {
        report(e, s, bad < t->candidate.count ? t->candidate.entries[bad].id : 0,
               offset, 0);
        return s;
    }
    t->live = t->candidate;
    report(e, CAD_UP_OK, 0, 0, 0);
    return CAD_UP_OK;
}
static CadUserParamStatus fail(CadUserParamError *e, CadUserParamStatus s,
                               unsigned id, unsigned offset)
{
    report(e, s, id, offset, 0);
    return s;
}

void cad_userparams_init(CadUserParams *t, CadUnits units)
{
    if (!t) return;
    memset(t, 0, sizeof(*t));
    t->units = units;
    t->live.next_id = t->candidate.next_id = 1;
}
CadUserParamStatus cad_userparams_set_units(CadUserParams *t, CadUnits units,
                                            CadUserParamError *e)
{
    CadUnits old;
    CadUserParamStatus s;
    if (!t) return fail(e, CAD_UP_ARGUMENT, 0, 0);
    old = t->units; t->units = units;
    t->candidate = t->live;
    s = commit(t, e);
    if (s != CAD_UP_OK) t->units = old;
    return s;
}
CadUnits cad_userparams_units(const CadUserParams *t)
{
    return t ? t->units : CAD_UNITS_UNSPECIFIED;
}
unsigned cad_userparams_count(const CadUserParams *t) { return t ? t->live.count : 0; }
unsigned cad_userparams_find(const CadUserParams *t, const char *name)
{
    unsigned i;
    if (!t || !find_index(&t->live, name, &i)) return 0;
    return t->live.entries[i].id;
}
static void format_value(long ticks, CadUserParamType type, char *out)
{
    char digits[12];
    unsigned long mag = ticks < 0 ? (unsigned long)(-(ticks + 1)) + 1UL :
                                    (unsigned long)ticks;
    unsigned long whole = mag / 100UL, frac = mag % 100UL;
    unsigned n = 0, k = 0;
    if (ticks < 0) out[k++] = '-';
    do { digits[n++] = (char)('0' + (int)(whole % 10UL)); whole /= 10UL; } while (whole);
    while (n) out[k++] = digits[--n];
    if (frac) {
        out[k++] = '.';
        out[k++] = (char)('0' + (int)(frac / 10UL));
        if (frac % 10UL) out[k++] = (char)('0' + (int)(frac % 10UL));
    }
    if (type == CAD_UP_LENGTH) { out[k++] = ' '; out[k++] = 'm'; out[k++] = 'm'; }
    out[k] = 0;
}
CadUserParamStatus cad_userparams_at(const CadUserParams *t, unsigned index,
                                     CadUserParamInfo *info)
{
    const CadUserParamEntry *p;
    if (!t || !info || index >= t->live.count) return CAD_UP_NOT_FOUND;
    p = &t->live.entries[index];
    info->id = p->id; info->name = p->name; info->expression = p->expression;
    info->type = p->type; info->ticks = p->ticks;
    format_value(p->ticks, p->type, info->display);
    return CAD_UP_OK;
}
CadUserParamStatus cad_userparams_get(const CadUserParams *t, unsigned id,
                                      CadUserParamInfo *info)
{
    unsigned i;
    if (!t || !index_of(&t->live, id, &i)) return CAD_UP_NOT_FOUND;
    return cad_userparams_at(t, i, info);
}

CadUserParamStatus cad_userparams_add(CadUserParams *t, const char *name,
                                      const char *expression, unsigned *id,
                                      CadUserParamError *e)
{
    CadUserParamEntry *p;
    CadUserParamStatus s;
    unsigned i, offset;
    if (!t) return fail(e, CAD_UP_ARGUMENT, 0, 0);
    if ((s = check_name(name)) != CAD_UP_OK) return fail(e, s, 0, 0);
    if ((s = check_expression(expression, &offset)) != CAD_UP_OK)
        return fail(e, s, 0, offset);
    if (find_index(&t->live, name, &i))
        return fail(e, CAD_UP_DUPLICATE, t->live.entries[i].id, 0);
    if (t->live.count >= CAD_USERPARAM_MAX || !t->live.next_id)
        return fail(e, CAD_UP_FULL, 0, 0);
    t->candidate = t->live;
    p = &t->candidate.entries[t->candidate.count++];
    memset(p, 0, sizeof(*p));
    p->id = t->candidate.next_id;
    t->candidate.next_id = p->id == ID_LIMIT ? 0U : p->id + 1U;
    strcpy(p->name, name);
    strcpy(p->expression, expression);
    s = commit(t, e);
    if (s == CAD_UP_OK && id) *id = p->id;
    return s;
}
CadUserParamStatus cad_userparams_set_expression(CadUserParams *t, unsigned id,
    const char *expression, CadUserParamError *e)
{
    unsigned i, offset;
    CadUserParamStatus s;
    if (!t) return fail(e, CAD_UP_ARGUMENT, 0, 0);
    if (!index_of(&t->live, id, &i)) return fail(e, CAD_UP_NOT_FOUND, id, 0);
    if ((s = check_expression(expression, &offset)) != CAD_UP_OK)
        return fail(e, s, id, offset);
    t->candidate = t->live;
    strcpy(t->candidate.entries[i].expression, expression);
    return commit(t, e);
}
CadUserParamStatus cad_userparams_set_expression_named(CadUserParams *t,
    const char *name, const char *expression, CadUserParamError *e)
{
    unsigned id = cad_userparams_find(t, name);
    if (!t || !name) return fail(e, CAD_UP_ARGUMENT, 0, 0);
    if (!id) return fail(e, CAD_UP_NOT_FOUND, 0, 0);
    return cad_userparams_set_expression(t, id, expression, e);
}
/* Rewrites references to old as replacement; 0 if the result is too long. */
static int rewrite(const char *src, const char *old, const char *replacement,
                   char dst[CAD_USERPARAM_EXPR_MAX + 1])
{
    unsigned pos = 0, start, len, copied = 0, out = 0, rlen, n;
    int kind;
    rlen = (unsigned)strlen(replacement);
    for (;;) {
        kind = next_token(src, &pos, &start, &len);
        if (kind && (kind != 1 || !same_span(old, src + start, len))) continue;
        if (!kind) { start = pos; len = 0; }
        n = start - copied;
        if (out + n > CAD_USERPARAM_EXPR_MAX) return 0;
        memcpy(dst + out, src + copied, n); out += n;
        if (!kind) break;
        if (out + rlen > CAD_USERPARAM_EXPR_MAX) return 0;
        memcpy(dst + out, replacement, rlen); out += rlen;
        copied = start + len;
    }
    dst[out] = 0;
    return 1;
}
CadUserParamStatus cad_userparams_rename(CadUserParams *t, unsigned id,
    const char *new_name, CadUserParamError *e)
{
    unsigned i, j;
    char old[CAD_USERPARAM_NAME_MAX + 1], buffer[CAD_USERPARAM_EXPR_MAX + 1];
    CadUserParamStatus s;
    if (!t) return fail(e, CAD_UP_ARGUMENT, 0, 0);
    if (!index_of(&t->live, id, &i)) return fail(e, CAD_UP_NOT_FOUND, id, 0);
    if ((s = check_name(new_name)) != CAD_UP_OK) return fail(e, s, id, 0);
    if (find_index(&t->live, new_name, &j) && j != i)
        return fail(e, CAD_UP_DUPLICATE, t->live.entries[j].id, 0);
    t->candidate = t->live;
    strcpy(old, t->live.entries[i].name);
    for (j = 0; j < t->candidate.count; ++j) {
        if (!rewrite(t->candidate.entries[j].expression, old, new_name, buffer))
            return fail(e, CAD_UP_CAPACITY, t->candidate.entries[j].id, 0);
        strcpy(t->candidate.entries[j].expression, buffer);
    }
    strcpy(t->candidate.entries[i].name, new_name);
    return commit(t, e);
}
unsigned cad_userparams_references(const CadUserParams *t, const char *name)
{
    unsigned i;
    if (!t || !bounded(name, CAD_USERPARAM_NAME_MAX) || !name[0]) return 0;
    for (i = 0; i < t->live.count; ++i)
        if (uses_name(t->live.entries[i].expression, name))
            return t->live.entries[i].id;
    return 0;
}
CadUserParamStatus cad_userparams_remove(CadUserParams *t, unsigned id,
                                         CadUserParamError *e)
{
    unsigned i, user;
    if (!t) return fail(e, CAD_UP_ARGUMENT, 0, 0);
    if (!index_of(&t->live, id, &i)) return fail(e, CAD_UP_NOT_FOUND, id, 0);
    user = cad_userparams_references(t, t->live.entries[i].name);
    if (user) return fail(e, CAD_UP_IN_USE, user, 0);
    t->candidate = t->live;
    --t->candidate.count;
    memmove(&t->candidate.entries[i], &t->candidate.entries[i + 1],
            (t->candidate.count - i) * sizeof(CadUserParamEntry));
    return commit(t, e);
}
CadUserParamStatus cad_userparams_evaluate(CadUserParams *t, CadUserParamError *e)
{
    if (!t) return fail(e, CAD_UP_ARGUMENT, 0, 0);
    t->candidate = t->live;
    return commit(t, e);
}

CadUserParamStatus cad_userparams_eval_expression(CadUserParams *t,
    const char *text, long *model_units, CadUserParamError *e)
{
    unsigned bad, offset;
    unsigned long mag, rounded;
    CadValue v;
    CadUserParamStatus s;
    if (!t || !model_units) return fail(e, CAD_UP_ARGUMENT, 0, 0);
    if ((s = check_expression(text, &offset)) != CAD_UP_OK)
        return fail(e, s, 0, offset);
    s = solve(t, &t->live, text, &v, &bad, &offset);
    if (s != CAD_UP_OK) return fail(e, s, 0, offset);
    mag = v.ticks < 0 ? (unsigned long)(-(v.ticks + 1)) + 1UL : (unsigned long)v.ticks;
    rounded = (mag + (unsigned long)(CAD_SCALE / 2)) / (unsigned long)CAD_SCALE;
    if (rounded > (unsigned long)CAD_COORD_MAX)
        return fail(e, CAD_UP_RANGE, 0, 0);
    *model_units = v.ticks < 0 ? -(long)rounded : (long)rounded;
    report(e, CAD_UP_OK, 0, 0, 0);
    return CAD_UP_OK;
}

CadUserParamStatus cad_userparams_write_text(const CadUserParams *t,
    char *buffer, unsigned capacity, unsigned *length)
{
    unsigned long size;
    unsigned i, k = 0, n;
    const CadUserParamEntry *p;
    if (!t || !length || (!buffer && capacity)) return CAD_UP_ARGUMENT;
    size = text_size(&t->live);
    *length = (unsigned)size;
    if (size > (unsigned long)capacity) return CAD_UP_CAPACITY;
    for (i = 0; i < t->live.count; ++i) {
        p = &t->live.entries[i];
        buffer[k++] = 'P'; buffer[k++] = ' ';
        n = (unsigned)strlen(p->name); memcpy(buffer + k, p->name, n); k += n;
        buffer[k++] = ' ';
        n = (unsigned)strlen(p->expression); memcpy(buffer + k, p->expression, n); k += n;
        buffer[k++] = '\n';
    }
    if (k < capacity) buffer[k] = 0;
    return CAD_UP_OK;
}
static CadUserParamStatus read_fail(CadUserParamError *e, CadUserParamStatus s,
                                    unsigned line, unsigned offset)
{
    report(e, s, 0, offset, line);
    return s;
}
CadUserParamStatus cad_userparams_read_text(CadUserParams *t, const char *text,
    unsigned length, CadUnits units, CadUserParamError *e)
{
    CadUserParamSet *c;
    CadUserParamEntry *p;
    CadUserParamStatus s;
    CadUnits old;
    unsigned pos = 0, line = 0, start, k, n, bad, offset;
    if (!t || (!text && length)) return read_fail(e, CAD_UP_ARGUMENT, 0, 0);
    if (length > CAD_USERPARAM_TEXT_MAX) return read_fail(e, CAD_UP_CAPACITY, 0, 0);
    c = &t->candidate;
    memset(c, 0, sizeof(*c));
    while (pos < length) {
        start = pos; ++line;
        while (pos < length && text[pos] != '\n') {
            if (!printable(text[pos])) return read_fail(e, CAD_UP_FORMAT, line, pos - start);
            ++pos;
        }
        if (pos == length) return read_fail(e, CAD_UP_FORMAT, line, pos - start);
        if (pos - start < 2 || text[start] != 'P' || text[start + 1] != ' ')
            return read_fail(e, CAD_UP_FORMAT, line, 0);
        if (c->count == CAD_USERPARAM_MAX) return read_fail(e, CAD_UP_FULL, line, 0);
        p = &c->entries[c->count];
        memset(p, 0, sizeof(*p));
        for (k = start + 2, n = 0; k < pos && text[k] != ' '; ++k, ++n) {
            if (n == CAD_USERPARAM_NAME_MAX) return read_fail(e, CAD_UP_NAME, line, k - start);
            p->name[n] = text[k];
        }
        if (check_name(p->name) != CAD_UP_OK) return read_fail(e, CAD_UP_NAME, line, 2);
        if (find_index(c, p->name, &bad)) return read_fail(e, CAD_UP_DUPLICATE, line, 2);
        if (k == pos) return read_fail(e, CAD_UP_FORMAT, line, k - start);
        ++k;
        n = pos - k;
        if (!n || n > CAD_USERPARAM_EXPR_MAX)
            return read_fail(e, CAD_UP_EXPRESSION, line, k - start);
        memcpy(p->expression, text + k, n);
        p->id = ++c->count;
        ++pos;
    }
    c->next_id = c->count + 1U;
    old = t->units; t->units = units;
    s = solve(t, c, 0, 0, &bad, &offset);
    if (s != CAD_UP_OK) {
        t->units = old;
        report(e, s, 0, offset, bad < c->count ? bad + 1U : 0U);
        return s;
    }
    t->live = *c;
    report(e, CAD_UP_OK, 0, 0, 0);
    return CAD_UP_OK;
}

const char *cad_userparams_status_string(CadUserParamStatus s)
{
    static const char * const messages[] = {
        "success", "invalid argument", "parameter table full", "invalid name",
        "duplicate name", "unknown parameter", "invalid expression",
        "expression nesting limit", "dependency cycle", "incompatible dimensions",
        "incompatible or unrepresentable units", "signed32 overflow",
        "division by zero", "result outside model range -400..400",
        "parameter is referenced by another parameter",
        "text capacity exceeded", "malformed parameter line"
    };
    if ((unsigned)s >= sizeof(messages) / sizeof(messages[0])) return "unknown status";
    return messages[(unsigned)s];
}
