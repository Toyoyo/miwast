#!/bin/bash
# jarvisdither.sh - one image, dithered every way we have tried, to compare.
#
#   tools/jarvisdither.sh <image> <output-dir>
#
# Takes the full-size original (e.g. game/images/miwaclass.png), halves it as
# mkart.sh does (lanczos, odd sizes rounded up like the compiler's
# half_size), and writes into <output-dir>:
#
#   original.png       the halved image, undithered, for reference
#   atkinson.png       mkart.sh's dither, the default in art/; on flat
#                      mid-tones it falls into regular "\\\" stripes
#   atk-serp.png       Atkinson scanned serpentine: the stripes become
#                      chevrons, still regular
#   atk-n32.png        serpentine, and the threshold jittered by up to 32
#   atk-n48.png        or 48 /255ths of lightness at mid-grey, fading fast
#                      (gdither -f 3) so light areas stay as clean as
#                      atkinson.png's: the cycles are broken, for a little
#                      grain. 32 leaves a faint diagonal on flat greys, 48
#                      none
#   atk-n48c8.png      atk-n48 jittered on flat areas only (gdither -c 8),
#                      so edges, lines and small shapes are as crisp as
#                      atkinson.png's; at -c 4 gentle shading starts to
#                      stripe again
#   atk-n48c16.png     the same, jittered on a little more than flat
#   atk-c16-k65.png    atk-n48c16 passing on 0.65 of the error, not 0.75:
#                      more contrast overall, cleaner whites, denser darks
#   atk-c16-u05.png    atk-n48c16 sharpened first (gdither -u 0.5 or 1):
#   atk-c16-u1.png     edges and small shapes stand out; at 1, light halos
#                      start to show beside dark edges
#   atk-c16-k65u07.png both, at -k 0.65 -u 0.7
#   jarvis.png         Jarvis-Judice-Ninke, all the error passed on
#   jarvis75.png       Jarvis passing 3/4 of it, Atkinson's contrast
#   steer.png          gradient-steered (gdither.c), strongly: streaky
#   steer75.png        the same at 3/4
#   s2-e128-k75.png    steered gently, on strong edges only; the name is
#   s2-e256-k75.png    gdither's steer, edge and keep (75 = 0.75)
#   s1-e192-k75.png
#
# All but atkinson.png are gdither's, which dithers as ffmpeg does (in
# colour, black or white chosen in OkLab); the atk-* ones in whole numbers
# too (-i), as ffmpeg's integer arithmetic drops errors too small to share,
# which is what keeps its near-whites white. So their tones and clean whites
# are atkinson.png's; only the pattern changes.
#
# Nothing in art/ is touched: pick one, or mix parts of several in an
# editor, and copy the result over the art file by hand. An image with
# alpha gets mkart.sh's all-or-nothing mask at 50%, but not the outlines
# mkart.sh draws around sprites.

set -e

[ $# -eq 2 ] || { echo "usage: $0 <image> <output-dir>" >&2; exit 2; }
src=$1 out=$2
here=$(dirname "$0")

make -s -C "$here/.." tools/gdither
mkdir -p "$out"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

read -r w h < <(identify -format '%w %h\n' "$src" | head -1)
w=$(((w + 1) / 2)) h=$(((h + 1) / 2))
scale="scale=$w:$h:flags=lanczos"

ffmpeg -loglevel error -y -i "$src" -vf "$scale" "$out/original.png"
ffmpeg -loglevel error -y -i "$src" -vf "$scale,format=rgb24" "$tmp/in.ppm"

# Atkinson exactly as mkart.sh makes a backdrop.
convert -size 16x16 xc:white -fill black -draw 'rectangle 0,0 7,15' \
    -type TrueColor "png24:$tmp/pal.png" 2>/dev/null
ffmpeg -loglevel error -y -i "$src" -i "$tmp/pal.png" -lavfi \
    "[0]$scale,format=rgb24[s];[s][1]paletteuse=dither=atkinson" \
    "$out/atkinson.png"

# name, then gdither's options
while read -r name opts; do
    "$here/gdither" $opts "$tmp/in.ppm" "$tmp/out.pgm"
    convert "$tmp/out.pgm" "$out/$name.png"
done <<'EOF'
atk-serp -K atkinson -k 0.75 -i
atk-n32 -K atkinson -k 0.75 -i -n 32 -f 3
atk-n48 -K atkinson -k 0.75 -i -n 48 -f 3
atk-n48c8 -K atkinson -k 0.75 -i -n 48 -f 3 -c 8
atk-n48c16 -K atkinson -k 0.75 -i -n 48 -f 3 -c 16
atk-c16-k65 -K atkinson -k 0.65 -i -n 48 -f 3 -c 16
atk-c16-u05 -K atkinson -k 0.75 -i -n 48 -f 3 -c 16 -u 0.5
atk-c16-u1 -K atkinson -k 0.75 -i -n 48 -f 3 -c 16 -u 1
atk-c16-k65u07 -K atkinson -k 0.65 -i -n 48 -f 3 -c 16 -u 0.7
jarvis -K jarvis -k 1
jarvis75 -K jarvis -k 0.75
steer -s 6 -e 64 -k 1
steer75 -s 6 -e 64 -k 0.75
s2-e128-k75 -s 2 -e 128 -k 0.75
s2-e256-k75 -s 2 -e 256 -k 0.75
s1-e192-k75 -s 1 -e 192 -k 0.75
EOF

if [ "$(identify -format '%A' "$src" | head -c1)" != F ]; then
    ffmpeg -loglevel error -y -i "$src" -lavfi \
        "[0]$scale,format=rgba,alphaextract,lut=y='if(gte(val\,128)\,255\,0)'" \
        "$tmp/mask.png"
    for f in "$out"/*.png; do
        [ "$f" = "$out/original.png" ] && continue
        convert "$f" "$tmp/mask.png" -alpha off -compose CopyOpacity \
            -composite "$f"
    done
fi

echo "dithered $(basename "$src") ${w}x$h into $out/"
