#!/bin/bash
# drive.sh - run the game in Hatari and drive it from a command list.
#
#   tools/drive.sh <commands-file> [extra hatari options]
#
# One command per line, # comments allowed:
#
#   wait <seconds>      let it run (host seconds)
#   key <scancode>      press and release a key (57 Space, 28 Return,
#                       0x1 Esc -- a single digit is taken as a character)
#   down <scancode>     hold a key down (29 Control), up <scancode> to let go
#   shot <name>         screenshot to build/shots/<name>.png
#   rec                 start/stop recording sound to build/sound.wav
#   event <event>       any hatari-event: rightdown, rightup, doubleclick
#                       (the left button); Hatari cannot move the mouse
#   until <n> <regex>   wait for the n-th trace line matching regex
#
# Hatari is killed at the end of the list; NatFeats trace output is in
# build/drive.log.

set -e
here=$(cd "$(dirname "$0")/.." && pwd)
cmds=$1
shift

fifo="$here/build/fifo"
log="$here/build/drive.log"
shots="$here/build/shots"
mkdir -p "$here/build"
rm -f "$fifo" "$shots"/grab*.png

FIFO="$fifo" "$here/tools/hatari.sh" "$@" > "$log" 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null; wait $pid 2>/dev/null || true' EXIT

# Hatari creates the fifo itself once it is up.
for i in $(seq 100); do [ -p "$fifo" ] && break; sleep 0.1; done
[ -p "$fifo" ] || { echo "hatari did not start" >&2; exit 1; }

send() { echo "$1" > "$fifo"; }

while read -r cmd arg; do
    case "$cmd" in
    ''|'#'*) ;;
    wait) sleep "$arg" ;;
    key)  send "hatari-event keypress $arg"; sleep 0.3 ;;
    down) send "hatari-event keydown $arg"; sleep 0.1 ;;
    up)   send "hatari-event keyup $arg"; sleep 0.1 ;;
    rec)  send "hatari-shortcut recsound"; sleep 0.3 ;;
    event) send "hatari-event $arg"; sleep 0.3 ;;
    until)
        # until <n> <regex>: wait (15 minutes at most) for the trace to have n
        # lines matching
        n=${arg%% *}; re=${arg#* }
        for i in $(seq 9000); do
            [ "$(grep -a -c -E -- "$re" "$log" || true)" -ge "$n" ] && break
            # Hatari gone (it quit, crashed or was killed): nothing more
            # will come.
            kill -0 $pid 2>/dev/null || { echo "hatari exited" >&2; exit 1; }
            sleep 0.1
        done
        ;;
    shot)
        send "hatari-shortcut screenshot"
        for i in $(seq 50); do
            f=$(ls "$shots"/grab*.png 2>/dev/null | head -1)
            [ -n "$f" ] && break
            sleep 0.1
        done
        [ -n "$f" ] || { echo "no screenshot for $arg" >&2; exit 1; }
        sleep 0.2
        mv "$f" "$shots/$arg.png"
        echo "shot $arg"
        ;;
    *) echo "unknown command: $cmd" >&2; exit 1 ;;
    esac
done < "$cmds"
