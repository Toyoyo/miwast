/* avi.h - the AVI container, read forwards, with no codec in it.
 *
 * movie.c's second decoder path needs a container that carries MPEG-4 video
 * and MP3 audio, and AVI is the one worth writing: the whole of what this
 * file needs from the format is a tree of 8-byte chunk headers, and a
 * `movi` list whose chunks are already the elementary packets a decoder
 * wants. MP4 and Matroska both carry the same payloads behind a great deal
 * more structure -- sample tables, EBML, timestamps to reconcile -- and none
 * of it buys anything here, because playback is forwards from the start and
 * the frame rate is constant.
 *
 * **No codec and no decoder type crosses this line.** What comes out is a
 * byte range and a tag saying which stream it belongs to. That is what makes
 * this file the testable half of the xvid path: avitest drives it against a
 * real ffmpeg-produced file on the host, where xvidcore's MMX kernels cannot
 * run. CLAUDE.md's rule for yuvmmx.c applies here for the same reason --
 * split the risk and check each half.
 *
 * --- what is deliberately not supported -----------------------------------
 *
 * No index. `idx1` is at the *end* of the file and exists for seeking, and a
 * cutscene is never seeked: movie.c plays from the first frame to the last
 * and expresses a loop by reopening (see movie.h on mr_movie_replay). Reading
 * the index would mean a seek to the end of the archive at open, for
 * information that is already in the chunk headers we walk anyway.
 *
 * No OpenDML (`AVIX`) extension, which only matters past 2GB. No more than
 * one video and one audio stream; a cutscene has exactly those.
 *
 * --- the one subtlety: stream numbers are not positions --------------------
 *
 * A chunk id is four characters -- `00dc`, `01wb` -- and the first two are
 * the *stream number* in ASCII. That number indexes the order the `strl`
 * lists appeared in `hdrl`, which is not required to be video-then-audio and
 * is not required to be dense. So the header walk records which stream number
 * turned out to be the video and which the audio, and the `movi` walk
 * compares against those rather than assuming 0 and 1. An encoder that writes
 * audio first would otherwise have every packet handed to the wrong decoder,
 * which through a speaker and a window is not a symptom anybody would read
 * as "the demuxer assumed an ordering".
 */

#ifndef MR_AVI_H
#define MR_AVI_H

#include "mr.h"

/* How the demuxer gets at the bytes. The same shape as mr_plm_io and for the
 * same reason: the one implementation of these is the VFS, so a movie stays
 * an asset inside mrpy.pak and nothing here can open a file by name.
 *
 * `seek` and `tell` are required, unlike mr_plm_io's: the header walk skips
 * over chunks it does not care about, and `hdrl` can carry a `JUNK` chunk of
 * several kilobytes that there is no reason to read. */
typedef struct {
    long   (*read)(void *user, void *buf, long bytes);
    void   (*seek)(void *user, long offset);
    long   (*tell)(void *user);
    long     size;
    void    *user;
} mr_avi_io;

/* What the file turned out to contain. Filled by mr_avi_open; valid only if
 * that returned non-NULL. */
typedef struct {
    int    width, height;
    long   fourcc;           /* BITMAPINFOHEADER.biCompression, little-endian
                              * as stored: 'XVID' reads as 0x44495658 */
    double fps;
    long   duration_ms;      /* from the stream header's own rate and length */
    long   total_frames;

    int    has_audio;
    int    audio_format;     /* WAVEFORMATEX.wFormatTag; 0x0055 is MP3 */
    int    audio_rate;
    int    audio_channels;
} mr_avi_info;

typedef struct mr_avi mr_avi;

/* Open and parse the header tree, leaving the reader positioned at the first
 * chunk of `movi`. Returns NULL if it is not a RIFF/AVI file, has no video
 * stream, or could not be allocated -- without logging, as mr_plm_open does
 * not: the caller knows what it was trying to play. */
mr_avi *mr_avi_open(const mr_avi_io *io, mr_avi_info *info);

void mr_avi_close(mr_avi *a);

#define MR_AVI_VIDEO  1
#define MR_AVI_AUDIO  2

/* The next packet in the file, in stored order.
 *
 * Returns MR_AVI_VIDEO or MR_AVI_AUDIO and sets `*data` and `*len`, or 0 at
 * the end of the `movi` list. Chunks belonging to any other stream are
 * skipped rather than reported.
 *
 * `*data` points into the demuxer's own buffer and is valid only until the
 * next call -- the same contract mr_plm_decode_video's planes have, and for
 * the same reason: one buffer, reused, so a two-hour movie costs what a
 * two-second one does.
 *
 * A zero-length chunk is reported, not skipped. AVI uses one to mean "this
 * frame is identical to the last", and so does a one-byte 0x7f chunk from a
 * VfW-produced file; both are the encoder saying something, and swallowing
 * them here would make a dropped frame indistinguishable from a decoder that
 * had nothing to say. xvidcore handles both (decoder.c:1609) and this file
 * must not pre-empt it.
 */
int mr_avi_next(mr_avi *a, const unsigned char **data, long *len);

#endif /* MR_AVI_H */
