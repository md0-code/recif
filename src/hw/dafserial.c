#include "dafserial.h"

static void queue_key(void *context, uint8_t ch)
{
    DafSerial *terminal = context;
    rs232_queue_put(&terminal->keys, ch);
}

Rs232Format daf_serial_format(const DafLine *line, bool transmit)
{
    return (Rs232Format){transmit ? line->tx_baud : line->rx_baud, line->data_bits,
                         line->stop_bits, line->parity};
}

static void apply_line(DafSerial *terminal)
{
    const DafLine *line = &terminal->display.line;
    Rs232Format rx = daf_serial_format(line, false);
    terminal->transmitter = (Rs232Tx){0};
    rs232_rx_init(&terminal->receiver, &rx);
    terminal->line_changes = terminal->display.line_changes;
}

void daf_serial_reset(DafSerial *terminal, uint16_t switches)
{
    terminal->keys = (Rs232Queue){0};
    terminal->break_until = terminal->busy_until = 0;
    daf_reset(&terminal->display, switches, queue_key, terminal);
    apply_line(terminal);
}

void daf_serial_step(DafSerial *terminal, uint64_t now, bool rxd)
{
    Daf *d = &terminal->display;
    if (terminal->line_changes != d->line_changes)
        apply_line(terminal);
    Rs232Received input;
    if (rs232_sample(&terminal->receiver, now, rxd, &input)) {
        if (input.parity && !input.framing)
            daf_receive_error(d);
        else if (!input.framing)
            daf_receive(d, input.data);
    }
    if (d->busy_us) {
        terminal->busy_until = now + (uint64_t)d->busy_us * (RS232_HZ / 1000000);
        d->busy_us = 0;
    }
    if (now >= terminal->busy_until)
        daf_drain(d, 1);
    if (d->break_ms) {
        terminal->break_until = now + (uint64_t)d->break_ms * (RS232_HZ / 1000);
        terminal->transmitter = (Rs232Tx){0};
        d->break_ms = 0;
    }
    uint8_t ch;
    if (now >= terminal->break_until && rs232_idle(&terminal->transmitter, now) &&
        rs232_queue_get(&terminal->keys, &ch)) {
        Rs232Format tx = daf_serial_format(&d->line, true);
        rs232_send(&terminal->transmitter, &tx, now, ch);
    }
}

bool daf_serial_txd(const DafSerial *terminal, uint64_t now)
{
    return now >= terminal->break_until && rs232_level(&terminal->transmitter, now);
}
