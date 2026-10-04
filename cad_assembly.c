#include "cad_assembly.h"
#include <string.h>
#include <limits.h>

#if INT_MAX < 32767 || LONG_MAX < 2147483647L
#error Assembly requires at least 16-bit int and 32-bit long
#endif

/* Every multiply is promoted BEFORE multiplication. Validated inputs give
 * |matrix dot| <= 3*1024^2 = 3145728;
 * |translation dot| <= 3*1024*32767 = 100660224.
 * Rounding adds at most 512; adding a translation AFTER division gives
 * <=131068. Handedness uses rounded cross products (<=2048), so its
 * dot is <=6291456. Thus no intermediate approaches signed 32-bit overflow,
 * even when host long is wider. No negative shifts or unsigned arithmetic.
 */
static long magnitude(long n) { return n < 0 ? -n : n; }
static long rounded(long n)
{
    return n < 0 ? -((-n + 512L) / 1024L) : (n + 512L) / 1024L;
}

void CadAssemblyIdentity(CadAssemblyTransform *t)
{
    int i;
    if (!t) return;
    memset(t, 0, sizeof(*t));
    for (i = 0; i < 3; ++i) t->r[i][i] = CAD_ASSEMBLY_Q;
}

void CadAssemblyInit(CadAssembly *a)
{
    if (!a) return;
    memset(a, 0, sizeof(*a));
    a->next_id = 1;
}

CadAssemblyStatus CadAssemblyValidateTransform(const CadAssemblyTransform *t)
{
    int i, j, k;
    long row, col, target, cross[3], handed;
    if (!t) return CAD_ASSEMBLY_ARGUMENT;
    for (i = 0; i < 3; ++i) {
        if (t->t[i] < -CAD_ASSEMBLY_TRANSLATION ||
            t->t[i] > CAD_ASSEMBLY_TRANSLATION) return CAD_ASSEMBLY_BOUNDS;
        for (j = 0; j < 3; ++j)
            if (t->r[i][j] < -1024 || t->r[i][j] > 1024)
                return CAD_ASSEMBLY_TRANSFORM;
    }
    for (i = 0; i < 3; ++i) for (j = i; j < 3; ++j) {
        row = col = 0;
        for (k = 0; k < 3; ++k) {
            row += (long)t->r[i][k] * t->r[j][k];
            col += (long)t->r[k][i] * t->r[k][j];
        }
        target = i == j ? 1048576L : 0L;
        if (magnitude(row - target) > 4096L ||
            magnitude(col - target) > 4096L) return CAD_ASSEMBLY_TRANSFORM;
    }
    for (i = 0; i < 3; ++i) {
        j = (i + 1) % 3; k = (i + 2) % 3;
        cross[i] = rounded((long)t->r[0][j] * t->r[1][k] -
                           (long)t->r[0][k] * t->r[1][j]);
    }
    handed = 0;
    for (i = 0; i < 3; ++i) handed += cross[i] * t->r[2][i];
    if (magnitude(handed - 1048576L) > 8192L)
        return CAD_ASSEMBLY_TRANSFORM;
    return CAD_ASSEMBLY_OK;
}

CadAssemblyStatus CadAssemblyCompose(const CadAssemblyTransform *p,
    const CadAssemblyTransform *l, CadAssemblyTransform *out)
{
    CadAssemblyTransform t;
    CadAssemblyStatus s;
    int i, j, k;
    long n;
    if (!out) return CAD_ASSEMBLY_ARGUMENT;
    s = CadAssemblyValidateTransform(p); if (s) return s;
    s = CadAssemblyValidateTransform(l); if (s) return s;
    memset(&t, 0, sizeof(t));
    for (i = 0; i < 3; ++i) {
        for (j = 0; j < 3; ++j) {
            n = 0;
            for (k = 0; k < 3; ++k) n += (long)p->r[i][k] * l->r[k][j];
            t.r[i][j] = (int)rounded(n); /* <=3072 fits Win16 int */
        }
        n = 0;
        for (k = 0; k < 3; ++k) n += (long)p->r[i][k] * l->t[k];
        t.t[i] = rounded(n) + p->t[i];
    }
    s = CadAssemblyValidateTransform(&t);
    if (!s) *out = t;
    return s;
}

static int find(const CadAssembly *a, CadAssemblyId id)
{
    unsigned int i;
    if (!a || a->count > CAD_ASSEMBLY_MAX || !id) return -1;
    for (i = 0; i < a->count; ++i)
        if (a->occurrence[i].id == id) return (int)i;
    return -1;
}

/* Nonrecursive bounded walk: validate graph before composing any results. */
static CadAssemblyStatus evaluate(const CadAssembly *a,
    CadAssemblyTransform *out)
{
    unsigned int i, j;
    int chain[CAD_ASSEMBLY_DEPTH], depth, at, k;
    CadAssemblyStatus s;
    CadAssemblyTransform t;
    if (!a || a->count > CAD_ASSEMBLY_MAX || !a->next_id ||
        a->next_id > CAD_ASSEMBLY_ID_MAX + 1UL) return CAD_ASSEMBLY_ARGUMENT;
    for (i = 0; i < a->count; ++i) {
        if (!a->occurrence[i].id || a->occurrence[i].id >= a->next_id ||
            !a->occurrence[i].part_id ||
            a->occurrence[i].part_id > CAD_ASSEMBLY_ID_MAX)
            return CAD_ASSEMBLY_ARGUMENT;
        for (j = 0; j < i; ++j)
            if (a->occurrence[j].id == a->occurrence[i].id)
                return CAD_ASSEMBLY_ARGUMENT;
        s = CadAssemblyValidateTransform(&a->occurrence[i].local);
        if (s) return s;
        /* Check cycles first, even when a cycle is longer than depth limit. */
        at = (int)i;
        for (j = 0; j <= a->count; ++j) {
            if (!a->occurrence[at].parent_id) break;
            at = find(a, a->occurrence[at].parent_id);
            if (at < 0) return CAD_ASSEMBLY_NOT_FOUND;
        }
        if (j > a->count) return CAD_ASSEMBLY_CYCLE;
        if (j >= CAD_ASSEMBLY_DEPTH) return CAD_ASSEMBLY_TOO_DEEP;
    }
    for (i = 0; i < a->count; ++i) {
        depth = 0; at = (int)i;
        do {
            chain[depth++] = at;
            at = find(a, a->occurrence[at].parent_id);
        } while (at >= 0);
        CadAssemblyIdentity(&t);
        for (k = depth - 1; k >= 0; --k) {
            s = CadAssemblyCompose(&t, &a->occurrence[chain[k]].local, &t);
            if (s) return s;
        }
        out[i] = t;
    }
    return CAD_ASSEMBLY_OK;
}

CadAssemblyStatus CadAssemblyEvaluate(const CadAssembly *a,
    CadAssemblyTransform *worlds, unsigned int capacity)
{
    CadAssemblyTransform temp[CAD_ASSEMBLY_MAX];
    CadAssemblyStatus s;
    if (!a || !worlds || capacity < a->count) return CAD_ASSEMBLY_ARGUMENT;
    s = evaluate(a, temp);
    if (!s) memcpy(worlds, temp, a->count * sizeof(*worlds));
    return s;
}

CadAssemblyStatus CadAssemblyGet(const CadAssembly *a, CadAssemblyId id,
    CadAssemblyOccurrence *out)
{
    int i;
    if (!a || !out || a->count > CAD_ASSEMBLY_MAX) return CAD_ASSEMBLY_ARGUMENT;
    i = find(a, id); if (i < 0) return CAD_ASSEMBLY_NOT_FOUND;
    *out = a->occurrence[i];
    return CAD_ASSEMBLY_OK;
}

CadAssemblyStatus CadAssemblyGetWorld(const CadAssembly *a, CadAssemblyId id,
    CadAssemblyTransform *out)
{
    CadAssemblyTransform temp[CAD_ASSEMBLY_MAX];
    CadAssemblyStatus s;
    int i;
    if (!a || !out) return CAD_ASSEMBLY_ARGUMENT;
    i = find(a, id); if (i < 0) return CAD_ASSEMBLY_NOT_FOUND;
    s = evaluate(a, temp);
    if (!s) *out = temp[i];
    return s;
}

static int equal(const CadAssemblyTransform *a, const CadAssemblyTransform *b)
{
    int i, j;
    for (i = 0; i < 3; ++i) {
        if (a->t[i] != b->t[i]) return 0;
        for (j = 0; j < 3; ++j) if (a->r[i][j] != b->r[i][j]) return 0;
    }
    return 1;
}

/* Single-record transaction; avoid copying the entire assembly on Win16 stack. */
static CadAssemblyStatus replace(CadAssembly *a, int at,
    const CadAssemblyOccurrence *replacement)
{
    CadAssemblyOccurrence old;
    CadAssemblyTransform before[CAD_ASSEMBLY_MAX], after[CAD_ASSEMBLY_MAX];
    CadAssemblyStatus s;
    unsigned int i;
    s = evaluate(a, before); if (s) return s;
    if (a->occurrence[at].grounded) return CAD_ASSEMBLY_GROUNDED;
    old = a->occurrence[at]; a->occurrence[at] = *replacement;
    s = evaluate(a, after);
    if (!s) for (i = 0; i < a->count; ++i)
        if (a->occurrence[i].grounded && !equal(&before[i], &after[i])) {
            s = CAD_ASSEMBLY_GROUNDED; break;
        }
    if (s) a->occurrence[at] = old;
    return s;
}

CadAssemblyStatus CadAssemblyAdd(CadAssembly *a, CadAssemblyId part,
    CadAssemblyId parent, const CadAssemblyTransform *local, CadAssemblyId *id)
{
    CadAssemblyTransform temp[CAD_ASSEMBLY_MAX];
    CadAssemblyOccurrence item, old;
    CadAssemblyStatus s;
    if (!a || !id || !local || !part || part > CAD_ASSEMBLY_ID_MAX)
        return CAD_ASSEMBLY_ARGUMENT;
    s = evaluate(a, temp); if (s) return s;
    if (a->count == CAD_ASSEMBLY_MAX) return CAD_ASSEMBLY_FULL;
    if (a->next_id > CAD_ASSEMBLY_ID_MAX) return CAD_ASSEMBLY_ID_EXHAUSTED;
    if (parent && find(a, parent) < 0) return CAD_ASSEMBLY_NOT_FOUND;
    memset(&item, 0, sizeof(item));
    item.id = a->next_id; item.part_id = part; item.parent_id = parent;
    item.local = *local;
    old = a->occurrence[a->count];
    a->occurrence[a->count++] = item; ++a->next_id;
    s = evaluate(a, temp);
    if (s) { --a->next_id; --a->count; a->occurrence[a->count] = old; }
    else *id = item.id;
    return s;
}

CadAssemblyStatus CadAssemblyRemove(CadAssembly *a, CadAssemblyId id)
{
    CadAssemblyTransform temp[CAD_ASSEMBLY_MAX];
    CadAssemblyStatus s;
    int at;
    unsigned int i;
    s = evaluate(a, temp); if (s) return s;
    at = find(a, id); if (at < 0) return CAD_ASSEMBLY_NOT_FOUND;
    if (a->occurrence[at].grounded) return CAD_ASSEMBLY_GROUNDED;
    for (i = 0; i < a->count; ++i)
        if (a->occurrence[i].parent_id == id) return CAD_ASSEMBLY_HAS_CHILDREN;
    for (i = (unsigned int)at; i + 1 < a->count; ++i)
        a->occurrence[i] = a->occurrence[i + 1];
    --a->count;
    memset(&a->occurrence[a->count], 0, sizeof(a->occurrence[0]));
    return CAD_ASSEMBLY_OK;
}

CadAssemblyStatus CadAssemblySetLocal(CadAssembly *a, CadAssemblyId id,
    const CadAssemblyTransform *local)
{
    CadAssemblyOccurrence item;
    int at;
    if (!a || !local) return CAD_ASSEMBLY_ARGUMENT;
    at = find(a, id); if (at < 0) return CAD_ASSEMBLY_NOT_FOUND;
    item = a->occurrence[at];
    if (item.grounded) return CAD_ASSEMBLY_GROUNDED;
    if (item.constraint) return CAD_ASSEMBLY_CONSTRAINED;
    item.local = *local;
    return replace(a, at, &item);
}

CadAssemblyStatus CadAssemblyReparent(CadAssembly *a, CadAssemblyId id,
    CadAssemblyId parent)
{
    CadAssemblyOccurrence item;
    int at;
    if (!a) return CAD_ASSEMBLY_ARGUMENT;
    at = find(a, id); if (at < 0) return CAD_ASSEMBLY_NOT_FOUND;
    item = a->occurrence[at];
    if (item.grounded) return CAD_ASSEMBLY_GROUNDED;
    if (item.constraint) return CAD_ASSEMBLY_CONSTRAINED;
    item.parent_id = parent;
    return replace(a, at, &item);
}

CadAssemblyStatus CadAssemblyGround(CadAssembly *a, CadAssemblyId id, int flag)
{
    CadAssemblyTransform temp[CAD_ASSEMBLY_MAX];
    CadAssemblyStatus s;
    int at;
    if (flag != 0 && flag != 1) return CAD_ASSEMBLY_ARGUMENT;
    s = evaluate(a, temp); if (s) return s;
    at = find(a, id); if (at < 0) return CAD_ASSEMBLY_NOT_FOUND;
    a->occurrence[at].grounded = flag;
    return CAD_ASSEMBLY_OK;
}

static CadAssemblyStatus inverse(const CadAssemblyTransform *t,
    CadAssemblyTransform *out)
{
    int i, j;
    long n;
    CadAssemblyStatus s;
    s = CadAssemblyValidateTransform(t); if (s) return s;
    memset(out, 0, sizeof(*out));
    for (i = 0; i < 3; ++i) {
        n = 0;
        for (j = 0; j < 3; ++j) {
            out->r[i][j] = t->r[j][i];
            n -= (long)t->r[j][i] * t->t[j];
        }
        out->t[i] = rounded(n);
    }
    return CadAssemblyValidateTransform(out);
}

static CadAssemblyStatus datum_local(CadAssemblyOccurrence *item)
{
    static const int cosine[8] = {1024,724,0,-724,-1024,-724,0,724};
    static const int sine[8] = {0,724,1024,724,0,-724,-1024,-724};
    CadAssemblyTransform yaw, inv, t;
    CadAssemblyStatus s;
    CadAssemblyIdentity(&yaw);
    if (item->constraint == 2) {
        if (item->yaw_min < 0 || item->yaw_max >= CAD_ASSEMBLY_YAWS ||
            item->yaw_min > item->yaw_max || item->yaw < item->yaw_min ||
            item->yaw > item->yaw_max) return CAD_ASSEMBLY_ARGUMENT;
        yaw.r[0][0] = yaw.r[1][1] = cosine[item->yaw];
        yaw.r[1][0] = sine[item->yaw]; yaw.r[0][1] = -sine[item->yaw];
    }
    s = inverse(&item->child_frame, &inv); if (s) return s;
    s = CadAssemblyCompose(&item->parent_frame, &yaw, &t); if (s) return s;
    return CadAssemblyCompose(&t, &inv, &item->local);
}

static CadAssemblyStatus bind(CadAssembly *a, CadAssemblyId id,
    CadAssemblyId parent, const CadAssemblyTransform *pf,
    const CadAssemblyTransform *cf, int kind, int yaw, int min, int max)
{
    CadAssemblyOccurrence item;
    CadAssemblyStatus s;
    int at;
    if (!a || !pf || !cf) return CAD_ASSEMBLY_ARGUMENT;
    at = find(a, id); if (at < 0) return CAD_ASSEMBLY_NOT_FOUND;
    item = a->occurrence[at];
    item.parent_id = parent; item.constraint = kind;
    item.parent_frame = *pf; item.child_frame = *cf;
    item.yaw = yaw; item.yaw_min = min; item.yaw_max = max;
    s = datum_local(&item); if (s) return s;
    return replace(a, at, &item);
}

CadAssemblyStatus CadAssemblyBindFixed(CadAssembly *a, CadAssemblyId id,
    CadAssemblyId parent, const CadAssemblyTransform *pf,
    const CadAssemblyTransform *cf)
{
    return bind(a, id, parent, pf, cf, 1, 0, 0, 0);
}

CadAssemblyStatus CadAssemblyBindHinge(CadAssembly *a, CadAssemblyId id,
    CadAssemblyId parent, const CadAssemblyTransform *pf,
    const CadAssemblyTransform *cf, int yaw, int min, int max)
{
    return bind(a, id, parent, pf, cf, 2, yaw, min, max);
}

CadAssemblyStatus CadAssemblySetYaw(CadAssembly *a, CadAssemblyId id, int yaw)
{
    CadAssemblyOccurrence item;
    CadAssemblyStatus s;
    int at;
    if (!a) return CAD_ASSEMBLY_ARGUMENT;
    at = find(a, id); if (at < 0) return CAD_ASSEMBLY_NOT_FOUND;
    item = a->occurrence[at];
    if (item.constraint != 2) return CAD_ASSEMBLY_CONSTRAINED;
    item.yaw = yaw;
    s = datum_local(&item); if (s) return s;
    return replace(a, at, &item);
}

CadAssemblyStatus CadAssemblyClearConstraint(CadAssembly *a, CadAssemblyId id)
{
    CadAssemblyOccurrence item;
    int at;
    if (!a) return CAD_ASSEMBLY_ARGUMENT;
    at = find(a, id); if (at < 0) return CAD_ASSEMBLY_NOT_FOUND;
    item = a->occurrence[at]; item.constraint = 0;
    CadAssemblyIdentity(&item.parent_frame);
    CadAssemblyIdentity(&item.child_frame);
    item.yaw = item.yaw_min = item.yaw_max = 0;
    return replace(a, at, &item);
}
