# miwast — Miwa no Utsuroi on the Atari STE

A port of the Ren'Py game to a 2MB STE (DMA sound, blitter), monochrome
high resolution, 640x400. Hard disk only; the SNDH tracks are 0.8–1.3MB each.

`../minirenpy-video` is the C89 DOS/Win32s engine: read-only from here. Its
parser is copied verbatim into `tools/rpy/`, with one addition: a POSIX
branch in `vfs.c`'s `mr_vfs_enum`, so `tl.c` can find the translation files
as loose files on Linux. Its `NOTES.md` is the record of bugs not to
repeat, and was read before writing rollback and skip.

## Build

    make            script → images → data → pak (dist/MIWA.PAK)
    make st         + dist/st/MIWA.PRG, beside a copy of the pak: what goes on the disk
    make clean      never removes art/

| step | what | output |
|---|---|---|
| `make script` | `tools/mwc` parses `game/*.rpy` with the C89 parser and walks it in execution order | `dist/SCRIPT.MWB`, `dist/TEXT_FR.MWT`, `dist/TEXT_EN.MWT`, `dist/manifest.txt`, `dist/script.dump` (`tools/mwdump`, both languages) |
| `make images` | `tools/mkart.sh`: Atkinson dither + inner 1px black outline and outer 1px white one for sprites (the white added to all of art/sprite by hand 2026-11-13) | **`art/bg`, `art/sprite` — hand-edited by the user (mouths etc.)**. Only missing files are made; `REDO="name"` overwrites named ones |
| `make data` | `tools/mwimg`: art → `.MBG` / `.MSP`, flips applied here | `dist/data/` |
| `make pak` | `tools/mwpak` | `dist/MIWA.PAK` |

Never regenerate `art/` wholesale: it is source now. Check mtimes before touching it.
To try other dithers, outside art/: `tools/jarvisdither.sh <image> <dir>`
(every variant, no outlines) and `tools/mksprite.sh <image> <out.png>` (one
sprite as atk-c16-k65u07, `GDITHER=` for other options, with mkart.sh's
outlines; never overwrites). `mkart.sh` on one sprite elsewhere: grep its
line out of `dist/manifest.txt` and give a scratch art dir.

Test variants (Hatari only — NatFeats opcodes crash a real ST):
`make st NATFEATS=1` (trace to the host), `AUTOPLAY=1` (plays itself, every
transition included), `STOPAT=<byte offset from dist/script.dump>` (hands back
to the keyboard there; transitions are dropped on the way), `SLOW=<n>`
(transitions n times slower, for screenshots mid-way), `SPRBUDGET=<bytes>`
(a smaller sprite arena, as on a machine with less free), `SELFTEST=1` (blitter
sprites against the CPU reference, 380 placements, at start, then the
dissolve stress). **`SELFTEST=1` needs `SPRBUDGET=300000`** or so: its
buffers are allocated after the sprite arena has taken nearly everything,
and without room it returns silently — no `selftest:`/`stress:` lines at
all. Run with `until 1 stress: [0-9]+ of 300|fatal`, `FAST=1` (~45s).
The build prints a linker map to `build/MIWA.map`; text starts at the address
`main: text at` traces.

## Hatari

Only through `tools/hatari.sh` / `tools/drive.sh`. `--configfile` is read *on
top of* `~/.config/hatari/hatari.cfg` (the user's own), so these give Hatari an
empty HOME and use `emu/hatari-test.cfg` + `emu/tos206us.img`, 2MB STE, mono,
`dist/st` as C: (`STDIR=` another). Headless unless `VISIBLE=1`. **`FAST=1`**:
32MHz CPU + fast-forward, for runs that check behaviour (the game's clock is
the emulated 200Hz one, so it still behaves; timings mean nothing) — a run to
offset 40000 is 17s instead of 10+ minutes. Measure at 8MHz.
hatari.sh points `DBUS_SESSION_BUS_ADDRESS` nowhere: from a shell without
a session bus, SDL made libdbus autolaunch one, which started the document
portal and gvfsd-fuse with the empty HOME; they mounted inside
build/emuhome, outlived Hatari, and the next run could not clear it ("hatari
did not start"). If it happens again: `grep emuhome /proc/mounts`.

Keys (`emu/hatari-keymap.txt`, named by absolute path in the test cfg):
Page Up is the ST's Undo (rollback), Page Down its Help (rollforward) —
Ren'Py's own keys for them. Hatari's defaults are Scroll Lock and Print
Screen. `--trace keymap` shows the file being read.

`drive.sh <cmds>`: `wait s`, `key sc`, `down sc`/`up sc` (hold), `shot name`,
`rec` (sound to build/sound.wav), `until <n> <regex>` (wait for the n-th
matching trace line -- time shots against what the program did, not the
clock: TOS boot and loading vary). Scancodes: 57 Space, 72 Up, 80 Down, 29 Ctrl;
**a single digit is a character**, so Esc is `0x1`. Stop with `pkill -x hatari`
(`until` then gives up at once; it used to poll on for 15 minutes and fight
the next run over drive.log). Read the log for the exact text before writing
an `until` regex: "vm: keyframes" never matches "vm: 45 keyframes", and a
wrong regex looks exactly like a hung emulator.

`event rightdown`/`rightup`/`doubleclick` send Hatari's mouse events; Hatari
cannot move the mouse from a script (a nested `Xephyr` with `xdotool` can, if
its window has the focus).

**`--auto` is not a full desktop.** Hatari starts the program with the VDI up
but without the desktop's memory in use, so it hides the real 2MB problem: on a
2MB STE with a full GEM desktop there is not enough memory, and the game is
started before GEM instead, by `miwa_stub/` (`AUTO\MIWALDR.PRG` runs
`\MIWA\MIWA.PRG`, then resets). To test that path, `STDIR=` a directory holding
`AUTO/` and `MIWA/`. Before GEM there is no VDI: no mouse handler, Line-A's
INTIN is NULL — hence the game's own mouse (`src/mouse.s`, pointer drawn by
the VBL in gfx.c), which must keep working under GEM too. The stub keeps a 2KB
stack (`_stksize`): libcmini's default cost ~18KB of sprite arena, and the
credits' 115KB sprite no longer fitted (budget 112,620).

Crashes: a screenshot after the fact shows the desktop — TOS recovers from the
bombs. Use `--trace cpu_exception` and read the log after `exit`.

## Formats (src/mwb.h, version 5)

Script: header, image/character/music tables, a command stream of 16-bit words
(lengths in `MWB_CMD_WORDS`). Text is by string number; each language has a
`.MWT` of them (Atari charset, inline style codes, lines pre-wrapped for that
language, 78 cols × 4). Positions are final 640x360 pixels;
the compiler simulated the scene list (`scene.c` placement and `image.c`
attribute matching, copied with citations). `SHOW` carries the z index, which
is how `behind` works. Where it differs from `scene.c`: a re-show with a
placement starts from the `default` transform's state if the tag had none
(`take_state`, scenelists.py:306,444), so `show park:` + `yoffset 400` moves
it down from the bottom-centre. And a line indented under one without a colon
is an error, as in Ren'Py (lexer.py:930); the shared parser skipped it
silently.

The backdrop. Ren'Py's `scene X` is a clear then a show of tag X: the image is
the master layer's bottom entry, and later statements name it by its tag. The
ST's `.MBG` is one screen copied whole, so only a full-stage image at the
unplaced position can be one. Any other scene image (`park`, 1280x1460) is
compiled as black plus a bottom slot, bottom-centre. A full-stage one is the
backdrop until a statement needs it as a tag:
- an unplaced re-show of a full-stage image (`show gozan light`) swaps the
  backdrop under the sprites. The old compiler put an opaque sprite on top,
  hiding the characters at script.rpy:2787 and wherever `show miwaroom night`
  ran with Miwa up;
- a placed or ATL re-show, or a `behind` naming it, makes it the bottom slot
  (`bg_to_slot`), then `with None` so the ST's move glides it (`move_plan`
  glides only a tag already in the baseline). After other stage changes since
  the last interaction, that `with None` would drop them out of the transition,
  so it is left out and the compiler warns: show the backdrop's tag first;
- `hide X` clears it to black (a `moveout` cuts where Ren'Py would slide it).
`tools/fixtures/backdrop` has a case for each, diffed by `make check`.

Images: `.MBG` 28800 raw bytes; `.MSP` header + mask plane + data plane
(opaque sprites have no mask). Uncompressed on purpose (disk is faster than an
8MHz inflate). Pak: sorted directory of 8.3 names.

## Branching (for Hinata; steps 1–3 done: compiler, runtime, rollback log)

Plan: 1 compiler, 2 runtime (variables, `if`, menus, the menu screen,
`[var]`, persistent on disk), 3 rollback log and saves through menus. The
minirenpy lessons that apply (menu = checkpoint, the answer and the future
saved, resume *into* a menu after a load, no duplicate checkpoint on resume,
two inputs in one frame) are for steps 2–3.

**Format 5.** Commands `GOTO pc`, `IF expr pc` (pc taken when false), `SET var
expr`, `MENU who str opt nopt pc` (pc: none offered). Tables: variables
(`FNV-1a(name)`, persistent flag, initial value), expressions (RPN words, end
with `EX_END`), options (`str cond pc`). Values are a type (None/bool/int) and
16 bits; Python's semantics (`and`/`or` give an operand, `True == 1`, None
equals only None). `[var]` in a say is `MWB_TXT_VAR` + `0x30+var`, and such a
line is not pre-wrapped (only the ST knows the width); the compiler checks it
fits 4 rows with every value at its widest (`-32768`). Ren'Py prints with
`format(value, "")` (`substitutions.py:100`): `True`, not minirenpy's `1`.

**The compile** (`compile_from`). The display state (backdrop, slots with
their image and half-scale position, title card, chapter, music, window,
skip/rollback flags) plus what the compiler needs to settle what follows
(each slot's placement and attributes, the `title` variable) is a `cstate`.
What follows a statement is a function of (statement, cstate), so every
statement compiled is recorded under that pair, and a path arriving at one
already compiled with an equal state emits `GOTO` and stops. Branches are
queued with the state they are reached with. Paths that leave the stage
differently are compiled once per version until it agrees again — usually the
next `scene`. Consequence: **the display state is still a function of the
position** (per language); only variables differ between visits. States
compare as bytes after zeroing the slots past the last (a `scene` leaves the
old ones behind; without that every path stayed duplicated until the end).

Constants: a name a `define`/`default` gives a literal and nothing assigns is
folded; an `if` the compiler can decide compiles only its branch; an option
whose condition is constant-false is left out (Ren'Py hides it,
`config.menu_include_disabled = False`, `config.py:985`,
`menuexports.py:101`). Variables: every name a `$` line assigns (not the
engine's: `_*`, `config.`, `preferences.`, `gui.`, `renpy.`, `title`,
`quick_menu`), and every `persistent.` name; `default` gives the initial
value, else None. Miwa's two `$ persistent.x = True` are now `SET`s — its code
differs from before by exactly those two commands, and its save signature
not at all.

**The path check** (`check_paths`, every build). Every sequence of menu
answers, in both languages, played twice: the compiled code run as the ST runs
it (`xrun`), and the source walked down that one path (`orun`) with the C89
engine's own evaluator (`expr.c`) for conditions and assignments. Both print,
at every say, menu and end, the text and the whole display state; they must
agree line for line. And per language, **every command must be reached with
one display state over all paths** — what the replay assumes. Fixture:
`tools/fixtures/branch/game` (Miwa's images by symlink), every form the parser
accepts: both caption forms, conditional options, a menu that can offer
nothing, nested menus, `if`/`elif`/`else` with `and`/`or`/`not` and
comparisons, jumps both ways and a loop, `default`/`define`/`persistent`,
`[var]`, branches that leave the stage, the music or the title different, a
language branch. 32 paths, all agree.

*Joins* (2026-11-13). A path stops at a menu ("joined") when it reaches it,
past its given answers, as an earlier path did. The compiled side decides
that: the menu's code offset (which names the compile state), the
variables and the display state. Everything after it was explored from that
first visit, where every answer was queued. The source side must agree: its
statement, the reference store's variables and its walk state are recorded
at the first visit and compared at each join. A mismatch is reported ("menu
N is reached as before by the compiled code, but not by the source").
Before this, a loop was played round to the 64-menu cut on every path. A
menu with two ways back doubled the paths at every lap (2⁶⁴: `make script`
never finished), and Miwa's test loop (`jump chap0` from the first menu)
took 258 paths and 10s. Now 6 paths and 0.3s, and the fixture 32 (was 976).
`MWC_TRACE` (check-st's side) never joins. Re-checked with each broken
version below, plus two for joins: a compiled key without the variables
(caught by the source-side compare) and one without the display state
(not caught, and harmless: the code offset already fixes the display
state, as "one display state per command" checks).

The check can fail — each mutant built in a scratch copy, run on the fixture
(recipe: `cp tools/mwc.c build/mut/`, `sed -i` the change there, then
`gcc -O2 -Itools/rpy -Isrc -o build/mut/mwc build/mut/mwc.c tools/rpy/*.c
-w -lm` and `build/mut/mwc tools/fixtures/branch/game build/mut/x.mwb
build/mut/x.txt`; check `cmp` says the sed changed something):
options ignoring their condition, `IF` going the wrong way, `!=` compiled as
`==`, a branch queued with the state reset, `SET` writing the wrong variable
(each a line-by-line difference); merging that ignores the music, the slots
or the `title` variable (each "code offset N is reached with two display
states"). The music one was first missed: each path still *plays* right,
because the shared code never sets the music; only the second rule sees it.
The `title` one was missed until the fixture got branches that set the title
differently and meet.

**Step 2, the runtime (done 2026-11-12).** `GOTO`/`IF`/`SET`/`MENU` run.
- *Keyframes follow the code* (`make_keyframes`): from the start, every
  branch of an `IF`, every option and the fall-through of a `MENU`, every
  `GOTO` target gets one, plus every 64th checkpoint (code order). So the
  code from a keyframe to the next is straight: a replay never meets a
  `GOTO`/`MENU` and goes on through an `IF` (anything after it not reached
  by a jump is reached by not jumping). `replay_to` traces if that is ever
  broken. Miwa: 45 keyframes (the old 44 and the top), 645ms at 8MHz
  (the file-order sweep was 455ms; the bound is counted in `vm_load`'s
  passes, 115ms saved, and the maps are tested a byte first). Rolling back
  3 and forward 1 at offset 40000: each re-run line's traced state is the
  same as the first time through.
- *The choice screen* (`vm.c choice`, `menu.c choice_draw`): the game's
  `screen choice` — buttons centred, middle at 270/720 (here the stage's
  middle, 180: the user's choice, as the text strip is below the stage
  rather than over it), 790 wide (400 here,
  wider for a long option), 22 apart; options whose condition fails are left
  out, none offered = carry on (`menuexports.py:101,116`). The caption types
  in the strip; a press finishes it, a click on a button chooses. Keys:
  arrows focus (Ren'Py's focus keys — Undo is rollback at a menu), Return
  chooses the focused one, Space finishes the caption; the pointer focuses
  what it is over. Esc/right click: the game menu; a language change puts the
  menu up again. Menus are checkpoints (as `save_sig` already counted them).
  Limits checked by `mwc`: 12 options, 70 characters (a value at 6).
- *Skip*: a menu ends it and a still-held Ctrl does not restart it until let
  go (`choice_for_skipping`, `menuexports.py:153`; skip starts on key down,
  `behavior.py:348`).
- *`[var]`*: printed as `format(value, "")` (None/True/False/number/the
  string) and the line wrapped on the ST by the compiler's greedy wrap. The
  compiler checks each line with every value at the widest one prints
  (-32768, or the longest string in the script), at the end of the compile
  once every string is known (`wide_check`).
- *Strings* (2026-11-13, format 6): `$ x = "..."`, `default x = "..."`,
  `==`/`!=`, ordering, `not`, truthiness. A value string table in the MWB,
  apart from the .MWT text, because Ren'Py prints a string value
  untranslated (`substitutions.py:100`; `!t` is the opt-in, `:282`). Each
  text is stored once, so equality is by number. Its `{i}`/`{b}` tags are
  read (Ren'Py reads tags after substituting) and its brackets are not (one
  substitution pass). At most 70 characters; escapes `\" \' \\ \n`. Ordering
  is by Atari bytes: the same as Python for ASCII, not for accented letters.
  Saves and `MIWA.PER` keep a string as FNV-1a of its text, since a rebuild
  renumbers the strings. A string this build lacks gives the variable its
  `default`; in `MIWA.PER` the old value is kept until the story sets it.
  Values on disk are now 6 bytes (`val_put`): the `.LOG` magic is `MWL2`,
  so older logs are ignored and those slots load from their checkpoint, as
  before logs existed. `MIWA.PER` is `MWP2`; an old `MWPS` file is still
  read. Checked: the fixture's path check, with both broken versions caught
  (truthiness, ordering); `check-st` `0 0,0,0,1,0,0`, `1 1,0,1`,
  `0 0,-8,1,0,1`, `0 -300,0,-400,1,0,1` and `1 0,-300,1,-400,0,0,1,0,0`
  (a string saved in a log and loaded back), with a broken hash-to-string
  mapping on load caught. Not checkable against the reference: `x or "y"`
  as a value. minirenpy's `expr.c:1122,1133` turns `and`/`or` into 0/1,
  where Python (and the ST) gives the operand. Also, a persistent value set
  in a stretch that check-st then rolls back over stays set on the ST, as in
  Ren'Py, but the compiler's walk never saw it, so the fixture sets its
  persistent string past those stretches. The strings added lines after
  the first menu, so the rollback counts in the runs listed under "Checked
  on the ST" (`-3`, `-8`) now land elsewhere. From the second menu, the
  first is now 8 back on the "Oui" path.
- *Persistent*: `MIWA.PER`, entries by name hash, unknown ones kept; loaded
  at start, written at the next interaction after a persistent value
  changes. Ren'Py writes at save/quit (`loadsave.py:172`, `main.py:608`); an
  ST is switched off, not quit — Ren'Py's mobile background save
  (`core.py:1903`) is the same reasoning.

**Step 3, the rollback log (done 2026-11-12).** Ren'Py's `RollbackLog`
(`rollback.py`): an entry per checkpoint reached — its offset, the story
variables there (not the persistent ones), and its answer (`ANS_NONE` while
first waited at; 0 for a line or pause; a menu's option, counted from its
first in the table). A ring of 128 (`config.rollback_length`): behind, then
the future. Rollback restores an entry's variables and replays the display
to it (the keyframes, as before); the entries rolled over become the future.
A re-run (after a rollback, a load, a language change) is the same entry,
not a new one (`g_resume`).
- *Rollforward runs live*, as `roll_forward_core` does: the line or menu is
  answered as last time — a menu only if that option is still offered
  (`menuexports.py:306-309`) — and the game runs to the next checkpoint with
  transitions and timed pauses dropped and music deferred, as a skip; that
  checkpoint takes the next future entry if it is the same offset, else the
  future goes. Only a rollforward keeps the future (`rollback.py:787`): a
  click, a skip, any answer chosen — even the same one — ends it; an
  untimed pause clicked keeps it (`keep_rollback`,
  `statementexports.py:196`). **Changed for Miwa too**: it used to roll
  forward over anything reached, clicked or not. NatFeats builds trace if a
  rollforward arrives with variables other than the log's.
- *Keys*: at a menu the arrows are focus, so Undo goes back and **Help**
  forward (Ren'Py's Page Down); Help also works where Down does.
- *Saves* carry the whole log, future too — minirenpy's "a save carries no
  future" gap is not here. `MIWA.SAV`'s records and preferences are as they
  were; each slot's log is its own file, `MIWAnn.LOG` (`vm.c log_write`):
  the script signature and the slot's date/time (a log is good only for
  its save), the variables by name hash (one the save lacks gets its
  `default`, as Ren'Py's store does), entries by checkpoint *number* (what
  the signature fixes; offsets move with staging edits). A load restores it
  and replays to its last entry, so a save at a menu resumes into the menu.
  A slot with no log (older saves) loads from its checkpoint as before,
  with — for a script with no branches, Miwa's — the checkpoints before it
  in the file as its past, up to its rollback limit. Checked on a copy of
  the existing `dist/st/MIWA.SAV`: slot 0 loads at checkpoint 467 and rolls
  back 3.

**Checked on the ST** (`tools/check-st.sh <lang> <answers>`, ~15s each
with `FAST=1`). Give answers that end the game: with action tokens
(`-k`, `-300-s`...) in the list, a path left to loop to the 64-menu cut
is cut at a different line on the ST, a false diff at the very end.
The fixture
autoplayed under Hatari with given menu answers (`ANSWERS=`, `STLANG=`),
every say/menu/end traced with the whole display state, diffed against the
compiler's walk of the same path (`MWC_TRACE="<lang> <answers>"`). Paths run,
all agreeing: French `0` (the loop, cut at 64 menus as the path check cuts
it, 138 lines), `0,1,0,0,0,1,0,1` both languages, `1,0,1,1` (a menu offering
nothing), English `1,2,0,1`. Mutant: `IF` inverted on the ST — diff at the
first condition. Screenshots: two buttons over Miwa's room, caption in the
strip, Down inverts "Oui", Return takes its branch.

The trace has the story variables and the untimed pauses too, and the
answers can be actions at that menu (vm.c "the test build's player"): `-k`
back k times, `-100-k` forward, `-200-k` both, `-300-s` save in slot s,
`-400-s` load it. `tools/rbtrace.awk` takes out of the ST's trace what was
rolled back over or loaded away from (checking each rollforward re-answers
as before); the rest must be the compiler's straight walk of the answers
left. A roll the ST cannot do is traced, and the third argument is how many
are expected — otherwise a lost future would look like a path clicked
through. All agree: the five above, plus `0,-3,1,…` (back over the first
menu, answered the other way), `0,-203,…` (back 3, forward 3 through it),
`0,1,-208,…` (8 back over two menus, 8 forward), `-300,0,-400,1,…` (save,
load, answer differently), `0,-3,-301,1,-401,-103,…` (save while rolled
back, load, roll forward through the future from the file; both
languages), `0,1,-8,-101,… 1` (the click ends the future: one refusal).
Mutants, each caught: no variable restore on rollback (diff), a rollforward
picking the next option (rbtrace), a save without its future (2 refusals),
a click keeping the future (rbtrace). By hand (`build/cmds-keys`, real
keys): Undo at a menu, Up, Up, Help re-answers the first menu, Down, Down,
Help at the unanswered second does nothing. Miwa at 40000
(`build/cmds-rb2`): back 3, forward 2, each re-run line's traced state the
same as the first pass; a click, then Down: nothing.

Hinata probe (scratch copy, its French relabelled `english`): no error from
its 9 menus, jumps or variable `if`s. What stops it is the rest of the port:
138 "bad text tag", 39 alpha/zoom, two transitions, `{cps}`, the rhythm
section's Python (`renpy.variant`, `preferences.get_mute`/`set_volume`, `_return`),
`mr.add`, `renpy.checkpoint()`, its original language being English, blurred
image declarations, a 1279px backdrop, and the SNDH conversions.

## Runtime (src/)

`main.c` start/input/exit, `pak.c`, `gfx.c` (two screens, flip at VBL, Line-A
8x8 font — TOS's own glyphs cover every French letter; the blitter for sprites
and whole-stage passes), `music.c` (stvn's gwEm SNDH player,
`sndh.c`/`sndhisr.s` unmodified), `vm.c`, `menu.c`, `nf.c`, `super.s`.

Things that bit, and stay fixed:
- **Exit bombed**: `Super(old_ssp)` returns to user mode on the USP saved at
  `Super(0)` — a different call depth. `super.s` `to_user()` instead.
- **Keyclick between tracks**: the SNDH player saves/restores conterm ($484);
  the game clears keyclick+bell at start, restores after music shutdown.
- **A tune is always stopped before its buffer is written** (two ISRs = crash).
  `music_play` is the only writer and stops first.
- **Anything drawing a screen's stage outside `frame()`** (transitions, the
  credits, menus) zeroes its `g_screen_gen` — and only *after* using it: the
  first version forgot the screens before checking whether the baseline was on
  them, and every dissolve opened with a needless cut.
- **The credits roll draws only the stage**: the back screen kept the line said
  two frames before; the strip is cleared on every step.
- **Credits missing after a whole game** (user, real play with skips; never
  after a load near the end): sprites came from libcmini's malloc, which
  carves small requests from shared 32KB hunks and returns a hunk to TOS only
  when all of it is free. A long run left TOS memory in pieces; the roll's
  115KB sprite found none big enough (`no memory for S115.MSP`, once in four
  skip-throughs here). Sprites now live in one arena taken at load,
  compacted when a gap is too small: 59 compactions over a full game,
  ~100ms each, always next to a disk read.
- **"Fin" twice in French** (user, right after the arena): at the end card the
  fade needs the black sprite with the credits, the stage sprite and "fin" —
  152,672 bytes in French against a 152,984 budget here, less on the user's
  side; English credits are 756 bytes smaller. The arena is a hard limit where
  malloc had borrowed from the reserve, so the black failed and the fade went
  back to "fin". A slot wholly off the stage (the rolled-up credits) is now
  neither drawn nor kept. Test: `SPRBUDGET=130000 STOPAT=47920`, Space
  through the roll and the end card: `no room for S061.MSP` with the old
  pinning, clean with the new.

### Blitter

**Hog mode, 128 words at a time** (`blt_go`, `BLT_CHUNK_WORDS`; 3 lines of
the stage). It was 32 lines (~3ms), and that opened the game menu out of
nowhere when the pointer moved over a choice (in Hatari and on real
hardware, 2026-11-13): the IKBD sends a byte every 1.28ms and the ACIA holds
one, so a mouse packet arriving during a chunk lost a byte, and TOS read the
lost header's movement byte as a key — 1px right is scancode 1, Esc (or a
misread header with the right button). It needed a redraw *while* the mouse
moved: a hover changing the focus redraws, moving outside the buttons
doesn't. Any blit while the mouse moves was exposed (a typing caption, a
dissolve). Rule: **no hog chunk, nor anything else holding interrupts off,
longer than ~1ms.** After the change: sprites 0 of 152 differ, stress 0 of
300 with music; the user's real-hardware hover test "seems fixed". The cost
(120 chunk restarts per stage pass instead of 12, estimated 2–3%) is not
measured.

The first version used the
shared-bus mode with Atari's `bset #7` restart loop, and under Hatari a long
interrupt landing mid-blit — the tunes' own ADPCM decoder, milliseconds per
call — left passes wrong partway down. Seen by the user as a fade's last
screen holding a half-faded stage, flickering against the good one while the
next line typed; rare, timing-dependent. `SELFTEST=1`'s stress (300 full
dissolves into a buffer, random steps): shared mode 0/150 wrong without music,
96/150 with; hog chunks 0/300. Whether the real STE shares the fault is
unknown — hog chunks are right either way. One hog blit per pass would hold
off Timer C ~36ms (lost clock ticks); the address and halftone-line
registers carry on between chunks.

Sprites: mask `NOT S AND D`, data `S OR D`; skew + FXSR with word `k0-1`
primed, end masks keep the neighbouring row's words off the screen; opaque
sprites (no mask plane, widths like 321 and 402) clear their rectangle with an
all-ones source. `SELFTEST=1` checks 380 placements against `gfx_sprite_cpu`
(0 differ; 138 differ with `em3` broken, so the test can fail).

### Transitions (vm.c)

- Baseline as in Ren'Py: what was last shown, or the state at the last
  `with None` (`core.py:1482`, `:2724`). If that was never on screen, the
  transition starts with a cut to it, as Ren'Py's would.
- Dissolve/fade: `g_diff` = from XOR to, once; each step XORs in the part whose
  16x16 Bayer threshold the clock has passed since that screen was drawn (one
  pass, `HOP src AND halftone`, `OP XOR`), so a slow step only means a bigger
  next one. Fade = to black, hold, from black. The clock starts before the
  setup (compose + diff, ~100ms), as Ren'Py's starts at the interaction.
- Moves recompose every frame (~40ms: backdrop copy + blitter sprites).
  Ren'Py 8.5.3's semantics, which differ from the C89 engine's: without an
  enter/leave factory, new images are not drawn until the end and hidden ones
  vanish at once (`movetransition.py:631-646`); with one, from/to the edge.
- Click/Space/Return ends one (`core.py:2643`); Ctrl ends it; Up/Down roll
  back/forward from it (as from a timed pause); Esc ends it and opens the
  menu at the next interaction. Skipping drops them (a dropped one is a
  `with None`).
- Measured over the full autoplay: dissolve 686ms average before the clock
  fix (500 nominal), fade 1353 (1000), move 560 (500).

### Languages

French (the script's own, `None`) and English (`tl/english/`). The compiler
makes every string in both: say lines by their translation identifier
(`mr_tl_dialogue`, the parser computes Ren'Py's `label_md5`), name plates and
`[title!t]` by `mr_tl_string`, falling back to the original as Ren'Py does.
Every English line fits the box. The user's own title-card line had no
translation; a block for it (`chap0_48ab453d`) was added to
`game/tl/english/script.rpy`. `ī` and curly quotes have no Atari glyph: `i`,
`'`, `"`.

The ST holds one `.MWT` (~100KB, buffer sized for the larger) plus its
offset table: 5KB less than when the text was in the script. State, saves
and keyframes keep string numbers, never pointers.

`if _preferences.language == ...` (the credits roll, `credits` vs
`credits-en`) compiles to `IFLANG lang, words`: both branches are in the
code, the other language's is stepped over (`next_pc`). The compiler
simulates each branch from the scene list as it was before the group of
`if`s, then continues from the French one's; branches must leave as many
images, and may not hold a say or an untimed pause, so checkpoints are the
same in both languages. `art/sprite/credits-en.png` was generated like the
rest (not hand-worked yet).

Switching is in the Esc menu ("Langue": Français / English, Ren'Py's flag
buttons in the full game's navigation), saved in the preferences record.
As Ren'Py's `change_language`: rollback blocked (`translation/__init__.py:
857`), and at a say the line starts again in the new language, typed
(`check_language`, `:864`). At a pause, a title card is redrawn; images a
branch already chose stay. The text is read again and the keyframes made
again: ~670ms. The menus' own strings are a table in `menu.c`: the game's
English labels, Ren'Py's English originals for the confirmations.

### The end

`END` starts the game again from the top, as Ren'Py returns to its main menu
at the end of `start` (no wait): state reset, rollback limit 0 and nothing
reached, so no rolling back into the last playthrough; the baseline stays the
last picture, so the opening `with fade` fades out from it into the title
card. AUTOPLAY builds still exit there.

### Credits roll

`SCROLL` puts the slot at y1 in the state (where it ends — saves and replays
see that); the roll itself is live only, from the first frame of the pause,
drawn straight into the back screen. ~80ms a step: 1–2px steps at 16px/s.
Menu time is added back to the pause and the roll.

### Rollback is a replay

The display state is a function of position (variables only choose the way
at a branch; see "Branching").
Rollback = reset, `apply()` the commands to the target checkpoint with nothing
drawn/waited/loaded, `music_sync`. Keyframes every 64 checkpoints (built at
load, 455ms, 6.5KB) keep a step at ≤5ms; from the top it was 635ms at the end
of the game. Checkpoints: every say, untimed pause (title cards) and menu.
Limit as Ren'Py's rollback_limit (100, `block_rollback` zeroes it). The
variables and the way back and forward are the rollback log ("Branching",
step 3). Keys: Up/Undo back, Down/Help forward (at a menu, Undo and Help).

`music_sync` never reopens a playing track (the C89 engine lost its music that
way on real hardware).

### Skip

Ctrl held, unconditional (unread text too). Every line gets its frame, then
skip_delay (5ms) -- Ren'Py draws before the say behaviour can dismiss
(`behavior.py:811-815`). A 100ms frame throttle (from when a frame cost a
stage copy) made a skip unreadable: the opening to school in seconds, a
handful of frames. Transitions dropped; pauses crossed, hard ones too.
`_skipping = False` is a fence, not a cancel. **Divergence:** tracks are not
loaded during a skip; the right one starts where it ends. The skip decision is
made once per interaction (frame + wait), or a release in between waits on an
undrawn line. ~17 lines/s (was ~100 with the throttle).

### Menu, save, load (menu.c)

Esc or right click (Ren'Py's game_menu keys) at any say or pause: a box over
the frame — Reprendre, Sauvegarde, Charger, Vitesse du texte, Langue,
Recommencer, Quitter (all of it in English with the language). Recommencer
(2026-11-18) stands for the game's "Menu principal" then Start: confirmed
with Ren'Py's main-menu question reworded, then `new_game()` (vm.c), what
the end of the script does — defaults, no rollback, music stopped, the
title card; saves and persistent kept. The file screens follow
the game's screens.rpy: 3x2 grid, thumbnail, date, the line; 5 pages. Ren'Py's
French confirmation strings (overwrite, load, delete, quit); no Ê in the Atari
font, so "Etes-vous". Keyboard and mouse (pointer shown, hover selects).
Esc from a file screen returns to the game, as in Ren'Py.

A save is the checkpoint at or before the position, the rollback limit, the
date, the preview text (line or chapter) and a 160x90 thumbnail (each 4x4
block's grey re-dithered through a 4x4 Bayer — a majority vote made the
backdrops black), and beside it in `MIWAnn.LOG` the rollback log, future
included ("Branching", step 3). Load = the log back, `replay_to` its last
entry, limit restored. `MIWA.SAV`: header with the script's save signature, made by `mwc` from
the checkpoints alone (each one's kind, a say's French speaker and text):
staging edits -- positions, transitions, music, art, English -- keep saves;
adding, removing or rewording a French line drops them (verified both ways in
Hatari, 2026-11-12). Another signature's file reads as empty and is
recreated on save.
The preferences are one record after the 30 slots (`PR`, cps, language), apart from any
save as in Ren'Py; a file from before it just ends early and the default
stands.

### Typewriter

`preferences.text_cps` = 50 (`options.rpy:128`), changed by the Esc menu's
"Vitesse du texte" bar (Ren'Py's 0..200 with the far end instant,
`00preferences.rpy:347`; steps of 10). Each screen keeps how much of the line
it holds (-1: its strip still has the previous line) and catches up when it is
next the back screen, so a step draws only new glyphs. Complete = the *shown*
screen has every character (minirenpy lost a last glyph twice deciding it
otherwise). Characters are counted as drawn: style codes not, a wrap's `\n`
yes (it replaced a space). A click finishes the line and nothing else
(`text/text.py:2661`); lines come up whole while skipping and after a
rollback, rollforward or load (`character.py:652`, `after_rollback`).

Checked: a 160-character, 3-row italic line typed to the end is
pixel-identical to it drawn whole (and differs when the incremental draw drops
a glyph); a skip stopping mid-scene types its line to the end; 89 characters
take 1790ms at 50cps, 2990ms at 30 after a restart.

Memory: the menus need ~25KB transient; TOS had 33KB free during the credits
(the roll's sprite is ~150KB), the libcmini heap more.

## Measured (Hatari, 2MB STE)

- 217KB free after the music buffer, script, text, stage/backdrop/diff buffers;
  ~153KB sprite budget. (Older; 2026-11-23, with the rhythm game: 129KB,
  and the music's memory now shared with the sprites -- "The rhythm game".)
- Blitter: a stage pass ~36ms; a dissolve/fade step ~50ms (12 per 500ms).
- Full autoplay with every transition: 2779 lines, 1069 transitions, to END.

### The rhythm game: would it run? (2026-11-23, `RHYTHMTEST=0`, M00)

`make NATFEATS=1 RHYTHMTEST=<n> BUILD=… DIST=… <DIST>/st/MIWA.PRG` runs
`src/rtest.c` instead of the script; the disk needs MIWA.PAK and the song's
Ren'Py beatmap as BEAT.TXT. Space starts the tune (record first: drive.sh
`rec`); `scratch/rtest/sync.py` finds the original in the recording.

- **The music takes 31% of the CPU** (ADPCM, 12.5kHz mono, sound mode $81,
  decoded in the 50Hz Timer C call: ~190 cycles a sample). An idle loop's
  count, same loop with and without; the blitter agrees: a stage pass 17ms
  in silence, 24ms with the tune (hog chunks hold the decoder off, it
  catches up between them).
- **No drift**: the tune in Hatari's recording against `wav/`, 2s windows
  from 1s to 206s, all 95–100ms after the start call -- which is the 95ms
  `SNDH_PlayTune` itself takes (the player's init). So the clock for the
  notes is `now_ms()` when `music_start` returns, give or take a few ms. On
  real hardware not checked.
- A first try said 73%: the silent loop skipped the per-note work the
  music one did. Measure with the same loop.
- So: a whole-stage redraw a frame (~40ms with sprites) fits 24fps
  (every 3rd of the 71Hz VBLs); 35fps needs redrawing only round the
  notes.

### The rhythm game (2026-11-23)

`call screen rhythm_game(...)` (script_game.rpy) is `MWB_RHYTHM`, played
by `src/rhythm.c` after the .rpy (game/00-renpythm, kept as the reference;
the .rpyc is not used).

- **Compiler**: the beatmap is read at build time into `R<nn>.BTM` beside
  the script (stride taken, tracks or the C version's PRNG, mwb.h
  "beatmaps"), packed like the .MWT. Its header has the tune's end: the
  SNDH conversions play on into noise past it, so the ST stops them there,
  read from the player's `move.l #len,8(a6)` (all nine within 30ms of
  `wav/`). The notes are ordinary sprites plus 1.25x copies (`_x125` art);
  mkart makes the notes' art black discs with white arrows (a dither lost
  the arrows). `a, b, c = _return` is three SETs of `MWB_EX_RET`; the path
  check takes the results as 0. `main_menu` and `preferences.get_mute()`
  are False; `renpy.checkpoint()`, `set_volume`, `set_mute` ignored.
- **Clock**: 3s countdown, 2s of notes in silence, the tune; its 0 is when
  `music_start` returns (the player's ~95ms init).
- **Pause** (Esc, right button: the game's Oui/Non box): Timer C back to
  TOS's handler and the DMA stopped where it was; resumed by starting the
  DMA there and setting the ring's start back for the next turn
  (double-buffered registers). Recorded across a 2290ms pause: the tune
  moved 2286ms, i.e. kept its place.
- **Speed**: 32fps with every note hit (RHYTHMBOT=1, whole song), 35 with
  none. What it took: dirty rectangles over a layer of backdrop and
  tracks (a whole copy was 24ms); the HUD (health, score, combo) in the
  strip under the stage, CPU-drawn on change (two dozen blits a frame
  on the stage); text sprites made from a copy of the font (gfx_glyph is
  ~110us a row) without 32-bit multiplies. **A blit costs ~0.2ms before a
  pixel moves** (500 one-word rects: 95ms; 125ms with music) -- partly a
  libgcc `__divsi3` per blit, now a `divu` (was 0.25ms).
- **Flip**: `gfx_flip_poll` writes the shifter base itself. With
  Setscreen, TOS's VBL handler writes it after the shifter has reloaded
  for the new frame, so a screen shows a frame later than the VBL waited
  for -- a rhythm frame was seen half-drawn. `gfx_flip` (the rest of the
  game) still has that; nothing there redraws a screen in place fast
  enough to show it so far.
- High scores: persistent file, 32-bit, key in the .BTM; "Nouveau record !"
  as in the .rpy. Autoplay (test builds) skips the game: (0, notes, 0).
- **Values are 32-bit** (they were 16): a full-combo score is ~187000, and
  a script may test it (`if rhythm_score > 40000`). `MWB_EX_LONG` for
  constants past 16 bits, the variable table's entries 12 bytes with a
  32-bit default. Saves, logs and the persistent file always held 32.
- **`call`** is followed in line by the compiler, the return places part
  of the walk's state (`cstate.ret`), so a label called from two places is
  compiled for each. `return` with none is the end, as Ren'Py's
  (execution.py:790). Script_game's labels end in `return`: reached by
  `jump`, the game ends there; by `call`, it carries on. Checked: `call
  game_chichinoito` then `if rhythm_score > 40000` said "Score 187057,
  plus de 40000." on the ST (RHYTHMBOT).
- **On a real STE** (the user, 2026-11-23): plays, the pause included
  (the DMA restart); English strings, the mouse and clicks on the lanes
  work. The HUD in the strip is the user's choice to keep. RHYTHMTEST and
  RHYTHMBOT stay in the Makefile.
- **In the script, instead of the extras screen** (the user's choice: no
  game-specific menu in the engine, no screen language): chap0 calls
  `start_menu` after the title, a plain menu ("Commencer la lecture." /
  "Mini-jeu de rythme"), and that `rhythm_menu`: "Meilleurs scores", the
  songs, Watashi no Oto `if persistent.watashi`, the last `if
  persistent.ending` (screens.rpy:1731-1744), "Retour". Both labels live in
  script_game.rpy: a label in chap0 would rename the translation ids of
  every line after it (111 lost their English when I tried). 11 options,
  one page (MWB_MENU_MAXOPT 12). The scores are four says, through the
  original's `rhythm_game_get_highscore(path)` (.rpy:103-108): mwc reads it
  as a persistent variable named `rhythm_game_get_highscore:<file>`, written
  under the beatmap's key (`var.key`), default 0; persist_record sets it
  too, so a record shows at once. English in tl/english/script_game.rpy.
- **Bug found by the menu**: after a choice menu the fade into the game had
  to put its baseline up first, and composing it reloaded the scene's
  backdrop into g_bgbuf over the song's: the game ran on title-st. rhythm()
  reloads it after the fade when g_bgbuf_img moved.
- **The save screen stopped opening** (the user, after the menus): the
  keyframes' table (185 × 148 = 27KB, more with the menus' jump targets) was
  taken after the sprite budget, out of the 64KB reserve; Malloc(-1) said
  31,034 when the menu opened, less than the 32KB hunk libcmini wants for
  the page of slots, so files() failed and the menu took it for a back-out.
  vm_load now takes the table before the budget: 63,766 free at the menu,
  saving works, check-st passes with a save and a load.
- **The credits did not fit** (found then): `no room for S111.MSP (115568
  bytes)` with a 65KB budget (98KB before the fix above, short too).
  **Fixed by sharing the music's memory**: the arena and the tune are one
  block (Malloc'd at vm_load, free less the 64KB reserve), the tune loaded
  at its top, the arena ending where the tune starts (arena_cede: a longer
  tune slides the sprites down, dropping the least recently used if it must;
  a stop gives the room back). The conversions keep everything inside the
  file (the player's variables at 0x282, its 4KB DMA ring at 0xDB2, the
  samples end the file) and the player is position-independent, so this is
  safe. Budget now 65KB with M00 (the longest), 175KB with the credits'
  M08. Checked: the roll and the end card's fade clean, the full autoplay to
  the end with no `no room` (48 compactions), the rhythm game, the scores
  and a save; check-st with a save and a load. The scores page reuses four
  variables (`hs1`..`hs4`): each story variable costs 768 bytes of rollback
  log. rtest.c is built only with RHYTHMTEST (it keeps a buffer of its own:
  no VM, no arena).
- **Out of the game** (the user: a fade from title-st to title-st): the
  next `with` now starts from the game's last frame, as Ren'Py's does (its
  old scene is the last displayed, core.py:2416/2724, and held the `call
  screen`'s screen). rhythm() gives the shown screen a generation of its
  own and makes it the baseline. Checked with SLOW=4: song backdrop and
  notes, to black, to the menu. Then (the user): no transition at all when
  the summary was closed with a click -- main.c's button edge was from
  before the game, so the button still down read as a click and skipped
  the fade. rhythm() calls input_reset() after the game. Space was fine
  (4025ms fade); a single click cannot be sent to Hatari (drive.sh has
  doubleclick only), so the click case is the user's to check.
- **Scores over the picture** (the user): Ren'Py's `centered` character
  (00definitions.rpy:440; its window has no box and its text is centered,
  00style.rpy:317-328). A say whose character is defined with
  `window_style="centered_window"` compiles with who MWB_WHO_CENTERED
  (MWB_VERSION 8) and up to MWB_CENTER_ROWS rows; the VM draws it in a
  white bordered box in the stage's middle, each line centered, typed as
  usual (each step redraws box and text over a fresh stage), the strip
  empty. The parser only knows the game's characters (rpy/parse.c:1711:
  `centered "..."` was silently dropped) and keeps no Character styles, so
  script_game.rpy defines `centered` exactly as Ren'Py does, and mwc reads
  the define lines itself (char_centered). The speaker is part of a
  translation id: the English blocks changed ids and say `centered` too.
  Then one page, untyped: `{fast}` is supported (MWB_TXT_FAST 0x05, the
  variable marker moved to 0x06, codes 1..MWB_TXT_CODE_LAST zero-width;
  MWB_VERSION 9): typing starts at the last one (character.py:163-172),
  a line all before it is instant. Nine score variables again (hs1..hs9),
  one say per combination of the two unlocks.
- The pointer blinked through the game: each frame took it away, waited
  for the VBL, and drew it on the new screen, so it was off the screen
  most of the time. gfx_flip_poll now keeps it: one pointer state per
  screen, drawn into the back before the flip, taken out of the old screen
  after, the VBL following the mouse in the new one from the flip on.
  Hatari shots during play: 0 of 6 with the pointer before, 6 of 6 after.
  First done with the VBL masked while the pointer was drawn and the base
  written: the notes flickered. A base written just as the VBL comes is
  too late for the shifter's reload but before the interrupt is taken, so
  the clock moves on with the old screen still shown and the next frame is
  drawn into it (a shot caught one half drawn). Counted with the shifter's
  counter read once the display had started, at 8MHz: 18 of ~370 flips
  with the mask, 4 of 356 the old way -- the race was always there, the
  mask widened it. Now no mask (the VBL draws only into the shown screen),
  and gfx_flip_poll reads the counter after the VBL: still in the old
  screen, it waits for the next one. 2 of 320 retried, 0 missed.
- Hit sounds, on the YM2149 (the DMA is the tune's; the STE mixes both).
  Ren'Py plays a plucked-string sample a hit (play_hit_sound, .rpy:500),
  the song's own `<base>.hit0-3.wav` if all four are there, else do mi
  sol la (.rpy:20-27, :89-101). They are one sample resampled 0, +2, +5,
  +7, +9, +10 semitones (the lengths say so to the frame); the attack is
  mostly a partial 2 octaves and a fourth up, the decay the fundamental:
  Bb2 C3 Eb3 F3 G3 Ab3. mwc (hit_notes) reads the wavs from game/audio --
  copied there like the beatmaps, not shipped -- and takes each one's note
  by YIN over 30ms windows from 30% to 70% of it, the most voted semitone
  (8 of 9 windows on each sample; the first is in the attack); without
  them, C3 E3 G3 A3. It prints each song's four, and they go in the
  beatmap's header (hit[4], MIDI notes; header 16 bytes, MWB_VERSION 10).
  The ST (music.c, "hit sounds") plays two octaves up, a square on the
  three channels in turn, its level falling from 10 to 0 at each VBL from
  gfx_vbl_hook (~0.28s), interrupts off for each register's select and
  write. In Hatari, over a stretch of M00 with the bot hitting: Bb4 Eb5
  F5 4-18x above the tune alone at their pitches, the RMS 509 to 744 at
  level 11 (at 13, 1199: too loud); after Esc / Oui, no tone left
  ringing. 11 was still a bit loud on the user's side: 10, one step
  (~3dB) down.
- Before the sharing, at the budget, in Hatari --auto: 1,776,826 at start, music
  1,320,240 (the longest tune, M00), script 55,340, variables and rollback
  log ~16K (9,984 of it the log's 13 story variables × 128 × 6 bytes), text
  118,116, checkpoints and keyframes ~33K, stage buffers ~98K: 128,906
  left. The end card wants ~153KB of budget, so ~217KB there ("Measured").
  Where the other ~60KB went is not known: the program is ~70KB, the rhythm
  port's share of everything ~30KB.
- Not done: the judgement's fade, the help screen (screens.rpy:1828).
