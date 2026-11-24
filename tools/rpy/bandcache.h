/* bandcache.h - the composited dialogue strip, kept between frames.
 *
 * A line typing itself out is the commonest frame in the game, and the band
 * path (main.c, mr_present_band) already narrows it from a 1280x720
 * recomposite to the dialogue strip. What is left inside that strip is still
 * almost all work whose *result does not change* while the line types: the
 * stage is filled black, the background is blended in, the sprites over it,
 * and then the textbox art is blended on top. Only the glyphs differ from one
 * frame to the next.
 *
 * So the strip is kept. A band frame copies it back onto the stage and draws
 * the name and the line over it -- a 27 ns/px copy in place of a 43..137 ns/px
 * blend over the same pixels. Measured figures are in NOTES.md under "The
 * dialogue band strip cache".
 *
 * What this module does *not* do is decide when that is sound. It compares the
 * rectangle and nothing else; the caller owns the question of whether the
 * scene is at rest, and gets to say so with `keep`. That split is deliberate:
 * the policy lives where the frame's state is known (main.c and bandtest),
 * and the mechanism is small enough to be exactly testable.
 *
 * The seam is "after the textbox art, before the namebox". It is the latest
 * point where everything painted so far is static for the whole line, and it
 * is why ui.h splits mr_ui_draw into mr_ui_draw_panel and mr_ui_draw_front
 * rather than caching after mr_ui_draw as a whole. Paint order is unchanged:
 * the cache stands in for drawing, it does not reorder it.
 */
#ifndef MR_BANDCACHE_H
#define MR_BANDCACHE_H

#include "mr.h"

/* Does a strip for exactly this rectangle exist? If so it is copied onto
 * `stage` at `y` and 1 is returned, and the caller may skip everything up to
 * the seam. 0 means there is nothing in hand and the caller must composite as
 * usual -- which is also the answer when the rectangle has moved, when the
 * last offer said not to keep it, and when the allocation failed.
 *
 * The rectangle is the full stage width, as the band always is (main.c), so
 * only y and h are passed.
 */
int  mr_band_cache_try(mr_surface *stage, int y, int h);

/* The stage has just been composited up to the seam: take a copy of the
 * strip, or throw away what is held.
 *
 * `keep` is the caller's judgement that what is on the stage now will still be
 * what belongs under the next line's glyphs -- nothing animating, no
 * transition in flight, no screen painting into the strip. keep == 0 does not
 * merely decline to store: it *invalidates*, because the reason not to store
 * is always that the strip in hand has gone stale.
 *
 * Offering a rectangle of a different size reallocates. Offering one that does
 * not fit on the stage invalidates and stores nothing.
 */
void mr_band_cache_offer(const mr_surface *stage, int y, int h, int keep);

/* Release the buffer outright. For shutdown, and for the one change `keep`
 * cannot see: mr_ui_sync_geometry can move the strip or repoint the box art
 * without the frame loop having any reason to think the scene moved. */
void mr_band_cache_drop(void);

/* How many bytes are held, for memreport. 0 when nothing is allocated -- the
 * buffer is taken on the first offer, not at startup, so a game that never
 * reaches a line of dialogue never pays for it. */
long mr_band_cache_bytes(void);

#endif
