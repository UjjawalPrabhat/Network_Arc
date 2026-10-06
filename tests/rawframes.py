#!/usr/bin/env python3
"""Independent BHTTP/1 peer written from SPEC.md only (shares no code with the C).

Client scenarios (print the status codes seen, space-separated):
    rawframes.py PORT unknown | malformed | badidx | stray-data | literal |
                      pipeline | toobig | method
Fake server (one connection, injects an unknown frame before the response):
    rawframes.py PORT fakeserver
"""
import socket
import struct
import sys

HEADERS, DATA, END = 0x01, 0x02, 0x01
TABLE = [None, "host", "user-agent", "accept", "content-type", "content-length",
         "server", "date", "connection", "last-modified", "cache-control"]


def frame(ftype, flags, payload, reserved=0):
    return struct.pack(">IBBH", len(payload), ftype, flags, reserved) + payload


def field(name, value):
    v = value.encode()
    if name in TABLE:
        head = bytes([TABLE.index(name)])
    else:
        n = name.encode()
        head = bytes([0, len(n)]) + n
    return head + struct.pack(">H", len(v)) + v


def request(path, method=1, fields=(("host", "test"),)):
    p = path.encode()
    body = struct.pack(">BH", method, len(p)) + p + bytes([len(fields)])
    body += b"".join(field(n, v) for n, v in fields)
    return frame(HEADERS, END, body)


def recv_exact(s, n):
    out = b""
    while len(out) < n:
        chunk = s.recv(n - len(out))
        if not chunk:
            raise EOFError
        out += chunk
    return out


def read_frame(s):
    """Returns (type, flags, payload), skipping unknown types per the spec."""
    while True:
        length, ftype, flags, _ = struct.unpack(">IBBH", recv_exact(s, 8))
        payload = recv_exact(s, length)
        if ftype in (HEADERS, DATA):
            return ftype, flags, payload


def read_response(s):
    ftype, flags, payload = read_frame(s)
    assert ftype == HEADERS, "expected HEADERS"
    status = struct.unpack(">H", payload[:2])[0]
    body = b""
    while not flags & END:
        ftype, flags, chunk = read_frame(s)
        assert ftype == DATA, "expected DATA"
        body += chunk
    return status, body


def connect(port):
    return socket.create_connection(("127.0.0.1", port), timeout=5)


def client(port, scenario):
    s = connect(port)
    seen = []
    if scenario == "unknown":
        s.sendall(frame(0x7F, 0xFF, b"future extension payload", reserved=0xBEEF))
        s.sendall(request("/hello.txt"))
        seen.append(read_response(s)[0])
    elif scenario == "malformed":
        # path_len claims 200 bytes, only 5 present; the frame itself is well-formed
        s.sendall(frame(HEADERS, END, struct.pack(">BH", 1, 200) + b"/oops" + b"\x00"))
        seen.append(read_response(s)[0])
        s.sendall(request("/hello.txt"))           # same socket still usable
        seen.append(read_response(s)[0])
    elif scenario == "badidx":
        p = b"/hello.txt"
        bad = struct.pack(">BH", 1, len(p)) + p + bytes([1, 42]) + struct.pack(">H", 1) + b"x"
        s.sendall(frame(HEADERS, END, bad))
        seen.append(read_response(s)[0])
        s.sendall(request("/hello.txt"))
        seen.append(read_response(s)[0])
    elif scenario == "stray-data":
        s.sendall(frame(DATA, END, b"nobody asked"))
        seen.append(read_response(s)[0])
        s.sendall(request("/hello.txt"))
        seen.append(read_response(s)[0])
    elif scenario == "literal":
        s.sendall(request("/hello.txt", fields=(("host", "t"), ("x-trace-id", "abc123"))))
        seen.append(read_response(s)[0])
    elif scenario == "pipeline":
        s.sendall(request("/hello.txt") + request("/missing") + request("/index.html"))
        seen += [read_response(s)[0] for _ in range(3)]
    elif scenario == "method":
        s.sendall(request("/hello.txt", method=9))
        seen.append(read_response(s)[0])
    elif scenario == "toobig":
        s.sendall(struct.pack(">IBBH", 0x7FFFFFFF, HEADERS, END, 0))
        seen.append(read_response(s)[0])
        try:
            closed = s.recv(1) == b""
        except OSError:
            closed = True
        seen.append("closed" if closed else "open")
    else:
        sys.exit(f"unknown scenario {scenario}")
    s.close()
    print(" ".join(map(str, seen)))


def fakeserver(port):
    ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", port))
    ls.listen(1)
    print("ready", flush=True)
    c, _ = ls.accept()
    read_frame(c)  # the request
    body = b"via fake server\n"
    hdr = struct.pack(">H", 200) + bytes([2]) + field("content-type", "text/plain") \
        + field("content-length", str(len(body)))
    c.sendall(frame(0x42, 0, b"\x01\x02\x03 ignore me")       # unknown, before HEADERS
              + frame(HEADERS, 0, hdr)
              + frame(0x99, END, b"also ignore")              # unknown, between frames
              + frame(DATA, END, body))
    c.close()
    ls.close()


if __name__ == "__main__":
    port, mode = int(sys.argv[1]), sys.argv[2]
    fakeserver(port) if mode == "fakeserver" else client(port, mode)
