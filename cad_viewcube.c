#include "cad_viewcube.h"
#include <limits.h>

static const int quarter[65] = {
    0,25,50,75,100,125,150,175,200,224,249,273,297,321,345,369,
    392,415,438,460,483,505,526,548,569,590,610,630,650,669,688,707,
    724,742,759,775,792,807,822,836,851,865,878,891,903,915,926,936,
    946,955,964,972,980,987,994,1000,1005,1009,1013,1016,1019,1021,
    1023,1024,1024
};
static const int faceCorners[6][4] = {
    {4,5,7,6}, {1,0,2,3}, {2,6,7,3},
    {0,1,5,4}, {1,3,7,5}, {0,4,6,2}
};
static const char * const faceLabels[6] = {
    "FRONT", "BACK", "TOP", "BOTTOM", "RIGHT", "LEFT"
};

static int wrap(int a)
{
    a %= 256;
    return a < 0 ? a + 256 : a;
}

static int sine(int a)
{
    a = wrap(a);
    if (a <= 64) return quarter[a];
    if (a <= 128) return quarter[128-a];
    if (a <= 192) return -quarter[a-128];
    return -quarter[256-a];
}

static int rounded(long value)
{
    if (value < 0) return (int)(-((-value + 524288L) / 1048576L));
    return (int)((value + 524288L) / 1048576L);
}

static CadCubeBounds pointBounds(CadCubePoint p)
{
    CadCubeBounds b;
    b.left = b.right = p.x;
    b.top = b.bottom = p.y;
    return b;
}

static void extend(CadCubeBounds *b, CadCubePoint p)
{
    if (p.x < b->left) b->left = p.x;
    if (p.x > b->right) b->right = p.x;
    if (p.y < b->top) b->top = p.y;
    if (p.y > b->bottom) b->bottom = p.y;
}

static long cross(CadCubePoint a, CadCubePoint b, CadCubePoint p)
{
    return ((long)b.x-a.x)*((long)p.y-a.y)
         - ((long)b.y-a.y)*((long)p.x-a.x);
}

int CadCubeBuild(int yaw, int pitch, int left, int top, int size,
                 CadCubeGeometry *geometry)
{
    CadCubeGeometry g = {0};
    CadCubeQuad *q;
    long sy, cy, sp, cp, x, y, z, u, z1, v, normals[6];
    long sx, syScreen, area;
    int i, j, k, half;
    if (!geometry || size < CAD_CUBE_MIN_SIZE || size > CAD_CUBE_MAX_SIZE)
        return 0;
    /* Check origin before adding size: safe even with 32-bit host int. */
#if INT_MIN < (-32767 - 1)
    if (left < -32768L || top < -32768L) return 0;
#endif
#if INT_MAX > 32767
    if (left > 32767L || top > 32767L) return 0;
#endif
    if ((long)left + size - 1 > 32767L ||
        (long)top + size - 1 > 32767L) return 0;
    g.yaw = wrap(yaw); g.pitch = wrap(pitch); g.size = size;
    g.bounds.left = left; g.bounds.top = top;
    g.bounds.right = (int)((long)left + size - 1);
    g.bounds.bottom = (int)((long)top + size - 1);
    g.center.x = (int)((long)left + size/2);
    g.center.y = (int)((long)top + size/2);
    g.outerOrbitRing.center = g.center;
    g.outerOrbitRing.innerRadius = (int)(((long)size*40+99)/100);
    g.outerOrbitRing.outerRadius = (int)((long)size*48/100);
    i = g.outerOrbitRing.outerRadius;
    g.outerOrbitRing.bounds.left = g.center.x-i;
    g.outerOrbitRing.bounds.right = g.center.x+i;
    g.outerOrbitRing.bounds.top = g.center.y-i;
    g.outerOrbitRing.bounds.bottom = g.center.y+i;
    sy = sine(g.yaw); cy = sine(wrap(g.yaw+64));
    sp = sine(g.pitch); cp = sine(wrap(g.pitch+64));
    half = (int)((long)size*22/100);
    for (i = 0; i < 8; ++i) {
        x = (i & 1) ? 1L : -1L;
        y = (i & 2) ? 1L : -1L;
        z = (i & 4) ? 1L : -1L;
        u = x*cy + z*sy;
        z1 = -x*sy + z*cy;
        v = y*cp*1024L-z1*sp;
        g.cornerDepth[i] = y*sp*1024L+z1*cp;
        g.corners[i].x = g.center.x + rounded(u*1024L*half);
        g.corners[i].y = g.center.y - rounded(v*half);
        g.cornerHits[i] = (CadCubeHit)(CAD_CUBE_CORNER_BASE+i);
        if (!i) g.cubeBounds = pointBounds(g.corners[i]);
        else extend(&g.cubeBounds, g.corners[i]);
    }
    normals[0] = cy*cp; normals[1] = -normals[0];
    normals[2] = sp*1024L; normals[3] = -normals[2];
    normals[4] = -sy*cp; normals[5] = -normals[4];
    for (i = 0; i < 6; ++i) {
        if (normals[i] <= 0) continue;
        q = &g.quads[g.quadCount];
        q->hit = (CadCubeHit)(i+1); q->label = faceLabels[i];
        q->depth = normals[i]; sx = syScreen = 0;
        for (j = 0; j < 4; ++j) {
            k = faceCorners[i][j];
            q->corners[j] = k; q->points[j] = g.corners[k];
            sx += q->points[j].x; syScreen += q->points[j].y;
            if (!j) q->bounds = pointBounds(q->points[j]);
            else extend(&q->bounds, q->points[j]);
        }
        area = cross(q->points[0],q->points[1],q->points[2])
             + cross(q->points[0],q->points[2],q->points[3]);
        if (!area) continue;
        q->labelCenter.x = (int)(sx/4);
        q->labelCenter.y = (int)(syScreen/4);
        for (j = 0; j < 4; ++j) g.cornerVisible[q->corners[j]] = 1;
        ++g.quadCount;
    }
    *geometry = g;
    return 1;
}

static int inQuad(const CadCubeQuad *q, CadCubePoint p)
{
    long c;
    int j, positive = 0, negative = 0;
    if (p.x < q->bounds.left || p.x > q->bounds.right ||
        p.y < q->bounds.top || p.y > q->bounds.bottom) return 0;
    for (j = 0; j < 4; ++j) {
        c = cross(q->points[j], q->points[(j+1)%4], p);
        if (c > 0) positive = 1;
        if (c < 0) negative = 1;
    }
    return !(positive && negative);
}

CadCubeHit cad_cube_hit(const CadCubeGeometry *g, int x, int y)
{
    CadCubePoint p;
    long dx, dy, distance, bestDistance = 37L;
    int i, best = -1;
    if (!g) return CAD_CUBE_NONE;
    /* Bound rejection BEFORE subtraction protects 32-bit long at INT limits. */
    if (x < g->bounds.left || x > g->bounds.right ||
        y < g->bounds.top || y > g->bounds.bottom) return CAD_CUBE_NONE;
    p.x = x; p.y = y;
    for (i = 0; i < 8; ++i) {
        if (!g->cornerVisible[i]) continue;
        dx = (long)x-g->corners[i].x; dy = (long)y-g->corners[i].y;
        if (dx < -6 || dx > 6 || dy < -6 || dy > 6) continue;
        distance = dx*dx+dy*dy;
        if (distance <= 36L && (distance < bestDistance ||
            (distance == bestDistance && best >= 0 &&
             g->cornerDepth[i] > g->cornerDepth[best]))) {
            best = i; bestDistance = distance;
        }
    }
    if (best >= 0) return g->cornerHits[best];
    for (i = 0; i < g->quadCount; ++i)
        if (inQuad(&g->quads[i],p)) return g->quads[i].hit;
    dx = (long)x-g->center.x; dy = (long)y-g->center.y;
    distance = dx*dx+dy*dy;
    if (distance >= (long)g->outerOrbitRing.innerRadius*
                    g->outerOrbitRing.innerRadius &&
        distance <= (long)g->outerOrbitRing.outerRadius*
                    g->outerOrbitRing.outerRadius) return CAD_CUBE_ORBIT;
    return CAD_CUBE_NONE;
}

int cad_cube_orientation(int hit, int *yaw, int *pitch)
{
    int y, p, corner;
    if (!yaw || !pitch || yaw == pitch) return 0;
    y = p = 0;
    switch (hit) {
    case CAD_CUBE_FRONT: break;
    case CAD_CUBE_BACK: y = 128; break;
    case CAD_CUBE_TOP: p = 64; break;
    case CAD_CUBE_BOTTOM: p = 192; break;
    case CAD_CUBE_RIGHT: y = 192; break;
    case CAD_CUBE_LEFT: y = 64; break;
    default:
        if (hit < CAD_CUBE_CORNER_BASE || hit > CAD_CUBE_CORNER_LAST)
            return 0;
        corner = hit-CAD_CUBE_CORNER_BASE;
        if (corner & 4) y = (corner & 1) ? 224 : 32;
        else y = (corner & 1) ? 160 : 96;
        p = (corner & 2) ? 25 : 231;
        break;
    }
    *yaw = y; *pitch = p;
    return 1;
}
