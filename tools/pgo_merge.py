#!/usr/bin/env python3
"""Turns the counters of make PGO=gen timedemos (port/m20_pgo.c, saved by
tdrun.py --perf) into the .gcda files make PGO=use reads.

pgo_merge.py DIR LOG...   (DIR: build-pgo; its old .gcda files go first)

gcov-tool merge-stream does the merge. The freestanding dump leaves each
file's summary empty (runs=0, sum_max=0): GCC then calls all code hot, and
LTO crashes (get_hot_bb_threshold: no profile_info). So they're filled in
here: the runs, and the largest counter of the whole program."""
import glob
import os
import struct
import subprocess
import sys

SUMMARY, ARCS = 0xA1000000, 0x01A10000
d, logs = sys.argv[1], sys.argv[2:]
for f in glob.glob(os.path.join(d, "*.gcda")):
    os.remove(f)
for log in logs:
    text = "".join(l for l in open(log, errors="replace") if not l.startswith("perf "))
    subprocess.run(["riscv64-unknown-elf-gcov-tool", "merge-stream"],
                   input=bytes.fromhex("".join(text.split())), check=True, stdout=subprocess.DEVNULL)


def records(data):
    """(tag, offset of its data, length in bytes) of each record"""
    i = 16                                  # magic, version, stamp, checksum
    while i + 8 <= len(data):
        tag, n = struct.unpack_from("<Ii", data, i)
        if tag == 0:
            break
        yield tag, i + 8, n
        i += 8 + max(n, 0)                  # (a negative length: that many zeros)


files = {f: bytearray(open(f, "rb").read()) for f in glob.glob(os.path.join(d, "*.gcda"))}
top = 0
for data in files.values():
    for tag, at, n in records(data):
        if tag == ARCS and n > 0:
            top = max(top, max(struct.unpack_from(f"<{n // 8}Q", data, at)))
for f, data in files.items():
    assert struct.unpack_from("<II", data, 16) == (SUMMARY, 8), f
    struct.pack_into("<II", data, 24, len(logs), min(top, 0xFFFFFFFF))
    open(f, "wb").write(data)
print(f"{len(files)} files, {len(logs)} runs, largest counter {top}")
