#ifndef RECIF_MACHINE_SESSION_H
#define RECIF_MACHINE_SESSION_H

#include "machine.h"
#include "serial_link.h"

typedef struct {
    Machine machine;
    SerialLink serial;
    uint64_t wire_time;
    bool begun;
} MachineSession;

bool session_open(MachineSession *s, const MachineConfig *config, const char *endpoint,
                  char *error, size_t size);
void session_close(MachineSession *s);
bool session_mount(MachineSession *s, const char *image, const char *lengths, char *error, size_t size);
void session_eject(MachineSession *s);
bool session_mount_disk(MachineSession *s, unsigned unit, const char *image, char *error, size_t size);
void session_eject_disk(MachineSession *s, unsigned unit);
bool session_start(MachineSession *s, StartMode mode, const uint8_t *raw, size_t length,
                   uint16_t load, uint16_t entry, unsigned option, char *error, size_t size);
unsigned session_run(MachineSession *s, unsigned steps, FILE *transcript);
typedef enum {
    SESSION_RUNNING, SESSION_OUTPUT_ERROR, SESSION_WAITING, SESSION_DOUBLE_FAULT, SESSION_HALTED, SESSION_WAIT
} SessionStatus;
SessionStatus session_status(const MachineSession *s);
const char *session_state(const MachineSession *s);
bool read_unsigned(const char *text, int base, unsigned limit, unsigned *value);

#endif
