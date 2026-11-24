#!/bin/bash

if [ "$2" = "" ]
then
  echo "Usage: [input file] [out file] (border)"
  exit 1
fi

basedir=$(dirname "$0")

infile=$(mktemp)
rm "$infile"
outfile=$(mktemp)
rm "$outfile"

ffmpeg -loglevel error -y -i "$1" -vf "format=rgb24" "${infile}.ppm"

"$basedir"/gdither -K atkinson -k 0.65 -i -n 48 -f 3 -c 16 -u 0.7 "${infile}.ppm" "${outfile}.pgm"

convert "${outfile}.pgm" "$2"

# PPM and PGM have no alpha: put the input's back, all-or-nothing at 50%.
if [ "$(identify -format '%A' "$1" | head -c1)" != F ]; then
    ffmpeg -loglevel error -y -i "$1" -lavfi \
        "[0]format=rgba,alphaextract,lut=y='if(gte(val\,128)\,255\,0)'" \
        "${infile}.mask.png"
    convert "$2" "${infile}.mask.png" -alpha off -compose CopyOpacity \
        -composite "$2"
    rm "${infile}.mask.png"
fi

rm "${infile}.ppm" "${outfile}.pgm"

if [ "$3" == "border" ]
then
  convert "$2" \( +clone -fill black -colorize 100 \) \
    \( -clone 0 -alpha extract -morphology EdgeIn Diamond:1 \) \
    -compose over -composite "$2" 2>/dev/null
  convert "$2" \( +clone -alpha extract -morphology EdgeOut Diamond:1 \
    -background white -alpha shape \) \
    -compose DstOver -composite "$2" 2>/dev/null
fi
