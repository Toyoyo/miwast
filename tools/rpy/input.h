/* input.h - what a keypress or a click *means*.
 *
 * This is the body of what used to be main.c's wnd_proc, with the message
 * decoding left behind on the platform side. The decoding was never the
 * interesting part: three hundred lines of "a click on a lane is a tap on
 * that lane's note, checked before the focus list", "Ctrl skips while held,
 * Tab toggles", "a screen's own `key` binding is resolved before the default
 * behaviour" -- none of which has an opinion about WM_LBUTTONDOWN.
 *
 * Splitting it out buys two things. A second platform gets all of it for
 * free, which is the reason it happened. And it is now reachable from a
 * harness, which main.c never was -- `main.c is linked by no harness` has
 * been the standing caveat on every input-ordering fix in NOTES, and most of
 * what it covered is in here now.
 *
 * Coordinates coming in are *output* coordinates -- pixels in the window
 * client area, or on the screen under DOS. Mapping them onto the 1280x720
 * stage, and rejecting the ones that land in the letterbox, is done here
 * through mr_present_map_point rather than by each caller.
 *
 * Keys coming in are MR_KEY_* from keys.h, not the platform's own codes.
 */

#ifndef MR_INPUT_H
#define MR_INPUT_H

#define MR_MB_LEFT  1
#define MR_MB_RIGHT 2

/* The pointer moved to an output pixel. A point in the letterbox clears the
 * hover unless a drag is in progress, which keeps its grab. */
void mr_input_mouse_move(int ox, int oy);

/* A button went down at an output pixel. Left dispatches through the
 * minigame, the choice menu, the focus list and finally the story; right
 * toggles the game menu, as Ren'Py's does. */
void mr_input_mouse_down(int button, int ox, int oy);

/* A button came up. Only the left one matters, and only to end a drag. */
void mr_input_mouse_up(int button);

/* `notches` is positive away from the player. Over a scrollable region this
 * scrolls it; elsewhere it is rollback and rollforward, which is Ren'Py's
 * mousedown_4 / mousedown_5. There is no wheel on Windows 3.1, so this only
 * ever fires on a host. */
void mr_input_wheel(int notches, int ox, int oy);

/* A key went down, or came up. Auto-repeat arrives as repeated downs with no
 * up between them, which is deliberate -- holding Page Up is how a player
 * rolls back a long way. */
void mr_input_key_down(int key);
void mr_input_key_up(int key);

/* Flip `_preferences.display`. Bound to F11 and bare 'f' inside
 * mr_input_key_down; exposed because Alt+Enter is the third binding and does
 * not arrive as an ordinary key down on every platform -- on Win32 it is a
 * system key with its own message. */
void mr_input_fullscreen_toggle(void);

/* The engine lost the keyboard. Without this, losing focus with Ctrl held
 * leaves the skip held forever: no key-up is coming, because the key goes up
 * somewhere else. A platform that cannot lose the keyboard never calls it. */
void mr_input_focus_lost(void);

#endif /* MR_INPUT_H */
