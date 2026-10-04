#ifndef CAD_AIRCRAFT_H
#define CAD_AIRCRAFT_H

#include "cad_model.h"
#include "cad_export.h"

#define CAD_AIRCRAFT_MAX_VERTICES 240
#define CAD_AIRCRAFT_MAX_EDGES 512
#define CAD_AIRCRAFT_MAX_TRIANGLES 512
#define CAD_AIRCRAFT_MAX_COMPONENTS 14
#define CAD_AIRCRAFT_MATERIAL_COUNT 5

typedef enum CadAircraftMaterial {
    CAD_AIRCRAFT_BODY = 0, CAD_AIRCRAFT_WING, CAD_AIRCRAFT_TAIL,
    CAD_AIRCRAFT_ENGINE, CAD_AIRCRAFT_CABIN_GLASS
} CadAircraftMaterial;

/* Unsigned short is the portable 16-bit WORD, without windows.h. */
typedef struct CadAircraftTriangle {
    unsigned short a, b, c, material;
} CadAircraftTriangle;

typedef struct CadAircraftColor {
    unsigned char red, green, blue;
} CadAircraftColor;

typedef struct CadAircraftComponent {
    int first_vertex, vertex_count;
    int first_triangle, triangle_count;
    int material;
} CadAircraftComponent;

typedef struct CadAircraft {
    int vertex_count, edge_count, triangle_count, component_count;
    CadExportVertex vertices[CAD_AIRCRAFT_MAX_VERTICES];
    CadExportEdge edges[CAD_AIRCRAFT_MAX_EDGES];
    CadAircraftTriangle triangles[CAD_AIRCRAFT_MAX_TRIANGLES];
    CadAircraftComponent components[CAD_AIRCRAFT_MAX_COMPONENTS];
    CadAircraftColor palette[CAD_AIRCRAFT_MATERIAL_COUNT];
    CadUnits units;
    int up_axis;                 /* CAD_EXPORT_Y_UP; nose +X, lateral Z. */
    int ticks;                   /* Suggested drawing/grid tick: 100. */
    long scale_numerator, scale_denominator; /* Metadata: 1/100. */
    long reference_length_cm, reference_span_cm; /* 7066, 6444. */
} CadAircraft;

/* Hand-authored, integer-only approximate Boeing 747-400 exterior demo,
 * not an engineering reconstruction. 707 x 644 model-unit silhouette,
 * inspired by public 70.66 m length / 64.44 m span; rounded at about ten
 * model units per metre. Coordinates and document units remain UNSPECIFIED.
 * Scale 1/100 and 100-unit ticks are presentation metadata, not conversion.
 * Fused upper-deck hump, swept tapered wings with upturned winglets,
 * stabilizers, fin, four nacelles and four pylons; no logos or landing gear.
 * Each of 14 components is closed and outward-wound, but they intersect:
 * this is NOT a Boolean-unioned watertight aircraft or an engine interior.
 * Nacelle end caps stand in for intake/exhaust. Palette glass is a cockpit
 * surface treatment, not extra detached geometry.
 *
 * All indices are zero-based asset indices, never CadIds. Wireframe edges
 * are unique ring/longitudinal structural edges, omitting mesh diagonals
 * and cap triangulation. All export styles are 0 (no house-layer semantics).
 * Render wireframe from edges; filled rendering uses triangles and palette.
 * The const singleton is initialized on first access; NULL means internal
 * validation failed. No allocation. Non-reentrant, intended for GUI thread.
 */
const CadAircraft *cad_aircraft_get(void);
/* Bounds, metadata, unique edges, nondegenerate faces and component ranges.
 * This is an input/storage validator, not a general manifold/union checker;
 * the supplied asset's closed topology and winding are tested separately. */
CadStatus cad_aircraft_validate(const CadAircraft *aircraft);

/* Explicit File/Demo replacement, like File/New: fresh monotonic IDs from
 * 1 in asset vertex order, unspecified units, and empty undo/redo history.
 * Uses a private static CadModel, <=64 deltas per transaction, publishing
 * only after complete success. NULL/active-transaction failures leave live
 * byte-for-byte untouched. Existing capacity/ID exhaustion is irrelevant
 * to a new document. Immediately after load, map each face index i through
 * live->vertices[i].id; preserve that mapping if subsequent edits reorder
 * vertices. The static triangles are not a live-model topology database.
 * Compile this module AND callers/dependencies with Watcom -ml -zt=1:
 * stage and asset are separate <64KiB objects, not stack/DGROUP buffers.
 * No default startup or UI registration is performed by this module.
 */
CadStatus cad_aircraft_load(CadModel *live);

#endif
