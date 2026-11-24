/* plmdec.h - the engine's view of pl_mpeg. No pl_mpeg type crosses this line.
 *
 * pl_mpeg is written in C99 and the engine is C89 (CLAUDE.md: "declarations at
 * the top of a block"). That is not a style quarrel -- the harness build is
 * gcc and the target build is Open Watcom, and the C89 discipline is what
 * stops a construct that gcc accepts from reaching a compiler that does not.
 * One `#include "pl_mpeg.h"` would have put a file full of `//` comments into
 * the engine's own translation units and forced -za99 on them, and from then
 * on nothing would check our code against the rule.
 *
 * So the vendor header is included in exactly one place, plmimpl.c, which is
 * the only file with -za99. Everything else talks to this instead, in C89,
 * and the decoder's types stay behind the opaque mr_plm.
 *
 * The interchange types are the engine's own and not translations of
 * pl_mpeg's:
 *
 *   a frame   mr_yuv_frame, from yuv.h -- three byte planes and two pitches,
 *             which is all a frame is. It was defined that way before this
 *             header existed, so that the conversion and its harness need no
 *             decoder at all.
 *   audio     interleaved 16-bit stereo, which is what the mixer's ring is.
 *             pl_mpeg produces floats; the conversion is on the far side of
 *             this line, with the decoder that made them.
 *
 * Reading is through callbacks, so that the one implementation of them can be
 * the VFS and a movie stays an asset inside mrpy.pak like any other. There is
 * no path here that opens a file by name.
 */

#ifndef MR_PLMDEC_H
#define MR_PLMDEC_H

#include "mr.h"
#include "yuv.h"

typedef struct mr_plm mr_plm;

/* How the decoder gets at the bytes.
 *
 * `read` must return the number of bytes read, 0 at end of file. `seek` and
 * `tell` are needed for one thing only -- finding the container's duration,
 * which the demuxer does by seeking near the end of the file and reading
 * forward for the last timestamp -- and a reader that cannot seek may leave
 * them NULL and get a duration of 0.
 *
 * `size` is the whole stream's length in bytes, which the duration search
 * measures backwards from. */
typedef struct {
    long   (*read)(void *user, void *buf, long bytes);
    void   (*seek)(void *user, long offset);
    long   (*tell)(void *user);
    long     size;
    void    *user;
} mr_plm_io;

/* What the stream turned out to be. Filled by mr_plm_open; every field is
 * valid only if that returned non-NULL. */
typedef struct {
    int    width, height;
    double fps;
    long   duration_ms;    /* 0 if the io could not seek, or there is no PTS */
    int    has_audio;
    int    sample_rate;
} mr_plm_info;

/* Open an MPEG program stream. Returns NULL if it is not one, or has no video
 * track, or could not be allocated -- without logging; the caller knows what
 * it was trying to play and what to say about it.
 *
 * The io struct is copied, but `user` must outlive the decoder: every read
 * goes through it. */
mr_plm *mr_plm_open(const mr_plm_io *io, mr_plm_info *info);

void mr_plm_close(mr_plm *p);

/* Decode the next video frame in *display* order.
 *
 * Returns 1 and fills `out` and `*pts_ms`, or 0 at the end of the stream.
 * `out`'s plane pointers are into the decoder's own buffers and are valid
 * only until the next call -- a frame cannot be held back and shown later,
 * which is why movie.c decides whether to present a frame at the moment it
 * decodes it rather than queueing one.
 *
 * Display order matters and is not free: an MPEG-1 stream with B pictures
 * carries them out of order, since a B picture is predicted from a frame that
 * comes after it. pl_mpeg reorders internally, so `pts_ms` increases
 * monotonically and a caller can compare it against a clock without knowing
 * any of that. ending.mpg does have B pictures.
 */
int mr_plm_decode_video(mr_plm *p, mr_yuv_frame *out, long *pts_ms);

/* Decode the next audio frame into `pcm` as interleaved 16-bit stereo.
 *
 * Returns the number of *frames* (sample pairs) written, or 0 when none is
 * available yet. `max_frames` is the room in `pcm`; a caller must allow at
 * least MR_PLM_AUDIO_FRAME or a whole decoded frame will not fit and the
 * remainder is dropped.
 *
 * Mono streams are duplicated to stereo here, so the mixer never sees a
 * channel count.
 */
int mr_plm_decode_audio(mr_plm *p, short *pcm, int max_frames);

/* Frames in one decoded audio frame. MPEG-1 Layer II is 1152 samples per
 * channel, always; it is a constant of the format rather than of this file,
 * and the buffer a caller offers has to be at least this big. */
#define MR_PLM_AUDIO_FRAME 1152

#endif /* MR_PLMDEC_H */
