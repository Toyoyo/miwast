/* scene.h - the display list and the transitions between display lists.
 *
 * Ren'Py's model: `scene` clears everything and sets a background, `show`
 * adds or replaces an image by its *tag* (the first word of its name, so
 * `show miwa biwa talk` replaces `miwa biwa`), and `hide` removes by tag.
 * A transition then animates from the previous display list to the current
 * one.
 */

#ifndef MR_SCENE_H
#define MR_SCENE_H

#include "mr.h"
#include "script.h"

/* How many images can be on the display list at once. Left at 12 on purpose,
 * and the cost is the reason: an mr_shown is 408 bytes and there are ~102
 * scenelists live (g_cur, g_prev, and MR_ROLLBACK_MAX checkpoints), so each
 * extra slot costs about 42KB. Overflow is survivable rather than fatal --
 * the rest are dropped and mr_scene_show_anim logs "too many images shown".
 *
 * The evidence for 12: it covers Miwa and Hinata with room to spare, and the
 * worst case found in any script so far is Azami's summoning scene, which
 * wants 16. See the section in NOTES.md before changing it. */
#define MR_MAX_SHOWN 12

typedef struct mr_shown {
    char     name[MR_MAX_NAME];
    char     tag[MR_MAX_NAME];
    mr_place place;

    /* Did the statement that put this entry here say anything about where it
     * goes? Ren'Py applies config.default_transform -- bottom centre -- only
     * to a show that is not already a Transform (scenelists.py:450), so a
     * bare `show kalea` stands on the bottom while `show kalea: yoffset 40`
     * leaves x at 0. mr_place_resolve takes the distinction as an argument;
     * this is where the answer has to survive until the entry is drawn.
     *
     * It was missing, and render_list passed a literal 1, so every sprite in
     * every game was placed as though it carried a transform. Both shipped
     * games hid it by never once using a bare `show`. */
    int      has_place;

    /* Animation, when the show statement carried a transform block with timed
     * steps. The mr_atl itself belongs to the statement and outlives us. */
    const mr_atl *atl;
    mr_place      atl_base;    /* placement the block started from */
    mr_place      step_from;   /* placement the current step started from */
    int           step;
    long          step_start;
} mr_shown;

/* Room for a story screen (a title card), a menu screen, a confirm dialog
 * over it, and the quick menu overlay, with slack. It was 4 before the
 * overlay existed. */
#define MR_MAX_SCREENS 8

typedef struct mr_scenelist {
    char     bg[MR_MAX_NAME];   /* empty string means no background */
    mr_shown v[MR_MAX_SHOWN];
    int      n;

    /* Screens shown over the scene. They live in the display list rather
     * than in a separate overlay so that a `show screen title with fade`
     * fades the title in along with everything else: the transition
     * snapshots then differ by the screen, as they should. */
    char screens[MR_MAX_SCREENS][MR_MAX_NAME];
    /* 1 if the script put it there with `show screen`, 0 if it belongs to
     * the interface -- the game menu, a confirm dialog. Only the first kind
     * is part of the story, and only the first kind belongs in a save: a
     * chapter title card has to come back on load, the save menu must not. */
    int  screen_story[MR_MAX_SCREENS];
    /* The screen's `zorder`. Screens are drawn in ascending order of it, so
     * `screen quick_menu(): zorder 100` sits over a chapter title card
     * whichever was shown first. Everything else leaves it at 0. */
    int  screen_z[MR_MAX_SCREENS];
    int  nscreens;
} mr_scenelist;

int  mr_scene_init(void);
void mr_scene_shutdown(void);

/* Display list edits. These change the pending list; the change becomes
 * visible at the next render, animated if a transition has been started. */
void mr_scene_scene(const char *bg);
void mr_scene_show(const char *name, const mr_place *place, int has_place);

/* As mr_scene_show, but the transform block had timed steps to animate, and
 * `behind` carries the statement's `behind a,b` list (NULL when it had none).
 * The list decides where the image lands in the z-order: Ren'Py inserts at
 * the first entry whose tag is this one or one of those (scenelists.py:313),
 * and render_list draws in list order, so being earlier is being behind. */
void mr_scene_show_anim(const char *name, const mr_place *place, int has_place,
                        const mr_atl *atl, long now_ms, const char *behind);
void mr_scene_hide(const char *tag);
void mr_scene_clear(void);

/* An interface screen: the game menu, a modal dialog. Not saved. */
void mr_scene_show_screen(const char *name);

/* The script's `show screen`. Part of the story, and so part of a save. */
void mr_scene_show_story_screen(const char *name);

/* The screen's zorder, for the draw order. Set once when it is shown; the
 * interpreter reads it off the screen definition. */
void mr_scene_screen_set_zorder(const char *name, int z);

void mr_scene_hide_screen(const char *name);

/* How many screens are on the scene right now. A screen can repaint anywhere
 * on the stage, so a partial frame is only safe when there are none. */
int  mr_scene_screen_count(void);

/* How many of those are drawn *with the scene* -- zorder <= 0, the ones
 * draw_screens paints and mr_scene_render_overlay does not. The band cache
 * asks because it stands in for that drawing: a screen under the window has
 * its pixels in the cached strip, so replaying it would blend it over itself,
 * and skipping it would drop its entries from the focus list that the next
 * click reads. Neither is acceptable, so a strip is only kept while this is
 * zero -- which during ordinary dialogue it is, the quick menu being
 * `zorder 100` and therefore an overlay. See bandcache.h. */
int  mr_scene_screen_count_under_window(void);

/* The name of the i'th screen on the scene, or NULL. Lets a caller ask
 * *which* screens are up rather than only how many. */
const char *mr_scene_screen_name(int i);

/* Installed by the UI layer, which owns the fonts needed to draw a screen. */
typedef void (*mr_screen_draw_fn)(mr_surface *dst, const char *name,
                                  int interactive);
void mr_scene_set_screen_drawer(mr_screen_draw_fn fn);

/* Start animating from the list as it was at the last mr_scene_commit to the
 * list as it is now. */
void mr_scene_transition(const mr_transition *t, long now_ms);

/* Accept the current list as the new baseline without animating. */
void mr_scene_commit(void);

/* Copy the display list out for a save or a rollback checkpoint, and put one
 * back. Restoring makes the list both current and visible, with no
 * transition in flight. */
void mr_scene_capture(mr_scenelist *out);
void mr_scene_restore(const mr_scenelist *in);

/* --- putting a loaded save's animations back on the clock ---------------- */

/* Somewhere stable to keep an ATL block that came out of a file.
 *
 * In memory an animating entry's `atl` points into the script's statement
 * array, which outlives the display list. A block read from a save has no
 * such owner, so it is copied here -- one slot per display list entry -- and
 * the entry points at the copy. Returns that pointer; never NULL.
 *
 * The copy, not the script's statement: see mr_put_atl for why a save carries
 * the block by value rather than naming the statement that owns it.
 */
const mr_atl *mr_scene_park_atl(int slot, const mr_atl *a);

/* Re-base the restored list's animations onto the current clock.
 *
 * A capture records `step_start` as an absolute timestamp, which is what a
 * rollback checkpoint wants -- it is restored in the same process, with the
 * clock well past the end of the step, so the animation finishes on the first
 * frame and the sprite is simply where it ended up. A file cannot mean that:
 * the timestamps belong to a process that has exited. So a save stores the ms
 * *elapsed* into the current step, and this adds the clock back.
 *
 * The effect is that loading resumes the animation from where it was saved
 * and runs it to the end, which is what Ren'Py does: `st`, `st_offset` and
 * `atl_state` are ordinary attributes of the ATLTransform and go into the
 * pickle (transform.py:811, atl.py:386), and the st_offset clamp at
 * transform.py:965 keeps `st` from going backwards when the restored
 * displayable is rendered against a fresh show_time.
 */
void mr_scene_resume_anim(long now_ms);

/* True while a transition is still running. Note this one *ends* a
 * transition whose time is up, so it is a step as well as a question. */
int mr_scene_busy(long now_ms);

/* Whether a transition is in flight, asked without changing anything. */
int mr_scene_in_transition(void);

/* Whether that transition is one of the move family, which composites by
 * re-rendering both lists rather than by blending two snapshots. */
int mr_scene_move_transition(void);

/* True while anything on the scene is still moving: a transition, or a
 * transform block that has not finished. */
int mr_scene_animating(long now_ms);

/* Finish any running transition immediately, as a click does. */
void mr_scene_skip(void);

/* Composite the current state onto the stage. Screens at zorder <= 0 are
 * included; higher ones are not. */
void mr_scene_render(mr_surface *stage, long now_ms);

/* The screens above zorder 0, drawn over everything including the dialogue
 * window. Call after the text layer. */
void mr_scene_render_overlay(mr_surface *stage);

/* Resolve a placement to top-left stage coordinates for an image of this
 * size. Exposed so the text layer can position things the same way. */
void mr_place_resolve(const mr_place *pl, int has_place,
                      int img_w, int img_h, int *x, int *y);

#endif /* MR_SCENE_H */
