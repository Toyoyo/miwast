/* wave.h - the seam between the mixer and the sound card.
 *
 * audio.c decodes, resamples, mixes and fades; it produces interleaved
 * 16-bit stereo at 44100Hz and nothing else. Getting that out of a speaker
 * is the only part of it that knows what machine it is on, and this is where
 * that part was cut away -- the same cut as plat.h and present.h, and for the
 * same reason: a second target.
 *
 *     wavew32.c    waveOut, on Win32s
 *     wavedos.c    a Sound Blaster, on DOS
 *     tools/hostshim/wavehost.c   a device that can be driven from a harness
 *
 * The model is a fixed ring of buffers the sink owns. The mixer asks for one
 * that is free, fills it, and hands it back; the sink plays them in the order
 * it was given them. Nothing here says *how* they are played -- queued with
 * waveOutWrite, or handed to a DMA controller that is already running -- and
 * nothing above here needs to know.
 *
 * Two consequences of that shape are worth stating, because both were
 * properties of the Win32 code before they were an interface.
 *
 *   - The sink owns the memory. On DOS it has to: a DMA buffer must be in the
 *     first megabyte, physically contiguous, and not crossing a 64K page,
 *     none of which malloc knows about.
 *
 *   - "Outstanding" is the queue depth the mixer reasons about. A sound
 *     started now is heard only after everything already outstanding has
 *     played, which is what mr_audio_set_queue_ms is about and why the
 *     rhythm game's hit sounds were once audibly late.
 */

#ifndef MR_WAVE_H
#define MR_WAVE_H

/* One output format, everywhere. The device is opened once and never
 * reconfigured; sources at other rates are resampled by the mixer. */
#define MR_WAVE_RATE    44100
#define MR_WAVE_CH      2

/* Runway is BUFS * FRAMES: 24 x 2048 frames is 1114ms at 44.1kHz, in 192KB.
 * The long comment in audio.c explains why it is that long and why it is
 * split so finely -- in short, a transition on a Pentium III can take a
 * couple of hundred milliseconds in one frame and the mixer only runs
 * between frames. The figures live here now because the sink allocates the
 * buffers; call mr_audio_max_queue_ms rather than writing 1114 down. */
#define MR_WAVE_BUFS    24
#define MR_WAVE_FRAMES  2048
#define MR_WAVE_SAMPLES (MR_WAVE_FRAMES * MR_WAVE_CH)
#define MR_WAVE_BYTES   (MR_WAVE_SAMPLES * 2)

/* Open the device and allocate the ring. Returns 0 if there is no device or
 * no memory, having logged why; the engine then runs silently rather than
 * refusing to start, because a sound card is not a requirement for reading a
 * novel. */
int  mr_wave_open(void);
void mr_wave_close(void);

/* A buffer that is free to fill, or NULL if every one of them is still
 * queued. The pointer stays valid until mr_wave_close; the sink does not
 * move its buffers about. */
short *mr_wave_claim(void);

/* Hand a claimed buffer back, full. It must be one mr_wave_claim returned
 * and not yet submitted; the sink plays them in submission order.
 *
 * Returns 0 if the device would not take it, in which case the buffer is
 * free again and the caller should stop filling for now rather than hand it
 * straight back -- a sink that refuses one will refuse the next, and the
 * loop would spin refilling the same buffer. */
int mr_wave_submit(short *buf);

/* How many buffers are submitted and not yet played. */
int  mr_wave_outstanding(void);

/* Frames the device has actually played, counted from the first one ever
 * submitted and running monotonically for the session. -1 if the device
 * cannot say, in which case callers fall back to what the mixer has
 * produced, which runs up to a second ahead.
 *
 * The rhythm game judges taps against this, so a sink that guesses here is
 * worse than one that admits it does not know. */
long mr_wave_played_frames(void);

#endif /* MR_WAVE_H */
