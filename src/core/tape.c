#include "tape.h"
#include "bru.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_LIMIT (256L * 1024L * 1024L)
#define RECORD_LIMIT 4000000u

static uint16_t le16(const uint8_t *b)
{
    return (uint16_t)(b[0] | (unsigned)b[1] << 8);
}

static uint32_t le32(const uint8_t *b)
{
    return le16(b) | (uint32_t)le16(b + 2) << 16;
}

uint8_t *read_image(const char *path, size_t *size, char *err, size_t errlen)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        snprintf(err, errlen, "Cannot open %.300s", path);
        return NULL;
    }
    long length = -1;
    if (!fseek(file, 0, SEEK_END))
        length = ftell(file);
    if (length < 0 || length > IMAGE_LIMIT || fseek(file, 0, SEEK_SET)) {
        snprintf(err, errlen, "Invalid or oversized image %.300s", path);
        fclose(file);
        return NULL;
    }
    uint8_t *bytes = malloc(length ? (size_t)length : 1);
    if (!bytes || fread(bytes, 1, (size_t)length, file) != (size_t)length) {
        snprintf(err, errlen, "Cannot read %.300s", path);
        fclose(file);
        free(bytes);
        return NULL;
    }
    fclose(file);
    *size = (size_t)length;
    return bytes;
}

static bool append(Tape *t, size_t offset, uint32_t length, TapeKind kind)
{
    if (t->count == t->capacity) {
        if (t->capacity >= RECORD_LIMIT)
            return false;
        size_t capacity = t->capacity ? t->capacity * 2 : 256;
        if (capacity > RECORD_LIMIT)
            capacity = RECORD_LIMIT;
        TapeRecord *records = realloc(t->records, capacity * sizeof(*records));
        if (!records)
            return false;
        t->records = records;
        t->capacity = capacity;
    }
    t->records[t->count++] = (TapeRecord){offset, length, kind};
    return true;
}

static bool tdup(Tape *t, const uint8_t *lengths, size_t size, char *err, size_t errlen)
{
    t->format = TAPE_TDUP;
    if (size < 256 || size % 2 || memcmp(lengths, "TDUP\0", 5)) {
        snprintf(err, errlen, "Invalid TDUP length file");
        return false;
    }
    size_t offset = 0;
    unsigned marks = 0;
    for (size_t i = 256; i < size; i += 2) {
        unsigned length = le16(lengths + i);
        if (length > t->size - offset) {
            snprintf(err, errlen, "TDUP record %zu extends past end of data", (i - 256) / 2 + 1);
            return false;
        }
        if (!append(t, offset, length, length ? TAPE_DATA : TAPE_MARK)) {
            snprintf(err, errlen, "Tape index allocation failed");
            return false;
        }
        offset += length;
        marks = length ? 0 : marks + 1;
    }
    if (offset != t->size) {
        snprintf(err, errlen, "TDUP length file accounts for %zu of %zu bytes", offset, t->size);
        return false;
    }
    t->prefix_only = marks < 2;
    return true;
}

static bool simh(Tape *t, char *err, size_t errlen)
{
    t->format = TAPE_SIMH;
    size_t offset = 0;
    unsigned marks = 0;
    while (t->size - offset >= 4) {
        uint32_t descriptor = le32(t->bytes + offset);
        if (!descriptor || descriptor == UINT32_MAX || descriptor == UINT32_MAX - 1) {
            TapeKind kind = !descriptor ? TAPE_MARK : descriptor == UINT32_MAX ? TAPE_END : TAPE_GAP;
            if (!append(t, offset, 0, kind))
                break;
            offset += 4;
            if (kind == TAPE_END) {
                t->unparsed = t->size - offset;
                return true;
            }
            marks = kind == TAPE_MARK ? marks + 1 : 0;
            continue;
        }
        uint32_t length = descriptor & 0x00ffffffu;
        size_t padded = (size_t)length + (length & 1u);
        if ((descriptor & 0x7f000000u) || !length || t->size - offset < padded + 8 ||
            le32(t->bytes + offset + 4 + padded) != descriptor)
            break;
        TapeKind kind = descriptor >> 31 ? TAPE_BAD : TAPE_DATA;
        if (!append(t, offset + 4, length, kind))
            break;
        offset += padded + 8;
        marks = 0;
    }
    if (t->count && offset == t->size) {
        t->prefix_only = marks < 2;
        return true;
    }
    snprintf(err, errlen, "Invalid or unsupported SIMH tape framing at byte %zu", offset);
    return false;
}

static bool is_at(const Tape *t, size_t offset, const char *tag, size_t length)
{
    return offset <= t->size && length <= t->size - offset &&
        !memcmp(t->bytes + offset, tag, length);
}

static bool take(Tape *t, size_t *offset, unsigned length)
{
    if (length > t->size - *offset || !append(t, *offset, length, length ? TAPE_DATA : TAPE_MARK))
        return false;
    *offset += length;
    return true;
}

/* The flat captures dropped filemarks and physical lengths. Only rebuild the
 * observed DEC BRU record families, and stop at the first ambiguous boundary. */
static bool bru_flat(Tape *t, char *err, size_t errlen)
{
    t->format = TAPE_BRU_FLAT;
    size_t offset = 0;
    if (!take(t, &offset, 80) || !take(t, &offset, 512))
        goto invalid;
    while (is_at(t, offset, "HDR1", 4) && is_at(t, offset + 80, "HDR2U04144", 10)) {
        static const unsigned header[] = {80, 80, 0, 80, 512, 512};
        for (size_t i = 0; i < sizeof(header) / sizeof(header[0]); ++i)
            if (!take(t, &offset, header[i]))
                goto invalid;

        enum { DIRECTORIES, HEADERS, DATA } phase = DIRECTORIES;
        while (offset < t->size && !is_at(t, offset, "EOF1", 4)) {
            unsigned next = 0;
            if (is_at(t, offset, "HEADHEAD", 8)) {
                phase = HEADERS;
                next = 80;
            } else if (is_at(t, offset, "DATA", 4) && is_at(t, offset + 8, "DATADATA", 8)) {
                phase = DATA;
                next = 80;
            } else if (phase != DATA && is_at(t, offset, "UFD\0", 4)) {
                next = 80;
            } else if (phase == DIRECTORIES) {
                next = 512;
            } else if (phase == HEADERS) {
                for (unsigned n = 0; n < 8 && offset + next + 512 <= t->size; ++n) {
                    if (!bru_header_shape(t->bytes + offset + next))
                        break;
                    next += 512;
                }
                size_t boundary = offset + next;
                if (next < 4096 && !is_at(t, boundary, "UFD\0", 4) &&
                    !is_at(t, boundary, "HEADHEAD", 8) && !is_at(t, boundary, "EOF1", 4) &&
                    !(is_at(t, boundary, "DATA", 4) && is_at(t, boundary + 8, "DATADATA", 8)))
                    next = 0;
            } else {
                for (unsigned n = 1; n <= 8; ++n) {
                    size_t boundary = offset + 48 + n * 512;
                    if (boundary > t->size)
                        break;
                    if ((n == 8 || is_at(t, boundary, "EOF1", 4) || is_at(t, boundary, "HEADHEAD", 8) ||
                         (is_at(t, boundary, "DATA", 4) && is_at(t, boundary + 8, "DATADATA", 8))) &&
                        bru_data_layout(t->bytes + offset, n)) {
                        next = 48 + n * 512;
                        break;
                    }
                }
            }
            if (!next || !take(t, &offset, next))
                goto invalid;
        }
        if (!is_at(t, offset, "EOF1", 4) || !is_at(t, offset + 80, "EOF2", 4) ||
            !take(t, &offset, 0) || !take(t, &offset, 80) || !take(t, &offset, 80) ||
            !take(t, &offset, 0))
            goto invalid;
        if (offset == t->size) {
            if (!take(t, &offset, 0))
                goto invalid;
            return true;
        }
    }
invalid:
    if (t->count >= 8) {
        t->prefix_only = true;
        t->unparsed = t->size - offset;
        return true;
    }
    snprintf(err, errlen, "Flat BRU framing could not be reconstructed at byte %zu. Supply the TDUP length file if available.", offset);
    return false;
}

static bool sidecar(const char *image, const char *ext, char *name, size_t size)
{
    const char *suffix = strrchr(image, '.');
    if (!suffix || (strcmp(suffix, ".dat") && strcmp(suffix, ".DAT")))
        return false;
    size_t stem = (size_t)(suffix - image);
    if (stem >= size || strlen(ext) >= size - stem)
        return false;
    memcpy(name, image, stem);
    strcpy(name + stem, ext);
    FILE *probe = fopen(name, "rb");
    if (!probe)
        return false;
    fclose(probe);
    return true;
}

bool tape_open(Tape *t, const char *image, const char *lengths, char *err, size_t errlen)
{
    Tape next = {0};
    next.bytes = read_image(image, &next.size, err, errlen);
    if (!next.bytes)
        return false;
    char name[4096];
    if (!lengths && (sidecar(image, ".LEN", name, sizeof(name)) ||
                     sidecar(image, ".len", name, sizeof(name))))
        lengths = name;
    bool ok;
    if (lengths) {
        size_t size;
        uint8_t *bytes = read_image(lengths, &size, err, errlen);
        ok = bytes && tdup(&next, bytes, size, err, errlen);
        free(bytes);
    } else if (is_at(&next, 0, "VOL1BACKUP", 10) && is_at(&next, 592, "HDR1", 4)) {
        ok = bru_flat(&next, err, errlen);
    } else {
        ok = simh(&next, err, errlen);
    }
    if (ok) {
        tape_close(t);
        *t = next;
    } else {
        tape_close(&next);
    }
    return ok;
}

void tape_close(Tape *t)
{
    free(t->bytes);
    free(t->records);
    memset(t, 0, sizeof(*t));
}

const TapeRecord *tape_step(Tape *t, bool reverse)
{
    while (reverse ? t->position > 0 : t->position < t->count) {
        size_t index = reverse ? --t->position : t->position++;
        const TapeRecord *record = t->records + index;
        if (record->kind == TAPE_END) {
            if (!reverse) {
                --t->position;
                return NULL;
            }
            continue;
        }
        if (record->kind != TAPE_GAP)
            return record;
    }
    return NULL;
}

static bool write_word(FILE *out, uint32_t word)
{
    const uint8_t b[] = {(uint8_t)word, (uint8_t)(word >> 8),
                         (uint8_t)(word >> 16), (uint8_t)(word >> 24)};
    return fwrite(b, 1, sizeof(b), out) == sizeof(b);
}

bool tape_export(const Tape *t, FILE *out)
{
    for (size_t i = 0; i < t->count; ++i) {
        const TapeRecord *r = t->records + i;
        uint32_t word = r->length;
        switch (r->kind) {
        case TAPE_MARK: word = 0; break;
        case TAPE_GAP: word = UINT32_MAX - 1u; break;
        case TAPE_END: word = UINT32_MAX; break;
        case TAPE_BAD: word |= 0x80000000u; break;
        case TAPE_DATA: break;
        }
        if (!write_word(out, word))
            return false;
        if (r->length && (fwrite(t->bytes + r->offset, 1, r->length, out) != r->length ||
                          ((r->length & 1u) && fputc(0, out) == EOF) ||
                          !write_word(out, word)))
            return false;
    }
    return true;
}

const char *tape_format_name(const Tape *t)
{
    switch (t->format) {
    case TAPE_TDUP: return "TDUP (.DAT + .LEN)";
    case TAPE_BRU_FLAT: return "DEC BRU (reconstructed framing)";
    case TAPE_SIMH: return "SIMH TAP";
    }
    return "Unknown";
}

static uint8_t ebcdic(uint8_t ch)
{
    if (ch >= 0xf0 && ch <= 0xf9) return (uint8_t)('0' + ch - 0xf0);
    if (ch >= 0xc1 && ch <= 0xc9) return (uint8_t)('A' + ch - 0xc1);
    if (ch >= 0xd1 && ch <= 0xd9) return (uint8_t)('J' + ch - 0xd1);
    if (ch >= 0xe2 && ch <= 0xe9) return (uint8_t)('S' + ch - 0xe2);
    if (ch == 0x40) return ' ';
    if (ch == 0x4b) return '.';
    return '?';
}

void tape_report(const Tape *t, FILE *out)
{
    size_t marks = 0, bad = 0, gaps = 0, data = 0;
    for (size_t i = 0; i < t->count; ++i) {
        switch (t->records[i].kind) {
        case TAPE_DATA: ++data; break;
        case TAPE_MARK: ++marks; break;
        case TAPE_BAD: ++bad; break;
        case TAPE_GAP: ++gaps; break;
        case TAPE_END: break;
        }
    }
    fprintf(out, "%s: %zu data records, %zu filemarks, %zu bad records, %zu gaps\n",
            tape_format_name(t), data, marks, bad, gaps);
    if (t->prefix_only)
        fputs("Capture ends without a two-filemark terminator. The final backup set may be partial.\n", out);
    if (t->unparsed)
        fprintf(out, "%zu trailing bytes outside the mounted tape%s.\n", t->unparsed,
                t->format == TAPE_BRU_FLAT ? " after an ambiguous record boundary" : " after end of medium");
    for (size_t i = 0; i < t->count; ++i) {
        const TapeRecord *r = t->records + i;
        if (r->kind != TAPE_DATA || r->length != 80)
            continue;
        const uint8_t *src = t->bytes + r->offset;
        char label[81];
        bool is_ebcdic = src[3] == 0xf1 || src[3] == 0xf2;
        for (size_t j = 0; j < 80; ++j) {
            uint8_t ch = is_ebcdic ? ebcdic(src[j]) : src[j];
            label[j] = ch >= 32 && ch <= 126 ? (char)ch : ' ';
        }
        label[80] = 0;
        if (!strncmp(label, "VOL1", 4) || !strncmp(label, "HDR1", 4) ||
            !strncmp(label, "EOF1", 4) || !strncmp(label, "EOV1", 4)) {
            size_t end = !strncmp(label, "VOL1", 4) ? 10 : 21;
            while (end > 4 && label[end - 1] == ' ')
                --end;
            label[end] = 0;
            fprintf(out, "%5zu  %s%s\n", i + 1, label, is_ebcdic ? " (EBCDIC)" : "");
        }
    }
}
