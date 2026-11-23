#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "stream.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

void rc_host_program_dir(char *path, size_t capacity)
{
    ssize_t n = capacity ? readlink("/proc/self/exe", path, capacity - 1) : -1;
    if (n <= 0) {
        if (capacity)
            path[0] = '\0';
        return;
    }
    path[n] = '\0';
    char *slash = strrchr(path, '/');
    if (slash)
        slash[1] = '\0';
    else
        path[0] = '\0';
}

uint64_t rc_host_now_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000 + (unsigned long)now.tv_nsec / 1000;
}

void rc_host_sleep_us(uint32_t us) {
    struct timespec t;
    t.tv_sec = (time_t)(us / 1000000u);
    t.tv_nsec = (long)(us % 1000000u) * 1000;
    nanosleep(&t, NULL);
}

/* --- byte streams ------------------------------------------------------------ */

/* TCP ends retry a connection this long after the last attempt. */
#define RETRY_MS 1000

struct rc_host_stream {
    int kind;
    int fd;                     /* the device, or the connected socket (-1 none) */
    int listener;               /* a served pipe's or a TCP server's listening socket (-1 none) */
    char path[RC_HOST_PATH];
    struct sockaddr_in peer;    /* TCP_CLIENT: where to connect */
    int connecting;             /* TCP_CLIENT: a connection attempt is under way */
    int connected;              /* TCP: a connection is up */
    long long retry_at;         /* TCP_CLIENT: when to try again (milliseconds) */
    unsigned local_port;
};

static rc_host_stream *fail(rc_host_stream *s, char *error, size_t capacity, const char *what, const char *path) {
    snprintf(error, capacity, "%s %s (%s)", what, path, strerror(errno));
    if (s->fd >= 0) close(s->fd);
    if (s->listener >= 0) close(s->listener);
    free(s);
    return NULL;
}

static void accept_client(rc_host_stream *s) {
    if (s->listener < 0 || s->fd >= 0) return;
    int fd = accept(s->listener, NULL, NULL);
    if (fd >= 0) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        s->fd = fd;
    }
}

/* --- TCP: non-blocking sockets polled by the caller's reads and writes ---------- */

static int is_tcp(const rc_host_stream *s) { return s->kind == RC_STREAM_TCP_CLIENT || s->kind == RC_STREAM_TCP_LISTEN; }

static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* "HOST:PORT", "PORT" or ":PORT" into a host name (empty when absent) and a port. */
static int split_target(const char *path, char *host, size_t capacity, unsigned *port) {
    const char *colon = strrchr(path, ':');
    const char *digits = colon ? colon + 1 : path;
    char *end = NULL;
    if (!*digits) return 0;
    const unsigned long value = strtoul(digits, &end, 10);
    if (*end || value > 65535) return 0;
    *port = (unsigned)value;
    const size_t n = colon ? (size_t)(colon - path) : 0;
    if (n >= capacity) return 0;
    memcpy(host, path, n);
    host[n] = 0;
    return 1;
}

static int resolve_ipv4(const char *host, unsigned port, struct sockaddr_in *out) {
    struct addrinfo hints, *found = NULL;
    char service[8];
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(service, sizeof service, "%u", port);
    if (getaddrinfo(host, service, &hints, &found) || !found) return 0;
    memcpy(out, found->ai_addr, sizeof *out);
    freeaddrinfo(found);
    return 1;
}

static void tcp_options(int fd) {
    int nodelay = 1;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof nodelay);
}

/* The connection is gone: a client tries again in a second, a listener takes the next one. */
static void tcp_drop(rc_host_stream *s) {
    if (s->fd >= 0) close(s->fd);
    s->fd = -1;
    s->connected = 0;
    s->connecting = 0;
    s->retry_at = now_ms() + RETRY_MS;
}

static void tcp_begin(rc_host_stream *s) {
    s->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->fd < 0) { s->retry_at = now_ms() + RETRY_MS; return; }
    tcp_options(s->fd);
    if (connect(s->fd, (const struct sockaddr *)&s->peer, sizeof s->peer) == 0) s->connected = 1;
    else if (errno == EINPROGRESS) s->connecting = 1;
    else tcp_drop(s);
}

/* Moves a connection along: a listener accepts, a client finishes or starts its attempt. */
static void tcp_service(rc_host_stream *s) {
    if (s->kind == RC_STREAM_TCP_LISTEN) {
        if (s->connected) return;
        const int client = accept(s->listener, NULL, NULL);
        if (client < 0) return;
        tcp_options(client);
        s->fd = client;
        s->connected = 1;
    } else if (s->connecting) {
        struct pollfd p = {s->fd, POLLOUT, 0};
        if (poll(&p, 1, 0) <= 0) return;
        int error = 0;
        socklen_t length = sizeof error;
        if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &error, &length) || error) tcp_drop(s);
        else { s->connecting = 0; s->connected = 1; }
    } else if (!s->connected && now_ms() >= s->retry_at) tcp_begin(s);
}

static rc_host_stream *tcp_open(rc_host_stream *s, const char *path, char *error, size_t capacity) {
    char host[256];
    unsigned port = 0;
    if (!split_target(path, host, sizeof host, &port) || (s->kind == RC_STREAM_TCP_CLIENT && (!*host || !port))) {
        snprintf(error, capacity, "Cannot use the TCP address %s", path);
        free(s);
        return NULL;
    }
    if (s->kind == RC_STREAM_TCP_CLIENT) {
        if (!resolve_ipv4(host, port, &s->peer)) {
            snprintf(error, capacity, "Cannot find the address %s", path);
            free(s);
            return NULL;
        }
        tcp_begin(s);
        return s;
    }
    struct sockaddr_in local;
    socklen_t length = sizeof local;
    int reuse = 1;
    if (!resolve_ipv4(*host ? host : "127.0.0.1", port, &local)) {
        snprintf(error, capacity, "Cannot use the address %s", path);
        free(s);
        return NULL;
    }
    s->listener = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listener < 0) return fail(s, error, capacity, "Cannot listen on", path);
    setsockopt(s->listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
    if (bind(s->listener, (const struct sockaddr *)&local, sizeof local) || listen(s->listener, 1)) return fail(s, error, capacity, "Cannot listen on", path);
    fcntl(s->listener, F_SETFL, fcntl(s->listener, F_GETFL) | O_NONBLOCK);
    if (getsockname(s->listener, (struct sockaddr *)&local, &length) == 0) s->local_port = ntohs(local.sin_port);
    return s;
}

rc_host_stream *rc_host_stream_open(int kind, const char *path, char *error, size_t capacity) {
    rc_host_stream *s = calloc(1, sizeof *s);
    if (!s) { snprintf(error, capacity, "Out of memory"); return NULL; }
    s->kind = kind;
    s->fd = s->listener = -1;
    snprintf(s->path, sizeof s->path, "%s", path);
    if (kind == RC_STREAM_TCP_CLIENT || kind == RC_STREAM_TCP_LISTEN) return tcp_open(s, path, error, capacity);
    if (kind == RC_STREAM_SERIAL) {
        s->fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (s->fd < 0) return fail(s, error, capacity, "Cannot open the serial port", path);
        struct termios t;
        if (!tcgetattr(s->fd, &t)) {
            cfmakeraw(&t);
            tcsetattr(s->fd, TCSANOW, &t);
        }
        return s;
    }
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof a.sun_path) { errno = ENAMETOOLONG; return fail(s, error, capacity, "Cannot open the pipe", path); }
    strcpy(a.sun_path, path);
    /* A socket someone else serves is joined; otherwise Recif serves it. */
    s->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s->fd < 0) return fail(s, error, capacity, "Cannot open the pipe", path);
    if (connect(s->fd, (struct sockaddr *)&a, sizeof a) == 0) {
        fcntl(s->fd, F_SETFL, fcntl(s->fd, F_GETFL) | O_NONBLOCK);
        return s;
    }
    int connect_error = errno;
    close(s->fd);
    s->fd = -1;
    if (connect_error != ENOENT) {
        errno = connect_error;
        return fail(s, error, capacity, "Cannot open the pipe", path);
    }
    s->listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s->listener < 0 || bind(s->listener, (struct sockaddr *)&a, sizeof a))
        return fail(s, error, capacity, "Cannot open the pipe", path);
    if (listen(s->listener, 1)) {
        int listen_error = errno;
        unlink(path);
        errno = listen_error;
        return fail(s, error, capacity, "Cannot open the pipe", path);
    }
    fcntl(s->listener, F_SETFL, fcntl(s->listener, F_GETFL) | O_NONBLOCK);
    accept_client(s);
    return s;
}

static void broken(rc_host_stream *s) {
    if (s->kind == RC_STREAM_PIPE && s->fd >= 0) { close(s->fd); s->fd = -1; }
}

long rc_host_stream_read(rc_host_stream *s, uint8_t *data, size_t capacity) {
    if (!s || !capacity) return 0;
    if (is_tcp(s)) {
        tcp_service(s);
        if (!s->connected) return 0;
        const ssize_t got = recv(s->fd, data, capacity, 0);
        if (got > 0) return (long)got;
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
        tcp_drop(s);                    /* closed by the other side, or broken */
        return -1;
    }
    accept_client(s);
    if (s->fd < 0) return 0;
    ssize_t n = read(s->fd, data, capacity);
    if (n > 0) return (long)n;
    if (n == 0 && s->kind == RC_STREAM_PIPE) { broken(s); return -1; }
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { broken(s); return -1; }
    return 0;
}

long rc_host_stream_write(rc_host_stream *s, const uint8_t *data, size_t count) {
    if (!s || !count) return 0;
    if (is_tcp(s)) {
        tcp_service(s);
        if (!s->connected) return (long)count;          /* nobody listens: the bytes are gone */
        const ssize_t sent = send(s->fd, data, count, MSG_NOSIGNAL);
        if (sent >= 0) return (long)sent;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        tcp_drop(s);
        return -1;
    }
    accept_client(s);
    if (s->fd < 0) return (long)count;          /* nobody listens: the bytes are gone */
    ssize_t n = s->kind == RC_STREAM_PIPE ? send(s->fd, data, count, MSG_NOSIGNAL) : write(s->fd, data, count);
    if (n >= 0) return (long)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    broken(s);
    return -1;
}

int rc_host_stream_connected(rc_host_stream *s)
{
    if (!s) return 0;
    if (is_tcp(s)) {
        tcp_service(s);
        return s->connected;
    }
    accept_client(s);
    return s->fd >= 0;
}

unsigned rc_host_stream_local_port(const rc_host_stream *s) { return s ? s->local_port : 0; }

void rc_host_stream_close(rc_host_stream *s) {
    if (!s) return;
    if (s->fd >= 0) close(s->fd);
    if (s->listener >= 0) {
        close(s->listener);
        if (s->kind == RC_STREAM_PIPE) unlink(s->path);
    }
    free(s);
}

void rc_host_locale(char *name, size_t capacity)
{
    const char *lang = getenv("LC_ALL");
    if (!lang || !*lang)
        lang = getenv("LANG");
    if (capacity)
        snprintf(name, capacity, "%s", lang ? lang : "");
}

unsigned rc_host_list_serial(char names[][RC_HOST_PORT_NAME], unsigned capacity)
{
    static const char *const kinds[] = {"/dev/ttyS%u", "/dev/ttyUSB%u", "/dev/ttyACM%u"};
    unsigned count = 0;
    for (unsigned k = 0; k < 3; ++k) {
        for (unsigned i = 0; i < 8 && count < capacity; ++i) {
            char path[RC_HOST_PORT_NAME];
            snprintf(path, sizeof(path), kinds[k], i);
            if (access(path, R_OK | W_OK) == 0)
                memcpy(names[count++], path, sizeof(path));
        }
    }
    return count;
}
