/* main.c - start, input, time, and the way out.
 *
 * The program runs in supervisor mode on its own stack: Super(0) makes the
 * current user stack the supervisor one, so the stack size below is the one
 * that counts.
 */

#include <stdlib.h>
#include <mint/osbind.h>
#include <mint/sysvars.h>
#include <mint/basepage.h>

#include "st.h"

/* libcmini's crt0 keeps this much stack for us and gives the rest back. */
long _stksize = 32768L;

static long g_old_ssp;
static int  g_gfx_up;

void to_user(long old_ssp);         /* super.s */

/* conterm, the console configuration byte: bit 0 is the keyclick, bit 2
 * the bell on ^G. Both are off while the game runs. The SNDH player clears
 * it for as long as a tune plays and puts back what it found when the tune
 * stops -- which, before this, was the click switched on, so every key
 * pressed between two tracks (dismissing a title card) clicked. */
#define CONTERM ((volatile u8 *)0x484L)
static u8  g_old_conterm;
static int g_conterm_saved;

#ifdef MW_AUTOPLAY
int g_autoplay = 1;
#else
int g_autoplay = 0;
#endif

u32 now_ms(void)
{
    return *_hz_200 * 5;
}

static void shutdown(void)
{
    /* The music first: stopping the player restores conterm to the value
     * it saved, and the original has to be written after that. */
    music_shutdown();
    if (g_conterm_saved)
        *CONTERM = g_old_conterm;
    g_conterm_saved = 0;
    if (g_gfx_up)
        gfx_shutdown();
    g_gfx_up = 0;
    pak_close();
    /* Not Super(g_old_ssp): see super.s. */
    if (g_old_ssp)
        to_user(g_old_ssp);
    g_old_ssp = 0;
}

void fatal(const char *msg)
{
    trace("fatal: %s", msg);
    shutdown();
    (void)Cconws("\r\nMIWA: ");
    (void)Cconws(msg);
    (void)Cconws("\r\nPress a key.\r\n");
    Cconin();
    Pterm(1);
}

/* Ctrl, from the live shift state rather than from a key event, so the skip
 * lasts exactly as long as the key is held. */
int skip_held(void)
{
    return (Kbshift(-1) & 4) != 0;
}

/* Scancodes. The ST has no Page Up/Down and no wheel, which are Ren'Py's
 * rollback keys (00keymap.rpy); the arrows stand in, and Undo and Help
 * (beside it) are the obvious names for going back and forward -- at a
 * menu, where the arrows move the focus, the only ones.
 */
#define SC_UP   0x48
#define SC_DOWN 0x50
#define SC_UNDO 0x61
#define SC_HELP 0x62

static int g_buttons;               /* mouse buttons at the last poll */

void input_reset(void)
{
    g_buttons = gfx_mouse_buttons();
}

long read_key(void)
{
    return Bconstat(2) ? Bconin(2) : 0;
}

/* What the player did since the last poll. A button counts when it goes
 * down, so one still held from before input_reset() does nothing. Esc and
 * the right button are Ren'Py's game_menu keys (00keymap.rpy). */
int poll_input(int flags)
{
    int hard = flags & W_HARD;
    int b, pressed;

    while (Bconstat(2)) {
        long k = Bconin(2);
        int c = (int)(k & 0xFF);
        int sc = (int)((k >> 16) & 0xFF);
        trace("key %08lx", (unsigned long)k);
        if (c == 0x1B)
            return g_autoplay ? IN_QUIT : IN_MENU;
        if (sc == SC_UP || sc == SC_UNDO)
            return IN_BACK;
        if (sc == SC_DOWN || sc == SC_HELP)
            return IN_FORWARD;
        if (!hard && (c == ' ' || c == '\r'))
            return IN_ADVANCE;
    }
    if ((flags & W_SKIPOK) && skip_held())
        return IN_SKIP;
    b = gfx_mouse_buttons();
    pressed = b & ~g_buttons;
    g_buttons = b;
    if (pressed & 2)
        return IN_MENU;
    if ((pressed & 1) && !hard)
        return IN_ADVANCE;
    return IN_NONE;
}

/* Wait for the player, or for `ms` to pass when it is not 0. A hard wait
 * cannot be clicked through, only quit -- or skipped, which Ren'Py lets
 * cross a hard pause too (statementexports.py:131). */
int wait_input(u32 ms, int flags)
{
    u32 until = now_ms() + ms;

    /* The test build plays itself: every wait is over at once, so a run
     * goes through the whole script -- every load, every track -- under
     * Hatari's fast-forward, or up to the STOPAT offset (vm.c). Esc still
     * quits. */
    if (g_autoplay) {
        while (Bconstat(2))
            if ((Bconin(2) & 0xFF) == 0x1B)
                return IN_QUIT;
        return ms ? IN_TIMEOUT : IN_ADVANCE;
    }

    input_reset();
    for (;;) {
        int r = poll_input(flags);
        if (r != IN_NONE)
            return r;
        if (ms && (long)(now_ms() - until) >= 0)
            return IN_TIMEOUT;
        Vsync();
    }
}

int main(void)
{
    /* Where the program's text starts, to read a crash PC against the
     * linker map (build/MIWA.map). */
    trace("main: text at %p", (void *)_base->p_tbase);

    g_old_ssp = Super(0L);
    g_old_conterm = *CONTERM;
    g_conterm_saved = 1;
    *CONTERM = g_old_conterm & ~(u8)0x05;

    if (!gfx_init())
        fatal("needs monochrome high resolution and 64KB for the screens");
    g_gfx_up = 1;
    if (!pak_open("MIWA.PAK"))
        fatal("cannot open MIWA.PAK");
    if (!music_init())
        fatal("not enough memory for the music");
#ifdef MW_RHYTHMTEST
    rtest_run();
    trace("exit");
    shutdown();
    return 0;
#endif
    if (!vm_load())
        fatal("cannot load the script");
    menu_init();

    vm_run();

    trace("exit");
    shutdown();
    return 0;
}
