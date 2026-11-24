/* sbconv.h - mixer frames to what an older Sound Blaster will take.
 *
 * audio.c produces one format and only one: interleaved signed 16-bit stereo
 * at 44100Hz. An SB16 plays that natively, which is why wavedos.c had no
 * conversion in it at all. Everything before the 16 does not:
 *
 *     SB 1.x    8-bit unsigned mono, no auto-init DMA, about 22kHz at best
 *     SB 2.0    8-bit unsigned mono, auto-init, higher rates
 *     SB Pro    8-bit unsigned, stereo only via the mixer chip
 *     ESS1868   answers as an SB Pro (and has a native 16-bit mode we do
 *               not use)
 *
 * So something has to downconvert, and this is it: the arithmetic, with no
 * card in it, so that it can be tested on the host where a wrong answer is a
 * failing assertion rather than a noise on a machine in another room. The
 * card-poking stays in wavedos.c. Same split as vbe.c against vbedos.c, and
 * pack.c against present.c.
 *
 * --- the two things that make it sound like 1991 rather than like noise ----
 *
 * Both were measured on the real music, against a brick-walled reference,
 * after a recording from the machine said the first version was noisy. The
 * numbers are in NOTES; the short version:
 *
 *   The filter. Averaging two samples is a two-tap filter, 3dB down at
 *   11kHz and only 6dB down at 15kHz, so most of what sits above the new
 *   Nyquist folds back into the music. On this track that was 24.3dB
 *   signal-to-error where a proper half-band gives 40.3. Hence FIR_TAPS
 *   below: an eleven-tap half-band, which is four multiplies per output
 *   because every other tap of a half-band is zero.
 *
 *   The level. Eight bits is 48dB *at full scale*, and a visual novel's
 *   music does not run at full scale: this track peaks at -11dBFS, the
 *   player's music volume multiplies that again, and what reaches the
 *   quantiser uses a fifth of the range. Half its samples land within two
 *   steps of silence. That is the hiss, and no filter touches it -- the
 *   only lever is to use more of the range, which is what the gain is for.
 *
 * --- why there is no phase accumulator -------------------------------------
 *
 * The honest general answer to "44100 into 22050" is a resampler with a
 * fractional phase carried between buffers, and the seam between two buffers
 * is where that sort of thing clicks. We do not need one. Every rate we ask
 * an old card for is 44100 divided by a whole number, so the conversion is
 * decimation: average `decim` input frames into one output frame. With
 * MR_WAVE_FRAMES a power of two the buffer boundary always lands on a group
 * boundary, so no input frame is ever split across two calls and there is no
 * phase to carry.
 *
 * That restricts the rates to 44100, 22050 and 11025, which is not a
 * restriction worth minding: those are the rates these cards were sold with.
 *
 * The averaging is a box filter, which is a poor anti-alias filter. It is
 * also four adds. On the machine that would play through an SB 1.x -- a 386
 * or a 486, or a PIII whose only card is an ESS -- the difference between a
 * box filter and a windowed sinc is not audible through an eight-bit DAC.
 *
 * --- what does carry across a buffer ---------------------------------------
 *
 * The filter's history, now, as well as the dither error. The filter needs
 * five input frames either side of the one it is centred on, so the output
 * runs five frames behind the input and the last ten frames of each buffer
 * are kept for the next call. A converter that started each buffer from
 * zeroed history would put a dip at every 46ms boundary.
 *
 * And the dither error. Eight bits of unsigned PCM is 48dB, and the quiet parts
 * of a visual novel's piano loop sit inside the bottom few bits of that;
 * truncating gives the grainy, crunchy quantisation that 8-bit audio is
 * remembered for, and most of that memory is of truncation rather than of
 * eight bits. The error from each sample is fed into the next, which trades
 * the correlated grain for a steady hiss and keeps the *average* level
 * correct below one LSB. That residue is per channel and it must survive
 * from one buffer to the next, so it lives in the state struct -- and it is
 * the one thing a seam test here can actually catch.
 */

#ifndef MR_SBCONV_H
#define MR_SBCONV_H

#include "mr.h"

/* Eleven taps, of which four are zero; see the comment on FIR[] in the .c */
#define MR_SBCONV_TAPS 11
#define MR_SBCONV_HIST (MR_SBCONV_TAPS - 1)

typedef struct {
    int  decim;         /* input frames per output frame */
    int  channels;      /* 1 or 2 */
    int  bits;          /* 8 or 16 */
    int  gain;          /* percent; 100 is the mixer's own level */
    int  dither;        /* add noise before quantising; see set_dither */
    unsigned long rng;  /* the noise, which has to be reproducible */
    long err[2];        /* shaping residue, per channel, 8-bit output only */
    long clipped;       /* samples the gain pushed past full scale */
    short hist[MR_SBCONV_HIST][2];      /* the tail of the last buffer */
} mr_sbconv;

/* Set up for an output format, and reset the dither state.
 *
 * `rate` must divide MR_WAVE_RATE exactly by 1, 2 or 4; anything else is
 * refused rather than approximated, because a card fed at a rate the
 * converter only nearly produced drifts, and drift is much harder to
 * recognise by ear than a missing sound. Returns 0 on a format this cannot
 * produce, having touched nothing. */
int mr_sbconv_init(mr_sbconv *c, int rate, int channels, int bits);

/* Bytes mr_sbconv_run will write for that many input frames. */
long mr_sbconv_out_bytes(const mr_sbconv *c, int in_frames);

/* Convert. `in` is in_frames interleaved stereo 16-bit frames; `out` gets
 * mr_sbconv_out_bytes of them. in_frames must be a multiple of decim --
 * MR_WAVE_FRAMES is 2048, so every supported decimation divides it.
 *
 * 8-bit output is unsigned with 0x80 as silence, which is what the DAC on
 * these cards wants; 16-bit output is signed little-endian, i.e. the input
 * byte for byte when nothing else is being changed. */
void mr_sbconv_run(mr_sbconv *c, const short *in, int in_frames,
                   unsigned char *out);

/* Digital makeup gain, in percent, applied just before the quantiser.
 *
 * This is the lever for the level problem described at the top, and it is a
 * knob rather than a constant because it is a trade with no right answer:
 * every doubling is six more dB of signal over the quantiser's noise and
 * six less dB of headroom before clipping. It is also the one place in the
 * engine that deliberately plays louder than the mixer asked, so it is
 * worth being able to turn off -- mrpy.ini's `audiogain=100` does that.
 *
 * Count the clipping rather than hoping: mr_sbconv_clipped is logged at
 * shutdown, so a player who sets it too high has something to read. */
void mr_sbconv_set_gain(mr_sbconv *c, int percent);
long mr_sbconv_clipped(const mr_sbconv *c);

/* Add a triangular one-step noise before the quantiser. On by default for
 * 8-bit output; `audiodither=0` in mrpy.ini turns it off.
 *
 * Shaping and dithering are two different things and this file does both.
 * Feeding the error forward (what err[] does) keeps the *average* right
 * but leaves the error correlated with the signal, and a correlated error
 * on quiet tonal material is not hiss -- it is granulation and idle tones,
 * grit that rides on the notes and comes and goes with them. Adding noise
 * before the decision decorrelates it, which costs about 3dB of measured
 * noise power and buys a defect the ear treats as background rather than
 * as part of the music. It is the one audible thing that no aggregate
 * measurement of this converter showed, which is why it is a switch: the
 * person with the sound card gets to decide. */
void mr_sbconv_set_dither(mr_sbconv *c, int on);

/* Silence, in the output's own idea of it: 0x80 for 8-bit, 0 for 16. The
 * underrun path needs this and getting it wrong is a loud click, not a
 * quiet one. */
unsigned char mr_sbconv_silence(const mr_sbconv *c);

#endif /* MR_SBCONV_H */
