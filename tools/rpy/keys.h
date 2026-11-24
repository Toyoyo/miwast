/* keys.h - key codes the engine core understands.
 *
 * The core used to speak Windows virtual keys: rhythm.c switched on VK_LEFT,
 * main.c's keysym table on VK_PRIOR, and both got the constants from
 * windows.h by way of mr.h. That is a platform leak into code that has no
 * other reason to know what platform it is on, and it is the one thing
 * standing between the input logic and a harness.
 *
 * The values here are deliberately *identical* to the Win32 VK_ codes they
 * replace. Two reasons:
 *
 *   - the Win32 translation stays a cast rather than a lookup table, so this
 *     change cannot alter what the shipped build does;
 *   - letters and digits are their own ASCII codes in both schemes, which is
 *     what lets `key == 'R'` keep working unchanged.
 *
 * platw32.c asserts the equality at compile time, so the two cannot drift.
 * A platform whose own codes differ -- DOS scancodes, say -- maps into these
 * in its input translation and the core never sees the difference.
 *
 * Only the keys the engine actually binds are listed. Adding one means adding
 * it here, mapping it in every platform's translation, and extending the
 * compile-time check.
 */

#ifndef MR_KEYS_H
#define MR_KEYS_H

#define MR_KEY_BACKSPACE 0x08
#define MR_KEY_TAB       0x09
#define MR_KEY_RETURN    0x0D
#define MR_KEY_SHIFT     0x10
#define MR_KEY_CTRL      0x11
#define MR_KEY_ALT       0x12
#define MR_KEY_ESCAPE    0x1B
#define MR_KEY_SPACE     0x20
#define MR_KEY_PAGEUP    0x21
#define MR_KEY_PAGEDOWN  0x22
#define MR_KEY_END       0x23
#define MR_KEY_HOME      0x24
#define MR_KEY_LEFT      0x25
#define MR_KEY_UP        0x26
#define MR_KEY_RIGHT     0x27
#define MR_KEY_DOWN      0x28
#define MR_KEY_DELETE    0x2E

/* '0'..'9' are 0x30..0x39 and 'A'..'Z' are 0x41..0x5A -- their ASCII codes,
 * which is also what Win32 uses. Letters arrive uppercase whatever the shift
 * state, again as Win32 delivers them. */

#define MR_KEY_NUMPAD0   0x60
#define MR_KEY_NUMPAD1   0x61
#define MR_KEY_NUMPAD2   0x62
#define MR_KEY_NUMPAD3   0x63
#define MR_KEY_NUMPAD4   0x64
#define MR_KEY_NUMPAD5   0x65
#define MR_KEY_NUMPAD6   0x66
#define MR_KEY_NUMPAD7   0x67
#define MR_KEY_NUMPAD8   0x68
#define MR_KEY_NUMPAD9   0x69

#define MR_KEY_F1        0x70
#define MR_KEY_F11       0x7A

#endif /* MR_KEYS_H */
