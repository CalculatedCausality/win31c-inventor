#ifndef CAD_PARTS_H
#define CAD_PARTS_H

#include "cad_assembly.h"
#include "cad_model.h"

/* Inventor-style "Place Component": the open document is the assembly and
 * references other .CAD part files placed as occurrences.
 * Strict C89 / Win16: compile with -ml -zt=1 together with cad_document.c,
 * cad_model.c and cad_assembly.c. No heap, no floating point.
 *
 * Part cache: up to CAD_PARTS_CACHE distinct part paths, each loaded once
 * (cad_document_load) into its own static CadModel, plus one shared static
 * stage model and one static staging CadParts used by read_text. Because the
 * part models are module statics, only ONE live CadParts per process is
 * supported, and calls are not reentrant. Paths are kept exactly as given
 * (case-sensitive comparison, 1..127 printable bytes, no spaces). A path
 * whose file cannot be loaded still places a "missing" occurrence with no
 * geometry; reload retries. A cache slot is freed when its last occurrence
 * is removed.
 *
 * Occurrences: up to CAD_PARTS_MAX, all roots of an internal CadAssembly
 * (IDs 1..65535, monotonic, never recycled). Placement is an integer
 * translation (each component within +-2000) plus a yaw index 0..7 in
 * 45-degree steps about +Y (Q10, right-handed: yaw 2 maps +X to -Z):
 *   wx = round((c*x + s*z)/1024) + tx, wy = y + ty,
 *   wz = round((c*z - s*x)/1024) + tz   (round: nearest, ties away from 0)
 * so even yaws are exact. Grounded occurrences reject move; remove is
 * allowed (it ungrounds internally). Display name "<STEM>:<n>", STEM being
 * the upper-cased file stem (truncated to 12 chars) and n = 1 + the number of
 * occurrences of the same path with smaller IDs (derived, so it renumbers
 * after removal and survives serialization).
 *
 * Every status-returning call leaves the CadParts and outputs unchanged on
 * failure (static part models of unused slots may be refreshed).
 */
#define CAD_PARTS_MAX 16
#define CAD_PARTS_CACHE 4
#define CAD_PARTS_PATH_MAX 127
#define CAD_PARTS_NAME_MAX 23
#define CAD_PARTS_TRANSLATION 2000L
#define CAD_PARTS_TEXT_MAX 4096U

typedef enum CadPartsStatus {
    CAD_PARTS_OK = 0, CAD_PARTS_ARGUMENT, CAD_PARTS_NOT_FOUND,
    CAD_PARTS_FULL, CAD_PARTS_CACHE_FULL, CAD_PARTS_GROUNDED,
    CAD_PARTS_ID_EXHAUSTED, CAD_PARTS_CAPACITY, CAD_PARTS_FORMAT
} CadPartsStatus;

typedef struct CadPartsInfo {
    CadAssemblyId id;
    char name[CAD_PARTS_NAME_MAX + 1];
    char path[CAD_PARTS_PATH_MAX + 1];
    int missing;
    long tx, ty, tz;
    int yaw, grounded;
    unsigned int vertex_count, edge_count; /* 0 when missing */
} CadPartsInfo;

/* Storage exposed only for caller allocation; treat as opaque. */
typedef struct CadPartsRecord {
    CadAssemblyId id;
    int slot, yaw;
} CadPartsRecord;
typedef struct CadPartsSlot {
    char path[CAD_PARTS_PATH_MAX + 1]; /* empty = free */
    int missing;
} CadPartsSlot;
typedef struct CadParts {
    CadAssembly assembly;
    unsigned int count;
    CadPartsRecord record[CAD_PARTS_MAX]; /* ascending ID order */
    CadPartsSlot slot[CAD_PARTS_CACHE];
} CadParts;

typedef void (*CadPartsSegmentFn)(void *ctx, long x1, long y1, long z1,
                                  long x2, long y2, long z2);

void cad_parts_init(CadParts *p);
/* Path must be 1..127 bytes of 0x21..0x7e. FULL at 16 occurrences,
 * CACHE_FULL when a 5th distinct path would be needed. */
CadPartsStatus cad_parts_place(CadParts *p, const char *path, long tx,
    long ty, long tz, int yaw, CadAssemblyId *id);
CadPartsStatus cad_parts_move(CadParts *p, CadAssemblyId id, long tx,
    long ty, long tz, int yaw);
CadPartsStatus cad_parts_remove(CadParts *p, CadAssemblyId id);
CadPartsStatus cad_parts_ground(CadParts *p, CadAssemblyId id, int grounded);
unsigned int cad_parts_count(const CadParts *p);
/* index 0..count-1 in ascending ID order. */
CadPartsStatus cad_parts_at(const CadParts *p, unsigned int index,
    CadAssemblyId *id);
CadPartsStatus cad_parts_get(const CadParts *p, CadAssemblyId id,
    CadPartsInfo *info);
/* Re-reads every cached part file, refreshing missing flags. */
CadPartsStatus cad_parts_reload(CadParts *p);
/* World-space edges of one occurrence (id) or all (id 0); missing
 * occurrences emit nothing. */
CadPartsStatus cad_parts_segments(const CadParts *p, CadAssemblyId id,
    CadPartsSegmentFn fn, void *ctx);
/* Returns 1 and fills min/max over all placed world vertices, else 0. */
int cad_parts_bounds(const CadParts *p, long min[3], long max[3]);

/* Lines: "N <next_id>" first, then "O <id> <yaw> <grounded> <tx> <ty> <tz>
 * <path>" in ascending ID order; canonical decimal, single spaces, "\n"
 * terminated. Empty text reads as the initial state. write returns
 * CAPACITY (buffer and *length untouched) when it does not fit; no NUL is
 * appended. Both return CadPartsStatus values.
 * read is fail-atomic, reports the 1-based offending line in *line (0 when
 * not line specific), re-loads every referenced part, allows missing files. */
int cad_parts_write_text(const CadParts *p, char *buffer, unsigned capacity,
    unsigned *length);
int cad_parts_read_text(CadParts *p, const char *text, unsigned length,
    unsigned *line);

#endif
