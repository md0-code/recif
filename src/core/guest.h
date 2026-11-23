#ifndef RECIF_GUEST_H
#define RECIF_GUEST_H

#include <stdbool.h>
#include <stdint.h>

uint16_t guest_monitor(uint8_t *ram, bool tape_controller);
uint16_t guest_tape_boot(uint8_t *ram, bool first_record);
uint16_t guest_disk_boot(uint8_t *ram, unsigned unit);
uint16_t guest_rp_boot(uint8_t *ram, unsigned unit);

#endif
