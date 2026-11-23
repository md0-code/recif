#include "machine_session.h"

#include <errno.h>
#include <stdlib.h>

bool read_unsigned(const char *text, int base, unsigned limit, unsigned *value)
{
    if (text[0] < '0' || text[0] > '9')
        return false;
    char *end;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, base);
    if (*end || errno || parsed > limit)
        return false;
    *value = (unsigned)parsed;
    return true;
}

bool session_open(MachineSession *s, const MachineConfig *config, const char *endpoint,
                  char *error, size_t size)
{
    *s = (MachineSession){0};
    if (!machine_init(&s->machine, config)) {
        snprintf(error, size, "Cannot configure machine memory or clock.");
        return false;
    }
    if (!serial_link_open(&s->serial, endpoint, &config->console, error, size)) {
        session_close(s);
        return false;
    }
    return true;
}

void session_close(MachineSession *s)
{
    serial_link_close(&s->serial);
    machine_destroy(&s->machine);
    *s = (MachineSession){0};
}

bool session_mount(MachineSession *s, const char *image, const char *lengths, char *error, size_t size)
{
    if (!s->machine.config.tm11) {
        snprintf(error, size, "Enable the TM11 controller to mount a tape.");
        return false;
    }
    if (!tape_open(&s->machine.tape.medium, image, lengths, error, size))
        return false;
    tm11_changed_medium(&s->machine.tape);
    return true;
}

void session_eject(MachineSession *s)
{
    tape_close(&s->machine.tape.medium);
    tm11_changed_medium(&s->machine.tape);
}

bool session_mount_disk(MachineSession *s, unsigned unit, const char *image, char *error, size_t size)
{
    return machine_disk_mount(&s->machine, unit, image, error, size);
}

void session_eject_disk(MachineSession *s, unsigned unit)
{
    machine_disk_eject(&s->machine, unit);
}

bool session_start(MachineSession *s, StartMode mode, const uint8_t *raw, size_t length,
                   uint16_t load, uint16_t entry, unsigned option, char *error, size_t size)
{
    if (!machine_start(&s->machine, mode, raw, length, load, entry, option, error, size))
        return false;
    s->serial.input = (Rs232Tx){0};
    rs232_rx_init(&s->serial.output, &s->machine.config.console);
    return true;
}

unsigned session_run(MachineSession *s, unsigned steps, FILE *transcript)
{
    serial_link_pump(&s->serial);
    if (s->serial.dropped || (!s->begun && s->serial.stream && !s->serial.connected))
        return 0;
    if (steps)
        s->begun = true;
    for (unsigned i = 0; i < steps; ++i) {
        s->wire_time += RS232_TICK;
        bool before = machine_txd(&s->machine, s->wire_time);
        bool rxd = serial_link_rx(&s->serial, s->wire_time, before, machine_receive_ready(&s->machine));
        machine_step(&s->machine, s->wire_time, rxd);
        bool txd = machine_txd(&s->machine, s->wire_time);
        if (transcript) {
            Rs232Received ch;
            if (rs232_sample(&s->serial.output, s->wire_time, txd, &ch) && !ch.framing && !ch.parity)
                fputc(ch.data, transcript);
        } else {
            serial_link_tx(&s->serial, s->wire_time, txd);
        }
    }
    return steps;
}

SessionStatus session_status(const MachineSession *s)
{
    if (s->serial.dropped)
        return SESSION_OUTPUT_ERROR;
    if (!s->begun && s->serial.stream)
        return SESSION_WAITING;
    if (s->machine.cpu.double_fault)
        return SESSION_DOUBLE_FAULT;
    if (s->machine.cpu.halted)
        return SESSION_HALTED;
    if (s->machine.cpu.waiting)
        return SESSION_WAIT;
    return SESSION_RUNNING;
}

const char *session_state(const MachineSession *s)
{
    static const char *const names[] = {
        "Running", "Serial output error", "Waiting for serial connection", "Double fault", "Halted",
        "Waiting for interrupt"
    };
    return names[session_status(s)];
}
