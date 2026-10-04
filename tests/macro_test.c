/* Host: cc -std=c89 -pedantic -Wall -Wextra -Werror tests/macro_test.c
 *          -o /tmp/opencode/macro_test
 * Win16 compile: wcc -q -bt=windows -zW -ms -2 -w4 -wx cad_macro.c
 * Implementation included solely for deterministic stream fault injection.
 * All program storage is static, including on a 16-bit-int target.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdarg.h>

static long read_budget = -1;
static int injected_error, flush_fail, write_fail, formatted_fail;
static int test_getc(FILE *f)
{
    if (read_budget == 0) {
        injected_error = 1;
        return EOF;
    }
    if (read_budget > 0) --read_budget;
    return fgetc(f);
}
static int test_error(FILE *f)
{
    return injected_error || ferror(f);
}
static int test_flush(FILE *f)
{
    if (flush_fail) return EOF;
    return fflush(f);
}
static int test_puts(const char *s, FILE *f)
{
    if (write_fail) return EOF;
    return fputs(s, f);
}
static int test_print(FILE *f, const char *format, ...)
{
    va_list args;
    int rc;
    if (formatted_fail) return -1;
    va_start(args, format);
    rc = vfprintf(f, format, args);
    va_end(args);
    return rc;
}
#define fgetc test_getc
#ifdef ferror
#undef ferror
#endif
#define ferror test_error
#define fflush test_flush
#define fputs test_puts
#define fprintf test_print
#include "../cad_macro.c"
#undef fgetc
#undef ferror
#undef fflush
#undef fputs
#undef fprintf

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); \
} } while (0)

static CadMacroProgram program, saved;
static CadMacroError error;
static int model[90], original[90], result[90];
static unsigned int calls;
static char notifications[3][64];
static int snapshots[3][90];
static const int expected_house[90] = {
    -100,-70,70, 100,-70,70, 100,-70,-70, -100,-70,-70,
    -100,30,70, 100,30,70, 100,30,-70, -100,30,-70,
    -100,100,0, 100,100,0,
    -15,-70,70, -15,0,70, 15,0,70, 15,-70,70,
    -75,-30,70, -35,-30,70, -35,10,70, -75,10,70,
    35,-30,70, 75,-30,70, 75,10,70, 35,10,70,
    40,55,-45, 65,55,-45, 65,75,-25, 40,75,-25,
    40,120,-45, 65,120,-45, 65,120,-25, 40,120,-25
};

static FILE *stream(const char *text)
{
    FILE *f;
    size_t n;
    f = tmpfile(); CHECK(f != NULL);
    n = strlen(text);
    CHECK(fwrite(text, 1, n, f) == n);
    CHECK(fflush(f) == 0); rewind(f);
    return f;
}

static CadMacroStatus parse(const char *text)
{
    FILE *f;
    CadMacroStatus status;
    f = stream(text);
    status = CadMacroRead(f, &program, &error);
    /* FILE remains open and caller-owned, even following failure. */
    CHECK(fseek(f, 0L, SEEK_SET) == 0);
    CHECK(fclose(f) == 0);
    return status;
}

static void bad(const char *text, CadMacroStatus status,
                unsigned long line, unsigned int command)
{
    CadMacroStatus actual;
    memcpy(&saved, &program, sizeof(saved));
    actual = parse(text);
    if (actual != status)
        fprintf(stderr, "input [%s]: status %d, expected %d\n",
                text, (int)actual, (int)status);
    CHECK(actual == status);
    CHECK(error.status == status && error.line == line &&
          error.command == command);
    CHECK(!memcmp(&saved, &program, sizeof(saved)));
}

static void notify(void *context, const char *canonical,
                   const int *xyz, unsigned int count)
{
    CHECK(context == &calls && calls < 3 && count == 90);
    /* Commit has already happened when the first callback arrives. */
    CHECK(!memcmp(model, result, sizeof(model)));
    CHECK(strlen(canonical) < sizeof(notifications[0]));
    strcpy(notifications[calls], canonical);
    memcpy(snapshots[calls], xyz, sizeof(model));
    ++calls;
}

static void init_model(void)
{
    unsigned int i;
    for (i = 0; i < 90; ++i) model[i] = (int)i - 45;
    memcpy(original, model, sizeof(model));
}

static void execute_bad(CadMacroStatus status, unsigned long line,
                        unsigned int command)
{
    memcpy(original, model, sizeof(model));
    calls = 0;
    CHECK(CadMacroExecute(&program, model, 90, notify, &calls, &error) == status);
    CHECK(error.status == status && error.line == line &&
          error.command == command);
    CHECK(!memcmp(model, original, sizeof(model)) && calls == 0);
}

static void parser_tests(void)
{
    char longline[180], binary[40];
    FILE *f;
    unsigned int i;
    CHECK(parse(" \t# preamble\r\n\n HCMD\t1 \r\n# end\n") == CAD_MACRO_OK);
    CHECK(program.count == 0 && error.status == CAD_MACRO_OK &&
          error.line == 0 && error.command == 0);
    bad("", CAD_MACRO_HEADER, 1, 0);
    bad("# comment\n", CAD_MACRO_HEADER, 2, 0);
    bad("HCMD 2\n", CAD_MACRO_HEADER, 1, 0);
    bad("HCMD 01\n", CAD_MACRO_HEADER, 1, 0);
    bad("HCMD 1 extra\n", CAD_MACRO_HEADER, 1, 0);
    bad("RESET HOUSE\n", CAD_MACRO_HEADER, 1, 0);
    bad("HCMD 1\nHCMD 1\n", CAD_MACRO_UNSUPPORTED, 2, 1);
    bad("HCMD 1\nRESET\n", CAD_MACRO_SYNTAX, 2, 1);
    bad("HCMD 1\nRESET OTHER\n", CAD_MACRO_SYNTAX, 2, 1);
    bad("HCMD 1\nRESET HOUSE # inline\n", CAD_MACRO_SYNTAX, 2, 1);
    bad("HCMD 1\nSETV 0 1 2\n", CAD_MACRO_SYNTAX, 2, 1);
    bad("HCMD 1\nSETV 0 1 2 3 4 5 6\n", CAD_MACRO_SYNTAX, 2, 1);
    bad("HCMD 1\nSETV 0 1.0 2 3\n", CAD_MACRO_NUMBER, 2, 1);
    bad("HCMD 1\nSETV 0 + 2 3\n", CAD_MACRO_NUMBER, 2, 1);
    bad("HCMD 1\nSETV 0 1e2 2 3\n", CAD_MACRO_NUMBER, 2, 1);
    bad("HCMD 1\nSETV 0 0x10 2 3\n", CAD_MACRO_NUMBER, 2, 1);
    bad("HCMD 1\nMOVEV 0 32768 0 0\n", CAD_MACRO_NUMBER, 2, 1);
    bad("HCMD 1\nMOVEV 0 -32769 0 0\n", CAD_MACRO_NUMBER, 2, 1);
    bad("HCMD 1\nSETV 999999999999999999999 0 0 0\n", CAD_MACRO_NUMBER, 2, 1);
    bad("HCMD 1\nSETV -1 0 0 0\n", CAD_MACRO_RANGE, 2, 1);
    bad("HCMD 1\nSETV 30 0 0 0\n", CAD_MACRO_RANGE, 2, 1);
    bad("HCMD 1\nSETV 0 -401 0 0\n", CAD_MACRO_RANGE, 2, 1);
    bad("HCMD 1\nSETV 0 0 401 0\n", CAD_MACRO_RANGE, 2, 1);
    bad("HCMD 1\n# ignored\nSETV 0 0 0 401\n", CAD_MACRO_RANGE, 3, 1);
    bad("HCMD 1\nRESET HOUSE\n# ignored\nSAVE C:\\HOUSE.CAD\n",
        CAD_MACRO_UNSUPPORTED, 4, 2);
    bad("HCMD 1\nREPEAT 2\n", CAD_MACRO_UNSUPPORTED, 2, 1);
    bad("HCMD 1\nSYSTEM echo hello\n", CAD_MACRO_UNSUPPORTED, 2, 1);
    bad("HCMD 1\nsetv 0 0 0 0\n", CAD_MACRO_UNSUPPORTED, 2, 1);
    bad("HCMD 1\r", CAD_MACRO_ASCII, 1, 0);
    bad("HCMD 1\n#\177\n", CAD_MACRO_ASCII, 2, 1);
    bad("HCMD 1\n#\200\n", CAD_MACRO_ASCII, 2, 1);
    bad("HCMD 1\n#\001\n", CAD_MACRO_ASCII, 2, 1);
    memset(longline, ' ', sizeof(longline));
    memcpy(longline, "HCMD 1", 6); longline[160] = '\0';
    CHECK(parse(longline) == CAD_MACRO_OK);
    longline[160] = ' '; longline[161] = '\0';
    bad(longline, CAD_MACRO_LINE_TOO_LONG, 1, 0);
    memset(longline, ' ', sizeof(longline));
    memcpy(longline, "HCMD 1\n#", 8); longline[167] = '\0';
    CHECK(parse(longline) == CAD_MACRO_OK); /* 160-byte comment. */
    longline[167] = ' '; longline[168] = '\0';
    bad(longline, CAD_MACRO_LINE_TOO_LONG, 2, 1);
    longline[167] = '\r'; longline[168] = '\n'; longline[169] = '\0';
    CHECK(parse(longline) == CAD_MACRO_OK); /* CRLF excluded. */
    memset(longline, ' ', sizeof(longline));
    memcpy(longline, "HCMD 1\nSETV 0 1 2 3", 19);
    longline[167] = '\0';
    CHECK(parse(longline) == CAD_MACRO_OK);
    longline[167] = ' '; longline[168] = '\0';
    bad(longline, CAD_MACRO_LINE_TOO_LONG, 2, 1);
    memcpy(&saved, &program, sizeof(saved));
    memcpy(binary, "HCMD 1\n# abc", 12); binary[10] = '\0';
    f = tmpfile(); CHECK(f != NULL);
    CHECK(fwrite(binary, 1, 12, f) == 12); rewind(f);
    CHECK(CadMacroRead(f, &program, &error) == CAD_MACRO_ASCII);
    CHECK(error.line == 2 && error.command == 1);
    CHECK(!memcmp(&saved, &program, sizeof(saved))); CHECK(fclose(f) == 0);
    f = tmpfile(); CHECK(f != NULL);
    CHECK(fputs("HCMD 1\n", f) >= 0);
    for (i = 0; i < 1024; ++i) CHECK(fputs("MOVEV 0 0 0 0\n", f) >= 0);
    CHECK(fputs("# comment after limit\n\n", f) >= 0); rewind(f);
    CHECK(CadMacroRead(f, &program, &error) == CAD_MACRO_OK);
    CHECK(program.count == 1024 && program.commands[1023].line == 1025);
    memcpy(&saved, &program, sizeof(saved));
    CHECK(fseek(f, 0L, SEEK_END) == 0);
    CHECK(fputs("RESET HOUSE\n", f) >= 0); rewind(f);
    CHECK(CadMacroRead(f, &program, &error) == CAD_MACRO_TOO_MANY);
    CHECK(error.line == 1028 && error.command == 1025);
    CHECK(!memcmp(&saved, &program, sizeof(saved))); CHECK(fclose(f) == 0);
    init_model();
    CHECK(CadMacroExecute(&program, model, 90, NULL, NULL, &error) == CAD_MACRO_OK);
    CHECK(!memcmp(model, original, sizeof(model)));
}

static void execution_tests(void)
{
    unsigned int i;
    init_model();
    CHECK(parse("HCMD 1") == CAD_MACRO_OK);
    calls = 0;
    CHECK(CadMacroExecute(&program, model, 90, notify, &calls, &error) == CAD_MACRO_OK);
    CHECK(calls == 0 && !memcmp(model, original, sizeof(model)));
    CHECK(parse("HCMD 1\nRESET HOUSE\nSETV +029 -0400 +00400 -0\n"
                "MOVEV 29 +0800 -0800 +01") == CAD_MACRO_OK);
    memcpy(&saved, &program, sizeof(saved));
    CHECK(parse("HCMD 1\nRESET HOUSE\nSETV +029 -0400 +00400 -0\n"
                "MOVEV 29 +0800 -0800 +01") == CAD_MACRO_OK);
    CHECK(!memcmp(&saved, &program, sizeof(saved)));
    memcpy(result, expected_house, sizeof(result));
    result[87] = 400; result[88] = -400; result[89] = 1;
    calls = 0;
    CHECK(CadMacroExecute(&program, model, 90, notify, &calls, &error) == CAD_MACRO_OK);
    CHECK(calls == 3 && !memcmp(model, result, sizeof(model)));
    CHECK(!strcmp(notifications[0], "RESET HOUSE"));
    CHECK(!strcmp(notifications[1], "SETV 29 -400 400 0"));
    CHECK(!strcmp(notifications[2], "MOVEV 29 800 -800 1"));
    CHECK(!memcmp(snapshots[0], expected_house, sizeof(model)));
    CHECK(snapshots[1][87] == -400 && snapshots[1][88] == 400 &&
          snapshots[1][89] == 0);
    CHECK(!memcmp(snapshots[2], result, sizeof(model)));
    for (i = 0; i < 10; ++i) {
        CHECK(CadMacroExecute(&program, model, 90, NULL, NULL, &error) == CAD_MACRO_OK);
        CHECK(!memcmp(model, result, sizeof(model)));
    }
    CHECK(parse("HCMD 1\nRESET HOUSE\nSETV 0 400 -400 0\nMOVEV 0 1 0 0\n") == CAD_MACRO_OK);
    execute_bad(CAD_MACRO_RANGE, 4, 3);
    program.commands[2].x = 0; program.commands[2].y = -1;
    execute_bad(CAD_MACRO_RANGE, 4, 3);
    program.commands[2].y = 0; program.commands[2].z = -32767 - 1;
    execute_bad(CAD_MACRO_RANGE, 4, 3);
    program.commands[2].z = 32767;
    execute_bad(CAD_MACRO_RANGE, 4, 3);
    program.commands[2].z = 0; program.commands[2].id = 30;
    execute_bad(CAD_MACRO_RANGE, 4, 3);
    program.commands[2].id = 0; program.commands[2].op = (CadMacroOp)99;
    execute_bad(CAD_MACRO_UNSUPPORTED, 4, 3);
    program.commands[2].op = CAD_MACRO_SETV; program.commands[2].x = 401;
    execute_bad(CAD_MACRO_RANGE, 4, 3);
    program.commands[2].op = CAD_MACRO_RESET; program.commands[2].x = 1;
    execute_bad(CAD_MACRO_SYNTAX, 4, 3);
    program.count = 1025; execute_bad(CAD_MACRO_TOO_MANY, 0, 0);
    program.count = 0;
    model[89] = 401; execute_bad(CAD_MACRO_ARGUMENT, 0, 0);
    init_model();
    CHECK(CadMacroExecute(NULL, model, 90, NULL, NULL, &error) == CAD_MACRO_ARGUMENT);
    CHECK(CadMacroExecute(&program, NULL, 90, NULL, NULL, NULL) == CAD_MACRO_ARGUMENT);
    CHECK(CadMacroExecute(&program, model, 89, NULL, NULL, &error) == CAD_MACRO_ARGUMENT);
    CHECK(!memcmp(model, original, sizeof(model)));
    CHECK(parse("HCMD 1\nMOVEV 0 -32768 32767 0\n") == CAD_MACRO_OK);
    CHECK(program.commands[0].x == -32767 - 1 &&
          program.commands[0].y == 32767);
    execute_bad(CAD_MACRO_RANGE, 2, 1);
#if INT_MAX > 32767
    program.count = 1; program.commands[0].op = CAD_MACRO_MOVEV;
    program.commands[0].id = 0; program.commands[0].x = INT_MAX;
    program.commands[0].y = 0; program.commands[0].z = 0;
    execute_bad(CAD_MACRO_NUMBER, program.commands[0].line, 1);
#endif
}

static void io_tests(void)
{
    FILE *f;
    long budget;
    unsigned int i;
    char text[80];
    CHECK(parse("HCMD 1\nRESET HOUSE\n") == CAD_MACRO_OK);
    memcpy(&saved, &program, sizeof(saved));
    for (budget = 0; budget <= 18; ++budget) {
        f = stream("HCMD 1\nRESET HOUSE\n");
        read_budget = budget;
        CHECK(CadMacroRead(f, &program, &error) == CAD_MACRO_IO);
        CHECK(!memcmp(&saved, &program, sizeof(saved)));
        read_budget = -1; injected_error = 0; CHECK(fclose(f) == 0);
    }
    CHECK(CadMacroRead(NULL, &program, &error) == CAD_MACRO_ARGUMENT);
    f = stream("HCMD 1\n");
    CHECK(CadMacroRead(f, NULL, NULL) == CAD_MACRO_ARGUMENT);
    CHECK(ftell(f) == 0L); CHECK(fclose(f) == 0);
    for (i = 0; i < 90; ++i) model[i] = i % 2 ? 400 : -400;
    f = tmpfile(); CHECK(f != NULL);
    CHECK(CadMacroSnapshot(f, model, 90, &error) == CAD_MACRO_OK);
    rewind(f);
    CHECK(fgets(text, sizeof(text), f) != NULL && !strcmp(text, "HCMD 1\n"));
    CHECK(fgets(text, sizeof(text), f) != NULL &&
          !strcmp(text, "SETV 0 -400 400 -400\n"));
    for (i = 1; i < 30; ++i) CHECK(fgets(text, sizeof(text), f) != NULL);
    CHECK(!strcmp(text, "SETV 29 400 -400 400\n"));
    CHECK(fgetc(f) == EOF && !ferror(f));
    rewind(f);
    CHECK(CadMacroRead(f, &program, &error) == CAD_MACRO_OK);
    CHECK(program.count == 30 && program.commands[29].id == 29);
    memset(result, 0, sizeof(result));
    CHECK(CadMacroExecute(&program, result, 90, NULL, NULL, &error) == CAD_MACRO_OK);
    CHECK(!memcmp(model, result, sizeof(model)));
    CHECK(fclose(f) == 0);
    f = tmpfile(); CHECK(f != NULL);
    model[89] = -401;
    CHECK(CadMacroSnapshot(f, model, 90, &error) == CAD_MACRO_ARGUMENT);
    CHECK(ftell(f) == 0L); model[89] = 0;
    CHECK(CadMacroSnapshot(f, model, 91, NULL) == CAD_MACRO_ARGUMENT);
    CHECK(CadMacroSnapshot(f, NULL, 90, NULL) == CAD_MACRO_ARGUMENT);
    CHECK(CadMacroSnapshot(NULL, model, 90, NULL) == CAD_MACRO_ARGUMENT);
    CHECK(ftell(f) == 0L);
    write_fail = 1;
    CHECK(CadMacroSnapshot(f, model, 90, &error) == CAD_MACRO_IO);
    CHECK(error.line == 1 && error.command == 0 && ftell(f) == 0L);
    write_fail = 0; formatted_fail = 1;
    CHECK(CadMacroSnapshot(f, model, 90, &error) == CAD_MACRO_IO);
    CHECK(error.line == 2 && error.command == 1);
    formatted_fail = 0; flush_fail = 1;
    CHECK(CadMacroSnapshot(f, model, 90, &error) == CAD_MACRO_IO);
    CHECK(error.line == 31 && error.command == 30);
    flush_fail = 0; CHECK(fclose(f) == 0);
}

int main(void)
{
    parser_tests();
    execution_tests();
    io_tests();
    puts("macro tests passed (C89, bounds, deterministic replay, atomic model, IO)");
    return 0;
}
