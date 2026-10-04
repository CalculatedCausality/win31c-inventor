/* Place Component: part-file occurrences over cad_assembly/cad_document. */
#include <stdio.h>
#include <string.h>
#include "cad_parts.h"
#include "cad_document.h"

/* Separate objects so Watcom -zt=1 gives each its own far segment. */
static CadModel parts_model0;
static CadModel parts_model1;
static CadModel parts_model2;
static CadModel parts_model3;
static CadModel parts_stage;
static CadParts parts_scratch;

static const int parts_cos[CAD_ASSEMBLY_YAWS] =
    {1024, 724, 0, -724, -1024, -724, 0, 724};
static const int parts_sin[CAD_ASSEMBLY_YAWS] =
    {0, 724, 1024, 724, 0, -724, -1024, -724};

static CadModel *slot_model(int slot)
{
    switch (slot) {
    case 0: return &parts_model0;
    case 1: return &parts_model1;
    case 2: return &parts_model2;
    default: return &parts_model3;
    }
}

static long rounded(long n)
{
    return n >= 0 ? (n + 512L) / 1024L : -((-n + 512L) / 1024L);
}

static void apply(const CadAssemblyTransform *t, const CadVertex *v,
                  long out[3])
{
    int i;
    for (i = 0; i < 3; ++i)
        out[i] = rounded((long)t->r[i][0] * v->x + (long)t->r[i][1] * v->y +
                         (long)t->r[i][2] * v->z) + t->t[i];
}

static void make_transform(long tx, long ty, long tz, int yaw,
                           CadAssemblyTransform *t)
{
    memset(t, 0, sizeof(*t));
    t->r[0][0] = parts_cos[yaw]; t->r[0][2] = parts_sin[yaw];
    t->r[1][1] = CAD_ASSEMBLY_Q;
    t->r[2][0] = -parts_sin[yaw]; t->r[2][2] = parts_cos[yaw];
    t->t[0] = tx; t->t[1] = ty; t->t[2] = tz;
}

static int valid_placement(long tx, long ty, long tz, int yaw)
{
    return yaw >= 0 && yaw < CAD_ASSEMBLY_YAWS &&
        tx >= -CAD_PARTS_TRANSLATION && tx <= CAD_PARTS_TRANSLATION &&
        ty >= -CAD_PARTS_TRANSLATION && ty <= CAD_PARTS_TRANSLATION &&
        tz >= -CAD_PARTS_TRANSLATION && tz <= CAD_PARTS_TRANSLATION;
}

/* Length 1..127 of a printable, space-free path ending at NUL or at limit
 * bytes, else 0. */
static unsigned path_length(const char *path, unsigned limit)
{
    unsigned n;
    for (n = 0; n < limit && path[n]; ++n) {
        unsigned char c = (unsigned char)path[n];
        if (c < 0x21 || c > 0x7e) return 0;
    }
    return n <= CAD_PARTS_PATH_MAX ? n : 0;
}

static CadPartsStatus map_status(CadAssemblyStatus s)
{
    switch (s) {
    case CAD_ASSEMBLY_OK: return CAD_PARTS_OK;
    case CAD_ASSEMBLY_NOT_FOUND: return CAD_PARTS_NOT_FOUND;
    case CAD_ASSEMBLY_FULL: return CAD_PARTS_FULL;
    case CAD_ASSEMBLY_ID_EXHAUSTED: return CAD_PARTS_ID_EXHAUSTED;
    case CAD_ASSEMBLY_GROUNDED: return CAD_PARTS_GROUNDED;
    default: return CAD_PARTS_ARGUMENT;
    }
}

static int find_record(const CadParts *p, CadAssemblyId id)
{
    unsigned i;
    for (i = 0; i < p->count; ++i)
        if (p->record[i].id == id) return (int)i;
    return -1;
}

static int find_slot(const CadParts *p, const char *path, unsigned n)
{
    int i;
    for (i = 0; i < CAD_PARTS_CACHE; ++i)
        if (p->slot[i].path[0] && strlen(p->slot[i].path) == n &&
            !memcmp(p->slot[i].path, path, n)) return i;
    return -1;
}

static int free_slot(const CadParts *p)
{
    int i;
    for (i = 0; i < CAD_PARTS_CACHE; ++i)
        if (!p->slot[i].path[0]) return i;
    return -1;
}

static void load_slot(CadParts *p, int slot)
{
    p->slot[slot].missing = cad_document_load(p->slot[slot].path,
        slot_model(slot), &parts_stage) != CAD_DOC_OK;
}

void cad_parts_init(CadParts *p)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));
    CadAssemblyInit(&p->assembly);
}

CadPartsStatus cad_parts_place(CadParts *p, const char *path, long tx,
    long ty, long tz, int yaw, CadAssemblyId *id)
{
    CadAssemblyTransform t;
    CadAssemblyId nid;
    CadAssemblyStatus s;
    unsigned n;
    int slot, fresh = 0;

    if (!p || !path || !id || !valid_placement(tx, ty, tz, yaw))
        return CAD_PARTS_ARGUMENT;
    n = path_length(path, CAD_PARTS_PATH_MAX + 1);
    if (!n) return CAD_PARTS_ARGUMENT;
    if (p->count >= CAD_PARTS_MAX) return CAD_PARTS_FULL;
    if (p->assembly.next_id > CAD_ASSEMBLY_ID_MAX)
        return CAD_PARTS_ID_EXHAUSTED;
    slot = find_slot(p, path, n);
    if (slot < 0) {
        slot = free_slot(p);
        if (slot < 0) return CAD_PARTS_CACHE_FULL;
        fresh = 1;
        memcpy(p->slot[slot].path, path, n);
        p->slot[slot].path[n] = 0;
        load_slot(p, slot);
    }
    make_transform(tx, ty, tz, yaw, &t);
    s = CadAssemblyAdd(&p->assembly, (CadAssemblyId)slot + 1UL, 0, &t, &nid);
    if (s != CAD_ASSEMBLY_OK) {
        if (fresh) memset(&p->slot[slot], 0, sizeof(p->slot[slot]));
        return map_status(s);
    }
    p->record[p->count].id = nid;
    p->record[p->count].slot = slot;
    p->record[p->count].yaw = yaw;
    ++p->count;
    *id = nid;
    return CAD_PARTS_OK;
}

CadPartsStatus cad_parts_move(CadParts *p, CadAssemblyId id, long tx,
    long ty, long tz, int yaw)
{
    CadAssemblyTransform t;
    CadAssemblyStatus s;
    int at;

    if (!p || !valid_placement(tx, ty, tz, yaw)) return CAD_PARTS_ARGUMENT;
    at = find_record(p, id);
    if (at < 0) return CAD_PARTS_NOT_FOUND;
    make_transform(tx, ty, tz, yaw, &t);
    s = CadAssemblySetLocal(&p->assembly, id, &t);
    if (s != CAD_ASSEMBLY_OK) return map_status(s);
    p->record[at].yaw = yaw;
    return CAD_PARTS_OK;
}

CadPartsStatus cad_parts_remove(CadParts *p, CadAssemblyId id)
{
    CadAssemblyOccurrence occ;
    CadAssemblyStatus s;
    unsigned i;
    int at, slot, used = 0;

    if (!p) return CAD_PARTS_ARGUMENT;
    at = find_record(p, id);
    if (at < 0) return CAD_PARTS_NOT_FOUND;
    s = CadAssemblyGet(&p->assembly, id, &occ);
    if (s == CAD_ASSEMBLY_OK && occ.grounded)
        s = CadAssemblyGround(&p->assembly, id, 0);
    if (s == CAD_ASSEMBLY_OK) {
        s = CadAssemblyRemove(&p->assembly, id);
        if (s != CAD_ASSEMBLY_OK && occ.grounded)
            CadAssemblyGround(&p->assembly, id, 1);
    }
    if (s != CAD_ASSEMBLY_OK) return map_status(s);
    slot = p->record[at].slot;
    for (i = (unsigned)at; i + 1 < p->count; ++i)
        p->record[i] = p->record[i + 1];
    --p->count;
    for (i = 0; i < p->count; ++i)
        if (p->record[i].slot == slot) used = 1;
    if (!used) memset(&p->slot[slot], 0, sizeof(p->slot[slot]));
    return CAD_PARTS_OK;
}

CadPartsStatus cad_parts_ground(CadParts *p, CadAssemblyId id, int grounded)
{
    if (!p) return CAD_PARTS_ARGUMENT;
    if (find_record(p, id) < 0) return CAD_PARTS_NOT_FOUND;
    return map_status(CadAssemblyGround(&p->assembly, id, grounded != 0));
}

unsigned int cad_parts_count(const CadParts *p)
{
    return p ? p->count : 0;
}

CadPartsStatus cad_parts_at(const CadParts *p, unsigned int index,
    CadAssemblyId *id)
{
    if (!p || !id) return CAD_PARTS_ARGUMENT;
    if (index >= p->count) return CAD_PARTS_NOT_FOUND;
    *id = p->record[index].id;
    return CAD_PARTS_OK;
}

static void make_name(const CadParts *p, int at, char *name)
{
    const char *path = p->slot[p->record[at].slot].path;
    unsigned i, base = 0, end, n = 0, k = 1;
    for (i = 0; path[i]; ++i)
        if (path[i] == '/' || path[i] == '\\' || path[i] == ':') base = i + 1;
    end = i;
    for (i = base; path[i]; ++i)
        if (path[i] == '.') end = i;
    for (i = base; i < end && n < 12; ++i) {
        char c = path[i];
        name[n++] = (char)(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    }
    for (i = 0; i < (unsigned)at; ++i) /* records are in ascending ID order */
        if (p->record[i].slot == p->record[at].slot) ++k;
    sprintf(name + n, ":%u", k);
}

CadPartsStatus cad_parts_get(const CadParts *p, CadAssemblyId id,
    CadPartsInfo *info)
{
    CadAssemblyOccurrence occ;
    CadPartsInfo out;
    const CadModel *m;
    int at, slot;

    if (!p || !info) return CAD_PARTS_ARGUMENT;
    at = find_record(p, id);
    if (at < 0) return CAD_PARTS_NOT_FOUND;
    if (CadAssemblyGet(&p->assembly, id, &occ) != CAD_ASSEMBLY_OK)
        return CAD_PARTS_NOT_FOUND;
    memset(&out, 0, sizeof(out));
    slot = p->record[at].slot;
    out.id = id;
    make_name(p, at, out.name);
    strcpy(out.path, p->slot[slot].path);
    out.missing = p->slot[slot].missing;
    out.tx = occ.local.t[0]; out.ty = occ.local.t[1]; out.tz = occ.local.t[2];
    out.yaw = p->record[at].yaw;
    out.grounded = occ.grounded;
    if (!out.missing) {
        m = slot_model(slot);
        out.vertex_count = m->vertex_count;
        out.edge_count = m->edge_count;
    }
    *info = out;
    return CAD_PARTS_OK;
}

CadPartsStatus cad_parts_reload(CadParts *p)
{
    int i;
    if (!p) return CAD_PARTS_ARGUMENT;
    for (i = 0; i < CAD_PARTS_CACHE; ++i)
        if (p->slot[i].path[0]) load_slot(p, i);
    return CAD_PARTS_OK;
}

CadPartsStatus cad_parts_segments(const CadParts *p, CadAssemblyId id,
    CadPartsSegmentFn fn, void *ctx)
{
    CadAssemblyOccurrence occ;
    CadVertex a, b;
    const CadModel *m;
    long wa[3], wb[3];
    unsigned i, e;

    if (!p || !fn) return CAD_PARTS_ARGUMENT;
    if (id && find_record(p, id) < 0) return CAD_PARTS_NOT_FOUND;
    for (i = 0; i < p->count; ++i) {
        if (id && p->record[i].id != id) continue;
        if (p->slot[p->record[i].slot].missing) continue;
        if (CadAssemblyGet(&p->assembly, p->record[i].id, &occ) !=
            CAD_ASSEMBLY_OK) continue;
        m = slot_model(p->record[i].slot);
        for (e = 0; e < m->edge_count; ++e) {
            if (cad_model_get_vertex(m, m->edges[e].a, &a) != CAD_OK ||
                cad_model_get_vertex(m, m->edges[e].b, &b) != CAD_OK)
                continue;
            apply(&occ.local, &a, wa);
            apply(&occ.local, &b, wb);
            fn(ctx, wa[0], wa[1], wa[2], wb[0], wb[1], wb[2]);
        }
    }
    return CAD_PARTS_OK;
}

int cad_parts_bounds(const CadParts *p, long min[3], long max[3])
{
    CadAssemblyOccurrence occ;
    const CadModel *m;
    long w[3], lo[3], hi[3];
    unsigned i, v;
    int k, any = 0;

    if (!p || !min || !max) return 0;
    for (i = 0; i < p->count; ++i) {
        if (p->slot[p->record[i].slot].missing) continue;
        if (CadAssemblyGet(&p->assembly, p->record[i].id, &occ) !=
            CAD_ASSEMBLY_OK) continue;
        m = slot_model(p->record[i].slot);
        for (v = 0; v < m->vertex_count; ++v) {
            apply(&occ.local, &m->vertices[v], w);
            for (k = 0; k < 3; ++k) {
                if (!any || w[k] < lo[k]) lo[k] = w[k];
                if (!any || w[k] > hi[k]) hi[k] = w[k];
            }
            any = 1;
        }
    }
    if (!any) return 0;
    for (k = 0; k < 3; ++k) { min[k] = lo[k]; max[k] = hi[k]; }
    return 1;
}

/* Formats line i (0 = N line, i>0 = record i-1) into line; returns length. */
static unsigned format_line(const CadParts *p, unsigned i, char *line)
{
    CadAssemblyOccurrence occ;
    const CadPartsRecord *r;
    if (!i) return (unsigned)sprintf(line, "N %lu\n", p->assembly.next_id);
    r = &p->record[i - 1];
    memset(&occ, 0, sizeof(occ));
    CadAssemblyGet(&p->assembly, r->id, &occ);
    return (unsigned)sprintf(line, "O %lu %d %d %ld %ld %ld %s\n", r->id,
        r->yaw, occ.grounded ? 1 : 0, occ.local.t[0], occ.local.t[1],
        occ.local.t[2], p->slot[r->slot].path);
}

int cad_parts_write_text(const CadParts *p, char *buffer, unsigned capacity,
    unsigned *length)
{
    char line[CAD_PARTS_PATH_MAX + 64];
    unsigned i, total = 0, n;

    if (!p || !length || (!buffer && capacity)) return CAD_PARTS_ARGUMENT;
    for (i = 0; i <= p->count; ++i) total += format_line(p, i, line);
    if (total > capacity) return CAD_PARTS_CAPACITY;
    total = 0;
    for (i = 0; i <= p->count; ++i) {
        n = format_line(p, i, line);
        memcpy(buffer + total, line, n);
        total += n;
    }
    *length = total;
    return CAD_PARTS_OK;
}

/* Canonical decimal in [lo,hi] at *s, followed by stop (0 = end of line). */
static int parse_num(const char **s, const char *end, long lo, long hi,
                     char stop, long *out)
{
    const char *q = *s;
    long v = 0;
    int neg = 0, digits = 0;
    if (q < end && *q == '-') { neg = 1; ++q; }
    if (q < end && *q == '0' && q + 1 < end && q[1] >= '0' && q[1] <= '9')
        return 0;
    while (q < end && *q >= '0' && *q <= '9') {
        if (++digits > 6) return 0;
        v = v * 10L + (*q++ - '0');
    }
    if (!digits || (neg && !v)) return 0;
    if (neg) v = -v;
    if (v < lo || v > hi) return 0;
    if (stop) {
        if (q >= end || *q != stop) return 0;
        ++q;
    } else if (q != end) return 0;
    *s = q; *out = v;
    return 1;
}

int cad_parts_read_text(CadParts *p, const char *text, unsigned length,
    unsigned *line)
{
    CadParts *x = &parts_scratch;
    CadAssemblyTransform t;
    CadAssemblyId nid;
    long pos[CAD_PARTS_MAX][3], next = 1, v[6];
    int ground[CAD_PARTS_MAX];
    const char *s, *end;
    unsigned at = 0, no = 0, n, i;
    int slot, k;

    if (!p || (!text && length)) return CAD_PARTS_ARGUMENT;
    memset(x, 0, sizeof(*x));
    while (at < length) {
        ++no;
        s = text + at;
        for (end = s; end < text + length && *end != '\n'; ++end)
            if ((unsigned char)*end < 0x20 || (unsigned char)*end > 0x7e)
                goto bad;
        if (end >= text + length) goto bad; /* unterminated */
        at = (unsigned)(end - text) + 1;
        if (no == 1) {
            if (end - s < 2 || s[0] != 'N' || s[1] != ' ') goto bad;
            s += 2;
            if (!parse_num(&s, end, 1L, (long)CAD_ASSEMBLY_ID_MAX + 1L, 0,
                           &next)) goto bad;
            continue;
        }
        if (end - s < 2 || s[0] != 'O' || s[1] != ' ') goto bad;
        s += 2;
        if (!parse_num(&s, end, 1L, next - 1L, ' ', &v[0]) ||
            (x->count && (CadAssemblyId)v[0] <= x->record[x->count - 1].id) ||
            !parse_num(&s, end, 0L, CAD_ASSEMBLY_YAWS - 1L, ' ', &v[1]) ||
            !parse_num(&s, end, 0L, 1L, ' ', &v[2])) goto bad;
        for (k = 3; k < 6; ++k)
            if (!parse_num(&s, end, -CAD_PARTS_TRANSLATION,
                           CAD_PARTS_TRANSLATION, ' ', &v[k])) goto bad;
        n = path_length(s, (unsigned)(end - s));
        if (!n || n != (unsigned)(end - s)) goto bad;
        if (x->count >= CAD_PARTS_MAX) {
            if (line) *line = no;
            return CAD_PARTS_FULL;
        }
        slot = find_slot(x, s, n);
        if (slot < 0) {
            slot = free_slot(x);
            if (slot < 0) {
                if (line) *line = no;
                return CAD_PARTS_CACHE_FULL;
            }
            memcpy(x->slot[slot].path, s, n);
            x->slot[slot].path[n] = 0;
        }
        x->record[x->count].id = (CadAssemblyId)v[0];
        x->record[x->count].slot = slot;
        x->record[x->count].yaw = (int)v[1];
        ground[x->count] = (int)v[2];
        for (k = 0; k < 3; ++k) pos[x->count][k] = v[3 + k];
        ++x->count;
    }
    /* Rebuild the assembly with the recorded IDs (all roots). */
    CadAssemblyInit(&x->assembly);
    for (i = 0; i < x->count; ++i) {
        x->assembly.next_id = x->record[i].id;
        make_transform(pos[i][0], pos[i][1], pos[i][2], x->record[i].yaw, &t);
        if (CadAssemblyAdd(&x->assembly, (CadAssemblyId)x->record[i].slot + 1UL,
                           0, &t, &nid) != CAD_ASSEMBLY_OK ||
            (ground[i] && CadAssemblyGround(&x->assembly, nid, 1) !=
                CAD_ASSEMBLY_OK)) {
            if (line) *line = 0;
            return CAD_PARTS_FORMAT;
        }
    }
    x->assembly.next_id = (CadAssemblyId)next;
    for (k = 0; k < CAD_PARTS_CACHE; ++k)
        if (x->slot[k].path[0]) load_slot(x, k);
    *p = *x;
    if (line) *line = 0;
    return CAD_PARTS_OK;
bad:
    if (line) *line = no;
    return CAD_PARTS_FORMAT;
}
