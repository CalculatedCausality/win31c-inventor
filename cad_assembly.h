#ifndef CAD_ASSEMBLY_H
#define CAD_ASSEMBLY_H

/* Portable C89, including Win16 (16-bit int, >=32-bit long). No allocation,
 * geometry, part registry, or Windows dependency. Part IDs are opaque references
 * owned by the caller: editing a definition is separate from placing occurrences.
 * Initialize before use; treat the exposed storage as read-only thereafter.
 * Distinct instances have independent local placements even with the same part.
 * All status-returning APIs leave state/output unchanged on failure. Outputs
 * must not overlap assembly storage. Calls on distinct assemblies are reentrant.
 * IDs 1..65535 are monotonic, never recycled; zero means world/no parent.
 * Depth counts occurrences, so a root has depth 1. Removal is leaf-only.
 * Reparenting preserves LOCAL placement, not world placement.
 * Grounding locks world placement: direct edits and ancestor-induced movement
 * are rejected. Unground explicitly to edit. Constraints lock direct placement
 * until cleared; fixed and hinge frames are copied, not live geometry datums.
 */
#define CAD_ASSEMBLY_MAX 32
#define CAD_ASSEMBLY_DEPTH 8
#define CAD_ASSEMBLY_Q 1024
#define CAD_ASSEMBLY_TRANSLATION 32767L
#define CAD_ASSEMBLY_ID_MAX 65535UL
#define CAD_ASSEMBLY_YAWS 8

typedef unsigned long CadAssemblyId;
typedef enum CadAssemblyStatus {
    CAD_ASSEMBLY_OK = 0, CAD_ASSEMBLY_ARGUMENT, CAD_ASSEMBLY_NOT_FOUND,
    CAD_ASSEMBLY_FULL, CAD_ASSEMBLY_ID_EXHAUSTED, CAD_ASSEMBLY_CYCLE,
    CAD_ASSEMBLY_TOO_DEEP, CAD_ASSEMBLY_TRANSFORM, CAD_ASSEMBLY_BOUNDS,
    CAD_ASSEMBLY_GROUNDED, CAD_ASSEMBLY_HAS_CHILDREN,
    CAD_ASSEMBLY_CONSTRAINED
} CadAssemblyStatus;

/* Column vectors: p_parent = r*p_local/1024 + t. r is Q10; t is
 * integral model units. Round once per composition, nearest, ties away from
 * zero. Authoritative locals are never overwritten by evaluated worlds.
 * Approximate rigidity: every row AND column norm/dot differs from identity
 * by <=4096 Q20 units, and positive handedness is checked. This tolerates
 * quantization, not exact orthogonality: no claim to eliminate tiny shear.
 * Reflections, scale and material shear are rejected. Composition can fail
 * if its rounded result exceeds these same tolerances or translation bounds.
 */
typedef struct CadAssemblyTransform {
    int r[3][3];
    long t[3];
} CadAssemblyTransform;

typedef struct CadAssemblyOccurrence {
    CadAssemblyId id, part_id, parent_id;
    int grounded;
    CadAssemblyTransform local;
    int constraint; /* 0 free, 1 fixed, 2 hinge */
    CadAssemblyTransform parent_frame, child_frame;
    int yaw, yaw_min, yaw_max;
} CadAssemblyOccurrence;

typedef struct CadAssembly {
    unsigned int count;
    CadAssemblyId next_id;
    CadAssemblyOccurrence occurrence[CAD_ASSEMBLY_MAX];
} CadAssembly;

void CadAssemblyInit(CadAssembly *assembly);
void CadAssemblyIdentity(CadAssemblyTransform *transform);
CadAssemblyStatus CadAssemblyValidateTransform(const CadAssemblyTransform *t);
CadAssemblyStatus CadAssemblyCompose(const CadAssemblyTransform *parent,
    const CadAssemblyTransform *local, CadAssemblyTransform *result);
CadAssemblyStatus CadAssemblyGet(const CadAssembly *a, CadAssemblyId id,
    CadAssemblyOccurrence *result);
CadAssemblyStatus CadAssemblyAdd(CadAssembly *a, CadAssemblyId part_id,
    CadAssemblyId parent_id, const CadAssemblyTransform *local,
    CadAssemblyId *new_id);
CadAssemblyStatus CadAssemblyRemove(CadAssembly *a, CadAssemblyId id);
CadAssemblyStatus CadAssemblySetLocal(CadAssembly *a, CadAssemblyId id,
    const CadAssemblyTransform *local);
CadAssemblyStatus CadAssemblyReparent(CadAssembly *a, CadAssemblyId id,
    CadAssemblyId parent_id);
CadAssemblyStatus CadAssemblyGround(CadAssembly *a, CadAssemblyId id,
    int grounded);
/* Writes count worlds in occurrence storage order; capacity must be >=count.
 * GetWorld is the stable-ID alternative. Neither function caches results. */
CadAssemblyStatus CadAssemblyEvaluate(const CadAssembly *a,
    CadAssemblyTransform *worlds, unsigned int capacity);
CadAssemblyStatus CadAssemblyGetWorld(const CadAssembly *a, CadAssemblyId id,
    CadAssemblyTransform *world);
/* Datum equation: local * child_frame = parent_frame * yaw.
 * Frames map datum coordinates into the respective occurrence coordinates.
 * Parent zero refers to the world datum. Fixed uses identity yaw. Hinge is
 * ONLY local datum-Z yaw from the built-in 0,45,...315 degree Q10 table.
 * Inclusive index limits cannot wrap: 0 <= min <= yaw <= max <= 7.
 * Datum inverse uses the rotation transpose and rounded inverse translation;
 * the equation is approximate for quantized frames, not an exact matrix inverse.
 * Intermediate inverse and composed placements must also fit the bounds.
 * No arbitrary solver. Bind also sets parent_id; hinge angles are absolute,
 * regenerated from the copied frames (never incremental rotations).
 */
CadAssemblyStatus CadAssemblyBindFixed(CadAssembly *a, CadAssemblyId id,
    CadAssemblyId parent_id, const CadAssemblyTransform *parent_frame,
    const CadAssemblyTransform *child_frame);
CadAssemblyStatus CadAssemblyBindHinge(CadAssembly *a, CadAssemblyId id,
    CadAssemblyId parent_id, const CadAssemblyTransform *parent_frame,
    const CadAssemblyTransform *child_frame, int yaw, int min, int max);
CadAssemblyStatus CadAssemblySetYaw(CadAssembly *a, CadAssemblyId id, int yaw);
CadAssemblyStatus CadAssemblyClearConstraint(CadAssembly *a, CadAssemblyId id);

#endif
