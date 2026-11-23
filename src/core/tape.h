#ifndef RECIF_TAPE_H
#define RECIF_TAPE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

typedef enum { TAPE_DATA, TAPE_MARK, TAPE_BAD, TAPE_GAP, TAPE_END } TapeKind;
typedef enum { TAPE_SIMH, TAPE_TDUP, TAPE_BRU_FLAT } TapeFormat;

typedef struct {
    size_t offset;
    uint32_t length;
    TapeKind kind;
} TapeRecord;

typedef struct {
    uint8_t *bytes;
    size_t size;
    TapeRecord *records;
    size_t count, capacity, position, unparsed;
    TapeFormat format;
    bool prefix_only;
} Tape;

bool tape_open(Tape *tape, const char *image, const char *lengths, char *err, size_t errlen);
void tape_close(Tape *tape);
const TapeRecord *tape_step(Tape *tape, bool reverse);
bool tape_export(const Tape *tape, FILE *out);
void tape_report(const Tape *tape, FILE *out);
const char *tape_format_name(const Tape *tape);
uint8_t *read_image(const char *path, size_t *size, char *err, size_t errlen);

#endif
