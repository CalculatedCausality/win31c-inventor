#ifndef CAD_MESHVIEW_H
#define CAD_MESHVIEW_H

/* Faceted painter renderer, portable C89 (16-bit int, 32-bit long), no GDI,
 * allocation, model IDs or automatic fitting. Average-depth ordering is an
 * approximation, not general occlusion/hidden-line removal. */
#define CAD_MESHVIEW_MAX_VERTICES 256
#define CAD_MESHVIEW_MAX_FACES 512
#define CAD_MESHVIEW_MAX_TRIANGLES 1024
#define CAD_MESHVIEW_MAX_MATERIALS 8
#define CAD_MESHVIEW_NEAR 16

typedef struct { int x, y, z; } CadMeshViewVertex;
typedef struct {
    unsigned short a, b, c;
    unsigned short material;
} CadMeshViewFace;

typedef struct {
    /* One turn = 256 units; yaw wraps (including negative values), pitch
     * must be -64..64. Right-handed Y-up world, camera looks along +Z.
     * Yaw about Y then pitch about X; eye is (0,0,-cameraDistance).
     * World origin projects to viewport center with zero translation. */
    int yaw, pitch;
    int left, top, width, height;
    int cameraDistance;             /* 800..16000 */
    int focal;                      /* explicit pixels, 1..32767 */
    /* Camera-space translation AFTER rotation, each -16000..16000.
     * Zero is the stable world-origin baseline; useful also for near clips. */
    int translateX, translateY, translateZ;
    int backfaceCull;               /* exactly 0 or 1 */
} CadMeshViewCamera;

typedef struct {
    /* World-space center, subtracted BEFORE yaw/pitch. Zero centers are the
     * default; fractional values (e.g. bounds midpoints ending in .5) retain
     * precision. Each must be finite and within -16000..16000. */
    double centerX, centerY, centerZ;
    double pixelsPerUnit;           /* finite, >0 and <=8 */
} CadMeshViewOrtho;

typedef struct {
    void *context;
    /* Screen Y downward. shadeIndex = material*8 + level (0..63).
     * Nonzero continues; zero stops with SINK_FAILED and partial output.
     * A driver can fill or outline triangles; graph-source wireframe remains
     * the caller's separate mode. Empty meshes emit no callbacks; the sink
     * and its triangle function must still be supplied. */
    int (*triangle)(void *context, int x1, int y1, int x2, int y2,
                    int x3, int y3, unsigned short shadeIndex);
} CadMeshViewSink;

typedef enum {
    CAD_MESHVIEW_OK = 0, CAD_MESHVIEW_INVALID, CAD_MESHVIEW_RANGE,
    CAD_MESHVIEW_NO_ROOM, CAD_MESHVIEW_SINK_FAILED, CAD_MESHVIEW_BUSY
} CadMeshViewStatus;

/* Counts 0..limits above, vertices -400..400, indices in range, material
 * 0..7. A nonzero count requires its array. Repeated indices and zero-area
 * faces are INVALID, even if invisible. All inputs are validated and all
 * clipping prepared BEFORE callbacks. Invalid/range/no-room returns emit
 * nothing. Rounded zero-area screen triangles are silently omitted.
 * Viewport left/top >=0, width/height >0, inclusive last pixel <=32767.
 * Near clipping yields 0..4 vertices; viewport clipping may split further.
 * At most 1024 final triangles; excess returns NO_ROOM atomically.
 * Backface culling keeps outward normals pointing toward the eye. Two-sided
 * mode flips lighting on back faces. Camera-fixed light is (-1,+1,-2), with
 * deterministic L1-normalized buckets, levels 1..7 (ambient minimum 1).
 * Far-to-near average clipped depth, stable ties in input/fan order.
 * Arrays/sink must remain unchanged during calls. Static scratch <64KiB per
 * array, no heap, non-reentrant: recursive calls return BUSY. Not thread-safe.
 * No metadata output; callback arguments are values, not borrowed buffers. */
CadMeshViewStatus cad_meshview_render(const CadMeshViewVertex *vertices,
    int vertexCount, const CadMeshViewFace *faces, int faceCount,
    const CadMeshViewCamera *camera, const CadMeshViewSink *sink);

/* Orthographic CAD view, viewer on +Z looking toward origin. Same input,
 * sink, resource limits, statuses and atomic preparation as the perspective
 * API. ortho is required; initialize all its fields (zero center is valid).
 * camera remains fully validated, but cameraDistance, focal and translateX/Y/Z
 * have no projection effect. Only yaw/pitch, viewport and backfaceCull apply.
 * Q10 sine/cosine coefficients match genericCAD ToCamera; there is no integer
 * truncation between the rotations. For centered x,y,z:
 *   u=(x*cosYaw+z*sinYaw)/1024
 *   z1=(-x*sinYaw+z*cosYaw)/1024
 *   v=(y*cosPitch-z1*sinPitch)/1024
 *   depth=(y*sinPitch+z1*cosPitch)/1024
 * Screen=(left+width/2.0+u*pixelsPerUnit,
 *         top+height/2.0-v*pixelsPerUnit), rounded to nearest pixel and clipped
 * to inclusive viewport bounds. No near plane; signed/zero depths are valid.
 * Smallest average clipped depth is painted first; ties stay stable. Culling
 * keeps normals with nz>0; two-sided mode flips back normals for lighting.
 * Light=(-1,+1,+2), the viewer-side counterpart of the perspective light.
 * Both entry points share the same non-reentrant static scratch/ BUSY guard. */
CadMeshViewStatus cad_meshview_render_orthographic(
    const CadMeshViewVertex *vertices, int vertexCount,
    const CadMeshViewFace *faces, int faceCount,
    const CadMeshViewCamera *camera, const CadMeshViewSink *sink,
    const CadMeshViewOrtho *ortho);

#endif
