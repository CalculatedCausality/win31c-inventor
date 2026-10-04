#ifndef CAD_DOCUMENT_H
#define CAD_DOCUMENT_H

#include <stdio.h>
#include "cad_model.h"

#define CAD_DOC_OK 0
#define CAD_DOC_FORMAT 1
#define CAD_DOC_RANGE 2
#define CAD_DOC_IO 3
#define CAD_DOC_ARG 4
#define CAD_DOC_COLLISION 5
#define CAD_DOC_PATH_MAX 127
#define CAD_DOC_WIRE_MAX 5146

/* Version 1, all words explicitly little endian, no ABI-dependent storage:
 * offset  size  meaning
 *   0      4    ASCII CADG
 *   4      2    version (1)
 *   6      2    units (0 unspecified, 1 mm, 2 inches)
 *   8      2    vertex count (0..256)
 *  10      2    edge count (0..512)
 *  12      4    next vertex ID (1..65536)
 *  16      4    next edge ID (1..65536)
 *  20      4    payload byte count (8*vertices + 6*edges)
 *  24     8*v   vertices: u16 ID, signed two's-complement i16 x,y,z
 *        6*e   edges: u16 ID, u16 endpoint a, u16 endpoint b
 *          2   CRC-16/CCITT-FALSE of all preceding bytes: polynomial
 *              0x1021, initial 0xffff, non-reflected, no final XOR.
 * EOF must immediately follow CRC. Coordinates are exactly -400..400.
 * IDs are nonzero and unique within each namespace; endpoints must exist;
 * each next ID must exceed every ID in its namespace (65536 = exhausted).
 * Empty/disconnected graphs, cycles, parallel edges and self-edges are legal.
 * Bad magic/version/length/CRC/IDs/references => FORMAT; unsupported numeric
 * domains (including counts, units and high-water values) => RANGE.
 */

/* Caller owns binary streams, checks their close, and supplies distinct,
 * nonoverlapping live/stage models allocated outside the Win16 stack.
 * stage is disposable scratch, even on failure. live is untouched on any
 * failure. Successful reads clear ALL history/transaction storage, preserve
 * stable IDs/order/allocator high-water marks, and replace live in one copy.
 * Stream functions neither rewind nor close. write rejects active model
 * transactions (ARG), validates geometry before emitting any bytes, and
 * reports short writes/flush failures as IO. read publishes at validated EOF;
 * use load when publication must also wait for a successful fclose.
 * Compile Win16 callers and module with -ml -zt=1. No static model/buffer.
 */
int cad_document_read(FILE *stream, CadModel *live, CadModel *stage);
int cad_document_write(FILE *stream, const CadModel *model);
int cad_document_load(const char *path, CadModel *live, CadModel *stage);
int cad_document_save(const char *path, const CadModel *model);

/* Paths and derived sidecars fit 127 characters, excluding NUL. Final names
 * use DOS 8.3 letters/digits/_/-; device names and TMP/BAK extensions are
 * rejected (case-insensitive). Load uses the same path policy as save.
 * Save checks regular target/absent sidecars, writes and closes stem.TMP,
 * renames existing target to stem.BAK, then installs TMP. Existing sidecars
 * are COLLISION and are never overwritten. On installation failure restore
 * BAK; retain complete TMP (and BAK if rollback fails) for recovery. After
 * successful replacement remove BAK; failed cleanup returns IO and retains
 * old BAK. Failure before installation removes only the newly created TMP.
 * C89 filesystem calls require single-writer/exclusive directory access;
 * these operations are recoverable, not crash-atomic or concurrent locking.
 */

#endif
