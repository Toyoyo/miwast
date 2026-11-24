/* gfx.c - screens, the stage, sprites and text, in mono high-res.
 *
 * Two screens: one shown, one drawn into. A frame is always composed whole
 * into the back screen and then flipped at the next VBL, so nothing is ever
 * seen half-drawn.
 *
 * A set bit is a black pixel (the default mono palette, white paper).
 *
 * Sprites and whole-stage operations go through the STE's blitter: a
 * transition redraws the stage a dozen times a second, which the CPU
 * (80ms a sprite) could not. Text and the menus are the CPU's.
 */

#include <string.h>
#include <mint/osbind.h>
#include <mint/sysvars.h>

#include "st.h"

static u8  *g_mem;
static u8  *g_screen[2];
static int  g_back;
static void *g_old_phys, *g_old_log;

/* The system 8x8 font, from Line-A: chars are one byte wide and stored
 * side by side, so row r of char c is dat[r * form_width + c - first]. */
static const u8 *g_font;
static u16       g_font_w, g_font_first, g_font_last;

/* Line-A's variable block. */
static const u8 *g_linea;

/* Whether GEM's VDI is up -- started from the desktop, not from the AUTO
 * folder. Line-A's INTIN pointer is the VDI's: it points at the arrays of
 * the last VDI call, and before GEM nothing has made one, so it is NULL. */
static int g_vdi;

static void *linea_init(void **fonts)
{
    register void *a0 __asm__("a0");
    register void *a1 __asm__("a1");

    __asm__ volatile(".dc.w 0xa000"
                     : "=r"(a0), "=r"(a1)
                     :
                     : "d0", "d1", "d2", "a2", "memory", "cc");
    *fonts = a1;
    return a0;
}

static void linea_hide_mouse(void)
{
    __asm__ volatile(".dc.w 0xa00a"
                     :
                     :
                     : "d0", "d1", "d2", "a0", "a1", "a2", "memory", "cc");
}

/* INTIN[0] = 0 shows the pointer whatever the hide count was; anything
 * else would honour a count this program never kept. INTIN is pointed at
 * our own word for the call, not the last VDI caller's array. Only under
 * GEM: without the VDI there is no pointer to show, and INTIN is NULL. */
static void linea_show_mouse(void)
{
    static s16 intin[1];
    s16 **p = (s16 **)(g_linea + 8);
    s16 *old = *p;

    intin[0] = 0;
    *p = intin;
    __asm__ volatile(".dc.w 0xa009"
                     :
                     :
                     : "d0", "d1", "d2", "a0", "a1", "a2", "memory", "cc");
    *p = old;
}

/* ---- the mouse and its pointer ----
 *
 * Both the program's own, so that they work started from the AUTO folder
 * too, where GEM's VDI, which keeps Line-A's mouse variables and draws
 * TOS's pointer, is not there yet: there, the pointer did not move and
 * showing it crashed (INTIN, above). mouse.s keeps the position and the
 * buttons from the IKBD's packets and passes them on, so under GEM the
 * VDI follows the mouse as before; its own pointer stays hidden.
 *
 * The pointer is drawn as the VDI drew it: by the VBL, into the screen
 * being shown, putting back what was under it before each move. Whoever
 * draws into the shown screen or flips hides it first (menu.c). */
extern volatile s16 ms_x, ms_y, ms_bt;      /* mouse.s */
extern void (*ms_old)(void *);
void ms_handler(void *);

#define SCREEN_H (SCREEN_BYTES / LINE_BYTES)

/* TOS's arrow: the mask cleared out of the screen, the data set in. */
static const u16 g_arrow_mask[16] = {
    0xC000, 0xE000, 0xF000, 0xF800, 0xFC00, 0xFE00, 0xFF00, 0xFF80,
    0xFFC0, 0xFFE0, 0xFE00, 0xEF00, 0xCF00, 0x8780, 0x0780, 0x0380
};
static const u16 g_arrow_data[16] = {
    0x0000, 0x4000, 0x6000, 0x7000, 0x7800, 0x7C00, 0x7E00, 0x7F00,
    0x7F80, 0x7C00, 0x6C00, 0x4600, 0x0600, 0x0300, 0x0300, 0x0000
};

static _KBDVECS *g_kbdvecs;
static void (**g_vbl_slot)(void);   /* ours in the VBL queue, or NULL */
/* The pointer as drawn into one screen. */
typedef struct {
    int drawn, x, y, rows, wide;
    u32 under[16];                  /* what it covers, two words a row */
} ptr_state;

static ptr_state g_ptr[2];          /* by screen */
static volatile int g_ptr_on;
static volatile int g_ptr_scr;      /* the screen it follows the mouse in */

/* Raise the interrupt mask to `level` (never lower it); the old SR back
 * with ipl_restore. The program runs in supervisor mode. */
static u16 ipl_raise(int level)
{
    u16 sr;

    __asm__ volatile("move.w %%sr,%0" : "=d"(sr));
    if ((sr & 0x700) < (level << 8))
        __asm__ volatile("move.w %0,%%sr"
                         :
                         : "d"((u16)((sr & ~0x700) | (level << 8)))
                         : "memory", "cc");
    return sr;
}

static void ipl_restore(u16 sr)
{
    __asm__ volatile("move.w %0,%%sr" : : "d"(sr) : "memory", "cc");
}

/* The hot spot is the arrow's top left corner. Two screen words a row,
 * one at the right edge; rows past the bottom are not drawn. Longs at
 * even addresses are fine on a 68000. */
static void ptr_draw(int i, int x, int y)
{
    ptr_state *ps = &g_ptr[i];
    int s = x & 15, r;
    u8 *p = g_screen[i] + (u32)y * LINE_BYTES + (x >> 4) * 2;

    ps->rows = y + 16 > SCREEN_H ? SCREEN_H - y : 16;
    ps->wide = (x >> 4) < LINE_BYTES / 2 - 1;
    for (r = 0; r < ps->rows; r++, p += LINE_BYTES) {
        u32 m = ((u32)g_arrow_mask[r] << 16) >> s;
        u32 d = ((u32)g_arrow_data[r] << 16) >> s;

        if (ps->wide) {
            u32 under = *(u32 *)p;
            ps->under[r] = under;
            *(u32 *)p = (under & ~m) | d;
        } else {
            u16 under = *(u16 *)p;
            ps->under[r] = under;
            *(u16 *)p = (u16)((under & ~(m >> 16)) | (d >> 16));
        }
    }
    ps->x = x;
    ps->y = y;
    ps->drawn = 1;
}

static void ptr_undraw(int i)
{
    ptr_state *ps = &g_ptr[i];
    u8 *p;
    int r;

    if (!ps->drawn)
        return;
    p = g_screen[i] + (u32)ps->y * LINE_BYTES + (ps->x >> 4) * 2;
    for (r = 0; r < ps->rows; r++, p += LINE_BYTES) {
        if (ps->wide)
            *(u32 *)p = ps->under[r];
        else
            *(u16 *)p = (u16)ps->under[r];
    }
    ps->drawn = 0;
}

static void (*volatile g_vbl_hook)(void);

/* In TOS's VBL queue, which calls it with every register saved. The
 * hook, gfx_vbl_hook's, first. */
static void ptr_vbl(void)
{
    int x = ms_x, y = ms_y;
    ptr_state *ps;

    if (g_vbl_hook)
        g_vbl_hook();
    if (!g_ptr_on)
        return;
    ps = &g_ptr[g_ptr_scr];
    if (ps->drawn && x == ps->x && y == ps->y)
        return;
    ptr_undraw(g_ptr_scr);
    ptr_draw(g_ptr_scr, x, y);
}

/* Our handler on the IKBD's mouse vector and the pointer in the VBL
 * queue. The position starts where GEM's pointer was, or mid-screen. */
static void mouse_init(void)
{
    void (**q)(void) = (void (**)(void))*_vblqueue;
    int i;

    if (g_vdi) {
        ms_x = *(const volatile s16 *)(g_linea - 602);     /* GCURX */
        ms_y = *(const volatile s16 *)(g_linea - 600);     /* GCURY */
    } else {
        ms_x = 320;
        ms_y = 200;
    }
    ms_bt = 0;
    g_kbdvecs = Kbdvbase();
    ms_old = g_kbdvecs->mousevec;
    g_kbdvecs->mousevec = ms_handler;

    for (i = 0; i < *nvbls; i++) {
        if (!q[i]) {
            q[i] = ptr_vbl;
            g_vbl_slot = &q[i];
            break;
        }
    }
    trace("gfx: VDI %s, pointer %s", g_vdi ? "up" : "not up",
          g_vbl_slot ? "in the VBL queue" : "fixed, the VBL queue is full");
}

static void mouse_shutdown(void)
{
    gfx_pointer(0);
    if (g_vbl_slot)
        *g_vbl_slot = NULL;
    g_vbl_slot = NULL;
    if (g_kbdvecs)
        g_kbdvecs->mousevec = ms_old;
    g_kbdvecs = NULL;
}

int gfx_init(void)
{
    void *fonts;
    const u8 *fh;
    u32 a;

    if (Getrez() != 2)
        return 0;

    /* Font header layout: first_ade at 36, last_ade at 38, dat_table at 76,
     * form_width at 80. Entry 1 of the table is the 8x8 font. */
    g_linea = linea_init(&fonts);
    fh = ((const u8 **)fonts)[1];
    g_font_first = *(const u16 *)(fh + 36);
    g_font_last = *(const u16 *)(fh + 38);
    g_font = *(const u8 *const *)(fh + 76);
    g_font_w = *(const u16 *)(fh + 80);
    trace("gfx: font %u..%u, form width %u", g_font_first, g_font_last,
          g_font_w);

    /* Screens must be on a 256-byte boundary on an ST; the STE would take
     * any even address, but there is no reason to depend on it. */
    g_mem = (u8 *)Malloc(2L * SCREEN_BYTES + 256);
    if (!g_mem)
        return 0;
    a = ((u32)g_mem + 255) & ~255UL;
    g_screen[0] = (u8 *)a;
    g_screen[1] = (u8 *)a + SCREEN_BYTES;
    memset(g_screen[0], 0, 2L * SCREEN_BYTES);

    g_old_phys = Physbase();
    g_old_log = Logbase();
    g_vdi = *(s16 *const *)(g_linea + 8) != NULL;
    if (g_vdi)
        linea_hide_mouse();
    Setscreen(g_screen[0], g_screen[0], -1);
    Vsync();
    g_back = 1;
    mouse_init();
    return 1;
}

void gfx_shutdown(void)
{
    if (!g_mem)
        return;
    mouse_shutdown();
    Setscreen(g_old_log, g_old_phys, -1);
    Vsync();
    if (g_vdi)
        linea_show_mouse();
    Mfree(g_mem);
    g_mem = NULL;
}

int gfx_mouse_buttons(void)
{
    return ms_bt;
}

void gfx_mouse_pos(int *x, int *y)
{
    *x = ms_x;
    *y = ms_y;
}

/* The pointer, for the menus, drawn into the screen being shown: the
 * menus draw there, and flip with gfx_flip, only with it hidden.
 * gfx_flip_poll keeps it. Shown, it follows the mouse at each VBL. The
 * VBL is held off while it is drawn or taken away here, so the two never
 * draw at once. */
void gfx_pointer(int on)
{
    u16 sr = ipl_raise(4);

    if (on) {
        if (!g_ptr_on) {
            g_ptr_scr = g_back ^ 1;
            ptr_draw(g_ptr_scr, ms_x, ms_y);
            g_ptr_on = 1;
        }
    } else {
        g_ptr_on = 0;
        ptr_undraw(0);
        ptr_undraw(1);
    }
    ipl_restore(sr);
}

/* `f` called at each VBL from now, in the interrupt (NULL: none). 0 if
 * there is no VBL to call it from: the queue was full. */
int gfx_vbl_hook(void (*f)(void))
{
    if (!g_vbl_slot)
        return 0;
    g_vbl_hook = f;
    return 1;
}

u8 *gfx_back(void)
{
    return g_screen[g_back];
}

u8 *gfx_front(void)
{
    return g_screen[g_back ^ 1];
}

int gfx_back_index(void)
{
    return g_back;
}

void gfx_flip(void)
{
    /* The physical base is latched at the next VBL; wait for it so the
     * screen just shown is not drawn into. */
    Setscreen(g_screen[g_back], g_screen[g_back], -1);
    Vsync();
    g_back ^= 1;
}

static u16 glyph_row(int c, int r, int style);

/* gfx_flip, calling `poll` while it waits for the VBL: the rhythm game
 * reads the keys then, so a press is timed to the ms, not the frame.
 *
 * And the shifter's base written here, not by Setscreen: Setscreen leaves
 * it to TOS's VBL handler, which writes it after the shifter has already
 * reloaded its counter for the frame starting -- so the new screen shows a
 * frame later than the VBL waited for, and the screen drawn into next is
 * still the one shown for that frame. A rhythm frame is drawn in place
 * over the last but one (rhythm.c), and was seen half-drawn (a note over
 * the PERFECT drawn after it). Written now, the base is reloaded at the
 * coming VBL. The screens are 256-byte aligned: the STE's low byte
 * ($FF820D) is 0, and writing the others clears it.
 *
 * Written just as the VBL comes, though, the base can be too late for the
 * reload and still before the interrupt is taken: the clock moves on with
 * the old screen still shown, and the next frame was drawn into it -- notes
 * half drawn, a flicker. So the shifter's counter is read after the VBL: in
 * the old screen, the flip missed it, and waits for the next. In Hatari, at
 * 8MHz, ~1 flip in 100 missed it so (4 of 356, NOTES.md). Read in the
 * blank, the counter is the new base reloaded, or the old screen's end --
 * the new one's start when the screens are in that order, never inside
 * the old one.
 *
 * A pointer shown stays shown: drawn into the new screen before it is
 * shown, and taken out of the old one, now the one drawn into, after. Taken
 * away for the wait, as the menus do, it was off the screen most of each
 * frame and blinked through the game. The VBL draws it only into the
 * screen shown, g_ptr_scr, so drawing it into the other needs no mask: a
 * mask held over the base's writing made the VBL late, and the misses
 * above four times as many. */
static u32 shifter_count(void)
{
    return ((u32)*(volatile u8 *)0xFFFF8205L << 16) |
           ((u32)*(volatile u8 *)0xFFFF8207L << 8) |
           *(volatile u8 *)0xFFFF8209L;
}

void gfx_flip_poll(void (*poll)(void))
{
    u32 vbl = *(volatile u32 *)0x462L;      /* _vbclock */
    u32 a = (u32)g_screen[g_back];
    u32 old = (u32)g_screen[g_back ^ 1], c;
    int tries = 3;                          /* never hung on it */
    u16 sr;

    if (g_ptr_on)
        ptr_draw(g_back, ms_x, ms_y);
    *(volatile u8 *)0xFFFF8201L = (u8)(a >> 16);
    *(volatile u8 *)0xFFFF8203L = (u8)(a >> 8);
    do {
        while (*(volatile u32 *)0x462L == vbl)
            poll();
        vbl = *(volatile u32 *)0x462L;
        c = shifter_count();
    } while (c >= old && c < old + SCREEN_BYTES && --tries);
    g_back ^= 1;
    sr = ipl_raise(4);
    if (g_ptr_on) {
        g_ptr_scr = g_back ^ 1;
        ptr_undraw(g_back);
    }
    ipl_restore(sr);
}

/* Row r (0-7) of character c in the 8x8 font, leftmost pixel in bit 7. */
u8 gfx_glyph(int c, int r)
{
    return (u8)(glyph_row(c, r, 0) >> 8);
}

/* ---- the blitter ----
 *
 * Registers at $FF8A00. It runs in hog mode, BLT_CHUNK lines at a time.
 *
 * Not in the shared-bus mode this first used, with Atari's bset restart
 * loop: under Hatari, a long interrupt landing mid-blit -- the music's
 * ADPCM decoder, milliseconds at a time -- left passes wrong partway down.
 * Stressed (vm.c, SELFTEST), 96 of 150 full dissolves came out wrong with a
 * tune playing and 0 of 150 without; in hog mode, 0 of 300 either way. It
 * showed as a fade's last screen holding a half-faded stage, flickering
 * against the good one while the next line typed.
 *
 * Not one hog blit per pass either: a stage pass is ~36ms, and holding the
 * bus that long would lose the 200Hz Timer C ticks the clock and the music
 * run on. Interrupts are served between chunks, and the address and
 * halftone-line registers carry on from where it stopped.
 *
 * The keyboard sets the chunk's length. The IKBD sends a byte every 1.28ms
 * (7812.5 baud) and the ACIA holds one: a chunk longer than that loses a
 * byte when a mouse packet arrives mid-blit, and TOS reads a lost header's
 * movement byte as a key -- 1 pixel right is scancode 1, Esc. Chunks of 32
 * lines (~3ms) opened the game menu when the pointer moved over a choice
 * and the screen was redrawn, in Hatari and on real hardware. At ~3.5us a
 * word (a dissolve step), BLT_CHUNK_WORDS is under 0.5ms. */
#define BLT_CHUNK_WORDS 128
#define BLT_HT     ((volatile u16 *)0xFFFF8A00L)    /* halftone RAM, 16 words */
#define BLT_SXINC  (*(volatile s16 *)0xFFFF8A20L)
#define BLT_SYINC  (*(volatile s16 *)0xFFFF8A22L)
#define BLT_SADDR  (*(volatile const void **)0xFFFF8A24L)
#define BLT_EM1    (*(volatile u16 *)0xFFFF8A28L)
#define BLT_EM2    (*(volatile u16 *)0xFFFF8A2AL)
#define BLT_EM3    (*(volatile u16 *)0xFFFF8A2CL)
#define BLT_DXINC  (*(volatile s16 *)0xFFFF8A2EL)
#define BLT_DYINC  (*(volatile s16 *)0xFFFF8A30L)
#define BLT_DADDR  (*(volatile void **)0xFFFF8A32L)
#define BLT_XCOUNT (*(volatile u16 *)0xFFFF8A36L)
#define BLT_YCOUNT (*(volatile u16 *)0xFFFF8A38L)
#define BLT_HOP    (*(volatile u8 *)0xFFFF8A3AL)
#define BLT_OP     (*(volatile u8 *)0xFFFF8A3BL)
#define BLT_CTRL   (*(volatile u8 *)0xFFFF8A3CL)
#define BLT_SKEW   (*(volatile u8 *)0xFFFF8A3DL)

#define SKEW_FXSR 0x80

/* a / b by the 68000's divu. GCC makes an int division of it otherwise,
 * a call to libgcc's __divsi3 -- in every blit, and the rhythm game, a
 * few dozen small blits a frame, measured ~0.25ms a blit before any
 * pixel moved. */
static u16 divu16(u16 a, u16 b)
{
    u32 x = a;

    __asm__("divu.w %1,%0" : "+d"(x) : "d"(b) : "cc");
    return (u16)x;
}

/* Run the blit set up: `xcount` words by `lines` lines, the halftone's line
 * number starting at `line`, a chunk at a time. Hog mode stops the CPU
 * until a chunk is done; the loop is only for safety. */
static void blt_go(int xcount, int lines, int line)
{
    int chunk = xcount > 0 && xcount < BLT_CHUNK_WORDS ?
                divu16(BLT_CHUNK_WORDS, (u16)xcount) : 1;

    while (lines > 0) {
        int n = lines > chunk ? chunk : lines;
        BLT_XCOUNT = (u16)xcount;
        BLT_YCOUNT = (u16)n;
        BLT_CTRL = (u8)(0xC0 | (line & 15));
        while (BLT_CTRL & 0x80)
            ;
        lines -= n;
        line += n;
    }
}

/* One pass over the whole stage: dst = op(hop(src, halftone), dst). The
 * halftone's line 0 is stage line 0. */
static void blt_stage(u8 *dst, const u8 *src, int hop, int op)
{
    BLT_SXINC = 2;
    BLT_SYINC = 2;
    BLT_SADDR = src;
    BLT_EM1 = BLT_EM2 = BLT_EM3 = 0xFFFF;
    BLT_DXINC = 2;
    BLT_DYINC = 2;
    BLT_DADDR = dst;
    BLT_HOP = (u8)hop;
    BLT_OP = (u8)op;
    BLT_SKEW = 0;
    blt_go(LINE_BYTES / 2, MWB_STAGE_H, 0);
}

void gfx_copy_stage(u8 *dst, const u8 *src)
{
    blt_stage(dst, src, BLT_HOP_SRC, BLT_OP_S);
}

/* Pixels [x0, x1) x [y0, y1) of one stage copied into another, widened to
 * whole words, clipped to the stage: the rhythm game puts back what its
 * notes covered rather than the whole backdrop. */
void gfx_copy_rect(u8 *dst, const u8 *src, int x0, int y0, int x1, int y1)
{
    int n;
    s16 inc;
    u32 off;

    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > MWB_STAGE_W)
        x1 = MWB_STAGE_W;
    if (y1 > MWB_STAGE_H)
        y1 = MWB_STAGE_H;
    if (x0 >= x1 || y0 >= y1)
        return;
    n = ((x1 - 1) >> 4) - (x0 >> 4) + 1;
    inc = (s16)(LINE_BYTES - 2 * (n - 1));
    off = (u32)y0 * LINE_BYTES + (x0 >> 4) * 2;
    BLT_SXINC = 2;
    BLT_SYINC = inc;
    BLT_SADDR = src + off;
    BLT_EM1 = BLT_EM2 = BLT_EM3 = 0xFFFF;
    BLT_DXINC = 2;
    BLT_DYINC = inc;
    BLT_DADDR = dst + off;
    BLT_HOP = BLT_HOP_SRC;
    BLT_OP = BLT_OP_S;
    BLT_SKEW = 0;
    blt_go(n, y1 - y0, 0);
}

void gfx_fill_stage(u8 *dst, u8 v)
{
    blt_stage(dst, dst, BLT_HOP_ONES, v ? BLT_OP_ONE : BLT_OP_ZERO);
}

void gfx_blit_stage(u8 *dst, const u8 *src, int hop, int op,
                    const u16 *halftone)
{
    int i;

    if (halftone)
        for (i = 0; i < 16; i++)
            BLT_HT[i] = halftone[i];
    blt_stage(dst, src, hop, op);
}

/* The rectangle of pixels [x0, x1) on rows [y0, y1), already clipped,
 * through `op` with an all-ones source: the hole an opaque sprite punches
 * before its data is ORed in. */
static void blt_rect(u8 *dst, int x0, int x1, int y0, int y1, int op)
{
    int w0 = x0 >> 4, w1 = (x1 - 1) >> 4;
    u16 em1 = (u16)(0xFFFF >> (x0 & 15));
    u16 em3 = (u16)(0xFFFF << (15 - ((x1 - 1) & 15)));
    int n = w1 - w0 + 1;

    if (n == 1)
        em1 &= em3;
    BLT_EM1 = em1;
    BLT_EM2 = 0xFFFF;
    BLT_EM3 = em3;
    BLT_DXINC = 2;
    BLT_DYINC = (s16)(LINE_BYTES - 2 * (n - 1));
    BLT_DADDR = dst + (u32)y0 * LINE_BYTES + w0 * 2;
    BLT_HOP = BLT_HOP_ONES;
    BLT_OP = (u8)op;
    BLT_SKEW = 0;
    blt_go(n, y1 - y0, 0);
}

/* A sprite (format in mwb.h) at stage position x, y, clipped to the stage:
 * the mask cleared out of the screen, then the data ORed in -- or, for an
 * opaque sprite, which has no mask, its rectangle cleared.
 *
 * With x not a multiple of 16 a sprite row of wpr words covers wpr + 1
 * screen words, word k being the end of source word k-1 and the start of
 * word k: the blitter's skew. FXSR primes it with word k0-1 so the first
 * screen word drawn has its left part even when the sprite is clipped on
 * the left. Where k-1 or k is outside the row (k = 0, k = wpr) the read
 * is of a neighbouring row's word and the end masks keep it off the
 * screen. */
static void blt_plane(u8 *dst, const u16 *plane, int wpr, int x, int y,
                      int y0, int y1, int op)
{
    int s = x & 15, wx = (x - s) / 16;
    int nd = s ? wpr + 1 : wpr;
    int k0 = wx < 0 ? -wx : 0;
    int k1 = nd - 1;
    int n;
    u16 em1 = 0xFFFF, em3 = 0xFFFF;
    const u16 *src = plane + (u32)(y0 - y) * wpr + k0;

    if (wx + k1 > LINE_BYTES / 2 - 1)
        k1 = LINE_BYTES / 2 - 1 - wx;
    if (k0 > k1)
        return;
    n = k1 - k0 + 1;

    if (s) {
        if (k0 == 0)
            em1 = (u16)(0xFFFF >> s);
        if (k1 == wpr)
            em3 = (u16)~(0xFFFF >> s);
        src--;                          /* FXSR reads word k0-1 first */
        /* n + 1 reads, the last at word k1, then on to the next row */
        BLT_SYINC = (s16)(2 * (wpr - n));
        BLT_SKEW = (u8)(SKEW_FXSR | s);
    } else {
        BLT_SYINC = (s16)(2 * (wpr - n + 1));
        BLT_SKEW = 0;
    }
    if (n == 1)
        em1 &= em3;
    BLT_SXINC = 2;
    BLT_SADDR = src;
    BLT_EM1 = em1;
    BLT_EM2 = 0xFFFF;
    BLT_EM3 = em3;
    BLT_DXINC = 2;
    BLT_DYINC = (s16)(LINE_BYTES - 2 * (n - 1));
    BLT_DADDR = dst + (u32)y0 * LINE_BYTES + (wx + k0) * 2;
    BLT_HOP = BLT_HOP_SRC;
    BLT_OP = (u8)op;
    blt_go(n, y1 - y0, 0);
}

void gfx_sprite(u8 *dst, const u8 *spr, int x, int y)
{
    const u16 *hp = (const u16 *)spr;
    int w = hp[0], h = hp[1], wpr = hp[2], opaque = hp[3] & MWB_SPR_OPAQUE;
    const u16 *mask = hp + 4;
    const u16 *data = opaque ? mask : mask + (u32)wpr * h;
    int y0 = y < 0 ? 0 : y;
    int y1 = y + h > MWB_STAGE_H ? MWB_STAGE_H : y + h;

    if (y0 >= y1 || x >= MWB_STAGE_W || x + w <= 0)
        return;
    if (opaque)
        blt_rect(dst, x < 0 ? 0 : x,
                 x + w > MWB_STAGE_W ? MWB_STAGE_W : x + w, y0, y1,
                 BLT_OP_ZERO);
    else
        blt_plane(dst, mask, wpr, x, y, y0, y1, BLT_OP_NOTS_AND_D);
    blt_plane(dst, data, wpr, x, y, y0, y1, BLT_OP_S_OR_D);
}

/* The same, by the CPU: the reference the blitter path is checked against
 * (vm.c's self-test). Each source word straddles two screen words when x is
 * not a multiple of 16, so it is widened to 32 bits and shifted: the high
 * half lands in the first word, the low half in the next. */
void gfx_sprite_cpu(u8 *dst, const u8 *spr, int x, int y)
{
    const u16 *hp = (const u16 *)spr;
    int w = hp[0], h = hp[1], wpr = hp[2], opaque = hp[3] & MWB_SPR_OPAQUE;
    const u16 *mask = hp + 4;
    const u16 *data = opaque ? mask : mask + (u32)wpr * h;
    int shift = x & 15, wx = (x - shift) / 16;
    int y0 = y < 0 ? 0 : y;
    int y1 = y + h > MWB_STAGE_H ? MWB_STAGE_H : y + h;
    u16 lastmask = (w & 15) ? (u16)(0xFFFF << (16 - (w & 15))) : 0xFFFF;
    int r, c;

    for (r = y0; r < y1; r++) {
        u16 *drow = (u16 *)(dst + (u32)r * LINE_BYTES);
        const u16 *mrow = mask + (u32)(r - y) * wpr;
        const u16 *srow = data + (u32)(r - y) * wpr;

        for (c = 0; c < wpr; c++) {
            u16 m = opaque ? (c == wpr - 1 ? lastmask : 0xFFFF) : mrow[c];
            u32 mm, dd;
            int i = wx + c;

            if (!m)
                continue;
            mm = ((u32)m << 16) >> shift;
            dd = ((u32)srow[c] << 16) >> shift;
            if (i >= 0 && i < LINE_BYTES / 2)
                drow[i] = (drow[i] & ~(u16)(mm >> 16)) | (u16)(dd >> 16);
            if (shift && i + 1 >= 0 && i + 1 < LINE_BYTES / 2)
                drow[i + 1] = (drow[i + 1] & ~(u16)mm) | (u16)dd;
        }
    }
}

void gfx_text_clear(u8 *scr)
{
    memset(scr + STAGE_BYTES, 0, SCREEN_BYTES - STAGE_BYTES);
}

/* One glyph row, with the styles applied. Bold is the glyph ORed with
 * itself one pixel to the right; italic leans the top rows right, as the
 * VDI's skew does. Returned as 16 bits, the glyph in the high byte, so
 * italic can spill into the next character cell. */
static u16 glyph_row(int c, int r, int style)
{
    u16 g;

    if (c < g_font_first || c > g_font_last)
        c = '?';
    g = (u16)g_font[(u32)r * g_font_w + (c - g_font_first)] << 8;
    if (style & 2)
        g |= g >> 1;
    if (style & 1)
        g >>= (7 - r) / 3;
    return g;
}

/* Draw `len` bytes of compiled text at character column `col`, pixel row
 * `y`, black on white. The inline codes switch styles; the style in force
 * is carried in and returned, so it can span lines. */
int gfx_text(u8 *scr, int col, int y, const u8 *s, int len, int style)
{
    int i, r;

    for (i = 0; i < len; i++) {
        int c = s[i];
        u8 *p;

        if (c == MWB_TXT_ITALIC_ON)       { style |= 1;  continue; }
        if (c == MWB_TXT_ITALIC_OFF)      { style &= ~1; continue; }
        if (c == MWB_TXT_BOLD_ON)         { style |= 2;  continue; }
        if (c == MWB_TXT_BOLD_OFF)        { style &= ~2; continue; }
        if (c == MWB_TXT_FAST)
            continue;
        if (col >= LINE_BYTES)
            break;
        p = scr + (u32)y * LINE_BYTES + col;
        for (r = 0; r < 8; r++, p += LINE_BYTES) {
            u16 g = glyph_row(c, r, style);
            p[0] |= (u8)(g >> 8);
            if (col + 1 < LINE_BYTES)
                p[1] |= (u8)g;
        }
        col++;
    }
    return style;
}

/* The `title` screen: `add "black.png"` under a centred text
 * (script.rpy:6-8). The text is white on the black stage, its rows doubled
 * to 8x16 so it reads as a heading. */
void gfx_title(u8 *stage, const u8 *s)
{
    int len = (int)strlen((const char *)s);
    int col = (LINE_BYTES - len) / 2;
    int y = (MWB_STAGE_H - 16) / 2;
    int i, r;

    gfx_fill_stage(stage, 0xFF);
    if (col < 0)
        col = 0;
    for (i = 0; i < len && col + i < LINE_BYTES; i++) {
        u8 *p = stage + (u32)y * LINE_BYTES + col + i;
        for (r = 0; r < 16; r++, p += LINE_BYTES)
            *p &= ~(u8)(glyph_row(s[i], r / 2, 0) >> 8);
    }
}

/* ---- for the menus ---- */

/* A rectangle of the screen through a blitter op with an all-ones source:
 * BLT_OP_ONE black, BLT_OP_ZERO white, BLT_OP_NOT_D inverted. Pixel
 * coordinates, end exclusive, anywhere on the 640x400 screen. */
void gfx_rect(u8 *scr, int x0, int y0, int x1, int y1, int op)
{
    if (x0 < x1 && y0 < y1)
        blt_rect(scr, x0, x1, y0, y1, op);
}

/* A white box with a one-pixel black border. */
void gfx_box(u8 *scr, int x0, int y0, int x1, int y1)
{
    gfx_rect(scr, x0, y0, x1, y1, BLT_OP_ONE);
    gfx_rect(scr, x0 + 1, y0 + 1, x1 - 1, y1 - 1, BLT_OP_ZERO);
}

/* A save's thumbnail: the stage at a quarter of its size, 160x90. Each
 * 4x4 block's count of black pixels, its grey, is dithered again through
 * a 4x4 Bayer matrix: a plain majority vote turned every mid-grey of the
 * Atkinson-dithered backdrops black. */
void gfx_thumb(const u8 *stage, u8 *out)
{
    static const u8 bits[16] = { 0, 1, 1, 2, 1, 2, 2, 3,
                                 1, 2, 2, 3, 2, 3, 3, 4 };
    static const u8 bayer4[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 },
                                     { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
    int ty, tx, r;

    for (ty = 0; ty < THUMB_H; ty++) {
        const u8 *row = stage + (u32)ty * 4 * LINE_BYTES;
        for (tx = 0; tx < THUMB_W / 8; tx++) {
            u8 o = 0;
            int b;
            for (b = 0; b < 4; b++) {
                int hi = 0, lo = 0;
                for (r = 0; r < 4; r++) {
                    u8 v = row[r * LINE_BYTES + tx * 4 + b];
                    hi += bits[v >> 4];
                    lo += bits[v & 15];
                }
                {
                    const u8 *t = bayer4[ty & 3];
                    int px = (tx * 8 + b * 2) & 3;
                    o = (u8)((o << 2) | (hi > t[px] ? 2 : 0) |
                             (lo > t[px + 1] ? 1 : 0));
                }
            }
            out[ty * (THUMB_W / 8) + tx] = o;
        }
    }
}

/* Put a thumbnail on the screen at byte column `col`, pixel row y. */
void gfx_put_thumb(u8 *scr, int col, int y, const u8 *thumb)
{
    int r;

    for (r = 0; r < THUMB_H; r++)
        memcpy(scr + (u32)(y + r) * LINE_BYTES + col,
               thumb + r * (THUMB_W / 8), THUMB_W / 8);
}
