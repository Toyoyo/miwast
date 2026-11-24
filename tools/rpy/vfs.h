/* vfs.h - file access through the MRPK archive, or loose files.
 *
 * The target filesystem is FAT with 8.3 names, so the game's real filenames
 * cannot exist on disk. Everything is read out of a single archive --
 * MRPY.PAK by default, `MR_PAK_NAME` in main.c, and mrpy.ini can name
 * another -- whose directory holds the full long names.
 *
 * A loose-file backend is kept for development on a host where long names
 * work: if no archive is present, names resolve against a directory instead.
 * Both backends present the same interface so nothing above this layer has to
 * know which is in use.
 */

#ifndef MR_VFS_H
#define MR_VFS_H

#include "mr.h"

typedef struct mr_file mr_file;

/* Mount. `pak_path` may be NULL or absent, in which case `loose_dir` is used.
 * Returns 0 if neither is usable. */
int  mr_vfs_init(const char *pak_path, const char *loose_dir);
void mr_vfs_shutdown(void);

/* 1 if the archive (rather than loose files) is backing the VFS. */
int mr_vfs_using_pak(void);

/* Names are always '/' separated and matched case-insensitively. */
mr_file *mr_vfs_open(const char *name);
void     mr_vfs_close(mr_file *f);

long mr_vfs_read(mr_file *f, void *buf, long bytes);
int  mr_vfs_seek(mr_file *f, long offset, int whence);   /* SEEK_* */
long mr_vfs_tell(mr_file *f);
long mr_vfs_size(mr_file *f);

int  mr_vfs_exists(const char *name);

/* Read a whole file into a malloc'd buffer with a NUL appended, which is what
 * the script lexer wants. Returns NULL on failure; caller frees. */
char *mr_vfs_read_all(const char *name, long *size_out);

/* Enumerate entries whose name starts with `prefix` and ends with `suffix`
 * (either may be NULL). `idx` counts from 0; returns 0 when exhausted. */
int mr_vfs_enum(const char *prefix, const char *suffix, int idx,
                char *out, int outsz);

/* Bounded string building, replacing two sprintfs that trusted their inputs
 * -- see "Two sprintfs that trusted their inputs" in NOTES. Both always NUL
 * terminate and both return 1 if the whole thing fit, 0 if it was truncated.
 *
 * mr_vfs_cat concatenates verbatim. mr_vfs_join inserts a '/' between the two
 * only when `dir` is non-empty and does not already end in a separator.
 *
 * The distinction matters: mr_vfs_enum matches a *prefix*, not a directory,
 * so "images/bg_" is a legal argument and its output must not grow a
 * separator -- the archive branch and the loose branch have to produce the
 * same name for the same prefix.
 *
 * Exposed rather than static because the overflow they fix lives in
 * `#ifdef _WIN32` code that a Linux harness never compiles. enumtest drives
 * these directly, which is the only way to show the clamp working.
 *
 * Truncated output is still returned rather than blanked: a caller that
 * ignores the return value then gets a wrong filename and a failed open,
 * which is diagnosable, instead of a buffer overrun. */
int mr_vfs_cat(char *out, int outsz, const char *a, const char *b);
int mr_vfs_join(char *out, int outsz, const char *dir, const char *name);

#endif /* MR_VFS_H */
