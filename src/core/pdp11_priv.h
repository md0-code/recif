#ifndef RECIF_PDP11_PRIV_H
#define RECIF_PDP11_PRIV_H

/* Internal CPU services shared by the integer core and the FP11. */

#include "pdp11.h"

typedef struct {
    uint16_t addr;
    uint8_t reg;
    bool direct, data;
} PdpOperand;

/* Effective address of a general operand. Modes 2 and 4 move a general
 * register by `step` (PC always by 2); deferred modes always by 2. */
PdpOperand pdp_operand(Pdp11 *c, unsigned spec, unsigned step);
uint16_t pdp_read(Pdp11 *c, uint16_t address, bool data);
void pdp_write(Pdp11 *c, uint16_t address, bool data, uint16_t value);
void pdp_set_cc(Pdp11 *c, unsigned nzvc);

unsigned fp11_execute(Pdp11 *c, uint16_t op);
void fp11_reset(Fp11 *fp);

#endif
