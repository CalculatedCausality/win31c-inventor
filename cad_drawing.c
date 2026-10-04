#include "cad_drawing.h"
#include <limits.h>
#include <stdio.h>

static int in32(long n)
{
#if LONG_MAX > 2147483647L
    return n >= CAD_DRAWING_I32_MIN && n <= CAD_DRAWING_I32_MAX;
#else
    (void)n;
    return 1;
#endif
}

static int point32(const CadDrawingPoint *p)
{
    return in32(p->x) && in32(p->y) && in32(p->z);
}

static CadDrawingStatus add32(long a, long b, long *out)
{
    if ((b > 0 && a > CAD_DRAWING_I32_MAX - b) ||
        (b < 0 && a < CAD_DRAWING_I32_MIN - b))
        return CAD_DRAWING_OVERFLOW;
    *out = a + b;
    return CAD_DRAWING_OK;
}

/* hi >= lo, both already range checked. */
static CadDrawingStatus distance(long hi, long lo, long *out)
{
    if (lo < 0 && hi > CAD_DRAWING_I32_MAX + lo)
        return CAD_DRAWING_OVERFLOW;
    *out = hi - lo;
    return CAD_DRAWING_OK;
}

static CadDrawingStatus scaled(long n, CadDrawingScale scale, long *out)
{
    if (n > CAD_DRAWING_I32_MAX / scale.numerator)
        return CAD_DRAWING_OVERFLOW;
    *out = (n * scale.numerator) / scale.denominator;
    return CAD_DRAWING_OK;
}

CadDrawingStatus CadDrawingProject(CadDrawingView view,
    const CadDrawingPoint *point, CadDrawingUV *out)
{
    CadDrawingUV p;
    long negative;
    if (point == NULL || out == NULL || view < CAD_DRAWING_FRONT ||
        view > CAD_DRAWING_BOTTOM) return CAD_DRAWING_INVALID;
    if (!point32(point)) return CAD_DRAWING_RANGE;
    p.u = point->x;
    p.v = point->y;
    negative = 0;
    switch (view) {
    case CAD_DRAWING_FRONT: break;
    case CAD_DRAWING_TOP: negative = point->z; p.v = 0; break;
    case CAD_DRAWING_RIGHT: negative = point->z; p.u = 0; break;
    case CAD_DRAWING_LEFT: p.u = point->z; break;
    case CAD_DRAWING_REAR: negative = point->x; p.u = 0; break;
    case CAD_DRAWING_BOTTOM: p.v = point->z; break;
    }
    if (negative == CAD_DRAWING_I32_MIN) return CAD_DRAWING_OVERFLOW;
    if (view == CAD_DRAWING_TOP) p.v = -negative;
    if (view == CAD_DRAWING_RIGHT || view == CAD_DRAWING_REAR)
        p.u = -negative;
    *out = p;
    return CAD_DRAWING_OK;
}

CadDrawingStatus CadDrawingGetBounds(const CadDrawingModel *model,
    CadDrawingBounds *out)
{
    CadDrawingBounds b;
    const CadDrawingPoint *p;
    CadDrawingStatus s;
    int i;
    if (model == NULL || out == NULL || model->vertices == NULL ||
        model->vertex_count < 1 ||
        model->vertex_count > CAD_DRAWING_MAX_VERTICES ||
        model->edge_count < 0 || model->edge_count > CAD_DRAWING_MAX_EDGES ||
        (model->edge_count != 0 && model->edges == NULL))
        return CAD_DRAWING_INVALID;
    for (i = 0; i < model->edge_count; ++i) {
        if (model->edges[i].a < 0 || model->edges[i].b < 0 ||
            model->edges[i].a >= model->vertex_count ||
            model->edges[i].b >= model->vertex_count)
            return CAD_DRAWING_INVALID;
    }
    b.min = b.max = model->vertices[0];
    for (i = 0; i < model->vertex_count; ++i) {
        p = &model->vertices[i];
        if (!point32(p)) return CAD_DRAWING_RANGE;
        if (p->x < b.min.x) b.min.x = p->x;
        if (p->y < b.min.y) b.min.y = p->y;
        if (p->z < b.min.z) b.min.z = p->z;
        if (p->x > b.max.x) b.max.x = p->x;
        if (p->y > b.max.y) b.max.y = p->y;
        if (p->z > b.max.z) b.max.z = p->z;
    }
    s = distance(b.max.x, b.min.x, &b.width);
    if (s != CAD_DRAWING_OK) return s;
    s = distance(b.max.y, b.min.y, &b.height);
    if (s != CAD_DRAWING_OK) return s;
    s = distance(b.max.z, b.min.z, &b.depth);
    if (s != CAD_DRAWING_OK) return s;
    *out = b;
    return CAD_DRAWING_OK;
}

CadDrawingStatus CadDrawingMakeLayout(const CadDrawingModel *model,
    const CadDrawingSheet *sheet, CadDrawingLayout *out)
{
    CadDrawingLayout l;
    CadDrawingStatus s;
    CadDrawingUV uv;
    long w, h, d, limit;
    int i, v;
    if (sheet == NULL || out == NULL) return CAD_DRAWING_INVALID;
    if (!in32(sheet->width) || !in32(sheet->height) ||
        !in32(sheet->margin) || !in32(sheet->gap) ||
        !in32(sheet->scale.numerator) || !in32(sheet->scale.denominator))
        return CAD_DRAWING_RANGE;
    if (sheet->width < 1 || sheet->width > 32767L ||
        sheet->height < 1 || sheet->height > 32767L ||
        sheet->margin < 8 || sheet->gap < 32 ||
        sheet->scale.numerator < 1 || sheet->scale.denominator < 1)
        return CAD_DRAWING_RANGE;
    s = CadDrawingGetBounds(model, &l.model_bounds);
    if (s != CAD_DRAWING_OK) return s;
    /* Projection's signed basis must also be representable, even when
     * normalizing a constant coordinate would otherwise hide its overflow. */
    for (i = 0; i < model->vertex_count; ++i) {
        for (v = 0; v < 3; ++v) {
            s = CadDrawingProject((CadDrawingView)v, &model->vertices[i], &uv);
            if (s != CAD_DRAWING_OK) return s;
        }
    }
    l.scale = sheet->scale;
    s = scaled(l.model_bounds.width, l.scale, &w);
    if (s != CAD_DRAWING_OK) return s;
    s = scaled(l.model_bounds.height, l.scale, &h);
    if (s != CAD_DRAWING_OK) return s;
    s = scaled(l.model_bounds.depth, l.scale, &d);
    if (s != CAD_DRAWING_OK) return s;
    l.sheet_bounds.left = l.sheet_bounds.top = 0;
    l.sheet_bounds.right = sheet->width;
    l.sheet_bounds.bottom = sheet->height;
    l.background_white = l.foreground_black = 1;
    s = add32(sheet->margin, 24L, &l.views[0].left);
    if (s != CAD_DRAWING_OK) return s;
    l.views[1].left = l.views[0].left;
    s = add32(sheet->margin, 12L, &l.views[1].top);
    if (s != CAD_DRAWING_OK) return s;
    s = add32(l.views[1].top, d, &l.views[1].bottom);
    if (s != CAD_DRAWING_OK) return s;
    s = add32(l.views[0].left, w, &l.views[0].right);
    if (s != CAD_DRAWING_OK) return s;
    l.views[1].right = l.views[0].right;
    s = add32(l.views[1].bottom, sheet->gap, &l.views[0].top);
    if (s != CAD_DRAWING_OK) return s;
    s = add32(l.views[0].top, h, &l.views[0].bottom);
    if (s != CAD_DRAWING_OK) return s;
    s = add32(l.views[0].right, sheet->gap, &l.views[2].left);
    if (s != CAD_DRAWING_OK) return s;
    s = add32(l.views[2].left, d, &l.views[2].right);
    if (s != CAD_DRAWING_OK) return s;
    l.views[2].top = l.views[0].top;
    l.views[2].bottom = l.views[0].bottom;
    s = add32(sheet->height, -sheet->margin, &l.title_top);
    if (s != CAD_DRAWING_OK) return s;
    s = add32(l.title_top, -36L, &l.title_top);
    if (s != CAD_DRAWING_OK) return s;
    s = add32(l.views[0].bottom, 24L, &limit);
    if (s != CAD_DRAWING_OK) return s;
    if (limit > l.title_top) return CAD_DRAWING_NO_ROOM;
    s = add32(l.views[2].right, 12L, &limit);
    if (s != CAD_DRAWING_OK) return s;
    if (limit > sheet->width - sheet->margin) return CAD_DRAWING_NO_ROOM;
    *out = l;
    return CAD_DRAWING_OK;
}

/* Normalize against one shared model datum. Front/top use min X; front/right
 * use max Y. These differences preserve alignment even with fractional scale
 * and translated models. Top screen Y is +Z; right screen X is -Z. */
static CadDrawingStatus map_point(const CadDrawingLayout *l, int view,
    const CadDrawingPoint *p, CadDrawingUV *out)
{
    long u, v;
    CadDrawingStatus s;
    if (view == CAD_DRAWING_RIGHT)
        s = distance(l->model_bounds.max.z, p->z, &u);
    else s = distance(p->x, l->model_bounds.min.x, &u);
    if (s != CAD_DRAWING_OK) return s;
    if (view == CAD_DRAWING_TOP)
        s = distance(p->z, l->model_bounds.min.z, &v);
    else s = distance(l->model_bounds.max.y, p->y, &v);
    if (s != CAD_DRAWING_OK) return s;
    s = scaled(u, l->scale, &u);
    if (s != CAD_DRAWING_OK) return s;
    s = scaled(v, l->scale, &v);
    if (s != CAD_DRAWING_OK) return s;
    s = add32(l->views[view].left, u, &out->u);
    if (s != CAD_DRAWING_OK) return s;
    return add32(l->views[view].top, v, &out->v);
}

static int line(const CadDrawingSink *sink, long a, long b, long c, long d,
    unsigned int flags)
{
    return sink->line(sink->context, a, b, c, d,
                      flags | CAD_DRAWING_SOLID);
}

static int dimension(const CadDrawingSink *sink, const CadDrawingRect *r,
    int vertical, long value)
{
    char label[12];
    long a, b, c;
    unsigned int f;
    f = CAD_DRAWING_DIMENSION;
    sprintf(label, "%ld", value);
    if (vertical) {
        a = r->left - 8;
        b = r->top;
        c = r->bottom;
        if (!line(sink, a, b, a, c, f) ||
            !line(sink, r->left, b, a - 3, b, f | CAD_DRAWING_EXTENSION) ||
            !line(sink, r->left, c, a - 3, c, f | CAD_DRAWING_EXTENSION) ||
            !line(sink, a - 2, b + 2, a + 2, b - 2, f) ||
            !line(sink, a - 2, c + 2, a + 2, c - 2, f)) return 0;
        return sink->text(sink->context, r->left - 24, b + (c - b) / 2,
                          label, f | CAD_DRAWING_LABEL);
    }
    a = r->left;
    b = r->right;
    c = r->bottom + 8;
    if (!line(sink, a, c, b, c, f) ||
        !line(sink, a, r->bottom, a, c + 3, f | CAD_DRAWING_EXTENSION) ||
        !line(sink, b, r->bottom, b, c + 3, f | CAD_DRAWING_EXTENSION) ||
        !line(sink, a - 2, c + 2, a + 2, c - 2, f) ||
        !line(sink, b - 2, c + 2, b + 2, c - 2, f)) return 0;
    return sink->text(sink->context, a + (b - a) / 2, c + 4, label,
                      f | CAD_DRAWING_LABEL);
}

CadDrawingStatus CadDrawingRender(const CadDrawingModel *model,
    const CadDrawingSheet *sheet, const CadDrawingSink *sink,
    CadDrawingLayout *out)
{
    CadDrawingLayout l;
    CadDrawingStatus s;
    CadDrawingUV a, b;
    const CadDrawingEdge *e;
    const char *names[3];
    long m, r, bottom;
    int i, v;
    if (sink == NULL || sink->line == NULL || sink->text == NULL)
        return CAD_DRAWING_INVALID;
    s = CadDrawingMakeLayout(model, sheet, &l);
    if (s != CAD_DRAWING_OK) return s;
    /* All mapped coordinates are bounded by scaled overall extents already
     * checked above. Preflight nevertheless covers every point explicitly. */
    for (v = 0; v < 3; ++v) {
        for (i = 0; i < model->vertex_count; ++i) {
            s = map_point(&l, v, &model->vertices[i], &a);
            if (s != CAD_DRAWING_OK) return s;
        }
    }
    m = sheet->margin;
    r = sheet->width - m;
    bottom = sheet->height - m;
    if (!line(sink, m, m, r, m, CAD_DRAWING_BORDER) ||
        !line(sink, r, m, r, bottom, CAD_DRAWING_BORDER) ||
        !line(sink, r, bottom, m, bottom, CAD_DRAWING_BORDER) ||
        !line(sink, m, bottom, m, m, CAD_DRAWING_BORDER) ||
        !line(sink, m, l.title_top, r, l.title_top,
              CAD_DRAWING_BORDER | CAD_DRAWING_TITLE))
        return CAD_DRAWING_SINK_FAILED;
    names[0] = "FRONT"; names[1] = "TOP"; names[2] = "RIGHT";
    for (v = 0; v < 3; ++v) {
        if (!sink->text(sink->context, l.views[v].left,
                        l.views[v].top - 12, names[v], CAD_DRAWING_LABEL))
            return CAD_DRAWING_SINK_FAILED;
        for (i = 0; i < model->edge_count; ++i) {
            e = &model->edges[i];
            s = map_point(&l, v, &model->vertices[e->a], &a);
            if (s != CAD_DRAWING_OK) return s;
            s = map_point(&l, v, &model->vertices[e->b], &b);
            if (s != CAD_DRAWING_OK) return s;
            if (!line(sink, a.u, a.v, b.u, b.v, CAD_DRAWING_MODEL_LINE))
                return CAD_DRAWING_SINK_FAILED;
        }
    }
    if (!dimension(sink, &l.views[0], 0, l.model_bounds.width) ||
        !dimension(sink, &l.views[0], 1, l.model_bounds.height) ||
        !dimension(sink, &l.views[2], 0, l.model_bounds.depth) ||
        !sink->text(sink->context, m + 4, l.title_top + 4,
                    "ORTHOGRAPHIC DRAWING", CAD_DRAWING_TITLE) ||
        !sink->text(sink->context, m + 4, l.title_top + 14,
                    "THIRD ANGLE / WIREFRAME", CAD_DRAWING_TITLE) ||
        !sink->text(sink->context, m + 4, l.title_top + 24,
                    "MODEL UNITS UNSPECIFIED", CAD_DRAWING_TITLE))
        return CAD_DRAWING_SINK_FAILED;
    if (out != NULL) *out = l;
    return CAD_DRAWING_OK;
}
