#include "main_unit.h"

#include "lang.h"
#include "machine_session.h"
#include "prefs.h"
#include "rc_version.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <stdlib.h>
#include <string.h>

enum {
    MODEL = 1, LANGUAGE, MEMORY, CLOCK, TM11, FPU, SERIAL, FORMAT, IMAGE, BROWSE, EJECT,
    DISKS, DISK0, BROWSE0, EJECT0, DISK1, BROWSE1, EJECT1, BOOT,
    POWER, RESTART, PAUSE, STOP, HARDWARE, DEVICES, CPU, RAM, STATUS,
    GROUP_RECORD, GROUP_REFERENCE, L_MODEL, L_LANGUAGE, L_PROCESSOR, L_MEMORY, L_CLOCK, L_SERIAL,
    L_FORMAT, L_TAPE, L_DISKS, L_DISK0, L_DISK1, L_BOOT, FULL_SPEED, CONTROL_COUNT,
    CLIENT_W = 700, CLIENT_H = 556, BOOT_CHOICES = 5
};

typedef struct {
    StartMode mode;
    unsigned option;
} BootChoice;

typedef struct {
    HWND window, control[CONTROL_COUNT];
    HFONT font;
    int dpi;
    Language lang;
    MachineSession session;
    Prefs prefs;
    BootChoice boot[BOOT_CHOICES];
    unsigned boot_count;
    uint64_t last_tick, last_status;
    char status[200];
    bool powered, paused;
} Panel;

static const unsigned clock_rates[] = {50, 60, 0};

static const char *t(const Panel *p, const char *en, const char *ro)
{
    return TR(p->lang, en, ro);
}

static int scale(const Panel *p, int n)
{
    return MulDiv(n, p->dpi, 96);
}

static void wide(const char *text, wchar_t *out, int size)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, text, -1, out, size))
        out[0] = 0;
}

static void set_text(Panel *p, int id, const char *text)
{
    wchar_t w[RC_HOST_PATH + 64];
    wide(text, w, (int)(sizeof(w) / sizeof(w[0])));
    SetWindowTextW(p->control[id], w);
}

static HWND control(Panel *p, int id, const wchar_t *kind, DWORD style, int x, int y, int w, int h)
{
    DWORD ex = !wcscmp(kind, L"EDIT") ? WS_EX_CLIENTEDGE : 0;
    HWND item = CreateWindowExW(ex, kind, L"", WS_CHILD | WS_VISIBLE | style,
                                scale(p, x), scale(p, y), scale(p, w), scale(p, h),
                                p->window, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(item, WM_SETFONT, (WPARAM)p->font, FALSE);
    p->control[id] = item;
    return item;
}

static void label(Panel *p, int id, int x, int y, int w)
{
    control(p, id, L"STATIC", SS_LEFT, x, y, w, 23);
}

static void button(Panel *p, int id, int x, int y, int w)
{
    control(p, id, L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, x, y, w, 26);
}

static HWND combo(Panel *p, int id, int x, int y, int w)
{
    return control(p, id, L"COMBOBOX", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, x, y, w, 160);
}

static void add_item(Panel *p, int id, const char *text)
{
    wchar_t w[128];
    wide(text, w, 128);
    SendMessageW(p->control[id], CB_ADDSTRING, 0, (LPARAM)w);
}

static unsigned selected(Panel *p, int id)
{
    return (unsigned)SendMessageW(p->control[id], CB_GETCURSEL, 0, 0);
}

static void set_selection(Panel *p, int id, int index)
{
    SendMessageW(p->control[id], CB_SETCURSEL, (WPARAM)index, 0);
}

static bool checked(Panel *p, int id)
{
    return SendMessageW(p->control[id], BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static void set_check(Panel *p, int id, bool on)
{
    SendMessageW(p->control[id], BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

static bool has_text(Panel *p, int id)
{
    return GetWindowTextLengthW(p->control[id]) > 0;
}

static void get_text(Panel *p, int id, char *out, size_t size)
{
    GetWindowTextA(p->control[id], out, (int)size);
}

static const CoralProfile *profile(Panel *p)
{
    unsigned i = selected(p, MODEL);
    return &coral_profiles[i < CORAL_MODEL_COUNT ? i : CORAL_MODEL_COUNT - 1];
}

static DiskSystem disks(Panel *p)
{
    unsigned i = selected(p, DISKS);
    return i < DISKS_COUNT ? (DiskSystem)i : DISKS_NONE;
}

static void show_ram(Panel *p)
{
    char text[96] = "";
    MachineConfig c = machine_defaults(profile(p));
    get_text(p, MEMORY, text, sizeof(text));
    unsigned ram = 0;
    if (read_unsigned(text, 10, c.profile->capacity_kib, &c.memory_kib))
        ram = machine_ram_kib(&c);
    if (ram)
        snprintf(text, sizeof(text), t(p, "%u KiB addressable as RAM", "%u Kocteți adresabili ca RAM"), ram);
    else
        snprintf(text, sizeof(text), t(p, "Enter 4 to %u KiB", "De la 4 la %u Kocteți"), c.profile->capacity_kib);
    set_text(p, RAM, text);
}

static void show_profile_text(Panel *p)
{
    const CoralProfile *model = profile(p);
    set_text(p, HARDWARE, p->lang == LANG_RO ? model->hardware_ro : model->hardware);
    set_text(p, DEVICES, p->lang == LANG_RO ? model->peripherals_ro : model->peripherals);
    set_text(p, CPU, pdp_reference_name(model->cpu));
}

/* The boot list offers only devices that are fitted and loaded. */
static void fill_boot(Panel *p)
{
    unsigned old = selected(p, BOOT);
    BootChoice previous = old < p->boot_count ? p->boot[old] : (BootChoice){START_MONITOR, 0};
    SendMessageW(p->control[BOOT], CB_RESETCONTENT, 0, 0);
    p->boot_count = 0;
    p->boot[p->boot_count++] = (BootChoice){START_MONITOR, 0};
    add_item(p, BOOT, t(p, "Diagnostic monitor", "Monitor de diagnosticare"));
    if (checked(p, TM11) && has_text(p, IMAGE)) {
        p->boot[p->boot_count++] = (BootChoice){START_TAPE, 0};
        add_item(p, BOOT, t(p, "Labelled tape bootstrap", "Încărcare de pe banda etichetată"));
        p->boot[p->boot_count++] = (BootChoice){START_TAPE, 1};
        add_item(p, BOOT, t(p, "First tape record", "Prima înregistrare de pe bandă"));
    }
    for (unsigned u = 0; u < MACHINE_DISK_UNITS; ++u) {
        if (disks(p) != DISKS_NONE && has_text(p, u ? DISK1 : DISK0)) {
            p->boot[p->boot_count++] = (BootChoice){START_DISK, u};
            add_item(p, BOOT, u ? t(p, "Disk unit 1", "Discul unității 1") : t(p, "Disk unit 0", "Discul unității 0"));
        }
    }
    int index = 0;
    for (unsigned i = 0; i < p->boot_count; ++i)
        if (p->boot[i].mode == previous.mode && p->boot[i].option == previous.option)
            index = (int)i;
    set_selection(p, BOOT, index);
}

static void fill_lists(Panel *p)
{
    unsigned clock = selected(p, CLOCK), disk = selected(p, DISKS);
    SendMessageW(p->control[CLOCK], CB_RESETCONTENT, 0, 0);
    add_item(p, CLOCK, "50 Hz");
    add_item(p, CLOCK, "60 Hz");
    add_item(p, CLOCK, t(p, "Off", "Oprit"));
    set_selection(p, CLOCK, clock < 3 ? (int)clock : 0);
    SendMessageW(p->control[DISKS], CB_RESETCONTENT, 0, 0);
    add_item(p, DISKS, t(p, "None", "Fără"));
    add_item(p, DISKS, t(p, "RH11 with RM03 (67 MB)", "RH11 cu RM03 (67 MB)"));
    add_item(p, DISKS, t(p, "RH11 with RP06 (174 MB)", "RH11 cu RP06 (174 MB)"));
    add_item(p, DISKS, t(p, "RP11-C with RP03 (40 MB)", "RP11-C cu RP03 (40 MB)"));
    set_selection(p, DISKS, disk < DISKS_COUNT ? (int)disk : 0);
}

static void relabel(Panel *p)
{
    char title[96];
    snprintf(title, sizeof(title), "Recif %s | %s", RECIF_VERSION, t(p, "CORAL main unit", "Unitate centrală CORAL"));
    wchar_t w[96];
    wide(title, w, 96);
    SetWindowTextW(p->window, w);
    static const struct { int id; const char *en, *ro; } texts[] = {
        {GROUP_RECORD, "CORAL hardware record", "Echipare CORAL documentată"},
        {GROUP_REFERENCE, "DEC reference configuration", "Configurație de referință DEC"},
        {L_MODEL, "Model", "Model"}, {L_LANGUAGE, "Language", "Limbă"},
        {L_PROCESSOR, "Processor", "Procesor"}, {L_MEMORY, "Installed RAM (KiB)", "Memorie RAM (Kocteți)"},
        {L_CLOCK, "KW11-L clock", "Ceas KW11-L"}, {L_SERIAL, "DL11 serial line", "Legătură serială DL11"},
        {L_FORMAT, "Format", "Format"}, {L_TAPE, "TM0 tape image", "Imagine bandă TM0"},
        {L_DISKS, "Disk controller", "Cuplor de disc"}, {L_DISK0, "Unit 0 image", "Imagine unitate 0"},
        {L_DISK1, "Unit 1 image", "Imagine unitate 1"}, {L_BOOT, "Boot source", "Încărcare inițială"},
        {FPU, "FP11 floating point", "Virgulă mobilă FP11"}, {TM11, "TM11 read-only tape", "Bandă TM11, numai citire"},
        {FULL_SPEED, "Full host speed", "Viteza maximă a gazdei"},
        {BROWSE, "Browse...", "Alegere..."}, {BROWSE0, "Browse...", "Alegere..."}, {BROWSE1, "Browse...", "Alegere..."},
        {EJECT, "Eject", "Scoatere"}, {EJECT0, "Eject", "Scoatere"}, {EJECT1, "Eject", "Scoatere"},
        {POWER, "Start reference", "Pornire"}, {RESTART, "Restart", "Repornire"}, {STOP, "Stop", "Oprire"},
    };
    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); ++i)
        set_text(p, texts[i].id, t(p, texts[i].en, texts[i].ro));
    fill_lists(p);
    show_profile_text(p);
    show_ram(p);
    p->status[0] = '\0';
}

static void set_profile(Panel *p)
{
    const CoralProfile *model = profile(p);
    char text[96];
    show_profile_text(p);
    set_check(p, TM11, model->tape_reported);
    set_check(p, FPU, model->fpu_reported);
    set_selection(p, DISKS, (int)model->disks);
    snprintf(text, sizeof(text), "%u", model->nominal_kib);
    set_text(p, MEMORY, text);
    show_ram(p);
}

static void update_controls(Panel *p)
{
    static const int locked[] = {MODEL, MEMORY, CLOCK, TM11, FPU, DISKS, SERIAL, FORMAT, POWER};
    for (size_t i = 0; i < sizeof(locked) / sizeof(locked[0]); ++i)
        EnableWindow(p->control[locked[i]], !p->powered);
    if (!pdp_fpu_option(profile(p)->cpu)) {
        set_check(p, FPU, false);
        EnableWindow(p->control[FPU], FALSE);
    }
    bool tape = checked(p, TM11), disk = disks(p) != DISKS_NONE;
    EnableWindow(p->control[IMAGE], tape);
    EnableWindow(p->control[BROWSE], tape);
    EnableWindow(p->control[EJECT], tape && has_text(p, IMAGE));
    static const int units[MACHINE_DISK_UNITS][3] = {{DISK0, BROWSE0, EJECT0}, {DISK1, BROWSE1, EJECT1}};
    for (unsigned u = 0; u < MACHINE_DISK_UNITS; ++u) {
        EnableWindow(p->control[units[u][0]], disk);
        EnableWindow(p->control[units[u][1]], disk);
        EnableWindow(p->control[units[u][2]], disk && has_text(p, units[u][0]));
    }
    fill_boot(p);
    EnableWindow(p->control[BOOT], p->boot_count > 1);
    EnableWindow(p->control[RESTART], p->powered);
    EnableWindow(p->control[PAUSE], p->powered);
    EnableWindow(p->control[STOP], p->powered);
    set_text(p, PAUSE, p->paused ? t(p, "Resume", "Continuare") : t(p, "Pause", "Pauză"));
}

static void show_status(Panel *p)
{
    char text[200];
    snprintf(text, sizeof(text), "%s", t(p, "Powered off", "Oprit"));
    if (p->powered) {
        Machine *m = &p->session.machine;
        static const char *const states[][2] = {
            {"Running", "În funcțiune"}, {"Serial output error", "Eroare la ieșirea serială"},
            {"Waiting for the serial line", "Așteaptă legătura serială"}, {"Double fault", "Eroare dublă"},
            {"Halted", "Oprit la HALT"}, {"Waiting for an interrupt", "Așteaptă o întrerupere"}
        };
        const char *const *state = states[session_status(&p->session)];
        const char *line = p->session.serial.connected ? t(p, "connected", "conectată") :
            p->session.serial.stream ? t(p, "disconnected", "deconectată") : t(p, "none", "neconectată");
        int n = snprintf(text, sizeof(text), "%s   |   PC %06o   |   %s %s",
                         p->paused ? t(p, "Paused", "Pauză") : t(p, state[0], state[1]), m->cpu.r[7],
                         t(p, "Serial line", "Legătură"), line);
        if (m->tape.medium.bytes && n > 0 && (size_t)n < sizeof(text))
            n += snprintf(text + n, sizeof(text) - (size_t)n, "   |   %s %zu/%zu", t(p, "Tape", "Bandă"),
                          m->tape.medium.position, m->tape.medium.count);
        if (machine_disk_busy(m) && n > 0 && (size_t)n < sizeof(text))
            snprintf(text + n, sizeof(text) - (size_t)n, "   |   %s", t(p, "Disk", "Disc"));
    }
    if (strcmp(text, p->status)) {
        set_text(p, STATUS, text);
        strcpy(p->status, text);
    }
}

static void error_box(Panel *p, const char *text)
{
    wchar_t message[600], caption[64];
    wide(text, message, 600);
    wide(t(p, "Recif main unit", "Unitate centrală Recif"), caption, 64);
    MessageBoxW(p->window, message, caption, MB_OK | MB_ICONERROR);
}

static bool start_guest(Panel *p, char *error, size_t size)
{
    unsigned i = selected(p, BOOT);
    BootChoice choice = i < p->boot_count ? p->boot[i] : (BootChoice){START_MONITOR, 0};
    return session_start(&p->session, choice.mode, NULL, 0, 0, 0, choice.option, error, size);
}

static void save_prefs(Panel *p)
{
    char text[RC_HOST_PATH + 16];
    prefs_set(&p->prefs, "language", lang_name(p->lang));
    prefs_set(&p->prefs, "model", profile(p)->id);
    static const int fields[] = {MEMORY, SERIAL, FORMAT, IMAGE, DISK0, DISK1};
    static const char *keys[] = {"ram", "serial", "format", "tape", "disk0", "disk1"};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        get_text(p, fields[i], text, sizeof(text));
        prefs_set(&p->prefs, keys[i], text);
    }
    snprintf(text, sizeof(text), "%u", clock_rates[selected(p, CLOCK) % 3]);
    prefs_set(&p->prefs, "clock", text);
    prefs_set(&p->prefs, "fpu", checked(p, FPU) ? "on" : "off");
    prefs_set(&p->prefs, "tm11", checked(p, TM11) ? "on" : "off");
    prefs_set(&p->prefs, "speed", checked(p, FULL_SPEED) ? "full" : "real");
    prefs_set(&p->prefs, "disks", disk_system_name(disks(p)));
    unsigned b = selected(p, BOOT);
    snprintf(text, sizeof(text), "%u,%u", b < p->boot_count ? (unsigned)p->boot[b].mode : 0,
             b < p->boot_count ? p->boot[b].option : 0);
    prefs_set(&p->prefs, "boot", text);
    prefs_save(&p->prefs);
}

static void load_prefs(Panel *p)
{
    const CoralProfile *model = coral_find(prefs_get(&p->prefs, "model", "4030"));
    if (!model)
        return;
    set_selection(p, MODEL, (int)(model - coral_profiles));
    set_profile(p);
    static const int fields[] = {MEMORY, SERIAL, FORMAT, IMAGE, DISK0, DISK1};
    static const char *keys[] = {"ram", "serial", "format", "tape", "disk0", "disk1"};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        const char *value = prefs_get(&p->prefs, keys[i], NULL);
        if (value)
            SetWindowTextA(p->control[fields[i]], value);
    }
    unsigned clock = (unsigned)atoi(prefs_get(&p->prefs, "clock", "50"));
    set_selection(p, CLOCK, clock == 60 ? 1 : clock ? 0 : 2);
    static const int checks[] = {FPU, TM11};
    static const char *flags[] = {"fpu", "tm11"};
    for (size_t i = 0; i < 2; ++i) {
        const char *value = prefs_get(&p->prefs, flags[i], NULL);
        if (value)
            set_check(p, checks[i], !strcmp(value, "on"));
    }
    set_check(p, FULL_SPEED, !strcmp(prefs_get(&p->prefs, "speed", "real"), "full"));
    DiskSystem saved;
    if (disk_system_parse(prefs_get(&p->prefs, "disks", ""), &saved))
        set_selection(p, DISKS, (int)saved);
    unsigned mode = 0, option = 0;
    sscanf(prefs_get(&p->prefs, "boot", "0,0"), "%u,%u", &mode, &option);
    update_controls(p);
    for (unsigned i = 0; i < p->boot_count; ++i)
        if ((unsigned)p->boot[i].mode == mode && p->boot[i].option == option)
            set_selection(p, BOOT, (int)i);
    show_ram(p);
}

static void start_panel(Panel *p)
{
    MachineConfig c = machine_defaults(profile(p));
    char serial[RC_HOST_PATH + 16], format[32], path[RC_HOST_PATH], memory[24], error[512];
    get_text(p, MEMORY, memory, sizeof(memory));
    if (!read_unsigned(memory, 10, c.profile->capacity_kib, &c.memory_kib) || !machine_ram_kib(&c)) {
        error_box(p, t(p, "Choose a RAM size in the displayed range.", "Se alege o memorie RAM din intervalul afișat."));
        return;
    }
    get_text(p, FORMAT, format, sizeof(format));
    if (!rs232_parse_format(format, &c.console)) {
        error_box(p, t(p, "Enter a serial format such as 9600,8N1.", "Se introduce un format serial, de exemplu 9600,8N1."));
        return;
    }
    c.clock_hz = clock_rates[selected(p, CLOCK) % 3];
    c.tm11 = checked(p, TM11);
    c.fpu = checked(p, FPU);
    c.disks = disks(p);
    get_text(p, SERIAL, serial, sizeof(serial));

    if (!session_open(&p->session, &c, *serial ? serial : NULL, error, sizeof(error))) {
        error_box(p, error);
        return;
    }
    bool ok = true;
    char full[RC_HOST_PATH];
    get_text(p, IMAGE, path, sizeof(path));
    if (c.tm11 && *path)
        ok = session_mount(&p->session, prefs_beside_program(path, full, sizeof(full)), NULL, error, sizeof(error));
    for (unsigned u = 0; ok && c.disks != DISKS_NONE && u < MACHINE_DISK_UNITS; ++u) {
        get_text(p, u ? DISK1 : DISK0, path, sizeof(path));
        if (*path)
            ok = session_mount_disk(&p->session, u, prefs_beside_program(path, full, sizeof(full)), error, sizeof(error));
    }
    if (!ok || !start_guest(p, error, sizeof(error))) {
        session_close(&p->session);
        error_box(p, error);
        return;
    }
    p->powered = true;
    p->last_tick = rc_host_now_us();
    save_prefs(p);
}

static void change_tape(Panel *p, const char *path)
{
    if (!checked(p, TM11))
        return;
    char error[512];
    if (p->powered) {
        if (!*path)
            session_eject(&p->session);
        else if (!session_mount(&p->session, path, NULL, error, sizeof(error))) {
            error_box(p, error);
            return;
        }
    }
    SetWindowTextA(p->control[IMAGE], path);
}

static void change_disk(Panel *p, unsigned unit, const char *path)
{
    if (disks(p) == DISKS_NONE)
        return;
    char error[512];
    if (p->powered) {
        if (!*path)
            session_eject_disk(&p->session, unit);
        else if (!session_mount_disk(&p->session, unit, path, error, sizeof(error))) {
            error_box(p, error);
            return;
        }
    }
    SetWindowTextA(p->control[unit ? DISK1 : DISK0], path);
}

static bool choose_file(Panel *p, const char *title, const char *filter, bool existing, char *name, size_t size)
{
    OPENFILENAMEA ofn = {0};
    name[0] = '\0';
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = p->window;
    ofn.lpstrTitle = title;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = name;
    ofn.nMaxFile = (DWORD)size;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (existing ? OFN_FILEMUSTEXIST : 0);
    return GetOpenFileNameA(&ofn) != 0;
}

static void command(Panel *p, unsigned id, unsigned notification)
{
    if (id == MODEL && notification == CBN_SELCHANGE) {
        set_profile(p);
        update_controls(p);
        return;
    }
    if (id == LANGUAGE && notification == CBN_SELCHANGE) {
        p->lang = selected(p, LANGUAGE) == 1 ? LANG_RO : LANG_EN;
        relabel(p);
        update_controls(p);
        show_status(p);
        return;
    }
    if (id == DISKS && notification == CBN_SELCHANGE) {
        update_controls(p);
        return;
    }
    if (id == MEMORY && notification == EN_CHANGE) {
        show_ram(p);
        return;
    }
    if (notification != BN_CLICKED)
        return;
    char error[512], name[RC_HOST_PATH];
    switch (id) {
    case POWER: start_panel(p); break;
    case RESTART:
        if (!start_guest(p, error, sizeof(error)))
            error_box(p, error);
        else
            p->paused = false;
        break;
    case PAUSE: p->paused = !p->paused; break;
    case STOP:
        session_close(&p->session);
        p->powered = p->paused = false;
        break;
    case BROWSE:
        if (choose_file(p, t(p, "Tape for TM0", "Bandă pentru TM0"),
                        "Tape images (*.tap;*.dat;*.img)\0*.tap;*.dat;*.img\0*.*\0*.*\0", true, name, sizeof(name)))
            change_tape(p, name);
        break;
    case EJECT: change_tape(p, ""); break;
    case BROWSE0: case BROWSE1:
        /* A new file name creates an empty pack. */
        if (choose_file(p, t(p, "Disk image", "Imagine disc"), "Disk images (*.dsk;*.img)\0*.dsk;*.img\0*.*\0*.*\0",
                        false, name, sizeof(name)))
            change_disk(p, id == BROWSE1, name);
        break;
    case EJECT0: case EJECT1: change_disk(p, id == EJECT1, ""); break;
    }
    update_controls(p);
    show_status(p);
}

static void create_controls(Panel *p)
{
    control(p, GROUP_RECORD, L"BUTTON", BS_GROUPBOX, 12, 9, 676, 141);
    label(p, L_MODEL, 24, 35, 130);
    HWND list = combo(p, MODEL, 163, 31, 178);
    for (unsigned i = 0; i < CORAL_MODEL_COUNT; ++i)
        add_item(p, MODEL, coral_profiles[i].name);
    (void)list;
    set_selection(p, MODEL, CORAL_MODEL_COUNT - 1);
    label(p, L_LANGUAGE, 430, 35, 90);
    combo(p, LANGUAGE, 524, 31, 150);
    add_item(p, LANGUAGE, "English");
    add_item(p, LANGUAGE, "Română");
    set_selection(p, LANGUAGE, p->lang == LANG_RO);
    control(p, HARDWARE, L"STATIC", SS_LEFT, 24, 66, 650, 34);
    control(p, DEVICES, L"STATIC", SS_LEFT, 24, 103, 650, 42);

    control(p, GROUP_REFERENCE, L"BUTTON", BS_GROUPBOX, 12, 157, 676, 318);
    label(p, L_PROCESSOR, 24, 183, 135);
    control(p, CPU, L"STATIC", SS_LEFT, 163, 183, 160, 21);
    control(p, FPU, L"BUTTON", WS_TABSTOP | BS_AUTOCHECKBOX, 330, 178, 316, 24);
    label(p, L_MEMORY, 24, 212, 135);
    HWND ram = control(p, MEMORY, L"EDIT", WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 163, 208, 90, 24);
    SendMessageW(ram, EM_SETLIMITTEXT, 4, 0);
    control(p, RAM, L"STATIC", SS_LEFT, 270, 212, 389, 21);

    label(p, L_CLOCK, 24, 243, 135);
    combo(p, CLOCK, 163, 239, 126);
    control(p, TM11, L"BUTTON", WS_TABSTOP | BS_AUTOCHECKBOX, 330, 238, 316, 24);

    label(p, L_SERIAL, 24, 274, 135);
    HWND serial = control(p, SERIAL, L"EDIT", WS_TABSTOP | ES_AUTOHSCROLL, 163, 270, 340, 24);
    SendMessageW(serial, EM_SETLIMITTEXT, RC_HOST_PATH + 14, 0);
    SetWindowTextA(serial, "tcp-listen:2323");
    label(p, L_FORMAT, 518, 274, 56);
    HWND format = control(p, FORMAT, L"EDIT", WS_TABSTOP | ES_AUTOHSCROLL, 578, 270, 96, 24);
    SendMessageW(format, EM_SETLIMITTEXT, 30, 0);
    SetWindowTextA(format, "9600,8N1");

    label(p, L_TAPE, 24, 305, 135);
    control(p, IMAGE, L"EDIT", ES_READONLY | ES_AUTOHSCROLL, 163, 301, 351, 24);
    button(p, BROWSE, 522, 300, 78);
    button(p, EJECT, 608, 300, 66);

    label(p, L_DISKS, 24, 338, 135);
    combo(p, DISKS, 163, 334, 264);
    label(p, L_DISK0, 24, 369, 135);
    control(p, DISK0, L"EDIT", ES_READONLY | ES_AUTOHSCROLL, 163, 365, 351, 24);
    button(p, BROWSE0, 522, 364, 78);
    button(p, EJECT0, 608, 364, 66);
    label(p, L_DISK1, 24, 400, 135);
    control(p, DISK1, L"EDIT", ES_READONLY | ES_AUTOHSCROLL, 163, 396, 351, 24);
    button(p, BROWSE1, 522, 395, 78);
    button(p, EJECT1, 608, 395, 66);

    label(p, L_BOOT, 24, 436, 135);
    combo(p, BOOT, 163, 432, 264);
    control(p, FULL_SPEED, L"BUTTON", WS_TABSTOP | BS_AUTOCHECKBOX, 440, 431, 234, 24);

    button(p, POWER, 12, 489, 145);
    button(p, RESTART, 169, 489, 98);
    button(p, PAUSE, 279, 489, 98);
    button(p, STOP, 389, 489, 98);
    control(p, STATUS, L"STATIC", SS_LEFT, 12, 528, 676, 21);
    relabel(p);
    set_profile(p);
    load_prefs(p);
    update_controls(p);
    show_status(p);
}

static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    Panel *p = (Panel *)GetWindowLongPtrW(window, GWLP_USERDATA);
    switch (message) {
    case WM_CREATE:
        p = ((CREATESTRUCTW *)lparam)->lpCreateParams;
        p->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)p);
        create_controls(p);
        DragAcceptFiles(window, TRUE);
        SetTimer(window, 1, 10, NULL);
        return 0;
    case WM_COMMAND:
        command(p, LOWORD(wparam), HIWORD(wparam));
        return 0;
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wparam;
        char path[RC_HOST_PATH];
        UINT length = DragQueryFileA(drop, 0, NULL, 0);
        if (length < sizeof(path) && DragQueryFileA(drop, 0, path, sizeof(path)))
            change_tape(p, path);
        DragFinish(drop);
        update_controls(p);
        return 0;
    }
    case WM_TIMER:
        if (p->powered) {
            uint64_t now = rc_host_now_us(), elapsed = now - p->last_tick;
            p->last_tick = now;
            if (elapsed > 50000)
                elapsed = 50000;
            unsigned steps = p->paused ? 0 : (unsigned)(elapsed * (RS232_HZ / RS232_TICK) / 1000000);
            if (!p->paused && checked(p, FULL_SPEED) && !p->session.machine.cpu.waiting) {
                /* As many instructions as fit in most of the timer period,
                 * while the guest computes; real time while it idles. */
                while (rc_host_now_us() - now < 8000 && !p->session.machine.cpu.waiting &&
                       session_run(&p->session, 20000, NULL))
                    ;
            } else {
                session_run(&p->session, steps, NULL);
            }
            if (now - p->last_status >= 100000) {
                p->last_status = now;
                show_status(p);
            }
        }
        return 0;
    case WM_DESTROY:
        KillTimer(window, 1);
        save_prefs(p);
        if (p->powered)
            session_close(&p->session);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int main_unit_window(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    DWORD processes[2];
    if (GetConsoleProcessList(processes, 2) == 1)
        FreeConsole();
    SetProcessDPIAware();
    HDC screen = GetDC(NULL);
    Panel *p = calloc(1, sizeof(*p));
    if (!p)
        return 1;
    p->dpi = screen ? GetDeviceCaps(screen, LOGPIXELSX) : 96;
    if (screen)
        ReleaseDC(NULL, screen);
    prefs_open(&p->prefs, "recif.yaml");
    static const char *const bsd[] = {"model", "4021", "disks", "rp06", "disk0", "disks\\unix211-rp06.dsk",
                                      "boot", "3,0", NULL};
    prefs_first_start(&p->prefs, bsd);
    p->lang = lang_choose(prefs_get(&p->prefs, "language", NULL));
    NONCLIENTMETRICSW metrics = {0};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
        p->font = CreateFontIndirectW(&metrics.lfMessageFont);
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW cls = {0};
    cls.lpfnWndProc = procedure;
    cls.hInstance = instance;
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    cls.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    cls.hbrBackground = (HBRUSH)(INT_PTR)(COLOR_BTNFACE + 1);
    cls.lpszClassName = L"RecifMainUnit";
    HWND window = NULL;
    if (RegisterClassW(&cls)) {
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        RECT bounds = {0, 0, scale(p, CLIENT_W), scale(p, CLIENT_H)};
        AdjustWindowRect(&bounds, style, FALSE);
        window = CreateWindowW(cls.lpszClassName, L"Recif", style, CW_USEDEFAULT, CW_USEDEFAULT,
                               bounds.right - bounds.left, bounds.bottom - bounds.top, NULL, NULL, instance, p);
    }
    if (window) {
        ShowWindow(window, SW_SHOWNORMAL);
        MSG message;
        while (GetMessageW(&message, NULL, 0, 0) > 0) {
            if (!IsDialogMessageW(window, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    } else {
        MessageBoxA(NULL, "Cannot open the main unit panel.", "Recif", MB_OK | MB_ICONERROR);
    }
    if (p->font)
        DeleteObject(p->font);
    free(p);
    return window ? 0 : 1;
}
