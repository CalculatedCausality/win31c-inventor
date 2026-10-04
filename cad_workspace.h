#ifndef CAD_WORKSPACE_H
#define CAD_WORKSPACE_H

/* Include this bridge BEFORE cad_params.h. The standalone params and model
 * APIs unfortunately use the same enum names. Only type/enumerator spellings
 * change here; external function names and ABI remain unchanged. */
#ifdef CAD_PARAMS_H
#error Include cad_workspace.h before cad_params.h
#endif
#define CadStatus CadParamStatus
#define CAD_OK CAD_PARAM_OK
#include "cad_params.h"
#undef CAD_OK
#undef CadStatus
#include "cad_model.h"
#include "cad_sketch.h"
#include "cad_export.h"
#include "cad_drawing.h"
#include <stdio.h>

typedef enum CadWorkspaceStatus {
    CAD_WORKSPACE_OK = 0, CAD_WORKSPACE_ARGUMENT, CAD_WORKSPACE_PARAMETER,
    CAD_WORKSPACE_SKETCH, CAD_WORKSPACE_MODEL, CAD_WORKSPACE_UNSUPPORTED,
    CAD_WORKSPACE_IO, CAD_WORKSPACE_FORMAT, CAD_WORKSPACE_REVISION,
    CAD_WORKSPACE_TOPOLOGY
} CadWorkspaceStatus;
typedef enum CadWorkspaceKind {
    CAD_WORKSPACE_EMPTY = 0, CAD_WORKSPACE_HOUSE, CAD_WORKSPACE_EXTRUSION
} CadWorkspaceKind;
#define CAD_WORKSPACE_MAP_DEFAULTS_TO_LEGACY 1U

typedef struct CadWorkspaceIntent {
    CadWorkspaceKind kind;
    CadUnits units;
    CadModelScale scale;
    CadParams parameters;
    int point_count, height;
    CadSketchPoint profile[CAD_SKETCH_MAX_POINTS];
} CadWorkspaceIntent;
typedef struct CadWorkspace {
    CadModel model;
    CadWorkspaceIntent intent;
    /* Extrusion mesh in feature order; house is explicitly only wireframe. */
    CadSketchMesh mesh;
    unsigned long revision, saved_revision;
} CadWorkspace;
typedef struct CadWorkspaceView {
    CadId ids[CAD_MAX_VERTICES];
    CadExportVertex vertices[CAD_MAX_VERTICES];
    CadExportEdge edges[CAD_MAX_EDGES];
    CadDrawingPoint points[CAD_MAX_VERTICES];
    CadDrawingEdge lines[CAD_MAX_EDGES];
    int vertex_count, edge_count;
} CadWorkspaceView;
typedef union CadWorkspaceStage {
    struct {
        CadWorkspace candidate;
        CadXYZ house[CAD_HOUSE_VERTICES];
        CadId ids[CAD_MAX_VERTICES];
        int a[CAD_MAX_EDGES], b[CAD_MAX_EDGES];
    } edit;
    CadWorkspaceView view;
} CadWorkspaceStage;
#ifdef __WATCOMC__
#pragma disable_message(124)
#endif
typedef char CadWorkspaceFitsSegment[(sizeof(CadWorkspace) < 65536UL) ? 1 : -1];
typedef char CadWorkspaceStageFitsSegment[(sizeof(CadWorkspaceStage) < 65536UL) ? 1 : -1];
#ifdef __WATCOMC__
#pragma enable_message(124)
#endif

/* Prototype, one feature per document. Allocate workspace and distinct stage
 * OUTSIDE the stack. Each object is <64KiB on host/Win16; no global scratch in
 * this controller. Stage must not overlap workspace or inputs. Module sketch
 * scratch makes the combined API single-task/non-reentrant; drawing callbacks
 * must not mutate/reenter. Storage is inspectable, READ ONLY after init.
 *
 * Candidate copies are caller-owned, never automatic CadModel variables.
 * Commands publish geometry + evaluated intent + revision together. Model
 * history is internal batching, NOT feature undo: use workspace commands only.
 * Undo, constraints, assemblies and legacy HCMD vertex macros return explicit
 * UNSUPPORTED. No silent partial feature update. A future GUI may use the model
 * for selection, but must not call its mutation/history API on this document.
 *
 * Same kind/count/connectivity preserves all vertex and edge IDs, including
 * height/parameter edits. Replacements delete then allocate fresh monotonic
 * IDs. Creation/replacement counts as a revision even if coordinates match;
 * changing a source to an identical source is a no-op. Revision <=2147483647.
 *
 * Small-model DGROUP must budget sizeof(CadWorkspace)+sizeof(Stage), module
 * scratch, GUI globals, runtime and stack TOGETHER (tests print host sizes).
 * Both near objects together nearly fill/exceed 64KiB before module scratch;
 * a full Win16 application needs a large-data model/segmented allocation and
 * matching module pointer ABI, not two near static objects in GUI DGROUP.
 * -ms object compilation verifies syntax, not a small-model link budget.
 * Measured Watcom 16-bit sizes: workspace 32348, stage 35088, combined
 * 67436 bytes; sketch adds 9474 bytes of static scratch. Controller itself
 * adds only the 120-byte constant house edge table, no mutable globals.
 * Use -ml -zt=1 for segmented large data, consistently across callers and
 * linked modules; the current small-model GUI cannot directly near-allocate
 * these objects. Host LP64 sizes: 39056 / 44024 bytes (ABI dependent).
 */
CadWorkspaceStatus cad_workspace_init(CadWorkspace *workspace);
/* Explicit mapping: flag=1 requires UNSPECIFIED + LEGACY scale; rewrites every
 * default literal's unit to LEGACY without changing ticks. flag=0 requires
 * MM scale and model MM, or INCHES with exactly 2540 ticks/model unit.
 * Never infer mm for an unspecified legacy model. */
CadWorkspaceStatus cad_workspace_create_house(CadWorkspace *workspace,
    CadWorkspaceStage *stage, CadUnits units, CadModelScale scale,
    unsigned flags);
CadWorkspaceStatus cad_workspace_change_house_parameter(CadWorkspace *workspace,
    CadWorkspaceStage *stage, const char *name, const char *expression);
/* Workspace expressions are bounded printable ASCII on one line (spaces OK;
 * tabs/newlines rejected) so the versioned intent format round-trips exactly. */
CadWorkspaceStatus cad_workspace_set_house_value(CadWorkspace *workspace,
    CadWorkspaceStage *stage, const char *name, CadValue value);
/* Profile XY -> workspace X,Y; positive extrusion is along +Z. Units are
 * explicitly supplied; integer profile/height already in those model units. */
CadWorkspaceStatus cad_workspace_create_extrusion(CadWorkspace *workspace,
    CadWorkspaceStage *stage, const CadSketchPoint *profile, int count,
    int height, CadUnits units);
CadWorkspaceStatus cad_workspace_change_height(CadWorkspace *workspace,
    CadWorkspaceStage *stage, int height);
CadWorkspaceStatus cad_workspace_undo(CadWorkspace *workspace);
CadWorkspaceStatus cad_workspace_constraints(CadWorkspace *workspace);
CadWorkspaceStatus cad_workspace_assembly(CadWorkspace *workspace);
CadWorkspaceStatus cad_workspace_macro(CadWorkspace *workspace);
int cad_workspace_modified(const CadWorkspace *workspace);
void cad_workspace_mark_saved(CadWorkspace *workspace);
/* Materializes every endpoint through an ID map, never id-1. Outputs scratch
 * view only on success; export uses actual runtime geometry, OBJ is wireframe
 * v/l (not mesh faces). Export status and drawing status retain module errors. */
CadWorkspaceStatus cad_workspace_materialize(const CadWorkspace *workspace,
    CadWorkspaceView *view);
int cad_workspace_export(const CadWorkspace *workspace, CadWorkspaceStage *stage,
    const char *path, int format, const CadExportOptions *options);
CadDrawingStatus cad_workspace_draw(const CadWorkspace *workspace,
    CadWorkspaceStage *stage, const CadDrawingSheet *sheet,
    const CadDrawingSink *sink, CadDrawingLayout *layout);
/* ASCII CWS 1 feature-intent stream, not native structs or model history/IDs.
 * Read regenerates in stage, replaces current topology with fresh IDs, and
 * marks loaded revision saved. Save flushes/checks errors, leaves modified
 * untouched: caller checks close before mark_saved. Neither closes FILE*.
 * Read rejects unknown version/fields/trailing bytes and is fail-atomic.
 * Snapshot/write failure may leave partial stream data. */
CadWorkspaceStatus cad_workspace_write(const CadWorkspace *workspace, FILE *file);
CadWorkspaceStatus cad_workspace_read(CadWorkspace *workspace,
    CadWorkspaceStage *stage, FILE *file);
#endif
