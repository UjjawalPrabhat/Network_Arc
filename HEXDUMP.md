# Annotated hexdump: `GET /hello.txt`

This is a real capture made with `./bcurl -v localhost:9000/hello.txt` against `./bserve ./www 9000`.
`>` marks bytes the client sent and `<` marks bytes it received. Offsets count from the start of each frame.
`www/hello.txt` holds the 20 bytes `hello, binary world\n`.

## Request: one HEADERS frame, 55 bytes on the wire

```
> 00000000  00 00 00 2f 01 01 00 00  01 00 0a 2f 68 65 6c 6c  |.../......./hell|
> 00000010  6f 2e 74 78 74 03 01 00  0e 6c 6f 63 61 6c 68 6f  |o.txt....localho|
> 00000020  73 74 3a 39 30 30 30 02  00 07 62 63 75 72 6c 2f  |st:9000...bcurl/|
> 00000030  31 03 00 03 2a 2f 2a                              |1...*/*|
```

| Offset | Bytes | Field | Meaning |
|---|---|---|---|
| 0x00 | `00 00 00 2f` | length | 47-byte payload |
| 0x04 | `01` | type | HEADERS |
| 0x05 | `01` | flags | END: the request is complete, with no body |
| 0x06 | `00 00` | reserved | always 0 |
| 0x08 | `01` | method | GET |
| 0x09 | `00 0a` | path_len | 10 |
| 0x0b | `2f 68 65 6c 6c 6f 2e 74 78 74` | path | `/hello.txt` |
| 0x15 | `03` | count | 3 header fields follow |
| 0x16 | `01` | idx | static table 1 = **host** |
| 0x17 | `00 0e` | value_len | 14 |
| 0x19 | `6c 6f … 30 30` | value | `localhost:9000` |
| 0x27 | `02` | idx | static table 2 = **user-agent** |
| 0x28 | `00 07` | value_len | 7 |
| 0x2a | `62 63 75 72 6c 2f 31` | value | `bcurl/1` |
| 0x31 | `03` | idx | static table 3 = **accept** |
| 0x32 | `00 03` | value_len | 3 |
| 0x34 | `2a 2f 2a` | value | `*/*` |

Payload check: 1 + 2 + 10 + 1 + (1+2+14) + (1+2+7) + (1+2+3) = **47** = `0x2f`.

## Response, frame 1: HEADERS, 119 bytes on the wire

```
< 00000000  00 00 00 6f 01 00 00 00  00 c8 05 04 00 19 74 65  |...o..........te|
< 00000010  78 74 2f 70 6c 61 69 6e  3b 20 63 68 61 72 73 65  |xt/plain; charse|
< 00000020  74 3d 75 74 66 2d 38 05  00 02 32 30 06 00 08 62  |t=utf-8...20...b|
< 00000030  73 65 72 76 65 2f 31 07  00 1d 54 75 65 2c 20 30  |serve/1...Tue, 0|
< 00000040  36 20 4f 63 74 20 32 30  32 36 20 31 33 3a 33 35  |6 Oct 2026 13:35|
< 00000050  3a 32 33 20 47 4d 54 09  00 1d 54 75 65 2c 20 30  |:23 GMT...Tue, 0|
< 00000060  36 20 4f 63 74 20 32 30  32 36 20 31 33 3a 33 34  |6 Oct 2026 13:34|
< 00000070  3a 32 37 20 47 4d 54                              |:27 GMT|
```

| Offset | Bytes | Field | Meaning |
|---|---|---|---|
| 0x00 | `00 00 00 6f` | length | 111-byte payload |
| 0x04 | `01` | type | HEADERS |
| 0x05 | `00` | flags | END is **not** set, so a body follows |
| 0x06 | `00 00` | reserved | 0 |
| 0x08 | `00 c8` | status | 200 |
| 0x0a | `05` | count | 5 header fields |
| 0x0b | `04` `00 19` | idx, value_len | **content-type**, 25 bytes |
| 0x0e | `74 65 … 2d 38` | value | `text/plain; charset=utf-8` |
| 0x27 | `05` `00 02` | idx, value_len | **content-length**, 2 bytes |
| 0x2a | `32 30` | value | `20` |
| 0x2c | `06` `00 08` | idx, value_len | **server**, 8 bytes |
| 0x2f | `62 73 … 2f 31` | value | `bserve/1` |
| 0x37 | `07` `00 1d` | idx, value_len | **date**, 29 bytes |
| 0x3a | `54 75 … 4d 54` | value | `Tue, 06 Oct 2026 13:35:23 GMT` |
| 0x57 | `09` `00 1d` | idx, value_len | **last-modified**, 29 bytes |
| 0x5a | `54 75 … 4d 54` | value | `Tue, 06 Oct 2026 13:34:27 GMT` |

Payload check: 2 + 1 + (3+25) + (3+2) + (3+8) + (3+29) + (3+29) = **111** = `0x6f`.

## Response, frame 2: DATA, 28 bytes on the wire

```
< 00000000  00 00 00 14 02 01 00 00  68 65 6c 6c 6f 2c 20 62  |........hello, b|
< 00000010  69 6e 61 72 79 20 77 6f  72 6c 64 0a              |inary world.|
```

| Offset | Bytes | Field | Meaning |
|---|---|---|---|
| 0x00 | `00 00 00 14` | length | 20-byte payload |
| 0x04 | `02` | type | DATA |
| 0x05 | `01` | flags | END: the response is complete |
| 0x06 | `00 00` | reserved | 0 |
| 0x08 | `68 65 … 64 0a` | payload | `hello, binary world\n`, written verbatim to stdout |

After this frame the connection stays open. `bcurl` would send its next request on it if more URLs had been given.

## Totals

The request is 55 bytes. The response is 119 + 28 = 147 bytes, of which 20 are body. Most of the
overhead is the two 29-byte date strings, and a version 2 could replace them with a binary timestamp.
