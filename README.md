# Quake on the m20

Quake (erysdren's [quakegeneric](https://github.com/erysdren/quakegeneric),
the GPL WinQuake source with a small platform layer) as a program for the
m20 SoC (`../m20`, a VexiiRiscv with a single-precision FPU on the Tang
Nano 20K).

**Status:** early. It builds (`make`; `make FPU=0` for soft float), loads
through the core's serial loader (`../m20/tools/run.py build/quake.bin`)
and starts; it stops at the first data file until `pak0.pak` is on the
disk (`data/README.md`).

## Layout

| Path | Contents |
|---|---|
| `quakegeneric/` | quakegeneric's engine sources at `UPSTREAM_COMMIT`, with changes marked `M20` |
| `port/quakegeneric_m20.c` | the platform layer: video, palette, UART keys, time, main loop |

## Changes to quakegeneric

- `quakegeneric.c`: the heap's size (`M20_MEMSIZE`, 6 MB) and the game's
  directory (`QUAKE`; FatFs has no `.`).
- `sys_null.c`, `sys.h`: `Sys_FloatTime` from the core's clock counter,
  `Sys_Error` halts.

**License:** GPL v2 (`quakegeneric/LICENSE`).
