/* st.h - the ST runtime's shared declarations.
 *
 * The whole program runs in supervisor mode (main.c switches with Super(0)
 * at start, keeping its own stack), because it touches the hardware -- the
 * 200Hz counter, the SNDH player's Timer C vector -- throughout.
 */

#ifndef ST_H
#define ST_H

#include "mwb.h"

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned long  u32;
typedef short          s16;
typedef long           s32;

/* ---- main.c ---- */
void fatal(const char *msg);
u32  now_ms(void);              /* from the 200Hz system counter */
int  skip_held(void);           /* Ctrl is down */

/* wait_input: wait for the player, or for `ms` when it is not 0.
 * poll_input: what the player has done since the last poll, or IN_NONE;
 * input_reset starts the mouse edge detection afresh. */
#define W_HARD   1              /* a hard pause: only the menu ends it */
#define W_SKIPOK 2              /* holding Ctrl may end it */
int  wait_input(u32 ms, int flags);
int  poll_input(int flags);
void input_reset(void);
long read_key(void);            /* for the menus: Bconin's value, or 0 */
#define IN_NONE    (-1)
#define IN_TIMEOUT 0
#define IN_ADVANCE 1            /* Space, Return, left click */
#define IN_QUIT    2            /* the menu's Quit, confirmed */
#define IN_SKIP    3            /* Ctrl went down */
#define IN_BACK    4            /* rollback: Up arrow, Undo */
#define IN_FORWARD 5            /* rollforward: Down arrow */
#define IN_MENU    6            /* Esc, right click: the game menu */

extern int g_autoplay;

/* ---- nf.c: Hatari NatFeats tracing, a no-op unless built with it ---- */
#ifdef MW_NATFEATS
void trace(const char *fmt, ...);
#else
#define trace(...) ((void)0)
#endif

/* ---- pak.c ---- */
int   pak_open(const char *path);
void  pak_close(void);
s32   pak_size(const char *name);           /* -1 if absent */
int   pak_read(const char *name, void *buf, u32 max);
void *pak_load(const char *name, u32 *size);  /* malloc'd; NULL if absent */
s32   pak_max_size(const char *prefix);     /* largest entry with prefix */

/* ---- gfx.c ---- */
#define SCREEN_BYTES 32000
#define STAGE_BYTES  MWB_BG_BYTES           /* the top 360 lines */
#define LINE_BYTES   80

int   gfx_init(void);
void  gfx_shutdown(void);
int   gfx_mouse_buttons(void);              /* bit 0 left, bit 1 right */
void  gfx_mouse_pos(int *x, int *y);
void  gfx_pointer(int on);                  /* the mouse pointer, for menus */
int   gfx_vbl_hook(void (*f)(void));        /* f at each VBL, or NULL */
u8   *gfx_back(void);                       /* the screen being drawn */
u8   *gfx_front(void);                      /* the screen being shown */
int   gfx_back_index(void);                 /* 0 or 1, which one it is */
void  gfx_flip(void);                       /* show it, at the next VBL */
void  gfx_flip_poll(void (*poll)(void));    /* the same, polling meanwhile */
u8    gfx_glyph(int c, int r);              /* 8x8 font row, bit 7 left */

/* Blitter HOP (source) and OP (logic) codes. */
#define BLT_HOP_ONES      0
#define BLT_HOP_HT        1
#define BLT_HOP_SRC       2
#define BLT_HOP_SRC_AND_HT 3
#define BLT_OP_ZERO       0
#define BLT_OP_S          3
#define BLT_OP_NOTS_AND_D 4
#define BLT_OP_S_XOR_D    6
#define BLT_OP_S_OR_D     7
#define BLT_OP_NOT_D      10
#define BLT_OP_NOTS       12
#define BLT_OP_ONE        15

void  gfx_copy_stage(u8 *dst, const u8 *src);
void  gfx_copy_rect(u8 *dst, const u8 *src, int x0, int y0, int x1, int y1);
void  gfx_fill_stage(u8 *dst, u8 v);        /* 0 white, 0xFF black */
void  gfx_blit_stage(u8 *dst, const u8 *src, int hop, int op,
                     const u16 *halftone);  /* halftone: 16 words or NULL */
void  gfx_sprite(u8 *dst, const u8 *spr, int x, int y);
void  gfx_sprite_cpu(u8 *dst, const u8 *spr, int x, int y);
void  gfx_rect(u8 *scr, int x0, int y0, int x1, int y1, int op);
void  gfx_box(u8 *scr, int x0, int y0, int x1, int y1);
#define THUMB_W 160
#define THUMB_H 90
#define THUMB_BYTES (THUMB_W / 8 * THUMB_H)
void  gfx_thumb(const u8 *stage, u8 *out);
void  gfx_put_thumb(u8 *scr, int col, int y, const u8 *thumb);
void  gfx_text_clear(u8 *scr);
int   gfx_text(u8 *scr, int col, int y, const u8 *s, int len, int style);
#define GFX_ITALIC 1
#define GFX_BOLD   2
void  gfx_title(u8 *stage, const u8 *s);

/* ---- music.c ---- */
int   music_init(void);
s32   music_max(void);              /* the longest tune's bytes */
void  music_play(int id, u16 flags);
int   music_load(int id);           /* music_play in two: read, */
void  music_start(int id);          /* then start (after a load only) */
void  music_pause(void);            /* the tune held where it is, */
void  music_resume(void);           /* and on from there */
void  music_stop(void);
void  music_sync(int id);
void  music_shutdown(void);
void  hit_begin(void);              /* the YM's hit sounds, for the rhythm */
void  hit_play(int note);           /* game: a MIDI note, plucked */
void  hit_end(void);

/* ---- vm.c ---- */
int   vm_load(void);
void  vm_run(void);
u8   *arena_cede(u32 bytes);        /* the block's top bytes, for a tune */
extern u32 g_script_sig;            /* which build of the script this is */

/* ---- rhythm.c: the rhythm minigame (MWB_RHYTHM) ---- */
typedef struct rhythm_in {
    const u8 *bg;                   /* the backdrop, STAGE_BYTES */
    const u8 *note[8];              /* sprites: left, up, down, right,
                                     * then the same at 1.25x */
    const u8 *btm;                  /* R<nn>.BTM, as in the pak */
    int       mus;                  /* the track */
    u8       *work;                 /* STAGE_BYTES it may use */
} rhythm_in;
int   rhythm_begin(const rhythm_in *in);    /* 0 if it cannot */
void  rhythm_first(u8 *stage);              /* its first frame's stage */
void  rhythm_run(s32 ret[3]);               /* (hits, notes, score) */
void  rhythm_end(void);                     /* frees what begin took */
/* vm.c: the song's high score raised to `score` if that is higher, in the
 * persistent file; 1 if it was. */
int   persist_record(u32 key, s32 score);

/* ---- rtest.c: the rhythm game's feasibility test (RHYTHMTEST=<track>) */
void  rtest_run(void);

/* ---- menu.c: the game menu, save and load ---- */
#define SAVE_TEXT 80
typedef struct save_rec {
    u16 used;
    u16 cp;                         /* the checkpoint a load re-runs, by
                                     * number: what a slot with no log
                                     * file is loaded from */
    u16 rb_limit;                   /* the rollback limit there */
    u16 date, time;                 /* GEMDOS format */
    u8  text[SAVE_TEXT];            /* the line, or the chapter */
    u8  thumb[THUMB_BYTES];
} save_rec;

#define MENU_RESUME 0
#define MENU_LOAD   1               /* *loaded, slot *slot, is to be loaded */
#define MENU_QUIT   2
#define MENU_RESTART 3              /* a new game, from the top */
int   menu_run(const save_rec *cur, save_rec *loaded, int *slot);
int   save_put(int slot, const save_rec *cur);
void  save_get(int slot, save_rec *r);
/* vm.c: a slot's rollback log, in a file of its own. */
int   log_write(int slot, const save_rec *r);
void  log_delete(int slot);
/* The choice screen: n one-line buttons, `sel` focused (-1 none). */
void  choice_draw(u8 *scr, const u8 *const *label, int n, int sel);
int   choice_at(int x, int y, const u8 *const *label, int n);  /* or -1 */
void  menu_init(void);                /* reads the preferences */
int   menu_confirm(const char *q);    /* the Oui/Non dialog: 1 for Oui */
extern int g_text_cps;              /* preferences.text_cps; 0 instant */
extern int g_lang;                  /* preferences.language: MWB_LANG_* */

#endif
