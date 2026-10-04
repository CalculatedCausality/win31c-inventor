#include "cad_meshview.h"
#include <limits.h>
#include <stddef.h>

typedef struct { long x, y, z; } ViewPoint;
typedef struct { double x, y, z; } ClipPoint;
typedef struct {
    double depth;
    int x[3], y[3];
    unsigned short shade;
} ViewTriangle;

/* Even with 32-bit host int and 8-byte double this array is 40960 bytes.
 * Large-model Watcom -zt=1 puts each scratch array in a separate segment. */
static ViewPoint transformed[CAD_MESHVIEW_MAX_VERTICES];
static ClipPoint orthoTransformed[CAD_MESHVIEW_MAX_VERTICES];
static ViewTriangle triangles[CAD_MESHVIEW_MAX_TRIANGLES];
static unsigned short order[CAD_MESHVIEW_MAX_TRIANGLES];
static int rendering;

/* Quarter-wave Q10, rounded to nearest. No per-vertex trig or libm. */
static const int quarter[65] = {
    0,25,50,75,100,125,150,175,200,224,249,273,297,321,345,369,
    392,415,438,461,483,505,526,548,569,590,610,630,650,669,688,706,
    724,742,759,775,792,807,822,837,851,865,878,891,903,915,926,936,
    946,955,964,972,980,987,993,999,1004,1009,1013,1016,1019,1021,
    1023,1024,1024
};

/* genericCAD's legacy Q10 table differs by one at a few entries. Retain the
 * perspective table/behavior; orthographic uses the UI's exact coefficients. */
static const int orthoQuarter[65] = {
    0,25,50,75,100,125,150,175,200,224,249,273,297,321,345,369,
    392,415,438,460,483,505,526,548,569,590,610,630,650,669,688,707,
    724,742,759,775,792,807,822,836,851,865,878,891,903,915,926,936,
    946,955,964,972,980,987,994,1000,1005,1009,1013,1016,1019,1021,
    1023,1024,1024
};

static int sine(int a, const int *table)
{
    a %= 256;
    if (a < 0) a += 256;
    if (a <= 64) return table[a];
    if (a <= 128) return table[128-a];
    if (a <= 192) return -table[a-128];
    return -table[256-a];
}

static long magnitude(long a) { return a < 0 ? -a : a; }

static void normal(const ViewPoint *a, const ViewPoint *b,
                   const ViewPoint *c, long *nx, long *ny, long *nz)
{
    long ux, uy, uz, vx, vy, vz;
    ux = b->x-a->x; uy = b->y-a->y; uz = b->z-a->z;
    vx = c->x-a->x; vy = c->y-a->y; vz = c->z-a->z;
    *nx = uy*vz-uz*vy;
    *ny = uz*vx-ux*vz;
    *nz = ux*vy-uy*vx;
}

/* Sutherland-Hodgman, with depth carried through projection/viewport edges.
 * Near triangle <=4 points; convex quad clipped to rectangle <=8 points.
 * Buffers have 12 slots including conservative intermediate capacity. */
static double distance_to(const ClipPoint *p, int plane, double bound)
{
    if (plane == 0) return p->z-bound;
    if (plane == 1) return p->x-bound;
    if (plane == 2) return bound-p->x;
    if (plane == 3) return p->y-bound;
    return bound-p->y;
}

static int clip(const ClipPoint *in, int n, ClipPoint *out,
                int plane, double bound, int perspective)
{
    int i, count;
    ClipPoint a, b, p;
    double da, db, t;
    if (!n) return 0;
    count = 0; a = in[n-1]; da = distance_to(&a, plane, bound);
    for (i = 0; i < n; ++i) {
        b = in[i]; db = distance_to(&b, plane, bound);
        /* Boundary vertices are emitted once, not once as an intersection
         * and again as an endpoint (keeps the convex polygon size bound). */
        if ((da < 0.0 && db > 0.0) || (da > 0.0 && db < 0.0)) {
            t = da/(da-db);
            p.x = a.x+(b.x-a.x)*t;
            p.y = a.y+(b.y-a.y)*t;
            p.z = a.z+(b.z-a.z)*t;
            /* After projection, reciprocal depth is linear in screen space. */
            if (perspective && plane != 0)
                p.z = 1.0/((1.0-t)/a.z+t/b.z);
            if (plane == 0) p.z = bound;
            else if (plane <= 2) p.x = bound;
            else p.y = bound;
            out[count++] = p;
        }
        if (db >= 0.0) out[count++] = b;
        a = b; da = db;
    }
    return count;
}

static int pixel(double value, int low, int high)
{
    if (value <= low) return low;
    if (value >= high) return high;
    return (int)(value+0.5);
}

static CadMeshViewStatus prepare(const CadMeshViewVertex *v, int nv,
    const CadMeshViewFace *f, int nf, const CadMeshViewCamera *c,
    const CadMeshViewSink *sink, const CadMeshViewOrtho *ortho, int *count)
{
    int i, j, k, sy, cy, sp, cp, n, m, right, bottom, level;
    long x, y, z, rx, rz, nx, ny, nz, norm, light;
    ViewPoint a, b, d;
    ClipPoint p[12], q[12];
    ViewTriangle *tri;
    double facing, area, dx, dy, dz, du, dz1, ux, uy, uz, vx, vy, vz;
    double dnx, dny, dnz, dnorm, dlight;
    const int *table;
    unsigned short shade, key;
    *count = 0;
    if (!c || !sink || !sink->triangle || nv < 0 || nf < 0 ||
        (nv && !v) || (nf && !f)) return CAD_MESHVIEW_INVALID;
    if (nv > CAD_MESHVIEW_MAX_VERTICES || nf > CAD_MESHVIEW_MAX_FACES)
        return CAD_MESHVIEW_RANGE;
    /* Positive comparisons reject NaN as well as infinities in portable C89.
     * Bounded doubles keep all orthographic math finite (no casts to long). */
    if (ortho &&
        (!(ortho->centerX >= -16000.0 && ortho->centerX <= 16000.0) ||
         !(ortho->centerY >= -16000.0 && ortho->centerY <= 16000.0) ||
         !(ortho->centerZ >= -16000.0 && ortho->centerZ <= 16000.0) ||
         !(ortho->pixelsPerUnit > 0.0 && ortho->pixelsPerUnit <= 8.0)))
        return CAD_MESHVIEW_RANGE;
#if INT_MAX > 32767
    /* Reject wide-host values before evaluating even the viewport sums. */
    if (c->left > 32767 || c->top > 32767 || c->width > 32767 ||
        c->height > 32767 || c->focal > 32767)
        return CAD_MESHVIEW_RANGE;
#endif
    if (c->pitch < -64 || c->pitch > 64 || c->left < 0 || c->top < 0 ||
        c->width <= 0 || c->height <= 0 ||
        (long)c->left+c->width-1 > 32767L ||
        (long)c->top+c->height-1 > 32767L ||
        c->cameraDistance < 800 || c->cameraDistance > 16000 ||
        c->focal <= 0 ||
        c->translateX < -16000 || c->translateX > 16000 ||
        c->translateY < -16000 || c->translateY > 16000 ||
        c->translateZ < -16000 || c->translateZ > 16000 ||
        (c->backfaceCull != 0 && c->backfaceCull != 1))
        return CAD_MESHVIEW_RANGE;
    right = (int)((long)c->left+c->width-1);
    bottom = (int)((long)c->top+c->height-1);
    for (i = 0; i < nv; ++i) {
        if (v[i].x < -400 || v[i].x > 400 || v[i].y < -400 ||
            v[i].y > 400 || v[i].z < -400 || v[i].z > 400)
            return CAD_MESHVIEW_RANGE;
    }
    /* Validate ORIGINAL faces before rotation rounding can collapse them.
     * Original differences <=800: cross <=1,280,000, safely signed 32-bit. */
    for (i = 0; i < nf; ++i) {
        if (f[i].a >= nv || f[i].b >= nv || f[i].c >= nv)
            return CAD_MESHVIEW_INVALID;
        if (f[i].material >= CAD_MESHVIEW_MAX_MATERIALS)
            return CAD_MESHVIEW_RANGE;
        a.x=v[f[i].a].x; a.y=v[f[i].a].y; a.z=v[f[i].a].z;
        b.x=v[f[i].b].x; b.y=v[f[i].b].y; b.z=v[f[i].b].z;
        d.x=v[f[i].c].x; d.y=v[f[i].c].y; d.z=v[f[i].c].z;
        normal(&a,&b,&d,&nx,&ny,&nz);
        if (!nx && !ny && !nz) return CAD_MESHVIEW_INVALID;
    }
    table=ortho ? orthoQuarter : quarter;
    sy=sine(c->yaw,table); cy=sine((c->yaw % 256)+64,table);
    sp=sine(c->pitch,table); cp=sine(c->pitch+64,table);
    for (i = 0; i < nv; ++i) {
        if (ortho) {
            /* Centered inputs <=16400, first stage <=32800, second <=49200;
             * screen offsets <=393600. Edge differences cancel the center,
             * remain <=2400, and double cross products are bounded by 11520000.
             * No fractional coordinate/normal is narrowed to a 16-bit int
             * or 32-bit long; only clipped screen pixels are converted. */
            dx=v[i].x-ortho->centerX;
            dy=v[i].y-ortho->centerY;
            dz=v[i].z-ortho->centerZ;
            du=(dx*cy+dz*sy)/1024.0;
            dz1=(-dx*sy+dz*cy)/1024.0;
            orthoTransformed[i].x=du;
            orthoTransformed[i].y=(dy*cp-dz1*sp)/1024.0;
            orthoTransformed[i].z=(dy*sp+dz1*cp)/1024.0;
            continue;
        }
        x=v[i].x; y=v[i].y; z=v[i].z;
        rx=(x*cy+z*sy)/1024L; rz=(-x*sy+z*cy)/1024L;
        transformed[i].x=rx+c->translateX;
        transformed[i].y=(y*cp-rz*sp)/1024L+c->translateY;
        transformed[i].z=(y*sp+rz*cp)/1024L+c->translateZ+
            c->cameraDistance;
    }
    /* Conservative rotated components <=1200 (first stage <=800),
     * differences <=2400; cross <=11,520,000; L1 <=34,560,000;
     * light and 3*light <=207,360,000. Translation cancels in differences.
     * Rotation products <=1,228,800. No long exceeds signed 32 bits.
     * Facing/projection/screen area use double to avoid long product overflow. */
    for (i = 0; i < nf; ++i) {
        if (ortho) {
            p[0]=orthoTransformed[f[i].a];
            p[1]=orthoTransformed[f[i].b];
            p[2]=orthoTransformed[f[i].c];
            ux=p[1].x-p[0].x; uy=p[1].y-p[0].y; uz=p[1].z-p[0].z;
            vx=p[2].x-p[0].x; vy=p[2].y-p[0].y; vz=p[2].z-p[0].z;
            dnx=uy*vz-uz*vy; dny=uz*vx-ux*vz; dnz=ux*vy-uy*vx;
            if (c->backfaceCull && dnz <= 0.0) continue;
            if (dnz < 0.0) { dnx=-dnx; dny=-dny; dnz=-dnz; }
            dnorm=(dnx < 0.0 ? -dnx : dnx)+
                (dny < 0.0 ? -dny : dny)+(dnz < 0.0 ? -dnz : dnz);
            if (dnorm == 0.0) continue;
            dlight=-dnx+dny+2.0*dnz;
            if (dlight < 0.0) dlight=0.0;
            level=1+(int)(3.0*dlight/dnorm);
            if (level > 7) level=7;
            shade=(unsigned short)(f[i].material*8+level);
            n=3;
            for (j=0; j<n; ++j) {
                p[j].x=c->left+c->width/2.0+p[j].x*ortho->pixelsPerUnit;
                p[j].y=c->top+c->height/2.0-p[j].y*ortho->pixelsPerUnit;
            }
        } else {
            a=transformed[f[i].a]; b=transformed[f[i].b];
            d=transformed[f[i].c];
            normal(&a,&b,&d,&nx,&ny,&nz);
            norm=magnitude(nx)+magnitude(ny)+magnitude(nz);
            if (!norm) continue;
            facing=(double)nx*a.x+(double)ny*a.y+(double)nz*a.z;
            if (c->backfaceCull && facing >= 0.0) continue;
            if (facing > 0.0) { nx=-nx; ny=-ny; nz=-nz; }
            light=-nx+ny-2L*nz;
            if (light < 0) light=0;
            level=1+(int)((3L*light)/norm);
            if (level > 7) level=7;
            shade=(unsigned short)(f[i].material*8+level);
            p[0].x=a.x; p[0].y=a.y; p[0].z=a.z;
            p[1].x=b.x; p[1].y=b.y; p[1].z=b.z;
            p[2].x=d.x; p[2].y=d.y; p[2].z=d.z;
            n=clip(p,3,q,0,CAD_MESHVIEW_NEAR,1);
            for (j=0; j<n; ++j) {
                p[j].x=c->left+(c->width-1)/2.0+c->focal*q[j].x/q[j].z;
                p[j].y=c->top+(c->height-1)/2.0-c->focal*q[j].y/q[j].z;
                p[j].z=q[j].z;
            }
        }
        m=clip(p,n,q,1,c->left,ortho == NULL);
        n=clip(q,m,p,2,right,ortho == NULL);
        m=clip(p,n,q,3,c->top,ortho == NULL);
        n=clip(q,m,p,4,bottom,ortho == NULL);
        for (j=1; j+1<n; ++j) {
            /* Temporary slot independent of scratch capacity for collapsed
             * fan triangles at the resource boundary. */
            ViewTriangle candidate;
            tri=&candidate;
            for (k=0; k<3; ++k) {
                m=(k == 0 ? 0 : j+k-1);
                tri->x[k]=pixel(p[m].x,c->left,right);
                tri->y[k]=pixel(p[m].y,c->top,bottom);
            }
            area=((double)tri->x[1]-tri->x[0])*
                ((double)tri->y[2]-tri->y[0])-
                ((double)tri->y[1]-tri->y[0])*
                ((double)tri->x[2]-tri->x[0]);
            if (area == 0.0) continue;
            if (*count == CAD_MESHVIEW_MAX_TRIANGLES)
                return CAD_MESHVIEW_NO_ROOM;
            tri->depth=(p[0].z+p[j].z+p[j+1].z)/3.0;
            /* Negated orthographic depth reuses the descending stable sort:
             * +Z is toward its viewer, so smallest actual depth paints first. */
            if (ortho) tri->depth=-tri->depth;
            tri->shade=shade;
            triangles[*count]=*tri;
            order[*count]=(unsigned short)*count;
            ++*count;
        }
    }
    /* Stable insertion sort, <=523776 comparisons; move only 16-bit indices. */
    for (i=1; i<*count; ++i) {
        key=order[i]; j=i;
        while (j > 0 && triangles[order[j-1]].depth < triangles[key].depth) {
            order[j]=order[j-1]; --j;
        }
        order[j]=key;
    }
    return CAD_MESHVIEW_OK;
}

static CadMeshViewStatus render_mesh(const CadMeshViewVertex *vertices,
    int vertexCount, const CadMeshViewFace *faces, int faceCount,
    const CadMeshViewCamera *camera, const CadMeshViewSink *sink,
    const CadMeshViewOrtho *ortho)
{
    CadMeshViewStatus status;
    int count, i;
    ViewTriangle *t;
    if (rendering) return CAD_MESHVIEW_BUSY;
    rendering=1;
    status=prepare(vertices,vertexCount,faces,faceCount,camera,sink,ortho,&count);
    if (status == CAD_MESHVIEW_OK) {
        for (i=0; i<count; ++i) {
            t=&triangles[order[i]];
            if (!sink->triangle(sink->context,t->x[0],t->y[0],
                t->x[1],t->y[1],t->x[2],t->y[2],t->shade)) {
                status=CAD_MESHVIEW_SINK_FAILED;
                break;
            }
        }
    }
    rendering=0;
    return status;
}

CadMeshViewStatus cad_meshview_render(const CadMeshViewVertex *vertices,
    int vertexCount, const CadMeshViewFace *faces, int faceCount,
    const CadMeshViewCamera *camera, const CadMeshViewSink *sink)
{
    return render_mesh(vertices,vertexCount,faces,faceCount,camera,sink,NULL);
}

CadMeshViewStatus cad_meshview_render_orthographic(
    const CadMeshViewVertex *vertices, int vertexCount,
    const CadMeshViewFace *faces, int faceCount,
    const CadMeshViewCamera *camera, const CadMeshViewSink *sink,
    const CadMeshViewOrtho *ortho)
{
    if (!ortho) return rendering ? CAD_MESHVIEW_BUSY : CAD_MESHVIEW_INVALID;
    return render_mesh(vertices,vertexCount,faces,faceCount,camera,sink,ortho);
}
