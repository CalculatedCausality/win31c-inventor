#include "../cad_viewcube.h"
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static const double pi = 3.14159265358979323846;

static const CadCubeQuad *face(const CadCubeGeometry *g, int hit)
{
    int i;
    for (i = 0; i < g->quadCount; ++i)
        if ((int)g->quads[i].hit == hit) return &g->quads[i];
    return NULL;
}

static void cardinal_test(void)
{
    static const int yaw[6] = {0,128,0,0,192,64};
    static const int pitch[6] = {0,0,64,192,0,0};
    static const char * const labels[6] = {
        "FRONT","BACK","TOP","BOTTOM","RIGHT","LEFT"
    };
    CadCubeGeometry g;
    const CadCubeQuad *q;
    int i, j, y, p, visible;
    for (i = 0; i < 6; ++i) {
        assert(cad_cube_orientation(i+1,&y,&p));
        assert(y == yaw[i] && p == pitch[i]);
        assert(CadCubeBuild(y,p,20,30,100,&g));
        assert(g.quadCount == 1);
        q = &g.quads[0];
        assert((int)q->hit == i+1 && !strcmp(q->label,labels[i]));
        assert(q->bounds.left == 48 && q->bounds.right == 92);
        assert(q->bounds.top == 58 && q->bounds.bottom == 102);
        assert(q->labelCenter.x == 70 && q->labelCenter.y == 80);
        assert((int)cad_cube_hit(&g,70,80) == i+1);
        visible = 0;
        for (j = 0; j < 8; ++j) {
            if (g.cornerVisible[j]) {
                ++visible;
                assert(cad_cube_hit(&g,g.corners[j].x,g.corners[j].y)
                       == g.cornerHits[j]);
            }
        }
        assert(visible == 4);
        /* Boundary is a face, not an axis-aligned widget shortcut. */
        assert((int)cad_cube_hit(&g,q->bounds.left,80) == i+1);
        assert(cad_cube_hit(&g,q->bounds.left-1,80) == CAD_CUBE_NONE);
    }
    assert(CadCubeBuild(0,0,0,0,100,&g));
    assert(!face(&g,CAD_CUBE_RIGHT));
    assert(g.corners[4].x == 28 && g.corners[4].y == 72);
    assert(g.corners[7].x == 72 && g.corners[7].y == 28);
    /* Hidden back corners project identically, but never win hit ties. */
    for (i = 0; i < 4; ++i) {
        assert(!g.cornerVisible[i]);
        assert(cad_cube_hit(&g,g.corners[i].x,g.corners[i].y)
               == g.cornerHits[i+4]);
    }
    assert(CadCubeBuild(128,0,0,0,100,&g));
    assert(g.corners[0].x == 72 && g.corners[0].y == 72);
    assert(g.corners[3].x == 28 && g.corners[3].y == 28);
    assert(CadCubeBuild(0,64,0,0,100,&g));
    assert(g.corners[6].y == 72 && g.corners[2].y == 28);
    assert(CadCubeBuild(192,0,0,0,100,&g));
    assert(g.corners[5].x == 28 && g.corners[1].x == 72);
}

static void corner_test(void)
{
    static const int expectedYaw[8] = {96,160,96,160,32,224,32,224};
    CadCubeGeometry g;
    int i, j, yaw, pitch, visible, nearest;
    for (i = 0; i < 8; ++i) {
        assert(cad_cube_orientation(16+i,&yaw,&pitch));
        assert(yaw == expectedYaw[i]);
        assert(pitch == ((i & 2) ? 25 : 231));
        assert(CadCubeBuild(yaw,pitch,-100,-100,100,&g));
        assert(g.quadCount == 3);
        assert(face(&g,(i & 1) ? CAD_CUBE_RIGHT : CAD_CUBE_LEFT));
        assert(face(&g,(i & 2) ? CAD_CUBE_TOP : CAD_CUBE_BOTTOM));
        assert(face(&g,(i & 4) ? CAD_CUBE_FRONT : CAD_CUBE_BACK));
        visible = 0; nearest = 0;
        for (j = 0; j < 8; ++j) {
            assert(g.cornerHits[j] == (CadCubeHit)(16+j));
            visible += g.cornerVisible[j];
            if (g.cornerDepth[j] > g.cornerDepth[nearest]) nearest = j;
            if (g.cornerVisible[j])
                assert(cad_cube_hit(&g,g.corners[j].x,g.corners[j].y)
                       == g.cornerHits[j]);
        }
        assert(visible == 7 && nearest == i);
        assert(!g.cornerVisible[i ^ 7]);
        /* Actual inverse lookup: canonical viewer sign corner sits at center,
         * whereas the opposite rear corner is excluded despite overlapping. */
        assert(cad_cube_hit(&g,g.center.x,g.center.y) == (CadCubeHit)(16+i));
        assert(cad_cube_hit(&g,g.corners[i ^ 7].x,g.corners[i ^ 7].y)
               == (CadCubeHit)(16+i));
        for (j = 0; j < 3; ++j)
            assert(cad_cube_hit(&g,g.quads[j].labelCenter.x,
                                g.quads[j].labelCenter.y) == g.quads[j].hit);
    }
}

/* Independent analytic projection, not copies of implementation tables. */
static void projection_test(void)
{
    static const int angles[] = {0,17,25,32,63,64,65,96,128,160,192,224,231,255};
    CadCubeGeometry g;
    double sy, cy, sp, cp, x, y, z, u, v, depth, nx, ny, nz;
    int a, b, i, size, visible;
    for (size = 24; size <= 512; size += 61) {
        for (a = 0; a < 14; ++a) for (b = 0; b < 14; ++b) {
            assert(CadCubeBuild(angles[a],angles[b],-500,-600,size,&g));
            sy = sin(angles[a]*pi/128); cy = cos(angles[a]*pi/128);
            sp = sin(angles[b]*pi/128); cp = cos(angles[b]*pi/128);
            for (i = 0; i < 8; ++i) {
                x = (i & 1) ? 1.0 : -1.0;
                y = (i & 2) ? 1.0 : -1.0;
                z = (i & 4) ? 1.0 : -1.0;
                u = x*cy+z*sy;
                v = y*cp-(-x*sy+z*cy)*sp;
                depth = y*sp+(-x*sy+z*cy)*cp;
                assert(fabs(g.corners[i].x-(g.center.x+u*(size*22/100))) < 1.1);
                assert(fabs(g.corners[i].y-(g.center.y-v*(size*22/100))) < 1.1);
                assert(fabs(g.cornerDepth[i]/1048576.0-depth) < 0.004);
                assert(g.corners[i].x >= g.bounds.left);
                assert(g.corners[i].x <= g.bounds.right);
                assert(g.corners[i].y >= g.bounds.top);
                assert(g.corners[i].y <= g.bounds.bottom);
            }
            for (i = 0; i < 6; ++i) {
                nx = ny = nz = 0.0;
                if (i == 0) nz = 1;
                if (i == 1) nz = -1;
                if (i == 2) ny = 1;
                if (i == 3) ny = -1;
                if (i == 4) nx = 1;
                if (i == 5) nx = -1;
                depth = ny*sp+(-nx*sy+nz*cy)*cp;
                visible = face(&g,i+1) != NULL;
                if (depth > 0.15) assert(visible);
                if (depth < 0.00001) assert(!visible);
            }
        }
    }
}

static void hit_test(void)
{
    CadCubeGeometry g;
    int c, r, x, y;
    assert(CadCubeBuild(0,0,-100,-100,100,&g));
    c = g.center.x; r = g.outerOrbitRing.outerRadius;
    assert(g.outerOrbitRing.innerRadius == 40 && r == 48);
    assert(g.outerOrbitRing.bounds.left == c-r);
    assert(cad_cube_hit(&g,c+r,c) == CAD_CUBE_ORBIT);
    assert(cad_cube_hit(&g,c-r,c) == CAD_CUBE_ORBIT);
    assert(cad_cube_hit(&g,c,c+r) == CAD_CUBE_ORBIT);
    assert(cad_cube_hit(&g,c+40,c) == CAD_CUBE_ORBIT);
    assert(cad_cube_hit(&g,c+39,c) == CAD_CUBE_NONE);
    assert(cad_cube_hit(&g,c+r+1,c) == CAD_CUBE_NONE);
    assert(cad_cube_hit(&g,c+r,c+r) == CAD_CUBE_NONE);
    x = g.corners[4].x; y = g.corners[4].y;
    assert(cad_cube_hit(&g,x-6,y) == (CadCubeHit)20);
    assert(cad_cube_hit(&g,x-7,y) == CAD_CUBE_NONE);
    assert(cad_cube_hit(&g,x-4,y+4) == (CadCubeHit)20);
    assert(cad_cube_hit(&g,x-5,y+4) == CAD_CUBE_NONE);
    assert(cad_cube_hit(&g,INT_MIN,INT_MAX) == CAD_CUBE_NONE);
    assert(cad_cube_hit(&g,INT_MAX,INT_MIN) == CAD_CUBE_NONE);
    assert(cad_cube_hit(NULL,0,0) == CAD_CUBE_NONE);
    /* A visible face's bounding rectangle includes points outside its quad. */
    assert(CadCubeBuild(224,25,0,0,100,&g));
    assert(cad_cube_hit(&g,22,22) == CAD_CUBE_NONE);
    /* Exact shared edge, well clear of the corner selectors. Stable tie
     * chooses FRONT rather than TOP regardless of quad drawing order. */
    assert(CadCubeBuild(224,25,-256,-256,512,&g));
    x = (g.corners[6].x+g.corners[7].x)/2;
    y = (g.corners[6].y+g.corners[7].y)/2;
    assert(cad_cube_hit(&g,x,y) == CAD_CUBE_FRONT);
}

static void invalid_test(void)
{
    CadCubeGeometry g, saved, wrapped;
    int yaw = 77, pitch = 88, hit;
    assert(CadCubeBuild(32,25,0,0,100,&g)); saved = g;
    assert(!CadCubeBuild(0,0,0,0,23,&g));
    assert(!CadCubeBuild(0,0,0,0,513,&g));
    assert(!CadCubeBuild(0,0,32700,0,100,&g));
    assert(!CadCubeBuild(0,0,0,32700,100,&g));
    assert(!CadCubeBuild(0,0,INT_MAX,INT_MIN,100,&g));
#if INT_MIN < (-32767 - 1)
    assert(!CadCubeBuild(0,0,-32769,0,100,&g));
    assert(!CadCubeBuild(0,0,0,-32769,100,&g));
#endif
    assert(!CadCubeBuild(0,0,0,0,100,NULL));
    assert(!memcmp(&g,&saved,sizeof(g)));
    assert(CadCubeBuild(-224,-231,0,0,100,&wrapped));
    assert(g.yaw == wrapped.yaw && g.pitch == wrapped.pitch);
    assert(!memcmp(g.corners,wrapped.corners,sizeof(g.corners)));
    assert(!memcmp(g.cornerDepth,wrapped.cornerDepth,sizeof(g.cornerDepth)));
    assert(CadCubeBuild(INT_MIN,INT_MAX,-32768,-32768,512,&g));
    assert(CadCubeBuild(224,25,32256,32256,512,&g));
    assert(g.bounds.right == 32767 && g.bounds.bottom == 32767);
    assert(cad_cube_hit(&g,g.center.x,g.center.y) == (CadCubeHit)23);
    for (hit = -1; hit < 26; ++hit) {
        if ((hit >= 1 && hit <= 6) || (hit >= 16 && hit <= 23)) continue;
        assert(!cad_cube_orientation(hit,&yaw,&pitch));
        assert(yaw == 77 && pitch == 88);
    }
    assert(!cad_cube_orientation(1,NULL,&pitch));
    assert(!cad_cube_orientation(1,&yaw,NULL));
    assert(!cad_cube_orientation(1,&yaw,&yaw));
    assert(yaw == 77 && pitch == 88);
}

static void all_angles_test(void)
{
    static const int sizes[5] = {24,25,100,511,512};
    CadCubeGeometry g;
    CadCubePoint a, b, c;
    int yaw, pitch, i, j, count, s;
    long dx, dy, turn;
    for (s = 0; s < 5; ++s)
    for (yaw = 0; yaw < 256; ++yaw) for (pitch = 0; pitch < 256; ++pitch) {
        assert(CadCubeBuild(yaw,pitch,-256,-256,sizes[s],&g));
        assert(g.quadCount >= 1 && g.quadCount <= 3);
        count = 0;
        for (i = 0; i < 8; ++i) {
            if (g.cornerVisible[i]) ++count;
            dx = (long)g.corners[i].x-g.center.x;
            dy = (long)g.corners[i].y-g.center.y;
            assert(dx*dx+dy*dy < (long)g.outerOrbitRing.innerRadius*
                                      g.outerOrbitRing.innerRadius);
        }
        assert(count >= 4 && count <= 7);
        for (i = 0; i < g.quadCount; ++i) {
            assert(g.quads[i].depth > 0);
            for (j = 0; j < 4; ++j) {
                assert(g.cornerVisible[g.quads[i].corners[j]]);
                a = g.quads[i].points[j];
                b = g.quads[i].points[(j+1)%4];
                c = g.quads[i].points[(j+2)%4];
                turn = ((long)b.x-a.x)*((long)c.y-b.y)
                     - ((long)b.y-a.y)*((long)c.x-b.x);
                assert(turn <= 0);
            }
        }
    }
}

int main(void)
{
    cardinal_test(); corner_test(); projection_test(); hit_test();
    invalid_test(); all_angles_test();
    puts("viewcube: cardinal labels, analytic projection, inverse corner lookup,"
         " polygon/ring hits, limits and all 65536 orientations at five sizes PASS");
    return 0;
}
