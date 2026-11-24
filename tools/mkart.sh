#!/bin/bash
# mkart.sh - dither every image the script uses into art/, for hand editing.
#
#   tools/mkart.sh <game-dir> <manifest.txt> <art-dir> [name...]
#
# Writes art/bg/<name>.png (black and white, 640x360) and
# art/sprite/<name>.png (black, white and fully transparent, at half scale),
# Atkinson-dithered from the original. An existing file is never touched:
# art/ is the source of truth once it exists, and `make data` reads only
# that. To regenerate one image from the original, delete it, or name it on
# the command line (the basename, e.g. "tamae talk") to overwrite it.
# Files in art/ that the manifest does not name are listed as unused.
#
# Flipped sprites have no file of their own; `make data` flips the unflipped
# art, so an edit shows up both ways round.

set -e

[ $# -ge 3 ] || { echo "usage: $0 <game-dir> <manifest> <art-dir> [name...]" >&2; exit 2; }
game=$1 manifest=$2 art=$3
shift 3
force=("$@")

mkdir -p "$art/bg" "$art/sprite"
pal=$(mktemp --suffix=.png)
trap 'rm -f "$pal"' EXIT
# A 16x16 palette of pure black and pure white: paletteuse wants 256 entries.
convert -size 16x16 xc:white -fill black -draw 'rectangle 0,0 7,15' \
    -type TrueColor "png24:$pal" 2>/dev/null

forced() {
    local n
    for n in "${force[@]}"; do [ "$n" = "$1" ] && return 0; done
    return 1
}

made=0 kept=0
declare -A seen
while IFS=$'\t' read -r tag out kind flip w h artfile src; do
    [ "$tag" = IMG ] || continue
    name=$(basename "$src" .png)
    # The compiler chose the art file: one per source, whatever the flips,
    # and under bg/ for a source that is ever a backdrop.
    dst="$art/$artfile"
    [ -n "${seen[$dst]}" ] && continue
    seen[$dst]=1

    if [ -e "$dst" ] && ! forced "$name"; then
        kept=$((kept + 1))
        continue
    fi

    if [ "${src#00-renpythm/}" != "$src" ]; then
        # The rhythm game's notes: a white arrow on a coloured disc. Any
        # dither turns the light green and pink discs near-white and the
        # arrow goes with them; so the arrow (near-white in the source)
        # white, the rest black, then the white outline outside as below.
        thr="if(gte(min(min(r(X,Y),g(X,Y)),b(X,Y)),200),255,0)"
        ffmpeg -loglevel error -y -i "$game/$src" -lavfi \
            "scale=$w:$h:flags=lanczos,format=rgba,\
             geq=r='$thr':g='$thr':b='$thr':a='if(gte(alpha(X,Y),128),255,0)'" \
            "$dst"
        convert "$dst" \( +clone -alpha extract -morphology EdgeOut Diamond:1 \
            -background white -alpha shape \) \
            -compose DstOver -composite "$dst" 2>/dev/null
    elif [ "${artfile%%/*}" = bg ]; then
        ffmpeg -loglevel error -y -i "$game/$src" -i "$pal" -lavfi \
            "[0]scale=$w:$h:flags=lanczos,format=rgb24[s];[s][1]paletteuse=dither=atkinson" \
            "$dst"
    else
        # Colour and coverage are separated: the colour is dithered on its
        # own, and the alpha becomes all-or-nothing at 50%, so a soft edge is
        # not blended into white first and dithered into a speckled halo.
        ffmpeg -loglevel error -y -i "$game/$src" -i "$pal" -lavfi \
            "[0]scale=$w:$h:flags=lanczos,format=rgba,split[c][a];\
             [c]format=rgb24[c2];[c2][1]paletteuse=dither=atkinson,format=rgba[d];\
             [a]alphaextract,lut=y='if(gte(val\,128)\,255\,0)'[m];\
             [d][m]alphamerge" \
            "$dst"
        # A 1-pixel black outline on the inside of the mask edge: the
        # original's thin line art does not survive halving and dithering,
        # and a light sprite would otherwise melt into a light backdrop.
        # Inside, so the size and the mask -- which the script's positions
        # were resolved with -- do not change. The image border is not an
        # edge (EdgeIn sees the virtual pixels beyond it as more of the
        # same), so feet cut off by the bottom of the frame stay open.
        convert "$dst" \( +clone -fill black -colorize 100 \) \
            \( -clone 0 -alpha extract -morphology EdgeIn Diamond:1 \) \
            -compose over -composite "$dst" 2>/dev/null
        # Then a 1-pixel white outline just outside it (the user's choice),
        # so a dark sprite stands out from a dark backdrop too. It goes in
        # the transparent margin, so the size stays; where the sprite meets
        # the frame there is no room and none is drawn.
        convert "$dst" \( +clone -alpha extract -morphology EdgeOut Diamond:1 \
            -background white -alpha shape \) \
            -compose DstOver -composite "$dst" 2>/dev/null
    fi
    echo "  made $dst"
    made=$((made + 1))
done < "$manifest"

# Art the manifest no longer names is listed, never removed: an image that
# changed kind (a sprite become a backdrop, or back) is made afresh under its
# new directory, and a hand edit of the old file would otherwise be lost
# without a word.
unused=0
while IFS= read -r -d '' f; do
    [ -n "${seen[$f]}" ] && continue
    echo "  unused $f"
    unused=$((unused + 1))
done < <(find "$art/bg" "$art/sprite" -name '*.png' -print0 | sort -z)

echo "art: $made made, $kept kept as they were, $unused unused"
