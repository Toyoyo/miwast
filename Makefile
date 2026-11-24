# miwast - Miwa no Utsuroi for the Atari STE, mono high-res.
#
# The build is four steps, and the second one's output is yours to edit:
#
#   make script   game/*.rpy -> dist/SCRIPT.MWB, dist/TEXT_FR.MWT and
#                 TEXT_EN.MWT (the text, a file per language) +
#                 dist/manifest.txt
#   make images   dither every image the script uses into art/bg and
#                 art/sprite, as editable black/white(/transparent) PNGs.
#                 Only missing files are made: art/ is never overwritten.
#                 To redo some from the originals:
#                     make images REDO="tamae talk" REDO+="miwaclass"
#                 (basenames; this overwrites them)
#                 To compare other dithers of one image first, see
#                 tools/jarvisdither.sh (writes elsewhere, never to art/).
#   make data     art/ -> dist/data/*.MBG, *.MSP (the ST's formats)
#   make pak      dist/MIWA.PAK: script, images and music in one file
#
# `make` alone runs all four. `make check` compiles the branching fixture
# (tools/fixtures/branch), which plays every path of it.
#
#   make st       the ST program, dist/st/MIWA.PRG, beside a copy of the
#                 pak: dist/st/ is what goes on the hard disk.
#                 NATFEATS=1 builds a Hatari-only variant that traces to the
#                 host -- it will crash a real machine. AUTOPLAY=1 makes
#                 it play itself through to the end, for testing;
#                 STOPAT=<offset> (from dist/script.dump) hands back to the
#                 keyboard there, to look at one frame deep in the script.
#                 ANSWERS=1,0,2 is what autoplay answers the menus it
#                 meets (0 is the first option shown; past the list, 0).
#
#                 STLANG=1 starts in English when there is no MIWA.SAV.
#
#                 RHYTHMTEST=<n> (with NATFEATS=1) runs src/rtest.c instead
#                 of the script: CPU and blitter time with track M<n>
#                 playing, and BEAT.TXT from the hard disk flashed in time
#                 with it.
#
#                 RHYTHMBOT=1 plays the rhythm game by itself: every note
#                 hit on its onset, so a test run gets to the song's end.
#
#   make libcmini        rebuild deps/libcmini (make st only builds it when
#                        it is missing); make libcmini-clean removes its
#                        build. make clean never touches it.
#
#   tools/check-st.sh <lang> <answers>
#                 the branching fixture played by the ST under Hatari,
#                 autoplaying those answers, its trace diffed against the
#                 compiler's walk of the same path (MWC_TRACE).

GAME   ?= game
DIST   ?= dist
ART    ?= art

HOSTCC     = gcc
HOSTCFLAGS = -O1 -g -Wall -Wno-unused-function

RPY_SRC = $(addprefix tools/rpy/, lex.c parse.c vfs.c expr.c style.c slang.c \
          slparse.c tl.c md5.c list.c pyfn.c)

MANIFEST = $(DIST)/manifest.txt

.PHONY: all script images data pak clean check

all: pak

tools/mwc: tools/mwc.c src/mwb.h $(RPY_SRC)
	$(HOSTCC) $(HOSTCFLAGS) -Itools/rpy -o $@ tools/mwc.c $(RPY_SRC) -w

tools/mwdump: tools/mwdump.c src/mwb.h
	$(HOSTCC) $(HOSTCFLAGS) -o $@ tools/mwdump.c

tools/mwimg: tools/mwimg.c src/mwb.h
	$(HOSTCC) $(HOSTCFLAGS) -o $@ tools/mwimg.c

tools/mwpak: tools/mwpak.c src/mwb.h
	$(HOSTCC) $(HOSTCFLAGS) -o $@ tools/mwpak.c

# Only for tools/jarvisdither.sh, which builds it: not part of `make`.
tools/gdither: tools/gdither.c
	$(HOSTCC) $(HOSTCFLAGS) -o $@ tools/gdither.c -lm

script: tools/mwc tools/mwdump
	mkdir -p $(DIST)
	./tools/mwc $(GAME) $(DIST)/SCRIPT.MWB $(MANIFEST)
	./tools/mwdump $(DIST)/SCRIPT.MWB $(DIST)/TEXT_FR.MWT \
		$(DIST)/TEXT_EN.MWT > $(DIST)/script.dump

images: script
	./tools/mkart.sh $(GAME) $(MANIFEST) $(ART) $(REDO)

data: images tools/mwimg
	mkdir -p $(DIST)/data
	./tools/mwimg $(MANIFEST) $(ART) $(DIST)/data

pak: data tools/mwpak
	./tools/mwpak $(DIST)/MIWA.PAK $(DIST)/SCRIPT.MWB $(MANIFEST) \
		$(DIST)/data $(GAME)

# The branching fixture through the compiler, whose path check plays every
# path of it in both languages (mwc.c check_paths). Fails if any disagree.
# Then the backdrop fixture, whose code must be expect.dump's, checked by
# hand against the comments in its script.
check: tools/mwc tools/mwdump
	mkdir -p build/check
	./tools/mwc tools/fixtures/branch/game build/check/SCRIPT.MWB \
		build/check/manifest.txt
	./tools/mwdump build/check/SCRIPT.MWB build/check/TEXT_FR.MWT \
		build/check/TEXT_EN.MWT > build/check/script.dump
	mkdir -p build/check/backdrop
	./tools/mwc tools/fixtures/backdrop/game \
		build/check/backdrop/SCRIPT.MWB build/check/backdrop/manifest.txt
	./tools/mwdump build/check/backdrop/SCRIPT.MWB \
		build/check/backdrop/TEXT_FR.MWT build/check/backdrop/TEXT_EN.MWT \
		> build/check/backdrop/script.dump
	diff -u tools/fixtures/backdrop/expect.dump build/check/backdrop/script.dump

# ---- the ST program ----

LIBCMINI = deps/libcmini
CROSS    = m68k-atari-mintelf-
STCC     = $(CROSS)gcc
STCFLAGS = -m68000 -O2 -fomit-frame-pointer -Wall -nostdlib -nostdinc \
           -I$(LIBCMINI)/include -Isrc
ifeq ($(NATFEATS),1)
STCFLAGS += -DMW_NATFEATS
endif
ifeq ($(AUTOPLAY),1)
STCFLAGS += -DMW_AUTOPLAY
endif
ifneq ($(STOPAT),)
STCFLAGS += -DMW_STOPAT=$(STOPAT)
endif
ifneq ($(ANSWERS),)
STCFLAGS += -DMW_ANSWERS=$(ANSWERS)
endif
ifneq ($(STLANG),)
STCFLAGS += -DMW_LANG=$(STLANG)
endif
ifeq ($(SELFTEST),1)
STCFLAGS += -DMW_SELFTEST
endif
ifneq ($(SLOW),)
STCFLAGS += -DMW_SLOW=$(SLOW)
endif
ifneq ($(SPRBUDGET),)
STCFLAGS += -DMW_SPRBUDGET=$(SPRBUDGET)
endif
ifneq ($(RHYTHMTEST),)
STCFLAGS += -DMW_RHYTHMTEST=$(RHYTHMTEST)
endif
ifeq ($(RHYTHMBOT),1)
STCFLAGS += -DMW_RHYTHMBOT
endif
ASM      = vasmm68k_mot
ASMFLAGS = -Felf -quiet -x -m68000 -spaces

BUILD    = build
ST_SRC   = main.c pak.c gfx.c music.c vm.c menu.c nf.c sndh.c rhythm.c
ifneq ($(RHYTHMTEST),)
ST_SRC  += rtest.c
endif
ST_OBJ   = $(addprefix $(BUILD)/, $(ST_SRC:.c=.o)) $(BUILD)/sndhisr.o \
           $(BUILD)/super.o $(BUILD)/mouse.o

$(LIBCMINI)/build/libcmini.a:
	$(MAKE) -C $(LIBCMINI)

# `make st` builds libcmini only when it is missing. These rebuild it by
# hand (after a change in deps/libcmini: its own make redoes what is stale)
# or remove it; `make clean` leaves it, as our flags never change it.
libcmini:
	$(MAKE) -C $(LIBCMINI)

libcmini-clean:
	$(MAKE) -C $(LIBCMINI) clean

.PHONY: libcmini libcmini-clean

# Objects depend on the flags too, so NATFEATS=1 and a plain build never
# share an object.
$(BUILD)/flags: FORCE
	@mkdir -p $(BUILD)
	@echo '$(STCFLAGS)' | cmp -s - $@ || echo '$(STCFLAGS)' > $@

$(BUILD)/%.o: src/%.c src/st.h src/mwb.h $(BUILD)/flags
	$(STCC) $(STCFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.s
	@mkdir -p $(BUILD)
	$(ASM) $(ASMFLAGS) $< -o $@

$(DIST)/st/MIWA.PRG: $(ST_OBJ) $(LIBCMINI)/build/libcmini.a
	mkdir -p $(DIST)/st
	$(STCC) -m68000 -nostdlib $(LIBCMINI)/build/objs/crt0.o $(ST_OBJ) \
		-o $@ -s -Wl,--gc-sections -Wl,-Map=$(BUILD)/MIWA.map -L$(LIBCMINI)/build -lcmini -lgcc

st: $(DIST)/st/MIWA.PRG pak
	cp $(DIST)/MIWA.PAK $(DIST)/st/MIWA.PAK

.PHONY: st FORCE
FORCE:

# Never removes art/: that is hand-edited source, not build output.
clean:
	rm -f tools/mwc tools/mwdump tools/mwimg tools/mwpak tools/gdither
	rm -rf $(DIST) $(BUILD)
