#ifndef RECIF_SERIAL_LINK_H
#define RECIF_SERIAL_LINK_H

#include "rs232.h"
#include "stream.h"

#include <stdio.h>

typedef struct {
    rc_host_stream *stream;
    FILE *file;
    Rs232Format format;
    Rs232Rx output;
    Rs232Tx input;
    Rs232Queue incoming, outgoing;
    bool loopback, connected, dropped;
} SerialLink;

#define SERIAL_ENDS "none, loopback, tcp:HOST:PORT, tcp-listen:[HOST:]PORT, pipe:NAME, COMn or file:PATH"

bool serial_link_open(SerialLink *link, const char *endpoint, const Rs232Format *format,
                      char *error, size_t size);
void serial_link_set_format(SerialLink *link, const Rs232Format *format);
/* Separate framing toward the emulated device and from it. */
void serial_link_set_formats(SerialLink *link, const Rs232Format *to_device, const Rs232Format *from_device);
void serial_link_pump(SerialLink *link);
void serial_link_close(SerialLink *link);
bool serial_link_rx(SerialLink *link, uint64_t now, bool txd, bool ready);
void serial_link_tx(SerialLink *link, uint64_t now, bool txd);

#endif
