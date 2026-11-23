#include "daf2020.h"
#include "font5x7.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Section references are to the FEPER DAF 2020 technical manual (1987).
 * The manual contradicts itself on SI/SO (2.6.1 against 2.7.7-8); this
 * model follows 2.6.1, where SI is the initial, upper-case state. */

enum {
    IDLE, ESC, SPECIAL, PATTERN_HI, PATTERN_LO, GLYPH_CODE, GLYPH_ROWS,
    CSI, VT_G0, VT_G1, VT_LINE, VT52_ROW, VT52_COL
};
enum { ALPHA, GRAPHICS };
enum { UNDERLINE = 2, INVERSE = 4, BLINK = 8 };
enum { VT_LEFT = 16, VT_W = 6, VT_H = 12, TEK_W = 7, TEK_H = 11, TEK_HOME = 380, TEK_VIEW = 102 };
enum { SETUP_NONE, SETUP_A, SETUP_B };
enum { BELL_MS = 200, BREAK_MS = 230, LONG_BREAK_MS = 3500, SMOOTH_SCROLL_US = 166667 };
enum { XON = 17, XOFF = 19 };

static const unsigned rates[] = {150, 300, 600, 1200, 2400, 4800, 9600, 19200};

static int bound(int x, int low, int high)
{
    return x < low ? low : x > high ? high : x;
}

void daf_line_from_switches(DafLine *line, uint16_t s)
{
    unsigned rate = (s & DAF_SWITCH(1) ? 4u : 0) | (s & DAF_SWITCH(2) ? 2u : 0) | (s & DAF_SWITCH(3) ? 1u : 0);
    line->tx_baud = line->rx_baud = 150u << rate;
    /* S5 enables parity and S4 selects even, as on the 8251 USART. */
    line->even = (s & DAF_SWITCH(4)) != 0;
    line->parity = !(s & DAF_SWITCH(5)) ? RS232_NO_PARITY : line->even ? RS232_EVEN : RS232_ODD;
    line->stop_bits = s & DAF_SWITCH(6) ? 2 : 1;
    line->data_bits = s & DAF_SWITCH(7) ? 8 : 7;
    line->online = (s & DAF_SWITCH(8)) != 0;
    line->newline = (s & DAF_SWITCH(12)) != 0;
    line->keyclick = (s & DAF_SWITCH(14)) != 0;
}

static void transmit(Daf *d, uint8_t ch)
{
    if (!d->send || !d->line.online)
        return;
    if (!d->peer_stopped || ch == XON || ch == XOFF) {
        d->send(d->send_context, ch);
    } else if (d->tx_count < DAF_FIFO_SIZE) {
        d->tx[(d->tx_head + d->tx_count++) % DAF_FIFO_SIZE] = ch;
    }
}

static void release_output(Daf *d)
{
    d->peer_stopped = false;
    while (d->tx_count) {
        uint8_t ch = d->tx[d->tx_head];
        d->tx_head = (d->tx_head + 1) % DAF_FIFO_SIZE;
        --d->tx_count;
        transmit(d, ch);
    }
}

/* XON/XOFF: the FIFO watermarks (2.5), keyboard special commands, SET-UP
 * and the SCRL hold all stop the host; it resumes once none applies. */
static bool flow_enabled(const Daf *d)
{
    return !d->vt_mode || d->vt.auto_xon;
}

static void flow(Daf *d)
{
    bool stop = d->rx_count > DAF_FIFO_SIZE - 128;
    bool go = d->rx_count < DAF_FIFO_SIZE - 256;
    bool held = d->keyboard_special || d->setup || d->hold;
    if (!flow_enabled(d) && !d->keyboard_special) {
        if (d->flow_stopped) {
            transmit(d, XON);
            d->flow_stopped = false;
        }
        return;
    }
    if (!d->flow_stopped && (stop || held)) {
        transmit(d, XOFF);
        d->flow_stopped = true;
    } else if (d->flow_stopped && go && !held) {
        transmit(d, XON);
        d->flow_stopped = false;
    }
}

static void bell(Daf *d)
{
    d->bell_ms = BELL_MS;
}

/* GIN and ENQ replies: four 0x20-0x3F bytes, then the S9/S10 terminator. */
static void report(Daf *d, int x, int y, int prefix)
{
    unsigned hx = (unsigned)bound(x, 0, 511) * 2;
    unsigned hy = (unsigned)bound(y, 0, 511) * 2;
    if (prefix >= 0)
        transmit(d, (uint8_t)prefix);
    transmit(d, (uint8_t)(0x20 | (hx >> 5)));
    transmit(d, (uint8_t)(0x20 | (hx & 31)));
    transmit(d, (uint8_t)(0x20 | (hy >> 5)));
    transmit(d, (uint8_t)(0x20 | (hy & 31)));
    if (d->switches & DAF_SWITCH(10)) {
        transmit(d, 13);
        if (d->switches & DAF_SWITCH(9))
            transmit(d, 4);
    }
}

/* Keeps rows low..high of the work space inside the 288-line window. */
static void show(Daf *d, int low, int high)
{
    if (low < d->viewport)
        d->viewport = bound(low, 0, DAF_WORK_HEIGHT - DAF_HEIGHT);
    else if (high >= d->viewport + DAF_HEIGHT)
        d->viewport = bound(high - DAF_HEIGHT + 1, 0, DAF_WORK_HEIGHT - DAF_HEIGHT);
}

static void track_alpha(Daf *d)
{
    if (d->track && d->tek_mode == ALPHA && !d->gin)
        show(d, d->y, d->y + TEK_H - 1);
}

static void plot(Daf *d, int x, int y, uint8_t value)
{
    if ((unsigned)x < DAF_WIDTH && (unsigned)y < DAF_WORK_HEIGHT)
        d->bitmap[y][x] = value;
}

static void vector(Daf *d, int x, int y)
{
    int px = d->gx, py = d->gy, dx = abs(x - px), dy = -abs(y - py);
    int sx = px < x ? 1 : -1, sy = py < y ? 1 : -1, error = dx + dy;
    for (;;) {
        plot(d, px, py, (uint8_t)((d->pattern >> (7 - d->phase)) & 1));
        d->phase = (d->phase + 1) & 7;
        if (px == x && py == y)
            break;
        int e = error * 2;
        if (e >= dy) { error += dy; px += sx; }
        if (e <= dx) { error += dx; py += sy; }
    }
}

/* 2.6.2: HiY and HiX share a byte class; a high byte after LoY is HiX.
 * LoX completes the point. */
static void coordinate(Daf *d, uint8_t ch)
{
    int n = ch & 31;
    switch (ch & 0x60) {
    case 0x20:
        if (d->coord_lo >= 0)
            d->coord_hi_x = (unsigned)n;
        else
            d->coord_hi = n;
        break;
    case 0x60:
        d->coord_lo = n;
        break;
    case 0x40: {
        if (d->coord_hi >= 0)
            d->coord_hi_y = (unsigned)d->coord_hi;
        if (d->coord_lo >= 0)
            d->coord_lo_y = (unsigned)d->coord_lo;
        d->coord_lo_x = (unsigned)n;
        int x = (int)((d->coord_hi_x * 32 + d->coord_lo_x) / 2);
        int y = (int)((d->coord_hi_y * 32 + d->coord_lo_y) / 2);
        if (!d->first_point)
            vector(d, x, y);
        d->first_point = false;
        d->gx = x;
        d->gy = y;
        d->coord_hi = d->coord_lo = -1;
        break;
    }
    default: break;
    }
}

/* Two small letters on a diagonal, as the DEC special graphics shows the
 * control-character pictures. */
static unsigned pair_glyph(char top, char bottom, int row)
{
    static const char letters[] = "CFHLNRTV";
    static const uint8_t shapes[][4] = {
        {3, 4, 4, 3}, {7, 4, 6, 4}, {5, 7, 5, 5}, {4, 4, 4, 7},
        {5, 7, 7, 5}, {6, 5, 6, 5}, {7, 2, 2, 2}, {5, 5, 5, 2}
    };
    const char *a = strchr(letters, top), *b = strchr(letters, bottom);
    if (row >= 1 && row <= 4)
        return (unsigned)shapes[a - letters][row - 1] << 3;
    if (row >= 6 && row <= 9)
        return shapes[b - letters][row - 6];
    return 0;
}

/* DEC special graphics, codes 137-176 octal (table 4.11), six dots wide
 * with bit 5 on the left; the middle row is 5. */
static bool special_glyph(uint8_t ch, int row, unsigned *bits)
{
    enum { LEFT = 1, RIGHT = 2, UP = 4, DOWN = 8 };
    static const uint8_t rows[][12] = {
        ['`' - '`'] = {0, 0, 0, 0x08, 0x1c, 0x3e, 0x1c, 0x08, 0, 0, 0, 0},
        ['f' - '`'] = {0, 0x18, 0x24, 0x18, 0, 0, 0, 0, 0, 0, 0, 0},
        ['g' - '`'] = {0, 0, 0x08, 0x08, 0x3e, 0x08, 0x08, 0, 0x3e, 0, 0, 0},
        ['y' - '`'] = {0, 0, 0x02, 0x0c, 0x30, 0x0c, 0x02, 0, 0x3e, 0, 0, 0},
        ['z' - '`'] = {0, 0, 0x20, 0x18, 0x06, 0x18, 0x20, 0, 0x3e, 0, 0, 0},
        ['{' - '`'] = {0, 0, 0, 0x3e, 0x14, 0x14, 0x14, 0x14, 0, 0, 0, 0},
        ['|' - '`'] = {0, 0, 0x02, 0x3e, 0x08, 0x3e, 0x20, 0, 0, 0, 0, 0},
        ['}' - '`'] = {0, 0x0c, 0x12, 0x10, 0x3c, 0x10, 0x10, 0x3e, 0, 0, 0, 0},
        ['~' - '`'] = {0, 0, 0, 0, 0, 0x08, 0, 0, 0, 0, 0, 0},
    };
    unsigned arms = 0;
    switch (ch) {
    case '_': *bits = 0; return true;
    case '`': case 'f': case 'g': case 'y': case 'z': case '{': case '|': case '}': case '~':
        *bits = rows[ch - '`'][row];
        return true;
    case 'a': *bits = row % 2 ? 0x15u : 0x2au; return true;
    case 'b': *bits = pair_glyph('H', 'T', row); return true;
    case 'c': *bits = pair_glyph('F', 'F', row); return true;
    case 'd': *bits = pair_glyph('C', 'R', row); return true;
    case 'e': *bits = pair_glyph('L', 'F', row); return true;
    case 'h': *bits = pair_glyph('N', 'L', row); return true;
    case 'i': *bits = pair_glyph('V', 'T', row); return true;
    case 'j': arms = LEFT | UP; break;
    case 'k': arms = LEFT | DOWN; break;
    case 'l': arms = RIGHT | DOWN; break;
    case 'm': arms = RIGHT | UP; break;
    case 'n': arms = LEFT | RIGHT | UP | DOWN; break;
    case 'o': case 'p': case 'q': case 'r': case 's':
        *bits = row == 1 + (ch - 'o') * 2 ? 0x3fu : 0;
        return true;
    case 't': arms = RIGHT | UP | DOWN; break;
    case 'u': arms = LEFT | UP | DOWN; break;
    case 'v': arms = LEFT | RIGHT | UP; break;
    case 'w': arms = LEFT | RIGHT | DOWN; break;
    case 'x': arms = UP | DOWN; break;
    default: return false;
    }
    unsigned b = 0;
    if (row == 5) {
        if (arms & LEFT) b |= 0x38;
        if (arms & RIGHT) b |= 0x0f;
    }
    if ((row <= 5 && (arms & UP)) || (row >= 5 && (arms & DOWN)))
        b |= 0x08;
    *bits = b;
    return true;
}

static unsigned ascii_row(uint8_t ch, int row)
{
    if (row < 1 || row > DAF_FONT_ROWS)
        return 0;
    return daf_font5x7[ch - 32][row - 1];
}

/* Fonts: 'B' ASCII, 'A' UK, '0' special graphics, '1' programmable,
 * '2' programmable through 137 octal with special graphics above. */
static unsigned font_row(const Daf *d, uint8_t ch, int row, uint8_t font)
{
    if (ch < 32 || ch > 126)
        ch = '?';
    if (font == '1' || (font == '2' && ch <= '_'))
        return d->glyphs[ch - 32][row];
    unsigned bits;
    if ((font == '0' || font == '2') && ch >= '_' && special_glyph(ch, row, &bits))
        return bits;
    if (font == 'A' && ch == '#' && special_glyph('}', row, &bits))
        return bits;
    return ascii_row(ch, row);
}

/* Draws a cell whose top row is `top`; the cell is `height` rows tall. */
static void glyph(Daf *d, int x, int top, uint8_t ch, uint8_t font, int width, int height,
                  unsigned attrs, bool overprint, unsigned line_mode)
{
    for (int row = 0; row < height; ++row) {
        int font_y = line_mode == '3' ? row / 2 : line_mode == '4' ? (row + 12) / 2 : row;
        unsigned dots = font_row(d, ch, font_y, font);
        if ((attrs & UNDERLINE) && row == 10)
            dots = 63;
        int stretch = width == 12 ? 2 : 1;
        for (int i = 0; i < width; ++i) {
            unsigned col = (unsigned)(i / stretch);
            bool ink = col < 6 && (dots & (32u >> col));
            if (attrs & INVERSE)
                ink = !ink;
            if (ink || !overprint)
                plot(d, x + i, top - row, ink ? (uint8_t)(attrs & BLINK ? 2 : 1) : 0);
        }
    }
}

/* ---- Tektronix 4010 personality ---------------------------------------- */

static void tek_alpha_home(Daf *d)
{
    d->tek_mode = ALPHA;
    d->print_alpha = true;
    d->gin = false;
    d->margin = d->x = 0;
    d->y = TEK_HOME;
    d->coord_hi = d->coord_lo = -1;
}

/* PAGE and RESET (2.2.2): alpha at line 380, automatic tracking, solid
 * vectors, standard overprinted font, parameters from the switches. */
static void tek_reset(Daf *d, bool clear)
{
    if (clear)
        memset(d->bitmap, 0, sizeof(d->bitmap));
    tek_alpha_home(d);
    d->viewport = TEK_VIEW;
    d->track = true;
    d->pattern = 255;
    d->phase = 0;
    d->first_point = true;
    d->overprint = true;
    d->programmable = false;
    d->lower_case = false;
    d->parser = IDLE;
    d->rx_count = d->rx_head = 0;
    d->tx_count = d->tx_head = 0;
    d->hold = false;
    daf_line_from_switches(&d->line, d->switches);
    ++d->line_changes;
}

static int tek_last_column(const Daf *d)
{
    return d->margin + ((DAF_WIDTH - d->margin) / TEK_W - 1) * TEK_W;
}

static void tek_linefeed(Daf *d)
{
    d->y -= TEK_H;
    if (d->y < 0) {
        /* Off the bottom: break if S13 is on, other margin, line 380. */
        if (d->switches & DAF_SWITCH(13))
            d->break_ms = BREAK_MS;
        int column = d->x - d->margin;
        d->margin ^= 256;
        d->x = bound(d->margin + column, d->margin, tek_last_column(d));
        d->y = TEK_HOME;
        if (d->track)
            d->viewport = TEK_VIEW;
    }
    track_alpha(d);
}

static void tek_enter_alpha(Daf *d, bool carriage_return)
{
    bool point = d->tek_mode == GRAPHICS && !d->first_point;
    if (point) {
        d->x = bound(d->gx, 0, DAF_WIDTH - 1);
        d->y = bound(d->gy, 0, DAF_WORK_HEIGHT - 1);
    }
    if (carriage_return) {
        /* CR from graphics puts the cursor at the screen's left edge. */
        if (d->tek_mode == GRAPHICS)
            d->margin = 0;
        d->x = d->margin;
    }
    d->gin = false;
    d->tek_mode = ALPHA;
    d->print_alpha = true;
    track_alpha(d);
}

static void tek_print(Daf *d, uint8_t ch)
{
    if (!d->programmable && ch >= 'a' && ch <= 'z' && !d->lower_case)
        ch = (uint8_t)(ch - 32);
    else if (!d->programmable && ch >= 'A' && ch <= 'Z' && d->lower_case)
        ch = (uint8_t)(ch + 32);
    glyph(d, d->x, d->y + TEK_H - 1, ch, d->programmable ? '1' : 'B', TEK_W, TEK_H, 0, d->overprint, 0);
    d->x += TEK_W;
    if (DAF_WIDTH - d->x < TEK_W) {
        d->x = d->margin;
        tek_linefeed(d);
    }
}

static void enter_vt(Daf *d);

static void special_command(Daf *d, uint8_t ch)
{
    switch (ch) {
    case '1': enter_vt(d); break;
    case '2': d->overprint = true; break;
    case '3': d->overprint = false; break;
    case 'A': d->programmable = false; break;
    case 'P': d->programmable = true; break;
    case 'G': d->parser = GLYPH_CODE; return;
    case 1: d->parser = PATTERN_HI; return;
    /* T, N, S and D drive the printer port, which has no printer here. */
    default: break;
    }
    if (d->keyboard_special) {
        d->keyboard_special = false;
        flow(d);
    }
}

static void tek_escape(Daf *d, uint8_t ch)
{
    switch (ch) {
    case 'X': d->parser = SPECIAL; break;
    case 12:
        memset(d->bitmap, 0, sizeof(d->bitmap));
        tek_alpha_home(d);
        track_alpha(d);
        break;
    case 23:
        /* Hard copy needs an operational printer; it reactivates alpha. */
        d->gin = false;
        d->print_alpha = true;
        break;
    case 26:
        if (!d->gin) {
            d->cross_x = d->gx;
            d->cross_y = bound(d->gy, 0, DAF_WORK_HEIGHT - 1);
        }
        d->gin = true;
        show(d, d->cross_y, d->cross_y);
        break;
    case 5:
        if (d->gin) {
            report(d, d->cross_x, d->cross_y, -1);
            d->gin = false;
        } else {
            int status = 0x31 | (d->margin ? 2 : 0) | (d->tek_mode == ALPHA ? 4 : 0) |
                (!d->first_point ? 8 : 0);
            report(d, d->tek_mode == ALPHA ? d->x : d->gx,
                   d->tek_mode == ALPHA ? d->y : d->gy, status);
        }
        d->print_alpha = false;
        break;
    default: break;
    }
}

static void tek(Daf *d, uint8_t ch)
{
    unsigned p = d->parser;
    d->parser = IDLE;
    if (p == ESC) {
        tek_escape(d, ch);
        return;
    }
    if (p == SPECIAL) {
        special_command(d, ch);
        return;
    }
    if (p == GLYPH_CODE) {
        if (ch >= 32 && ch <= 126) {
            d->glyph = ch - 32u;
            d->glyph_row = 0;
            d->parser = GLYPH_ROWS;
        } else {
            special_command(d, 0);
        }
        return;
    }
    if (p == GLYPH_ROWS) {
        if (ch & 64) {
            d->glyphs[d->glyph][d->glyph_row++] = ch & 63;
            if (d->glyph_row == 12) {
                ++d->glyph;
                d->glyph_row = 0;
            }
            if (d->glyph < 95) {
                d->parser = GLYPH_ROWS;
                return;
            }
        }
        special_command(d, 0);
        return;
    }
    if (p == PATTERN_HI) {
        d->pattern = (ch & 15u) << 4;
        d->parser = PATTERN_LO;
        return;
    }
    if (p == PATTERN_LO) {
        d->pattern |= ch & 15;
        d->phase = 0;
        special_command(d, 0);
        return;
    }

    /* A command from the line ends graphic input (2.6.3). */
    if (d->gin && (ch == 7 || (ch >= 8 && ch <= 11) || ch == 13 || ch == 31))
        d->gin = false;
    bool graphics = d->tek_mode == GRAPHICS && !d->gin;
    switch (ch) {
    case 27: d->parser = ESC; return;
    case 29:
        d->gin = false;
        d->tek_mode = GRAPHICS;
        d->first_point = true;
        d->coord_hi = d->coord_lo = -1;
        return;
    case 31: tek_enter_alpha(d, false); return;
    case 13: tek_enter_alpha(d, true); return;
    case 7: bell(d); d->print_alpha = true; return;
    case 8:
        if (graphics) {
            d->gx = bound(d->gx - TEK_W, 0, DAF_WIDTH - 1);
        } else {
            d->print_alpha = true;
            d->x = d->x - TEK_W < d->margin ? tek_last_column(d) : d->x - TEK_W;
        }
        return;
    case 9:
        if (graphics) {
            d->gx = bound(d->gx + TEK_W, 0, DAF_WIDTH - 1);
        } else {
            d->print_alpha = true;
            d->x += TEK_W;
            if (DAF_WIDTH - d->x < TEK_W) {
                d->x = d->margin;
                tek_linefeed(d);
            }
        }
        return;
    case 10:
        if (graphics) {
            d->gy = bound(d->gy - TEK_H, 0, DAF_WORK_HEIGHT - 1);
        } else {
            d->print_alpha = true;
            if (d->line.newline)
                d->x = d->margin;
            tek_linefeed(d);
        }
        return;
    case 11:
        if (graphics) {
            d->gy = bound(d->gy + TEK_H, 0, DAF_WORK_HEIGHT - 1);
        } else {
            d->print_alpha = true;
            if (d->y + TEK_H <= TEK_HOME)
                d->y += TEK_H;
            track_alpha(d);
        }
        return;
    case 14: d->lower_case = true; return;
    case 15: d->lower_case = false; return;
    case 127: return;
    default: break;
    }
    if (ch < 32 || d->gin)
        return;
    if (graphics)
        coordinate(d, ch);
    else if (d->tek_mode == ALPHA && d->print_alpha)
        tek_print(d, ch);
}

/* ---- VT100 / VT52 personality ------------------------------------------- */

static void vt_defaults(DafVT *v)
{
    memset(v, 0, sizeof(*v));
    v->bottom = DAF_ROWS - 1;
    v->ansi = v->cursor = true;
    v->g[0] = v->saved_g[0] = 'B';
    v->g[1] = v->saved_g[1] = '0';
    for (int i = 0; i < DAF_COLS; i += 8)
        v->tabs[i] = true;
    /* First entry (3.1): ANSI, smooth scroll, repeat, block cursor,
     * no wraparound, no margin bell, XON/XOFF, dark screen. */
    v->smooth = v->autorepeat = v->block_cursor = v->auto_xon = true;
    v->entered = true;
}

static void enter_vt(Daf *d)
{
    if (!d->vt.entered)
        vt_defaults(&d->vt);
    d->vt_mode = true;
    d->parser = IDLE;
}

static void leave_vt(Daf *d)
{
    d->vt_mode = false;
    d->setup = SETUP_NONE;
    tek_alpha_home(d);
    d->viewport = TEK_VIEW;
    d->track = true;
    d->first_point = true;
    flow(d);
}

static void power_on(Daf *d)
{
    void (*send)(void *, uint8_t) = d->send;
    void *context = d->send_context;
    uint16_t switches = d->switches;
    unsigned changes = d->line_changes;
    bool stopped = d->flow_stopped;
    memset(d, 0, sizeof(*d));
    d->send = send;
    d->send_context = context;
    d->switches = switches;
    d->line_changes = changes;
    d->flow_stopped = stopped;
    tek_reset(d, true);
    flow(d);
}

static void vt_reply(Daf *d, const char *bytes)
{
    for (; *bytes; ++bytes)
        transmit(d, (uint8_t)*bytes);
}

static int vt_width(const DafVT *v, int row)
{
    return v->line_mode[row] && v->line_mode[row] != '5' ? 12 : VT_W;
}

static int vt_columns(const DafVT *v, int row)
{
    return DAF_COLS * VT_W / vt_width(v, row);
}

static int vt_top_pixel(int row)
{
    return DAF_HEIGHT - 1 - row * VT_H;
}

static void vt_erase(Daf *d, int row, int start, int end)
{
    int top = vt_top_pixel(row);
    int width = vt_width(&d->vt, row);
    start = bound(start * width, 0, DAF_COLS * VT_W);
    end = bound(end * width, start, DAF_COLS * VT_W);
    for (int i = 0; i < VT_H; ++i)
        memset(&d->bitmap[top - i][VT_LEFT + start], 0, (size_t)(end - start));
}

static void vt_scroll(Daf *d, int top, int bottom, int count)
{
    DafVT *v = &d->vt;
    int height = bottom - top + 1;
    int n = bound(abs(count), 0, height);
    if (!n)
        return;
    int lo = vt_top_pixel(bottom) - (VT_H - 1);
    int shift = n * VT_H;
    size_t rows = (size_t)(height - n);
    if (count > 0) {
        memmove(d->bitmap[lo + shift], d->bitmap[lo], rows * VT_H * DAF_WIDTH);
        memset(d->bitmap[lo], 0, (size_t)shift * DAF_WIDTH);
        memmove(v->line_mode + top, v->line_mode + top + n, rows);
        memset(v->line_mode + bottom - n + 1, 0, (size_t)n);
    } else {
        memmove(d->bitmap[lo], d->bitmap[lo + shift], rows * VT_H * DAF_WIDTH);
        memset(d->bitmap[lo + (height - n) * VT_H], 0, (size_t)shift * DAF_WIDTH);
        memmove(v->line_mode + top + n, v->line_mode + top, rows);
        memset(v->line_mode + top, 0, (size_t)n);
    }
}

static void vt_linefeed(Daf *d, bool reverse)
{
    DafVT *v = &d->vt;
    bool scroll = reverse ? v->row == v->top : v->row == v->bottom;
    if (scroll) {
        vt_scroll(d, v->top, v->bottom, reverse ? -1 : 1);
        /* Smooth scroll moves at most six lines a second. */
        if (v->smooth)
            d->busy_us += SMOOTH_SCROLL_US;
    } else {
        v->row = bound(v->row + (reverse ? -1 : 1), 0, DAF_ROWS - 1);
    }
    v->col = bound(v->col, 0, vt_columns(v, v->row) - 1);
    v->wrapped = false;
}

static uint8_t vt_font(const DafVT *v)
{
    uint8_t font = v->g[v->bank];
    return font == 'B' && v->uk ? 'A' : font;
}

static void vt_print(Daf *d, uint8_t ch)
{
    DafVT *v = &d->vt;
    if (v->wrapped && v->wrap) {
        v->col = 0;
        vt_linefeed(d, false);
    }
    int width = vt_width(v, v->row), last = vt_columns(v, v->row) - 1;
    int x = VT_LEFT + bound(v->col, 0, last) * width;
    glyph(d, x, vt_top_pixel(v->row), ch, vt_font(v), width, VT_H,
          v->attrs, false, v->line_mode[v->row]);
    if (v->col < last) {
        ++v->col;
    } else {
        /* Without wraparound new characters overwrite the last column. */
        v->col = last;
        v->wrapped = v->wrap;
    }
}

static void vt_checkerboard(Daf *d)
{
    DafVT *v = &d->vt;
    uint8_t old = v->g[v->bank];
    v->g[v->bank] = '0';
    vt_print(d, 'a');
    v->g[v->bank] = old;
}

static void vt_position(DafVT *v, int row, int col)
{
    v->row = v->origin ? bound(row, v->top, v->bottom) : bound(row, 0, DAF_ROWS - 1);
    v->col = bound(col, 0, vt_columns(v, v->row) - 1);
    v->wrapped = false;
}

static void vt_save(DafVT *v)
{
    v->saved_row = v->row;
    v->saved_col = v->col;
    v->saved_attrs = v->attrs;
    v->saved_bank = v->bank;
    memcpy(v->saved_g, v->g, sizeof(v->g));
}

static void vt_restore(DafVT *v)
{
    vt_position(v, v->saved_row, v->saved_col);
    v->attrs = v->saved_attrs;
    v->bank = v->saved_bank;
    memcpy(v->g, v->saved_g, sizeof(v->g));
}

static void vt_edit_chars(Daf *d, int count, bool insert)
{
    DafVT *v = &d->vt;
    int width = vt_width(v, v->row);
    int column = bound(v->col * width, 0, DAF_COLS * VT_W);
    int remain = DAF_COLS * VT_W - column;
    int n = bound(count * width, 0, remain);
    int y = vt_top_pixel(v->row);
    for (int i = 0; i < VT_H; ++i) {
        uint8_t *b = &d->bitmap[y - i][VT_LEFT + column];
        memmove(b + (insert ? n : 0), b + (insert ? 0 : n), (size_t)(remain - n));
        memset(b + (insert ? 0 : remain - n), 0, (size_t)n);
    }
}

static void vt_alignment(Daf *d)
{
    DafVT *v = &d->vt;
    for (int row = 0; row < DAF_ROWS; ++row) {
        v->line_mode[row] = 0;
        for (int col = 0; col < DAF_COLS; ++col)
            glyph(d, VT_LEFT + col * VT_W, vt_top_pixel(row), 'E', 'B', VT_W, VT_H, 0, false, 0);
    }
    vt_position(v, 0, 0);
}

static void vt_modes(Daf *d, bool on)
{
    DafVT *v = &d->vt;
    for (unsigned i = 0; i < d->param_count; ++i) {
        unsigned p = d->params[i];
        if (!d->csi_private) {
            if (p == 20) {
                d->line.newline = on;
                ++d->line_changes;
            }
            continue;
        }
        switch (p) {
        case 1: v->app_cursor = on; break;
        case 2: v->ansi = on; break;
        case 4: v->smooth = on; break;
        case 5: v->reverse = on; break;
        case 6: v->origin = on; vt_position(v, on ? v->top : 0, 0); break;
        case 7: v->wrap = on; v->wrapped = false; break;
        case 8: v->autorepeat = on; break;
        case 25: v->cursor = on; break;
        default: break;
        }
    }
}

static void vt_csi(Daf *d, uint8_t ch)
{
    DafVT *v = &d->vt;
    int arg = (int)d->params[0], second = (int)d->params[1];
    int n = arg ? arg : 1;
    if (d->csi_private && ch != 'h' && ch != 'l')
        return;
    switch (ch) {
    case 'A': vt_position(v, v->row - n, v->col); break;
    case 'B': vt_position(v, v->row + n, v->col); break;
    case 'C': vt_position(v, v->row, v->col + n); break;
    case 'D': vt_position(v, v->row, v->col - n); break;
    case 'H': case 'f': vt_position(v, n - 1 + (v->origin ? v->top : 0), second ? second - 1 : 0); break;
    case 'J':
        if (arg == 2) {
            memset(d->bitmap, 0, DAF_HEIGHT * DAF_WIDTH);
        } else if (arg == 0) {
            vt_erase(d, v->row, v->col, DAF_COLS);
            for (int i = v->row + 1; i < DAF_ROWS; ++i)
                vt_erase(d, i, 0, DAF_COLS);
        } else if (arg == 1) {
            for (int i = 0; i < v->row; ++i)
                vt_erase(d, i, 0, DAF_COLS);
            vt_erase(d, v->row, 0, v->col + 1);
        }
        break;
    case 'K':
        if (arg == 0) vt_erase(d, v->row, v->col, DAF_COLS);
        if (arg == 1) vt_erase(d, v->row, 0, v->col + 1);
        if (arg == 2) vt_erase(d, v->row, 0, DAF_COLS);
        break;
    case 'L': case 'M':
        if (v->row >= v->top && v->row <= v->bottom)
            vt_scroll(d, v->row, v->bottom, ch == 'L' ? -n : n);
        break;
    case 'P': case '@': vt_edit_chars(d, n, ch == '@'); break;
    case 'c': if (!arg) vt_reply(d, "\033[?1;0c"); break;
    case 'g':
        if (arg == 0) v->tabs[v->col] = false;
        if (arg == 3) memset(v->tabs, 0, sizeof(v->tabs));
        break;
    case 'h': case 'l': vt_modes(d, ch == 'h'); break;
    case 'm':
        for (unsigned i = 0; i < d->param_count; ++i) {
            switch (d->params[i]) {
            case 0: v->attrs = 0; break;
            case 4: v->attrs |= UNDERLINE; break;
            case 5: v->attrs |= BLINK; break;
            case 7: v->attrs |= INVERSE; break;
            default: break;
            }
        }
        break;
    case 'n':
        if (arg == 5) vt_reply(d, "\033[0n");
        if (arg == 6) {
            char reply[24];
            snprintf(reply, sizeof(reply), "\033[%d;%dR", v->row + 1 - (v->origin ? v->top : 0), v->col + 1);
            vt_reply(d, reply);
        }
        break;
    case 'r': {
        int end = second ? second : DAF_ROWS;
        if (n < end && end <= DAF_ROWS) {
            v->top = n - 1;
            v->bottom = end - 1;
            vt_position(v, v->origin ? v->top : 0, 0);
        }
        break;
    }
    case 'y':
        /* DECTST: the DAF answers with a reset. */
        if (arg == 2)
            power_on(d);
        break;
    default: break;
    }
}

static void vt_escape(Daf *d, uint8_t ch)
{
    DafVT *v = &d->vt;
    if (ch == '1') {
        leave_vt(d);
        return;
    }
    if (ch == '=' || ch == '>') {
        v->app_keypad = ch == '=';
        return;
    }
    if (ch == 'Z') {
        vt_reply(d, "\033/Z");
        return;
    }
    if (!v->ansi) {
        switch (ch) {
        case 'A': vt_position(v, v->row - 1, v->col); break;
        case 'B': vt_position(v, v->row + 1, v->col); break;
        case 'C': vt_position(v, v->row, v->col + 1); break;
        case 'D': vt_position(v, v->row, v->col - 1); break;
        case 'F': v->g[0] = '0'; break;
        case 'G': v->g[0] = 'B'; break;
        case 'H': vt_position(v, 0, 0); break;
        case 'I': vt_linefeed(d, true); break;
        case 'J': d->params[0] = 0; d->param_count = 1; d->csi_private = false; vt_csi(d, 'J'); break;
        case 'K': d->params[0] = 0; d->param_count = 1; d->csi_private = false; vt_csi(d, 'K'); break;
        case 'Y': d->parser = VT52_ROW; break;
        case '<': v->ansi = true; break;
        default: break;
        }
        return;
    }
    switch (ch) {
    case '[':
        d->parser = CSI;
        d->csi_private = false;
        d->param_count = 1;
        memset(d->params, 0, sizeof(d->params));
        break;
    case '(' : d->parser = VT_G0; break;
    case ')' : d->parser = VT_G1; break;
    case '#': d->parser = VT_LINE; break;
    case '7': vt_save(v); break;
    case '8': vt_restore(v); break;
    case 'D': vt_linefeed(d, false); break;
    case 'E': v->col = 0; vt_linefeed(d, false); break;
    case 'H': v->tabs[v->col] = true; break;
    case 'M': vt_linefeed(d, true); break;
    case 'c': power_on(d); break;
    default: break;
    }
}

static void vt(Daf *d, uint8_t ch)
{
    DafVT *v = &d->vt;
    switch (ch) {
    case 0: case 127: return;
    case 5: vt_reply(d, d->answerback); return;
    case 7: bell(d); return;
    case 8: vt_position(v, v->row, v->col - 1); return;
    case 9: {
        int col = v->col + 1;
        while (col < DAF_COLS && !v->tabs[col])
            ++col;
        vt_position(v, v->row, col);
        return;
    }
    case 10: case 11: case 12:
        if (d->line.newline)
            v->col = 0;
        vt_linefeed(d, false);
        return;
    case 13: v->col = 0; v->wrapped = false; return;
    case 14: v->bank = 1; return;
    case 15: v->bank = 0; return;
    case XON: release_output(d); return;
    case XOFF: d->peer_stopped = true; return;
    case 24: case 26:
        if (d->parser != IDLE)
            vt_checkerboard(d);
        d->parser = IDLE;
        return;
    case 27: d->parser = ESC; return;
    default: break;
    }
    if (ch < 32)
        return;
    unsigned p = d->parser;
    d->parser = IDLE;
    switch (p) {
    case ESC: vt_escape(d, ch); break;
    case CSI:
        if (ch == '?' && d->param_count == 1 && !d->params[0]) {
            d->csi_private = true;
            d->parser = CSI;
        } else if (ch >= '0' && ch <= '9') {
            unsigned *value = d->params + d->param_count - 1;
            *value = *value < 1000 ? *value * 10 + ch - '0' : 9999;
            d->parser = CSI;
        } else if (ch == ';') {
            if (d->param_count < sizeof(d->params) / sizeof(d->params[0]))
                ++d->param_count;
            d->parser = CSI;
        } else if (ch >= 0x40 && ch <= 0x7e) {
            vt_csi(d, ch);
        }
        break;
    case VT_G0: case VT_G1:
        if (strchr("AB012", ch))
            v->g[p == VT_G1] = ch;
        break;
    case VT_LINE:
        if (ch >= '3' && ch <= '6')
            v->line_mode[v->row] = ch;
        else if (ch == '8')
            vt_alignment(d);
        break;
    case VT52_ROW: d->params[0] = ch - 32u; d->parser = VT52_COL; break;
    case VT52_COL: vt_position(v, (int)d->params[0], ch - 32); break;
    default: vt_print(d, ch); break;
    }
}

/* ---- VT SET-UP A and B (4.1.2) ------------------------------------------ */

static bool *setup_bit(Daf *d, unsigned index, bool *scratch)
{
    DafVT *v = &d->vt;
    *scratch = false;
    switch (index) {
    case 0: return &v->smooth;
    case 1: return &v->autorepeat;
    case 2: return &v->reverse;
    case 3: return &v->block_cursor;
    case 4: return &v->margin_bell;
    case 5: return &d->line.keyclick;
    case 6: return &v->ansi;
    case 7: return &v->auto_xon;
    case 8: return &v->uk;
    case 9: return &v->wrap;
    case 10: return &d->line.newline;
    default: return scratch;
    }
}

static bool setup_value(Daf *d, unsigned index)
{
    bool scratch;
    switch (index) {
    case 12: return d->line.even;
    case 13: return d->line.parity != RS232_NO_PARITY;
    case 14: return d->line.data_bits == 8;
    default: return *setup_bit(d, index, &scratch);
    }
}

static unsigned next_rate(unsigned baud)
{
    for (unsigned i = 0; i < 8; ++i)
        if (rates[i] == baud)
            return rates[(i + 1) % 8];
    return rates[6];
}

static void setup_toggle(Daf *d, unsigned index)
{
    bool scratch;
    DafLine *l = &d->line;
    if (index == 12 || index == 13) {
        bool enabled = l->parity != RS232_NO_PARITY;
        if (index == 12)
            l->even = !l->even;
        else
            enabled = !enabled;
        l->parity = !enabled ? RS232_NO_PARITY : l->even ? RS232_EVEN : RS232_ODD;
    } else if (index == 14) {
        l->data_bits = l->data_bits == 8 ? 7 : 8;
    } else {
        bool *bit = setup_bit(d, index, &scratch);
        *bit = !*bit;
    }
    ++d->line_changes;
}

static void setup_exit(Daf *d)
{
    d->setup = SETUP_NONE;
    d->answer_state = 0;
    flow(d);
}

static void setup_key_char(Daf *d, uint8_t ch)
{
    if (d->answer_state == 1) {
        d->answer_delimiter = (char)ch;
        d->answer_length = 0;
        d->answer_state = 2;
        return;
    }
    if (d->answer_state == 2) {
        if ((char)ch == d->answer_delimiter || d->answer_length == DAF_ANSWERBACK) {
            d->answer_state = 0;
        } else {
            d->answerback[d->answer_length++] = (char)ch;
            d->answerback[d->answer_length] = '\0';
        }
        return;
    }
    switch (ch) {
    case '0': power_on(d); return;
    case '4': d->line.online = !d->line.online; ++d->line_changes; return;
    case '5':
        d->setup = d->setup == SETUP_A ? SETUP_B : SETUP_A;
        d->setup_col = 0;
        return;
    case ' ': d->setup_col = (d->setup_col + 1) % (d->setup == SETUP_A ? DAF_COLS : 16); return;
    case 13: d->setup_col = 0; return;
    case 9:
        d->setup_col = d->setup == SETUP_A ? (d->setup_col + 8) / 8 * 8 % DAF_COLS : (d->setup_col + 4) / 4 * 4 % 16;
        return;
    default: break;
    }
    if (d->setup == SETUP_A) {
        if (ch == '2')
            d->vt.tabs[d->setup_col] = !d->vt.tabs[d->setup_col];
        else if (ch == '3')
            memset(d->vt.tabs, 0, sizeof(d->vt.tabs));
        return;
    }
    if (ch == '6' && (d->setup_col < 8 || d->setup_col % 4 != 3))
        setup_toggle(d, d->setup_col);
    else if (ch == '7')
        d->line.tx_baud = next_rate(d->line.tx_baud), ++d->line_changes;
    else if (ch == '8')
        d->line.rx_baud = next_rate(d->line.rx_baud), ++d->line_changes;
    else if (ch == 'A')
        d->answer_state = 1;
}

static void setup_text(uint32_t *frame, int row, int col, const char *text, bool inverse,
                       uint32_t off, uint32_t on)
{
    for (; *text && col < DAF_COLS; ++text, ++col) {
        uint8_t ch = (uint8_t)*text;
        int x0 = VT_LEFT + col * VT_W, y0 = row * VT_H;
        for (int r = 0; r < VT_H; ++r) {
            unsigned dots = ch >= 32 && ch < 127 ? ascii_row(ch, r) : 0;
            for (int i = 0; i < VT_W; ++i) {
                bool ink = (dots & (32u >> i)) != 0;
                frame[(size_t)(y0 + r) * DAF_WIDTH + (size_t)(x0 + i)] = ink != inverse ? on : off;
            }
        }
    }
}

static void render_setup(const Daf *d, uint32_t *frame, uint32_t off, uint32_t on, bool blink)
{
    for (size_t i = 0; i < (size_t)DAF_WIDTH * DAF_HEIGHT; ++i)
        frame[i] = off;
    setup_text(frame, 0, 0, d->setup == SETUP_A ? "SET-UP A" : "SET-UP B", false, off, on);
    setup_text(frame, 1, 0, "TO EXIT PRESS SCRL+CTRL", false, off, on);
    setup_text(frame, 3, 0, d->line.online ? "ON LINE" : "LOCAL", false, off, on);
    if (d->answer_state)
        setup_text(frame, 5, 0, d->answer_state == 1 ? "A= (TYPE DELIMITER)" : "A=", false, off, on);
    if (d->answer_state == 2)
        setup_text(frame, 5, 3, d->answerback, false, off, on);
    if (d->setup == SETUP_A) {
        char tabs[DAF_COLS + 1], ruler[DAF_COLS + 1];
        for (int i = 0; i < DAF_COLS; ++i) {
            tabs[i] = d->vt.tabs[i] ? 'T' : ' ';
            ruler[i] = (char)('0' + (i + 1) % 10);
        }
        tabs[DAF_COLS] = ruler[DAF_COLS] = '\0';
        setup_text(frame, DAF_ROWS - 2, 0, tabs, false, off, on);
        setup_text(frame, DAF_ROWS - 1, 0, ruler, false, off, on);
        if (blink)
            setup_text(frame, DAF_ROWS - 1, (int)d->setup_col, (char[]){ruler[d->setup_col], 0}, true, off, on);
        return;
    }
    char line[DAF_COLS + 1];
    int at = 0;
    for (unsigned g = 0; g < 4; ++g) {
        at += snprintf(line + at, sizeof(line) - (size_t)at, " %u ", g + 1);
        for (unsigned b = 0; b < 4; ++b)
            line[at++] = b == 3 && g >= 2 ? 'X' : setup_value((Daf *)d, g * 4 + b) ? '1' : '0';
        line[at++] = ' ';
    }
    snprintf(line + at, sizeof(line) - (size_t)at, "   T SPEED %-5u  R SPEED %u", d->line.tx_baud, d->line.rx_baud);
    setup_text(frame, DAF_ROWS - 1, 0, line, false, off, on);
    if (blink) {
        int col = 3 + (int)(d->setup_col / 4) * 8 + (int)(d->setup_col % 4);
        setup_text(frame, DAF_ROWS - 1, col, (char[]){line[col], 0}, true, off, on);
    }
}

/* ---- Input and keyboard -------------------------------------------------- */

void daf_reset(Daf *d, uint16_t switches, void (*output)(void *, uint8_t), void *context)
{
    memset(d, 0, sizeof(*d));
    d->send = output;
    d->send_context = context;
    d->switches = switches;
    tek_reset(d, true);
}

void daf_restart(Daf *d)
{
    power_on(d);
}

void daf_set_switches(Daf *d, uint16_t switches)
{
    d->switches = switches;
    daf_line_from_switches(&d->line, switches);
    ++d->line_changes;
}

void daf_set_mode(Daf *d, bool vt)
{
    if (vt && !d->vt_mode)
        enter_vt(d);
    else if (!vt && d->vt_mode)
        leave_vt(d);
}

bool daf_receive(Daf *d, uint8_t byte)
{
    if (!d->line.online)
        return true;
    /* VT mode drops NUL and DEL before the FIFO (4.2.4.1). */
    if (d->vt_mode && ((byte & 127) == 0 || (byte & 127) == 127))
        return true;
    if (d->rx_count == DAF_FIFO_SIZE)
        return false;
    d->rx[(d->rx_head + d->rx_count++) % DAF_FIFO_SIZE] = byte;
    flow(d);
    return true;
}

void daf_receive_error(Daf *d)
{
    /* A character with a parity error shows as the checkerboard. */
    if (d->vt_mode && d->line.online)
        daf_receive(d, 0x1a);
}

bool daf_processing(const Daf *d)
{
    return !d->setup && !d->keyboard_special;
}

void daf_drain(Daf *d, unsigned budget)
{
    while (d->rx_count && budget-- && daf_processing(d) && !d->busy_us) {
        uint8_t ch = d->rx[d->rx_head] & 127;
        d->rx_head = (d->rx_head + 1) % DAF_FIFO_SIZE;
        --d->rx_count;
        if (d->vt_mode) {
            /* SUB from a parity error prints the checkerboard. */
            if (ch == 26 && d->parser == IDLE)
                vt_checkerboard(d);
            else
                vt(d, ch);
        } else {
            tek(d, ch);
        }
    }
    flow(d);
}

void daf_puts(Daf *d, const char *s)
{
    for (; *s; ++s) {
        daf_receive(d, (uint8_t)*s);
        daf_drain(d, 1);
    }
}

static void local(Daf *d, uint8_t ch)
{
    if (d->vt_mode)
        vt(d, ch & 127);
    else
        tek(d, ch & 127);
}

static void click(Daf *d)
{
    if (d->line.keyclick)
        ++d->clicks;
}

static void key_code(Daf *d, uint8_t ch)
{
    if (!d->line.online) {
        local(d, ch);
        return;
    }
    transmit(d, ch);
    if (d->switches & DAF_SWITCH(11))
        local(d, ch);
}

void daf_key_char(Daf *d, uint8_t ch)
{
    if (d->setup) {
        setup_key_char(d, ch);
        return;
    }
    click(d);
    if (d->keyboard_special) {
        tek(d, ch);
        return;
    }
    if (!d->vt_mode) {
        /* CAPS is always on in the Tektronix personality. */
        if (ch >= 'a' && ch <= 'z')
            ch = (uint8_t)(ch - 32);
        if (d->gin) {
            report(d, d->cross_x, d->cross_y, ch);
            return;
        }
        key_code(d, ch);
        return;
    }
    DafVT *v = &d->vt;
    if (v->margin_bell && ch >= 32 && v->col == vt_columns(v, v->row) - 8)
        bell(d);
    key_code(d, ch);
    if (ch == 13 && d->line.newline)
        key_code(d, 10);
}

static void send_sequence(Daf *d, char lead, char final)
{
    key_code(d, 27);
    if (lead)
        key_code(d, (uint8_t)lead);
    key_code(d, (uint8_t)final);
}

static void tek_key(Daf *d, DafKey key, unsigned modifiers)
{
    int step = modifiers & DAF_SHIFT ? 8 : 1;
    if (key == DAF_LINEFEED)
        click(d);
    switch (key) {
    case DAF_LEFT: case DAF_RIGHT: case DAF_UP: case DAF_DOWN:
        if (d->gin) {
            int x = d->cross_x, y = d->cross_y;
            if (key == DAF_LEFT) x -= step;
            if (key == DAF_RIGHT) x += step;
            if (key == DAF_UP) y += step;
            if (key == DAF_DOWN) y -= step;
            d->cross_x = bound(x, 0, DAF_WIDTH - 1);
            d->cross_y = bound(y, 0, DAF_WORK_HEIGHT - 1);
            show(d, d->cross_y, d->cross_y);
        } else if (key == DAF_UP || key == DAF_DOWN) {
            d->viewport = bound(d->viewport + (key == DAF_UP ? step : -step), 0, DAF_WORK_HEIGHT - DAF_HEIGHT);
        }
        break;
    case DAF_PF1:
        if ((modifiers & DAF_CTRL) && !d->keyboard_special) {
            d->keyboard_special = true;
            d->parser = SPECIAL;
            flow(d);
        }
        break;
    case DAF_PF2:
        /* PRINT: no printer, but it leaves graphic input; CTRL+PF2
         * reactivates alpha printing. */
        d->gin = false;
        if (modifiers & DAF_CTRL)
            d->print_alpha = true;
        break;
    case DAF_PAGE: case DAF_RESET:
        tek_reset(d, key == DAF_PAGE);
        flow(d);
        break;
    case DAF_SCRL:
        if (modifiers & DAF_SHIFT) {
            d->track = false;
        } else if (modifiers & DAF_CTRL) {
            d->track = true;
            if (d->tek_mode == ALPHA)
                show(d, d->y, d->y + TEK_H - 1);
            else
                show(d, d->gy, d->gy);
        }
        break;
    case DAF_LINEFEED:
        if (d->line.newline)
            key_code(d, 13);
        key_code(d, 10);
        break;
    default: break;
    }
}

static void vt_key(Daf *d, DafKey key, unsigned modifiers)
{
    DafVT *v = &d->vt;
    static const char cursor[] = "DCAB";
    if (key <= DAF_PF4 || key == DAF_LINEFEED)
        click(d);
    switch (key) {
    case DAF_LEFT: case DAF_RIGHT: case DAF_UP: case DAF_DOWN:
        if (!v->ansi)
            send_sequence(d, 0, cursor[key]);
        else
            send_sequence(d, v->app_keypad && v->app_cursor ? 'O' : '[', cursor[key]);
        break;
    case DAF_PF1: case DAF_PF2: case DAF_PF3: case DAF_PF4:
        send_sequence(d, v->ansi ? 'O' : 0, (char)('P' + (key - DAF_PF1)));
        break;
    case DAF_ENTER:
        if (v->app_keypad)
            send_sequence(d, v->ansi ? 'O' : '?', 'M');
        else
            daf_key_char(d, 13);
        break;
    case DAF_LINEFEED:
        key_code(d, 10);
        break;
    case DAF_SCRL:
        if (modifiers & DAF_CTRL) {
            d->setup = SETUP_A;
            d->setup_col = 0;
            flow(d);
        } else if (v->auto_xon) {
            d->hold = !d->hold;
            flow(d);
        }
        break;
    default: break;
    }
}

void daf_key(Daf *d, DafKey key, unsigned modifiers)
{
    if (key == DAF_BREAK) {
        if (!d->line.online)
            return;
        if (d->vt_mode && (modifiers & DAF_CTRL))
            vt_reply(d, d->answerback);
        else
            d->break_ms = modifiers & DAF_SHIFT ? LONG_BREAK_MS : BREAK_MS;
        return;
    }
    if (d->setup) {
        if (key == DAF_SCRL && (modifiers & DAF_CTRL))
            setup_exit(d);
        else if (key == DAF_LEFT)
            d->setup_col = (d->setup_col + (d->setup == SETUP_A ? DAF_COLS : 16) - 1) % (d->setup == SETUP_A ? DAF_COLS : 16);
        else if (key == DAF_RIGHT)
            setup_key_char(d, ' ');
        else if (key == DAF_ENTER)
            setup_key_char(d, 13);
        return;
    }
    if (d->vt_mode)
        vt_key(d, key, modifiers);
    else
        tek_key(d, key, modifiers);
}

/* Keypad keys: inert in the Tektronix personality, application sequences
 * in VT mode when selected. Returns true when the key was consumed. */
bool daf_keypad(Daf *d, uint8_t key)
{
    if (!key)
        return false;
    if (!d->vt_mode || d->setup)
        return !d->setup;
    if (!d->vt.app_keypad)
        return false;
    static const char ordinary[] = "0123456789.,-\r";
    static const char application[] = "pqrstuvwxynlmM";
    const char *found = strchr(ordinary, key);
    if (!found)
        return false;
    click(d);
    send_sequence(d, d->vt.ansi ? 'O' : '?', application[found - ordinary]);
    return true;
}

void daf_render(const Daf *d, uint32_t *frame, uint32_t off, uint32_t on, uint32_t cursor, bool blink)
{
    if (d->vt_mode && d->setup) {
        render_setup(d, frame, off, on, blink);
        return;
    }
    int vx = -1, vy = -1, vw = 0, vh = 0;
    if (d->vt_mode && d->vt.cursor && blink) {
        vw = vt_width(&d->vt, d->vt.row);
        vh = d->vt.block_cursor ? VT_H : 1;
        vx = VT_LEFT + bound(d->vt.col * vw, 0, DAF_COLS * VT_W - vw);
        vy = vt_top_pixel(d->vt.row) - (d->vt.block_cursor ? 0 : VT_H - 2);
    } else if (!d->vt_mode && !d->gin && d->tek_mode == ALPHA && d->print_alpha && blink) {
        /* The 7x11 blinking matrix cursor of the alpha position. */
        vx = d->x;
        vy = d->y + TEK_H - 1;
        vw = TEK_W;
        vh = TEK_H;
    }
    int origin = d->vt_mode ? 0 : d->viewport;
    bool reverse = d->vt_mode && d->vt.reverse;
    for (int row = 0; row < DAF_HEIGHT; ++row) {
        int y = origin + DAF_HEIGHT - 1 - row;
        for (int x = 0; x < DAF_WIDTH; ++x) {
            uint8_t pixel = d->bitmap[y][x];
            bool pixel_on = pixel == 1 || (pixel == 2 && blink);
            if ((x >= vx && x < vx + vw && y <= vy && y > vy - vh) ||
                (!d->vt_mode && d->gin && (x == d->cross_x || y == d->cross_y))) {
                *frame++ = pixel_on ? off : cursor;
            } else {
                *frame++ = pixel_on != reverse ? on : off;
            }
        }
    }
}

static void demo_point(Daf *d, int x, int y)
{
    unsigned hx = (unsigned)x * 2, hy = (unsigned)y * 2;
    daf_receive(d, (uint8_t)(0x20 | (hy >> 5)));
    daf_receive(d, (uint8_t)(0x60 | (hy & 31)));
    daf_receive(d, (uint8_t)(0x20 | (hx >> 5)));
    daf_receive(d, (uint8_t)(0x40 | (hx & 31)));
    daf_drain(d, 4);
}

void daf_demo(Daf *d)
{
    daf_reset(d, DAF_DEFAULT_SWITCHES, NULL, NULL);
    daf_puts(d, "DAF-2020   TEK GRAPHICS\r\n\r\n");
    daf_puts(d, "\035");
    demo_point(d, 56, 160);
    demo_point(d, 56, 310);
    demo_point(d, 456, 310);
    demo_point(d, 456, 160);
    demo_point(d, 56, 160);
    for (int i = 0; i < 6; ++i) {
        daf_puts(d, "\033X\001EE\035");
        demo_point(d, 56, 185 + i * 20);
        demo_point(d, 456, 185 + i * 20);
    }
    daf_puts(d, "\033X\001??\035");
    for (int x = 56; x <= 456; x += 2) {
        int wave = (x - 56) % 100;
        if (wave > 50)
            wave = 100 - wave;
        int y = 183 + wave * (100 - wave) / 25;
        demo_point(d, x, y);
    }
    daf_puts(d, "\035");
    demo_point(d, 56, 139);
    daf_puts(d, "\037" "512 X 288 VIEWPORT");
}
