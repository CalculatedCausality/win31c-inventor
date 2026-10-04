#include "../cad_drawing.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

/* Local initial-house fixture; deliberately independent of the editor,
 * Windows headers, preview transforms and build harness. X/Z/Y=200/140/190. */
static const CadDrawingPoint house[] = {
    {-100,-70,70}, {100,-70,70}, {100,-70,-70}, {-100,-70,-70},
    {-100,30,70}, {100,30,70}, {100,30,-70}, {-100,30,-70},
    {-100,100,0}, {100,100,0},
    {-15,-70,70}, {-15,0,70}, {15,0,70}, {15,-70,70},
    {-75,-30,70}, {-35,-30,70}, {-35,10,70}, {-75,10,70},
    {35,-30,70}, {75,-30,70}, {75,10,70}, {35,10,70},
    {40,55,-45}, {65,55,-45}, {65,75,-25}, {40,75,-25},
    {40,120,-45}, {65,120,-45}, {65,120,-25}, {40,120,-25}
};
static const CadDrawingEdge edges[] = {
    {0,1}, {1,2}, {2,3}, {3,0}, {4,5}, {5,6}, {6,7}, {7,4},
    {0,4}, {1,5}, {2,6}, {3,7}, {8,9}, {4,8}, {7,8}, {5,9}, {6,9},
    {10,11}, {11,12}, {12,13}, {14,15}, {15,16}, {16,17}, {17,14},
    {18,19}, {19,20}, {20,21}, {21,18}, {22,23}, {23,24}, {24,25},
    {25,22}, {26,27}, {27,28}, {28,29}, {29,26}, {22,26}, {23,27},
    {24,28}, {25,29}
};
#define HOUSE_COUNT ((int)(sizeof(house) / sizeof(house[0])))
#define EDGE_COUNT ((int)(sizeof(edges) / sizeof(edges[0])))
#define RECORD_COUNT 128

typedef struct { long x1, y1, x2, y2; } Record;
typedef struct {
    Record model[RECORD_COUNT];
    int calls, model_count, border_count, dimension_count, text_count;
    int fail_at, label_count, units_seen, third_angle_seen;
    long width, height;
    char labels[3][12];
} Capture;

static void inside(const Capture *c, long x, long y)
{
    assert(x >= 0 && x <= c->width);
    assert(y >= 0 && y <= c->height);
}

static int capture_line(void *context, long x1, long y1, long x2, long y2,
    unsigned int flags)
{
    Capture *c;
    Record *r;
    c = (Capture *)context;
    ++c->calls;
    if (c->calls == c->fail_at) return 0;
    inside(c, x1, y1); inside(c, x2, y2);
    assert((flags & CAD_DRAWING_SOLID) != 0);
    if ((flags & CAD_DRAWING_BORDER) != 0) ++c->border_count;
    if ((flags & CAD_DRAWING_DIMENSION) != 0) ++c->dimension_count;
    if ((flags & CAD_DRAWING_MODEL_LINE) != 0) {
        assert(c->model_count < RECORD_COUNT);
        r = &c->model[c->model_count++];
        r->x1 = x1; r->y1 = y1; r->x2 = x2; r->y2 = y2;
    }
    return 1;
}

static int capture_text(void *context, long x, long y, const char *text,
    unsigned int flags)
{
    Capture *c;
    c = (Capture *)context;
    ++c->calls;
    if (c->calls == c->fail_at) return 0;
    inside(c, x, y);
    ++c->text_count;
    if ((flags & CAD_DRAWING_DIMENSION) != 0) {
        assert((flags & CAD_DRAWING_LABEL) != 0);
        assert(c->label_count < 3);
        assert(strlen(text) < sizeof(c->labels[0]));
        strcpy(c->labels[c->label_count++], text);
    }
    if (strcmp(text, "MODEL UNITS UNSPECIFIED") == 0) c->units_seen = 1;
    if (strcmp(text, "THIRD ANGLE / WIREFRAME") == 0) c->third_angle_seen = 1;
    return 1;
}

static CadDrawingModel house_model(void)
{
    CadDrawingModel m;
    m.vertices = house; m.vertex_count = HOUSE_COUNT;
    m.edges = edges; m.edge_count = EDGE_COUNT;
    return m;
}

static CadDrawingSheet standard_sheet(void)
{
    CadDrawingSheet s;
    s.width = 800; s.height = 600; s.margin = 16; s.gap = 40;
    s.scale.numerator = 1; s.scale.denominator = 1;
    return s;
}

static CadDrawingSink init_capture(Capture *c, const CadDrawingSheet *s)
{
    CadDrawingSink sink;
    memset(c, 0, sizeof(*c));
    c->width = s->width; c->height = s->height;
    sink.context = c; sink.line = capture_line; sink.text = capture_text;
    return sink;
}

static void basis_test(void)
{
    static const long expected[6][2] = {
        {2,3}, {2,-5}, {-5,3}, {5,3}, {-2,3}, {2,5}
    };
    CadDrawingPoint p;
    CadDrawingUV uv;
    int i;
    p.x = 2; p.y = 3; p.z = 5;
    for (i = 0; i < 6; ++i) {
        assert(CadDrawingProject((CadDrawingView)i, &p, &uv) == CAD_DRAWING_OK);
        assert(uv.u == expected[i][0] && uv.v == expected[i][1]);
    }
    uv.u = 111; uv.v = 222;
    assert(CadDrawingProject((CadDrawingView)6, &p, &uv) == CAD_DRAWING_INVALID);
    assert(uv.u == 111 && uv.v == 222);
    assert(CadDrawingProject(CAD_DRAWING_FRONT, NULL, &uv) == CAD_DRAWING_INVALID);
    assert(CadDrawingProject(CAD_DRAWING_FRONT, &p, NULL) == CAD_DRAWING_INVALID);
    p.x = CAD_DRAWING_I32_MIN;
    assert(CadDrawingProject(CAD_DRAWING_FRONT, &p, &uv) == CAD_DRAWING_OK);
    assert(uv.u == CAD_DRAWING_I32_MIN);
    assert(CadDrawingProject(CAD_DRAWING_REAR, &p, &uv) == CAD_DRAWING_OVERFLOW);
    p.z = CAD_DRAWING_I32_MIN;
    assert(CadDrawingProject(CAD_DRAWING_RIGHT, &p, &uv) == CAD_DRAWING_OVERFLOW);
    assert(CadDrawingProject(CAD_DRAWING_TOP, &p, &uv) == CAD_DRAWING_OVERFLOW);
    assert(CadDrawingProject(CAD_DRAWING_BOTTOM, &p, &uv) == CAD_DRAWING_OK);
#if LONG_MAX > 2147483647L
    p.x = CAD_DRAWING_I32_MAX + 1L;
    assert(CadDrawingProject(CAD_DRAWING_FRONT, &p, &uv) == CAD_DRAWING_RANGE);
    p.x = CAD_DRAWING_I32_MIN - 1L;
    assert(CadDrawingProject(CAD_DRAWING_FRONT, &p, &uv) == CAD_DRAWING_RANGE);
#endif
}

static void alignment(const Capture *c, const CadDrawingLayout *l)
{
    const Record *f, *t, *r;
    int i;
    assert(l->views[0].left == l->views[1].left);
    assert(l->views[0].right == l->views[1].right);
    assert(l->views[0].top == l->views[2].top);
    assert(l->views[0].bottom == l->views[2].bottom);
    assert(l->views[1].bottom < l->views[0].top);
    assert(l->views[0].right < l->views[2].left);
    assert(c->model_count == EDGE_COUNT * 3);
    for (i = 0; i < EDGE_COUNT; ++i) {
        f = &c->model[i]; t = &c->model[EDGE_COUNT + i];
        r = &c->model[EDGE_COUNT * 2 + i];
        assert(f->x1 == t->x1 && f->x2 == t->x2);
        assert(f->y1 == r->y1 && f->y2 == r->y2);
    }
    /* Edge 1 runs from front (+Z) to rear (-Z). */
    assert(c->model[EDGE_COUNT + 1].y1 > c->model[EDGE_COUNT + 1].y2);
    assert(c->model[EDGE_COUNT * 2 + 1].x1 < c->model[EDGE_COUNT * 2 + 1].x2);
    /* Edge 8 rises from floor to wall top: screen Y must decrease. */
    assert(c->model[8].y1 > c->model[8].y2);
}

static void layout_scale_test(void)
{
    CadDrawingModel m;
    CadDrawingSheet s;
    CadDrawingLayout l;
    CadDrawingBounds b;
    CadDrawingSink sink;
    Capture c;
    m = house_model(); s = standard_sheet();
    assert(CadDrawingGetBounds(&m, &b) == CAD_DRAWING_OK);
    assert(b.width == 200 && b.depth == 140 && b.height == 190);
    assert(b.min.x == -100 && b.max.y == 120 && b.min.z == -70);
    sink = init_capture(&c, &s);
    assert(CadDrawingRender(&m, &s, &sink, &l) == CAD_DRAWING_OK);
    assert(l.background_white && l.foreground_black);
    assert(l.sheet_bounds.left == 0 && l.sheet_bounds.top == 0);
    assert(l.sheet_bounds.right == 800 && l.sheet_bounds.bottom == 600);
    alignment(&c, &l);
    assert(c.border_count == 5 && c.dimension_count == 15);
    assert(c.text_count == 9 && c.label_count == 3);
    assert(strcmp(c.labels[0], "200") == 0);
    assert(strcmp(c.labels[1], "190") == 0);
    assert(strcmp(c.labels[2], "140") == 0);
    assert(c.units_seen && c.third_angle_seen);
    s.scale.numerator = 2; s.scale.denominator = 3;
    sink = init_capture(&c, &s);
    assert(CadDrawingRender(&m, &s, &sink, &l) == CAD_DRAWING_OK);
    alignment(&c, &l);
    assert(l.views[0].right - l.views[0].left == 133);
    assert(l.views[0].bottom - l.views[0].top == 126);
    assert(l.views[1].bottom - l.views[1].top == 93);
    assert(strcmp(c.labels[0], "200") == 0);
    assert(strcmp(c.labels[1], "190") == 0);
    assert(strcmp(c.labels[2], "140") == 0);
    s.scale.numerator = 1; s.scale.denominator = 1000;
    sink = init_capture(&c, &s);
    assert(CadDrawingRender(&m, &s, &sink, &l) == CAD_DRAWING_OK);
    assert(l.views[0].right == l.views[0].left);
    assert(strcmp(c.labels[0], "200") == 0);
}

static void independence_test(void)
{
    CadDrawingModel m;
    CadDrawingSheet s;
    CadDrawingLayout l;
    CadDrawingSink sink;
    CadDrawingPoint translated[HOUSE_COUNT], preview[HOUSE_COUNT];
    Capture a, b;
    int i;
    m = house_model(); s = standard_sheet();
    s.scale.numerator = 2; s.scale.denominator = 3;
    sink = init_capture(&a, &s);
    assert(CadDrawingRender(&m, &s, &sink, &l) == CAD_DRAWING_OK);
    /* A separate arbitrary rotated/zoomed preview buffer has no role in API. */
    for (i = 0; i < HOUSE_COUNT; ++i) {
        preview[i].x = -house[i].z * 7;
        preview[i].y = house[i].x * 7;
        preview[i].z = house[i].y * 7 + 900;
        translated[i].x = house[i].x + 1001;
        translated[i].y = house[i].y - 301;
        translated[i].z = house[i].z + 501;
    }
    assert(preview[0].x != house[0].x);
    s.width = 1200; s.height = 900;
    sink = init_capture(&b, &s);
    assert(CadDrawingRender(&m, &s, &sink, &l) == CAD_DRAWING_OK);
    assert(memcmp(a.model, b.model, sizeof(a.model)) == 0);
    m.vertices = translated;
    sink = init_capture(&b, &s);
    assert(CadDrawingRender(&m, &s, &sink, &l) == CAD_DRAWING_OK);
    alignment(&b, &l);
    assert(memcmp(a.model, b.model, sizeof(a.model)) == 0);
}

static void rejected(CadDrawingModel *m, CadDrawingSheet *s,
    CadDrawingStatus expected)
{
    Capture c;
    CadDrawingSink sink;
    CadDrawingLayout l, saved;
    memset(&l, 0x5a, sizeof(l)); saved = l;
    sink = init_capture(&c, s);
    assert(CadDrawingRender(m, s, &sink, &l) == expected);
    assert(c.calls == 0);
    assert(memcmp(&l, &saved, sizeof(l)) == 0);
}

static void invalid_test(void)
{
    CadDrawingModel m;
    CadDrawingSheet s;
    CadDrawingEdge edge;
    CadDrawingPoint p[2];
    CadDrawingBounds bounds;
    CadDrawingLayout layout;
    m = house_model(); s = standard_sheet();
    m.vertex_count = 0; rejected(&m, &s, CAD_DRAWING_INVALID);
    m.vertex_count = -1; rejected(&m, &s, CAD_DRAWING_INVALID);
    m.vertex_count = CAD_DRAWING_MAX_VERTICES + 1;
    rejected(&m, &s, CAD_DRAWING_INVALID);
    m = house_model(); m.edge_count = -1; rejected(&m, &s, CAD_DRAWING_INVALID);
    m.edge_count = CAD_DRAWING_MAX_EDGES + 1;
    rejected(&m, &s, CAD_DRAWING_INVALID);
    m = house_model(); m.vertices = NULL; rejected(&m, &s, CAD_DRAWING_INVALID);
    m = house_model(); m.edges = NULL; rejected(&m, &s, CAD_DRAWING_INVALID);
    m = house_model(); m.edges = &edge; m.edge_count = 1;
    edge.a = -1; edge.b = 0; rejected(&m, &s, CAD_DRAWING_INVALID);
    edge.a = 0; edge.b = HOUSE_COUNT; rejected(&m, &s, CAD_DRAWING_INVALID);
    m = house_model(); s.scale.numerator = 0; rejected(&m, &s, CAD_DRAWING_RANGE);
    s = standard_sheet(); s.scale.denominator = 0;
    rejected(&m, &s, CAD_DRAWING_RANGE);
    s.scale.denominator = -1; rejected(&m, &s, CAD_DRAWING_RANGE);
    s = standard_sheet(); s.width = 32768L; rejected(&m, &s, CAD_DRAWING_RANGE);
    s = standard_sheet(); s.height = -1; rejected(&m, &s, CAD_DRAWING_RANGE);
    s = standard_sheet(); s.margin = 7; rejected(&m, &s, CAD_DRAWING_RANGE);
    s = standard_sheet(); s.gap = 31; rejected(&m, &s, CAD_DRAWING_RANGE);
    s = standard_sheet(); s.width = 200; rejected(&m, &s, CAD_DRAWING_NO_ROOM);
    s = standard_sheet(); s.height = 200; rejected(&m, &s, CAD_DRAWING_NO_ROOM);
    s = standard_sheet(); s.scale.numerator = CAD_DRAWING_I32_MAX;
    s.scale.denominator = CAD_DRAWING_I32_MAX;
    rejected(&m, &s, CAD_DRAWING_OVERFLOW);
    s = standard_sheet(); s.margin = CAD_DRAWING_I32_MAX;
    rejected(&m, &s, CAD_DRAWING_OVERFLOW);
    s = standard_sheet(); s.gap = CAD_DRAWING_I32_MAX;
    rejected(&m, &s, CAD_DRAWING_OVERFLOW);
    s = standard_sheet();
    p[0].x = CAD_DRAWING_I32_MIN; p[0].y = p[0].z = 0;
    p[1] = p[0]; p[1].x = CAD_DRAWING_I32_MAX;
    m.vertices = p; m.vertex_count = 2; m.edge_count = 0; m.edges = NULL;
    rejected(&m, &s, CAD_DRAWING_OVERFLOW);
    p[0].x = p[1].x = 0; p[0].y = CAD_DRAWING_I32_MIN;
    p[1].y = CAD_DRAWING_I32_MAX; rejected(&m, &s, CAD_DRAWING_OVERFLOW);
    p[0].y = p[1].y = 0; p[0].z = p[1].z = CAD_DRAWING_I32_MIN;
    rejected(&m, &s, CAD_DRAWING_OVERFLOW);
#if LONG_MAX > 2147483647L
    p[0].z = CAD_DRAWING_I32_MAX + 1L; rejected(&m, &s, CAD_DRAWING_RANGE);
    m = house_model(); s.scale.denominator = CAD_DRAWING_I32_MAX + 1L;
    rejected(&m, &s, CAD_DRAWING_RANGE);
    s = standard_sheet(); s.width = CAD_DRAWING_I32_MAX + 1L;
    rejected(&m, &s, CAD_DRAWING_RANGE);
#endif
    assert(CadDrawingGetBounds(NULL, &bounds) == CAD_DRAWING_INVALID);
    m = house_model(); s = standard_sheet();
    assert(CadDrawingGetBounds(&m, NULL) == CAD_DRAWING_INVALID);
    assert(CadDrawingMakeLayout(&m, NULL, &layout) == CAD_DRAWING_INVALID);
    assert(CadDrawingMakeLayout(&m, &s, NULL) == CAD_DRAWING_INVALID);
}

static void sink_and_degenerate_test(void)
{
    CadDrawingModel m;
    CadDrawingSheet s;
    CadDrawingLayout l, saved;
    CadDrawingSink sink;
    CadDrawingPoint p;
    Capture c;
    int fail, total;
    m = house_model(); s = standard_sheet();
    sink = init_capture(&c, &s);
    assert(CadDrawingRender(&m, &s, &sink, &l) == CAD_DRAWING_OK);
    total = c.calls;
    for (fail = 1; fail <= total; ++fail) {
        sink = init_capture(&c, &s); c.fail_at = fail;
        memset(&l, 0x5a, sizeof(l)); saved = l;
        assert(CadDrawingRender(&m, &s, &sink, &l) == CAD_DRAWING_SINK_FAILED);
        assert(c.calls == fail && memcmp(&l, &saved, sizeof(l)) == 0);
    }
    sink = init_capture(&c, &s);
    assert(CadDrawingRender(&m, &s, NULL, NULL) == CAD_DRAWING_INVALID);
    sink.line = NULL;
    assert(CadDrawingRender(&m, &s, &sink, NULL) == CAD_DRAWING_INVALID);
    sink = init_capture(&c, &s); sink.text = NULL;
    assert(CadDrawingRender(&m, &s, &sink, NULL) == CAD_DRAWING_INVALID);
    p.x = CAD_DRAWING_I32_MAX; p.y = CAD_DRAWING_I32_MIN; p.z = 0;
    m.vertices = &p; m.vertex_count = 1; m.edge_count = 0; m.edges = NULL;
    sink = init_capture(&c, &s);
    assert(CadDrawingRender(&m, &s, &sink, NULL) == CAD_DRAWING_OK);
    assert(c.model_count == 0 && c.label_count == 3);
    assert(strcmp(c.labels[0], "0") == 0 && strcmp(c.labels[1], "0") == 0);
}

static void signed_limit_test(void)
{
    CadDrawingPoint p[2];
    CadDrawingEdge edge;
    CadDrawingModel m;
    CadDrawingSheet s;
    CadDrawingLayout l;
    CadDrawingSink sink;
    Capture c;
    /* Largest legal extents, including subtraction from LONG_MIN. Scaling
     * by 1/MAX is legal: the product fits, and the final sheet spans one unit.
     */
    p[0].x = -1; p[1].x = CAD_DRAWING_I32_MAX - 1;
    p[0].y = CAD_DRAWING_I32_MIN; p[1].y = -1;
    p[0].z = -CAD_DRAWING_I32_MAX; p[1].z = 0;
    edge.a = 0; edge.b = 1;
    m.vertices = p; m.vertex_count = 2; m.edges = &edge; m.edge_count = 1;
    s = standard_sheet(); s.scale.denominator = CAD_DRAWING_I32_MAX;
    sink = init_capture(&c, &s);
    assert(CadDrawingRender(&m, &s, &sink, &l) == CAD_DRAWING_OK);
    assert(l.model_bounds.width == CAD_DRAWING_I32_MAX);
    assert(l.model_bounds.height == CAD_DRAWING_I32_MAX);
    assert(l.model_bounds.depth == CAD_DRAWING_I32_MAX);
    assert(l.views[0].right - l.views[0].left == 1);
    assert(l.views[0].bottom - l.views[0].top == 1);
    assert(strcmp(c.labels[0], "2147483647") == 0);
    assert(strcmp(c.labels[1], "2147483647") == 0);
    assert(strcmp(c.labels[2], "2147483647") == 0);
}

int main(void)
{
    basis_test();
    layout_scale_test();
    independence_test();
    invalid_test();
    sink_and_degenerate_test();
    signed_limit_test();
    puts("drawing_test: all tests passed");
    return 0;
}
