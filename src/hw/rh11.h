#ifndef RECIF_RH11_H
#define RECIF_RH11_H

/* RH11 MASSBUS adapter with RP06 or RM03 disk drives (DEC reference
 * controller at 176700, vector 254, BR5). */

#include "pdp11.h"

#include <stdio.h>

enum { RH11_DRIVES = 2, RH11_CSR = 0176700, RH11_VECTOR = 0254 };
typedef enum { DISK_RP06, DISK_RM03 } DiskType;

typedef struct {
    FILE *file;
    bool read_only;
    uint64_t done_at;
    uint16_t function, da, ds, er1, mr, of, dc, cylinder, hr, mr2;
    DiskType type;              /* MASSBUS drives may differ by unit */
} RhDrive;

typedef struct {
    DiskType type;
    RhDrive drive[RH11_DRIVES];
    uint16_t cs1, wc, ba, cs2, db, bae, cs3;
    bool iff, extended;         /* RH70-style BAE and CS3 at CSR+050/052 */
} Rh11;

const char *rh11_type_name(DiskType type);
uint32_t rh11_blocks(DiskType type);
bool rh11_mount(Rh11 *rh, unsigned unit, const char *path, char *error, size_t size);
void rh11_eject(Rh11 *rh, unsigned unit);
void rh11_close(Rh11 *rh);
void rh11_reset(Rh11 *rh);
bool rh11_io(Rh11 *rh, uint16_t address, bool write, bool byte, uint16_t *value, uint64_t now);
void rh11_tick(Rh11 *rh, Pdp11 *cpu, uint64_t now);
bool rh11_busy(const Rh11 *rh);
bool rh11_interrupt(const Rh11 *rh);
void rh11_acknowledge(Rh11 *rh);

#endif
