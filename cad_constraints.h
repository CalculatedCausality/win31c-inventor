#ifndef CAD_CONSTRAINTS_H
#define CAD_CONSTRAINTS_H

/* Standalone integer, scalar linear-offset solver; no Windows dependencies.
 * C89, including Win16 (16-bit int, >=32-bit long). All arithmetic is checked
 * against the signed 32-bit range even on hosts with wider long.
 * XYZ callers flatten vertices as 3*v + axis (axis 0,1,2); at most 40 vertices.
 * The independent scalar API permits 0..128 variables and 0..128 equations.
 * It does NOT solve distances, angles, rotations, or nonlinear geometry.
 */
#define CAD_CONSTRAINT_MAX_VARIABLES 128
#define CAD_CONSTRAINT_MAX_VERTICES 40
#define CAD_CONSTRAINT_MAX_CONSTRAINTS 128
#define CAD_CONSTRAINT_COORD_MIN (-400)
#define CAD_CONSTRAINT_COORD_MAX 400
#define CAD_CONSTRAINT_FIXED (-1)
#define CAD_CONSTRAINT_ID_MAX 2147483647UL

typedef unsigned long CadConstraintId;

typedef enum CadConstraintStatus {
    CAD_CONSTRAINT_OK = 0,
    CAD_CONSTRAINT_REDUNDANT, /* Successful add; equation is stored with an ID. */
    CAD_CONSTRAINT_CONFLICT,  /* Inconsistent equation, anchor, or edit. */
    CAD_CONSTRAINT_RANGE,     /* No bounded solution, or checked32 overflow. */
    CAD_CONSTRAINT_CAPACITY,  /* Variable/equation limit or exhausted ID space. */
    CAD_CONSTRAINT_INVALID,   /* NULL, bad count/index, malformed argument. */
    CAD_CONSTRAINT_NOT_FOUND  /* Unknown/stale explicit constraint ID. */
} CadConstraintStatus;

typedef struct CadConstraint {
    CadConstraintId id;
    int a;                  /* FIXED means the constant zero, otherwise index. */
    int b;
    long offset;            /* value[b] - value[a] = offset; fixed => anchor. */
} CadConstraint;

/* Caller-owned storage, no allocation. Members are inspectable but read-only
 * after initialization; use the API to mutate. Do not serialize native structs.
 */
typedef struct CadConstraints {
    int variable_count;
    int constraint_count;
    CadConstraintId next_id;
    int values[CAD_CONSTRAINT_MAX_VARIABLES];
    CadConstraint constraints[CAD_CONSTRAINT_MAX_CONSTRAINTS];
} CadConstraints;

typedef struct CadConstraintAnalysis {
    int variable_count;
    int constraint_count;
    int independent;        /* Rank of equations, including fixed anchors. */
    int redundant;          /* Consistent equations not increasing rank. */
    int free_root_dof;      /* Algebraic unanchored roots; bounds can pin a root. */
} CadConstraintAnalysis;

/* All failures preserve the entire model and output arguments. OK and
 * REDUNDANT are successes. No-op drag/edit returns OK. Static bounded scratch
 * storage avoids a large Win16 stack: calls are non-reentrant, single-task.
 * Init resets IDs to 1; NULL values is permitted only for count == 0.
 * Output pointers must not alias the model or each other. The documented
 * values/candidate input aliasing for Init/Edit is supported.
 */
CadConstraintStatus CadConstraintsInit(CadConstraints *model,
                                     const int *values, int count);

/* Add stages equations and a bounded candidate before committing. Each free
 * root preserves its lowest-index member if feasible, otherwise clamps the
 * ROOT to the common feasible interval. Anchors take precedence. Every member
 * is then derived from that root; coordinates are never separately clamped.
 * IDs increase monotonically within an initialized model, never reused after
 * removal. id_out may be NULL. Redundant adds consume a slot and an ID.
 */
CadConstraintStatus CadConstraintsAddOffset(CadConstraints *model, int a, int b,
                                           long offset, CadConstraintId *id_out);
CadConstraintStatus CadConstraintsAddAnchor(CadConstraints *model, int variable,
                                           long coordinate,
                                           CadConstraintId *id_out);
CadConstraintStatus CadConstraintsRemove(CadConstraints *model,
                                        CadConstraintId id);
CadConstraintStatus CadConstraintsGet(const CadConstraints *model,
                                     CadConstraintId id, CadConstraint *out);
CadConstraintStatus CadConstraintsAnalyze(const CadConstraints *model,
                                         CadConstraintAnalysis *out);

/* Range returns feasible COORDINATES for the selected member, computed by
 * intersecting every member's +/-400 root interval. An anchored group has a
 * singleton range. Drag is absolute and exact: it propagates the whole group,
 * returns RANGE outside its feasible interval, CONFLICT on a changed anchor.
 */
CadConstraintStatus CadConstraintsRange(const CadConstraints *model, int variable,
                                       long *minimum, long *maximum);
CadConstraintStatus CadConstraintsDrag(CadConstraints *model, int variable,
                                      long coordinate);

/* Atomic full candidate edit: validates count, all coordinates, all equations
 * and anchors, then commits. No automatic solving/clamping of this candidate.
 * Candidate may alias model->values. NULL is permitted only for count == 0.
 */
CadConstraintStatus CadConstraintsEdit(CadConstraints *model,
                                      const int *candidate, int count);

#endif
