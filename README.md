# Quake on the m20

Quake (erysdren's [quakegeneric](https://github.com/erysdren/quakegeneric),
the GPL WinQuake source with a small platform layer) as a program for the
m20 SoC (`../core`, a VexiiRiscv with a single-precision FPU on the Tang
Nano 20K).

**Status:** early, but it plays: E1M1 at about 9.3 fps (in the start
area), with keys from the serial terminal only and no sound. It builds
(`make`; `make FPU=0` for soft float), loads through the core's serial
loader (`../core/tools/run.py build-pgo/quake.bin`, 2.6 s at 3 Mbaud) and reads
`QUAKE/id1/pak0.pak` from the disk (`data/README.md`): without an SD card,
a cut-down shareware pak in the SPI flash (`tools/mkpak.py`, then
`../core/tools/mkdisk/mkdisk.sh data/card`).

## Layout

| Path | Contents |
|---|---|
| `quakegeneric/` | quakegeneric's engine sources at `UPSTREAM_COMMIT`, with changes marked `M20` |
| `port/quakegeneric_m20.c` | the platform layer: video, palette, UART keys, time, main loop |
| `port/m20_math.h` | maps the double math functions to their float versions (included ahead of every Quake source) |
| `tools/mkpak.py` | a cut-down `pak0.pak` from the shareware one, for the 6 MB flash disk |

## Inspecting it

`../core/tools/m9bctl.py shot OUT.png` saves the frame on screen (about 7 s
at 115200 baud; the game stops meanwhile). `make PROFILE=1` builds with the
PC-sampling profiler (60 samples a second, at the vertical blank):
`m9bctl.py prof OUT.log` saves what it gathered since the last report, and
`../core/tools/prof.py build-prof/quake.elf OUT.log` sums it by function.

## Changes to quakegeneric

- `quakegeneric.c`: the heap's size (`M20_MEMSIZE`, 6 MB) and the game's
  directory (`QUAKE`; FatFs has no `.`).
- `sys_null.c`, `sys.h`: `Sys_FloatTime` from the core's clock counter,
  `Sys_Error` halts.
- `common.c`: no "registered version" error for the cut-down pak (it
  counts as modified).
- Doubles: the FPU is single precision, so doubles are software.
  `-fsingle-precision-constant`, the math functions mapped to their float
  versions (`port/m20_math.h`), and the hot renderer's `double` locals as
  `float` (`m20_real` in `quakedef.h`: `r_edge.c`, `r_bsp.c`, `d_scan.c`,
  `r_main.c`; `mathlib.c`'s own `sqrt` declaration dropped). They took a
  quarter of the time: 6.4 fps became 9.3. Game time (`cl.time`,
  `sv.time`) stays double, as Quake needs it.
- Built with `-fno-aggressive-loop-optimizations`: Quake indexes past
  array ends on purpose (`model.c`'s texinfo vectors), and GCC's loop
  optimizations broke those loops.

**License:** GPL v2 (`quakegeneric/LICENSE`).
