#!/usr/bin/env python3
"""Rewrites a Quake pak file with every file's data 64-byte aligned (so
32-bit aligned too), zero-filled between files, in the same order, with the
directory 64-byte aligned after them and the file padded with zeros to a
multiple of 2 KB. The stock shareware PAK0.PAK has 163 of its 339 files at
odd offsets, and about 400 KB of junk after its directory, which goes.

From the stock 1.06 shareware PAK0.PAK this gives the SD card's pak0.pak
(18290688 bytes, md5 f2b054dc4d550c9281961c309b21e35a).

Usage: alignpak.py SRC.pak DST.pak [--align N] [--pad N]"""
import argparse
import struct

ap = argparse.ArgumentParser()
ap.add_argument("src")
ap.add_argument("dst")
ap.add_argument("--align", type=int, default=64, help="each file's alignment (default 64)")
ap.add_argument("--pad", type=int, default=2048, help="the whole file's size is a multiple of this (default 2048)")
args = ap.parse_args()


def up(x, m):
    return (x + m - 1) // m * m


src = open(args.src, "rb").read()
magic, off, size = struct.unpack("<4sii", src[:12])
if magic != b"PACK":
    raise SystemExit(f"{args.src}: not a pak file")

out = bytearray(up(12, args.align))          # the header goes in at the end
directory = bytearray()
for i in range(size // 64):
    name, o, n = struct.unpack("<56sii", src[off + 64 * i: off + 64 * i + 64])
    out += bytes(up(len(out), args.align) - len(out))
    directory += struct.pack("<56sii", name, len(out), n)   # (the name field kept as it is)
    out += src[o:o + n]
out += bytes(up(len(out), args.align) - len(out))
dir_off = len(out)
out += directory
out += bytes(up(len(out), args.pad) - len(out))
out[0:12] = struct.pack("<4sii", b"PACK", dir_off, len(directory))
open(args.dst, "wb").write(out)
print(f"{args.dst}: {size // 64} files, {len(out)} bytes")
