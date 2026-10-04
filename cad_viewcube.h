#ifndef CAD_VIEWCUBE_H
#define CAD_VIEWCUBE_H

/* Portable C89, including 16-bit int / 32-bit long, no Windows/GDI or libm.
 * Pure navigation geometry: these functions never access a CAD document.
 * Caller renders quads, labels, corner selectors and ring with its own GDI. */
typedef enum {
    CAD_CUBE_NONE = 0,
    CAD_CUBE_FRONT = 1, CAD_CUBE_BACK = 2,
    CAD_CUBE_TOP = 3, CAD_CUBE_BOTTOM = 4,
    CAD_CUBE_RIGHT = 5, CAD_CUBE_LEFT = 6,
    CAD_CUBE_ORBIT = 8,
    CAD_CUBE_CORNER_BASE = 16,
    CAD_CUBE_CORNER_LAST = 23
} CadCubeHit;

#define CAD_CUBE_MIN_SIZE 24
#define CAD_CUBE_MAX_SIZE 512
#define CAD_CUBE_CORNER_RADIUS 6
#define CAD_CUBE_MAX_QUADS 3

typedef struct { int x, y; } CadCubePoint;
typedef struct { int left, top, right, bottom; } CadCubeBounds;
typedef struct {
    CadCubePoint center;
    int innerRadius, outerRadius;
    CadCubeBounds bounds;           /* inclusive outer circle bounding box */
} CadCubeRing;
typedef struct {
    CadCubePoint points[4];         /* perimeter order; fill/outline polygon */
    int corners[4];                 /* indices into geometry.corners */
    CadCubeHit hit;
    const char *label;              /* static uppercase face name */
    CadCubePoint labelCenter;       /* polygon centroid, screen Y downward */
    CadCubeBounds bounds;           /* inclusive polygon bounds */
    long depth;                    /* signed Q20 face-center depth, eye +Z */
} CadCubeQuad;
typedef struct {
    int yaw, pitch;                 /* normalized to 0..255 */
    int size;
    CadCubeBounds bounds;           /* inclusive widget bounds */
    CadCubeBounds cubeBounds;       /* all eight projected vertices */
    CadCubePoint center;
    CadCubeRing outerOrbitRing;
    CadCubePoint corners[8];
    CadCubeHit cornerHits[8];       /* 16 + index, even for hidden vertices */
    int cornerVisible[8];          /* belongs to an emitted visible quad */
    long cornerDepth[8];            /* signed Q20, larger = nearer */
    int quadCount;                  /* 1..3, visible nondegenerate quads only */
    CadCubeQuad quads[CAD_CUBE_MAX_QUADS]; /* stable face-enum order */
} CadCubeGeometry;

/* Y-up unit cube, FRONT +Z, BACK -Z, RIGHT +X, LEFT -X, TOP +Y.
 * u=x*cosYaw+z*sinYaw; z1=-x*sinYaw+z*cosYaw;
 * v=y*cosPitch-z1*sinPitch; depth=y*sinPitch+z1*cosPitch.
 * Orthographic projection, viewer +Z, screen Y downward. Quarter-wave Q10
 * coefficients match cad.c; four coefficients are cached per build and no
 * intermediate rotation is truncated. Half-edge floor(size*22/100), giving
 * a roughly 60px isometric body in a 100px widget (44px cardinal face).
 * Ring radii ceil(size*40/100), floor(size*48/100), outside the cube.
 * Corner index bits: bit 0 = positive X, bit 1 = positive Y, bit 2 = positive Z.
 * All angles (including negative ones) wrap modulo 256. size is 24..512;
 * left/top and inclusive last pixel must be in -32768..32767 on every host.
 * Returns 1 on success, 0 for invalid arguments, preserving *geometry on
 * failure. No allocation or shared mutable cache; calls are reentrant. */
int CadCubeBuild(int yaw, int pitch, int left, int top, int size,
                 CadCubeGeometry *geometry);

/* Hit priority: nearest visible corner within Euclidean radius 6 (inclusive),
 * then visible convex quad including edges, then inclusive circular ring band.
 * Equal-distance corners choose greater depth, then lower index. Shared face
 * edges choose lower face enum. Hidden corners are never candidates. Outside
 * widget or NULL geometry returns NONE. Input geometry must be a successful
 * unmodified build. Coordinates may be any int, even host INT_MIN/INT_MAX. */
CadCubeHit cad_cube_hit(const CadCubeGeometry *geometry, int x, int y);

/* Returns 1 and writes both angles for faces/corners; invalid/NONE/ORBIT or
 * NULL output pointers return 0 preserving both outputs. Outputs must be
 * distinct. Faces request cardinal/orthographic navigation; corners request
 * isometric navigation. Caller decides projection mode and handles ORBIT
 * with its ordinary camera orbit drag (e.g. RightOrbit), not a model edit.
 * Faces: FRONT (0,0), BACK (128,0), RIGHT (192,0), LEFT (64,0),
 * TOP (0,64), BOTTOM (0,192). Corner viewer X/Z signs map to yaw
 * +/+ 224, -/+ 32, +/- 160, -/- 96; Y+ pitch25, Y- pitch231. */
int cad_cube_orientation(int hit, int *yaw, int *pitch);

#endif
