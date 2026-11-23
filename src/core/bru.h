#ifndef RECIF_BRU_H
#define RECIF_BRU_H

#include "tape.h"

bool bru_header_shape(const uint8_t *block);
bool bru_file_header(const uint8_t *block);
bool bru_data_layout(const uint8_t *record, unsigned blocks);
bool bru_catalog(const Tape *tape, const char *directory, FILE *listing, char *err, size_t errlen);
bool bru_restore(const Tape *tape, const char *image, uint32_t blocks, FILE *report, char *err,
                 size_t errlen);

#endif
