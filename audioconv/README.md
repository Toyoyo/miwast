# audioconv

Turns an audio file into an Atari STE **SNDH** file that plays the recording
itself through the STE's DMA sound: 8-bit mono at 12517 Hz, compressed, and
decoded on the fly by a small 68000 replay routine.

Two codecs:

| codec   | size vs raw 8-bit | quality                                  | approx. size per minute |
|---------|-------------------|------------------------------------------|-------------------------|
| `rice`  | ~40-45% smaller   | lossless: bit-identical to the 8-bit samples | ~420 KB              |
| `adpcm` | 50% smaller       | 4-bit ADPCM, some audible hiss (~25 dB SNR) | ~370 KB              |

Raw 8-bit at 12517 Hz would be ~730 KB per minute. Savings for `rice` depend
on the music; quiet or simple material packs better.

## Requirements

- Python 3 (tested with 3.13) and the packages in `requirements.txt`
- [vasm](http://sun.hasenbraten.de/vasm/) 68k assembler, Motorola syntax:
  `vasmm68k_mot` in your `PATH`

```sh
python -m venv venv
. venv/bin/activate
pip install -r requirements.txt
```

## Usage

```sh
./convert.sh rice|adpcm <input audio> <output.sndh> [--title "Title"] [--preview out.wav]
```

- `<input audio>`: any file librosa can read (wav, flac, ogg, mp3, ...)
- `--title`: SNDH title (default: taken from the file name)
- `--preview`: also write the decoded 8-bit result as a WAV, to hear the
  result without an Atari or emulator

Example, converting a whole folder:

```sh
for f in music/*.wav; do
  ./convert.sh adpcm "$f" "out/$(basename "$f" .wav).sndh"
done
```

## Playback

- Needs an STE-compatible machine with DMA sound (STE, Mega STE). Tested in
  the Hatari emulator (STE, 2MB, TOS 2.06) and a real STE (4MB/TOS 2.06)
- The whole song is loaded into memory: check the file size against the
  machine's free RAM (a 3-minute song needs a 2 MB machine).
- Standard SNDH entry points (init / exit / play) with `TC50`: the player
  must call `play` 50 times per second. The YM chip is silenced.
- `rice` decoding costs noticeably more CPU than `adpcm`. Not measured yet;
  roughly estimated at a third of an 8 MHz 68000 for `rice` and a few
  percent for `adpcm`.

## How it works

- `convert_codec.py` resamples the input to 12517 Hz mono, peak-normalises
  it, quantises it to 8 bits and encodes it:
  - `rice`: each sample is predicted as `2*x[n-1] - x[n-2]`; the prediction
    error is stored as a Rice code, with the best Rice parameter chosen per
    block of 256 samples.
  - `adpcm`: IMA-style 4-bit ADPCM; the encoder tries all 16 codes per sample
    and keeps the closest.
  It also writes the SNDH header (title, length, timer tag).
- `build.sh` assembles `sndh_stream.s` with that data into the `.sndh` file.
- At run time, the DMA loops over a 4 KB ring buffer. On every 50 Hz call,
  the replay reads the DMA position and decodes just enough samples to keep
  the buffer full. All code is position independent, as SNDH requires.

## Files

| file               | purpose                                        |
|--------------------|------------------------------------------------|
| `convert.sh`       | entry point: encode, then assemble             |
| `convert_codec.py` | encoder (rice / adpcm) and SNDH header writer  |
| `build.sh`         | assembles `sndh_stream.s` with the encoded data |
| `sndh_stream.s`    | 68000 replay routine for both codecs           |
| `requirements.txt` | Python dependencies                            |
