/* blendmmx.h - the arithmetic the MMX alpha blend uses, written down so it
 * can be proved somewhere the kernel itself cannot run.
 *
 * Same arrangement as yuvmmx.h, and for the same reason: src/blendmmx.c is
 * Open Watcom inline assembly, so the harness suite on the build host cannot
 * compile a line of it, never mind execute one. The risk is split in two and
 * each half is checked where it can be:
 *
 *   the algebra        Does the blend survive being done in 16-bit lanes?
 *                      MMX multiplies and adds words, so every intermediate
 *                      has to fit in 16 bits unsigned or it wraps silently.
 *                      That is a question about integers, and blendtest
 *                      answers it **exhaustively** -- all 16,777,216
 *                      (src, dst, alpha) triples, not a sample.
 *
 *   the transcription  Does the assembly compute that algebra, and put the
 *                      bytes back in the order a surface wants? Only the
 *                      target can say, and probe/mmxcheck.c asks it: it runs
 *                      both paths over the same surfaces and exits non-zero
 *                      on the first pixel they disagree about.
 *
 * Neither half is worth anything alone. What is left unchecked by both is
 * only "does MMX exist on this machine", which is what mrpy.ini's `simd=0`
 * is for.
 *
 * --- what is novel here, and what is not ----------------------------------
 *
 * Unlike yuvmmx.h there is no new formula. The kernel computes exactly what
 * gfx.c's scalar loop computes, per channel:
 *
 *     MR_DIV255(s*a + d*(255-a))
 *
 * with MR_DIV255 spelled out of mr.h as `t = x + 128; (t + (t >> 8)) >> 8`.
 * The novelty is *the lane width*, and it is the only thing that could make
 * the vector answer differ from the scalar one. So that is what the macros
 * below pin: the same arithmetic with every step truncated to 16 bits, the
 * way `pmullw` and `paddw` truncate.
 *
 * It fits, and with room to spare at every step. The bound worth writing out
 * is the first one, because the rest follow from it:
 *
 *     x = s*a + d*(255-a)  <=  255*a + 255*(255-a)  =  255*255  =  65025
 *
 * -- the maximum is at s = d = 255 and is independent of a, which is the
 * useful part: there is no alpha that pushes it higher. Then
 *
 *     x + 128            <=  65153   < 65536   ok
 *     (x+128) + (x+128)>>8 <= 65407  < 65536   ok
 *     result             <=  255
 *
 * so no step wraps and the vector result is the scalar result. The margin on
 * the tightest step is 129, which is not much; it is also not a coincidence,
 * since MR_DIV255's whole design is to keep `(x+127)/255` inside 16 bits
 * (mr.h says so at more length). If anyone ever widens a channel past 8 bits
 * or changes the rounding bias, this is the file that stops being true, and
 * blendtest is what will say so.
 *
 * --- why the endpoints are still branched in scalar ------------------------
 *
 * The lane arithmetic is exact at a = 0 and a = 255 -- MR_DIV255(d*255) is d
 * and MR_DIV255(s*255) is s -- so the kernel would be *correct* over the
 * whole alpha range and could take every pixel. It deliberately does not.
 *
 * `a == 0` must not write the destination at all. The scalar loop `continue`s
 * on it, leaving the destination pixel's alpha byte alone, where the kernel
 * forces alpha to 255 as MR_ARGB does. On the stage that is a distinction
 * without a difference, because the stage's alpha is 255 everywhere -- but
 * mr_blit is general and the harnesses blit into surfaces it is not, so
 * "identical to the scalar path" is cheaper to keep than to reason about.
 *
 * `a == 255` is a plain store, and no vector sequence beats one.
 *
 * Keeping both also keeps the thing that makes sprites fast. Measured, a
 * sprite blend already runs at 47.9-49.0 ns/px against a ~45 ns/px memory
 * floor, because those two branches skip the arithmetic on most of a
 * bounding box; a kernel that swallowed them whole would have made the
 * engine's sprites slower to make its textbox faster. The textbox is the
 * thing this was written for -- 144,076 of its 148,000 pixels are at partial
 * alpha, so it takes the vector path on essentially every pixel. It went
 * 129.7 -> 81.4 ns/px, and the sprites stayed at 49.5 against a scalar 49.2
 * -- but only after a first version whose wrapper cost them 20%, which is the
 * story in blendmmx.c beside the two pointer declarations.
 */

#ifndef MR_BLENDMMX_H
#define MR_BLENDMMX_H

#include "mr.h"

/* One 16-bit lane, as `pmullw` and `paddw` leave it. */
#define MR_BL_W(x)          ((unsigned)(x) & 0xFFFFu)

/* The blend of one channel, step by step, each step named for the
 * instruction that performs it and truncated the way that instruction
 * truncates. Written as macros rather than described in prose so that the
 * kernel's author and the test's author are reading the same line, and so
 * that this comment cannot drift away from what is proved.
 *
 * s, d and a are channel values in 0..255. */
#define MR_BL_PROD(v, m)    MR_BL_W((unsigned)(v) * (unsigned)(m))     /* pmullw */
#define MR_BL_SUM(s, d, a)  MR_BL_W(MR_BL_PROD(s, a) + \
                                    MR_BL_PROD(d, 255u - (unsigned)(a)))  /* paddw */
#define MR_BL_BIAS(s, d, a) MR_BL_W(MR_BL_SUM(s, d, a) + 128u)         /* paddw */

/* The MR_DIV255 tail: t + (t >> 8), then >> 8. Two psrlw and a paddw. */
#define MR_BL_LANE(s, d, a) \
    (MR_BL_W(MR_BL_BIAS(s, d, a) + (MR_BL_BIAS(s, d, a) >> 8)) >> 8)

/* And the same channel the way gfx.c computes it, in 32-bit arithmetic. The
 * claim blendtest proves is that these two are equal for every s, d and a.
 * Kept here beside the lane version rather than left implicit in the test,
 * because the pair *is* the specification. */
#define MR_BL_REF(s, d, a) \
    MR_DIV255((unsigned)(s) * (unsigned)(a) + \
              (unsigned)(d) * (255u - (unsigned)(a)))

/* Blend n pixels of src over dst with the MMX kernel, exactly as gfx.c's
 * scalar loop would -- including leaving a fully transparent source pixel's
 * destination untouched, and storing a fully opaque one directly.
 *
 * Only declared when the kernel is compiled in. Callers go through
 * mr_blend_simd() rather than testing the macro, so that the run-time switch
 * and the compile-time one are asked in one place (gfx.c). */
#ifdef MR_BLEND_MMX
void mr_blend_row_mmx(mr_u32 *dp, const mr_u32 *sp, int n);
#endif

/* Can the kernel run here, and is it on?
 *
 * Both live in gfx.c beside the scalar loop they switch between, following
 * yuv.c. mrpy.ini's `simd` turns it off at run time -- the same key the
 * colour conversion reads, because a machine that wants one off wants the
 * other off.
 *
 * "Can run" is two questions and mr_blend_have_simd() asks both: whether
 * MR_BLEND_MMX was defined, and whether *this processor* has MMX. The second
 * is CPUID, via src/cpu.c, and it is asked inside the predicate so that no
 * call site can forget it -- see cpu.h, which also explains why `simd` has a
 * third value for overruling the answer. */
int  mr_blend_have_simd(void);
int  mr_blend_simd(void);
void mr_blend_set_simd(int on);

#endif /* MR_BLENDMMX_H */
