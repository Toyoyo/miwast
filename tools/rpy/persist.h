/* persist.h - data that outlives a playthrough.
 *
 * Ren'Py keeps one `persistent` object per game, pickled to a file beside the
 * saves, holding two unrelated things:
 *
 *   - `persistent.<name>` attributes the script sets, which this game uses to
 *     unlock extras (`persistent.watashi`, `persistent.ending`);
 *   - `persistent._preferences`, the Preferences object: text speed, volumes,
 *     the selected language.
 *
 * Both already live in the ordinary variable store under those exact names, so
 * persisting them is a matter of writing out the variables whose names begin
 * with `persistent.` or `_preferences.` and reading them back at startup. No
 * separate object, and a new preference costs nothing here.
 *
 * Ren'Py writes the file when the script calls renpy.save_persistent() and
 * when the game quits, and not otherwise in a single-process run: its idle
 * path only re-reads the file in case another copy of the game changed it.
 * The same two moments are what this does.
 *
 * Unset attributes read as None in Ren'Py, which is falsey, and the variable
 * store already behaves that way -- so `if persistent.watashi` on a fresh
 * install is simply false, with no file and no initialisation.
 */

#ifndef MR_PERSIST_H
#define MR_PERSIST_H

/* Read the file into the variable store. Missing or unreadable is not an
 * error: it is what a first run looks like. Returns the number of values
 * restored. */
int mr_persist_load(void);

/* Write it. Called by renpy.save_persistent() and at shutdown. */
int mr_persist_save(void);

/* Ren'Py's persistent._clear(): forget every `persistent.<name>` the script
 * set, while keeping the underscore-prefixed fields -- `_preferences` above
 * all, so resetting progress does not also reset the volumes. `progress`
 * additionally clears the seen-text set below. */
void mr_persist_clear(int progress);

/* --- seen text ---
 *
 * Which lines of dialogue the player has already read, which is what the
 * "Texte non lu" preference is about: with it off, skipping stops the moment
 * it reaches something new.
 *
 * Ren'Py keeps a set of translation identifiers in
 * `persistent._seen_translates` and asks it through seen_translation(tlid)
 * (behavior.py:825). We already compute the same identifier for every say
 * statement, so the key is free -- but a set of 2800 strings is not, so what
 * is stored is a sorted array of 32-bit hashes of it. That is 11KB rather
 * than 56KB, at the cost of a roughly 1-in-1000 chance across a whole script
 * that two lines collide and one is treated as read before it has been. The
 * consequence of a collision is that skipping runs past one line, which is
 * not worth 45KB on this machine.
 *
 * Marking is cheap and happens on every line; the array is kept sorted so
 * the test is a binary search. */
void mr_persist_seen_mark(const char *tlid);
int  mr_persist_seen(const char *tlid);

/* How many lines have been seen -- for the harness, and for a log line. */
int  mr_persist_seen_count(void);

/* Hold the persistent values aside and put them back. Loading a save resets
 * every variable to the init-time baseline, which is right for story state
 * and wrong for this: the volumes you set this session, and an extra you
 * unlocked since the slot was written, must both survive the load. */
int  mr_persist_snapshot(void);
void mr_persist_restore(void);

/* Whether a variable belongs to the persistent data rather than to a
 * playthrough. Save slots use this to leave it out: loading a save from
 * before you turned the music down must not turn it back up, and must not
 * re-lock an extra you have since unlocked. */
int mr_persist_is_persistent(const char *name);

#endif /* MR_PERSIST_H */
