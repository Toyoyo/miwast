"""Generate a beatmap whose tracks follow the music.

Unlike generate_beatmap.py, which only writes onset timestamps and lets the
game assign the tracks pseudo-randomly, this script also decides which track
each onset belongs to, so that the note the player hears matches the harmony
of the song:

  * the key of the song is estimated (Krumhansl-Schmuckler),
  * the four most used degrees of the matching pentatonic scale become the four
    tracks, ordered from the lowest (left) to the highest (right),
  * each onset goes to the track best supported by what is playing at that
    moment (the note being played, plus the surrounding harmony).

Because a pentatonic scale has no dissonant interval, any two notes played
close together still sound fine, even when the estimate is off - which happens,
notably on percussive onsets that have no pitch at all.

Output, for my_music.mp3:

  my_music.beatmap.txt   one "seconds<TAB>track" line per onset
  my_music.hit0.wav      the hit sound of track 0 (leftmost, lowest)
  ...
  my_music.hit3.wav      the hit sound of track 3 (rightmost, highest)

The hit sounds are the reference sample resampled to the four pitches, the way
a sampler would do it. The game falls back to its default sounds and to the
pseudo-random tracks when these files are absent, so beatmaps in the old format
keep working.

Usage:
    python3 generate_beatmap-pentatonique.py my_music.mp3 [more files or a dir]
    python3 generate_beatmap-pentatonique.py --sample "fx corde.mp3" songs/
"""

import os
import sys
import wave
import struct

# python-packages directory
import_dir = os.path.abspath('python-packages')
sys.path.append(import_dir)

import numpy as np
from aubio import source, onset

NUM_TRACKS = 4
NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']

# default name of the sample the hit sounds are pitched from, looked up next to
# the song unless --sample says otherwise
DEFAULT_SAMPLE = 'fx corde.mp3'

# the four pitches are placed in the octave closest to the sample they are
# made from, so the resampling ratios stay small and the sample keeps its
# timbre whatever its own pitch is; this is the fallback when the pitch of the
# sample could not be measured
CENTER_MIDI = 50.0

# Krumhansl-Schmuckler key profiles
MAJOR_PROFILE = np.array(
    [6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88])
MINOR_PROFILE = np.array(
    [6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17])
MAJOR_PENTATONIC = [0, 2, 4, 7, 9]
MINOR_PENTATONIC = [0, 3, 5, 7, 10]

# pitch candidates used to build the chroma, one per semitone
CANDIDATE_MIDI = np.arange(36, 85, 1.0)


def read_samples(file_path, sample_rate=0):
    """Whole file as mono float64, plus its sample rate."""
    hop_size = 4096
    src_func = source(file_path, sample_rate, hop_size)
    blocks = []
    while True:
        samples, num_frames_read = src_func()
        blocks.append(samples[:num_frames_read].copy())
        if num_frames_read < hop_size:
            break
    return np.concatenate(blocks).astype(np.float64), src_func.samplerate


def get_onset_times(file_path):
    """Onset timestamps in seconds."""
    window_size = 1024 # FFT size
    hop_size = window_size // 4

    src_func = source(file_path, 0, hop_size)
    sample_rate = src_func.samplerate
    # the sample rate must be passed explicitly: aubio assumes 44100 Hz
    # otherwise, and every timestamp ends up scaled by sample_rate / 44100
    onset_func = onset('default', window_size, hop_size, sample_rate)

    duration = float(src_func.duration) / sample_rate

    onset_times = []
    while True:
        samples, num_frames_read = src_func()
        if onset_func(samples):
            onset_time = onset_func.get_last_s()
            if onset_time < duration:
                onset_times.append(onset_time)
        if num_frames_read < hop_size:
            break

    return onset_times


def spectrum(samples, sample_rate, start_s, end_s):
    """Magnitude spectrum of a slice, or (None, None) if too short."""
    start = max(0, int(start_s * sample_rate))
    end = min(len(samples), int(end_s * sample_rate))
    segment = samples[start:end]
    if len(segment) < 1024:
        return None, None
    size = 1 << int(np.ceil(np.log2(len(segment))))
    padded = np.zeros(size)
    padded[:len(segment)] = segment * np.hanning(len(segment))
    return np.abs(np.fft.rfft(padded)), np.fft.rfftfreq(size, 1.0 / sample_rate)


def chroma(samples, sample_rate, start_s, end_s, num_harmonics=6, decay=0.8):
    """Pitch class energies, summing each candidate's harmonics.

    Summing harmonics instead of folding the raw spectrum matters: otherwise a
    low note feeds the pitch classes of its fifth and third as much as its own.
    """
    spec, freqs = spectrum(samples, sample_rate, start_s, end_s)
    if spec is None:
        return None
    fundamentals = 440.0 * 2 ** ((CANDIDATE_MIDI - 69) / 12.0)
    salience = np.zeros(len(CANDIDATE_MIDI))
    for harmonic in range(1, num_harmonics + 1):
        salience += (decay ** (harmonic - 1)) * np.interp(
            fundamentals * harmonic, freqs, spec, left=0, right=0)
    pitch_classes = np.zeros(12)
    np.add.at(pitch_classes, CANDIDATE_MIDI.astype(int) % 12, salience)
    total = pitch_classes.sum()
    if total <= 0:
        return None
    return pitch_classes / total


def estimate_key(histogram):
    """(tonic, 'major'|'minor', correlation) best matching the pitch profile."""
    normalized = (histogram - histogram.mean()) / (histogram.std() + 1e-12)
    best = None
    for tonic in range(12):
        for mode, profile in (('major', MAJOR_PROFILE), ('minor', MINOR_PROFILE)):
            rotated = np.roll(profile, tonic)
            rotated = (rotated - rotated.mean()) / rotated.std()
            correlation = float(np.dot(normalized, rotated)) / 12
            if best is None or correlation > best[2]:
                best = (tonic, mode, correlation)
    return best


def pick_track_pitches(histogram, tonic, mode, center_midi=CENTER_MIDI):
    """Four MIDI notes, lowest first, taken from the pentatonic of the key.

    They are spelled in the octave closest to center_midi - the pitch of the
    sample they will be made from - so that no track ends up several octaves
    away from it.
    """
    scale = MAJOR_PENTATONIC if mode == 'major' else MINOR_PENTATONIC
    pentatonic = [(tonic + degree) % 12 for degree in scale]
    # the four degrees the song uses the most
    chosen = sorted(pentatonic, key=lambda pc: -histogram[pc])[:NUM_TRACKS]
    return sorted(pc + 12 * int(round((center_midi - pc) / 12.0)) for pc in chosen)


# the reference sample is expected to be a pitched instrument note in this
# range; outside of it the autocorrelation below returns an octave error, so
# --sample is best given a bass, guitar, koto, marimba... rather than a whistle
SAMPLE_PITCH_MIN_HZ = 60.0
SAMPLE_PITCH_MAX_HZ = 400.0


def detect_sample_pitch(samples, sample_rate):
    """Fundamental of the reference sample, as a MIDI note, or None.

    Plain autocorrelation, restricted to SAMPLE_PITCH_MIN_HZ..MAX_HZ: widening
    that range is not free, since short lags always correlate well and the peak
    then lands on a harmonic instead of the fundamental.
    """
    segment = samples[int(0.03 * sample_rate):int(0.25 * sample_rate)]
    if len(segment) < 1024:
        return None
    segment = segment - segment.mean()
    correlation = np.correlate(segment, segment, 'full')[len(segment) - 1:]
    low = int(sample_rate / SAMPLE_PITCH_MAX_HZ)
    high = min(int(sample_rate / SAMPLE_PITCH_MIN_HZ), len(correlation))
    if high - low < 2:
        return None
    lag = low + int(np.argmax(correlation[low:high]))
    return 69 + 12 * np.log2((sample_rate / float(lag)) / 440.0)


def write_wav(path, samples, sample_rate):
    peak = np.abs(samples).max()
    if peak > 1.0:
        samples = samples / peak
    data = (samples * 32767.0).astype(np.int16)
    with wave.open(path, 'wb') as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(sample_rate)
        out.writeframes(data.tobytes())


def resample(samples, ratio):
    """Change pitch and speed together, the way a sampler does."""
    length = int(len(samples) / ratio)
    return np.interp(
        np.arange(length) * ratio, np.arange(len(samples)), samples)


def find_sample(song_path, sample_option):
    if sample_option:
        return sample_option
    candidate = os.path.join(os.path.dirname(song_path) or '.', DEFAULT_SAMPLE)
    return candidate if os.path.isfile(candidate) else None


def process(file_path, sample_option):
    onset_times = get_onset_times(file_path)
    if not onset_times:
        raise ValueError('no onset found')

    samples, sample_rate = read_samples(file_path)

    # pitch content at each onset (just after the attack, whose transient
    # carries no pitch) and over the surrounding bar
    local = {}
    context = {}
    histogram = np.zeros(12)
    for onset_time in onset_times:
        near = chroma(samples, sample_rate, onset_time + 0.02, onset_time + 0.22)
        if near is None:
            continue
        local[onset_time] = near
        histogram += near
        around = chroma(samples, sample_rate, onset_time - 0.4, onset_time + 0.6)
        context[onset_time] = around if around is not None else near
    if not local:
        raise ValueError('no pitch content found')

    tonic, mode, correlation = estimate_key(histogram)

    # the sample is loaded first: its own pitch decides in which octave the
    # four tracks are spelled, so the resampling never goes far
    sample_path = find_sample(file_path, sample_option)
    sample, sample_sr, base_midi = None, None, None
    if sample_path:
        sample, sample_sr = read_samples(sample_path)
        base_midi = detect_sample_pitch(sample, sample_sr)

    track_midis = pick_track_pitches(
        histogram, tonic, mode,
        base_midi if base_midi is not None else CENTER_MIDI)
    track_classes = [midi % 12 for midi in track_midis]

    # each onset goes to the track best supported by the music right now: the
    # note being played, weighted with the harmony around it
    track_indices = []
    for onset_time in onset_times:
        if onset_time not in local:
            # no pitch at all: keep the note, spread it over the tracks
            track_indices.append(len(track_indices) % NUM_TRACKS)
            continue
        scores = [0.6 * local[onset_time][pc] + 0.4 * context[onset_time][pc]
                  for pc in track_classes]
        track_indices.append(int(np.argmax(scores)))

    base_name, _ = os.path.splitext(file_path)
    with open(base_name + '.beatmap.txt', 'wt') as out:
        out.write('\n'.join(
            '%.4f\t%d' % pair for pair in zip(onset_times, track_indices)))

    # hit sounds, pitched from the reference sample
    if sample_path is None:
        sounds = 'no sample found, hit sounds not generated'
    elif base_midi is None:
        sounds = 'no pitch detected in %s, hit sounds not generated' % (
            os.path.basename(sample_path),)
    else:
        for track_idx, midi in enumerate(track_midis):
            ratio = 2 ** ((midi - base_midi) / 12.0)
            write_wav('%s.hit%d.wav' % (base_name, track_idx),
                      resample(sample, ratio), sample_sr)
        sounds = '%s (%s%d) pitched %+.1f..%+.1f semitones' % (
            os.path.basename(sample_path),
            NOTE_NAMES[int(round(base_midi)) % 12], int(round(base_midi)) // 12 - 1,
            track_midis[0] - base_midi, track_midis[-1] - base_midi)

    counts = np.bincount(track_indices, minlength=NUM_TRACKS)
    return ('%s: %d onsets, key %s %s (%.2f), tracks %s, spread %s, %s' % (
        os.path.basename(file_path), len(onset_times),
        NOTE_NAMES[tonic], mode, correlation,
        ' '.join(NOTE_NAMES[pc] for pc in track_classes),
        '/'.join('%d%%' % (100.0 * c / len(track_indices)) for c in counts),
        sounds))


def main():
    args = [arg for arg in sys.argv[1:]]
    sample_option = None
    if '--sample' in args:
        index = args.index('--sample')
        sample_option = args[index + 1]
        del args[index:index + 2]

    if not args:
        print('Usage: python generate_beatmap-pentatonique.py [--sample corde.mp3] [input]\n'
              'Generate my_music.beatmap.txt and my_music.hit0..3.wav from my_music.mp3\n'
              '[input] could be a file or directory containing audio files')
        return

    files = []
    for path in args:
        if os.path.isdir(path):
            files.extend(os.path.join(path, name) for name in sorted(os.listdir(path))
                         if not name.startswith('.'))
        else:
            files.append(path)

    for file_path in files:
        try:
            print(process(file_path, sample_option))
        except Exception as e:
            print('Error processing ' + file_path + '\n' + str(e))


if __name__ == '__main__':
    main()
