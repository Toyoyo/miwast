#ifndef MR_INTERP_H
#define MR_INTERP_H

#include "mr.h"
#include "script.h"
#include "expr.h"

int  mr_interp_init(mr_script *s, const char *game_dir);
void mr_interp_shutdown(void);

/* Run the init-phase declarations and jump to a label. */
void mr_interp_start(const char *label);

/* Open on the main menu instead of running the script immediately. */
void mr_interp_main_menu(void);

/* Show a screen, replacing any other screen sharing its tag. */
void mr_interp_show_screen(const char *name);

/* Run a screen action expression, such as `ShowMenu("load")`. */
void mr_interp_action(const char *expr);

/* Right-click: open the game menu during play, close it if already open. */
void mr_interp_game_menu_toggle(void);

/* Step back one line of dialogue. */
void mr_interp_rollback(void);

/* Ren'Py's rollforward: undo a rollback. Does nothing when no rollback is
 * outstanding, which is why PageDown and the wheel down are silent during
 * ordinary play rather than advancing the story. */
void mr_interp_rollforward(void);

/* Engine-provided lists, for `for x in <name>` in a screen. An item is a set
 * of dotted suffixes the loop binds beneath the loop variable, so
 * `for h in _history_list` gives `h.who`, `h.what` and `h.who_args.color`. */
typedef struct {
    const char *suffix;
    mr_value    value;
} mr_list_field;

#define MR_LIST_FIELDS 6

/* Length, or -1 if the name is not an engine list. */
int mr_interp_list_len(const char *name);
int mr_interp_list_item(const char *name, int i, mr_list_field *out, int max);

/* Set a preference from a 0..1 fraction, as a dragged bar does. `name` is the
 * Ren'Py preference name, e.g. "music volume". */
void mr_interp_pref_set_fraction(const char *name, double frac);
double mr_interp_pref_get_fraction(const char *name);

/* Whether `Preference(name, value)` as an action is currently selected, which
 * is what makes the radio and check buttons show which one is chosen. */
int mr_interp_pref_selected(const char *name, const char *value);

/* Run whatever a visible screen bound to this Ren'Py keysym ("K_SPACE"), and
 * say whether anything was bound. */
int mr_interp_screen_key(const char *sym);

/* --- skipping ---
 *
 * Ctrl held skips while it is down, Tab toggles it. Skipping stops by itself
 * at a line that has never been read (unless "Texte non lu" is on) and at a
 * menu (unless "Apres les choix" is on), so the caller does not have to
 * police it. */
int  mr_interp_skipping(void);
void mr_interp_skip_hold(int down);
void mr_interp_skip_toggle(void);
void mr_interp_skip_stop(void);

/* Advance execution as far as it can go without waiting. */
void mr_interp_update(long now_ms);

/* Is the dialogue window on screen? Governed by `window show`/`window hide`,
 * and forced on while a line of dialogue is up. A transition does not affect
 * it: the window sits above the effect. */
int mr_interp_window_visible(void);

/* Whether the last update left anything that still needs repainting. */
int mr_interp_needs_redraw(void);

/* *What* needs repainting, which is worth knowing because a full stage
 * composite costs a few hundred milliseconds on the target and a line typing
 * itself out changes a couple of hundred pixels inside the dialogue box.
 * MR_REDRAW_DIALOGUE promises that nothing outside that box has changed. */
#define MR_REDRAW_NONE     0
#define MR_REDRAW_DIALOGUE 1
#define MR_REDRAW_ALL      2
int mr_interp_redraw_kind(void);

/* The same answer with the "a statement ran this frame" override removed, for
 * the MR_REDRAWPROBE measurement in main.c and nothing else. Never use this to
 * decide what to draw: it is deliberately the *unsafe* answer, and the probe's
 * whole job is to find out how often it would have been the right one. */
int mr_interp_redraw_kind_nostep(void);

/* Where the story is, for a save or a rollback checkpoint. */
struct mr_gamestate;
void mr_interp_capture(struct mr_gamestate *g);
void mr_interp_restore(const struct mr_gamestate *g);

/* Composite the current game state. */
void mr_interp_render(mr_surface *stage, long now_ms);

/* The screens that sit above the dialogue window -- `zorder 100` on the
 * quick menu. Call after the text layer has drawn, or the textbox art is
 * composited over them. */
void mr_interp_render_overlay(mr_surface *stage);

/* Player input during the rhythm minigame. Both return 1 if the event was
 * consumed, in which case the frame loop must not also treat it as advancing
 * the story or as quitting the game -- Escape opens the minigame's give-up
 * dialog rather than closing the program. */
int  mr_interp_rhythm_key(int key);   /* an MR_KEY_*, see keys.h */
int  mr_interp_rhythm_click(int x, int y);

/* Pointer motion during the minigame. 1 when its give-up dialog took it, in
 * which case the screen layer's hover must not also run. */
int  mr_interp_rhythm_hover(int x, int y);

/* Player input. `advance` is a click or a key that dismisses dialogue. */
void mr_interp_advance(void);
void mr_interp_choose(int row);    /* pick the menu row, 0-based */

int  mr_interp_finished(void);

/* Screen definitions, for the UI layer's screen drawer. */
const sl_screen *mr_interp_find_screen(const char *name);

/* Current dialogue, for the text layer. Returns 0 when nothing is showing. */
/* The dialogue to draw, if any. `shown` receives how many characters of it
 * have been revealed at the current text speed, or -1 for all of it. */
int mr_interp_dialogue(const char **who, const char **what, mr_u32 *color,
                       int *shown);

/* Is the story parked on a wait -- `renpy.pause()`, or a movie cutscene
 * holding its frame? True for both the timed and the click-only kind. */
int mr_interp_waiting(void);

/* Run `label splashscreen` if the game has one. */
void mr_interp_splashscreen(void);

/* Whether the quick menu's buttons may be pressed. Ren'Py greys a button
 * whose action is not sensitive and ignores the click. */
int mr_interp_can_rollback(void);
int mr_interp_quicksave_exists(void);

/* The save page on display, for FilePage()'s selected state. */
const char *mr_interp_file_page(void);

/* Its display name -- "Page 1", "Quick saves". The save screen's `input`
 * asks for this every frame, so the label follows the page. */
const char *mr_interp_file_page_name(void);

/* What FilePageNameInputValue() evaluates to; see interp.c. */
#define MR_PAGENAME_HANDLE "\001FP"
int mr_interp_can_skip(void);

/* The text layer tells the interpreter it has painted the current line, and
 * whether it painted the whole of it. What stops the frame loop skipping the
 * frame that would have shown the last few characters. */
void mr_interp_say_painted(int complete);

/* A transient message from Notify()/renpy.notify, or NULL once it has timed
 * out. Drawn by the text layer rather than being a screen, so that it takes
 * no part in saves or transition snapshots. */
const char *mr_interp_notify_text(void);

/* Menu state, for the text layer. The rows offered, which is not always every
 * option the script declared: a choice written
 *
 *     "Then, so be it" if lifechoice1 == True:
 *
 * is left out when its condition is false. Rows are numbered from 0 and are
 * what mr_interp_choose takes. */
int mr_interp_menu(int *count, const char **texts, int max);

/* Whether a row may be picked. Always true unless the game sets
 * config.menu_include_disabled, which keeps a failed condition's option on
 * screen instead of dropping it; such a row draws insensitive and takes
 * neither hover nor click. */
int mr_interp_menu_sensitive(int row);

#endif /* MR_INTERP_H */
