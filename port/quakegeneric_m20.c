// quakegeneric platform layer for the m20 SoC (../README.md). A first
// version: enough to see Quake run.
//
// Video: Quake draws its 320 x 240 frame into vid_buffer (vid_null.c);
// QG_DrawFrame copies it into one of two display buffers, writes that back
// from the data cache, and points the line table at it: each Quake row on
// two of the 480 lines, as 8 bpp 2x lines (640 pixels, columns 40..679 by
// VIDEO_HWIN). The table is rewritten at once, not at the vertical blank,
// so a frame can tear.
//
// Input: keys from the UART (tools/run.py's terminal): arrows, Enter, Esc,
// Tab, Backspace, space (jump), f (fire: Ctrl), other characters as
// themselves (Quake's default.cfg binds ',' and '.' to strafing, ` to the
// console). A terminal only sends presses, so a key counts as held until
// 150 ms after its last repeat. (USB keyboard and mouse: to do.)
//
// Time: CLOCK_CNT (the CPU clock, 54 MHz) extended to 64 bits.
//
// Inspection over the UART (../m20/tools/m9bctl.py): 0x1C sends the last
// frame shown (shot), and in a profiling build (make PROFILE=1) 0x10
// (Ctrl-P) prints the PC-sample profile gathered since the last one
// (prof), sampled at each vertical blank.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quakegeneric.h"
#include "clock.h"      // CLOCK_HZ: ../m20/firmware
#include "fatfs/ff.h"   // f_stat (../m20/firmware/fatfs)

int m9b_fs_mount(void);       // ../m20/runtime/syscalls.c
int m9b_fs_use_flash(void);
int disk_from_flash(void);    // ../m20/firmware/diskio.c
#include "m9b_map.h"

#define CLOCK_CNT     (*(volatile uint32_t *)(M9B_TIMING + 0xCu))
#define UART_DATA     (*(volatile uint32_t *)M9B_UART)
#define UART_RX_VALID (1u << 8)
#define VRAM          ((volatile uint32_t *)M9B_VRAM)
#define VRAM_PALETTE  (VRAM + 512)   // 256 x RGB565, two per word
#define VRAM_TABLE    (VRAM + 640)   // one entry per line
#define VIDEO_CTRL    (*(volatile uint32_t *)M9B_CTRL)
#define VIDEO_HWIN    (*(volatile uint32_t *)(M9B_CTRL + 0x18u))
#define LINE_BLANK    (0u << 22)
#define LINE_8BPP     (2u << 22)
#define LINE_2X       (1u << 28)

#define W QUAKEGENERIC_RES_X
#define H QUAKEGENERIC_RES_Y

// ---------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------
static uint32_t clock_last, clock_hi;

static uint64_t ticks(void) {
    uint32_t now = CLOCK_CNT;
    if (now < clock_last) clock_hi++;
    clock_last = now;
    return (uint64_t)clock_hi << 32 | now;
}

double M20_Time(void) {
    return (double)ticks() / (double)CLOCK_HZ;
}

// The runtime's trap handler (../m20/runtime/trap.c): misaligned loads and
// stores emulated, other exceptions reported; its interrupt hooks (no
// interrupts are enabled yet).
extern void trap_entry(void);
extern uint32_t misaligned_traps;
// With -DM20_STALL: every 2 s, where the program was (a stall finder).
void vblank_flip(void) {
#ifdef M20_STALL
    static uint32_t n;
    if (++n % 120u == 0) {
        uint32_t pc, ra;
        asm volatile("csrr %0, mepc" : "=r"(pc));
        ra = 0;
        printf("[pc %08lx]\n", (unsigned long)pc);
        (void)ra;
    }
#endif
}
void sound_irq(uint32_t pending) {
    (void)pending;
}

void M20_Halt(void) {
    for (;;) {
    }
}

// ---------------------------------------------------------------------
// Video
// ---------------------------------------------------------------------
static uint8_t *disp[2];   // 64-byte aligned display buffers
static int back;           // the one not on screen
static uint16_t pal565[256];   // the palette as shown (for screenshots)

// Line-table entry for 8 bpp data at a (cached, even) SDRAM address: the
// 256-byte block and the offset into it in 2-byte units.
static uint32_t line_8bpp(uintptr_t addr) {
    uint32_t off = (uint32_t)addr - M9B_PSRAM_BASE;
    return LINE_8BPP | ((off & 255u) / 2u) << 15 | (off >> 8);
}

#include "quakedef.h"   // (Cvar_RegisterVariable, for the span engine's switch)
extern cvar_t r_m20span, r_m20lit;    // port/span_engine.c

void QG_Init(void) {
    Cvar_RegisterVariable(&r_m20span);
    Cvar_RegisterVariable(&r_m20lit);
    uint8_t *p = malloc(2u * W * H + 64u);
    if (!p) Sys_Error("QG_Init: no memory for the display buffers");
    uintptr_t base = ((uintptr_t)p + 63u) & ~(uintptr_t)63;
    disp[0] = (uint8_t *)base;
    disp[1] = (uint8_t *)(base + W * H);
    memset(disp[0], 0, 2u * W * H);
    for (uint32_t y = 0; y < 480u; y++) VRAM_TABLE[y] = LINE_BLANK;
    VIDEO_HWIN = (40u + 2u * W) << 16 | 40u;
    VIDEO_CTRL = 1;
}

void QG_DrawFrame(void *pixels) {
    uint8_t *buf = disp[back];
    memcpy(buf, pixels, W * H);
    for (uintptr_t a = (uintptr_t)buf; a < (uintptr_t)buf + W * H; a += 64u)
        asm volatile("cbo.clean (%0)" :: "r"(a) : "memory");
    for (uint32_t y = 0; y < H; y++) {
        uint32_t e = line_8bpp((uintptr_t)&buf[y * W]) | LINE_2X;
        VRAM_TABLE[2u * y] = e;
        VRAM_TABLE[2u * y + 1u] = e;
    }
    back ^= 1;
}

void QG_SetPalette(unsigned char palette[768]) {
    for (uint32_t i = 0; i < 256u; i += 2u) {
        const unsigned char *c = &palette[3u * i];
        uint32_t a = (uint32_t)(c[0] >> 3) << 11 | (uint32_t)(c[1] >> 2) << 5 | (c[2] >> 3);
        uint32_t b = (uint32_t)(c[3] >> 3) << 11 | (uint32_t)(c[4] >> 2) << 5 | (c[5] >> 3);
        VRAM_PALETTE[i / 2u] = a | b << 16;
        pal565[i] = (uint16_t)a;
        pal565[i + 1u] = (uint16_t)b;
    }
}

// ---------------------------------------------------------------------
// Input: UART keys, held until 150 ms after their last repeat
// ---------------------------------------------------------------------
#define HOLD_TICKS (CLOCK_HZ / 1000u * 150u)

static int held_key = -1;
static uint32_t held_since;
static int esc_state;   // 0, 1 after ESC, 2 after ESC [
static int queue[16];
static int q_head, q_tail;

static void push(int down, int key) {
    int next = (q_tail + 1) & 15;
    if (next == q_head) return;
    queue[q_tail] = down ? key : -1 - key;
    q_tail = next;
}

static void press(int key) {
    if (held_key != key) {
        if (held_key >= 0) push(0, held_key);
        push(1, key);
        held_key = key;
    }
    held_since = CLOCK_CNT;
}

extern void m20_screenshot(uint32_t w, uint32_t h, const uint16_t *pal565,
                           const uint8_t *pixels, uint32_t stride);   // runtime/shot.c
extern void prof_report(void);                                        // runtime/trap.c

static void uart_poll(void) {
    for (;;) {
        uint32_t d = UART_DATA;
        if (!(d & UART_RX_VALID)) break;
        int c = (int)(d & 0xFFu);
        if (c == 0x1C) {   // m9bctl.py shot: the frame on screen
            if (disp[0]) m20_screenshot(W, H, pal565, disp[back ^ 1], W);
            continue;
        }
#ifdef M9B_PROFILE
        if (c == 0x10) {   // m9bctl.py prof: the profile so far
            prof_report();
            continue;
        }
#endif
        if (esc_state == 1) {
            if (c == '[') { esc_state = 2; continue; }
            esc_state = 0;
            press(K_ESCAPE);
        }
        if (esc_state == 2) {
            esc_state = 0;
            if (c == 'A') press(K_UPARROW);
            else if (c == 'B') press(K_DOWNARROW);
            else if (c == 'C') press(K_RIGHTARROW);
            else if (c == 'D') press(K_LEFTARROW);
            continue;
        }
        if (c == 27) { esc_state = 1; continue; }
        if (c == '\r' || c == '\n') press(K_ENTER);
        else if (c == '\t') press(K_TAB);
        else if (c == 8 || c == 127) press(K_BACKSPACE);
        else if (c == 'f') press(K_CTRL);
        else if (c >= 32 && c < 127) press(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    if (esc_state == 1) {   // a lone ESC: Escape
        esc_state = 0;
        press(K_ESCAPE);
    }
    if (held_key >= 0 && CLOCK_CNT - held_since > HOLD_TICKS) {
        push(0, held_key);
        held_key = -1;
    }
}

int QG_GetKey(int *down, int *key) {
    if (q_head == q_tail) uart_poll();
    if (q_head == q_tail) return 0;
    int e = queue[q_head];
    q_head = (q_head + 1) & 15;
    *down = e >= 0;
    *key = e >= 0 ? e : -1 - e;
    return 1;
}

void QG_GetMouseMove(int *x, int *y) {
    *x = 0;
    *y = 0;
}

void QG_GetJoyAxes(float *axes) {
    for (int i = 0; i < QUAKEGENERIC_JOY_MAX_AXES; i++) axes[i] = 0.0f;
}

void QG_Quit(void) {
    printf("quit\n");
    M20_Halt();
}

// ---------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------
int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    asm volatile("csrw mtvec, %0" :: "r"(&trap_entry));
#if defined(M20_STALL) || defined(M9B_PROFILE)
    *(volatile uint32_t *)(M9B_CTRL + 0x4u) = 15u;   // IRQ_PENDING: clear
    *(volatile uint32_t *)(M9B_CTRL + 0x8u) = 1u;    // IRQ_ENABLE: vblank
    asm volatile("csrs mie, %0" :: "r"(1u << 11));
    asm volatile("csrs mstatus, %0" :: "r"(1u << 3));
#endif
    // The command line: QUAKE/m20args.txt on the disk if it's there (say
    // "+timedemo demo1"), else straight into the first map (the cut-down
    // pak has no demo loop)
    static char line[256];
    static char *args[32] = {"quake", "+map", "e1m1", NULL};
    int nargs = 3;
#ifdef M20_ARGS
    // (make ARGS="...": the command line when the disk has no m20args.txt)
    static char built_in[] = M20_ARGS;
    nargs = 1;
    for (char *p = strtok(built_in, " "); p && nargs < 31; p = strtok(NULL, " "))
        args[nargs++] = p;
    args[nargs] = NULL;
#endif
    // An SD card without the game: the disk image in the flash instead
    if (m9b_fs_mount() == 0 && !disk_from_flash() && f_stat(M20_BASEDIR "/id1", NULL) != FR_OK) {
        printf("no " M20_BASEDIR "/id1 on the SD card: %s\n",
               m9b_fs_use_flash() ? "using the flash's disk image" : "and no disk image in the flash");
    }
    FILE *f = fopen(M20_BASEDIR "/m20args.txt", "r");
    if (f) {
        size_t n = fread(line, 1, sizeof line - 1, f);
        fclose(f);
        line[n] = 0;
        nargs = 1;
        for (char *p = strtok(line, " \t\r\n"); p && nargs < 31; p = strtok(NULL, " \t\r\n"))
            args[nargs++] = p;
        args[nargs] = NULL;
    }
    printf("Quake on the m20:");
    for (int i = 1; i < nargs; i++) printf(" %s", args[i]);
    printf("\n");
    QG_Create(nargs, args);
    double last = M20_Time();
    uint32_t frames = 0;
    double report = last;
    for (;;) {
        double now = M20_Time();
        QG_Tick(now - last);
        last = now;
        frames++;
        if (now - report >= 5.0) {
            // the span engine's counters (../m20/rtl/span_engine.v): pixels,
            // texel cache misses, cpu_clk cycles busy
            static uint32_t e_pix, e_miss, e_busy;
            volatile uint32_t *eng = (volatile uint32_t *)0x490000u;
            uint32_t p = eng[21], m = eng[22], b = eng[23];
            printf("%u frames in %d ms: %d.%d fps, %u misaligned; engine per frame: %u pixels, "
                   "%u misses, %u%% busy\n", (unsigned)frames,
                   (int)((now - report) * 1000.0), (int)(frames / (now - report)),
                   (int)(frames * 10 / (now - report)) % 10, (unsigned)misaligned_traps,
                   (unsigned)((p - e_pix) / frames), (unsigned)((m - e_miss) / frames),
                   (unsigned)((uint64_t)(b - e_busy) * 100u / (uint64_t)((now - report) * CLOCK_HZ)));
            e_pix = p;
            e_miss = m;
            e_busy = b;
            frames = 0;
            report = now;
        }
    }
}
