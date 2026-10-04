#include "cad_constraints.h"
#include <limits.h>
#include <string.h>

#if LONG_MAX < 2147483647L || INT_MAX < 32767
#error CadConstraints requires at least 16-bit int and 32-bit long
#endif

#define CC_MIN32 (-2147483647L - 1L)
#define CC_MAX32 2147483647L
#define CC_NODES (CAD_CONSTRAINT_MAX_VARIABLES + 1)

/* delta[i] = value[i] - value[parent[i]]. Union by rank bounds tree depth;
 * no recursive find or large automatic arrays are needed on Win16.
 */
typedef struct CcWork {
    int parent[CC_NODES];
    int rank[CC_NODES];
    long delta[CC_NODES];
    int root[CC_NODES];
    long potential[CC_NODES];
    long low[CC_NODES];
    long high[CC_NODES];
    long value[CC_NODES];
    int used[CC_NODES];
    int candidate[CAD_CONSTRAINT_MAX_VARIABLES];
    int independent;
    int redundant;
    int free_dof;
    int fixed_root;
} CcWork;

static CcWork cc;

static int cc_32(long x)
{
#if LONG_MAX > 2147483647L
    return x >= CC_MIN32 && x <= CC_MAX32;
#else
    /* Every long is already signed32 on Win16; avoid Watcom W124. */
    (void)x;
    return 1;
#endif
}

static int cc_add(long a, long b, long *out)
{
    if (!cc_32(a) || !cc_32(b)) return 0;
    if ((b > 0L && a > CC_MAX32 - b) ||
        (b < 0L && a < CC_MIN32 - b)) return 0;
    *out = a + b;
    return 1;
}

static int cc_sub(long a, long b, long *out)
{
    if (!cc_32(a) || !cc_32(b)) return 0;
    if ((b > 0L && a < CC_MIN32 + b) ||
        (b < 0L && a > CC_MAX32 + b)) return 0;
    *out = a - b;
    return 1;
}

static CadConstraintStatus cc_valid(const CadConstraints *m)
{
    int i;
    if (m == NULL || m->variable_count < 0 ||
        m->variable_count > CAD_CONSTRAINT_MAX_VARIABLES ||
        m->constraint_count < 0 ||
        m->constraint_count > CAD_CONSTRAINT_MAX_CONSTRAINTS)
        return CAD_CONSTRAINT_INVALID;
    for (i = 0; i < m->variable_count; ++i)
        if (m->values[i] < CAD_CONSTRAINT_COORD_MIN ||
            m->values[i] > CAD_CONSTRAINT_COORD_MAX)
            return CAD_CONSTRAINT_RANGE;
    return CAD_CONSTRAINT_OK;
}

static int cc_index(const CadConstraints *m, int i)
{
    return i >= 0 && i < m->variable_count;
}

static int cc_find(int node, int *root, long *potential)
{
    long p;
    p = 0L;
    while (cc.parent[node] != node) {
        if (!cc_add(p, cc.delta[node], &p)) return 0;
        node = cc.parent[node];
    }
    *root = node;
    *potential = p;
    return 1;
}

static CadConstraintStatus cc_equation(const CadConstraints *m,
                                       const CadConstraint *e)
{
    int a, ra, rb;
    long da, db, d, actual;
    if (!cc_index(m, e->b) ||
        (e->a != CAD_CONSTRAINT_FIXED && !cc_index(m, e->a)))
        return CAD_CONSTRAINT_INVALID;
    if (!cc_32(e->offset)) return CAD_CONSTRAINT_RANGE;
    a = e->a == CAD_CONSTRAINT_FIXED ? m->variable_count : e->a;
    if (!cc_find(a, &ra, &da) || !cc_find(e->b, &rb, &db))
        return CAD_CONSTRAINT_RANGE;
    if (ra == rb) {
        if (!cc_sub(db, da, &actual)) return CAD_CONSTRAINT_RANGE;
        if (actual != e->offset) return CAD_CONSTRAINT_CONFLICT;
        ++cc.redundant;
        return CAD_CONSTRAINT_REDUNDANT;
    }
    /* root[b] - root[a] = offset + da - db. */
    if (!cc_add(e->offset, da, &d) || !cc_sub(d, db, &d))
        return CAD_CONSTRAINT_RANGE;
    if (cc.rank[ra] < cc.rank[rb]) {
        if (!cc_sub(0L, d, &d)) return CAD_CONSTRAINT_RANGE;
        cc.parent[ra] = rb;
        cc.delta[ra] = d;
    } else {
        cc.parent[rb] = ra;
        cc.delta[rb] = d;
        if (cc.rank[ra] == cc.rank[rb]) ++cc.rank[ra];
    }
    ++cc.independent;
    return CAD_CONSTRAINT_OK;
}

/* Rebuild existing equations, optionally skip an ID and/or append one staged
 * equation. Derive all feasible root intervals and a complete edit candidate.
 */
static CadConstraintStatus cc_build(const CadConstraints *m,
                                    CadConstraintId skip,
                                    const CadConstraint *extra,
                                    CadConstraintStatus *extra_status)
{
    CadConstraintStatus s;
    int i, r, fixed;
    long low, high, p, v;
    s = cc_valid(m);
    if (s != CAD_CONSTRAINT_OK) return s;
    memset(&cc, 0, sizeof(cc));
    for (i = 0; i <= m->variable_count; ++i) cc.parent[i] = i;
    for (i = 0; i < m->constraint_count; ++i) {
        if (m->constraints[i].id == skip) continue;
        s = cc_equation(m, &m->constraints[i]);
        if (s != CAD_CONSTRAINT_OK && s != CAD_CONSTRAINT_REDUNDANT) return s;
    }
    if (extra != NULL) {
        s = cc_equation(m, extra);
        if (s != CAD_CONSTRAINT_OK && s != CAD_CONSTRAINT_REDUNDANT) return s;
        *extra_status = s;
    }
    fixed = m->variable_count;
    for (i = 0; i <= fixed; ++i) {
        if (!cc_find(i, &cc.root[i], &cc.potential[i]))
            return CAD_CONSTRAINT_RANGE;
    }
    cc.fixed_root = cc.root[fixed];
    for (i = 0; i < fixed; ++i) {
        r = cc.root[i];
        p = cc.potential[i];
        if (!cc_sub((long)CAD_CONSTRAINT_COORD_MIN, p, &low) ||
            !cc_sub((long)CAD_CONSTRAINT_COORD_MAX, p, &high))
            return CAD_CONSTRAINT_RANGE;
        if (!cc.used[r]) {
            cc.used[r] = 1;
            cc.low[r] = low;
            cc.high[r] = high;
            if (!cc_sub((long)m->values[i], p, &cc.value[r]))
                return CAD_CONSTRAINT_RANGE;
        } else {
            if (low > cc.low[r]) cc.low[r] = low;
            if (high < cc.high[r]) cc.high[r] = high;
        }
        if (cc.low[r] > cc.high[r]) return CAD_CONSTRAINT_RANGE;
    }
    for (r = 0; r <= fixed; ++r) {
        if (!cc.used[r]) continue;
        if (r == cc.fixed_root) {
            if (!cc_sub(0L, cc.potential[fixed], &v))
                return CAD_CONSTRAINT_RANGE;
            if (v < cc.low[r] || v > cc.high[r]) return CAD_CONSTRAINT_RANGE;
            cc.low[r] = v;
            cc.high[r] = v;
            cc.value[r] = v;
        } else {
            ++cc.free_dof;
            if (cc.value[r] < cc.low[r]) cc.value[r] = cc.low[r];
            if (cc.value[r] > cc.high[r]) cc.value[r] = cc.high[r];
        }
    }
    for (i = 0; i < fixed; ++i) {
        if (!cc_add(cc.value[cc.root[i]], cc.potential[i], &v) ||
            v < CAD_CONSTRAINT_COORD_MIN || v > CAD_CONSTRAINT_COORD_MAX)
            return CAD_CONSTRAINT_RANGE;
        cc.candidate[i] = (int)v;
    }
    return CAD_CONSTRAINT_OK;
}

static void cc_commit_values(CadConstraints *m)
{
    int i;
    for (i = 0; i < m->variable_count; ++i) m->values[i] = cc.candidate[i];
}

CadConstraintStatus CadConstraintsInit(CadConstraints *m,
                                     const int *values, int count)
{
    int i;
    if (m == NULL || count < 0 || (count != 0 && values == NULL))
        return CAD_CONSTRAINT_INVALID;
    if (count > CAD_CONSTRAINT_MAX_VARIABLES) return CAD_CONSTRAINT_CAPACITY;
    /* Stage first: values can point inside m, including reinitialization. */
    for (i = 0; i < count; ++i) {
        if (values[i] < CAD_CONSTRAINT_COORD_MIN ||
            values[i] > CAD_CONSTRAINT_COORD_MAX) return CAD_CONSTRAINT_RANGE;
        cc.candidate[i] = values[i];
    }
    memset(m, 0, sizeof(*m));
    m->variable_count = count;
    m->next_id = 1UL;
    cc_commit_values(m);
    return CAD_CONSTRAINT_OK;
}

static CadConstraintStatus cc_append(CadConstraints *m, int a, int b,
                                     long offset, CadConstraintId *id_out)
{
    CadConstraint e;
    CadConstraintStatus s, added;
    s = cc_valid(m);
    if (s != CAD_CONSTRAINT_OK) return s;
    if (!cc_index(m, b) || (a != CAD_CONSTRAINT_FIXED && !cc_index(m, a)))
        return CAD_CONSTRAINT_INVALID;
    if (!cc_32(offset)) return CAD_CONSTRAINT_RANGE;
    if (m->constraint_count == CAD_CONSTRAINT_MAX_CONSTRAINTS ||
        m->next_id == 0UL || m->next_id > CAD_CONSTRAINT_ID_MAX)
        return CAD_CONSTRAINT_CAPACITY;
    memset(&e, 0, sizeof(e));
    e.id = m->next_id;
    e.a = a;
    e.b = b;
    e.offset = offset;
    added = CAD_CONSTRAINT_OK;
    s = cc_build(m, 0UL, &e, &added);
    if (s != CAD_CONSTRAINT_OK) return s;
    m->constraints[m->constraint_count] = e;
    ++m->constraint_count;
    m->next_id = e.id == CAD_CONSTRAINT_ID_MAX ? 0UL : e.id + 1UL;
    cc_commit_values(m);
    if (id_out != NULL) *id_out = e.id;
    return added;
}

CadConstraintStatus CadConstraintsAddOffset(CadConstraints *m, int a, int b,
                                           long offset, CadConstraintId *id_out)
{
    /* Constant-zero sentinel is available only through AddAnchor. */
    if (a < 0) return CAD_CONSTRAINT_INVALID;
    return cc_append(m, a, b, offset, id_out);
}

CadConstraintStatus CadConstraintsAddAnchor(CadConstraints *m, int variable,
                                           long coordinate,
                                           CadConstraintId *id_out)
{
    return cc_append(m, CAD_CONSTRAINT_FIXED, variable, coordinate, id_out);
}

static int cc_lookup(const CadConstraints *m, CadConstraintId id)
{
    int i;
    if (id == 0UL) return -1;
    for (i = 0; i < m->constraint_count; ++i)
        if (m->constraints[i].id == id) return i;
    return -1;
}

CadConstraintStatus CadConstraintsGet(const CadConstraints *m,
                                     CadConstraintId id, CadConstraint *out)
{
    CadConstraintStatus s;
    int i;
    s = cc_valid(m);
    if (s != CAD_CONSTRAINT_OK) return s;
    if (out == NULL) return CAD_CONSTRAINT_INVALID;
    i = cc_lookup(m, id);
    if (i < 0) return CAD_CONSTRAINT_NOT_FOUND;
    *out = m->constraints[i];
    return CAD_CONSTRAINT_OK;
}

CadConstraintStatus CadConstraintsRemove(CadConstraints *m, CadConstraintId id)
{
    CadConstraintStatus s;
    int i, j;
    s = cc_valid(m);
    if (s != CAD_CONSTRAINT_OK) return s;
    i = cc_lookup(m, id);
    if (i < 0) return CAD_CONSTRAINT_NOT_FOUND;
    s = cc_build(m, id, NULL, NULL);
    if (s != CAD_CONSTRAINT_OK) return s;
    for (j = i; j + 1 < m->constraint_count; ++j)
        m->constraints[j] = m->constraints[j + 1];
    --m->constraint_count;
    memset(&m->constraints[m->constraint_count], 0, sizeof(CadConstraint));
    cc_commit_values(m);
    return CAD_CONSTRAINT_OK;
}

CadConstraintStatus CadConstraintsAnalyze(const CadConstraints *m,
                                         CadConstraintAnalysis *out)
{
    CadConstraintStatus s;
    if (out == NULL) return CAD_CONSTRAINT_INVALID;
    s = cc_build(m, 0UL, NULL, NULL);
    if (s != CAD_CONSTRAINT_OK) return s;
    out->variable_count = m->variable_count;
    out->constraint_count = m->constraint_count;
    out->independent = cc.independent;
    out->redundant = cc.redundant;
    out->free_root_dof = cc.free_dof;
    return CAD_CONSTRAINT_OK;
}

CadConstraintStatus CadConstraintsRange(const CadConstraints *m, int variable,
                                       long *minimum, long *maximum)
{
    CadConstraintStatus s;
    int r;
    long low, high;
    s = cc_valid(m);
    if (s != CAD_CONSTRAINT_OK) return s;
    if (!cc_index(m, variable) || minimum == NULL || maximum == NULL ||
        minimum == maximum) return CAD_CONSTRAINT_INVALID;
    s = cc_build(m, 0UL, NULL, NULL);
    if (s != CAD_CONSTRAINT_OK) return s;
    r = cc.root[variable];
    if (!cc_add(cc.low[r], cc.potential[variable], &low) ||
        !cc_add(cc.high[r], cc.potential[variable], &high))
        return CAD_CONSTRAINT_RANGE;
    *minimum = low;
    *maximum = high;
    return CAD_CONSTRAINT_OK;
}

CadConstraintStatus CadConstraintsDrag(CadConstraints *m, int variable,
                                      long coordinate)
{
    CadConstraintStatus s;
    int i, r;
    long root_value, v;
    s = cc_valid(m);
    if (s != CAD_CONSTRAINT_OK) return s;
    if (!cc_index(m, variable)) return CAD_CONSTRAINT_INVALID;
    if (!cc_32(coordinate) || coordinate < CAD_CONSTRAINT_COORD_MIN ||
        coordinate > CAD_CONSTRAINT_COORD_MAX) return CAD_CONSTRAINT_RANGE;
    s = cc_build(m, 0UL, NULL, NULL);
    if (s != CAD_CONSTRAINT_OK) return s;
    r = cc.root[variable];
    if (!cc_sub(coordinate, cc.potential[variable], &root_value))
        return CAD_CONSTRAINT_RANGE;
    if (r == cc.fixed_root && root_value != cc.value[r])
        return CAD_CONSTRAINT_CONFLICT;
    if (root_value < cc.low[r] || root_value > cc.high[r])
        return CAD_CONSTRAINT_RANGE;
    for (i = 0; i < m->variable_count; ++i) {
        if (cc.root[i] != r) continue;
        if (!cc_add(root_value, cc.potential[i], &v) ||
            v < CAD_CONSTRAINT_COORD_MIN || v > CAD_CONSTRAINT_COORD_MAX)
            return CAD_CONSTRAINT_RANGE;
        cc.candidate[i] = (int)v;
    }
    cc_commit_values(m);
    return CAD_CONSTRAINT_OK;
}

CadConstraintStatus CadConstraintsEdit(CadConstraints *m,
                                      const int *candidate, int count)
{
    CadConstraintStatus s;
    int i;
    long a, difference;
    const CadConstraint *e;
    s = cc_valid(m);
    if (s != CAD_CONSTRAINT_OK) return s;
    if (count != m->variable_count || (count != 0 && candidate == NULL))
        return CAD_CONSTRAINT_INVALID;
    for (i = 0; i < count; ++i) {
        if (candidate[i] < CAD_CONSTRAINT_COORD_MIN ||
            candidate[i] > CAD_CONSTRAINT_COORD_MAX) return CAD_CONSTRAINT_RANGE;
        cc.candidate[i] = candidate[i];
    }
    for (i = 0; i < m->constraint_count; ++i) {
        e = &m->constraints[i];
        if (!cc_index(m, e->b) ||
            (e->a != CAD_CONSTRAINT_FIXED && !cc_index(m, e->a)))
            return CAD_CONSTRAINT_INVALID;
        a = e->a == CAD_CONSTRAINT_FIXED ? 0L : (long)cc.candidate[e->a];
        if (!cc_sub((long)cc.candidate[e->b], a, &difference) ||
            !cc_32(e->offset)) return CAD_CONSTRAINT_RANGE;
        if (difference != e->offset) return CAD_CONSTRAINT_CONFLICT;
    }
    cc_commit_values(m);
    return CAD_CONSTRAINT_OK;
}
