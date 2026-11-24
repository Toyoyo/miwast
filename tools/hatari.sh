#!/bin/bash
# hatari.sh - run dist/st/MIWA.PRG on an emulated 2MB STE in mono.
#
#   tools/hatari.sh [extra hatari options]
#
# Clean configuration only: Hatari's --configfile is read *on top of*
# ~/.config/hatari/hatari.cfg, so HOME points at an empty directory here and
# the user's own setup is never seen. Everything else is emu/hatari-test.cfg
# and the options below.
#
# STDIR=<dir> is the hard disk instead of dist/st. Headless unless VISIBLE=1. With FIFO=<path>, Hatari reads remote commands
# from that fifo ("hatari-event keypress 57", "hatari-shortcut screenshot");
# screenshots go to build/shots.

set -e
here=$(cd "$(dirname "$0")/.." && pwd)

emuhome="$here/build/emuhome"
rm -rf "$emuhome"
mkdir -p "$emuhome" "$here/build/shots"

if [ "$VISIBLE" != 1 ]; then
    export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
fi

# emu/hatari-test.cfg as given, plus where a sound recording goes
# ("hatari-shortcut recsound" toggles it).
cfg="$here/build/hatari.cfg"
{
    cat "$here/emu/hatari-test.cfg"
    printf '\n[Sound]\nszYMCaptureFileName = %s\n' "$here/build/sound.wav"
} > "$cfg"

args=(
    --configfile "$cfg"
    --tos "$here/emu/tos206us.img"
    --machine ste --memsize 2 --monitor mono
    --harddrive "${STDIR:-$here/dist/st}" --gemdos-drive C
    --auto 'C:\MIWA.PRG'
    --natfeats yes
    --fast-boot yes
    --screenshot-dir "$here/build/shots"
    --screenshot-format png
    --confirm-quit no
)
[ -n "$FIFO" ] && args+=(--cmd-fifo "$FIFO")
# FAST=1: a 32MHz CPU, and emulated time run as fast as the host can, for
# test runs that check what the game does rather than how long it takes.
# The game's clock is the emulated 200Hz counter, so it still behaves; only
# measurements of speed mean nothing.
[ "$FAST" = 1 ] && args+=(--cpuclock 32 --fast-forward yes)

# No session bus: SDL asks D-Bus for one, and with none in the environment
# libdbus autolaunches its own, which starts the document portal and gvfs
# with this HOME -- they mount themselves inside it, outlive Hatari, and the
# rm -rf above then fails. An address that leads nowhere stops the
# autolaunch.
HOME="$emuhome" XDG_CONFIG_HOME="$emuhome" \
    DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent \
    exec hatari "${args[@]}" "$@"
