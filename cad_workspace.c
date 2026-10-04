#include "cad_workspace.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>

static const unsigned char house_edges[40][3] = {
    {0,1,0},{1,2,0},{2,3,0},{3,0,0},{4,5,0},{5,6,0},{6,7,0},{7,4,0},
    {0,4,0},{1,5,0},{2,6,0},{3,7,0},{8,9,1},{4,8,1},{7,8,1},
    {5,9,1},{6,9,1},{10,11,2},{11,12,2},{12,13,2},
    {14,15,3},{15,16,3},{16,17,3},{17,14,3},
    {18,19,3},{19,20,3},{20,21,3},{21,18,3},
    {22,23,4},{23,24,4},{24,25,4},{25,22,4},
    {26,27,4},{27,28,4},{28,29,4},{29,26,4},
    {22,26,4},{23,27,4},{24,28,4},{25,29,4}
};

static int valid_units(CadUnits u)
{
    return u == CAD_UNITS_UNSPECIFIED || u == CAD_UNITS_MM ||
           u == CAD_UNITS_INCHES;
}
static int house_scale(const CadWorkspaceIntent *i)
{
    if (i->scale.ticks_per_model_unit <= 0) return 0;
#if LONG_MAX > 2147483647L
    if (i->scale.ticks_per_model_unit > CAD_LONG_MAX) return 0;
#endif
    if (i->units == CAD_UNITS_UNSPECIFIED)
        return i->scale.unit == CAD_UNIT_LEGACY;
    if (i->scale.unit != CAD_UNIT_MM) return 0;
    if (i->units == CAD_UNITS_MM)
        return i->scale.ticks_per_model_unit == CAD_SCALE;
    return i->units == CAD_UNITS_INCHES &&
           i->scale.ticks_per_model_unit == 2540L;
}
CadWorkspaceStatus cad_workspace_init(CadWorkspace *w)
{
    if (!w) return CAD_WORKSPACE_ARGUMENT;
    memset(w, 0, sizeof(*w));
    (void)cad_model_init(&w->model);
    return CAD_WORKSPACE_OK;
}
static CadWorkspaceStatus prepare(CadWorkspace *w, CadWorkspaceStage *s)
{
    if (!w || !s || (void *)w == (void *)s)
        return CAD_WORKSPACE_ARGUMENT;
    if (w->model.transaction_active) return CAD_WORKSPACE_MODEL;
    if (w->revision >= 2147483647UL) return CAD_WORKSPACE_REVISION;
    memcpy(&s->edit.candidate, w, sizeof(*w));
    return CAD_WORKSPACE_OK;
}
/* Candidate transaction batches may commit individually: live data is never
 * touched until all batches succeed. This avoids the core's 64-delta limit. */
static CadStatus room(CadModel *m)
{
    CadStatus r;
    if (!m->transaction_active) return cad_model_begin(m);
    if (m->staged_count < CAD_MAX_TRANSACTION_DELTAS) return CAD_OK;
    r = cad_model_commit(m);
    if (r != CAD_OK) return r;
    return cad_model_begin(m);
}
static CadWorkspaceStatus regenerate(CadWorkspace *w, CadWorkspaceStage *s,
                                    int replace)
{
    CadWorkspace *c = &s->edit.candidate;
    CadModel *m = &c->model;
    CadWorkspaceIntent *i = &c->intent;
    CadId *ids = s->edit.ids, id;
    int *a = s->edit.a, *b = s->edit.b;
    int nv, ne, j, k, t, q, x, y, z;
    CadSketchTriangle *tri;
    CadStatus r;
    if (i->kind == CAD_WORKSPACE_HOUSE) {
        if (!house_scale(i)) return CAD_WORKSPACE_ARGUMENT;
        if (cad_house_generate(&i->parameters, i->scale, s->edit.house, 0)
            != CAD_PARAM_OK) return CAD_WORKSPACE_PARAMETER;
        memset(&c->mesh, 0, sizeof(c->mesh));
        nv = CAD_HOUSE_VERTICES; ne = 40;
        for (j = 0; j < ne; ++j) {
            a[j] = house_edges[j][0]; b[j] = house_edges[j][1];
        }
    } else if (i->kind == CAD_WORKSPACE_EXTRUSION) {
        if (!valid_units(i->units)) return CAD_WORKSPACE_ARGUMENT;
        if (cad_sketch_extrude(i->profile, i->point_count, i->height, &c->mesh)
            != CAD_SKETCH_OK) return CAD_WORKSPACE_SKETCH;
        nv = c->mesh.vertex_count; ne = 0;
        /* Unique undirected mesh edges, including triangulation diagonals. */
        for (t = 0; t < c->mesh.triangle_count; ++t) {
            tri = &c->mesh.triangles[t];
            for (q = 0; q < 3; ++q) {
                j = q == 0 ? tri->a : (q == 1 ? tri->b : tri->c);
                k = q == 0 ? tri->b : (q == 1 ? tri->c : tri->a);
                if (j > k) { x = j; j = k; k = x; }
                for (x = 0; x < ne; ++x)
                    if (a[x] == j && b[x] == k) break;
                if (x == ne) {
                    if (ne == CAD_MAX_EDGES) return CAD_WORKSPACE_TOPOLOGY;
                    a[ne] = j; b[ne++] = k;
                }
            }
        }
    } else return CAD_WORKSPACE_UNSUPPORTED;
    if (!replace) {
        if (m->vertex_count != nv || m->edge_count != ne)
            return CAD_WORKSPACE_TOPOLOGY;
        for (j = 0; j < nv; ++j) ids[j] = m->vertices[j].id;
        for (j = 0; j < ne; ++j)
            if (m->edges[j].a != ids[a[j]] || m->edges[j].b != ids[b[j]])
                return CAD_WORKSPACE_TOPOLOGY;
    } else {
        while (m->edge_count) {
            if (room(m) != CAD_OK || cad_model_delete_edge(m,
                m->edges[m->edge_count-1].id) != CAD_OK)
                return CAD_WORKSPACE_MODEL;
        }
        while (m->vertex_count) {
            if (room(m) != CAD_OK || cad_model_delete_vertex(m,
                m->vertices[m->vertex_count-1].id) != CAD_OK)
                return CAD_WORKSPACE_MODEL;
        }
    }
    for (j = 0; j < nv; ++j) {
        if (i->kind == CAD_WORKSPACE_HOUSE) {
            x = s->edit.house[j].x; y = s->edit.house[j].y;
            z = s->edit.house[j].z;
        } else {
            x = c->mesh.vertices[j].x; y = c->mesh.vertices[j].y;
            z = c->mesh.vertices[j].z;
        }
        if (room(m) != CAD_OK) return CAD_WORKSPACE_MODEL;
        if (replace) r = cad_model_add_vertex(m, x, y, z, &ids[j]);
        else r = cad_model_move_vertex(m, ids[j], x, y, z);
        if (r != CAD_OK) return CAD_WORKSPACE_MODEL;
    }
    if (replace) for (j = 0; j < ne; ++j) {
        if (room(m) != CAD_OK ||
            cad_model_add_edge(m, ids[a[j]], ids[b[j]], &id) != CAD_OK)
            return CAD_WORKSPACE_MODEL;
    }
    if (room(m) != CAD_OK || cad_model_set_units(m, i->units) != CAD_OK ||
        cad_model_commit(m) != CAD_OK) return CAD_WORKSPACE_MODEL;
    c->revision = w->revision + 1;
    memcpy(w, c, sizeof(*w));
    return CAD_WORKSPACE_OK;
}
CadWorkspaceStatus cad_workspace_create_house(CadWorkspace *w,
    CadWorkspaceStage *s, CadUnits units, CadModelScale scale, unsigned flags)
{
    CadWorkspaceStatus r = prepare(w, s);
    CadWorkspaceIntent *i;
    unsigned j;
    if (r != CAD_WORKSPACE_OK) return r;
    if (flags != 0 && flags != CAD_WORKSPACE_MAP_DEFAULTS_TO_LEGACY)
        return CAD_WORKSPACE_ARGUMENT;
    i = &s->edit.candidate.intent;
    memset(i, 0, sizeof(*i));
    i->kind = CAD_WORKSPACE_HOUSE; i->units = units; i->scale = scale;
    if (!house_scale(i) || ((flags != 0) != (units == CAD_UNITS_UNSPECIFIED)))
        return CAD_WORKSPACE_ARGUMENT;
    cad_house_defaults(&i->parameters);
    if (flags) for (j = 0; j < i->parameters.count; ++j) {
        i->parameters.params[j].literal.unit = CAD_UNIT_LEGACY;
        i->parameters.params[j].value.unit = CAD_UNIT_LEGACY;
    }
    return regenerate(w, s, 1);
}
static CadWorkspaceStatus edit_parameter(CadWorkspace *w, CadWorkspaceStage *s,
    const char *name, const char *expression, const CadValue *value)
{
    CadWorkspaceStatus r;
    CadParams *p;
    unsigned id;
    unsigned j;
    const CadParameter *old;
    if (!w || !s || (void *)w == (void *)s || !name || (!expression && !value))
        return CAD_WORKSPACE_ARGUMENT;
    if (expression) {
        for (j = 0; j <= CAD_EXPR_MAX && expression[j]; ++j)
            if ((unsigned char)expression[j] < 32 ||
                (unsigned char)expression[j] > 126) return CAD_WORKSPACE_PARAMETER;
        if (j == 0 || j > CAD_EXPR_MAX) return CAD_WORKSPACE_PARAMETER;
    }
    if (w->intent.kind != CAD_WORKSPACE_HOUSE) return CAD_WORKSPACE_UNSUPPORTED;
    id = cad_params_find(&w->intent.parameters, name);
    if (!id) return CAD_WORKSPACE_PARAMETER;
    old = cad_params_get(&w->intent.parameters, id);
    if (expression && !strcmp(old->expression, expression)) return CAD_WORKSPACE_OK;
    if (value && !old->expression[0] && old->literal.ticks == value->ticks &&
        old->literal.type == value->type && old->literal.unit == value->unit)
        return CAD_WORKSPACE_OK;
    r = prepare(w, s); if (r != CAD_WORKSPACE_OK) return r;
    p = &s->edit.candidate.intent.parameters;
    if ((value ? cad_params_set_value(p, id, *value) :
        cad_params_set_expression(p, id, expression)) != CAD_PARAM_OK)
        return CAD_WORKSPACE_PARAMETER;
    return regenerate(w, s, 0);
}
CadWorkspaceStatus cad_workspace_change_house_parameter(CadWorkspace *w,
    CadWorkspaceStage *s, const char *name, const char *expression)
{
    return edit_parameter(w, s, name, expression, 0);
}
CadWorkspaceStatus cad_workspace_set_house_value(CadWorkspace *w,
    CadWorkspaceStage *s, const char *name, CadValue value)
{
    return edit_parameter(w, s, name, 0, &value);
}
CadWorkspaceStatus cad_workspace_create_extrusion(CadWorkspace *w,
    CadWorkspaceStage *s, const CadSketchPoint *profile, int count,
    int height, CadUnits units)
{
    CadWorkspaceStatus r;
    CadWorkspaceIntent *i;
    if (!profile || count < 3 || count > CAD_SKETCH_MAX_POINTS ||
        !valid_units(units)) return CAD_WORKSPACE_ARGUMENT;
    r = prepare(w, s); if (r != CAD_WORKSPACE_OK) return r;
    i = &s->edit.candidate.intent;
    memset(i, 0, sizeof(*i));
    i->kind = CAD_WORKSPACE_EXTRUSION; i->units = units;
    i->height = height; i->point_count = count;
    memcpy(i->profile, profile, (size_t)count * sizeof(*profile));
    return regenerate(w, s, 1);
}
CadWorkspaceStatus cad_workspace_change_height(CadWorkspace *w,
    CadWorkspaceStage *s, int height)
{
    CadWorkspaceStatus r;
    if (!w || !s || (void *)w == (void *)s) return CAD_WORKSPACE_ARGUMENT;
    if (w->intent.kind != CAD_WORKSPACE_EXTRUSION) return CAD_WORKSPACE_UNSUPPORTED;
    if (height == w->intent.height) return CAD_WORKSPACE_OK;
    r = prepare(w, s); if (r != CAD_WORKSPACE_OK) return r;
    s->edit.candidate.intent.height = height;
    return regenerate(w, s, 0);
}
CadWorkspaceStatus cad_workspace_undo(CadWorkspace *w)
{ return w ? CAD_WORKSPACE_UNSUPPORTED : CAD_WORKSPACE_ARGUMENT; }
CadWorkspaceStatus cad_workspace_constraints(CadWorkspace *w)
{ return cad_workspace_undo(w); }
CadWorkspaceStatus cad_workspace_assembly(CadWorkspace *w)
{ return cad_workspace_undo(w); }
CadWorkspaceStatus cad_workspace_macro(CadWorkspace *w)
{ return cad_workspace_undo(w); }
int cad_workspace_modified(const CadWorkspace *w)
{ return w && w->revision != w->saved_revision; }
void cad_workspace_mark_saved(CadWorkspace *w)
{ if (w) w->saved_revision = w->revision; }

CadWorkspaceStatus cad_workspace_materialize(const CadWorkspace *w,
    CadWorkspaceView *v)
{
    const CadModel *m;
    int j, k, a, b;
    if (!w || !v) return CAD_WORKSPACE_ARGUMENT;
    m = &w->model;
    if (m->transaction_active || m->vertex_count > CAD_MAX_VERTICES ||
        m->edge_count > CAD_MAX_EDGES) return CAD_WORKSPACE_MODEL;
    /* Preflight ALL IDs/endpoints/coordinates before writing the output. */
    for (j = 0; j < m->vertex_count; ++j) {
        if (!m->vertices[j].id || m->vertices[j].x < -400 || m->vertices[j].x > 400 ||
            m->vertices[j].y < -400 || m->vertices[j].y > 400 ||
            m->vertices[j].z < -400 || m->vertices[j].z > 400)
            return CAD_WORKSPACE_TOPOLOGY;
        for (k = 0; k < j; ++k)
            if (m->vertices[j].id == m->vertices[k].id) return CAD_WORKSPACE_TOPOLOGY;
    }
    for (j = 0; j < m->edge_count; ++j) {
        a = b = -1;
        if (!m->edges[j].id) return CAD_WORKSPACE_TOPOLOGY;
        for (k = 0; k < j; ++k)
            if (m->edges[j].id == m->edges[k].id) return CAD_WORKSPACE_TOPOLOGY;
        for (k = 0; k < m->vertex_count; ++k) {
            if (m->vertices[k].id == m->edges[j].a) a = k;
            if (m->vertices[k].id == m->edges[j].b) b = k;
        }
        if (a < 0 || b < 0) return CAD_WORKSPACE_TOPOLOGY;
    }
    v->vertex_count = m->vertex_count; v->edge_count = m->edge_count;
    for (j = 0; j < v->vertex_count; ++j) {
        v->ids[j] = m->vertices[j].id;
        v->vertices[j].x = (int)m->vertices[j].x;
        v->vertices[j].y = (int)m->vertices[j].y;
        v->vertices[j].z = (int)m->vertices[j].z;
        v->points[j].x = m->vertices[j].x;
        v->points[j].y = m->vertices[j].y;
        v->points[j].z = m->vertices[j].z;
    }
    for (j = 0; j < v->edge_count; ++j) {
        a = b = 0;
        for (k = 0; k < v->vertex_count; ++k) {
            if (v->ids[k] == m->edges[j].a) a = k;
            if (v->ids[k] == m->edges[j].b) b = k;
        }
        v->edges[j].a = v->lines[j].a = a;
        v->edges[j].b = v->lines[j].b = b;
        v->edges[j].style = w->intent.kind == CAD_WORKSPACE_HOUSE && j < 40 ?
                            house_edges[j][2] : CAD_EXPORT_WALL;
    }
    return CAD_WORKSPACE_OK;
}
int cad_workspace_export(const CadWorkspace *w, CadWorkspaceStage *s,
    const char *path, int format, const CadExportOptions *options)
{
    if (!s || (const void *)w == (void *)s ||
        cad_workspace_materialize(w, &s->view) != CAD_WORKSPACE_OK)
        return CAD_EXPORT_INVALID;
    return CadExportWrite(path, format, s->view.vertices, s->view.vertex_count,
                          s->view.edges, s->view.edge_count, options);
}
CadDrawingStatus cad_workspace_draw(const CadWorkspace *w, CadWorkspaceStage *s,
    const CadDrawingSheet *sheet, const CadDrawingSink *sink, CadDrawingLayout *out)
{
    CadDrawingModel m;
    if (!s || (const void *)w == (void *)s ||
        cad_workspace_materialize(w, &s->view) != CAD_WORKSPACE_OK)
        return CAD_DRAWING_INVALID;
    m.vertices = s->view.points; m.edges = s->view.lines;
    m.vertex_count = s->view.vertex_count; m.edge_count = s->view.edge_count;
    return CadDrawingRender(&m, sheet, sink, out);
}

/* Bounded line and numeric readers: strtol overflow is rejected on both ABIs.
 * File format uses one decimal field per line, no scanf numeric overflow. */
static int line(FILE *f, char *b, unsigned size)
{
    unsigned n = 0;
    int ch;
    while ((ch = fgetc(f)) != EOF && ch != '\n') {
        if (ch < 32 || ch > 126 || n + 1 >= size) return 0;
        b[n++] = (char)ch;
    }
    if (ch != '\n') return 0;
    b[n] = 0; return 1;
}
static int number(FILE *f, long *value)
{
    char b[32], *end;
    long n;
    unsigned j;
    if (!line(f, b, sizeof(b)) || !b[0]) return 0;
    j = b[0] == '-' ? 1U : 0U;
    if (!b[j]) return 0;
    for (; b[j]; ++j) if (b[j] < '0' || b[j] > '9') return 0;
    errno = 0; n = strtol(b, &end, 10);
    if (errno == ERANGE || *end) return 0;
#if LONG_MAX > 2147483647L
    if (n < CAD_LONG_MIN || n > CAD_LONG_MAX) return 0;
#endif
    *value = n; return 1;
}
static int bounded(FILE *f, long *v, long lo, long hi)
{ return number(f, v) && *v >= lo && *v <= hi; }
CadWorkspaceStatus cad_workspace_write(const CadWorkspace *w, FILE *f)
{
    const CadWorkspaceIntent *i;
    const CadParameter *p;
    unsigned j;
    if (!w || !f) return CAD_WORKSPACE_ARGUMENT;
    i = &w->intent;
    if (i->kind != CAD_WORKSPACE_HOUSE && i->kind != CAD_WORKSPACE_EXTRUSION)
        return CAD_WORKSPACE_UNSUPPORTED;
    if (fprintf(f, "CWS 1\n%ld\n%ld\n", (long)i->kind, (long)i->units) < 0)
        return CAD_WORKSPACE_IO;
    if (i->kind == CAD_WORKSPACE_HOUSE) {
        if (fprintf(f, "%ld\n%ld\n%u\n", (long)i->scale.unit,
            i->scale.ticks_per_model_unit, i->parameters.count) < 0)
            return CAD_WORKSPACE_IO;
        for (j = 0; j < i->parameters.count; ++j) {
            p = &i->parameters.params[j];
            if (fprintf(f, "%s\n%s\n%ld\n%ld\n%ld\n", p->name, p->expression,
                (long)p->literal.type, (long)p->literal.unit, p->literal.ticks) < 0)
                return CAD_WORKSPACE_IO;
        }
    } else {
        if (fprintf(f, "%d\n%d\n", i->point_count, i->height) < 0)
            return CAD_WORKSPACE_IO;
        for (j = 0; j < (unsigned)i->point_count; ++j)
            if (fprintf(f, "%d\n%d\n", i->profile[j].x, i->profile[j].y) < 0)
                return CAD_WORKSPACE_IO;
    }
    if (fputs("END\n", f) == EOF || fflush(f) || ferror(f)) return CAD_WORKSPACE_IO;
    return CAD_WORKSPACE_OK;
}
CadWorkspaceStatus cad_workspace_read(CadWorkspace *w, CadWorkspaceStage *s, FILE *f)
{
    CadWorkspaceStatus r;
    CadWorkspaceIntent *i;
    CadValue v;
    char text[CAD_EXPR_MAX + 1], name[CAD_NAME_MAX + 1];
    long n;
    unsigned count, j, id;
    if (!f) return CAD_WORKSPACE_ARGUMENT;
    r = prepare(w, s); if (r != CAD_WORKSPACE_OK) return r;
    i = &s->edit.candidate.intent;
    memset(i, 0, sizeof(*i));
    if (!line(f, text, sizeof(text)) || strcmp(text, "CWS 1")) goto bad;
    if (!bounded(f, &n, 1, 2)) goto bad;
    i->kind = (CadWorkspaceKind)n;
    if (!bounded(f, &n, 0, 2)) goto bad;
    i->units = (CadUnits)n;
    if (i->kind == CAD_WORKSPACE_HOUSE) {
        if (!bounded(f, &n, 1, 2)) goto bad;
        i->scale.unit = (CadUnit)n;
        if (!bounded(f, &n, 1, CAD_LONG_MAX)) goto bad;
        i->scale.ticks_per_model_unit = n;
        if (!house_scale(i) || !bounded(f, &n, 1, CAD_PARAM_MAX)) goto bad;
        count = (unsigned)n;
        cad_params_init(&i->parameters);
        for (j = 0; j < count; ++j) {
            if (!line(f, name, sizeof(name)) || !line(f, text, sizeof(text))) goto bad;
            if (!bounded(f, &n, 0, 1)) goto bad;
            v.type = (CadType)n;
            if (!bounded(f, &n, 0, 2)) goto bad;
            v.unit = (CadUnit)n;
            if (!number(f, &v.ticks)) goto bad;
            if (cad_params_add(&i->parameters, name, v, &id) != CAD_PARAM_OK ||
                (text[0] && cad_params_set_expression(&i->parameters, id, text)
                 != CAD_PARAM_OK)) goto bad;
        }
    } else {
        if (!bounded(f, &n, 3, CAD_SKETCH_MAX_POINTS)) goto bad;
        i->point_count = (int)n;
        if (!bounded(f, &n, 1, 400)) goto bad;
        i->height = (int)n;
        for (j = 0; j < (unsigned)i->point_count; ++j) {
            if (!bounded(f, &n, -400, 400)) goto bad;
            i->profile[j].x = (int)n;
            if (!bounded(f, &n, -400, 400)) goto bad;
            i->profile[j].y = (int)n;
        }
    }
    if (!line(f, text, sizeof(text)) || strcmp(text, "END") || fgetc(f) != EOF)
        goto bad;
    if (ferror(f)) return CAD_WORKSPACE_IO;
    r = regenerate(w, s, 1);
    if (r == CAD_WORKSPACE_OK) cad_workspace_mark_saved(w);
    return r;
bad:
    return ferror(f) ? CAD_WORKSPACE_IO : CAD_WORKSPACE_FORMAT;
}
