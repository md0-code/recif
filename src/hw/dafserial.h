#ifndef RECIF_DAF_SERIAL_H
#define RECIF_DAF_SERIAL_H

#include "daf2020.h"
#include "rs232.h"

/* The terminal's serial port: its own transmit and receive framing from
 * the line parameters, BREAK on the transmit line and the processing
 * pauses of smooth scrolling. */
typedef struct {
    Daf display;
    Rs232Rx receiver;
    Rs232Tx transmitter;
    Rs232Queue keys;
    uint64_t break_until, busy_until;
    unsigned line_changes;
} DafSerial;

Rs232Format daf_serial_format(const DafLine *line, bool transmit);
void daf_serial_reset(DafSerial *terminal, uint16_t switches);
void daf_serial_step(DafSerial *terminal, uint64_t now, bool rxd);
bool daf_serial_txd(const DafSerial *terminal, uint64_t now);

#endif
