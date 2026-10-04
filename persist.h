#ifndef CAD_PERSIST_H
#define CAD_PERSIST_H

#include <windows.h>

/* Exactly 30 vertices, in houseInit order, flattened x,y,z; +/-400.
 * Wire format (194 bytes, all words unsigned little-endian):
 *  0: "CADH"; 4: version=1; 6: topology=1 (fixed house edges);
 *  8: coordinate count=90; 10: payload byte length=180;
 * 12: CRC-16/CCITT-FALSE (poly 0x1021, init 0xffff, no reflection/xor),
 *     over bytes 0..11 followed by bytes 14..193;
 * 14: 90 signed two's-complement 16-bit coordinates.
 * No padding or trailing bytes are permitted. Never a native struct dump.
 *
 * Paths must be nonempty and at most 127 bytes, with a DOS 8.3 basename
 * (ASCII letters/digits/_/-, optional 1..3 character extension).
 * DOS device names and destination extensions TMP/BAK are rejected.
 * Sidecars replace the extension with .TMP/.BAK in the same directory.
 * Existing sidecars, directories and stat errors are rejected, not removed.
 *
 * Read failures leave xyz untouched, including close failures.
 * Write validates all inputs before creating a file. Replacement moves the
 * old file to .BAK, installs .TMP, then removes .BAK. On installation failure
 * rollback is attempted; .TMP is retained and, if rollback fails, .BAK holds
 * the old file. A FALSE result can mean the new file was installed but backup
 * cleanup failed; inspect the destination and sidecars before retrying.
 * This is recoverable, NOT power-loss atomic: stdio has no durable flush,
 * and crashes can leave sidecars or a missing destination. No concurrent
 * writers/path mutation: portable stdio cannot exclusively reserve a name.
 * Static buffers make this API non-reentrant. Call from one task at a time.
 */
BOOL CadRead(LPCSTR path, int *xyz, WORD count);
BOOL CadWrite(LPCSTR path, const int *xyz, WORD count);

#endif
