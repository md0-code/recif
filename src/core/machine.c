#include "machine.h"

#include "guest.h"

#include <stdlib.h>
#include <string.h>

enum {
    DONE = 0200, IE = 0100, ACTIVE = 04000, BREAK = 1,
    RX_ERROR = 0100000, RX_OVERRUN = 040000, RX_FRAME = 020000, RX_PARITY = 010000,
    MACHINE_HZ = RS232_HZ / RS232_TICK
};

static void reset_bus(void *context)
{
    Machine *m = context;
    m->rxcsr = 0;
    m->txcsr = DONE;
    m->clock_csr = DONE;
    m->rx_irq = m->tx_irq = m->clock_irq = false;
    m->tx_pending = false;
    m->rx = 0;
    m->console_tx = (Rs232Tx){0};
    rs232_rx_init(&m->console_rx, &m->config.console);
    m->clock_phase = 0;
    tm11_reset(&m->tape);
    rh11_reset(&m->massbus);
    rp11_reset(&m->pack);
}

static void update_irq(uint16_t old, uint16_t next, bool *request)
{
    if ((next & (IE | DONE)) != (IE | DONE))
        *request = false;
    else if ((old & (IE | DONE)) != (IE | DONE))
        *request = true;
}

static bool basic_io(Machine *m, uint16_t a, bool write, bool byte, uint16_t *value);

static bool io(Machine *m, uint16_t a, bool write, bool byte, uint16_t *value)
{
    if (a >= 0172520 && a <= 0172533)
        return m->config.tm11 && tm11_io(&m->tape, a, write, byte, value, m->time);
    if (m->config.disks == DISKS_RP03)
        return rp11_io(&m->pack, a, write, byte, value, m->time) || basic_io(m, a, write, byte, value);
    if (m->config.disks != DISKS_NONE && rh11_io(&m->massbus, a, write, byte, value, m->time))
        return true;
    return basic_io(m, a, write, byte, value);
}

static bool basic_io(Machine *m, uint16_t a, bool write, bool byte, uint16_t *value)
{
    bool high = byte && (a & 1);
    switch (a & 0177776) {
    case 0177546:
        if (!m->config.clock_hz)
            return false;
        if (write && !high) {
            uint16_t next = (uint16_t)(*value & (IE | DONE) & (m->clock_csr | IE));
            update_irq(m->clock_csr, next, &m->clock_irq);
            m->clock_csr = next;
        } else if (!write) {
            *value = high ? 0 : m->clock_csr;
        }
        return true;
    case 0177560: case 0177564: {
        bool receive = (a & 6) == 0;
        uint16_t *csr = receive ? &m->rxcsr : &m->txcsr;
        if (write && !high) {
            uint16_t mask = receive ? IE : IE | BREAK;
            uint16_t next = (uint16_t)((*csr & DONE) | (*value & mask));
            update_irq(*csr, next, receive ? &m->rx_irq : &m->tx_irq);
            *csr = next;
        } else if (!write) {
            uint16_t state = *csr;
            if (receive && m->console_rx.active)
                state |= ACTIVE;
            *value = byte ? (high ? state >> 8 : state & 0377) : state;
        }
        return true;
    }
    case 0177562:
        if (!write) {
            *value = byte ? (high ? m->rx >> 8 : m->rx & 0377) : m->rx;
            if (!high) {
                m->rxcsr &= (uint16_t)~DONE;
                m->rx_irq = false;
            }
        }
        return true;
    case 0177566:
        if (!write) {
            *value = 0;
        } else if (!high && (m->txcsr & DONE)) {
            m->tx = (uint8_t)*value;
            m->txcsr &= (uint16_t)~DONE;
            m->tx_irq = false;
            m->tx_pending = true;
        }
        return true;
    case 0177570:
        if (!write)
            *value = 0;
        return true;
    default: return false;
    }
}

static bool bus_access(void *context, uint32_t address, bool write, bool byte, uint16_t *value)
{
    Machine *m = context;
    if (address >= PDP_IO_PAGE)
        return io(m, (uint16_t)address, write, byte, value);
    if (address >= m->ram_size || (!byte && address + 1 >= m->ram_size))
        return false;
    if (write) {
        m->ram[address] = (uint8_t)*value;
        if (!byte)
            m->ram[address + 1] = (uint8_t)(*value >> 8);
    } else {
        *value = m->ram[address];
        if (!byte)
            *value |= (uint16_t)((unsigned)m->ram[address + 1] << 8);
    }
    return true;
}

static unsigned bus_interrupt(void *context, unsigned ipl, bool acknowledge)
{
    Machine *m = context;
    bool *request;
    unsigned vector;
    bool rh = m->config.disks == DISKS_RM03 || m->config.disks == DISKS_RP06;
    bool rp = m->config.disks == DISKS_RP03;
    if (ipl < 5 && !(ipl < 6 && m->clock_irq) &&
        ((rh && rh11_interrupt(&m->massbus)) || (rp && rp11_interrupt(&m->pack)))) {
        if (acknowledge && rh)
            rh11_acknowledge(&m->massbus);
        else if (acknowledge)
            rp11_acknowledge(&m->pack);
        return rh ? RH11_VECTOR : RP11_VECTOR;
    }
    if (ipl < 6 && m->clock_irq)      { vector = 0100; request = &m->clock_irq; }
    else if (ipl < 5 && m->tape.irq)  { vector = 0224; request = &m->tape.irq; }
    else if (ipl < 4 && m->rx_irq)    { vector = 0060; request = &m->rx_irq; }
    else if (ipl < 4 && m->tx_irq)    { vector = 0064; request = &m->tx_irq; }
    else return 0;
    if (acknowledge)
        *request = false;
    return vector;
}

MachineConfig machine_defaults(const CoralProfile *profile)
{
    return (MachineConfig){profile, rs232_console_format, profile->nominal_kib, 50, profile->disks,
                           profile->tape_reported, profile->fpu_reported};
}

unsigned machine_ram_kib(const MachineConfig *config)
{
    if (!config->profile || config->memory_kib < 4 || config->memory_kib > config->profile->capacity_kib)
        return 0;
    unsigned limit = pdp_ram_limit(config->profile->cpu) / 1024;
    return config->memory_kib < limit ? config->memory_kib : limit;
}

bool machine_init(Machine *m, const MachineConfig *config)
{
    memset(m, 0, sizeof(*m));
    m->config = *config;
    m->config.fpu = config->fpu && pdp_fpu_option(config->profile->cpu);
    m->massbus.type = config->disks == DISKS_RP06 ? DISK_RP06 : DISK_RM03;
    for (unsigned u = 0; u < RH11_DRIVES; ++u)
        m->massbus.drive[u].type = m->massbus.type;
    m->massbus.extended = config->profile->direct_dma;
    m->ram_size = machine_ram_kib(config) * 1024;
    if (!m->ram_size || (config->clock_hz && config->clock_hz != 50 && config->clock_hz != 60))
        return false;
    m->ram = calloc(1, m->ram_size);
    return m->ram != NULL;
}

void machine_destroy(Machine *m)
{
    tape_close(&m->tape.medium);
    rh11_close(&m->massbus);
    rp11_close(&m->pack);
    free(m->ram);
}

bool machine_start(Machine *m, StartMode mode, const uint8_t *raw, size_t length,
                   uint16_t load, uint16_t entry, unsigned option, char *err, size_t errlen)
{
    if (mode == START_TAPE && (!m->config.tm11 || !m->tape.medium.bytes)) {
        snprintf(err, errlen, "Mount a tape before booting TM0.");
        return false;
    }
    if (mode == START_DISK && !machine_disk_mounted(m, option)) {
        snprintf(err, errlen, "Mount a disk image on unit %u before booting it.", option);
        return false;
    }
    unsigned boot_limit = m->ram_size < PDP_IO_VIRTUAL ? m->ram_size : PDP_IO_VIRTUAL;
    if (mode == START_RAW && (!length || (entry & 1) || entry >= boot_limit ||
                              load >= boot_limit || length > boot_limit - load)) {
        snprintf(err, errlen, "Invalid binary load range or entry address (unmapped RAM 000000-%06o).",
                 boot_limit - 1);
        return false;
    }
    m->time = 0;
    memset(m->ram, 0, m->ram_size);
    reset_bus(m);
    if (mode == START_MONITOR) {
        entry = guest_monitor(m->ram, m->config.tm11);
    } else if (mode == START_TAPE) {
        tm11_changed_medium(&m->tape);
        entry = guest_tape_boot(m->ram, option != 0);
    } else if (mode == START_DISK) {
        entry = m->config.disks == DISKS_RP03 ? guest_rp_boot(m->ram, option) : guest_disk_boot(m->ram, option);
    } else {
        memcpy(m->ram + load, raw, length);
    }
    pdp_start(&m->cpu, m->config.profile->cpu, m->config.fpu,
              (PdpBus){m, bus_access, bus_interrupt, reset_bus}, entry);
    m->cpu.mmr3_alias = m->config.profile->mmr3_alias;
    m->cpu.direct_dma = m->config.profile->direct_dma;
    return true;
}

bool machine_txd(const Machine *m, uint64_t now)
{
    return !(m->txcsr & BREAK) && rs232_level(&m->console_tx, now);
}

bool machine_receive_ready(const Machine *m)
{
    return !(m->rxcsr & DONE);
}

static void advance_devices(Machine *m, uint64_t now, bool rxd)
{
    ++m->time;
    m->clock_phase += m->config.clock_hz;
    if (m->clock_phase >= MACHINE_HZ) {
        m->clock_phase -= MACHINE_HZ;
        m->clock_csr |= DONE;
        m->clock_irq = (m->clock_csr & IE) != 0;
    }
    Rs232Received received;
    if (rs232_sample(&m->console_rx, now, rxd, &received)) {
        uint16_t errors = (m->rxcsr & DONE ? RX_OVERRUN : 0) |
                          (received.parity ? RX_PARITY : 0) | (received.framing ? RX_FRAME : 0);
        m->rx = (uint16_t)(received.data | errors | (errors ? RX_ERROR : 0));
        m->rxcsr |= DONE;
        m->rx_irq = (m->rxcsr & IE) != 0;
    }
    if (m->tx_pending && rs232_idle(&m->console_tx, now)) {
        rs232_send(&m->console_tx, &m->config.console, now, m->tx);
        m->tx_pending = false;
        m->txcsr |= DONE;
        m->tx_irq = (m->txcsr & IE) != 0;
    }
    if (m->tape.ready_at)
        tm11_tick(&m->tape, &m->cpu, m->time);
    if (m->config.disks == DISKS_RP03) {
        if (rp11_busy(&m->pack))
            rp11_tick(&m->pack, &m->cpu, m->time);
    } else if (m->config.disks != DISKS_NONE && rh11_busy(&m->massbus)) {
        rh11_tick(&m->massbus, &m->cpu, m->time);
    }
}

void machine_step(Machine *m, uint64_t now, bool rxd)
{
    advance_devices(m, now, rxd);
    pdp_step(&m->cpu);
}

bool machine_disk_mount(Machine *m, unsigned unit, const char *image, char *err, size_t errlen)
{
    if (m->config.disks == DISKS_NONE || unit >= MACHINE_DISK_UNITS) {
        snprintf(err, errlen, "Choose a disk controller to mount a disk.");
        return false;
    }
    if (m->config.disks == DISKS_RP03)
        return rp11_mount(&m->pack, unit, image, err, errlen);
    return rh11_mount(&m->massbus, unit, image, err, errlen);
}

void machine_disk_eject(Machine *m, unsigned unit)
{
    if (m->config.disks == DISKS_RP03)
        rp11_eject(&m->pack, unit);
    else if (m->config.disks != DISKS_NONE)
        rh11_eject(&m->massbus, unit);
}

bool machine_disk_mounted(const Machine *m, unsigned unit)
{
    if (m->config.disks == DISKS_NONE || unit >= MACHINE_DISK_UNITS)
        return false;
    return m->config.disks == DISKS_RP03 ? m->pack.drive[unit].file != NULL : m->massbus.drive[unit].file != NULL;
}

bool machine_disk_busy(const Machine *m)
{
    if (m->config.disks == DISKS_RP03)
        return rp11_busy(&m->pack);
    return m->config.disks != DISKS_NONE && rh11_busy(&m->massbus);
}
