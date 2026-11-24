#!/bin/sh
# Assemble an SNDH from the files convert_codec.py wrote into <workdir>.
# usage: build.sh rice|adpcm <workdir> <output.sndh>
set -e
here=$(cd "$(dirname "$0")" && pwd)
[ $# -eq 3 ] || { echo "usage: $0 rice|adpcm <workdir> <output.sndh>" >&2; exit 2; }
case "$1" in
  rice)  defs=-DCODEC=1 ;;
  adpcm) defs=-DCODEC=2 ;;
  *) echo "unknown codec: $1 (rice or adpcm)" >&2; exit 2 ;;
esac
vasmm68k_mot -quiet -Fbin $defs -I"$2" -o "$3" "$here/sndh_stream.s"
