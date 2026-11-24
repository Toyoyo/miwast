#ifndef MR_AUDIO_H
#define MR_AUDIO_H

#include "mr.h"
#include "script.h"    /* mr_channel */

int  mr_audio_init(const char *game_dir);
void mr_audio_shutdown(void);

/* Called every frame. On Win32s the completion callback is not dependable, so
 * the mixer is driven by polling from the frame loop instead. */
void mr_audio_poll(void);

void mr_audio_play(mr_channel ch, const char *file, int loop,
                   long fadein_ms, float volume);

/* The full form the `play` statement needs. `fadeout_ms` fades out whatever is
 * playing first and starts this track when that finishes, which is what
 * Ren'Py's play() does -- fadeout() then enqueue(). `if_changed` keeps a track
 * that is already playing rather than restarting it; Ren'Py defaults it to
 * false and only the main menu turns it on. */
void mr_audio_play_ex(mr_channel ch, const char *file, int loop,
                      long fadein_ms, long fadeout_ms, float volume,
                      int if_changed);
void mr_audio_queue(mr_channel ch, const char *file, float volume);

/* --- a channel fed from outside, for a cutscene's soundtrack -------------
 *
 * Everything above hands the mixer a *name* and lets it open, read and decode
 * the file itself. A movie cannot be played that way: its audio is MP2
 * packets interleaved with the video inside one MPEG program stream, so the
 * only thing that can get at them is the demuxer that is already pulling the
 * picture out of the same file. For MR_CH_MOVIE, therefore, the mixer is a
 * sink and movie.c does the decoding.
 *
 * Only the input side differs. Resampling, the fade ramp, the music mixer's
 * volume and mute, and the mix itself are the same code as for a file --
 * which is the reason it is a channel at all rather than a second audio path
 * bolted to the video player.
 *
 * The sequence is: open, then push repeatedly while the movie runs, then end.
 *
 *     mr_audio_push_open(MR_CH_MOVIE, 44100, 1.0f);
 *     ... each frame:  n = mr_audio_push_space(MR_CH_MOVIE);
 *                      decode at most n frames and push them
 *     mr_audio_push_end(MR_CH_MOVIE);      the stream is over
 *
 * `rate` is the stream's sample rate; the mixer resamples as it does for a
 * 48kHz track. PCM is interleaved stereo 16-bit, which is what the ring is.
 *
 * **Ask for the room before decoding.** mr_audio_push takes what fits and
 * returns how much that was, so audio is dropped rather than a buffer
 * overrun -- but silently dropping the middle of a soundtrack is not a
 * failure anyone can diagnose from the speaker. Pushing only what
 * mr_audio_push_space allows means the decoder stays in step instead.
 *
 * Ending is not stopping. mr_audio_push_end says no more is coming; whatever
 * is still in the ring plays out, and the channel goes idle after it. Calling
 * mr_audio_stop instead cuts it off, which is what a skipped cutscene wants.
 */
void mr_audio_push_open(mr_channel ch, int rate, float volume);
long mr_audio_push_space(mr_channel ch);
long mr_audio_push(mr_channel ch, const short *pcm, long frames);
void mr_audio_push_end(mr_channel ch);

/* Output frames of silence this pushed channel has emitted because its ring
 * was empty when the mixer wanted it -- that is, how far the decoder fell
 * behind, in samples.
 *
 * For the log at the end of a cutscene. Audio starvation on the target is the
 * first thing that goes wrong when a machine cannot keep up, and through a
 * speaker a late decoder and a broken one sound identical. A number in
 * log.txt tells them apart without a second run. */
long mr_audio_push_starved(mr_channel ch);
void mr_audio_stop(mr_channel ch, long fadeout_ms);
void mr_audio_set_volume(mr_channel ch, float volume);

/* A channel's volume, which is the preference slider's value: anything that
 * ducks a channel temporarily has to put this back rather than 1.0, or the
 * player's setting is lost. */
float mr_audio_get_volume(mr_channel ch);
void mr_audio_set_mute(mr_channel ch, int mute);

/* Is this channel playing anything at all?
 *
 * Distinct from "has it reached the end of the file": a channel stays active
 * while a fade-out runs, and a pushed channel stays active while its ring
 * drains after mr_audio_push_end. Both are "still making sound", which is the
 * question a caller waiting for a track to finish is actually asking --
 * movie.c waits on this so a cutscene that ends on a cymbal is allowed to
 * finish it instead of being cut off at the last video frame. */
int mr_audio_active(mr_channel ch);

/* Whether a channel is muted. The preferences screen's state and the mixer's
 * are two different things, and a bug in between is silent audio under an
 * unticked button -- so it is worth being able to ask both. */
int mr_audio_muted(mr_channel ch);

/* Where the channel is in its track, and how long the track is, in
 * milliseconds -- renpy.music.get_pos and get_duration. Both 0 when the
 * channel is idle. The jukebox's `bar value AudioPositionValue()` is the
 * only caller. */
long mr_audio_position_ms(mr_channel ch);
long mr_audio_duration_ms(mr_channel ch);

/* Where the channel is *as heard*, in milliseconds.
 *
 * mr_audio_position_ms counts frames the mixer has produced, which are then
 * queued to the driver: there is up to a full runway's worth queued -- see
 * mr_audio_max_queue_ms -- so the mixed position runs that far ahead of the
 * speaker.
 * A progress bar cannot tell. The rhythm game can: its hit window is 300ms,
 * so judging a tap against the mixed position would score notes the player
 * has not heard yet. This asks the device what it has actually played.
 *
 * Falls back to the mixed position if the driver will not answer. */
long mr_audio_output_position_ms(mr_channel ch);

/* How long from now until audio handed to the mixer would be heard: the
 * frames already mixed but not yet played, which is up to the whole runway
 * (mr_audio_max_queue_ms). Anything that must sound at a particular moment has
 * to start it this far ahead. The rhythm game does, so that its first note
 * reaches the hit bar exactly as the track becomes audible. 0 if the driver
 * will not say. */
long mr_audio_latency_ms(void);

/* Cap how much audio may be queued ahead of the speaker, which *is* the
 * latency of every sound started from then on. 0 restores the default depth.
 *
 * For a screen that renders cheaply and needs to answer the player quickly:
 * the rhythm game's hit sounds were a queue depth behind the key, which at
 * the default is longer than its own 300ms hit window. Shallower trades
 * against underruns, so there is no good global value -- mrpy.ini's
 * rhythm_latency_ms exists so the floor can be found on real hardware.
 *
 * Rounded up to whole buffers and floored at two; mr_audio_queue_ms gives
 * back what was actually chosen. */
void mr_audio_set_queue_ms(long ms);
long mr_audio_queue_ms(void);

/* The deepest the queue goes, and so the default latency. Clamp a configured
 * value against this rather than against a literal: the runway has been
 * changed three times (8x4096, 16x2048, 24x2048) and every hardcoded copy of
 * the figure went stale without saying so. */
long mr_audio_max_queue_ms(void);

/* What a channel is currently playing, for a save. */
void mr_audio_current(mr_channel ch, char *out, int outsz, int *loop);

#endif /* MR_AUDIO_H */
