#include "cad_model.h"
#include <string.h>

typedef char CadDeltaMustBe16Bytes[(sizeof(CadDelta) == 16) ? 1 : -1];
typedef char CadHistoryMustFit[(sizeof(((CadModel *)0)->history) +
    sizeof(((CadModel *)0)->staged) + sizeof(((CadModel *)0)->lengths) +
    5 * sizeof(unsigned short) < 32768UL) ? 1 : -1];

#define V_ADD 1
#define V_REMOVE 2
#define V_MOVE 3
#define E_ADD 4
#define E_REMOVE 5
#define UNITS 6

static int vertex_index(const CadModel *m, CadId id)
{
    unsigned short i;
    for (i = 0; i < m->vertex_count; ++i)
        if (m->vertices[i].id == id) return (int)i;
    return -1;
}

static int edge_index(const CadModel *m, CadId id)
{
    unsigned short i;
    for (i = 0; i < m->edge_count; ++i)
        if (m->edges[i].id == id) return (int)i;
    return -1;
}

static int in_range(long x, long y, long z)
{
    return x >= CAD_COORD_MIN && x <= CAD_COORD_MAX &&
           y >= CAD_COORD_MIN && y <= CAD_COORD_MAX &&
           z >= CAD_COORD_MIN && z <= CAD_COORD_MAX;
}

static CadStatus room(CadModel *m, unsigned short count)
{
    if (!m) return CAD_INVALID_ARGUMENT;
    if (!m->transaction_active) return CAD_NO_TRANSACTION;
    if (count > CAD_MAX_TRANSACTION_DELTAS - m->staged_count)
        return CAD_TRANSACTION_FULL;
    return CAD_OK;
}

/* Endpoint words use a reversible bias, never an implementation-defined
 * conversion from an unsigned ID above SHRT_MAX to signed short. */
static short pack_id(CadId id) { return (short)((long)id - 32768L); }
static CadId unpack_id(short word) { return (CadId)((long)word + 32768L); }

static void apply(CadModel *m, const CadDelta *d, int forward)
{
    int i, add, offset;
    CadVertex *v;
    CadEdge *e;
    if (d->kind == V_ADD || d->kind == V_REMOVE) {
        add = (d->kind == V_ADD) == forward;
        if (add) {
            v = &m->vertices[m->vertex_count++];
            v->id = d->id;
            v->x = d->data[0]; v->y = d->data[1]; v->z = d->data[2];
        } else {
            i = vertex_index(m, d->id);
            --m->vertex_count;
            m->vertices[i] = m->vertices[m->vertex_count];
        }
    } else if (d->kind == V_MOVE) {
        v = &m->vertices[vertex_index(m, d->id)];
        offset = forward ? 3 : 0;
        v->x = d->data[offset]; v->y = d->data[offset + 1];
        v->z = d->data[offset + 2];
    } else if (d->kind == E_ADD || d->kind == E_REMOVE) {
        add = (d->kind == E_ADD) == forward;
        if (add) {
            e = &m->edges[m->edge_count++];
            e->id = d->id;
            e->a = unpack_id(d->data[0]); e->b = unpack_id(d->data[1]);
        } else {
            i = edge_index(m, d->id);
            --m->edge_count;
            m->edges[i] = m->edges[m->edge_count];
        }
    } else {
        m->units = (CadUnits)d->data[forward ? 1 : 0];
    }
}

static void stage(CadModel *m, const CadDelta *d)
{
    m->staged[m->staged_count++] = *d;
    apply(m, d, 1);
}

static CadDelta vertex_delta(unsigned short kind, const CadVertex *v)
{
    CadDelta d;
    memset(&d, 0, sizeof(d));
    d.kind = kind; d.id = v->id;
    d.data[0] = (short)v->x; d.data[1] = (short)v->y;
    d.data[2] = (short)v->z;
    return d;
}

static CadDelta edge_delta(unsigned short kind, const CadEdge *e)
{
    CadDelta d;
    memset(&d, 0, sizeof(d));
    d.kind = kind; d.id = e->id;
    d.data[0] = pack_id(e->a); d.data[1] = pack_id(e->b);
    return d;
}

CadStatus cad_model_init(CadModel *m)
{
    if (!m) return CAD_INVALID_ARGUMENT;
    memset(m, 0, sizeof(*m));
    m->next_vertex_id = m->next_edge_id = 1L;
    return CAD_OK;
}

CadStatus cad_model_begin(CadModel *m)
{
    if (!m) return CAD_INVALID_ARGUMENT;
    if (m->transaction_active) return CAD_TRANSACTION_ACTIVE;
    m->transaction_active = 1; m->staged_count = 0;
    return CAD_OK;
}

CadStatus cad_model_add_vertex(CadModel *m, long x, long y, long z, CadId *id)
{
    CadVertex v;
    CadDelta d;
    CadStatus s;
    if (!m || !id) return CAD_INVALID_ARGUMENT;
    s = room(m, 1); if (s != CAD_OK) return s;
    if (!in_range(x, y, z)) return CAD_RANGE;
    if (m->vertex_count == CAD_MAX_VERTICES) return CAD_CAPACITY;
    if (m->next_vertex_id > 65535L) return CAD_ID_EXHAUSTED;
    v.id = (CadId)m->next_vertex_id; v.x = x; v.y = y; v.z = z;
    d = vertex_delta(V_ADD, &v);
    stage(m, &d); ++m->next_vertex_id; *id = v.id;
    return CAD_OK;
}

CadStatus cad_model_move_vertex(CadModel *m, CadId id, long x, long y, long z)
{
    CadDelta d;
    int i;
    CadStatus s = room(m, 0);
    if (s != CAD_OK) return s;
    if (!in_range(x, y, z)) return CAD_RANGE;
    i = vertex_index(m, id); if (i < 0) return CAD_NOT_FOUND;
    if (m->vertices[i].x == x && m->vertices[i].y == y &&
        m->vertices[i].z == z) return CAD_OK;
    s = room(m, 1); if (s != CAD_OK) return s;
    d = vertex_delta(V_MOVE, &m->vertices[i]);
    d.data[3] = (short)x; d.data[4] = (short)y; d.data[5] = (short)z;
    stage(m, &d);
    return CAD_OK;
}

CadStatus cad_model_add_edge(CadModel *m, CadId a, CadId b, CadId *id)
{
    CadEdge e;
    CadDelta d;
    CadStatus s;
    if (!m || !id) return CAD_INVALID_ARGUMENT;
    s = room(m, 1); if (s != CAD_OK) return s;
    if (vertex_index(m, a) < 0 || vertex_index(m, b) < 0)
        return CAD_NOT_FOUND;
    if (m->edge_count == CAD_MAX_EDGES) return CAD_CAPACITY;
    if (m->next_edge_id > 65535L) return CAD_ID_EXHAUSTED;
    e.id = (CadId)m->next_edge_id; e.a = a; e.b = b;
    d = edge_delta(E_ADD, &e);
    stage(m, &d); ++m->next_edge_id; *id = e.id;
    return CAD_OK;
}

CadStatus cad_model_delete_edge(CadModel *m, CadId id)
{
    int i;
    CadDelta d;
    CadStatus s = room(m, 1);
    if (s != CAD_OK) return s;
    i = edge_index(m, id); if (i < 0) return CAD_NOT_FOUND;
    d = edge_delta(E_REMOVE, &m->edges[i]); stage(m, &d);
    return CAD_OK;
}

CadStatus cad_model_delete_vertex(CadModel *m, CadId id)
{
    int i;
    unsigned short j, needed = 1;
    CadDelta d;
    CadStatus s = room(m, 0);
    if (s != CAD_OK) return s;
    i = vertex_index(m, id); if (i < 0) return CAD_NOT_FOUND;
    for (j = 0; j < m->edge_count; ++j)
        if (m->edges[j].a == id || m->edges[j].b == id) ++needed;
    s = room(m, needed); if (s != CAD_OK) return s;
    j = 0;
    while (j < m->edge_count) {
        if (m->edges[j].a == id || m->edges[j].b == id) {
            d = edge_delta(E_REMOVE, &m->edges[j]); stage(m, &d);
        } else ++j;
    }
    d = vertex_delta(V_REMOVE, &m->vertices[i]); stage(m, &d);
    return CAD_OK;
}

CadStatus cad_model_set_units(CadModel *m, CadUnits units)
{
    CadDelta d;
    CadStatus s = room(m, 0);
    if (s != CAD_OK) return s;
    if (units != CAD_UNITS_UNSPECIFIED && units != CAD_UNITS_MM &&
        units != CAD_UNITS_INCHES) return CAD_INVALID_ARGUMENT;
    if (units == m->units) return CAD_OK;
    s = room(m, 1); if (s != CAD_OK) return s;
    memset(&d, 0, sizeof(d)); d.kind = UNITS;
    d.data[0] = (short)m->units; d.data[1] = (short)units;
    stage(m, &d); return CAD_OK;
}

CadStatus cad_model_get_vertex(const CadModel *m, CadId id, CadVertex *v)
{
    int i;
    if (!m || !v) return CAD_INVALID_ARGUMENT;
    i = vertex_index(m, id); if (i < 0) return CAD_NOT_FOUND;
    *v = m->vertices[i]; return CAD_OK;
}

CadStatus cad_model_get_edge(const CadModel *m, CadId id, CadEdge *e)
{
    int i;
    if (!m || !e) return CAD_INVALID_ARGUMENT;
    i = edge_index(m, id); if (i < 0) return CAD_NOT_FOUND;
    *e = m->edges[i]; return CAD_OK;
}

/* Compare each entity's first preimage with its final state. This detects
 * move-back, add/delete and metadata no-ops without a geometry snapshot. */
static int changed(const CadModel *m)
{
    unsigned short i, j, group, prior;
    int k;
    const CadDelta *d;
    const CadVertex *v;
    const CadEdge *e;
    for (i = 0; i < m->staged_count; ++i) {
        d = &m->staged[i];
        group = d->kind <= V_MOVE ? 0 : (d->kind <= E_REMOVE ? 1 : 2);
        for (j = 0; j < i; ++j) {
            prior = m->staged[j].kind <= V_MOVE ? 0 :
                (m->staged[j].kind <= E_REMOVE ? 1 : 2);
            if (prior == group && m->staged[j].id == d->id) break;
        }
        if (j < i) continue;
        if (group == 0) {
            k = vertex_index(m, d->id);
            if (d->kind == V_ADD) { if (k >= 0) return 1; }
            else {
                if (k < 0) return 1;
                v = &m->vertices[k];
                if (v->x != d->data[0] || v->y != d->data[1] ||
                    v->z != d->data[2]) return 1;
            }
        } else if (group == 1) {
            k = edge_index(m, d->id);
            if (d->kind == E_ADD) { if (k >= 0) return 1; }
            else {
                if (k < 0) return 1;
                e = &m->edges[k];
                if (e->a != unpack_id(d->data[0]) ||
                    e->b != unpack_id(d->data[1])) return 1;
            }
        } else if (m->units != (CadUnits)d->data[0]) return 1;
    }
    return 0;
}

CadStatus cad_model_cancel(CadModel *m)
{
    if (!m) return CAD_INVALID_ARGUMENT;
    if (!m->transaction_active) return CAD_NO_TRANSACTION;
    while (m->staged_count) apply(m, &m->staged[--m->staged_count], 0);
    m->transaction_active = 0; return CAD_OK;
}

CadStatus cad_model_commit(CadModel *m)
{
    unsigned short i, used = 0, remove;
    if (!m) return CAD_INVALID_ARGUMENT;
    if (!m->transaction_active) return CAD_NO_TRANSACTION;
    if (changed(m)) {
        for (i = 0; i < m->history_cursor; ++i) used += m->lengths[i];
        m->history_count = m->history_cursor; m->history_used = used;
        while (m->history_count == CAD_MAX_HISTORY_COMMANDS ||
               m->history_used + m->staged_count > CAD_HISTORY_DELTAS) {
            remove = m->lengths[0];
            m->history_used -= remove; --m->history_count;
            memmove(m->history, m->history + remove,
                    (size_t)m->history_used * sizeof(CadDelta));
            memmove(m->lengths, m->lengths + 1,
                    (size_t)m->history_count * sizeof(unsigned short));
        }
        memcpy(m->history + m->history_used, m->staged,
               (size_t)m->staged_count * sizeof(CadDelta));
        m->history_used += m->staged_count;
        m->lengths[m->history_count++] = m->staged_count;
        m->history_cursor = m->history_count;
    }
    m->staged_count = 0; m->transaction_active = 0; return CAD_OK;
}

CadStatus cad_model_undo(CadModel *m)
{
    unsigned short i, end = 0, start;
    if (!m) return CAD_INVALID_ARGUMENT;
    if (m->transaction_active) return CAD_TRANSACTION_ACTIVE;
    if (!m->history_cursor) return CAD_NO_HISTORY;
    for (i = 0; i < m->history_cursor; ++i) end += m->lengths[i];
    start = end - m->lengths[m->history_cursor - 1];
    while (end > start) apply(m, &m->history[--end], 0);
    --m->history_cursor; return CAD_OK;
}

CadStatus cad_model_redo(CadModel *m)
{
    unsigned short i, start = 0, end;
    if (!m) return CAD_INVALID_ARGUMENT;
    if (m->transaction_active) return CAD_TRANSACTION_ACTIVE;
    if (m->history_cursor == m->history_count) return CAD_NO_HISTORY;
    for (i = 0; i < m->history_cursor; ++i) start += m->lengths[i];
    end = start + m->lengths[m->history_cursor];
    while (start < end) apply(m, &m->history[start++], 1);
    ++m->history_cursor; return CAD_OK;
}

const char *cad_model_status_string(CadStatus status)
{
    switch (status) {
    case CAD_OK: return "ok";
    case CAD_INVALID_ARGUMENT: return "invalid argument";
    case CAD_RANGE: return "coordinate outside -400..400";
    case CAD_NOT_FOUND: return "ID not found";
    case CAD_CAPACITY: return "model capacity reached";
    case CAD_ID_EXHAUSTED: return "16-bit ID namespace exhausted";
    case CAD_TRANSACTION_ACTIVE: return "transaction already active";
    case CAD_NO_TRANSACTION: return "no active transaction";
    case CAD_TRANSACTION_FULL: return "transaction exceeds 64 deltas";
    case CAD_NO_HISTORY: return "no history in this direction";
    default: return "unknown status";
    }
}
