/*
 * The host's end of a serial port (hostser.h): a file, an input file, or a
 * TCP connection, polled once a frame without blocking.
 */
#include "hostser.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define sock_close closesocket
static int would_block(void) { int e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAENOTCONN || e == WSAEINPROGRESS; }
static void nonblock(sock_t s) { u_long on = 1; ioctlsocket(s, FIONBIO, &on); }
static int net_init(void) { static int done; WSADATA w; if (!done && WSAStartup(MAKEWORD(2, 2), &w)) return -1; done = 1; return 0; }
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define sock_close close
static int would_block(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == ENOTCONN; }
static void nonblock(sock_t s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
static int net_init(void) { return 0; }
#endif

struct hostser {
    FILE *out;                  /* output to a file */
    FILE *in;                   /* input from a file, once */
    sock_t listener, conn;      /* tcp: the listening socket (server), the connection */
    char host[256], port[16];   /* tcp:HOST:PORT: where to connect (again, after a close) */
    int pending;                /* a byte the QL had no room for, or -1 */
    uint8_t rx[512];
    int rx_n, rx_pos;
};

static hostser *new_hostser(void) {
    hostser *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->listener = h->conn = BAD_SOCK;
    h->pending = -1;
    return h;
}

static int tcp_listen(hostser *h, const char *port) {
    struct addrinfo hints = { 0 }, *ai;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo("127.0.0.1", port, &hints, &ai)) return -1;
    sock_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    int ok = s != BAD_SOCK;
    if (ok) {
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
        ok = bind(s, ai->ai_addr, (int)ai->ai_addrlen) == 0 && listen(s, 1) == 0;
    }
    freeaddrinfo(ai);
    if (!ok) { if (s != BAD_SOCK) sock_close(s); return -1; }
    nonblock(s);
    h->listener = s;
    return 0;
}

static void tcp_connect(hostser *h) {
    struct addrinfo hints = { 0 }, *ai;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(h->host, h->port, &hints, &ai)) return;
    sock_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s != BAD_SOCK) {
        nonblock(s);
        if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0 || would_block()) h->conn = s;
        else sock_close(s);
    }
    freeaddrinfo(ai);
}

hostser *hostser_open(const char *spec) {
    hostser *h = new_hostser();
    if (!h) return NULL;
    if (!strncmp(spec, "tcp:", 4)) {
        if (net_init()) { fprintf(stderr, "openql: no sockets\n"); free(h); return NULL; }
        const char *rest = spec + 4, *colon = strrchr(rest, ':');
        if (!colon) {                               /* tcp:PORT: listen */
            if (tcp_listen(h, rest)) { fprintf(stderr, "openql: cannot listen on 127.0.0.1:%s\n", rest); free(h); return NULL; }
            fprintf(stderr, "openql: ser1 waits for a connection on 127.0.0.1:%s\n", rest);
        } else {                                    /* tcp:HOST:PORT: connect */
            size_t n = (size_t)(colon - rest);
            if (n >= sizeof h->host || strlen(colon + 1) >= sizeof h->port) { free(h); return NULL; }
            memcpy(h->host, rest, n); h->host[n] = 0;
            strcpy(h->port, colon + 1);
            tcp_connect(h);
            fprintf(stderr, "openql: ser1 connects to %s:%s\n", h->host, h->port);
        }
        return h;
    }
    if (!(h->out = fopen(spec, "ab"))) { perror(spec); free(h); return NULL; }
    return h;
}

int hostser_input_file(hostser **hp, const char *path) {
    if (!*hp && !(*hp = new_hostser())) return -1;
    if (!((*hp)->in = fopen(path, "rb"))) { perror(path); return -1; }
    return 0;
}

static void drop(hostser *h) {
    if (h->conn != BAD_SOCK) sock_close(h->conn);
    h->conn = BAD_SOCK;
    h->rx_n = h->rx_pos = 0;
}

void hostser_poll(hostser *h) {
    if (!h) return;
    if (h->listener != BAD_SOCK && h->conn == BAD_SOCK) {
        sock_t s = accept(h->listener, NULL, NULL);
        if (s != BAD_SOCK) { nonblock(s); h->conn = s; fprintf(stderr, "openql: ser1 connected\n"); }
    }
    if (h->host[0] && h->conn == BAD_SOCK) {        /* the other end went: try again, about once a second */
        static int tries;
        if (++tries % 50 == 0) tcp_connect(h);
    }
}

void hostser_put(hostser *h, uint8_t ch) {
    if (!h) { fputc(ch, stdout); fflush(stdout); return; }
    if (h->out) { fputc(ch, h->out); fflush(h->out); }
    if (h->conn != BAD_SOCK) {
        char c = (char)ch;
        if (send(h->conn, &c, 1, 0) != 1 && !would_block()) drop(h);
    } else if (!h->out && h->listener == BAD_SOCK && !h->host[0]) { fputc(ch, stdout); fflush(stdout); }
}

int hostser_get(hostser *h) {
    if (!h) return -1;
    if (h->pending >= 0) { int c = h->pending; h->pending = -1; return c; }
    if (h->in) {
        int c = fgetc(h->in);
        if (c != EOF) return c;
        fclose(h->in);
        h->in = NULL;
    }
    if (h->conn == BAD_SOCK) return -1;
    if (h->rx_pos >= h->rx_n) {
        int n = (int)recv(h->conn, (char *)h->rx, sizeof h->rx, 0);
        if (n == 0 || (n < 0 && !would_block())) { drop(h); fprintf(stderr, "openql: ser1 disconnected\n"); return -1; }
        if (n < 0) return -1;
        h->rx_n = n; h->rx_pos = 0;
    }
    return h->rx[h->rx_pos++];
}

void hostser_unget(hostser *h, int ch) { if (h) h->pending = ch; }

void hostser_close(hostser *h) {
    if (!h) return;
    if (h->out) fclose(h->out);
    if (h->in) fclose(h->in);
    drop(h);
    if (h->listener != BAD_SOCK) sock_close(h->listener);
    free(h);
}
