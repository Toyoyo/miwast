#!/bin/bash
# check-st.sh - the branching fixture played by the ST, against the compiler.
#
#   tools/check-st.sh <lang> [answers [refused]]   e.g. tools/check-st.sh 0 1,0,2
#
# Builds tools/fixtures/branch into build/fixture (its art copied from art/,
# which is only read), then a NatFeats autoplay ST build that answers the
# menus with <answers> (0 is the first option shown; past the list, 0) and
# starts in language <lang> (0 French, 1 English). Hatari plays it to the
# end; every say, menu and the end, with the display state, are traced
# ("path: ..."), and diffed against the compiler's walk of the same path
# (MWC_TRACE). Each run starts with no MIWA.SAV, logs or MIWA.PER, as the
# compiler's walk assumes.
#
# An answer can also be something done at that menu first (vm.c, "the test
# build's player"): -k roll back k times, -100-k forward, -200-k back then
# forward, -300-s save in slot s, -400-s load it. tools/rbtrace.awk takes
# out of the ST's trace what was rolled back over or loaded away from, and
# checks each rollforward re-answers as before; the rest is diffed against
# the compiler's walk of the answers left in it. A roll the ST cannot do is
# traced ("refused"): <refused> is how many the test expects (default 0) --
# a lost future otherwise looks like a path that was simply clicked through.
#
# Leaves build/fixture/{st,eff,mwc}.trace. Exit status 0 when they agree.

set -e
here=$(cd "$(dirname "$0")/.." && pwd)
cd "$here"

lang=${1:?usage: $0 <lang> [answers]}
ans=${2:-0}
want_refused=${3:-0}
F=build/fixture
G=tools/fixtures/branch/game

mkdir -p "$F/art"
cp -rn art/. "$F/art/"
make -s pak GAME=$G DIST=$F ART=$F/art > "$F/build.log"
make -s st GAME=$G DIST=$F ART=$F/art NATFEATS=1 AUTOPLAY=1 \
    ANSWERS="$ans" STLANG="$lang" >> "$F/build.log"
rm -f "$F/st/MIWA.SAV" "$F/st/MIWA.PER" "$F"/st/MIWA*.LOG

cat > "$F/cmds" <<'EOF'
until 1 path: (end|cut)|fatal|vm: the replay
EOF
FAST=1 STDIR="$here/$F/st" tools/drive.sh "$F/cmds"

grep -a '^path: ' build/drive.log > "$F/st.trace" || true
# The ST's own complaints: a resume or a rollforward the log disagrees with.
if grep -a -E '^vm: (resumed at|rolled forward to|the replay)' build/drive.log; then
    echo "check-st: the ST complained (above)"
    exit 1
fi
if ! LC_ALL=C awk -f tools/rbtrace.awk "$F/st.trace" > "$F/eff.full"; then
    echo "check-st: language $lang, answers $ans: the log went wrong (above)"
    exit 1
fi
grep -a -v '^#' "$F/eff.full" > "$F/eff.trace" || true
eff=$(LC_ALL=C sed -n 's/^#answers //p' "$F/eff.full")
refused=$(LC_ALL=C sed -n 's/^#refused //p' "$F/eff.full")
if [ "$refused" != "$want_refused" ]; then
    echo "check-st: language $lang, answers $ans: $refused rolls refused," \
         "$want_refused expected"
    exit 1
fi
MWC_TRACE="$lang ${eff:-0}" ./tools/mwc $G "$F/check.mwb" "$F/check.txt" 2>/dev/null |
    grep -a '^path: ' > "$F/mwc.trace"

if diff "$F/mwc.trace" "$F/eff.trace" > "$F/trace.diff"; then
    echo "check-st: language $lang, answers $ans: $(wc -l < "$F/eff.trace")" \
         "lines agree ($(wc -l < "$F/st.trace") traced, answers ${eff})"
else
    echo "check-st: language $lang, answers $ans: the ST differs" \
         "(< compiler, > ST):"
    head -20 "$F/trace.diff"
    exit 1
fi
