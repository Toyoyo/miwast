/* scale.h - the stage-to-output scalers, with no GDI in them.
 *
 * Split out of present.c so a harness can drive them. present.c is the one
 * file the suite cannot reach -- it is HDCs and DIBs from top to bottom, and
 * it is not in run.sh's source list -- which meant the three scalers, the
 * only part of it that is pure arithmetic, had no coverage either. A seam one
 * row deep at the edge of a partial frame is exactly the sort of thing that
 * is obvious once you know to look and invisible until then.
 *
 * Everything here works on mr_surface and knows nothing about the DIB, except
 * for one inherited quirk: output rows are written bottom-up, because Windows
 * 3.1 GDI has no top-down DIBs and a negative biHeight there draws nothing
 * rather than failing. Destination row y lands at buffer row (dst_h - 1 - y).
 * See mr_scale_rows.
 */

#ifndef MR_SCALE_H
#define MR_SCALE_H

#include "mr.h"

/* How a given source/destination size pair is scaled. Chosen once per resize
 * rather than per band, and exposed so the harness can assert which path a
 * size lands on -- "bilinear is on" and "bilinear is what runs" are different
 * claims, and at 2:1 the second one is deliberately false. */
typedef enum {
    MR_SCALE_COPY,       /* 1:1, rows reversed and nothing else */
    MR_SCALE_HALF,       /* exactly 2:1 on both axes: a 2x2 box filter */
    MR_SCALE_POINT,      /* anything else, nearest neighbour */
    MR_SCALE_BILINEAR    /* anything else, with mrpy.ini's bilinear=1 */
} mr_scale_kind;

typedef struct {
    int  src_w, src_h;
    int  dst_w, dst_h;

    mr_scale_kind kind;

    /* Per destination pixel: the source index, and an 8.8 fraction towards
     * the next source pixel. The fraction tables are NULL unless the kind is
     * MR_SCALE_BILINEAR, so the option costs no memory when it is off. */
    int *xmap, *ymap;
    int *xfrac, *yfrac;
} mr_scaler;

/* Returns 0 if it could not allocate. `bilinear` is the ini setting; it is a
 * request, not a result -- read s->kind to find out what it got.
 *
 * Note that bilinear is deliberately ignored for the 1:1 and exact-2:1 cases.
 * At 1:1 it is the identity. At exactly 2:1 a 2x2 box average *is* the
 * bilinear result, and the existing MR_SCALE_HALF computes it with two
 * multiplies rather than twelve, so switching would cost time to produce the
 * same pixels. This is why turning the option on changes nothing at the
 * default window size. */
int  mr_scale_init(mr_scaler *s, int src_w, int src_h,
                   int dst_w, int dst_h, int bilinear);
void mr_scale_free(mr_scaler *s);

/* Scale destination rows [r0, r1) from src into dst.
 *
 * dst must be dst_w x dst_h. Output is written bottom-up: destination row y
 * goes to dst row (dst_h - 1 - y). A caller that wants the whole picture
 * passes 0 and dst_h.
 *
 * The range is destination rows, not source rows, because the partial-frame
 * path scales only the strip the dialogue box occupies. Bilinear reads one
 * source row past what the other scalers do, which is why present.c rounds
 * its band outwards by a row at each end. */
void mr_scale_rows(const mr_scaler *s, const mr_surface *src,
                   mr_surface *dst, int r0, int r1);

/* --- the fit: where the stage lands inside the output --------------------
 *
 * The stage is 1280x720 and the output is whatever the platform has -- a
 * window the player dragged to an odd size, or a video mode. The largest
 * 16:9 rectangle that fits goes in the middle and the rest is letterbox.
 *
 * This lives here, with the other arithmetic, rather than in a presenter,
 * for two reasons. Both presenters need the same answer, and -- the reason
 * it moved -- input.c needs it too, to turn a click into a stage
 * coordinate. While it was a static in present.c, every file that wanted to
 * map a point had to link GDI, which is why input.c could not be put in a
 * harness.
 *
 * One output, so one fit, held module-wide rather than passed about. The
 * presenter sets it from its resize path; everyone else reads.
 */

/* Recompute for an output of this size. Sizes below 16 in either direction
 * are ignored, matching what the presenter did with them. */
void mr_fit_set(int out_w, int out_h);

/* The destination rectangle, and the output it sits in -- the presenter
 * needs both to paint the bars. Any pointer may be NULL. */
void mr_fit_get(int *x, int *y, int *w, int *h, int *out_w, int *out_h);

/* Map an output point onto the stage. Returns 0 if it landed in the
 * letterbox, in which case *sx and *sy are untouched. */
int  mr_fit_map_point(int ox, int oy, int *sx, int *sy);

/* --- the letterbox bars ---------------------------------------------------
 *
 * One row of a letterbox bar, filled from a texture: scaled to the height of
 * the bar and repeated horizontally across it. That shape is the point -- a
 * bar is a short, very wide rectangle whose height depends on the mode and
 * whose width is the whole screen, so no single image can be the right size.
 * The author draws one tile at whatever size suits them; it is fitted to the
 * bar's height and repeated along it.
 *
 * The scale is the *same* on both axes -- one tile is tex->w * bar_h / tex->h
 * pixels wide -- so the art keeps its proportions. Scaling only the height
 * and tiling at the texture's stored width is what this deliberately is not:
 * a tile wider than the bar then never wraps, and all that reaches the
 * screen is the left-hand part of a squashed image.
 *
 * `row` is counted from the top of the bar. The source row is picked
 * nearest-neighbour: the bars are painted once when the mode is set, so the
 * cost would not matter, but a gradient strip stretched point-wise over a
 * bar a hundred pixels tall is indistinguishable from an interpolated one
 * and the seam-free guarantee is easier to state.
 *
 * `out` receives out_w pixels in the stage's own 0xAARRGGBB. A texture with
 * alpha is composited over black here rather than handed on, because the
 * pack loops have nowhere to put an alpha channel and a half-transparent
 * border would otherwise come out at full strength over whatever the
 * transparent pixels happened to carry.
 *
 * Does nothing if the texture is empty or bar_h is not positive; `row` is
 * clamped into the bar.
 */
void mr_border_row(const mr_surface *tex, int bar_h, int row,
                   mr_u32 *out, int out_w);

#endif /* MR_SCALE_H */
