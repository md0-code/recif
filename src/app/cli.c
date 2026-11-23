#include "bru.h"
#include "machine_session.h"
#include "rc_version.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#ifdef RECIF_CONTROL_PANEL
#include "main_unit.h"
#endif

static void usage(FILE *out)
{
    fputs("Recif " RECIF_VERSION " | CORAL main unit\n"
          "recif models [4001|4011|4021|4030]\n"
          "recif tape-info IMAGE [LENGTHS]\n"
          "recif tape-convert IMAGE OUTPUT.tap [LENGTHS]\n"
          "recif bru-list IMAGE [LENGTHS]\n"
          "recif bru-extract IMAGE DIRECTORY [LENGTHS]\n"
          "recif bru-restore IMAGE DISK [LENGTHS] [--disk rp06|rm03|rp03]\n"
          "recif run [--model 4001|4011|4021|4030] [--ram KIB] [--clock 50|60|off]\n"
          "          [--fpu | --no-fpu] [--no-tm11] [--disks none|rm03|rp06|rp03]\n"
          "          [--tape IMAGE [--lengths LENGTHS]] [--disk IMAGE] [--disk1 IMAGE]\n"
          "          [--boot-tape | --boot-first | --boot-disk 0|1]\n"
          "          [--raw IMAGE --load OCTAL --entry OCTAL] [--steps N] [--input TEXT]\n"
          "          [--serial ENDPOINT] [--serial-format 9600,8N1] [--full-speed]\n"
          "Run uses a DEC compatibility core and controller configuration.\n"
          "Serial endpoints: " SERIAL_ENDS "\n", out);
}

static int list_models(int argc, char **argv)
{
    const CoralProfile *selected = argc == 3 ? coral_find(argv[2]) : NULL;
    if (argc > 3 || (argc == 3 && !selected)) {
        usage(stderr);
        return 2;
    }
    for (unsigned i = 0; i < CORAL_MODEL_COUNT; ++i) {
        const CoralProfile *profile = &coral_profiles[i];
        if (selected && selected != profile)
            continue;
        MachineConfig config = machine_defaults(profile);
        printf("%s\n  %s\n  %s\n  %s reference%s, %u KiB addressable RAM%s%s\n",
               profile->name, profile->hardware, profile->peripherals,
               pdp_reference_name(profile->cpu), config.fpu ? " with FP11" : "",
               machine_ram_kib(&config), config.tm11 ? ", TM11" : "",
               config.disks == DISKS_RP03 ? ", RP11 with RP03" : config.disks == DISKS_RP06 ?
               ", RH11 with RP06" : config.disks == DISKS_RM03 ? ", RH11 with RM03" : "");
    }
    return 0;
}

typedef enum { INFO, CONVERT, CATALOG, EXTRACT, RESTORE } TapeCommand;

static int tape_command(int argc, char **argv, TapeCommand command)
{
    DiskSystem disks = DISKS_RP06;
    if (command == RESTORE && argc >= 2 && !strcmp(argv[argc - 2], "--disk")) {
        if (!disk_system_parse(argv[argc - 1], &disks) || disks == DISKS_NONE) {
            usage(stderr);
            return 2;
        }
        argc -= 2;
    }
    int required = command == INFO || command == CATALOG ? 3 : 4;
    if (argc < required || argc > required + 1) {
        usage(stderr);
        return 2;
    }

    char err[512];
    Tape tape = {0};
    if (!tape_open(&tape, argv[2], argc > required ? argv[required] : NULL, err, sizeof(err))) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    bool ok = true;
    if (command == CONVERT) {
        FILE *out = fopen(argv[3], "wbx");
        if (!out) {
            fprintf(stderr, "Cannot create %s: %s\n", argv[3], strerror(errno));
            ok = false;
        } else {
            ok = tape_export(&tape, out);
            if (fclose(out))
                ok = false;
            if (!ok) {
                remove(argv[3]);
                fprintf(stderr, "Could not write %s\n", argv[3]);
            }
        }
    }
    if (ok)
        tape_report(&tape, stdout);
    if (ok && command == RESTORE) {
        uint32_t blocks = disks == DISKS_RP03 ? rp11_blocks() :
            rh11_blocks(disks == DISKS_RM03 ? DISK_RM03 : DISK_RP06);
        ok = bru_restore(&tape, argv[3], blocks, stdout, err, sizeof(err));
        if (ok)
            printf("%s pack image %s, %" PRIu32 " blocks\n", disk_system_name(disks), argv[3], blocks);
        else
            fprintf(stderr, "%s\n", err);
    }
    if (ok && (command == CATALOG || command == EXTRACT)) {
        ok = bru_catalog(&tape, command == EXTRACT ? argv[3] : NULL, stdout, err, sizeof(err));
        if (!ok)
            fprintf(stderr, "%s\n", err);
    }
    tape_close(&tape);
    return ok ? 0 : 1;
}

static bool run_console(MachineSession *s, FILE *transcript, const char *input, uint64_t limit, bool full)
{
    if (input)
        for (; *input; ++input)
            rs232_queue_put(&s->serial.incoming, (uint8_t)*input);

    unsigned port = rc_host_stream_local_port(s->serial.stream);
    if (port)
        fprintf(stderr, "Serial listening on port %u\n", port);
    uint64_t executed = 0, next = rc_host_now_us();
    while (executed < limit && !s->serial.dropped) {
        unsigned batch = limit - executed < 500 ? (unsigned)(limit - executed) : 500;
        batch = session_run(s, batch, transcript);
        executed += batch;
        if (!batch) {
            rc_host_sleep_us(10000);
            next = rc_host_now_us();
        } else if (full && !s->machine.cpu.waiting) {
            /* Full speed while the guest computes; real time while it idles. */
            next = rc_host_now_us();
        } else if (s->serial.stream) {
            next += (uint64_t)batch * RS232_TICK * 1000000 / RS232_HZ;
            uint64_t real = rc_host_now_us();
            if (next > real)
                rc_host_sleep_us((uint32_t)(next - real));
            else if (real - next > 50000)
                next = real;
        }
    }
    session_run(s, 0, transcript);
    return !s->serial.dropped;
}

static int run_command(int argc, char **argv)
{
    const char *image = NULL, *lengths = NULL, *raw_path = NULL, *input = NULL, *endpoint = NULL;
    const char *disks[MACHINE_DISK_UNITS] = {NULL};
    const CoralProfile *profile = coral_find("4030");
    MachineConfig config = machine_defaults(profile);
    unsigned memory = 0, option = 0, clock_hz = config.clock_hz;
    unsigned steps = 250000, load = 01000, entry = 01000;
    int fpu = -1, disk_system = -1;
    bool entry_set = false, steps_set = false, tape_off = false, full = false;
    StartMode start = START_MONITOR;
    for (int i = 2; i < argc; ++i) {
        const char *opt = argv[i];
        if (!strcmp(opt, "--no-tm11")) {
            tape_off = true;
            continue;
        }
        if (!strcmp(opt, "--full-speed")) {
            full = true;
            continue;
        }
        if (!strcmp(opt, "--fpu") || !strcmp(opt, "--no-fpu")) {
            fpu = !strcmp(opt, "--fpu");
            continue;
        }
        if (!strcmp(opt, "--boot-tape") || !strcmp(opt, "--boot-first")) {
            if (start != START_MONITOR)
                goto wrong_args;
            start = START_TAPE;
            option = !strcmp(opt, "--boot-first");
            continue;
        }
        if (++i == argc)
            goto wrong_args;
        const char *arg = argv[i];
        if (!strcmp(opt, "--model")) {
            profile = coral_find(arg);
            if (!profile)
                goto wrong_args;
        } else if (!strcmp(opt, "--boot-disk")) {
            if (start != START_MONITOR || !read_unsigned(arg, 10, MACHINE_DISK_UNITS - 1, &option))
                goto wrong_args;
            start = START_DISK;
        } else if (!strcmp(opt, "--disk")) {
            disks[0] = arg;
        } else if (!strcmp(opt, "--disk1")) {
            disks[1] = arg;
        } else if (!strcmp(opt, "--disks")) {
            DiskSystem parsed;
            if (!disk_system_parse(arg, &parsed))
                goto wrong_args;
            disk_system = (int)parsed;
        } else if (!strcmp(opt, "--ram")) {
            if (!read_unsigned(arg, 10, 4096, &memory) || memory < 4)
                goto wrong_args;
        } else if (!strcmp(opt, "--clock")) {
            if (!strcmp(arg, "off"))
                clock_hz = 0;
            else if (!strcmp(arg, "50") || !strcmp(arg, "60"))
                clock_hz = arg[0] == '5' ? 50u : 60u;
            else
                goto wrong_args;
        } else if (!strcmp(opt, "--tape"))
            image = arg;
        else if (!strcmp(opt, "--lengths"))
            lengths = arg;
        else if (!strcmp(opt, "--raw")) {
            if (start != START_MONITOR)
                goto wrong_args;
            start = START_RAW;
            raw_path = arg;
        } else if (!strcmp(opt, "--input"))
            input = arg;
        else if (!strcmp(opt, "--serial"))
            endpoint = arg;
        else if (!strcmp(opt, "--serial-format")) {
            if (!rs232_parse_format(arg, &config.console))
                goto wrong_args;
        } else if (!strcmp(opt, "--steps")) {
            if (!read_unsigned(arg, 10, 100000000, &steps) || !steps)
                goto wrong_args;
            steps_set = true;
        } else if (!strcmp(opt, "--load")) {
            if (!read_unsigned(arg, 8, 0177777, &load))
                goto wrong_args;
        } else if (!strcmp(opt, "--entry")) {
            if (!read_unsigned(arg, 8, 0177777, &entry))
                goto wrong_args;
            entry_set = true;
        } else {
            goto wrong_args;
        }
    }
    if (!entry_set)
        entry = load;
    Rs232Format console = config.console;
    config = machine_defaults(profile);
    config.console = console;
    config.clock_hz = clock_hz;
    if (fpu >= 0)
        config.fpu = fpu != 0;
    bool has_disk = disks[0] || disks[1];
    if (disk_system >= 0)
        config.disks = (DiskSystem)disk_system;
    else if (has_disk && config.disks == DISKS_NONE)
        config.disks = DISKS_RM03;
    if ((lengths && !image) || (start == START_TAPE && !image) || (image && tape_off) ||
        (start == START_DISK && !disks[option]) || (has_disk && config.disks == DISKS_NONE) ||
        (config.fpu && !pdp_fpu_option(profile->cpu)) ||
        (input && (strlen(input) > RS232_BUFFER || (endpoint && strcmp(endpoint, "none")))))
        goto wrong_args;
    config.memory_kib = memory ? memory : config.profile->nominal_kib;
    config.tm11 = !tape_off && (config.profile->tape_reported || image);
    if (!machine_ram_kib(&config)) {
        fprintf(stderr, "%s accepts 4 to %u KiB for this reference configuration.\n",
                config.profile->name, config.profile->capacity_kib);
        return 2;
    }

    char err[512];
    MachineSession session;
    if (!session_open(&session, &config, endpoint, err, sizeof(err))) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    Machine *machine = &session.machine;
    uint8_t *raw = NULL;
    size_t size = 0;
    bool ok = !image || session_mount(&session, image, lengths, err, sizeof(err));
    for (unsigned u = 0; ok && u < MACHINE_DISK_UNITS; ++u)
        if (disks[u])
            ok = session_mount_disk(&session, u, disks[u], err, sizeof(err));
    if (ok && raw_path) {
        raw = read_image(raw_path, &size, err, sizeof(err));
        ok = raw != NULL;
    }
    if (ok)
        ok = session_start(&session, start, raw, size, (uint16_t)load, (uint16_t)entry, option, err, sizeof(err));
    free(raw);
    if (ok) {
        uint64_t limit = !steps_set && session.serial.stream ? UINT64_MAX : steps;
        FILE *transcript = !endpoint || !strcmp(endpoint, "none") ? stdout : NULL;
        ok = run_console(&session, transcript, input, limit, full);
        if (!ok)
            snprintf(err, sizeof(err), "Serial output overrun or write error.");
    }
    if (ok) {
        fflush(stdout);
        fprintf(stderr, "%s  %s reference  RAM=%u KiB  %s  PC=%06o  PS=%06o  instructions=%" PRIu64
                "  tape=%zu/%zu", config.profile->name, pdp_reference_name(config.profile->cpu),
                machine->ram_size / 1024, session_state(&session), machine->cpu.r[7], machine->cpu.psw,
                machine->cpu.instructions, machine->tape.medium.position, machine->tape.medium.count);
        if (machine->cpu.last_vector)
            fprintf(stderr, "  vector=%06o  address=%08" PRIo32, machine->cpu.last_vector, machine->cpu.last_address);
        fputc('\n', stderr);
        ok = !machine->cpu.double_fault;
    } else {
        fprintf(stderr, "%s\n", err);
    }
    session_close(&session);
    return ok ? 0 : 1;

wrong_args:
    usage(stderr);
    return 2;
}

int main(int argc, char **argv)
{
#ifdef RECIF_CONTROL_PANEL
    if (argc == 1)
        return main_unit_window(argc, argv);
#ifndef _WIN32
    if (!strcmp(argv[1], "--hidden"))
        return main_unit_window(argc, argv);
#endif
#endif
#ifdef _WIN32
    /* Preserve exact serial output, including the guest CR/LF bytes. */
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (argc < 2 || !strcmp(argv[1], "--help")) {
        usage(stdout);
        return 0;
    }
    if (!strcmp(argv[1], "--version")) {
        puts(RECIF_VERSION);
        return 0;
    }
    if (!strcmp(argv[1], "run"))
        return run_command(argc, argv);
    if (!strcmp(argv[1], "models"))
        return list_models(argc, argv);
    if (!strcmp(argv[1], "tape-info"))
        return tape_command(argc, argv, INFO);
    if (!strcmp(argv[1], "tape-convert"))
        return tape_command(argc, argv, CONVERT);
    if (!strcmp(argv[1], "bru-list"))
        return tape_command(argc, argv, CATALOG);
    if (!strcmp(argv[1], "bru-extract"))
        return tape_command(argc, argv, EXTRACT);
    if (!strcmp(argv[1], "bru-restore"))
        return tape_command(argc, argv, RESTORE);
    usage(stderr);
    return 2;
}
