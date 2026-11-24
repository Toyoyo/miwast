/* dosmem.h - memory a DOS device can reach, and memory it cannot lose.
 *
 * Two things that both halves of the DOS port need and that both got wrong
 * on their own at least once between them.
 *
 * **Conventional memory.** Firmware and hardware do not know about the
 * protected-mode address space we live in: the VESA BIOS wants a buffer it
 * can address with a real-mode segment, and the DMA controller wants a
 * physical address below the 16MB line that it can be handed in three
 * registers. Both mean a block out of the first megabyte, which only DOS can
 * hand out.
 *
 * **Locking.** Under DOS/32A and DOS/4GW nothing pages, so a locked region
 * and an unlocked one are the same thing. Under a host that does page --
 * CWSDPMI, HDPMI32, a DOS box inside Windows -- an interrupt handler that
 * touches a swapped-out page faults inside the interrupt, which is a hang
 * with no message on the screen and nothing in the log. Anything an IRQ
 * reads or writes has to be locked first.
 *
 * It is one file because of what happens when it is two. A real-mode
 * *segment* and a protected-mode *selector* are both `unsigned short`;
 * DPMI 0100h returns one of each and DPMI 0101h wants the selector. Getting
 * that backwards compiles in silence and cost this port a hang at process
 * exit that looked like anything but a memory bug -- MODES.EXE printed its
 * entire output correctly and then never returned to the prompt, because the
 * damage only surfaces when DOS walks the arena to reclaim the process. See
 * NOTES. Two copies of that reasoning is one copy too many.
 */

#ifndef MR_DOSMEM_H
#define MR_DOSMEM_H

/* A block of conventional memory, as DPMI 0100h describes it: two numbers,
 * and they are not interchangeable. Named rather than returned loose for
 * exactly that reason. */
typedef struct {
    unsigned seg;   /* real-mode segment: what firmware and DMA are given */
    unsigned sel;   /* protected-mode selector: what 0101h frees */
} mr_dos_block;

/* Allocate `paras` paragraphs (16 bytes each) below the 640K line. Returns 0
 * and leaves the block zeroed if it could not. */
int  mr_dos_alloc(mr_dos_block *b, unsigned paras);

/* Give it back. Safe on a block that was never allocated, and safe twice. */
void mr_dos_free(mr_dos_block *b);

/* The flat address of a block, which under the zero-based flat model of
 * DOS/32A and DOS/4GW is its physical address too. */
unsigned long mr_dos_linear(const mr_dos_block *b);

/* DPMI 0600h / 0601h: lock a linear region against paging, or let it go.
 * Returns 0 if the host refused; on a host that does not page at all that is
 * not worth failing over, so callers log it and carry on. */
int  mr_dpmi_lock(void *addr, unsigned long len);
void mr_dpmi_unlock(void *addr, unsigned long len);

/* Lock the program itself: code, data, BSS and the stack.
 *
 * An interrupt handler may not touch anything that could be paged out,
 * because the page fault would happen in interrupt context and a DPMI host
 * cannot generally service that -- it is a hang with nothing in the log.
 * The buffers a handler reads are locked where they are allocated; this
 * covers the other half, which is the handlers' own code, the statics they
 * keep their state in, and the stack they run on.
 *
 * Call it once, before any vector is hooked. Returns 0 if the host would
 * not lock something, having logged what; the caller carries on, because
 * on the extenders this build targets nothing pages and the locking is a
 * formality. It stops being one inside a Windows DOS box.
 *
 * Not covered: the heap. Anything malloc'd that a handler touches has to
 * be locked by whoever allocated it, which is what wavedos.c does for the
 * mixer buffers. */
int  mr_dpmi_lock_image(void);

/* Conventional memory still free, in bytes, or -1 if the host will not say.
 *
 * Asks the host to allocate a megabyte below the 640K line, which cannot
 * succeed, and reads the largest-available figure it refuses with. Nothing
 * is allocated.
 *
 * It is logged at two points -- at startup and again once the DMA ring is
 * in place -- so that the readme can state a real requirement rather than
 * an arithmetic one. The difference between the two is what this engine
 * takes for itself; the difference between MEM at the prompt and the first
 * of them is what DOS/32A and the stub take, which is the larger half and
 * is not ours to calculate. See NOTES, "How much conventional memory". */
long mr_dos_conventional_free(void);

/* Three lines in the log: where the image ends, where malloc starts, and
 * what DPMI 0500h says is free. Cheap, once, at startup, and independent
 * of the locking above -- `dpmilock=0` is one of the things being tried
 * when the memory picture is wanted.
 *
 * It earns its place because DOS has no other way to ask. Read the free
 * figure against the machine's actual RAM, though: during the FreeDOS
 * hunt it moved by 190MB between two runs and the cause was the VM being
 * resized. See NOTES, "Not with Jemm". */
void mr_dpmi_report_memory(void);

/* Watch four bytes and trap on the instruction that writes them.
 *
 * DPMI 0B00h, which is the debug registers with a portable face on. This
 * is the tool for "something writes to this address and nothing in the
 * source admits to it": the next write raises an exception, and the
 * extender's crash dump names the *writing* instruction, which the map
 * turns into a function. Without it the only evidence is the damage,
 * which is found arbitrarily later and says nothing about its author.
 *
 * One watchpoint at a time, which is all four debug registers are worth
 * worrying about here: a second call replaces the first. Returns 0 if the
 * host has no watchpoints to give -- some do not, and it is not an error
 * worth stopping for, just a diagnostic that will stay quiet.
 *
 * Diagnostic only; nothing calls it unless `scriptcheck` is on. */
int  mr_dpmi_watch_write(const void *addr);
void mr_dpmi_unwatch(void);

#endif /* MR_DOSMEM_H */
