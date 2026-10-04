#ifndef CAD_FEATUREUI_H
#define CAD_FEATUREUI_H

/* CAD.EXE glue between the editor (cad.c) and the parametric feature tree
 * (cad_features.h). This module owns the large feature storage (document,
 * regenerated output, saved/scratch text, a 4-step feature undo ring) so it
 * stays out of cad.c's nearly full far data segments.
 *
 * The portable part (everything below except the __WINDOWS__ block) is C89
 * with no Windows headers and links into the host controller test. It is NOT
 * reentrant (single editor instance).
 *
 * Dialogs work through one in/out record, cad_fui_dialog: prepare() fills it
 * for a new or existing feature, the native dialog (or a test) edits its
 * strings, apply() validates and commits it. apply() is all-or-nothing:
 * the edit is committed only if it regenerates without a NEW error on the
 * edited feature; otherwise the previous tree is restored and message tells
 * why (the dialog stays open). Later parameter changes can still put
 * features into error; the browser shows that ("(!) ...").
 *
 * Feature edits are document state (dirty, saved in [FEATURES]) but are not
 * model undo steps. Instead the last CAD_FUI_UNDO_DEPTH feature edits can be
 * reverted with cad_fui_undo() (cleared by New/Open). */
#include "cad_features.h"
#include "cad_export.h"
#include "cad_drawing.h"

#define CAD_FUI_UNDO_DEPTH 4
#define CAD_FUI_MESSAGE 200
#define CAD_FUI_POINTS_MAX CAD_FEAT_PROFILE_TEXT   /* polygon text field */

/* Dialog kinds (cad_fui_dialog.kind). */
#define CAD_FUI_SKETCH 1
#define CAD_FUI_RECT 2
#define CAD_FUI_CIRCLE 3
#define CAD_FUI_POLY 4
#define CAD_FUI_EXTRUDE 5
#define CAD_FUI_REVOLVE 6
#define CAD_FUI_RENAME 7

/* Field use per kind:
 *   SKETCH   option = plane 0..2 (XY/XZ/YZ); fields[0] = offset
 *   RECT     fields[0..3] = cx, cy, w, h
 *   CIRCLE   fields[0..2] = cx, cy, r; fields[3] = sides (8..32)
 *   POLY     points = "x,y;x,y;..." (3..16 points, expressions)
 *   EXTRUDE  sketch; fields[0] = profile ("ALL" or 1-based number);
 *            fields[1] = distance; option = direction 0..2 (+, -, symmetric)
 *   REVOLVE  sketch; fields[0] = profile; fields[1] = segments (12..24);
 *            option = axis 0..1 (U, V)
 *   RENAME   fields[0] = name
 * target = feature being edited (0 = new); for profiles target = sketch ID
 * and profile = index being edited (-1 = new). */
typedef struct CadFuiDialog {
    int kind;
    unsigned target;
    int profile;
    unsigned sketch;
    int option;
    char fields[4][CAD_FEAT_EXPR_MAX + 1];
    char points[CAD_FUI_POINTS_MAX + 1];
    char title[48];
    char message[CAD_FUI_MESSAGE];
    /* apply() output: new feature ID (sketch ID for a new profile) and new
     * profile index (-1 if none); 0/-1 for edits. */
    unsigned created;
    int created_profile;
} CadFuiDialog;

extern CadFuiDialog cad_fui_dialog;

/* Binds the parameter table used for evaluation and empties everything. */
void cad_fui_init(CadUserParams *params);
/* Empty feature tree, no sketch mode, undo ring cleared (New/demo). */
void cad_fui_reset(void);
const CadFeatures *cad_fui_features(void);
const CadFeatOutput *cad_fui_output(void);
/* Re-evaluates every feature against the bound parameters; returns and
 * remembers the ERROR+BLOCKED count. Call after any feature, parameter,
 * unit or document change. */
unsigned cad_fui_regenerate(void);
unsigned cad_fui_errors(void);
unsigned cad_fui_count(void);
int cad_fui_bounds(long min[3], long max[3]);

/* Sketch mode: the sketch receiving Rectangle/Circle/Polygon (0 = none).
 * cad_fui_validate() clears it (and returns 0) if that sketch vanished. */
unsigned cad_fui_active_sketch(void);
void cad_fui_set_active_sketch(unsigned id);
unsigned cad_fui_validate(void);
/* Last sketch in the list (default for Extrude/Revolve), or 0. */
unsigned cad_fui_last_sketch(void);
/* Name of a feature or "" (pointer valid until the next mutation). */
const char *cad_fui_name(unsigned id);
CadFeatType cad_fui_type(unsigned id);

/* Fills cad_fui_dialog. kind 1..7 with target 0 starts a new item (new
 * profiles go to the active sketch); RENAME needs target. kind 0 edits the
 * existing feature target (profile < 0) or profile `profile` of sketch
 * target, choosing the matching dialog kind. Returns 0 with message set if
 * the command is not applicable (e.g. no active sketch for a new profile). */
int cad_fui_prepare(int kind, unsigned target, int profile);
/* Nth (0-based) sketch selectable in the current Extrude/Revolve dialog
 * (sketches preceding the edited feature), or 0 past the end. */
unsigned cad_fui_sketch_choice(unsigned n);
/* Validates and commits cad_fui_dialog; returns 1 (created/created_profile
 * set), or 0 with message. A new sketch enters sketch mode. */
int cad_fui_apply(void);

/* Browser commands; each returns 1, or 0 with message (<= CAD_FUI_MESSAGE
 * bytes incl. NUL). profile >= 0 addresses a profile of sketch id. */
int cad_fui_remove(unsigned id, int profile, char *message);
int cad_fui_toggle_suppress(unsigned id, char *message);
int cad_fui_move(unsigned id, int up, char *message);
int cad_fui_can_undo(void);
int cad_fui_undo(char *message);

/* Browser rows after the "Part" header, in tree order: every feature, then
 * each of a sketch's profiles indented ("  Rectangle 1"). Health markers
 * lead the row so a narrow browser still shows them: "(!) ", "(blocked) ",
 * "(suppressed) "; an ERROR row ends with " - <field>: <message>.", a failing
 * profile reads "  (!) Rectangle 1"; the active sketch has " [sketch mode]".
 * Returns 0 past the last row. */
int cad_fui_row(unsigned row, char *text, unsigned capacity,
                unsigned *feature, int *profile);

/* Serialized tree (no header). Returns the length (NUL appended), or 0 for
 * an empty tree (no [FEATURES] section is written). If the text does not fit
 * capacity, returns 0 and sets *overflow (may be null). */
unsigned cad_fui_text(char *buffer, unsigned capacity, int *overflow);
/* Replaces the tree (fail-atomic); ends sketch mode, clears the undo ring.
 * 0 with message "[FEATURES] line n: ..." on error. */
int cad_fui_read(const char *text, unsigned length, char *message);
/* backup() snapshots the current tree; restore() puts it back exactly (used
 * when a later document section fails to load). */
void cad_fui_backup(void);
void cad_fui_restore(void);
void cad_fui_mark_saved(void);
int cad_fui_dirty(void);

/* Appends regenerated body edges (not sketch curves) to export/drawing
 * arrays already holding *vertex_count / *edge_count entries, sharing
 * vertices within a feature. Stops at max_vertices / max_edges; returns the
 * number of body edges that did not fit (0 = complete). Either array pair
 * may be null. */
int cad_fui_materialize(CadExportVertex *export_vertices, CadDrawingPoint *points,
    int *vertex_count, int max_vertices, CadExportEdge *export_edges,
    CadDrawingEdge *edges, int *edge_count, int max_edges);

/* /UITEST second telemetry word (24 payload bits + checksum):
 *  bits 0-4 feature count, 5-9 ERROR+BLOCKED count, 10 sketch mode,
 *  11 feature faces were shaded, 12-21 X extent of the feature bounds
 *  (max-min, saturating at 1023, 0 without bounds), 22 output truncated,
 *  23 has bounds; checksum (p ^ p>>8 ^ p>>16 ^ 0x5c) & 255 in bits 24-31. */
unsigned long cad_fui_telemetry(int shaded);

#ifdef __WINDOWS__
/* One dialog procedure for IDD_FEAT_SKETCH/PROFILE/SOLID/RENAME. */
BOOL FAR PASCAL __export FeatureDlg(HWND dlg, UINT msg, WPARAM wp, LPARAM lp);
/* Paints the feature faces through cad_meshview_render_orthographic with the
 * editor camera; flip mirrors an upside-down view (see cad.c). Returns TRUE
 * if any face was drawn. */
BOOL cad_fui_draw_shaded(HDC dc, const CadMeshViewCamera *camera,
    const CadMeshViewOrtho *ortho, BOOL flip);
void cad_fui_free_brushes(void);
#endif

#endif
