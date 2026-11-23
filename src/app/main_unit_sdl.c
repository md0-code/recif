/* The main unit's panel where the Windows one is not available: the same
 * settings, controls and settings file, drawn with the terminal's 8x8 font
 * into an SDL window and worked with the keyboard or the mouse. */

#include "main_unit.h"

#include "font8x8.h"
#include "icon_data.h"
#include "lang.h"
#include "machine_session.h"
#include "prefs.h"
#include "rc_version.h"
#include "video.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

enum {
    FRAME_W = 656, FRAME_H = 446, LABEL_X = 16, VALUE_X = 216, VALUE_CHARS = 53, FILE_CHARS = 44,
    BOOT_CHOICES = 5, POPUP_ITEMS = 8, LIST_ITEMS = 512, LIST_ROWS = 20, LIST_Y = 78, LOOP_US = 10000,
    TEXT_SIZE = RC_HOST_PATH + 16
};
enum {
    R_MODEL, R_LANGUAGE, R_FPU, R_MEMORY, R_CLOCK, R_TM11, R_SERIAL, R_FORMAT, R_TAPE, R_DISKS, R_DISK0,
    R_DISK1, R_BOOT, R_SPEED, R_BUTTONS, ROWS
};
enum { B_POWER, B_RESTART, B_PAUSE, B_STOP, BUTTONS };
enum { KIND_CHOICE, KIND_TOGGLE, KIND_TEXT, KIND_FILE, KIND_BUTTONS };

static const int row_y[ROWS] = {46, 60, 192, 206, 220, 234, 248, 262, 276, 290, 304, 318, 332, 346, 366};
static const unsigned clock_rates[] = {50, 60, 0};

/* Colours: a dark panel, the terminal's phosphor green for headings. */
static const uint32_t C_BACK = 0xff1d2023, C_LINE = 0xff3a4046, C_SELECT = 0xff2c4a63, C_TEXT = 0xffe6e8ea,
                      C_DIM = 0xff9aa1a8, C_OFF = 0xff5d6369, C_HEAD = 0xff53d568, C_WARN = 0xffffbb90,
                      C_BOX = 0xff262a2e;

typedef struct {
    StartMode mode;
    unsigned option;
} BootChoice;

typedef struct {
    char name[256];
    bool dir;
} Entry;

typedef struct {
    bool hidden, start;
    unsigned frames;
    const char *screenshot;
} Options;

typedef struct {
    mx_video *video;
    uint32_t frame[FRAME_W * FRAME_H];
    Language lang;
    Prefs prefs;
    MachineSession session;
    unsigned model, clock, disks, boot, boot_count, row, button;
    bool fpu, tm11, full, powered, paused;
    char memory[8], serial[TEXT_SIZE], format[32], tape[RC_HOST_PATH], disk[MACHINE_DISK_UNITS][RC_HOST_PATH];
    BootChoice boots[BOOT_CHOICES];
    /* A pop-up list of a choice row. */
    bool popup_open;
    unsigned popup_count, popup_index;
    char popup[POPUP_ITEMS][48];
    /* A text row being edited, or a file name being typed in the picker. */
    bool editing;
    char edit[TEXT_SIZE];
    /* The file picker of a file row. */
    bool picking, naming;
    unsigned pick_row, entry_count, entry_index, entry_top;
    char dir[RC_HOST_PATH];
    Entry entries[LIST_ITEMS];
    char message[512];
    uint64_t last_tick;
} Panel;

static const char *t(const Panel *p, const char *en, const char *ro)
{
    return TR(p->lang, en, ro);
}

/* --- drawing ------------------------------------------------------------------------------------------- */

/* The next character of UTF-8 text as the font draws it: Romanian letters
 * without their diacritics, anything else outside ASCII as '?'. */
static char next_char(const char **text)
{
    const unsigned char *s = (const unsigned char *)*text;
    if (s[0] < 0x80) {
        *text += 1;
        return (char)s[0];
    }
    unsigned length = s[0] >= 0xf0 ? 4 : s[0] >= 0xe0 ? 3 : 2;
    for (unsigned i = 1; i < length; ++i)
        if (!s[i]) {
            *text += i;
            return '?';
        }
    *text += length;
    if (length != 2)
        return '?';
    unsigned code = ((s[0] & 0x1fu) << 6) | (s[1] & 0x3fu);
    switch (code) {
    case 0x103: case 0xe2: return 'a';
    case 0x102: case 0xc2: return 'A';
    case 0xee: return 'i';
    case 0xce: return 'I';
    case 0x219: case 0x15f: return 's';
    case 0x218: case 0x15e: return 'S';
    case 0x21b: case 0x163: return 't';
    case 0x21a: case 0x162: return 'T';
    default: return '?';
    }
}

static size_t text_chars(const char *text)
{
    size_t n = 0;
    while (*text) {
        next_char(&text);
        ++n;
    }
    return n;
}

static void rect(Panel *p, int x, int y, int w, int h, uint32_t color)
{
    for (int j = y < 0 ? 0 : y; j < y + h && j < FRAME_H; ++j)
        for (int i = x < 0 ? 0 : x; i < x + w && i < FRAME_W; ++i)
            p->frame[(size_t)j * FRAME_W + (size_t)i] = color;
}

/* At most `limit` characters (0: to the frame's edge). */
static void text(Panel *p, int x, int y, const char *s, uint32_t color, size_t limit)
{
    if (y < 0 || y + 8 > FRAME_H)
        return;
    for (size_t n = 0; *s && x <= FRAME_W - 8 && (!limit || n < limit); ++n, x += 8) {
        unsigned ch = (unsigned char)next_char(&s);
        if (ch < 32 || ch > 126)
            ch = '?';
        const uint8_t *bitmap = mx_font8x8_ascii[ch - 32];
        for (int j = 0; j < 8; ++j)
            for (int i = 0; i < 8; ++i)
                if (bitmap[j] & (128 >> i))
                    p->frame[(size_t)(y + j) * FRAME_W + (size_t)(x + i)] = color;
    }
}

/* A path in `chars` characters: its end, after "...", when it is longer. */
static void fitted(Panel *p, int x, int y, const char *s, uint32_t color, size_t chars)
{
    size_t n = text_chars(s);
    if (n <= chars) {
        text(p, x, y, s, color, chars);
        return;
    }
    text(p, x, y, "...", C_DIM, 3);
    for (size_t skip = n - (chars - 3); skip; --skip)
        next_char(&s);
    text(p, x + 24, y, s, color, chars - 3);
}

/* Up to `lines` lines of `width` characters, broken at spaces. */
static void wrapped(Panel *p, int x, int y, const char *s, uint32_t color, size_t width, int lines)
{
    while (*s && lines-- > 0) {
        const char *end = s, *cut = NULL;
        size_t n = 0;
        while (*end && n < width) {
            if (*end == ' ')
                cut = end;
            next_char(&end);
            ++n;
        }
        if (*end && cut && lines > 0)
            end = cut;
        char line[400];
        size_t size = (size_t)(end - s) < sizeof(line) - 1 ? (size_t)(end - s) : sizeof(line) - 1;
        memcpy(line, s, size);
        line[size] = '\0';
        text(p, x, y, line, color, width);
        s = *end == ' ' ? end + 1 : end;
        y += 12;
    }
}

/* --- the settings -------------------------------------------------------------------------------------- */

static const CoralProfile *profile(const Panel *p)
{
    return &coral_profiles[p->model < CORAL_MODEL_COUNT ? p->model : CORAL_MODEL_COUNT - 1];
}

static bool fpu_option(const Panel *p)
{
    return pdp_fpu_option(profile(p)->cpu);
}

static int kind(unsigned row)
{
    switch (row) {
    case R_FPU: case R_TM11: case R_SPEED: return KIND_TOGGLE;
    case R_MEMORY: case R_SERIAL: case R_FORMAT: return KIND_TEXT;
    case R_TAPE: case R_DISK0: case R_DISK1: return KIND_FILE;
    case R_BUTTONS: return KIND_BUTTONS;
    default: return KIND_CHOICE;
    }
}

static char *file_value(Panel *p, unsigned row)
{
    return row == R_TAPE ? p->tape : p->disk[row == R_DISK1];
}

static bool button_enabled(const Panel *p, unsigned button)
{
    return button == B_POWER ? !p->powered : p->powered;
}

static bool enabled(const Panel *p, unsigned row)
{
    switch (row) {
    case R_MODEL: case R_MEMORY: case R_CLOCK: case R_TM11: case R_DISKS: case R_SERIAL: case R_FORMAT:
        return !p->powered;
    case R_FPU: return !p->powered && fpu_option(p);
    case R_TAPE: return p->tm11;
    case R_DISK0: case R_DISK1: return p->disks != DISKS_NONE;
    case R_BOOT: return p->boot_count > 1;
    default: return true;
    }
}

/* The boot list offers only devices that are fitted and loaded. */
static void fill_boot(Panel *p)
{
    BootChoice previous = p->boot < p->boot_count ? p->boots[p->boot] : (BootChoice){START_MONITOR, 0};
    p->boot_count = 0;
    p->boots[p->boot_count++] = (BootChoice){START_MONITOR, 0};
    if (p->tm11 && *p->tape) {
        p->boots[p->boot_count++] = (BootChoice){START_TAPE, 0};
        p->boots[p->boot_count++] = (BootChoice){START_TAPE, 1};
    }
    for (unsigned u = 0; u < MACHINE_DISK_UNITS; ++u)
        if (p->disks != DISKS_NONE && *p->disk[u])
            p->boots[p->boot_count++] = (BootChoice){START_DISK, u};
    p->boot = 0;
    for (unsigned i = 0; i < p->boot_count; ++i)
        if (p->boots[i].mode == previous.mode && p->boots[i].option == previous.option)
            p->boot = i;
}

static const char *boot_label(const Panel *p, BootChoice b)
{
    switch (b.mode) {
    case START_TAPE: return b.option ? t(p, "First tape record", "Prima înregistrare de pe bandă")
                                     : t(p, "Labelled tape bootstrap", "Încărcare de pe banda etichetată");
    case START_DISK: return b.option ? t(p, "Disk unit 1", "Discul unității 1") : t(p, "Disk unit 0", "Discul unității 0");
    default: return t(p, "Diagnostic monitor", "Monitor de diagnosticare");
    }
}

static const char *disks_label(const Panel *p, unsigned disks)
{
    switch (disks) {
    case DISKS_RM03: return t(p, "RH11 with RM03 (67 MB)", "RH11 cu RM03 (67 MB)");
    case DISKS_RP06: return t(p, "RH11 with RP06 (174 MB)", "RH11 cu RP06 (174 MB)");
    case DISKS_RP03: return t(p, "RP11-C with RP03 (40 MB)", "RP11-C cu RP03 (40 MB)");
    default: return t(p, "None", "Fără");
    }
}

static const char *clock_label(const Panel *p, unsigned clock)
{
    return clock == 0 ? "50 Hz" : clock == 1 ? "60 Hz" : t(p, "Off", "Oprit");
}

static void set_profile(Panel *p)
{
    const CoralProfile *model = profile(p);
    p->tm11 = model->tape_reported;
    p->fpu = model->fpu_reported && fpu_option(p);
    p->disks = model->disks;
    snprintf(p->memory, sizeof(p->memory), "%u", model->nominal_kib);
    fill_boot(p);
}

static void save_prefs(Panel *p)
{
    char value[32];
    prefs_set(&p->prefs, "language", lang_name(p->lang));
    prefs_set(&p->prefs, "model", profile(p)->id);
    prefs_set(&p->prefs, "ram", p->memory);
    prefs_set(&p->prefs, "serial", p->serial);
    prefs_set(&p->prefs, "format", p->format);
    prefs_set(&p->prefs, "tape", p->tape);
    prefs_set(&p->prefs, "disk0", p->disk[0]);
    prefs_set(&p->prefs, "disk1", p->disk[1]);
    snprintf(value, sizeof(value), "%u", clock_rates[p->clock % 3]);
    prefs_set(&p->prefs, "clock", value);
    prefs_set(&p->prefs, "fpu", p->fpu ? "on" : "off");
    prefs_set(&p->prefs, "tm11", p->tm11 ? "on" : "off");
    prefs_set(&p->prefs, "speed", p->full ? "full" : "real");
    prefs_set(&p->prefs, "disks", disk_system_name((DiskSystem)p->disks));
    BootChoice b = p->boot < p->boot_count ? p->boots[p->boot] : (BootChoice){START_MONITOR, 0};
    snprintf(value, sizeof(value), "%u,%u", (unsigned)b.mode, b.option);
    prefs_set(&p->prefs, "boot", value);
    prefs_save(&p->prefs);
}

static void copy(char *out, size_t size, const char *value)
{
    if (value)
        snprintf(out, size, "%s", value);
}

static void load_prefs(Panel *p)
{
    const CoralProfile *model = coral_find(prefs_get(&p->prefs, "model", "4030"));
    p->model = model ? (unsigned)(model - coral_profiles) : CORAL_MODEL_COUNT - 1;
    set_profile(p);
    copy(p->memory, sizeof(p->memory), prefs_get(&p->prefs, "ram", NULL));
    copy(p->serial, sizeof(p->serial), prefs_get(&p->prefs, "serial", NULL));
    copy(p->format, sizeof(p->format), prefs_get(&p->prefs, "format", NULL));
    copy(p->tape, sizeof(p->tape), prefs_get(&p->prefs, "tape", NULL));
    copy(p->disk[0], sizeof(p->disk[0]), prefs_get(&p->prefs, "disk0", NULL));
    copy(p->disk[1], sizeof(p->disk[1]), prefs_get(&p->prefs, "disk1", NULL));
    unsigned clock = (unsigned)atoi(prefs_get(&p->prefs, "clock", "50"));
    p->clock = clock == 60 ? 1 : clock ? 0 : 2;
    const char *fpu = prefs_get(&p->prefs, "fpu", NULL), *tm11 = prefs_get(&p->prefs, "tm11", NULL);
    if (fpu)
        p->fpu = !strcmp(fpu, "on") && fpu_option(p);
    if (tm11)
        p->tm11 = !strcmp(tm11, "on");
    p->full = !strcmp(prefs_get(&p->prefs, "speed", "real"), "full");
    DiskSystem saved;
    if (disk_system_parse(prefs_get(&p->prefs, "disks", ""), &saved))
        p->disks = saved;
    unsigned mode = 0, option = 0;
    sscanf(prefs_get(&p->prefs, "boot", "0,0"), "%u,%u", &mode, &option);
    p->boot = 0;
    fill_boot(p);
    for (unsigned i = 0; i < p->boot_count; ++i)
        if ((unsigned)p->boots[i].mode == mode && p->boots[i].option == option)
            p->boot = i;
}

/* --- the machine --------------------------------------------------------------------------------------- */

static bool start_guest(Panel *p, char *error, size_t size)
{
    BootChoice b = p->boot < p->boot_count ? p->boots[p->boot] : (BootChoice){START_MONITOR, 0};
    return session_start(&p->session, b.mode, NULL, 0, 0, 0, b.option, error, size);
}

static void power_on(Panel *p)
{
    MachineConfig c = machine_defaults(profile(p));
    char error[512], full[RC_HOST_PATH];
    if (!read_unsigned(p->memory, 10, c.profile->capacity_kib, &c.memory_kib) || !machine_ram_kib(&c)) {
        copy(p->message, sizeof(p->message), t(p, "Choose a RAM size in the displayed range.",
                                               "Se alege o memorie RAM din intervalul afișat."));
        return;
    }
    if (!rs232_parse_format(p->format, &c.console)) {
        copy(p->message, sizeof(p->message), t(p, "Enter a serial format such as 9600,8N1.",
                                               "Se introduce un format serial, de exemplu 9600,8N1."));
        return;
    }
    c.clock_hz = clock_rates[p->clock % 3];
    c.tm11 = p->tm11;
    c.fpu = p->fpu;
    c.disks = (DiskSystem)p->disks;
    if (!session_open(&p->session, &c, *p->serial ? p->serial : NULL, error, sizeof(error))) {
        copy(p->message, sizeof(p->message), error);
        return;
    }
    bool ok = true;
    if (c.tm11 && *p->tape)
        ok = session_mount(&p->session, prefs_beside_program(p->tape, full, sizeof(full)), NULL, error, sizeof(error));
    for (unsigned u = 0; ok && c.disks != DISKS_NONE && u < MACHINE_DISK_UNITS; ++u)
        if (*p->disk[u])
            ok = session_mount_disk(&p->session, u, prefs_beside_program(p->disk[u], full, sizeof(full)), error,
                                    sizeof(error));
    if (!ok || !start_guest(p, error, sizeof(error))) {
        session_close(&p->session);
        copy(p->message, sizeof(p->message), error);
        return;
    }
    p->powered = true;
    p->paused = false;
    p->button = B_STOP;
    p->last_tick = rc_host_now_us();
    p->message[0] = '\0';
    save_prefs(p);
}

static void press(Panel *p, unsigned button)
{
    char error[512];
    if (!button_enabled(p, button))
        return;
    switch (button) {
    case B_POWER: power_on(p); break;
    case B_RESTART:
        if (!start_guest(p, error, sizeof(error)))
            copy(p->message, sizeof(p->message), error);
        else
            p->paused = false;
        break;
    case B_PAUSE: p->paused = !p->paused; break;
    default:
        session_close(&p->session);
        p->powered = p->paused = false;
        p->button = B_POWER;
        break;
    }
}

/* A new tape or disk; the running machine takes it at once. */
static void change_file(Panel *p, unsigned row, const char *path)
{
    char error[512], full[RC_HOST_PATH];
    if (!enabled(p, row))
        return;
    if (p->powered) {
        bool ok = true;
        if (row == R_TAPE && !*path)
            session_eject(&p->session);
        else if (row == R_TAPE)
            ok = session_mount(&p->session, prefs_beside_program(path, full, sizeof(full)), NULL, error, sizeof(error));
        else if (!*path)
            session_eject_disk(&p->session, row == R_DISK1);
        else
            ok = session_mount_disk(&p->session, row == R_DISK1, prefs_beside_program(path, full, sizeof(full)),
                                    error, sizeof(error));
        if (!ok) {
            copy(p->message, sizeof(p->message), error);
            return;
        }
    }
    copy(file_value(p, row), RC_HOST_PATH, path);
    fill_boot(p);
}

static void run(Panel *p)
{
    if (!p->powered)
        return;
    uint64_t now = rc_host_now_us(), elapsed = now - p->last_tick;
    p->last_tick = now;
    if (elapsed > 50000)
        elapsed = 50000;
    if (p->paused)
        return;
    if (p->full && !p->session.machine.cpu.waiting) {
        /* As many instructions as fit in most of the loop period, while the
         * guest computes; real time while it idles. */
        while (rc_host_now_us() - now < LOOP_US * 8 / 10 && !p->session.machine.cpu.waiting &&
               session_run(&p->session, 20000, NULL))
            ;
    } else {
        session_run(&p->session, (unsigned)(elapsed * (RS232_HZ / RS232_TICK) / 1000000), NULL);
    }
}

/* --- choices, text and files --------------------------------------------------------------------------- */

static unsigned choice_count(const Panel *p, unsigned row)
{
    switch (row) {
    case R_MODEL: return CORAL_MODEL_COUNT;
    case R_LANGUAGE: return 2;
    case R_CLOCK: return 3;
    case R_DISKS: return DISKS_COUNT;
    case R_BOOT: return p->boot_count;
    default: return 0;
    }
}

static unsigned choice_value(const Panel *p, unsigned row)
{
    switch (row) {
    case R_MODEL: return p->model;
    case R_LANGUAGE: return p->lang;
    case R_CLOCK: return p->clock;
    case R_DISKS: return p->disks;
    default: return p->boot;
    }
}

static const char *choice_label(const Panel *p, unsigned row, unsigned i)
{
    switch (row) {
    case R_MODEL: return coral_profiles[i].name;
    case R_LANGUAGE: return i ? "Română" : "English";
    case R_CLOCK: return clock_label(p, i);
    case R_DISKS: return disks_label(p, i);
    default: return i < p->boot_count ? boot_label(p, p->boots[i]) : "";
    }
}

static void set_choice(Panel *p, unsigned row, unsigned i)
{
    switch (row) {
    case R_MODEL:
        p->model = i;
        set_profile(p);
        break;
    case R_LANGUAGE: p->lang = i ? LANG_RO : LANG_EN; break;
    case R_CLOCK: p->clock = i; break;
    case R_DISKS:
        p->disks = i;
        fill_boot(p);
        break;
    default: p->boot = i; break;
    }
}

static void toggle(Panel *p, unsigned row)
{
    if (row == R_FPU)
        p->fpu = !p->fpu;
    else if (row == R_TM11) {
        p->tm11 = !p->tm11;
        fill_boot(p);
    } else
        p->full = !p->full;
}

static char *text_value(Panel *p, unsigned row, size_t *size)
{
    switch (row) {
    case R_MEMORY: *size = sizeof(p->memory); return p->memory;
    case R_SERIAL: *size = sizeof(p->serial); return p->serial;
    default: *size = sizeof(p->format); return p->format;
    }
}

static void type_text(Panel *p, const char *input)
{
    size_t length = strlen(p->edit), limit = sizeof(p->edit);
    if (!p->naming)
        text_value(p, p->row, &limit);
    if (!p->naming && p->row == R_MEMORY)
        limit = 5;
    for (; *input && length + 1 < limit; ++input) {
        unsigned char c = (unsigned char)*input;
        if (c < 32 || c == 127 || (!p->naming && p->row == R_MEMORY && (c < '0' || c > '9')))
            continue;
        p->edit[length++] = (char)c;
    }
    p->edit[length] = '\0';
}

static int compare_entries(const void *a, const void *b)
{
    const Entry *x = a, *y = b;
    if (x->dir != y->dir)
        return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static bool wanted(unsigned row, const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot)
        return false;
    if (!strcasecmp(dot, ".img"))
        return true;
    return row == R_TAPE ? !strcasecmp(dot, ".tap") || !strcasecmp(dot, ".dat") : !strcasecmp(dot, ".dsk");
}

/* The picker's list: a line for typing a name, the parent folder, the
 * folders, then the images of the row's kind. */
static void list_folder(Panel *p)
{
    p->entry_count = p->entry_index = p->entry_top = 0;
    p->entries[p->entry_count++] = (Entry){"", false};
    if (strcmp(p->dir, "/"))
        p->entries[p->entry_count++] = (Entry){"..", true};
    DIR *d = opendir(p->dir);
    if (!d)
        return;
    unsigned first = p->entry_count;
    struct dirent *e;
    while ((e = readdir(d)) && p->entry_count < LIST_ITEMS) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof(p->entries[0].name))
            continue;
        char path[RC_HOST_PATH * 2];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", p->dir, e->d_name);
        if (stat(path, &st))
            continue;
        bool dir = S_ISDIR(st.st_mode);
        if (!dir && !wanted(p->pick_row, e->d_name))
            continue;
        Entry *entry = &p->entries[p->entry_count++];
        snprintf(entry->name, sizeof(entry->name), "%s", e->d_name);
        entry->dir = dir;
    }
    closedir(d);
    qsort(p->entries + first, p->entry_count - first, sizeof(Entry), compare_entries);
}

static void open_picker(Panel *p, unsigned row)
{
    char full[RC_HOST_PATH];
    const char *current = file_value(p, row);
    if (*current) {
        snprintf(p->dir, sizeof(p->dir), "%s", prefs_beside_program(current, full, sizeof(full)));
        char *slash = strrchr(p->dir, '/');
        if (slash && slash != p->dir)
            *slash = '\0';
        else
            snprintf(p->dir, sizeof(p->dir), "/");
    } else {
        rc_host_program_dir(p->dir, sizeof(p->dir));
        size_t n = strlen(p->dir);
        if (n > 1 && p->dir[n - 1] == '/')
            p->dir[n - 1] = '\0';
    }
    struct stat st;
    if (stat(p->dir, &st) || !S_ISDIR(st.st_mode))
        snprintf(p->dir, sizeof(p->dir), "/");
    p->pick_row = row;
    p->picking = true;
    p->naming = false;
    list_folder(p);
}

static void enter_folder(Panel *p, const char *name)
{
    size_t n = strlen(p->dir);
    if (!strcmp(name, "..")) {
        char *slash = strrchr(p->dir, '/');
        if (slash && slash != p->dir)
            *slash = '\0';
        else
            snprintf(p->dir, sizeof(p->dir), "/");
    } else if (n + strlen(name) + 2 < sizeof(p->dir)) {
        snprintf(p->dir + n, sizeof(p->dir) - n, "%s%s", n && p->dir[n - 1] == '/' ? "" : "/", name);
    }
    list_folder(p);
}

static void pick(Panel *p, const char *name)
{
    char path[RC_HOST_PATH + TEXT_SIZE + 2];
    if (name[0] == '/')
        snprintf(path, sizeof(path), "%s", name);
    else
        snprintf(path, sizeof(path), "%s%s%s", p->dir, strcmp(p->dir, "/") ? "/" : "", name);
    if (strlen(path) >= RC_HOST_PATH)
        return;
    p->picking = p->naming = false;
    change_file(p, p->pick_row, path);
}

static void open_entry(Panel *p)
{
    if (p->entry_index >= p->entry_count)
        return;
    const Entry *e = &p->entries[p->entry_index];
    if (p->entry_index == 0) {
        /* A new file name creates an empty pack. */
        p->naming = true;
        p->edit[0] = '\0';
    } else if (e->dir) {
        enter_folder(p, e->name);
    } else {
        pick(p, e->name);
    }
}

static void keep_visible(Panel *p)
{
    if (p->entry_index < p->entry_top)
        p->entry_top = p->entry_index;
    else if (p->entry_index >= p->entry_top + LIST_ROWS)
        p->entry_top = p->entry_index - LIST_ROWS + 1;
}

static void open_popup(Panel *p)
{
    p->popup_count = choice_count(p, p->row);
    if (p->popup_count > POPUP_ITEMS)
        p->popup_count = POPUP_ITEMS;
    for (unsigned i = 0; i < p->popup_count; ++i)
        snprintf(p->popup[i], sizeof(p->popup[i]), "%s", choice_label(p, p->row, i));
    p->popup_index = choice_value(p, p->row);
    p->popup_open = p->popup_count > 0;
}

static void commit_edit(Panel *p)
{
    size_t size;
    char *value = text_value(p, p->row, &size);
    snprintf(value, size, "%s", p->edit);
    p->editing = false;
}

/* Enter, a click or Space on the selected row. */
static void activate(Panel *p)
{
    p->message[0] = '\0';
    if (p->row == R_BUTTONS) {
        press(p, p->button);
        return;
    }
    if (!enabled(p, p->row))
        return;
    switch (kind(p->row)) {
    case KIND_TOGGLE: toggle(p, p->row); break;
    case KIND_TEXT: {
        size_t size;
        snprintf(p->edit, sizeof(p->edit), "%s", text_value(p, p->row, &size));
        p->editing = true;
        break;
    }
    case KIND_FILE: open_picker(p, p->row); break;
    default: open_popup(p); break;
    }
}

/* --- input --------------------------------------------------------------------------------------------- */

static void edit_key(Panel *p, SDL_Keysym k)
{
    bool ctrl = (k.mod & KMOD_CTRL) != 0;
    if (k.sym == SDLK_RETURN || k.sym == SDLK_KP_ENTER) {
        if (p->naming) {
            if (*p->edit)
                pick(p, p->edit);
        } else {
            commit_edit(p);
        }
    } else if (k.sym == SDLK_ESCAPE) {
        p->editing = p->naming = false;
    } else if (k.sym == SDLK_BACKSPACE && *p->edit) {
        p->edit[strlen(p->edit) - 1] = '\0';
    } else if (ctrl && k.sym == SDLK_a) {
        p->edit[0] = '\0';
    } else if (ctrl && k.sym == SDLK_v) {
        char *clip = SDL_GetClipboardText();
        if (clip)
            type_text(p, clip);
        SDL_free(clip);
    }
}

static void picker_key(Panel *p, SDL_Keysym k)
{
    switch (k.sym) {
    case SDLK_UP: if (p->entry_index) --p->entry_index; break;
    case SDLK_DOWN: if (p->entry_index + 1 < p->entry_count) ++p->entry_index; break;
    case SDLK_PAGEUP: p->entry_index = p->entry_index > LIST_ROWS ? p->entry_index - LIST_ROWS : 0; break;
    case SDLK_PAGEDOWN:
        p->entry_index = p->entry_index + LIST_ROWS < p->entry_count ? p->entry_index + LIST_ROWS : p->entry_count - 1;
        break;
    case SDLK_HOME: p->entry_index = 0; break;
    case SDLK_END: p->entry_index = p->entry_count - 1; break;
    case SDLK_BACKSPACE: enter_folder(p, ".."); break;
    case SDLK_RETURN: case SDLK_KP_ENTER: open_entry(p); break;
    case SDLK_ESCAPE: p->picking = false; break;
    default: break;
    }
    keep_visible(p);
}

static void key(Panel *p, SDL_Keysym k)
{
    if (p->editing || p->naming) {
        edit_key(p, k);
        return;
    }
    if (p->picking) {
        picker_key(p, k);
        return;
    }
    if (p->popup_open) {
        if (k.sym == SDLK_UP)
            p->popup_index = (p->popup_index + p->popup_count - 1) % p->popup_count;
        else if (k.sym == SDLK_DOWN)
            p->popup_index = (p->popup_index + 1) % p->popup_count;
        else if (k.sym == SDLK_RETURN || k.sym == SDLK_KP_ENTER || k.sym == SDLK_SPACE) {
            p->popup_open = false;
            set_choice(p, p->row, p->popup_index);
        } else if (k.sym == SDLK_ESCAPE)
            p->popup_open = false;
        return;
    }
    switch (k.sym) {
    case SDLK_UP: case SDLK_DOWN: {
        int step = k.sym == SDLK_UP ? ROWS - 1 : 1;
        p->row = (p->row + (unsigned)step) % ROWS;
        break;
    }
    case SDLK_TAB:
        p->row = (p->row + ((k.mod & KMOD_SHIFT) ? ROWS - 1 : 1)) % ROWS;
        break;
    case SDLK_LEFT: case SDLK_RIGHT: {
        bool left = k.sym == SDLK_LEFT;
        if (p->row == R_BUTTONS) {
            p->button = (p->button + (left ? BUTTONS - 1 : 1)) % BUTTONS;
        } else if (enabled(p, p->row) && kind(p->row) == KIND_CHOICE) {
            unsigned n = choice_count(p, p->row);
            if (n)
                set_choice(p, p->row, (choice_value(p, p->row) + (left ? n - 1 : 1)) % n);
        } else if (enabled(p, p->row) && kind(p->row) == KIND_TOGGLE) {
            toggle(p, p->row);
        }
        break;
    }
    case SDLK_DELETE:
        if (kind(p->row) == KIND_FILE)
            change_file(p, p->row, "");
        break;
    case SDLK_SPACE:
        /* Its text would land in a field it opened. */
        if (kind(p->row) != KIND_TEXT && kind(p->row) != KIND_FILE)
            activate(p);
        break;
    case SDLK_RETURN: case SDLK_KP_ENTER: activate(p); break;
    default: break;
    }
}

static int popup_top(const Panel *p)
{
    int height = (int)p->popup_count * 14 + 8, top = row_y[p->row] + 12;
    return top + height > FRAME_H - 4 ? row_y[p->row] - 4 - height : top;
}

/* Where each button sits on the button row. */
static void button_box(const Panel *p, unsigned button, int *x, int *w);

static void click(Panel *p, int x, int y, unsigned clicks)
{
    if (p->editing || p->naming) {
        if (p->editing)
            commit_edit(p);
        else
            return;
    }
    if (p->picking) {
        int row = (y - (LIST_Y - 3)) / 14;
        if (x >= 24 && x < FRAME_W - 24 && y >= LIST_Y - 3 && row < LIST_ROWS &&
            p->entry_top + (unsigned)row < p->entry_count) {
            p->entry_index = p->entry_top + (unsigned)row;
            if (clicks >= 2 || p->entry_index == 0)
                open_entry(p);
        }
        return;
    }
    if (p->popup_open) {
        int top = popup_top(p), item = (y - top - 4) / 14;
        p->popup_open = false;
        if (x >= VALUE_X - 8 && x < VALUE_X + 248 && y >= top + 4 && item >= 0 && (unsigned)item < p->popup_count)
            set_choice(p, p->row, (unsigned)item);
        return;
    }
    for (unsigned row = 0; row < ROWS; ++row) {
        if (y < row_y[row] - 3 || y >= row_y[row] + 11)
            continue;
        p->row = row;
        if (row == R_BUTTONS) {
            for (unsigned b = 0; b < BUTTONS; ++b) {
                int bx, bw;
                button_box(p, b, &bx, &bw);
                if (x >= bx && x < bx + bw) {
                    p->button = b;
                    activate(p);
                }
            }
        } else if (kind(row) == KIND_FILE && x >= FRAME_W - 16 - 8 * 8) {
            if (*file_value(p, row))
                change_file(p, row, "");
        } else if (x >= LABEL_X) {
            activate(p);
        }
        return;
    }
}

/* --- the picture --------------------------------------------------------------------------------------- */

static const char *button_label(const Panel *p, unsigned button)
{
    switch (button) {
    case B_POWER: return t(p, "Start reference", "Pornire");
    case B_RESTART: return t(p, "Restart", "Repornire");
    case B_PAUSE: return p->paused ? t(p, "Resume", "Continuare") : t(p, "Pause", "Pauză");
    default: return t(p, "Stop", "Oprire");
    }
}

static void button_box(const Panel *p, unsigned button, int *x, int *w)
{
    int left = LABEL_X;
    for (unsigned b = 0; b <= button; ++b) {
        int width = (int)text_chars(button_label(p, b)) * 8 + 24;
        if (width < 96)
            width = 96;
        if (b == button) {
            *x = left;
            *w = width;
            return;
        }
        left += width + 10;
    }
}

static const char *row_label(const Panel *p, unsigned row)
{
    switch (row) {
    case R_MODEL: return t(p, "Model", "Model");
    case R_LANGUAGE: return t(p, "Language", "Limbă");
    case R_FPU: return t(p, "FP11 floating point", "Virgulă mobilă FP11");
    case R_MEMORY: return t(p, "Installed RAM (KiB)", "Memorie RAM (Kocteți)");
    case R_CLOCK: return t(p, "KW11-L clock", "Ceas KW11-L");
    case R_TM11: return t(p, "TM11 read-only tape", "Bandă TM11, numai citire");
    case R_SERIAL: return t(p, "DL11 serial line", "Legătură serială DL11");
    case R_FORMAT: return t(p, "Format", "Format");
    case R_TAPE: return t(p, "TM0 tape image", "Imagine bandă TM0");
    case R_DISKS: return t(p, "Disk controller", "Cuplor de disc");
    case R_DISK0: return t(p, "Unit 0 image", "Imagine unitate 0");
    case R_DISK1: return t(p, "Unit 1 image", "Imagine unitate 1");
    case R_BOOT: return t(p, "Boot source", "Încărcare inițială");
    case R_SPEED: return t(p, "Full host speed", "Viteza maximă a gazdei");
    default: return "";
    }
}

static const char *hint(const Panel *p)
{
    if (p->naming)
        return t(p, "Type a file name   Enter accept   Esc cancel", "Se scrie numele fișierului   Enter acceptă   Esc renunță");
    if (p->editing)
        return t(p, "Type   Enter accept   Esc cancel   Ctrl+V paste", "Se scrie   Enter acceptă   Esc renunță   Ctrl+V lipire");
    if (p->picking)
        return t(p, "Enter open   Backspace up   Esc cancel   Double-click choose",
                 "Enter deschide   Backspace în sus   Esc renunță   Dublu clic alege");
    if (p->popup_open)
        return t(p, "Up/Down choose   Enter accept   Esc cancel", "Sus/Jos alege   Enter acceptă   Esc renunță");
    switch (kind(p->row)) {
    case KIND_TOGGLE: return t(p, "Up/Down row   Enter, Space or click switch", "Sus/Jos rând   Enter, Spațiu sau clic comută");
    case KIND_TEXT: return t(p, "Up/Down row   Enter or click edit", "Sus/Jos rând   Enter sau clic modifică");
    case KIND_FILE: return t(p, "Up/Down row   Enter or click choose a file   Delete eject",
                             "Sus/Jos rând   Enter sau clic alege fișierul   Delete scoate");
    case KIND_BUTTONS: return t(p, "Left/Right button   Enter or click press", "Stânga/Dreapta buton   Enter sau clic apasă");
    default: return t(p, "Up/Down row   Left/Right change   Enter or click list",
                      "Sus/Jos rând   Stânga/Dreapta schimbă   Enter sau clic listă");
    }
}

static void status(Panel *p, char *out, size_t size)
{
    snprintf(out, size, "%s", t(p, "Powered off", "Oprit"));
    if (!p->powered)
        return;
    Machine *m = &p->session.machine;
    static const char *const states[][2] = {
        {"Running", "În funcțiune"}, {"Serial output error", "Eroare la ieșirea serială"},
        {"Waiting for the serial line", "Așteaptă legătura serială"}, {"Double fault", "Eroare dublă"},
        {"Halted", "Oprit la HALT"}, {"Waiting for an interrupt", "Așteaptă o întrerupere"}
    };
    const char *const *state = states[session_status(&p->session)];
    const char *line = p->session.serial.connected ? t(p, "connected", "conectată") :
        p->session.serial.stream ? t(p, "disconnected", "deconectată") : t(p, "none", "neconectată");
    int n = snprintf(out, size, "%s | PC %06o | %s %s", p->paused ? t(p, "Paused", "Pauză") : t(p, state[0], state[1]),
                     m->cpu.r[7], t(p, "Serial line", "Legătură"), line);
    if (m->tape.medium.bytes && n > 0 && (size_t)n < size)
        n += snprintf(out + n, size - (size_t)n, " | %s %zu/%zu", t(p, "Tape", "Bandă"), m->tape.medium.position,
                      m->tape.medium.count);
    if (machine_disk_busy(m) && n > 0 && (size_t)n < size)
        snprintf(out + n, size - (size_t)n, " | %s", t(p, "Disk", "Disc"));
}

static void heading(Panel *p, int y, const char *title)
{
    text(p, LABEL_X, y, title, C_HEAD, 0);
    int x = LABEL_X + (int)text_chars(title) * 8 + 8;
    rect(p, x, y + 4, FRAME_W - 16 - x, 1, C_LINE);
}

static void render_value(Panel *p, unsigned row, uint32_t color)
{
    int y = row_y[row];
    char buffer[96];
    switch (kind(row)) {
    case KIND_TOGGLE: {
        bool on = row == R_FPU ? p->fpu : row == R_TM11 ? p->tm11 : p->full;
        rect(p, VALUE_X, y - 1, 10, 10, color);
        rect(p, VALUE_X + 1, y, 8, 8, C_BACK);
        if (on)
            rect(p, VALUE_X + 2, y + 1, 6, 6, enabled(p, row) ? C_HEAD : C_OFF);
        text(p, VALUE_X + 18, y, on ? t(p, "On", "Da") : t(p, "Off", "Nu"), color, 0);
        break;
    }
    case KIND_TEXT: {
        size_t size;
        const char *value = p->editing && p->row == row ? p->edit : text_value(p, row, &size);
        fitted(p, VALUE_X, y, value, color, VALUE_CHARS - 1);
        if (p->editing && p->row == row) {
            size_t n = text_chars(value);
            rect(p, VALUE_X + (int)(n < VALUE_CHARS - 1 ? n : VALUE_CHARS - 1) * 8, y + 8, 8, 1, C_TEXT);
        }
        if (row == R_MEMORY) {
            MachineConfig c = machine_defaults(profile(p));
            unsigned ram = 0;
            if (read_unsigned(p->memory, 10, c.profile->capacity_kib, &c.memory_kib))
                ram = machine_ram_kib(&c);
            if (ram)
                snprintf(buffer, sizeof(buffer), t(p, "%u KiB addressable as RAM", "%u Kocteți adresabili ca RAM"), ram);
            else
                snprintf(buffer, sizeof(buffer), t(p, "Enter 4 to %u KiB", "De la 4 la %u Kocteți"), c.profile->capacity_kib);
            text(p, VALUE_X + 64, y, buffer, C_DIM, 0);
        }
        break;
    }
    case KIND_FILE: {
        const char *value = file_value(p, row);
        fitted(p, VALUE_X, y, *value ? value : "-", color, FILE_CHARS);
        if (*value)
            text(p, FRAME_W - 16 - 7 * 8, y, t(p, "[Eject]", "[Scoate]"), enabled(p, row) ? C_DIM : C_OFF, 8);
        break;
    }
    case KIND_CHOICE: {
        unsigned n = choice_count(p, row);
        text(p, VALUE_X, y, n ? choice_label(p, row, choice_value(p, row)) : "-", color, VALUE_CHARS);
        if (enabled(p, row) && n > 1)
            text(p, FRAME_W - 16 - 8, y, "v", C_DIM, 1);
        break;
    }
    default: break;
    }
}

static void render_picker(Panel *p)
{
    rect(p, 22, 30, FRAME_W - 44, FRAME_H - 58, C_LINE);
    rect(p, 24, 32, FRAME_W - 48, FRAME_H - 62, C_BOX);
    text(p, 36, 40, p->pick_row == R_TAPE ? t(p, "Tape for TM0", "Bandă pentru TM0") : t(p, "Disk image", "Imagine disc"),
         C_HEAD, 0);
    fitted(p, 36, 56, p->dir, C_DIM, (FRAME_W - 72) / 8);
    for (unsigned i = 0; i < LIST_ROWS && p->entry_top + i < p->entry_count; ++i) {
        unsigned index = p->entry_top + i;
        const Entry *e = &p->entries[index];
        int y = LIST_Y + (int)i * 14;
        bool selected = index == p->entry_index;
        if (selected)
            rect(p, 28, y - 3, FRAME_W - 56, 14, C_SELECT);
        char name[300];
        if (index == 0)
            snprintf(name, sizeof(name), "%s", p->pick_row == R_TAPE ? t(p, "Type a file name...", "Se scrie un nume...")
                                                                     : t(p, "Type a file name (a new name makes an empty pack)...",
                                                                         "Se scrie un nume (un nume nou face un pachet gol)..."));
        else
            snprintf(name, sizeof(name), "%s%s", e->name, e->dir ? "/" : "");
        text(p, 36, y, name, index == 0 ? C_DIM : selected ? C_TEXT : e->dir ? C_HEAD : C_TEXT, (FRAME_W - 72) / 8);
    }
    if (p->naming) {
        int y = LIST_Y + LIST_ROWS * 14 + 4;
        rect(p, 28, y - 3, FRAME_W - 56, 14, C_SELECT);
        fitted(p, 36, y, p->edit, C_TEXT, (FRAME_W - 80) / 8);
        size_t n = text_chars(p->edit), max = (FRAME_W - 80) / 8;
        rect(p, 36 + (int)(n < max ? n : max) * 8, y + 8, 8, 1, C_TEXT);
    }
}

static void render(Panel *p)
{
    char line[200];
    rect(p, 0, 0, FRAME_W, FRAME_H, C_BACK);
    snprintf(line, sizeof(line), "Recif %s | %s", RECIF_VERSION, t(p, "CORAL main unit", "Unitate centrală CORAL"));
    text(p, LABEL_X, 10, line, C_TEXT, 0);
    heading(p, 28, t(p, "CORAL hardware record", "Echipare CORAL documentată"));
    const CoralProfile *model = profile(p);
    wrapped(p, LABEL_X, 78, p->lang == LANG_RO ? model->hardware_ro : model->hardware, C_DIM, 78, 3);
    wrapped(p, LABEL_X, 116, p->lang == LANG_RO ? model->peripherals_ro : model->peripherals, C_DIM, 78, 3);
    heading(p, 160, t(p, "DEC reference configuration", "Configurație de referință DEC"));
    text(p, LABEL_X, 178, t(p, "Processor", "Procesor"), C_DIM, 0);
    text(p, VALUE_X, 178, pdp_reference_name(model->cpu), C_TEXT, 0);
    for (unsigned row = 0; row < R_BUTTONS; ++row) {
        bool on = enabled(p, row), selected = row == p->row;
        if (selected)
            rect(p, 10, row_y[row] - 3, FRAME_W - 20, 14, C_SELECT);
        text(p, LABEL_X, row_y[row], row_label(p, row), on ? (selected ? C_TEXT : C_DIM) : C_OFF, 24);
        render_value(p, row, on ? C_TEXT : C_OFF);
    }
    for (unsigned b = 0; b < BUTTONS; ++b) {
        int x, w, y = row_y[R_BUTTONS];
        button_box(p, b, &x, &w);
        bool on = button_enabled(p, b), selected = p->row == R_BUTTONS && p->button == b;
        rect(p, x, y - 5, w, 18, selected ? C_TEXT : on ? C_DIM : C_LINE);
        rect(p, x + 1, y - 4, w - 2, 16, selected ? C_SELECT : C_BOX);
        int label = (int)text_chars(button_label(p, b)) * 8;
        text(p, x + (w - label) / 2, y, button_label(p, b), on ? C_TEXT : C_OFF, 0);
    }
    rect(p, 10, 386, FRAME_W - 20, 1, C_LINE);
    text(p, LABEL_X, 394, hint(p), C_DIM, 0);
    if (p->message[0])
        text(p, LABEL_X, 408, p->message, C_WARN, 0);
    status(p, line, sizeof(line));
    text(p, LABEL_X, 426, line, p->powered ? C_HEAD : C_DIM, 0);
    if (p->popup_open) {
        int top = popup_top(p), height = (int)p->popup_count * 14 + 8;
        rect(p, VALUE_X - 10, top - 2, 260, height + 4, C_DIM);
        rect(p, VALUE_X - 8, top, 256, height, C_BOX);
        for (unsigned i = 0; i < p->popup_count; ++i) {
            int y = top + 6 + (int)i * 14;
            if (i == p->popup_index)
                rect(p, VALUE_X - 6, y - 3, 252, 14, C_SELECT);
            text(p, VALUE_X, y, p->popup[i], i == p->popup_index ? C_TEXT : C_DIM, 30);
        }
    }
    if (p->picking)
        render_picker(p);
}

/* --- the window ---------------------------------------------------------------------------------------- */

static bool parse(Options *o, int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--hidden"))
            o->hidden = true;
        else if (!strcmp(argv[i], "--start"))
            o->start = true;
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc)
            o->screenshot = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            o->frames = (unsigned)strtoul(argv[++i], NULL, 10);
        else
            return false;
    }
    return true;
}

/* The window at twice the picture's size when the desktop has room. */
static int window_scale(void)
{
    SDL_Rect usable;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) || SDL_GetDisplayUsableBounds(0, &usable))
        return 2;
    int scale = usable.w >= FRAME_W * 2 && usable.h >= FRAME_H * 2 + 40 ? 2 : 1;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return scale;
}

int main_unit_window(int argc, char **argv)
{
    SDL_SetMainReady();
    Options o = {0};
    if (!parse(&o, argc, argv)) {
        fputs("recif [--hidden] [--start] [--frames N] [--screenshot FILE.png]\n", stderr);
        return 2;
    }
    Panel *p = calloc(1, sizeof(*p));
    if (!p)
        return 1;
    prefs_open(&p->prefs, "recif.yaml");
    static const char *const bsd[] = {"model", "4021", "disks", "rp06", "disk0", PREFS_BSD_PACK, "boot", "3,0", NULL};
    prefs_first_start(&p->prefs, bsd);
    p->lang = lang_choose(prefs_get(&p->prefs, "language", NULL));
    snprintf(p->serial, sizeof(p->serial), "tcp-listen:2323");
    snprintf(p->format, sizeof(p->format), "9600,8N1");
    load_prefs(p);
    p->row = R_BUTTONS;

    mx_settings settings = {0};
    settings.aspect = MX_ASPECT_SQUARE;
    settings.window_scale = o.hidden ? 1 : window_scale();
    settings.filter = MX_FILTER_NEAREST;
    settings.vsync = false;
    char error[512] = "";
    p->video = mx_video_open("Recif " RECIF_VERSION, &settings, FRAME_W, FRAME_H, FRAME_W, FRAME_H, o.hidden, error,
                             sizeof(error));
    if (!p->video) {
        fprintf(stderr, "recif: %s\n", error);
        free(p);
        return 1;
    }
    if (!o.hidden)
        mx_video_set_icon(p->video, recif_icon_pixels, RECIF_ICON_SIZE);
    mx_video_set_look(p->video, MX_VIDEO_LOOK_PLAIN, MX_VIDEO_PHOSPHOR_COLOR);
    if (o.start)
        press(p, B_POWER);
    SDL_StartTextInput();
    bool open = true;
    unsigned frames = 0;
    uint64_t next = rc_host_now_us();
    while (open) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                open = false;
            } else if (e.type == SDL_KEYDOWN) {
                key(p, e.key.keysym);
            } else if (e.type == SDL_TEXTINPUT) {
                if ((p->editing || p->naming) && !(SDL_GetModState() & (KMOD_CTRL | KMOD_ALT)))
                    type_text(p, e.text.text);
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                int x, y;
                if (mx_video_to_screen(p->video, e.button.x, e.button.y, &x, &y))
                    click(p, x, y, e.button.clicks);
            } else if (e.type == SDL_MOUSEWHEEL && p->picking) {
                unsigned step = 3;
                if (e.wheel.y > 0)
                    p->entry_top = p->entry_top > step ? p->entry_top - step : 0;
                else if (e.wheel.y < 0 && p->entry_count > LIST_ROWS)
                    p->entry_top = p->entry_top + step + LIST_ROWS < p->entry_count ? p->entry_top + step
                                                                                     : p->entry_count - LIST_ROWS;
            }
        }
        run(p);
        render(p);
        bool end = o.frames && ++frames == o.frames;
        if (end && o.screenshot)
            mx_video_request_shot(p->video, o.screenshot, true);
        mx_video_present(p->video, p->frame, FRAME_W, FRAME_H);
        if (end)
            break;
        next += LOOP_US;
        uint64_t now = rc_host_now_us();
        if (next > now)
            rc_host_sleep_us((uint32_t)(next - now));
        else if (now - next > 50000)
            next = now;
    }
    SDL_StopTextInput();
    if (!o.hidden)
        save_prefs(p);
    if (p->powered)
        session_close(&p->session);
    mx_video_close(p->video);
    free(p);
    return 0;
}
