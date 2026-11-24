/* ansi.h - UTF-8 to the Windows ANSI code page, for the few strings that
 * cross into an -A entry point.
 *
 * Script files are UTF-8 (Ren'Py reads them as UTF-8 unconditionally). Win32s
 * has no usable -W entry points, so everything we hand to USER32 is ANSI, and
 * on a Western Windows 3.1 that is code page 1252. Passing UTF-8 through
 * unconverted is how `Le départ de Kalea` reached the title bar as
 * `Le dÃ©part de Kalea`: CP1252 renders the two bytes C3 A9 as `Ã` and `©`.
 */

#ifndef MR_ANSI_H
#define MR_ANSI_H

/* Convert `in` from UTF-8 to CP1252 into `out`, always NUL-terminating.
 *
 * Returns the number of bytes written, not counting the terminator. Output is
 * never longer than the input, because every CP1252 character is one byte and
 * every non-ASCII UTF-8 character is at least two -- so an out buffer the size
 * of the input always suffices, and `out` may not alias `in`.
 *
 * A character with no CP1252 representation becomes a best-effort ASCII
 * substitute where there is an obvious one (the dashes, the curly quotes that
 * are not already in CP1252's 0x80-0x9F block) and '?' otherwise. Dropping it
 * would silently shorten a title; '?' is visible and says what happened.
 */
int mr_utf8_to_ansi(const char *in, char *out, int outsz);

#endif
