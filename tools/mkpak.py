#!/usr/bin/env python3
"""Builds a cut-down pak0.pak from Quake's shareware PAK0.PAK, small enough
for the m20's 6 MB flash disk: by default the maps given (e1m1), the brush
models (maps/b_*.bsp), all the models, graphics and progs.dat, no sounds
(the port has no sound driver yet: snd_null loads none) and no demos, with
quake.rc's startdemos line dropped (the demos' maps aren't there).

Usage: mkpak.py PAK0.PAK out/pak0.pak [--maps e1m1,e1m3] [--sounds] [--demos]"""
import argparse
import struct

ap = argparse.ArgumentParser()
ap.add_argument("src")
ap.add_argument("dst")
ap.add_argument("--maps", default="e1m1", help="maps to keep, comma separated")
ap.add_argument("--sounds", action="store_true", help="keep sound/")
ap.add_argument("--demos", action="store_true", help="keep the demos (and startdemos)")
args = ap.parse_args()

src = open(args.src, "rb").read()
magic, off, size = struct.unpack("<4sii", src[:12])
if magic != b"PACK":
    raise SystemExit(f"{args.src}: not a pak file")
files = []
for i in range(size // 64):
    name, o, n = struct.unpack("<56sii", src[off + 64 * i: off + 64 * i + 64])
    files.append((name.split(b"\0")[0].decode(), src[o:o + n]))

maps = {f"maps/{m}.bsp" for m in args.maps.split(",") if m}
keep = []
for name, data in files:
    # (maps/b_*.bsp are brush models, the ammo boxes and such: every map
    # needs them)
    if name.startswith("maps/") and name not in maps and not name.startswith("maps/b_"):
        continue
    if name.startswith("sound/") and not args.sounds:
        continue
    if name.endswith(".dem") and not args.demos:
        continue
    if name == "quake.rc" and not args.demos:
        data = b"\n".join(l for l in data.split(b"\n") if not l.startswith(b"startdemos"))
    keep.append((name, data))

missing = maps - {n for n, _ in keep}
if missing:
    raise SystemExit(f"not in {args.src}: {', '.join(sorted(missing))}")

body = bytearray()
directory = bytearray()
for name, data in keep:
    directory += struct.pack("<56sii", name.encode(), 12 + len(body), len(data))
    body += data
out = struct.pack("<4sii", b"PACK", 12 + len(body), len(directory)) + body + directory
open(args.dst, "wb").write(out)
print(f"{args.dst}: {len(keep)} of {len(files)} files, {len(out) / 1024:.0f} KB")
