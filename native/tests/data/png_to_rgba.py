#!/usr/bin/env python3
"""Convert a non-interlaced 8-bit RGB/RGBA PNG into the H1 sidecar format.

Sidecar layout (little-endian):
    magic   8 bytes  b"H1RGBA\0\0"
    width   uint32
    height  uint32
    pixels  width * height * 4 bytes, RGBA8 interleaved

Why a sidecar and not the PNG itself: native/tests/test_heif_color.cpp is the
build gate, and giving it a PNG decoder would mean either a new third-party
dependency inside ceyx/native (forbidden for a test fixture) or a hand-rolled
inflate. The conversion happens once, on the machine that creates the fixture,
with the standard library.

Usage: png_to_rgba.py <in.png> <out.rgba>
"""
import struct
import sys
import zlib


def read_chunks(data):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("not a PNG")
    pos = 8
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        yield ctype, body
        pos += 12 + length  # length + type + body + crc


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: png_to_rgba.py <in.png> <out.rgba>")
    raw = open(sys.argv[1], "rb").read()

    width = height = None
    idat = bytearray()
    channels = None
    for ctype, body in read_chunks(raw):
        if ctype == b"IHDR":
            width, height, depth, colour, comp, filt, interlace = struct.unpack(
                ">IIBBBBB", body[:13])
            if depth != 8:
                raise SystemExit(f"bit depth {depth} unsupported (need 8)")
            if interlace != 0:
                raise SystemExit("interlaced PNG unsupported")
            if colour == 2:
                channels = 3
            elif colour == 6:
                channels = 4
            else:
                raise SystemExit(f"colour type {colour} unsupported (need 2 or 6)")
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break

    if width is None or channels is None:
        raise SystemExit("PNG has no IHDR")

    data = zlib.decompress(bytes(idat))
    stride = width * channels
    out = bytearray(width * height * 4)
    prev = bytearray(stride)
    pos = 0
    for y in range(height):
        ftype = data[pos]
        pos += 1
        line = bytearray(data[pos:pos + stride])
        pos += stride
        for x in range(stride):
            a = line[x - channels] if x >= channels else 0
            b = prev[x]
            c = prev[x - channels] if x >= channels else 0
            if ftype == 1:
                line[x] = (line[x] + a) & 0xFF
            elif ftype == 2:
                line[x] = (line[x] + b) & 0xFF
            elif ftype == 3:
                line[x] = (line[x] + (a + b) // 2) & 0xFF
            elif ftype == 4:
                line[x] = (line[x] + paeth(a, b, c)) & 0xFF
            elif ftype != 0:
                raise SystemExit(f"unknown PNG filter {ftype}")
        for x in range(width):
            src = x * channels
            dst = (y * width + x) * 4
            out[dst + 0] = line[src + 0]
            out[dst + 1] = line[src + 1]
            out[dst + 2] = line[src + 2]
            out[dst + 3] = line[src + 3] if channels == 4 else 255
        prev = line

    with open(sys.argv[2], "wb") as fh:
        fh.write(b"H1RGBA\0\0")
        fh.write(struct.pack("<II", width, height))
        fh.write(bytes(out))
    print(f"[h1] wrote {sys.argv[2]}: {width}x{height} RGBA8")


if __name__ == "__main__":
    main()
