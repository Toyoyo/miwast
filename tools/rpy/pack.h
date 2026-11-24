/* pack.h - turning 32bpp ARGB rows into whatever the display wants.
 *
 * The last step before the pixels reach hardware, and the only part of that
 * step that is arithmetic rather than plumbing. It is here, on its own, for
 * the same reason the VBE mode picker is: on DOS a wrong answer in this code
 * is a sheared or wrongly coloured screen on a machine with no debugger and
 * no log, while here it is a harness that prints the bytes it got.
 *
 * Both presenters use it. present.c (GDI) packs 24bpp and 8bpp DIBs;
 * presdos.c packs whatever depth the VBE mode turned out to be. Neither of
 * them owns a pack loop any more, which means the Win32 build exercises this
 * code on every frame and the DOS paths at least share its structure.
 *
 * Nothing in here touches a window, a framebuffer or a palette register: a
 * source surface and a destination pointer with a pitch go in, bytes come
 * out. The caller says where the memory is.
 */

#ifndef MR_PACK_H
#define MR_PACK_H

#include "mr.h"

/* The destination pixel format.
 *
 * `bpp` is the *storage* size -- 8, 16, 24 or 32 -- not the number of
 * significant bits. A 15bpp mode (5:5:5) is bpp 16 with rsize+gsize+bsize of
 * 15, which is exactly how VBE reports it, and getting that distinction wrong
 * is what makes a 32bpp mode with a blank RsvdMaskSize stride 24 bits through
 * a 32-bit buffer and shear. See mr_vbe_decode, which already resolves it.
 *
 * The field positions and sizes are the generic description: red occupies
 * `rsize` bits starting at bit `rpos`. They are ignored when `bpp` is 8, and
 * they come straight from the VBE ModeInfoBlock otherwise, so a card with an
 * unusual layout is handled without anybody enumerating it here.
 */
typedef struct {
    int bpp;
    int rpos, rsize;
    int gpos, gsize;
    int bpos, bsize;
} mr_pack_fmt;

/* The ordinary little-endian B,G,R layout, for callers that are not reading a
 * mode from a BIOS: 8 (palettised), 16 (5:6:5), 24 and 32 (0xAARRGGBB, which
 * on x86 is bytes B,G,R,A -- a Windows BI_RGB DIB and a VGA-style linear
 * framebuffer agree here). Returns 0 and leaves *f alone for any other depth.
 */
int mr_pack_fmt_std(mr_pack_fmt *f, int bpp);

/* Pack source rows [y0, y1) into dst, row y landing at dst + y * pitch.
 *
 * The row index is used as well as the offset: the 8bpp dither reads it, so
 * packing a band must pass the same y the whole-frame path would, or the
 * dither pattern shifts at the band edge and leaves a visible seam.
 *
 * Rows are clamped to the surface. pitch is in bytes and may exceed the row's
 * width, which it usually does -- DIB rows are DWORD aligned and a VBE
 * scanline is whatever the card says it is.
 *
 * pitch may also be *negative*, which steps upward through the destination
 * and so writes the picture bottom-up. That is not a curiosity: the scalers
 * in scale.c write their output bottom-up, because a Windows DIB is stored
 * that way, and a linear framebuffer is stored the other way up. Rather than
 * give scale.c a second orientation or flip a megabyte per frame, the DOS
 * presenter points dst at the *last* row of the picture and passes a negative
 * pitch. The dither phase still follows the row index, so a band is still
 * identical to the same rows of a full frame.
 */
void mr_pack_rows(const mr_pack_fmt *f, const mr_surface *src,
                  mr_u8 *dst, long pitch, int y0, int y1);

/* Ordered-dither the quantisation to 15 and 16bpp, instead of truncating.
 *
 * On by default, because truncation is what made the 16bpp path look worse
 * than the 24bpp one: it loses up to 7 counts a channel, always downwards and
 * identically on every pixel, so the error is correlated across whole regions
 * and reads as banding. Dithering removes that and rounds at the same time --
 * measured on real backgrounds at 0.69 against the 24bpp path's 3.35, error
 * after a blur. See the comment above pack16.
 *
 * The 8bpp palette path dithers unconditionally and always has; this switch is
 * only about the 15/16bpp and generic loops. It exists because the only oracle
 * for "does the screen look right" is the screen, and this is a file whose
 * output has been wrong on real hardware before -- not because there is a
 * reason to turn it off. `dither=0` in mrpy.ini.
 */
void mr_pack_dither(int on);

/* Snap a rectangle out to the dither grid, then clip it to a surface.
 *
 * For repacking a *part* of a picture -- which on DOS means the few hundred
 * pixels under the mouse pointer, repainted as it moves. The natural way to
 * do that is to point mr_pack_rows at a sub-rectangle of the source, since
 * an mr_surface with a pitch already describes one; but then the row and
 * column indices the 8bpp dither reads start from the corner of the
 * rectangle rather than from the corner of the picture, and the patch comes
 * out in the wrong phase. On a flat colour that is a visible 4x4 square
 * following the pointer around.
 *
 * Growing the rectangle until both corners sit on a multiple of 4 costs at
 * most three pixels a side and makes the phases agree by construction.
 * Returns 0 if nothing is left after clipping, in which case the rectangle
 * is not worth painting.
 *
 * It is here rather than in the caller because it is a property of the
 * dither, which is here; the caller has no reason to know the period.
 */
/* Build a format from channel bit masks, the way both a BI_BITFIELDS DIB and
 * a VBE ModeInfoBlock describe themselves.
 *
 * mr_pack_fmt_std covers the layouts we choose ourselves; this covers the one
 * the *display* chooses and tells us about. present.c asks GDI what a 16bpp
 * pixel looks like on this machine -- 5:6:5 and 5:5:5 are both common and
 * guessing tints the whole screen -- and hands the answer straight here.
 *
 * Returns 0, and leaves `f` refusing to pack anything, for a mask with a hole
 * in it, channels that overlap, or a channel running past the end of the
 * pixel. Those are rejected rather than approximated: this is the only entry
 * point that cannot see the layout it is given, it runs on a machine where a
 * wrong answer is a silently discoloured screen, and its caller's fallback
 * (keep the 24bpp DIB) is always correct.
 */
int mr_pack_fmt_masks(mr_pack_fmt *f, int bpp,
                      mr_u32 rmask, mr_u32 gmask, mr_u32 bmask);

/* Is this format one where a 32bpp 0xAARRGGBB surface pixel is already the
 * display's pixel, so that packing it is a copy and nothing else?
 *
 * Asked by mr_yuv_rows_fused, which skips the picture buffer entirely and
 * converts a frame of video straight into display memory. That is only sound
 * where no repacking is needed, and "no repacking needed" is a property of
 * the format -- so the question is answered here, where the format is
 * interpreted, rather than by a caller comparing field positions itself and
 * getting a different answer from pack32's.
 */
int mr_pack_is_bgra32(const mr_pack_fmt *f);

int mr_pack_snap_rect(int *x, int *y, int *w, int *h, int clip_w, int clip_h);

/* The 256-entry palette the 8bpp path quantises to: a 6x6x6 colour cube in
 * entries 0..215 and a 40-step grey ramp above it. Written as 768 bytes,
 * R,G,B per entry, each 0..255 -- so a VGA DAC write needs them shifted right
 * by two and GDI's RGBQUAD needs them as they are.
 *
 * 216 entries for the cube leaves room for the ramp, which matters because
 * both games so far are full of dark interiors and night scenes that a 6-level
 * cube renders in visible bands.
 */
#define MR_PACK_PAL_BYTES 768
void mr_pack_palette(mr_u8 *rgb);

#endif /* MR_PACK_H */
