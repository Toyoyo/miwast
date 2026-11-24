/* cursor.h - the mouse pointer, for a platform that has to draw its own.
 *
 * Windows hands its applications an arrow; DOS hands them nothing. The
 * pixels below are generated at build time from a PNG by tools/mkcursor.py
 * into src/cursordat.c, so the art is a picture in a file rather than a
 * table in a source file, and a game with a reason to have its own pointer
 * changes CURSOR_PNG in Makefile.dos and nothing else.
 *
 * Straight-alpha 0xAARRGGBB, which is mr_surface's own format, so the
 * engine's blitter composites it directly. Row 0 is the top row, as it is in
 * the PNG; presdos.c flips it once at startup, because its scaled buffer is
 * bottom-up.
 *
 * The hotspot is where the pointer actually points -- the tip of the arrow,
 * not the corner of the box it is drawn in. It is baked in beside the pixels
 * rather than guessed at: a pointer that clicks four pixels away from where
 * it looks is maddening and nearly invisible as a bug.
 */

#ifndef MR_CURSOR_H
#define MR_CURSOR_H

#include "mr.h"

extern const int    mr_cursor_w;
extern const int    mr_cursor_h;
extern const int    mr_cursor_hotx;
extern const int    mr_cursor_hoty;
extern const mr_u32 mr_cursor_px[];

#endif /* MR_CURSOR_H */
