/* movie.h - cutscene playback.
 *
 * The engine's side of a video decoder. The codecs are behind viddec.h -- two
 * of them, picked from the file's own bytes -- the colour conversion is yuv.c,
 * and what is left, which is what this file is, is four things no decoder has
 * an opinion about:
 *
 *   where the bytes come from    the VFS, so a movie lives in mrpy.pak like
 *                                every other asset
 *   when a frame is shown        the sound card's own clock, not a timer
 *   what to do when behind       drop frames, never audio
 *   where the picture goes       straight into the presenter's picture
 *                                buffer, at the size that buffer is
 *
 * Two formats, and the file says which it is: MPEG-1 video with MP2 audio in
 * a program stream (.mpg), or MPEG-4 Simple Profile video with MP3 audio in
 * an AVI. ffmpeg produces either. The second exists because the first is too
 * expensive to decode on the target at 640x320 -- 26.89ms a frame against a
 * 40ms budget -- and xvidcore is two to three times faster on the same
 * content before its MMX kernels are counted. viddec.h has the measurements
 * and the reasoning.
 *
 * Nothing newer than those two, and that is not a limitation worth removing:
 * everything past MPEG-4 ASP is out of reach of a Pentium III at this frame
 * size, which is the same reason the port has no VP8 decoder. Note that
 * MPEG-4 is only cheaper *within Simple Profile* -- B-frames, quarter-pel
 * motion and global motion compensation would make it dearer than the MPEG-1
 * it replaces, so the encoding recipe is part of the design and is written
 * down in tools/harness/avitest.c.
 *
 * --- the shape of a cutscene ----------------------------------------------
 *
 *     $ renpy.movie_cutscene('audio/ending.mpg')
 *
 * interp.c executes that like any other statement: it opens the movie here,
 * parks the interpreter in ST_WAIT, and the frame loop calls mr_movie_update
 * until it says the movie is over or the player dismisses it. The statement's
 * Ren'Py semantics -- music stopped and restarted, no rollback checkpoint, no
 * game menu, a click always skips -- are interp.c's and are documented there
 * against exports/mediaexports.py:29.
 *
 * --- it is not a displayable, and that is deliberate -----------------------
 *
 * Ren'Py has two ways to show a movie: `Movie()`, a displayable that sits on
 * a layer and composites with everything else, and `movie_cutscene`, which
 * takes the screen. Only the second is implemented, because the first cannot
 * be on this target at any useful size.
 *
 * A displayable has to go through the stage: 1280x720 of 32bpp ARGB, composited
 * with whatever else is shown, then scaled down to the window. That is the
 * path every image takes and it costs about a third of a second a frame on
 * the PIII -- call it 3fps, against the 25 the file wants. A cutscene instead
 * writes converted pixels straight into the presenter's picture buffer and
 * the stage is not touched at all. The measured difference is in yuv.h.
 *
 * So `Movie()` in a script is not supported, and the honest symptom is a log
 * line rather than a slideshow.
 */

#ifndef MR_MOVIE_H
#define MR_MOVIE_H

#include "mr.h"

/* Open `path` -- a VFS name, as in the statement's argument -- and begin
 * playing. Returns 0 if it will not play, having said why in the log, in
 * which case nothing was started and the caller should fall back to the
 * poster-frame stand-in. Reasons: no such file, not a program stream, no
 * video in it, or a frame size that cannot be allocated.
 *
 * The caller must have stopped the music first if it wants it stopped; this
 * does not touch MR_CH_MUSIC. It does take over MR_CH_MOVIE.
 */
int  mr_movie_open(const char *path);

/* Stop, free the decoder and release MR_CH_MOVIE. Safe to call when nothing
 * is playing, and safe to call twice; the dismiss path and the ran-to-the-end
 * path both come through here. */
void mr_movie_close(void);

int  mr_movie_active(void);

/* Put the frame that is already on screen there again, without decoding
 * anything. Returns 0 if there is nothing to repaint -- no cutscene running,
 * or one that has not produced its first frame yet -- in which case the
 * caller should fall back to whatever it would otherwise have painted.
 *
 * This is for a repaint that does *not* come from the frame loop. On Win32
 * the window procedure gets WM_PAINT from an uncover, an activate, or a
 * resize, and its handler painted the stage -- which during a cutscene is
 * the scene from before the cutscene started, because main.c stops
 * compositing for the duration. The result was the previous background and
 * the quick menu flashing over the video, and during a drag-resize it was
 * worse than a flash: a resize runs a modal loop inside DefWindowProc, so
 * the application's message loop does not run, no frame is decoded, and
 * WM_PAINT is the *only* thing drawing. The whole drag showed the scene.
 *
 * Repainting rather than decoding is the point. The frame's planes belong to
 * the decoder and stay valid until the next decode, so this is a colour
 * conversion and a blit with no decoding, no clock and no audio -- safe to
 * call as often as Windows asks, and it cannot advance or disturb playback.
 */
int  mr_movie_repaint(void);

/* Start the same movie again from the beginning, for `loops`. Returns 0 if it
 * could not be reopened, in which case nothing is playing and the caller must
 * end the cutscene.
 *
 * A reopen rather than a rewind, which is a real choice and not laziness.
 * pl_mpeg can rewind a stream, but only by seeking its buffer back and
 * resetting the decoders, and the demuxer's idea of where it is would then
 * have to be reconciled with the VFS handle's -- two positions that are
 * currently kept in step by there only being one way to move either. Opening
 * again costs a file open and the duration search, which against a repeat of
 * a whole movie is nothing.
 *
 * Ren'Py expresses loops the same way: movie_start queues the same filename
 * (loops + 1) times on the movie channel (display/video.py:76) rather than
 * looping a decoder.
 */
int  mr_movie_replay(void);

/* Decode and present as much as the clock says is due, and return 1 while the
 * movie is still running, 0 once it has ended.
 *
 * Called once per frame from the frame loop, and it is the *only* thing that
 * draws while a cutscene is running -- the stage compositor is skipped
 * entirely, which is both why it is fast enough and why nothing else may draw
 * over it.
 *
 * Returning 0 does not close the movie. The caller decides what ending means
 * (interp.c leaves the wait, which may have a few milliseconds still to run)
 * and calls mr_movie_close.
 */
int  mr_movie_update(void);

/* The container's duration in milliseconds, or 0 if the demuxer would not say.
 *
 * This is what makes a cutscene's length known *at the moment it starts*,
 * which the poster-frame stand-in never managed: an MP3 does not record its
 * length, so that path had to start at a floor and extend the wait once the
 * decoder had seen a frame (interp.c). A program stream has the figure in it,
 * so a real movie needs none of that.
 */
long mr_movie_duration_ms(void);

/* --- diagnostics ---------------------------------------------------------
 *
 * Written to the log when a cutscene ends, because the thing that will go
 * wrong on the target is "it could not keep up" and that has three distinct
 * shapes which look identical on the screen:
 *
 *   frames dropped       the decoder is behind the sound. The picture stutters
 *                        and the audio is fine. This is the *designed*
 *                        failure and a few drops are not a fault.
 *   audio starved        the decoder is so far behind it could not even keep
 *                        the mixer fed. Audible, and much worse.
 *   neither, but slow    the file is simply long. Not a fault at all.
 *
 * mr_movie_stats fills in whichever pointers are not NULL; `shown` and
 * `dropped` count video frames, `starved_ms` is silence the mixer emitted.
 */
void mr_movie_stats(long *shown, long *dropped, long *starved_ms);

/* How many video frames the decoder actually produced this cutscene.
 *
 * Exposed for one reason: it is the denominator of the decode timing logged
 * by mr_movie_close, and that timing cannot be checked on the build host --
 * the platform clock there is far finer than the work and the shims make the
 * whole thing run as fast as the test polls. What *can* be checked is that
 * the denominator is the right one, and it has a closed form: every frame the
 * decoder produced was either shown or dropped, so this must equal
 * shown + dropped exactly. movietest asserts that; a miscounted decode shows
 * up there rather than as a plausible-looking millisecond figure on the
 * target, which is the failure this is guarding against.
 */
long mr_movie_decoded(void);

#endif /* MR_MOVIE_H */
