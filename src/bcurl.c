/* bcurl — BHTTP/1 client.
 *
 *   usage: ./bcurl [-v] [-I] host:port/path [host:port/path ...]
 *
 * All URLs must name the same host:port: they are fetched in order over ONE
 * TCP connection. Bodies go to stdout; -v traces every frame to stderr.
 * Exit: 0 = all 2xx/3xx, 1 = some 4xx, 2 = some 5xx, 3 = network/protocol error. */
#include "proto.h"

#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define USER_AGENT "bcurl/1"
#define EXIT_NET 3

static int verbose;

struct url {
    char host[256];
    char port[8];
    const char *path;
};

/* host[:port][/path]; port defaults to 9000, path to "/" */
static int parse_url(const char *s, struct url *u)
{
    const char *slash = strchr(s, '/');
    const char *hp_end = slash ? slash : s + strlen(s);
    const char *colon = memchr(s, ':', (size_t)(hp_end - s));
    const char *h_end = colon ? colon : hp_end;
    size_t hl = (size_t)(h_end - s);
    size_t pl = colon ? (size_t)(hp_end - colon - 1) : 0;

    if (hl == 0 || hl >= sizeof u->host || (colon && (pl == 0 || pl >= sizeof u->port)))
        return -1;
    memcpy(u->host, s, hl);
    u->host[hl] = '\0';
    if (colon) {
        memcpy(u->port, colon + 1, pl);
        u->port[pl] = '\0';
    } else {
        strcpy(u->port, "9000");
    }
    u->path = slash ? slash : "/";
    return strlen(u->path) > 0xFFFF ? -1 : 0;
}

static int dial(const struct url *u)
{
    struct addrinfo hints, *res, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(u->host, u->port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "bcurl: %s: %s\n", u->host, gai_strerror(rc));
        return -1;
    }
    int fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) fprintf(stderr, "bcurl: cannot connect to %s:%s\n", u->host, u->port);
    else if (verbose) fprintf(stderr, "* connected to %s port %s (one connection for all requests)\n",
                              u->host, u->port);
    return fd;
}

static void print_fields(const char *dir, const struct fields *fs)
{
    for (int i = 0; i < fs->n; i++)
        fprintf(stderr, "%s %.*s: %.*s\n", dir,
                (int)fs->f[i].name_len, fs->f[i].name,
                (int)fs->f[i].value_len, (const char *)fs->f[i].value);
}

/* Sends one request and reads its full response. Returns the exit code for it. */
static int fetch(int fd, const struct url *u, uint8_t method)
{
    char hostval[300];
    struct buf b;
    buf_init(&b);
    put_u8(&b, method);
    put_u16(&b, (uint16_t)strlen(u->path));
    put_bytes(&b, u->path, strlen(u->path));
    size_t count = put_count(&b);
    snprintf(hostval, sizeof hostval, "%s:%s", u->host, u->port);
    put_field(&b, count, "host", hostval);
    put_field(&b, count, "user-agent", USER_AGENT);
    put_field(&b, count, "accept", "*/*");

    if (verbose) fprintf(stderr, "> %s %s\n", method_name(method), u->path);
    int rc = frame_write(fd, T_HEADERS, F_END, b.p, (uint32_t)b.len, verbose ? stderr : NULL);
    buf_free(&b);
    if (rc < 0) { perror("bcurl: write"); return EXIT_NET; }

    /* response HEADERS */
    struct frame f;
    int r = frame_read(fd, &f, verbose ? stderr : NULL);
    if (r != FR_OK) {
        fprintf(stderr, "bcurl: %s\n", r == FR_EOF ? "server closed the connection"
                                       : r == FR_TOOBIG ? "oversized frame from server"
                                       : "read error");
        return EXIT_NET;
    }
    if (f.type != T_HEADERS) {
        fprintf(stderr, "bcurl: protocol error: expected HEADERS, got %s\n", frame_type_name(f.type));
        frame_free(&f);
        return EXIT_NET;
    }
    uint16_t status;
    struct fields fs;
    if (parse_response(f.payload, f.len, &status, &fs) < 0) {
        fprintf(stderr, "bcurl: protocol error: malformed response HEADERS\n");
        frame_free(&f);
        return EXIT_NET;
    }
    if (verbose) {
        fprintf(stderr, "< status: %u %s\n", status, status_reason(status));
        print_fields("<", &fs);
    }
    int done = f.flags & F_END;
    frame_free(&f);

    /* body */
    unsigned long long total = 0;
    while (!done) {
        r = frame_read(fd, &f, verbose ? stderr : NULL);
        if (r != FR_OK) {
            fprintf(stderr, "bcurl: connection lost mid-body\n");
            return EXIT_NET;
        }
        if (f.type != T_DATA) {
            fprintf(stderr, "bcurl: protocol error: expected DATA, got %s\n", frame_type_name(f.type));
            frame_free(&f);
            return EXIT_NET;
        }
        if (f.len) fwrite(f.payload, 1, f.len, stdout);
        total += f.len;
        done = f.flags & F_END;
        frame_free(&f);
    }
    fflush(stdout);
    if (verbose) fprintf(stderr, "* response complete: %u, %llu body bytes\n", status, total);

    if (status >= 500) return 2;
    if (status >= 400) return 1;
    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: bcurl [-v] [-I] host:port/path [host:port/path ...]\n"
                    "  -v  hexdump every frame to stderr\n"
                    "  -I  send HEAD instead of GET\n");
}

int main(int argc, char **argv)
{
    uint8_t method = M_GET;
    int opt;
    while ((opt = getopt(argc, argv, "vIh")) != -1) {
        switch (opt) {
        case 'v': verbose = 1; break;
        case 'I': method = M_HEAD; break;
        default: usage(); return EXIT_NET;
        }
    }
    int n = argc - optind;
    if (n < 1) { usage(); return EXIT_NET; }

    struct url *urls = calloc((size_t)n, sizeof *urls);
    if (!urls) return EXIT_NET;
    for (int i = 0; i < n; i++) {
        if (parse_url(argv[optind + i], &urls[i]) < 0) {
            fprintf(stderr, "bcurl: bad URL: %s\n", argv[optind + i]);
            return EXIT_NET;
        }
        if (i > 0 && (strcmp(urls[i].host, urls[0].host) || strcmp(urls[i].port, urls[0].port))) {
            fprintf(stderr, "bcurl: all URLs must share one host:port (one connection only)\n");
            return EXIT_NET;
        }
    }

    int fd = dial(&urls[0]);
    if (fd < 0) return EXIT_NET;

    int worst = 0;
    for (int i = 0; i < n; i++) {
        int rc = fetch(fd, &urls[i], method);
        if (rc > worst) worst = rc;
        if (rc == EXIT_NET) break;   /* connection is unusable; never redial */
    }
    close(fd);
    free(urls);
    return worst;
}
