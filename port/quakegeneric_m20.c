// quakegeneric platform layer for the m20 SoC (../README.md).
//
// Video: RGB565. Quake draws its 640 x 360 frame straight into one of two
// display buffers (vid.buffer: the one not on the screen; the span engine
// draws the world's surfaces into it, lit per pixel); QG_DrawFrame writes
// it back from the data cache, points the line table at it (each Quake row
// on two of the 720 lines, as 16 bpp 2x lines: 640 pixels doubled into
// the 1280 columns) and gives Quake the other. The table is rewritten at
// once, not at the vertical blank, so a frame can tear. d_8to16 is the
// palette as RGB565 (with its flashes), for everything else Quake draws.
//
// Input: keys from the UART (tools/run.py's terminal): arrows, Enter, Esc,
// Tab, Backspace, space (jump), f (fire: Ctrl), other characters as
// themselves (Quake's default.cfg binds ',' and '.' to strafing, ` to the
// console). A terminal only sends presses, so a key counts as held until
// 150 ms after its last repeat. USB keyboards and mice on either of the
// two USB ports too (usb_poll()).
//
// Time: CLOCK_CNT (the CPU clock, 54 MHz) extended to 64 bits.
//
// Inspection over the UART (../core/tools/m9bctl.py): 0x1C sends the last
// frame shown (shot), and in a profiling build (make PROFILE=1) 0x10
// (Ctrl-P) prints the PC-sample profile gathered since the last one
// (prof), sampled at each vertical blank.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quakegeneric.h"
#include "clock.h"      // CLOCK_HZ: ../core/firmware
#include "fatfs/ff.h"   // f_stat (../core/firmware/fatfs)

int m9b_fs_mount(void);       // ../core/runtime/syscalls.c
int m9b_fs_use_flash(void);
int disk_from_flash(void);    // ../core/firmware/diskio.c
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
#define LINE_16BPP    (1u << 22)
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

// The runtime's trap handler (../core/runtime/trap.c): misaligned loads and
// stores emulated, other exceptions reported; its interrupt hooks (no
// interrupts are enabled yet).
extern void trap_entry(void);
extern uint32_t misaligned_traps;
// With -DM20_STALL: every 2 s, where the program was and the span engine's
// state (a stall finder: a hang with interrupts on keeps printing).
void vblank_flip(void) {
#ifdef M20_STALL
    static uint32_t n;
    if (++n % 120u == 0) {
        uint32_t pc;
        volatile uint32_t *hw = (volatile uint32_t *)0x490000u;   // the span engine
        asm volatile("csrr %0, mepc" : "=r"(pc));
        printf("[pc %08lx engine %08lx pixels %08lx misses %08lx cycles %08lx]\n", (unsigned long)pc,
               (unsigned long)hw[20], (unsigned long)hw[21], (unsigned long)hw[22], (unsigned long)hw[23]);
        // The same place as last time: a hang. The registers, and the edge
        // list (r_edge.c's edge_t) from a5 and a6, as R_StepActiveU walks it.
        static uint32_t last, told;
        extern uint32_t *m9b_irq_regs;
        const uint32_t *r = m9b_irq_regs;
        if (pc - last + 8u <= 16u && told < 2u) {
            told++;
            for (int i = 1; i < 32; i++)
                printf("x%-2d %08lx%s", i, (unsigned long)r[i], i % 8 == 7 ? "\n" : "  ");
            printf("\n");
            for (int w = 0; w < 2; w++) {
                uint32_t e = r[15 + w];
                printf("walk from %s\n", w ? "a6" : "a5");
                for (int i = 0; i < 12 && e >= 0x800000u && e < 0xFFFFE0u && !(e & 3u); i++) {
                    const uint32_t *q = (const uint32_t *)(uintptr_t)e;
                    printf(" %08lx u %08lx step %08lx prev %08lx next %08lx surfs %08lx\n", (unsigned long)e,
                           (unsigned long)q[0], (unsigned long)q[1], (unsigned long)q[2], (unsigned long)q[3], (unsigned long)q[4]);
                    e = q[2];
                }
            }
        }
        last = pc;
    }
#endif
}
void M20_Halt(void) {
    for (;;) {
    }
}

// ---------------------------------------------------------------------
// Video
// ---------------------------------------------------------------------
static uint16_t *disp[2];  // 64-byte aligned display buffers, RGB565
static int back;           // the one not on screen (vid.buffer)
unsigned short d_8to16[256];   // the palette as shown, RGB565 (vid.h, SCR)
uint32_t m20_pal888[256];      // and as RGB888, for the span engine
int m20_pal_changed;           // (port/span_engine.c loads it into the engine)

// Line-table entry for 16 bpp data at a (cached, even) SDRAM address: the
// 256-byte block and the offset into it in 2-byte units.
static uint32_t line_16bpp(uintptr_t addr) {
    uint32_t off = (uint32_t)addr - M9B_PSRAM_BASE;
    return LINE_16BPP | ((off & 255u) / 2u) << 15 | (off >> 8);
}

#include "quakedef.h"   // (Cvar_RegisterVariable, for the span engine's switch)
extern cvar_t r_m20span, r_m20lit;    // port/span_engine.c

void QG_Init(void) {
    Cvar_RegisterVariable(&r_m20span);
    Cvar_RegisterVariable(&r_m20lit);
    uint8_t *p = malloc(4u * W * H + 64u);
    if (!p) Sys_Error("QG_Init: no memory for the display buffers");
    uintptr_t base = ((uintptr_t)p + 63u) & ~(uintptr_t)63;
    disp[0] = (uint16_t *)base;
    disp[1] = (uint16_t *)(base + 2u * W * H);
    memset(disp[0], 0, 4u * W * H);
    back = 0;
    vid.buffer = vid.conbuffer = disp[back];
    for (uint32_t y = 0; y < 720u; y++) VRAM_TABLE[y] = LINE_BLANK;
    VIDEO_HWIN = (640u + W) << 16 | (640u - W);   // centred in the 1280 columns
    VIDEO_CTRL = 1;
}

#ifdef M20_FRAMEHASH
// rand(), counted, and the same on every run (-Drand=m20_rand)
static uint32_t m20_rand_calls, m20_rand_state = 1;
int m20_rand(void) {
    m20_rand_calls++;
    m20_rand_state = m20_rand_state * 1103515245u + 12345u;
    return (int)(m20_rand_state >> 16 & 0x7fff);
}
#endif

void QG_DrawFrame(void *pixels) {
    uint16_t *buf = pixels;   // (disp[back])
    for (uintptr_t a = (uintptr_t)buf; a < (uintptr_t)buf + 2u * W * H; a += 64u)
        asm volatile("cbo.clean (%0)" :: "r"(a) : "memory");
#ifdef M20_FRAMEHASH   // each frame's hash, for comparing builds pixel by pixel
    {                  // (with +host_framerate, so that frames don't depend on time)
        static uint32_t n;
        for (uintptr_t a = (uintptr_t)buf; a < (uintptr_t)buf + 2u * W * H; a += 64u)
            asm volatile("cbo.flush (%0)" :: "r"(a) : "memory");
        uint32_t h = 2166136261u;
        const uint32_t *p = (const uint32_t *)buf;
        for (uint32_t i = 0; i < W * H / 2u; i++) h = (h ^ p[i]) * 16777619u;
        printf("fh %lu %08lx", (unsigned long)n++, (unsigned long)h);
        for (uint32_t b = 0; b < 15u; b++) {
            uint32_t g = 2166136261u;
            for (uint32_t i = b * 24u * W / 2u; i < (b + 1u) * 24u * W / 2u; i++) g = (g ^ p[i]) * 16777619u;
            printf(" %04lx", (unsigned long)(g >> 16));
        }
        // What the game decided, apart from how it was drawn
        uint32_t st = 2166136261u;
#define ST(p_, n_) do { const uint32_t *w_ = (const uint32_t *)(p_); \
                        for (uint32_t k_ = 0; k_ < (n_) / 4u; k_++) st = (st ^ w_[k_]) * 16777619u; } while (0)
        ST(r_refdef.vieworg, sizeof(vec3_t));
        ST(r_refdef.viewangles, sizeof(vec3_t));
        ST(&cl.time, sizeof(cl.time));
        ST(cl_dlights, sizeof(cl_dlights));
        for (int e = 0; e < cl_numvisedicts; e++) {
            ST(cl_visedicts[e]->origin, sizeof(vec3_t));
            ST(cl_visedicts[e]->angles, sizeof(vec3_t));
            st = (st ^ (uint32_t)cl_visedicts[e]->frame) * 16777619u;
        }
        printf(" s %08lx %d r %lu\n", (unsigned long)st, cl_numvisedicts, (unsigned long)m20_rand_calls);
#ifdef M20_SCAN
        if (n % 50u == 0) { extern void M20_ScanReport(void); M20_ScanReport(); }
#endif
#ifdef M20_SPAN_CHECK
        if (n % 100u == 0) { extern void M20_SpanCheckReport(void); M20_SpanCheckReport(); }
#endif
    }
#endif
    for (uint32_t y = 0; y < H; y++) {
        uint32_t e = line_16bpp((uintptr_t)&buf[y * W]) | LINE_2X;
        VRAM_TABLE[2u * y] = e;
        VRAM_TABLE[2u * y + 1u] = e;
    }
    back ^= 1;
    vid.buffer = vid.conbuffer = disp[back];
}

void QG_SetPalette(unsigned char palette[768]) {
    // The status bar's pixels are RGB565, made with the palette of the moment
    // and only redrawn when it changes: without this it would keep a pickup's
    // or a hit's tint, a different shade in each display buffer.
    extern void Sbar_Changed(void);
    Sbar_Changed();
    for (uint32_t i = 0; i < 256u; i += 2u) {
        const unsigned char *c = &palette[3u * i];
        uint32_t a = (uint32_t)(c[0] >> 3) << 11 | (uint32_t)(c[1] >> 2) << 5 | (c[2] >> 3);
        uint32_t b = (uint32_t)(c[3] >> 3) << 11 | (uint32_t)(c[4] >> 2) << 5 | (c[5] >> 3);
        VRAM_PALETTE[i / 2u] = a | b << 16;
        d_8to16[i] = (uint16_t)a;
        d_8to16[i + 1u] = (uint16_t)b;
        m20_pal888[i] = (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
        m20_pal888[i + 1u] = (uint32_t)c[3] << 16 | (uint32_t)c[4] << 8 | c[5];
    }
    m20_pal_changed = 1;
}

// ---------------------------------------------------------------------
// Input: UART keys, held until 150 ms after their last repeat
// ---------------------------------------------------------------------
#define HOLD_TICKS (CLOCK_HZ / 1000u * 150u)

static int held_key = -1;
static uint32_t held_since;
static int esc_state;   // 0, 1 after ESC, 2 after ESC [
static int queue[64];
static int q_head, q_tail;

static void push(int down, int key) {
    int next = (q_tail + 1) & 63;
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

extern void m20_screenshot16(uint32_t w, uint32_t h, const uint16_t *pixels,
                             uint32_t stride);                         // runtime/shot.c
extern void prof_report(void);                                        // runtime/trap.c

static void uart_poll(void) {
    for (;;) {
        uint32_t d = UART_DATA;
        if (!(d & UART_RX_VALID)) break;
        int c = (int)(d & 0xFFu);
        if (c == 0x1C) {   // m9bctl.py shot: the frame on screen
            if (disp[0]) m20_screenshot16(W, H, disp[back ^ 1], W);
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

// ---------------------------------------------------------------------
// USB keyboards and mice (../core/rtl/usb_port.v), polled: a keyboard's
// report lists the keys held (HID usage codes, up to four here) and its
// modifiers, so the changes from the last report are the key events; a
// mouse's motion comes from MOUSE_POS, the hardware's running sums.
// ---------------------------------------------------------------------
#define USB_REG(p, o)    (*(volatile uint32_t *)(M9B_USB + 0x20u * (p) + (o)))
#define USB_STATUS(p)    USB_REG(p, 0x00u)   // bits 1:0 type, 15:8 report count
#define USB_KEYS(p)      USB_REG(p, 0x04u)   // {k3, k2, k1, modifiers}
#define USB_KEYS2(p)     USB_REG(p, 0x08u)   // k4
#define USB_MOUSE(p)     USB_REG(p, 0x0Cu)   // {dy, dx, buttons}
#define USB_MOUSE_POS(p) USB_REG(p, 0x14u)   // {y, x}, 16-bit running sums

// HID usage codes to Quake's keys
static int hid_key(uint32_t k) {
    static const char row[] = "\r\0\0\0 -=[]\\\0;'`,./";   // 0x28..0x38
    if (k >= 0x04u && k <= 0x1Du) return 'a' + (int)k - 0x04;
    if (k >= 0x1Eu && k <= 0x26u) return '1' + (int)k - 0x1E;
    if (k == 0x27u) return '0';
    if (k >= 0x3Au && k <= 0x45u) return K_F1 + (int)k - 0x3A;
    switch (k) {
    case 0x28u: return K_ENTER;
    case 0x29u: return K_ESCAPE;
    case 0x2Au: return K_BACKSPACE;
    case 0x2Bu: return K_TAB;
    case 0x48u: return K_PAUSE;
    case 0x49u: return K_INS;
    case 0x4Au: return K_HOME;
    case 0x4Bu: return K_PGUP;
    case 0x4Cu: return K_DEL;
    case 0x4Du: return K_END;
    case 0x4Eu: return K_PGDN;
    case 0x4Fu: return K_RIGHTARROW;
    case 0x50u: return K_LEFTARROW;
    case 0x51u: return K_DOWNARROW;
    case 0x52u: return K_UPARROW;
    default: break;
    }
    return k >= 0x2Cu && k <= 0x38u ? row[k - 0x28u] : 0;
}

static const struct { uint8_t mask; int key; } hid_mods[3] = {
    {0x11u, K_CTRL}, {0x22u, K_SHIFT}, {0x44u, K_ALT},   // left and right
};

static struct {
    uint32_t type, count;
    uint8_t keys[4], mods;
    uint32_t pos;       // mouse: MOUSE_POS at the last look
    uint8_t buttons;
} usb[2];

static int key_in(uint8_t k, const uint8_t *keys) {
    return k == keys[0] || k == keys[1] || k == keys[2] || k == keys[3];
}

static void usb_keys(int p, const uint8_t *keys, uint8_t mods) {
    for (int i = 0; i < 3; i++) {
        int was = (usb[p].mods & hid_mods[i].mask) != 0, is = (mods & hid_mods[i].mask) != 0;
        if (was != is) push(is, hid_mods[i].key);
    }
    for (int i = 0; i < 4; i++) {
        uint8_t k = usb[p].keys[i];
        if (k && !key_in(k, keys) && hid_key(k)) push(0, hid_key(k));
    }
    for (int i = 0; i < 4; i++) {
        uint8_t k = keys[i];
        if (k && !key_in(k, usb[p].keys) && hid_key(k)) push(1, hid_key(k));
    }
    memcpy(usb[p].keys, keys, 4);
    usb[p].mods = mods;
}

static void usb_buttons(int p, uint8_t b) {
    for (int i = 0; i < 3; i++)
        if ((usb[p].buttons ^ b) >> i & 1u) push(b >> i & 1u, K_MOUSE1 + i);
    usb[p].buttons = b;
}

static void usb_poll(void) {
    static const uint8_t none[4];
    for (int p = 0; p < 2; p++) {
        uint32_t st = USB_STATUS(p), type = st & 3u, count = st >> 8 & 0xFFu;
        if (type != usb[p].type) {   // unplugged or replaced: release everything
            usb_keys(p, none, 0);
            usb_buttons(p, 0);
            usb[p].pos = USB_MOUSE_POS(p);
            usb[p].type = type;
            usb[p].count = count;
            printf("usb %d: %s\n", p, type == 1u ? "keyboard" : type == 2u ? "mouse" :
                                      type == 3u ? "gamepad" : "nothing");
            if (type == 2u) Cbuf_AddText("+mlook\n");   // the mouse looks around
            continue;
        }
        if (count == usb[p].count) continue;
        usb[p].count = count;
        if (type == 1u) {
            uint32_t k = USB_KEYS(p);
            uint8_t keys[4] = {(uint8_t)(k >> 8), (uint8_t)(k >> 16), (uint8_t)(k >> 24),
                               (uint8_t)USB_KEYS2(p)};
            usb_keys(p, keys, (uint8_t)k);
        } else if (type == 2u) {
            usb_buttons(p, (uint8_t)(USB_MOUSE(p) & 7u));
        }
    }
}

int QG_GetKey(int *down, int *key) {
    if (q_head == q_tail) {
        uart_poll();
        usb_poll();
    }
    if (q_head == q_tail) return 0;
    int e = queue[q_head];
    q_head = (q_head + 1) & 63;
    *down = e >= 0;
    *key = e >= 0 ? e : -1 - e;
    return 1;
}

void QG_GetMouseMove(int *x, int *y) {
    *x = 0;
    *y = 0;
    for (int p = 0; p < 2; p++) {
        if (usb[p].type != 2u) continue;
        uint32_t pos = USB_MOUSE_POS(p);
        *x += (int16_t)(pos - usb[p].pos);
        *y += (int16_t)((pos >> 16) - (usb[p].pos >> 16));
        usb[p].pos = pos;
    }
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
#ifdef M20_FRAMEHASH   // (the firmware in the flash may be built for another clock)
    *(volatile uint32_t *)(M9B_UART + 0x8u) = UART_DIV_FOR(115200u);
#endif
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
#if defined(M20_FPUTEST) && !defined(M20_SCREEN)
    { extern void M20_RegTest(void), M20_AluTest(void), M20_SdramTest(void), M20_CollideTest(void), M20_FetchTest(void), M20_FetchFpuTest(void); M20_FetchFpuTest(); M20_FetchTest(); M20_CollideTest(); M20_AluTest(); M20_RegTest(); }
#endif
    QG_Create(nargs, args);
#ifdef M20_SCREEN
    { extern void M20_ScreenTest(uint16_t *, uint16_t *, int, int, void (*)(void *)); M20_ScreenTest(disp[0], disp[1], W, H, QG_DrawFrame); }
#endif
#ifdef M20_FPUTEST
    { extern void M20_FpuTest(void); M20_FpuTest(); }
#endif
    double last = M20_Time();
    uint32_t frames = 0;
    double report = last;
    for (;;) {
        double now = M20_Time();
#ifdef M20_IOTRACE
        // frames over 100 ms, and how much of them went to file reads
        extern uint32_t m9b_read_bytes, m9b_read_cycles;
        uint32_t rb = m9b_read_bytes, rc = m9b_read_cycles;
        QG_Tick(now - last);
        double took = M20_Time() - now;
        if (took > 0.1)
            printf("iot slow frame %d ms: read %u bytes in %u ms\n", (int)(took * 1000.0),
                   (unsigned)(m9b_read_bytes - rb), (unsigned)((m9b_read_cycles - rc) / (CLOCK_HZ / 1000u)));
#else
        QG_Tick(now - last);
#endif
        last = now;
        frames++;
        if (now - report >= 5.0) {
            // the span engine's counters (../core/rtl/span_engine.v): pixels,
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
