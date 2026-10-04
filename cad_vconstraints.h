#ifndef CAD_VCONSTRAINTS_H
#define CAD_VCONSTRAINTS_H

/* Inventor-style constraints between CadModel vertices, identified by stable
 * CadId, built on the cad_constraints.h scalar offset solver. Strict C89,
 * no Windows headers, no heap, no floating point. Win16 large-model safe.
 *
 * Kinds (axis 0 = X, 1 = Y, 2 = Z):
 *   COINCIDENT(a,b)          a.x=b.x, a.y=b.y, a.z=b.z            (3 equations)
 *   ALIGN(a,b,axis)          a.axis = b.axis  (Horizontal = ALIGN Y) (1)
 *   DISTANCE(a,b,axis,off)   b.axis - a.axis = off, |off| <= 800    (1)
 *   FIX(a)                   a anchored at its coordinates when added (3)
 *
 * The constraint set holds records only. The solver is rebuilt from the
 * HEALTHY records and the current CadModel geometry on every call (static
 * scratch: non-reentrant, single task). Geometry changes made outside this
 * module (undo/redo/load/delete/manual edits) must be followed by
 * cad_vc_sync(), which removes records whose vertices vanished and flags
 * records the geometry no longer satisfies as SICK. Sick records are kept,
 * listed and serialized but excluded from solving; sync heals them once the
 * geometry satisfies them again, and cad_vc_set_distance re-solves a sick
 * distance. If a healthy record is unsatisfied when solving (sync was not
 * called) the call returns CAD_VC_STALE and changes nothing.
 *
 * Geometry is never modified here. Solving calls return a CadVcMoves list the
 * caller stages in ONE CadModel transaction (cad_vc_stage_moves helps). At
 * most 40 vertices participate in healthy records, so a move list (<= 40
 * entries) always fits one 64-delta transaction.
 *
 * Every failure leaves the constraint set and every output unchanged.
 */
#include "cad_model.h"
#include "cad_constraints.h"

#define CAD_VC_MAX_RECORDS 64
#define CAD_VC_MAX_VERTICES CAD_CONSTRAINT_MAX_VERTICES  /* 40 */
#define CAD_VC_MAX_MOVES CAD_VC_MAX_VERTICES
#define CAD_VC_MAX_DISTANCE 800L
#define CAD_VC_ID_MAX 2147483647UL
#define CAD_VC_TEXT_MAX 4096U
#define CAD_VC_DESCRIBE_MAX 48U   /* Buffer size that fits any display string. */

typedef enum CadVcStatus {
    CAD_VC_OK = 0,
    CAD_VC_INVALID,     /* NULL, bad kind/axis, a == b, malformed argument. */
    CAD_VC_NOT_FOUND,   /* Unknown constraint ID or missing model vertex. */
    CAD_VC_CONFLICT,    /* Over-constrained: fixed vertex or inconsistent. */
    CAD_VC_RANGE,       /* Solution leaves -400..400 or offset out of range. */
    CAD_VC_CAPACITY,    /* Records, 40 vertices, 128 equations or ID space. */
    CAD_VC_STALE,       /* Healthy record unsatisfied: call cad_vc_sync. */
    CAD_VC_PARSE,       /* Malformed/unknown serialized line. */
    CAD_VC_BUFFER       /* Output text buffer too small. */
} CadVcStatus;

typedef enum CadVcKind {
    CAD_VC_COINCIDENT = 1,
    CAD_VC_ALIGN = 2,
    CAD_VC_DISTANCE = 3,
    CAD_VC_FIX = 4
} CadVcKind;

typedef enum CadVcHealth {
    CAD_VC_HEALTHY = 0,
    CAD_VC_SICK = 1
} CadVcHealth;

typedef unsigned long CadVcId;

typedef struct CadVConstraint {
    CadVcId id;          /* Stable, nonzero, monotonic, never reused. */
    int kind;            /* CadVcKind. */
    int axis;            /* 0..2 for ALIGN/DISTANCE, otherwise 0. */
    int health;          /* CadVcHealth (runtime state, not serialized). */
    CadId a, b;          /* b is 0 for FIX. */
    long offset;         /* DISTANCE value, otherwise 0. */
    long fix[3];         /* FIX anchor coordinates, otherwise 0. */
} CadVConstraint;

/* Caller-owned; read-only fields. Records are kept in ascending ID order. */
typedef struct CadVConstraints {
    int count;
    CadVcId next_id;     /* High-water mark; 0 when the ID space is spent. */
    CadVConstraint items[CAD_VC_MAX_RECORDS];
} CadVConstraints;

typedef struct CadVcMove {
    CadId id;
    long x, y, z;
} CadVcMove;

typedef struct CadVcMoves {
    int count;
    CadVcMove items[CAD_VC_MAX_MOVES];
} CadVcMoves;

typedef struct CadVcAnalysis {
    int records, healthy, sick;
    int vertices;        /* Distinct vertices in healthy records. */
    int equations;       /* Scalar equations of healthy records. */
    int independent;     /* Solver rank (including anchors). */
    int redundant;       /* Consistent equations not adding rank. */
    int free_dof;        /* 3 * vertices - independent. */
} CadVcAnalysis;

CadVcStatus cad_vc_init(CadVConstraints *vc);

/* Add a record. If the model already satisfies it, moves->count is 0.
 * Otherwise the second vertex's (b) constrained group is moved; if that group
 * is fixed, a's group moves instead (FIX never moves). Returned moves must be
 * staged in one transaction. On failure nothing changes. id_out may be NULL.
 * Pass b = 0, axis = 0, offset = 0 for FIX; axis/offset are ignored where
 * the kind does not use them but must be 0.
 */
CadVcStatus cad_vc_add(CadVConstraints *vc, const CadModel *model, int kind,
                       CadId a, CadId b, int axis, long offset,
                       CadVcMoves *moves, CadVcId *id_out);
CadVcStatus cad_vc_remove(CadVConstraints *vc, CadVcId id);
CadVcStatus cad_vc_get(const CadVConstraints *vc, CadVcId id,
                       CadVConstraint *out);
/* Listing: index 0..count-1, ascending ID order. */
CadVcStatus cad_vc_at(const CadVConstraints *vc, int index,
                      CadVConstraint *out);
/* "Coincident V3-V7", "Align Y V1-V2", "Distance X V1-V2 = 50", "Fix V4";
 * sick records get the suffix " (Sick)". NUL-terminated; capacity must be
 * large enough (CAD_VC_DESCRIBE_MAX always suffices) else CAD_VC_BUFFER.
 */
CadVcStatus cad_vc_describe(const CadVConstraint *c, char *buffer,
                            unsigned capacity);

/* Edit a DISTANCE value (healthy or sick) and re-solve: b's group moves, or
 * a's group if b's is fixed. Success makes the record healthy.
 */
CadVcStatus cad_vc_set_distance(CadVConstraints *vc, const CadModel *model,
                                CadVcId id, long offset, CadVcMoves *moves);

/* Grip drag / numeric move of one vertex to an absolute position. Moves
 * list the dragged vertex first (always present), then every other vertex
 * whose position changes. Unconstrained vertices (or ones only in sick
 * records) pass through. RANGE if the group would leave -400..400, CONFLICT
 * if a fixed vertex/group would have to move.
 */
CadVcStatus cad_vc_drag(const CadVConstraints *vc, const CadModel *model,
                        CadId vertex, long x, long y, long z,
                        CadVcMoves *moves);

/* Rebuild against current geometry: removes records with missing vertices,
 * then in ID order marks a record healthy iff the geometry satisfies it and
 * the healthy set stays within 40 vertices / 128 equations, else SICK.
 * removed/sick may be NULL. Never moves geometry.
 */
CadVcStatus cad_vc_sync(CadVConstraints *vc, const CadModel *model,
                        int *removed, int *sick);

CadVcStatus cad_vc_analyze(const CadVConstraints *vc, const CadModel *model,
                           CadVcAnalysis *out);

/* Stage every move into the caller's ACTIVE transaction. Prechecks room and
 * vertex existence, so on failure nothing is staged.
 */
CadVcStatus cad_vc_stage_moves(CadModel *model, const CadVcMoves *moves);

/* Serialization (no section header), one line each, "\n" terminated:
 *   N <next_id>
 *   C <id> COINCIDENT <a> <b>
 *   C <id> ALIGN <a> <b> <X|Y|Z>
 *   C <id> DISTANCE <a> <b> <X|Y|Z> <offset>
 *   C <id> FIX <a> <x> <y> <z>
 * Writer emits N first, then records in ascending ID order, and a trailing
 * NUL (capacity must include it); *length excludes the NUL. Reader accepts
 * exactly that canonical form (single spaces, decimal without leading zeros
 * or "-0"), requires one N line first (empty text = initialized empty set),
 * IDs ascending and below next_id. Fail-atomic; *error_line (may be NULL)
 * gets the 1-based offending line or 0. Loaded records are HEALTHY until the
 * caller runs cad_vc_sync against the loaded model.
 */
CadVcStatus cad_vc_write_text(const CadVConstraints *vc, char *buffer,
                              unsigned capacity, unsigned *length);
CadVcStatus cad_vc_read_text(CadVConstraints *vc, const char *text,
                             unsigned length, unsigned *error_line);

const char *cad_vc_status_string(CadVcStatus status);

#endif
