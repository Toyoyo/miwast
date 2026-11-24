/* rtest.c - would the rhythm game run here? Built with RHYTHMTEST=<track>
 * (a number, the M<nn>.SND to play), and run instead of the script. It
 * measures three things, all traced (so NATFEATS=1):
 *
 * - the blitter: 50 stage passes, without music and with it;
 * - the CPU the music takes: an idle loop's iterations per second while
 *   the track plays, against its count in silence;
 * - sync: BEAT.TXT from the hard disk (a Ren'Py beatmap, "seconds<TAB>
 *   track" a line) flashed in four bands as the track plays. A YM beep
 *   starts exactly a second before the player does, so a recording
 *   (drive.sh's `rec`) shows how late the tune comes out after its start
 *   is called, and, against the original, whether it drifts.
 *
 * Space starts the beep and the tune (after the silent measurements, so
 * a recording can be started first); Esc ends the song early.
 */

#ifdef MW_RHYTHMTEST

#include <stdlib.h>
#include <mint/osbind.h>

#include "st.h"

#define MAXNOTES   1400
#define FLASH_MS   80
#define BAND_W     128

static u32 g_note[MAXNOTES];        /* ms from the tune's start */
static u8  g_ntrack[MAXNOTES];
static int g_nnotes;

/* "209.4182\t2" lines into milliseconds and a track, 0-3. */
static int load_beatmap(void)
{
    long fh = Fopen("BEAT.TXT", 0);
    long size;
    char *buf, *p, *end;

    if (fh < 0)
        return 0;
    size = Fseek(0, (short)fh, 2);
    Fseek(0, (short)fh, 0);
    buf = malloc(size + 1);
    if (!buf || Fread((short)fh, size, buf) != size) {
        Fclose((short)fh);
        return 0;
    }
    Fclose((short)fh);
    buf[size] = 0;
    p = buf;
    end = buf + size;
    while (p < end && g_nnotes < MAXNOTES) {
        u32 v = 0;                  /* the time in 1/10000 s */
        int digits = 0, track = 0;

        while (*p >= '0' && *p <= '9')
            v = v * 10 + (*p++ - '0');
        v *= 10000;
        if (*p == '.') {
            u32 scale = 1000;
            p++;
            while (*p >= '0' && *p <= '9') {
                v += (*p++ - '0') * scale;
                scale /= 10;
                digits++;
            }
        }
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p >= '0' && *p <= '3') {
            track = *p - '0';
            g_note[g_nnotes] = (v + 5) / 10;
            g_ntrack[g_nnotes] = (u8)track;
            g_nnotes++;
        }
        while (p < end && *p != '\n')
            p++;
        p++;
        (void)digits;
    }
    free(buf);
    return g_nnotes > 0;
}

static void band(int k)
{
    int x0 = k * 160 + (160 - BAND_W) / 2;

    gfx_rect(gfx_front(), x0, 40, x0 + BAND_W, 360, BLT_OP_NOT_D);
}

/* The loop whose iterations per second are the idle CPU: it flashes the
 * notes due (times from t0) and runs `ms`, or with ms 0 to the last note
 * plus a second, or Esc. The silent run is the same loop, flashes and all,
 * so the difference is the music's alone. Traces each second's count, and
 * with `base` (the silent count) the share the music took. Returns the
 * mean count per second. */
static u32 loop(u32 ms, u32 t0, u32 base)
{
    u32 start = now_ms(), sec = start + 1000, n = 0, total = 0;
    u32 stop = ms ? ms : g_note[g_nnotes - 1] + 1000;
    u32 off[4] = { 0, 0, 0, 0 };
    int on[4] = { 0, 0, 0, 0 };
    int i = 0, k, secs = 0, maxload = 0;
    long late, maxlate = 0, sumload = 0;

    for (;;) {
        u32 now = now_ms();
        u32 t = now - t0;

        {
            while (i < g_nnotes && g_note[i] <= t) {
                k = g_ntrack[i];
                /* Not the first second's: the player's init holds
                 * everything up then. */
                late = (long)(t - g_note[i]);
                if (late > maxlate && g_note[i] >= 1000)
                    maxlate = late;
                if (!on[k]) {
                    band(k);
                    on[k] = 1;
                }
                off[k] = g_note[i] + FLASH_MS;
                i++;
            }
            for (k = 0; k < 4; k++)
                if (on[k] && (long)(t - off[k]) >= 0) {
                    band(k);
                    on[k] = 0;
                }
        }
        if ((long)(now - sec) >= 0) {
            secs++;
            total += n;
            if (base) {
                int load = (int)(100 - (n * 100 + base / 2) / base);
                if (load > maxload)
                    maxload = load;
                sumload += load;
                trace("rtest: s %d count %lu load %d%%", secs,
                      (unsigned long)n, load);
            } else {
                trace("rtest: s %d count %lu", secs, (unsigned long)n);
            }
            n = 0;
            sec += 1000;
        }
        if ((long)(t - stop) >= 0)
            break;
        if (!(n & 1023) && Bconstat(2) && (Bconin(2) & 0xFF) == 0x1B)
            break;
        n++;
    }
    for (k = 0; k < 4; k++)
        if (on[k])
            band(k);
    if (base && secs)
        trace("rtest: music load mean %ld%% max %d%% over %ds", sumload / secs,
              maxload, secs);
    trace("rtest: %d of %d notes, flashed %ldms late at most", i,
          g_nnotes, maxlate);
    return secs ? total / secs : 0;
}

static void passes(const char *what)
{
    u32 t = now_ms();
    int i;

    for (i = 0; i < 50; i++)
        gfx_copy_stage(gfx_back(), gfx_front());
    t = now_ms() - t;
    trace("rtest: 50 stage passes %s: %lums, %lums each", what,
          (unsigned long)t, (unsigned long)t / 50);
}

static void ym(int reg, int v)
{
    (void)Giaccess(v, reg | 0x80);
}

/* YM channel A, 1kHz (2MHz / 16 / 125), full volume or off. */
static void beep(int on)
{
    if (on) {
        ym(0, 125);
        ym(1, 0);
        ym(7, (Giaccess(0, 7) & 0xC0) | 0x3E);
        ym(8, 15);
    } else {
        ym(8, 0);
    }
}

void rtest_run(void)
{
    u32 base, t, t0;

    gfx_pointer(0);
    gfx_rect(gfx_front(), 0, 0, 640, 400, BLT_OP_ZERO);
    gfx_rect(gfx_back(), 0, 0, 640, 400, BLT_OP_ZERO);
    if (!load_beatmap())
        fatal("cannot read BEAT.TXT");
    trace("rtest: %d notes, last at %lums", g_nnotes,
          (unsigned long)g_note[g_nnotes - 1]);

    passes("in silence");
    base = loop(3000, now_ms(), 0);
    trace("rtest: silent count %lu a second", (unsigned long)base);

    if (!music_load(MW_RHYTHMTEST))
        fatal("cannot load the track");
    while (Bconstat(2))
        Bconin(2);
    trace("rtest: loaded, Space to start");
    while ((Bconin(2) & 0xFF) != ' ')
        ;

    /* The beep from one tick edge, so it starts within a tick of t. */
    t = now_ms();
    while (now_ms() == t)
        ;
    t = now_ms();
    beep(1);
    while (now_ms() - t < 100)
        ;
    beep(0);
    while (now_ms() - t < 1000)
        ;
    t0 = now_ms();
    music_start(MW_RHYTHMTEST);
    trace("rtest: beep at %lu, start called at %lu, returned after %lums",
          (unsigned long)t, (unsigned long)t0,
          (unsigned long)(now_ms() - t0));
    {
        /* What the player runs on: the MFP's enables and timers. */
        volatile u8 *mfp = (volatile u8 *)0xFFFFFA00L;
        trace("rtest: IERA %02x IERB %02x IMRA %02x IMRB %02x "
              "TACR %02x TADR %02x TBCR %02x TBDR %02x TCDCR %02x "
              "TCDR %02x TDDR %02x", mfp[0x07], mfp[0x09], mfp[0x13],
              mfp[0x15], mfp[0x19], mfp[0x1F], mfp[0x1B], mfp[0x21],
              mfp[0x1D], mfp[0x23], mfp[0x25]);
        trace("rtest: Timer C vector %08lx, VBL %08lx, sound mode %02x",
              *(volatile u32 *)0x114L, *(volatile u32 *)0x70L,
              *(volatile u8 *)0xFFFF8921L);
    }

    loop(0, t0, base);
    passes("with music");
    music_stop();
    trace("rtest: done");
}

#endif
