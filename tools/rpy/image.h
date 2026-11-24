#ifndef MR_IMAGE_H
#define MR_IMAGE_H

#include "mr.h"
#include "script.h"     /* mr_anim: an animated image definition */

/* PNG decoding (png.c) */

/* Decode a PNG file into a freshly allocated 32bpp surface, or NULL on
 * failure (the reason goes to the log). The caller owns the surface. */
mr_surface *mr_png_load(const char *path);

/* Image registry and cache (image.c)
 *
 * The game refers to images by the multi-word names used in `image` and `show`
 * statements ("miwa biwa talk"). Those map to a file, optionally horizontally
 * flipped, exactly as im.Flip() does in the original scripts.
 *
 * Decoded pixels are held in a bounded LRU cache. On a cache miss the file is
 * re-decoded, which MR_PROF measures at about 190ms per image on the real
 * Pentium III -- not the "few tens of milliseconds" this comment claimed
 * before anyone timed it. Holding all 121 images (about 190MB decoded) is
 * still out of the question, so the budget stands, but a decode is an
 * expensive event and worth avoiding rather than shrugging at.
 *
 * The budget is not being thrashed: a background is 3.5MB and the sprites
 * 0.3-0.9MB, so a scene and the one it is dissolving from both fit inside
 * 12MB. The decodes seen in a profile are genuine first loads.
 */

#define MR_CACHE_BUDGET (12L * 1024L * 1024L)

void mr_image_init(const char *game_dir);
void mr_image_shutdown(void);

/* Declare name -> file, as `image <name> = "<file>"` does. flip mirrors the
 * image horizontally at decode time. Returns 0 if the table is full. */
int mr_image_define(const char *name, const char *file, int flip);

/* As mr_image_define, plus the im.Blur radii (0 for no blur) and
 * im.FactorScale's factors (0 for no scaling -- see image.c, zero rather than
 * one is the no-op on purpose) to apply to the decoded pixels. The
 * manipulators run in the order Ren'Py nests them in the scripts: the flip
 * first, then the blur, then the scale. */
int mr_image_define_manip(const char *name, const char *file, int flip,
                          float xrad, float yrad, float xscale, float yscale);

/* Declare an animated name: `image fireworks animated:` and its block.
 *
 * It goes here, rather than on the display list entry that shows it, because
 * an animated image is part of what a *name* means -- the same reasoning that
 * puts im.Flip and im.Blur here. The payoff is that it works everywhere a
 * name is used: `show`, a `scene` background, a screen's `add`, a side image.
 * Putting the state on mr_shown would have covered `show` alone and needed
 * doing again for each of the others.
 *
 * The price is one divergence from Ren'Py, which is worth knowing before
 * chasing it as a bug: the phase comes from a single clock
 * (mr_image_set_time), so two simultaneous shows of one animated image are in
 * step, where Ren'Py gives each displayable its own `st` counting from when it
 * was shown. For a repeating ambient loop -- which is what animations in these
 * scripts are -- nothing can tell. A non-repeating one-shot would be the case
 * where it shows, and none exists yet.
 *
 * The same property is why none of this is in a save: the phase is derived
 * from the clock, not stored, and a load restarts the animation. The save
 * format is untouched.
 *
 * That part *is* a divergence, and the note used to claim the opposite: Ren'Py
 * pickles displayable animation state. `st`, `st_offset` and `atl_state` are
 * ordinary attributes of the transform (transform.py:811, atl.py:386) and go
 * into the save with everything else, which is why a save taken during a
 * `show x: linear 90 ...` resumes the roll in Ren'Py rather than freezing it.
 * A transform block on the display list now does the same -- see
 * mr_scene_resume_anim -- but a registry animation still restarts, because
 * there is no per-show state to restore it into. For a repeating ambient loop,
 * which is what these all are, nothing can tell.
 *
 * The anim is copied; the caller keeps ownership of what it passes in. */
int mr_image_define_anim(const char *name, const mr_anim *anim);

/* Resolve a show name that carries only some of an image's attributes.
 *
 *     show azami pout        # azami wears "pout"
 *     show azami at left270  # still pout, just moved
 *
 * There is no `azami.png`. In Ren'Py an image name is a tag plus a *set* of
 * attributes, and a `show` that names fewer than the image has keeps the ones
 * the tag is already wearing (display/image.py:949 apply_attributes, :1004
 * choose_image). Azami does this 377 times in one playthrough; without it the
 * sprite simply vanishes, which is what it did.
 *
 * `current` is the full name the tag is showing now, or NULL if it is not on
 * screen -- that is where the attributes to keep come from. The chosen name
 * goes to `out`; the return is 0 when nothing matches, in which case the
 * caller should keep the name it had and let the miss be logged where it
 * would have been.
 *
 * Attributes are matched as a set, so `eari uniform flip talk` finds the
 * image declared as `eari uniform talk flip`. A leading `-` removes an
 * attribute rather than requiring it. An exact hit short-circuits, so a
 * `show` that names a real image never inherits anything -- which is how
 * `show azami pout` drops `casual`. */
int mr_image_choose(const char *name, const char *current,
                    char *out, int outsz);

/* The clock the animations run off. Set once per frame, before anything
 * renders, so that every mr_image_get in one frame -- including the ones
 * behind a transition snapshot -- agrees on which frame is showing. */
void mr_image_set_time(long now_ms);

/* True if this name is an animated one. */
int mr_image_is_anim(const char *name);

/* Whether this animated name would now resolve to something different from
 * what it last handed out -- and so whether the stage needs compositing
 * again. 0 for a name that is not animated.
 *
 * This, not mr_image_is_anim, is what the frame loop asks. An animation holds
 * each frame for seconds at a time and resolves to the identical cached
 * surface throughout; treating "is animated" as "is moving" made the loop
 * composite the full 1280x720 stage sixty-odd times to produce the same
 * image, which on the target is a third of a second apiece. Asking the
 * narrower question turns an eleven-second fireworks loop from roughly 330
 * full composites into about 15 -- and it lets the dialogue-band path stay
 * available while an animation is merely sitting there, which it could not
 * before.
 *
 * It reports changed until something has actually been drawn, so the first
 * frame is never skipped. */
int mr_image_anim_changed(const char *name);

/* Whether the game declares any animated image at all. A per-frame caller
 * asks this first so that a game without animations -- which is every game
 * but one so far -- behaves and costs exactly what it did before animations
 * existed, rather than paying a name hash per shown image for an answer that
 * is always no. */
int mr_image_any_anim(void);

/* Declare a name that resolves to a file by Ren'Py's own convention, i.e.
 * "miwa biwa talk" -> "images/miwa biwa talk.png". Used for every image that
 * the scripts show without an explicit `image` statement. */
void mr_image_autodefine_dir(void);

/* Marks the start of a render pass. Images fetched after this call are
 * pinned until the next one, so a surface obtained earlier in the pass cannot
 * be evicted and freed by a later fetch of a different image. Without this,
 * holding a surface pointer across another mr_image_get is a use-after-free,
 * which is easy to write by accident and shows up as a page fault on a small
 * address when the freed memory is read back as a pointer. */
void mr_image_begin_frame(void);

/* Look up and decode, going through the cache. Returns NULL if the name is
 * unknown or the file will not decode. The returned surface belongs to the
 * cache; it is guaranteed valid for the rest of the current frame (see
 * mr_image_begin_frame) but must not be held across frames. */
mr_surface *mr_image_get(const char *name);

/* As mr_image_get, but resampled to w x h and optionally mirrored -- what a
 * Transform's xzoom/yzoom asks for. The copy is cached (keyed by name, size
 * and mirroring, so a parked zoom resamples once rather than once per frame)
 * and belongs to the cache under exactly the same rule as mr_image_get's
 * return: good for the rest of the current render pass, never held across one.
 *
 * Returns the unzoomed image rather than NULL if the resample could not
 * allocate, on the grounds that a sprite at the wrong size is a better
 * failure than no sprite. */
mr_surface *mr_image_get_zoom(const char *name, int w, int h,
                              int flipx, int flipy);

/* MR_PROF: milliseconds spent decoding PNGs, and how many were decoded,
 * since the last call. Reading them clears them. A decode lands inside
 * whichever phase first asked for the image, so it is counted on its own. */
void mr_image_profile_enable(int on);
void mr_image_profile(long *ms, int *loads);

/* True if the name resolves to something we can show. */
int mr_image_exists(const char *name);

/* Drop cached pixels without forgetting the name table.
 *
 * Evicts unconditionally, including entries pinned by the current render pass
 * (see mr_image_begin_frame), so it must not be called from inside one. That
 * was harmless while the only caller was mr_image_shutdown; a caller that
 * flushes on a save load runs from an event handler, which is between passes,
 * but the rule is now load-bearing rather than theoretical. */
void mr_image_flush(void);

/* What the caches are actually holding, for memreport.
 *
 * The budgets are ceilings, not occupancy, and the difference is the whole
 * question when asking whether a flush would save anything: under the budget
 * the LRU never runs, so pixels from a context the player has left stay
 * resident instead of being evicted. Only a measurement distinguishes the
 * two, hence this.
 *
 * `blend_bytes` is the cross-fade scratch, which is not in the LRU and not
 * counted against the budget -- it is a per-image working buffer, not cached
 * pixels -- but it is decoded-image-sized memory that mr_image_flush frees,
 * so a report that left it out would understate the saving. */
typedef struct mr_image_stats {
    long pixels_bytes;      /* in the LRU */
    int  pixels_entries;
    long pixels_budget;

    long zoom_bytes;        /* resampled copies, a budget of their own */
    int  zoom_entries;
    int  zoom_slots;
    long zoom_budget;

    long blend_bytes;       /* cross-fade scratch, uncounted by either */
    int  blend_entries;
} mr_image_stats;

void mr_image_stats_get(mr_image_stats *st);

#endif /* MR_IMAGE_H */
