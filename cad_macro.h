#ifndef CAD_MACRO_H
#define CAD_MACRO_H

#include <stdio.h>

#define CAD_MACRO_VERTICES 30
#define CAD_MACRO_COORDS 90
#define CAD_MACRO_LINE_MAX 160
#define CAD_MACRO_COMMAND_MAX 1024
#define CAD_MACRO_COORD_LIMIT 400

typedef enum CadMacroStatus {
    CAD_MACRO_OK = 0,
    CAD_MACRO_ARGUMENT,
    CAD_MACRO_IO,
    CAD_MACRO_LINE_TOO_LONG,
    CAD_MACRO_ASCII,
    CAD_MACRO_HEADER,
    CAD_MACRO_SYNTAX,
    CAD_MACRO_NUMBER,
    CAD_MACRO_RANGE,
    CAD_MACRO_TOO_MANY,
    CAD_MACRO_UNSUPPORTED
} CadMacroStatus;

typedef enum CadMacroOp {
    CAD_MACRO_RESET = 1,
    CAD_MACRO_SETV,
    CAD_MACRO_MOVEV
} CadMacroOp;

typedef struct CadMacroCommand {
    unsigned long line;
    CadMacroOp op;
    int id;
    int x, y, z;
} CadMacroCommand;

typedef struct CadMacroProgram {
    unsigned int count;
    CadMacroCommand commands[CAD_MACRO_COMMAND_MAX];
} CadMacroProgram;

typedef struct CadMacroError {
    CadMacroStatus status;
    unsigned long line;
    unsigned int command;
} CadMacroError;

/* Called in source order ONLY after successful validation and model commit.
 * canonical has no newline; xyz is the staged snapshot after that command.
 * Pointers are borrowed for this call only. Notifications cannot veto commit.
 * The callback must not modify the program/model or reenter these functions.
 */
typedef void (*CadMacroCallback)(void *context, const char *canonical,
                                const int *xyz, unsigned int count);

/* ASCII grammar, case-sensitive; one header/command per physical line:
 *   HCMD 1
 *   RESET HOUSE
 *   SETV id x y z
 *   MOVEV id dx dy dz
 * IDs are decimal 0..29; signed decimal integers are -32768..32767,
 * independent of host int width. SETV and resulting MOVEV coordinates must
 * be -400..400. Optional +/- and leading zeroes are accepted.
 * Space/tab separate tokens. Blank lines and full-line # comments (possibly
 * indented) may occur anywhere. No inline comments, nesting, other commands,
 * paths, UI state, locale-dependent tokens, or arbitrary code are supported.
 * LF and CRLF are accepted, as is a final line without newline. Bare CR,
 * NUL, non-ASCII and control bytes other than tab/newline are rejected, even
 * in comments. At most 160 bytes before LF (excluding CR in CRLF), including
 * comments/blank lines; at most 1024 commands (header/comments don't count).
 * Header-only input is a successful no-op. Empty input is invalid.
 *
 * Return status equals error->status when error is non-NULL. Error line is
 * one-based, command is one-based excluding the header; zero means argument
 * or header context. Success clears the error. A malformed command consumes
 * its ordinal. Reader checks ferror, does not close/rewind/clear the caller's
 * FILE, and leaves the entire output program untouched on failure.
 * Read uses a static staging program to avoid a large Win16 stack: all APIs
 * are single-task, non-reentrant. Keep programs in static/heap storage too.
 */
CadMacroStatus CadMacroRead(FILE *input, CadMacroProgram *output,
                            CadMacroError *error);

/* Requires exactly 90 valid input coordinates. Even manually constructed
 * programs are validated. Executes on a staged 90-int model, then commits
 * only if ALL commands succeed. Failure leaves xyz untouched and makes no
 * callbacks. RESET HOUSE uses the fixed houseInit order from house.c.
 */
CadMacroStatus CadMacroExecute(const CadMacroProgram *program, int *xyz,
                               unsigned int count, CadMacroCallback callback,
                               void *context, CadMacroError *error);

/* Canonical replayable snapshot: HCMD 1 followed by 30 SETV lines. Validates
 * the whole model before writing. Caller owns FILE and must check its eventual
 * close; this function checks writes/ferror and fflush. IO failure may leave
 * partial stream output. No paths are accepted or opened by any API.
 */
CadMacroStatus CadMacroSnapshot(FILE *output, const int *xyz,
                                unsigned int count, CadMacroError *error);

#endif
