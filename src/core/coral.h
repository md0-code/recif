#ifndef RECIF_CORAL_H
#define RECIF_CORAL_H

#include "pdp11.h"

enum { CORAL_MODEL_COUNT = 4 };

/* The DEC reference disk subsystem: none, an RH11 with RM03 or RP06 drives,
 * or an RP11-C with RP03 drives. */
typedef enum { DISKS_NONE, DISKS_RM03, DISKS_RP06, DISKS_RP03, DISKS_COUNT } DiskSystem;
const char *disk_system_name(DiskSystem disks);
bool disk_system_parse(const char *text, DiskSystem *disks);

typedef struct {
    const char *id, *name;
    PdpModel cpu;
    unsigned nominal_kib, capacity_kib;
    const char *hardware, *peripherals, *hardware_ro, *peripherals_ro;
    bool tape_reported, fpu_reported;
    DiskSystem disks;
    uint16_t mmr3_alias;
    bool direct_dma;
} CoralProfile;

extern const CoralProfile coral_profiles[CORAL_MODEL_COUNT];
const CoralProfile *coral_find(const char *id);

#endif
