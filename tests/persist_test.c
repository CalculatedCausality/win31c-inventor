/* Host: cc -std=c89 -pedantic -Wall -Wextra -Werror
 *          -Itests/win_stub tests/persist_test.c -o /tmp/opencode/persist_test
 * Run in an empty scratch directory. Implementation included ONLY to inject
 * deterministic stdio/rename failures; production needs no test hooks.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>

static int rename_call, rename_fail, rollback_fail, close_fail;
static int write_fail, backup_remove_fail;
static int test_rename(const char *from, const char *to)
{
    ++rename_call;
    if (rename_call == rename_fail ||
        (rollback_fail && rename_call == 3)) {
        errno = EACCES;
        return -1;
    }
    return rename(from, to);
}
static int test_close(FILE *f)
{
    int rc;
    rc = fclose(f);
    if (close_fail) { close_fail = 0; return EOF; }
    return rc;
}
static size_t test_write(const void *p, size_t size, size_t n, FILE *f)
{
    if (write_fail) {
        write_fail = 0;
        return fwrite(p, size, n / 2, f); /* A real partial temp file. */
    }
    return fwrite(p, size, n, f);
}
static int test_remove(const char *path)
{
    if (backup_remove_fail && !strcmp(path, "HOUSE.BAK")) {
        errno = EACCES;
        return -1;
    }
    return remove(path);
}
#define rename test_rename
#define fclose test_close
#define fwrite test_write
#define remove test_remove
#include "../persist.c"
#undef rename
#undef fclose
#undef fwrite
#undef remove

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static int input[90], output[90];
static unsigned char good[194], bad[195];

static void load(const char *path, unsigned char *bytes)
{
    FILE *f;
    f = fopen(path, "rb"); CHECK(f != NULL);
    CHECK(fread(bytes, 1, 194, f) == 194);
    CHECK(fgetc(f) == EOF && !ferror(f)); CHECK(fclose(f) == 0);
}
static void save(const char *path, const unsigned char *bytes, size_t n)
{
    FILE *f;
    f = fopen(path, "wb"); CHECK(f != NULL);
    CHECK(fwrite(bytes, 1, n, f) == n); CHECK(fclose(f) == 0);
}
static void unchanged_read(const char *path)
{
    int i;
    for (i = 0; i < 90; ++i) output[i] = 1234;
    CHECK(!CadRead(path, output, 90));
    for (i = 0; i < 90; ++i) CHECK(output[i] == 1234);
}
static void still_old(void)
{
    unsigned char bytes[194];
    load("HOUSE.CAD", bytes); CHECK(!memcmp(bytes, good, 194));
}
/* Independent bit implementation, not the production checksum helper. */
static unsigned long reference_crc(const unsigned char *bytes)
{
    unsigned long crc;
    int i, b, feedback;
    crc = 65535UL;
    for (i = 0; i < 194; ++i) {
        if (i == 12 || i == 13) continue;
        for (b = 7; b >= 0; --b) {
            feedback = ((crc >> 15) & 1UL) ^ ((bytes[i] >> b) & 1);
            crc = (crc << 1) & 65535UL;
            if (feedback) crc ^= 0x1021UL;
        }
    }
    return crc;
}
int main(void)
{
    int i, offsets[6];
    size_t n;
    unsigned long crc;
    char longpath[140];
    CHECK(INT_MAX >= 2147483647L); /* Exercise native host int32. */
    CHECK(state("HOUSE.CAD") == 0 && state("HOUSE.TMP") == 0 &&
          state("HOUSE.BAK") == 0 && state("BAD.CAD") == 0 &&
          state("DIR") == 0);
    for (i = 0; i < 90; ++i) input[i] = i % 3 == 0 ? -400 :
                                         i % 3 == 1 ? 400 : 0;
    CHECK(CadWrite("HOUSE.CAD", input, 90));
    CHECK(CadRead("HOUSE.CAD", output, 90));
    CHECK(!memcmp(input, output, sizeof(input)));
    load("HOUSE.CAD", good);
    CHECK(!memcmp(good, "CADH\001\000\001\000\132\000\264\000", 12));
    CHECK(good[14] == 0x70 && good[15] == 0xfe &&
          good[16] == 0x90 && good[17] == 1 && !good[18] && !good[19]);
    crc = reference_crc(good);
    CHECK(good[12] == (crc & 255UL) && good[13] == (crc >> 8));
    for (n = 0; n < 194; ++n) {
        save("BAD.CAD", good, n); unchanged_read("BAD.CAD");
    }
    offsets[0] = 0; offsets[1] = 4; offsets[2] = 6;
    offsets[3] = 8; offsets[4] = 10; offsets[5] = 12;
    for (i = 0; i < 6; ++i) {
        memcpy(bad, good, 194); bad[offsets[i]] ^= 1;
        /* Header rejection must also work with a valid checksum. */
        if (i != 5) {
            crc = reference_crc(bad);
            bad[12] = (unsigned char)crc; bad[13] = (unsigned char)(crc >> 8);
        }
        save("BAD.CAD", bad, 194); unchanged_read("BAD.CAD");
    }
    memcpy(bad, good, 194); bad[100] ^= 1;
    save("BAD.CAD", bad, 194); unchanged_read("BAD.CAD");
    memcpy(bad, good, 194); bad[194] = 0;
    save("BAD.CAD", bad, 195); unchanged_read("BAD.CAD");
    /* Last coordinate invalid, valid CRC: no partial caller update. */
    for (i = 0; i < 2; ++i) {
        memcpy(bad, good, 194);
        bad[192] = i ? 0x6f : 0x91; bad[193] = i ? 0xfe : 1;
        crc = reference_crc(bad);
        bad[12] = (unsigned char)crc; bad[13] = (unsigned char)(crc >> 8);
        save("BAD.CAD", bad, 194); unchanged_read("BAD.CAD");
    }
    for (i = 0; i < 2; ++i) {
        input[89] = i ? -401 : 401;
        CHECK(!CadWrite("HOUSE.CAD", input, 90)); still_old();
        CHECK(state("HOUSE.TMP") == 0);
    }
    input[89] = 0;
    CHECK(!CadWrite("HOUSE.CAD", input, 89));
    CHECK(!CadRead("HOUSE.CAD", output, 91));
    CHECK(!CadRead(NULL, output, 90)); CHECK(!CadWrite(NULL, input, 90));
    CHECK(!CadRead("HOUSE.CAD", NULL, 90));
    CHECK(!CadWrite("HOUSE.CAD", NULL, 90));
    memset(longpath, 'X', sizeof(longpath)); longpath[139] = 0;
    CHECK(!CadWrite(longpath, input, 90));
    CHECK(!CadRead(longpath, output, 90));
    CHECK(!CadWrite("TOOLONG99.CAD", input, 90));
    CHECK(!CadWrite("HOUSE.tmp", input, 90));
    CHECK(!CadWrite("CON.CAD", input, 90));
    CHECK(!CadWrite("", input, 90));
    unchanged_read("MISSING.CAD");
    /* Target fits in 127 bytes, but the longer sidecar does not. */
    memset(longpath, 'X', 125); longpath[125] = '/';
    longpath[126] = 'A'; longpath[127] = 0;
    CHECK(!CadWrite(longpath, input, 90));
    CHECK(mkdir("DIR", 0700) == 0);
    CHECK(!CadWrite("DIR", input, 90)); unchanged_read("DIR");
    CHECK(mkdir("HOUSE.TMP", 0700) == 0);
    CHECK(!CadWrite("HOUSE.CAD", input, 90)); still_old();
    CHECK(remove("HOUSE.TMP") == 0);
    CHECK(mkdir("HOUSE.BAK", 0700) == 0);
    CHECK(!CadWrite("HOUSE.CAD", input, 90)); still_old();
    CHECK(remove("HOUSE.BAK") == 0);
    save("HOUSE.TMP", good, 194);
    CHECK(!CadWrite("HOUSE.CAD", input, 90)); still_old();
    load("HOUSE.TMP", bad); CHECK(!memcmp(bad, good, 194));
    CHECK(remove("HOUSE.TMP") == 0);
    save("HOUSE.BAK", good, 194);
    CHECK(!CadWrite("HOUSE.CAD", input, 90)); still_old();
    load("HOUSE.BAK", bad); CHECK(!memcmp(bad, good, 194));
    CHECK(remove("HOUSE.BAK") == 0);
    input[0] = 100; /* New contents must differ from the recoverable old file. */
    close_fail = 1; unchanged_read("HOUSE.CAD");
    close_fail = 1; CHECK(!CadWrite("HOUSE.CAD", input, 90)); still_old();
    CHECK(state("HOUSE.TMP") == 0);
    write_fail = 1; CHECK(!CadWrite("HOUSE.CAD", input, 90)); still_old();
    CHECK(state("HOUSE.TMP") == 0);
    rename_call = 0; rename_fail = 1;
    CHECK(!CadWrite("HOUSE.CAD", input, 90)); still_old();
    CHECK(state("HOUSE.TMP") == 0 && state("HOUSE.BAK") == 0);
    rename_call = 0; rename_fail = 2;
    CHECK(!CadWrite("HOUSE.CAD", input, 90)); still_old();
    CHECK(state("HOUSE.TMP") == 1 && state("HOUSE.BAK") == 0);
    load("HOUSE.TMP", bad); CHECK(bad[14] == 100 && bad[15] == 0);
    CHECK(remove("HOUSE.TMP") == 0);
    rename_call = 0; rollback_fail = 1;
    CHECK(!CadWrite("HOUSE.CAD", input, 90));
    CHECK(state("HOUSE.CAD") == 0 && state("HOUSE.TMP") == 1);
    load("HOUSE.TMP", bad); CHECK(bad[14] == 100 && bad[15] == 0);
    load("HOUSE.BAK", bad); CHECK(!memcmp(bad, good, 194));
    CHECK(rename("HOUSE.BAK", "HOUSE.CAD") == 0);
    CHECK(remove("HOUSE.TMP") == 0);
    rename_fail = 0; rollback_fail = 0; rename_call = 0;
    backup_remove_fail = 1;
    CHECK(!CadWrite("HOUSE.CAD", input, 90));
    CHECK(CadRead("HOUSE.CAD", output, 90));
    CHECK(!memcmp(input, output, sizeof(input)));
    load("HOUSE.BAK", bad); CHECK(!memcmp(bad, good, 194));
    backup_remove_fail = 0; CHECK(remove("HOUSE.BAK") == 0);
    /* First creation can fail installation too; complete temp is retained. */
    rename_call = 0; rename_fail = 1;
    CHECK(!CadWrite("FIRST.CAD", input, 90));
    CHECK(state("FIRST.CAD") == 0 && state("FIRST.TMP") == 1);
    CHECK(remove("FIRST.TMP") == 0);
    rename_fail = 0;
    for (i = 0; i < 90; ++i) input[i] = 0;
    CHECK(CadWrite("HOUSE.CAD", input, 90));
    CHECK(CadRead("HOUSE.CAD", output, 90));
    CHECK(!memcmp(input, output, sizeof(input)));
    load("HOUSE.CAD", bad);
    for (i = 14; i < 194; ++i) CHECK(bad[i] == 0);
    CHECK(state("HOUSE.TMP") == 0 && state("HOUSE.BAK") == 0);
    CHECK(remove("HOUSE.CAD") == 0); CHECK(remove("BAD.CAD") == 0);
    CHECK(remove("DIR") == 0);
    puts("persist tests passed (int32, wire format, validation, rollback)");
    return 0;
}
