#ifndef MR_UI_H
#define MR_UI_H

#include "mr.h"

#include "font.h"

int  mr_ui_init(const char *game_dir);

/* Re-read gui.text_size and gui.name_text_size for the dialogue box. Call
 * after the init phase has run -- mr_ui_init happens before the script is
 * parsed, so it can only use defaults. */
void mr_ui_sync_fonts(void);

/* The same, for the say window's geometry: gui.textbox_height and _yalign,
 * gui.dialogue_xpos/_ypos/_width, gui.name_xpos/_ypos, gui.namebox_borders.
 * Call alongside mr_ui_sync_fonts and for the same reason.
 *
 * Not optional for a new game: mr_ui_dialogue_rect derives the partial-redraw
 * band from gui.textbox_height, so skipping this leaves a game whose textbox
 * is a different height repainting the wrong strip. */
void mr_ui_sync_geometry(void);

/* Nearest baked font to a requested point size, in the default face -- the
 * one the game's gui.text_font names. */
mr_font *mr_ui_font(int size);

/* The same in a named face: `name` is the font filename as the script's
 * `gui.*_text_font` gave it (Hinata's "Pecita-0owz.otf"), which arrives here
 * from the `font` style property. A NULL name, an unknown one, or a face
 * whose atlas failed to load all give the default face, so a game that names
 * nothing behaves exactly as before.
 *
 * The returned atlas has its fallback set to the default face at the same
 * size: a named face is baked with only the block it covers, so a kanji in an
 * interface label is found there rather than nowhere. */
mr_font *mr_ui_font_named(const char *name, int size);
void mr_ui_shutdown(void);

/* Which focusable element the pointer is over, by focus id. */
void mr_ui_set_hover(int id);
int  mr_ui_hover(void);

/* The choice menu is drawn natively rather than as a screen, so it does its
 * own hit testing: the index of the option under a stage point, or -1.
 * mr_ui_set_menu_hover takes that index and repaints if it changed. */
int  mr_ui_menu_hit(int sx, int sy);
void mr_ui_set_menu_hover(int i);

/* Draw the dialogue box and any menu on top of the composited scene. */
void mr_ui_draw(mr_surface *stage);

/* The same drawing, in the two halves the band cache is taken between.
 * `panel` is the textbox art, which is identical for every frame of a line;
 * `front` is the namebox, the name, the line and any menu, which is what a
 * typing frame changes. mr_ui_draw is the two in order and nothing else, so a
 * caller that does not want the seam can ignore it entirely.
 *
 * Called in this order or the box art paints over the text. See bandcache.h
 * for why the seam is here rather than after mr_ui_draw as a whole. */
void mr_ui_draw_panel(mr_surface *stage);
void mr_ui_draw_front(mr_surface *stage);

/* The rectangle the dialogue window occupies, for a partial repaint. Returns
 * 0 when the window is not up, or when something is drawn outside it -- a
 * menu -- and the whole stage has to be repainted instead. */
int mr_ui_dialogue_rect(int *x, int *y, int *w, int *h);

#endif /* MR_UI_H */
