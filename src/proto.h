/* proto.h — BHTTP/1 framing, shared by bserve and bcurl. See SPEC.md. */
#ifndef PROTO_H
#define PROTO_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Frame header: length u32 | type u8 | flags u8 | reserved u16  (8 bytes, big-endian) */
#define FRAME_HDR_LEN   8
#define MAX_FRAME       (1u << 20)      /* largest payload a receiver accepts */

#define T_HEADERS       0x01
#define T_DATA          0x02

#define F_END           0x01

#define M_GET           1
#define M_HEAD          2

#define STATIC_TABLE_LEN 10
#define MAX_FIELDS       255

/* frame_read() results */
enum { FR_OK = 0, FR_EOF = -1, FR_IO = -2, FR_TOOBIG = -3 };

struct frame {
    uint32_t len;
    uint8_t  type;
    uint8_t  flags;
    uint8_t *payload;   /* malloc'd, len bytes; caller frees with frame_free() */
};

/* A decoded header field. Pointers alias the frame payload (not NUL-terminated). */
struct field {
    const char    *name;
    uint8_t        name_len;
    const uint8_t *value;
    uint16_t       value_len;
};

struct fields {
    int          n;
    struct field f[MAX_FIELDS];
};

/* Growable byte buffer used to build HEADERS payloads. */
struct buf {
    uint8_t *p;
    size_t   len, cap;
};

/* I/O */
int  read_full(int fd, void *dst, size_t n);          /* 1 ok, 0 clean EOF at start, -1 error/short */
int  write_full(int fd, const void *src, size_t n);   /* 0 ok, -1 error */

/* Reads the next KNOWN frame. Frames of unknown type are read and discarded
 * here (traced if trace != NULL), so callers only ever see HEADERS or DATA. */
int  frame_read(int fd, struct frame *f, FILE *trace);
int  frame_write(int fd, uint8_t type, uint8_t flags,
                 const void *payload, uint32_t len, FILE *trace);
void frame_free(struct frame *f);

/* Buffer helpers */
void buf_init(struct buf *b);
void buf_free(struct buf *b);
void put_u8(struct buf *b, uint8_t v);
void put_u16(struct buf *b, uint16_t v);
void put_bytes(struct buf *b, const void *src, size_t n);
/* A field block starts with a count byte: put_count() writes it as 0 and
 * returns its offset; each put_field() then bumps it. name must be 1..255
 * bytes and value at most 65535 bytes. */
size_t put_count(struct buf *b);
void put_field(struct buf *b, size_t count_at, const char *name, const char *value);

/* HEADERS payload codecs. Return 0 on success, -1 if malformed. */
int parse_request(const uint8_t *p, uint32_t len, uint8_t *method,
                  const char **path, uint16_t *path_len, struct fields *fs);
int parse_response(const uint8_t *p, uint32_t len, uint16_t *status,
                   struct fields *fs);

void hexdump(FILE *out, const char *prefix, const uint8_t *data, size_t len);
const char *frame_type_name(uint8_t type);
const char *method_name(uint8_t m);
const char *status_reason(uint16_t status);

#endif
