/* bserve — BHTTP/1 static file server.
 *
 *   usage: ./bserve <root-dir> <port>
 *
 * One process per connection (fork). Each connection serves requests
 * sequentially until the client closes it. */
#include "proto.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SERVER_NAME "bserve/1"
#define DATA_CHUNK  (64u * 1024)   /* body bytes per DATA frame */

static char root[PATH_MAX];
static size_t root_len;
static char peer[64];

static void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%d %s] ", (int)getpid(), peer);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void http_date(time_t t, char *out, size_t n)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, n, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

static const char *content_type(const char *path)
{
    static const struct { const char *ext, *type; } types[] = {
        { ".html", "text/html; charset=utf-8" },
        { ".txt",  "text/plain; charset=utf-8" },
        { ".css",  "text/css" },
        { ".js",   "text/javascript" },
        { ".json", "application/json" },
        { ".png",  "image/png" },
        { ".jpg",  "image/jpeg" },
        { ".svg",  "image/svg+xml" },
    };
    const char *dot = strrchr(path, '.');
    if (dot && !strchr(dot, '/'))
        for (size_t i = 0; i < sizeof types / sizeof types[0]; i++)
            if (strcasecmp(dot, types[i].ext) == 0) return types[i].type;
    return "application/octet-stream";
}

/* Builds and sends a response HEADERS frame. */
static int send_headers(int fd, uint16_t status, const char *ctype,
                        long long clen, time_t mtime, int end)
{
    struct buf b;
    char num[32], date[64], lm[64];
    buf_init(&b);
    put_u16(&b, status);
    size_t count = put_count(&b);
    put_field(&b, count, "content-type", ctype);
    snprintf(num, sizeof num, "%lld", clen);
    put_field(&b, count, "content-length", num);
    put_field(&b, count, "server", SERVER_NAME);
    http_date(time(NULL), date, sizeof date);
    put_field(&b, count, "date", date);
    if (mtime) {
        http_date(mtime, lm, sizeof lm);
        put_field(&b, count, "last-modified", lm);
    }
    int r = frame_write(fd, T_HEADERS, end ? F_END : 0, b.p, (uint32_t)b.len, NULL);
    buf_free(&b);
    return r;
}

/* Small text/plain error response: HEADERS + one DATA (END). HEAD gets no body. */
static int send_error(int fd, uint16_t status, int head_only)
{
    char body[64];
    int n = snprintf(body, sizeof body, "%u %s\n", status, status_reason(status));
    if (send_headers(fd, status, "text/plain; charset=utf-8", n, 0, head_only) < 0)
        return -1;
    if (head_only) return 0;
    return frame_write(fd, T_DATA, F_END, body, (uint32_t)n, NULL);
}

/* Maps a request path to a file under root. Returns an open fd (and fills st,
 * resolved) or sets *status to the error code and returns -1. */
static int resolve(const char *path, uint16_t path_len, struct stat *st,
                   char *resolved, uint16_t *status)
{
    char want[PATH_MAX];

    if (path[0] != '/' || memchr(path, '\0', path_len)) { *status = 400; return -1; }
    if (root_len + path_len >= sizeof want) { *status = 404; return -1; }
    memcpy(want, root, root_len);
    memcpy(want + root_len, path, path_len);
    want[root_len + path_len] = '\0';

    if (!realpath(want, resolved)) {
        *status = (errno == EACCES) ? 403 : 404;
        return -1;
    }
    /* must stay inside root */
    if (strncmp(resolved, root, root_len) != 0 ||
        (resolved[root_len] != '/' && resolved[root_len] != '\0')) {
        *status = 403;
        return -1;
    }
    if (stat(resolved, st) < 0) { *status = 404; return -1; }
    if (S_ISDIR(st->st_mode)) {
        if (strlen(resolved) + sizeof "/index.html" > PATH_MAX) { *status = 404; return -1; }
        strcat(resolved, "/index.html");
        if (stat(resolved, st) < 0) { *status = 404; return -1; }
    }
    if (!S_ISREG(st->st_mode)) { *status = 404; return -1; }

    int ffd = open(resolved, O_RDONLY);
    if (ffd < 0) { *status = (errno == EACCES) ? 403 : 500; return -1; }
    return ffd;
}

/* Handles one request. Returns 0 to keep the connection, -1 to close it. */
static int handle_request(int fd, const struct frame *f)
{
    uint8_t method;
    const char *path;
    uint16_t path_len;
    struct fields fs;

    if (parse_request(f->payload, f->len, &method, &path, &path_len, &fs) < 0) {
        logf_("malformed HEADERS (%u bytes) -> 400", f->len);
        return send_error(fd, 400, 0);
    }
    int head_only = (method == M_HEAD);
    if (method != M_GET && method != M_HEAD) {
        logf_("method %u %.*s -> 405", method, (int)path_len, path);
        return send_error(fd, 405, 0);
    }

    struct stat st;
    char resolved[PATH_MAX];
    uint16_t status = 200;
    int ffd = resolve(path, path_len, &st, resolved, &status);
    if (ffd < 0) {
        logf_("%s %.*s -> %u", method_name(method), (int)path_len, path, status);
        return send_error(fd, status, head_only);
    }

    long long size = (long long)st.st_size;
    int no_body = head_only || size == 0;
    logf_("%s %.*s -> 200 (%lld bytes)", method_name(method), (int)path_len, path, size);

    int rc = send_headers(fd, 200, content_type(resolved), size, st.st_mtime, no_body);
    if (rc == 0 && !no_body) {
        static uint8_t chunk[DATA_CHUNK];
        long long left = size;
        while (left > 0) {
            size_t want = left < (long long)sizeof chunk ? (size_t)left : sizeof chunk;
            ssize_t n = read(ffd, chunk, want);
            if (n <= 0) {
                /* file shrank under us: headers already promised a length,
                 * so the only honest move is to drop the connection */
                logf_("short read on %s; closing", resolved);
                rc = -1;
                break;
            }
            left -= n;
            if (frame_write(fd, T_DATA, left == 0 ? F_END : 0, chunk, (uint32_t)n, NULL) < 0) {
                rc = -1;
                break;
            }
        }
    }
    close(ffd);
    return rc;
}

static void serve_connection(int fd)
{
    logf_("accept");
    for (;;) {
        struct frame f;
        int r = frame_read(fd, &f, NULL);
        if (r == FR_EOF) { logf_("client closed"); break; }
        if (r == FR_IO)  { logf_("read error / truncated frame; closing"); break; }
        if (r == FR_TOOBIG) {
            logf_("frame length > MAX_FRAME -> 400, closing");
            send_error(fd, 400, 0);
            break;
        }
        int keep;
        if (f.type == T_HEADERS) {
            keep = handle_request(fd, &f) == 0;
        } else {
            /* requests carry no body; a stray DATA frame is a protocol error */
            logf_("unexpected %s frame -> 400", frame_type_name(f.type));
            keep = send_error(fd, 400, 0) == 0;
        }
        frame_free(&f);
        if (!keep) break;
    }
    close(fd);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <root-dir> <port>\n", argv[0]);
        return 2;
    }
    if (!realpath(argv[1], root)) { perror(argv[1]); return 2; }
    root_len = strlen(root);
    if (root_len == 1) root_len = 0;  /* root is "/": every path is inside it */

    char *endp;
    long port = strtol(argv[2], &endp, 10);
    if (*endp || port < 1 || port > 65535) {
        fprintf(stderr, "bad port: %s\n", argv[2]);
        return 2;
    }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = SA_NOCLDWAIT;   /* auto-reap children */
    sigaction(SIGCHLD, &sa, NULL);

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(ls, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    if (listen(ls, 64) < 0) { perror("listen"); return 1; }

    strcpy(peer, "listen");
    logf_("serving %s on port %ld", root_len ? root : "/", port);

    for (;;) {
        struct sockaddr_in ca;
        socklen_t cl = sizeof ca;
        int cfd = accept(ls, (struct sockaddr *)&ca, &cl);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); close(cfd); continue; }
        if (pid == 0) {
            close(ls);
            snprintf(peer, sizeof peer, "%s:%u", inet_ntoa(ca.sin_addr), ntohs(ca.sin_port));
            serve_connection(cfd);
            _exit(0);
        }
        close(cfd);
    }
}
