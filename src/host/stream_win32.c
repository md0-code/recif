#include "stream.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rc_host_program_dir(char *path, size_t capacity)
{
    DWORD n = GetModuleFileNameA(NULL, path, (DWORD)capacity);
    if (!n || n >= capacity) {
        path[0] = '\0';
        return;
    }
    char *slash = strrchr(path, '\\');
    if (slash)
        slash[1] = '\0';
    else
        path[0] = '\0';
}

uint64_t rc_host_now_us(void)
{
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    uint64_t t = (uint64_t)now.QuadPart, hz = (uint64_t)frequency.QuadPart;
    return t / hz * 1000000 + t % hz * 1000000 / hz;
}

void rc_host_sleep_us(uint32_t us) {
    /* A high-resolution waitable timer (Windows 10 version 1803 and later) keeps its resolution when the
     * process's window is hidden or covered; older systems fall back to Sleep. */
    static HANDLE timer;
    static int tried;
    if (!tried) {
        tried = 1;
        timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    }
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)us * 10;          /* 100 ns units, relative */
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
    Sleep((us + 999u) / 1000u);
}

/* --- byte streams ------------------------------------------------------------ */

/* TCP ends retry a connection this long after the last attempt. */
#define RETRY_MS 1000

struct rc_host_stream {
    int kind;
    HANDLE handle;
    int server;                 /* the pipe was created here (waits for a client) */
    int connected;              /* pipe: a client is (or the server was) there; TCP: a connection is up */
    SOCKET sock;                /* TCP: the connection, or the client accepted by a listener */
    SOCKET listener;            /* TCP_LISTEN: the listening socket */
    struct sockaddr_in peer;    /* TCP_CLIENT: where to connect */
    int connecting;             /* TCP_CLIENT: a connection attempt is under way */
    ULONGLONG retry_at;         /* TCP_CLIENT: when to try again */
    unsigned local_port;
};

static rc_host_stream *fail(rc_host_stream *s, char *error, size_t capacity, const char *what, const char *path) {
    snprintf(error, capacity, "%s %s (Windows error %lu)", what, path, (unsigned long)GetLastError());
    free(s);
    return NULL;
}

/* A server end waits for a client without blocking. */
static void listen_pipe(rc_host_stream *s) {
    if (ConnectNamedPipe(s->handle, NULL) || GetLastError() == ERROR_PIPE_CONNECTED) s->connected = 1;
}

/* --- TCP: non-blocking sockets polled by the caller's reads and writes ---------- */

static int net_users;

static int net_start(void) {
    if (!net_users) {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data)) return 0;
    }
    net_users++;
    return 1;
}

static void net_stop(void) {
    if (net_users && !--net_users) WSACleanup();
}

static int is_tcp(const rc_host_stream *s) { return s->kind == RC_STREAM_TCP_CLIENT || s->kind == RC_STREAM_TCP_LISTEN; }

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

static void tcp_options(SOCKET sock) {
    u_long nonblocking = 1;
    BOOL nodelay = TRUE;
    ioctlsocket(sock, FIONBIO, &nonblocking);
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof nodelay);
}

/* The connection is gone: a client tries again in a second, a listener takes the next one. */
static void tcp_drop(rc_host_stream *s) {
    if (s->sock != INVALID_SOCKET) closesocket(s->sock);
    s->sock = INVALID_SOCKET;
    s->connected = 0;
    s->connecting = 0;
    s->retry_at = GetTickCount64() + RETRY_MS;
}

static void tcp_begin(rc_host_stream *s) {
    s->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s->sock == INVALID_SOCKET) { s->retry_at = GetTickCount64() + RETRY_MS; return; }
    tcp_options(s->sock);
    if (connect(s->sock, (const struct sockaddr *)&s->peer, sizeof s->peer) == 0) s->connected = 1;
    else if (WSAGetLastError() == WSAEWOULDBLOCK) s->connecting = 1;
    else tcp_drop(s);
}

/* Moves a connection along: a listener accepts, a client finishes or starts its attempt. */
static void tcp_service(rc_host_stream *s) {
    if (s->kind == RC_STREAM_TCP_LISTEN) {
        if (s->connected) return;
        const SOCKET client = accept(s->listener, NULL, NULL);
        if (client == INVALID_SOCKET) return;
        tcp_options(client);
        s->sock = client;
        s->connected = 1;
    } else if (s->connecting) {
        fd_set writable, failed;
        struct timeval none = {0, 0};
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(s->sock, &writable);
        FD_SET(s->sock, &failed);
        if (select(0, NULL, &writable, &failed, &none) <= 0) return;
        if (FD_ISSET(s->sock, &failed) || !FD_ISSET(s->sock, &writable)) tcp_drop(s);
        else { s->connecting = 0; s->connected = 1; }
    } else if (!s->connected && GetTickCount64() >= s->retry_at) tcp_begin(s);
}

static rc_host_stream *tcp_open(rc_host_stream *s, const char *path, char *error, size_t capacity) {
    char host[256];
    unsigned port = 0;
    s->sock = s->listener = INVALID_SOCKET;
    if (!split_target(path, host, sizeof host, &port) || (s->kind == RC_STREAM_TCP_CLIENT && (!*host || !port))) {
        snprintf(error, capacity, "Cannot use the TCP address %s", path);
        free(s);
        return NULL;
    }
    if (!net_start()) { snprintf(error, capacity, "Cannot start the network (Windows error %lu)", (unsigned long)GetLastError()); free(s); return NULL; }
    if (s->kind == RC_STREAM_TCP_CLIENT) {
        if (!resolve_ipv4(host, port, &s->peer)) {
            snprintf(error, capacity, "Cannot find the address %s", path);
            net_stop();
            free(s);
            return NULL;
        }
        tcp_begin(s);
        return s;
    }
    if (!resolve_ipv4(*host ? host : "127.0.0.1", port, &s->peer)) {
        snprintf(error, capacity, "Cannot use the address %s", path);
        net_stop();
        free(s);
        return NULL;
    }
    s->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL exclusive = TRUE;
    int length = (int)sizeof s->peer;
    if (s->listener == INVALID_SOCKET) goto failed;
    setsockopt(s->listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive, sizeof exclusive);
    if (bind(s->listener, (const struct sockaddr *)&s->peer, sizeof s->peer) || listen(s->listener, 1)) goto failed;
    tcp_options(s->listener);
    if (getsockname(s->listener, (struct sockaddr *)&s->peer, &length) == 0) s->local_port = ntohs(s->peer.sin_port);
    return s;
failed:
    snprintf(error, capacity, "Cannot listen on %s (Windows error %lu)", path, (unsigned long)WSAGetLastError());
    if (s->listener != INVALID_SOCKET) closesocket(s->listener);
    net_stop();
    free(s);
    return NULL;
}

rc_host_stream *rc_host_stream_open(int kind, const char *path, char *error, size_t capacity) {
    rc_host_stream *s = calloc(1, sizeof *s);
    if (!s) { snprintf(error, capacity, "Out of memory"); return NULL; }
    s->kind = kind;
    if (kind == RC_STREAM_TCP_CLIENT || kind == RC_STREAM_TCP_LISTEN) return tcp_open(s, path, error, capacity);
    if (kind == RC_STREAM_SERIAL) {
        char name[64];
        snprintf(name, sizeof name, "\\\\.\\%s", path);
        HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) return fail(s, error, capacity, "Cannot open the serial port", path);
        /* Reads return what is there at once; writes give up after 100 ms. */
        COMMTIMEOUTS t = {MAXDWORD, 0, 0, 0, 100};
        SetCommTimeouts(h, &t);
        s->handle = h;
        return s;
    }
    /* A pipe someone else serves is joined; otherwise Recif serves it. */
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
        SetNamedPipeHandleState(h, &mode, NULL, NULL);
        s->handle = h;
        s->connected = 1;
        return s;
    }
    h = CreateNamedPipeA(path, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return fail(s, error, capacity, "Cannot open the pipe", path);
    s->handle = h;
    s->server = 1;
    listen_pipe(s);
    return s;
}

/* A pipe whose other end went away: a server listens again; a client is
 * done (the pipe is gone with its server). */
static void pipe_broken(rc_host_stream *s) {
    s->connected = 0;
    if (s->server) {
        DisconnectNamedPipe(s->handle);
        listen_pipe(s);
    }
}

long rc_host_stream_read(rc_host_stream *s, uint8_t *data, size_t capacity) {
    if (!s || !capacity) return 0;
    if (is_tcp(s)) {
        tcp_service(s);
        if (!s->connected) return 0;
        const int got = recv(s->sock, (char *)data, capacity > 0x10000 ? 0x10000 : (int)capacity, 0);
        if (got > 0) return got;
        if (got < 0 && WSAGetLastError() == WSAEWOULDBLOCK) return 0;
        tcp_drop(s);                    /* closed by the other side, or broken */
        return -1;
    }
    DWORD avail = capacity > 0x10000 ? 0x10000 : (DWORD)capacity;
    if (s->kind == RC_STREAM_PIPE) {
        if (!s->connected) {
            if (s->server) listen_pipe(s);
            if (!s->connected) return 0;
        }
        DWORD there = 0;
        if (!PeekNamedPipe(s->handle, NULL, 0, NULL, &there, NULL)) { pipe_broken(s); return -1; }
        if (!there) return 0;
        if (there < avail) avail = there;
    }
    DWORD got = 0;
    if (!ReadFile(s->handle, data, avail, &got, NULL)) {
        if (s->kind == RC_STREAM_PIPE && GetLastError() != ERROR_NO_DATA) { pipe_broken(s); return -1; }
        return 0;
    }
    return (long)got;
}

long rc_host_stream_write(rc_host_stream *s, const uint8_t *data, size_t count) {
    if (!s || !count) return 0;
    if (is_tcp(s)) {
        tcp_service(s);
        if (!s->connected) return (long)count;          /* nobody listens: the bytes are gone */
        const int sent = send(s->sock, (const char *)data, count > 0x10000 ? 0x10000 : (int)count, 0);
        if (sent >= 0) return sent;
        if (WSAGetLastError() == WSAEWOULDBLOCK) return 0;
        tcp_drop(s);
        return -1;
    }
    if (s->kind == RC_STREAM_PIPE && !s->connected) return (long)count;   /* nobody listens: the bytes are gone */
    DWORD wrote = 0;
    if (!WriteFile(s->handle, data, count > 0x10000 ? 0x10000 : (DWORD)count, &wrote, NULL)) {
        if (s->kind == RC_STREAM_PIPE) { pipe_broken(s); return -1; }
        return 0;
    }
    return (long)wrote;
}

int rc_host_stream_connected(rc_host_stream *s)
{
    if (!s) return 0;
    if (s->kind == RC_STREAM_SERIAL) return 1;
    if (is_tcp(s)) tcp_service(s);
    else if (!s->connected && s->server) listen_pipe(s);
    return s->connected;
}

unsigned rc_host_stream_local_port(const rc_host_stream *s) { return s ? s->local_port : 0; }

void rc_host_stream_close(rc_host_stream *s) {
    if (!s) return;
    if (is_tcp(s)) {
        if (s->sock != INVALID_SOCKET) closesocket(s->sock);
        if (s->listener != INVALID_SOCKET) closesocket(s->listener);
        net_stop();
    } else if (s->handle && s->handle != INVALID_HANDLE_VALUE) CloseHandle(s->handle);
    free(s);
}

void rc_host_locale(char *name, size_t capacity)
{
    wchar_t wide[LOCALE_NAME_MAX_LENGTH];
    int n = GetUserDefaultLocaleName(wide, LOCALE_NAME_MAX_LENGTH);
    if (!capacity)
        return;
    name[0] = '\0';
    if (n <= 1 || (size_t)n > capacity)
        return;
    for (int i = 0; i < n; ++i)
        name[i] = wide[i] < 128 ? (char)wide[i] : '?';
    name[n - 1] = '\0';
}

static int port_order(const void *a, const void *b)
{
    const char *x = a, *y = b;
    size_t lx = strlen(x), ly = strlen(y);
    return lx != ly ? (lx < ly ? -1 : 1) : strcmp(x, y);
}

unsigned rc_host_list_serial(char names[][RC_HOST_PORT_NAME], unsigned capacity)
{
    HKEY key;
    unsigned count = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return 0;
    for (DWORD i = 0; count < capacity; ++i) {
        char value[128], data[RC_HOST_PORT_NAME];
        DWORD value_size = sizeof(value), data_size = sizeof(data) - 1, type = 0;
        LONG rc = RegEnumValueA(key, i, value, &value_size, NULL, &type, (BYTE *)data, &data_size);
        if (rc == ERROR_NO_MORE_ITEMS)
            break;
        if (rc != ERROR_SUCCESS || type != REG_SZ)
            continue;
        data[data_size < sizeof(data) ? data_size : sizeof(data) - 1] = '\0';
        if (!strncmp(data, "COM", 3))
            memcpy(names[count++], data, sizeof(data));
    }
    RegCloseKey(key);
    qsort(names, count, RC_HOST_PORT_NAME, port_order);
    return count;
}
