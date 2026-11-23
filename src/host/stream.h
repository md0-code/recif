#ifndef RECIF_STREAM_H
#define RECIF_STREAM_H

#include <stddef.h>
#include <stdint.h>

#define RC_HOST_PATH 1024

typedef struct rc_host_stream rc_host_stream;
enum { RC_STREAM_SERIAL, RC_STREAM_PIPE, RC_STREAM_TCP_CLIENT, RC_STREAM_TCP_LISTEN };

rc_host_stream *rc_host_stream_open(int kind, const char *path, char *error, size_t capacity);
long rc_host_stream_read(rc_host_stream *s, uint8_t *data, size_t capacity);
long rc_host_stream_write(rc_host_stream *s, const uint8_t *data, size_t count);
int rc_host_stream_connected(rc_host_stream *s);
unsigned rc_host_stream_local_port(const rc_host_stream *s);
void rc_host_stream_close(rc_host_stream *s);

uint64_t rc_host_now_us(void);
/* The directory holding the running executable, with a trailing separator. */
void rc_host_program_dir(char *path, size_t capacity);
/* The user's locale name ("ro-RO"); empty when unknown. */
void rc_host_locale(char *name, size_t capacity);
/* The host's serial ports ("COM3", "/dev/ttyUSB0"), sorted; their count. */
enum { RC_HOST_PORTS = 16, RC_HOST_PORT_NAME = 32 };
unsigned rc_host_list_serial(char names[][RC_HOST_PORT_NAME], unsigned capacity);
void rc_host_sleep_us(uint32_t us);

#endif
