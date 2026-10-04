#ifndef CAD_PARAMS_H
#define CAD_PARAMS_H

/* Portable C89, no allocation, no floating point, no 64-bit arithmetic. */
#define CAD_PARAM_MAX 32
#define CAD_NAME_MAX 31
#define CAD_EXPR_MAX 128
#define CAD_NEST_MAX 16
#define CAD_SCALE 100L
#define CAD_LONG_MAX 2147483647L
#define CAD_LONG_MIN (-2147483647L - 1L)
#define CAD_HOUSE_VERTICES 30
#define CAD_MODEL_LIMIT 400

typedef enum { CAD_SCALAR, CAD_LENGTH } CadType;
typedef enum { CAD_UNIT_NONE, CAD_UNIT_MM, CAD_UNIT_LEGACY } CadUnit;
typedef enum {
    CAD_OK, CAD_ERR_ARGUMENT, CAD_ERR_FULL, CAD_ERR_NAME,
    CAD_ERR_DUPLICATE, CAD_ERR_NOT_FOUND, CAD_ERR_EXPRESSION,
    CAD_ERR_NESTING, CAD_ERR_CYCLE, CAD_ERR_DIMENSION, CAD_ERR_UNITS,
    CAD_ERR_OVERFLOW, CAD_ERR_DIV_ZERO, CAD_ERR_GEOMETRY
} CadStatus;

typedef struct { long ticks; CadType type; CadUnit unit; } CadValue;
typedef struct {
    unsigned id;
    char name[CAD_NAME_MAX + 1];
    char expression[CAD_EXPR_MAX + 1];
    CadType type;
    CadValue literal;             /* source when expression is empty */
    CadValue value;               /* last successfully evaluated value */
} CadParameter;
typedef struct { unsigned count; CadParameter params[CAD_PARAM_MAX]; } CadParams;
typedef struct {
    CadStatus status;
    unsigned parameter_id;        /* failing expression, zero for table errors */
    unsigned offset;              /* zero-based byte offset in expression */
} CadError;
typedef struct { int x, y, z; } CadXYZ;
typedef struct {
    CadUnit unit;                 /* MM or LEGACY, never inferred */
    long ticks_per_model_unit;    /* positive; 100 means one source unit */
} CadModelScale;

void cad_params_init(CadParams *table);
unsigned cad_params_find(const CadParams *table, const char *name);
const CadParameter *cad_params_get(const CadParams *table, unsigned id);
/* IDs are append-only, stable, 1..32. No deletion or renumbering. */
CadStatus cad_params_add(CadParams *table, const char *name, CadValue literal,
                         unsigned *id);
/* Edits change sources only; committed values remain until evaluate succeeds. */
CadStatus cad_params_set_value(CadParams *table, unsigned id, CadValue value);
CadStatus cad_params_set_expression(CadParams *table, unsigned id,
                                    const char *expression);
/* Bare literals are scalars. mm, cm, in are physical lengths (25.4 mm/in).
 * At most two fractional digits; conversions must be exact at scale 100.
 * All operations truncate toward zero; intermediate results obey signed32.
 * L/L -> scalar; L*S, S*L, L/S -> length; S/L and L*L rejected.
 * Legacy lengths can be seeded with set_value but never silently become mm.
 */
CadStatus cad_params_evaluate(CadParams *table, CadError *error);
const char *cad_status_string(CadStatus status);

/* Initializes an authoritative physical-mm preset matching house.c exactly
 * at scale {CAD_UNIT_MM,100}. WindowSill is relative to BaseY; WindowCenter
 * is the positive center of two mirrored windows. ChimneyBack is a positive
 * distance behind the ridge; ChimneyDepth runs toward the ridge; ChimneyHeight
 * is measured above the higher roof intersection, giving a level top.
 */
void cad_house_defaults(CadParams *table);
/* Evaluates staged parameters, validates geometry, then commits both values
 * and all 30 vertices. On failure neither values nor output change. Explicit
 * scale converts ticks to integer model coordinates, truncating toward zero.
 */
CadStatus cad_house_generate(CadParams *table, CadModelScale scale,
                             CadXYZ output[CAD_HOUSE_VERTICES], CadError *error);

#endif
