#ifndef CAD_CREATE_H
#define CAD_CREATE_H

#include "cad_model.h"
#include "cad_sketch.h"

/* C89 / Win16: compile this module and its model/sketch dependencies with
 * -ml -zt=1. Shared static scratch: calls are not reentrant. Initialize the
 * caller's model first. Primitives append independent stable-ID graphs, each
 * as one transaction / undo command; an active transaction is rejected.
 * Coordinates are -400..400, dimensions strictly positive. Rectangles lie
 * in XY; boxes extend along +X,+Y,+Z. Zero-length lines are invalid arguments.
 * Expected input/capacity/ID failures preserve the entire document, including
 * allocators and redo. Unexpected staging failures cancel geometry changes;
 * cancellation deliberately does not rewind IDs (cad_model's safety policy).
 */
CadStatus cad_create_line(CadModel *model, long x1, long y1, long z1,
                          long x2, long y2, long z2);
CadStatus cad_create_rectangle(CadModel *model, long x, long y, long z,
                               long width, long height);
CadStatus cad_create_box(CadModel *model, long x, long y, long z,
                         long width, long height, long depth);

/* Simple implicit-closed XY profile, 3..12 points, base Z=0, height 1..400.
 * Validates the helper's closed triangular mesh but stores only 2*n vertices
 * and 3*n boundary/vertical edges, never internal triangulation diagonals.
 * Invalid profiles return CAD_INVALID_ARGUMENT; range errors return CAD_RANGE.
 */
CadStatus cad_create_extrusion(CadModel *model, const CadSketchPoint *points,
                               int count, int height);

/* Explicit optional sample import, like File Open, NOT an append or startup
 * default. Replaces the document with the house.c fixture (30 vertices,
 * 40 edges), fresh IDs, unspecified units, and empty undo/redo history.
 * Two internal transactions build a private staging model; publication only
 * happens after complete graph validation. Any failure preserves *model.
 * UI selection / saved-file metadata is owned by the caller, not CadModel.
 */
CadStatus cad_create_house_demo(CadModel *model);

#endif
