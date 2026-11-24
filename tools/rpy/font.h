#ifndef MR_FONT_H
#define MR_FONT_H

#include "mr.h"

typedef struct mr_font mr_font;

mr_font *mr_font_load(const char *path);
void     mr_font_free(mr_font *f);

/* Where to look for a glyph this atlas does not hold.
 *
 * A game can put a second face on part of its interface (Hinata's
 * gui.interface_text_font). mkfont.py bakes such a face with only the block it
 * covers -- Latin for a handwriting font, nothing above U+2E7F -- so that the
 * CJK glyphs, which are most of an atlas, are stored once in the default face
 * rather than once per face. This is what makes the rest reachable.
 *
 * `fb` is *borrowed*: mr_font_free does not touch it, because ui.c owns every
 * atlas it loads and frees them all in one pass. Chains are followed to a
 * fixed depth, so a cycle costs a few wasted probes rather than hanging. */
void     mr_font_set_fallback(mr_font *f, mr_font *fb);

int mr_font_line_height(const mr_font *f);

/* Width in pixels of a UTF-8 string. {b} and {i} select a baked variant and
 * so change the width; {u} and every unrendered tag cost nothing. */
int mr_font_text_width(const mr_font *f, const char *utf8);

/* Lines the text will occupy if it is not wrapped: one more than the
 * newlines in it. */
int mr_font_text_lines(const char *utf8);

/* Draw one line. Returns the x position just past the last glyph. */
int mr_font_draw(mr_surface *dst, const mr_font *f, int x, int y,
                 const char *utf8, mr_u32 color);

/* As above with a one-pixel outline in `outline`, which is how the game's
 * character names are styled (who_outlines=[(1, "#000000", 0, 0)]). */
int mr_font_draw_outlined(mr_surface *dst, const mr_font *f, int x, int y,
                          const char *utf8, mr_u32 color, mr_u32 outline);

/* Draw with word wrapping inside `width`. Returns the number of lines drawn.
 * Passing a NULL dst measures without drawing. Style tags carry across the
 * wrap: {b} opened on one line is still in force on the next. */
int mr_font_draw_wrapped(mr_surface *dst, const mr_font *f, int x, int y,
                         int width, const char *utf8, mr_u32 color,
                         int outlined, mr_u32 outline);

/* As above, but stop after `limit` characters (-1 for all of them). The text
 * is laid out in full regardless, so a partly revealed line does not reflow
 * as the rest of it arrives. */
int mr_font_draw_revealed(mr_surface *dst, const mr_font *f, int x, int y,
                          int width, const char *utf8, mr_u32 color,
                          int outlined, mr_u32 outline, int limit);

/* As above, with each line shifted within `width` by `align` (0 left, 0.5
 * centred, 1.0 right) -- Ren'Py's textalign. */
int mr_font_draw_aligned(mr_surface *dst, const mr_font *f, int x, int y,
                         int width, const char *utf8, mr_u32 color,
                         int outlined, mr_u32 outline, int limit, float align);

#endif /* MR_FONT_H */
