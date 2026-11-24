/* viddec.h - one video decoder interface, two codecs behind it.
 *
 * movie.c owns every decision about *when* a frame is shown: the sound card
 * is the clock, frames whose moment has passed are decoded and dropped, the
 * mixer is fed before anything expensive runs. None of that depends on which
 * codec produced the frame, and this header is what keeps it that way -- it
 * is plmdec.h's interface with the pl_mpeg out of the name, and the dispatch
 * in viddec.c picks a backend from the file's own first twelve bytes.
 *
 * The two backends:
 *
 *   plmdec.h    MPEG-1 video with MP2 audio in a program stream (.mpg).
 *               pl_mpeg. The original path and still the fallback.
 *   xviddec.h   MPEG-4 Simple Profile video with MP3 audio in AVI (.avi).
 *               xvidcore and minimp3.
 *
 * --- why there are two ----------------------------------------------------
 *
 * Decode cost on the target. A 640x320 cutscene logged 26.89ms of decode per
 * frame against a 40ms budget; two thirds of the frames were dropped and the
 * soundtrack starved for 2466ms over the run. The same content through
 * xvidcore, measured on the host with both libraries at -O2 and **both on
 * their pure C paths**, is 0.680ms a frame against pl_mpeg's 1.462ms at the
 * same bitrate -- so a factor of two before any assembly is involved,
 * because xvid's C is better code. At half the bitrate, which looks no worse,
 * it is 0.463ms. On top of that the Pentium III runs xvidcore's MMX kernels
 * and pl_mpeg has none.
 *
 * What this does *not* fix is the other half of the frame: 39.20ms of colour
 * conversion, packing and blitting per shown frame, which is the same work
 * whatever decoded the picture. A cheaper codec shortens the update and so
 * stops the mixer starving; it does not make a 640x320 cutscene free. See
 * NOTES.md under "Movie cutscenes".
 *
 * --- sniffed, not chosen by extension -------------------------------------
 *
 * The dispatch reads the file. An extension is a claim by whoever named the
 * asset and the engine has no reason to trust it -- and `ask the narrow
 * question` (CLAUDE.md) applies to a decoder as much as to a repaint: the
 * question is "what is this file", and the file answers it. A .avi that is
 * really a program stream plays, and so does the reverse.
 *
 * Both codecs stay compiled in. Dropping MPEG-1 would strand every cutscene
 * already encoded, and the fallback is worth more than the kilobytes: a
 * machine that will not play one format can be given the other by
 * re-encoding one asset, with no rebuild.
 */

#ifndef MR_VIDDEC_H
#define MR_VIDDEC_H

#include "mr.h"
#include "yuv.h"

typedef struct mr_vid mr_vid;

/* How the decoder gets at the bytes. Identical in shape to mr_plm_io and
 * mr_avi_io, because the one implementation of it is the VFS: a movie is an
 * asset inside mrpy.pak and there is no path here that opens a file by name.
 *
 * Unlike mr_plm_io's, `seek` and `tell` are not optional. The sniff needs to
 * put the file back to the start, and the AVI header walk skips over chunk
 * payloads rather than reading them. */
typedef struct {
    long   (*read)(void *user, void *buf, long bytes);
    void   (*seek)(void *user, long offset);
    long   (*tell)(void *user);
    long     size;
    void    *user;
} mr_vid_io;

typedef struct {
    int    width, height;
    double fps;
    long   duration_ms;    /* 0 if it could not be determined */
    int    has_audio;
    int    sample_rate;

    /* Which backend took it, for the log line movie.c writes at open. Worth
     * having in there: "the cutscene is slow" and "the cutscene fell back to
     * the other codec because the .avi would not parse" are the same symptom
     * on the machine, and one log line separates them. Points to static
     * storage. */
    const char *codec;
} mr_vid_info;

/* Open whatever it turns out to be. Returns NULL if neither backend will take
 * it, or it has no video track, or memory ran out -- without logging; the
 * caller knows what it was trying to play and what to say about it.
 *
 * The io struct is copied, but `user` must outlive the decoder. */
mr_vid *mr_vid_open(const mr_vid_io *io, mr_vid_info *info);

void mr_vid_close(mr_vid *v);

/* Decode the next video frame in display order. Returns 1 and fills `out` and
 * `*pts_ms`, or 0 at the end of the stream.
 *
 * `out`'s planes point into the decoder's own buffers and are valid only
 * until the next call. That is a contract both backends keep and it is what
 * lets movie.c decide whether to present a frame at the moment it decodes it,
 * rather than queueing frames and choosing later. */
int mr_vid_decode_video(mr_vid *v, mr_yuv_frame *out, long *pts_ms);

/* Decode the next audio into `pcm` as interleaved 16-bit stereo. Returns the
 * number of frames (sample pairs) written, or 0 when none is available yet.
 * `max_frames` must be at least MR_VID_AUDIO_FRAME. Mono is duplicated to
 * stereo inside, so the mixer never sees a channel count. */
int mr_vid_decode_audio(mr_vid *v, short *pcm, int max_frames);

/* The scratch a caller must offer mr_vid_decode_audio.
 *
 * MPEG-1 Layer II is 1152 frames and Layer III at 44.1kHz is 1152 too, so the
 * two backends agree -- but this is deliberately the max of the two rather
 * than either one's constant, so that adding a codec with a longer frame is a
 * change here and not a buffer overrun in movie.c. */
#define MR_VID_AUDIO_FRAME 1152

/* mrpy.ini's `simd`, passed down to whichever backend has SIMD to turn off.
 * Must be called before mr_vid_open: xvidcore latches its cpu flags in a
 * one-time global init. See xviddec.h on why this is a diagnostic and not a
 * tuning knob. */
void mr_vid_set_simd(int on);

#endif /* MR_VIDDEC_H */
