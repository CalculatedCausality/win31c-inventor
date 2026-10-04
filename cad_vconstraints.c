#include "cad_vconstraints.h"
#include <string.h>

/* Static scratch keeps Win16 stack use small; calls are non-reentrant. */
static CadConstraints vs;
static CadConstraints vs_save;
static CadId slot_id[CAD_VC_MAX_VERTICES];
static int slot_count;
static int base_values[CAD_CONSTRAINT_MAX_VARIABLES];
static CadVcMoves move_scratch;
static CadVConstraints read_scratch;

static long vc_coord(const CadVertex *v, int axis)
{
    return axis == 0 ? v->x : axis == 1 ? v->y : v->z;
}

static int vc_two_vertex(int kind)
{
    return kind == CAD_VC_COINCIDENT || kind == CAD_VC_ALIGN ||
           kind == CAD_VC_DISTANCE;
}

static int vc_find(const CadVConstraints *vc, CadVcId id)
{
    int i;
    if (id == 0UL) return -1;
    for (i = 0; i < vc->count; ++i)
        if (vc->items[i].id == id) return i;
    return -1;
}

static int vc_valid_set(const CadVConstraints *vc)
{
    return vc != NULL && vc->count >= 0 && vc->count <= CAD_VC_MAX_RECORDS;
}

/* Field-level validation shared by add and the text reader. */
static CadVcStatus vc_check_record(const CadVConstraint *c)
{
    int i;
    if (c->a == 0) return CAD_VC_INVALID;
    if (vc_two_vertex(c->kind)) {
        if (c->b == 0 || c->a == c->b) return CAD_VC_INVALID;
        if (c->fix[0] != 0L || c->fix[1] != 0L || c->fix[2] != 0L)
            return CAD_VC_INVALID;
    } else if (c->kind == CAD_VC_FIX) {
        if (c->b != 0 || c->axis != 0 || c->offset != 0L)
            return CAD_VC_INVALID;
        for (i = 0; i < 3; ++i)
            if (c->fix[i] < CAD_COORD_MIN || c->fix[i] > CAD_COORD_MAX)
                return CAD_VC_RANGE;
        return CAD_VC_OK;
    } else {
        return CAD_VC_INVALID;
    }
    if (c->kind == CAD_VC_COINCIDENT) {
        if (c->axis != 0 || c->offset != 0L) return CAD_VC_INVALID;
    } else {
        if (c->axis < 0 || c->axis > 2) return CAD_VC_INVALID;
        if (c->kind == CAD_VC_ALIGN && c->offset != 0L) return CAD_VC_INVALID;
        if (c->offset < -CAD_VC_MAX_DISTANCE || c->offset > CAD_VC_MAX_DISTANCE)
            return CAD_VC_RANGE;
    }
    return CAD_VC_OK;
}

/* 1 satisfied, 0 unsatisfied, -1 a vertex is missing. */
static int vc_satisfied(const CadVConstraint *c, const CadModel *m)
{
    CadVertex va, vb;
    int i;
    if (cad_model_get_vertex(m, c->a, &va) != CAD_OK) return -1;
    if (c->kind == CAD_VC_FIX) {
        for (i = 0; i < 3; ++i)
            if (vc_coord(&va, i) != c->fix[i]) return 0;
        return 1;
    }
    if (cad_model_get_vertex(m, c->b, &vb) != CAD_OK) return -1;
    if (c->kind == CAD_VC_COINCIDENT) {
        for (i = 0; i < 3; ++i)
            if (vc_coord(&va, i) != vc_coord(&vb, i)) return 0;
        return 1;
    }
    return vc_coord(&vb, c->axis) - vc_coord(&va, c->axis) == c->offset;
}

static int vc_equation_count(const CadVConstraint *c)
{
    return c->kind == CAD_VC_ALIGN || c->kind == CAD_VC_DISTANCE ? 1 : 3;
}

static int vc_slot_of(CadId id)
{
    int i;
    for (i = 0; i < slot_count; ++i)
        if (slot_id[i] == id) return i;
    return -1;
}

static int vc_slot_add(CadId id)
{
    int i;
    i = vc_slot_of(id);
    if (i >= 0) return i;
    if (slot_count == CAD_VC_MAX_VERTICES) return -1;
    slot_id[slot_count] = id;
    return slot_count++;
}

/* Expand a record into scalar equations: value[b]-value[a]=off, a<0 anchor. */
static int vc_equations(const CadVConstraint *c, int *ea, int *eb, long *off)
{
    int sa, sb, i;
    sa = vc_slot_of(c->a);
    if (c->kind == CAD_VC_FIX) {
        for (i = 0; i < 3; ++i) {
            ea[i] = -1; eb[i] = 3 * sa + i; off[i] = c->fix[i];
        }
        return 3;
    }
    sb = vc_slot_of(c->b);
    if (c->kind == CAD_VC_COINCIDENT) {
        for (i = 0; i < 3; ++i) {
            ea[i] = 3 * sa + i; eb[i] = 3 * sb + i; off[i] = 0L;
        }
        return 3;
    }
    ea[0] = 3 * sa + c->axis; eb[0] = 3 * sb + c->axis; off[0] = c->offset;
    return 1;
}

static CadVcStatus vc_map(CadConstraintStatus s)
{
    switch (s) {
    case CAD_CONSTRAINT_OK:
    case CAD_CONSTRAINT_REDUNDANT: return CAD_VC_OK;
    case CAD_CONSTRAINT_CONFLICT: return CAD_VC_CONFLICT;
    case CAD_CONSTRAINT_RANGE: return CAD_VC_RANGE;
    case CAD_CONSTRAINT_CAPACITY: return CAD_VC_CAPACITY;
    default: return CAD_VC_INVALID;
    }
}

static CadConstraintStatus vc_add_eq(int a, int b, long off)
{
    if (a < 0) return CadConstraintsAddAnchor(&vs, b, off, NULL);
    return CadConstraintsAddOffset(&vs, a, b, off, NULL);
}

/* Load slots and the solver with every healthy record except skip, plus the
 * vertices of extra (not its equations). Healthy records must already hold.
 */
static CadVcStatus vc_build(const CadVConstraints *vc, const CadModel *m,
                            CadVcId skip, const CadVConstraint *extra)
{
    const CadVConstraint *c;
    CadVertex v;
    int i, k, n, ea[3], eb[3];
    long off[3];
    CadConstraintStatus s;
    slot_count = 0;
    for (i = 0; i < vc->count; ++i) {
        c = &vc->items[i];
        if (c->id == skip || c->health != CAD_VC_HEALTHY) continue;
        if (vc_satisfied(c, m) != 1) return CAD_VC_STALE;
        if (vc_slot_add(c->a) < 0) return CAD_VC_CAPACITY;
        if (c->b != 0 && vc_slot_add(c->b) < 0) return CAD_VC_CAPACITY;
    }
    if (extra != NULL) {
        if (cad_model_get_vertex(m, extra->a, &v) != CAD_OK ||
            (extra->b != 0 && cad_model_get_vertex(m, extra->b, &v) != CAD_OK))
            return CAD_VC_NOT_FOUND;
        if (vc_slot_add(extra->a) < 0) return CAD_VC_CAPACITY;
        if (extra->b != 0 && vc_slot_add(extra->b) < 0) return CAD_VC_CAPACITY;
    }
    for (i = 0; i < slot_count; ++i) {
        if (cad_model_get_vertex(m, slot_id[i], &v) != CAD_OK)
            return CAD_VC_STALE;
        for (k = 0; k < 3; ++k) base_values[3 * i + k] = (int)vc_coord(&v, k);
    }
    s = CadConstraintsInit(&vs, base_values, 3 * slot_count);
    if (s != CAD_CONSTRAINT_OK) return vc_map(s);
    for (i = 0; i < vc->count; ++i) {
        c = &vc->items[i];
        if (c->id == skip || c->health != CAD_VC_HEALTHY) continue;
        n = vc_equations(c, ea, eb, off);
        for (k = 0; k < n; ++k) {
            s = vc_add_eq(ea[k], eb[k], off[k]);
            if (s != CAD_CONSTRAINT_OK && s != CAD_CONSTRAINT_REDUNDANT)
                return vc_map(s);
        }
    }
    for (i = 0; i < 3 * slot_count; ++i)
        if (vs.values[i] != base_values[i]) return CAD_VC_STALE;
    return CAD_VC_OK;
}

static int vc_ok(CadConstraintStatus s)
{
    return s == CAD_CONSTRAINT_OK || s == CAD_CONSTRAINT_REDUNDANT;
}

/* Satisfy one new equation: move b's group, else a's group, then add it. */
static CadVcStatus vc_solve_eq(int a, int b, long off)
{
    CadConstraintStatus first, second;
    long va, vb;
    va = a < 0 ? 0L : (long)vs.values[a];
    vb = (long)vs.values[b];
    if (vb - va == off) return vc_map(vc_add_eq(a, b, off));
    memcpy(&vs_save, &vs, sizeof(vs));
    first = CadConstraintsDrag(&vs, b, va + off);
    if (vc_ok(first)) first = vc_add_eq(a, b, off);
    if (vc_ok(first)) return CAD_VC_OK;
    memcpy(&vs, &vs_save, sizeof(vs));
    if (a < 0) return vc_map(first);
    second = CadConstraintsDrag(&vs, a, vb - off);
    if (vc_ok(second)) second = vc_add_eq(a, b, off);
    if (vc_ok(second)) return CAD_VC_OK;
    memcpy(&vs, &vs_save, sizeof(vs));
    return vc_map(first);   /* Report why b's group could not move. */
}

static CadVcStatus vc_solve_record(const CadVConstraint *c)
{
    int k, n, ea[3], eb[3];
    long off[3];
    CadVcStatus s;
    n = vc_equations(c, ea, eb, off);
    for (k = 0; k < n; ++k) {
        s = vc_solve_eq(ea[k], eb[k], off[k]);
        if (s != CAD_VC_OK) return s;
    }
    return CAD_VC_OK;
}

static void vc_move_push(CadId id, int slot)
{
    CadVcMove *mv;
    mv = &move_scratch.items[move_scratch.count++];
    mv->id = id;
    mv->x = (long)vs.values[3 * slot];
    mv->y = (long)vs.values[3 * slot + 1];
    mv->z = (long)vs.values[3 * slot + 2];
}

/* Primary (if nonzero, must be a slot) first, then every changed slot. */
static void vc_collect(CadId primary)
{
    int i, k, changed;
    move_scratch.count = 0;
    if (primary != 0) vc_move_push(primary, vc_slot_of(primary));
    for (i = 0; i < slot_count; ++i) {
        if (slot_id[i] == primary) continue;
        changed = 0;
        for (k = 0; k < 3; ++k)
            if (vs.values[3 * i + k] != base_values[3 * i + k]) changed = 1;
        if (changed) vc_move_push(slot_id[i], i);
    }
}

CadVcStatus cad_vc_init(CadVConstraints *vc)
{
    if (vc == NULL) return CAD_VC_INVALID;
    memset(vc, 0, sizeof(*vc));
    vc->next_id = 1UL;
    return CAD_VC_OK;
}

CadVcStatus cad_vc_add(CadVConstraints *vc, const CadModel *model, int kind,
                       CadId a, CadId b, int axis, long offset,
                       CadVcMoves *moves, CadVcId *id_out)
{
    CadVConstraint c;
    CadVertex v;
    CadVcStatus s;
    int i;
    if (!vc_valid_set(vc) || model == NULL || moves == NULL)
        return CAD_VC_INVALID;
    memset(&c, 0, sizeof(c));
    c.kind = kind;
    c.a = a;
    c.b = b;
    c.axis = axis;
    c.offset = offset;
    if (kind == CAD_VC_FIX) {
        if (a == 0) return CAD_VC_INVALID;
        if (cad_model_get_vertex(model, a, &v) != CAD_OK)
            return CAD_VC_NOT_FOUND;
        for (i = 0; i < 3; ++i) c.fix[i] = vc_coord(&v, i);
    }
    s = vc_check_record(&c);
    if (s != CAD_VC_OK) return s;
    if (vc->count == CAD_VC_MAX_RECORDS || vc->next_id == 0UL ||
        vc->next_id > CAD_VC_ID_MAX) return CAD_VC_CAPACITY;
    c.id = vc->next_id;
    s = vc_build(vc, model, 0UL, &c);
    if (s == CAD_VC_OK) s = vc_solve_record(&c);
    if (s != CAD_VC_OK) return s;
    vc_collect(0);
    vc->items[vc->count++] = c;
    vc->next_id = c.id == CAD_VC_ID_MAX ? 0UL : c.id + 1UL;
    *moves = move_scratch;
    if (id_out != NULL) *id_out = c.id;
    return CAD_VC_OK;
}

CadVcStatus cad_vc_remove(CadVConstraints *vc, CadVcId id)
{
    int i;
    if (!vc_valid_set(vc)) return CAD_VC_INVALID;
    i = vc_find(vc, id);
    if (i < 0) return CAD_VC_NOT_FOUND;
    for (; i + 1 < vc->count; ++i) vc->items[i] = vc->items[i + 1];
    --vc->count;
    memset(&vc->items[vc->count], 0, sizeof(CadVConstraint));
    return CAD_VC_OK;
}

CadVcStatus cad_vc_get(const CadVConstraints *vc, CadVcId id,
                       CadVConstraint *out)
{
    int i;
    if (!vc_valid_set(vc) || out == NULL) return CAD_VC_INVALID;
    i = vc_find(vc, id);
    if (i < 0) return CAD_VC_NOT_FOUND;
    *out = vc->items[i];
    return CAD_VC_OK;
}

CadVcStatus cad_vc_at(const CadVConstraints *vc, int index,
                      CadVConstraint *out)
{
    if (!vc_valid_set(vc) || out == NULL) return CAD_VC_INVALID;
    if (index < 0 || index >= vc->count) return CAD_VC_NOT_FOUND;
    *out = vc->items[index];
    return CAD_VC_OK;
}

CadVcStatus cad_vc_set_distance(CadVConstraints *vc, const CadModel *model,
                                CadVcId id, long offset, CadVcMoves *moves)
{
    CadVConstraint c;
    CadVcStatus s;
    int i;
    if (!vc_valid_set(vc) || model == NULL || moves == NULL)
        return CAD_VC_INVALID;
    i = vc_find(vc, id);
    if (i < 0) return CAD_VC_NOT_FOUND;
    c = vc->items[i];
    if (c.kind != CAD_VC_DISTANCE) return CAD_VC_INVALID;
    c.offset = offset;
    s = vc_check_record(&c);
    if (s != CAD_VC_OK) return s;
    s = vc_build(vc, model, id, &c);
    if (s == CAD_VC_OK) s = vc_solve_record(&c);
    if (s != CAD_VC_OK) return s;
    vc_collect(0);
    c.health = CAD_VC_HEALTHY;
    vc->items[i] = c;
    *moves = move_scratch;
    return CAD_VC_OK;
}

CadVcStatus cad_vc_drag(const CadVConstraints *vc, const CadModel *model,
                        CadId vertex, long x, long y, long z,
                        CadVcMoves *moves)
{
    CadVertex v;
    CadVcStatus s;
    CadConstraintStatus cs;
    long target[3];
    int slot, k;
    if (!vc_valid_set(vc) || model == NULL || moves == NULL || vertex == 0)
        return CAD_VC_INVALID;
    if (cad_model_get_vertex(model, vertex, &v) != CAD_OK)
        return CAD_VC_NOT_FOUND;
    target[0] = x; target[1] = y; target[2] = z;
    for (k = 0; k < 3; ++k)
        if (target[k] < CAD_COORD_MIN || target[k] > CAD_COORD_MAX)
            return CAD_VC_RANGE;
    s = vc_build(vc, model, 0UL, NULL);
    if (s != CAD_VC_OK) return s;
    slot = vc_slot_of(vertex);
    if (slot < 0) {
        moves->count = 1;
        moves->items[0].id = vertex;
        moves->items[0].x = x;
        moves->items[0].y = y;
        moves->items[0].z = z;
        return CAD_VC_OK;
    }
    for (k = 0; k < 3; ++k) {
        cs = CadConstraintsDrag(&vs, 3 * slot + k, target[k]);
        if (!vc_ok(cs)) return vc_map(cs);
    }
    vc_collect(vertex);
    *moves = move_scratch;
    return CAD_VC_OK;
}

CadVcStatus cad_vc_sync(CadVConstraints *vc, const CadModel *model,
                        int *removed, int *sick)
{
    CadVConstraint *c;
    int i, j, gone, ill, equations, saved_slots;
    if (!vc_valid_set(vc) || model == NULL) return CAD_VC_INVALID;
    gone = 0;
    for (i = j = 0; i < vc->count; ++i) {
        c = &vc->items[i];
        if (vc_satisfied(c, model) < 0) { ++gone; continue; }
        vc->items[j++] = *c;
    }
    for (i = j; i < vc->count; ++i)
        memset(&vc->items[i], 0, sizeof(CadVConstraint));
    vc->count = j;
    ill = 0;
    equations = 0;
    slot_count = 0;
    for (i = 0; i < vc->count; ++i) {
        c = &vc->items[i];
        c->health = CAD_VC_SICK;
        if (vc_satisfied(c, model) != 1 ||
            equations + vc_equation_count(c) > CAD_CONSTRAINT_MAX_CONSTRAINTS) {
            ++ill;
            continue;
        }
        saved_slots = slot_count;
        if (vc_slot_add(c->a) < 0 || (c->b != 0 && vc_slot_add(c->b) < 0)) {
            slot_count = saved_slots;
            ++ill;
            continue;
        }
        equations += vc_equation_count(c);
        c->health = CAD_VC_HEALTHY;
    }
    if (removed != NULL) *removed = gone;
    if (sick != NULL) *sick = ill;
    return CAD_VC_OK;
}

CadVcStatus cad_vc_analyze(const CadVConstraints *vc, const CadModel *model,
                           CadVcAnalysis *out)
{
    CadConstraintAnalysis a;
    CadVcAnalysis r;
    CadVcStatus s;
    CadConstraintStatus cs;
    int i;
    if (!vc_valid_set(vc) || model == NULL || out == NULL)
        return CAD_VC_INVALID;
    s = vc_build(vc, model, 0UL, NULL);
    if (s != CAD_VC_OK) return s;
    cs = CadConstraintsAnalyze(&vs, &a);
    if (cs != CAD_CONSTRAINT_OK) return vc_map(cs);
    memset(&r, 0, sizeof(r));
    r.records = vc->count;
    for (i = 0; i < vc->count; ++i)
        if (vc->items[i].health == CAD_VC_HEALTHY) ++r.healthy;
    r.sick = r.records - r.healthy;
    r.vertices = slot_count;
    r.equations = a.constraint_count;
    r.independent = a.independent;
    r.redundant = a.redundant;
    r.free_dof = a.free_root_dof;
    *out = r;
    return CAD_VC_OK;
}

CadVcStatus cad_vc_stage_moves(CadModel *model, const CadVcMoves *moves)
{
    CadVertex v;
    int i;
    if (model == NULL || moves == NULL || moves->count < 0 ||
        moves->count > CAD_VC_MAX_MOVES) return CAD_VC_INVALID;
    if (!model->transaction_active) return CAD_VC_INVALID;
    if (moves->count > CAD_MAX_TRANSACTION_DELTAS - (int)model->staged_count)
        return CAD_VC_CAPACITY;
    for (i = 0; i < moves->count; ++i) {
        if (cad_model_get_vertex(model, moves->items[i].id, &v) != CAD_OK)
            return CAD_VC_NOT_FOUND;
        if (moves->items[i].x < CAD_COORD_MIN || moves->items[i].x > CAD_COORD_MAX ||
            moves->items[i].y < CAD_COORD_MIN || moves->items[i].y > CAD_COORD_MAX ||
            moves->items[i].z < CAD_COORD_MIN || moves->items[i].z > CAD_COORD_MAX)
            return CAD_VC_RANGE;
    }
    for (i = 0; i < moves->count; ++i)
        if (cad_model_move_vertex(model, moves->items[i].id, moves->items[i].x,
                                  moves->items[i].y, moves->items[i].z) != CAD_OK)
            return CAD_VC_INVALID; /* Unreachable after the prechecks. */
    return CAD_VC_OK;
}

/* ---- Text output ---- */

typedef struct VcOut {
    char *buf;           /* NULL: measure only. */
    unsigned cap, len;
    int over;
} VcOut;

static void vc_put(VcOut *o, const char *s)
{
    while (*s) {
        if (o->len >= o->cap) { o->over = 1; return; }
        if (o->buf != NULL) o->buf[o->len] = *s;
        ++o->len;
        ++s;
    }
}

static void vc_put_ulong(VcOut *o, unsigned long v)
{
    char tmp[12];
    int n;
    n = 11;
    tmp[n] = '\0';
    do {
        tmp[--n] = (char)('0' + (int)(v % 10UL));
        v /= 10UL;
    } while (v != 0UL);
    vc_put(o, tmp + n);
}

static void vc_put_long(VcOut *o, long v)
{
    /* Values here are bounded (|v| <= 800), so negation is safe. */
    if (v < 0L) { vc_put(o, "-"); v = -v; }
    vc_put_ulong(o, (unsigned long)v);
}

static const char *const vc_axis_name[3] = { "X", "Y", "Z" };

CadVcStatus cad_vc_describe(const CadVConstraint *c, char *buffer,
                            unsigned capacity)
{
    VcOut o;
    int pass;
    if (c == NULL || buffer == NULL || capacity == 0U) return CAD_VC_INVALID;
    if (vc_check_record(c) != CAD_VC_OK) return CAD_VC_INVALID;
    for (pass = 0; pass < 2; ++pass) {
        o.buf = pass ? buffer : NULL;
        o.cap = capacity - 1U;
        o.len = 0U;
        o.over = 0;
        switch (c->kind) {
        case CAD_VC_COINCIDENT: vc_put(&o, "Coincident V"); break;
        case CAD_VC_ALIGN:
            vc_put(&o, "Align "); vc_put(&o, vc_axis_name[c->axis]);
            vc_put(&o, " V");
            break;
        case CAD_VC_DISTANCE:
            vc_put(&o, "Distance "); vc_put(&o, vc_axis_name[c->axis]);
            vc_put(&o, " V");
            break;
        default: vc_put(&o, "Fix V"); break;
        }
        vc_put_ulong(&o, (unsigned long)c->a);
        if (c->kind != CAD_VC_FIX) {
            vc_put(&o, "-V");
            vc_put_ulong(&o, (unsigned long)c->b);
        }
        if (c->kind == CAD_VC_DISTANCE) {
            vc_put(&o, " = ");
            vc_put_long(&o, c->offset);
        }
        if (c->health == CAD_VC_SICK) vc_put(&o, " (Sick)");
        if (o.over) return CAD_VC_BUFFER;
        if (pass) buffer[o.len] = '\0';
    }
    return CAD_VC_OK;
}

static void vc_write_all(const CadVConstraints *vc, VcOut *o)
{
    const CadVConstraint *c;
    int i, k;
    vc_put(o, "N ");
    vc_put_ulong(o, vc->next_id);
    vc_put(o, "\n");
    for (i = 0; i < vc->count; ++i) {
        c = &vc->items[i];
        vc_put(o, "C ");
        vc_put_ulong(o, c->id);
        switch (c->kind) {
        case CAD_VC_COINCIDENT: vc_put(o, " COINCIDENT "); break;
        case CAD_VC_ALIGN: vc_put(o, " ALIGN "); break;
        case CAD_VC_DISTANCE: vc_put(o, " DISTANCE "); break;
        default: vc_put(o, " FIX "); break;
        }
        vc_put_ulong(o, (unsigned long)c->a);
        if (c->kind == CAD_VC_FIX) {
            for (k = 0; k < 3; ++k) {
                vc_put(o, " ");
                vc_put_long(o, c->fix[k]);
            }
        } else {
            vc_put(o, " ");
            vc_put_ulong(o, (unsigned long)c->b);
            if (c->kind != CAD_VC_COINCIDENT) {
                vc_put(o, " ");
                vc_put(o, vc_axis_name[c->axis]);
            }
            if (c->kind == CAD_VC_DISTANCE) {
                vc_put(o, " ");
                vc_put_long(o, c->offset);
            }
        }
        vc_put(o, "\n");
    }
}

CadVcStatus cad_vc_write_text(const CadVConstraints *vc, char *buffer,
                              unsigned capacity, unsigned *length)
{
    VcOut o;
    int i;
    if (!vc_valid_set(vc) || buffer == NULL || length == NULL ||
        capacity == 0U) return CAD_VC_INVALID;
    for (i = 0; i < vc->count; ++i)
        if (vc_check_record(&vc->items[i]) != CAD_VC_OK) return CAD_VC_INVALID;
    o.buf = NULL;
    o.cap = capacity - 1U;
    o.len = 0U;
    o.over = 0;
    vc_write_all(vc, &o);
    if (o.over) return CAD_VC_BUFFER;
    o.buf = buffer;
    o.len = 0U;
    vc_write_all(vc, &o);
    buffer[o.len] = '\0';
    *length = o.len;
    return CAD_VC_OK;
}

/* ---- Text input ---- */

#define VC_MAX_TOKENS 8

typedef struct VcTok {
    const char *p;
    unsigned n;
} VcTok;

static int vc_tok_is(const VcTok *t, const char *s)
{
    unsigned i;
    for (i = 0; i < t->n; ++i)
        if (s[i] != t->p[i]) return 0;
    return s[t->n] == '\0';
}

/* Canonical unsigned decimal: no sign, no leading zeros. */
static int vc_tok_ulong(const VcTok *t, unsigned long max, unsigned long *out)
{
    unsigned i;
    unsigned long v;
    int d;
    if (t->n == 0U || t->n > 10U) return 0;
    if (t->p[0] == '0' && t->n > 1U) return 0;
    v = 0UL;
    for (i = 0; i < t->n; ++i) {
        if (t->p[i] < '0' || t->p[i] > '9') return 0;
        d = t->p[i] - '0';
        if (v > (max - (unsigned long)d) / 10UL) return 0;
        v = v * 10UL + (unsigned long)d;
    }
    *out = v;
    return 1;
}

/* Canonical signed decimal within [-limit, limit]; "-0" rejected. */
static int vc_tok_long(const VcTok *t, long limit, long *out)
{
    VcTok u;
    unsigned long v;
    int neg;
    neg = t->n > 0U && t->p[0] == '-';
    u.p = t->p + (neg ? 1 : 0);
    u.n = t->n - (neg ? 1U : 0U);
    if (!vc_tok_ulong(&u, (unsigned long)limit, &v)) return 0;
    if (neg && v == 0UL) return 0;
    *out = neg ? -(long)v : (long)v;
    return 1;
}

static int vc_tok_vertex(const VcTok *t, CadId *out)
{
    unsigned long v;
    if (!vc_tok_ulong(t, 65535UL, &v) || v == 0UL) return 0;
    *out = (CadId)v;
    return 1;
}

static int vc_tok_axis(const VcTok *t, int *axis)
{
    if (vc_tok_is(t, "X")) *axis = 0;
    else if (vc_tok_is(t, "Y")) *axis = 1;
    else if (vc_tok_is(t, "Z")) *axis = 2;
    else return 0;
    return 1;
}

/* Parse one line's tokens into a record; returns CAD_VC_OK or an error. */
static CadVcStatus vc_parse_record(const VcTok *t, int n, CadVConstraint *c)
{
    unsigned long id;
    int k;
    memset(c, 0, sizeof(*c));
    if (n < 4 || !vc_tok_ulong(&t[1], CAD_VC_ID_MAX, &id) || id == 0UL)
        return CAD_VC_PARSE;
    c->id = id;
    if (!vc_tok_vertex(&t[3], &c->a)) return CAD_VC_PARSE;
    if (vc_tok_is(&t[2], "FIX")) {
        c->kind = CAD_VC_FIX;
        if (n != 7) return CAD_VC_PARSE;
        for (k = 0; k < 3; ++k)
            if (!vc_tok_long(&t[4 + k], CAD_COORD_MAX, &c->fix[k]))
                return CAD_VC_PARSE;
    } else {
        if (vc_tok_is(&t[2], "COINCIDENT")) {
            c->kind = CAD_VC_COINCIDENT;
            if (n != 5) return CAD_VC_PARSE;
        } else if (vc_tok_is(&t[2], "ALIGN")) {
            c->kind = CAD_VC_ALIGN;
            if (n != 6 || !vc_tok_axis(&t[5], &c->axis)) return CAD_VC_PARSE;
        } else if (vc_tok_is(&t[2], "DISTANCE")) {
            c->kind = CAD_VC_DISTANCE;
            if (n != 7 || !vc_tok_axis(&t[5], &c->axis) ||
                !vc_tok_long(&t[6], CAD_VC_MAX_DISTANCE, &c->offset))
                return CAD_VC_PARSE;
        } else {
            return CAD_VC_PARSE;
        }
        if (!vc_tok_vertex(&t[4], &c->b)) return CAD_VC_PARSE;
    }
    return vc_check_record(c) == CAD_VC_OK ? CAD_VC_OK : CAD_VC_PARSE;
}

CadVcStatus cad_vc_read_text(CadVConstraints *vc, const char *text,
                             unsigned length, unsigned *error_line)
{
    VcTok tok[VC_MAX_TOKENS];
    CadVConstraints *r;
    CadVConstraint c;
    CadVcStatus s;
    unsigned pos, start, line;
    unsigned long next;
    int n, have_next;
    char ch;
    if (error_line != NULL) *error_line = 0U;
    if (vc == NULL || (text == NULL && length != 0U)) return CAD_VC_INVALID;
    r = &read_scratch;
    memset(r, 0, sizeof(*r));
    r->next_id = 1UL;
    have_next = 0;
    pos = 0U;
    line = 0U;
    while (pos < length) {
        ++line;
        start = pos;
        n = 0;
        s = CAD_VC_PARSE;
        for (;;) {
            if (pos >= length) goto fail;   /* Unterminated last line. */
            ch = text[pos];
            if (ch == '\n' || ch == ' ') {
                if (pos == start || n == VC_MAX_TOKENS) goto fail;
                tok[n].p = text + start;
                tok[n].n = pos - start;
                ++n;
                ++pos;
                start = pos;
                if (ch == '\n') break;
                continue;
            }
            if (ch < '!' || ch > '~') goto fail;
            ++pos;
        }
        if (vc_tok_is(&tok[0], "N")) {
            if (have_next || line != 1U || n != 2 ||
                !vc_tok_ulong(&tok[1], CAD_VC_ID_MAX, &next)) goto fail;
            r->next_id = next;
            have_next = 1;
        } else if (vc_tok_is(&tok[0], "C")) {
            if (!have_next) goto fail;
            s = vc_parse_record(tok, n, &c);
            if (s != CAD_VC_OK) goto fail;
            s = CAD_VC_PARSE;
            if ((r->count > 0 && c.id <= r->items[r->count - 1].id) ||
                (r->next_id != 0UL && c.id >= r->next_id)) goto fail;
            if (r->count == CAD_VC_MAX_RECORDS) {
                s = CAD_VC_CAPACITY;
                goto fail;
            }
            r->items[r->count++] = c;
        } else {
            goto fail;
        }
    }
    memcpy(vc, r, sizeof(*vc));
    return CAD_VC_OK;
fail:
    if (error_line != NULL) *error_line = line;
    return s;
}

const char *cad_vc_status_string(CadVcStatus status)
{
    switch (status) {
    case CAD_VC_OK: return "OK";
    case CAD_VC_INVALID: return "Invalid argument";
    case CAD_VC_NOT_FOUND: return "Not found";
    case CAD_VC_CONFLICT: return "Over-constrained (conflict)";
    case CAD_VC_RANGE: return "Out of range";
    case CAD_VC_CAPACITY: return "Constraint capacity exceeded";
    case CAD_VC_STALE: return "Constraints out of sync with geometry";
    case CAD_VC_PARSE: return "Malformed constraint text";
    case CAD_VC_BUFFER: return "Buffer too small";
    default: return "Unknown status";
    }
}
