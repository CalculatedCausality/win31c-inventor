#include "../cad_features.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static CadUserParams params;
static CadFeatures doc, other;
static CadFeatOutput out, out2;
static CadFeatError err;
static char text[CAD_FEAT_TEXT_MAX + 64], before[CAD_FEAT_TEXT_MAX + 64];

static void reset(void)
{
    cad_features_init(&doc);
    cad_userparams_init(&params, CAD_UNITS_MM);
}
static void param(const char *name, const char *expr)
{
    CadUserParamError pe;
    unsigned id = cad_userparams_find(&params, name);
    CadUserParamStatus s = id ? cad_userparams_set_expression(&params, id, expr, &pe)
                              : cad_userparams_add(&params, name, expr, 0, &pe);
    assert(s == CAD_UP_OK);
}
static unsigned sketch(const char *name, CadFeatPlane plane, const char *off)
{
    unsigned id = 0;
    CadFeatStatus s = cad_features_add_sketch(&doc, name, plane, off, &id, &err);
    if (s != CAD_FEAT_OK) fprintf(stderr, "add_sketch: %s\n", err.message);
    assert(s == CAD_FEAT_OK && id != 0);
    return id;
}
static void rect(unsigned sk, const char *cx, const char *cy, const char *w,
                 const char *h)
{
    const char *e[4];
    e[0] = cx; e[1] = cy; e[2] = w; e[3] = h;
    assert(cad_features_add_profile(&doc, sk, CAD_FEAT_RECT, e, 4, 0, 0, &err)
           == CAD_FEAT_OK);
}
static void circle(unsigned sk, const char *cx, const char *cy, const char *r,
                   unsigned sides)
{
    const char *e[3];
    e[0] = cx; e[1] = cy; e[2] = r;
    assert(cad_features_add_profile(&doc, sk, CAD_FEAT_CIRCLE, e, 3, sides, 0,
                                    &err) == CAD_FEAT_OK);
}
/* coords: "x1 y1 x2 y2 ..." single tokens separated by spaces */
static void poly(unsigned sk, const char *coords)
{
    static char buf[256];
    const char *e[CAD_FEAT_PROFILE_EXPRS];
    unsigned n = 0;
    char *p;
    strcpy(buf, coords);
    for (p = strtok(buf, " "); p; p = strtok(0, " ")) e[n++] = p;
    assert(cad_features_add_profile(&doc, sk, CAD_FEAT_POLY, e, n, 0, 0, &err)
           == CAD_FEAT_OK);
}
static unsigned extrude(unsigned sk, int profile, CadFeatDirection dir,
                        const char *dist)
{
    unsigned id = 0;
    assert(cad_features_add_extrude(&doc, 0, sk, profile, dir, dist, &id, &err)
           == CAD_FEAT_OK);
    return id;
}
static unsigned regen(void)
{
    unsigned errors = 99;
    assert(cad_features_regenerate(&doc, &params, &out, &errors) == CAD_FEAT_OK);
    return errors;
}
static const CadFeatHealth *health(unsigned id)
{
    int i = cad_features_index(&doc, id);
    assert(i >= 0 && (unsigned)i < out.feature_count);
    assert(out.health[i].feature_id == id);
    return &out.health[i];
}
/* Six times signed volume of a feature's shading faces. */
static long volume6(unsigned id)
{
    const CadFeatHealth *h = health(id);
    long sum = 0;
    unsigned i;
    for (i = h->first_face; i < h->first_face + h->face_count; ++i) {
        const CadMeshViewVertex *a = &out.vertices[out.faces[i].a];
        const CadMeshViewVertex *b = &out.vertices[out.faces[i].b];
        const CadMeshViewVertex *c = &out.vertices[out.faces[i].c];
        sum += (long)a->x * ((long)b->y * c->z - (long)b->z * c->y)
             - (long)a->y * ((long)b->x * c->z - (long)b->z * c->x)
             + (long)a->z * ((long)b->x * c->y - (long)b->y * c->x);
    }
    return sum;
}
/* Every face valid for cad_meshview: indices in range, distinct, nonzero area. */
static void check_faces(void)
{
    unsigned i;
    for (i = 0; i < out.face_count; ++i) {
        const CadMeshViewFace *t = &out.faces[i];
        const CadMeshViewVertex *a, *b, *c;
        long ux, uy, uz, vx, vy, vz;
        assert(t->a < out.vertex_count && t->b < out.vertex_count &&
               t->c < out.vertex_count && t->material < 6);
        assert(t->a != t->b && t->b != t->c && t->a != t->c);
        a = &out.vertices[t->a]; b = &out.vertices[t->b]; c = &out.vertices[t->c];
        ux = b->x - a->x; uy = b->y - a->y; uz = b->z - a->z;
        vx = c->x - a->x; vy = c->y - a->y; vz = c->z - a->z;
        assert(uy * vz - uz * vy || uz * vx - ux * vz || ux * vy - uy * vx);
    }
    for (i = 0; i < out.vertex_count; ++i)
        assert(out.vertices[i].x >= -400 && out.vertices[i].x <= 400 &&
               out.vertices[i].y >= -400 && out.vertices[i].y <= 400 &&
               out.vertices[i].z >= -400 && out.vertices[i].z <= 400);
}
/* Twice the signed area (in the given world axes) of a feature's sketch segments. */
static long area2(unsigned id, int ax, int ay)
{
    const CadFeatHealth *h = health(id);
    long sum = 0;
    unsigned i;
    for (i = h->first_segment; i < h->first_segment + h->segment_count; ++i)
        sum += out.segments[i].a[ax] * out.segments[i].b[ay] -
               out.segments[i].b[ax] * out.segments[i].a[ay];
    return sum;
}
static unsigned count_kind(unsigned id, int kind)
{
    unsigned i, n = 0;
    for (i = 0; i < out.segment_count; ++i)
        if (out.segments[i].feature_id == id && out.segments[i].kind == kind) ++n;
    return n;
}
static void seg_is(unsigned index, long ax, long ay, long az, long bx, long by,
                   long bz)
{
    const CadFeatSegment *s = &out.segments[index];
    if (s->a[0] != ax || s->a[1] != ay || s->a[2] != az || s->b[0] != bx ||
        s->b[1] != by || s->b[2] != bz)
        fprintf(stderr, "segment %u: (%ld %ld %ld)-(%ld %ld %ld)\n", index,
                s->a[0], s->a[1], s->a[2], s->b[0], s->b[1], s->b[2]);
    assert(s->a[0] == ax && s->a[1] == ay && s->a[2] == az);
    assert(s->b[0] == bx && s->b[1] == by && s->b[2] == bz);
}
static unsigned snapshot(const CadFeatures *f, char *buf)
{
    unsigned n = 0;
    assert(cad_features_write_text(f, buf, sizeof(text), &n) == CAD_FEAT_OK);
    return n;
}
static void save(void) { snapshot(&doc, before); }
static void unchanged(void)
{
    snapshot(&doc, text);
    assert(strcmp(text, before) == 0);
}

static void test_planes(void)
{
    static const CadFeatPlane planes[3] = { CAD_FEAT_XY, CAD_FEAT_XZ, CAD_FEAT_YZ };
    /* expected bounds of a 10x20 triangle extruded 6 from offset 5 */
    static const long lo[3][3] = { {0, 0, 5}, {0, 5, -20}, {5, 0, 0} };
    static const long hi[3][3] = { {10, 20, 11}, {10, 11, 0}, {11, 10, 20} };
    unsigned p, sk, ex;
    long mn[3], mx[3];
    int k;
    for (p = 0; p < 3; ++p) {
        reset();
        sk = sketch(0, planes[p], "5");
        poly(sk, "0 0 10 0 0 20");
        ex = extrude(sk, CAD_FEAT_ALL, CAD_FEAT_POSITIVE, "6");
        assert(regen() == 0);
        assert(out.segment_count == 3 + 9 && out.vertex_count == 6 &&
               out.face_count == 8);
        if (p == 0) {
            seg_is(0, 0, 0, 5, 10, 0, 5);
            seg_is(1, 10, 0, 5, 0, 20, 5);
        } else if (p == 1) {   /* XZ: (u, n, -v) */
            seg_is(0, 0, 5, 0, 10, 5, 0);
            seg_is(1, 10, 5, 0, 0, 5, -20);
        } else {               /* YZ: (n, u, v) */
            seg_is(0, 5, 0, 0, 5, 10, 0);
            seg_is(1, 5, 10, 0, 5, 0, 20);
        }
        /* right-handed mapping keeps outward winding: positive volume */
        assert(volume6(ex) == 6L * 100L * 6L);
        assert(cad_features_bounds(&out, mn, mx));
        for (k = 0; k < 3; ++k) assert(mn[k] == lo[p][k] && mx[k] == hi[p][k]);
        check_faces();
    }
    printf("planes: PASS\n");
}

static void test_extrude_shapes(void)
{
    unsigned s1, s2, s3, e1, e2, e3, e4, e5;
    long a, mn[3], mx[3];
    CadFeatInfo info;
    reset();
    s1 = sketch(0, CAD_FEAT_XY, "0");
    rect(s1, "0", "0", "40", "20");
    e1 = extrude(s1, CAD_FEAT_ALL, CAD_FEAT_POSITIVE, "30 mm");
    s2 = sketch(0, CAD_FEAT_XY, "0");
    circle(s2, "100", "100", "100", 0);
    e2 = extrude(s2, 0, CAD_FEAT_NEGATIVE, "10");
    s3 = sketch(0, CAD_FEAT_XY, "-50");
    poly(s3, "-100 -100 -50 -100 -50 -80 -80 -80 -80 -50 -100 -50");  /* L, area 1600 */
    e3 = extrude(s3, CAD_FEAT_ALL, CAD_FEAT_SYMMETRIC, "31");
    assert(regen() == 0);
    check_faces();
    /* rect: 4 sketch curves, 12 body edges, 8 vertices, 12 triangles */
    assert(count_kind(s1, CAD_FEAT_SEG_SKETCH) == 4 && count_kind(e1, CAD_FEAT_SEG_EDGE) == 12);
    assert(health(e1)->face_count == 12 && volume6(e1) == 6L * 40L * 20L * 30L);
    /* circle 16 sides: 16 curves, 48 edges, 60 triangles */
    assert(count_kind(s2, CAD_FEAT_SEG_SKETCH) == 16 && count_kind(e2, CAD_FEAT_SEG_EDGE) == 48);
    assert(health(e2)->face_count == 4 * 16 - 4);
    a = area2(s2, 0, 1);
    assert(a > 2L * 30500L && a < 2L * 30700L);   /* 8*r^2*sin(22.5 deg) = 30615 */
    assert(volume6(e2) == 3L * a * 10L);
    /* poly L: 6 curves, 18 edges, 20 triangles; symmetric about z=-50 */
    assert(count_kind(s3, CAD_FEAT_SEG_SKETCH) == 6 && count_kind(e3, CAD_FEAT_SEG_EDGE) == 18);
    assert(area2(s3, 0, 1) == 3200L && volume6(e3) == 3L * 3200L * 31L);
    assert(cad_features_bounds(&out, mn, mx));
    assert(mn[0] == -100 && mn[1] == -100 && mn[2] == -65 && mx[0] == 200 &&
           mx[1] == 200 && mx[2] == 30);       /* sym: -50-15 .. -50+16 */
    /* materials follow feature slot % 6 */
    assert(out.faces[health(e1)->first_face].material == 1);
    assert(out.faces[health(e3)->first_face].material == 5);
    /* browser descriptions */
    assert(cad_features_get(&doc, e1, &info) == CAD_FEAT_OK);
    assert(strcmp(info.description, "Extrusion1 (Sketch1, 30 mm)") == 0);
    assert(cad_features_get(&doc, e2, &info) == CAD_FEAT_OK);
    assert(strcmp(info.description, "Extrusion2 (Sketch2 profile 1, 10, reversed)") == 0);
    assert(cad_features_get(&doc, s3, &info) == CAD_FEAT_OK);
    assert(strcmp(info.description, "Sketch3 (XY, offset -50, 1 profile)") == 0);
    /* multi-profile sketch: ALL makes one body per profile */
    rect(s1, "100", "-100", "20", "20");
    assert(regen() == 0);
    assert(count_kind(e1, CAD_FEAT_SEG_EDGE) == 24 && health(e1)->face_count == 24);
    assert(volume6(e1) == 6L * (40L * 20L * 30L + 20L * 20L * 30L));
    e4 = extrude(s1, 1, CAD_FEAT_POSITIVE, "5");
    assert(regen() == 0 && volume6(e4) == 6L * 20L * 20L * 5L);
    /* body leaving -400..400 is an error on the distance */
    e5 = extrude(s3, CAD_FEAT_ALL, CAD_FEAT_NEGATIVE, "351");
    assert(regen() == 1);
    assert(health(e5)->state == CAD_FEAT_STATE_ERROR &&
           health(e5)->error == CAD_FEAT_RANGE &&
           health(e5)->field == CAD_FEAT_FIELD_DISTANCE &&
           health(e5)->segment_count == 0 && health(e5)->face_count == 0);
    assert(cad_features_edit_extrude(&doc, e5, s3, CAD_FEAT_ALL,
                                     CAD_FEAT_NEGATIVE, "350", &err) == CAD_FEAT_OK);
    assert(regen() == 0);
    printf("extrude shapes: PASS\n");
}

static void test_associativity(void)
{
    unsigned s1, e1;
    long mn[3], mx[3], v1;
    reset();
    param("Width", "40 mm");
    param("Depth", "Width / 4");
    s1 = sketch(0, CAD_FEAT_XZ, "Depth - 5 mm");
    rect(s1, "0", "0", "Width", "Width / 2");
    e1 = extrude(s1, CAD_FEAT_ALL, CAD_FEAT_POSITIVE, "Depth * 2");
    assert(regen() == 0);
    v1 = volume6(e1);
    assert(v1 == 6L * 40L * 20L * 20L);
    assert(cad_features_bounds(&out, mn, mx));
    assert(mn[0] == -20 && mx[0] == 20 && mn[1] == 5 && mx[1] == 25);
    param("Width", "60 mm");                  /* same features, new geometry */
    assert(regen() == 0);
    assert(volume6(e1) == 6L * 60L * 30L * 30L && volume6(e1) != v1);
    assert(cad_features_bounds(&out, mn, mx));
    assert(mn[0] == -30 && mx[0] == 30 && mn[1] == 10 && mx[1] == 40);
    printf("associativity: PASS\n");
}

static void test_fail_soft(void)
{
    unsigned s1, s2, e1, e2, e3, errors, i;
    const CadFeatHealth *h;
    reset();
    param("Len", "50 mm");
    s1 = sketch(0, CAD_FEAT_XY, "0");
    rect(s1, "0", "0", "Len", "Len");
    e1 = extrude(s1, CAD_FEAT_ALL, CAD_FEAT_POSITIVE, "Nope + 1 mm");
    s2 = sketch(0, CAD_FEAT_YZ, "0");
    rect(s2, "0", "0", "10", "10");
    rect(s2, "100", "0", "Len *", "10");      /* bad expression in profile 1 */
    e2 = extrude(s2, 0, CAD_FEAT_POSITIVE, "10");
    e3 = extrude(s1, CAD_FEAT_ALL, CAD_FEAT_POSITIVE, "Len");
    errors = regen();
    assert(errors == 3);
    assert(health(s1)->state == CAD_FEAT_STATE_OK);
    h = health(e1);
    assert(h->state == CAD_FEAT_STATE_ERROR && h->error == CAD_FEAT_EVAL &&
           h->field == CAD_FEAT_FIELD_DISTANCE && h->param_status != CAD_UP_OK &&
           h->segment_count == 0 && h->message[0] != 0);
    h = health(s2);
    assert(h->state == CAD_FEAT_STATE_ERROR && h->error == CAD_FEAT_EVAL &&
           h->field == CAD_FEAT_FIELD_PROFILE && h->profile == 1 &&
           h->expr_index == 2 && h->segment_count == 0);
    h = health(e2);
    assert(h->state == CAD_FEAT_STATE_BLOCKED && h->blocked_by == s2);
    h = health(e3);                            /* independent: still builds */
    assert(h->state == CAD_FEAT_STATE_OK && volume6(e3) == 6L * 50L * 50L * 50L);
    for (i = 0; i < out.segment_count; ++i)
        assert(out.segments[i].feature_id == s1 || out.segments[i].feature_id == e3);
    /* invalid polygon geometry is reported on the profile */
    {
        const char *e[6];
        e[0] = "0"; e[1] = "0"; e[2] = "10"; e[3] = "10"; e[4] = "10"; e[5] = "0";
        assert(cad_features_edit_profile(&doc, s2, 1, CAD_FEAT_POLY, e, 6, 0,
                                         &err) == CAD_FEAT_OK);
    }
    {
        const char *e[8];   /* bow tie */
        e[0] = "20"; e[1] = "0"; e[2] = "30"; e[3] = "10"; e[4] = "30"; e[5] = "0";
        e[6] = "20"; e[7] = "10";
        assert(cad_features_edit_profile(&doc, s2, 0, CAD_FEAT_POLY, e, 8, 0,
                                         &err) == CAD_FEAT_OK);
    }
    regen();
    h = health(s2);
    assert(h->state == CAD_FEAT_STATE_ERROR && h->error == CAD_FEAT_GEOMETRY &&
           h->profile == 0 && strcmp(h->message, "profile intersects itself") == 0);
    /* fix everything */
    assert(cad_features_edit_extrude(&doc, e1, s1, CAD_FEAT_ALL, CAD_FEAT_POSITIVE,
                                     "Len + 1 mm", &err) == CAD_FEAT_OK);
    assert(cad_features_remove_profile(&doc, s2, 0, &err) == CAD_FEAT_IN_USE &&
           err.feature_id == e2);
    assert(cad_features_edit_extrude(&doc, e2, s2, 1, CAD_FEAT_POSITIVE, "10",
                                     &err) == CAD_FEAT_OK);
    assert(cad_features_remove_profile(&doc, s2, 0, &err) == CAD_FEAT_OK);
    {
        CadFeatInfo info;
        assert(cad_features_get(&doc, e2, &info) == CAD_FEAT_OK && info.profile == 0);
    }
    assert(regen() == 0);
    assert(volume6(e1) == 6L * 50L * 50L * 51L);
    printf("fail-soft regeneration: PASS\n");
}

static void test_suppress_remove_move(void)
{
    unsigned s1, s2, e1, e2;
    reset();
    s1 = sketch("Base", CAD_FEAT_XY, "0");
    rect(s1, "0", "0", "10", "10");
    e1 = extrude(s1, CAD_FEAT_ALL, CAD_FEAT_POSITIVE, "5");
    s2 = sketch(0, CAD_FEAT_XY, "0");
    rect(s2, "50", "0", "10", "10");
    e2 = extrude(s2, CAD_FEAT_ALL, CAD_FEAT_POSITIVE, "5");
    assert(cad_features_suppress(&doc, e1, 1, &err) == CAD_FEAT_OK);
    assert(regen() == 0);
    assert(health(e1)->state == CAD_FEAT_STATE_SUPPRESSED &&
           health(e1)->segment_count == 0 && health(e1)->face_count == 0);
    assert(health(e2)->state == CAD_FEAT_STATE_OK);
    assert(cad_features_suppress(&doc, s2, 1, &err) == CAD_FEAT_OK);
    assert(regen() == 1);
    assert(health(e2)->state == CAD_FEAT_STATE_BLOCKED && health(e2)->blocked_by == s2 &&
           strcmp(health(e2)->message, "sketch is suppressed") == 0);
    assert(cad_features_suppress(&doc, s2, 0, &err) == CAD_FEAT_OK);
    assert(cad_features_suppress(&doc, e1, 0, &err) == CAD_FEAT_OK);
    assert(regen() == 0 && out.face_count == 24);
    {
        CadFeatInfo info;
        assert(cad_features_suppress(&doc, e2, 1, &err) == CAD_FEAT_OK);
        assert(cad_features_get(&doc, e2, &info) == CAD_FEAT_OK &&
               strcmp(info.description, "Extrusion2 (Sketch1, 5) [suppressed]") == 0);
        assert(cad_features_suppress(&doc, e2, 0, &err) == CAD_FEAT_OK);
    }
    /* remove: referenced sketch rejected, reporting the referencing feature */
    save();
    assert(cad_features_remove(&doc, s1, &err) == CAD_FEAT_IN_USE &&
           err.feature_id == e1);
    unchanged();
    /* move: a feature cannot pass its sketch, independent ones may */
    assert(cad_features_move_up(&doc, e1, &err) == CAD_FEAT_ORDER && err.feature_id == s1);
    assert(cad_features_move_down(&doc, s1, &err) == CAD_FEAT_ORDER && err.feature_id == e1);
    assert(cad_features_move_up(&doc, s1, &err) == CAD_FEAT_ARGUMENT);
    assert(cad_features_move_down(&doc, e2, &err) == CAD_FEAT_ARGUMENT);
    unchanged();
    assert(cad_features_move_up(&doc, s2, &err) == CAD_FEAT_OK);     /* s1 s2 e1 e2 */
    assert(cad_features_move_up(&doc, s2, &err) == CAD_FEAT_OK);     /* s2 s1 e1 e2 */
    assert(cad_features_index(&doc, s2) == 0 && cad_features_index(&doc, e1) == 2);
    assert(cad_features_move_down(&doc, e1, &err) == CAD_FEAT_OK);   /* s2 s1 e2 e1 */
    assert(regen() == 0);
    /* editing a reference to a later sketch is rejected */
    assert(cad_features_move_down(&doc, s1, &err) == CAD_FEAT_OK);   /* s2 e2 s1 e1 */
    save();
    assert(cad_features_edit_extrude(&doc, e2, s1, CAD_FEAT_ALL, CAD_FEAT_POSITIVE,
                                     "5", &err) == CAD_FEAT_REFERENCE &&
           err.field == CAD_FEAT_FIELD_SKETCH);
    unchanged();
    /* removing unreferenced features (and the sketch with its profiles) */
    assert(cad_features_remove(&doc, e1, &err) == CAD_FEAT_OK);
    assert(cad_features_remove(&doc, s1, &err) == CAD_FEAT_OK);
    assert(doc.profile_count == 1 && cad_features_count(&doc) == 2);
    assert(cad_features_remove(&doc, s1, &err) == CAD_FEAT_NOT_FOUND);
    assert(cad_features_find(&doc, "base") == 0 && cad_features_find(&doc, "SKETCH1") == s2);
    /* IDs are never reused */
    assert(sketch(0, CAD_FEAT_XY, "0") == e2 + 1);
    printf("suppress/remove/move: PASS\n");
}

static unsigned revolve(unsigned sk, CadFeatAxis axis, unsigned segs)
{
    unsigned id = 0;
    assert(cad_features_add_revolve(&doc, 0, sk, CAD_FEAT_ALL, axis, segs, &id,
                                    &err) == CAD_FEAT_OK);
    return id;
}

static void test_revolve(void)
{
    unsigned s1, s2, s3, s4, s5, r1, r2, r3, r4, r5, s6, r6;
    CadFeatInfo info;
    reset();
    s1 = sketch(0, CAD_FEAT_XY, "0");
    poly(s1, "20 10 30 10 30 20 20 20");          /* CCW, v in 10..20 */
    r1 = revolve(s1, CAD_FEAT_AXIS_U, 12);
    s2 = sketch(0, CAD_FEAT_XZ, "0");
    poly(s2, "20 -10 20 -20 30 -20 30 -10");      /* CW, v in -20..-10 */
    r2 = revolve(s2, CAD_FEAT_AXIS_U, 12);
    s3 = sketch(0, CAD_FEAT_YZ, "0");
    poly(s3, "10 20 20 20 20 30 10 30");          /* axis V, u in 10..20 */
    r3 = revolve(s3, CAD_FEAT_AXIS_V, 12);
    assert(regen() == 0);
    check_faces();
    assert(count_kind(r1, CAD_FEAT_SEG_EDGE) == 2 * 4 * 12 &&
           health(r1)->face_count == 2 * 4 * 12);
    /* faceted revolve of a 10x10 square at radius 10..20, 12 segments:
     * 10 * (area of rounded 12-gon r=20 - r=10) = 10 * 866 (exact after
     * rounding ring points to integers; unrounded would be 9000) */
    assert(volume6(r1) == 6L * 8660L);
    assert(volume6(r2) == 6L * 8660L);         /* CW profile, v < 0 */
    assert(volume6(r3) == 6L * 8660L);         /* axis V */
    assert(cad_features_get(&doc, r3, &info) == CAD_FEAT_OK &&
           strcmp(info.description, "Revolution3 (Sketch3, axis V, 12 segments)") == 0);
    /* crossing / touching the axis */
    s4 = sketch(0, CAD_FEAT_XY, "0");
    poly(s4, "0 -5 10 -5 10 5 0 5");
    r4 = revolve(s4, CAD_FEAT_AXIS_U, 12);
    s5 = sketch(0, CAD_FEAT_XY, "0");
    poly(s5, "0 1 1 1 1 2 0 2");                   /* too small for 24 segs */
    r5 = revolve(s5, CAD_FEAT_AXIS_U, 24);
    s6 = sketch(0, CAD_FEAT_XY, "100");
    poly(s6, "0 300 10 300 10 350 0 350");         /* z reaches 450 */
    r6 = revolve(s6, CAD_FEAT_AXIS_U, 12);
    assert(regen() == 3);
    assert(health(r4)->state == CAD_FEAT_STATE_ERROR &&
           health(r4)->field == CAD_FEAT_FIELD_AXIS && health(r4)->profile == 0);
    assert(health(r5)->state == CAD_FEAT_STATE_ERROR &&
           health(r5)->error == CAD_FEAT_GEOMETRY &&
           health(r5)->field == CAD_FEAT_FIELD_SEGMENTS);
    assert(health(r6)->state == CAD_FEAT_STATE_ERROR &&
           health(r6)->error == CAD_FEAT_RANGE);
    assert(health(r1)->state == CAD_FEAT_STATE_OK);
    check_faces();
    printf("revolve: PASS\n");
}

static void test_output_limits(void)
{
    unsigned s1, e1, s2, r2;
    reset();
    s1 = sketch(0, CAD_FEAT_XY, "0");
    circle(s1, "-300", "0", "50", 32);
    circle(s1, "-150", "0", "50", 32);
    circle(s1, "0", "0", "50", 32);
    circle(s1, "150", "0", "50", 32);
    circle(s1, "300", "0", "50", 32);
    e1 = extrude(s1, CAD_FEAT_ALL, CAD_FEAT_POSITIVE, "10");
    assert(regen() == 0);
    assert(out.shading_truncated && !out.wireframe_truncated);
    assert(out.vertex_count == 256 && out.face_count == 4 * 124);
    assert(count_kind(e1, CAD_FEAT_SEG_EDGE) == 5 * 96);
    assert(health(e1)->state == CAD_FEAT_STATE_OK);
    check_faces();
    reset();
    s2 = sketch(0, CAD_FEAT_XY, "0");
    circle(s2, "0", "100", "50", 32);
    r2 = revolve(s2, CAD_FEAT_AXIS_U, 24);
    assert(regen() == 0);
    assert(out.shading_truncated && out.wireframe_truncated);
    assert(out.face_count == 0 && out.segment_count == CAD_FEAT_OUT_SEGMENTS);
    assert(health(r2)->state == CAD_FEAT_STATE_OK);
    printf("output limits: PASS\n");
}

static void test_capacity(void)
{
    unsigned i, sk[4], n;
    CadFeatStatus s;
    const char *e[32];
    char name[8];
    reset();
    for (i = 0; i < CAD_FEAT_MAX; ++i) sketch(0, CAD_FEAT_XY, "0");
    save();
    assert(cad_features_add_sketch(&doc, 0, CAD_FEAT_XY, "0", 0, &err) == CAD_FEAT_FULL);
    unchanged();
    {
        CadFeatInfo info;
        assert(cad_features_at(&doc, CAD_FEAT_MAX - 1, &info) == CAD_FEAT_OK &&
               strcmp(info.name, "Sketch24") == 0);
        assert(cad_features_at(&doc, CAD_FEAT_MAX, &info) == CAD_FEAT_NOT_FOUND);
    }
    /* profiles: 8 per sketch, 32 per document */
    reset();
    for (i = 0; i < 4; ++i) sk[i] = sketch(0, CAD_FEAT_XY, "0");
    n = sketch(0, CAD_FEAT_XY, "0");                 /* fifth sketch */
    for (i = 0; i < 32; ++i) rect(sk[i / 8], "0", "0", "1", "1");
    save();
    e[0] = "0"; e[1] = "0"; e[2] = "1"; e[3] = "1";
    assert(cad_features_add_profile(&doc, sk[0], CAD_FEAT_RECT, e, 4, 0, 0, &err)
           == CAD_FEAT_FULL && err.profile == 8);
    assert(cad_features_add_profile(&doc, n, CAD_FEAT_RECT, e, 4, 0, 0, &err)
           == CAD_FEAT_FULL);
    unchanged();
    /* per-profile expression text */
    reset();
    sk[0] = sketch(0, CAD_FEAT_XY, "0");
    for (i = 0; i < 32; ++i) e[i] = "0+0+0+0+0+0+0+1";   /* 15 chars: 512 bytes */
    assert(cad_features_add_profile(&doc, sk[0], CAD_FEAT_POLY, e, 32, 0, 0, &err)
           == CAD_FEAT_OK);
    e[31] = "0+0+0+0+0+0+0+11";
    save();
    assert(cad_features_add_profile(&doc, sk[0], CAD_FEAT_POLY, e, 32, 0, 0, &err)
           == CAD_FEAT_CAPACITY && err.expr_index == 31);
    unchanged();
    e[31] = "0+0+0+0+0+0+0+1";
    /* the serialized text limit */
    sk[1] = sketch(0, CAD_FEAT_XY, "0");
    sk[2] = sketch(0, CAD_FEAT_XY, "0");
    for (n = 1, s = CAD_FEAT_OK; s == CAD_FEAT_OK; ++n) {
        save();
        s = cad_features_add_profile(&doc, sk[n / 8], CAD_FEAT_POLY, e, 32, 0, 0, &err);
    }
    assert(s == CAD_FEAT_CAPACITY && n > 10);
    unchanged();
    assert(snapshot(&doc, text) <= CAD_FEAT_TEXT_MAX);
    {
        unsigned len = 0;
        assert(cad_features_write_text(&doc, text, 10, &len) == CAD_FEAT_CAPACITY &&
               len == strlen(before));
    }
    /* argument validation */
    reset();
    sk[0] = sketch(0, CAD_FEAT_XY, "0");
    save();
    e[0] = "0"; e[1] = "0"; e[2] = "5";
    assert(cad_features_add_profile(&doc, sk[0], CAD_FEAT_CIRCLE, e, 3, 7, 0, &err)
           == CAD_FEAT_ARGUMENT && err.field == CAD_FEAT_FIELD_SIDES);
    assert(cad_features_add_profile(&doc, sk[0], CAD_FEAT_CIRCLE, e, 3, 33, 0, &err)
           == CAD_FEAT_ARGUMENT);
    assert(cad_features_add_profile(&doc, sk[0], CAD_FEAT_RECT, e, 3, 0, 0, &err)
           == CAD_FEAT_ARGUMENT);
    assert(cad_features_add_profile(&doc, sk[0], CAD_FEAT_POLY, e, 5, 0, 0, &err)
           == CAD_FEAT_ARGUMENT);
    assert(cad_features_add_profile(&doc, sk[0], CAD_FEAT_POLY, e, 4, 0, 0, &err)
           == CAD_FEAT_ARGUMENT);
    assert(cad_features_add_revolve(&doc, 0, sk[0], CAD_FEAT_ALL, CAD_FEAT_AXIS_U,
                                    11, 0, &err) == CAD_FEAT_ARGUMENT);
    assert(cad_features_add_revolve(&doc, 0, sk[0], CAD_FEAT_ALL, CAD_FEAT_AXIS_U,
                                    25, 0, &err) == CAD_FEAT_ARGUMENT);
    assert(cad_features_add_sketch(&doc, 0, (CadFeatPlane)3, "0", 0, &err)
           == CAD_FEAT_ARGUMENT && err.field == CAD_FEAT_FIELD_PLANE);
    unchanged();
    /* auto names skip names already taken */
    strcpy(name, "Sketch2");
    assert(cad_features_rename(&doc, sk[0], name, &err) == CAD_FEAT_OK);
    assert(sketch(0, CAD_FEAT_XY, "0") && cad_features_find(&doc, "Sketch1") != 0);
    assert(sketch(0, CAD_FEAT_XY, "0") && cad_features_find(&doc, "Sketch3") != 0);
    printf("capacity limits: PASS\n");
}

static void test_fail_atomic(void)
{
    unsigned s1, s2, e1;
    static char longexpr[CAD_FEAT_EXPR_MAX + 2];
    const char *e[6];
    reset();
    s1 = sketch(0, CAD_FEAT_XY, "0");
    rect(s1, "0", "0", "10", "10");
    e1 = extrude(s1, 0, CAD_FEAT_POSITIVE, "5");
    s2 = sketch("Other", CAD_FEAT_XY, "0");
    memset(longexpr, '1', CAD_FEAT_EXPR_MAX + 1);
    save();
    assert(cad_features_add_sketch(&doc, 0, CAD_FEAT_XY, "1\"", 0, &err) ==
           CAD_FEAT_EXPRESSION && err.field == CAD_FEAT_FIELD_OFFSET);
    assert(cad_features_add_sketch(&doc, 0, CAD_FEAT_XY, longexpr, 0, &err) ==
           CAD_FEAT_EXPRESSION);
    longexpr[CAD_FEAT_EXPR_MAX] = 0;           /* exactly 64 is fine */
    assert(cad_features_edit_sketch(&doc, s2, CAD_FEAT_YZ, "", &err) == CAD_FEAT_EXPRESSION);
    assert(cad_features_add_sketch(&doc, " lead", CAD_FEAT_XY, "0", 0, &err) == CAD_FEAT_NAME);
    assert(cad_features_add_sketch(&doc, "a\tb", CAD_FEAT_XY, "0", 0, &err) == CAD_FEAT_NAME);
    assert(cad_features_add_sketch(&doc, "0123456789012345678901234567890X",
                                   CAD_FEAT_XY, "0", 0, &err) == CAD_FEAT_NAME);
    assert(cad_features_rename(&doc, e1, "other", &err) == CAD_FEAT_DUPLICATE &&
           err.feature_id == s2);
    assert(cad_features_rename(&doc, 99, "x", &err) == CAD_FEAT_NOT_FOUND);
    e[0] = "0"; e[1] = "0"; e[2] = "10"; e[3] = "0\x7f"; e[4] = "0"; e[5] = "5";
    assert(cad_features_edit_profile(&doc, s1, 0, CAD_FEAT_POLY, e, 6, 0, &err) ==
           CAD_FEAT_EXPRESSION && err.profile == 0 && err.expr_index == 3);
    assert(cad_features_edit_profile(&doc, s1, 1, CAD_FEAT_POLY, e, 6, 0, &err) ==
           CAD_FEAT_NOT_FOUND);
    assert(cad_features_add_extrude(&doc, 0, e1, CAD_FEAT_ALL, CAD_FEAT_POSITIVE,
                                    "5", 0, &err) == CAD_FEAT_REFERENCE);
    assert(cad_features_add_extrude(&doc, 0, s1, 1, CAD_FEAT_POSITIVE, "5", 0, &err)
           == CAD_FEAT_REFERENCE && err.field == CAD_FEAT_FIELD_PROFILE);
    assert(cad_features_add_extrude(&doc, 0, s1, 0, (CadFeatDirection)3, "5", 0, &err)
           == CAD_FEAT_ARGUMENT);
    assert(cad_features_edit_sketch(&doc, e1, CAD_FEAT_XY, "0", &err) == CAD_FEAT_ARGUMENT);
    assert(cad_features_edit_revolve(&doc, e1, s1, 0, CAD_FEAT_AXIS_U, 12, &err) ==
           CAD_FEAT_ARGUMENT);
    assert(cad_features_add_profile(&doc, e1, CAD_FEAT_RECT, e, 4, 0, 0, &err) ==
           CAD_FEAT_ARGUMENT);
    assert(cad_features_remove_profile(&doc, s1, 0, &err) == CAD_FEAT_IN_USE &&
           err.feature_id == e1);
    assert(cad_features_add_sketch(0, 0, CAD_FEAT_XY, "0", 0, &err) == CAD_FEAT_ARGUMENT);
    unchanged();
    assert(cad_features_edit_sketch(&doc, s2, CAD_FEAT_YZ, longexpr, &err) == CAD_FEAT_OK);
    assert(cad_features_rename(&doc, e1, "OTHER2", &err) == CAD_FEAT_OK);
    assert(cad_features_rename(&doc, s2, "OTHER", &err) == CAD_FEAT_OK);   /* own name */
    printf("fail-atomic edits: PASS\n");
}

static void expect_read_error(const char *t, CadFeatStatus status, unsigned line)
{
    CadFeatStatus s = cad_features_read_text(&doc, t, (unsigned)strlen(t), &err);
    if (s != status || err.line != line)
        fprintf(stderr, "read [%s]: %s line %u\n", t, err.message, err.line);
    assert(s == status && err.status == status && err.line == line);
    unchanged();
}

static void test_serialization(void)
{
    static const char expected[] =
        "N 4\n"
        "F 1 SKETCH A \"Base Plate\" XY \"1 mm\"\n"
        "R RECT \"0\" \"0\" \"40\" \"20\"\n"
        "R CIRCLE 12 \"50\" \"30\" \"Width / 4\"\n"
        "F 2 EXTRUDE S \"Extrusion1\" 1 ALL SYM \"8 mm\"\n"
        "F 3 REVOLVE A \"Revolution1\" 1 1 U 16\n";
    static const char *const bad[][2] = {
        /* text, expected line (as a digit) */
        { "F 1 SKETCH A \"S\" XY \"0\"\n", "1" },
        { "N 02\n", "1" },
        { "N 0\n", "1" },
        { "N 2\nF 1 SKETCH A \"S\" XY \"0\"", "2" },
        { "N 2\nF 1 SKETCH A \"S\"  XY \"0\"\n", "2" },
        { "N 2\nF 1 SKETCH A \"S\" XY \"0\" \n", "2" },
        { "N 2\nF 1 SKETCH A \"S\" XX \"0\"\n", "2" },
        { "N 2\nF 1 SKETCH A \"S\" XY \"0\n", "2" },
        { "N 2\nF 1 SKETCH A \"S\"\tXY \"0\"\n", "2" },
        { "N 2\nF 1 SKETCH X \"S\" XY \"0\"\n", "2" },
        { "N 2\nF 1 SKETCH A \"\" XY \"0\"\n", "2" },
        { "N 2\nF 1 SKETCH A \"S\" XY \"\"\n", "2" },
        { "N 2\nF 1 SKETCH A S XY \"0\"\n", "2" },
        { "N 2\nF 2 SKETCH A \"S\" XY \"0\"\n", "2" },
        { "N 9\nF 1 SKETCH A \"S\" XY \"0\"\nF 1 SKETCH A \"T\" XY \"0\"\n", "3" },
        { "N 9\nF 1 SKETCH A \"S\" XY \"0\"\nF 2 SKETCH A \"s\" XY \"0\"\n", "3" },
        { "N 9\nR RECT \"0\" \"0\" \"1\" \"1\"\n", "2" },
        { "N 9\nF 1 SKETCH A \"S\" XY \"0\"\nR TRI \"0\"\n", "3" },
        { "N 9\nF 1 SKETCH A \"S\" XY \"0\"\nR CIRCLE 7 \"0\" \"0\" \"1\"\n", "3" },
        { "N 9\nF 1 SKETCH A \"S\" XY \"0\"\nR POLY 3 \"0\" \"0\" \"1\" \"0\" \"1\"\n", "3" },
        { "N 9\nF 1 SKETCH A \"S\" XY \"0\"\nR RECT \"0\" \"0\" \"1\" \"1\"\n"
          "F 2 EXTRUDE A \"E\" 1 ALL POS \"5\"\nR RECT \"0\" \"0\" \"1\" \"1\"\n", "5" },
        { "N 9\nF 2 EXTRUDE A \"E\" 3 ALL POS \"5\"\nF 3 SKETCH A \"S\" XY \"0\"\n", "2" },
        { "N 9\nF 1 SKETCH A \"S\" XY \"0\"\nF 2 EXTRUDE A \"E\" 1 0 POS \"5\"\n", "3" },
        { "N 9\nF 1 SKETCH A \"S\" XY \"0\"\nF 2 EXTRUDE A \"E\" 1 ALL UP \"5\"\n", "3" },
        { "N 9\nF 1 SKETCH A \"S\" XY \"0\"\nF 2 REVOLVE A \"E\" 1 ALL U 25\n", "3" },
        { "N 9\nF 1 SKETCH A \"S\" XY \"0\"\nF 2 EXTRUDE A \"E\" 1 ALL POS\n", "3" },
        { "N 9\n\n", "2" },
        { "N 9\nX\n", "2" },
        { "N 9\nF 1 SKETCH A \"S\" XY \"0\"\nF 2 EXTRUDE A \"E\" 2 ALL POS \"5\"\n", "3" },
    };
    char longline[200];
    unsigned i, n;
    unsigned s1, s2, s3, e1;
    CadFeatStatus s;
    reset();
    param("Width", "40 mm");
    s1 = sketch("Base Plate", CAD_FEAT_XY, "1 mm");
    rect(s1, "0", "0", "40", "20");
    circle(s1, "50", "30", "Width / 4", 12);
    e1 = extrude(s1, CAD_FEAT_ALL, CAD_FEAT_SYMMETRIC, "8 mm");
    assert(cad_features_suppress(&doc, e1, 1, &err) == CAD_FEAT_OK);
    revolve(s1, CAD_FEAT_AXIS_U, 16);
    assert(cad_features_edit_revolve(&doc, 3, s1, 1, CAD_FEAT_AXIS_U, 16, &err) == CAD_FEAT_OK);
    n = snapshot(&doc, text);
    if (strcmp(text, expected) != 0) fprintf(stderr, "%s", text);
    assert(strcmp(text, expected) == 0 && n == strlen(expected));
    assert(cad_features_read_text(&other, expected, n, &err) == CAD_FEAT_OK);
    snapshot(&other, before);
    assert(strcmp(before, expected) == 0);
    /* richer document: regenerate both and compare output */
    s2 = sketch(0, CAD_FEAT_XZ, "Width - 45 mm");
    poly(s2, "0 0 30 0 30 10 15 25 0 10");
    extrude(s2, 0, CAD_FEAT_NEGATIVE, "Width / 8");
    s3 = sketch(0, CAD_FEAT_YZ, "-100");
    circle(s3, "0", "0", "(Width + 2 mm) / 2", 0);
    extrude(s3, CAD_FEAT_ALL, CAD_FEAT_POSITIVE, "12.5 mm");
    assert(cad_features_suppress(&doc, s3, 1, &err) == CAD_FEAT_OK);
    n = snapshot(&doc, text);
    assert(cad_features_read_text(&other, text, n, &err) == CAD_FEAT_OK);
    snapshot(&other, before);
    assert(strcmp(before, text) == 0);
    assert(memcmp(&other, &doc, sizeof(doc)) == 0);
    regen();
    out2 = out;
    assert(cad_features_regenerate(&other, &params, &out, 0) == CAD_FEAT_OK);
    assert(memcmp(&out, &out2, sizeof(out)) == 0);
    assert(out.segment_count > 0 && out.face_count > 0);
    /* malformed input: rejected with line numbers, document unchanged */
    save();
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        CadFeatStatus want = CAD_FEAT_FORMAT;
        if (i == 10) want = CAD_FEAT_NAME;
        if (i == 11) want = CAD_FEAT_EXPRESSION;
        if (i == 15) want = CAD_FEAT_DUPLICATE;
        if (i == 21 || i == 22 || i == 28) want = CAD_FEAT_REFERENCE;
        if (i == 12) want = CAD_FEAT_NAME;
        expect_read_error(bad[i][0], want, (unsigned)(bad[i][1][0] - '0'));
    }
    /* expression longer than 64 characters */
    strcpy(longline, "N 9\nF 1 SKETCH A \"S\" XY \"");
    for (i = 0; i < 65; ++i) strcat(longline, "1");
    strcat(longline, "\"\n");
    expect_read_error(longline, CAD_FEAT_EXPRESSION, 2);
    /* over the text limit */
    s = cad_features_read_text(&doc, text, CAD_FEAT_TEXT_MAX + 1, &err);
    assert(s == CAD_FEAT_CAPACITY);
    unchanged();
    /* empty text is an empty document */
    assert(cad_features_read_text(&other, "", 0, &err) == CAD_FEAT_OK &&
           cad_features_count(&other) == 0);
    n = snapshot(&other, text);
    assert(strcmp(text, "N 1\n") == 0 && n == 4);
    /* next_id survives a round trip so IDs are never reused */
    assert(cad_features_read_text(&other, "N 40\n", 5, &err) == CAD_FEAT_OK);
    {
        unsigned id = 0;
        assert(cad_features_add_sketch(&other, 0, CAD_FEAT_XY, "0", &id, &err) ==
               CAD_FEAT_OK && id == 40);
    }
    printf("serialization: PASS\n");
}

int main(void)
{
    setvbuf(stdout, 0, _IONBF, 0);
    printf("sizeof CadFeatures=%u CadFeatOutput=%u\n", (unsigned)sizeof(CadFeatures),
           (unsigned)sizeof(CadFeatOutput));
    test_planes();
    test_extrude_shapes();
    test_associativity();
    test_fail_soft();
    test_suppress_remove_move();
    test_revolve();
    test_output_limits();
    test_capacity();
    test_fail_atomic();
    test_serialization();
    printf("features: all tests PASS\n");
    return 0;
}
