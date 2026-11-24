/* yuv.h - YUV 4:2:0 to the stage's pixel format, scaled, in one pass.
 *
 * This is the only part of video playback that is ours rather than pl_mpeg's,
 * and it is the part worth owning: on the target it is the second-biggest cost
 * after the IDCT, and it is the one that depends on the output size.
 *
 * Shaped like scale.h on purpose -- init once per output size, then rows --
 * for the same reason that file gives: it is pure arithmetic over surfaces
 * with no presenter in it, so the harness can drive it (yuvtest). The
 * presenters are the one thing the suite cannot reach.
 *
 * --- why the conversion scales, instead of a scaler running after it --------
 *
 * The obvious construction is to convert the frame to RGB at its own size,
 * put that on the 1280x720 stage, and let the presenter scale the stage down
 * as it does for everything else. That is two resampling passes and one of
 * them is over the stage, which is 921,600 pixels whatever the window is.
 *
 * Measured on the host against ending.mpg (640x320): converting the frame
 * costs 0.54ms, and the stage route costs about 2.7ms for the same picture --
 * five times as much, because the stage is five times the pixels. Decode is
 * 1.22ms and is fixed by the video's own size, so the stage route would make
 * the *cheap* half of playback the expensive one. Those are host figures and
 * the target is perhaps twenty times slower; what they establish is the
 * ratio, which the hardware does not change.
 *
 * So the conversion writes straight into the presenter's picture buffer, at
 * the size that buffer actually is, and the stage is not involved in a frame
 * of video at all.
 *
 * --- the colour conversion -------------------------------------------------
 *
 * BT.601, in integer, from pl_mpeg's own PLM_PUT_PIXEL (libs/pl_mpeg.h:3607)
 * and its PLM_DEFINE_FRAME_CONVERT_FUNCTION at :3613. Taken from there rather
 * than re-derived from the standard, deliberately: it is the same coefficients
 * (76309, 104597, 25674, 53278, 132201 in 16.16) the library's own
 * plm_frame_to_bgra uses, so a disagreement between this file and the vendor
 * function is a bug in this file and nothing else. yuvtest asserts exactly
 * that -- it converts a frame both ways and compares.
 *
 * That is also why this does not use plm_frame_to_bgra itself: the vendor
 * function writes at the frame's size with a fixed stride and no scaling, no
 * destination offset and no bottom-up row order, and all three are required
 * here.
 */

#ifndef MR_YUV_H
#define MR_YUV_H

#include "mr.h"
#include "pack.h"       /* mr_pack_fmt, for the straight-to-display path */

/* One decoded frame, as a decoder hands it over: three planes, 4:2:0, so the
 * chroma planes are half the luma's size on both axes.
 *
 * This is deliberately not plm_frame_t. Nothing in this file or in its
 * harness includes pl_mpeg.h -- the conversion is arithmetic over three byte
 * planes and has no business knowing what produced them, and a fixture can
 * build one of these by hand where it could not build a plm_frame_t. movie.c
 * does the one-line translation. */
typedef struct {
    int w, h;              /* the luma size, and so the picture's size */

    const mr_u8 *y;
    const mr_u8 *cb;
    const mr_u8 *cr;

    int y_pitch;           /* in bytes */
    int c_pitch;           /* both chroma planes share it, as 4:2:0 implies */
} mr_yuv_frame;

/* How a given size pair is converted. Exposed for the same reason
 * mr_scale_kind is: "the 1:1 path exists" and "the 1:1 path is what ran" are
 * different claims, and at the default window size the second one is the
 * interesting one. */
typedef enum {
    MR_YUV_DIRECT,         /* destination is the frame's own size */
    MR_YUV_SCALED          /* anything else, nearest neighbour */
} mr_yuv_kind;

typedef struct {
    int src_w, src_h;
    int dst_w, dst_h;

    mr_yuv_kind kind;

    /* Destination column/row to source column/row. NULL when the kind is
     * MR_YUV_DIRECT, where the mapping is the identity and the loop walks
     * both planes with pointer arithmetic instead. */
    int *xmap, *ymap;
} mr_yuv_scaler;

/* Returns 0 if it could not allocate. Sizes must all be positive. */
int  mr_yuv_init(mr_yuv_scaler *s, int src_w, int src_h,
                 int dst_w, int dst_h);
void mr_yuv_free(mr_yuv_scaler *s);

/* Convert destination rows [r0, r1) of the frame into dst, placing the
 * picture's top-left corner at (dx, dy) in destination coordinates.
 *
 * Rows are written **bottom-up**, exactly as mr_scale_rows does and for the
 * same inherited reason -- Windows 3.1 GDI has no top-down DIB -- so
 * destination row y lands at dst row (dst->h - 1 - y). A caller that wants
 * the whole picture passes 0 and dst_h.
 *
 * Pixels are written opaque: alpha is 0xFF, not the frame's business. The
 * destination is the presenter's picture buffer, which is composited over
 * nothing.
 *
 * Clipped to dst. Does nothing if the scaler was not initialised for this
 * frame's size, which is the one mistake that would otherwise read off the
 * end of a plane -- a frame size can change mid-stream at a sequence header,
 * and the scaler is built from the size seen at the first one. */
void mr_yuv_rows(const mr_yuv_scaler *s, const mr_yuv_frame *f,
                 mr_surface *dst, int dx, int dy, int r0, int r1);

/* The same conversion written straight into display memory, skipping the
 * picture buffer and the pack pass altogether.
 *
 * `dst` and `pitch` are the pair the caller would otherwise hand
 * mr_pack_rows -- buffer row b at dst + b * pitch, pitch possibly negative
 * for a framebuffer that is stored the other way up. `dst_w` and `dst_h`
 * describe the picture buffer that pair addresses, and (dx, dy) and [r0, r1)
 * mean what they mean in mr_yuv_rows.
 *
 * **Returns 0 if it did nothing**, in which case the caller must convert into
 * the picture buffer and pack as before. That is not an error path: it is the
 * answer for every display format where a converted pixel is not already the
 * display's pixel, which is all of them except plain BGRA32. Treating the
 * return value as advisory would leave the screen holding the previous frame.
 *
 * Worth saying why this is here at all, since it is the one place the YUV
 * conversion knows a display format exists. On the target the pack costs more
 * than twice the conversion and is a `memcpy`: the framebuffer is mapped
 * uncached, so the cost is a bus transaction per store instruction rather
 * than per byte. The conversion already builds display-ready pixels in MMX
 * registers and stores them eight bytes at a time; routing them into main
 * memory so that a second pass can read them back and store them again is
 * the expensive part, and it is pure plumbing. See yuv.c for the figures and
 * for the 16bpp experiment that established the per-transaction cost.
 */
int mr_yuv_rows_fused(const mr_yuv_scaler *s, const mr_yuv_frame *f,
                      const mr_pack_fmt *fmt, mr_u8 *dst, long pitch,
                      int dst_w, int dst_h, int dx, int dy, int r0, int r1);

/* --- the fit --------------------------------------------------------------
 *
 * The largest box_w x box_h rectangle with the frame's aspect ratio, centred.
 * Separate from mr_fit_set because it answers a different question and nests
 * inside its answer: mr_fit puts the 16:9 *stage* inside the output, and this
 * puts the *video* inside that, so a 2:1 video in a 16:9 game area in a 4:3
 * mode is letterboxed twice and both sets of bars are correct.
 *
 * The video's own bars are what the brief calls the "optional black border
 * given the aspect ratio": they are only there when the ratios differ, and
 * for a 16:9 video in a 16:9 game area there are none.
 *
 * Rounded to even numbers on both axes. 4:2:0 chroma is shared by a 2x2 luma
 * block and an odd destination size makes the last row or column sample a
 * chroma pixel the one beside it does not, which shows up as a one-pixel
 * fringe of the wrong hue along two edges. Even sizes also keep the MMX
 * kernel's eight-pixel step from needing a scalar tail on every row. */
void mr_yuv_fit(int src_w, int src_h, int box_w, int box_h,
                int *out_w, int *out_h);

/* Black everything in `dst` *outside* the rectangle the video occupies --
 * the bars mr_yuv_fit's answer implies.
 *
 * Bottom-up, like mr_yuv_rows, and the rectangle is given in the same
 * picture coordinates: (vx, vy) is its top-left corner as mr_yuv_rows would
 * be asked to place it.
 *
 * --- why this is a function, and why it is called more often than it looks
 * like it needs to be ---
 *
 * The obvious implementation is to black the whole buffer once, when the
 * video's rectangle is worked out, and leave it: every pixel inside the
 * rectangle is rewritten by every frame, so the bars only have to be painted
 * when they move.
 *
 * That was the implementation, and it was wrong on Win32. The picture buffer
 * is shared with the *stage* path -- it is the same buffer mr_present scales
 * the composited stage into -- and the stage covers all of it, bars included.
 * main.c skips compositing while a cutscene runs, so this looked safe, but
 * platw32.c's WM_PAINT handler calls mr_present(mr_stage()) directly
 * (platw32.c:300) and does not go through main.c at all. So any repaint
 * during a cutscene -- a resize, an uncover, a window dragged -- fills the
 * bars with the scene that was there before, and the next video frame only
 * rewrites the rectangle. The reported symptom was a cutscene with the quick
 * menu and the previous background showing in the letterbox.
 *
 * The same happens across cutscenes without any resize: the stage is
 * presented when one ends, and if the next one has the same frame size the
 * scaler is not rebuilt and the bars were never repainted. That is the
 * "after rollback" half of the same report.
 *
 * So the bars are repainted whenever anything else has written to the
 * buffer, which the presenters track with a flag rather than doing it every
 * frame -- the narrow question ("could the bars have been overwritten?")
 * rather than the broad one, as CLAUDE.md asks. This only fills the strips,
 * not the whole buffer, so even getting that wrong in the safe direction is
 * cheap.
 *
 * Does nothing if the rectangle covers the whole surface, which is the
 * common case of a 16:9 video in a 16:9 picture area. */
void mr_yuv_bars(mr_surface *dst, int vx, int vy, int vw, int vh);

/* --- SIMD ----------------------------------------------------------------
 *
 * The MMX kernel is a hand-written inner loop for the MR_YUV_DIRECT case,
 * which is the one that runs at the default window size. See yuv.c for what
 * it does and for why this is the loop that got hand-written when the
 * decoder's hotter ones did not.
 *
 * On by default where it exists, because the Pentium III baseline has MMX.
 * mrpy.ini's `simd=0` turns it off, and so does building anywhere the kernel
 * is not compiled in -- the host harness, for one, where mr_yuv_simd()
 * returns 0 and the C path is what the suite tests.
 *
 * The switch is not for tuning. It is so that "the picture is wrong" and "the
 * MMX kernel is wrong" can be told apart on the machine, in one run, without
 * a rebuild -- which is the only way to tell them apart at all, since the
 * kernel cannot be reached from the host suite. probe/mmxcheck.c is the other
 * half: it runs both paths over the same frames on the target and exits
 * non-zero on the first pixel they disagree about. */
int  mr_yuv_simd(void);        /* 1 if the MMX path can run and is on */
void mr_yuv_set_simd(int on);
/* 1 if the kernel is compiled in *and this processor can execute it* -- the
 * CPUID question is asked here rather than at the call site. See cpu.h. */
int  mr_yuv_have_simd(void);

#endif /* MR_YUV_H */
