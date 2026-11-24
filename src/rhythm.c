/* rhythm.c - the rhythm minigame, `call screen rhythm_game(...)`
 * (script_game.rpy), as game/00-renpythm/rhythm_game_displayable.rpy -- "the
 * .rpy" below -- plays it, at half scale and in black and white.
 *
 * The clock is the .rpy's: a 3s countdown with nothing but the digit, then
 * one note lifetime (2s) of notes falling in silence, then the tune, its
 * first onset reaching the hit-zone bar as it starts (.rpy:362-366). The
 * run ends when the tune does, at the end the compiler read from the SNDH
 * (mwb.h, "beatmaps"); the SNDH player is not asked, it plays on past it.
 *
 * A frame puts back, from a layer of backdrop and tracks, only what was
 * drawn over it two frames before, then draws the bar, the notes and the
 * judgement by the blitter -- every blit costing ~0.2ms before a pixel
 * moves, so there are few. With the music taking 31% of the CPU, play
 * runs at 32-35fps (Hatari: every note hit, none). The texts on the stage
 * are sprites made once, white with a black outline as the .rpy's `score`
 * style, thicker for the dither.
 *
 * What differs: the health bar, score and combo are in the strip under
 * the stage (draw_hud); no hit sounds (the tunes have the DMA, and there
 * is no mixer); the judgement does not fade in and out; colours are white
 * and black; Esc's dialog is the game's own Oui/Non box. The quit button
 * is for touch screens (.rpy:142) and has no use here.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <mint/osbind.h>

#include "st.h"

/* ---- layout: the .rpy's numbers, halved ----------------------------- */

#define RG_TRACKS     4
#define RG_X0         50        /* x_offset 100 (.rpy:300) */
#define RG_SPACING    180       /* (1280 - 2 * 100) / 3 (.rpy:500) */
#define RG_LINE_W     2         /* track_width 4 */
#define RG_BAR_Y      341       /* track_bar_height 720 - 28 - 10 */
#define RG_BAR_H      14        /* horizontal_bar_height 28 */
#define RG_BAR_X      (RG_X0 - RG_LINE_W / 2)
#define RG_BAR_W      542       /* (1080 + 4) / 2 (.rpy:461) */
#define RG_MID_Y      348       /* bar_mid_y 696 (.rpy:507) */
#define RG_TRAVEL     375       /* note_travel 750 */
#define RG_NOTE_HALF  27        /* note_width / 2: both sizes are placed
                                 * with it vertically (.rpy:652) */
#define RG_BIG_HALF   34        /* 1.25x, 68 wide here */

/* ---- timing, ms --------------------------------------------------------- */

#define RG_COUNTDOWN  3000      /* countdown 3.0 (.rpy:359) */
#define RG_LEAD       2000      /* RHYTHM_NOTE_OFFSET */
#define RG_SILENCE    (RG_COUNTDOWN + RG_LEAD)
#define RG_APPEAR     2010      /* note_offset + time_difference_threshold */
#define RG_HIT        300       /* hit_threshold (.rpy:408) */
#define RG_PERFECT    200       /* RHYTHM_PERFECT_WINDOW */
#define RG_JUDGE      400       /* judgement_duration */
#define RG_GRACE      800       /* the summary's timer (.rpy:166) */

#define RG_MAX_HEALTH 100       /* RHYTHM_MAX_HEALTH */
#define RG_MISS       5         /* RHYTHM_MISS_DAMAGE */

/* ---- strings ---------------------------------------------------------- */

/* The .rpy's _() strings and tl/english/rhythm_game_displayable.rpy's
 * translations of them. "GAME OVER", PERFECT and GREAT are English in both
 * (.rpy:445). */
enum { T_QUIT, T_RECORD, T_COMBO, T_CONTINUE, T_N };
static const char *const g_txt[MWB_NLANG][T_N] = {
    {
        "Quitter le jeu de rythme ?", "Nouveau record !", "Full combo !",
        "Appuyez sur Espace ou cliquez pour continuer."
    }, {
        "Quit the rhythm game?", "New record!", "Full combo!",
        "Press Space or click to continue."
    }
};

/* ---- state ------------------------------------------------------------ */

static const rhythm_in *g_in;
static u32  g_nnotes, g_end_ms, g_key, g_last_ms;
static const u8 *g_bt;              /* the notes, 4 bytes each */
static const u8 *g_hit;             /* a track's hit sound, a MIDI note */
static signed char *g_status;       /* 0 pending, 1 hit, -1 missed */
static u32  g_first;                /* notes before it are all settled */

static long g_score, g_combo;
static int  g_hits, g_perfect, g_great, g_health, g_full;
static int  g_ended, g_game_over, g_record, g_full_combo, g_done;
static int  g_music_on;
static long g_end_time;             /* song time the run ended at */
static u32  g_ended_at;             /* now_ms() then */
static int  g_judge;                /* 0 none, 1 perfect, 2 great */
static long g_judge_at;

/* Screen time: ms since the first frame, the pauses taken out. */
static u32  g_base, g_paused_for;

static u32  note_ms(u32 i)
{
    const u8 *p = g_bt + 4 * i;
    return (((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) |
            p[3]) & MWB_BTM_MS_MASK;
}

static int note_track(u32 i)
{
    return g_bt[4 * i] >> 6;
}

static long screen_time(void)
{
    return (long)(now_ms() - g_base - g_paused_for);
}

/* ---- text as sprites --------------------------------------------------- */

/* Bits [x, x + n) of a row of words set. */
static void set_run(u16 *row, int x, int n)
{
    for (; n > 0; n--, x++)
        row[x >> 4] |= (u16)(0x8000 >> (x & 15));
}

/* A plane of wpr x h words grown by a pixel each way, into `out`. */
static void grow(const u16 *in, u16 *out, int wpr, int h)
{
    int y, k;

    for (y = 0; y < h; y++)
        for (k = 0; k < wpr; k++) {
            const u16 *c = in + (u32)y * wpr + k;
            u16 v = *c | (u16)(*c >> 1) | (u16)(*c << 1);
            if (k > 0)
                v |= (u16)(c[-1] << 15);
            if (k + 1 < wpr)
                v |= (u16)(c[1] >> 15);
            if (y > 0)
                v |= c[-wpr];
            if (y + 1 < h)
                v |= c[wpr];
            out[(u32)y * wpr + k] = v;
        }
}

/* The 8x8 font, read once: gfx_glyph is ~110us a row (measured), and the
 * score -- made again at every hit -- was most of a frame of it. */
static u8  g_font[256][8];
static int g_font_ok;

static void font_read(void)
{
    int c, r;

    if (g_font_ok)
        return;
    for (c = 0; c < 256; c++)
        for (r = 0; r < 8; r++)
            g_font[c][r] = gfx_glyph(c, r);
    g_font_ok = 1;
}

/* `s`'s glyphs, scaled sx by sy, ORed into a zeroed plane at (o, o). At
 * sx 1 a glyph row is a byte shifted into place, which the score needs:
 * bit by bit it took 75-120ms. */
static void glyphs(u16 *plane, int wpr, const char *s, int sx, int sy, int o)
{
    int len = (int)strlen(s), i, r, b, y;
    u16 *top = plane;

    for (y = 0; y < o; y++)
        top += wpr;
    if (sx == 1) {
        /* No multiplying in here: the 68000's 32-bit one is a library
         * call, and it was most of the time left. */
        for (i = 0; i < len; i++) {
            const u8 *f = g_font[(u8)s[i]];
            int x = o + (i << 3), sh = 24 - (x & 15);
            int two = (x >> 4) + 1 < wpr;
            u16 *p = top + (x >> 4);
            for (r = 0; r < 8; r++) {
                u32 v = (u32)f[r] << sh;
                for (y = 0; y < sy; y++, p += wpr) {
                    p[0] |= (u16)(v >> 16);
                    if (two)
                        p[1] |= (u16)v;
                }
            }
        }
        return;
    }
    for (i = 0; i < len; i++)
        for (r = 0; r < 8; r++) {
            u8 bits = g_font[(u8)s[i]][r];
            if (!bits)
                continue;
            for (b = 0; b < 8; b++)
                if (bits & (0x80 >> b))
                    for (y = o + r * sy; y < o + (r + 1) * sy; y++)
                        set_run(plane + (u32)y * wpr, o + (i * 8 + b) * sx,
                                sx);
        }
}

/* `s` (Atari charset) in the 8x8 font scaled sx by sy as an MSP sprite,
 * white letters, malloc'd (or NULL). Black round them: an outline `o`
 * pixels thick, or with `box` a whole box, `o` its margin. The .rpy's
 * outline is a pixel at 1280x720 (its `score` style), but on a dithered
 * backdrop that thin a line is lost in the dots, and at 8x16 even a thick
 * one is. */
static u8 *text_sprite(const char *s, int sx, int sy, int o, int box)
{
    int len = (int)strlen(s), w = len * 8 * sx + 2 * o, h = 8 * sy + 2 * o;
    int wpr = (w + 15) / 16, i, y;
    u32 plane = (u32)wpr * h, k;
    u16 *hp, *m, *d;
    u8 *spr = malloc(8 + 4 * plane);

    if (!spr)
        return NULL;
    hp = (u16 *)spr;
    hp[0] = (u16)w;
    hp[1] = (u16)h;
    hp[2] = (u16)wpr;
    hp[3] = 0;
    m = hp + 4;
    d = m + plane;
    /* The glyphs go in the data plane; the mask is the box, or them grown
     * `o` times; the data is then the mask less the glyphs. */
    memset(d, 0, plane * 2);
    glyphs(d, wpr, s, sx, sy, o);
    if (box) {
        u16 end = (u16)(0xFFFF << ((16 - (w & 15)) & 15)), *row = m;
        for (y = 0; y < h; y++, row += wpr) {
            for (i = 0; i < wpr - 1; i++)
                row[i] = 0xFFFF;
            row[wpr - 1] = end;
        }
    } else {
        u16 *t = malloc(plane * 2);
        if (!t) {
            free(spr);
            return NULL;
        }
        memcpy(m, d, plane * 2);
        for (i = 0; i < o; i++) {
            grow(m, t, wpr, h);
            memcpy(m, t, plane * 2);
        }
        free(t);
    }
    for (k = 0; k < plane; k++)
        d[k] = (u16)(m[k] & ~d[k]);
    return spr;
}

static int spr_w(const u8 *s) { return ((const u16 *)s)[0]; }
static int spr_h(const u8 *s) { return ((const u16 *)s)[1]; }

/* ---- what each screen has had drawn over the layer ----
 *
 * The layer is the backdrop with the tracks, made once. A screen is put
 * back to it where it was drawn on last time -- two frames ago, the
 * screens alternating -- rather than copied whole: the notes, judgement
 * and score cover a fifth of the stage, and the whole copy was most of a
 * frame. What is opaque and drawn in the same place every frame (the bar,
 * the health) needs no putting back. */
#define NRECT 48
typedef struct { s16 x0, y0, x1, y1; } rect;
static rect g_rect[2][NRECT];
static int  g_nrect[2], g_whole[2];
static int  g_b;                    /* the screen being drawn */
static u8  *g_layer;

static void mark(int x, int y, int w, int h)
{
    rect *r;

    if (g_nrect[g_b] == NRECT) {
        g_whole[g_b] = 1;
        return;
    }
    r = &g_rect[g_b][g_nrect[g_b]++];
    r->x0 = (s16)x;
    r->y0 = (s16)y;
    r->x1 = (s16)(x + w);
    r->y1 = (s16)(y + h);
}

static void put(u8 *scr, const u8 *s, int x, int y)
{
    if (s) {
        gfx_sprite(scr, s, x, y);
        mark(x, y, spr_w(s), spr_h(s));
    }
}

/* Centred across the stage, top at y. */
static void put_mid(u8 *scr, const u8 *s, int y)
{
    if (s)
        put(scr, s, (MWB_STAGE_W - spr_w(s)) / 2, y);
}

/* Ren'Py's align: the stage's free height times `num`/100. */
static int align_y(const u8 *s, int num)
{
    return s ? (int)((long)(MWB_STAGE_H - spr_h(s)) * num / 100) : 0;
}

/* The texts on the stage, made once (the tally at the end). */
enum {
    S_3, S_2, S_1, S_PERFECT, S_GREAT, S_GAMEOVER, S_COMBO, S_RECORD,
    S_CONTINUE, S_TALLY, S_N
};
static u8 *g_spr[S_N];

/* Two pixels of outline, three round the countdown's big digits; the
 * 8x16 line on a box. */
static void remake(int i, const char *s, int sx, int sy)
{
    free(g_spr[i]);
    g_spr[i] = text_sprite(s, sx, sy, sx >= 6 ? 3 : 2, sx == 1);
}

/* ---- the rules (.rpy:779-850) ------------------------------------------ */

static void game_over(void)
{
    g_game_over = 1;
    g_ended = 1;
}

/* A note that left the hit window unhit is missed: the combo goes, and
 * some health (.rpy:798-815). */
static void detect_misses(long t)
{
    u32 i;
    int missed = 0;

    while (g_first < g_nnotes && g_status[g_first])
        g_first++;
    for (i = g_first; i < g_nnotes; i++) {
        long on = (long)note_ms(i);
        if (!g_status[i] && t > on + RG_HIT) {
            g_status[i] = -1;
            g_combo = 0;
            g_full = 0;
            g_health -= RG_MISS;
            if (g_health < 0)
                g_health = 0;
            missed = 1;
        } else if (on > t) {
            break;
        }
    }
    if (missed && g_health <= 0 && !g_game_over)
        game_over();
}

/* Python's round() of points * 1.5: half to even (.rpy:788). */
static long perfect_points(long p)
{
    long p3 = p * 3, q = p3 / 2;

    if (!(p3 & 1))
        return q;
    return (q & 1) ? q + 1 : q;
}

/* The first pending note of the track inside the hit window (.rpy:775). */
static void try_hit(int track, long t)
{
    u32 i;

    for (i = g_first; i < g_nnotes; i++) {
        long on = (long)note_ms(i), before = on - t, d;
        if (before > RG_APPEAR)
            break;
        if (note_track(i) != track || g_status[i] || before < -RG_HIT)
            continue;
        d = before < 0 ? -before : before;
        if (d <= RG_HIT) {
            long points = 10 + g_combo;
            g_status[i] = 1;
            g_hits++;
            if (d <= RG_PERFECT) {
                points = perfect_points(points);
                g_perfect++;
                g_judge = 1;
            } else {
                g_great++;
                g_judge = 2;
            }
            g_judge_at = t;
            g_score += points;
            g_combo++;
            hit_play(g_hit[track]);     /* play_hit_sound (.rpy:500) */
            return;
        }
    }
}

/* The tune came to its end: the run is recorded (.rpy:621-630). */
static void song_over(long t)
{
    g_ended = 1;
    g_end_time = t;
    music_stop();
    g_music_on = 0;
    g_record = persist_record(g_key, g_score);
    g_full_combo = g_full && g_hits > 0;
}

/* ---- input ------------------------------------------------------------ */

/* Keys and clicks, each with the time it was read: read while waiting
 * for the VBL too (gfx_flip_poll), so a press is placed to the 5ms tick. */
#define EV_MAX   32
#define EV_LEFT  0x40000000L        /* | x */
#define EV_RIGHT 0x20000000L
typedef struct { long k; u32 t; } event;
static event g_ev[EV_MAX];
static int   g_nev, g_buttons;

static void poll(void)
{
    int b, pressed;

    while (g_nev < EV_MAX && Bconstat(2)) {
        g_ev[g_nev].k = Bconin(2);
        g_ev[g_nev++].t = now_ms();
    }
    b = gfx_mouse_buttons();
    pressed = b & ~g_buttons;
    g_buttons = b;
    if (pressed && g_nev < EV_MAX) {
        int x, y;
        gfx_mouse_pos(&x, &y);
        g_ev[g_nev].k = (pressed & 2) ? EV_RIGHT : EV_LEFT | x;
        g_ev[g_nev++].t = now_ms();
    }
}

/* The .rpy's keys (.rpy:425-432) as the ST's: the arrows; A or Q, and S,
 * by the character, as SDL's keycodes follow the layout; keypad 1 and 2 by
 * the scancode. */
static int track_of_key(long k)
{
    int c = (int)(k & 0xFF), sc = (int)((k >> 16) & 0xFF);

    if (sc == 0x4B || c == 'a' || c == 'A' || c == 'q' || c == 'Q')
        return 0;
    if (sc == 0x48 || c == 's' || c == 'S')
        return 1;
    if (sc == 0x50 || sc == 0x6D)
        return 2;
    if (sc == 0x4D || sc == 0x6E)
        return 3;
    return -1;
}

/* The nearest track, inside its half-lane (.rpy:768). */
static int track_at(int x)
{
    int k = (x - RG_X0 + RG_SPACING / 2) / RG_SPACING;
    int d = x - (RG_X0 + k * RG_SPACING);

    if (x < RG_X0 - RG_SPACING / 2 || k >= RG_TRACKS)
        return -1;
    return (d < 0 ? -d : d) > RG_SPACING / 2 ? -1 : k;
}

static void hud_invalidate(void);

/* Esc's dialog. The game's clock and the tune wait while it is up; Oui
 * ends the run as it stands (.rpy:156-160, :528-536). */
static void pause_game(void)
{
    u32 t0 = now_ms();
    int quit;

    music_pause();
    quit = menu_confirm(g_txt[g_lang][T_QUIT]);
    if (quit) {
        music_stop();
        g_music_on = 0;
        g_done = 1;
        return;
    }
    music_resume();
    g_paused_for += now_ms() - t0;
    trace("rhythm: paused for %lums", (unsigned long)(now_ms() - t0));
    /* The dialog copied the shown screen into the other. */
    g_whole[0] = g_whole[1] = 1;
    hud_invalidate();
    g_nev = 0;
    while (Bconstat(2))
        Bconin(2);
    g_buttons = gfx_mouse_buttons();
}

static void handle(const event *e)
{
    long st = (long)(e->t - g_base - g_paused_for);
    int mouse = (e->k & (EV_LEFT | EV_RIGHT)) != 0;
    int c = mouse ? 0 : (int)(e->k & 0xFF), k;

    if (g_ended) {
        /* The summary, once its grace is over (.rpy:163-176). */
        if (now_ms() - g_ended_at >= RG_GRACE &&
            (c == ' ' || c == '\r' || (e->k & EV_LEFT)))
            g_done = 1;
        return;
    }
    if (c == 0x1B || e->k == EV_RIGHT) {
        pause_game();
        return;
    }
    if (st < RG_COUNTDOWN)
        return;                     /* no hits during the countdown */
    if (e->k & EV_LEFT)
        k = track_at((int)(e->k & 0xFFFF));
    else
        k = track_of_key(e->k);
    if (k >= 0)
        try_hit(k, st - RG_SILENCE);
}

/* ---- drawing ------------------------------------------------------------ */

static int track_x(int k)
{
    return RG_X0 + k * RG_SPACING;
}

/* ---- the HUD, in the strip under the stage ----
 *
 * The .rpy puts the health bar and the score at the stage's top left
 * (.rpy:206-232). Here they go in the 40 lines under it, which the game
 * has for its text and the minigame does not otherwise use: black on
 * white in the system font, drawn by the CPU when a value changes, never
 * under a note. On the stage they were white on a dithered backdrop -- a
 * box or an outline each -- and two dozen blits a frame, at ~0.2ms a blit
 * whatever its size (measured): a fifth of the frame. Each screen keeps
 * what it shows, as both are drawn in turn. */
#define HUD_Y   (MWB_STAGE_H + 15)
typedef struct { int health; long score, combo; } hud;
static hud g_hud[2];

static void hud_invalidate(void)
{
    g_hud[0].health = g_hud[1].health = -1;
}

static void draw_hud(u8 *scr)
{
    hud *h = &g_hud[g_b];
    char s[48];
    int w, y, every;

    if (h->health == g_health && h->score == g_score && h->combo == g_combo)
        return;
    h->health = g_health;
    h->score = g_score;
    h->combo = g_combo;
    gfx_text_clear(scr);
    /* The bar: its colour -- green, amber, red (.rpy:819-826) -- is a fill
     * growing sparser. */
    w = 112 * g_health / RG_MAX_HEALTH;
    gfx_rect(scr, 8, HUD_Y - 2, 122, HUD_Y + 11, BLT_OP_ONE);
    every = g_health * 2 > RG_MAX_HEALTH ? 1 :
            g_health * 4 > RG_MAX_HEALTH ? 2 : 3;
    for (y = HUD_Y - 1; w > 0 && y < HUD_Y + 10; y++)
        if ((y - (HUD_Y - 1)) % every == 0)
            gfx_rect(scr, 9, y, 9 + w, y + 1, BLT_OP_ZERO);
    sprintf(s, "Score: %ld", g_score);
    gfx_text(scr, 17, HUD_Y, (const u8 *)s, (int)strlen(s), GFX_BOLD);
    if (g_combo > 1) {
        sprintf(s, "Combo x%ld (+%ld%%)", g_combo, g_combo * 10);
        gfx_text(scr, 40, HUD_Y, (const u8 *)s, (int)strlen(s), GFX_BOLD);
    }
}

static void draw_notes(u8 *scr, long t)
{
    u32 i;

    for (i = g_first; i < g_nnotes; i++) {
        long before = (long)note_ms(i) - t, yc;
        int k;
        if (before > RG_APPEAR)
            break;
        if (g_status[i] || before < -RG_HIT)
            continue;
        k = note_track(i);
        yc = RG_MID_Y - before * RG_TRAVEL / RG_LEAD;
        if (before <= RG_HIT && before >= -RG_HIT)
            put(scr, g_in->note[4 + k], track_x(k) - RG_BIG_HALF,
                (int)yc - RG_NOTE_HALF);
        else
            put(scr, g_in->note[k], track_x(k) - RG_NOTE_HALF,
                (int)yc - RG_NOTE_HALF);
    }
}

/* The tracks, white lines edged in black (.rpy:575-578). */
static void draw_tracks(u8 *scr)
{
    int k;

    for (k = 0; k < RG_TRACKS; k++) {
        int x = track_x(k) - RG_LINE_W / 2;
        gfx_rect(scr, x - 1, 0, x + RG_LINE_W + 1, MWB_STAGE_H, BLT_OP_ONE);
        gfx_rect(scr, x, 0, x + RG_LINE_W, MWB_STAGE_H, BLT_OP_ZERO);
    }
}

/* The frame at screen time st (.rpy:543-680, and the screen's texts). */
static void draw(u8 *scr, long st)
{
    long t = st - RG_SILENCE, cur, fill;
    int i;

    if (!g_ended && st < RG_COUNTDOWN) {
        /* Only the digit (.rpy:557-569). */
        const u8 *d = g_spr[st < RG_COUNTDOWN - 2000 ? S_3 :
                            st < RG_COUNTDOWN - 1000 ? S_2 : S_1];
        gfx_copy_stage(scr, g_in->bg);
        if (d)
            gfx_sprite(scr, d, (MWB_STAGE_W - spr_w(d)) / 2,
                       (MWB_STAGE_H - spr_h(d)) / 2);
        g_whole[0] = g_whole[1] = 1;
        return;
    }
    g_b = gfx_back_index();
    if (g_whole[g_b] || g_ended) {
        gfx_copy_stage(scr, g_layer);
        g_whole[g_b] = 0;
    } else {
        for (i = 0; i < g_nrect[g_b]; i++) {
            const rect *r = &g_rect[g_b][i];
            gfx_copy_rect(scr, g_layer, r->x0, r->y0, r->x1, r->y1);
        }
    }
    g_nrect[g_b] = 0;
    /* The hit-zone bar, filling up as the song goes (.rpy:586-596). */
    cur = g_ended ? g_end_time : t;
    fill = cur <= 0 ? 0 : cur >= (long)g_last_ms ? RG_BAR_W - 2 :
           cur * (RG_BAR_W - 2) / (long)g_last_ms;
    gfx_box(scr, RG_BAR_X, RG_BAR_Y, RG_BAR_X + RG_BAR_W, RG_BAR_Y + RG_BAR_H);
    if (fill > 0)
        gfx_rect(scr, RG_BAR_X + 1, RG_BAR_Y + 1, RG_BAR_X + 1 + (int)fill,
                 RG_BAR_Y + RG_BAR_H - 1, BLT_OP_ONE);

    if (!g_ended) {
        draw_notes(scr, t);
        if (g_judge && t - g_judge_at <= RG_JUDGE) {
            const u8 *j = g_spr[g_judge == 1 ? S_PERFECT : S_GREAT];
            if (j)
                put_mid(scr, j, RG_BAR_Y - spr_h(j) - 10);
        } else {
            g_judge = 0;
        }
    }
    draw_hud(scr);

    if (g_ended) {
        if (now_ms() - g_ended_at >= RG_GRACE)
            put_mid(scr, g_spr[S_CONTINUE], align_y(g_spr[S_CONTINUE], 68));
        if (g_game_over)
            put_mid(scr, g_spr[S_GAMEOVER], align_y(g_spr[S_GAMEOVER], 35));
        if (g_full_combo)
            put_mid(scr, g_spr[S_COMBO], align_y(g_spr[S_COMBO], 33));
        if (g_record)
            put_mid(scr, g_spr[S_RECORD], align_y(g_spr[S_RECORD], 45));
        put_mid(scr, g_spr[S_TALLY], align_y(g_spr[S_TALLY], 57));
    }
}

/* ---- the run ------------------------------------------------------------ */

int rhythm_begin(const rhythm_in *in)
{
    const u8 *b = in->btm;

    g_in = in;
    g_key = ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
    g_end_ms = ((u32)b[4] << 24) | ((u32)b[5] << 16) | ((u32)b[6] << 8) |
               b[7];
    g_nnotes = ((u32)b[8] << 8) | b[9];
    g_hit = b + MWB_BTM_HIT;
    g_bt = b + MWB_BTM_HEADER_SIZE;
    if (!g_nnotes)
        return 0;
    /* song_duration, for the bar: the last onset (.rpy:376). */
    g_last_ms = note_ms(g_nnotes - 1);
    if (!g_last_ms)
        g_last_ms = 1;
    g_status = calloc(g_nnotes, 1);
    if (!g_status)
        return 0;

    g_first = 0;
    g_score = g_combo = 0;
    g_hits = g_perfect = g_great = 0;
    g_health = RG_MAX_HEALTH;
    g_full = 1;
    g_ended = g_game_over = g_record = g_full_combo = g_done = 0;
    g_music_on = 0;
    g_judge = 0;
    g_paused_for = 0;
    g_nev = 0;
    memset(g_spr, 0, sizeof(g_spr));

    /* Sizes from the .rpy's text sizes halved: 120 -> 56, 72 -> 32,
     * 48 and 44 -> 24, 32 -> 16, 28 -> 8x16. */
    font_read();
    remake(S_3, "3", 7, 7);
    remake(S_2, "2", 7, 7);
    remake(S_1, "1", 7, 7);
    remake(S_PERFECT, "PERFECT", 3, 3);
    remake(S_GREAT, "GREAT", 3, 3);
    remake(S_GAMEOVER, "GAME OVER", 4, 4);
    remake(S_COMBO, g_txt[g_lang][T_COMBO], 3, 3);
    remake(S_RECORD, g_txt[g_lang][T_RECORD], 3, 3);
    remake(S_CONTINUE, g_txt[g_lang][T_CONTINUE], 1, 2);
    trace("rhythm: %lu notes, last %lums, tune ends %lums",
          (unsigned long)g_nnotes, (unsigned long)g_last_ms,
          (unsigned long)g_end_ms);
    return 1;
}

/* The first frame, the countdown's 3, for the transition into it. */
void rhythm_first(u8 *stage)
{
    draw(stage, 0);
}

void rhythm_run(s32 ret[3])
{
    long kb = Kbrate(-1, -1);
    u32 frames = 0, t0, pf = 0, pt0 = 0;
    int i;

    (void)t0;                       /* only the traces read them */
    (void)pt0;

    /* No key repeat while it runs: a held key would hit again. */
    (void)Kbrate(255, 255);
    while (Bconstat(2))
        Bconin(2);
    g_buttons = gfx_mouse_buttons();
    gfx_text_clear(gfx_back());
    gfx_text_clear(gfx_front());
    /* The layer: made here, its buffer being the transition's until now. */
    g_layer = g_in->work;
    gfx_copy_stage(g_layer, g_in->bg);
    draw_tracks(g_layer);
    g_whole[0] = g_whole[1] = 1;
    g_nrect[0] = g_nrect[1] = 0;
    hud_invalidate();
    gfx_pointer(1);
    hit_begin();
    g_base = now_ms();
    t0 = g_base;

    while (!g_done) {
        long st, t;

        poll();
        for (i = 0; i < g_nev && !g_done; i++)
            handle(&g_ev[i]);
        g_nev = 0;
        if (g_done)
            break;

        st = screen_time();
        t = st - RG_SILENCE;
        if (!g_ended && !g_music_on && t >= 0) {
            /* The tune comes out when the player's init returns (~95ms):
             * its start is the song's 0 (NOTES.md, "The rhythm game"). */
            music_start(g_in->mus);
            g_music_on = 1;
            g_base = now_ms() - g_paused_for - RG_SILENCE;
            st = RG_SILENCE;
            t = 0;
        }
#ifdef MW_RHYTHMBOT
        /* The test build's player: every note hit on its onset, so a run
         * reaches the song's end. */
        if (!g_ended && t >= 0) {
            u32 n;
            for (n = g_first; n < g_nnotes && (long)note_ms(n) <= t; n++)
                if (!g_status[n])
                    try_hit(note_track(n), (long)note_ms(n));
        }
#endif
        if (!g_ended && t >= 0) {
            detect_misses(t);
            if (g_ended) {
                /* Game over: no record, the tune stopped (.rpy:817). */
                g_end_time = t;
                music_stop();
                g_music_on = 0;
            } else if ((u32)t >= g_end_ms) {
                song_over(t);
            }
            if (g_ended) {
                char s[48];
                g_ended_at = now_ms();
                sprintf(s, "PERFECT %d    GREAT %d", g_perfect, g_great);
                remake(S_TALLY, s, 2, 2);
                trace("rhythm: over at %ldms: %d hits of %lu, score %ld, "
                      "%s; %lu frames in play in %lums", t, g_hits,
                      (unsigned long)g_nnotes, g_score,
                      g_game_over ? "game over" : g_record ? "a record" :
                      "no record", (unsigned long)pf,
                      (unsigned long)(now_ms() - pt0));
            }
        }
        draw(gfx_back(), st);
        gfx_flip_poll(poll);
        frames++;
        if (!g_ended && st >= RG_COUNTDOWN && !pf++)
            pt0 = now_ms();
    }
    trace("rhythm: %lu frames in %lums", (unsigned long)frames,
          (unsigned long)(now_ms() - t0));

    if (g_music_on)
        music_stop();
    g_music_on = 0;
    (void)Kbrate((int)(kb >> 8) & 0xFF, (int)kb & 0xFF);
    hit_end();
    gfx_pointer(0);

    /* (hits, notes, score); a game over is (0, notes, 0) (.rpy:830). */
    ret[0] = g_game_over ? 0 : g_hits;
    ret[1] = (s32)g_nnotes;
    ret[2] = g_game_over ? 0 : g_score;
}

void rhythm_end(void)
{
    int i;

    for (i = 0; i < S_N; i++) {
        free(g_spr[i]);
        g_spr[i] = NULL;
    }
    free(g_status);
    g_status = NULL;
}
