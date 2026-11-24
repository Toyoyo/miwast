/* config.h - mrpy.ini, the handful of settings that cannot be compiled in.
 *
 * The engine is meant to run other Ren'Py games, and two things have to be
 * known before anything else exists: which archive to open, and how big to
 * make the window. Neither can come from the game data -- the first *is* how
 * the game data is found -- so they come from a plain INI file beside the
 * executable, read with stdio rather than through the VFS.
 *
 * Everything else the engine needs it still takes from the game's own .rpy
 * files, which is where a Ren'Py game says it. This is deliberately not a
 * place to start mirroring gui.rpy.
 */

#ifndef MR_CONFIG_H
#define MR_CONFIG_H

#define MR_CONFIG_FILE "mrpy.ini"

/* Read the file. Missing or unreadable is not an error: every getter then
 * returns its default, which is the behaviour of a build with no ini at all.
 * Safe to call more than once; the last call wins. */
void mr_config_load(const char *path);

/* Values from the [Minirenpy] section. Keys are matched without regard to
 * case. Other sections are parsed and ignored, so a game may keep its own
 * settings in the same file. */
const char *mr_config_str(const char *key, const char *dflt);

/* As above for a whole number. A value that is not one -- or that falls
 * outside [lo, hi] -- is logged and the default used, because a window 0
 * pixels wide is worse than a window that ignored the ini.
 *
 * Decimal, or hexadecimal with an explicit `0x`. Deliberately not C's base
 * 0, which would also read a leading zero as octal and turn `width=0640`
 * into 416 without saying so. The hex form exists because MODES.EXE prints
 * VBE mode numbers in hex, as the standard and every other mode utility do,
 * and `vbemode` could not previously be given the number it printed. */
long mr_config_int(const char *key, long dflt, long lo, long hi);

#endif /* MR_CONFIG_H */
