/* dosisr.h -- running a protected-mode interrupt handler on a stack of our
 * own, because SS is not DS and the compiler assumes it is.
 *
 * Watcom's flat model (-mf) is entitled to assume SS and DS address the same
 * bytes: that is what "flat" means, and under DOS/32A it is true at every
 * instant, including inside an interrupt. It is not true under DOS/4GW. A
 * hardware interrupt delivered to a protected-mode handler there arrives on
 * the extender's own stack, and that stack is a separate selector:
 *
 *     DS= 26F, page granular, limit FFFFFFFF, base 00000000
 *     SS= 187, byte granular, limit 00007CFF, base 001D1B90
 *
 * Nothing in the handler's own code notices, because `__interrupt` begins
 * with `mov ebp,esp` and every global it names is addressed absolutely
 * through DS. The trap is one call further down. In a *callee*, EBP is an
 * ordinary register, and the compiler will cheerfully pick it to index an
 * array -- at which point the ModRM byte says "base EBP", whose default
 * segment is SS, and a perfectly ordinary global store goes looking for a
 * flat address inside a 32KB stack:
 *
 *     0771  C6 85 ...  mov byte ptr _g_busy[ebp],0x00     ; g_busy[idx] = 0
 *
 * That is exception 0Ch, a stack fault, at wavedos.c's fill_half, and it is
 * the whole reason this header exists. It cannot be fixed by writing the C
 * differently; any global indexed by any variable may compile to it, in any
 * function an interrupt handler reaches, and the choice of register is a
 * register allocator's and not ours.
 *
 * So fix the premise instead. Switch to a stack whose selector *is* DS before
 * running any of the handler's work, and the compiler's assumption is true
 * again for everything below. On DOS/32A the switch is a no-op in effect --
 * SS already equals DS -- and costs four instructions per interrupt.
 *
 * Why a pragma and not an `_asm` block: an `_asm` block lives inside a
 * function the compiler has already given a prologue, and after the switch
 * any compiler-generated EBP access in that function would read the *new*
 * stack. An inline pragma with a declared register contract has no body for
 * the compiler to put anything in.
 *
 * The saved SS:ESP go on the new stack rather than into registers, because
 * the C function we call is only required to preserve ESI, EDI and EBP --
 * EAX, EBX, ECX and EDX are the argument registers and are fair game.
 *
 * The `mov esp` must be the instruction immediately after the `mov ss`, both
 * ways round. Loading SS inhibits interrupts for exactly one instruction, and
 * that window is the only thing standing between us and an interrupt arriving
 * on a half-switched stack.
 *
 * Not every handler wants this:
 *
 *   - platdos.c's tick_isr cannot have it. It ends in `_chain_intr`, which
 *     unwinds the interrupt frame and jumps; from inside a nested call there
 *     is no frame to unwind.
 *   - platdos.c's kbd_isr does not need it. It calls nothing, and `wdis
 *     src/platdos.do` shows its one indexed store as `mov _g_kq[ecx],al` --
 *     DS-relative. A leaf handler with no callee has no register allocator
 *     to surprise it.
 *
 * Both are judgements about today's generated code, so if either grows a call
 * to anything, it needs this.
 */
#ifndef MR_DOSISR_H
#define MR_DOSISR_H

/* Big enough for the audio handler's path -- fill_half, a conversion, a
 * memcpy of one buffer -- with the margin that an interrupt stack wants,
 * since overflowing one is a fault with no message. It lives in BSS, so
 * mr_dpmi_lock_image has already locked it against a paging host.
 */
#define MR_DOS_ISR_STACK 16384

/* Call fn on the stack ending at stack_top, then put SS:ESP back.
 *
 * stack_top is the address one past the last usable byte, rounded down to a
 * multiple of 16 -- MR_DOS_ISR_TOP does that. The caller must be a function
 * whose body is this call and nothing else; see the note on `_asm` above.
 */
void mr_dos_isr_call(void (*fn)(void), void *stack_top);
#pragma aux mr_dos_isr_call =   \
    "mov  ecx, esp"             \
    "mov  bx, ss"               \
    "mov  si, ds"               \
    "mov  ss, si"               \
    "mov  esp, edx"             \
    "push ebx"                  \
    "push ecx"                  \
    "call eax"                  \
    "pop  ecx"                  \
    "pop  ebx"                  \
    "mov  ss, bx"               \
    "mov  esp, ecx"             \
    parm [eax] [edx]            \
    modify [eax ebx ecx edx esi];

/* The top of a stack declared as `static unsigned char name[MR_DOS_ISR_STACK]`.
 * Keeping ESP 16-byte aligned is not required by anything here, but an
 * unaligned interrupt stack is the kind of thing that is only ever noticed
 * once something starts using SSE. */
#define MR_DOS_ISR_TOP(arr) \
    ((void *)(((unsigned long)(arr) + sizeof(arr)) & ~15UL))

#endif
