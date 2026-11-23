/* ASCII glyphs of the DAF-2020 cell, six dots per row, leftmost at bit 5. */
#ifndef DAF_FONT5X7_H
#define DAF_FONT5X7_H

#include <stdint.h>

enum { DAF_FONT_ROWS = 9 };

extern const uint8_t daf_font5x7[95][DAF_FONT_ROWS];

#endif /* DAF_FONT5X7_H */
