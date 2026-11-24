/* datefmt.h - strftime for a translated calendar.
 *
 * Ren'Py does not use the C library's locale for day and month names: it
 * intercepts %a %A %b %B and takes the names from the translation catalogue,
 * passing everything else to strftime (00action_file.rpy:77, `_strftime`).
 * This is the same thing, for the same reason -- `strftime("%B")` on Win32s
 * gives the C locale's English whatever language the game is in.
 */
#ifndef MR_DATEFMT_H
#define MR_DATEFMT_H

/* Format `epoch` (seconds, local time) into `out` following `fmt`.
 *
 * `fmt` is a strftime pattern and may carry a leading {#tag}, because the
 * format itself is a translatable string -- the screen writes
 * `_("{#file_time}%A %d %B %Y, %H:%M")` and a translation may reorder it.
 * The tag is dropped rather than printed.
 *
 * Returns the number of bytes written, not counting the terminator.
 */
int mr_format_time(char *out, int outsz, const char *fmt, long epoch);

#endif /* MR_DATEFMT_H */
