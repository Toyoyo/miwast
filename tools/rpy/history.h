/* history.h - the text history and the rollback log.
 *
 * Both are records of what has happened, written at the same moment (each
 * line of dialogue), so they live together.
 *
 * Ren'Py keeps config.rollback_length = 128 checkpoints and lets the player
 * move back config.hard_rollback_limit = 100 of them, and it stores the log
 * as *deltas* -- the previous values of whatever each step changed -- rather
 * than as whole-state snapshots. That is what makes 100 steps affordable, and
 * it is what is done here: a checkpoint is an execution point plus a display
 * list plus the list of variables the step was about to overwrite.
 *
 * config.history_length = 250 in this game, which is what the history screen
 * scrolls through.
 */

#ifndef MR_HISTORY_H
#define MR_HISTORY_H

#include "mr.h"
#include "script.h"
#include "save.h"
#include "expr.h"
#include <stdio.h>

/* --- text history --- */

#define MR_HISTORY_MAX  250
/* A history entry is a replay of a line of dialogue, so it has to be able to
 * hold one: the same MR_MAX_TEXT the script and g_what use. It was 384, below
 * both that and the old value cap, which meant a long line displayed in full
 * and came back clipped in the history screen alone. The table is 250 of
 * these (268KB) and the save writes them length-prefixed, so the cost of the
 * headroom is RAM only. */
#define MR_HISTORY_TEXT MR_MAX_TEXT

void mr_history_reset(void);
void mr_history_add(const char *who, const char *what, mr_u32 color);

/* Drop the newest `n` entries, which is what a rollback does. */
void mr_history_pop(int n);

/* Entries newest first, which is the order the history screen wants. */
int  mr_history_count(void);
int  mr_history_get(int i, const char **who, const char **what, mr_u32 *color);

/* --- rollback --- */

void mr_rollback_reset(void);

/* Trace every rollback and rollforward to the log: one line each, naming the
 * statement, the deltas being undone, the point being stepped over and the
 * forward count. Off by default, because that is a line per keypress.
 *
 * `MR_ROLLTRACE` in main.c, alongside MR_STATS and MR_PROF. It exists
 * because a report of a title card drawing the wrong text could not be
 * reproduced in any harness, and these lines identified the faulty step from
 * one play session's log.txt -- a rollback with no point recorded for the way
 * forward, followed by forward steps that restored nothing.
 *
 * The *anomaly* that was is now detected without this being on; see the
 * fwd_captured check in mr_rollforward. This is for reading the shape of a
 * sequence, not for catching that one bug again. */
void mr_rollback_trace(int on);

/* What kind of statement a checkpoint was taken at.
 *
 * Not decoration: a say writes a text-history entry and a menu does not, and
 * rolling back pops as many history entries as it drops checkpoints. While
 * every checkpoint was a say those two were the same number and the log could
 * assume it. MR_CP_SAY is 0 so that a zeroed point is a say, which is what
 * every point in an older save is. */
typedef enum {
    MR_CP_SAY = 0,
    MR_CP_MENU,
    /* A menu with a caption. It is a menu for every other purpose, but its
     * caption is a line of dialogue -- Ren'Py emits a real say node for it --
     * so it *does* write a history entry and rolling back over it must pop
     * one. Distinguished from MR_CP_MENU rather than reported as a say
     * because the answer replay has to keep treating it as a menu. */
    MR_CP_MENU_CAPTION,
    /* An untimed pause -- `pause` with no delay, or `renpy.pause()`. Ren'Py
     * checkpoints it and not the timed kind: statementexports.py:151-154
     * defaults `checkpoint` to False when a delay was given and True when it
     * was not, and the call at :191 is a *hard* checkpoint for an untimed
     * pause (`hard=config.pause_after_rollback or (delay is None)`).
     *
     * This is what makes a chapter title card -- `show screen title` then
     * `$ renpy.pause()` -- somewhere the player can come back to. Without it
     * the card was the one thing in the script that rollback could not reach:
     * saving and loading on it worked, but rolling back from the line after
     * it crossed it without stopping. Reported from play.
     *
     * Writes no history entry, like a bare menu: a pause is not a line of
     * dialogue. New in this version of the kind field rather than a new save
     * format; a slot written before it has no point of this kind, and an
     * older binary reading a newer slot clamps it to MR_CP_SAY, which costs
     * one spurious history pop and nothing else. */
    MR_CP_PAUSE
} mr_cp_kind;

/* Note a checkpoint: somewhere the player can come back to. `pc` is the
 * statement to resume at, which for dialogue is the say statement itself --
 * re-running it redisplays the line without any other effect, and for a menu
 * is the `menu` statement, whose re-run offers it again. */
void mr_rollback_checkpoint(const mr_gamestate *g, long pc, mr_cp_kind kind);

/* About to resume at `pc`, which will re-execute and take its own checkpoint.
 * If the newest point in the log is already that statement, rewind so the
 * re-run reuses its slot rather than appending a second copy of it.
 *
 * In-session rollback gets this for free: it rewinds the head itself and the
 * re-run overwrites the target's slot. A log read back from a save does not
 * -- the head sits just past the newest point -- so a load that re-executes
 * its statement grew a duplicate. A menu is the one statement that does
 * re-execute (mr_interp_restore resumes it as ST_RUN so the options come
 * back), and the duplicate was visible immediately: mr_rollback_back steps
 * over two points when the newest one is what is on screen, so the first
 * Page Up after loading at a menu landed on the *other* copy of that same
 * menu and appeared to do nothing. A second press then worked. Reported from
 * play. */
void mr_rollback_resume_at(long pc);


/* How many steps back are available. */
int  mr_rollback_depth(void);

/* Step back one checkpoint, restoring the variables the steps since then
 * changed. Returns 0 when there is nowhere to go.
 *
 * `here` is the statement the player is sitting on. The log compares it with
 * the newest checkpoint to work out whether that checkpoint is what is on
 * screen -- a line of dialogue, or a menu -- in which case going back has to
 * step over two points, or whether they are somewhere later, such as a pause
 * under a title card, where it steps over one.
 *
 * This was a boolean the caller computed as `g_state == ST_SAY`, and making a
 * menu a checkpoint silently falsified it: at a menu the newest point *was*
 * the thing on screen, the caller said otherwise, and rolling back landed on
 * the menu it was already showing, so the first Page Up did nothing and the
 * player had to press again. Reported from play. Comparing against the log
 * cannot go stale the same way -- any statement that becomes a checkpoint
 * later is handled without touching this. */
int  mr_rollback_back(mr_gamestate *out, long *pc, long here);

/* --- rollforward ---
 *
 * Ren'Py's `rollforward`, bound to PageDown and the wheel down
 * (00keymap.rpy:66): having rolled back, go forward again. Zero steps are
 * available until a rollback happens, and a rollback followed by a *different*
 * choice clears them -- so at the newest point these do nothing, which is
 * what they do in Ren'Py too.
 */
int  mr_rollforward_depth(void);

/* --- choice menus ---
 *
 * A menu is a checkpoint like a line of dialogue, and its checkpoint carries
 * the option the player took: `renpy.exports.checkpoint(rv)` at the end of
 * display_menu (menuexports.py:432). That one piece of recorded data is what
 * makes a menu participate in both directions -- rolling back lands on the
 * menu and offers it again, and rolling forward answers it from the record
 * instead of dead-ending there.
 *
 * Record the answer on the newest point, which is the menu's own checkpoint.
 * `mr_rollback_answer_here` reads it back, and returns -1 when the newest
 * point is not an answered menu.
 */
void mr_rollback_answer(int n);
int  mr_rollback_answer_here(void);

/* Ren'Py keeps the future across a checkpoint only when three things hold
 * (rollback.py:781-790):
 *
 *     self.current.context.current == fwd.name    same statement
 *     and data == fwd.data                        same answer
 *     and (keep_rollback or self.rolled_forward)  arrived by rolling forward
 *
 * The first is the pc comparison in mr_rollback_checkpoint. The second is why
 * answers are recorded at all. The third is *not* implemented, and see the
 * note at that comparison for why: it asks how the player left a statement,
 * and our checkpoints are taken when a statement is displayed rather than
 * when it is dismissed, so there is nothing to ask yet.
 */

/* Throw the forward steps away without touching the backward log.
 *
 * For a decision the player has just re-made. The ordinary rule -- a
 * checkpoint whose pc differs from the point it lands on is a branch -- does
 * not catch a *converging* menu, which is the shape almost every visual
 * novel menu has: branch for a line, set a flag, rejoin. After the join the
 * statement is the same one on either path, so the pc matches and the future
 * looks valid while being from the branch the player just abandoned. */
void mr_rollforward_clear(void);

/* Step forward one checkpoint, re-applying the variables that step changed.
 * `out` and `pc` are filled exactly as mr_rollback_back fills them, so the
 * caller installs them the same way -- including the music, which is part of
 * the checkpoint and has to be reapplied or a rolled-forward scene plays the
 * wrong track. Returns 0 when there is nowhere to go. */
int  mr_rollforward(mr_gamestate *out, long *pc);

/* Record a variable about to be overwritten. Installed on the variable store
 * as a journal; see mr_expr_set_journal. */
void mr_rollback_journal(const char *name, const mr_value *old, int existed);

/* Stop recording, for renpy.block_rollback(). */
void mr_rollback_block(void);

/* The log is part of a save: that is what makes rollback work after a load.
 * Only the most recent checkpoints are written; see the note in history.c.
 *
 * `version` is the save's own version, because the checkpoint record grew a
 * field in version 9 -- the mr_cp_kind above. Passed in rather than read from
 * a global so that the one function that knows the layout also knows which
 * layout it is looking at. Anything below 9 has no kind and reads as
 * MR_CP_SAY, which is what those saves effectively claimed anyway. */
void mr_history_write(FILE *f);
void mr_history_read(FILE *f, int version);

void mr_history_shutdown(void);

#endif /* MR_HISTORY_H */
