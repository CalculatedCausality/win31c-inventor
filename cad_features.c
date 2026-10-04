/* Parametric feature history; see cad_features.h for the model. */
#include "cad_features.h"
#include "cad_sketch.h"
#include <string.h>

#define LIMIT 400L
#define TURN 23040L                 /* one turn in 1/64 degree */
#define REV_VERTS (CAD_FEAT_CIRCLE_MAX * CAD_FEAT_REVOLVE_MAX)

/* Candidate copy for fail-atomic mutations and read_text. */
static CadFeatures cand;
static CadFeatError last;
typedef char CadFeatDocFits64K[(unsigned long)sizeof(CadFeatures) < 65000UL ? 1 : -1];
/* Output must be one < 64 KiB object on the 16-bit target. */
typedef char CadFeatOutFits64K[(sizeof(int) > 2 ||
    (unsigned long)sizeof(CadFeatOutput) < 65000UL) ? 1 : -1];

static const short sin_table[91] = {
    0, 286, 572, 857, 1143, 1428, 1713, 1997, 2280, 2563,
    2845, 3126, 3406, 3686, 3964, 4240, 4516, 4790, 5063, 5334,
    5604, 5872, 6138, 6402, 6664, 6924, 7182, 7438, 7692, 7943,
    8192, 8438, 8682, 8923, 9162, 9397, 9630, 9860, 10087, 10311,
    10531, 10749, 10963, 11174, 11381, 11585, 11786, 11982, 12176, 12365,
    12551, 12733, 12911, 13085, 13255, 13421, 13583, 13741, 13894, 14044,
    14189, 14330, 14466, 14598, 14726, 14849, 14968, 15082, 15191, 15296,
    15396, 15491, 15582, 15668, 15749, 15826, 15897, 15964, 16026, 16083,
    16135, 16182, 16225, 16262, 16294, 16322, 16344, 16362, 16374, 16382,
    16384
};

const char *cad_features_status_string(CadFeatStatus status)
{
    switch (status) {
    case CAD_FEAT_OK: return "OK";
    case CAD_FEAT_ARGUMENT: return "invalid argument";
    case CAD_FEAT_FULL: return "feature list is full";
    case CAD_FEAT_NAME: return "invalid name";
    case CAD_FEAT_DUPLICATE: return "name already used";
    case CAD_FEAT_NOT_FOUND: return "feature not found";
    case CAD_FEAT_EXPRESSION: return "invalid expression text";
    case CAD_FEAT_REFERENCE: return "invalid sketch reference";
    case CAD_FEAT_IN_USE: return "referenced by another feature";
    case CAD_FEAT_ORDER: return "feature would precede its sketch";
    case CAD_FEAT_CAPACITY: return "text capacity exceeded";
    case CAD_FEAT_FORMAT: return "malformed feature line";
    case CAD_FEAT_EVAL: return "expression error";
    case CAD_FEAT_RANGE: return "value out of range";
    case CAD_FEAT_GEOMETRY: return "invalid geometry";
    }
    return "unknown status";
}

/* ---- small helpers ---- */

static CadFeatStatus fail(CadFeatStatus s, unsigned id, CadFeatField field,
                          int profile, int expr)
{
    last.status = s;
    last.feature_id = id;
    last.field = field;
    last.profile = profile;
    last.expr_index = expr;
    last.line = 0;
    last.message = cad_features_status_string(s);
    return s;
}

static int valid_text(const char *s, unsigned max)
{
    unsigned n = 0;
    if (s == 0) return 0;
    for (; s[n]; ++n)
        if (n >= max || s[n] < 0x20 || s[n] > 0x7E || s[n] == '"') return 0;
    return n > 0;
}
static int valid_name(const char *s)
{
    return valid_text(s, CAD_FEAT_NAME_MAX) && s[0] != ' ' &&
           s[strlen(s) - 1] != ' ';
}
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; }
static int same_name(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b)) { ++a; ++b; }
    return *a == 0 && *b == 0;
}

static int index_of(const CadFeatures *f, unsigned id)
{
    unsigned i;
    if (id == 0) return -1;
    for (i = 0; i < f->count; ++i)
        if (f->items[i].id == id) return (int)i;
    return -1;
}
int cad_features_index(const CadFeatures *f, unsigned id)
{
    return f ? index_of(f, id) : -1;
}
unsigned cad_features_count(const CadFeatures *f) { return f ? f->count : 0; }
unsigned cad_features_find(const CadFeatures *f, const char *name)
{
    unsigned i;
    if (f == 0 || name == 0) return 0;
    for (i = 0; i < f->count; ++i)
        if (same_name(f->items[i].name, name)) return f->items[i].id;
    return 0;
}
/* Name used by a feature other than `except` (slot) ? */
static int name_taken(const CadFeatures *f, const char *name, int except)
{
    unsigned i;
    for (i = 0; i < f->count; ++i)
        if ((int)i != except && same_name(f->items[i].name, name)) return 1;
    return 0;
}

static unsigned profile_count_of(const CadFeatures *f, unsigned sketch)
{
    unsigned i, n = 0;
    for (i = 0; i < f->profile_count; ++i)
        if (f->profiles[i].owner == sketch) ++n;
    return n;
}
/* Pool slot of profile `index` of `sketch`, or -1. */
static int profile_slot(const CadFeatures *f, unsigned sketch, unsigned index)
{
    unsigned i, n = 0;
    for (i = 0; i < f->profile_count; ++i)
        if (f->profiles[i].owner == sketch && n++ == index) return (int)i;
    return -1;
}
static const char *profile_expr(const CadFeatProfileData *p, unsigned i)
{
    const char *s = p->text;
    while (i--) s += strlen(s) + 1;
    return s;
}

static char *put_unsigned(char *d, unsigned long v)
{
    char tmp[12];
    int n = 0;
    do { tmp[n++] = (char)('0' + (int)(v % 10UL)); v /= 10UL; } while (v);
    while (n) *d++ = tmp[--n];
    *d = 0;
    return d;
}

/* ---- text writer (also used to enforce CAD_FEAT_TEXT_MAX) ---- */

static const char *const plane_names[3] = { "XY", "XZ", "YZ" };
static const char *const dir_names[3] = { "POS", "NEG", "SYM" };
static const char *const axis_names[2] = { "U", "V" };
static const char *const kind_names[4] = { "", "RECT", "CIRCLE", "POLY" };
static const char *const type_names[4] = { "", "SKETCH", "EXTRUDE", "REVOLVE" };

typedef struct Out { char *buf; unsigned cap; unsigned long len; } Out;
static void out_char(Out *o, char c)
{
    if (o->buf != 0 && o->len < (unsigned long)o->cap) o->buf[(unsigned)o->len] = c;
    ++o->len;
}
static void out_str(Out *o, const char *s) { while (*s) out_char(o, *s++); }
static void out_num(Out *o, unsigned long v)
{
    char t[12];
    put_unsigned(t, v);
    out_str(o, t);
}
static void out_quoted(Out *o, const char *s)
{
    out_str(o, " \"");
    out_str(o, s);
    out_char(o, '"');
}
static void out_profile_ref(Out *o, int profile)
{
    out_char(o, ' ');
    if (profile == CAD_FEAT_ALL) out_str(o, "ALL");
    else out_num(o, (unsigned long)profile);
}

static void write_all(const CadFeatures *f, Out *o)
{
    unsigned i, j, k;
    out_str(o, "N ");
    out_num(o, f->next_id);
    out_char(o, '\n');
    for (i = 0; i < f->count; ++i) {
        const CadFeatureData *fe = &f->items[i];
        out_str(o, "F ");
        out_num(o, fe->id);
        out_char(o, ' ');
        out_str(o, type_names[fe->type]);
        out_str(o, fe->suppressed ? " S" : " A");
        out_quoted(o, fe->name);
        out_char(o, ' ');
        if (fe->type == CAD_FEAT_SKETCH) {
            out_str(o, plane_names[fe->plane]);
            out_quoted(o, fe->expr);
            out_char(o, '\n');
            for (j = 0; j < f->profile_count; ++j) {
                const CadFeatProfileData *p = &f->profiles[j];
                if (p->owner != fe->id) continue;
                out_str(o, "R ");
                out_str(o, kind_names[p->kind]);
                if (p->kind == CAD_FEAT_CIRCLE) {
                    out_char(o, ' ');
                    out_num(o, p->sides);
                } else if (p->kind == CAD_FEAT_POLY) {
                    out_char(o, ' ');
                    out_num(o, p->expr_count / 2U);
                }
                for (k = 0; k < p->expr_count; ++k)
                    out_quoted(o, profile_expr(p, k));
                out_char(o, '\n');
            }
            continue;
        }
        out_num(o, fe->sketch_id);
        out_profile_ref(o, fe->profile);
        out_char(o, ' ');
        if (fe->type == CAD_FEAT_EXTRUDE) {
            out_str(o, dir_names[fe->direction]);
            out_quoted(o, fe->expr);
        } else {
            out_str(o, axis_names[fe->axis]);
            out_char(o, ' ');
            out_num(o, fe->segments);
        }
        out_char(o, '\n');
    }
}

CadFeatStatus cad_features_write_text(const CadFeatures *f, char *buffer,
                                      unsigned capacity, unsigned *length)
{
    Out o;
    if (f == 0 || length == 0 || (buffer == 0 && capacity != 0))
        return CAD_FEAT_ARGUMENT;
    o.buf = buffer;
    o.cap = capacity;
    o.len = 0;
    write_all(f, &o);
    *length = (unsigned)o.len;   /* <= ~30 KB by construction */
    if (o.len > CAD_FEAT_TEXT_MAX || o.len > (unsigned long)capacity)
        return CAD_FEAT_CAPACITY;
    if (o.len < (unsigned long)capacity) buffer[(unsigned)o.len] = 0;
    return CAD_FEAT_OK;
}

/* ---- mutation scaffolding ---- */

static void begin(const CadFeatures *f)
{
    cand = *f;
    fail(CAD_FEAT_OK, 0, CAD_FEAT_FIELD_NONE, -1, -1);
}
static CadFeatStatus finish(CadFeatures *f, CadFeatStatus s, CadFeatError *e)
{
    if (s == CAD_FEAT_OK) {
        Out o;
        o.buf = 0; o.cap = 0; o.len = 0;
        write_all(&cand, &o);
        if (o.len > CAD_FEAT_TEXT_MAX)
            s = fail(CAD_FEAT_CAPACITY, 0, CAD_FEAT_FIELD_NONE, -1, -1);
        else
            *f = cand;
    }
    if (e != 0) *e = last;
    return s;
}
static CadFeatStatus bad_argument(CadFeatError *e)
{
    CadFeatStatus s = fail(CAD_FEAT_ARGUMENT, 0, CAD_FEAT_FIELD_NONE, -1, -1);
    if (e != 0) *e = last;
    return s;
}

void cad_features_init(CadFeatures *f)
{
    if (f == 0) return;
    memset(f, 0, sizeof(*f));
    f->next_id = 1;
}

/* ---- read-only views ---- */

typedef struct Desc { char *d; unsigned n; } Desc;
static void app(Desc *x, const char *s)
{
    while (*s && x->n < CAD_FEAT_DESC_MAX) x->d[x->n++] = *s++;
    x->d[x->n] = 0;
}
static void app_num(Desc *x, unsigned long v)
{
    char t[12];
    put_unsigned(t, v);
    app(x, t);
}

static void describe(const CadFeatures *f, const CadFeatureData *fe, char *d)
{
    Desc x;
    int s;
    x.d = d; x.n = 0; d[0] = 0;
    app(&x, fe->name);
    app(&x, " (");
    if (fe->type == CAD_FEAT_SKETCH) {
        unsigned n = profile_count_of(f, fe->id);
        app(&x, plane_names[fe->plane]);
        if (strcmp(fe->expr, "0") != 0) {
            app(&x, ", offset ");
            app(&x, fe->expr);
        }
        app(&x, ", ");
        app_num(&x, n);
        app(&x, n == 1 ? " profile)" : " profiles)");
    } else {
        s = index_of(f, fe->sketch_id);
        app(&x, s >= 0 ? f->items[s].name : "?");
        if (fe->profile != CAD_FEAT_ALL) {
            app(&x, " profile ");
            app_num(&x, (unsigned long)fe->profile + 1UL);  /* 1-based */
        }
        app(&x, ", ");
        if (fe->type == CAD_FEAT_EXTRUDE) {
            app(&x, fe->expr);
            if (fe->direction == CAD_FEAT_NEGATIVE) app(&x, ", reversed");
            if (fe->direction == CAD_FEAT_SYMMETRIC) app(&x, ", symmetric");
        } else {
            app(&x, "axis ");
            app(&x, axis_names[fe->axis]);
            app(&x, ", ");
            app_num(&x, fe->segments);
            app(&x, " segments");
        }
        app(&x, ")");
    }
    if (fe->suppressed) app(&x, " [suppressed]");
}

CadFeatStatus cad_features_at(const CadFeatures *f, unsigned index,
                              CadFeatInfo *info)
{
    const CadFeatureData *fe;
    if (f == 0 || info == 0) return CAD_FEAT_ARGUMENT;
    if (index >= f->count) return CAD_FEAT_NOT_FOUND;
    fe = &f->items[index];
    memset(info, 0, sizeof(*info));
    info->id = fe->id;
    info->index = index;
    info->type = (CadFeatType)fe->type;
    info->suppressed = fe->suppressed;
    info->name = fe->name;
    info->offset = "";
    info->distance = "";
    info->profile = CAD_FEAT_ALL;
    if (fe->type == CAD_FEAT_SKETCH) {
        info->plane = (CadFeatPlane)fe->plane;
        info->offset = fe->expr;
        info->profile_count = profile_count_of(f, fe->id);
    } else {
        info->sketch_id = fe->sketch_id;
        info->profile = fe->profile;
        if (fe->type == CAD_FEAT_EXTRUDE) {
            info->direction = (CadFeatDirection)fe->direction;
            info->distance = fe->expr;
        } else {
            info->axis = (CadFeatAxis)fe->axis;
            info->segments = fe->segments;
        }
    }
    describe(f, fe, info->description);
    return CAD_FEAT_OK;
}

CadFeatStatus cad_features_get(const CadFeatures *f, unsigned id,
                               CadFeatInfo *info)
{
    int i;
    if (f == 0 || info == 0) return CAD_FEAT_ARGUMENT;
    i = index_of(f, id);
    if (i < 0) return CAD_FEAT_NOT_FOUND;
    return cad_features_at(f, (unsigned)i, info);
}

CadFeatStatus cad_features_profile(const CadFeatures *f, unsigned sketch_id,
                                   unsigned index, CadFeatProfileInfo *info)
{
    const CadFeatProfileData *p;
    unsigned k;
    int slot;
    if (f == 0 || info == 0) return CAD_FEAT_ARGUMENT;
    slot = profile_slot(f, sketch_id, index);
    if (slot < 0) return CAD_FEAT_NOT_FOUND;
    p = &f->profiles[slot];
    memset(info, 0, sizeof(*info));
    info->kind = (CadFeatProfileKind)p->kind;
    info->sides = p->kind == CAD_FEAT_CIRCLE ? p->sides : 0U;
    info->expr_count = p->expr_count;
    for (k = 0; k < p->expr_count; ++k) info->exprs[k] = profile_expr(p, k);
    return CAD_FEAT_OK;
}

/* ---- mutations (all operate on cand) ---- */

static CadFeatStatus add_common(CadFeatType type, const char *name,
                                unsigned *slot)
{
    static const char *const bases[4] = { "", "Sketch", "Extrusion", "Revolution" };
    CadFeatureData *fe;
    unsigned n;
    if (cand.count >= CAD_FEAT_MAX || cand.next_id >= 65535U)
        return fail(CAD_FEAT_FULL, 0, CAD_FEAT_FIELD_NONE, -1, -1);
    fe = &cand.items[cand.count];
    memset(fe, 0, sizeof(*fe));
    if (name != 0) {
        if (!valid_name(name))
            return fail(CAD_FEAT_NAME, 0, CAD_FEAT_FIELD_NAME, -1, -1);
        if (name_taken(&cand, name, -1))
            return fail(CAD_FEAT_DUPLICATE, cad_features_find(&cand, name),
                        CAD_FEAT_FIELD_NAME, -1, -1);
        strcpy(fe->name, name);
    } else {
        for (n = 1; ; ++n) {
            strcpy(fe->name, bases[type]);
            put_unsigned(fe->name + strlen(fe->name), n);
            if (!name_taken(&cand, fe->name, -1)) break;
        }
    }
    fe->id = cand.next_id++;
    fe->type = (unsigned char)type;
    fe->profile = CAD_FEAT_ALL;
    *slot = cand.count++;
    return CAD_FEAT_OK;
}

/* Sketch reference valid for a feature at list position `slot`? */
static CadFeatStatus check_ref(unsigned self, int slot, unsigned sketch_id,
                               int profile)
{
    int s = index_of(&cand, sketch_id);
    if (s < 0 || cand.items[s].type != CAD_FEAT_SKETCH || s >= slot)
        return fail(CAD_FEAT_REFERENCE, self, CAD_FEAT_FIELD_SKETCH, -1, -1);
    if (profile != CAD_FEAT_ALL &&
        (profile < 0 || (unsigned)profile >= profile_count_of(&cand, sketch_id)))
        return fail(CAD_FEAT_REFERENCE, self, CAD_FEAT_FIELD_PROFILE, profile, -1);
    return CAD_FEAT_OK;
}

static CadFeatStatus set_sketch(unsigned slot, CadFeatPlane plane,
                                const char *offset)
{
    CadFeatureData *fe = &cand.items[slot];
    if ((int)plane < 0 || (int)plane > (int)CAD_FEAT_YZ)
        return fail(CAD_FEAT_ARGUMENT, fe->id, CAD_FEAT_FIELD_PLANE, -1, -1);
    if (!valid_text(offset, CAD_FEAT_EXPR_MAX))
        return fail(CAD_FEAT_EXPRESSION, fe->id, CAD_FEAT_FIELD_OFFSET, -1, -1);
    fe->plane = (unsigned char)plane;
    strcpy(fe->expr, offset);
    return CAD_FEAT_OK;
}

static CadFeatStatus set_extrude(unsigned slot, unsigned sketch_id, int profile,
                                 CadFeatDirection dir, const char *distance)
{
    CadFeatureData *fe = &cand.items[slot];
    CadFeatStatus s;
    if ((int)dir < 0 || (int)dir > (int)CAD_FEAT_SYMMETRIC)
        return fail(CAD_FEAT_ARGUMENT, fe->id, CAD_FEAT_FIELD_DIRECTION, -1, -1);
    if (!valid_text(distance, CAD_FEAT_EXPR_MAX))
        return fail(CAD_FEAT_EXPRESSION, fe->id, CAD_FEAT_FIELD_DISTANCE, -1, -1);
    s = check_ref(fe->id, (int)slot, sketch_id, profile);
    if (s != CAD_FEAT_OK) return s;
    fe->sketch_id = sketch_id;
    fe->profile = profile;
    fe->direction = (unsigned char)dir;
    strcpy(fe->expr, distance);
    return CAD_FEAT_OK;
}

static CadFeatStatus set_revolve(unsigned slot, unsigned sketch_id, int profile,
                                 CadFeatAxis axis, unsigned segments)
{
    CadFeatureData *fe = &cand.items[slot];
    CadFeatStatus s;
    if ((int)axis < 0 || (int)axis > (int)CAD_FEAT_AXIS_V)
        return fail(CAD_FEAT_ARGUMENT, fe->id, CAD_FEAT_FIELD_AXIS, -1, -1);
    if (segments < CAD_FEAT_REVOLVE_MIN || segments > CAD_FEAT_REVOLVE_MAX)
        return fail(CAD_FEAT_ARGUMENT, fe->id, CAD_FEAT_FIELD_SEGMENTS, -1, -1);
    s = check_ref(fe->id, (int)slot, sketch_id, profile);
    if (s != CAD_FEAT_OK) return s;
    fe->sketch_id = sketch_id;
    fe->profile = profile;
    fe->axis = (unsigned char)axis;
    fe->segments = segments;
    return CAD_FEAT_OK;
}

/* Slot of an existing feature of the given type (0 = any). */
static CadFeatStatus find_slot(unsigned id, int type, unsigned *slot)
{
    int i = index_of(&cand, id);
    if (i < 0) return fail(CAD_FEAT_NOT_FOUND, id, CAD_FEAT_FIELD_NONE, -1, -1);
    if (type != 0 && cand.items[i].type != type)
        return fail(CAD_FEAT_ARGUMENT, id, CAD_FEAT_FIELD_NONE, -1, -1);
    *slot = (unsigned)i;
    return CAD_FEAT_OK;
}

CadFeatStatus cad_features_add_sketch(CadFeatures *f, const char *name,
    CadFeatPlane plane, const char *offset, unsigned *id, CadFeatError *e)
{
    CadFeatStatus s;
    unsigned slot;
    if (f == 0) return bad_argument(e);
    begin(f);
    s = add_common(CAD_FEAT_SKETCH, name, &slot);
    if (s == CAD_FEAT_OK) s = set_sketch(slot, plane, offset);
    s = finish(f, s, e);
    if (s == CAD_FEAT_OK && id != 0) *id = f->items[slot].id;
    return s;
}

CadFeatStatus cad_features_edit_sketch(CadFeatures *f, unsigned id,
    CadFeatPlane plane, const char *offset, CadFeatError *e)
{
    CadFeatStatus s;
    unsigned slot;
    if (f == 0) return bad_argument(e);
    begin(f);
    s = find_slot(id, CAD_FEAT_SKETCH, &slot);
    if (s == CAD_FEAT_OK) s = set_sketch(slot, plane, offset);
    return finish(f, s, e);
}

CadFeatStatus cad_features_add_extrude(CadFeatures *f, const char *name,
    unsigned sketch_id, int profile, CadFeatDirection dir,
    const char *distance, unsigned *id, CadFeatError *e)
{
    CadFeatStatus s;
    unsigned slot;
    if (f == 0) return bad_argument(e);
    begin(f);
    s = add_common(CAD_FEAT_EXTRUDE, name, &slot);
    if (s == CAD_FEAT_OK) s = set_extrude(slot, sketch_id, profile, dir, distance);
    s = finish(f, s, e);
    if (s == CAD_FEAT_OK && id != 0) *id = f->items[slot].id;
    return s;
}

CadFeatStatus cad_features_edit_extrude(CadFeatures *f, unsigned id,
    unsigned sketch_id, int profile, CadFeatDirection dir,
    const char *distance, CadFeatError *e)
{
    CadFeatStatus s;
    unsigned slot;
    if (f == 0) return bad_argument(e);
    begin(f);
    s = find_slot(id, CAD_FEAT_EXTRUDE, &slot);
    if (s == CAD_FEAT_OK) s = set_extrude(slot, sketch_id, profile, dir, distance);
    return finish(f, s, e);
}

CadFeatStatus cad_features_add_revolve(CadFeatures *f, const char *name,
    unsigned sketch_id, int profile, CadFeatAxis axis, unsigned segments,
    unsigned *id, CadFeatError *e)
{
    CadFeatStatus s;
    unsigned slot;
    if (f == 0) return bad_argument(e);
    begin(f);
    s = add_common(CAD_FEAT_REVOLVE, name, &slot);
    if (s == CAD_FEAT_OK) s = set_revolve(slot, sketch_id, profile, axis, segments);
    s = finish(f, s, e);
    if (s == CAD_FEAT_OK && id != 0) *id = f->items[slot].id;
    return s;
}

CadFeatStatus cad_features_edit_revolve(CadFeatures *f, unsigned id,
    unsigned sketch_id, int profile, CadFeatAxis axis, unsigned segments,
    CadFeatError *e)
{
    CadFeatStatus s;
    unsigned slot;
    if (f == 0) return bad_argument(e);
    begin(f);
    s = find_slot(id, CAD_FEAT_REVOLVE, &slot);
    if (s == CAD_FEAT_OK) s = set_revolve(slot, sketch_id, profile, axis, segments);
    return finish(f, s, e);
}

/* Validates and stores a profile into pool slot `slot` of cand. */
static CadFeatStatus pack_profile(unsigned slot, unsigned owner, int pindex,
    CadFeatProfileKind kind, const char *const *exprs, unsigned count,
    unsigned sides)
{
    CadFeatProfileData *p = &cand.profiles[slot];
    unsigned i, total = 0, len;
    int ok;
    if (kind == CAD_FEAT_RECT) ok = count == 4U;
    else if (kind == CAD_FEAT_CIRCLE) ok = count == 3U;
    else if (kind == CAD_FEAT_POLY)
        ok = count % 2U == 0U && count >= 2U * CAD_FEAT_POLY_MIN &&
             count <= 2U * CAD_FEAT_POLY_MAX;
    else ok = 0;
    if (!ok || exprs == 0)
        return fail(CAD_FEAT_ARGUMENT, owner, CAD_FEAT_FIELD_PROFILE, pindex, -1);
    if (kind == CAD_FEAT_CIRCLE) {
        if (sides == 0U) sides = CAD_FEAT_CIRCLE_DEFAULT;
        if (sides < CAD_FEAT_CIRCLE_MIN || sides > CAD_FEAT_CIRCLE_MAX)
            return fail(CAD_FEAT_ARGUMENT, owner, CAD_FEAT_FIELD_SIDES, pindex, -1);
    } else if (sides != 0U) {
        return fail(CAD_FEAT_ARGUMENT, owner, CAD_FEAT_FIELD_SIDES, pindex, -1);
    }
    for (i = 0; i < count; ++i) {
        if (!valid_text(exprs[i], CAD_FEAT_EXPR_MAX))
            return fail(CAD_FEAT_EXPRESSION, owner, CAD_FEAT_FIELD_PROFILE,
                        pindex, (int)i);
        total += (unsigned)strlen(exprs[i]) + 1U;
        if (total > CAD_FEAT_PROFILE_TEXT)
            return fail(CAD_FEAT_CAPACITY, owner, CAD_FEAT_FIELD_PROFILE,
                        pindex, (int)i);
    }
    memset(p, 0, sizeof(*p));
    p->owner = owner;
    p->kind = (unsigned char)kind;
    p->sides = (unsigned char)sides;
    p->expr_count = (unsigned char)count;
    for (i = 0, total = 0; i < count; ++i) {
        len = (unsigned)strlen(exprs[i]) + 1U;
        memcpy(p->text + total, exprs[i], len);
        total += len;
    }
    return CAD_FEAT_OK;
}

CadFeatStatus cad_features_add_profile(CadFeatures *f, unsigned sketch_id,
    CadFeatProfileKind kind, const char *const *exprs, unsigned count,
    unsigned sides, unsigned *index, CadFeatError *e)
{
    CadFeatStatus s;
    unsigned slot, n = 0;
    if (f == 0) return bad_argument(e);
    begin(f);
    s = find_slot(sketch_id, CAD_FEAT_SKETCH, &slot);
    if (s == CAD_FEAT_OK) {
        n = profile_count_of(&cand, sketch_id);
        if (n >= CAD_FEAT_SKETCH_PROFILES || cand.profile_count >= CAD_FEAT_MAX_PROFILES)
            s = fail(CAD_FEAT_FULL, sketch_id, CAD_FEAT_FIELD_PROFILE, (int)n, -1);
    }
    if (s == CAD_FEAT_OK)
        s = pack_profile(cand.profile_count, sketch_id, (int)n, kind, exprs,
                         count, sides);
    if (s == CAD_FEAT_OK) ++cand.profile_count;
    s = finish(f, s, e);
    if (s == CAD_FEAT_OK && index != 0) *index = n;
    return s;
}

CadFeatStatus cad_features_edit_profile(CadFeatures *f, unsigned sketch_id,
    unsigned index, CadFeatProfileKind kind, const char *const *exprs,
    unsigned count, unsigned sides, CadFeatError *e)
{
    CadFeatStatus s;
    unsigned slot;
    int ps;
    if (f == 0) return bad_argument(e);
    begin(f);
    s = find_slot(sketch_id, CAD_FEAT_SKETCH, &slot);
    if (s == CAD_FEAT_OK) {
        ps = profile_slot(&cand, sketch_id, index);
        if (ps < 0)
            s = fail(CAD_FEAT_NOT_FOUND, sketch_id, CAD_FEAT_FIELD_PROFILE,
                     (int)index, -1);
        else
            s = pack_profile((unsigned)ps, sketch_id, (int)index, kind, exprs,
                             count, sides);
    }
    return finish(f, s, e);
}

static void drop_profile(unsigned ps)
{
    unsigned i;
    for (i = ps; i + 1U < cand.profile_count; ++i)
        cand.profiles[i] = cand.profiles[i + 1U];
    --cand.profile_count;
    memset(&cand.profiles[cand.profile_count], 0, sizeof(cand.profiles[0]));
}

CadFeatStatus cad_features_remove_profile(CadFeatures *f, unsigned sketch_id,
    unsigned index, CadFeatError *e)
{
    CadFeatStatus s;
    unsigned slot, i;
    int ps = -1;
    if (f == 0) return bad_argument(e);
    begin(f);
    s = find_slot(sketch_id, CAD_FEAT_SKETCH, &slot);
    if (s == CAD_FEAT_OK) {
        ps = profile_slot(&cand, sketch_id, index);
        if (ps < 0)
            s = fail(CAD_FEAT_NOT_FOUND, sketch_id, CAD_FEAT_FIELD_PROFILE,
                     (int)index, -1);
    }
    for (i = 0; s == CAD_FEAT_OK && i < cand.count; ++i) {
        CadFeatureData *fe = &cand.items[i];
        if (fe->type == CAD_FEAT_SKETCH || fe->sketch_id != sketch_id ||
            fe->profile == CAD_FEAT_ALL) continue;
        if (fe->profile == (int)index)
            s = fail(CAD_FEAT_IN_USE, fe->id, CAD_FEAT_FIELD_PROFILE, (int)index, -1);
        else if (fe->profile > (int)index)
            --fe->profile;
    }
    if (s == CAD_FEAT_OK) drop_profile((unsigned)ps);
    return finish(f, s, e);
}

CadFeatStatus cad_features_rename(CadFeatures *f, unsigned id,
                                  const char *name, CadFeatError *e)
{
    CadFeatStatus s;
    unsigned slot;
    if (f == 0) return bad_argument(e);
    begin(f);
    s = find_slot(id, 0, &slot);
    if (s == CAD_FEAT_OK && !valid_name(name))
        s = fail(CAD_FEAT_NAME, id, CAD_FEAT_FIELD_NAME, -1, -1);
    if (s == CAD_FEAT_OK && name_taken(&cand, name, (int)slot))
        s = fail(CAD_FEAT_DUPLICATE, cad_features_find(&cand, name),
                 CAD_FEAT_FIELD_NAME, -1, -1);
    if (s == CAD_FEAT_OK) strcpy(cand.items[slot].name, name);
    return finish(f, s, e);
}

CadFeatStatus cad_features_suppress(CadFeatures *f, unsigned id,
                                    int suppressed, CadFeatError *e)
{
    CadFeatStatus s;
    unsigned slot;
    if (f == 0) return bad_argument(e);
    begin(f);
    s = find_slot(id, 0, &slot);
    if (s == CAD_FEAT_OK) cand.items[slot].suppressed = (unsigned char)(suppressed != 0);
    return finish(f, s, e);
}

CadFeatStatus cad_features_remove(CadFeatures *f, unsigned id, CadFeatError *e)
{
    CadFeatStatus s;
    unsigned slot, i;
    if (f == 0) return bad_argument(e);
    begin(f);
    s = find_slot(id, 0, &slot);
    for (i = 0; s == CAD_FEAT_OK && i < cand.count; ++i)
        if (cand.items[i].type != CAD_FEAT_SKETCH && cand.items[i].sketch_id == id)
            s = fail(CAD_FEAT_IN_USE, cand.items[i].id, CAD_FEAT_FIELD_NONE, -1, -1);
    if (s == CAD_FEAT_OK) {
        for (i = cand.profile_count; i-- > 0U; )
            if (cand.profiles[i].owner == id) drop_profile(i);
        for (i = slot; i + 1U < cand.count; ++i) cand.items[i] = cand.items[i + 1U];
        --cand.count;
        memset(&cand.items[cand.count], 0, sizeof(cand.items[0]));
    }
    return finish(f, s, e);
}

static CadFeatStatus move(CadFeatures *f, unsigned id, int down, CadFeatError *e)
{
    CadFeatStatus s;
    unsigned slot, a = 0;
    CadFeatureData t;
    if (f == 0) return bad_argument(e);
    begin(f);
    s = find_slot(id, 0, &slot);
    if (s == CAD_FEAT_OK) {
        if (down ? slot + 1U >= cand.count : slot == 0U)
            s = fail(CAD_FEAT_ARGUMENT, id, CAD_FEAT_FIELD_NONE, -1, -1);
        else
            a = down ? slot : slot - 1U;   /* swap a and a+1 */
    }
    if (s == CAD_FEAT_OK && cand.items[a + 1U].type != CAD_FEAT_SKETCH &&
        cand.items[a + 1U].sketch_id == cand.items[a].id)
        s = fail(CAD_FEAT_ORDER, down ? cand.items[a + 1U].id : cand.items[a].id,
                 CAD_FEAT_FIELD_SKETCH, -1, -1);
    if (s == CAD_FEAT_OK) {
        t = cand.items[a];
        cand.items[a] = cand.items[a + 1U];
        cand.items[a + 1U] = t;
    }
    return finish(f, s, e);
}
CadFeatStatus cad_features_move_up(CadFeatures *f, unsigned id, CadFeatError *e)
{
    return move(f, id, 0, e);
}
CadFeatStatus cad_features_move_down(CadFeatures *f, unsigned id, CadFeatError *e)
{
    return move(f, id, 1, e);
}

/* ---- regeneration ---- */

static CadSketchMesh rg_mesh;
static CadSketchPoint rg_pts[CAD_FEAT_MAX_PROFILES][CAD_FEAT_CIRCLE_MAX];
static unsigned char rg_n[CAD_FEAT_MAX_PROFILES];
static long rg_area[CAD_FEAT_MAX_PROFILES];
static long rg_off[CAD_FEAT_MAX];
static long rg_val[CAD_FEAT_PROFILE_EXPRS];
static CadMeshViewVertex rg_v[REV_VERTS];
static CadMeshViewFace rg_f[2 * REV_VERTS];
static CadFeatOutput *rg_out;
static CadFeatHealth *rg_h;
static unsigned rg_fid, rg_plane, rg_material;

static long sin_q14(long a)
{
    long v, frac;
    int idx, neg = 0;
    a %= TURN;
    if (a < 0L) a += TURN;
    if (a >= TURN / 2L) { neg = 1; a -= TURN / 2L; }
    if (a > TURN / 4L) a = TURN / 2L - a;
    idx = (int)(a / 64L);
    frac = a % 64L;
    v = sin_table[idx];
    if (frac != 0L)
        v += ((long)(sin_table[idx + 1] - sin_table[idx]) * frac + 32L) / 64L;
    return neg ? -v : v;
}
static long cos_q14(long a) { return sin_q14(a + TURN / 4L); }
static long scale_q14(long r, long q)
{
    long p = r * q;
    return p >= 0L ? (p + 8192L) / 16384L : -((-p + 8192L) / 16384L);
}
static long step_angle(unsigned k, unsigned n)
{
    return ((long)k * TURN + (long)(n / 2U)) / (long)n;
}

static int h_fail(CadFeatStatus s, CadFeatField field, int profile, int expr,
                  const char *message)
{
    rg_h->state = CAD_FEAT_STATE_ERROR;
    rg_h->error = s;
    rg_h->field = field;
    rg_h->profile = profile;
    rg_h->expr_index = expr;
    rg_h->message = message;
    return 0;
}
static int eval(CadUserParams *params, const char *text, long *value,
                CadFeatField field, int profile, int expr)
{
    CadUserParamError pe;
    if (cad_userparams_eval_expression(params, text, value, &pe) == CAD_UP_OK)
        return 1;
    rg_h->param_status = pe.status;
    rg_h->offset = pe.offset;
    return h_fail(CAD_FEAT_EVAL, field, profile, expr,
                  pe.message ? pe.message : "expression error");
}

static const char *sketch_message(CadSketchStatus s)
{
    switch (s) {
    case CAD_SKETCH_RANGE: return "profile outside -400..400";
    case CAD_SKETCH_ZERO_EDGE: return "profile has a zero-length edge";
    case CAD_SKETCH_DUPLICATE: return "profile repeats a point";
    case CAD_SKETCH_INTERSECTION: return "profile intersects itself";
    case CAD_SKETCH_ZERO_AREA: return "profile has zero area";
    default: break;
    }
    return "invalid profile";
}

static int put_pt(CadSketchPoint *p, long x, long y, int pindex)
{
    if (x < -LIMIT || x > LIMIT || y < -LIMIT || y > LIMIT)
        return h_fail(CAD_FEAT_RANGE, CAD_FEAT_FIELD_PROFILE, pindex, -1,
                      "profile outside -400..400");
    p->x = (int)x;
    p->y = (int)y;
    return 1;
}

/* Evaluates profile `pindex` (pool slot ps) into rg_pts[ps]. */
static int eval_profile(CadUserParams *params, const CadFeatProfileData *p,
                        int pindex, unsigned ps)
{
    CadSketchPoint *pts = rg_pts[ps];
    unsigned i, n = 0;
    long x0, y0, *v = rg_val;
    CadSketchStatus st;
    for (i = 0; i < p->expr_count; ++i)
        if (!eval(params, profile_expr(p, i), &v[i], CAD_FEAT_FIELD_PROFILE,
                  pindex, (int)i)) return 0;
    if (p->kind == CAD_FEAT_RECT) {
        if (v[2] < 1L)
            return h_fail(CAD_FEAT_RANGE, CAD_FEAT_FIELD_PROFILE, pindex, 2,
                          "width must be positive");
        if (v[3] < 1L)
            return h_fail(CAD_FEAT_RANGE, CAD_FEAT_FIELD_PROFILE, pindex, 3,
                          "height must be positive");
        x0 = v[0] - v[2] / 2L;
        y0 = v[1] - v[3] / 2L;
        n = 4;
        if (!put_pt(&pts[0], x0, y0, pindex) ||
            !put_pt(&pts[1], x0 + v[2], y0, pindex) ||
            !put_pt(&pts[2], x0 + v[2], y0 + v[3], pindex) ||
            !put_pt(&pts[3], x0, y0 + v[3], pindex)) return 0;
    } else if (p->kind == CAD_FEAT_CIRCLE) {
        if (v[2] < 1L)
            return h_fail(CAD_FEAT_RANGE, CAD_FEAT_FIELD_PROFILE, pindex, 2,
                          "radius must be positive");
        n = p->sides;
        for (i = 0; i < n; ++i) {
            long a = step_angle(i, n);
            if (!put_pt(&pts[i], v[0] + scale_q14(v[2], cos_q14(a)),
                        v[1] + scale_q14(v[2], sin_q14(a)), pindex)) return 0;
        }
    } else {
        n = p->expr_count / 2U;
        for (i = 0; i < n; ++i)
            if (!put_pt(&pts[i], v[2U * i], v[2U * i + 1U], pindex)) return 0;
    }
    st = cad_sketch_validate(pts, (int)n, &rg_area[ps]);
    if (st != CAD_SKETCH_OK)
        return h_fail(CAD_FEAT_GEOMETRY, CAD_FEAT_FIELD_PROFILE, pindex, -1,
                      sketch_message(st));
    rg_n[ps] = (unsigned char)n;
    return 1;
}

/* Sketch (u, v) at normal distance n to world; see header. */
static void map_point(long u, long v, long n, long w[3])
{
    if (rg_plane == CAD_FEAT_XY) { w[0] = u; w[1] = v; w[2] = n; }
    else if (rg_plane == CAD_FEAT_XZ) { w[0] = u; w[1] = n; w[2] = -v; }
    else { w[0] = n; w[1] = u; w[2] = v; }
}
static void seg(unsigned char kind, const long a[3], const long b[3])
{
    CadFeatSegment *s;
    if (rg_out->segment_count >= CAD_FEAT_OUT_SEGMENTS) {
        rg_out->wireframe_truncated = 1;
        return;
    }
    s = &rg_out->segments[rg_out->segment_count++];
    memcpy(s->a, a, sizeof(s->a));
    memcpy(s->b, b, sizeof(s->b));
    s->feature_id = rg_fid;
    s->kind = kind;
}
static void seg_local(unsigned char kind, const CadSketchPoint *p,
                      const CadSketchPoint *q, long n0, long n1)
{
    long a[3], b[3];
    map_point(p->x, p->y, n0, a);
    map_point(q->x, q->y, n1, b);
    seg(kind, a, b);
}
static void seg_vertex(const CadMeshViewVertex *p, const CadMeshViewVertex *q)
{
    long a[3], b[3];
    a[0] = p->x; a[1] = p->y; a[2] = p->z;
    b[0] = q->x; b[1] = q->y; b[2] = q->z;
    seg(CAD_FEAT_SEG_EDGE, a, b);
}
/* Adds one body's shading atomically, or flags truncation. */
static void emit_body(unsigned nv, unsigned nf)
{
    CadFeatOutput *o = rg_out;
    unsigned i, base = o->vertex_count;
    if (nv > CAD_FEAT_OUT_VERTICES - o->vertex_count ||
        nf > CAD_FEAT_OUT_FACES - o->face_count) {
        o->shading_truncated = 1;
        return;
    }
    for (i = 0; i < nv; ++i) o->vertices[base + i] = rg_v[i];
    for (i = 0; i < nf; ++i) {
        CadMeshViewFace *d = &o->faces[o->face_count++];
        d->a = (unsigned short)(rg_f[i].a + base);
        d->b = (unsigned short)(rg_f[i].b + base);
        d->c = (unsigned short)(rg_f[i].c + base);
        d->material = (unsigned short)rg_material;
    }
    o->vertex_count += nv;
}

static void regen_sketch(const CadFeatures *f, CadUserParams *params,
                         unsigned slot)
{
    const CadFeatureData *fe = &f->items[slot];
    unsigned ps, i, n;
    int pi = 0;
    if (!eval(params, fe->expr, &rg_off[slot], CAD_FEAT_FIELD_OFFSET, -1, -1))
        return;
    for (ps = 0; ps < f->profile_count; ++ps)
        if (f->profiles[ps].owner == fe->id)
            if (!eval_profile(params, &f->profiles[ps], pi++, ps)) return;
    for (ps = 0; ps < f->profile_count; ++ps) {
        if (f->profiles[ps].owner != fe->id) continue;
        n = rg_n[ps];
        for (i = 0; i < n; ++i)
            seg_local(CAD_FEAT_SEG_SKETCH, &rg_pts[ps][i],
                      &rg_pts[ps][(i + 1U) % n], rg_off[slot], rg_off[slot]);
    }
}

/* Next selected profile pool slot after *ps (start with -1); index in *pi. */
static int next_profile(const CadFeatures *f, const CadFeatureData *fe,
                        int *ps, int *pi)
{
    while (++*ps < (int)f->profile_count) {
        if (f->profiles[*ps].owner != fe->sketch_id) continue;
        ++*pi;
        if (fe->profile == CAD_FEAT_ALL || fe->profile == *pi) return 1;
    }
    return 0;
}

static void regen_extrude(const CadFeatures *f, CadUserParams *params,
                          const CadFeatureData *fe, long off)
{
    long d, base, w[3];
    int ps = -1, pi = -1, any = 0;
    unsigned i, n;
    CadSketchStatus st;
    if (!eval(params, fe->expr, &d, CAD_FEAT_FIELD_DISTANCE, -1, -1)) return;
    if (d < 1L) {
        h_fail(CAD_FEAT_RANGE, CAD_FEAT_FIELD_DISTANCE, -1, -1,
               "distance must be 1..400");
        return;
    }
    base = fe->direction == CAD_FEAT_POSITIVE ? off :
           fe->direction == CAD_FEAT_NEGATIVE ? off - d : off - d / 2L;
    if (base < -LIMIT || base + d > LIMIT) {
        h_fail(CAD_FEAT_RANGE, CAD_FEAT_FIELD_DISTANCE, -1, -1,
               "body outside model range -400..400");
        return;
    }
    while (next_profile(f, fe, &ps, &pi)) {
        const CadSketchPoint *pts = rg_pts[ps];
        any = 1;
        n = rg_n[ps];
        st = cad_sketch_extrude(pts, (int)n, (int)d, &rg_mesh);
        if (st != CAD_SKETCH_OK) {
            h_fail(CAD_FEAT_GEOMETRY, CAD_FEAT_FIELD_PROFILE, pi, -1,
                   sketch_message(st));
            return;
        }
        for (i = 0; i < n; ++i) {
            const CadSketchPoint *q = &pts[(i + 1U) % n];
            seg_local(CAD_FEAT_SEG_EDGE, &pts[i], q, base, base);
            seg_local(CAD_FEAT_SEG_EDGE, &pts[i], q, base + d, base + d);
            seg_local(CAD_FEAT_SEG_EDGE, &pts[i], &pts[i], base, base + d);
        }
        for (i = 0; i < rg_mesh.vertex_count; ++i) {
            const CadSketchVertex *v = &rg_mesh.vertices[i];
            map_point(v->x, v->y, base + v->z, w);
            rg_v[i].x = (int)w[0]; rg_v[i].y = (int)w[1]; rg_v[i].z = (int)w[2];
        }
        for (i = 0; i < rg_mesh.triangle_count; ++i) {
            rg_f[i].a = rg_mesh.triangles[i].a;
            rg_f[i].b = rg_mesh.triangles[i].b;
            rg_f[i].c = rg_mesh.triangles[i].c;
        }
        emit_body(rg_mesh.vertex_count, rg_mesh.triangle_count);
    }
    if (!any)
        h_fail(CAD_FEAT_GEOMETRY, CAD_FEAT_FIELD_PROFILE, -1, -1,
               "sketch has no profiles");
}

static int degenerate(const CadMeshViewFace *t)
{
    const CadMeshViewVertex *a = &rg_v[t->a], *b = &rg_v[t->b], *c = &rg_v[t->c];
    long ux = (long)b->x - a->x, uy = (long)b->y - a->y, uz = (long)b->z - a->z;
    long vx = (long)c->x - a->x, vy = (long)c->y - a->y, vz = (long)c->z - a->z;
    return uy * vz - uz * vy == 0L && uz * vx - ux * vz == 0L &&
           ux * vy - uy * vx == 0L;
}

static void regen_revolve(const CadFeatures *f, const CadFeatureData *fe,
                          long off)
{
    int ps = -1, pi = -1, any = 0, flip;
    unsigned i, k, m, s = fe->segments, nv, nf, a, b, c, d;
    long r, along, cs, sn, w[3];
    while (next_profile(f, fe, &ps, &pi)) {
        const CadSketchPoint *pts = rg_pts[ps];
        any = 1;
        m = rg_n[ps];
        for (i = 0; i < m; ++i) {
            r = fe->axis == CAD_FEAT_AXIS_U ? pts[i].y : pts[i].x;
            if (r == 0L || (r < 0L) != ((fe->axis == CAD_FEAT_AXIS_U ?
                                         pts[0].y : pts[0].x) < 0)) {
                h_fail(CAD_FEAT_GEOMETRY, CAD_FEAT_FIELD_AXIS, pi, -1,
                       "profile touches or crosses the revolve axis");
                return;
            }
        }
        flip = (rg_area[ps] < 0L) != ((fe->axis == CAD_FEAT_AXIS_U ?
                                       pts[0].y : pts[0].x) < 0);
        for (k = 0; k < s; ++k) {
            cs = cos_q14(step_angle(k, s));
            sn = sin_q14(step_angle(k, s));
            for (i = 0; i < m; ++i) {
                CadMeshViewVertex *v = &rg_v[k * m + i];
                if (fe->axis == CAD_FEAT_AXIS_U) {
                    r = pts[i].y; along = pts[i].x;
                    map_point(along, scale_q14(r, cs), off + scale_q14(r, sn), w);
                } else {
                    r = pts[i].x; along = pts[i].y;
                    map_point(scale_q14(r, cs), along, off + scale_q14(r, sn), w);
                }
                if (w[0] < -LIMIT || w[0] > LIMIT || w[1] < -LIMIT ||
                    w[1] > LIMIT || w[2] < -LIMIT || w[2] > LIMIT) {
                    h_fail(CAD_FEAT_RANGE, CAD_FEAT_FIELD_PROFILE, pi, -1,
                           "body outside model range -400..400");
                    return;
                }
                v->x = (int)w[0]; v->y = (int)w[1]; v->z = (int)w[2];
            }
        }
        nv = m * s;
        nf = 0;
        for (k = 0; k < s; ++k) {
            for (i = 0; i < m; ++i) {
                a = k * m + i;
                b = k * m + (i + 1U) % m;
                c = ((k + 1U) % s) * m + (i + 1U) % m;
                d = ((k + 1U) % s) * m + i;
                rg_f[nf].a = (unsigned short)a;
                rg_f[nf].b = (unsigned short)(flip ? c : b);
                rg_f[nf].c = (unsigned short)(flip ? b : c);
                rg_f[nf + 1U].a = (unsigned short)a;
                rg_f[nf + 1U].b = (unsigned short)(flip ? d : c);
                rg_f[nf + 1U].c = (unsigned short)(flip ? c : d);
                if (degenerate(&rg_f[nf]) || degenerate(&rg_f[nf + 1U])) {
                    h_fail(CAD_FEAT_GEOMETRY, CAD_FEAT_FIELD_SEGMENTS, pi, -1,
                           "revolve too small for its segment count");
                    return;
                }
                nf += 2U;
            }
        }
        for (k = 0; k < s; ++k)
            for (i = 0; i < m; ++i) {
                a = k * m + i;
                seg_vertex(&rg_v[a], &rg_v[k * m + (i + 1U) % m]);
                seg_vertex(&rg_v[a], &rg_v[((k + 1U) % s) * m + i]);
            }
        emit_body(nv, nf);
    }
    if (!any)
        h_fail(CAD_FEAT_GEOMETRY, CAD_FEAT_FIELD_PROFILE, -1, -1,
               "sketch has no profiles");
}

CadFeatStatus cad_features_regenerate(const CadFeatures *f,
    CadUserParams *params, CadFeatOutput *out, unsigned *error_count)
{
    unsigned slot, errors = 0, i, seg0, v0, f0;
    int wt, st, s, k;
    if (error_count != 0) *error_count = 0;
    if (f == 0 || params == 0 || out == 0) return CAD_FEAT_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->feature_count = f->count;
    rg_out = out;
    for (slot = 0; slot < f->count; ++slot) {
        const CadFeatureData *fe = &f->items[slot];
        CadFeatHealth *h = &out->health[slot];
        rg_h = h;
        h->feature_id = fe->id;
        h->state = CAD_FEAT_STATE_OK;
        h->error = CAD_FEAT_OK;
        h->field = CAD_FEAT_FIELD_NONE;
        h->profile = -1;
        h->expr_index = -1;
        h->param_status = CAD_UP_OK;
        h->message = "";
        seg0 = out->segment_count; v0 = out->vertex_count; f0 = out->face_count;
        wt = out->wireframe_truncated; st = out->shading_truncated;
        rg_fid = fe->id;
        rg_material = slot % 6U;
        if (fe->suppressed) {
            h->state = CAD_FEAT_STATE_SUPPRESSED;
            h->message = "suppressed";
        } else if (fe->type == CAD_FEAT_SKETCH) {
            rg_plane = fe->plane;
            regen_sketch(f, params, slot);
        } else {
            s = index_of(f, fe->sketch_id);
            if (s < 0 || (unsigned)s >= slot || f->items[s].type != CAD_FEAT_SKETCH) {
                h_fail(CAD_FEAT_REFERENCE, CAD_FEAT_FIELD_SKETCH, -1, -1,
                       "invalid sketch reference");
            } else if (out->health[s].state != CAD_FEAT_STATE_OK) {
                h->state = CAD_FEAT_STATE_BLOCKED;
                h->blocked_by = fe->sketch_id;
                h->field = CAD_FEAT_FIELD_SKETCH;
                h->message = out->health[s].state == CAD_FEAT_STATE_SUPPRESSED ?
                    "sketch is suppressed" : "sketch failed";
            } else {
                rg_plane = f->items[s].plane;
                if (fe->type == CAD_FEAT_EXTRUDE)
                    regen_extrude(f, params, fe, rg_off[s]);
                else
                    regen_revolve(f, fe, rg_off[s]);
            }
        }
        if (h->state == CAD_FEAT_STATE_ERROR) {   /* drop partial output */
            out->segment_count = seg0; out->vertex_count = v0;
            out->face_count = f0;
            out->wireframe_truncated = wt; out->shading_truncated = st;
        }
        if (h->state == CAD_FEAT_STATE_ERROR || h->state == CAD_FEAT_STATE_BLOCKED)
            ++errors;
        h->first_segment = seg0;
        h->segment_count = out->segment_count - seg0;
        h->first_face = f0;
        h->face_count = out->face_count - f0;
    }
    for (i = 0; i < out->segment_count; ++i) {
        const CadFeatSegment *sg = &out->segments[i];
        for (k = 0; k < 3; ++k) {
            if (!out->has_bounds || sg->a[k] < out->min[k]) out->min[k] = sg->a[k];
            if (!out->has_bounds || sg->a[k] > out->max[k]) out->max[k] = sg->a[k];
        }
        out->has_bounds = 1;
        for (k = 0; k < 3; ++k) {
            if (sg->b[k] < out->min[k]) out->min[k] = sg->b[k];
            if (sg->b[k] > out->max[k]) out->max[k] = sg->b[k];
        }
    }
    if (error_count != 0) *error_count = errors;
    return CAD_FEAT_OK;
}

int cad_features_bounds(const CadFeatOutput *out, long min[3], long max[3])
{
    int k;
    if (out == 0 || !out->has_bounds) return 0;
    for (k = 0; k < 3; ++k) {
        if (min != 0) min[k] = out->min[k];
        if (max != 0) max[k] = out->max[k];
    }
    return 1;
}

/* ---- text reader ---- */

#define MAX_TOK (3 + CAD_FEAT_PROFILE_EXPRS)
typedef struct Tok { const char *p; unsigned n; int quoted; } Tok;
static Tok tok[MAX_TOK];
static unsigned ntok;
static char rd_expr[CAD_FEAT_PROFILE_EXPRS][CAD_FEAT_EXPR_MAX + 1];
static const char *rd_ptr[CAD_FEAT_PROFILE_EXPRS];

/* Splits on single spaces; "..." tokens are quoted strings. */
static int split(const char *s, unsigned len)
{
    unsigned i = 0, j;
    ntok = 0;
    if (len == 0) return 0;
    while (i < len) {
        if (ntok >= MAX_TOK) return 0;
        if (s[i] == '"') {
            for (j = i + 1U; j < len && s[j] != '"'; ++j) {}
            if (j >= len) return 0;
            tok[ntok].p = s + i + 1; tok[ntok].n = j - i - 1U; tok[ntok].quoted = 1;
            i = j + 1U;
        } else {
            for (j = i; j < len && s[j] != ' ' && s[j] != '"'; ++j) {}
            if (j == i) return 0;
            tok[ntok].p = s + i; tok[ntok].n = j - i; tok[ntok].quoted = 0;
            i = j;
        }
        ++ntok;
        if (i < len) {
            if (s[i] != ' ' || i + 1U >= len) return 0;
            ++i;
        }
    }
    return 1;
}
static int is(unsigned t, const char *word)
{
    return !tok[t].quoted && tok[t].n == strlen(word) &&
           memcmp(tok[t].p, word, tok[t].n) == 0;
}
/* Canonical decimal (no sign, no leading zeros) in min..max. */
static int num(unsigned t, unsigned long min, unsigned long max, unsigned *out)
{
    unsigned long v = 0;
    unsigned i;
    if (tok[t].quoted || tok[t].n == 0 || tok[t].n > 5 ||
        (tok[t].n > 1 && tok[t].p[0] == '0')) return 0;
    for (i = 0; i < tok[t].n; ++i) {
        if (tok[t].p[i] < '0' || tok[t].p[i] > '9') return 0;
        v = v * 10UL + (unsigned long)(tok[t].p[i] - '0');
    }
    if (v < min || v > max) return 0;
    *out = (unsigned)v;
    return 1;
}
static int str(unsigned t, char *buf, unsigned max)
{
    if (!tok[t].quoted || tok[t].n > max) return 0;
    memcpy(buf, tok[t].p, tok[t].n);
    buf[tok[t].n] = 0;
    return 1;
}
static int word_index(unsigned t, const char *const *words, int n)
{
    int i;
    for (i = 0; i < n; ++i)
        if (words[i][0] && is(t, words[i])) return i;
    return -1;
}

static CadFeatStatus read_feature(unsigned *sketch)
{
    char name[CAD_FEAT_NAME_MAX + 1], expr[CAD_FEAT_EXPR_MAX + 1];
    unsigned id, slot, sk, segs;
    int type, mode, sel, choice;
    CadFeatureData *fe;
    if (ntok < 5 || !num(1, 1UL, 65534UL, &id) || id >= cand.next_id ||
        index_of(&cand, id) >= 0)
        return fail(CAD_FEAT_FORMAT, 0, CAD_FEAT_FIELD_NONE, -1, -1);
    type = word_index(2, type_names, 4);
    mode = is(3, "A") ? 0 : is(3, "S") ? 1 : -1;
    if (type < 0 || mode < 0 || ntok != (type == CAD_FEAT_SKETCH ? 7U : 9U))
        return fail(CAD_FEAT_FORMAT, id, CAD_FEAT_FIELD_NONE, -1, -1);
    if (!str(4, name, CAD_FEAT_NAME_MAX) || !valid_name(name))
        return fail(CAD_FEAT_NAME, id, CAD_FEAT_FIELD_NAME, -1, -1);
    if (name_taken(&cand, name, -1))
        return fail(CAD_FEAT_DUPLICATE, cad_features_find(&cand, name),
                    CAD_FEAT_FIELD_NAME, -1, -1);
    if (cand.count >= CAD_FEAT_MAX)
        return fail(CAD_FEAT_FULL, id, CAD_FEAT_FIELD_NONE, -1, -1);
    slot = cand.count++;
    fe = &cand.items[slot];
    memset(fe, 0, sizeof(*fe));
    fe->id = id;
    fe->type = (unsigned char)type;
    fe->suppressed = (unsigned char)mode;
    fe->profile = CAD_FEAT_ALL;
    strcpy(fe->name, name);
    *sketch = 0;
    if (type == CAD_FEAT_SKETCH) {
        choice = word_index(5, plane_names, 3);
        if (choice < 0) return fail(CAD_FEAT_FORMAT, id, CAD_FEAT_FIELD_PLANE, -1, -1);
        if (!str(6, expr, CAD_FEAT_EXPR_MAX))
            return fail(CAD_FEAT_EXPRESSION, id, CAD_FEAT_FIELD_OFFSET, -1, -1);
        *sketch = id;
        return set_sketch(slot, (CadFeatPlane)choice, expr);
    }
    if (!num(5, 1UL, 65534UL, &sk))
        return fail(CAD_FEAT_FORMAT, id, CAD_FEAT_FIELD_SKETCH, -1, -1);
    if (is(6, "ALL")) sel = CAD_FEAT_ALL;
    else if (num(6, 0UL, CAD_FEAT_SKETCH_PROFILES - 1UL, &segs)) sel = (int)segs;
    else return fail(CAD_FEAT_FORMAT, id, CAD_FEAT_FIELD_PROFILE, -1, -1);
    if (type == CAD_FEAT_EXTRUDE) {
        choice = word_index(7, dir_names, 3);
        if (choice < 0)
            return fail(CAD_FEAT_FORMAT, id, CAD_FEAT_FIELD_DIRECTION, -1, -1);
        if (!str(8, expr, CAD_FEAT_EXPR_MAX))
            return fail(CAD_FEAT_EXPRESSION, id, CAD_FEAT_FIELD_DISTANCE, -1, -1);
        return set_extrude(slot, sk, sel, (CadFeatDirection)choice, expr);
    }
    choice = word_index(7, axis_names, 2);
    if (choice < 0) return fail(CAD_FEAT_FORMAT, id, CAD_FEAT_FIELD_AXIS, -1, -1);
    if (!num(8, CAD_FEAT_REVOLVE_MIN, CAD_FEAT_REVOLVE_MAX, &segs))
        return fail(CAD_FEAT_FORMAT, id, CAD_FEAT_FIELD_SEGMENTS, -1, -1);
    return set_revolve(slot, sk, sel, (CadFeatAxis)choice, segs);
}

static CadFeatStatus read_profile(unsigned sketch)
{
    unsigned first, count, sides = 0, pts, i, n;
    int kind;
    if (sketch == 0 || ntok < 2)
        return fail(CAD_FEAT_FORMAT, sketch, CAD_FEAT_FIELD_PROFILE, -1, -1);
    n = profile_count_of(&cand, sketch);
    kind = word_index(1, kind_names, 4);
    if (kind == CAD_FEAT_RECT) { first = 2; count = 4; }
    else if (kind == CAD_FEAT_CIRCLE) {
        if (ntok < 3 || !num(2, CAD_FEAT_CIRCLE_MIN, CAD_FEAT_CIRCLE_MAX, &sides))
            return fail(CAD_FEAT_FORMAT, sketch, CAD_FEAT_FIELD_SIDES, (int)n, -1);
        first = 3; count = 3;
    } else if (kind == CAD_FEAT_POLY) {
        if (ntok < 3 || !num(2, CAD_FEAT_POLY_MIN, CAD_FEAT_POLY_MAX, &pts))
            return fail(CAD_FEAT_FORMAT, sketch, CAD_FEAT_FIELD_PROFILE, (int)n, -1);
        first = 3; count = 2U * pts;
    } else {
        return fail(CAD_FEAT_FORMAT, sketch, CAD_FEAT_FIELD_PROFILE, (int)n, -1);
    }
    if (ntok != first + count)
        return fail(CAD_FEAT_FORMAT, sketch, CAD_FEAT_FIELD_PROFILE, (int)n, -1);
    for (i = 0; i < count; ++i) {
        if (!str(first + i, rd_expr[i], CAD_FEAT_EXPR_MAX))
            return fail(CAD_FEAT_EXPRESSION, sketch, CAD_FEAT_FIELD_PROFILE,
                        (int)n, (int)i);
        rd_ptr[i] = rd_expr[i];
    }
    if (n >= CAD_FEAT_SKETCH_PROFILES || cand.profile_count >= CAD_FEAT_MAX_PROFILES)
        return fail(CAD_FEAT_FULL, sketch, CAD_FEAT_FIELD_PROFILE, (int)n, -1);
    if (pack_profile(cand.profile_count, sketch, (int)n, (CadFeatProfileKind)kind,
                     rd_ptr, count, sides) != CAD_FEAT_OK)
        return last.status;
    ++cand.profile_count;
    return CAD_FEAT_OK;
}

CadFeatStatus cad_features_read_text(CadFeatures *f, const char *text,
                                     unsigned length, CadFeatError *e)
{
    CadFeatStatus s = CAD_FEAT_OK;
    unsigned pos = 0, end, line = 0, sketch = 0;
    if (f == 0 || (text == 0 && length != 0)) return bad_argument(e);
    cad_features_init(&cand);
    fail(CAD_FEAT_OK, 0, CAD_FEAT_FIELD_NONE, -1, -1);
    if (length > CAD_FEAT_TEXT_MAX)
        s = fail(CAD_FEAT_CAPACITY, 0, CAD_FEAT_FIELD_NONE, -1, -1);
    while (s == CAD_FEAT_OK && pos < length) {
        ++line;
        for (end = pos; end < length && text[end] != '\n'; ++end)
            if (text[end] < 0x20 || text[end] > 0x7E) break;
        if (end >= length || text[end] != '\n' || !split(text + pos, end - pos)) {
            s = fail(CAD_FEAT_FORMAT, 0, CAD_FEAT_FIELD_NONE, -1, -1);
        } else if (line == 1) {
            if (ntok != 2 || !is(0, "N") || !num(1, 1UL, 65535UL, &cand.next_id))
                s = fail(CAD_FEAT_FORMAT, 0, CAD_FEAT_FIELD_NONE, -1, -1);
        } else if (is(0, "F")) {
            s = read_feature(&sketch);
        } else if (is(0, "R")) {
            s = read_profile(sketch);
        } else {
            s = fail(CAD_FEAT_FORMAT, 0, CAD_FEAT_FIELD_NONE, -1, -1);
        }
        pos = end + 1U;
    }
    if (s != CAD_FEAT_OK) {
        last.line = line;
        if (e != 0) *e = last;
        return s;
    }
    *f = cand;
    if (e != 0) *e = last;
    return CAD_FEAT_OK;
}
