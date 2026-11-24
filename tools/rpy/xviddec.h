/* xviddec.h - the engine's view of xvidcore and minimp3, behind one opaque type.
 *
 * The second decoder path, beside plmdec.h's MPEG-1. Same shape deliberately:
 * an opaque handle, io callbacks so a movie stays an asset inside mrpy.pak,
 * an mr_yuv_frame out of the video side and interleaved 16-bit stereo out of
 * the audio side. movie.c -- where every decision about timing and frame
 * dropping lives -- then needs no new concepts to drive it, only a different
 * open; see viddec.h for the dispatch.
 *
 * --- why a second codec at all --------------------------------------------
 *
 * Decode cost. pl_mpeg's IDCT and motion compensation are scalar C, and on
 * the target they are the whole of the problem: a 640x320 cutscene logged
 * 26.89ms of decode per frame against a 40ms budget, with 39.20ms of display
 * on top, so two thirds of the frames were dropped and the soundtrack starved
 * for 2466ms over the run. xvidcore's kernels are MMX, which a Pentium III
 * has.
 *
 * That is the claim and it is worth stating what is measured and what is not.
 * Measured on the host, same source material, xvidcore built *pure C* with no
 * assembly at all: 0.375ms a frame against pl_mpeg's 1.22ms. The ratio is
 * about three, before any SIMD is involved, because xvid's C is simply better
 * code than pl_mpeg's. What the MMX adds on top of that is a target-only
 * figure and nothing on this host can produce it -- see avitest, which says
 * so in its own output.
 *
 * MPEG-4 ASP is a *more* expensive format than MPEG-1 per macroblock if the
 * encoder uses what it has: B-frames, quarter-pel motion, global motion
 * compensation. So the encoding recipe is part of the design and not a
 * footnote. avitest.c carries it, and it asks for Simple Profile -- no
 * B-frames, no qpel, no GMC -- because the point of the exercise is cheaper
 * decoding, and a stream that uses those features is not that.
 *
 * --- zero copy ------------------------------------------------------------
 *
 * XVID_CSP_INTERNAL makes the decoder hand back pointers into its own edged
 * reference buffers rather than converting anything. For a 640-wide picture
 * the strides come out 768 and 384, which is precisely what mr_yuv_frame's
 * y_pitch and c_pitch exist to express, so a decoded frame reaches yuv.c with
 * no intermediate copy and no colour conversion but ours.
 *
 * It needs one piece of care, recorded here because it cost an afternoon:
 * decoder.c:1541 guards the output on `frame->output.plane[0] != NULL`, and
 * CSP_INTERNAL *writes* plane[0]. Passing the NULL that seems right for an
 * output the decoder is supposed to fill means the guard fails, nothing is
 * written, and every decode succeeds with three null planes. A non-NULL dummy
 * is required to get any output at all.
 */

#ifndef MR_XVIDDEC_H
#define MR_XVIDDEC_H

#include "mr.h"
#include "yuv.h"
#include "avi.h"

typedef struct mr_xvid mr_xvid;

/* Open an AVI holding MPEG-4 video and, optionally, MP3 audio. Returns NULL
 * if the container will not parse, the video is not a fourcc xvidcore
 * decodes, or memory ran out -- without logging, as mr_plm_open does not.
 *
 * `info` is filled from the container: picture size, frame rate, duration,
 * and the audio format. The io struct is copied, but `user` must outlive the
 * decoder.
 *
 * The audio is reported as absent unless it is MP3 (wFormatTag 0x0055), which
 * is the one thing minimp3 decodes. An AVI with PCM or AC3 audio therefore
 * plays as a silent movie rather than failing to open -- the picture is still
 * the point, and a silent cutscene has a working clock (movie.c falls back to
 * mr_plat_ticks). The log line says which happened.
 */
mr_xvid *mr_xvid_open(const mr_avi_io *io, mr_avi_info *info);

void mr_xvid_close(mr_xvid *x);

/* Decode the next video frame. Returns 1 and fills `out` and `*pts_ms`, or 0
 * at the end of the stream.
 *
 * `out`'s planes point into the decoder's own buffers and are valid only
 * until the next call -- the same contract mr_plm_decode_video has, so
 * movie.c's "decide whether to present at the moment of decoding" holds
 * unchanged.
 *
 * The timestamp is computed from the frame number and the container's frame
 * rate, not read from the stream. AVI carries no per-frame timestamp, and for
 * a constant-rate file -- which is what an encoder asked for a cutscene
 * produces -- the frame number *is* the timestamp. That is a simplification
 * over the MPEG-1 path, not a loss: pl_mpeg has to reorder B-pictures to make
 * its PTS monotonic, and a Simple Profile stream has none to reorder.
 */
int mr_xvid_decode_video(mr_xvid *x, mr_yuv_frame *out, long *pts_ms);

/* Decode the next audio into `pcm` as interleaved 16-bit stereo. Returns the
 * number of frames (sample pairs) written, or 0 when none is available yet.
 * Mono is duplicated to stereo here, so the mixer never sees a channel count.
 *
 * `max_frames` must be at least MR_XVID_AUDIO_FRAME. */
int mr_xvid_decode_audio(mr_xvid *x, short *pcm, int max_frames);

/* Frames in one decoded MP3 frame. 1152 for MPEG-1 Layer III, which is what
 * ffmpeg produces at 44.1kHz; a Layer III frame at a lower sample rate is 576
 * and fits the same buffer. Same figure as MR_PLM_AUDIO_FRAME, from the same
 * property of the format, and deliberately spelled separately: they are two
 * codecs that happen to agree, not one constant. */
#define MR_XVID_AUDIO_FRAME 1152

/* mrpy.ini's `simd`, as mr_yuv_set_simd is for our own kernel. Off forces
 * xvidcore's pure C paths.
 *
 * The same reasoning as yuvmmx.h's: this is not a tuning knob. xvidcore's
 * assembly cannot be executed by anything on the host, so "the picture is
 * wrong" and "a SIMD kernel is wrong" can only be told apart on the machine,
 * in one run, without a rebuild. Must be called before mr_xvid_open; xvidcore
 * latches its cpu flags in its one-time global init. */
void mr_xvid_set_simd(int on);

/* Which kernels xvidcore actually chose: "mmx", "mmxext", "sse", "sse2",
 * "none", or "?" before the first movie has opened. Static storage.
 *
 * For the log line, and it earns its place there. The Win32 library is built
 * with xvidcore's MMX assembly and the DOS one is built without it, so the
 * same engine source gives two different answers -- and "the cutscene is slow
 * because the picture is big" and "the cutscene is slow because the SIMD
 * silently is not there" are the same thing to look at. `simd=0` also lands
 * here as "none", which is what makes that switch checkable rather than
 * merely settable. */
const char *mr_xvid_simd_name(void);

#endif /* MR_XVIDDEC_H */
