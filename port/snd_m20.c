// Sound for the m20: Quake's own mixer (snd_dma.c, snd_mix.c) paints
// 11025 Hz 16-bit stereo into a ring here, as into a sound card's DMA
// buffer. The hardware (../m20/README.md, Audio) plays 48 kHz from two
// halves of 256 frames and raises an interrupt when it moves on from one:
// the interrupt (runtime/trap.c -> sound_irq) refills that half from the
// ring, linearly interpolated at 24 kHz with each result held for two
// frames, and clears what it has read, so that if the game stalls the ring
// plays silence instead of itself again. The position it has reached is
// the "DMA position" the mixer paints ahead of.
//
// Integers only in here: the trap handler doesn't save the FP registers.
#include <stdint.h>
#include <string.h>

#include "quakedef.h"
#include "m9b_map.h"

#define IRQ_PENDING (*(volatile uint32_t *)(M9B_CTRL + 0x4u))
#define IRQ_ENABLE  (*(volatile uint32_t *)(M9B_CTRL + 0x8u))
#define AUDIO_CTRL  (*(volatile uint32_t *)(M9B_CTRL + 0xCu))
#define AUDIO_PTR   ((volatile uint32_t *)(M9B_CTRL + 0x10u))   // [0] half A, [1] half B
#define IRQ_VBLANK  (1u << 0)
#define IRQ_AUDIO_A (1u << 1)
#define IRQ_AUDIO_B (1u << 2)

#define RATE   11025u
#define RING   8192u      // frames (0.74 s), a power of 2
#define FRAMES 256u       // a half
// Ring frames per pair of output frames, 16.16: 11025 / 24000
#define STEP   ((uint32_t)(((uint64_t)RATE << 16) / 24000u))

static int16_t ring[RING * 2u] __attribute__((aligned(64)));
static uint32_t audio_buf[2][FRAMES] __attribute__((aligned(256)));
static volatile uint32_t rpos;    // read position, 16.16 frames (wraps with the ring)
static int running;

static void refill(uint32_t half) {
    uint32_t *out = audio_buf[half];
    uint32_t pos = rpos, was = pos >> 16;
    for (uint32_t i = 0; i < FRAMES; i += 2u) {
        uint32_t k = (pos >> 16) & (RING - 1u), n = (k + 1u) & (RING - 1u);
        int32_t f = (int32_t)(pos >> 8 & 255u);
        int32_t l0 = ring[2u * k], r0 = ring[2u * k + 1u];
        int32_t l = l0 + (((ring[2u * n] - l0) * f) >> 8);
        int32_t r = r0 + (((ring[2u * n + 1u] - r0) * f) >> 8);
        uint32_t w = (uint16_t)l | (uint32_t)r << 16;
        out[i] = w;
        out[i + 1u] = w;
        pos += STEP;
    }
    rpos = pos;
    for (uint32_t k = was; k != pos >> 16; k = (k + 1u) & 0xFFFFu) {
        ring[2u * (k & (RING - 1u))] = 0;
        ring[2u * (k & (RING - 1u)) + 1u] = 0;
    }
    for (uintptr_t a = (uintptr_t)out; a < (uintptr_t)(out + FRAMES); a += 64u)
        asm volatile("cbo.clean (%0)" :: "r"(a) : "memory");
    AUDIO_PTR[half] = (uint32_t)(uintptr_t)out - M9B_PSRAM_BASE;
}

// From trap.c, with the IRQ_PENDING bits it has just cleared.
void sound_irq(uint32_t pending) {
    if (!running) return;
    if (pending & IRQ_AUDIO_A) refill(0);
    if (pending & IRQ_AUDIO_B) refill(1);
}

qboolean SNDDMA_Init(void) {
    shm = &sn;
    memset((void *)shm, 0, sizeof(*shm));
    shm->splitbuffer = 0;
    shm->samplebits = 16;
    shm->speed = RATE;
    shm->channels = 2;
    shm->samples = RING * 2u;
    shm->samplepos = 0;
    shm->submission_chunk = 1;
    shm->buffer = (unsigned char *)ring;
    memset(ring, 0, sizeof ring);
    memset(audio_buf, 0, sizeof audio_buf);
    for (uintptr_t a = (uintptr_t)audio_buf; a < (uintptr_t)audio_buf + sizeof audio_buf; a += 64u)
        asm volatile("cbo.clean (%0)" :: "r"(a) : "memory");
    AUDIO_PTR[0] = (uint32_t)(uintptr_t)audio_buf[0] - M9B_PSRAM_BASE;
    AUDIO_PTR[1] = (uint32_t)(uintptr_t)audio_buf[1] - M9B_PSRAM_BASE;
    running = 1;
    IRQ_PENDING = IRQ_AUDIO_A | IRQ_AUDIO_B;
    IRQ_ENABLE |= IRQ_AUDIO_A | IRQ_AUDIO_B;
    AUDIO_CTRL = 1u;
    asm volatile("csrs mie, %0" :: "r"(1u << 11));
    asm volatile("csrs mstatus, %0" :: "r"(1u << 3));
    return true;
}

int SNDDMA_GetDMAPos(void) {
    shm->samplepos = (int)(((rpos >> 16) & (RING - 1u)) * 2u);
    return shm->samplepos;
}

void SNDDMA_Submit(void) {
}

void SNDDMA_Shutdown(void) {
    AUDIO_CTRL = 0u;
    IRQ_ENABLE &= ~(IRQ_AUDIO_A | IRQ_AUDIO_B);
    running = 0;
}
