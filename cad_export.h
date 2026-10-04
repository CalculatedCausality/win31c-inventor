#ifndef CAD_EXPORT_H
#define CAD_EXPORT_H

#define CAD_EXPORT_MAX_VERTICES 256
#define CAD_EXPORT_MAX_EDGES 512
#define CAD_EXPORT_COORD_LIMIT 400
#define CAD_EXPORT_MAX_PATH 127

#define CAD_EXPORT_WALL 0
#define CAD_EXPORT_ROOF 1
#define CAD_EXPORT_DOOR 2
#define CAD_EXPORT_WINDOW 3
#define CAD_EXPORT_CHIMNEY 4

#define CAD_EXPORT_Y_UP 0
#define CAD_EXPORT_Z_UP 1
#define CAD_EXPORT_DXF 0
#define CAD_EXPORT_OBJ 1

#define CAD_EXPORT_OK 0
#define CAD_EXPORT_INVALID 1
#define CAD_EXPORT_EXISTS 2
#define CAD_EXPORT_IO 3
#define CAD_EXPORT_CLEANUP 4

typedef struct CadExportVertex {
    int x, y, z;
} CadExportVertex;

typedef struct CadExportEdge {
    int a, b;                 /* Zero-based vertex indices. */
    int style;                /* CAD_EXPORT_WALL through CHIMNEY. */
} CadExportEdge;

typedef struct CadExportOptions {
    int axis;                 /* Y_UP: x,y,z; Z_UP: x,-z,y. */
    long scale_numerator;
    long scale_denominator;
} CadExportOptions;

/* Standalone model-space wireframe export; no camera or house dependency.
 * DXF is ASCII R12 (AC1009), 3D LINE entities on five named layers.
 * OBJ uses v/l records, 1-based indices and the same five named groups:
 * wall, roof, door, window, chimney. This is not a surface/solid exporter.
 * Units are unspecified model units, explicitly commented in both formats.
 * Optional positive rational scale is METADATA ONLY, not coordinate scaling
 * or a claim about physical units. 0/0 omits it; mixed zero/negative invalid.
 * NULL options means native Y-up, no scale metadata.
 * Counts are explicit, 0..256 / 0..512; NULL arrays only for zero counts.
 * Coordinates must be in [-400,400]; all endpoints/styles are validated.
 * Self edges and repeated edges are permitted and emitted in input order.
 *
 * Paths: nonempty, <=127 bytes, DOS 8.3 basename (letters/digits/_/-),
 * optional 1..3 letter/digit/_/- extension; DOS devices are rejected.
 * All validation precedes opening. Exclusive create refuses existing files
 * (also directories/symlinks on POSIX); no overwrite or temporary sidecars.
 * Uses C89 syntax and POSIX/Watcom open(O_EXCL), fdopen, close adaptations.
 * Every write, stream error and close is checked. Failed new output is
 * removed; CLEANUP means removal failed and an incomplete file may remain.
 * No durability/atomic-publication promise: a new file is visible while
 * streaming. Caller must not mutate input arrays or the destination path
 * during the call. Constant auxiliary memory; no global mutable state.
 * Win16 small model uses ordinary near pointers; caller supplies near data.
 */
int CadExportWrite(const char *path, int format,
                   const CadExportVertex *vertices, int vertex_count,
                   const CadExportEdge *edges, int edge_count,
                   const CadExportOptions *options);

#endif
