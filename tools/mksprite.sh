#!/bin/bash
# mksprite.sh - one sprite, dithered as jarvisdither.sh's atk-c16-k65u07.png,
# with mkart.sh's outlines.
#
#   tools/mksprite.sh <image> <out.png>     e.g. game/images/miwa.png x.png
#
# Halves the full-size original (lanczos, odd sizes rounded up like the
# compiler's half_size), dithers it with gdither at -K atkinson -k 0.65 -i
# -n 48 -f 3 -c 16 -u 0.7 (GDITHER="<options>" for another of
# jarvisdither.sh's), then does to it what mkart.sh does to a sprite: the
# alpha all-or-nothing at 50%, a 1-pixel black outline inside the mask edge
# and a 1-pixel white one just outside it.
#
# <out.png> must not exist: art/ is hand-edited, so nothing is overwritten.
# Write it elsewhere and copy it over the art file by hand.

set -e

[ $# -eq 2 ] || { echo "usage: $0 <image> <out.png>" >&2; exit 2; }
src=$1 out=$2
here=$(dirname "$0")
opts=${GDITHER:--K atkinson -k 0.65 -i -n 48 -f 3 -c 16 -u 0.7}

[ -e "$out" ] && { echo "$out exists; not overwriting it" >&2; exit 1; }
[ "$(identify -format '%A' "$src" | head -c1)" != F ] ||
    { echo "$src has no alpha: not a sprite" >&2; exit 1; }

make -s -C "$here/.." tools/gdither
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

read -r w h < <(identify -format '%w %h\n' "$src" | head -1)
w=$(((w + 1) / 2)) h=$(((h + 1) / 2))
scale="scale=$w:$h:flags=lanczos"

# The colour and the coverage apart, as mkart.sh does.
ffmpeg -loglevel error -y -i "$src" -vf "$scale,format=rgb24" "$tmp/in.ppm"
ffmpeg -loglevel error -y -i "$src" -lavfi \
    "[0]$scale,format=rgba,alphaextract,lut=y='if(gte(val\,128)\,255\,0)'" \
    "$tmp/mask.png"
"$here/gdither" $opts "$tmp/in.ppm" "$tmp/out.pgm"
convert "$tmp/out.pgm" "$tmp/mask.png" -alpha off -compose CopyOpacity \
    -composite "$tmp/s.png"

# mkart.sh's outlines, as it draws them (see there): black inside the mask
# edge, then white just outside it; none along the image border.
convert "$tmp/s.png" \( +clone -fill black -colorize 100 \) \
    \( -clone 0 -alpha extract -morphology EdgeIn Diamond:1 \) \
    -compose over -composite "$tmp/s.png" 2>/dev/null
convert "$tmp/s.png" \( +clone -alpha extract -morphology EdgeOut Diamond:1 \
    -background white -alpha shape \) \
    -compose DstOver -composite "$tmp/s.png" 2>/dev/null

mv "$tmp/s.png" "$out"
echo "made $out (${w}x$h, gdither $opts)"
