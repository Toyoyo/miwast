/* music.c - SNDH music through gwEm's Timer C player (sndh.c, sndhisr.s,
 * from stvn).
 *
 * The tune that is playing is held at the top of the sprites' block
 * (vm.c, arena_cede): the block has room for the longest, and what a
 * shorter one leaves -- 110KB for the credits' tune -- is the sprites'.
 * The conversions keep everything inside the file (the player's variables
 * and its 4KB DMA ring are in it, the samples end it), and the player is
 * position-independent, so a tune needs its file's size, wherever.
 *
 * The player's interrupt runs code inside the tune, so **a tune is always
 * stopped before its memory is written** -- two player ISRs, or one
 * running over a half-loaded tune, or over sprites, is a sure crash.
 * music_load is the only writer, and it stops first, unconditionally;
 * a stop hands the memory back to the sprites.
 */

#include <stdio.h>
#include <string.h>
#include <mint/osbind.h>

#include "st.h"
#include "sndh.h"

static u8      *g_buf;              /* the tune loaded, if any */
static s32      g_bufsize;          /* the longest tune */
#ifdef MW_RHYTHMTEST
static u8      *g_own;              /* rtest.c runs with no sprites' block */
#endif
static int      g_playing = -1;     /* track id, or -1 */
static int      g_paused;           /* by music_pause */
static SNDHTune g_tune;

#ifdef MW_NATFEATS
/* What a tune may hook: the 68000 autovectors and the MFP's 16 vectors,
 * and the MFP's enable and mask registers. Snapshotted before the first
 * tune and compared after every stop, so a player whose deinit leaves
 * something behind is named in the trace. */
static u32 g_vec0[64];
static u8  g_mfp0[4];

static void hw_snapshot(u32 *vec, u8 *mfp)
{
    int i;
    for (i = 0; i < 64; i++)
        vec[i] = ((volatile u32 *)0x0L)[i + 24];    /* $60..$15C */
    mfp[0] = *(volatile u8 *)0xFFFFFA07L;            /* IERA */
    mfp[1] = *(volatile u8 *)0xFFFFFA09L;            /* IERB */
    mfp[2] = *(volatile u8 *)0xFFFFFA13L;            /* IMRA */
    mfp[3] = *(volatile u8 *)0xFFFFFA15L;            /* IMRB */
}

static void hw_compare(const char *when)
{
    u32 vec[64];
    u8 mfp[4];
    int i, diff = 0;

    hw_snapshot(vec, mfp);
    for (i = 0; i < 64; i++) {
        if (vec[i] != g_vec0[i]) {
            trace("hw %s: vector $%03x %08lx -> %08lx", when, (i + 24) * 4,
                  (unsigned long)g_vec0[i], (unsigned long)vec[i]);
            diff++;
        }
    }
    for (i = 0; i < 4; i++) {
        if (mfp[i] != g_mfp0[i]) {
            trace("hw %s: MFP %s %02x -> %02x", when,
                  i == 0 ? "IERA" : i == 1 ? "IERB" : i == 2 ? "IMRA" : "IMRB",
                  g_mfp0[i], mfp[i]);
            diff++;
        }
    }
    if (!diff)
        trace("hw %s: vectors and MFP as at start", when);
}
#else
#define hw_compare(when) ((void)0)
#endif

int music_init(void)
{
#ifdef MW_NATFEATS
    hw_snapshot(g_vec0, g_mfp0);
#endif
    g_bufsize = pak_max_size("M");
    if (g_bufsize < 0)
        g_bufsize = 0;              /* a pak with no music is fine */
    g_bufsize = (g_bufsize + 3) & ~3L;
#ifdef MW_RHYTHMTEST
    if (g_bufsize) {
        g_own = (u8 *)Malloc(g_bufsize);
        trace("music: %ld byte buffer at %p", (long)g_bufsize, g_own);
        return g_own != NULL;
    }
#endif
    return 1;
}

s32 music_max(void)
{
    return g_bufsize;
}

void music_stop(void)
{
    if (g_playing >= 0) {
        /* Paused or not: the stop puts back the vector the player found,
         * and the DMA is stopped by the deinit or already. */
        g_paused = 0;
        SNDH_StopTune();
        trace("music: stop %d", g_playing);
        hw_compare("after stop");
    }
    g_playing = -1;
    if (g_buf) {
        g_buf = NULL;
#ifndef MW_RHYTHMTEST
        arena_cede(0);
#endif
    }
}

/* `play music`. Ren'Py restarts a track that is already playing unless
 * if_changed is set (audio.c:1064 in the C89 engine, after renpy's
 * music.play). A restart reloads the file: SNDH init routines may depack or
 * patch themselves in place, so re-running init over a used buffer is not
 * safe in general. */
void music_play(int id, u16 flags)
{
    if ((flags & MWB_MUS_IFCHANGED) && id == g_playing)
        return;
    if (music_load(id))
        music_start(id);
}

/* music_play's two halves, for a caller that times the start (rtest.c):
 * the load stops what plays and reads the tune, 1MB or more; the start is
 * only the player's init. */
int music_load(int id)
{
    char name[16];
    s32 size;

    music_stop();
    sprintf(name, "M%02d.SND", id);
    size = pak_size(name);
    if (size <= 0 || size > g_bufsize) {
        trace("music: no %s", name);
        return 0;
    }
#ifdef MW_RHYTHMTEST
    g_buf = g_own;
#else
    g_buf = arena_cede(((u32)size + 3) & ~3UL);
#endif
    if (!g_buf) {
        trace("music: no room for %s", name);
        return 0;
    }
    if (!pak_read(name, g_buf, (u32)size)) {
        trace("music: cannot read %s", name);
        music_stop();
        return 0;
    }
    memset(&g_tune, 0, sizeof(g_tune));
    SNDH_GetTuneInfo(g_buf, &g_tune);
    if (!g_tune.tuneadr) {
        trace("music: %s is not SNDH", name);
        music_stop();
        return 0;
    }
    return 1;
}

/* ---- pausing, for the rhythm game's quit dialog ----
 *
 * Ren'Py pauses the channel (renpy.music.set_pause, rhythm_game_displayable
 * .rpy:524) and the song resumes where it was; the game's clock waits with
 * it. The SNDH player has no pause, and the conversions are ADPCM decoded
 * into a looping DMA ring by the player's 50Hz call (music_start's trace;
 * NOTES.md, "The rhythm game"). So, under the player's feet: Timer C gets
 * TOS's handler back, which stops the decoding but not the 200Hz clock,
 * and the DMA stops, its place noted. Resuming starts the DMA there -- the
 * frame registers are read when it starts -- then sets the ring's own
 * start back, which the STE takes at the frame's end (its registers are
 * double-buffered), and hands Timer C back to the player. */
#define TIMER_C_VEC (*(volatile u32 *)0x114L)
#define DMA(o)      (*(volatile u8 *)(0xFFFF8900L + (o)))
#define DMA_CTRL    0x01
#define DMA_START   0x03
#define DMA_COUNT   0x09
#define DMA_END     0x0F

static u32 g_tc_tos;                /* Timer C's vector without the player */
static u32 g_tc_player;             /* the player's, while paused */
static u32 g_dma_pos, g_dma_start, g_dma_end;
static u8  g_dma_ctrl;

static u16 ints_off(void)
{
    u16 sr;

    __asm__ volatile("move.w %%sr,%0\n\tor.w #0x0700,%%sr"
                     : "=d"(sr) : : "memory", "cc");
    return sr;
}

static void ints_restore(u16 sr)
{
    __asm__ volatile("move.w %0,%%sr" : : "d"(sr) : "memory", "cc");
}

/* A DMA address register: three bytes, high first, a word apart. */
static u32 dma_get(int o)
{
    return ((u32)DMA(o) << 16) | ((u32)DMA(o + 2) << 8) | DMA(o + 4);
}

static void dma_set(int o, u32 a)
{
    DMA(o) = (u8)(a >> 16);
    DMA(o + 2) = (u8)(a >> 8);
    DMA(o + 4) = (u8)a;
}

void music_start(int id)
{
    g_tc_tos = TIMER_C_VEC;
    g_paused = 0;
    SNDH_PlayTune(&g_tune, 1);
    g_playing = id;
    trace("music: play M%02d.SND at %uHz", id, g_tune.freq);
}

void music_pause(void)
{
    u16 sr;
    u32 a;

    if (g_playing < 0 || g_paused)
        return;
    sr = ints_off();
    g_tc_player = TIMER_C_VEC;
    TIMER_C_VEC = g_tc_tos;
    /* The counter runs on while read a byte at a time: read until two
     * readings agree. */
    do {
        a = dma_get(DMA_COUNT);
    } while (a != dma_get(DMA_COUNT));
    g_dma_ctrl = DMA(DMA_CTRL);
    DMA(DMA_CTRL) = 0;
    g_dma_pos = a & ~1UL;
    g_dma_start = dma_get(DMA_START);
    g_dma_end = dma_get(DMA_END);
    ints_restore(sr);
    g_paused = 1;
    trace("music: paused at %06lx in %06lx-%06lx", (unsigned long)g_dma_pos,
          (unsigned long)g_dma_start, (unsigned long)g_dma_end);
}

void music_resume(void)
{
    u16 sr;

    if (g_playing < 0 || !g_paused)
        return;
    sr = ints_off();
    dma_set(DMA_START, g_dma_pos);
    dma_set(DMA_END, g_dma_end);
    DMA(DMA_CTRL) = g_dma_ctrl;
    dma_set(DMA_START, g_dma_start);
    TIMER_C_VEC = g_tc_player;
    ints_restore(sr);
    g_paused = 0;
    trace("music: resumed");
}

/* Make the channel be `id` (-1: silence) without restarting it if it
 * already is. This is what a rollback and the end of a skip do: the state
 * they arrive at says which track should be playing, and a track that is
 * already playing is left alone. The C89 engine reopened one once on a
 * rollback, and on the target the reopen failed and the music died
 * (minirenpy-video NOTES.md, "if_changed, and why a rollback stopped the
 * music"); here it would also cost a 1.3MB reload. */
void music_sync(int id)
{
    if (id == g_playing)
        return;
    if (id < 0)
        music_stop();
    else
        music_play(id, MWB_MUS_IFCHANGED);
}

void music_shutdown(void)
{
    music_stop();
#ifdef MW_RHYTHMTEST
    if (g_own)
        Mfree(g_own);
    g_own = NULL;
#endif
}

/* ---- hit sounds ----------------------------------------------------------
 *
 * The rhythm game's (rhythm.c): Ren'Py plays a plucked string sample a
 * hit, its note the track's (mwc.c, hit_notes). The DMA is the tune's, but
 * the YM2149 is free beside it -- the player only silences it as it starts
 * -- and the STE mixes the two. A hit is a square wave two octaves above
 * the sample's fundamental, so the bright register of the sample's own
 * ring, falling from HIT_LEVEL to silence in HIT_VBLS VBLs (~0.28s at
 * 71Hz, about the samples' length), one volume step being ~3dB: a pluck.
 * At 13 the hits more than doubled the RMS of a stretch of M00 in Hatari
 * (509 to 1199); at 11 they were about the tune's level (744), and a bit
 * loud by ear: 10. Keep HIT_LEVEL * HIT_Q / HIT_VBLS at 2, the
 * fall's slope.
 * The three channels are taken in turn, so a hit rings on under the next
 * two, as Ren'Py spreads its hits over channels (.rpy:47-53).
 *
 * The YM's register is selected, then written, at $FF8800/$FF8802: the
 * pair is never split, interrupts off for it, in case something else's
 * interrupt (TOS's sound, at Timer C) selects another between. Register 7
 * keeps its two top bits, the I/O ports' directions (port A is the
 * floppy's select); 14 and 15 are never written. */
#define YM_SEL      (*(volatile u8 *)0xFFFF8800L)
#define YM_DATA     (*(volatile u8 *)0xFFFF8802L)
#define HIT_LEVEL   10
#define HIT_VBLS    20
#define HIT_Q       4                   /* level in quarter steps */

static volatile s16 g_hit_q[3];         /* level * HIT_Q, a channel */
static int g_hit_next;
static int g_hit_on;
static u8  g_ym_mixer;                  /* register 7 before */

static void ym_set(int r, int v)
{
    u16 sr = ints_off();

    YM_SEL = (u8)r;
    YM_DATA = (u8)v;
    ints_restore(sr);
}

static int ym_get(int r)
{
    u16 sr = ints_off();
    int v;

    YM_SEL = (u8)r;
    v = YM_SEL;
    ints_restore(sr);
    return v;
}

/* At each VBL, in the interrupt: each channel's level down a step. */
static void hit_vbl(void)
{
    int c;

    for (c = 0; c < 3; c++) {
        s16 q = g_hit_q[c];
        if (q <= 0)
            continue;
        q -= HIT_LEVEL * HIT_Q / HIT_VBLS;
        if (q < 0)
            q = 0;
        g_hit_q[c] = q;
        ym_set(8 + c, (q + HIT_Q - 1) / HIT_Q);
    }
}

void hit_begin(void)
{
    int c;

    g_ym_mixer = (u8)ym_get(7);
    for (c = 0; c < 3; c++) {
        g_hit_q[c] = 0;
        ym_set(8 + c, 0);
    }
    g_hit_next = 0;
    g_hit_on = gfx_vbl_hook(hit_vbl);
    if (!g_hit_on)
        trace("music: no VBL, no hit sounds");
}

/* The tone period of MIDI note m, 2MHz / 16 / f: octave 1's (C1 32.7Hz)
 * x16, shifted down an octave at a time and rounded. */
void hit_play(int m)
{
    static const u16 oct1[12] = {
        61156, 57724, 54484, 51426, 48540, 45815,
        43244, 40817, 38526, 36364, 34323, 32396
    };
    int c = g_hit_next, o;
    u32 p;
    u16 sr;

    if (!g_hit_on)
        return;
    m += 24;                            /* two octaves up */
    if (m < 24)                         /* the 12-bit period's floor */
        m = 24 + m % 12;
    o = (m - 24) / 12;
    p = ((u32)oct1[(m - 24) % 12] + (8UL << o)) >> (4 + o);
    if (!p)
        p = 1;
    g_hit_next = (c + 1) % 3;
    /* The VBL is held off while the channel is set up. */
    sr = ints_off();
    g_hit_q[c] = 0;
    YM_SEL = (u8)(2 * c);
    YM_DATA = (u8)p;
    YM_SEL = (u8)(2 * c + 1);
    YM_DATA = (u8)(p >> 8);
    YM_SEL = 7;                         /* tone on, noise off, here */
    YM_DATA = (u8)((YM_SEL & ~(9 << c)) | (8 << c));
    YM_SEL = (u8)(8 + c);
    YM_DATA = HIT_LEVEL;
    g_hit_q[c] = HIT_LEVEL * HIT_Q;
    ints_restore(sr);
}

void hit_end(void)
{
    int c;

    if (g_hit_on)
        gfx_vbl_hook(NULL);
    g_hit_on = 0;
    for (c = 0; c < 3; c++) {
        g_hit_q[c] = 0;
        ym_set(8 + c, 0);
    }
    ym_set(7, g_ym_mixer);
}
