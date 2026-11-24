/* mr.h - minirenpy common definitions
 *
 * Target: Win32s on Windows 3.1x, OpenWatcom 2.0, Pentium III class machine.
 * Written to C89 rules: declarations at the top of a block, no // comments.
 */

#ifndef MR_H
#define MR_H

/* No windows.h. It used to be here, and because nearly every file includes
 * mr.h, that one line meant the whole engine compiled against the Win32 API
 * whether it used it or not -- rhythm.c switched on VK_LEFT, and the
 * accident only became visible when it had to be taken away.
 *
 * The four places that genuinely need the Windows API -- platw32.c,
 * present.c, audio.c and image.c's profiling clock -- include it themselves.
 * Everything else is portable C89. See plat.h. */

/* The logical stage. The game is authored at 1280x720 and we composite at
 * that size regardless of window size, so a window enlarged to 1280x720 on a
 * capable display driver is pixel exact. */
#define MR_STAGE_W 1280
#define MR_STAGE_H 720

/* Default window client size: exactly half the stage. mrpy.ini's width and
 * height override it; see config.h. */
#define MR_WIN_W 640
#define MR_WIN_H 360

/* The engine's own name, for the log header, the error box and the window
 * caption a game that sets neither config.name nor config.window_title falls
 * back to. Shared between main.c and the platform, which is why it is here. */
#define MR_TITLE "Minirenpy"

/* A VFS name: the whole relative path inside the archive, '/' separated, so
 * 255 usable characters plus the terminator. This is the archive's on-disk
 * name field as well as every buffer that holds one, which is why it lives
 * here rather than in vfs.h -- image.c, audio.c, parse.c and musicroom.c all
 * store one, and they used to each pick their own 128.
 *
 * Changing it changes the MRPK format: the directory is read straight into an
 * mrpk_entry[] with one fread, so the entry size is 4 + 4 + this. Bump
 * MRPK_VERSION with it or an older archive is read as garbage. */
#define MR_PATH_MAX   256

/* A host filesystem path: the loose game directory plus '/' plus a VFS name.
 * Only the loose-file fallback builds one; the archive never touches the
 * filesystem beyond opening the pak itself. */
#define MR_FSPATH_MAX 512

typedef unsigned char  mr_u8;
typedef unsigned short mr_u16;

/* Exactly 32 bits, on the target *and* on the build host. `unsigned long` is
 * 32 bits under Watcom but 64 on a modern Linux host, which silently breaks
 * every host-side harness that reads a binary header or a pixel: the font
 * loader read 72 bytes into a 36-byte header and rejected the file. `int` is
 * 32 bits in both places, so it is the honest choice. Every printf of one of
 * these already casts to unsigned long at the call site. */
typedef unsigned int   mr_u32;

/* A 32bpp surface. Pixels are 0xAARRGGBB in a DWORD, which on little-endian
 * x86 is byte order B,G,R,A -- exactly what GDI calls a 32bpp BI_RGB DIB, so a
 * surface can be handed to SetDIBitsToDevice with no conversion. Alpha is
 * straight (not premultiplied); it is meaningful for images and ignored for
 * the stage. */
typedef struct mr_surface {
    int     w;
    int     h;
    int     pitch;      /* in pixels, not bytes */
    mr_u32 *px;
    int     has_alpha;  /* 0 if every pixel is known opaque */
} mr_surface;

/* Mask, not a cast to mr_u8, and that is a performance decision rather than a
 * stylistic one. The two are arithmetically identical -- mr_u8 is an 8-bit
 * unsigned type, so casting to it *is* a mod-256 truncation -- but Watcom
 * implements the cast by storing the 32-bit value to a stack slot and reading
 * its low byte back:
 *
 *     mov   dword ptr 1cH[esp],ebx     ; 32-bit store
 *     mov   bl,byte ptr 1cH[esp]       ; 8-bit load of the same address
 *     movzx ebx,bl
 *
 * On a P6 a narrow load from a just-written wider store cannot forward and
 * waits for the store to reach L1. In mr_blit's blend that happened three
 * times per pixel. Rewriting these four macros as masks took the loop from
 * 107 instructions to 96, the stack accesses from 36 to 27, and both the
 * store-forwarding stalls and the seven movzx to zero.
 *
 * Note the grep in CLAUDE.md for the partial-register hazard does not find
 * this shape: the destination is a full register (`movzx ebx,bl`), not a
 * 16-bit one. Read the loop, do not pattern-match it. */
#define MR_A(p) (((p) >> 24) & 0xFFu)
#define MR_R(p) (((p) >> 16) & 0xFFu)
#define MR_G(p) (((p) >> 8) & 0xFFu)
#define MR_B(p) ((p) & 0xFFu)
#define MR_ARGB(a, r, g, b) \
    (((mr_u32)(a) << 24) | ((mr_u32)(r) << 16) | ((mr_u32)(g) << 8) | (mr_u32)(b))
#define MR_RGB(r, g, b) MR_ARGB(255, r, g, b)

/* --- dividing by 255 without dividing -------------------------------------
 *
 * An alpha blend is `(s*a + d*(255-a)) / 255` per channel, and written that
 * way Watcom emits a real `DIV r/m32` -- three of them per pixel in mr_blit,
 * two with the divisor in a stack slot. `DIV` is 36 to 40 cycles on a Pentium
 * III and does not pipeline, so three is most of a hundred cycles spent on
 * arithmetic worth about six.
 *
 * It cost 270ns per pixel, measured. The dialogue window is 148,000 pixels of
 * which 144,076 are at partial alpha, so it misses both fast paths in mr_blit
 * and pays the full price on nearly every pixel: 38.7ms of every 119.6ms
 * dialogue frame on the target, for a frame whose job is to add one glyph.
 * See NOTES.md under "Splitting `scene`"; the compositor buckets in MR_PROF
 * are what found it.
 *
 * MR_DIV255 is `(x + 127) / 255` -- round to nearest -- and MR_FLOOR255 is
 * `x / 255`. Both are **exact**, not approximations: they were checked
 * against the divide for every input in range, which is the only reason this
 * is a safe change to make to a blender whose output nobody wants to argue
 * about. The naive `(x + 128) >> 8` that the old comment in gfx.c described
 * is *not* exact -- it is wrong for 32,259 of the 65,026 inputs, always
 * downwards, which is the uniform darkening scaletest caught in LERP2.
 *
 * Valid for x in 0..65025, which is the blend's whole range: the largest
 * numerator is `255*a + 255*(255-a)` = 255*255 = 65025, and the bound is
 * fixed by the channel maxima rather than by anything tunable, so it cannot
 * creep. MR_FLOOR255 is exact to 65535.
 *
 * Worth noting for later: x + 128 reaches 65153, so the whole computation
 * fits in a 16-bit lane. That is what would let an MMX version do two pixels
 * per register with pmullw -- but measure again first, because removing the
 * divides may well leave this loop memory-bound, which is where scale and
 * pack already are and where SIMD has nothing to give.
 */
#define MR_DIV255(x)   (((x) + 128u + (((x) + 128u) >> 8)) >> 8)
#define MR_FLOOR255(x) (((x) + 1u + (((x) + 1u) >> 8)) >> 8)

/* --- surfaces (gfx.c) --- */
mr_surface *mr_surface_new(int w, int h);
void        mr_surface_free(mr_surface *s);
void        mr_surface_fill(mr_surface *s, mr_u32 color);
void        mr_surface_copy(mr_surface *dst, const mr_surface *src);

/* Is every pixel's alpha 255? One pass, one AND per pixel, no branch in the
 * inner loop; the test is per row, so a sprite whose first row is transparent
 * costs one row.
 *
 * This exists because `has_alpha` is set from the PNG's declared colour type
 * (png.c), which says whether an alpha channel is *present* and not whether
 * anything in it differs from opaque -- and 49 of this game's 98 RGBA files,
 * 45 of them full-stage, carry a channel that is 255 throughout. (An earlier
 * version of this comment said "70, including all 22 backgrounds"; that was a
 * probe over a wider set of directories than game/images.) Each of those
 * costs mr_blit the per-pixel loop to discover
 * `a == 255` and assign, 43 ns/px measured, in place of a row memcpy at 27.
 * Over a 1280x720 background that is 14.7ms a blit.
 *
 * Returns 1 for a NULL or empty surface: "nothing is translucent" is the
 * answer that keeps the caller on the fast path, and there is nothing to
 * blit either way. */
int         mr_surface_opaque(const mr_surface *s);

/* An FNV-1a hash of every pixel *outside* rows [y, y+h), for answering "did
 * anything change up there?" about two successive frames of the same stage.
 *
 * This exists for the MR_REDRAWPROBE measurement in main.c, which asks how
 * often a full-stage repaint produced pixels identical to the ones already
 * there outside the dialogue band -- i.e. how often interp.c's `g_stepped`
 * forces 921,600 pixels of work for a few hundred changed ones. It is a
 * measurement tool, not part of any frame the player sees.
 *
 * Word-wise rather than byte-wise FNV, which is not the published function but
 * is what a change detector needs: a collision would under-report change, at
 * roughly 2^-32 a comparison, against a per-pixel cost low enough that the
 * probe does not dominate the frame it is measuring. It still costs a multiply
 * and a load per pixel over ~685,000 pixels, so a run with the probe on has
 * unusable *timings*; the counts are what it is for.
 *
 * h <= 0 digests the whole surface. Returns 0 for a NULL or empty one, which
 * is also a legal hash value -- the caller is expected to know it passed a
 * real surface, as main.c does. */
mr_u32      mr_surface_digest_outside(const mr_surface *s, int y, int h);

/* Blur in place, as im.Blur(im, xrad, yrad) does: an almost-Gaussian built
 * from three box blurs per axis. Allocates and frees a scratch surface of the
 * same size while it runs. Returns 0 only if that allocation failed or a
 * radius was negative, in which case the surface is untouched. */
int         mr_surface_blur(mr_surface *s, float xrad, float yrad);

/* Resample src to dst_w x dst_h, bilinearly, into a freshly allocated surface
 * the caller owns (NULL if it could not allocate). `flipx`/`flipy` mirror the
 * result, which is what a negative Transform zoom asks for.
 *
 * Sizes are absolute pixel counts, not factors: the one caller already has to
 * round the factor to whole pixels to decide where the image goes, and
 * passing the rounded size means the two cannot disagree. */
mr_surface *mr_surface_zoom(const mr_surface *src, int dst_w, int dst_h,
                            int flipx, int flipy);

/* Alpha-blend src over dst at (dx,dy), clipped to dst. If alpha is 255 for
 * the whole surface this degenerates to a row copy. */
void mr_blit(mr_surface *dst, const mr_surface *src, int dx, int dy);

/* As mr_blit but scales the whole source alpha by 0..255, for dissolves. */
void mr_blit_alpha(mr_surface *dst, const mr_surface *src, int dx, int dy, int alpha);

/* Blit a rectangle of the source into a rectangle of the destination,
 * resampling by nearest neighbour. Only the nine-slice Frame backgrounds need
 * this, and they stretch flat colour, so anything better would not show. */
void mr_blit_stretch(mr_surface *dst, const mr_surface *src,
                     int sx, int sy, int sw, int sh,
                     int dx, int dy, int dw, int dh);

/* Confine drawing to a rectangle until the matching pop. Nested pushes
 * intersect. Every blit and every text draw respects the innermost rect, which
 * is how a viewport shows only part of a taller child. */
void mr_clip_push(int x, int y, int w, int h);
void mr_clip_pop(void);
void mr_clip_reset(void);
void mr_clip_rect(const mr_surface *s, int *x, int *y, int *w, int *h);

/* dst = a*(1-t) + b*t, t in 0..255. All three must be the same size. The
 * result is opaque: this is for whole-stage snapshots, which are. */
void mr_lerp_surface(mr_surface *dst, const mr_surface *a,
                     const mr_surface *b, int t);

/* The same blend for art that is still going to be composited over something
 * else, so its alpha has to survive. Premultiplied-correct; see gfx.c. */
void mr_lerp_surface_a(mr_surface *dst, const mr_surface *a,
                       const mr_surface *b, int t);

/* Darken toward a solid colour in place, t in 0..255 (255 = fully colour). */
void mr_fade_to(mr_surface *s, mr_u32 color, int t);

/* --- compositor profile (gfx.c), for MR_PROF ------------------------------
 *
 * MR_PROF's `scene` is one number -- 175.76ms of a 304.86ms frame on the
 * Pentium III -- and the question it cannot answer is what kind of work that
 * is. An opaque background blit is a `memcpy` and runs at whatever the memory
 * does; an alpha-blended sprite is three multiply-add-divides and two
 * branches per pixel. The first is immovable and the second is not, so a
 * decision about SIMD, or about dirty rectangles, or about anything else that
 * touches the compositor needs them apart. The whole-phase figure divided by
 * the whole-phase pixel count is an average of two populations and describes
 * neither.
 *
 * Five buckets, split by what the inner loop costs rather than by which
 * function was called:
 *
 *   copy     opaque row copies -- mr_blit with a source known opaque, and
 *            mr_surface_copy. Reads and writes, no arithmetic.
 *   blend    per-pixel alpha blending -- mr_blit over a source with alpha,
 *            and mr_blit_alpha. The expensive one, and the only one where
 *            the pixel costs more than fetching it.
 *   fill     write-only -- mr_surface_fill. Half the traffic of a copy.
 *   lerp     whole-surface transition arithmetic -- mr_lerp_surface,
 *            mr_lerp_surface_a, mr_fade_to.
 *   stretch  resampling blits -- mr_blit_stretch, mr_surface_zoom.
 *
 * `px` counts pixels actually written, after clipping, which is the figure
 * that divides into the milliseconds to give a per-pixel cost. `n` counts
 * calls, and it is there to say when the measurement is distorting what it
 * measures: the clock is read twice per call, so a phase doing hundreds of
 * small blits is paying for the instrumentation and its ms must be read with
 * that in mind. A phase doing tens of large ones is not.
 */
typedef struct {
    long copy_ms,    copy_px,    copy_n;
    long blend_ms,   blend_px,   blend_n;
    long fill_ms,    fill_px,    fill_n;
    long lerp_ms,    lerp_px,    lerp_n;
    long stretch_ms, stretch_px, stretch_n;
} mr_gfx_prof;

/* Start accounting, using `clock` for the millisecond readings; NULL turns it
 * off. A hook rather than a direct mr_plat_ticks call for two reasons: gfx.c
 * is linked by harnesses that have no platform layer at all (scaletest,
 * packtest), where naming that symbol is a link error; and a test can pass a
 * clock whose advance it controls, which is the only way the millisecond
 * attribution is checkable on a host whose real clock is both too coarse and
 * shimmed. Not an #ifdef, so the suite reaches the same code the target does.
 *
 * Off costs one pointer test per blit call and nothing per pixel. */
void mr_gfx_profile_enable(unsigned int (*clock)(void));

/* Read the accumulators and zero them, so each caller gets the work done
 * since the last read. That is what lets main.c attribute the same buckets to
 * `scene` and to `ui` separately by reading between the two phases -- the
 * dialogue window's glyphs are hundreds of tiny blends and the scene's
 * sprites are a few large ones, and totalling them would hide exactly the
 * distinction this exists to draw. */
void mr_gfx_profile(mr_gfx_prof *out);

/* Ask the frame loop to exit (main.c). */
void mr_quit(void);

/* The composited stage, for whoever needs to read the finished picture --
 * a save thumbnail, for instance. */
mr_surface *mr_stage(void);

/* Mark the display as needing a redraw. The frame loop skips rendering
 * entirely when nothing has changed, which is what keeps a static menu from
 * starving the audio mixer, so anything that changes what is on screen
 * without going through the interpreter has to say so. */
void mr_invalidate(void);

/* --- error reporting (main.c) --- */
void mr_fatal(const char *fmt, ...);
void mr_log(const char *fmt, ...);

#endif /* MR_H */
