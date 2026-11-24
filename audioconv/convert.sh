#!/bin/sh
# Convert an audio file into an Atari STE SNDH (DMA sound, decoded on the fly).
# usage: convert.sh rice|adpcm <input audio> <output.sndh> [--title "Title"] [--preview out.wav]
#   rice   lossless, ~40-45% smaller than raw 8-bit 12.5 kHz
#   adpcm  4-bit ADPCM, half the raw size, some hiss
# Needs python (with requirements.txt installed) and vasmm68k_mot in PATH.
set -e
here=$(cd "$(dirname "$0")" && pwd)
if [ $# -lt 3 ]; then
  sed -n '3,6s/^# \{0,1\}//p' "$0" >&2; exit 2
fi
codec=$1; in=$2; out=$3; shift 3
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
"${PYTHON:-python}" "$here/convert_codec.py" "$in" "$codec" --outdir "$tmp" "$@"
"$here/build.sh" "$codec" "$tmp" "$out"
ls -l "$out"
