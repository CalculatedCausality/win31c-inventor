#ifndef CAD_USERPARAMS_H
#define CAD_USERPARAMS_H

/* Inventor-style "Parameters (fx)" user table built on the cad_params
 * expression engine. Portable C89: no allocation, no floating point, all
 * storage caller-owned (one CadUserParams is < 20 KiB on host and Win16;
 * keep it out of the Win16 stack). Not reentrant per table: evaluation uses
 * the table's own scratch area, so even "read" style evaluation calls take a
 * non-const pointer.
 *
 * This header deliberately does not include cad_params.h, so it can be used
 * together with cad_workspace.h / cad_model.h in any include order.
 *
 * Model
 * -----
 * Each parameter is a (name, expression) pair. Expressions use the
 * cad_params grammar: + - * / ( ), numbers with at most two decimals,
 * optional mm/cm/in suffix on literals, and references to other parameters
 * by name (case-insensitive). Types are inferred: a parameter is a LENGTH if
 * its expression evaluates to a length, otherwise a SCALAR. Dimensional rules
 * are strict (cad_params): length +/- scalar is an error; write "Width/2 +
 * 10 mm", not "Width/2 + 10", when Width is a length.
 *
 * Every mutation (add, set_expression, rename, remove, set_units, read_text)
 * re-evaluates the whole candidate table and commits only on success, so the
 * table is always fully evaluated and consistent; on any error it is
 * unchanged (fail-atomic).
 *
 * Document units
 * --------------
 *   CAD_UNITS_UNSPECIFIED: every parameter is a scalar; bare numbers are model
 *       units. Physical literals (mm, cm, in) are rejected with
 *       CAD_UP_UNITS at the offset of the unit suffix.
 *   CAD_UNITS_MM: 1 model unit = 1 mm. Lengths are allowed; cm and in are
 *       converted to mm exactly (inches must be multiples of 0.05 in). Bare
 *       numbers (scalars) are also accepted and mean model units (mm) when
 *       a scalar is used as a coordinate by cad_userparams_eval_expression.
 *   CAD_UNITS_INCHES documents are not supported: every evaluation fails
 *       with CAD_UP_UNITS.
 *
 * IDs are stable nonzero handles for the lifetime of the table (never reused
 * after remove; exhaustion after 65535 adds reports CAD_UP_FULL). They are
 * NOT serialized: read_text numbers parameters 1..n in text order.
 */
#include "cad_model.h"

#define CAD_USERPARAM_MAX 31          /* one cad_params slot is reserved */
#define CAD_USERPARAM_NAME_MAX 31
#define CAD_USERPARAM_EXPR_MAX 128
#define CAD_USERPARAM_TEXT_MAX 4096   /* serialized size limit, enforced */
#define CAD_USERPARAM_DISPLAY_MAX 23
#define CAD_USERPARAM_SCRATCH_BYTES 7168

typedef enum CadUserParamStatus {
    /* The first thirteen values match cad_params.h CadStatus order. */
    CAD_UP_OK = 0, CAD_UP_ARGUMENT, CAD_UP_FULL, CAD_UP_NAME,
    CAD_UP_DUPLICATE, CAD_UP_NOT_FOUND, CAD_UP_EXPRESSION, CAD_UP_NESTING,
    CAD_UP_CYCLE, CAD_UP_DIMENSION, CAD_UP_UNITS, CAD_UP_OVERFLOW,
    CAD_UP_DIV_ZERO,
    CAD_UP_RANGE,     /* eval_expression result outside -400..400 */
    CAD_UP_IN_USE,    /* remove of a parameter referenced by another */
    CAD_UP_CAPACITY,  /* 4096-byte text limit or caller buffer too small */
    CAD_UP_FORMAT     /* malformed serialized line */
} CadUserParamStatus;
typedef enum CadUserParamType { CAD_UP_SCALAR = 0, CAD_UP_LENGTH } CadUserParamType;

typedef struct CadUserParamError {
    CadUserParamStatus status;
    unsigned parameter_id;  /* failing parameter; for CAD_UP_IN_USE the
                               parameter that references the removed one;
                               for a failing add, the ID it would have had;
                               0 for one-off expressions, table and text errors */
    unsigned offset;        /* zero-based byte offset in the expression; for
                               read_text structural errors, offset in line */
    unsigned line;          /* read_text only: 1-based line, else 0 */
    const char *message;    /* static text for status, never null */
} CadUserParamError;

typedef struct CadUserParamEntry {
    unsigned id;
    char name[CAD_USERPARAM_NAME_MAX + 1];
    char expression[CAD_USERPARAM_EXPR_MAX + 1];
    CadUserParamType type;
    long ticks;             /* evaluated value in 1/100 units (mm or scalar) */
} CadUserParamEntry;
typedef struct CadUserParamSet {
    unsigned count, next_id;
    CadUserParamEntry entries[CAD_USERPARAM_MAX];
} CadUserParamSet;
/* Storage exposed only for caller allocation; treat as opaque/read-only. */
typedef struct CadUserParams {
    CadUnits units;
    CadUserParamSet live, candidate;
    union { long align; char bytes[CAD_USERPARAM_SCRATCH_BYTES]; } scratch;
} CadUserParams;

/* Read-only view; pointers stay valid until the next mutation. */
typedef struct CadUserParamInfo {
    unsigned id;
    const char *name;
    const char *expression;
    CadUserParamType type;
    long ticks;                                   /* value * 100 */
    char display[CAD_USERPARAM_DISPLAY_MAX + 1];  /* "200 mm", "0.15", "-2.5" */
} CadUserParamInfo;

void cad_userparams_init(CadUserParams *table, CadUnits units);
/* Changes document units and re-evaluates (e.g. MM -> UNSPECIFIED fails with
 * CAD_UP_UNITS while any expression uses a physical literal). */
CadUserParamStatus cad_userparams_set_units(CadUserParams *table, CadUnits units,
                                            CadUserParamError *error);
CadUnits cad_userparams_units(const CadUserParams *table);
unsigned cad_userparams_count(const CadUserParams *table);
/* Case-insensitive; returns ID or 0. */
unsigned cad_userparams_find(const CadUserParams *table, const char *name);
/* By display order index 0..count-1 (for the list) or by ID. */
CadUserParamStatus cad_userparams_at(const CadUserParams *table, unsigned index,
                                     CadUserParamInfo *info);
CadUserParamStatus cad_userparams_get(const CadUserParams *table, unsigned id,
                                      CadUserParamInfo *info);

/* Names: [A-Za-z_][A-Za-z0-9_]*, at most 31 chars, unique ignoring case.
 * Expressions: 1..128 printable ASCII characters (0x20..0x7E), stored
 * verbatim. id may be null. */
CadUserParamStatus cad_userparams_add(CadUserParams *table, const char *name,
                                      const char *expression, unsigned *id,
                                      CadUserParamError *error);
CadUserParamStatus cad_userparams_set_expression(CadUserParams *table,
    unsigned id, const char *expression, CadUserParamError *error);
CadUserParamStatus cad_userparams_set_expression_named(CadUserParams *table,
    const char *name, const char *expression, CadUserParamError *error);
/* Renames and rewrites every reference in other expressions to the new
 * spelling (unit suffixes such as "2 mm" are never touched). Fails with
 * CAD_UP_CAPACITY if a rewritten expression would exceed 128 characters. */
CadUserParamStatus cad_userparams_rename(CadUserParams *table, unsigned id,
    const char *new_name, CadUserParamError *error);
/* Rejected with CAD_UP_IN_USE (error->parameter_id = first referencing
 * parameter) while any other expression references the parameter. */
CadUserParamStatus cad_userparams_remove(CadUserParams *table, unsigned id,
                                         CadUserParamError *error);
/* Returns the ID of the first parameter whose expression references name
 * (case-insensitive), or 0 if none does. */
unsigned cad_userparams_references(const CadUserParams *table, const char *name);
/* Re-evaluates everything; fail-atomic. Normally a no-op check because every
 * mutation already evaluates. */
CadUserParamStatus cad_userparams_evaluate(CadUserParams *table,
                                           CadUserParamError *error);

/* Evaluates a one-off dialog-field expression such as "Width/2 + 10" against
 * the committed parameter values and the table's document units. The result
 * (scalar or mm length, both meaning model units) is rounded to an integer
 * model coordinate, half away from zero, and must lie in -400..400 (else
 * CAD_UP_RANGE). Errors carry parameter_id 0 and the offset in text.
 * The table is not modified. */
CadUserParamStatus cad_userparams_eval_expression(CadUserParams *table,
    const char *text, long *model_units, CadUserParamError *error);

/* Text form: one line per parameter, in order: "P <name> <expression>\n".
 * The expression is everything after the single separating space up to the
 * newline, verbatim; no escaping is needed because expressions are printable
 * ASCII without newlines. Empty table = empty text. write_text sets *length
 * to the required size even when it fails with CAD_UP_CAPACITY; a NUL is
 * appended only if capacity exceeds the length. */
CadUserParamStatus cad_userparams_write_text(const CadUserParams *table,
    char *buffer, unsigned capacity, unsigned *length);
/* Replaces the table (IDs renumbered 1..n) with text interpreted in the given
 * document units, re-evaluating everything. Fail-atomic; errors carry the
 * 1-based line. Every line must end in "\n"; blank or unknown lines,
 * non-printable bytes, duplicates and evaluation errors are rejected. */
CadUserParamStatus cad_userparams_read_text(CadUserParams *table,
    const char *text, unsigned length, CadUnits units, CadUserParamError *error);

const char *cad_userparams_status_string(CadUserParamStatus status);

#endif
