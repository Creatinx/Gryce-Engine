#!/usr/bin/env python3
"""Generate a procedural gradient-sky cubemap (6 PNG faces) with stdlib only.

Face order / orientation matches the engine convention:
  +X,-X,+Y,-Y,+Z,-Z ; rows top-down (v downward), no flip.
Texel -> direction:
  +X:(1,-v,-u)  -X:(-1,-v,u)  +Y:(u,1,v)  -Y:(u,-1,-v)  +Z:(u,-v,1)  -Z:(-1,-v,-u)
"""
import math
import os
import struct
import sys
import zlib

SIZE = 512

# palette
ZENITH = (0.18, 0.38, 0.75)
HORIZON = (0.72, 0.80, 0.90)
GROUND_H = (0.42, 0.40, 0.38)
GROUND_D = (0.22, 0.21, 0.20)
SUN_DIR = (0.5, 0.35, -0.6)  # normalized later
SUN_COL = (1.0, 0.95, 0.82)


def norm(v):
    l = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])
    return (v[0] / l, v[1] / l, v[2] / l)


SUN = norm(SUN_DIR)


def sky_color(d):
    d = norm(d)
    y = d[1]
    if y >= 0.0:
        t = min(1.0, y / 0.55)
        t = t * t * (3.0 - 2.0 * t)
        r = HORIZON[0] + (ZENITH[0] - HORIZON[0]) * t
        g = HORIZON[1] + (ZENITH[1] - HORIZON[1]) * t
        b = HORIZON[2] + (ZENITH[2] - HORIZON[2]) * t
    else:
        t = min(1.0, -y / 0.35)
        t = t * t * (3.0 - 2.0 * t)
        r = GROUND_H[0] + (GROUND_D[0] - GROUND_H[0]) * t
        g = GROUND_H[1] + (GROUND_D[1] - GROUND_H[1]) * t
        b = GROUND_H[2] + (GROUND_D[2] - GROUND_H[2]) * t
    # sun disc + glow
    dot = max(0.0, d[0] * SUN[0] + d[1] * SUN[1] + d[2] * SUN[2])
    glow = dot ** 32.0 * 0.25
    disc = 1.0 if dot > 0.9994 else 0.0
    r += SUN_COL[0] * (glow + disc)
    g += SUN_COL[1] * (glow + disc)
    b += SUN_COL[2] * (glow + disc * 0.9)
    return (min(1.0, r), min(1.0, g), min(1.0, b))


def face_dir(face, u, v):
    if face == 0:
        return (1.0, -v, -u)
    if face == 1:
        return (-1.0, -v, u)
    if face == 2:
        return (u, 1.0, v)
    if face == 3:
        return (u, -1.0, -v)
    if face == 4:
        return (u, -v, 1.0)
    return (-1.0, -v, -u)


def write_png(path, w, h, rows_rgb):
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + row for row in rows_rgb)
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 6))
        + chunk(b"IEND", b"")
    )
    with open(path, "wb") as f:
        f.write(png)


def main(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    names = ["px", "nx", "py", "ny", "pz", "nz"]
    for face in range(6):
        rows = []
        for j in range(SIZE):
            v = (j + 0.5) / SIZE * 2.0 - 1.0
            buf = bytearray()
            for i in range(SIZE):
                u = (i + 0.5) / SIZE * 2.0 - 1.0
                r, g, b = sky_color(face_dir(face, u, v))
                buf += bytes((int(r * 255 + 0.5), int(g * 255 + 0.5), int(b * 255 + 0.5)))
            rows.append(bytes(buf))
        write_png(os.path.join(out_dir, names[face] + ".png"), SIZE, SIZE, rows)
        print("wrote", names[face] + ".png")


if __name__ == "__main__":
    main(sys.argv[1])
