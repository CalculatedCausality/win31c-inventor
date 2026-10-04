#ifndef CAD_FEATURES_H
#define CAD_FEATURES_H

/* Inventor-style parametric feature history ("feature tree") for CAD.EXE.
 * Portable C89: no allocation, no floating point, no Windows headers; all
 * document and output storage is caller-owned. Mutations and regeneration
 * use private static scratch (each object < 64 KiB) and are NOT reentrant.
 *
 * Features
 * --------
 * An ordered list of at most CAD_FEAT_MAX features with stable, monotonic,
 * never reused nonzero IDs (next_id is serialized):
 *   SKETCH  - work plane (XY/XZ/YZ) + offset expression, 0..8 closed profiles
 *   EXTRUDE - one profile (or ALL) of an earlier sketch, distance expression,
 *             direction +normal / -normal / symmetric
 *   REVOLVE - one profile (or ALL) of an earlier sketch revolved 360 degrees
 *             about the sketch u or v axis in 12..24 segments
 * Extrude/revolve may reference only a sketch that precedes them in the list;
 * every edit, move and read enforces this.
 *
 * Names: 1..31 printable ASCII characters, no '"', no leading/trailing space,
 * unique ignoring ASCII case. Passing a null name to add_* generates
 * "Sketch<n>" / "Extrusion<n>" / "Revolution<n>" (first free n).
 * Expressions: 1..64 printable ASCII characters except '"', stored verbatim
 * and evaluated only by cad_features_regenerate against a CadUserParams table
 * with cad_userparams_eval_expression (rounded model units, -400..400).
 *
 * Work planes (right-handed: u x v = normal, so winding is preserved).
 * Sketch point (u,v) at normal distance n from the origin plane, where
 * n = offset (+ extrusion depth) maps to world:
 *   XY: (u, v, n)    normal +Z
 *   XZ: (u, n, -v)   normal +Y   (u = +X, v = -Z, as Inventor's XZ sketch)
 *   YZ: (n, u, v)    normal +X
 *
 * Profiles (local sketch coordinates, each must be a valid simple polygon by
 * cad_sketch_validate after evaluation, inside -400..400):
 *   RECT   cx cy w h   corners x0 = cx - w/2 (C division), x1 = x0 + w,
 *                      same for y; w, h >= 1
 *   CIRCLE cx cy r     inscribed regular polygon with `sides` (8..32) vertices
 *                      at angles k*360/sides starting on +u; r >= 1
 *   POLY   x1 y1 ...   3..16 points (6..32 expressions), implicit closing edge
 * The total stored expression text of one profile (each length + 1) must be
 * <= CAD_FEAT_PROFILE_TEXT bytes. Profiles are addressed by sketch ID and
 * 0-based index in creation order. Removing a profile is rejected while a
 * feature references that exact index; references to later indices of the
 * same sketch are renumbered.
 *
 * Extrude: distance must evaluate to 1..400. Body spans normal range
 *   POS [off, off+d], NEG [off-d, off], SYM [off-d/2, off-d/2+d] (C division)
 * and must stay inside -400..400 (else ERROR on the distance field).
 * Mesh = cad_sketch_extrude in sketch coordinates (2n vertices, 4n-4
 * triangles, outward winding) mapped to world. Wireframe = bottom ring, top
 * ring and n verticals (3n segments; no triangulation diagonals).
 * Revolve: every profile point must lie strictly on one side of the axis
 * (radius v for axis U, u for axis V; touching the axis is an error).
 *   axis U: (u, v*cos t, v*sin t)    axis V: (u*cos t, v, u*sin t)
 * in (u, v, normal) sketch-local terms before the plane mapping; the normal
 * component is added to the sketch offset. Mesh = m*s vertices, 2*m*s
 * triangles (outward winding, no caps needed); wireframe = s profile copies
 * plus m circles (2*m*s segments). Rounding to integer coordinates must not
 * create degenerate triangles (else ERROR "revolve too small").
 *
 * Regeneration (cad_features_regenerate) is fail-soft per feature: each
 * feature is OK, ERROR (message + offending field/profile/expression),
 * SUPPRESSED, or BLOCKED (its sketch is ERROR, BLOCKED or SUPPRESSED; the
 * blocking ID is reported). Later independent features still build.
 * Output: world segments (sketch curves and body edges) tagged with feature
 * ID; shading triangles in cad_meshview form (material = slot % 6). Shading
 * is added per body atomically; a body that does not fit sets
 * shading_truncated and is skipped (its wireframe is kept). Segments beyond
 * CAD_FEAT_OUT_SEGMENTS set wireframe_truncated. Profile overlap between
 * bodies, booleans and nested profiles (holes) are NOT computed.
 */
#include "cad_userparams.h"
#include "cad_meshview.h"

#define CAD_FEAT_MAX 24
#define CAD_FEAT_MAX_PROFILES 32         /* document-wide */
#define CAD_FEAT_SKETCH_PROFILES 8       /* per sketch */
#define CAD_FEAT_NAME_MAX 31
#define CAD_FEAT_EXPR_MAX 64
#define CAD_FEAT_PROFILE_TEXT 512
#define CAD_FEAT_PROFILE_EXPRS 32
#define CAD_FEAT_POLY_MIN 3
#define CAD_FEAT_POLY_MAX 16
#define CAD_FEAT_CIRCLE_MIN 8
#define CAD_FEAT_CIRCLE_MAX 32
#define CAD_FEAT_CIRCLE_DEFAULT 16
#define CAD_FEAT_REVOLVE_MIN 12
#define CAD_FEAT_REVOLVE_MAX 24
#define CAD_FEAT_ALL (-1)                /* profile selector: every profile */
#define CAD_FEAT_TEXT_MAX 8192           /* serialized limit, enforced */
#define CAD_FEAT_DESC_MAX 191
#define CAD_FEAT_OUT_SEGMENTS 1024
#define CAD_FEAT_OUT_VERTICES CAD_MESHVIEW_MAX_VERTICES   /* 256 */
#define CAD_FEAT_OUT_FACES CAD_MESHVIEW_MAX_FACES         /* 512 */

typedef enum CadFeatStatus {
    CAD_FEAT_OK = 0,
    CAD_FEAT_ARGUMENT,    /* null pointer or out-of-range enum/number */
    CAD_FEAT_FULL,        /* feature/profile capacity or ID exhaustion */
    CAD_FEAT_NAME,        /* invalid name */
    CAD_FEAT_DUPLICATE,   /* name already used */
    CAD_FEAT_NOT_FOUND,   /* no such feature/profile */
    CAD_FEAT_EXPRESSION,  /* invalid expression text (stored form) */
    CAD_FEAT_REFERENCE,   /* bad sketch reference (missing, not a sketch,
                             later in order, profile index out of range) */
    CAD_FEAT_IN_USE,      /* remove of a referenced feature/profile */
    CAD_FEAT_ORDER,       /* move would put a feature before its sketch */
    CAD_FEAT_CAPACITY,    /* text limit or caller buffer too small */
    CAD_FEAT_FORMAT,      /* malformed serialized line */
    /* regeneration-only error kinds (CadFeatHealth.error) */
    CAD_FEAT_EVAL,        /* expression evaluation failed (see param_status) */
    CAD_FEAT_RANGE,       /* value or body outside its allowed range */
    CAD_FEAT_GEOMETRY     /* invalid profile polygon / degenerate body */
} CadFeatStatus;

typedef enum CadFeatType {
    CAD_FEAT_SKETCH = 1, CAD_FEAT_EXTRUDE, CAD_FEAT_REVOLVE
} CadFeatType;
typedef enum CadFeatPlane { CAD_FEAT_XY = 0, CAD_FEAT_XZ, CAD_FEAT_YZ } CadFeatPlane;
typedef enum CadFeatDirection {
    CAD_FEAT_POSITIVE = 0, CAD_FEAT_NEGATIVE, CAD_FEAT_SYMMETRIC
} CadFeatDirection;
typedef enum CadFeatAxis { CAD_FEAT_AXIS_U = 0, CAD_FEAT_AXIS_V } CadFeatAxis;
typedef enum CadFeatProfileKind {
    CAD_FEAT_RECT = 1, CAD_FEAT_CIRCLE, CAD_FEAT_POLY
} CadFeatProfileKind;

/* Which part of a feature an error refers to. */
typedef enum CadFeatField {
    CAD_FEAT_FIELD_NONE = 0, CAD_FEAT_FIELD_NAME, CAD_FEAT_FIELD_PLANE,
    CAD_FEAT_FIELD_OFFSET, CAD_FEAT_FIELD_PROFILE, CAD_FEAT_FIELD_SKETCH,
    CAD_FEAT_FIELD_DISTANCE, CAD_FEAT_FIELD_DIRECTION, CAD_FEAT_FIELD_AXIS,
    CAD_FEAT_FIELD_SEGMENTS, CAD_FEAT_FIELD_SIDES
} CadFeatField;

typedef struct CadFeatError {
    CadFeatStatus status;
    unsigned feature_id;   /* offending feature; for IN_USE/ORDER the
                              referencing/blocking feature; 0 if none */
    CadFeatField field;
    int profile;           /* profile index or -1 */
    int expr_index;        /* expression index within the profile or -1 */
    unsigned line;         /* read_text: 1-based line, else 0 */
    const char *message;   /* static text, never null */
} CadFeatError;

/* Storage exposed only for caller allocation; treat as opaque. */
typedef struct CadFeatProfileData {
    unsigned owner;                    /* sketch feature ID */
    unsigned char kind, sides, expr_count;
    char text[CAD_FEAT_PROFILE_TEXT];  /* expr_count NUL-terminated strings */
} CadFeatProfileData;
typedef struct CadFeatureData {
    unsigned id, sketch_id, segments;
    unsigned char type, suppressed, plane, direction, axis;
    int profile;
    char name[CAD_FEAT_NAME_MAX + 1];
    char expr[CAD_FEAT_EXPR_MAX + 1];  /* sketch offset or extrude distance */
} CadFeatureData;
typedef struct CadFeatures {
    unsigned count, profile_count, next_id;
    CadFeatureData items[CAD_FEAT_MAX];
    CadFeatProfileData profiles[CAD_FEAT_MAX_PROFILES];
} CadFeatures;

/* Read-only views; pointers stay valid until the next mutation. */
typedef struct CadFeatInfo {
    unsigned id, index;
    CadFeatType type;
    int suppressed;
    const char *name;
    CadFeatPlane plane;            /* sketch */
    const char *offset;            /* sketch */
    unsigned profile_count;        /* sketch */
    unsigned sketch_id;            /* extrude/revolve */
    int profile;                   /* extrude/revolve: index or CAD_FEAT_ALL */
    CadFeatDirection direction;    /* extrude */
    const char *distance;          /* extrude */
    CadFeatAxis axis;              /* revolve */
    unsigned segments;             /* revolve */
    /* Browser line, e.g. "Extrusion1 (Sketch1, 30 mm)"; profile numbers
     * in it are 1-based ("Sketch1 profile 2" = index 1). */
    char description[CAD_FEAT_DESC_MAX + 1];
} CadFeatInfo;
typedef struct CadFeatProfileInfo {
    CadFeatProfileKind kind;
    unsigned sides;                /* CIRCLE only, else 0 */
    unsigned expr_count;           /* RECT 4, CIRCLE 3, POLY 2*points */
    const char *exprs[CAD_FEAT_PROFILE_EXPRS];
} CadFeatProfileInfo;

void cad_features_init(CadFeatures *features);
unsigned cad_features_count(const CadFeatures *features);
/* Index of the feature with this ID, or -1. */
int cad_features_index(const CadFeatures *features, unsigned id);
unsigned cad_features_find(const CadFeatures *features, const char *name);
CadFeatStatus cad_features_at(const CadFeatures *features, unsigned index,
                              CadFeatInfo *info);
CadFeatStatus cad_features_get(const CadFeatures *features, unsigned id,
                               CadFeatInfo *info);
CadFeatStatus cad_features_profile(const CadFeatures *features,
    unsigned sketch_id, unsigned index, CadFeatProfileInfo *info);

/* All mutations are fail-atomic (features unchanged on error, including
 * when the serialized form would exceed CAD_FEAT_TEXT_MAX). error and id
 * may be null. New features are appended at the end of the list. */
CadFeatStatus cad_features_add_sketch(CadFeatures *features, const char *name,
    CadFeatPlane plane, const char *offset, unsigned *id, CadFeatError *error);
CadFeatStatus cad_features_edit_sketch(CadFeatures *features, unsigned id,
    CadFeatPlane plane, const char *offset, CadFeatError *error);
/* exprs: RECT {cx,cy,w,h}; CIRCLE {cx,cy,r} with sides 8..32 (0 = 16);
 * POLY {x1,y1,...,xn,yn} with expr_count 6..32 even. sides is ignored
 * (must be 0) for RECT/POLY. index receives the new profile index. */
CadFeatStatus cad_features_add_profile(CadFeatures *features,
    unsigned sketch_id, CadFeatProfileKind kind, const char *const *exprs,
    unsigned expr_count, unsigned sides, unsigned *index, CadFeatError *error);
CadFeatStatus cad_features_edit_profile(CadFeatures *features,
    unsigned sketch_id, unsigned index, CadFeatProfileKind kind,
    const char *const *exprs, unsigned expr_count, unsigned sides,
    CadFeatError *error);
CadFeatStatus cad_features_remove_profile(CadFeatures *features,
    unsigned sketch_id, unsigned index, CadFeatError *error);
CadFeatStatus cad_features_add_extrude(CadFeatures *features, const char *name,
    unsigned sketch_id, int profile, CadFeatDirection direction,
    const char *distance, unsigned *id, CadFeatError *error);
CadFeatStatus cad_features_edit_extrude(CadFeatures *features, unsigned id,
    unsigned sketch_id, int profile, CadFeatDirection direction,
    const char *distance, CadFeatError *error);
CadFeatStatus cad_features_add_revolve(CadFeatures *features, const char *name,
    unsigned sketch_id, int profile, CadFeatAxis axis, unsigned segments,
    unsigned *id, CadFeatError *error);
CadFeatStatus cad_features_edit_revolve(CadFeatures *features, unsigned id,
    unsigned sketch_id, int profile, CadFeatAxis axis, unsigned segments,
    CadFeatError *error);
CadFeatStatus cad_features_rename(CadFeatures *features, unsigned id,
    const char *name, CadFeatError *error);
CadFeatStatus cad_features_suppress(CadFeatures *features, unsigned id,
    int suppressed, CadFeatError *error);
/* Rejected with CAD_FEAT_IN_USE (error->feature_id = first referencing
 * feature) while another feature references it. Removes sketch profiles. */
CadFeatStatus cad_features_remove(CadFeatures *features, unsigned id,
    CadFeatError *error);
/* Swap with the neighbour; CAD_FEAT_ORDER (feature_id = neighbour) when the
 * neighbour is this feature's sketch (up) or references it (down);
 * CAD_FEAT_ARGUMENT at either end of the list. */
CadFeatStatus cad_features_move_up(CadFeatures *features, unsigned id,
    CadFeatError *error);
CadFeatStatus cad_features_move_down(CadFeatures *features, unsigned id,
    CadFeatError *error);

/* ---- regeneration ---- */
typedef enum CadFeatState {
    CAD_FEAT_STATE_OK = 0, CAD_FEAT_STATE_ERROR, CAD_FEAT_STATE_SUPPRESSED,
    CAD_FEAT_STATE_BLOCKED
} CadFeatState;
typedef enum CadFeatSegKind { CAD_FEAT_SEG_SKETCH = 0, CAD_FEAT_SEG_EDGE } CadFeatSegKind;

typedef struct CadFeatHealth {
    unsigned feature_id;
    CadFeatState state;
    CadFeatStatus error;           /* ERROR: EVAL/RANGE/GEOMETRY/REFERENCE */
    CadFeatField field;
    int profile, expr_index;       /* -1 when not applicable */
    CadUserParamStatus param_status;   /* EVAL: cad_userparams status */
    unsigned offset;               /* EVAL: byte offset in the expression */
    unsigned blocked_by;           /* BLOCKED: sketch feature ID */
    const char *message;           /* static, never null ("" when OK) */
    unsigned first_segment, segment_count, first_face, face_count;
} CadFeatHealth;
typedef struct CadFeatSegment {
    long a[3], b[3];
    unsigned feature_id;
    unsigned char kind;            /* CadFeatSegKind */
} CadFeatSegment;
typedef struct CadFeatOutput {
    unsigned feature_count;        /* health[0..feature_count-1] by slot */
    unsigned segment_count, vertex_count, face_count;
    int wireframe_truncated, shading_truncated;
    int has_bounds;
    long min[3], max[3];           /* over all emitted segments */
    CadFeatHealth health[CAD_FEAT_MAX];
    CadFeatSegment segments[CAD_FEAT_OUT_SEGMENTS];
    CadMeshViewVertex vertices[CAD_FEAT_OUT_VERTICES];
    CadMeshViewFace faces[CAD_FEAT_OUT_FACES];
} CadFeatOutput;

/* Evaluates every feature in order into *out (fully overwritten). params is
 * not modified (non-const only because evaluation uses its scratch).
 * error_count (may be null) receives the number of ERROR + BLOCKED features.
 * Returns CAD_FEAT_ARGUMENT only for null features/params/out. */
CadFeatStatus cad_features_regenerate(const CadFeatures *features,
    CadUserParams *params, CadFeatOutput *out, unsigned *error_count);
/* Returns 1 and the bounds if the output has any segment, else 0. */
int cad_features_bounds(const CadFeatOutput *out, long min[3], long max[3]);

/* ---- serialization (no section header) ----
 * NOTE: the limit is CAD_FEAT_TEXT_MAX = 8192 bytes (twice the generic 4096
 * module budget); every mutation and read_text enforce it.
 * Lines, every one ending in "\n", strings double-quoted (no escapes since
 * names/expressions cannot contain '"'):
 *   N <next_id>                                     (first line, exactly once)
 *   F <id> SKETCH <A|S> "<name>" <XY|XZ|YZ> "<offset>"
 *   R RECT "<cx>" "<cy>" "<w>" "<h>"
 *   R CIRCLE <sides> "<cx>" "<cy>" "<r>"
 *   R POLY <points> "<x1>" "<y1>" ...
 *   F <id> EXTRUDE <A|S> "<name>" <sketchid> <index|ALL> <POS|NEG|SYM> "<dist>"
 *   F <id> REVOLVE <A|S> "<name>" <sketchid> <index|ALL> <U|V> <segments>
 * A = active, S = suppressed. R lines belong to the nearest preceding SKETCH
 * line and must directly follow it (or another R line). Single spaces only.
 * write_text sets *length to the required size even on CAD_FEAT_CAPACITY;
 * a NUL is appended only if capacity exceeds the length. read_text is
 * fail-atomic and reports the 1-based line in error->line. Empty text (length
 * 0) reads as an empty document (written back as "N 1\n"). */
CadFeatStatus cad_features_write_text(const CadFeatures *features,
    char *buffer, unsigned capacity, unsigned *length);
CadFeatStatus cad_features_read_text(CadFeatures *features, const char *text,
    unsigned length, CadFeatError *error);

const char *cad_features_status_string(CadFeatStatus status);

#endif
