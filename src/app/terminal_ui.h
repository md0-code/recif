#ifndef RECIF_TERMINAL_UI_H
#define RECIF_TERMINAL_UI_H

#include "daf2020.h"
#include "video.h"

/* The presented frame: the 512 x 288 screen inside a margin, so the CRT
 * look's rounded corners do not cut the picture, and a status line. */
enum {
    TERMINAL_MARGIN = 8, TERMINAL_FRAME_W = DAF_WIDTH + 2 * TERMINAL_MARGIN,
    TERMINAL_STATUS_Y = TERMINAL_MARGIN + DAF_HEIGHT + 3, TERMINAL_FRAME_H = TERMINAL_STATUS_Y + 8 + TERMINAL_MARGIN
};

/* Colours for the green phosphor: grey levels when the shader tints the
 * picture, green ones when it is drawn plain. Inverted swaps the screen's
 * dark and lit levels. */
typedef struct {
    uint32_t off, on, cursor, panel, select, text, dim, lit, warn, status;
} TerminalPalette;
TerminalPalette terminal_palette(bool shader, bool inverted);

void terminal_draw(const Daf *screen, uint32_t *frame, uint64_t now_ms, const TerminalPalette *p);
/* Screen coordinates: (0, 0) is the top left of the 512 x 288 screen. */
void terminal_text(uint32_t *frame, int x, int y, const char *text, uint32_t color);
void terminal_rect(uint32_t *frame, int x, int y, int width, int height, uint32_t color);
/* Host keys to DAF keys: F1-F4 PF1-PF4, F5 PAGE, F6 RESET, F7 LF,
 * F8 or Pause BREAK, F9 or Scroll Lock SCRL. */
void terminal_key(Daf *d, uint8_t *keypad_text, SDL_Keysym input, bool repeat);
void terminal_text_input(Daf *d, uint8_t keypad_text, const char *text);
bool terminal_replay(Daf *d, const char *path, char *error, size_t size);
bool terminal_screenshot(mx_video *video, const char *stem, unsigned *number, bool raw);

#endif
