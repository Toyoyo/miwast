#ifndef MR_PRESENT_H
#define MR_PRESENT_H

#include "mr.h"
#include "yuv.h"        /* mr_yuv_frame, for mr_present_video */

/* Presentation: scale the 1280x720 stage into whatever the platform gives us
 * to draw on, and put it there.
 *
 * One implementation per target, chosen by the makefile:
 *
 *     present.c     Win32s, through GDI into a window client area
 *
 * The interface names no platform type. It used to take an HDC on every
 * call, which meant windows.h in this header, which meant windows.h
 * everywhere -- present.h is included by main.c, and mr.h pulled it in for
 * the rest. The device context is now acquired and released inside the
 * implementation, where it belongs: the caller is handing over a stage, not
 * a place to put it.
 *
 * The 1280x720 stage is fixed and is *not* the output size. Both are the
 * presenter's problem: it fits the largest 16:9 rectangle into the output,
 * letterboxes the rest, and scales. So an output of 640x480, 800x600 or a
 * window the player dragged to an odd size all work without anything above
 * this line knowing.
 */

/* `oswindow` is whatever the platform's output is identified by -- an HWND on
 * Win32 -- or NULL for a platform that owns the whole screen and has nothing
 * to name. Must be called after the platform has its output up. */
int  mr_present_init(void *oswindow);
void mr_present_shutdown(void);

/* The output size changed: a window resize, or a mode switch. Recomputes the
 * letterboxed destination rect and resizes the intermediate buffer. */
void mr_present_resize(int out_w, int out_h);

/* Scale-convert the stage and put it on screen. */
void mr_present(const mr_surface *stage);

/* The same for one band of the stage, given in stage coordinates. Only the
 * destination rows that band covers are scaled, packed and blitted, which for
 * the dialogue box is a quarter of the work. The rest of the output keeps
 * what was last presented, so the caller must have left the rest of the stage
 * alone too. */
void mr_present_band(const mr_surface *stage, int y, int h);

/* --- video --------------------------------------------------------------
 *
 * Put one decoded video frame on screen. **The stage is not involved**: the
 * frame is colour-converted and scaled straight into the same picture buffer
 * mr_present scales the stage into, and then packed and blitted by the same
 * code. Everything below the buffer is shared; everything above it is
 * skipped.
 *
 * That is the whole reason a cutscene runs at a watchable rate. The stage is
 * 1280x720 whatever the window is, and compositing it costs about a third of
 * a second a frame on the target -- three frames a second against the
 * twenty-five a movie wants. Converting 640x320 of YUV directly into a
 * 640x360 window's picture rect is a fifth of the pixels and one pass instead
 * of two; see yuv.h for the measurements.
 *
 * The presenter owns the fit and the scaler, not the caller. It is the only
 * thing that knows how big its picture buffer is, and the video's letterbox
 * -- the "optional black border given the aspect ratio" -- is computed from
 * that with mr_yuv_fit. So a 2:1 movie in a 16:9 game area in a 4:3 screen
 * mode is letterboxed twice, by two different pieces of code, and neither
 * needs to know about the other.
 *
 * The bars belonging to the *video* are inside the picture rect, so they are
 * painted here when the rect changes rather than by paint_borders, which owns
 * the ones outside it.
 *
 * Does nothing if the frame is empty or the presenter is not up. Safe to call
 * every frame with the same frame size; the scaler is rebuilt only when the
 * size or the output changes.
 */
void mr_present_video(const mr_yuv_frame *frame);

/* MR_PROF: presenting is three distinct costs -- scaling the stage down,
 * packing it into the output's pixel format, and the call that hands it to
 * the display, which on Win32s goes through a 16-bit thunk and cannot be
 * measured on a host. Enabling this makes mr_present time each of them; the
 * getter returns the totals in milliseconds and clears them.
 *
 * The third figure is still called `gdi` at the call site in main.c's
 * profile line. On another platform it is whatever the equivalent handover
 * costs -- a write across the bus to a linear framebuffer, say. */
void mr_present_profile_enable(int on);
void mr_present_profile(long *scale_ms, long *pack_ms, long *blit_ms);

/* Bits per pixel the output reports. */
int mr_present_depth(void);

/* Mapping an output point onto the stage is mr_fit_map_point in scale.h, not
 * a presenter call: the letterbox arithmetic is the same whatever the output
 * is, and input.c needs it without linking a presenter. mr_present_resize is
 * what keeps the fit up to date. */

#endif /* MR_PRESENT_H */
