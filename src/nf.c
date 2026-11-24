/* nf.c - trace output to the host through Hatari's NatFeats (NF_STDERR).
 *
 * Built only with MW_NATFEATS: the NatFeats opcodes are illegal
 * instructions on a real ST, so a build for the machine must not contain
 * them. Run Hatari with --natfeats yes to see the output.
 */

#ifdef MW_NATFEATS

#include <stdarg.h>
#include <stdio.h>

#include "st.h"

/* The opcodes expect to be the body of a called function: the arguments
 * start at 4(sp), above a return address. Inline, a dummy long stands in
 * for it. */
static long nf_id(const char *name)
{
    register long ret __asm__("d0");

    __asm__ volatile("move.l %1,-(%%sp)\n\t"
                     "subq.l #4,%%sp\n\t"
                     ".dc.w 0x7300\n\t"
                     "addq.l #8,%%sp"
                     : "=r"(ret)
                     : "a"(name)
                     : "d1", "a0", "a1", "memory", "cc");
    return ret;
}

static void nf_call1(long id, const char *arg)
{
    __asm__ volatile("move.l %1,-(%%sp)\n\t"
                     "move.l %0,-(%%sp)\n\t"
                     "subq.l #4,%%sp\n\t"
                     ".dc.w 0x7301\n\t"
                     "lea 12(%%sp),%%sp"
                     :
                     : "d"(id), "a"(arg)
                     : "d0", "d1", "a0", "a1", "memory", "cc");
}

void trace(const char *fmt, ...)
{
    static long id = -1;
    char buf[2100];                 /* a path line (vm.c) is up to ~2KB */
    va_list ap;
    int n;

    if (id < 0)
        id = nf_id("NF_STDERR");
    if (!id)
        return;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(buf) - 3)
        n = (int)sizeof(buf) - 3;
    buf[n] = '\n';
    buf[n + 1] = '\0';
    nf_call1(id, buf);
}

#endif
