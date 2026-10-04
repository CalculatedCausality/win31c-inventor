#include "cad_document.h"
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

/* DOS has no symlinks. Host builds must not follow a dangling sidecar link
 * and accidentally truncate its referent. Strict C89 hides POSIX lstat. */
#if defined(__unix__) || defined(__APPLE__)
extern int lstat(const char *, struct stat *);
#define DOC_STAT lstat
#else
#define DOC_STAT stat
#endif

/* Win16 rejects oversized types itself (its sizeof result is 16-bit).
 * Keep an explicit assertion for hosts without a segment-size limit. */
#ifndef __WATCOMC__
typedef char CadDocumentModelFits[(sizeof(CadModel) < 65536UL) ? 1 : -1];
#endif

static unsigned long doc_word(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8);
}

static unsigned long doc_long(const unsigned char *p)
{
    return doc_word(p) | (doc_word(p + 2) << 16);
}

static void doc_put(unsigned char *p, unsigned long n, unsigned int size)
{
    unsigned int i;
    for (i = 0; i < size; ++i) {
        p[i] = (unsigned char)(n & 255UL);
        n >>= 8;
    }
}

static unsigned short doc_crc(unsigned short crc, const unsigned char *p,
                              unsigned int n)
{
    unsigned int i, bit;
    unsigned long c = crc;
    for (i = 0; i < n; ++i) {
        c ^= (unsigned long)p[i] << 8;
        for (bit = 0; bit < 8; ++bit)
            c = ((c & 32768UL) ? (c << 1) ^ 0x1021UL : c << 1)
                & 65535UL;
    }
    return (unsigned short)c;
}

static int doc_get(FILE *f, unsigned char *p, unsigned int n,
                   unsigned short *crc)
{
    if (fread(p, 1, n, f) != n)
        return ferror(f) ? CAD_DOC_IO : CAD_DOC_FORMAT;
    if (crc) *crc = doc_crc(*crc, p, n);
    return CAD_DOC_OK;
}

static int doc_emit(FILE *f, const unsigned char *p, unsigned int n,
                    unsigned short *crc)
{
    if (fwrite(p, 1, n, f) != n) return CAD_DOC_IO;
    if (crc) *crc = doc_crc(*crc, p, n);
    return CAD_DOC_OK;
}

static int doc_vertex_exists(const CadModel *m, CadId id)
{
    unsigned int i;
    for (i = 0; i < m->vertex_count; ++i)
        if (m->vertices[i].id == id) return 1;
    return 0;
}

static int doc_validate(const CadModel *m)
{
    unsigned int i, j;
    const CadVertex *v;
    const CadEdge *e;
    if (m->vertex_count > CAD_MAX_VERTICES || m->edge_count > CAD_MAX_EDGES ||
        (m->units != CAD_UNITS_UNSPECIFIED && m->units != CAD_UNITS_MM &&
         m->units != CAD_UNITS_INCHES) ||
        m->next_vertex_id < 1L || m->next_vertex_id > 65536L ||
        m->next_edge_id < 1L || m->next_edge_id > 65536L)
        return CAD_DOC_RANGE;
    for (i = 0; i < m->vertex_count; ++i) {
        v = &m->vertices[i];
        if (v->x < CAD_COORD_MIN || v->x > CAD_COORD_MAX ||
            v->y < CAD_COORD_MIN || v->y > CAD_COORD_MAX ||
            v->z < CAD_COORD_MIN || v->z > CAD_COORD_MAX)
            return CAD_DOC_RANGE;
        if (!v->id) return CAD_DOC_FORMAT;
        if (m->next_vertex_id <= (long)v->id) return CAD_DOC_RANGE;
        for (j = 0; j < i; ++j)
            if (m->vertices[j].id == v->id) return CAD_DOC_FORMAT;
    }
    for (i = 0; i < m->edge_count; ++i) {
        e = &m->edges[i];
        if (!e->id || !doc_vertex_exists(m, e->a) ||
            !doc_vertex_exists(m, e->b)) return CAD_DOC_FORMAT;
        if (m->next_edge_id <= (long)e->id) return CAD_DOC_RANGE;
        for (j = 0; j < i; ++j)
            if (m->edges[j].id == e->id) return CAD_DOC_FORMAT;
    }
    return CAD_DOC_OK;
}

static long doc_coordinate(const unsigned char *p)
{
    unsigned long n = doc_word(p);
    return n <= 32767UL ? (long)n : (long)n - 65536L;
}

/* Extension staging: read publishes it only after full validation. */
static unsigned char doc_ext[CAD_DOC_EXT_MAX];

int cad_document_check_extension(const char *ext, unsigned ext_len)
{
    unsigned int i;
    unsigned char c;
    if (ext_len > CAD_DOC_EXT_MAX) return CAD_DOC_RANGE;
    if (!ext_len) return CAD_DOC_OK;
    if (!ext) return CAD_DOC_FORMAT;
    for (i = 0; i < ext_len; ++i) {
        c = (unsigned char)ext[i];
        if (c != 0x0a && (c < 0x20 || c > 0x7e)) return CAD_DOC_FORMAT;
    }
    return CAD_DOC_OK;
}

/* Parses into m and doc_ext; *ext_len receives the stored length. */
static int doc_parse(FILE *f, CadModel *m, unsigned int *ext_len)
{
    unsigned char h[24], r[8];
    unsigned short crc = 65535U;
    unsigned long nv, ne, nextv, nexte, units, version, len = 0UL;
    unsigned int i;
    int status, ch;
    status = doc_get(f, h, sizeof(h), &crc);
    if (status) return status;
    version = doc_word(h + 4);
    if (memcmp(h, "CADG", 4) || (version != 1UL && version != 2UL))
        return CAD_DOC_FORMAT;
    units = doc_word(h + 6); nv = doc_word(h + 8); ne = doc_word(h + 10);
    nextv = doc_long(h + 12); nexte = doc_long(h + 16);
    if (units > 2UL || nv > CAD_MAX_VERTICES || ne > CAD_MAX_EDGES ||
        nextv < 1UL || nextv > 65536UL || nexte < 1UL || nexte > 65536UL)
        return CAD_DOC_RANGE;
    if (doc_long(h + 20) != nv * 8UL + ne * 6UL) return CAD_DOC_FORMAT;
    memset(m, 0, sizeof(*m));
    m->units = (CadUnits)units;
    m->vertex_count = (unsigned short)nv;
    m->edge_count = (unsigned short)ne;
    m->next_vertex_id = (long)nextv; m->next_edge_id = (long)nexte;
    for (i = 0; i < m->vertex_count; ++i) {
        status = doc_get(f, r, 8, &crc); if (status) return status;
        m->vertices[i].id = (CadId)doc_word(r);
        m->vertices[i].x = doc_coordinate(r + 2);
        m->vertices[i].y = doc_coordinate(r + 4);
        m->vertices[i].z = doc_coordinate(r + 6);
    }
    for (i = 0; i < m->edge_count; ++i) {
        status = doc_get(f, r, 6, &crc); if (status) return status;
        m->edges[i].id = (CadId)doc_word(r);
        m->edges[i].a = (CadId)doc_word(r + 2);
        m->edges[i].b = (CadId)doc_word(r + 4);
    }
    if (version == 2UL) {
        status = doc_get(f, r, 2, &crc); if (status) return status;
        len = doc_word(r);
        if (len > CAD_DOC_EXT_MAX) return CAD_DOC_RANGE;
        if (len) {
            status = doc_get(f, doc_ext, (unsigned int)len, &crc);
            if (status) return status;
        }
    }
    status = doc_get(f, r, 2, NULL); if (status) return status;
    ch = fgetc(f);
    if (ferror(f)) return CAD_DOC_IO;
    if (ch != EOF || doc_word(r) != crc) return CAD_DOC_FORMAT;
    if (cad_document_check_extension((const char *)doc_ext,
                                     (unsigned int)len))
        return CAD_DOC_FORMAT;
    status = doc_validate(m); if (status) return status;
    *ext_len = (unsigned int)len;
    return CAD_DOC_OK;
}

/* Publishes stage and staged text; capacity is checked before any copy. */
static int doc_publish(int status, CadModel *live, const CadModel *stage,
                       unsigned int len, char *buffer, unsigned int capacity,
                       unsigned int *ext_len)
{
    if (status) return status;
    if (buffer && len > capacity) return CAD_DOC_RANGE;
    if (buffer) {
        if (len) memcpy(buffer, doc_ext, len);
        if (capacity > len) buffer[len] = 0;
    }
    if (ext_len) *ext_len = len;
    memcpy(live, stage, sizeof(*live));
    return CAD_DOC_OK;
}

int cad_document_read_ex(FILE *f, CadModel *live, CadModel *stage,
                         char *ext_buffer, unsigned ext_capacity,
                         unsigned *ext_len)
{
    unsigned int len = 0;
    int status;
    if (!f || !live || !stage || live == stage) return CAD_DOC_ARG;
    status = doc_parse(f, stage, &len);
    return doc_publish(status, live, stage, len, ext_buffer, ext_capacity,
                       ext_len);
}

int cad_document_read(FILE *f, CadModel *live, CadModel *stage)
{
    return cad_document_read_ex(f, live, stage, NULL, 0, NULL);
}

/* Model/extension preconditions shared by write and save, before any I/O. */
static int doc_writable(const CadModel *m, const char *ext, unsigned int len)
{
    int status;
    if (!m || m->transaction_active) return CAD_DOC_ARG;
    status = cad_document_check_extension(ext, len);
    if (status) return status == CAD_DOC_FORMAT ? CAD_DOC_ARG : status;
    return doc_validate(m);
}

int cad_document_write_ex(FILE *f, const CadModel *m, const char *ext,
                          unsigned ext_len)
{
    unsigned char h[24], r[8];
    unsigned short crc = 65535U;
    unsigned int i;
    int status;
    if (!f) return CAD_DOC_ARG;
    status = doc_writable(m, ext, ext_len); if (status) return status;
    memcpy(h, "CADG", 4);
    doc_put(h + 4, 2UL, 2); doc_put(h + 6, (unsigned long)m->units, 2);
    doc_put(h + 8, m->vertex_count, 2); doc_put(h + 10, m->edge_count, 2);
    doc_put(h + 12, (unsigned long)m->next_vertex_id, 4);
    doc_put(h + 16, (unsigned long)m->next_edge_id, 4);
    doc_put(h + 20, (unsigned long)m->vertex_count * 8UL +
                   (unsigned long)m->edge_count * 6UL, 4);
    status = doc_emit(f, h, sizeof(h), &crc); if (status) return status;
    for (i = 0; i < m->vertex_count; ++i) {
        doc_put(r, m->vertices[i].id, 2);
        doc_put(r + 2, (unsigned long)m->vertices[i].x & 65535UL, 2);
        doc_put(r + 4, (unsigned long)m->vertices[i].y & 65535UL, 2);
        doc_put(r + 6, (unsigned long)m->vertices[i].z & 65535UL, 2);
        status = doc_emit(f, r, 8, &crc); if (status) return status;
    }
    for (i = 0; i < m->edge_count; ++i) {
        doc_put(r, m->edges[i].id, 2);
        doc_put(r + 2, m->edges[i].a, 2); doc_put(r + 4, m->edges[i].b, 2);
        status = doc_emit(f, r, 6, &crc); if (status) return status;
    }
    doc_put(r, ext_len, 2);
    status = doc_emit(f, r, 2, &crc); if (status) return status;
    if (ext_len) {
        status = doc_emit(f, (const unsigned char *)ext, ext_len, &crc);
        if (status) return status;
    }
    doc_put(r, crc, 2);
    status = doc_emit(f, r, 2, NULL);
    if (status || fflush(f) != 0 || ferror(f)) return CAD_DOC_IO;
    return CAD_DOC_OK;
}

int cad_document_write(FILE *f, const CadModel *m)
{
    return cad_document_write_ex(f, m, NULL, 0);
}

static int doc_upper(int c)
{
    return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

static int doc_namechar(int c)
{
    c = doc_upper(c);
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '_' || c == '-';
}

static int doc_paths(const char *path, char *tmp, char *bak)
{
    unsigned int n, base = 0, dot, i, stem, ext;
    char name[9], suffix[4];
    if (!path) return CAD_DOC_ARG;
    for (n = 0; n <= CAD_DOC_PATH_MAX && path[n]; ++n) { }
    if (!n || n > CAD_DOC_PATH_MAX) return CAD_DOC_ARG;
    for (i = 0; i < n; ++i)
        if (path[i] == '/' || path[i] == '\\' || path[i] == ':') base = i + 1;
    dot = n;
    for (i = base; i < n; ++i) {
        if (path[i] == '.' && dot == n) dot = i;
        else if (!doc_namechar((unsigned char)path[i])) return CAD_DOC_ARG;
    }
    stem = dot - base; ext = dot == n ? 0 : n - dot - 1;
    if (!stem || stem > 8 || ext > 3 || (dot != n && !ext) ||
        dot + 4 > CAD_DOC_PATH_MAX) return CAD_DOC_ARG;
    for (i = 0; i < stem; ++i) name[i] = (char)doc_upper(path[base + i]);
    name[stem] = 0;
    if (!strcmp(name, "CON") || !strcmp(name, "PRN") ||
        !strcmp(name, "AUX") || !strcmp(name, "NUL") ||
        (stem == 4 && (!strncmp(name, "COM", 3) || !strncmp(name, "LPT", 3)) &&
         name[3] >= '1' && name[3] <= '9')) return CAD_DOC_ARG;
    if (ext == 3) {
        for (i = 0; i < 3; ++i) suffix[i] = (char)doc_upper(path[dot + 1 + i]);
        suffix[3] = 0;
        if (!strcmp(suffix, "TMP") || !strcmp(suffix, "BAK"))
            return CAD_DOC_COLLISION;
    }
    memcpy(tmp, path, dot); memcpy(bak, path, dot);
    strcpy(tmp + dot, ".TMP"); strcpy(bak + dot, ".BAK");
    return CAD_DOC_OK;
}

/* 0 absent, 1 regular, 2 other filesystem object, -1 stat error. */
static int doc_state(const char *path)
{
    struct stat s;
    if (DOC_STAT(path, &s)) return errno == ENOENT ? 0 : -1;
#ifdef S_ISREG
    return S_ISREG(s.st_mode) ? 1 : 2;
#else
    return (s.st_mode & S_IFMT) == S_IFREG ? 1 : 2;
#endif
}

int cad_document_load_ex(const char *path, CadModel *live, CadModel *stage,
                         char *ext_buffer, unsigned ext_capacity,
                         unsigned *ext_len)
{
    char tmp[CAD_DOC_PATH_MAX + 1], bak[CAD_DOC_PATH_MAX + 1];
    FILE *f;
    unsigned int len = 0;
    int status;
    if (!live || !stage || live == stage) return CAD_DOC_ARG;
    status = doc_paths(path, tmp, bak); if (status) return status;
    if (doc_state(path) != 1) return CAD_DOC_IO;
    f = fopen(path, "rb"); if (!f) return CAD_DOC_IO;
    status = doc_parse(f, stage, &len);
    if (fclose(f) != 0) status = CAD_DOC_IO;
    return doc_publish(status, live, stage, len, ext_buffer, ext_capacity,
                       ext_len);
}

int cad_document_load(const char *path, CadModel *live, CadModel *stage)
{
    return cad_document_load_ex(path, live, stage, NULL, 0, NULL);
}

int cad_document_save_ex(const char *path, const CadModel *m, const char *ext,
                         unsigned ext_len)
{
    char tmp[CAD_DOC_PATH_MAX + 1], bak[CAD_DOC_PATH_MAX + 1];
    FILE *f;
    int status, old, t, b;
    if (!m || m->transaction_active) return CAD_DOC_ARG;
    status = doc_paths(path, tmp, bak); if (status) return status;
    status = doc_writable(m, ext, ext_len); if (status) return status;
    old = doc_state(path); t = doc_state(tmp); b = doc_state(bak);
    if (old < 0 || t < 0 || b < 0) return CAD_DOC_IO;
    if (old == 2 || t || b) return CAD_DOC_COLLISION;
    f = fopen(tmp, "wb"); if (!f) return CAD_DOC_IO;
    status = cad_document_write_ex(f, m, ext, ext_len);
    if (fclose(f) != 0) status = CAD_DOC_IO;
    if (status) { remove(tmp); return status; }
    if (old && rename(path, bak) != 0) {
        remove(tmp); return CAD_DOC_IO;
    }
    if (rename(tmp, path) != 0) {
        if (old) {
            /* Do not remove either recovery copy if restoration also fails. */
            if (rename(bak, path) != 0) return CAD_DOC_IO;
        }
        return CAD_DOC_IO;
    }
    if (old && remove(bak) != 0) return CAD_DOC_IO;
    return CAD_DOC_OK;
}

int cad_document_save(const char *path, const CadModel *m)
{
    return cad_document_save_ex(path, m, NULL, 0);
}
