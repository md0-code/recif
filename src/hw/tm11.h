#ifndef RECIF_TM11_H
#define RECIF_TM11_H

#include "pdp11.h"
#include "tape.h"

typedef struct {
    Tape medium;
    uint64_t ready_at;
    uint16_t command, errors, count, dma, data, read_lines;
    bool irq, offline;
} Tm11;

void tm11_reset(Tm11 *tm);
void tm11_changed_medium(Tm11 *tm);
bool tm11_io(Tm11 *tm, uint16_t reg, bool write, bool byte, uint16_t *value, uint64_t now);
void tm11_tick(Tm11 *tm, Pdp11 *cpu, uint64_t now);

#endif
