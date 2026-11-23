#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* fseeko */
#define _FILE_OFFSET_BITS 64
#endif
#include "bru.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

enum { BLOCK = 512, DESCRIPTORS = 48, FILE_NUMBERS = 65536 };

typedef struct {
    uint32_t key;               /* The 24-bit LBN is unique within a Files-11 volume. */
    uint16_t file;
    const uint8_t *data;
} BlockRef;

typedef struct {
    const uint8_t **headers;
    BlockRef *blocks;
    size_t used, capacity;
    unsigned number, listed, recovered, incomplete;
    char name[13];
} Catalog;

static unsigned word(const uint8_t *p)
{
    return p[0] | (unsigned)p[1] << 8;
}

static unsigned longword(const uint8_t *p)
{
    return word(p) << 16 | word(p + 2);
}

bool bru_header_shape(const uint8_t *p)
{
    return p[0] >= 23 && p[0] + 5 <= p[1] && p[1] <= 249 && word(p + 2) && word(p + 6) == 0401;
}

bool bru_file_header(const uint8_t *p)
{
    if (!bru_header_shape(p))
        return false;
    unsigned sum = 0;
    for (int i = 0; i < 510; i += 2)
        sum += word(p + i);
    return (sum & 65535) == word(p + 510);
}

bool bru_data_layout(const uint8_t *p, unsigned blocks)
{
    for (unsigned slot = 0; blocks && slot < DESCRIPTORS; slot += 6) {
        unsigned n = p[slot + 3] + 1u;
        unsigned lbn = (unsigned)p[slot + 2] << 16 | word(p + slot + 4);
        if (!word(p + slot) || n > blocks || lbn + n > 0x1000000u)
            return false;
        blocks -= n;
    }
    return blocks == 0;
}

static bool add_data(Catalog *c, const uint8_t *p, unsigned size)
{
    unsigned remaining = (size - DESCRIPTORS) / BLOCK;
    if (!bru_data_layout(p, remaining))
        return true;
    if (c->capacity < c->used + remaining) {
        size_t capacity = c->capacity ? c->capacity * 2 : 4096;
        BlockRef *blocks = realloc(c->blocks, capacity * sizeof(*blocks));
        if (!blocks)
            return false;
        c->blocks = blocks;
        c->capacity = capacity;
    }
    const uint8_t *data = p + DESCRIPTORS;
    for (unsigned slot = 0; remaining && slot < DESCRIPTORS; slot += 6) {
        unsigned file = word(p + slot);
        unsigned n = p[slot + 3] + 1u;
        unsigned lbn = (unsigned)p[slot + 2] << 16 | word(p + slot + 4);
        for (unsigned i = 0; i < n; ++i, data += BLOCK)
            c->blocks[c->used++] = (BlockRef){lbn + i, (uint16_t)file, data};
        remaining -= n;
    }
    return true;
}

static int compare(const void *ap, const void *bp)
{
    const BlockRef *a = ap, *b = bp;
    if (a->key != b->key)
        return a->key < b->key ? -1 : 1;
    return (int)a->file - (int)b->file;
}

static const uint8_t *find_block(const Catalog *c, unsigned file, unsigned ext, uint32_t lbn)
{
    if (!c->used)
        return NULL;
    BlockRef key = {lbn, (uint16_t)file, NULL};
    const BlockRef *ref = bsearch(&key, c->blocks, c->used, sizeof(*c->blocks), compare);
    if (!ref && ext != file) {
        key.file = (uint16_t)ext;
        ref = bsearch(&key, c->blocks, c->used, sizeof(*c->blocks), compare);
    }
    if (!ref)
        return NULL;
    const BlockRef *end = c->blocks + c->used;
    for (const BlockRef *p = ref; p > c->blocks && !compare(ref, p - 1); --p)
        if (memcmp(ref->data, (p - 1)->data, BLOCK))
            return NULL;
    for (const BlockRef *p = ref + 1; p < end && !compare(ref, p); ++p)
        if (memcmp(ref->data, p->data, BLOCK))
            return NULL;
    return ref->data;
}

static void radix50(const uint8_t *p, char *out, size_t words)
{
    static const char digits[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ$.%0123456789";
    size_t i = 0;
    for (; words; --words, p += 2) {
        unsigned n = word(p);
        if (n >= 64000)
            n = 29 * 1600 + 29 * 40 + 29;
        out[i++] = digits[n / 1600];
        out[i++] = digits[n / 40 % 40];
        out[i++] = digits[n % 40];
    }
    while (i && out[i - 1] == ' ')
        --i;
    out[i] = 0;
}

static bool file_data(const Catalog *c, unsigned file, uint64_t length, FILE *out)
{
    unsigned id = file, segment = 0;
    while (length && segment < 256) {
        const uint8_t *p = c->headers[id];
        if (!p)
            return false;
        const uint8_t *map = p + p[1] * 2;
        unsigned used = 2u * map[8], span = map[6] + map[7];
        bool small = map[6] == 1 && map[7] == 3;
        bool large = map[6] == 2 && map[7] == 4;
        if (segment != map[0] || used > 500u - p[1] * 2 || (!small && !large) || used % span)
            return false;
        for (unsigned i = 0; i < used && length; i += span) {
            const uint8_t *entry = map + 10 + i;
            unsigned n = (small ? entry[1] : word(entry)) + 1u;
            uint32_t lbn = small ? (uint32_t)entry[0] << 16 | word(entry + 2) : longword(entry + 2);
            for (unsigned j = 0; j < n && length; ++j) {
                const uint8_t *block = find_block(c, file, id, lbn + j);
                if (!block)
                    return false;
                size_t size = length < BLOCK ? (size_t)length : BLOCK;
                if (out && fwrite(block, 1, size, out) != size)
                    return false;
                length -= size;
            }
        }
        id = word(map + 2);
        if (!id || !c->headers[id] || word(c->headers[id] + 4) != word(map + 4) || map[1])
            break;
        ++segment;
    }
    return length == 0;
}

static bool write_file(const Catalog *c, unsigned id, uint64_t size, const char *directory,
                       const char *name, const char *ext, unsigned version, char *err, size_t errlen)
{
    char path[4096];
    const uint8_t *h = c->headers[id];
    int n = snprintf(path, sizeof(path), "%s/%02u-%03o-%03o-%05u-%s%s%s.%u", directory, c->number,
                     h[9], h[8], id, name, *ext ? "." : "", ext, version);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        snprintf(err, errlen, "Output path is too long");
        return false;
    }
    FILE *out = fopen(path, "wbx");
    if (!out) {
        snprintf(err, errlen, "Cannot create %.240s: %s", path, strerror(errno));
        return false;
    }
    bool ok = file_data(c, id, size, out);
    if (fclose(out))
        ok = false;
    if (!ok) {
        remove(path);
        snprintf(err, errlen, "Cannot write %.300s", path);
    }
    return ok;
}

static bool list_set(Catalog *c, const char *directory, FILE *listing, char *err, size_t errlen)
{
    if (!c->number)
        return true;
    if (c->used)
        qsort(c->blocks, c->used, sizeof(*c->blocks), compare);
    fprintf(listing, "\nBackup set %u  %s\n", c->number, c->name);
    for (unsigned id = 1; id < FILE_NUMBERS; ++id) {
        const uint8_t *h = c->headers[id];
        if (!h || h[h[1] * 2])
            continue;
        char name[10], ext[4];
        const uint8_t *ident = h + h[0] * 2;
        radix50(ident, name, 3);
        radix50(ident + 6, ext, 1);
        unsigned version = word(ident + 8);
        uint32_t vbn = longword(h + 22);
        unsigned end = word(h + 26);
        uint64_t size = vbn ? (uint64_t)(vbn - 1) * BLOCK + end : end;
        bool complete = vbn && end <= BLOCK && file_data(c, id, size, NULL);
        ++c->listed;
        if (!complete)
            ++c->incomplete;
        fprintf(listing, "[%03o,%03o] %-9s %-3s %5u  %10" PRIu64 " %s\n",
                h[9], h[8], name, ext, version, size, complete ? "" : "partial");
        if (directory && complete) {
            if (!write_file(c, id, size, directory, name, ext, version, err, errlen))
                return false;
            ++c->recovered;
        }
    }
    return true;
}

static bool make_directory(const char *directory)
{
#ifdef _WIN32
    return !_mkdir(directory) || errno == EEXIST;
#else
    return !mkdir(directory, 0777) || errno == EEXIST;
#endif
}

static bool backup_label(const Tape *t, size_t i)
{
    if (i + 1 >= t->count)
        return false;
    const TapeRecord *r = &t->records[i], *next = r + 1;
    return r->kind == TAPE_DATA && r->length == 80 &&
        next->kind == TAPE_DATA && next->length == 80 &&
        !memcmp(t->bytes + r->offset, "HDR1", 4) &&
        !memcmp(t->bytes + next->offset, "HDR2U04144", 10);
}

bool bru_catalog(const Tape *t, const char *directory, FILE *listing, char *err, size_t errlen)
{
    bool found = false;
    for (size_t i = 0; i < t->count && !found; ++i)
        found = backup_label(t, i);
    if (!found) {
        snprintf(err, errlen, "No supported DEC BRU backup set found");
        return false;
    }
    if (directory && !make_directory(directory)) {
        snprintf(err, errlen, "Cannot create %.240s: %s", directory, strerror(errno));
        return false;
    }
    Catalog c = {0};
    c.headers = calloc(FILE_NUMBERS, sizeof(*c.headers));
    bool ok = false;
    if (!c.headers) {
        snprintf(err, errlen, "BRU catalog allocation failed");
        return false;
    }
    enum { SKIP, HEADERS, CONTENT } phase = SKIP;
    for (size_t i = 0; i < t->count; ++i) {
        const TapeRecord *r = &t->records[i];
        if (r->kind != TAPE_DATA)
            continue;
        const uint8_t *p = t->bytes + r->offset;
        unsigned size = r->length;
        if (backup_label(t, i)) {
            if (!list_set(&c, directory, listing, err, errlen))
                goto done;
            c.used = 0;
            memset(c.headers, 0, FILE_NUMBERS * sizeof(*c.headers));
            ++c.number;
            for (unsigned j = 0; j < 12; ++j)
                c.name[j] = p[4 + j] >= 32 && p[4 + j] < 127 ? (char)p[4 + j] : ' ';
            phase = SKIP;
        } else if (!c.number) {
            continue;
        } else if (size == 80 && !memcmp(p, "HEADHEAD", 8)) {
            phase = HEADERS;
        } else if (size == 80 && !memcmp(p, "DATA", 4) && !memcmp(p + 8, "DATADATA", 8)) {
            phase = CONTENT;
        } else if (size == 80 && !memcmp(p, "EOF1", 4)) {
            phase = SKIP;
        } else if (phase == HEADERS && size <= 8 * BLOCK && size % BLOCK == 0) {
            for (unsigned j = 0; j < size; j += BLOCK)
                if (bru_file_header(p + j))
                    c.headers[word(p + j + 2)] = p + j;
        } else if (phase == CONTENT && size >= DESCRIPTORS + BLOCK &&
                   size <= DESCRIPTORS + 8 * BLOCK && (size - DESCRIPTORS) % BLOCK == 0) {
            if (!add_data(&c, p, size)) {
                snprintf(err, errlen, "BRU data index allocation failed");
                goto done;
            }
        }
    }
    if (!list_set(&c, directory, listing, err, errlen))
        goto done;
    fprintf(listing, "\n%u file headers, %u incomplete files", c.listed, c.incomplete);
    if (directory)
        fprintf(listing, ", %u files extracted", c.recovered);
    fputc('\n', listing);
    ok = true;
done:
    free(c.blocks);
    free(c.headers);
    return ok;
}

/* ---- Restore to a Files-11 volume image ---------------------------------
 *
 * Writes every captured block at its original LBN, so task images installed
 * in a saved system stay where the system expects them. The backup carries
 * the boot block, the home block and the directory contents but not the
 * index file, the storage bitmap or the directory headers; those are
 * rebuilt as INI lays them out (Files-11 ODS-1 specification, 1981). */

typedef struct {
    unsigned fnum, fseq, owner, protection, version, blocks;
    uint16_t name[3], type;
    uint8_t *data;
    uint32_t lbn;
} Ufd;

typedef struct {
    uint32_t lbn, count;
} Extent;

typedef struct {
    Catalog c;
    const uint8_t *boot, *home;
    Ufd *ufd;
    unsigned ufds;
    uint8_t *used;
    uint32_t size;
    char date[8], time[7];
} Volume;

static void put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v >> 16);
    put16(p + 2, v & 0xffff);
}

static unsigned rad50_word(const char *s)
{
    static const char digits[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ$.%0123456789";
    unsigned v = 0;
    for (int i = 0; i < 3; ++i) {
        const char *d = *s ? strchr(digits, *s) : NULL;
        v = v * 40 + (d ? (unsigned)(d - digits) : 0);
        if (*s)
            ++s;
    }
    return v;
}

static unsigned checksum(const uint8_t *p, unsigned words)
{
    unsigned sum = 0;
    for (unsigned i = 0; i < words; ++i)
        sum += word(p + 2 * i);
    return sum & 0xffff;
}

static bool home_block(const uint8_t *p)
{
    /* The format field also reads MINOSFILE11 or DOSDISKFCP on Romanian volumes. */
    unsigned level = word(p + 12);
    return word(p) && (word(p + 2) | word(p + 4)) && word(p + 6) && (level == 0401 || level == 0402) &&
        checksum(p, 29) == word(p + 58) && checksum(p, 255) == word(p + 510);
}

static void mark(uint8_t *used, uint32_t size, uint32_t lbn, uint32_t n)
{
    for (; n && lbn < size; --n, ++lbn)
        used[lbn / 8] |= (uint8_t)(1u << (lbn % 8));
}

static bool is_used(const Volume *v, uint32_t lbn)
{
    return v->used[lbn / 8] >> (lbn % 8) & 1;
}

/* First free run of n blocks at or after `near`, wrapping to the start. */
static bool allocate(Volume *v, uint32_t n, uint32_t near, uint32_t *out)
{
    for (int pass = 0; pass < 2; ++pass) {
        uint32_t run = 0, start = pass ? 0 : near, end = pass ? near : v->size;
        for (uint32_t lbn = start; lbn < end; ++lbn) {
            run = is_used(v, lbn) ? 0 : run + 1;
            if (run == n) {
                *out = lbn + 1 - n;
                mark(v->used, v->size, *out, n);
                return true;
            }
        }
    }
    return false;
}

/* Marks the blocks a header maps; false if the map is malformed or leaves the disk. */
static bool map_header(const uint8_t *h, Volume *v)
{
    const uint8_t *map = h + h[1] * 2;
    unsigned used = 2u * map[8], span = map[6] + map[7];
    bool small = map[6] == 1 && map[7] == 3, large = map[6] == 2 && map[7] == 4;
    if ((!small && !large) || used > 500u - h[1] * 2 || used % span)
        return false;
    for (unsigned i = 0; i < used; i += span) {
        const uint8_t *e = map + 10 + i;
        uint32_t n = (small ? e[1] : word(e)) + 1u;
        uint32_t lbn = small ? (uint32_t)e[0] << 16 | word(e + 2) : longword(e + 2);
        if (lbn + n > v->size)
            return false;
    }
    for (unsigned i = 0; i < used; i += span) {
        const uint8_t *e = map + 10 + i;
        uint32_t n = (small ? e[1] : word(e)) + 1u;
        uint32_t lbn = small ? (uint32_t)e[0] << 16 | word(e + 2) : longword(e + 2);
        mark(v->used, v->size, lbn, n);
    }
    return true;
}

static void make_header(const Volume *v, uint8_t *h, unsigned fnum, unsigned fseq, unsigned owner,
                        unsigned protection, const uint16_t name[4], unsigned rsize,
                        uint32_t eof_vbn, const Extent *ext, unsigned extents, uint32_t allocated)
{
    memset(h, 0, BLOCK);
    h[0] = 0x17;                        /* ident area at word 23 */
    h[1] = 0x2e;                        /* map area at word 46 */
    put16(h + 2, fnum);
    put16(h + 4, fseq);
    put16(h + 6, 0401);
    put16(h + 8, owner);
    put16(h + 10, protection);
    h[12] = fnum != 1 ? 0x80 : 0;       /* contiguous */
    h[14] = 1;                          /* fixed-length records */
    put16(h + 16, rsize);
    put32(h + 18, allocated);
    put32(h + 22, eof_vbn);
    uint8_t *id = h + 46;
    for (int i = 0; i < 4; ++i)
        put16(id + 2 * i, name[i]);
    put16(id + 8, 1);
    memcpy(id + 12, v->date, 7);
    memcpy(id + 19, v->time, 6);
    memcpy(id + 25, v->date, 7);
    memcpy(id + 32, v->time, 6);
    uint8_t *map = h + 92, *p = map + 10;
    map[6] = 1;
    map[7] = 3;
    map[9] = (uint8_t)((BLOCK - 2 - 92 - 10) / 2);
    for (unsigned i = 0; i < extents; ++i)
        for (uint32_t lbn = ext[i].lbn, left = ext[i].count; left;) {
            uint32_t n = left > 256 ? 256 : left;
            p[0] = (uint8_t)(lbn >> 16);
            p[1] = (uint8_t)(n - 1);
            put16(p + 2, lbn & 0xffff);
            p += 4;
            map[8] += 2;
            lbn += n;
            left -= n;
        }
    put16(h + 510, checksum(h, 255));
}

static void rad50_name(uint16_t out[4], const char *name, const char *type)
{
    char nine[10];
    snprintf(nine, sizeof(nine), "%-9s", name);
    for (int i = 0; i < 3; ++i)
        out[i] = (uint16_t)rad50_word(nine + 3 * i);
    out[3] = (uint16_t)rad50_word(type);
}

static bool put_block(FILE *f, uint32_t lbn, const uint8_t *data)
{
#ifdef _WIN32
    int sought = _fseeki64(f, (long long)lbn * BLOCK, SEEK_SET);
#else
    int sought = fseeko(f, (off_t)lbn * BLOCK, SEEK_SET);
#endif
    return !sought && fwrite(data, 1, BLOCK, f) == BLOCK;
}

static bool parse_volume(const Tape *t, Volume *v, char *err, size_t errlen)
{
    enum { LABEL, SUMMARY, BOOT, HOME, DIRECTORIES, HEADERS, CONTENT } phase = LABEL;
    Ufd *current = NULL;
    const uint8_t *previous[4] = {NULL};
    for (size_t i = 0; i < t->count; ++i) {
        const TapeRecord *r = &t->records[i];
        if (r->kind != TAPE_DATA)
            continue;
        const uint8_t *p = t->bytes + r->offset;
        unsigned size = r->length;
        /* Later backup sets of the same volume add their files; a set
         * from another volume ends the restore. */
        if (backup_label(t, i)) {
            phase = SUMMARY;
            current = NULL;
            ++i;
            continue;
        }
        if (phase == LABEL)
            continue;
        if (size == 80 && !memcmp(p, "HEADHEAD", 8)) {
            phase = HEADERS;
        } else if (size == 80 && !memcmp(p, "DATA", 4) && !memcmp(p + 8, "DATADATA", 8)) {
            phase = CONTENT;
        } else if (size == 80 && !memcmp(p, "EOF1", 4)) {
            phase = LABEL;
        } else if (phase == SUMMARY && size == 80) {
            phase = BOOT;
        } else if (phase == BOOT && size == BLOCK) {
            if (!v->boot)
                v->boot = p;
            phase = HOME;
        } else if (phase == HOME && size == BLOCK) {
            if (v->home && memcmp(p, v->home, BLOCK))
                break;
            if (!v->home && home_block(p))
                v->home = p;
            if (!v->home)
                break;
            previous[0] = p;
            phase = DIRECTORIES;
        } else if (phase == DIRECTORIES && size == 80 && !memcmp(p, "UFD", 4)) {
            /* The MFD is rebuilt; a directory seen again takes its new contents. */
            unsigned fnum = word(p + 4);
            current = NULL;
            if (fnum <= 5)
                continue;
            for (unsigned d = 0; d < v->ufds && !current; ++d)
                if (v->ufd[d].fnum == fnum)
                    current = &v->ufd[d];
            if (!current) {
                Ufd *u = realloc(v->ufd, (v->ufds + 1) * sizeof(*u));
                if (!u)
                    goto memory;
                v->ufd = u;
                current = &u[v->ufds++];
                memset(current, 0, sizeof(*current));
            }
            free(current->data);
            current->data = NULL;
            current->blocks = 0;
            current->fnum = fnum;
            current->fseq = word(p + 6);
            for (int j = 0; j < 3; ++j)
                current->name[j] = (uint16_t)word(p + 10 + 2 * j);
            current->type = (uint16_t)word(p + 16);
            current->version = word(p + 18);
            current->owner = word(p + 26);
            current->protection = word(p + 28);
        } else if (phase == DIRECTORIES && size == BLOCK && current) {
            uint8_t *data = realloc(current->data, (size_t)(current->blocks + 1) * BLOCK);
            if (!data)
                goto memory;
            /* BRU rotates its buffers: the entries past the end of a
             * directory still hold an earlier block's tail. */
            uint8_t *block = data + (size_t)current->blocks++ * BLOCK;
            memcpy(block, p, BLOCK);
            unsigned end = BLOCK;
            for (unsigned k = 0; k < 4; ++k) {
                unsigned same = BLOCK;
                while (previous[k] && same &&
                       !memcmp(p + same - 16, previous[k] + same - 16, 16))
                    same -= 16;
                if (same < end)
                    end = same;
            }
            memset(block + end, 0, BLOCK - end);
            current->data = data;
            memmove(previous + 1, previous, 3 * sizeof(*previous));
            previous[0] = p;
        } else if (phase == HEADERS && size <= 8 * BLOCK && size % BLOCK == 0) {
            for (unsigned j = 0; j < size; j += BLOCK)
                if (bru_file_header(p + j))
                    v->c.headers[word(p + j + 2)] = p + j;
        } else if (phase == CONTENT && size >= DESCRIPTORS + BLOCK &&
                   size <= DESCRIPTORS + 8 * BLOCK && (size - DESCRIPTORS) % BLOCK == 0) {
            if (!add_data(&v->c, p, size))
                goto memory;
        }
    }
    if (!v->home) {
        snprintf(err, errlen, "No BRU backup set with a Files-11 home block found");
        return false;
    }
    return true;
memory:
    snprintf(err, errlen, "BRU restore allocation failed");
    return false;
}

static bool build_volume(Volume *v, FILE *f, FILE *report, char *err, size_t errlen)
{
    const uint8_t *home = v->home;
    unsigned ibsz = word(home), fmax = word(home + 6);
    uint32_t iblb = longword(home + 2);
    memcpy(v->date, home + 60, 7);
    memcpy(v->time, home + 67, 6);
    if (iblb + ibsz + 16 > v->size || fmax > ibsz * 4096u || fmax >= FILE_NUMBERS) {
        snprintf(err, errlen, "The source volume's index file lies outside this disk");
        return false;
    }
    mark(v->used, v->size, 0, 2);
    mark(v->used, v->size, iblb, ibsz + 16);

    /* Restored files: every header that maps inside the disk. */
    unsigned files = 0, top = 5;
    for (unsigned id = 1; id < FILE_NUMBERS; ++id) {
        const uint8_t *h = v->c.headers[id];
        if (!h)
            continue;
        if (id <= 5 || id > fmax || !map_header(h, v)) {
            v->c.headers[id] = NULL;
            continue;
        }
        if (!h[h[1] * 2])
            ++files;
        if (id > top)
            top = id;
    }
    for (size_t i = 0; i < v->c.used; ++i) {
        if (v->c.blocks[i].key >= v->size) {
            snprintf(err, errlen, "Backup data at LBN %" PRIu32 " lies outside this disk",
                     v->c.blocks[i].key);
            return false;
        }
        mark(v->used, v->size, v->c.blocks[i].key, 1);
    }
    for (unsigned d = 0; d < v->ufds; ++d) {
        const Ufd *u = &v->ufd[d];
        if (u->fnum <= 5 || u->fnum > fmax || v->c.headers[u->fnum]) {
            snprintf(err, errlen, "Directory file number %u conflicts with a file header", u->fnum);
            return false;
        }
        if (u->fnum > top)
            top = u->fnum;
    }

    /* Index file: boot, home, bitmap and headers 1-16 in place, the
     * remaining headers in one new run near the index. */
    unsigned extra = top > 16 ? top - 16 + 64 : 0;
    Extent index[4] = {{0, 1}, {1, 1}, {iblb, ibsz + 16}, {0, 0}};
    unsigned index_extents = 3;
    if (extra) {
        if (!allocate(v, extra, iblb, &index[3].lbn))
            goto full;
        index[3].count = extra;
        index_extents = 4;
    }
    uint32_t map_blocks = (v->size + 4095) / 4096;
    Extent bitmap = {0, map_blocks + 1}, mfd = {0, (5 + v->ufds + BLOCK / 16 - 1) / (BLOCK / 16)};
    if (map_blocks > 126 || !allocate(v, bitmap.count, iblb, &bitmap.lbn) ||
        !allocate(v, mfd.count, iblb, &mfd.lbn))
        goto full;
    for (unsigned d = 0; d < v->ufds; ++d)
        if (!allocate(v, v->ufd[d].blocks ? v->ufd[d].blocks : 1, iblb, &v->ufd[d].lbn))
            goto full;

    uint8_t block[BLOCK];
    static const uint8_t zero[BLOCK] = {0};
    bool ok = put_block(f, v->size - 1, zero) && put_block(f, 1, home);
    if (v->boot)
        ok = ok && put_block(f, 0, v->boot);

    /* Captured data at its original place. */
    for (size_t i = 0; ok && i < v->c.used; ++i)
        ok = put_block(f, v->c.blocks[i].key, v->c.blocks[i].data);

    /* Directories keep the entries of restored files only. */
    unsigned entries = 0;
    for (unsigned d = 0; ok && d < v->ufds; ++d) {
        Ufd *u = &v->ufd[d];
        if (!u->blocks)
            ok = put_block(f, u->lbn, zero);
        for (unsigned b = 0; ok && b < u->blocks; ++b) {
            uint8_t *p = u->data + (size_t)b * BLOCK;
            for (unsigned e = 0; e < BLOCK; e += 16) {
                unsigned fnum = word(p + e);
                const uint8_t *h = fnum ? v->c.headers[fnum] : NULL;
                if (!fnum)
                    continue;
                if (!h || word(h + 4) != word(p + e + 2) || h[h[1] * 2])
                    memset(p + e, 0, 16);
                else
                    ++entries;
            }
            ok = put_block(f, u->lbn + b, p);
        }
    }

    /* Master file directory: the five known files and the directories. */
    static const char *const known[5][2] = {
        {"INDEXF", "SYS"}, {"BITMAP", "SYS"}, {"BADBLK", "SYS"}, {"000000", "DIR"}, {"CORIMG", "SYS"},
    };
    for (unsigned k = 0; ok && k < mfd.count * (BLOCK / 16); ++k) {
        if (k % (BLOCK / 16) == 0)
            memset(block, 0, BLOCK);
        uint8_t *e = block + 16 * (k % (BLOCK / 16));
        if (k >= 5 + v->ufds) {
            if (k % (BLOCK / 16) == BLOCK / 16 - 1)
                ok = put_block(f, mfd.lbn + k / (BLOCK / 16), block);
            continue;
        }
        uint16_t name[4];
        unsigned fnum = k + 1, fseq = k + 1, version = 1;
        if (k < 5) {
            rad50_name(name, known[k][0], known[k][1]);
        } else {
            const Ufd *u = &v->ufd[k - 5];
            memcpy(name, u->name, sizeof(u->name));
            name[3] = u->type;
            fnum = u->fnum;
            fseq = u->fseq;
            version = u->version ? u->version : 1;
        }
        put16(e, fnum);
        put16(e + 2, fseq);
        for (int j = 0; j < 4; ++j)
            put16(e + 6 + 2 * j, name[j]);
        put16(e + 14, version);
        if (k % (BLOCK / 16) == BLOCK / 16 - 1)
            ok = put_block(f, mfd.lbn + k / (BLOCK / 16), block);
    }

    /* Storage bitmap: a control block, then one bit per block, set when free. */
    memset(block, 0, BLOCK);
    block[3] = (uint8_t)map_blocks;
    put32(block + 4 + 4 * map_blocks, v->size);
    ok = ok && put_block(f, bitmap.lbn, block);
    for (uint32_t b = 0; ok && b < map_blocks; ++b) {
        memset(block, 0, BLOCK);
        for (uint32_t j = 0; j < 4096; ++j) {
            uint32_t lbn = b * 4096 + j;
            if (lbn < v->size && !is_used(v, lbn))
                block[j / 8] |= (uint8_t)(1u << (j % 8));
        }
        ok = put_block(f, bitmap.lbn + 1 + b, block);
    }

    /* Index file bitmap: bit n-1 for file n. */
    uint8_t *in_use = calloc(fmax + 1, 1);
    if (!in_use) {
        snprintf(err, errlen, "BRU restore allocation failed");
        return false;
    }
    for (unsigned id = 1; id <= fmax; ++id)
        in_use[id] = id <= 5 || v->c.headers[id] != NULL;
    for (unsigned d = 0; d < v->ufds; ++d)
        in_use[v->ufd[d].fnum] = 1;
    for (unsigned b = 0; ok && b < ibsz; ++b) {
        memset(block, 0, BLOCK);
        for (unsigned j = 0; j < 4096 && b * 4096 + j + 1 <= fmax; ++j)
            if (in_use[b * 4096 + j + 1])
                block[j / 8] |= (uint8_t)(1u << (j % 8));
        ok = put_block(f, iblb + b, block);
    }
    free(in_use);

    /* Headers: file n is index VBN 2 + IBSZ + n. */
#define HEADER_LBN(n) ((n) <= 16 ? iblb + ibsz + (n) - 1 : index[3].lbn + (n) - 17)
    unsigned owner = word(home + 30), protection = word(home + 36);
    uint16_t name[4];
    rad50_name(name, "INDEXF", "SYS");
    make_header(v, block, 1, 1, owner, protection, name, BLOCK, 2 + ibsz + top + 1, index,
                index_extents, 2 + ibsz + 16 + extra);
    ok = ok && put_block(f, HEADER_LBN(1), block);
    rad50_name(name, "BITMAP", "SYS");
    make_header(v, block, 2, 2, owner, protection, name, BLOCK, bitmap.count + 1, &bitmap, 1,
                bitmap.count);
    ok = ok && put_block(f, HEADER_LBN(2), block);
    rad50_name(name, "BADBLK", "SYS");
    make_header(v, block, 3, 3, owner, protection, name, BLOCK, 1, NULL, 0, 0);
    ok = ok && put_block(f, HEADER_LBN(3), block);
    rad50_name(name, "000000", "DIR");
    make_header(v, block, 4, 4, owner, protection, name, 16, mfd.count + 1, &mfd, 1, mfd.count);
    ok = ok && put_block(f, HEADER_LBN(4), block);
    rad50_name(name, "CORIMG", "SYS");
    make_header(v, block, 5, 5, owner, protection, name, BLOCK, 1, NULL, 0, 0);
    ok = ok && put_block(f, HEADER_LBN(5), block);
    for (unsigned d = 0; ok && d < v->ufds; ++d) {
        const Ufd *u = &v->ufd[d];
        Extent e = {u->lbn, u->blocks ? u->blocks : 1};
        memcpy(name, u->name, sizeof(u->name));
        name[3] = u->type;
        make_header(v, block, u->fnum, u->fseq, u->owner, u->protection, name, 16, e.count + 1, &e, 1,
                    e.count);
        put16(block + 46 + 8, u->version ? u->version : 1);
        put16(block + 510, checksum(block, 255));
        ok = put_block(f, HEADER_LBN(u->fnum), block);
    }
    for (unsigned id = 6; ok && id <= top; ++id)
        if (v->c.headers[id])
            ok = put_block(f, HEADER_LBN(id), v->c.headers[id]);
#undef HEADER_LBN
    if (!ok) {
        snprintf(err, errlen, "Cannot write the disk image");
        return false;
    }
    char label[13] = {0};
    for (int i = 0; i < 12 && home[14 + i] >= 32 && home[14 + i] < 127; ++i)
        label[i] = (char)home[14 + i];
    fprintf(report, "Volume %s: %u files in %u directories, %u directory entries, %s boot block\n",
            label, files, v->ufds, entries, v->boot ? "original" : "no");
    return true;
full:
    snprintf(err, errlen, "No free space left for the rebuilt file structure");
    return false;
}

bool bru_restore(const Tape *t, const char *image, uint32_t blocks, FILE *report, char *err,
                 size_t errlen)
{
    Volume v = {0};
    bool ok = false;
    v.size = blocks;
    v.c.headers = calloc(FILE_NUMBERS, sizeof(*v.c.headers));
    v.used = calloc(blocks / 8 + 1, 1);
    if (!v.c.headers || !v.used) {
        snprintf(err, errlen, "BRU restore allocation failed");
        goto done;
    }
    if (!parse_volume(t, &v, err, errlen))
        goto done;
    FILE *f = fopen(image, "wbx");
    if (!f) {
        snprintf(err, errlen, "Cannot create %.240s: %s", image, strerror(errno));
        goto done;
    }
    ok = build_volume(&v, f, report, err, errlen);
    if (fclose(f))
        ok = false;
    if (!ok)
        remove(image);
done:
    for (unsigned d = 0; d < v.ufds; ++d)
        free(v.ufd[d].data);
    free(v.ufd);
    free(v.used);
    free(v.c.blocks);
    free(v.c.headers);
    return ok;
}
