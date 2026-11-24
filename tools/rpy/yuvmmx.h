/* yuvmmx.h - the arithmetic the MMX kernel uses, written down so it can be
 * proved somewhere the kernel itself cannot run.
 *
 * The problem this file exists to solve: the MMX colour conversion is Open
 * Watcom inline assembly, so it is not compiled on the build host at all and
 * the harness suite cannot execute a single instruction of it. CLAUDE.md is
 * blunt about what that is worth -- "a vacuous assertion is the default
 * outcome" -- and a hand-written kernel on the hottest loop in video playback
 * is the last place to accept one.
 *
 * So the risk is split in two, and each half is checked where it can be:
 *
 *   the algebra        Is `yt = (Y-16) + (((Y-16)*10773) >> 16)` the same
 *                      number as `((Y-16)*76309) >> 16`, for every Y? That is
 *                      a question about integers, not about MMX, and yuvtest
 *                      answers it *exhaustively* on the host -- all 256
 *                      values, not a sample.
 *
 *   the transcription  Does the assembly compute the algebra? Only the target
 *                      can say, and probe/mmxcheck.c asks it: it runs both
 *                      paths over the same frames and exits non-zero on the
 *                      first pixel they disagree about.
 *
 * Neither half is interesting on its own and together they cover it. What is
 * left unchecked by both is only "does MMX exist on this machine", which is
 * what mrpy.ini's `simd=0` is for.
 *
 * --- why the luma term is decomposed --------------------------------------
 *
 * MMX multiplies 16-bit lanes. `pmulhw` gives the top 16 bits of a signed
 * 16x16 product, which is exactly `(a * b) >> 16` -- but only if `b` fits in
 * a signed 16-bit lane, and the luma coefficient 76309 does not.
 *
 * It splits, though, and splits *exactly*:
 *
 *     76309 = 65536 + 10773
 *
 *     ((Y-16) * 76309) >> 16
 *         = ((Y-16)*65536 + (Y-16)*10773) >> 16
 *         = (Y-16) + (((Y-16)*10773) >> 16)
 *
 * The second step is the one worth pausing on, because shifting a sum is not
 * generally the sum of the shifts. It is exact here because the first term is
 * a whole multiple of 65536: for any integers a and R,
 * `(a*65536 + R) >> 16 == a + (R >> 16)` under arithmetic (floor) shift, with
 * no rounding to lose. So the decomposition is an identity and not an
 * approximation, and 10773 fits a 16-bit lane with room to spare.
 *
 * --- and why the chroma terms are not -------------------------------------
 *
 * They could be -- `pmaddwd` would do `cr*104597` exactly as
 * `(cr*4)*26149 + cr*1` -- and they are deliberately left in scalar C anyway.
 *
 * Count the work. A 4:2:0 chroma sample is shared by the two luma pixels
 * beside it, so eight pixels need four chroma samples and twelve multiplies,
 * against eight luma multiplies plus twenty-four clamps and eight stores.
 * The per-pixel half is the bulk, and it is the half that vectorises without
 * any rearrangement: `packuswb` saturates to 0..255 for free, which is the
 * clamp table gone, and `punpck` interleaves straight into the destination's
 * B,G,R,A order.
 *
 * Vectorising the chroma would mean widening to 32-bit lanes, two `pmaddwd`
 * per component, three shifts, and packing back to words -- a dozen
 * instructions of shuffling to save three multiplies, plus a second piece of
 * novel algebra to get wrong. The chroma stays byte-for-byte the same C code
 * as the scalar path, which is also why a disagreement found by mmxcheck can
 * only be in the luma or the packing.
 */

#ifndef MR_YUVMMX_H
#define MR_YUVMMX_H

/* The luma coefficient, and its split. MR_YUV_K_LUMA is what the scalar path
 * multiplies by; the other two are what the kernel uses instead.
 *
 * MR_YUV_K_LUMA_HI is the part that is a whole multiple of 65536, expressed
 * as the multiplier it becomes after the shift -- which is 1, so the kernel
 * adds (Y-16) itself rather than multiplying by anything. It is named rather
 * than left as a bare `1` so that the identity below reads as arithmetic. */
#define MR_YUV_K_LUMA     76309
#define MR_YUV_K_LUMA_HI  1          /* 65536 >> 16 */
#define MR_YUV_K_LUMA_LO  10773      /* 76309 - 65536; fits a 16-bit lane */

/* The luma bias. Studio-range video puts black at 16, not 0. */
#define MR_YUV_BLACK      16

/* The identity yuvtest proves for every Y in 0..255. Written as a macro so
 * the test and this comment cannot drift apart, and so the kernel's author
 * and the test's author are reading the same line.
 *
 * Both sides must use arithmetic shift on a negative value, which Y < 16
 * produces. C89 leaves that implementation-defined; every compiler this
 * builds under does it, and pl_mpeg's own conversion relies on it
 * identically. yuv.c says the same thing at more length. */
#define MR_YUV_LUMA_SCALAR(Y) \
    ((((int)(Y) - MR_YUV_BLACK) * MR_YUV_K_LUMA) >> 16)

#define MR_YUV_LUMA_SPLIT(Y) \
    (((int)(Y) - MR_YUV_BLACK) * MR_YUV_K_LUMA_HI + \
     ((((int)(Y) - MR_YUV_BLACK) * MR_YUV_K_LUMA_LO) >> 16))

/* The chroma coefficients, unchanged from the scalar path and from pl_mpeg's
 * PLM_PUT_PIXEL (libs/pl_mpeg.h:3607). Here only so that the kernel's scalar
 * preamble and yuv.c's loop cannot drift; neither of them is vectorised. */
#define MR_YUV_K_R_CR     104597
#define MR_YUV_K_G_CB      25674
#define MR_YUV_K_G_CR      53278
#define MR_YUV_K_B_CB     132201

#endif /* MR_YUVMMX_H */
