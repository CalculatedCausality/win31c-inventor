#include "../cad_meshview.h"
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    CadMeshViewCamera camera;
    int calls, failAt, recurse;
    int minX, maxX, minY, maxY;
    unsigned short shades[CAD_MESHVIEW_MAX_TRIANGLES];
    int xy[CAD_MESHVIEW_MAX_TRIANGLES][6];
} Capture;
static Capture capture, saved;
static CadMeshViewVertex vertices[CAD_MESHVIEW_MAX_VERTICES];
static CadMeshViewFace faces[CAD_MESHVIEW_MAX_FACES];

static CadMeshViewCamera standard_camera(void)
{
    CadMeshViewCamera c;
    memset(&c,0,sizeof(c));
    c.left=10; c.top=20; c.width=601; c.height=401;
    c.cameraDistance=900; c.focal=400;
    return c;
}

static int triangle(void *context, int x1, int y1, int x2, int y2,
                    int x3, int y3, unsigned short shade)
{
    Capture *r;
    int x[3], y[3], i, n;
    double area;
    CadMeshViewSink sink;
    CadMeshViewOrtho ortho;
    r=(Capture *)context; n=r->calls++;
    assert(n < CAD_MESHVIEW_MAX_TRIANGLES);
    x[0]=x1; x[1]=x2; x[2]=x3;
    y[0]=y1; y[1]=y2; y[2]=y3;
    for (i=0; i<3; ++i) {
        assert(x[i] >= r->camera.left);
        assert(x[i] <= (long)r->camera.left+r->camera.width-1);
        assert(y[i] >= r->camera.top);
        assert(y[i] <= (long)r->camera.top+r->camera.height-1);
        if (x[i]<r->minX) r->minX=x[i];
        if (x[i]>r->maxX) r->maxX=x[i];
        if (y[i]<r->minY) r->minY=y[i];
        if (y[i]>r->maxY) r->maxY=y[i];
        r->xy[n][2*i]=x[i]; r->xy[n][2*i+1]=y[i];
    }
    area=((double)x2-x1)*((double)y3-y1)-
         ((double)y2-y1)*((double)x3-x1);
    assert(area != 0.0);
    assert(shade < 64 && shade%8 >= 1);
    r->shades[n]=shade;
    if (r->recurse) {
        sink.context=r; sink.triangle=triangle;
        assert(cad_meshview_render(NULL,0,NULL,0,&r->camera,&sink)==
               CAD_MESHVIEW_BUSY);
        memset(&ortho,0,sizeof(ortho)); ortho.pixelsPerUnit=0.4;
        assert(cad_meshview_render_orthographic(NULL,0,NULL,0,
            &r->camera,&sink,&ortho)==CAD_MESHVIEW_BUSY);
    }
    return r->failAt != r->calls;
}

static void reset(CadMeshViewCamera c)
{
    memset(&capture,0,sizeof(capture)); capture.camera=c;
    capture.minX=capture.minY=32767;
}

static CadMeshViewStatus render(int nv, int nf)
{
    CadMeshViewSink sink;
    sink.context=&capture; sink.triangle=triangle;
    return cad_meshview_render(vertices,nv,faces,nf,&capture.camera,&sink);
}

static CadMeshViewStatus render_ortho(int nv, int nf,
                                    const CadMeshViewOrtho *ortho)
{
    CadMeshViewSink sink;
    sink.context=&capture; sink.triangle=triangle;
    return cad_meshview_render_orthographic(vertices,nv,faces,nf,
        &capture.camera,&sink,ortho);
}

static CadMeshViewOrtho standard_ortho(void)
{
    CadMeshViewOrtho ortho;
    memset(&ortho,0,sizeof(ortho)); ortho.pixelsPerUnit=0.4;
    return ortho;
}

static void fixture(void)
{
    vertices[0].x=-200; vertices[0].y=-100; vertices[0].z=-100;
    vertices[1].x=200; vertices[1].y=-100; vertices[1].z=-100;
    vertices[2].x=0; vertices[2].y=100; vertices[2].z=-100;
    faces[0].a=0; faces[0].b=2; faces[0].c=1; faces[0].material=0;
}

static void projection_and_rotation(void)
{
    CadMeshViewCamera c;
    int width;
    fixture(); c=standard_camera(); reset(c);
    assert(render(3,1)==CAD_MESHVIEW_OK && capture.calls==1);
    assert(capture.minX==210 && capture.maxX==410);
    assert(capture.minY==170 && capture.maxY==270);
    assert(capture.shades[0]==7);
    saved=capture;
    reset(c); assert(render(3,1)==CAD_MESHVIEW_OK);
    assert(memcmp(saved.xy,capture.xy,sizeof(saved.xy))==0);
    c.yaw=32; reset(c); assert(render(3,1)==CAD_MESHVIEW_OK);
    width=capture.maxX-capture.minX;
    assert(width > 0 && width < saved.maxX-saved.minX);
    assert(capture.minX != saved.minX);
    saved=capture; c.yaw=-224; reset(c);
    assert(render(3,1)==CAD_MESHVIEW_OK);
    assert(memcmp(saved.xy,capture.xy,sizeof(saved.xy))==0);
    c.yaw=0; c.pitch=32; reset(c);
    assert(render(3,1)==CAD_MESHVIEW_OK && capture.calls==1);
    assert(capture.minY != 170 && capture.maxY-capture.minY < 100);
    c.pitch=0; c.translateX=100; reset(c);
    assert(render(3,1)==CAD_MESHVIEW_OK && capture.minX==260);
}

static void depth_culling_materials(void)
{
    CadMeshViewCamera c;
    int i;
    fixture();
    for (i=0; i<3; ++i) {
        vertices[i+3]=vertices[i]; vertices[i+3].z=100;
    }
    faces[1]=faces[0]; faces[1].a+=3; faces[1].b+=3; faces[1].c+=3;
    faces[1].material=7;
    c=standard_camera(); c.backfaceCull=1; reset(c);
    assert(render(6,2)==CAD_MESHVIEW_OK && capture.calls==2);
    assert(capture.shades[0]/8==7 && capture.shades[1]/8==0);
    assert(capture.xy[0][0] > capture.xy[1][0]); /* far face is smaller */
    faces[1].a=3; faces[1].b=4; faces[1].c=5;
    reset(c); assert(render(6,2)==CAD_MESHVIEW_OK && capture.calls==1);
    c.backfaceCull=0; reset(c);
    assert(render(6,2)==CAD_MESHVIEW_OK && capture.calls==2);
    assert(capture.shades[0]==63);
    /* Stable same-depth ordering and every material's separate palette bank. */
    for (i=0; i<8; ++i) { faces[i]=faces[0]; faces[i].material=(unsigned short)i; }
    reset(c); assert(render(3,8)==CAD_MESHVIEW_OK && capture.calls==8);
    for (i=0; i<8; ++i) assert(capture.shades[i]==i*8+7);
    c.backfaceCull=1; c.yaw=128; reset(c);
    assert(render(3,1)==CAD_MESHVIEW_OK && capture.calls==0);
}

static void clipping_and_limits(void)
{
    CadMeshViewCamera c;
    int i;
    c=standard_camera(); c.focal=20; c.translateZ=-900;
    vertices[0].x=-20; vertices[0].y=-20; vertices[0].z=0;
    vertices[1].x=20; vertices[1].y=-20; vertices[1].z=80;
    vertices[2].x=0; vertices[2].y=20; vertices[2].z=80;
    faces[0].a=0; faces[0].b=1; faces[0].c=2; faces[0].material=3;
    reset(c); assert(render(3,1)==CAD_MESHVIEW_OK && capture.calls==2);
    vertices[1].z=0; reset(c);
    assert(render(3,1)==CAD_MESHVIEW_OK && capture.calls==1);
    vertices[2].z=0; reset(c);
    assert(render(3,1)==CAD_MESHVIEW_OK && capture.calls==0);
    vertices[0].z=vertices[1].z=vertices[2].z=16;
    reset(c); assert(render(3,1)==CAD_MESHVIEW_OK && capture.calls==1);
    vertices[0].z=16; vertices[1].z=0; vertices[2].z=80;
    reset(c); assert(render(3,1)==CAD_MESHVIEW_OK && capture.calls==1);
    /* Convex viewport pentagon -> three nondegenerate fan triangles. */
    c=standard_camera(); c.left=c.top=0; c.width=c.height=101; c.focal=900;
    vertices[0].x=-400; vertices[0].y=-400; vertices[0].z=0;
    vertices[1].x=400; vertices[1].y=-400; vertices[1].z=0;
    vertices[2].x=0; vertices[2].y=40; vertices[2].z=0;
    reset(c); assert(render(3,1)==CAD_MESHVIEW_OK && capture.calls==3);
    assert(capture.minX==0 && capture.maxX==100 && capture.maxY==100);
    for (i=1; i<CAD_MESHVIEW_MAX_FACES; ++i) faces[i]=faces[0];
    reset(c); assert(render(3,342)==CAD_MESHVIEW_NO_ROOM && capture.calls==0);
    reset(c); assert(render(3,341)==CAD_MESHVIEW_OK && capture.calls==1023);
    /* Full viewport rectangle -> two triangles, exactly full resource array. */
    vertices[2].y=400;
    reset(c); assert(render(3,512)==CAD_MESHVIEW_OK && capture.calls==1024);
    c.translateX=16000; reset(c);
    assert(render(3,512)==CAD_MESHVIEW_OK && capture.calls==0);
    fixture(); c=standard_camera(); c.left=32766; c.top=32766;
    c.width=c.height=2; c.focal=32767; reset(c);
    assert(render(3,1)==CAD_MESHVIEW_OK);
    c.width=c.height=1; reset(c);
    assert(render(3,1)==CAD_MESHVIEW_OK && capture.calls==0);
    /* Every rotation extremum and maximum coordinate/depth ranges. */
    vertices[0].x=-400; vertices[0].y=-400; vertices[0].z=-400;
    vertices[1].x=400; vertices[1].y=-400; vertices[1].z=400;
    vertices[2].x=400; vertices[2].y=400; vertices[2].z=-400;
    c=standard_camera(); c.cameraDistance=16000;
    c.translateX=16000; c.translateY=-16000; c.translateZ=16000;
    c.focal=32767;
    for (i=0; i<256; ++i) {
        c.yaw=i; c.pitch=(i%129)-64; reset(c);
        assert(render(3,1)==CAD_MESHVIEW_OK);
    }
}

static void errors_and_callbacks(void)
{
    CadMeshViewCamera c;
    CadMeshViewSink sink;
    int i;
    fixture(); c=standard_camera(); reset(c);
    sink.context=&capture; sink.triangle=triangle;
    assert(cad_meshview_render(NULL,0,NULL,0,&c,&sink)==CAD_MESHVIEW_OK);
    assert(cad_meshview_render(NULL,3,faces,1,&c,&sink)==CAD_MESHVIEW_INVALID);
    assert(cad_meshview_render(vertices,3,NULL,1,&c,&sink)==CAD_MESHVIEW_INVALID);
    assert(cad_meshview_render(vertices,3,faces,1,NULL,&sink)==CAD_MESHVIEW_INVALID);
    assert(cad_meshview_render(vertices,3,faces,1,&c,NULL)==CAD_MESHVIEW_INVALID);
    sink.triangle=NULL;
    assert(cad_meshview_render(vertices,3,faces,1,&c,&sink)==CAD_MESHVIEW_INVALID);
    assert(render(-1,1)==CAD_MESHVIEW_INVALID);
    assert(render(257,1)==CAD_MESHVIEW_RANGE);
    assert(render(3,513)==CAD_MESHVIEW_RANGE);
    faces[1]=faces[0]; faces[1].a=3;
    assert(render(3,2)==CAD_MESHVIEW_INVALID && capture.calls==0);
    faces[1]=faces[0]; faces[1].material=8;
    assert(render(3,2)==CAD_MESHVIEW_RANGE && capture.calls==0);
    faces[1]=faces[0]; faces[1].a=faces[1].b;
    assert(render(3,2)==CAD_MESHVIEW_INVALID && capture.calls==0);
    vertices[2].x=401;
    assert(render(3,1)==CAD_MESHVIEW_RANGE && capture.calls==0);
    fixture(); vertices[2]=vertices[1];
    assert(render(3,1)==CAD_MESHVIEW_INVALID && capture.calls==0);
    fixture();
    for (i=0; i<14; ++i) {
        c=standard_camera();
        switch (i) {
        case 0: c.left=-1; break;
        case 1: c.top=-1; break;
        case 2: c.width=0; break;
        case 3: c.height=-1; break;
        case 4: c.left=32767; break;
        case 5: c.pitch=65; break;
        case 6: c.pitch=-65; break;
        case 7: c.focal=0; break;
        case 8: c.focal=-1; break;
        case 9: c.cameraDistance=799; break;
        case 10: c.cameraDistance=16001; break;
        case 11: c.translateZ=-16001; break;
        case 12: c.translateX=16001; break;
        default: c.backfaceCull=2; break;
        }
        reset(c); assert(render(3,1)==CAD_MESHVIEW_RANGE && capture.calls==0);
    }
    c=standard_camera(); c.yaw=INT_MIN; reset(c);
    assert(render(3,1)==CAD_MESHVIEW_OK);
    c.yaw=INT_MAX; reset(c); assert(render(3,1)==CAD_MESHVIEW_OK);
    c=standard_camera(); reset(c); capture.failAt=1;
    assert(render(3,1)==CAD_MESHVIEW_SINK_FAILED && capture.calls==1);
    faces[1]=faces[0]; reset(c); capture.failAt=2;
    assert(render(3,2)==CAD_MESHVIEW_SINK_FAILED && capture.calls==2);
    reset(c); capture.recurse=1;
    assert(render(3,1)==CAD_MESHVIEW_OK && capture.calls==1);
    for (i=3; i<256; ++i) vertices[i]=vertices[0];
    for (i=1; i<512; ++i) faces[i]=faces[0];
    reset(c); assert(render(256,512)==CAD_MESHVIEW_OK && capture.calls==512);
#if INT_MAX > 32767
    c=standard_camera(); c.left=INT_MAX; c.width=INT_MAX; reset(c);
    assert(render(3,1)==CAD_MESHVIEW_RANGE && capture.calls==0);
    c=standard_camera(); c.focal=32768; reset(c);
    assert(render(3,1)==CAD_MESHVIEW_RANGE && capture.calls==0);
#endif
}

/* Reproducible stress across near/viewport boundaries and all view angles.
 * Callback checks enforce bounds and nondegenerate integer output even for
 * nearly edge-on and heavily clipped triangles. No platform rand()/libm. */
static unsigned long randomState=1;
static int sample(int limit)
{
    randomState=(randomState*25173UL+13849UL)&65535UL;
    return (int)(randomState%(unsigned long)limit);
}

static void clipping_stress(void)
{
    CadMeshViewCamera c;
    CadMeshViewStatus status;
    int i, j;
    for (i=0; i<4096; ++i) {
        c=standard_camera();
        c.yaw=sample(1024)-512; c.pitch=sample(129)-64;
        c.translateX=sample(3201)-1600;
        c.translateY=sample(3201)-1600;
        c.translateZ=sample(2401)-1600;
        c.focal=sample(32767)+1;
        c.width=sample(601)+1; c.height=sample(401)+1;
        c.backfaceCull=sample(2);
        for (j=0; j<3; ++j) {
            vertices[j].x=sample(801)-400;
            vertices[j].y=sample(801)-400;
            vertices[j].z=sample(801)-400;
        }
        faces[0].a=0; faces[0].b=1; faces[0].c=2;
        faces[0].material=(unsigned short)sample(8);
        reset(c); status=render(3,1);
        assert(status==CAD_MESHVIEW_OK || status==CAD_MESHVIEW_INVALID);
        assert(capture.calls<=6);
    }
    /* Valid model area but subpixel output is omitted. */
    fixture(); c=standard_camera(); c.focal=1;
    vertices[0].x=vertices[0].y=0;
    vertices[1].x=1; vertices[1].y=0;
    vertices[2].x=0; vertices[2].y=1;
    reset(c); assert(render(3,1)==CAD_MESHVIEW_OK && capture.calls==0);
}

static void orthographic_positions(void)
{
    /* Independent analytic ToCamera fixtures: yaw, pitch, Q10 sy/cy/sp/cp.
     * Last case exercises entries where the legacy UI table differs from
     * the original perspective table. Geometry is not edge-on at cardinals. */
    static const int angles[][6]={
        {0,0,0,1024,0,1024}, {64,0,1024,0,0,1024},
        {128,0,0,-1024,0,1024}, {192,0,-1024,0,0,1024},
        {0,64,0,1024,1024,0}, {0,-64,0,1024,-1024,0},
        {32,24,724,724,569,851}, {19,39,460,915,836,590}
    };
    CadMeshViewCamera c;
    CadMeshViewOrtho ortho;
    double x,y,z,u,v,z1,sx,sy;
    int i,j,ex,ey;
    vertices[0].x=-120; vertices[0].y=-80; vertices[0].z=-90;
    vertices[1].x=160; vertices[1].y=-40; vertices[1].z=100;
    vertices[2].x=20; vertices[2].y=140; vertices[2].z=30;
    faces[0].a=0; faces[0].b=1; faces[0].c=2; faces[0].material=2;
    ortho=standard_ortho();
    ortho.centerX=20.5; ortho.centerY=-15.5; ortho.centerZ=7.5;
    c=standard_camera();
    for (i=0; i<(int)(sizeof(angles)/sizeof(angles[0])); ++i) {
        c.yaw=angles[i][0]; c.pitch=angles[i][1]; reset(c);
        assert(render_ortho(3,1,&ortho)==CAD_MESHVIEW_OK && capture.calls==1);
        for (j=0; j<3; ++j) {
            x=vertices[j].x-ortho.centerX;
            y=vertices[j].y-ortho.centerY;
            z=vertices[j].z-ortho.centerZ;
            u=(x*angles[i][3]+z*angles[i][2])/1024.0;
            z1=(-x*angles[i][2]+z*angles[i][3])/1024.0;
            v=(y*angles[i][5]-z1*angles[i][4])/1024.0;
            sx=c.left+c.width/2.0+u*ortho.pixelsPerUnit;
            sy=c.top+c.height/2.0-v*ortho.pixelsPerUnit;
            ex=(int)(sx+0.5); ey=(int)(sy+0.5);
            assert(capture.xy[0][2*j]==ex && capture.xy[0][2*j+1]==ey);
            /* UI truncates positive screen coordinates; tolerance <=1 pixel. */
            assert(abs(capture.xy[0][2*j]-(int)sx)<=1);
            assert(abs(capture.xy[0][2*j+1]-(int)sy)<=1);
        }
    }
    saved=capture;
    c.translateX=16000; c.translateY=-16000; c.translateZ=-16000;
    c.cameraDistance=16000; c.focal=1; reset(c);
    assert(render_ortho(3,1,&ortho)==CAD_MESHVIEW_OK && capture.calls==1);
    assert(memcmp(saved.xy,capture.xy,sizeof(saved.xy))==0);
    assert(memcmp(saved.shades,capture.shades,sizeof(saved.shades))==0);
    ortho.pixelsPerUnit=0.5; reset(c);
    assert(render_ortho(3,1,&ortho)==CAD_MESHVIEW_OK && capture.calls==1);
    assert(capture.maxX-capture.minX > saved.maxX-saved.minX);
}

static void orthographic_visibility(void)
{
    CadMeshViewCamera c;
    CadMeshViewOrtho ortho;
    int i;
    fixture(); faces[0].b=1; faces[0].c=2; /* outward +Z */
    for (i=0; i<3; ++i) {
        vertices[i+3]=vertices[i]; vertices[i+3].z=100;
    }
    faces[1]=faces[0];
    faces[0].a=3; faces[0].b=4; faces[0].c=5; faces[0].material=7;
    c=standard_camera(); c.backfaceCull=1; ortho=standard_ortho();
    reset(c); assert(render_ortho(6,2,&ortho)==CAD_MESHVIEW_OK && capture.calls==2);
    assert(capture.shades[0]==7 && capture.shades[1]==63); /* -Z before +Z */
    assert(memcmp(capture.xy[0],capture.xy[1],sizeof(capture.xy[0]))==0);
    faces[1].b=2; faces[1].c=1; /* outward -Z */
    reset(c); assert(render_ortho(6,2,&ortho)==CAD_MESHVIEW_OK && capture.calls==1);
    assert(capture.shades[0]==63);
    c.backfaceCull=0; reset(c);
    assert(render_ortho(6,2,&ortho)==CAD_MESHVIEW_OK && capture.calls==2);
    assert(capture.shades[0]==7 && capture.shades[1]==63);
    c.yaw=128; c.backfaceCull=1; reset(c);
    assert(render_ortho(6,2,&ortho)==CAD_MESHVIEW_OK && capture.calls==1);
    assert(capture.shades[0]/8==0); /* opposite winding is now visible */
    /* Two perpendicular visible faces with different known light buckets. */
    fixture(); faces[0].b=1; faces[0].c=2;
    vertices[3].x=0; vertices[3].y=-100; vertices[3].z=-100;
    vertices[4].x=0; vertices[4].y=-100; vertices[4].z=100;
    vertices[5].x=0; vertices[5].y=100; vertices[5].z=0;
    faces[1].a=3; faces[1].b=4; faces[1].c=5; faces[1].material=0;
    c.yaw=32; reset(c);
    assert(render_ortho(6,2,&ortho)==CAD_MESHVIEW_OK && capture.calls==2);
    assert((capture.shades[0]==2 && capture.shades[1]==5) ||
           (capture.shades[0]==5 && capture.shades[1]==2));
    /* Equal depth remains stable in input order across all palette banks. */
    c.yaw=0;
    for (i=0; i<8; ++i) { faces[i]=faces[0]; faces[i].material=(unsigned short)i; }
    reset(c); assert(render_ortho(3,8,&ortho)==CAD_MESHVIEW_OK && capture.calls==8);
    for (i=0; i<8; ++i) assert(capture.shades[i]==i*8+7);
}

static void orthographic_clipping_errors(void)
{
    CadMeshViewCamera c;
    CadMeshViewOrtho ortho, bad;
    double nanValue;
    int i;
    c=standard_camera(); c.left=c.top=0; c.width=c.height=100;
    ortho=standard_ortho(); ortho.pixelsPerUnit=1.0;
    vertices[0].x=-400; vertices[0].y=-400; vertices[0].z=-300;
    vertices[1].x=400; vertices[1].y=-400; vertices[1].z=0;
    vertices[2].x=0; vertices[2].y=40; vertices[2].z=400;
    faces[0].a=0; faces[0].b=1; faces[0].c=2; faces[0].material=0;
    /* Clipping through signed/zero depth must use linear depth, not 1/z. */
    reset(c); assert(render_ortho(3,1,&ortho)==CAD_MESHVIEW_OK && capture.calls==3);
    assert(capture.minX==0 && capture.maxX==99 && capture.maxY==99);
    /* Depth z=x on the sloping face is -50..49 after clipping. A constant
     * +60 face must paint after ALL its fans, despite being first in input.
     * This also catches accidental perspective reciprocal-depth clipping. */
    vertices[0].z=-400; vertices[1].z=400; vertices[2].z=0;
    for (i=0; i<3; ++i) { vertices[i+3]=vertices[i]; vertices[i+3].z=60; }
    faces[1]=faces[0]; faces[0].a=3; faces[0].b=4; faces[0].c=5;
    faces[0].material=7;
    reset(c); assert(render_ortho(6,2,&ortho)==CAD_MESHVIEW_OK && capture.calls==6);
    for (i=0; i<3; ++i) assert(capture.shades[i]/8==0);
    for (i=3; i<6; ++i) assert(capture.shades[i]/8==7);
    faces[0]=faces[1];
    for (i=1; i<512; ++i) faces[i]=faces[0];
    reset(c); assert(render_ortho(3,342,&ortho)==CAD_MESHVIEW_NO_ROOM && capture.calls==0);
    vertices[2].y=400; reset(c);
    assert(render_ortho(3,512,&ortho)==CAD_MESHVIEW_OK && capture.calls==1024);
    fixture(); reset(c); ortho.pixelsPerUnit=0.1;
    capture.failAt=1;
    assert(render_ortho(3,1,&ortho)==CAD_MESHVIEW_SINK_FAILED && capture.calls==1);
    reset(c); capture.recurse=1;
    assert(render_ortho(3,1,&ortho)==CAD_MESHVIEW_OK && capture.calls==1);
    reset(c); assert(render_ortho(3,1,NULL)==CAD_MESHVIEW_INVALID && capture.calls==0);
    for (i=0; i<8; ++i) {
        bad=ortho;
        switch (i) {
        case 0: bad.pixelsPerUnit=0.0; break;
        case 1: bad.pixelsPerUnit=-0.4; break;
        case 2: bad.pixelsPerUnit=8.001; break;
        case 3: bad.centerX=16000.5; break;
        case 4: bad.centerY=-16000.5; break;
        case 5: bad.centerZ=HUGE_VAL; break;
        case 6: bad.pixelsPerUnit=HUGE_VAL; break;
        default: bad.centerX=-HUGE_VAL; break;
        }
        reset(c); assert(render_ortho(3,1,&bad)==CAD_MESHVIEW_RANGE && capture.calls==0);
    }
    nanValue=strtod("nan",NULL); /* C89 libs without NaN text support skip this. */
    if (nanValue != nanValue) {
        bad=ortho; bad.centerX=nanValue; reset(c);
        assert(render_ortho(3,1,&bad)==CAD_MESHVIEW_RANGE && capture.calls==0);
        bad=ortho; bad.pixelsPerUnit=nanValue; reset(c);
        assert(render_ortho(3,1,&bad)==CAD_MESHVIEW_RANGE && capture.calls==0);
    }
    ortho.centerX=-16000; ortho.centerY=16000; ortho.centerZ=-16000;
    ortho.pixelsPerUnit=8.0; reset(c);
    assert(render_ortho(3,1,&ortho)==CAD_MESHVIEW_OK && capture.calls==0);
    /* Shared geometry/camera validation also precedes orthographic callbacks. */
    ortho=standard_ortho(); c.left=-1; reset(c);
    assert(render_ortho(3,1,&ortho)==CAD_MESHVIEW_RANGE && capture.calls==0);
    c=standard_camera(); reset(c); faces[1]=faces[0]; faces[1].material=8;
    assert(render_ortho(3,2,&ortho)==CAD_MESHVIEW_RANGE && capture.calls==0);
    faces[1]=faces[0]; faces[1].c=3;
    assert(render_ortho(3,2,&ortho)==CAD_MESHVIEW_INVALID && capture.calls==0);
    c.cameraDistance=0; reset(c);
    assert(render_ortho(3,1,&ortho)==CAD_MESHVIEW_RANGE && capture.calls==0);
}

int main(void)
{
    projection_and_rotation();
    depth_culling_materials();
    clipping_and_limits();
    errors_and_callbacks();
    clipping_stress();
    orthographic_positions();
    orthographic_visibility();
    orthographic_clipping_errors();
    puts("meshview tests passed");
    return 0;
}
