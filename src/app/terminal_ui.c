#include "terminal_ui.h"
#include "font8x8.h"

#include <stdio.h>
#include <string.h>

TerminalPalette terminal_palette(bool shader, bool inverted)
{
    TerminalPalette p = shader ?
        (TerminalPalette){0xff000000, 0xffc8c8c8, 0xffffffff, 0xff202020, 0xff464646,
                          0xffe8e8e8, 0xff9a9a9a, 0xffffffff, 0xffffffff, 0xff9a9a9a} :
        (TerminalPalette){0xff020d05, 0xff53d568, 0xffadffbd, 0xff142019, 0xff294839,
                          0xffe0ede3, 0xffb6cfbd, 0xff53d568, 0xffffbb90, 0xffb6cfbd};
    if (inverted) {
        uint32_t dark = p.off;
        p.off = p.on;
        p.on = p.cursor = p.status = dark;
    }
    return p;
}

void terminal_draw(const Daf *screen, uint32_t *frame, uint64_t now_ms, const TerminalPalette *p)
{
    static uint32_t picture[DAF_WIDTH * DAF_HEIGHT];
    daf_render(screen, picture, p->off, p->on, p->cursor, (now_ms / 440) % 2 == 0);
    for (size_t i = 0; i < (size_t)TERMINAL_FRAME_W * TERMINAL_FRAME_H; ++i)
        frame[i] = p->off;
    for (int y = 0; y < DAF_HEIGHT; ++y)
        memcpy(frame + (size_t)(y + TERMINAL_MARGIN) * TERMINAL_FRAME_W + TERMINAL_MARGIN,
               picture + (size_t)y * DAF_WIDTH, DAF_WIDTH * sizeof(*frame));
}

void terminal_rect(uint32_t *frame, int x, int y, int width, int height, uint32_t color)
{
    x += TERMINAL_MARGIN;
    y += TERMINAL_MARGIN;
    if (x < 0 || y < 0 || x + width > TERMINAL_FRAME_W || y + height > TERMINAL_FRAME_H)
        return;
    for (int j = y; j < y + height; ++j)
        for (int i = x; i < x + width; ++i)
            frame[(size_t)j * TERMINAL_FRAME_W + (size_t)i] = color;
}

void terminal_text(uint32_t *frame, int x, int y, const char *text, uint32_t color)
{
    x += TERMINAL_MARGIN;
    y += TERMINAL_MARGIN;
    if (y < 0 || y + 8 > TERMINAL_FRAME_H)
        return;
    for (; *text && x <= TERMINAL_FRAME_W - 8; ++text, x += 8) {
        unsigned ch = (unsigned char)*text;
        if (ch < 32 || ch > 126)
            ch = '?';
        const uint8_t *bitmap = mx_font8x8_ascii[ch - 32];
        for (int j = 0; j < 8; ++j)
            for (int i = 0; i < 8; ++i)
                if (bitmap[j] & (128 >> i))
                    frame[(size_t)(y + j) * TERMINAL_FRAME_W + (size_t)(x + i)] = color;
    }
}

static uint8_t keypad_char(SDL_Keycode sym)
{
    if (sym >= SDLK_KP_1 && sym <= SDLK_KP_9)
        return (uint8_t)('1' + sym - SDLK_KP_1);
    switch (sym) {
    case SDLK_KP_0: return '0';
    case SDLK_KP_PERIOD: return '.';
    case SDLK_KP_COMMA: return ',';
    case SDLK_KP_MINUS: return '-';
    case SDLK_KP_ENTER: return 13;
    default: return 0;
    }
}

static int control_char(SDL_Keycode sym)
{
    if (sym >= SDLK_a && sym <= SDLK_z)
        return (int)(sym - SDLK_a + 1);
    switch (sym) {
    case SDLK_SPACE: return 0;
    case SDLK_LEFTBRACKET: return 27;
    case SDLK_BACKSLASH: return 28;
    case SDLK_RIGHTBRACKET: return 29;
    case SDLK_BACKQUOTE: case SDLK_6: return 30;
    case SDLK_SLASH: case SDLK_MINUS: return 31;
    default: return -1;
    }
}

void terminal_key(Daf *d, uint8_t *keypad_text, SDL_Keysym input, bool repeat)
{
    SDL_Keycode sym = input.sym;
    bool ctrl = (input.mod & KMOD_CTRL) != 0, shift = (input.mod & KMOD_SHIFT) != 0;
    unsigned modifiers = (shift ? DAF_SHIFT : 0) | (ctrl ? DAF_CTRL : 0);
    *keypad_text = 0;
    /* ESC, TAB, RETURN, SCRL and CTRL combinations never repeat. */
    if (repeat && (ctrl || sym == SDLK_ESCAPE || sym == SDLK_TAB || sym == SDLK_RETURN ||
                   sym == SDLK_F9 || sym == SDLK_SCROLLLOCK))
        return;

    uint8_t keypad = keypad_char(sym);
    if (keypad) {
        if (daf_keypad(d, keypad))
            *keypad_text = keypad;
        else if (sym == SDLK_KP_ENTER)
            daf_key(d, DAF_ENTER, modifiers);
        return;
    }
    if (ctrl) {
        int code = control_char(sym);
        if (code >= 0) {
            daf_key_char(d, (uint8_t)code);
            return;
        }
    }
    switch (sym) {
    case SDLK_ESCAPE: daf_key_char(d, 27); break;
    case SDLK_RETURN: daf_key_char(d, 13); break;
    case SDLK_TAB: daf_key_char(d, 9); break;
    case SDLK_BACKSPACE: daf_key_char(d, 8); break;
    case SDLK_DELETE: daf_key_char(d, 127); break;
    case SDLK_LEFT: daf_key(d, DAF_LEFT, modifiers); break;
    case SDLK_RIGHT: daf_key(d, DAF_RIGHT, modifiers); break;
    case SDLK_UP: daf_key(d, DAF_UP, modifiers); break;
    case SDLK_DOWN: daf_key(d, DAF_DOWN, modifiers); break;
    case SDLK_F1: case SDLK_F2: case SDLK_F3: case SDLK_F4:
        daf_key(d, (DafKey)(DAF_PF1 + (sym - SDLK_F1)), modifiers);
        break;
    case SDLK_F5: daf_key(d, DAF_PAGE, modifiers); break;
    case SDLK_F6: daf_key(d, DAF_RESET, modifiers); break;
    case SDLK_F7: daf_key(d, DAF_LINEFEED, modifiers); break;
    case SDLK_F8: case SDLK_PAUSE: daf_key(d, DAF_BREAK, modifiers); break;
    case SDLK_F9: case SDLK_SCROLLLOCK: daf_key(d, DAF_SCRL, modifiers); break;
    default: break;
    }
}

void terminal_text_input(Daf *d, uint8_t keypad_text, const char *text)
{
    if (SDL_GetModState() & (KMOD_CTRL | KMOD_ALT))
        return;
    const unsigned char *p = (const unsigned char *)text;
    if (keypad_text && *p == keypad_text && p[1] == 0)
        return;
    for (; *p; ++p)
        if (*p >= 32 && *p < 127)
            daf_key_char(d, *p);
}

bool terminal_replay(Daf *d, const char *path, char *error, size_t size)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        snprintf(error, size, "Cannot open serial replay %.300s", path);
        return false;
    }
    daf_reset(d, DAF_DEFAULT_SWITCHES, NULL, NULL);
    uint8_t buf[1024];
    size_t count;
    while ((count = fread(buf, 1, sizeof(buf), file))) {
        for (size_t i = 0; i < count; ++i) {
            daf_receive(d, buf[i]);
            d->busy_us = 0;
            daf_drain(d, 1);
        }
    }
    bool ok = !ferror(file);
    fclose(file);
    if (!ok)
        snprintf(error, size, "Cannot read serial replay %.300s", path);
    return ok;
}

bool terminal_screenshot(mx_video *video, const char *stem, unsigned *number, bool raw)
{
    char name[48];
    while (*number < 10000) {
        snprintf(name, sizeof(name), "%s-%04u.png", stem, (*number)++);
        FILE *probe = fopen(name, "rb");
        if (probe) {
            fclose(probe);
            continue;
        }
        mx_video_request_shot(video, name, raw);
        return true;
    }
    return false;
}
