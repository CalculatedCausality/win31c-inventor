#ifndef CAD_SCRIPT_H
#define CAD_SCRIPT_H

#include <stdio.h>
#include "cad_model.h"

/* Generic CAD.EXE macro language (Tools > Run Macro), executed on a CadModel
 * through cad_create.h. Strict C89, no heap, no floating point. Large model
 * (-ml -zt=1) like cad_create. Uses one private static state (< 1 KiB) plus
 * cad_create's static scratch: NOT reentrant, single task only.
 *
 * Grammar (ASCII, case-sensitive, one statement per physical line):
 *   # comment                (full-line only, may be indented)
 *   CADSCRIPT 1              (first non-blank, non-comment line)
 *   UNITS MM | UNITS NONE
 *   LINE x1 y1 z1 x2 y2 z2
 *   RECT x y z w h
 *   BOX x y z w h d
 *   EXTRUDE h x1,y1 x2,y2 x3,y3 ...   (3..12 points, base Z=0)
 *   MOVEV id x y z           (ABSOLUTE target position, stable vertex ID)
 *   DELV id                  (also deletes incident edges)
 *   DELE id
 *   SET name expr            (script variable; reassignment allowed)
 * Space/tab separate tokens; every numeric argument is ONE token (no spaces
 * inside) holding an integer expression: decimal literals, names, binary
 * + - * /, unary + -, parentheses. Usual precedence, left associative.
 * Division truncates toward zero; division by zero is an error. Every literal,
 * looked-up value and intermediate result must stay within +/-1000000000.
 * Names: [A-Za-z_][A-Za-z0-9_]*, at most 15 chars. Lookup order: script
 * variables (max 16) first, then the optional lookup callback (returns
 * nonzero and stores *value when it knows the name). Unknown names fail.
 * Coordinates are checked against -400..400 and dimensions/heights must be
 * positive (cad_create rules); IDs must be 1..65535.
 *
 * Lines: at most 160 bytes excluding the LF (and the CR of a CRLF); LF or
 * CRLF endings, last line may lack a newline. Tab is the only accepted
 * control byte; NUL, bare CR and non-ASCII are rejected even in comments.
 * At most 1024 statements (SET counts; header/comments/blank lines do not).
 * A header-only script is a successful no-op. Empty input is a header error.
 */
#define CAD_SCRIPT_LINE_MAX 160
#define CAD_SCRIPT_COMMAND_MAX 1024
#define CAD_SCRIPT_VARIABLE_MAX 16
#define CAD_SCRIPT_NAME_MAX 15
#define CAD_SCRIPT_EXTRUDE_MIN 3
#define CAD_SCRIPT_EXTRUDE_MAX 12
#define CAD_SCRIPT_VALUE_LIMIT 1000000000L
#define CAD_SCRIPT_MESSAGE_MAX 64

typedef enum CadScriptStatus {
    CAD_SCRIPT_OK = 0,
    CAD_SCRIPT_ARGUMENT,          /* bad API arguments */
    CAD_SCRIPT_IO,                /* FILE read error */
    CAD_SCRIPT_LINE_TOO_LONG,
    CAD_SCRIPT_CHARACTER,         /* NUL, bare CR, control or non-ASCII */
    CAD_SCRIPT_HEADER,
    CAD_SCRIPT_SYNTAX,            /* unknown command / wrong arg count */
    CAD_SCRIPT_EXPRESSION,        /* malformed expression or name */
    CAD_SCRIPT_UNKNOWN_NAME,
    CAD_SCRIPT_DIVIDE_BY_ZERO,
    CAD_SCRIPT_OVERFLOW,          /* outside +/-CAD_SCRIPT_VALUE_LIMIT */
    CAD_SCRIPT_RANGE,             /* value outside command's domain */
    CAD_SCRIPT_TOO_MANY_COMMANDS,
    CAD_SCRIPT_TOO_MANY_VARIABLES,
    CAD_SCRIPT_MODEL              /* cad_create/cad_model rejected it */
} CadScriptStatus;

typedef int (*CadScriptLookup)(void *context, const char *name, long *value);

typedef struct CadScriptResult {
    CadScriptStatus status;
    unsigned long line;          /* 1-based failing line; 0 = no line */
    CadStatus model_status;      /* detail when status == CAD_SCRIPT_MODEL */
    char message[CAD_SCRIPT_MESSAGE_MAX]; /* short NUL-terminated text */
    /* Statements started, SET included (on failure: including the failing
     * one). On failure history_added and history_truncated are 0. */
    unsigned int commands;
    /* Undo grouping. Each geometry/units statement commits its own
     * cad_model transaction, so on success the script added exactly
     * history_added undo commands on top of live's history (net no-op
     * MOVEV / UNITS add none; creates and deletes add one each). Calling
     * cad_model_undo history_added times undoes the script as a group,
     * UNLESS history_truncated is nonzero: the 32-command / 1024-delta cap
     * then evicted some of the script's own commands, and the script can
     * only be partially undone (prior history was evicted as well). Pure
     * eviction of OLDER commands does not set the flag. Running the script
     * truncates live's redo list as any edit does. */
    unsigned int history_added;
    int history_truncated;
} CadScriptResult;

/* All-or-nothing execution. live must be initialized with no active
 * transaction; stage is caller scratch storage (distinct, not aliased).
 * *stage = *live, every statement runs on stage, and only after the whole
 * script succeeds is *live = *stage. On ANY failure live is untouched (stage
 * content is unspecified). result is required; it is always filled and
 * result->status is returned. Failing statements report their line number.
 * Text: length bytes (need not be NUL-terminated), one segment (< 64 KiB).
 */
CadScriptStatus cad_script_run(const char *text, unsigned int length,
                               CadModel *live, CadModel *stage,
                               CadScriptLookup lookup, void *context,
                               CadScriptResult *result);

/* Same, streaming lines from a caller-owned FILE (opened in binary mode so
 * CRLF reaches the parser). Does not close/rewind; checks ferror. */
CadScriptStatus cad_script_run_file(FILE *input, CadModel *live,
                                    CadModel *stage, CadScriptLookup lookup,
                                    void *context, CadScriptResult *result);

/* Evaluates one expression token (NUL-terminated, no whitespace) using only
 * the lookup callback (no script variables). *value set only on OK. */
CadScriptStatus cad_script_eval(const char *expression,
                                CadScriptLookup lookup, void *context,
                                long *value);

const char *cad_script_status_string(CadScriptStatus status);

#endif
