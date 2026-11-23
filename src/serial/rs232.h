#ifndef RECIF_RS232_H
#define RECIF_RS232_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One virtual clock for both ends of the cable. It divides the reference
 * CPU's functional tick rate and the DAF switch rates from 150 to 19200. */
enum { RS232_HZ = 4800000, RS232_TICK = 8, RS232_BUFFER = 4096 };

typedef enum { RS232_NO_PARITY, RS232_EVEN, RS232_ODD } Rs232Parity;
typedef struct {
    unsigned baud, data_bits, stop_bits;
    Rs232Parity parity;
} Rs232Format;
extern const Rs232Format rs232_console_format;
bool rs232_parse_format(const char *text, Rs232Format *format);

typedef struct {
    uint64_t start;
    unsigned bit_time, length, marks;
} Rs232Tx;

typedef struct {
    Rs232Format format;
    uint64_t sample_at;
    unsigned bit_time, bit;
    uint8_t data;
    bool active, last_mark, parity;
} Rs232Rx;

typedef struct {
    uint8_t data;
    bool parity, framing;
} Rs232Received;

typedef struct {
    uint8_t data[RS232_BUFFER];
    size_t start, count;
} Rs232Queue;

bool rs232_idle(const Rs232Tx *tx, uint64_t now);
/* Logic 1 is MARK (idle), logic 0 is SPACE. */
bool rs232_level(const Rs232Tx *tx, uint64_t now);
void rs232_send(Rs232Tx *tx, const Rs232Format *format, uint64_t now, uint8_t data);
void rs232_rx_init(Rs232Rx *rx, const Rs232Format *format);
/* Step at intervals shorter than half a bit. The output is available at
 * the centre of the first stop bit. */
bool rs232_sample(Rs232Rx *rx, uint64_t now, bool mark, Rs232Received *out);
bool rs232_queue_put(Rs232Queue *q, uint8_t data);
bool rs232_queue_get(Rs232Queue *q, uint8_t *data);

#endif
