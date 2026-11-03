# Quake (quakegeneric) for the m20 SoC: builds quake.bin, which the core
# firmware's loader copies into SDRAM and runs (see README.md).
# Requires riscv64-unknown-elf-gcc and picolibc-riscv64-unknown-elf.
#
#   make           single-precision floats in hardware (the core's F
#                  extension; build/)
#   make FPU=0     soft float, for a core without it (build-sf/)

CROSS   ?= riscv64-unknown-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE    := $(CROSS)size

# The m20 core: its firmware (SD card, the flash disk, FatFs) and the
# runtime the programs share (system calls, trap handler, linker script).
CORE ?= ../m20
CORE_FW := $(CORE)/firmware
CORE_RT := $(CORE)/runtime

FPU ?= 1
ifeq ($(FPU),1)
B    := build
ARCH := -march=rv32imf_zba_zicbop_zicsr_zifencei_zicbom -mabi=ilp32f
else
B    := build-sf
ARCH := -march=rv32im_zba_zicbop_zicsr_zifencei_zicbom -mabi=ilp32
endif

# Memory: the image from 0x800000, then .bss and the heap (Quake's hunk,
# M20_MEMSIZE, is one malloc), the stack (64 KB) at the top of the 8 MB.
MEMSIZE ?= 0x600000

CFLAGS  := $(ARCH) -O2 -g -specs=picolibc.specs --picolibc-buildtype=release \
           -ffunction-sections -fdata-sections -std=gnu99 \
           -DM20 -DM20_MEMSIZE=$(MEMSIZE) -DM20_BASEDIR=\"QUAKE\" -DFF_FS_READONLY=0 \
           -Iquakegeneric -Iport -I$(CORE_FW) -I$(CORE_FW)/fatfs \
           -Wall -Wno-unused-but-set-variable -Wno-unused-variable -Wno-parentheses \
           -Wno-missing-braces -Wno-dangling-else -Wno-pointer-sign -Wno-trigraphs \
           -Wno-unused-function -Wno-maybe-uninitialized -Wno-implicit-function-declaration
LDFLAGS := $(ARCH) -specs=picolibc.specs --picolibc-buildtype=release \
           -Wl,--defsym=__stack_size=0x10000 \
           -Wl,--gc-sections -Wl,-Map=$(B)/quake.map \
           -T$(CORE_RT)/m9b.ld

QUAKE_SRC := $(wildcard quakegeneric/*.c)
PORT_SRC  := $(wildcard port/*.c) $(CORE_RT)/string.c $(CORE_RT)/syscalls.c $(CORE_RT)/trap.c
PORT_ASM  := $(CORE_RT)/trap_entry.S
FW_SRC    := $(CORE_FW)/sd.c $(CORE_FW)/diskio.c $(CORE_FW)/flashdisk.c $(CORE_FW)/fatfs/ff.c $(CORE_FW)/fatfs/ffunicode.c
SRC       := $(QUAKE_SRC) $(PORT_SRC) $(FW_SRC)
OBJ        = $(patsubst %.c,$(B)/%.o,$(notdir $(SRC))) $(patsubst %.S,$(B)/%.o,$(notdir $(PORT_ASM)))

vpath %.c quakegeneric port $(CORE_RT) $(CORE_FW) $(CORE_FW)/fatfs
vpath %.S $(CORE_RT)

CFLAGS += -MMD -MP
CFLAGS += $(EXTRA)

all: $(B)/quake.bin

$(B)/%.o: %.c | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

# The string functions must not be turned back into calls to themselves.
$(B)/string.o: CFLAGS += -fno-tree-loop-distribute-patterns -fno-builtin

$(B)/%.o: %.S | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/quake.elf: $(OBJ) $(CORE_RT)/m9b.ld Makefile
	$(CC) $(LDFLAGS) $(OBJ) -lm -o $@
	$(SIZE) $@

$(B)/quake.bin: $(B)/quake.elf
	$(OBJCOPY) -O binary $< $@
	@ls -l $@

$(B):
	mkdir -p $(B)

-include $(OBJ:.o=.d)

clean:
	rm -rf build build-sf

.PHONY: all clean
