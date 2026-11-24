/* vbe.h - VESA BIOS Extensions: reading the mode list, and choosing from it.
 *
 * Split in two on purpose.
 *
 *   vbe.c      Pure. Decodes a raw ModeInfoBlock -- the 256 bytes function
 *              4F01h fills in -- into mr_vbe_mode, and picks the best of a
 *              list. No BIOS, no interrupts, no DOS. Harness: vbetest.
 *
 *   vbedos.c   The int 10h half: 4F00h for the mode list, 4F01h per mode,
 *              4F02h to set one. DOS only, and untestable here.
 *
 * The split is the whole reason the policy below can be trusted. "Which mode
 * should we run in?" is a judgement call with half a dozen inputs and a
 * dozen cards that answer differently; it is also the sort of thing that can
 * only be debugged on the machine, at a black screen, with no log visible.
 * Making it a function from a table of bytes to an index means it can be
 * argued about here instead.
 *
 * Reference: VESA BIOS Extension (VBE) Core Functions Standard, version 3.0,
 * section 4.4 (ReturnVBEModeInformation) for the block layout, and the mode
 * attribute bits in table 4.9. The practical reading of it -- which checks
 * actually matter and in what order -- follows PluMGMK's vbesvga.drv,
 * VIDMODES.ASM, which does the same enumeration for a Windows 3.1 display
 * driver and has been run against real hardware and several BIOSes.
 */

#ifndef MR_VBE_H
#define MR_VBE_H

/* MemoryModel, block offset 0x1B. Only these two are of any use to us: the
 * stage is 32bpp ARGB and gets packed into whatever the mode wants, which
 * needs the framebuffer to be one pixel after another. Planar (3) is how
 * plain VGA 16-colour works and is not reachable this way; the rest are
 * text, CGA, Hercules and YUV. */
#define MR_VBE_PACKED 4
#define MR_VBE_DIRECT 6

/* The size of the block 4F01h fills in. The spec says 256 bytes and every
 * caller must supply that much whatever it intends to read, because a BIOS
 * is entitled to write all of it. */
#define MR_VBE_BLOCK 256

/* One usable mode, as the rest of the engine wants to think about it. */
typedef struct {
    int number;          /* the mode number, as 4F02h takes it (no LFB bit) */

    int w, h;
    int pitch;           /* bytes per scanline, in linear framebuffer mode */

    /* Two different depths, and the difference matters. `bpp` is the total
     * including padding -- what a pixel occupies, and so what the pack loop
     * strides by. `sigbpp` is red+green+blue, what the picture is actually
     * worth. A 32bpp mode is 24 significant bits in 32; the 15bpp modes are
     * 15 in 16. */
    int bpp;
    int sigbpp;

    int model;           /* MR_VBE_PACKED or MR_VBE_DIRECT */

    /* Field layout, for the direct-colour modes. Meaningless when the model
     * is packed, where the pixel is an index into the DAC. */
    int rpos, rsize;
    int gpos, gsize;
    int bpos, bsize;

    unsigned long phys;  /* PhysBasePtr: where the framebuffer lives */
} mr_vbe_mode;

/* Turn one raw ModeInfoBlock into an mr_vbe_mode.
 *
 * Returns 0 -- and leaves *out alone -- for any mode we cannot use, which is
 * most of them. `number` is the mode number the block was fetched for, since
 * the block does not contain it. `vbe_version` is the BCD version from the
 * VbeInfoBlock (0x0200 for 2.0); it decides whether PhysBasePtr is even
 * present, as it is a VBE 2.0 addition.
 *
 * `why`, when it is not NULL, receives a short static string naming the
 * check that refused the mode, or NULL on success. There are eleven ways to
 * be refused and on the target the difference between them is the
 * difference between "this card cannot do it" and "we are reading the block
 * wrong" -- the first is a fact and the second is a bug, and from a mode
 * count of zero they look identical. MODES.EXE prints these.
 *
 * The block is read as bytes rather than cast to a struct: it comes from
 * firmware, the fields are unaligned, and a packed struct is one compiler
 * flag away from being read wrong in a way nothing would report. */
int mr_vbe_decode(const unsigned char *block, int number, int vbe_version,
                  mr_vbe_mode *out, const char **why);

/* What we would like, subject to what exists. */
typedef struct {
    int want_bpp;        /* from mrpy.ini `depth`; 0 for "no preference" */
    int stage_w;         /* MR_STAGE_W / MR_STAGE_H, passed in so the picker */
    int stage_h;         /* stays a function of its arguments */
    int allow_8bpp;      /* 0 to refuse a palettised mode outright */
} mr_vbe_want;

/* Choose. Returns an index into `list`, or -1 if nothing in it will do.
 *
 * The policy, in strict priority order. Each step only breaks ties left by
 * the one above it, so the whole thing is decidable by reading down:
 *
 *   0. an explicitly *palettised* depth, when mrpy.ini says `depth=8`.
 *      Above the colour class, and the only thing that is: otherwise step 1
 *      eliminates every 8bpp mode before the depth key below is ever read,
 *      and `depth=8` silently selects the shallowest truecolour mode
 *      instead -- which on a real mode list is the mode no setting at all
 *      would have chosen. `depth` works for 16, 24 and 32; it has to mean
 *      something for 8 too. `truecolour=1` still refuses outright and wins
 *      over this, since it removes the modes from the list rather than
 *      ranking them.
 *
 *   1. colour class. Truecolour (sigbpp >= 15) beats palettised, always,
 *      whatever it costs in resolution. This is a judgement call and the
 *      most arguable line here: 640x480x16 is preferred to 1280x720x8. The
 *      reason is that the 8bpp path dithers into a 6x6x6 cube, the text is
 *      anti-aliased, and dithered anti-aliased text at any resolution looks
 *      worse than clean text at half of it. `allow_8bpp` turns palettised
 *      modes off entirely; mrpy.ini's `vbemode` overrides the lot.
 *
 *   2. an explicitly named depth, when mrpy.ini says `depth`. This one
 *      step is above resolution, and only when the key is set: 0, the
 *      default, means no preference and leaves the order below untouched.
 *      It is here because the alternative is a setting that does nothing.
 *      On the Matrox the deepest modes stop at 800x600 while 1024x768 is
 *      available at 16bpp, so every value of `depth` chose the same mode
 *      while the key was only a tiebreak -- and a key that silently has
 *      no effect is worse than one that costs resolution on purpose.
 *
 *   3. the size of the *picture*, not of the mode. A 16:9 stage in a 4:3
 *      mode is letterboxed, so what 1024x768 really offers is 1024x576 --
 *      bigger than everything 800x600 can show, even though 800x600 has the
 *      smaller mode. Scored as the fitted area, clamped at the stage, since
 *      a mode larger than 1280x720 cannot show more of it and scaling up is
 *      pure cost.
 *
 *      Above depth because of the scale factor, not because of bandwidth:
 *      on the S3 the chosen 1152x864x16 costs slightly more bytes a frame
 *      than 800x600x32 and twice the per-pixel work, but it is a 0.9
 *      downscale of the stage against a 0.625 one, and a visual novel is
 *      mostly text. The arithmetic is in score_of.
 *
 *   4. wasted output. Between two modes that show the same picture, the one
 *      with less letterbox wins: 1280x720 and 1280x1024 both display the
 *      stage 1:1, but the second spends 389,120 pixels a frame painting
 *      black bars. This is the step that makes a 16:9 mode worth having,
 *      and the reason a patched BIOS is worth the trouble.
 *
 *      There is no second depth step after this one. The original policy
 *      ranked depth below resolution -- a visual novel is mostly text and
 *      the default should spend its bytes on size -- and when `depth` was
 *      promoted to step 2 the lower comparison was left behind. It is
 *      unreachable: with no depth named every mode's miss is zero, and
 *      with one named step 2 has already decided. Removed, rather than
 *      left in to read like a policy it cannot enforce. What that argument
 *      actually buys is step 3 ranking above depth by default, which is
 *      where it still lives.
 *
 *   5. more significant bits, between modes that cost exactly the same. A
 *      real S3 offers 1152x864x16 twice, as 5:5:5 and as 5:6:5; identical
 *      size, identical pixel, identical bandwidth, and half the colours in
 *      one of them. Without this step the tie fell to the mode number and
 *      took the worse one.
 *
 *   6. the lower mode number, so the choice is deterministic when a card
 *      reports the same mode twice -- which they do.
 *
 * Plain VGA 640x480x16-colour (mode 0x12) is *not* a candidate and never
 * appears in a list handed here: it is planar, has no linear framebuffer,
 * and is not a VBE mode at all.
 *
 * It was described here as "the caller's fallback for when this returns -1".
 * **It is not. Nothing implements it.** mr_present_init logs "none of the
 * modes will do" and returns 0 (presdos.c:477), so a VGA-only machine gets
 * no picture at all. Three things would have to exist first: a planar
 * presenter (four bit planes through the sequencer mask register, not a
 * linear framebuffer), a 16-colour dither to go with it, and a pack loop at
 * 4bpp -- pack.c has 8, 16, 24 and 32. There is also no way to *ask* for the
 * mode for testing: the `vbemode` ini override only honours modes that
 * survived enumeration, on purpose (presdos.c:461).
 *
 * Left as a note rather than a plan. The target is a Pentium III with a VBE
 * 2.0 card, and no machine this has run on has needed it.
 */
int mr_vbe_pick(const mr_vbe_mode *list, int n, const mr_vbe_want *want);

/* The fitted picture size for a given mode, which is step 2 above and is
 * also what the presenter needs in order to set up its scaler. Exposed
 * because the two must agree: if the picker scores a mode on a 1024x576
 * picture and the presenter then draws 800x450 into it, the choice was made
 * on a number nothing honours. */
void mr_vbe_fit(int mode_w, int mode_h, int stage_w, int stage_h,
                int *pic_w, int *pic_h);

/* --- the int 10h half (vbedos.c, DOS only) ------------------------------- */

/* Fill `list` with every usable mode the BIOS reports, at most `max` of
 * them. Returns the count, or -1 if there is no VBE at all. The VBE version
 * is written to *version when it is not NULL, for the log.
 *
 * Safe to call on a machine with no VESA BIOS: it checks the 'VESA'
 * signature before believing anything else. */
int mr_vbe_enumerate(mr_vbe_mode *list, int max, int *version);

/* Switch to a mode and map its framebuffer. Returns a pointer to the
 * framebuffer, or NULL. The pointer is valid until the next call.
 *
 * The LFB bit (0x4000) is added here rather than being carried in
 * mode->number, so that a number logged or written to mrpy.ini is the one
 * the VBE documentation and every mode-list utility would show. */
void *mr_vbe_set_mode(const mr_vbe_mode *mode);

/* Back to text mode (int 10h, AX=0003h). Must run before exit, and from the
 * fatal path too -- a DOS prompt left in a graphics mode is a dead
 * terminal. */
void mr_vbe_text_mode(void);

#endif /* MR_VBE_H */
