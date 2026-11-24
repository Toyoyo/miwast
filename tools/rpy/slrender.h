#ifndef MR_SLRENDER_H
#define MR_SLRENDER_H

#include "mr.h"
#include "slang.h"
#include "font.h"
#include "expr.h"

/* Install the screen-local variable scope into the expression evaluator. Must
 * be called once before any screen is rendered. */
void mr_sl_init(void);

/* Screen variables: `default x = ...` and SetScreenVariable. They persist
 * while a screen is shown, unlike the per-frame scope, and are discarded when
 * it is shown again. Returns 0 if there is no screen being rendered. */
int  mr_sl_set_screen_var(const char *name, const mr_value *v);

/* The same, for a screen other than the one being rendered. Screen arguments
 * are pushed in this way before the screen is first drawn. */
int  mr_sl_set_screen_var_for(const char *screen, const char *name,
                              const mr_value *v);

/* True while a `modal True` screen is up: it swallows clicks meant for what
 * is behind it, and the right-click that would otherwise toggle the game
 * menu belongs to it instead. */
int  mr_sl_modal_active(void);
void mr_sl_screen_vars_clear(const char *screen);

/* Log the next interactive screen's layout: every node, its resolved style,
 * and its rectangle. Driven by MR_LAYOUT=1. */
void mr_sl_dump_next(void);

/* Log how much layout work each screen render costs. MR_STATS=1. */
void mr_sl_stats(int on);

/* The renderer needs fonts but must not own them, so the UI layer installs a
 * lookup. Keyed by face *and* size: a game may name a second font on part of
 * its interface (gui.interface_text_font), which arrives here as the `font`
 * style property. A NULL or unknown `font` means the default face. */
typedef mr_font *(*mr_font_provider)(const char *font, int size);
void mr_sl_set_font_provider(mr_font_provider fn);

/* Resolving `use` needs to find other screens, which live in the script. */
typedef const sl_screen *(*mr_screen_lookup)(const char *name);
void mr_sl_set_screen_lookup(mr_screen_lookup fn);

/* Draw a whole screen over the stage.
 *
 * `capture` records the buttons drawn so they can be hit-tested; it is off
 * while painting transition snapshots, which are not interactive.
 * `hover` is the focus id to draw in its hover state, or -1. */
void mr_sl_render(mr_surface *dst, const sl_screen *sc, int capture, int hover);

/* Would any bar drawn last frame land on a different pixel if it were drawn
 * now? A bar's value can move with nothing else on screen moving -- the
 * jukebox's position bar -- and the frame loop skips idle frames, so this is
 * what gets one repainted, exactly when the picture would differ. */
int mr_sl_bars_moved(void);

/* Clear the focus list. Called once per frame before the screens are drawn. */
void mr_sl_focus_reset(void);

/* Focus id under a stage coordinate, or -1. Later entries win, so a button
 * drawn on top of another takes the click. */
int mr_sl_hit(int sx, int sy);

/* The unevaluated action expression of a focus id, or NULL. */
const char *mr_sl_focus_action(int id);

/* Actions are evaluated on click, but one written inside a `for` loop means
 * nothing without the loop variable -- every save slot's FileAction(slot)
 * would see the same value. Bracket the evaluation with these to restore the
 * scope the button was drawn in. */
void mr_sl_focus_scope(int id);
void mr_sl_focus_scope_end(void);

/* Not every focusable thing runs an action. A bar and a scrollbar are dragged
 * instead: press, then move with the button held. */
int  mr_sl_focus_draggable(int id);
void mr_sl_focus_drag(int id, int sx, int sy);

/* Scroll the viewport under a point by `delta` wheel notches. Returns 1 if
 * there was one. Windows 3.1 predates the wheel, so this only ever fires on
 * a modern host; the scrollbar is the real control. */
int mr_sl_scroll_at(int sx, int sy, int delta);

/* The action a shown screen binds to a keysym with `key "<sym>" action ...`,
 * or NULL. Rebuilt every frame from what was actually laid out, so a screen
 * that is no longer up binds nothing. `screen game_menu` uses this to send
 * Escape and the right-click back to the main menu, which is the only way
 * out of a sub-menu opened before the game starts. */
const char *mr_sl_key_action(const char *sym);

/* Whether an action expression reports itself as selected -- what decides
 * that a radio or check button draws in the selected_ prefix. */
int mr_sl_action_selected(const char *action);

/* The rectangle the screens painted since the last reset, or 0 if none did.
 * Lets the frame loop ask whether the screens that are up stayed inside the
 * dialogue strip -- if they did, a line typing itself out can still be a
 * partial frame instead of a full 1280x720 recomposite. */
void mr_sl_extent_reset(void);
int  mr_sl_extent(int *x, int *y, int *w, int *h);

/* A #rrggbb style property, with the same state-prefix search. */
mr_u32 mr_sl_style_color(const char *style, const char *state,
                         const char *name, mr_u32 dflt);

/* Draw a style's background -- Frame, image or #rrggbb -- for a caller that
 * has no screen node, so that natively drawn furniture (the choice menu)
 * looks exactly like the same style drawn by the screen language. `state` is
 * the style prefix *including its underscore* -- "idle_", "hover_", or NULL.
 * Passing "idle" silently matches nothing. */
void mr_sl_draw_style_background(mr_surface *dst, const char *style,
                                 const char *state, int x, int y,
                                 int w, int h);

/* Ren'Py's Frame(): the nine-slice the screen language draws a styled
 * background with, exposed for a caller that has no screen to hang it on --
 * the minigame's give-up dialog, which appears over a display the screen
 * system does not own. Does nothing if the image will not load. */
void mr_sl_draw_frame_image(mr_surface *dst, const char *image,
                            int l, int t, int r, int b,
                            int x, int y, int w, int h);

/* Size of the per-frame layout cache, the most entries any frame has used,
 * and how many there are. For memreport; the bytes are static, not heap, and
 * a high water mark at capacity means the cache has quietly stopped caching. */
void mr_sl_measure_cache_stats(long *bytes, int *high_water, int *capacity);

#endif /* MR_SLRENDER_H */
