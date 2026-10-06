# BHTTP/1: HTTP in binary

This is a course project with two tracks and one protocol.

| | |
|---|---|
| [SPEC.md](SPEC.md) | The protocol, about two pages. It is the only thing the client and server have to agree on. |
| [HEXDUMP.md](HEXDUMP.md) | One complete request and response, annotated byte by byte. |
| `src/bserve.c` | Track 1: the server. |
| `src/bcurl.c` | Track 2: the client. |
| `src/proto.[ch]` | Framing, the header-block codec and the hexdump. |
| `tests/` | End-to-end tests, plus an independent Python peer written only from the spec. |

## Build and run

```sh
make
./bserve ./www 9000                                   # terminal 1
./bcurl -v localhost:9000/index.html                  # terminal 2
./bcurl localhost:9000/hello.txt localhost:9000/logo.png > out   # 2 requests, 1 connection
./bcurl -I localhost:9000/hello.txt                   # HEAD
make test
```

`bcurl` writes the body to stdout. With `-v`, it hexdumps every frame to stderr. It exits with 0 on 2xx/3xx,
1 on 4xx, 2 on 5xx and 3 on a network or protocol error. If several URLs are given, the worst code wins.
`bserve` forks one process per connection and keeps each connection open until the client closes it.
