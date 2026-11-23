#include "serial_link.h"

#include <ctype.h>
#include <string.h>

static bool com_port(const char *s)
{
    if (strlen(s) < 4 || tolower((unsigned char)s[0]) != 'c' ||
        tolower((unsigned char)s[1]) != 'o' || tolower((unsigned char)s[2]) != 'm')
        return false;
    for (s += 3; *s; ++s)
        if (!isdigit((unsigned char)*s))
            return false;
    return true;
}

bool serial_link_open(SerialLink *link, const char *endpoint, const Rs232Format *format,
                      char *error, size_t size)
{
    *link = (SerialLink){0};
    serial_link_set_format(link, format);
    if (!endpoint || !strcmp(endpoint, "none"))
        return true;
    if (!strcmp(endpoint, "loopback")) {
        link->connected = link->loopback = true;
        return true;
    }
    if (!strncmp(endpoint, "file:", 5) && endpoint[5]) {
        link->file = fopen(endpoint + 5, "ab");
        if (!link->file) {
            snprintf(error, size, "Cannot open serial output %.250s", endpoint + 5);
            return false;
        }
        link->connected = true;
        return true;
    }

    const char *target = endpoint;
    char pipe_path[RC_HOST_PATH];
    int kind = RC_STREAM_SERIAL;
    if (!strncmp(endpoint, "tcp:", 4)) {
        kind = RC_STREAM_TCP_CLIENT;
        target += 4;
    } else if (!strncmp(endpoint, "tcp-listen:", 11)) {
        kind = RC_STREAM_TCP_LISTEN;
        target += 11;
    } else if (!strncmp(endpoint, "pipe:", 5) && endpoint[5]) {
        kind = RC_STREAM_PIPE;
        target += 5;
        if (!strchr(target, '/') && !strchr(target, '\\')) {
#ifdef _WIN32
            int n = snprintf(pipe_path, sizeof(pipe_path), "\\\\.\\pipe\\%s", target);
#else
            int n = snprintf(pipe_path, sizeof(pipe_path), "/tmp/%s", target);
#endif
            if (n < 0 || (size_t)n >= sizeof(pipe_path)) {
                snprintf(error, size, "Serial pipe path is too long");
                return false;
            }
            target = pipe_path;
        }
    } else if (!strncmp(endpoint, "serial:", 7) && endpoint[7]) {
        target += 7;
    } else if (!com_port(endpoint) && strncmp(endpoint, "/dev/", 5)) {
        snprintf(error, size, "Serial endpoint must be %s", SERIAL_ENDS);
        return false;
    }
    link->stream = rc_host_stream_open(kind, target, error, size);
    return link->stream != NULL;
}

void serial_link_set_format(SerialLink *link, const Rs232Format *format)
{
    serial_link_set_formats(link, format, format);
}

void serial_link_set_formats(SerialLink *link, const Rs232Format *to_device, const Rs232Format *from_device)
{
    link->format = *to_device;
    link->input = (Rs232Tx){0};
    rs232_rx_init(&link->output, from_device);
}

static void write_pending(SerialLink *link)
{
    Rs232Queue *q = &link->outgoing;
    if (!q->count)
        return;
    size_t length = sizeof(q->data) - q->start;
    if (length > q->count)
        length = q->count;
    long n = 0;
    if (link->file) {
        n = (long)fwrite(q->data + q->start, 1, length, link->file);
        if (fflush(link->file) || (size_t)n < length)
            link->dropped = true;
    } else if (link->stream) {
        n = rc_host_stream_write(link->stream, q->data + q->start, length);
    }
    if (n < 0) {
        q->count = 0;
        link->connected = false;
    } else if (n > 0) {
        q->start = (q->start + (size_t)n) % sizeof(q->data);
        q->count -= (size_t)n;
    }
}

void serial_link_pump(SerialLink *link)
{
    if (link->stream) {
        Rs232Queue *q = &link->incoming;
        link->connected = rc_host_stream_connected(link->stream) != 0;
        size_t end = (q->start + q->count) % sizeof(q->data);
        size_t size = sizeof(q->data) - end;
        if (size > sizeof(q->data) - q->count)
            size = sizeof(q->data) - q->count;
        long n = rc_host_stream_read(link->stream, q->data + end, size);
        if (n > 0)
            q->count += (size_t)n;
        else if (n < 0)
            link->connected = false;
    }
    write_pending(link);
}

void serial_link_close(SerialLink *link)
{
    if (link->file) {
        while (link->outgoing.count && !link->dropped)
            write_pending(link);
        fclose(link->file);
    } else {
        write_pending(link);
    }
    rc_host_stream_close(link->stream);
}

bool serial_link_rx(SerialLink *link, uint64_t now, bool txd, bool ready)
{
    if (link->loopback)
        return txd;
    uint8_t ch;
    if (ready && rs232_idle(&link->input, now) && rs232_queue_get(&link->incoming, &ch))
        rs232_send(&link->input, &link->format, now, ch);
    return rs232_level(&link->input, now);
}

void serial_link_tx(SerialLink *link, uint64_t now, bool txd)
{
    Rs232Received ch;
    if ((link->stream || link->file) && rs232_sample(&link->output, now, txd, &ch) &&
        !ch.framing && !ch.parity && !rs232_queue_put(&link->outgoing, ch.data))
        link->dropped = true;
}
