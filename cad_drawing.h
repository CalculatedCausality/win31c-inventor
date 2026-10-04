#ifndef CAD_DRAWING_H
#define CAD_DRAWING_H

/* Portable C89, including 16-bit int / 32-bit long targets. No GDI types.
 * All long inputs must fit signed 32 bits even on a 64-bit-long host.
 * Arrays must contain count elements; counts are bounded for Win16 segments.
 * No allocation, global state, preview camera, hidden-line removal or sections.
 */
#define CAD_DRAWING_MAX_VERTICES 1024
#define CAD_DRAWING_MAX_EDGES 4096
#define CAD_DRAWING_I32_MAX 2147483647L
#define CAD_DRAWING_I32_MIN (-2147483647L - 1L)

typedef enum {
    CAD_DRAWING_OK = 0, CAD_DRAWING_INVALID, CAD_DRAWING_RANGE,
    CAD_DRAWING_OVERFLOW, CAD_DRAWING_NO_ROOM, CAD_DRAWING_SINK_FAILED
} CadDrawingStatus;

/* Y-up bases: front=(x,y), top=(x,-z), right=(-z,y),
 * left=(z,y), rear=(-x,y), bottom=(x,z). Sheet Y increases downward.
 */
typedef enum {
    CAD_DRAWING_FRONT = 0, CAD_DRAWING_TOP, CAD_DRAWING_RIGHT,
    CAD_DRAWING_LEFT, CAD_DRAWING_REAR, CAD_DRAWING_BOTTOM
} CadDrawingView;

typedef struct { long x, y, z; } CadDrawingPoint;
typedef struct { long u, v; } CadDrawingUV;
typedef struct { int a, b; } CadDrawingEdge;
typedef struct {
    const CadDrawingPoint *vertices;
    int vertex_count;
    const CadDrawingEdge *edges;
    int edge_count;
} CadDrawingModel;
typedef struct {
    CadDrawingPoint min, max;
    long width, height, depth;
} CadDrawingBounds;
typedef struct { long numerator, denominator; } CadDrawingScale;
typedef struct { long left, top, right, bottom; } CadDrawingRect;
typedef struct {
    /* Logical sheet units, NOT pixels. Each dimension <=32767, margin>=8,
     * gap>=32. Annotations reserve fixed logical spacing; choose sink font
     * metrics to fit it. A sheet too small is rejected, never auto-fitted.
     */
    long width, height, margin, gap;
    CadDrawingScale scale;
} CadDrawingSheet;
typedef struct {
    CadDrawingBounds model_bounds;
    CadDrawingScale scale;
    CadDrawingRect sheet_bounds;
    /* Indexed FRONT, TOP, RIGHT. Shared X/Y datum, not separate centering.
     * Top above front; right to its right (third-angle arrangement).
     */
    CadDrawingRect views[3];
    long title_top;
    int background_white, foreground_black;
} CadDrawingLayout;

#define CAD_DRAWING_SOLID       0x0001U
#define CAD_DRAWING_MODEL_LINE  0x0002U
#define CAD_DRAWING_BORDER      0x0004U
#define CAD_DRAWING_DIMENSION   0x0008U
#define CAD_DRAWING_LABEL       0x0010U
#define CAD_DRAWING_TITLE       0x0020U
#define CAD_DRAWING_EXTENSION   0x0040U

typedef struct {
    void *context;
    int (*line)(void *context, long x1, long y1, long x2, long y2,
                unsigned int flags);
    int (*text)(void *context, long x, long y, const char *text,
                unsigned int flags);
} CadDrawingSink;

/* Outputs are untouched on error. Negation, extent subtraction, scaling
 * products and sheet sums are checked against signed 32 bits BEFORE use.
 * Scale is positive; scale(value)=floor(value*numerator/denominator) for
 * nonnegative distances. An overflowing intermediate product is rejected
 * even if division would fit. Positive sub-unit features can collapse.
 */
CadDrawingStatus CadDrawingProject(CadDrawingView view,
    const CadDrawingPoint *point, CadDrawingUV *out);
CadDrawingStatus CadDrawingGetBounds(const CadDrawingModel *model,
    CadDrawingBounds *out);
CadDrawingStatus CadDrawingMakeLayout(const CadDrawingModel *model,
    const CadDrawingSheet *sheet, CadDrawingLayout *out);
/* Recomputes/validates layout and all geometry before the first callback.
 * Sink paints a white sheet with black primitives using returned layout.
 * Text coordinates are anchors, not glyph extents. Strings are borrowed for
 * the callback only. Return nonzero to continue, zero to abort (partial
 * output then possible). Inputs must remain unchanged during callbacks.
 * out may be NULL; otherwise written only on complete success.
 * Dimensions are overall model bounds, labelled in unspecified model units.
 */
CadDrawingStatus CadDrawingRender(const CadDrawingModel *model,
    const CadDrawingSheet *sheet, const CadDrawingSink *sink,
    CadDrawingLayout *out);

#endif
