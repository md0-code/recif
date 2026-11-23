#include "dafserial.h"
#ifndef _WIN32
#include "icon_data.h"
#endif
#include "lang.h"
#include "prefs.h"
#include "rc_version.h"
#include "serial_link.h"
#include "terminal_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    HOST_HZ = 60, STEPS = RS232_HZ / RS232_TICK / HOST_HZ, ENDPOINT_SIZE = RC_HOST_PATH + 16,
    AUDIO_RATE = 22050, POPUP_ITEMS = RC_HOST_PORTS
};
enum { ROW_LANGUAGE, ROW_MODE, ROW_LINK, ROW_ADDRESS, ROW_SWITCHES, ROW_CRT, ROW_SCREEN, ROW_RESTART, ROWS };
enum { LINK_NONE, LINK_TCP, LINK_LISTEN, LINK_PIPE, LINK_SERIAL, LINK_LOOPBACK, LINKS };

typedef struct {
    Rs232Format format;
    const char *endpoint, *replay, *screenshot;
    unsigned frames;
    bool hidden, demo, plain, setup, echo, format_set;
} Options;

/* What the settings page edits; it applies when the page is left, except
 * the language, the CRT look and the inverted screen, which apply at once. */
typedef struct {
    unsigned link;
    char address[ENDPOINT_SIZE];
    uint16_t switches;
    Language lang;
    bool plain, inverted, vt;
} TerminalSettings;

typedef struct {
    DafSerial terminal;
    SerialLink link;
    mx_video *video;
    SDL_AudioDeviceID audio;
    Prefs prefs;
    TerminalPalette palette;
    uint32_t frame[TERMINAL_FRAME_W * TERMINAL_FRAME_H];
    uint64_t clock;
    uint8_t keypad_text;
    unsigned screenshot, row, switch_index, link_changes, popup_count, popup_index;
    char popup[POPUP_ITEMS][RC_HOST_PORT_NAME + 16], edit_text[ENDPOINT_SIZE];
    TerminalSettings settings, edit;
    char error[160];
    bool setup, drop_text, popup_open, editing, shader;
} Window;

static const char *const link_prefix[LINKS] = {"none", "tcp:", "tcp-listen:", "pipe:", "", "loopback"};
static const char *const link_default[LINKS] = {"", "127.0.0.1:2323", "2323", "recif", "COM1", ""};

static bool parse(Options *o, int argc, char **argv)
{
    o->format = rs232_console_format;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--hidden")) {
            o->hidden = true;
        } else if (!strcmp(arg, "--demo")) {
            o->demo = true;
        } else if (!strcmp(arg, "--plain")) {
            o->plain = true;
        } else if (!strcmp(arg, "--setup")) {
            o->setup = true;
        } else if (!strcmp(arg, "--local-echo")) {
            o->echo = true;
        } else if (!strcmp(arg, "--replay") && i + 1 < argc) {
            o->replay = argv[++i];
        } else if (!strcmp(arg, "--serial") && i + 1 < argc) {
            o->endpoint = argv[++i];
            if (strlen(o->endpoint) >= ENDPOINT_SIZE)
                return false;
        } else if (!strcmp(arg, "--serial-format") && i + 1 < argc) {
            if (!rs232_parse_format(argv[++i], &o->format))
                return false;
            o->format_set = true;
        } else if (!strcmp(arg, "--screenshot") && i + 1 < argc) {
            o->screenshot = argv[++i];
        } else if (!strcmp(arg, "--frames") && i + 1 < argc) {
            char *end;
            const char *count = argv[++i];
            unsigned long frames = strtoul(count, &end, 10);
            if (end == count || *end || !frames || frames > 100000)
                return false;
            o->frames = (unsigned)frames;
        } else {
            return false;
        }
    }
    if ((o->demo && o->replay) || ((o->demo || o->replay) && (o->endpoint || o->setup)))
        return false;
    return (!o->hidden && !o->screenshot) || o->frames;
}

/* An endpoint string as a connection kind and its address. */
static void split_endpoint(const char *endpoint, TerminalSettings *s)
{
    s->address[0] = '\0';
    if (!endpoint || !*endpoint || !strcmp(endpoint, "none")) {
        s->link = LINK_NONE;
    } else if (!strcmp(endpoint, "loopback")) {
        s->link = LINK_LOOPBACK;
    } else if (!strncmp(endpoint, "tcp-listen:", 11)) {
        s->link = LINK_LISTEN;
        snprintf(s->address, sizeof(s->address), "%s", endpoint + 11);
    } else if (!strncmp(endpoint, "tcp:", 4)) {
        s->link = LINK_TCP;
        snprintf(s->address, sizeof(s->address), "%s", endpoint + 4);
    } else if (!strncmp(endpoint, "pipe:", 5)) {
        s->link = LINK_PIPE;
        snprintf(s->address, sizeof(s->address), "%s", endpoint + 5);
    } else {
        s->link = LINK_SERIAL;
        snprintf(s->address, sizeof(s->address), "%s",
                 !strncmp(endpoint, "serial:", 7) ? endpoint + 7 : endpoint);
    }
}

static void join_endpoint(const TerminalSettings *s, char *out, size_t size)
{
    if (s->link == LINK_NONE || s->link == LINK_LOOPBACK)
        snprintf(out, size, "%s", link_prefix[s->link]);
    else
        snprintf(out, size, "%s%s", link_prefix[s->link], s->address);
}

/* S1-S7 for a serial format such as 9600,8N1. */
static uint16_t switches_for(uint16_t switches, const Rs232Format *f)
{
    switches &= 0177600; /* keep S8-S14 */
    unsigned rate = 0;
    while ((150u << rate) < f->baud)
        ++rate;
    if (rate & 4) switches |= DAF_SWITCH(1);
    if (rate & 2) switches |= DAF_SWITCH(2);
    if (rate & 1) switches |= DAF_SWITCH(3);
    if (f->parity != RS232_NO_PARITY)
        switches |= DAF_SWITCH(5) | (f->parity == RS232_EVEN ? DAF_SWITCH(4) : 0);
    if (f->stop_bits == 2)
        switches |= DAF_SWITCH(6);
    if (f->data_bits == 8)
        switches |= DAF_SWITCH(7);
    return switches;
}

static uint16_t parse_switches(const char *text, uint16_t fallback)
{
    if (strlen(text) != 14)
        return fallback;
    uint16_t value = 0;
    for (unsigned i = 0; i < 14; ++i) {
        if (text[i] != '0' && text[i] != '1')
            return fallback;
        if (text[i] == '1')
            value |= (uint16_t)DAF_SWITCH(i + 1);
    }
    return value;
}

static void save_settings(Window *w)
{
    char switches[15], endpoint[ENDPOINT_SIZE];
    for (unsigned i = 0; i < 14; ++i)
        switches[i] = w->settings.switches & DAF_SWITCH(i + 1) ? '1' : '0';
    switches[14] = '\0';
    join_endpoint(&w->settings, endpoint, sizeof(endpoint));
    prefs_set(&w->prefs, "serial", endpoint);
    prefs_set(&w->prefs, "switches", switches);
    prefs_set(&w->prefs, "mode", w->settings.vt ? "vt100" : "tektronix");
    prefs_set(&w->prefs, "crt", w->settings.plain ? "off" : "on");
    prefs_set(&w->prefs, "screen", w->settings.inverted ? "inverted" : "normal");
    prefs_set(&w->prefs, "language", lang_name(w->settings.lang));
    prefs_save(&w->prefs);
}

/* The link frames bytes with the terminal's own transmit and receive
 * settings, which SET-UP B can change. */
static void sync_link(Window *w)
{
    const DafLine *line = &w->terminal.display.line;
    if (w->link_changes == w->terminal.display.line_changes)
        return;
    Rs232Format to_terminal = daf_serial_format(line, false), from_terminal = daf_serial_format(line, true);
    serial_link_set_formats(&w->link, &to_terminal, &from_terminal);
    w->link_changes = w->terminal.display.line_changes;
}

/* The green phosphor: through the CRT shader's monochrome mode when it is
 * available, else drawn in green directly. */
static void set_look(Window *w, bool plain)
{
    w->shader = mx_video_set_look(w->video, plain ? MX_VIDEO_LOOK_PLAIN : MX_VIDEO_LOOK_CRT,
                                  MX_VIDEO_PHOSPHOR_GREEN);
    if (!w->shader)
        mx_video_set_look(w->video, MX_VIDEO_LOOK_PLAIN, MX_VIDEO_PHOSPHOR_COLOR);
    w->palette = terminal_palette(w->shader, w->settings.inverted);
}

/* Power off and on, in the personality chosen in the settings. */
static void power_cycle(Window *w)
{
    daf_serial_reset(&w->terminal, w->settings.switches);
    daf_set_mode(&w->terminal.display, w->settings.vt);
    w->link_changes = w->terminal.display.line_changes - 1;
}

static void open_settings(Window *w)
{
    w->settings.vt = w->terminal.display.vt_mode;
    w->edit = w->settings;
    w->error[0] = '\0';
    w->row = 0;
    w->popup_open = w->editing = false;
    w->setup = true;
}

static void leave_settings(Window *w)
{
    char old_endpoint[ENDPOINT_SIZE], endpoint[ENDPOINT_SIZE];
    join_endpoint(&w->settings, old_endpoint, sizeof(old_endpoint));
    join_endpoint(&w->edit, endpoint, sizeof(endpoint));
    if (strcmp(endpoint, old_endpoint)) {
        SerialLink link;
        Rs232Format format = daf_serial_format(&w->terminal.display.line, false);
        if (!serial_link_open(&link, endpoint, &format, w->error, sizeof(w->error))) {
            w->row = ROW_LINK;
            return;
        }
        serial_link_close(&w->link);
        w->link = link;
        w->link_changes = w->terminal.display.line_changes - 1;
        w->terminal.keys = (Rs232Queue){0};
        w->terminal.display.tx_head = w->terminal.display.tx_count = 0;
        w->terminal.display.peer_stopped = w->terminal.display.flow_stopped = false;
    }
    if (w->edit.switches != w->settings.switches)
        daf_set_switches(&w->terminal.display, w->edit.switches);
    if (w->edit.vt != w->terminal.display.vt_mode)
        daf_set_mode(&w->terminal.display, w->edit.vt);
    w->settings = w->edit;
    w->setup = false;
    sync_link(w);
    save_settings(w);
}

static const char *language_label(Language lang)
{
    return lang == LANG_RO ? "Romana" : "English";
}

static const char *link_label(Language lang, unsigned link)
{
    switch (link) {
    case LINK_TCP: return TR(lang, "TCP client", "Client TCP");
    case LINK_LISTEN: return TR(lang, "TCP server", "Server TCP");
    case LINK_PIPE: return TR(lang, "Named pipe", "Pipe");
    case LINK_SERIAL: return TR(lang, "Serial port", "Port serial");
    case LINK_LOOPBACK: return TR(lang, "Loopback", "Bucla");
    default: return TR(lang, "Not connected", "Neconectat");
    }
}

static void open_popup(Window *w)
{
    Language l = w->edit.lang;
    unsigned current = 0;
    w->popup_count = 0;
    switch (w->row) {
    case ROW_LANGUAGE:
        strcpy(w->popup[w->popup_count++], language_label(LANG_EN));
        strcpy(w->popup[w->popup_count++], language_label(LANG_RO));
        current = w->edit.lang;
        break;
    case ROW_MODE:
        strcpy(w->popup[w->popup_count++], "Tektronix 4010");
        strcpy(w->popup[w->popup_count++], "VT100");
        current = w->edit.vt;
        break;
    case ROW_LINK:
        for (unsigned i = 0; i < LINKS; ++i)
            strcpy(w->popup[w->popup_count++], link_label(l, i));
        current = w->edit.link;
        break;
    case ROW_ADDRESS: {
        char ports[RC_HOST_PORTS][RC_HOST_PORT_NAME];
        unsigned n = rc_host_list_serial(ports, RC_HOST_PORTS);
        for (unsigned i = 0; i < n; ++i) {
            strcpy(w->popup[w->popup_count++], ports[i]);
            if (!strcmp(ports[i], w->edit.address))
                current = i;
        }
        break;
    }
    case ROW_CRT:
        strcpy(w->popup[w->popup_count++], TR(l, "On", "Da"));
        strcpy(w->popup[w->popup_count++], TR(l, "Off", "Nu"));
        current = w->edit.plain;
        break;
    case ROW_SCREEN:
        strcpy(w->popup[w->popup_count++], TR(l, "Normal", "Normal"));
        strcpy(w->popup[w->popup_count++], TR(l, "Inverted", "Inversat"));
        current = w->edit.inverted;
        break;
    default: break;
    }
    w->popup_index = current;
    w->popup_open = w->popup_count > 0;
}

static void choose(Window *w)
{
    unsigned i = w->popup_index;
    w->popup_open = false;
    switch (w->row) {
    case ROW_LANGUAGE:
        w->edit.lang = w->settings.lang = (Language)i;
        break;
    case ROW_MODE: w->edit.vt = i == 1; break;
    case ROW_LINK:
        if (i != w->edit.link) {
            w->edit.link = i;
            snprintf(w->edit.address, sizeof(w->edit.address), "%s", link_default[i]);
        }
        break;
    case ROW_ADDRESS:
        snprintf(w->edit.address, sizeof(w->edit.address), "%s", w->popup[i]);
        break;
    case ROW_CRT:
        w->edit.plain = w->settings.plain = i == 1;
        set_look(w, w->edit.plain);
        break;
    case ROW_SCREEN:
        w->edit.inverted = w->settings.inverted = i == 1;
        set_look(w, w->edit.plain);
        break;
    default: break;
    }
    w->error[0] = '\0';
}

static void activate(Window *w)
{
    if (w->row == ROW_SWITCHES) {
        w->edit.switches ^= (uint16_t)DAF_SWITCH(w->switch_index + 1);
    } else if (w->row == ROW_ADDRESS) {
        if (w->edit.link == LINK_NONE || w->edit.link == LINK_LOOPBACK)
            return;
        if (w->edit.link == LINK_SERIAL)
            open_popup(w);
        if (!w->popup_open) {
            snprintf(w->edit_text, sizeof(w->edit_text), "%s", w->edit.address);
            w->editing = true;
        }
    } else if (w->row == ROW_RESTART) {
        leave_settings(w);
        if (!w->setup)
            power_cycle(w);
    } else if (w->row != ROW_CRT || mx_video_crt_available(w->video)) {
        open_popup(w);
    }
}

static void settings_text(Window *w, const char *input)
{
    if (!w->editing)
        return;
    size_t len = strlen(w->edit_text);
    for (; *input && len + 1 < sizeof(w->edit_text); ++input)
        if ((unsigned char)*input >= 32 && *input != 127)
            w->edit_text[len++] = *input;
    w->edit_text[len] = '\0';
}

static void settings_key(Window *w, SDL_Keysym k)
{
    bool ctrl = (k.mod & KMOD_CTRL) != 0;
    bool enter = k.sym == SDLK_RETURN || k.sym == SDLK_KP_ENTER;
    if (w->editing) {
        if (enter) {
            snprintf(w->edit.address, sizeof(w->edit.address), "%s", w->edit_text);
            w->editing = false;
        } else if (k.sym == SDLK_ESCAPE) {
            w->editing = false;
        } else if (k.sym == SDLK_BACKSPACE && *w->edit_text) {
            w->edit_text[strlen(w->edit_text) - 1] = '\0';
        } else if (ctrl && k.sym == SDLK_a) {
            w->edit_text[0] = '\0';
        } else if (ctrl && k.sym == SDLK_v) {
            char *clip = SDL_GetClipboardText();
            if (clip)
                settings_text(w, clip);
            SDL_free(clip);
        }
        return;
    }
    if (w->popup_open) {
        if (k.sym == SDLK_UP)
            w->popup_index = (w->popup_index + w->popup_count - 1) % w->popup_count;
        else if (k.sym == SDLK_DOWN)
            w->popup_index = (w->popup_index + 1) % w->popup_count;
        else if (enter)
            choose(w);
        else if (k.sym == SDLK_ESCAPE)
            w->popup_open = false;
        return;
    }
    switch (k.sym) {
    case SDLK_ESCAPE: case SDLK_F12: leave_settings(w); break;
    case SDLK_UP: w->row = (w->row + ROWS - 1) % ROWS; break;
    case SDLK_DOWN: w->row = (w->row + 1) % ROWS; break;
    case SDLK_LEFT: case SDLK_RIGHT:
        if (w->row == ROW_SWITCHES)
            w->switch_index = (w->switch_index + (k.sym == SDLK_LEFT ? 13 : 1)) % 14;
        break;
    case SDLK_RETURN: case SDLK_KP_ENTER: activate(w); break;
    default: break;
    }
}

static void key(Window *w, const Options *o, SDL_KeyboardEvent e)
{
    SDL_Keysym k = e.keysym;
    w->drop_text = false;
    if (k.sym == SDLK_RETURN && (k.mod & KMOD_ALT)) {
        mx_video_set_fullscreen(w->video, !mx_video_fullscreen(w->video));
    } else if (k.sym == SDLK_PRINTSCREEN) {
        terminal_screenshot(w->video, "daf2020", &w->screenshot, !w->shader);
    } else if (k.sym == SDLK_F12 && (k.mod & KMOD_SHIFT)) {
        SDL_Event quit = {.type = SDL_QUIT};
        SDL_PushEvent(&quit);
    } else if (k.sym == SDLK_F12 && (k.mod & (KMOD_CTRL | KMOD_ALT))) {
        /* Ctrl+F12 switches off and on, Alt+F12 resets. */
        if (w->setup)
            leave_settings(w);
        if (w->setup)
            return;
        if (k.mod & KMOD_CTRL) {
            power_cycle(w);
        } else {
            daf_restart(&w->terminal.display);
            daf_set_mode(&w->terminal.display, w->settings.vt);
        }
    } else if (w->setup) {
        settings_key(w, k);
    } else if (k.sym == SDLK_F12 && !o->demo && !o->replay) {
        open_settings(w);
    } else {
        Daf *d = &w->terminal.display;
        if (e.repeat && d->vt_mode && !d->vt.autorepeat) {
            w->drop_text = true;
            return;
        }
        terminal_key(d, &w->keypad_text, k, e.repeat != 0);
    }
}

static const char *switch_meaning(Language l, uint16_t on, unsigned index, char *text, size_t size)
{
    DafLine line;
    daf_line_from_switches(&line, on);
    switch (index + 1) {
    case 1: case 2: case 3: snprintf(text, size, TR(l, "S1-S3 speed %u baud", "S1-S3 viteza %u bauds"), line.tx_baud); break;
    case 4: case 5:
        snprintf(text, size, "S4,S5 %s", line.parity == RS232_NO_PARITY ? TR(l, "no parity", "fara paritate") :
                 line.parity == RS232_EVEN ? TR(l, "even parity", "paritate para") : TR(l, "odd parity", "paritate impara"));
        break;
    case 6: snprintf(text, size, "S6 %s", line.stop_bits > 1 ? TR(l, "two stop bits", "doi biti de STOP") :
                     TR(l, "one stop bit", "un bit de STOP")); break;
    case 7: snprintf(text, size, TR(l, "S7 %u-bit characters", "S7 caracter de %u biti"), line.data_bits); break;
    case 8: snprintf(text, size, "S8 %s", line.online ? TR(l, "on line", "cuplat la calculator") :
                     TR(l, "local", "necuplat la calculator")); break;
    case 9: case 10:
        snprintf(text, size, TR(l, "S9,S10 GIN end: %s", "S9,S10 terminator GIN: %s"),
                 !(on & DAF_SWITCH(10)) ? TR(l, "nothing", "nimic") : on & DAF_SWITCH(9) ? "CR EOT" : "CR");
        break;
    case 11: snprintf(text, size, "S11 %s", on & DAF_SWITCH(11) ? TR(l, "local echo", "cu ecou local") :
                      TR(l, "no local echo", "fara ecou local")); break;
    case 12: snprintf(text, size, "S12 %s", line.newline ? TR(l, "LF with CR", "LF si CR") :
                      TR(l, "LF only", "numai LF")); break;
    case 13: snprintf(text, size, "S13 %s", on & DAF_SWITCH(13) ? TR(l, "BREAK at page full", "BREAK la pagina plina") :
                      TR(l, "no page-full BREAK", "fara BREAK la pagina plina")); break;
    default: snprintf(text, size, "S14 %s", line.keyclick ? TR(l, "key click", "click pe tasta") :
                      TR(l, "no key click", "fara click pe tasta")); break;
    }
    return text;
}

static const char *row_hint(Window *w, char *text, size_t size)
{
    Language l = w->edit.lang;
    switch (w->row) {
    case ROW_LANGUAGE: return TR(l, "Interface language. Applies at once.", "Limba interfetei. Se aplica imediat.");
    case ROW_MODE: return TR(l, "As ESC X 1 and ESC 1. Applies on leaving.", "Ca ESC X 1 si ESC 1. Se aplica la iesire.");
    case ROW_LINK: return TR(l, "Line to the computer. Applies on leaving.", "Legatura cu calculatorul. Se aplica la iesire.");
    case ROW_ADDRESS:
        switch (w->edit.link) {
        case LINK_TCP: return TR(l, "Host and port, as 127.0.0.1:2323.", "Calculator si port, de ex. 127.0.0.1:2323.");
        case LINK_LISTEN: return TR(l, "Port, or address and port.", "Port, sau adresa si port.");
        case LINK_PIPE: return TR(l, "Pipe name, as recif.", "Numele pipe-ului, de ex. recif.");
        case LINK_SERIAL: return TR(l, "Serial port of this computer.", "Portul serial al acestui calculator.");
        default: return TR(l, "Not needed for this line.", "Nu este necesara pentru aceasta legatura.");
        }
    case ROW_SWITCHES: return switch_meaning(l, w->edit.switches, w->switch_index, text, size);
    case ROW_SCREEN: return TR(l, "Dark characters on a lit screen. Applies at once.",
                               "Caractere intunecate pe ecran luminos. Se aplica imediat.");
    case ROW_RESTART: return TR(l, "Power off and on, as Ctrl+F12.", "Oprire si pornire, ca Ctrl+F12.");
    default:
        return mx_video_crt_available(w->video) ? TR(l, "Tube curvature and glow. Applies at once.",
                                                     "Curbura si stralucirea tubului. Se aplica imediat.")
                                                : TR(l, "Needs OpenGL.", "Necesita OpenGL.");
    }
}

static void render_settings(Window *w)
{
    const TerminalPalette *p = &w->palette;
    Language l = w->edit.lang;
    terminal_rect(w->frame, 4, 4, DAF_WIDTH - 8, 268, p->panel);
    terminal_text(w->frame, 16, 12, TR(l, "DAF-2020 SETTINGS", "SETARI DAF-2020"), p->text);
    const char *labels[ROWS] = {
        TR(l, "Language", "Limba"), TR(l, "Mode", "Mod de lucru"), TR(l, "Connection", "Legatura"),
        TR(l, "Address", "Adresa"), TR(l, "Switches", "Comutatoare"), TR(l, "CRT effect", "Efect CRT"),
        TR(l, "Screen", "Ecran"), TR(l, "Restart", "Repornire")
    };
    static const int row_y[ROWS] = {34, 52, 70, 88, 106, 140, 158, 176};
    for (unsigned row = 0; row < ROWS; ++row) {
        int y = row_y[row], height = row == ROW_SWITCHES ? 30 : 16;
        if (row == w->row)
            terminal_rect(w->frame, 10, y - 4, DAF_WIDTH - 20, height, p->select);
        terminal_text(w->frame, 16, y, labels[row], row == w->row ? p->text : p->dim);
    }
    terminal_text(w->frame, 136, row_y[ROW_LANGUAGE], language_label(w->edit.lang), p->text);
    terminal_text(w->frame, 136, row_y[ROW_MODE], w->edit.vt ? "VT100" : "Tektronix 4010", p->text);
    terminal_text(w->frame, 136, row_y[ROW_LINK], link_label(l, w->edit.link), p->text);
    const char *address = w->editing ? w->edit_text : w->edit.link == LINK_NONE || w->edit.link == LINK_LOOPBACK
        ? "-" : w->edit.address;
    size_t length = strlen(address);
    int x = 136;
    if (length > 44) {
        terminal_text(w->frame, x, row_y[ROW_ADDRESS], "...", p->dim);
        address += length - 41;
        x += 24;
    }
    terminal_text(w->frame, x, row_y[ROW_ADDRESS], address, p->text);
    if (w->editing)
        terminal_rect(w->frame, x + (int)strlen(address) * 8, row_y[ROW_ADDRESS] + 7, 8, 1, p->lit);
    terminal_text(w->frame, 136, row_y[ROW_CRT], w->edit.plain ? TR(l, "Off", "Nu") : TR(l, "On", "Da"), p->text);
    terminal_text(w->frame, 136, row_y[ROW_SCREEN], w->edit.inverted ? TR(l, "Inverted", "Inversat") :
                  TR(l, "Normal", "Normal"), p->text);
    terminal_text(w->frame, 136, row_y[ROW_RESTART], "Enter", p->dim);
    for (unsigned i = 0; i < 14; ++i) {
        int sx = 136 + (int)i * 25, sy = row_y[ROW_SWITCHES] - 2;
        bool up = (w->edit.switches & DAF_SWITCH(i + 1)) != 0;
        bool selected = w->row == ROW_SWITCHES && i == w->switch_index;
        terminal_rect(w->frame, sx, sy, 10, 12, selected ? p->text : p->dim);
        terminal_rect(w->frame, sx + 2, up ? sy + 2 : sy + 6, 6, 4, up ? p->lit : p->off);
        char number[4];
        snprintf(number, sizeof(number), "%u", i + 1);
        terminal_text(w->frame, i + 1 >= 10 ? sx - 3 : sx + 1, sy + 14, number, selected ? p->text : p->dim);
    }
    char hint[64], line[64];
    terminal_text(w->frame, 16, 198, row_hint(w, hint, sizeof(hint)), p->text);
    DafLine summary;
    daf_line_from_switches(&summary, w->edit.switches);
    snprintf(line, sizeof(line), "%u %u%c%u  %s", summary.tx_baud, summary.data_bits,
             "NEO"[summary.parity], summary.stop_bits, summary.online ? "ON LINE" : "LOCAL");
    terminal_text(w->frame, 16, 212, line, p->dim);
    if (w->editing) {
        terminal_text(w->frame, 16, 228, TR(l, "Type the address  Ctrl+A clear  Ctrl+V paste",
                                               "Se scrie adresa  Ctrl+A sterge  Ctrl+V lipire"), p->dim);
        terminal_text(w->frame, 16, 242, TR(l, "Enter accept  Esc cancel", "Enter accepta  Esc renunta"), p->dim);
    } else {
        terminal_text(w->frame, 16, 228, TR(l, "Up/Down row  Left/Right switch", "Sus/Jos rind  Stinga/Dreapta comutator"), p->dim);
        terminal_text(w->frame, 16, 242, TR(l, "Enter select  Esc or F12 leave", "Enter alege  Esc sau F12 iesire"), p->dim);
    }
    if (w->error[0])
        terminal_text(w->frame, 16, 256, w->error, p->warn);
    if (w->popup_open) {
        int height = (int)w->popup_count * 14 + 12, top = 120 - height / 2;
        terminal_rect(w->frame, 166, top - 2, 200, height + 4, p->dim);
        terminal_rect(w->frame, 168, top, 196, height, p->panel);
        for (unsigned i = 0; i < w->popup_count; ++i) {
            int y = top + 6 + (int)i * 14;
            if (i == w->popup_index)
                terminal_rect(w->frame, 170, y - 3, 192, 14, p->select);
            terminal_text(w->frame, 176, y, w->popup[i], i == w->popup_index ? p->text : p->dim);
        }
    }
}

static void play(Window *w)
{
    Daf *d = &w->terminal.display;
    if (!w->audio) {
        d->bell_ms = d->clicks = 0;
        return;
    }
    unsigned bell = d->bell_ms, clicks = d->clicks;
    d->bell_ms = d->clicks = 0;
    if (!bell && !clicks)
        return;
    /* The speaker: a square-wave bell and short clicks. */
    unsigned samples = bell ? AUDIO_RATE * bell / 1000 : AUDIO_RATE / 250;
    int16_t *wave = calloc(samples, sizeof(*wave));
    if (!wave)
        return;
    for (unsigned i = 0; i < samples; ++i)
        wave[i] = (int16_t)(((i * 2 * 1000 / AUDIO_RATE) & 1) ? 3000 : -3000);
    if (SDL_GetQueuedAudioSize(w->audio) < AUDIO_RATE)
        SDL_QueueAudio(w->audio, wave, samples * (unsigned)sizeof(*wave));
    free(wave);
}

static void run_port(Window *w)
{
    serial_link_pump(&w->link);
    for (unsigned i = 0; i < STEPS; ++i) {
        w->clock += RS232_TICK;
        bool txd = daf_serial_txd(&w->terminal, w->clock);
        bool rxd = serial_link_rx(&w->link, w->clock, txd, true);
        daf_serial_step(&w->terminal, w->clock, rxd);
        sync_link(w);
        serial_link_tx(&w->link, w->clock, daf_serial_txd(&w->terminal, w->clock));
    }
}

static void status_line(Window *w, const Options *o, char *text, size_t size)
{
    const Daf *d = &w->terminal.display;
    Language l = w->settings.lang;
    const char *mode = !d->vt_mode ? "TEK" : d->vt.ansi ? "VT100" : "VT52";
    bool caps = !d->vt_mode || (SDL_GetModState() & KMOD_CAPS);
    const char *link = o->demo ? "DEMO" : o->replay ? "REPLAY" : w->link.dropped ? TR(l, "OVERRUN", "DEPASIRE") :
        w->link.stream && !w->link.connected ? TR(l, "WAIT", "ASTEPTARE") : "";
    snprintf(text, size, "%s%s%s %s %u %u%c%u %s%s", d->line.online ? "LINE " : "LOCAL", d->vt_mode ? " KB" : "",
             caps ? " CAPS" : "", mode, d->line.tx_baud, d->line.data_bits, "NEO"[d->line.parity],
             d->line.stop_bits, link, o->demo || o->replay ? "" : TR(l, "  F12 SETTINGS", "  F12 SETARI"));
}

static int window_loop(Window *w, const Options *o)
{
    bool open = true;
    uint64_t next = rc_host_now_us();
    unsigned frames = 0;
    SDL_StartTextInput();
    while (open) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT)
                open = false;
            else if (e.type == SDL_KEYDOWN)
                key(w, o, e.key);
            else if (e.type == SDL_KEYUP)
                w->keypad_text = 0;
            else if (e.type == SDL_TEXTINPUT) {
                if (w->setup && !(SDL_GetModState() & (KMOD_CTRL | KMOD_ALT)))
                    settings_text(w, e.text.text);
                else if (!w->setup && !w->drop_text)
                    terminal_text_input(&w->terminal.display, w->keypad_text, e.text.text);
                w->drop_text = false;
            }
        }
        if (!open)
            break;
        run_port(w);
        play(w);
        ++frames;
        bool end = o->frames && frames == o->frames;
        if (end && o->screenshot)
            mx_video_request_shot(w->video, o->screenshot, !w->shader);

        uint64_t now = o->hidden ? 0 : SDL_GetTicks64();
        terminal_draw(&w->terminal.display, w->frame, now, &w->palette);
        if (w->setup)
            render_settings(w);
        char status[96];
        status_line(w, o, status, sizeof(status));
        terminal_text(w->frame, 0, DAF_HEIGHT + 3, status, w->palette.status);
        mx_video_present(w->video, w->frame, TERMINAL_FRAME_W, TERMINAL_FRAME_H);
        if (end)
            break;
        if (!o->hidden || w->link.stream) {
            next += 1000000u / HOST_HZ;
            uint64_t t = rc_host_now_us();
            if (next > t)
                rc_host_sleep_us((uint32_t)(next - t));
            else if (t - next > 50000)
                next = t;
        }
    }
    SDL_StopTextInput();
    return 0;
}

static SDL_AudioDeviceID open_audio(void)
{
    if (SDL_InitSubSystem(SDL_INIT_AUDIO))
        return 0;
    SDL_AudioSpec want = {0}, have;
    want.freq = AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 512;
    SDL_AudioDeviceID device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (device)
        SDL_PauseAudioDevice(device, 0);
    return device;
}

int main(int argc, char **argv)
{
    SDL_SetMainReady();
    if (argc == 2 && !strcmp(argv[1], "--version")) {
        puts(RECIF_VERSION);
        return 0;
    }
    Options o = {0};
    bool help = argc == 2 && !strcmp(argv[1], "--help");
    bool args_ok = !help && parse(&o, argc, argv);
    Window *w = calloc(1, sizeof(*w));
    if (!w)
        return 1;
    if (!o.hidden)
        prefs_open(&w->prefs, "daf2020.yaml");
    /* 2.11BSD's console: 7E1 (S4, S5 on, S7 off) and a VT100. */
    static const char *const bsd[] = {"serial", "tcp:127.0.0.1:2323", "switches", "11011001000000",
                                      "mode", "vt100", NULL};
    if (!o.hidden)
        prefs_first_start(&w->prefs, bsd);
    w->settings.lang = lang_choose(prefs_get(&w->prefs, "language", NULL));
    Language l = w->settings.lang;
    if (!args_ok) {
        const char *message = TR(l,
            "DAF-2020 | Recif " RECIF_VERSION "\n"
            "daf2020 [--serial ENDPOINT | --demo | --replay FILE] [--serial-format 9600,8N1]\n"
            "        [--local-echo] [--plain] [--setup]\n"
            "F12 opens the settings: language, mode, connection, keyboard switches and CRT effect.\n"
            "Serial endpoints: " SERIAL_ENDS,
            "DAF-2020 | Recif " RECIF_VERSION "\n"
            "daf2020 [--serial LEGATURA | --demo | --replay FISIER] [--serial-format 9600,8N1]\n"
            "        [--local-echo] [--plain] [--setup]\n"
            "F12 deschide setarile: limba, modul de lucru, legatura, comutatoarele si efectul CRT.\n"
            "Legaturi seriale: " SERIAL_ENDS);
        fprintf(help ? stdout : stderr, "%s\n", message);
#ifdef _WIN32
        /* A window program has no console of its own to print to. */
        if (!o.hidden)
            SDL_ShowSimpleMessageBox(help ? SDL_MESSAGEBOX_INFORMATION : SDL_MESSAGEBOX_ERROR, "DAF-2020", message, NULL);
#endif
        free(w);
        return help ? 0 : 2;
    }

    char error[512] = "";
    int result = 1;
    split_endpoint(o.endpoint ? o.endpoint : prefs_get(&w->prefs, "serial", "none"), &w->settings);
    if (o.demo || o.replay)
        split_endpoint("none", &w->settings);
    w->settings.switches = parse_switches(prefs_get(&w->prefs, "switches", ""), DAF_DEFAULT_SWITCHES);
    if (o.format_set)
        w->settings.switches = switches_for(w->settings.switches, &o.format);
    if (o.echo)
        w->settings.switches |= (uint16_t)DAF_SWITCH(11);
    w->settings.plain = o.plain || !strcmp(prefs_get(&w->prefs, "crt", "on"), "off");
    w->settings.inverted = !strcmp(prefs_get(&w->prefs, "screen", "normal"), "inverted");
    daf_serial_reset(&w->terminal, w->settings.switches);
    w->settings.vt = !strcmp(prefs_get(&w->prefs, "mode", "tektronix"), "vt100");
    daf_set_mode(&w->terminal.display, w->settings.vt);
    Rs232Format format = daf_serial_format(&w->terminal.display.line, false);
    char endpoint[ENDPOINT_SIZE];
    join_endpoint(&w->settings, endpoint, sizeof(endpoint));
    if (!serial_link_open(&w->link, endpoint, &format, error, sizeof(error)))
        goto done;
    w->link_changes = w->terminal.display.line_changes - 1;
    sync_link(w);
    if (o.demo) {
        daf_demo(&w->terminal.display);
    } else if (o.replay && !terminal_replay(&w->terminal.display, o.replay, error, sizeof(error))) {
        goto done;
    }

    mx_settings settings = {0};
    settings.aspect = MX_ASPECT_ORIGINAL;
    settings.window_scale = 2;
    settings.filter = MX_FILTER_NEAREST;
    settings.crt = true;
    settings.vsync = !o.hidden;
    w->video = mx_video_open("DAF-2020  |  Recif " RECIF_VERSION, &settings, TERMINAL_FRAME_W, TERMINAL_FRAME_H,
                             4, 3, o.hidden, error, sizeof(error));
    if (w->video) {
#ifndef _WIN32                              /* on Windows the window has the program file's own icon */
        if (!o.hidden)
            mx_video_set_icon(w->video, recif_icon_pixels, RECIF_ICON_SIZE);
#endif
        set_look(w, w->settings.plain);
        if (!mx_video_crt_available(w->video))
            w->settings.plain = true;
        if (!o.hidden)
            w->audio = open_audio();
        if (o.setup)
            open_settings(w);
        result = window_loop(w, &o);
    }

done:
    if (result && !o.hidden)
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "DAF-2020", error, NULL);
    if (w->audio)
        SDL_CloseAudioDevice(w->audio);
    if (w->video)
        mx_video_close(w->video);
    serial_link_close(&w->link);
    free(w);
    return result;
}
