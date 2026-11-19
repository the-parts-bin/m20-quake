// The span engine's registers (../core/rtl/span_engine.v): SPAN_W(r, v)
// writes SPAN_HW[r] (the command FIFO at r = 0..15, the buffers' and the
// palette's registers above), SPAN_R(r) reads it (20: status, 21..23: the
// counters). The engine addresses SDRAM by byte offsets: SDRAM_OFF(p).
//
// In the host build (make host, tools/host/host.c) the engine's C emulator
// (../core/firmware/span_emu.c) takes the accesses, and SDRAM_OFF stops the
// harness on a pointer outside the 8 MB window at 0x800000 that stands for
// the SDRAM.
#ifndef SPAN_HW_H
#define SPAN_HW_H

#include <stdint.h>

#ifdef M20_HOST
#include "span_emu.h"
uint32_t m20_host_sdram_off(const void *p, const char *what);   // tools/host/host.c
#define SPAN_W(r, v)  span_emu_write((r), (uint32_t)(v))
#define SPAN_R(r)     span_emu_read(r)
#define SDRAM_OFF(p)  m20_host_sdram_off((p), #p)
#else
#define SPAN_HW       ((volatile uint32_t *)0x490000u)
#define SPAN_W(r, v)  (SPAN_HW[r] = (v))
#define SPAN_R(r)     (SPAN_HW[r])
#define SDRAM_OFF(p)  ((uint32_t)(uintptr_t)(p) - 0x800000u)
#endif

#endif
