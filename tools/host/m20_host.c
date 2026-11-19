// The Quake port's host test harness (make host, README.md "Host test
// harness"): the board's program (quakegeneric + port/, unchanged but for
// a few M20_HOST lines) as a 32-bit x86 Linux program, with the span
// engine's C emulator (../core/firmware/span_emu.c) in place of the
// hardware.
//
// Memory: the program is linked at 0x800000 (-Ttext-segment), so its
// image, its static buffers (the z buffer, the warp buffer, the lightmap
// ring) and the heap arena below (the board's heap size, every malloc of
// the game's from it) sit in [0x800000, 0x1000000), the window that stands
// for the 8 MB of SDRAM: the board's SDRAM_OFF(p) = p - 0x800000 holds, and
// span_emu_init((void *)0x800000). SDRAM_OFF (port/span_hw.h) checks each
// pointer it converts. The I/O windows (0x400000 - 0x4FFFFF) are plain
// memory (the line table, the palette, the USB ports' registers: zero,
// nothing plugged in); the span engine's registers go to the emulator,
// the clock and the UART to the functions below (port/quakegeneric_m20.c).
//
// Files: the SD card's files from a directory (M20_DATA, default data/sd:
// QUAKE/id1/pak0.pak), through the C library; FatFs's few calls stubbed.
//
// Time: a virtual clock, the same on every run: 1 ms per read of the
// clock, and each frame at least 1 / M20_FPS s (default 12, about the
// board's) after the last. M20_CLOCK=real: the PC's clock (the default
// with a window). The audio interrupt (port/snd_m20.c's sound_irq) comes
// from the same clock; the sound goes nowhere.
//
// Environment (all optional):
//   M20_STEPS=N        exit after frame N (frames count QG_DrawFrame's calls, from 0)
//   M20_SHOTS=a,b,..   save those frames as fNNNNN.ppm (what the line table shows,
//                      at the game's resolution); M20_SHOTEVERY=k every k-th frame
//   M20_OUT=dir        where the pictures go (default: the directory it started in)
//   M20_PREFIX=s       their names' prefix
//   M20_UART='F:text|F:text'  bytes on the UART from frame F on (one a poll, an
//                      escape sequence at once; \e \r \n \t \\ \xHH): the port's
//                      terminal keys (f fire, arrows as ESC [ A..D, ` the console)
//   M20_CMDS='F:cmd|F:cmd'    console commands (Cbuf_AddText) at frame F (F >= 1)
//   M20_DATA=dir       the SD card's directory (holding QUAKE/)
//   M20_FPS=n          the virtual clock's frame rate; M20_CLOCK=real|virtual
//   M20_WINDOW=1       an SDL window (make host SDL=1, the default when SDL2 is
//                      there): the frame at 1280 x 720, keys to the UART, F12 a picture
//   M20_EMUSTATS=n     span_emu_stats on stderr every n frames (and at the exit)
//   M20_FRAMELOG=1     a line a frame on stderr: the engine's work in it, warping or not
// The command line is Quake's (+timedemo demo1, +map e1m1, +r_m20far 1, ...), in
// place of QUAKE/m20args.txt and the build's ARGS.
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "clock.h"
#include "m9b_map.h"
#include "span_emu.h"
#include "fatfs/ff.h"
#ifdef M20_SDL
#include <SDL.h>
#endif

#define WIN_LO 0x800000u
#define WIN_HI 0x1000000u

extern char __executable_start[], _end[];
extern void Cbuf_AddText(char *text);   // quakegeneric/cmd.c
extern void sound_irq(uint32_t pending);   // port/snd_m20.c

static int frame_no;
static int steps = -1;
static char *shots;
static int shot_every;
static char out_dir[1024] = ".";
static const char *prefix = "";
static char *uart_script, *cmd_script;
static int emustats_every, framelog;
extern int r_dowarp;   // quakegeneric/r_main.c (a qboolean): the view under water, drawn into the warp buffer
static int real_clock;
static uint32_t fps = 12;
void m20_host_exit(int code);

// ---------------------------------------------------------------------
// The SDRAM window
// ---------------------------------------------------------------------
uint32_t m20_host_sdram_off(const void *p, const char *what) {
    uintptr_t a = (uintptr_t)p;
    if (a < WIN_LO || a >= WIN_HI) {
        fprintf(stderr, "SDRAM_OFF(%s): %p is outside the SDRAM window [0x800000, 0x1000000) (frame %d)\n",
                what, p, frame_no);
        abort();
    }
    return (uint32_t)a - WIN_LO;
}

// ---------------------------------------------------------------------
// The heap: the board's size (M20_HEAP, the Makefile: __heap_end -
// __heap_start of the board's ELF), first fit, 8-byte headers, coalescing
// (from trx/src/m20/host_sdl.c)
// ---------------------------------------------------------------------
#ifndef M20_HEAP
#define M20_HEAP 0x638800u
#endif
typedef struct block {
    uint32_t size;   // bytes, header included; bit 0: in use
    uint32_t prev;   // the previous block's size (0: the first)
} block_t;
static uint8_t arena[M20_HEAP] __attribute__((aligned(64)));
static int arena_ready;
static size_t heap_use, heap_peak;

static block_t *heap_next(block_t *b) {
    uint8_t *n = (uint8_t *)b + (b->size & ~7u);
    return n < arena + (M20_HEAP & ~7u) ? (block_t *)n : NULL;
}

void *__wrap_malloc(size_t size) {
    if (!arena_ready) {
        block_t *b = (block_t *)arena;
        b->size = M20_HEAP & ~7u;
        b->prev = 0;
        arena_ready = 1;
    }
    uint32_t need = (uint32_t)((size + sizeof(block_t) + 7u) & ~7u);
    for (block_t *b = (block_t *)arena; b; b = heap_next(b)) {
        if ((b->size & 1u) || b->size < need) continue;
        if (b->size - need >= 64u) {
            block_t *rest = (block_t *)((uint8_t *)b + need);
            rest->size = b->size - need;
            rest->prev = need;
            block_t *after = heap_next(rest);
            if (after) after->prev = rest->size;
            b->size = need;
        }
        b->size |= 1u;
        heap_use += b->size & ~7u;
        if (heap_use > heap_peak) heap_peak = heap_use;
        void *p = b + 1;
        memset(p, 0xA5, (b->size & ~7u) - sizeof(block_t));
        return p;
    }
    fprintf(stderr, "host: out of memory: malloc(%u) with %u of %u bytes in use\n", (unsigned)size,
            (unsigned)heap_use, (unsigned)M20_HEAP);
    return NULL;
}

void __wrap_free(void *p) {
    if (!p) return;
    block_t *b = (block_t *)p - 1;
    b->size &= ~1u;
    heap_use -= b->size;
    block_t *n = heap_next(b);
    if (n && !(n->size & 1u)) {
        b->size += n->size;
        block_t *nn = heap_next(b);
        if (nn) nn->prev = b->size;
    }
    if (b->prev) {
        block_t *pb = (block_t *)((uint8_t *)b - b->prev);
        if (!(pb->size & 1u)) {
            pb->size += b->size;
            block_t *nn = heap_next(pb);
            if (nn) nn->prev = pb->size;
        }
    }
}

void *__wrap_calloc(size_t n, size_t size) {
    void *p = __wrap_malloc(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}

void *__wrap_realloc(void *p, size_t size) {
    if (!p) return __wrap_malloc(size);
    block_t *b = (block_t *)p - 1;
    size_t have = (b->size & ~7u) - sizeof(block_t);
    if (have >= size) return p;
    void *q = __wrap_malloc(size);
    if (q) {
        memcpy(q, p, have);
        __wrap_free(p);
    }
    return q;
}

char *__wrap_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = __wrap_malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

// ---------------------------------------------------------------------
// What the runtime and the firmware gave the board's program
// (../core/runtime, ../core/firmware)
// ---------------------------------------------------------------------
void trap_entry(void) {}
uint32_t misaligned_traps;
void prof_report(void) {}
int m9b_fs_mount(void) { return 0; }
int m9b_fs_use_flash(void) { return 0; }
int disk_from_flash(void) { return 0; }

FRESULT f_stat(const TCHAR *path, FILINFO *fno) {
    struct stat st;
    (void)fno;
    return stat(path, &st) == 0 ? FR_OK : FR_NO_FILE;
}

static void save_ppm(const char *name, uint32_t w, uint32_t h, const uint16_t *const *rows) {
    char path[1400];
    snprintf(path, sizeof path, "%s/%s", out_dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "host: can't write %s: %s\n", path, strerror(errno));
        return;
    }
    fprintf(f, "P6\n%u %u\n255\n", (unsigned)w, (unsigned)h);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint16_t c = rows[y] ? rows[y][x] : 0;
            uint8_t rgb[3] = {(uint8_t)((c >> 11) << 3 | (c >> 13)), (uint8_t)(((c >> 5) & 63u) << 2 | ((c >> 9) & 3u)),
                              (uint8_t)((c & 31u) << 3 | ((c >> 2) & 7u))};
            fwrite(rgb, 3, 1, f);
        }
    fclose(f);
    fprintf(stderr, "host: saved %s\n", path);
}

// m9bctl.py shot (0x1C on the UART): the frame on screen
void m20_screenshot16(uint32_t w, uint32_t h, const uint16_t *pixels, uint32_t stride) {
    static int n;
    const uint16_t **rows = malloc(h * sizeof *rows);   // (the arena: fine)
    for (uint32_t y = 0; y < h; y++) rows[y] = pixels + y * stride;
    char name[64];
    snprintf(name, sizeof name, "%sshot%03d.ppm", prefix, n++);
    save_ppm(name, w, h, rows);
    free(rows);
}

// ---------------------------------------------------------------------
// The picture: what the line table shows (port/quakegeneric_m20.c,
// line_16bpp: 16 bpp lines, each Quake row on two of the 720, doubled)
// ---------------------------------------------------------------------
#define VRAM_TABLE ((volatile uint32_t *)(M9B_VRAM + 640u * 4u))
#define VIDEO_HWIN (*(volatile uint32_t *)(M9B_CTRL + 0x18u))
#define PIC_H 360u
static const uint16_t *pic_rows[PIC_H];
static uint32_t pic_w;

static void picture(void) {
    uint32_t hwin = VIDEO_HWIN, x0 = hwin & 0xFFFFu, x1 = hwin >> 16;
    pic_w = x1 > x0 ? x1 - x0 : 0;
    for (uint32_t y = 0; y < PIC_H; y++) {
        uint32_t e = VRAM_TABLE[2u * y];
        uint32_t w = pic_w >> ((e >> 28) & 1u);
        if (((e >> 22) & 3u) != 1u) {   // blank (or 8 bpp: not used)
            pic_rows[y] = NULL;
            continue;
        }
        uint32_t off = (e & 0x7FFFu) << 8 | ((e >> 15) & 127u) * 2u;
        pic_rows[y] = (const uint16_t *)(uintptr_t)(M9B_PSRAM_BASE + off);
        (void)m20_host_sdram_off(pic_rows[y] + w - 1u, "line table");
    }
    if (VRAM_TABLE[0] >> 28 & 1u) pic_w >>= 1;
}

// ---------------------------------------------------------------------
// The SDL window (M20_WINDOW=1)
// ---------------------------------------------------------------------
#ifdef M20_SDL
static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static void uart_push(const char *s, int n);

static void window_open(void) {
    SDL_Init(SDL_INIT_VIDEO);
    win = SDL_CreateWindow("Quake (m20 host)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, 0);
    ren = SDL_CreateRenderer(win, -1, 0);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, 640, PIC_H);
    SDL_StartTextInput();
}

static void window_frame(void) {
    static uint16_t *px;   // (outside the window: mmap)
    if (!px) px = mmap(NULL, 640u * PIC_H * 2u, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint32_t w = pic_w > 640u ? 640u : pic_w;
    for (uint32_t y = 0; y < PIC_H; y++)
        for (uint32_t x = 0; x < 640u; x++) px[y * 640u + x] = pic_rows[y] && x < w ? pic_rows[y][x] : 0;
    SDL_UpdateTexture(tex, NULL, px, 640 * 2);
    SDL_Rect src = {0, 0, (int)(w ? w : 640u), PIC_H};
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, &src, NULL);
    SDL_RenderPresent(ren);
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) m20_host_exit(0);
        if (e.type == SDL_TEXTINPUT) uart_push(e.text.text, (int)strlen(e.text.text));
        if (e.type != SDL_KEYDOWN) continue;
        switch (e.key.keysym.sym) {
        case SDLK_RETURN: uart_push("\r", 1); break;
        case SDLK_ESCAPE: uart_push("\033", 1); break;
        case SDLK_TAB: uart_push("\t", 1); break;
        case SDLK_BACKSPACE: uart_push("\177", 1); break;
        case SDLK_UP: uart_push("\033[A", 3); break;
        case SDLK_DOWN: uart_push("\033[B", 3); break;
        case SDLK_RIGHT: uart_push("\033[C", 3); break;
        case SDLK_LEFT: uart_push("\033[D", 3); break;
        case SDLK_F12: {
            char name[64];
            snprintf(name, sizeof name, "%sf%05d.ppm", prefix, frame_no);
            save_ppm(name, pic_w, PIC_H, pic_rows);
            break;
        }
        default: break;
        }
    }
}
#endif

// ---------------------------------------------------------------------
// The clock and the audio interrupt
// ---------------------------------------------------------------------
static uint64_t vclock, frame_clock, next_half;
static int half;

static uint64_t real_ticks(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * CLOCK_HZ + (uint64_t)ts.tv_nsec * (CLOCK_HZ / 1000000u) / 1000u;
}

static void audio(void) {
    // 256 frames of 48 kHz a half (port/snd_m20.c)
    while (vclock >= next_half) {
        next_half += (uint64_t)CLOCK_HZ * 256u / 48000u;
        sound_irq(half ? 4u : 2u);
        half ^= 1;
    }
}

uint32_t m20_host_clock(void) {
    vclock = real_clock ? real_ticks() : vclock + CLOCK_HZ / 1000u;
    audio();
    return (uint32_t)vclock;
}

// ---------------------------------------------------------------------
// The UART: the script's bytes (and the window's keys), one a poll
// ---------------------------------------------------------------------
static char uart_q[4096];
static int uart_head, uart_tail;   // [head, tail)
static int uart_run;               // bytes still to give in this poll (an escape sequence)
static int uart_gave;              // a byte given in this poll: the next read ends it
static uint8_t uart_last;
static uint64_t uart_last_clock;

static void uart_push(const char *s, int n) {
    for (int i = 0; i < n; i++) {
        int next = (uart_tail + 1) % (int)sizeof uart_q;
        if (next == uart_head) return;
        uart_q[uart_tail] = s[i];
        uart_tail = next;
    }
}

static int uart_peek(int k) {
    int i = (uart_head + k) % (int)sizeof uart_q;
    for (int j = 0; j < k; j++)
        if ((uart_head + j) % (int)sizeof uart_q == uart_tail) return -1;
    return i == uart_tail ? -1 : (uint8_t)uart_q[i];
}

uint32_t m20_host_uart(void) {
    if (uart_gave && !uart_run) {   // one byte a poll (the port's key logic sees a terminal's pace)
        uart_gave = 0;
        return 0;
    }
    if (uart_head == uart_tail) {
        uart_run = 0;
        return 0;
    }
    uint8_t c = (uint8_t)uart_q[uart_head];
    if (!uart_run) {
        // the same key again: after the port's 150 ms hold, or it's one long press
        if (c == uart_last && vclock - uart_last_clock < (uint64_t)CLOCK_HZ / 1000u * 200u) return 0;
        if (c == 27 && uart_peek(1) == '[') uart_run = 3;   // ESC [ X at once
        else uart_run = 1;
    }
    uart_head = (uart_head + 1) % (int)sizeof uart_q;
    uart_run--;
    uart_gave = 1;
    uart_last = c;
    uart_last_clock = vclock;
    return 0x100u | c;
}

// "F:text|F:text": the entries due at this frame, \-escapes undone
static void script(char *list, int is_cmd) {
    for (char *p = list; p && *p;) {
        char *end = strchr(p, '|');
        int f = atoi(p);
        char *c = strchr(p, ':');
        if (f == frame_no && c && (!end || c < end)) {
            char buf[1024];
            int n = 0;
            for (char *q = c + 1; *q && q != end && n < (int)sizeof buf - 2; q++) {
                if (*q != '\\' || !q[1]) { buf[n++] = *q; continue; }
                q++;
                switch (*q) {
                case 'e': buf[n++] = 27; break;
                case 'r': buf[n++] = '\r'; break;
                case 'n': buf[n++] = '\n'; break;
                case 't': buf[n++] = '\t'; break;
                case 'x': buf[n++] = (char)strtol((char[3]){q[1], q[1] ? q[2] : 0, 0}, NULL, 16); q += 2; break;
                default: buf[n++] = *q; break;
                }
            }
            if (is_cmd) {
                buf[n++] = '\n';
                buf[n] = 0;
                Cbuf_AddText(buf);
            } else {
                uart_push(buf, n);
            }
        }
        p = end ? end + 1 : NULL;
    }
}

static int listed(const char *list, int n) {
    for (const char *p = list; p && *p;) {
        if (atoi(p) == n) return 1;
        p = strchr(p, ',');
        if (p) p++;
    }
    return 0;
}

// ---------------------------------------------------------------------
// Each frame (QG_DrawFrame, the line table just pointed at it)
// ---------------------------------------------------------------------
static void emu_stats(const char *when) {
    span_emu_stats_t *s = &span_emu_stats;
    fprintf(stderr, "span_emu %s (frame %d): spans %u pixels %u (in %u) edges %u lightmaps %u zero_spans %u "
            "z_layout %u z_stale %u\n", when, frame_no, s->spans, s->pixels, s->pixels_in, s->edges,
            s->lightmaps, s->zero_spans, s->z_layout, s->z_stale);
}

void m20_host_exit(int code) {
    fflush(stdout);
    emu_stats("at the exit");
    fprintf(stderr, "host: heap peak %u KB of %u KB\n", (unsigned)(heap_peak >> 10), (unsigned)(M20_HEAP >> 10));
    exit(code);
}

void m20_host_frame(void) {
    picture();
    if ((shots && listed(shots, frame_no)) || (shot_every && frame_no % shot_every == 0)) {
        char name[64];
        snprintf(name, sizeof name, "%sf%05d.ppm", prefix, frame_no);
        save_ppm(name, pic_w, PIC_H, pic_rows);
    }
#ifdef M20_SDL
    if (win) window_frame();
#endif
    if (emustats_every && frame_no % emustats_every == 0) emu_stats("so far");
    if (framelog) {
        static span_emu_stats_t last;
        span_emu_stats_t *e = &span_emu_stats;
        fprintf(stderr, "frame %d: warp %d spans %u pixels %u edges %u lightmaps %u zero %u z_layout %u z_stale %u\n",
                frame_no, r_dowarp, e->spans - last.spans, e->pixels - last.pixels, e->edges - last.edges,
                e->lightmaps - last.lightmaps, e->zero_spans - last.zero_spans, e->z_layout - last.z_layout,
                e->z_stale - last.z_stale);
        last = *e;
    }
    if (steps >= 0 && frame_no >= steps) m20_host_exit(0);
    frame_no++;
    if (!real_clock) {   // the next frame 1 / M20_FPS s on, at least
        uint64_t next = frame_clock + CLOCK_HZ / fps;
        if (vclock < next) vclock = next;
        frame_clock = vclock;
    }
    script(cmd_script, 1);
    script(uart_script, 0);
}

// ---------------------------------------------------------------------
int __real_main(int argc, char **argv);

int __wrap_main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if ((uintptr_t)__executable_start < WIN_LO || (uintptr_t)_end > WIN_HI) {
        fprintf(stderr, "host: the image [%p, %p) isn't inside the SDRAM window [0x800000, 0x1000000)\n",
                (void *)__executable_start, (void *)_end);
        return 1;
    }
    // The I/O windows, as memory
    void *io = mmap((void *)(uintptr_t)M9B_IO_BASE, 0x100000, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (io != (void *)(uintptr_t)M9B_IO_BASE) {
        fprintf(stderr, "host: can't map the I/O windows at 0x400000: %s\n", strerror(errno));
        return 1;
    }
    span_emu_init((void *)(uintptr_t)WIN_LO);

    const char *s;
    if ((s = getenv("M20_STEPS"))) steps = atoi(s);
    shots = getenv("M20_SHOTS");
    if ((s = getenv("M20_SHOTEVERY"))) shot_every = atoi(s);
    if ((s = getenv("M20_PREFIX"))) prefix = s;
    if ((s = getenv("M20_EMUSTATS"))) emustats_every = atoi(s);
    if ((s = getenv("M20_FRAMELOG"))) framelog = atoi(s);
    if ((s = getenv("M20_FPS")) && atoi(s) > 0) fps = (uint32_t)atoi(s);
    uart_script = getenv("M20_UART");
    cmd_script = getenv("M20_CMDS");
    if (!getcwd(out_dir, sizeof out_dir)) strcpy(out_dir, ".");
    if ((s = getenv("M20_OUT"))) {
        if (s[0] == '/') snprintf(out_dir, sizeof out_dir, "%s", s);
        else {
            size_t n = strlen(out_dir);
            snprintf(out_dir + n, sizeof out_dir - n, "/%s", s);
        }
    }
    int window = (s = getenv("M20_WINDOW")) && atoi(s);
    real_clock = window;
    if ((s = getenv("M20_CLOCK"))) real_clock = !strcmp(s, "real");
#ifdef M20_SDL
    if (window) window_open();
#else
    if (window) fprintf(stderr, "host: built without SDL (make host SDL=1): no window\n");
#endif
    const char *data = getenv("M20_DATA");
    if (!data) data = "data/sd";
    if (chdir(data) != 0) {
        fprintf(stderr, "host: no data directory %s: %s\n", data, strerror(errno));
        return 1;
    }
    fprintf(stderr, "host: image %p-%p, heap arena %p-%p (%u KB), data %s\n", (void *)__executable_start,
            (void *)_end, (void *)arena, (void *)(arena + M20_HEAP), (unsigned)(M20_HEAP >> 10), data);
    if (real_clock) vclock = real_ticks();
    next_half = vclock;
    script(uart_script, 0);   // (frame 0's bytes; commands from frame 1, once Quake has its command buffer)
    return __real_main(argc, argv);
}
