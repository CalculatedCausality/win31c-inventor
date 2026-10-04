#ifndef CAD_SKETCH_H
#define CAD_SKETCH_H

/* Portable C89, including 16-bit int / 32-bit long implementations.
 * No allocation, floating point, Windows types, or external dependencies.
 * Calls use shared bounded scratch (< 64 KiB) and are NOT reentrant.
 * Input/output objects must not alias the implementation's private scratch.
 */
#define CAD_SKETCH_MAX_POINTS 64
#define CAD_SKETCH_MAX_VERTICES 128
#define CAD_SKETCH_MAX_TRIANGLES 252
#define CAD_SKETCH_LIMIT 400

typedef struct CadSketchPoint { int x, y; } CadSketchPoint;
typedef struct CadSketchVertex { int x, y, z; } CadSketchVertex;
typedef struct CadSketchTriangle { unsigned short a, b, c; } CadSketchTriangle;

typedef enum CadSketchStatus {
    CAD_SKETCH_OK = 0,
    CAD_SKETCH_ARGUMENT,
    CAD_SKETCH_COMPLEXITY,
    CAD_SKETCH_RANGE,
    CAD_SKETCH_ZERO_EDGE,
    CAD_SKETCH_DUPLICATE,
    CAD_SKETCH_INTERSECTION,
    CAD_SKETCH_ZERO_AREA,
    CAD_SKETCH_TRIANGULATION,
    CAD_SKETCH_NOT_CLOSED,
    CAD_SKETCH_TOPOLOGY,
    CAD_SKETCH_DEGENERATE
} CadSketchStatus;

typedef enum CadSketchMeshKind {
    CAD_SKETCH_EMPTY = 0,
    CAD_SKETCH_WIREFRAME,
    CAD_SKETCH_CLOSED_MESH
} CadSketchMeshKind;

typedef struct CadSketchMesh {
    CadSketchMeshKind kind;
    unsigned short vertex_count, triangle_count;
    CadSketchVertex vertices[CAD_SKETCH_MAX_VERTICES];
    CadSketchTriangle triangles[CAD_SKETCH_MAX_TRIANGLES];
} CadSketchMesh;

/* An implicit closing edge joins last to first; do not repeat the first point.
 * Rejects zero edges, any duplicate points, adjacent backtracking/overlap,
 * nonadjacent crossings, overlaps and endpoint contacts, and zero area.
 * Straight-through collinear vertices are allowed and retained.
 * signed_area2 receives twice the signed XY area (CCW positive), only on OK.
 */
CadSketchStatus cad_sketch_validate(const CadSketchPoint *points, int count,
                                  long *signed_area2);

/* Base Z=0, positive height 1..400. Normalizes CW/CCW input to outward
 * winding, triangulates both caps and sides, retains all input vertices.
 * Exactly 2*n vertices and 4*n-4 triangles. All failure paths preserve *out.
 * This construction proves a simple closed extrusion, not arbitrary solids.
 */
CadSketchStatus cad_sketch_extrude(const CadSketchPoint *points, int count,
                                 int height, CadSketchMesh *out);

/* Checks range, nondegenerate triangles, opposite paired edge incidence,
 * connected surface, no unused vertices, and connected vertex links.
 * CLOSED_MESH is a combinatorial status: this does NOT test arbitrary mesh
 * self-intersections, nesting, outward winding, or geometric solid validity.
 * A wireframe never qualifies as a closed triangle mesh.
 */
CadSketchStatus cad_sketch_mesh_validate(const CadSketchMesh *mesh);

/* Six times algebraic signed volume, via the Z divergence sum. Validates
 * topology first; preserves output on failure. Checks each product and sum
 * against signed 32-bit limits, even on hosts with wider long. A general
 * mesh can fail RANGE through intermediate cancellation. For this module's
 * extrusions all terms are nonnegative and total <= 1,536,000,000; volume
 * <= 256,000,000. General mesh algebraic volume is not a solid guarantee.
 */
CadSketchStatus cad_sketch_mesh_volume6(const CadSketchMesh *mesh,
                                      long *signed_volume6);

#endif
