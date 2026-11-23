#ifndef RECIF_DAF2020_H
#define RECIF_DAF2020_H

/* FEPER DAF-2020 terminal as described by its 1987 technical manual:
 * Tektronix 4010 personality, special commands and VT100/VT52 mode,
 * keyboard DIP switches, VT SET-UP A/B and XON/XOFF flow control. */

#include "rs232.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    DAF_WIDTH = 512, DAF_HEIGHT = 288, DAF_WORK_HEIGHT = 390,
    DAF_FIFO_SIZE = 512, DAF_COLS = 80, DAF_ROWS = 24, DAF_ANSWERBACK = 20
};

/* Keyboard switch Sn in the ON (up) position is bit n-1. */
#define DAF_SWITCH(n) (1u << ((n) - 1))
/* 9600 baud, no parity, one stop bit, 8 bits, on line. */
#define DAF_DEFAULT_SWITCHES (DAF_SWITCH(1) | DAF_SWITCH(2) | DAF_SWITCH(7) | DAF_SWITCH(8))

typedef enum {
    DAF_LEFT, DAF_RIGHT, DAF_UP, DAF_DOWN, DAF_PF1, DAF_PF2, DAF_PF3, DAF_PF4,
    DAF_PAGE, DAF_RESET, DAF_SCRL, DAF_BREAK, DAF_LINEFEED, DAF_ENTER
} DafKey;
enum { DAF_SHIFT = 1, DAF_CTRL = 2 };

/* The communication parameters in force: from the switches at power-on,
 * PAGE and RESET, changed by VT SET-UP B. */
typedef struct {
    unsigned tx_baud, rx_baud, data_bits, stop_bits;
    Rs232Parity parity;
    bool even, online, newline, keyclick;
} DafLine;

typedef struct {
    int row, col, saved_row, saved_col, top, bottom;
    unsigned attrs, saved_attrs;
    uint8_t g[2], saved_g[2], bank, saved_bank;
    uint8_t line_mode[DAF_ROWS];
    bool tabs[DAF_COLS];
    bool entered, ansi, wrap, wrapped, origin, cursor, app_cursor, app_keypad, reverse;
    bool smooth, autorepeat, block_cursor, auto_xon, margin_bell, uk;
} DafVT;

typedef struct {
    uint8_t bitmap[DAF_WORK_HEIGHT][DAF_WIDTH];
    uint8_t glyphs[95][12];
    uint8_t rx[DAF_FIFO_SIZE], tx[DAF_FIFO_SIZE];
    unsigned rx_head, rx_count, tx_head, tx_count;
    void (*send)(void *, uint8_t);
    void *send_context;
    DafVT vt;
    DafLine line;
    uint16_t switches;
    unsigned line_changes;
    char answerback[DAF_ANSWERBACK + 1];
    int x, y, gx, gy, cross_x, cross_y, viewport, margin;
    unsigned parser, tek_mode, coord_hi_x, coord_lo_x, coord_hi_y, coord_lo_y;
    unsigned glyph, glyph_row, pattern, phase, params[8], param_count;
    unsigned setup, setup_col, answer_state, answer_length;
    char answer_delimiter;
    int coord_hi, coord_lo;
    /* Requests for the host side: tone and click time, break length and
     * processing time, all consumed by the serial and sound layers. */
    unsigned bell_ms, clicks, break_ms;
    uint32_t busy_us;
    bool vt_mode, print_alpha, first_point, gin, track, overprint, programmable;
    bool flow_stopped, peer_stopped, csi_private, lower_case, keyboard_special, hold;
} Daf;

void daf_line_from_switches(DafLine *line, uint16_t switches);
void daf_reset(Daf *d, uint16_t switches, void (*send)(void *, uint8_t), void *context);
/* The terminal's own power-on reset (ESC c): clears the screen, keeps the
 * line and the switches. */
void daf_restart(Daf *d);
void daf_set_switches(Daf *d, uint16_t switches);
/* The personality, as ESC X 1 and ESC 1 select it. */
void daf_set_mode(Daf *d, bool vt);
bool daf_receive(Daf *d, uint8_t byte);
void daf_receive_error(Daf *d);
void daf_drain(Daf *d, unsigned budget);
bool daf_processing(const Daf *d);
void daf_key_char(Daf *d, uint8_t ch);
void daf_key(Daf *d, DafKey key, unsigned modifiers);
bool daf_keypad(Daf *d, uint8_t key);
void daf_render(const Daf *d, uint32_t *frame, uint32_t off, uint32_t on, uint32_t cursor, bool blink);
void daf_demo(Daf *d);
void daf_puts(Daf *d, const char *s);

#endif
