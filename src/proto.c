/* proto.c — BHTTP/1 framing, header-block codec, hexdump. See SPEC.md. */
#include "proto.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static const char *const static_table[STATIC_TABLE_LEN + 1] = {
    NULL,
    "host",            /* 1 */
    "user-agent",      /* 2 */
    "accept",          /* 3 */
    "content-type",    /* 4 */
    "content-length",  /* 5 */
    "server",          /* 6 */
    "date",            /* 7 */
    "connection",      /* 8 */
    "last-modified",   /* 9 */
    "cache-control",   /* 10 */
};

/* ---------- I/O ---------- */

int read_full(int fd, void *dst, size_t n)
{
    uint8_t *p = dst;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return got == 0 ? 0 : -1;
        got += (size_t)r;
    }
    return 1;
}

int write_full(int fd, const void *src, size_t n)
{
    const uint8_t *p = src;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void trace_frame(FILE *trace, const char *dir, const uint8_t *hdr,
                        const uint8_t *payload, uint32_t len, const char *note)
{
    if (!trace) return;
    uint8_t type = hdr[4], flags = hdr[5];
    fprintf(trace, "%s %s frame: length=%u type=0x%02x flags=0x%02x%s%s\n",
            dir, frame_type_name(type), len, type, flags,
            (flags & F_END) ? " (END)" : "", note ? note : "");
    /* dump header + payload as one contiguous run of offsets */
    uint8_t *all = malloc(FRAME_HDR_LEN + (size_t)len);
    if (!all) return;
    memcpy(all, hdr, FRAME_HDR_LEN);
    if (len) memcpy(all + FRAME_HDR_LEN, payload, len);
    hexdump(trace, dir, all, FRAME_HDR_LEN + (size_t)len);
    free(all);
}

int frame_read(int fd, struct frame *f, FILE *trace)
{
    for (;;) {
        uint8_t hdr[FRAME_HDR_LEN];
        int r = read_full(fd, hdr, sizeof hdr);
        if (r == 0) return FR_EOF;
        if (r < 0) return FR_IO;

        f->len = rd32(hdr);
        f->type = hdr[4];
        f->flags = hdr[5];
        f->payload = NULL;
        /* hdr[6..7] reserved: ignored on receipt */

        if (f->len > MAX_FRAME) {
            if (trace)
                fprintf(trace, "< frame length %u exceeds MAX_FRAME %u\n", f->len, MAX_FRAME);
            return FR_TOOBIG;
        }

        f->payload = malloc(f->len ? f->len : 1);
        if (!f->payload) return FR_IO;
        if (f->len && read_full(fd, f->payload, f->len) != 1) {
            frame_free(f);
            return FR_IO;
        }

        if (f->type == T_HEADERS || f->type == T_DATA) {
            trace_frame(trace, "<", hdr, f->payload, f->len, NULL);
            return FR_OK;
        }

        /* Unknown type: MUST skip cleanly. Payload already consumed; drop it. */
        trace_frame(trace, "<", hdr, f->payload, f->len, "  -> unknown type, skipped");
        frame_free(f);
    }
}

int frame_write(int fd, uint8_t type, uint8_t flags,
                const void *payload, uint32_t len, FILE *trace)
{
    uint8_t hdr[FRAME_HDR_LEN] = {
        (uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len,
        type, flags, 0, 0,
    };
    trace_frame(trace, ">", hdr, payload, len, NULL);
    if (write_full(fd, hdr, sizeof hdr) < 0) return -1;
    if (len && write_full(fd, payload, len) < 0) return -1;
    return 0;
}

void frame_free(struct frame *f)
{
    free(f->payload);
    f->payload = NULL;
}

/* ---------- buffer ---------- */

void buf_init(struct buf *b) { b->p = NULL; b->len = b->cap = 0; }
void buf_free(struct buf *b) { free(b->p); buf_init(b); }

static void buf_reserve(struct buf *b, size_t extra)
{
    if (b->len + extra <= b->cap) return;
    size_t cap = b->cap ? b->cap : 128;
    while (cap < b->len + extra) cap *= 2;
    uint8_t *np = realloc(b->p, cap);
    if (!np) { perror("realloc"); exit(3); }
    b->p = np;
    b->cap = cap;
}

void put_u8(struct buf *b, uint8_t v) { buf_reserve(b, 1); b->p[b->len++] = v; }
void put_u16(struct buf *b, uint16_t v) { put_u8(b, (uint8_t)(v >> 8)); put_u8(b, (uint8_t)v); }
void put_bytes(struct buf *b, const void *src, size_t n)
{
    buf_reserve(b, n);
    memcpy(b->p + b->len, src, n);
    b->len += n;
}

size_t put_count(struct buf *b)
{
    put_u8(b, 0);
    return b->len - 1;
}

static int static_index(const char *name)
{
    for (int i = 1; i <= STATIC_TABLE_LEN; i++)
        if (strcasecmp(name, static_table[i]) == 0) return i;
    return 0;
}

void put_field(struct buf *b, size_t count_at, const char *name, const char *value)
{
    int idx = static_index(name);
    put_u8(b, (uint8_t)idx);
    if (idx == 0) {
        size_t nlen = strlen(name);
        put_u8(b, (uint8_t)nlen);
        put_bytes(b, name, nlen);
    }
    size_t vlen = strlen(value);
    put_u16(b, (uint16_t)vlen);
    put_bytes(b, value, vlen);
    b->p[count_at]++;
}

/* ---------- header-block decoding ---------- */

/* Parses "count u8, fields..." and requires it to end exactly at end. */
static int parse_fields(const uint8_t *p, const uint8_t *end, struct fields *fs)
{
    if (end - p < 1) return -1;
    int count = *p++;
    fs->n = 0;
    for (int i = 0; i < count; i++) {
        struct field *fd = &fs->f[fs->n++];
        if (end - p < 1) return -1;
        uint8_t idx = *p++;
        if (idx == 0) {
            if (end - p < 1) return -1;
            uint8_t nlen = *p++;
            if (nlen == 0 || end - p < nlen) return -1;
            fd->name = (const char *)p;
            fd->name_len = nlen;
            p += nlen;
        } else if (idx <= STATIC_TABLE_LEN) {
            fd->name = static_table[idx];
            fd->name_len = (uint8_t)strlen(static_table[idx]);
        } else {
            return -1;
        }
        if (end - p < 2) return -1;
        uint16_t vlen = rd16(p);
        p += 2;
        if (end - p < vlen) return -1;
        fd->value = p;
        fd->value_len = vlen;
        p += vlen;
    }
    return p == end ? 0 : -1;   /* trailing garbage is malformed */
}

int parse_request(const uint8_t *p, uint32_t len, uint8_t *method,
                  const char **path, uint16_t *path_len, struct fields *fs)
{
    const uint8_t *end = p + len;
    if (len < 3) return -1;
    *method = *p++;
    *path_len = rd16(p);
    p += 2;
    if (*path_len == 0 || end - p < *path_len) return -1;
    *path = (const char *)p;
    p += *path_len;
    return parse_fields(p, end, fs);
}

int parse_response(const uint8_t *p, uint32_t len, uint16_t *status, struct fields *fs)
{
    if (len < 2) return -1;
    *status = rd16(p);
    return parse_fields(p + 2, p + len, fs);
}


/* ---------- misc ---------- */

void hexdump(FILE *out, const char *prefix, const uint8_t *data, size_t len)
{
    for (size_t off = 0; off < len; off += 16) {
        fprintf(out, "%s %08zx  ", prefix, off);
        for (size_t i = 0; i < 16; i++) {
            if (off + i < len) fprintf(out, "%02x ", data[off + i]);
            else fputs("   ", out);
            if (i == 7) fputc(' ', out);
        }
        fputs(" |", out);
        for (size_t i = 0; i < 16 && off + i < len; i++) {
            uint8_t c = data[off + i];
            fputc(isprint(c) ? c : '.', out);
        }
        fputs("|\n", out);
    }
}

const char *frame_type_name(uint8_t type)
{
    switch (type) {
    case T_HEADERS: return "HEADERS";
    case T_DATA:    return "DATA";
    default:        return "UNKNOWN";
    }
}

const char *method_name(uint8_t m)
{
    switch (m) {
    case M_GET:  return "GET";
    case M_HEAD: return "HEAD";
    default:     return "?";
    }
}

const char *status_reason(uint16_t status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 500: return "Internal Server Error";
    default:  return "";
    }
}
