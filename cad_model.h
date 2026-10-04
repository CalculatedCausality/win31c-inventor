#ifndef CAD_MODEL_H
#define CAD_MODEL_H

#include <limits.h>

/* No assumption that int is 32 bits: Win16 int is 16 bits. Host long may
 * be wider; the public coordinate domain is still exactly -400..400.
 * IDs and compact history words require an exact 16-bit unsigned short. */
#if CHAR_BIT != 8 || USHRT_MAX != 65535U || SHRT_MAX != 32767 || SHRT_MIN != (-32767 - 1) || INT_MAX < 32767 || LONG_MAX < 2147483647L || LONG_MIN > (-2147483647L - 1L)
#error Unsupported numerical widths for cad_model
#endif

#define CAD_MAX_VERTICES 256
#define CAD_MAX_EDGES 512
#define CAD_MAX_TRANSACTION_DELTAS 64
#define CAD_MAX_HISTORY_COMMANDS 32
#define CAD_HISTORY_DELTAS 1024
#define CAD_COORD_MIN (-400L)
#define CAD_COORD_MAX 400L

typedef unsigned short CadId;
typedef enum CadStatus {
    CAD_OK = 0, CAD_INVALID_ARGUMENT, CAD_RANGE, CAD_NOT_FOUND,
    CAD_CAPACITY, CAD_ID_EXHAUSTED, CAD_TRANSACTION_ACTIVE,
    CAD_NO_TRANSACTION, CAD_TRANSACTION_FULL, CAD_NO_HISTORY
} CadStatus;
typedef enum CadUnits {
    CAD_UNITS_UNSPECIFIED = 0, CAD_UNITS_MM, CAD_UNITS_INCHES
} CadUnits;
typedef struct CadVertex {
    CadId id;
    long x, y, z;
} CadVertex;
typedef struct CadEdge {
    CadId id, a, b;
} CadEdge;

/* Implementation storage is exposed solely to allow caller allocation in C89.
 * Treat all fields as read-only; do not copy a live document for history.
 * Each delta is 16 bytes on both supported ABIs. History storage (including
 * the active transaction and command lengths) is below 32KB. No static data.
 */
typedef struct CadDelta {
    unsigned short kind, id;
    short data[6];
} CadDelta;
typedef struct CadModel {
    CadVertex vertices[CAD_MAX_VERTICES];
    CadEdge edges[CAD_MAX_EDGES];
    unsigned short vertex_count, edge_count;
    long next_vertex_id, next_edge_id;
    CadUnits units;
    CadDelta history[CAD_HISTORY_DELTAS];
    CadDelta staged[CAD_MAX_TRANSACTION_DELTAS];
    unsigned short lengths[CAD_MAX_HISTORY_COMMANDS];
    unsigned short history_count, history_cursor, history_used;
    unsigned short staged_count, transaction_active;
} CadModel;

/* Initialize once before use. Zero is never a valid ID. Vertex and edge IDs
 * have independent monotonic namespaces; undo/cancel never rewind allocators.
 * All mutations require begin. Stage calls expose tentative geometry; cancel
 * restores the begin-state geometry/units using reverse deltas. Each failed
 * call leaves the model and output parameters unchanged. A failed stage does
 * not cancel earlier successful stages. Commit/cancel require an active txn.
 * Net-no-op commit adds no history and preserves redo. Changed commit truncates
 * redo. History retains at most 32 commands AND 1024 deltas, evicting oldest
 * whole commands. No allocator, UI globals, persistence or threading policy.
 * Deleting a vertex removes all incident edges atomically; it fails before
 * mutation if incident edges plus the vertex exceed remaining 64-delta room.
 * Self-edges and parallel edges are permitted. Dense arrays may reorder on
 * deletion; clients retain IDs, never indices or pointers into the arrays.
 */
CadStatus cad_model_init(CadModel *model);
CadStatus cad_model_begin(CadModel *model);
CadStatus cad_model_commit(CadModel *model);
CadStatus cad_model_cancel(CadModel *model);
CadStatus cad_model_add_vertex(CadModel *model, long x, long y, long z, CadId *id);
CadStatus cad_model_move_vertex(CadModel *model, CadId id, long x, long y, long z);
CadStatus cad_model_delete_vertex(CadModel *model, CadId id);
CadStatus cad_model_add_edge(CadModel *model, CadId a, CadId b, CadId *id);
CadStatus cad_model_delete_edge(CadModel *model, CadId id);
CadStatus cad_model_set_units(CadModel *model, CadUnits units);
CadStatus cad_model_get_vertex(const CadModel *model, CadId id, CadVertex *vertex);
CadStatus cad_model_get_edge(const CadModel *model, CadId id, CadEdge *edge);
CadStatus cad_model_undo(CadModel *model);
CadStatus cad_model_redo(CadModel *model);
const char *cad_model_status_string(CadStatus status);

#endif
