#!/usr/bin/env python3
"""Grab the framebuffer WayVNC serves, with nothing installed.

    python3 vnc-shot.py 127.0.0.1:5900 out.png

A minimal RFB 3.8 client: no auth (wayvnc runs without a password on loopback),
raw encoding only, one full-screen update.  It is here so the native Android
screen can be inspected without adding an image tool to a game server that has
none.
"""
import socket
import struct
import sys
import zlib


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError("connection closed after %d of %d bytes" % (len(buf), n))
        buf += chunk
    return buf


def read_rect(sock, x, y, w, h, px):
    """Raw encoding: w*h pixels, already in the format we asked for."""
    data = recv_exact(sock, w * h * px)
    return data


def main():
    host, _, port = sys.argv[1].partition(":")
    out = sys.argv[2]
    px = 4  # 32bpp

    sock = socket.create_connection((host, int(port or 5900)), timeout=20)
    sock.settimeout(20)

    version = recv_exact(sock, 12)
    if not version.startswith(b"RFB "):
        raise SystemExit("not an RFB server: %r" % version)
    sock.sendall(b"RFB 003.008\n")

    count = recv_exact(sock, 1)[0]
    types = recv_exact(sock, count)
    if 1 not in types:
        raise SystemExit("server offers no 'None' security: %r" % list(types))
    sock.sendall(b"\x01")
    if recv_exact(sock, 4) != b"\x00\x00\x00\x00":
        raise SystemExit("security handshake failed")

    sock.sendall(b"\x01")  # ClientInit, shared
    width, height = struct.unpack(">HH", recv_exact(sock, 4))
    recv_exact(sock, 16)  # the server's own pixel format, replaced below
    name_len = struct.unpack(">I", recv_exact(sock, 4))[0]
    name = recv_exact(sock, name_len).decode("utf-8", "replace")

    # SetPixelFormat: 32 bits, depth 24, big-endian false, true colour, maxima
    # 255 each, shifts R=16 G=8 B=0.
    sock.sendall(struct.pack(">BxxxBBBBHHHBBBxxx", 0, 32, 24, 0, 1, 255, 255, 255, 16, 8, 0))
    # SetEncodings: raw only.
    sock.sendall(struct.pack(">BxH", 2, 1) + struct.pack(">i", 0))
    sock.sendall(struct.pack(">BBHHHH", 3, 0, 0, 0, width, height))

    while True:
        kind = recv_exact(sock, 1)[0]
        if kind == 0:  # FramebufferUpdate
            recv_exact(sock, 1)
            (nrects,) = struct.unpack(">H", recv_exact(sock, 2))
            pixels = bytearray(width * height * px)
            for _ in range(nrects):
                rx, ry, rw, rh, enc = struct.unpack(">HHHHi", recv_exact(sock, 12))
                if enc != 0:
                    raise SystemExit("unexpected encoding %d" % enc)
                data = read_rect(sock, rx, ry, rw, rh, px)
                for row in range(rh):
                    start = ((ry + row) * width + rx) * px
                    pixels[start:start + rw * px] = data[row * rw * px:(row + 1) * rw * px]
            break
        elif kind == 1:  # SetColourMapEntries: not used with true colour
            recv_exact(sock, 5)
            (n,) = struct.unpack(">H", recv_exact(sock, 2))
            recv_exact(sock, n * 6)
        elif kind == 2:  # Bell
            continue
        elif kind == 3:  # ServerCutText
            recv_exact(sock, 3)
            (n,) = struct.unpack(">I", recv_exact(sock, 4))
            recv_exact(sock, n)
        else:
            raise SystemExit("unexpected server message %d" % kind)

    sock.close()

    # PNG: 8-bit RGB, filter 0 on every row.
    raw = bytearray()
    for row in range(height):
        raw.append(0)
        line = pixels[row * width * px:(row + 1) * width * px]
        for i in range(0, len(line), px):
            b, g, r = line[i], line[i + 1], line[i + 2]
            raw += bytes((r, g, b))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(raw), 6))
           + chunk(b"IEND", b""))
    with open(out, "wb") as fh:
        fh.write(png)
    print("%s: %dx%d (%s)" % (out, width, height, name))


if __name__ == "__main__":
    main()
