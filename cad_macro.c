#include "cad_macro.h"
#include <limits.h>
#include <string.h>

static CadMacroProgram read_stage;
static const int house[CAD_MACRO_COORDS] = {
    -100,-70,70, 100,-70,70, 100,-70,-70, -100,-70,-70,
    -100,30,70, 100,30,70, 100,30,-70, -100,30,-70,
    -100,100,0, 100,100,0,
    -15,-70,70, -15,0,70, 15,0,70, 15,-70,70,
    -75,-30,70, -35,-30,70, -35,10,70, -75,10,70,
    35,-30,70, 75,-30,70, 75,10,70, 35,10,70,
    40,55,-45, 65,55,-45, 65,75,-25, 40,75,-25,
    40,120,-45, 65,120,-45, 65,120,-25, 40,120,-25
};

static CadMacroStatus report(CadMacroError *e, CadMacroStatus status,
                             unsigned long line, unsigned int command)
{
    if (e) {
        e->status = status;
        e->line = line;
        e->command = command;
    }
    return status;
}

/* Reads at most 162 bytes, so even an unbounded comment fails promptly.
 * eof distinguishes a real empty line from clean end-of-stream.
 */
static CadMacroStatus read_line(FILE *f, char *s, int *eof)
{
    unsigned int n;
    int c, next;
    n = 0;
    *eof = 0;
    for (;;) {
        c = fgetc(f);
        if (c == EOF) {
            if (ferror(f)) return CAD_MACRO_IO;
            *eof = n == 0;
            break;
        }
        if (c == '\n') break;
        if (c == '\r') {
            next = fgetc(f);
            if (ferror(f)) return CAD_MACRO_IO;
            if (next != '\n') return CAD_MACRO_ASCII;
            break;
        }
        if ((c < 32 && c != '\t') || c > 126)
            return CAD_MACRO_ASCII;
        if (n == CAD_MACRO_LINE_MAX) return CAD_MACRO_LINE_TOO_LONG;
        s[n++] = (char)c;
    }
    s[n] = '\0';
    return CAD_MACRO_OK;
}

/* At most six tokens: five for SETV/MOVEV and one to detect trailing junk. */
static unsigned int tokens(char *s, char **words)
{
    unsigned int n;
    n = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t') ++s;
        if (!*s) break;
        if (n == 6) return 6;
        words[n++] = s;
        while (*s && *s != ' ' && *s != '\t') ++s;
        if (*s) *s++ = '\0';
    }
    return n;
}

static CadMacroStatus number(const char *s, int *out)
{
    unsigned int value, digit, limit;
    int negative;
    negative = *s == '-';
    if (*s == '-' || *s == '+') ++s;
    if (!*s) return CAD_MACRO_NUMBER;
    value = 0;
    limit = negative ? 32768U : 32767U;
    while (*s) {
        if (*s < '0' || *s > '9') return CAD_MACRO_NUMBER;
        digit = (unsigned int)(*s++ - '0');
        if (value > (limit - digit) / 10U) return CAD_MACRO_NUMBER;
        value = value * 10U + digit;
    }
    if (negative && value == 32768U) *out = -32767 - 1;
    else *out = negative ? -(int)value : (int)value;
    return CAD_MACRO_OK;
}

static CadMacroStatus valid_command(const CadMacroCommand *c)
{
    if (c->op == CAD_MACRO_RESET)
        return c->id || c->x || c->y || c->z ?
               CAD_MACRO_SYNTAX : CAD_MACRO_OK;
    if (c->op != CAD_MACRO_SETV && c->op != CAD_MACRO_MOVEV)
        return CAD_MACRO_UNSUPPORTED;
    if (c->id < 0 || c->id >= CAD_MACRO_VERTICES)
        return CAD_MACRO_RANGE;
    if ((long)c->x < -32768L || (long)c->x > 32767L ||
        (long)c->y < -32768L || (long)c->y > 32767L ||
        (long)c->z < -32768L || (long)c->z > 32767L)
        return CAD_MACRO_NUMBER;
    if (c->op == CAD_MACRO_SETV &&
        (c->x < -400 || c->x > 400 || c->y < -400 || c->y > 400 ||
         c->z < -400 || c->z > 400)) return CAD_MACRO_RANGE;
    return CAD_MACRO_OK;
}

CadMacroStatus CadMacroRead(FILE *input, CadMacroProgram *output,
                            CadMacroError *error)
{
    char line[CAD_MACRO_LINE_MAX + 1], *w[6], *p;
    unsigned long lineno;
    unsigned int n, ordinal;
    int eof, header;
    CadMacroCommand *c;
    CadMacroStatus status;
    if (!input || !output)
        return report(error, CAD_MACRO_ARGUMENT, 0, 0);
    memset(&read_stage, 0, sizeof(read_stage));
    lineno = 0;
    header = 0;
    for (;;) {
        ordinal = header ? read_stage.count + 1U : 0;
        if (lineno == ULONG_MAX)
            return report(error, CAD_MACRO_TOO_MANY, lineno, ordinal);
        status = read_line(input, line, &eof);
        ++lineno;
        if (status != CAD_MACRO_OK)
            return report(error, status, lineno, ordinal);
        if (eof) break;
        p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p || *p == '#') continue;
        n = tokens(p, w);
        if (!header) {
            if (n != 2 || strcmp(w[0], "HCMD") || strcmp(w[1], "1"))
                return report(error, CAD_MACRO_HEADER, lineno, 0);
            header = 1;
            continue;
        }
        if (read_stage.count == CAD_MACRO_COMMAND_MAX)
            return report(error, CAD_MACRO_TOO_MANY, lineno, ordinal);
        c = &read_stage.commands[read_stage.count];
        c->line = lineno;
        if (!strcmp(w[0], "RESET")) {
            if (n != 2 || strcmp(w[1], "HOUSE"))
                return report(error, CAD_MACRO_SYNTAX, lineno, ordinal);
            c->op = CAD_MACRO_RESET;
        } else if (!strcmp(w[0], "SETV") || !strcmp(w[0], "MOVEV")) {
            if (n != 5)
                return report(error, CAD_MACRO_SYNTAX, lineno, ordinal);
            c->op = !strcmp(w[0], "SETV") ? CAD_MACRO_SETV : CAD_MACRO_MOVEV;
            status = number(w[1], &c->id);
            if (status == CAD_MACRO_OK) status = number(w[2], &c->x);
            if (status == CAD_MACRO_OK) status = number(w[3], &c->y);
            if (status == CAD_MACRO_OK) status = number(w[4], &c->z);
            if (status != CAD_MACRO_OK)
                return report(error, status, lineno, ordinal);
        } else return report(error, CAD_MACRO_UNSUPPORTED, lineno, ordinal);
        status = valid_command(c);
        if (status != CAD_MACRO_OK)
            return report(error, status, lineno, ordinal);
        ++read_stage.count;
    }
    if (!header) return report(error, CAD_MACRO_HEADER, lineno, 0);
    memcpy(output, &read_stage, sizeof(read_stage));
    return report(error, CAD_MACRO_OK, 0, 0);
}

static int valid_model(const int *xyz, unsigned int count)
{
    unsigned int i;
    if (!xyz || count != CAD_MACRO_COORDS) return 0;
    for (i = 0; i < count; ++i)
        if (xyz[i] < -400 || xyz[i] > 400) return 0;
    return 1;
}

static CadMacroStatus apply(const CadMacroCommand *c, int *xyz)
{
    long values[3];
    unsigned int at, i;
    CadMacroStatus status;
    status = valid_command(c);
    if (status != CAD_MACRO_OK) return status;
    if (c->op == CAD_MACRO_RESET) {
        memcpy(xyz, house, sizeof(house));
        return CAD_MACRO_OK;
    }
    at = (unsigned int)c->id * 3U;
    values[0] = c->x; values[1] = c->y; values[2] = c->z;
    for (i = 0; i < 3; ++i) {
        if (c->op == CAD_MACRO_MOVEV) values[i] += (long)xyz[at + i];
        if (values[i] < -400L || values[i] > 400L) return CAD_MACRO_RANGE;
    }
    for (i = 0; i < 3; ++i) xyz[at + i] = (int)values[i];
    return CAD_MACRO_OK;
}

CadMacroStatus CadMacroExecute(const CadMacroProgram *program, int *xyz,
                               unsigned int count, CadMacroCallback callback,
                               void *context, CadMacroError *error)
{
    int staged[CAD_MACRO_COORDS], replay[CAD_MACRO_COORDS];
    unsigned int i;
    char canonical[64];
    const CadMacroCommand *c;
    CadMacroStatus status;
    if (!program || !valid_model(xyz, count))
        return report(error, CAD_MACRO_ARGUMENT, 0, 0);
    if (program->count > CAD_MACRO_COMMAND_MAX)
        return report(error, CAD_MACRO_TOO_MANY, 0, 0);
    memcpy(staged, xyz, sizeof(staged));
    for (i = 0; i < program->count; ++i) {
        c = &program->commands[i];
        status = apply(c, staged);
        if (status != CAD_MACRO_OK)
            return report(error, status, c->line, i + 1U);
    }
    if (callback) memcpy(replay, xyz, sizeof(replay));
    memcpy(xyz, staged, sizeof(staged));
    if (callback) {
        for (i = 0; i < program->count; ++i) {
            c = &program->commands[i];
            (void)apply(c, replay);
            if (c->op == CAD_MACRO_RESET) strcpy(canonical, "RESET HOUSE");
            else sprintf(canonical, "%s %d %d %d %d",
                         c->op == CAD_MACRO_SETV ? "SETV" : "MOVEV",
                         c->id, c->x, c->y, c->z);
            callback(context, canonical, replay, CAD_MACRO_COORDS);
        }
    }
    return report(error, CAD_MACRO_OK, 0, 0);
}

CadMacroStatus CadMacroSnapshot(FILE *output, const int *xyz,
                                unsigned int count, CadMacroError *error)
{
    unsigned int i;
    if (!output || !valid_model(xyz, count))
        return report(error, CAD_MACRO_ARGUMENT, 0, 0);
    if (ferror(output) || fputs("HCMD 1\n", output) == EOF)
        return report(error, CAD_MACRO_IO, 1, 0);
    for (i = 0; i < CAD_MACRO_VERTICES; ++i) {
        if (fprintf(output, "SETV %u %d %d %d\n", i,
                    xyz[i * 3U], xyz[i * 3U + 1U], xyz[i * 3U + 2U]) < 0)
            return report(error, CAD_MACRO_IO, (unsigned long)i + 2UL, i + 1U);
    }
    if (fflush(output) != 0 || ferror(output))
        return report(error, CAD_MACRO_IO, 31, 30);
    return report(error, CAD_MACRO_OK, 0, 0);
}
