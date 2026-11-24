/* plat.h - the seam between the engine and the machine it runs on.
 *
 * Everything in src/ is portable C89 except for four things: getting the
 * stage onto a screen, reading the keyboard and mouse, knowing what time it
 * is, and telling the player the game has died. Those four are what this
 * header names.
 *
 * There is one implementation per target:
 *
 *     platw32.c + present.c     Win32s on Windows 3.1x, through GDI
 *
 * and a DOS one to come, which is the reason the seam exists at all. The
 * division of labour is:
 *
 *     the platform owns      the process entry point, the event source, the
 *                            clock, and the surface the stage is blitted to
 *
 *     the core owns          the frame loop and its pacing, everything a
 *                            frame does, and what an input *means*
 *
 * That split is the useful one. `frame()` is identical on both targets, and
 * so is "Page Up rolls back unless a screen bound it first" -- that logic
 * lives in input.c, which knows nothing about messages or scancodes. What
 * differs between the two is only the dozen functions below.
 *
 * Two things are deliberately *not* here:
 *
 *   - presentation, which has its own header. present.h is written in
 *     platform-neutral terms for the same reason this file exists, and each
 *     platform supplies one implementation of it.
 *
 *   - file access. vfs.c has a single `#ifdef _WIN32` for loose-file
 *     enumeration and the rest is stdio, which DOS has too. Routing that
 *     through here would be ceremony around one FindFirstFile.
 */

#ifndef MR_PLAT_H
#define MR_PLAT_H

/* --- what the core asks of the platform ---------------------------------- */

/* Milliseconds since an arbitrary fixed point, wrapping at 2^32. Only
 * differences are ever taken, and they are taken as signed 32-bit, so the
 * wrap costs nothing. Must have genuine millisecond resolution: transitions
 * are time-based, and the profiler divides by it.
 *
 * On Win32s this is timeGetTime. The DOS build cannot use the 18.2Hz BIOS
 * tick for it. */
unsigned int mr_plat_ticks(void);

/* Hand the rest of this timeslice back, once per idle loop iteration. On a
 * cooperatively multitasked system this is load-bearing: the frame loop is
 * the only thing feeding the mixer, and a game that never yields makes the
 * whole machine unusable. Where there is nothing to yield to it may do
 * nothing, but it must still return promptly -- the caller is in a spin. */
void mr_plat_yield(void);

/* Drain whatever input has arrived and dispatch it through input.h. Returns
 * 0 when the player has asked to close the game, which is distinct from
 * mr_quit(): this is the window's close box, not the script's Quit(). */
int mr_plat_pump(void);

/* Called from inside an operation that is going to take a long time -- a
 * PNG decode is a couple of hundred milliseconds on the target -- so that a
 * platform which has to maintain something itself can do it.
 *
 * On DOS that something is the mouse pointer, which the engine draws and
 * which would otherwise freeze solid for the length of every image load:
 * hover a menu button, the button's art loads, and the pointer stops dead.
 * On Win32 there is nothing to do, because Windows draws the pointer.
 *
 * Deliberately narrow. It may move the pointer and poll a device; it must
 * **not** dispatch anything through input.h, because the caller is in the
 * middle of loading a file and the screen system is not in a state to be
 * asked about anything. Whatever it notices is delivered at the next
 * mr_plat_pump, in the ordinary way.
 *
 * It is called often -- every few scanlines -- so it has to be cheap. */
void mr_plat_service(void);

/* `Preference("display", "fullscreen")`. The core polls the preference once a
 * frame and calls this when it changes; what "fullscreen" means is the
 * platform's business -- a borderless window on Win32s, which has no mode
 * switching worth the name, and a different video mode under DOS. */
void mr_plat_set_fullscreen(int on);

/* Keep sending mouse motion while a bar or scrollbar is being dragged, even
 * once the pointer has wandered off the control -- that is what makes a
 * slider usable. input.c turns this on when a drag starts and off when the
 * button comes up.
 *
 * A platform that owns the whole screen and the whole mouse already behaves
 * this way and implements it as a no-op; it exists because Windows does not,
 * and an unreleased capture there is a wedged desktop. */
void mr_plat_capture_mouse(int on);

/* The 'r' hotkey: put the output back to the size the ini asked for. Only
 * meaningful where the output is a resizable window, so a platform whose
 * output is a video mode does nothing and the key is inert. */
void mr_plat_restore_size(void);

/* The window caption, from config.window_title or config.name. Already
 * converted out of UTF-8 to the local 8-bit encoding by the caller. A
 * platform with nowhere to put it ignores it. */
void mr_plat_set_title(const char *text);

/* Put the message where the player will see it, as the last thing before
 * exit(1). Called from mr_fatal with the window still up; the platform tears
 * down whatever it owns afterwards, as the core has already given back the
 * wave device and the palette by this point.
 *
 * It must not come back expecting to keep running. */
void mr_plat_fatal(const char *msg);

/* Trap the next instruction that writes these four bytes, if the platform
 * can. Diagnostic only, and called only when `scriptcheck` is on: it exists
 * because a DOS build had its statement array overwritten by a block of the
 * engine's own code, with nothing in the source that writes there, and the
 * damage is always found long after the write. A hardware watchpoint turns
 * "somebody wrote here" into the address of the instruction that did.
 *
 * DOS implements it with DPMI 0B00h; Win32 and the harness host do nothing,
 * which is honest -- the bug has only ever appeared on one machine and this
 * is not worth a second implementation until it appears on another. */
void mr_plat_watch_write(const void *addr);

/* --- what the platform asks of the core ---------------------------------- */

/* Bring the game up: VFS, images, audio, fonts, script, interpreter, and the
 * environment-variable diagnostics. `cmdline` is the raw command tail, used
 * only to locate the game directory. Returns 0 if it could not start, having
 * already logged why; a fatal condition calls mr_fatal and does not return.
 *
 * The platform must have created its output surface and called
 * mr_present_init before this: the startup draws nothing, but mr_fatal from
 * inside it wants somewhere to put the message. */
int  mr_main_startup(const char *cmdline);

/* The frame loop, with its own pacing. Returns when the player quits. */
void mr_main_loop(void);

/* Give everything back, in the reverse of the order it was taken. */
void mr_main_shutdown(void);

#endif /* MR_PLAT_H */
