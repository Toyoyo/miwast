/* cpu.h - what this processor can actually do, asked once at startup.
 *
 * The engine has two hand-written MMX kernels -- src/yuvmmx.c for the
 * cutscene colour conversion and src/blendmmx.c for the textbox blend -- and
 * until this file existed, both were enabled by a compile-time #define and an
 * ini key and nothing else. The Watcom builds define the macro, so the
 * kernels ran on whatever the exe was started on. On a 486 or a Cyrix 5x86
 * that is an invalid-opcode fault at the first `punpcklbw`, somewhere deep in
 * the first frame, with no log line and nothing to read afterwards.
 *
 * `simd=0` in mrpy.ini was the only answer, which requires knowing to write
 * it on a machine that crashes before drawing anything.
 *
 * So the kernels now ask the processor. This follows xvidcore's
 * check_cpu_features (libs/xvidcore-1.3.7/src/utils/x86_asm/cpuid.asm) rather
 * than inventing a sequence, because it is the detector already vendored in
 * this tree, it has been run on far more machines than this engine ever will
 * be, and the awkward part is not the feature bit but everything that has to
 * happen before CPUID can be executed at all.
 *
 * --- the part that is not just "read a bit" --------------------------------
 *
 * CPUID is itself a 486-and-later instruction, so asking for it is the same
 * fault it is meant to prevent. The test is indirect, and is xvid's
 * (cpuid.asm:103-111): **bit 21 of EFLAGS, the ID flag, is writable only on a
 * processor that has CPUID.** Toggle it with pushfd/popfd, read it back, and
 * compare. On a 386 or an early 486 it reads back unchanged.
 *
 * Then leaf 0 reports the highest leaf the processor supports, and leaf 1
 * must not be executed unless that is at least 1 -- xvid checks this too
 * (cpuid.asm:122), and it is not hypothetical: the very earliest CPUID
 * steppings answer leaf 0 and nothing else. Only then does EDX bit 23 mean
 * "MMX".
 *
 * Three conditions, in that order, and skipping any of them faults on exactly
 * the processor the check was added for.
 *
 * --- why only MMX ----------------------------------------------------------
 *
 * xvid's detector reports MMXEXT, SSE, SSE2, SSE3, SSE41, 3DNow and the TSC,
 * and needs them: it has a kernel for each. We have two kernels and both are
 * plain MMX, so one bit is the honest answer and a flag word full of features
 * nothing consults would only invite someone to consult them. MR_CPU_FLAGS is
 * a bitmask anyway, so the next kernel adds its bit here.
 *
 * Deliberately *not* reported: the vendor string is read (leaf 0 fills it as
 * a side effect of asking for the leaf count) and goes into the log line, but
 * nothing branches on it. Vendor-specific behaviour is where this kind of
 * code goes wrong, and the AMD extended leaves xvid reads are for 3DNow and
 * mmxext, neither of which we have a kernel for.
 */

#ifndef MR_CPU_H
#define MR_CPU_H

/* One bit per feature a kernel in this engine needs. */
#define MR_CPU_MMX      0x0001u

/* CPUID leaf 1, EDX bit 23. The same constant xvid spells CPUID_MMX
 * (cpuid.asm:34). */
#define MR_CPUID_EDX_MMX  0x00800000u

/* EFLAGS bit 21, the ID flag: writable iff CPUID exists (cpuid.asm:107). */
#define MR_EFLAGS_ID      0x00200000u

/* The whole decision, given what CPUID returned: the highest supported leaf
 * and leaf 1's EDX.
 *
 * It is a macro and it lives in the header because the assembly that feeds it
 * cannot run on the build host, and this is the half that can be checked
 * there -- the same split as yuvmmx.h and blendmmx.h. cputest drives it with
 * synthesised leaf values, including the ones no processor in this tree's
 * reach would produce. What is left for the target is whether the sequence
 * that produces `maxleaf` and `edx` is correct, which probe/cpuchk.c prints
 * and the engine's own log line records on every run.
 *
 * `maxleaf` is checked before `edx` is trusted because an `edx` read from a
 * leaf that was never executed is whatever was in the register. */
#define MR_CPU_DECODE(maxleaf, edx) \
    ((((unsigned)(maxleaf) >= 1u) && \
      (((unsigned)(edx) & MR_CPUID_EDX_MMX) != 0u)) ? MR_CPU_MMX : 0u)

/* The detected feature bits. Detection happens on the first call and the
 * answer is cached: CPUID is serialising and costs a few hundred cycles, and
 * mr_blend_have_simd() is asked per blit. */
unsigned mr_cpu_flags(void);

/* The one question the kernels ask. */
int mr_cpu_has_mmx(void);

/* Claim MMX whether or not it was detected -- mrpy.ini's `simd=2`.
 *
 * This exists because the detector is now the thing most likely to be wrong,
 * and its failure mode is silent: a machine that reports no CPUID loses both
 * kernels and runs 7ms slower a frame with nothing to say why. pushfd/popfd
 * under a V86 monitor is the case worth worrying about. xvidcore keeps the
 * same escape hatch and calls it XVID_CPU_FORCE (xvid.h), which is where the
 * idea comes from.
 *
 * **It is not a safety net and it is the only dangerous setting here.** On a
 * processor that genuinely has no MMX this reintroduces exactly the
 * invalid-opcode fault the detection removes -- a freeze in the first frame
 * with an empty log. It is for the *false negative* and nothing else. The safe
 * direction is `simd=0`.
 *
 * Call before anything asks mr_cpu_flags(), which in practice means startup;
 * it takes effect immediately either way, but a kernel already skipped is
 * skipped. */
void mr_cpu_force_mmx(void);

/* One line for the log: vendor, family/model/stepping, and what was found.
 * Static storage, built on first use.
 *
 * Worth the code it costs because it is the only evidence that reaches us
 * from a machine we do not have. "no CPUID" and "mmx" and "no mmx" are three
 * different bug reports and they look identical from a chair. */
const char *mr_cpu_desc(void);

#endif /* MR_CPU_H */
