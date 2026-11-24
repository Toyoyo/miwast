/* presdos.h - the part of the DOS presenter that only the DOS platform
 * calls. The counterpart of presw32.h, which exists for the same reason: one
 * thing passes between platdos.c and presdos.c that present.h has no business
 * naming.
 *
 * Here it is the mouse pointer. Windows draws its own and nothing in the
 * engine has ever had to; under DOS the presenter has to put it on the screen
 * and take it off again, because it is the only thing that knows where the
 * picture is, what format the framebuffer wants and what is underneath.
 *
 * It lives on the presenter's side rather than the platform's for a practical
 * reason as well: the clean, unscaled-back pixels under the pointer are in
 * the presenter's own buffer. Reading them back off the card instead would be
 * correct and ruinously slow.
 */

#ifndef MR_PRESDOS_H
#define MR_PRESDOS_H

/* The pointer is at this output pixel. Erases it from where it was and draws
 * it where it is now, immediately -- not at the next frame.
 *
 * Immediately matters: the frame loop skips drawing when nothing has changed,
 * which is most frames in a visual novel. A pointer that moved only when the
 * engine redrew would stick to the screen while the player waved the mouse at
 * a motionless dialogue box.
 */
void mr_presdos_cursor_move(int ox, int oy);

/* Stop drawing it, and take it off the screen. For the way out, and for
 * anything that is about to repaint the whole mode from underneath. */
void mr_presdos_cursor_hide(void);

#endif /* MR_PRESDOS_H */
