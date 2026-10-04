#include "cad_params.h"
#include <limits.h>
#include <string.h>

/* Internal control result: suspend parsing until this dependency is ready. */
#define CAD_PENDING ((CadStatus)99)
typedef struct {
    CadParams *table;
    CadValue values[CAD_PARAM_MAX];
    unsigned char state[CAD_PARAM_MAX];
    CadError error;
    unsigned pending;
} Evaluation;
typedef struct {
    Evaluation *eval;
    const char *text;
    unsigned pos, depth, id;
} Parser;

static int alpha(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
static int digit(int c) { return c >= '0' && c <= '9'; }
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c; }
static int equal_name(const char *a, const char *b)
{
    while (*a && *b && lower(*a) == lower(*b)) { ++a; ++b; }
    return !*a && !*b;
}
static int bounded(const char *s, unsigned max)
{
    unsigned n;
    if (!s) return 0;
    for (n = 0; n <= max; ++n) if (!s[n]) return 1;
    return 0;
}
static int in_range(long ticks)
{
#if LONG_MAX > 2147483647L
    return ticks >= CAD_LONG_MIN && ticks <= CAD_LONG_MAX;
#else
    (void)ticks;
    return 1;
#endif
}
static int valid_value(CadValue v)
{
    return in_range(v.ticks) &&
        ((v.type == CAD_SCALAR && v.unit == CAD_UNIT_NONE) ||
         (v.type == CAD_LENGTH &&
          (v.unit == CAD_UNIT_MM || v.unit == CAD_UNIT_LEGACY)));
}
void cad_params_init(CadParams *t) { if (t) memset(t, 0, sizeof(*t)); }
unsigned cad_params_find(const CadParams *t, const char *name)
{
    unsigned i;
    if (!t || !bounded(name, CAD_NAME_MAX) || t->count > CAD_PARAM_MAX) return 0;
    for (i = 0; i < t->count; ++i)
        if (equal_name(t->params[i].name, name)) return i + 1;
    return 0;
}
const CadParameter *cad_params_get(const CadParams *t, unsigned id)
{
    if (!t || t->count > CAD_PARAM_MAX || !id || id > t->count) return 0;
    return &t->params[id - 1];
}
CadStatus cad_params_add(CadParams *t, const char *name, CadValue v, unsigned *id)
{
    unsigned i;
    CadParameter *p;
    if (!t || t->count > CAD_PARAM_MAX || !valid_value(v)) return CAD_ERR_ARGUMENT;
    if (!bounded(name, CAD_NAME_MAX) || !alpha(name[0])) return CAD_ERR_NAME;
    for (i = 1; name[i]; ++i)
        if (!alpha(name[i]) && !digit(name[i])) return CAD_ERR_NAME;
    if (cad_params_find(t, name)) return CAD_ERR_DUPLICATE;
    if (t->count == CAD_PARAM_MAX) return CAD_ERR_FULL;
    p = &t->params[t->count];
    memset(p, 0, sizeof(*p));
    p->id = ++t->count;
    strcpy(p->name, name);
    p->type = v.type;
    p->literal = p->value = v;
    if (id) *id = p->id;
    return CAD_OK;
}
CadStatus cad_params_set_value(CadParams *t, unsigned id, CadValue v)
{
    CadParameter *p;
    if (!cad_params_get(t, id) || !valid_value(v)) return CAD_ERR_ARGUMENT;
    p = &t->params[id - 1];
    if (v.type != p->type) return CAD_ERR_DIMENSION;
    p->literal = v;
    p->expression[0] = 0;
    return CAD_OK;
}
CadStatus cad_params_set_expression(CadParams *t, unsigned id, const char *s)
{
    if (!cad_params_get(t, id) || !s) return CAD_ERR_ARGUMENT;
    if (!bounded(s, CAD_EXPR_MAX) || !s[0]) return CAD_ERR_EXPRESSION;
    memmove(t->params[id - 1].expression, s, strlen(s) + 1);
    return CAD_OK;
}

static unsigned long magnitude(long a)
{
    return a < 0 ? (unsigned long)(-(a + 1)) + 1UL : (unsigned long)a;
}
static CadStatus signed_result(unsigned long n, int negative, long *out)
{
    unsigned long limit = negative ? 2147483648UL : 2147483647UL;
    if (n > limit) return CAD_ERR_OVERFLOW;
    if (negative && n == 2147483648UL) *out = CAD_LONG_MIN;
    else *out = negative ? -(long)n : (long)n;
    return CAD_OK;
}
static CadStatus add(long a, long b, long *out)
{
    if ((b > 0 && a > CAD_LONG_MAX - b) ||
        (b < 0 && a < CAD_LONG_MIN - b)) return CAD_ERR_OVERFLOW;
    *out = a + b;
    return CAD_OK;
}
static CadStatus subtract(long a, long b, long *out)
{
    if ((b < 0 && a > CAD_LONG_MAX + b) ||
        (b > 0 && a < CAD_LONG_MIN + b)) return CAD_ERR_OVERFLOW;
    *out = a - b;
    return CAD_OK;
}
/* Exact unsigned multiply/divide using quotient/remainder long division.
 * No product or doubled remainder is formed; every temporary fits uint32.
 * This accepts e.g. MAX*100/100, even though the raw product exceeds long.
 */
static CadStatus muldiv(long a, long b, long d, long *out)
{
    unsigned long x, y, den, q, r, bq, br, bit, carry, inc, limit;
    int negative;
    if (!d) return CAD_ERR_DIV_ZERO;
    negative = ((a < 0) != (b < 0)) != (d < 0);
    limit = negative ? 2147483648UL : 2147483647UL;
    x = magnitude(a); y = magnitude(b); den = magnitude(d);
    bq = y / den; br = y % den; q = r = 0;
    bit = 2147483648UL;
    while (bit) {
        carry = 0;
        if (r >= den - r) { r -= den - r; carry = 1; }
        else r += r;
        inc = 0;
        if (x & bit) {
            inc = bq;
            if (r >= den - br) { r -= den - br; ++carry; }
            else r += br;
        }
        if (inc > limit - carry) return CAD_ERR_OVERFLOW;
        inc += carry;
        if (q > (limit - inc) / 2UL) return CAD_ERR_OVERFLOW;
        q = q * 2UL + inc;
        bit >>= 1;
    }
    return signed_result(q, negative, out);
}
static CadStatus fail(Parser *p, CadStatus s)
{
    if (p->eval->error.status == CAD_OK) {
        p->eval->error.status = s;
        p->eval->error.parameter_id = p->id;
        p->eval->error.offset = p->pos;
    }
    return s;
}
static void space(Parser *p)
{
    while (p->text[p->pos] == ' ' || p->text[p->pos] == '\t' ||
           p->text[p->pos] == '\r' || p->text[p->pos] == '\n') ++p->pos;
}
static CadStatus expression(Parser *p, CadValue *v);
static CadStatus evaluate_one(Evaluation *e, unsigned id);

static CadStatus number(Parser *p, int negative, CadValue *v)
{
    unsigned long n = 0, fraction = 0;
    unsigned decimals = 0;
    int any = 0;
    char unit[CAD_NAME_MAX + 1];
    unsigned len = 0;
    CadStatus s;
    while (digit(p->text[p->pos])) {
        unsigned d = (unsigned)(p->text[p->pos++] - '0');
        any = 1;
        if (n > (2147483648UL - d) / 10UL) return fail(p, CAD_ERR_OVERFLOW);
        n = n * 10UL + d;
    }
    if (p->text[p->pos] == '.') {
        ++p->pos;
        while (digit(p->text[p->pos])) {
            if (++decimals > 2) return fail(p, CAD_ERR_EXPRESSION);
            fraction = fraction * 10UL + (unsigned)(p->text[p->pos++] - '0');
            any = 1;
        }
    }
    if (!any) return fail(p, CAD_ERR_EXPRESSION);
    if (decimals == 1) fraction *= 10UL;
    if (n > (2147483648UL - fraction) / 100UL) return fail(p, CAD_ERR_OVERFLOW);
    n = n * 100UL + fraction;
    s = signed_result(n, negative, &v->ticks);
    if (s != CAD_OK) return fail(p, s);
    v->type = CAD_SCALAR; v->unit = CAD_UNIT_NONE;
    space(p);
    while (alpha(p->text[p->pos])) {
        if (len == CAD_NAME_MAX) return fail(p, CAD_ERR_UNITS);
        unit[len++] = p->text[p->pos++];
    }
    unit[len] = 0;
    if (!len) return CAD_OK;
    v->type = CAD_LENGTH; v->unit = CAD_UNIT_MM;
    if (equal_name(unit, "mm")) return CAD_OK;
    if (equal_name(unit, "cm")) s = muldiv(v->ticks, 10L, 1L, &v->ticks);
    else if (equal_name(unit, "in")) {
        /* 0.01 inch is 0.254 mm, not representable in 0.01 mm ticks. */
        if (v->ticks % 5L) return fail(p, CAD_ERR_UNITS);
        s = muldiv(v->ticks, 254L, 10L, &v->ticks);
    } else return fail(p, CAD_ERR_UNITS);
    return s == CAD_OK ? s : fail(p, s);
}
static CadStatus primary(Parser *p, CadValue *v)
{
    CadStatus s;
    unsigned id, len;
    char name[CAD_NAME_MAX + 1];
    int c;
    space(p); c = p->text[p->pos];
    if (c == '+' || c == '-' || c == '(') {
        if (p->depth == CAD_NEST_MAX) return fail(p, CAD_ERR_NESTING);
        ++p->depth; ++p->pos;
        if (c == '(') {
            s = expression(p, v); space(p);
            if (s == CAD_OK && p->text[p->pos] != ')') s = fail(p, CAD_ERR_EXPRESSION);
            if (s == CAD_OK) ++p->pos;
        } else {
            space(p);
            if (digit(p->text[p->pos]) || p->text[p->pos] == '.')
                s = number(p, c == '-', v);
            else {
                s = primary(p, v);
                if (s == CAD_OK && c == '-') {
                    if (v->ticks == CAD_LONG_MIN) s = fail(p, CAD_ERR_OVERFLOW);
                    else v->ticks = -v->ticks;
                }
            }
        }
        --p->depth; return s;
    }
    if (digit(c) || c == '.') return number(p, 0, v);
    if (!alpha(c)) return fail(p, CAD_ERR_EXPRESSION);
    len = 0;
    while (alpha(p->text[p->pos]) || digit(p->text[p->pos])) {
        if (len == CAD_NAME_MAX) return fail(p, CAD_ERR_NAME);
        name[len++] = p->text[p->pos++];
    }
    name[len] = 0;
    id = cad_params_find(p->eval->table, name);
    if (!id) return fail(p, CAD_ERR_NOT_FOUND);
    if (p->eval->state[id - 1] == 1) return fail(p, CAD_ERR_CYCLE);
    if (p->eval->state[id - 1] != 2) {
        p->eval->pending = id;
        return CAD_PENDING;
    }
    *v = p->eval->values[id - 1];
    return CAD_OK;
}
static CadStatus operation(Parser *p, int op, CadValue *a, CadValue b)
{
    CadStatus s;
    if (op == '+' || op == '-') {
        if (a->type != b.type) return fail(p, CAD_ERR_DIMENSION);
        if (a->unit != b.unit) return fail(p, CAD_ERR_UNITS);
        s = op == '+' ? add(a->ticks, b.ticks, &a->ticks) :
                        subtract(a->ticks, b.ticks, &a->ticks);
    } else if (op == '*') {
        if (a->type == CAD_LENGTH && b.type == CAD_LENGTH)
            return fail(p, CAD_ERR_DIMENSION);
        s = muldiv(a->ticks, b.ticks, CAD_SCALE, &a->ticks);
        if (b.type == CAD_LENGTH) { a->type = b.type; a->unit = b.unit; }
    } else {
        if (a->type == CAD_SCALAR && b.type == CAD_LENGTH)
            return fail(p, CAD_ERR_DIMENSION);
        if (a->type == CAD_LENGTH && b.type == CAD_LENGTH) {
            if (a->unit != b.unit) return fail(p, CAD_ERR_UNITS);
            a->type = CAD_SCALAR; a->unit = CAD_UNIT_NONE;
        }
        s = muldiv(a->ticks, CAD_SCALE, b.ticks, &a->ticks);
    }
    return s == CAD_OK ? s : fail(p, s);
}
static CadStatus product(Parser *p, CadValue *v)
{
    CadValue b;
    CadStatus s = primary(p, v);
    int op;
    while (s == CAD_OK) {
        space(p); op = p->text[p->pos];
        if (op != '*' && op != '/') break;
        ++p->pos; s = primary(p, &b);
        if (s == CAD_OK) s = operation(p, op, v, b);
    }
    return s;
}
static CadStatus expression(Parser *p, CadValue *v)
{
    CadValue b;
    CadStatus s = product(p, v);
    int op;
    while (s == CAD_OK) {
        space(p); op = p->text[p->pos];
        if (op != '+' && op != '-') break;
        ++p->pos; s = product(p, &b);
        if (s == CAD_OK) s = operation(p, op, v, b);
    }
    return s;
}
static CadStatus evaluate_one(Evaluation *e, unsigned id)
{
    Parser p;
    CadParameter *src = &e->table->params[id - 1];
    CadStatus s;
    if (e->state[id - 1] == 2) return CAD_OK;
    p.eval = e; p.text = src->expression; p.pos = p.depth = 0; p.id = id;
    if (src->expression[0]) {
        s = expression(&p, &e->values[id - 1]); space(&p);
        if (s == CAD_OK && p.text[p.pos]) s = fail(&p, CAD_ERR_EXPRESSION);
    } else {
        e->values[id - 1] = src->literal;
        s = valid_value(src->literal) ? CAD_OK : fail(&p, CAD_ERR_ARGUMENT);
    }
    if (s == CAD_OK && e->values[id - 1].type != src->type)
        s = fail(&p, CAD_ERR_DIMENSION);
    if (s == CAD_OK) e->state[id - 1] = 2;
    return s;
}
static CadStatus stage(Evaluation *e, CadParams *t)
{
    unsigned i, depth, id, path[CAD_PARAM_MAX];
    CadStatus s;
    memset(e, 0, sizeof(*e)); e->table = t;
    if (!t || t->count > CAD_PARAM_MAX) {
        e->error.status = CAD_ERR_ARGUMENT; return CAD_ERR_ARGUMENT;
    }
    for (i = 1; i <= t->count; ++i) {
        if (e->state[i - 1] == 2) continue;
        depth = 1; path[0] = i; e->state[i - 1] = 1;
        while (depth) {
            id = path[depth - 1];
            s = evaluate_one(e, id);
            if (s == CAD_PENDING) {
                /* Every push is an unvisited ID, so depth cannot exceed 32.
                 * Dependencies do not consume the Win16 C call stack. */
                path[depth++] = e->pending;
                e->state[e->pending - 1] = 1;
            } else if (s == CAD_OK) --depth;
            else return s;
        }
    }
    return CAD_OK;
}
static void commit(Evaluation *e)
{
    unsigned i;
    for (i = 0; i < e->table->count; ++i) e->table->params[i].value = e->values[i];
}
CadStatus cad_params_evaluate(CadParams *t, CadError *error)
{
    Evaluation e;
    CadStatus s = stage(&e, t);
    if (s == CAD_OK) commit(&e);
    if (error) *error = e.error;
    return s;
}
const char *cad_status_string(CadStatus s)
{
    static const char * const messages[] = {
        "success", "invalid argument", "parameter table full", "invalid name",
        "duplicate name", "unknown parameter", "invalid expression",
        "expression nesting limit", "dependency cycle", "incompatible dimensions",
        "incompatible or unrepresentable units", "signed32 overflow",
        "division by zero", "invalid or out-of-bounds house geometry"
    };
    if ((unsigned)s >= sizeof(messages) / sizeof(messages[0])) return "unknown status";
    return messages[(unsigned)s];
}

static const char * const house_names[] = {
    "Width", "Depth", "WallHeight", "RoofRise", "DoorWidth", "DoorHeight",
    "WindowWidth", "WindowHeight", "WindowSill", "WindowCenter", "BaseY",
    "ChimneyX", "ChimneyWidth", "ChimneyBack", "ChimneyDepth", "ChimneyHeight"
};
void cad_house_defaults(CadParams *t)
{
    static const int defaults[] = {
        200, 140, 100, 70, 30, 70, 40, 40, 40, 55, -70, 40, 25, 45, 20, 45
    };
    CadValue v;
    unsigned i;
    if (!t) return;
    cad_params_init(t); v.type = CAD_LENGTH; v.unit = CAD_UNIT_MM;
    for (i = 0; i < sizeof(defaults) / sizeof(defaults[0]); ++i) {
        v.ticks = (long)defaults[i] * CAD_SCALE;
        (void)cad_params_add(t, house_names[i], v, 0);
    }
}

CadStatus cad_house_generate(CadParams *t, CadModelScale scale,
                             CadXYZ output[CAD_HOUSE_VERTICES], CadError *error)
{
    Evaluation e;
    CadXYZ verts[CAD_HOUSE_VERTICES];
    long h[16], w, d, base, wall, ridge, door, sill, top, left, right;
    long cx, cw, back, front, cb, cf, ct, rise, x, y, z, chimney_right;
    unsigned i, id, j;
    CadStatus s = stage(&e, t);
    if (s != CAD_OK) { if (error) *error = e.error; return s; }
    if (!output || scale.ticks_per_model_unit <= 0 ||
        !in_range(scale.ticks_per_model_unit) ||
        (scale.unit != CAD_UNIT_MM && scale.unit != CAD_UNIT_LEGACY))
        s = CAD_ERR_ARGUMENT;
    for (i = 0; s == CAD_OK && i < 16; ++i) {
        id = cad_params_find(t, house_names[i]);
        e.error.parameter_id = id;
        if (!id) { s = CAD_ERR_NOT_FOUND; break; }
        if (e.values[id - 1].type != CAD_LENGTH) { s = CAD_ERR_DIMENSION; break; }
        if (e.values[id - 1].unit != scale.unit) { s = CAD_ERR_UNITS; break; }
        h[i] = e.values[id - 1].ticks;
    }
    if (s == CAD_OK) {
        for (i = 0; i < 16; ++i)
            if (i != 10 && i != 11 && h[i] <= 0) s = CAD_ERR_GEOMETRY;
    }
    if (s == CAD_OK) {
        w = h[0] / 2; d = h[1] / 2; base = h[10];
        rise = h[3];
        s = add(base, h[2], &wall);
        if (s == CAD_OK) s = add(wall, rise, &ridge);
        if (s == CAD_OK) s = add(base, h[5], &door);
        if (s == CAD_OK) s = add(base, h[8], &sill);
        if (s == CAD_OK) s = add(sill, h[7], &top);
        if (s == CAD_OK) s = subtract(h[9], h[6] / 2, &left);
        if (s == CAD_OK) s = add(h[9], h[6] / 2, &right);
        cx = h[11]; cw = h[12]; back = h[13]; front = back - h[14];
        if (s == CAD_OK) s = add(cx, cw, &chimney_right);
        if (s == CAD_OK && (!w || !d || h[4] >= h[0] || door > wall || top > wall ||
            left <= h[4] / 2 || right >= w || front < 0 || back > d ||
            cx < -w || chimney_right > w)) s = CAD_ERR_GEOMETRY;
        if (s == CAD_OK) s = muldiv(rise, d - back, d, &cb);
        if (s == CAD_OK) s = muldiv(rise, d - front, d, &cf);
        if (s == CAD_OK) s = add(cb, wall, &cb);
        if (s == CAD_OK) s = add(cf, wall, &cf);
        if (s == CAD_OK) s = add(cf, h[15], &ct);
        if (s == CAD_OK) {
            for (i = 0; i < CAD_HOUSE_VERTICES; ++i) {
                x = y = z = 0;
                if (i < 8) {
                    j = i % 4;
                    x = j == 0 || j == 3 ? -w : w;
                    y = i < 4 ? base : wall;
                    z = j < 2 ? d : -d;
                } else if (i < 10) { x = i == 8 ? -w : w; y = ridge; }
                else if (i < 14) {
                    j = i - 10; x = j < 2 ? -h[4] / 2 : h[4] / 2;
                    y = j == 0 || j == 3 ? base : door; z = d;
                } else if (i < 22) {
                    j = (i - 14) % 4;
                    x = j == 0 || j == 3 ? left : right;
                    if (i < 18) x = j == 0 || j == 3 ? -right : -left;
                    y = j < 2 ? sill : top; z = d;
                } else {
                    j = (i - 22) % 4;
                    x = j == 0 || j == 3 ? cx : chimney_right;
                    y = i >= 26 ? ct : (j < 2 ? cb : cf);
                    z = j < 2 ? -back : -front;
                }
                /* C89 leaves negative division rounding implementation-defined. */
                s = muldiv(x, 1L, scale.ticks_per_model_unit, &x);
                if (s == CAD_OK) s = muldiv(y, 1L, scale.ticks_per_model_unit, &y);
                if (s == CAD_OK) s = muldiv(z, 1L, scale.ticks_per_model_unit, &z);
                if (s != CAD_OK) break;
                if (x < -CAD_MODEL_LIMIT || x > CAD_MODEL_LIMIT ||
                    y < -CAD_MODEL_LIMIT || y > CAD_MODEL_LIMIT ||
                    z < -CAD_MODEL_LIMIT || z > CAD_MODEL_LIMIT) {
                    s = CAD_ERR_GEOMETRY; break;
                }
                verts[i].x = (int)x; verts[i].y = (int)y; verts[i].z = (int)z;
            }
        }
    }
    e.error.status = s;
    if (s == CAD_OK) {
        e.error.parameter_id = 0;
        commit(&e); memcpy(output, verts, sizeof(verts));
    }
    if (error) *error = e.error;
    return s;
}
