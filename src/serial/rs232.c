#include "rs232.h"

#include <stdlib.h>
#include <string.h>

const Rs232Format rs232_console_format = {9600, 8, 1, RS232_NO_PARITY};

bool rs232_parse_format(const char *text, Rs232Format *format)
{
    if (*text < '1' || *text > '9')
        return false;
    char *end;
    unsigned long baud = strtoul(text, &end, 10);
    if (baud < 150 || baud > 19200 || baud % 150 != 0 ||
        *end != ',' || strlen(end) != 4)
        return false;
    unsigned multiple = (unsigned)(baud / 150);
    if (multiple & (multiple - 1))
        return false;
    char data = end[1], parity = end[2], stop = end[3];
    if ((data != '7' && data != '8') || (parity != 'N' && parity != 'E' && parity != 'O') ||
        (stop != '1' && stop != '2'))
        return false;
    *format = (Rs232Format){(unsigned)baud, (unsigned)(data - '0'), (unsigned)(stop - '0'),
                            parity == 'N' ? RS232_NO_PARITY : parity == 'E' ? RS232_EVEN : RS232_ODD};
    return true;
}

bool rs232_idle(const Rs232Tx *tx, uint64_t now)
{
    return !tx->length || now >= tx->start + (uint64_t)tx->length * tx->bit_time;
}

bool rs232_level(const Rs232Tx *tx, uint64_t now)
{
    if (now < tx->start || rs232_idle(tx, now))
        return true;
    unsigned position = (unsigned)((now - tx->start) / tx->bit_time);
    return (tx->marks & (1u << position)) != 0;
}

void rs232_send(Rs232Tx *tx, const Rs232Format *format, uint64_t now, uint8_t data)
{
    unsigned marks = 0;
    bool parity = format->parity == RS232_ODD;
    unsigned count = 1; /* Start with a space. */
    for (unsigned i = 0; i < format->data_bits; ++i, ++count) {
        if (data & (1u << i)) {
            parity = !parity;
            marks |= 1u << count;
        }
    }
    if (format->parity != RS232_NO_PARITY) {
        marks |= (unsigned)parity << count;
        ++count;
    }
    marks |= ((1u << format->stop_bits) - 1) << count;
    *tx = (Rs232Tx){now, RS232_HZ / format->baud, count + format->stop_bits, marks};
}

void rs232_rx_init(Rs232Rx *rx, const Rs232Format *format)
{
    *rx = (Rs232Rx){0};
    rx->last_mark = true;
    rx->format = *format;
    rx->bit_time = RS232_HZ / format->baud;
}

bool rs232_sample(Rs232Rx *rx, uint64_t now, bool mark, Rs232Received *out)
{
    bool start = rx->last_mark && !mark;
    rx->last_mark = mark;
    if (!rx->active && start) {
        rx->sample_at = now + rx->bit_time / 2;
        rx->bit = 0;
        rx->data = 0;
        rx->parity = rx->format.parity == RS232_ODD;
        rx->active = true;
    }
    if (!rx->active || now < rx->sample_at)
        return false;

    unsigned bit = rx->bit++;
    rx->sample_at += rx->bit_time;
    if (bit == 0) {
        if (mark)
            rx->active = false; /* A start pulse shorter than half a bit. */
    } else if (bit <= rx->format.data_bits) {
        if (mark) {
            rx->data |= (uint8_t)(1u << (bit - 1));
            rx->parity = !rx->parity;
        }
    } else if (bit == rx->format.data_bits + 1 && rx->format.parity != RS232_NO_PARITY) {
        rx->parity = rx->parity != mark;
    } else {
        *out = (Rs232Received){rx->data, rx->format.parity != RS232_NO_PARITY && rx->parity, !mark};
        rx->active = false;
        return true;
    }
    return false;
}

bool rs232_queue_put(Rs232Queue *q, uint8_t data)
{
    if (q->count == sizeof(q->data))
        return false;
    q->data[(q->start + q->count++) % sizeof(q->data)] = data;
    return true;
}

bool rs232_queue_get(Rs232Queue *q, uint8_t *data)
{
    if (!q->count)
        return false;
    *data = q->data[q->start];
    q->start = (q->start + 1) % sizeof(q->data);
    --q->count;
    return true;
}
