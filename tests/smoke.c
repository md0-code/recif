#include "machine.h"
#include "dafserial.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    DafSerial terminal;
    Machine machine;
    Rs232Rx monitor;
    uint64_t clock;
    char serial[2048];
    size_t used;
} Fixture;

static void run_cable(Fixture *f, unsigned steps)
{
    for (unsigned i = 0; i < steps; ++i) {
        f->clock += RS232_TICK;
        machine_step(&f->machine, f->clock, daf_serial_txd(&f->terminal, f->clock));
        bool txd = machine_txd(&f->machine, f->clock);
        daf_serial_step(&f->terminal, f->clock, txd);
        Rs232Received ch;
        if (rs232_sample(&f->monitor, f->clock, txd, &ch) && !ch.framing && !ch.parity &&
            f->used < sizeof(f->serial) - 1)
            f->serial[f->used++] = (char)ch.data;
    }
}

static bool check_wire(void)
{
    const Rs232Format tx_format = {1200, 7, 2, RS232_EVEN};
    const Rs232Format rx_format = {1200, 7, 1, RS232_ODD};
    Rs232Tx tx = {0};
    Rs232Rx rx;
    rs232_rx_init(&rx, &rx_format);
    rs232_send(&tx, &tx_format, RS232_TICK, 0x55);
    for (uint64_t now = RS232_TICK; !rs232_idle(&tx, now); now += RS232_TICK) {
        Rs232Received ch;
        if (rs232_sample(&rx, now, rs232_level(&tx, now), &ch))
            return ch.data == 0x55 && ch.parity && !ch.framing;
    }
    return false;
}

static bool write_tape(void)
{
    static const char record[] = "VOL1RECIF ";
    FILE *data = fopen("test.dat", "wb");
    if (!data)
        return false;
    bool ok = fwrite(record, 1, sizeof(record) - 1, data) == sizeof(record) - 1;
    ok = fclose(data) == 0 && ok;
    FILE *table = fopen("test.len", "wb");
    if (!table)
        return false;
    uint8_t lengths[262] = {'T', 'D', 'U', 'P', 0};
    lengths[256] = sizeof(record) - 1;
    ok = fwrite(lengths, 1, sizeof(lengths), table) == sizeof(lengths) && ok;
    return fclose(table) == 0 && ok;
}

static bool run_words(Machine *m, const uint16_t *program, size_t size)
{
    uint8_t bytes[128];
    char error[128];
    if (size > sizeof(bytes))
        return false;
    for (size_t i = 0; i < size / 2; ++i) {
        bytes[i * 2] = (uint8_t)program[i];
        bytes[i * 2 + 1] = (uint8_t)(program[i] >> 8);
    }
    if (!machine_start(m, START_RAW, bytes, size, 01000, 01000, 0, error, sizeof(error)))
        return false;
    for (uint64_t i = 1; i <= 64; ++i)
        machine_step(m, i * RS232_TICK, true);
    return m->cpu.halted && !m->cpu.double_fault;
}

static bool check_profiles(void)
{
    static const unsigned ram[] = {56, 248, 2048, 4088};
    static const uint16_t vectors[] = {0010, 0010, 0, 0};
    static const uint16_t program[] = {
        012706, 01000,
        012737, 02000, 0010,     /* reserved instruction goes to a HALT */
        012700, 00003,
        070027, 00002,           /* MUL #2,R0, result in R0/R1 */
        000007,                  /* MFPT */
        000000
    };
    static const uint16_t unified_map[] = {
        012737, 077406, 0172300,  /* map instruction page zero */
        012737, 077406, 0172304,  /* map virtual page two */
        012737, 002000, 0172344,  /* page two starts above 64 KiB */
        012737, 000001, 0177572,  /* enable relocation */
        012737, 001234, 040000,
        000000
    };
    for (unsigned i = 0; i < CORAL_MODEL_COUNT; ++i) {
        MachineConfig config = machine_defaults(&coral_profiles[i]);
        config.tm11 = false;
        config.clock_hz = 0;
        Machine m;
        if (!machine_init(&m, &config))
            return false;
        bool ok = run_words(&m, program, sizeof(program)) && m.ram_size / 1024 == ram[i] &&
                  m.cpu.last_vector == vectors[i] && m.cpu.r[1] == (i ? 6 : 0) &&
                  m.cpu.r[0] == (i >= 2 ? 1 : i ? 0 : 3);
        uint16_t value;
        ok = ok && !m.cpu.bus.access(&m, m.ram_size, false, false, &value) &&
             !m.cpu.bus.access(&m, PDP_IO_PAGE + 012520, false, false, &value) &&
             !m.cpu.bus.access(&m, PDP_IO_PAGE + 017546, false, false, &value);
        if (ok && config.profile->cpu == PDP_34) {
            ok = run_words(&m, unified_map, sizeof(unified_map)) && !m.cpu.last_vector &&
                 m.ram[040000] == 0 && m.ram[0200000] == (001234 & 0377) &&
                 m.ram[0200001] == (001234 >> 8);
        }
        machine_destroy(&m);
        if (!ok)
            return false;
    }
    return true;
}

static bool check_mmu(Machine *m)
{
    static const uint16_t program[] = {
        012737, 077406, 0172300,  /* kernel I page zero, full range */
        012737, 077406, 0172320,  /* kernel D page zero, full range */
        012737, 010000, 0172360,  /* kernel D page zero starts above 18-bit memory */
        012737, 000024, 0172516,  /* split kernel I/D, 22-bit relocation */
        012737, 000001, 0177572,  /* enable relocation */
        012737, 001234, 000400,  /* write through the D map */
        000000
    };
    return run_words(m, program, sizeof(program)) && !m->cpu.last_vector && m->ram[0400] == 0 &&
           m->ram[01000400] == (001234 & 0377) && m->ram[01000401] == (001234 >> 8);
}

static bool check_tek(Daf *d)
{
    daf_reset(d, DAF_DEFAULT_SWITCHES, NULL, NULL);
    const uint8_t draw[] = {29, 34, 96, 33, 64, 34, 96, 34, 64, 31};
    for (size_t i = 0; i < sizeof(draw); ++i)
        if (!daf_receive(d, draw[i]))
            return false;
    daf_drain(d, DAF_FIFO_SIZE);
    bool ok = !d->vt_mode && d->bitmap[32][24] == 1 && d->bitmap[32][33] == 0 &&
              d->x == 32 && d->y == 32;
    /* After SI (power-on) lower-case codes print as capitals; ESC X 1
     * enters VT100 and ESC 1 returns. */
    daf_reset(d, DAF_DEFAULT_SWITCHES, NULL, NULL);
    daf_puts(d, "aA");
    ok = ok && !memcmp(d->bitmap[383], d->bitmap[383] + 7, 7) && d->x == 14;
    daf_puts(d, "\033X1");
    ok = ok && d->vt_mode;
    daf_puts(d, "\033" "1");
    return ok && !d->vt_mode;
}

/* FP11: (1.5 + 2.25) * 2.0 stored as F. */
static bool check_fpu(void)
{
    static const uint16_t program[] = {
        0170001,                 /* SETF */
        0172427, 0040300,        /* LDF #1.5,AC0 */
        0172027, 0040420,        /* ADDF #2.25,AC0 */
        0171027, 0040400,        /* MULF #2.0,AC0 */
        0174037, 0002000,        /* STF AC0,@#2000 */
        0000000
    };
    MachineConfig config = machine_defaults(coral_find("4021"));
    config.clock_hz = 0;
    Machine m;
    if (!config.fpu || !machine_init(&m, &config))
        return false;
    bool ok = run_words(&m, program, sizeof(program)) && !m.cpu.last_vector &&
              m.ram[02000] == (040760 & 0377) && m.ram[02001] == (040760 >> 8) &&
              m.ram[02002] == 0 && m.ram[02003] == 0;
    machine_destroy(&m);
    return ok;
}

/* CORAL 4030: MMR3 also answers at 165410 and the RH11 has a BAE register. */
static bool check_4030_io(void)
{
    static const uint16_t program[] = {
        012737, 000020, 0165410,  /* 22-bit relocation through the CORAL address */
        013703, 0172516,          /* MOV @#172516,R3 */
        012737, 000007, 0176750,  /* BAE */
        013704, 0176750,          /* MOV @#176750,R4 */
        0000000
    };
    MachineConfig config = machine_defaults(coral_find("4030"));
    config.clock_hz = 0;
    Machine m;
    if (!machine_init(&m, &config))
        return false;
    bool ok = run_words(&m, program, sizeof(program)) && !m.cpu.last_vector && m.cpu.r[3] == 020 &&
              m.cpu.r[4] == 7 && pdp_dma_address(&m.cpu, 07001000) == 07001000;
    machine_destroy(&m);
    return ok;
}

/* Disk bootstraps: the first block runs with R0 = unit and R1 = CSR. */
static bool check_disk(const char *model, uint16_t csr)
{
    static const uint8_t block[] = {0302, 025, 0123, 0247, 0, 0}; /* MOV #123523,R2 then HALT */
    FILE *image = fopen("test.dsk", "wb");
    if (!image)
        return false;
    bool ok = fwrite(block, 1, sizeof(block), image) == sizeof(block);
    ok = fclose(image) == 0 && ok;
    MachineConfig config = machine_defaults(coral_find(model));
    config.clock_hz = 0;
    Machine m;
    char error[128];
    if (!ok || !machine_init(&m, &config))
        return false;
    ok = machine_disk_mount(&m, 0, "test.dsk", error, sizeof(error)) &&
         machine_start(&m, START_DISK, NULL, 0, 0, 0, 0, error, sizeof(error));
    for (uint64_t i = 1; ok && i <= 20000 && !m.cpu.halted; ++i)
        machine_step(&m, i * RS232_TICK, true);
    ok = ok && m.cpu.halted && m.cpu.r[2] == 0123523 && m.cpu.r[0] == 0 && m.cpu.r[1] == csr;
    machine_destroy(&m);
    remove("test.dsk");
    return ok;
}

int main(void)
{
    Fixture *f = calloc(1, sizeof(*f));
    if (!f)
        return 1;
    MachineConfig config = machine_defaults(coral_find("4030"));
    bool ok = machine_init(&f->machine, &config);
    daf_serial_reset(&f->terminal, DAF_DEFAULT_SWITCHES);
    rs232_rx_init(&f->monitor, &rs232_console_format);
    char err[256] = "";
    if (ok)
        ok = write_tape() && tape_open(&f->machine.tape.medium, "test.dat", NULL, err, sizeof(err)) &&
            machine_start(&f->machine, START_MONITOR, NULL, 0, 0, 0, 0, err, sizeof(err));
    if (ok) {
        run_cable(f, 50000 * 48 / RS232_TICK);
        daf_key_char(&f->terminal.display, 'r');
        run_cable(f, 10000 * 48 / RS232_TICK);
        ok = !f->terminal.display.vt_mode && !f->machine.cpu.halted &&
            f->machine.tape.medium.position == 1 && !memcmp(f->machine.ram + 04000, "VOL1RECIF ", 10) &&
            strstr(f->serial, "Recif monitor") && strstr(f->serial, "TM0: VOL1RECIF");
    }

    if (ok) {
        const char *failed = !check_profiles() ? "model profile" : !check_mmu(&f->machine) ? "split I/D" :
            !check_tek(&f->terminal.display) ? "DAF-2020" : !check_wire() ? "RS-232 parity" :
            !check_fpu() ? "FP11" : !check_disk("4030", 0176700) ? "RH11 bootstrap" :
            !check_disk("4021", 0176714) ? "RP11 bootstrap" : !check_4030_io() ? "CORAL 4030 I/O" : NULL;
        ok = !failed;
        if (failed)
            snprintf(err, sizeof(err), "The %s check failed", failed);
    }
    if (!ok)
        fprintf(stderr, "%s\nPC=%06o vector=%06o tape=%zu\n%s\n", err, f->machine.cpu.r[7],
                f->machine.cpu.last_vector, f->machine.tape.medium.position, f->serial);
    machine_destroy(&f->machine);
    free(f);
    remove("test.dat");
    remove("test.len");
    return ok ? 0 : 1;
}
