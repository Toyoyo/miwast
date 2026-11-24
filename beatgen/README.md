# beatgen

`generate_beatmap-pentatonique.py` generates beatmaps and hit sounds for the
rhythm minigame, for Ren'Py and compatible engines. For each song it detects
the onsets, estimates the key, and assigns every note to one of four tracks
following the pentatonic scale of the song.

## Installation

aubio is compiled from source during the install. That needs a C compiler and
the FFmpeg development libraries. Without FFmpeg the install still succeeds,
but the script can only read WAV files.

On Debian/Ubuntu:

```sh
sudo apt install build-essential python3-dev python3-venv pkg-config \
    libavcodec-dev libavformat-dev libavutil-dev libswresample-dev \
    libsndfile1-dev libsamplerate0-dev
```

Then create a virtual environment and install the dependencies:

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

The last aubio release on PyPI (0.4.9) does not build against numpy 2, so
`requirements.txt` installs aubio from its upstream git repository instead.

## Usage

```sh
.venv/bin/python generate_beatmap-pentatonique.py my_music.mp3
.venv/bin/python generate_beatmap-pentatonique.py --sample "fx corde.mp3" songs/
```

The script takes audio files or directories. For `my_music.mp3` it writes:

| File                  | Content                                         |
|-----------------------|-------------------------------------------------|
| `my_music.beatmap.txt` | one `seconds<TAB>track` line per onset          |
| `my_music.hit0.wav`    | hit sound of track 0 (leftmost, lowest pitch)   |
| `my_music.hit1.wav`    | hit sound of track 1                            |
| `my_music.hit2.wav`    | hit sound of track 2                            |
| `my_music.hit3.wav`    | hit sound of track 3 (rightmost, highest pitch) |

The hit sounds are made by pitching a reference sample. By default the script
looks for `fx corde.mp3` next to the song. Use `--sample` to choose another
file.
