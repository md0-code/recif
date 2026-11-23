#ifndef RECIF_RP11_H
#define RECIF_RP11_H

/* RP11-C disk pack controller with RP03 drives (DEC reference controller
 * at 176710, vector 254, BR5), from the RP11-C maintenance manual
 * DEC-11-HRPCA-C-D (1974), chapter 3. */

#include "pdp11.h"

#include <stdio.h>

enum { RP11_DRIVES = 2, RP11_CSR = 0176710, RP11_VECTOR = 0254 };
enum { RP03_CYLINDERS = 406, RP03_TRACKS = 20, RP03_SECTORS = 10 };

typedef struct {
    FILE *file;
    bool read_only;
    uint16_t cylinder, track;
    uint64_t seek_done;
} RpDrive;

typedef struct {
    RpDrive drive[RP11_DRIVES];
    uint16_t cs, er, wc, ba, ca, da, attention, silo;
    uint64_t done_at;
    bool done_irq;
} Rp11;

uint32_t rp11_blocks(void);
bool rp11_mount(Rp11 *rp, unsigned unit, const char *path, char *error, size_t size);
void rp11_eject(Rp11 *rp, unsigned unit);
void rp11_close(Rp11 *rp);
void rp11_reset(Rp11 *rp);
bool rp11_io(Rp11 *rp, uint16_t address, bool write, bool byte, uint16_t *value, uint64_t now);
void rp11_tick(Rp11 *rp, Pdp11 *cpu, uint64_t now);
bool rp11_busy(const Rp11 *rp);
bool rp11_interrupt(const Rp11 *rp);
void rp11_acknowledge(Rp11 *rp);

#endif
