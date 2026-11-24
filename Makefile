# Quake (quakegeneric) for the m20 SoC: builds quake.bin, which the core
# firmware's loader copies into SDRAM and runs (see README.md).
# Requires riscv64-unknown-elf-gcc and picolibc-riscv64-unknown-elf.
#
#   make           single-precision floats in hardware (the core's F
#                  extension), profile-guided (build-pgo/)
#   make PGO=0     the same without the profile (build/)
#   make FPU=0     soft float, for a core without it (build-sf/)
#   make PERF=1    per-region CPU counters (build-perf/), printed at the
#                  timedemo's end (port/m20_perf.h; a CPU with
#                  --performance-counters 4)
#   make PGO=gen   collects a new profile (build-pgo/, below)
#   make PROFILE=1 the PC-sampling profiler (build-prof/): Ctrl-P on the
#                  UART (../core/tools/m9bctl.py prof) prints it
#   make HASH=1    the frame hash timedemo (build-hash/, below); SCAN=1 and
#                  CHECK=1 add the checks that go with it

CROSS   ?= riscv64-unknown-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE    := $(CROSS)size

# The m20 core: its firmware (SD card, the flash disk, FatFs) and the
# runtime the programs share (system calls, trap handler, linker script).
CORE ?= ../core
CORE_FW := $(CORE)/firmware
CORE_RT := $(CORE)/runtime

FPU ?= 1
ifeq ($(FPU),1)
ifdef PROFILE
B    := build-prof
else ifdef PERF
B    := build-perf
else
B    := build
endif
ARCH := -march=rv32imf_zba_zicbop_zicsr_zifencei_zicbom_zicboz -mabi=ilp32f
else
B    := build-sf
ARCH := -march=rv32im_zba_zicbop_zicsr_zifencei_zicbom_zicboz -mabi=ilp32
endif

# Memory: the image from 0x800000, then .bss and the heap (Quake's hunk,
# M20_MEMSIZE, is one malloc), the stack at the top of the 8 MB. The stack
# holds Quake's edges and surfaces: R_EdgeDrawing and R_ScanEdges alone
# take 214 KB.
# (SCAN=1's pattern area takes 256 KB of it)
ifdef SCAN
MEMSIZE ?= 0x4D0000
endif
# (PROFILE=1: with 0x510000 the profiler's buffers left no room for the
# display buffers, "QG_Init: no memory for the display buffers")
ifdef PROFILE
MEMSIZE ?= 0x480000
endif
MEMSIZE ?= 0x510000
STACK ?= 0x40000
# r_m20span at start-up: 0 Quake's own span drawers, 1 the engine, 2 the C model
SPAN ?= 1

CFLAGS  := $(ARCH) -O2 -flto -g -specs=picolibc.specs --picolibc-buildtype=release \
           -ffunction-sections -fdata-sections -std=gnu99 \
           -DM20 -DM20_MEMSIZE=$(MEMSIZE) -DMINIMUM_MEMORY=$(MEMSIZE) -DM20_SPAN_DEFAULT=\"$(SPAN)\" -DM20_BASEDIR=\"QUAKE\" -DFF_FS_READONLY=0 \
           -Iquakegeneric -Iport -I$(CORE_FW) -I$(CORE_FW)/fatfs \
           -Wall -Wno-unused-but-set-variable -Wno-unused-variable -Wno-parentheses \
           -Wno-missing-braces -Wno-dangling-else -Wno-pointer-sign -Wno-trigraphs \
           -Wno-unused-function -Wno-maybe-uninitialized -Wno-implicit-function-declaration
LDFLAGS := $(ARCH) -O2 -flto -specs=picolibc.specs --picolibc-buildtype=release \
           -Wl,--defsym=__stack_size=$(STACK) \
           -Wl,--gc-sections -Wl,-Map=$(B)/quake.map \
           -T$(CORE_RT)/m9b.ld

QUAKE_SRC := $(wildcard quakegeneric/*.c)
PORT_SRC  := $(wildcard port/*.c) $(CORE_RT)/string.c $(CORE_RT)/syscalls.c $(CORE_RT)/trap.c $(CORE_RT)/shot.c
PORT_ASM  := $(CORE_RT)/trap_entry.S
FW_SRC    := $(CORE_FW)/sd.c $(CORE_FW)/diskio.c $(CORE_FW)/flashdisk.c $(CORE_FW)/fatfs/ff.c $(CORE_FW)/fatfs/ffunicode.c
SRC       := $(QUAKE_SRC) $(PORT_SRC) $(FW_SRC)
OBJ        = $(patsubst %.c,$(B)/%.o,$(notdir $(SRC))) $(patsubst %.S,$(B)/%.o,$(notdir $(PORT_ASM)))

vpath %.c quakegeneric port $(CORE_RT) $(CORE_FW) $(CORE_FW)/fatfs
vpath %.S $(CORE_RT)

# Quake indexes past the end of arrays on purpose in places (model.c's
# texinfo vecs[2][4] as 8 floats, r_part.c): GCC's loop optimizations
# assume that never happens and broke the loops (a bad pointer in
# Mod_LoadTexinfo).
CFLAGS += -fno-aggressive-loop-optimizations
# The FPU is single precision: doubles are software. Unsuffixed constants
# as floats (they made whole expressions double), and the math functions'
# float versions (port/m20_math.h).
QUAKE_FLAGS := -fsingle-precision-constant -include port/m20_math.h
ifdef PROFILE
CFLAGS += -DM9B_PROFILE -DPROF_TOP=0   # every bucket, in address order
endif
# The frame hash timedemo: every frame the same on every run, so that builds,
# clocks and boards can be compared pixel by pixel. Each frame prints the
# hash of the picture, of 15 bands of 24 lines, of what the game decided and
# the count of rand() calls; tools/framehash.py compares the logs of runs
# (tdrun.py saves them). Time goes by fixed steps from the start, and rand()
# is a fixed sequence that the mixer and skipped frames leave alone.
#   SPAN=3   the C model instead of the engine: the reference
#   CHECK=1  each span of the engine's checked against the C model, from SDRAM
#   SCAN=1   memory, arithmetic and fetch checks with the engine at work
#            (port/m20_scan.c)
# EXTRA=-DM20_SPAN_SERIAL waits for the engine after each surface.
ifneq ($(SCAN)$(CHECK),)
HASH := 1
endif
ifdef HASH
B := build-hash
PGO ?= 0
ARGS ?= +host_framerate 0.05 +timedemo demo2
CFLAGS += -DM20_FRAMEHASH -Drand=m20_rand
endif
ifdef SCAN
CFLAGS += -DM20_SCAN
endif
ifdef CHECK
CFLAGS += -DM20_SPAN_CHECK
endif
ifdef ARGS
CFLAGS += -DM20_ARGS='"$(ARGS)"'   # the command line when the disk has no QUAKE/m20args.txt
endif
ifdef PERF
CFLAGS += -DM20_PERF   # per-region CPU counters, printed at the timedemo's end (port/m20_perf.h)
endif
# Profile-guided, the default: pgo/*.gcda, from the three timedemos, about
# 5% to 10% faster. A changed function just loses its profile; collect it
# anew after larger changes. PGO=0 builds without it, in build/.
#
# PGO=gen writes a timedemo's counters into build-pgo/*.gcda
# (port/m20_pgo.c); PGO=use builds from them. Both build in build-pgo/,
# where the .gcda files belong: remove its .o files between them, and copy
# new .gcda files to pgo/. PGO=gen's image
# is larger: make PGO=gen MEMSIZE=0x4A0000 STACK=0x20000
# ARGS="+timedemo demo1", and the same for
# demo2 and demo3, each run with tdrun.py --perf LOG; then
# tools/pgo_merge.py build-pgo LOG...
ifeq ($(FPU)$(PROFILE)$(PERF),1)
PGO ?= use
endif
ifeq ($(PGO),gen)
B := build-pgo
PGO_FLAGS := -fprofile-arcs -fprofile-info-section=gcov_info -fprofile-update=single
CFLAGS += -DM20_PGO_GEN $(PGO_FLAGS)
LDFLAGS += $(PGO_FLAGS)
else ifeq ($(PGO),use)
B := build-pgo
$(shell mkdir -p build-pgo && cp -u pgo/*.gcda build-pgo/)
PGO_FLAGS := -fprofile-use -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch
CFLAGS += $(PGO_FLAGS)
LDFLAGS += $(PGO_FLAGS)
endif
CFLAGS += -MMD -MP
CFLAGS += $(EXTRA)
LDFLAGS += $(EXTRA)

all: $(B)/quake.bin

$(B)/%.o: %.c Makefile | $(B)
	$(CC) $(CFLAGS) $(if $(filter quakegeneric/%,$<),$(QUAKE_FLAGS)) -c $< -o $@

# The string functions must not be turned back into calls to themselves.
$(B)/string.o: CFLAGS += -fno-tree-loop-distribute-patterns -fno-builtin -fno-lto

$(B)/%.o: %.S Makefile | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/quake.elf: $(OBJ) $(CORE_RT)/m9b.ld Makefile
	$(CC) $(LDFLAGS) $(OBJ) -lm -o $@
	$(SIZE) $@

$(B)/quake.bin: $(B)/quake.elf
	$(OBJCOPY) -O binary $< $@
	@ls -l $@

$(B):
	mkdir -p $(B)

# ARGS and SPAN change the built-in command line and r_m20span's
# default: a stamp, so that different values rebuild what uses them
$(B)/args.stamp: FORCE | $(B)
	@echo '$(ARGS)|$(SPAN)' | cmp -s - $@ || echo '$(ARGS)|$(SPAN)' > $@
$(B)/quakegeneric_m20.o $(B)/span_engine.o: $(B)/args.stamp

-include $(OBJ:.o=.d)

# The host test harness (tools/host/m20_host.c, README.md): the same sources
# as a 32-bit x86 Linux program, the span engine's C emulator
# (../core/firmware/span_emu.c) in place of the hardware, linked at
# 0x800000 so that what the engine reads and writes sits in the 8 MB at
# 0x800000 that stand for the SDRAM; the heap the board's size (from its
# ELF when it's built). build-host/quake; with HASH=1 (and SPAN=3, ARGS)
# the frame hash timedemo, build-host-hash/. SDL=0 without the window.
# HOST_FMA=1 contracts float multiply-adds as the board's FPU does
# (fmadd.s; -ffp-contract=fast, GCC's default), with x86's FMA.
HB := build-host$(if $(HASH),-hash)$(if $(filter-out 1,$(SPAN)),-span$(SPAN))$(if $(HOST_FMA),-fma)
BOARD_ELF ?= $(firstword $(wildcard build-pgo/quake.elf build/quake.elf))
HOST_HEAP := $(if $(BOARD_ELF),$(shell set -- $$($(CROSS)nm $(BOARD_ELF) 2>/dev/null | \
               awk '/ __heap_start$$/ {s = $$1} / __heap_end$$/ {e = $$1} END {print s, e}'); \
               [ -n "$$2" ] && printf '0x%x' $$((0x$$2 - 0x$$1))))
HOST_HEAP := $(or $(HOST_HEAP),0x638800)
SDL ?= $(if $(shell PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig pkg-config --exists sdl2 && echo y),1,0)
HOST_CFLAGS := -m32 -msse2 -mfpmath=sse -O2 -g -fno-pie -std=gnu99 $(if $(HOST_FMA),-mfma) \
               -DM20 -DM20_HOST -DM20_HEAP=$(HOST_HEAP)u -DM20_MEMSIZE=$(MEMSIZE) -DMINIMUM_MEMORY=$(MEMSIZE) \
               -DM20_SPAN_DEFAULT=\"$(SPAN)\" -DM20_BASEDIR=\"QUAKE\" -DFF_FS_READONLY=0 \
               $(if $(HASH),-DM20_FRAMEHASH -Drand=m20_rand) $(if $(ARGS),-DM20_ARGS='"$(ARGS)"') \
               -fno-aggressive-loop-optimizations -include tools/host/m20_host.h \
               -Iquakegeneric -Iport -I$(CORE_FW) -I$(CORE_FW)/fatfs \
               -Wall -Wno-unused-but-set-variable -Wno-unused-variable -Wno-parentheses \
               -Wno-missing-braces -Wno-dangling-else -Wno-pointer-sign -Wno-trigraphs \
               -Wno-unused-function -Wno-maybe-uninitialized -Wno-implicit-function-declaration \
               -Wno-format-truncation -Wno-unused-result -Wno-stringop-overflow -Wno-array-bounds \
               -Wno-misleading-indentation -Wno-format-overflow -Wno-dangling-pointer -Wno-array-parameter \
               -U_FORTIFY_SOURCE -fno-stack-protector \
               $(EXTRA)
HOST_LIBS := -lm
ifeq ($(SDL),1)
HOST_CFLAGS += -DM20_SDL $(shell PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig pkg-config --cflags sdl2)
HOST_LIBS += $(shell PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig pkg-config --libs sdl2)
endif
HOST_SRC := $(QUAKE_SRC) $(wildcard port/*.c) tools/host/m20_host.c $(CORE_FW)/span_emu.c
HOST_OBJ := $(patsubst %.c,$(HB)/%.o,$(notdir $(HOST_SRC)))
comma := ,
HOST_WRAP := $(addprefix -Wl$(comma)--wrap=,main malloc free calloc realloc strdup)

vpath %.c tools/host

host: $(HB)/quake

$(HB)/%.o: %.c Makefile | $(HB)
	gcc $(HOST_CFLAGS) $(if $(filter quakegeneric/%,$<),$(QUAKE_FLAGS)) -MMD -MP -c $< -o $@

$(HB)/quake: $(HOST_OBJ)
	gcc -m32 -no-pie -Wl,-Ttext-segment=0x800000 $(HOST_OBJ) $(HOST_WRAP) $(HOST_LIBS) -o $@
	@echo "$@: heap $(HOST_HEAP) (the board's), SDL=$(SDL)"; size $@

$(HB):
	mkdir -p $(HB)

$(HB)/args.stamp: FORCE | $(HB)
	@echo '$(ARGS)|$(SPAN)|$(HASH)' | cmp -s - $@ || echo '$(ARGS)|$(SPAN)|$(HASH)' > $@
$(HB)/quakegeneric_m20.o $(HB)/span_engine.o: $(HB)/args.stamp

-include $(HOST_OBJ:.o=.d)

clean:
	rm -rf build build-sf build-prof build-perf build-pgo build-hash build-host*

.PHONY: all clean FORCE host
