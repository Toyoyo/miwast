/* rhythm.h - the rhythm minigame.
 *
 * `call screen rhythm_game(audio, beatmap, background, beatmap_stride=2)` is
 * the only screen in this game that is not a layout: it is a Ren'Py
 * Displayable with a render() and an event() of its own
 * (game/00-renpythm/rhythm_game_displayable.rpy). Rather than teach the
 * screen language to host one, the interpreter hands the display to this
 * module for the duration and takes it back with a result.
 *
 * That .rpy is itself documented as a port *back* from a C implementation for
 * Win32s, so this is the third telling of the same design. Where the two
 * disagree the .rpy wins, because it is what the game ships and what the
 * player has played: the per-lane hit sounds, the PERFECT/GREAT windows, the
 * health bar, the game-over and the end-of-song summary are all from it, and
 * none of them existed in the older C version.
 *
 * Timing does not come from the frame rate. Notes are placed, and taps are
 * judged, against where the sound card says it actually is in the track (see
 * mr_audio_output_position_ms). A Pentium III compositing 1280x720 will not
 * hold 30fps here, and it does not have to: a slow frame makes the notes move
 * in bigger steps, it does not make them land in the wrong place or turn a
 * PERFECT into a GREAT.
 */

#ifndef MR_RHYTHM_H
#define MR_RHYTHM_H

#include "mr.h"

/* Begin a run. Paths are as the script writes them, relative to the game
 * directory: "audio/foo.mp3", "audio/foo.beatmap.txt", "images/bar.png".
 * `stride` keeps every Nth onset, as the screen's beatmap_stride does; 0 or
 * less means the default of 2. Returns 0 if the beatmap will not load, in
 * which case nothing is started and the caller should carry on. */
int  mr_rhythm_start(const char *audio, const char *beatmap,
                     const char *background, int stride);

/* Is a run in progress -- including the end-of-song summary, which is still
 * the minigame's display even though the song is over? */
int  mr_rhythm_active(void);

/* Advance the clock: spawn the music when the countdown ends, retire notes
 * that went past unhit, notice the end of the song. */
void mr_rhythm_update(long now_ms);

/* Draw the whole stage: background, tracks, notes, HUD, summary. */
void mr_rhythm_render(mr_surface *stage, long now_ms);

/* Input. Both return 1 if the event was consumed, so the frame loop knows not
 * to also treat it as advancing the story. `key` is an MR_KEY_* from keys.h
 * -- it used to be a Windows virtual key, which is how windows.h ended up
 * being a dependency of the minigame; click coordinates are in stage space. */
int  mr_rhythm_key(int key);
int  mr_rhythm_click(int x, int y);

/* Pointer motion, so the give-up dialog's buttons light on hover like every
 * other button. Returns 1 when the dialog is up and has taken the motion, in
 * which case the caller must not also run the screen layer's hover -- there
 * is no focus entry behind this dialog, it is drawn natively. Invalidates by
 * itself when the highlighted button changes. */
int  mr_rhythm_hover(int x, int y);

/* True once the player has dismissed the summary (or given up): the caller
 * should collect the result and return the display. */
int  mr_rhythm_finished(void);

/* (hits, notes, score), as the screen returns to the script. A game over
 * reports zeroes for hits and score so a lost run is distinguishable from a
 * finished one, exactly as RhythmGameDisplayable.result() does. */
void mr_rhythm_result(long *hits, long *notes, long *score);

/* Abandon a run and release everything. Safe when nothing is running. */
void mr_rhythm_stop(void);

/* The rest of the run's state, for the HUD and for the tests. Any pointer may
 * be NULL. `over` is set once the health has run out. */
void mr_rhythm_stats(long *combo, int *health, long *perfect, long *great,
                     int *over);

/* True once the song has finished and the summary is up. */
int  mr_rhythm_ended(void);

/* True while the give-up dialog is up and the song clock is held. */
int  mr_rhythm_paused(void);

/* Where the run is in the song, in milliseconds; negative during the lead-in.
 * This is what the notes are placed against, and it must never run backwards
 * -- a jump here is a screenful of notes moving the wrong way. */
long mr_rhythm_song_ms(void);

/* The high-score table, keyed by beatmap path. These back the two script
 * functions the extras menu calls:
 *
 *     rhythm_game_get_highscore(beatmap)   -> 0 for a song never played
 *     rhythm_game_set_highscore(beatmap, score) -> True if it is a record
 *
 * which is the contract screens.rpy builds its table from. The scores live in
 * the persistent store, one variable per song; see the note in rhythm.c for
 * why that rather than the dict Ren'Py keeps. `set` only writes when the
 * score is an improvement, and says so. */
long mr_rhythm_highscore_get(const char *beatmap_path);
int  mr_rhythm_highscore_set(const char *beatmap_path, long score);

#endif /* MR_RHYTHM_H */
