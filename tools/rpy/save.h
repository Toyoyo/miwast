/* save.h - save slots, the game state they hold, and the rollback log.
 *
 * Slots are named the way Ren'Py names them -- "1-1" through "9-6" for the
 * numbered pages, plus "auto-1" and "quick-1" -- but the files cannot be,
 * because the target filesystem is FAT with 8.3 names. A slot maps to
 * SAVEP<page>S<slot>.SAV, SAVEA<n>.SAV or SAVEQ<n>.SAV, all of which fit.
 *
 * Saving and rolling back need the same thing: a description of where the
 * story is. mr_gamestate is that description, and it is deliberately a plain
 * struct of fixed-size fields so it can be written to disk as it stands.
 */

#ifndef MR_SAVE_H
#define MR_SAVE_H

#include "mr.h"
#include "script.h"
#include "scene.h"

#define MR_SAVE_STACK   32
#define MR_SLOT_NAME    16

typedef struct mr_gamestate {
    long pc;
    long sp;
    long stack[MR_SAVE_STACK];

    char who[MR_MAX_NAME];
    char what[MR_MAX_TEXT];
    /* The line's translation identifier: `what` is the original, so without
     * this a loaded save would show the line untranslated until the next one.
     * See tl.h. */
    char tlid[MR_MAX_NAME + 24];
    int  has_dialogue;
    int  window_shown;

    mr_scenelist scene;

    char music[128];
    int  music_loop;

    /* What the story was waiting for. Without this a save taken during
     * `$ renpy.pause()` -- which is what a chapter title card sits on --
     * came back as "not waiting for anything" and ran straight on into the
     * next scene. Written in a trailing section so older saves, which have
     * none, still load and read as MR_WAIT_NONE. */
    int  wait_kind;     /* MR_WAIT_* */
    long wait_ms;       /* time left, for a timed pause */
    int  hard_wait;     /* a pause that cannot be clicked through */

    /* The clock the capture was taken on, so a writer can turn each animating
     * entry's absolute `step_start` into the ms elapsed into its step. Not
     * written to the file -- it is only meaningful inside the process that
     * set it. */
    long now;

    /* 1 when the `step_start` of every animating entry in `scene` holds ms
     * elapsed into the step rather than an absolute timestamp, which is how a
     * state read from a file carries it. mr_interp_restore puts the clock
     * back; see mr_scene_resume_anim. A capture leaves this 0. */
    int  anim_relative;
} mr_gamestate;

#define MR_WAIT_NONE  0
#define MR_WAIT_TIMED 1
#define MR_WAIT_CLICK 2

/* --- the slot layer --- */

int  mr_save_exists(const char *slot);
int  mr_save_write(const char *slot, const mr_surface *stage);
int  mr_save_load(const char *slot);
void mr_save_delete(const char *slot);

/* Rotate the first `count` slots of a page, which is what makes a quick save
 * keep a history instead of overwriting one slot.
 *
 * Ren'Py's cycle_saves (loadsave.py:707): slot `count-1` is renamed to
 * `count`, then `count-2` to `count-1`, down to 1 becoming 2 -- so the oldest
 * falls off the end and slot 1 is left free for the caller to write. A slot
 * that does not exist is skipped, as rename_save does (loadsave.py:674). */
void mr_save_cycle(const char *page, int count);

/* Metadata for the save screen. Both return NULL when the slot is empty; the
 * strings point at an internal buffer valid until the next call. */
/* The preformatted stamp a version 5-7 save carries, or NULL. New saves have
 * one too, but the screen uses the epoch below so that the format the game
 * asks for is honoured and the date follows the current language. */
const char *mr_save_time(const char *slot);

/* When the slot was written, as a local-time epoch; 0 for a save older than
 * version 8, which did not record one. */
long mr_save_time_t(const char *slot);
const char *mr_save_name(const char *slot);

/* The slot's thumbnail, decoded and cached, or NULL. */
mr_surface *mr_save_thumbnail(const char *slot);

/* Remember the scene as it looks now, for the next save's thumbnail. Called
 * on entry to the game menu, because by the time a slot is clicked the stage
 * shows the menu rather than the story. */
void mr_save_screenshot(const mr_surface *stage);

/* Forget everything cached about the slots, after a save or a delete. */
void mr_save_flush(void);

/* Live bytes in the slot cache's thumbnails, how many there are, and the
 * retained screenshot. For memreport; see mr_image_stats_get. */
void mr_save_cache_stats(long *bytes, int *entries, long *shot_bytes);

void mr_save_shutdown(void);

#endif /* MR_SAVE_H */
