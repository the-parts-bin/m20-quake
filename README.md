# Quake on the m20

Quake (erysdren's [quakegeneric](https://github.com/erysdren/quakegeneric),
the GPL WinQuake source with a small platform layer) as a program for the
m20 SoC (`../core`, a VexiiRiscv with a single-precision FPU on the Tang
Nano 20K).

**Status:** it plays, at 640 × 360 (doubled to the 1280 × 720 picture),
with sound (Quake's mixer at 11025 Hz, played at 48 kHz), USB keyboards
and mice or the serial terminal's keys, and the span engine drawing the
world's surfaces, lit per pixel in RGB565. `timedemo demo1` runs at about
12 fps (demo2 12.8, demo3 12.0); at 320 × 240 it was about 18. It builds
with `make` (profile-guided, `build-pgo/`; `make PGO=0` without the
profile, `make FPU=0` soft float), loads through the core's serial loader
(`../core/tools/run.py build-pgo/quake.bin`) or from the SD card
(`QUAKE.BIN` in the boot screen's browser), and reads `QUAKE/id1/pak0.pak`
from the disk (`data/README.md`): the SD card, or without one a cut-down
shareware pak in the SPI flash (`tools/mkpak.py`, then
`../core/tools/mkdisk/mkdisk.sh data/card`).

## Layout

| Path | Contents |
|---|---|
| `quakegeneric/` | quakegeneric's engine sources at `UPSTREAM_COMMIT`, with changes marked `M20` |
| `port/quakegeneric_m20.c` | the platform layer: video, palette, keys (UART and USB), time, main loop, screenshots and profiles over the UART |
| `port/span_engine.c` | the span engine's driver: each surface's gradients as the engine's fixed-point registers, its spans as commands; also the C model (`r_m20span 2`) |
| `port/snd_m20.c` | sound: Quake's mixer into a ring, resampled into the hardware's halves on the audio interrupt |
| `port/m20_math.h` | maps the double math functions to their float versions (included ahead of every Quake source) |
| `port/m20_pgo.c`, `tools/pgo_merge.py` | profile-guided builds: `make PGO=gen` counts branches on the board and prints them at the timedemo's end |
| `port/m20_perf.c`, `m20_perf.h` | per-region CPU counters (`make PERF=1`, a CPU with performance counters) |
| `port/m20_scan.c`, `port/m20_fputest.c` | checks while the game runs (`SCAN=1`), and the FPU and fetch tests (`EXTRA=-DM20_FPUTEST`) that found the core's block RAM fault |
| `tools/mkpak.py` | a cut-down `pak0.pak` from the shareware one, for the 6 MB flash disk |
| `tools/alignpak.py` | the shareware pak with every file 32-bit aligned, for the SD card |
| `tools/framehash.py` | compares the frame hash timedemo's logs |

## Inspecting it

`../core/tools/m9bctl.py shot OUT.png` saves the frame on screen (about 40 s
for 640 × 360 at 115200 baud; the game stops meanwhile). `../core/tools/tdrun.py`
runs a timedemo in one process that keeps reading the UART, with
screenshots and profiles, and leaves the board reset and quiet. `make PROFILE=1` builds with the
PC-sampling profiler (60 samples a second, at the vertical blank):
`m9bctl.py prof OUT.log` saves what it gathered since the last report, and
`../core/tools/prof.py build-prof/quake.elf OUT.log` sums it by function.

## Comparing builds

`make HASH=1` builds a timedemo that runs the same on every run: time goes by
fixed steps and `rand()` is a fixed sequence, so each frame's hash can be
compared between builds, clocks and boards. `tools/framehash.py LOG...`
compares the logs tdrun.py saves. `make HASH=1 SPAN=3` draws with the C model
of the engine instead, which is the reference. `CHECK=1` also compares each of
the engine's spans with the C model, and `SCAN=1` runs memory, arithmetic and
instruction fetch checks with the engine at work (`port/m20_scan.c`). A run
that finishes without errors proves little: wrong frames at 60 MHz showed up
only here.

## Host test harness

`make host` builds the same sources as a 32-bit x86 Linux program
(`build-host/quake`; gcc-multilib, and libsdl2-dev:i386 for the optional
window), with the span engine's C emulator (`../core/firmware/span_emu.c`)
in place of the hardware: `tools/host/m20_host.c`. It's linked at 0x800000,
so that what the engine reads and writes sits in the 8 MB at 0x800000 that
stand for the SDRAM, and the heap is the board's size (from
`build-pgo/quake.elf` when it's there): what doesn't fit on the board
doesn't fit here. The engine's registers go through `SPAN_W`, `SPAN_R` and
`SDRAM_OFF` (`port/span_hw.h`), the clock and the UART through
`m20_host_clock` and `m20_host_uart` (`M20_HOST` in
`port/quakegeneric_m20.c`); the cache and CSR instructions assemble to
nothing (`tools/host/m20_host.h`). The board's build doesn't change.

It reads `data/sd/QUAKE/id1` (`M20_DATA`), takes Quake's command line, and
runs on a virtual clock, the same on every run (12 frames a second). Run
from `quake/`:

    M20_STEPS=1000 M20_SHOTS=100,500 M20_OUT=/tmp/q ./build-host/quake +timedemo demo1 +r_m20far 1

saves frames 100 and 500 as PPM (what the line table shows) and stops
after frame 1000. `M20_UART='F:text|...'` types on the UART from frame F
(the terminal's keys), `M20_CMDS='F:cmd|...'` runs console commands,
`M20_FRAMELOG=1` prints the engine's work a frame, `M20_WINDOW=1` opens a
window (keys go to the UART); the rest is at the top of `m20_host.c`. At
the exit it prints `span_emu_stats` (the emulator's checks warn once on
stderr). `make host HASH=1` (`SPAN=3`) is the frame hash timedemo
(`build-host-hash/`): the same on every run, but not the board's after the
first frames (float contraction differs; `HOST_FMA=1` comes closer).

## Changes to quakegeneric

- `quakegeneric.c`: the hunk's size (`M20_MEMSIZE`, 5.06 MB: the display
  buffers and a 256 KB surface cache take the rest) and the game's
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
  quarter of the time (6.4 fps became 9.3, before the span engine). Game time (`cl.time`,
  `sv.time`) stays double, as Quake needs it.
- `r_draw.c`: `R_EmitEdge` kept a stale `r_ceilv1`, giving a NaN edge
  step on which `R_StepActiveU` froze (it showed on some bitstreams only,
  and looked like a hardware fault).
- The renderer's surface and span code: the span engine's hooks (the
  world's surfaces to the engine, lit per pixel; `r_m20span` picks the
  engine, the C model or Quake's own drawers) and the RGB565 frame
  (`d_8to16`, the palette with its flashes, for everything else).
- Built with `-fno-aggressive-loop-optimizations`: Quake indexes past
  array ends on purpose (`model.c`'s texinfo vectors), and GCC's loop
  optimizations broke those loops.

**License:** GPL v2 (`LICENSE`, a copy of `quakegeneric/LICENSE`), the port included.
