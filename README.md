# Quake on the m20

Quake (erysdren's [quakegeneric](https://github.com/erysdren/quakegeneric),
the GPL WinQuake source with a small platform layer) as a program for the
m20 SoC (`../m20`, a VexiiRiscv with a single-precision FPU on the Tang
Nano 20K).

**Status:** early, but it plays: E1M1 at about 6.4 fps (in the start
area), with keys from the serial terminal only and no sound. It builds
(`make`; `make FPU=0` for soft float), loads through the core's serial
loader (`../m20/tools/run.py build/quake.bin`, 2.6 s at 3 Mbaud) and reads
`QUAKE/id1/pak0.pak` from the disk (`data/README.md`): without an SD card,
a cut-down shareware pak in the SPI flash (`tools/mkpak.py`, then
`../m20/tools/mkdisk/mkdisk.sh data/card`).

## Layout

| Path | Contents |
|---|---|
| `quakegeneric/` | quakegeneric's engine sources at `UPSTREAM_COMMIT`, with changes marked `M20` |
| `port/quakegeneric_m20.c` | the platform layer: video, palette, UART keys, time, main loop |
| `tools/mkpak.py` | a cut-down `pak0.pak` from the shareware one, for the 6 MB flash disk |

## Changes to quakegeneric

- `quakegeneric.c`: the heap's size (`M20_MEMSIZE`, 6 MB) and the game's
  directory (`QUAKE`; FatFs has no `.`).
- `sys_null.c`, `sys.h`: `Sys_FloatTime` from the core's clock counter,
  `Sys_Error` halts.
- `common.c`: no "registered version" error for the cut-down pak (it
  counts as modified).
- Built with `-fno-aggressive-loop-optimizations`: Quake indexes past
  array ends on purpose (`model.c`'s texinfo vectors), and GCC's loop
  optimizations broke those loops.

**License:** GPL v2 (`quakegeneric/LICENSE`).
