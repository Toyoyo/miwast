/* presw32.h - the part of the GDI presenter that only platw32.c may call.
 *
 * One function. It is not in present.h because it takes an HDC, and keeping
 * every platform type out of present.h is the entire point of that header;
 * and it is not static in present.c because the palette messages arrive in
 * the window procedure, which is in the other file.
 *
 * A DOS presenter has no counterpart: setting the palette on a VGA DAC is
 * not something the event loop has to be told about.
 */

#ifndef MR_PRESW32_H
#define MR_PRESW32_H

#include <windows.h>

/* Select and realize the game's palette into a DC. Only meaningful on a
 * 256-colour display; a no-op otherwise. `background` is what a window
 * without the focus is allowed to ask for. */
void mr_present_realize(HDC hdc, int background);

#endif /* MR_PRESW32_H */
