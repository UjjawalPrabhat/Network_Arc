# BHTTP/1 — HTTP semantics in binary frames

**Status:** course project spec, version 1. The key words MUST, MUST NOT, SHOULD and MAY are used as in RFC 2119.

## 1. Overview

BHTTP/1 carries HTTP-style requests and responses (method, path, status, headers, body) over one TCP connection.
It uses length-prefixed binary frames instead of text lines. A client sends a request, the server sends the
full response, and the connection then stays open for the next request. Requests on a connection are handled
**strictly in order**: the server MUST NOT start a response before it has finished the previous one. A client MAY
send the next request before the previous response arrives (pipelining), and responses still come back in request
order. There is no handshake or preface. The first bytes on the wire are the first frame.

All integers are unsigned and big-endian (network byte order).

## 2. Frame layout

Every frame is an 8-byte header followed by `length` bytes of payload.

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+---------------------------------------------------------------+
|                          length (32)                          |
+---------------+---------------+-------------------------------+
|   type (8)    |   flags (8)   |         reserved (16)         |
+---------------+---------------+-------------------------------+
|                   payload (length bytes) ...                  |
```

| Field    | Bits | Meaning |
|----------|------|---------|
| length   | 32   | Payload size in bytes, not counting the 8-byte header. |
| type     | 8    | `0x01` HEADERS, `0x02` DATA. All other values are reserved for future versions. |
| flags    | 8    | `0x01` END: this is the last frame of the current message. Other bits are reserved. |
| reserved | 16   | Senders MUST set this to 0. Receivers MUST ignore it. |

**Size limit.** A receiver MUST accept payloads up to **1 MiB** (1,048,576 bytes) and MAY reject anything larger.
A sender MUST NOT send a frame larger than 1 MiB. A body larger than that is split across several DATA frames.

**Unknown types: the extension rule.** A receiver that meets a frame whose `type` it does not recognise MUST read
and discard exactly `length` payload bytes and then carry on as if the frame had never been sent. That applies
anywhere on the connection, including between the HEADERS and DATA frames of one message. It MUST NOT treat an
unknown type as an error. Receivers MUST likewise ignore flag bits they do not recognise. This rule is what lets
version 2 add frame types (such as PING or trailers) without breaking version-1 peers.

## 3. HEADERS payload

### 3.1 Request (client → server)

| Field    | Size      | Meaning |
|----------|-----------|---------|
| method   | u8        | `1` = GET, `2` = HEAD. Other values get a 405 response. |
| path_len | u16       | Length of `path`. MUST be at least 1. |
| path     | path_len  | Raw bytes starting with `/`, used as-is (no percent-decoding or query string). |
| count    | u8        | Number of header fields that follow (0–255). |
| fields   | variable  | `count` fields, encoded as in §3.3. |

### 3.2 Response (server → client)

| Field  | Size     | Meaning |
|--------|----------|---------|
| status | u16      | HTTP status code, for example 200 or 404. |
| count  | u8       | Number of header fields that follow. |
| fields | variable | `count` fields, encoded as in §3.3. |

### 3.3 Header field encoding

```
idx (u8)   if idx == 0:  name_len (u8, >= 1)  name (name_len bytes)
           value_len (u16)  value (value_len bytes)
```

If `idx` is 1–10, the field name is the matching entry in the static table below. If `idx` is 0, the name follows
as a length-prefixed literal. Names are lowercase ASCII and compared case-insensitively. An `idx` of 11–255 is
malformed. Values are opaque bytes and are not NUL-terminated.

| idx | name           | idx | name           |
|-----|----------------|-----|----------------|
| 1   | host           | 6   | server         |
| 2   | user-agent     | 7   | date           |
| 3   | accept         | 8   | connection     |
| 4   | content-type   | 9   | last-modified  |
| 5   | content-length | 10  | cache-control  |

The fields MUST use up the HEADERS payload exactly. Bytes left over after the last field make the payload malformed.

## 4. Messages

- **Request:** exactly one HEADERS frame with END set. Requests carry no body in version 1.
- **Response:** one HEADERS frame, then zero or more DATA frames.
  - If there is no body, END is set on the HEADERS frame. That covers responses to HEAD and empty files.
  - Otherwise END is set on the last DATA frame.
  - The response SHOULD include `content-length`. The body length is still defined by the END flag, not by the header.
- A HEAD response carries the same headers as a GET response would, but no DATA frames.

## 5. Server behaviour and errors

The server maps `path` to a file under its document root.
- A path ending in `/`, or a path that names a directory, maps to `index.html` inside that directory.
- After symlinks and `..` are resolved, the target MUST still be inside the root. Otherwise the server answers **403**.

| Situation | Response | Connection |
|-----------|----------|------------|
| File found | 200 + body | kept open |
| File missing or not a regular file | 404 | kept open |
| Path outside the root, or permission denied | 403 | kept open |
| Unknown method | 405 | kept open |
| Malformed HEADERS payload (a length runs past the end of the payload, idx > 10, trailing bytes, path not starting with `/`), or a DATA frame where a request was expected | 400 | **kept open**, because the frame boundary is still known |
| Frame `length` > 1 MiB | 400 | **closed**, because the stream can't be trusted to re-synchronise |
| I/O error after the headers were sent | none | closed (the client sees a truncated body) |

Error responses carry a short `text/plain` body such as `404 Not Found\n`. Either side MAY close the connection
between messages, and a client MUST be ready for that.

## 6. Client behaviour

A client opens **one** connection and sends every request over it. When the server closes the connection, the
client reports an error rather than silently reconnecting. Unknown frame types are skipped as described in §2.
A command-line client SHOULD exit with a non-zero status if any response is 4xx or 5xx.

## 7. Design rationale

**Why the header widths aren't HTTP/2's 24/8/8/31.** HTTP/2 spends 31 bits on a stream id because it multiplexes
many requests at once. BHTTP/1 runs one request at a time, so a stream id would always be the same value. Instead,
16 reserved bits stay in the header. They are always 0 today, and a version 2 could turn them into a stream id
without changing the header size. The length field is 32 bits rather than 24 because that keeps the header at
exactly 8 aligned bytes, which is trivial to read with one `read(fd, hdr, 8)`. That doesn't open the door to
4 GB allocations, because the 1 MiB cap sets the real limit. type and flags each get a byte, the same as in HTTP/2.
256 types and 8 flags leave far more room than version 2 will ever need.

**Why the static table and literals.** The table works like HPACK's static table without the dynamic table or
Huffman coding. The ten names this client and server actually send each cost 1 byte instead of up to 14. Any other
name still works as a literal, so the table never limits what can be expressed.

**Why the path and status are fixed fields.** The path and status are fixed fields rather than pseudo-headers
because every message has them. Putting them at fixed offsets means a receiver can route a request without
walking the field list.

**Why END and not content-length.** The sender only needs to know where the body ends, not its size up front, so a
version-2 server could stream a body of unknown length without changing this spec.
