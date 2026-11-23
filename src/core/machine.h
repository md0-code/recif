#ifndef RECIF_MACHINE_H
#define RECIF_MACHINE_H

#include "coral.h"
#include "rh11.h"
#include "rp11.h"
#include "rs232.h"
#include "tm11.h"

typedef enum { START_MONITOR, START_TAPE, START_RAW, START_DISK } StartMode;

typedef struct {
    const CoralProfile *profile;
    Rs232Format console;
    unsigned memory_kib, clock_hz;
    DiskSystem disks;
    bool tm11, fpu;
} MachineConfig;

typedef struct {
    MachineConfig config;
    Pdp11 cpu;
    Tm11 tape;
    Rh11 massbus;
    Rp11 pack;
    uint8_t *ram;
    unsigned ram_size, clock_phase;
    Rs232Tx console_tx;
    Rs232Rx console_rx;
    uint64_t time;
    uint16_t rxcsr, txcsr, clock_csr, rx;
    uint8_t tx;
    bool rx_irq, tx_irq, clock_irq, tx_pending;
} Machine;

MachineConfig machine_defaults(const CoralProfile *profile);
unsigned machine_ram_kib(const MachineConfig *config);
bool machine_init(Machine *m, const MachineConfig *config);
void machine_destroy(Machine *m);
bool machine_start(Machine *m, StartMode mode, const uint8_t *raw, size_t length,
                   uint16_t load, uint16_t entry, unsigned option, char *err, size_t errlen);
void machine_step(Machine *m, uint64_t now, bool rxd);
bool machine_txd(const Machine *m, uint64_t now);
bool machine_receive_ready(const Machine *m);
enum { MACHINE_DISK_UNITS = 2 };
bool machine_disk_mount(Machine *m, unsigned unit, const char *image, char *err, size_t errlen);
void machine_disk_eject(Machine *m, unsigned unit);
bool machine_disk_mounted(const Machine *m, unsigned unit);
bool machine_disk_busy(const Machine *m);

#endif
