/* vfs.c - the MRPK archive reader and the loose-file fallback.
 *
 * The archive directory is small enough (264 bytes an entry, so 78KB for this
 * game) that it is read once at startup and kept in memory. Lookups are
 * then a binary search with no disk access, and opening a file costs nothing
 * but recording an offset. Reads share a single handle and seek before each
 * transfer, which is safe because the engine is single threaded.
 *
 * File mapping is deliberately not used: Win32s's support for it is
 * unreliable, and this is exactly the sort of thing that would work under
 * Wine and fail on real hardware.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "vfs.h"

/* Only for the loose-file enumeration below, which is the one thing here
 * that stdio cannot do. Everything else in this file is portable, which is
 * why file access does not go through plat.h. */
#ifdef _WIN32
#include <windows.h>
#endif
#if defined(__DOS__) || defined(MR_TARGET_DOS)
#include <dos.h>        /* _dos_findfirst, for the loose-file branch */
#elif !defined(_WIN32)
#include <dirent.h>     /* miwast: the POSIX loose-file branch */
#endif

#define MRPK_MAGIC   0x4B50524DUL   /* 'MRPK' little-endian */

/* v1 was a 64-byte name field, which allowed 63 characters for the whole
 * relative path and was tighter than it looked: the longest name this game
 * ships is 50. v2 is MR_PATH_MAX. The version is checked, so a v1 archive is
 * refused with a log line rather than read as garbage -- the directory is one
 * fread into a struct array, so the entry size is part of the format. */
#define MRPK_VERSION 2
#define MRPK_NAMELEN MR_PATH_MAX

typedef struct {
    char   name[MRPK_NAMELEN];
    mr_u32 offset;
    mr_u32 size;
} mrpk_entry;

struct mr_file {
    /* Archive-backed: the window into the pak. Loose-backed: fp is its own
     * handle and base is 0. */
    FILE *fp;
    long  base;
    long  size;
    long  pos;
    int   owns_fp;
};

static FILE       *g_pak;
static mrpk_entry *g_dir;
static int         g_ndir;
static char        g_loose[192];
static int         g_have_loose;

/* Case-insensitive byte compare, matching the lowercasing mkpak.py applies. */
static int name_cmp(const char *a, const char *b)
{
    unsigned char ca, cb;

    for (;;) {
        ca = (unsigned char)*a++;
        cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
        if (ca == '\\') ca = '/';
        if (cb == '\\') cb = '/';
        if (ca != cb)
            return (int)ca - (int)cb;
        if (!ca)
            return 0;
    }
}

int mr_vfs_cat(char *out, int outsz, const char *a, const char *b)
{
    int n = 0;

    if (!out || outsz <= 0)
        return 0;
    if (!a) a = "";
    if (!b) b = "";

    while (*a) {
        if (n >= outsz - 1) { out[n] = '\0'; return 0; }
        out[n++] = *a++;
    }
    while (*b) {
        if (n >= outsz - 1) { out[n] = '\0'; return 0; }
        out[n++] = *b++;
    }
    out[n] = '\0';
    return 1;
}

int mr_vfs_join(char *out, int outsz, const char *dir, const char *name)
{
    size_t dl;

    if (!dir)
        dir = "";
    dl = strlen(dir);

    /* A separator only where there is something to separate and the caller
     * has not already supplied one. */
    if (dl > 0 && dir[dl - 1] != '/' && dir[dl - 1] != '\\') {
        char tmp[MR_FSPATH_MAX];
        if (!mr_vfs_cat(tmp, (int)sizeof(tmp), dir, "/"))
            return 0;
        return mr_vfs_cat(out, outsz, tmp, name);
    }
    return mr_vfs_cat(out, outsz, dir, name);
}

static int load_pak(const char *path)
{
    mr_u32 hdr[4];
    long need;

    g_pak = fopen(path, "rb");
    if (!g_pak)
        return 0;

    if (fread(hdr, sizeof(mr_u32), 4, g_pak) != 4) {
        mr_log("vfs: %s is too short to be an archive", path);
        fclose(g_pak);
        g_pak = NULL;
        return 0;
    }

    if (hdr[0] != MRPK_MAGIC || hdr[1] != MRPK_VERSION) {
        mr_log("vfs: %s is not an MRPK v%d archive (%08lX v%lu)",
               path, MRPK_VERSION, (unsigned long)hdr[0],
               (unsigned long)hdr[1]);
        fclose(g_pak);
        g_pak = NULL;
        return 0;
    }

    g_ndir = (int)hdr[2];
    if (g_ndir <= 0 || g_ndir > 65536) {
        mr_log("vfs: %s claims %d entries", path, g_ndir);
        fclose(g_pak);
        g_pak = NULL;
        return 0;
    }

    need = (long)g_ndir * (long)sizeof(mrpk_entry);
    g_dir = (mrpk_entry *)malloc((size_t)need);
    if (!g_dir) {
        mr_log("vfs: out of memory for a %ld byte directory", need);
        fclose(g_pak);
        g_pak = NULL;
        return 0;
    }

    if (fseek(g_pak, (long)hdr[3], SEEK_SET) != 0 ||
        fread(g_dir, sizeof(mrpk_entry), (size_t)g_ndir, g_pak)
            != (size_t)g_ndir) {
        mr_log("vfs: %s has a truncated directory", path);
        free(g_dir);
        g_dir = NULL;
        fclose(g_pak);
        g_pak = NULL;
        return 0;
    }

    mr_log("vfs: mounted %s, %d entries", path, g_ndir);
    return 1;
}

int mr_vfs_init(const char *pak_path, const char *loose_dir)
{
    g_pak = NULL;
    g_dir = NULL;
    g_ndir = 0;
    g_have_loose = 0;

    if (pak_path && load_pak(pak_path))
        return 1;

    if (loose_dir && *loose_dir) {
        strncpy(g_loose, loose_dir, sizeof(g_loose) - 1);
        g_loose[sizeof(g_loose) - 1] = '\0';
        g_have_loose = 1;
        mr_log("vfs: no archive, reading loose files from '%s'", g_loose);
        return 1;
    }

    mr_log("vfs: nothing to mount");
    return 0;
}

void mr_vfs_shutdown(void)
{
    if (g_pak)
        fclose(g_pak);
    free(g_dir);
    g_pak = NULL;
    g_dir = NULL;
    g_ndir = 0;
    g_have_loose = 0;
}

int mr_vfs_using_pak(void)
{
    return g_pak != NULL;
}

static const mrpk_entry *find_entry(const char *name)
{
    int lo = 0, hi = g_ndir - 1;

    if (!g_dir)
        return NULL;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = name_cmp(g_dir[mid].name, name);
        if (c == 0)
            return &g_dir[mid];
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

mr_file *mr_vfs_open(const char *name)
{
    mr_file *f;
    const mrpk_entry *e;

    if (!name || !*name)
        return NULL;

    f = (mr_file *)calloc(1, sizeof(mr_file));
    if (!f)
        return NULL;

    e = find_entry(name);
    if (e) {
        f->fp = g_pak;
        f->base = (long)e->offset;
        f->size = (long)e->size;
        f->pos = 0;
        f->owns_fp = 0;
        return f;
    }

    if (g_have_loose) {
        char path[MR_FSPATH_MAX];

        /* Bounded: g_loose is up to 191 bytes and `name` comes from the
         * script, so the old sprintf into 320 bytes was one long image name
         * away from a stack overrun. A truncated path simply fails to open. */
        if (!mr_vfs_join(path, (int)sizeof(path), g_loose, name))
            mr_log("vfs: path too long, not opening '%s'", name);
        f->fp = fopen(path, "rb");
        if (f->fp) {
            fseek(f->fp, 0, SEEK_END);
            f->size = ftell(f->fp);
            fseek(f->fp, 0, SEEK_SET);
            f->base = 0;
            f->pos = 0;
            f->owns_fp = 1;
            return f;
        }
    }

    free(f);
    return NULL;
}

void mr_vfs_close(mr_file *f)
{
    if (!f)
        return;
    if (f->owns_fp && f->fp)
        fclose(f->fp);
    free(f);
}

long mr_vfs_read(mr_file *f, void *buf, long bytes)
{
    long avail, got;

    if (!f || !f->fp || bytes <= 0)
        return 0;

    avail = f->size - f->pos;
    if (bytes > avail)
        bytes = avail;
    if (bytes <= 0)
        return 0;

    /* Archive entries share one handle, so the position has to be
     * re-established before every read. */
    if (fseek(f->fp, f->base + f->pos, SEEK_SET) != 0)
        return 0;

    got = (long)fread(buf, 1, (size_t)bytes, f->fp);
    if (got > 0)
        f->pos += got;
    return got;
}

int mr_vfs_seek(mr_file *f, long offset, int whence)
{
    long np;

    if (!f)
        return -1;

    if (whence == SEEK_SET)      np = offset;
    else if (whence == SEEK_CUR) np = f->pos + offset;
    else if (whence == SEEK_END) np = f->size + offset;
    else                         return -1;

    if (np < 0 || np > f->size)
        return -1;
    f->pos = np;
    return 0;
}

long mr_vfs_tell(mr_file *f) { return f ? f->pos : -1; }
long mr_vfs_size(mr_file *f) { return f ? f->size : -1; }

int mr_vfs_exists(const char *name)
{
    mr_file *f;

    if (find_entry(name))
        return 1;

    f = mr_vfs_open(name);
    if (f) {
        mr_vfs_close(f);
        return 1;
    }
    return 0;
}

char *mr_vfs_read_all(const char *name, long *size_out)
{
    mr_file *f;
    char *buf;
    long n;

    f = mr_vfs_open(name);
    if (!f)
        return NULL;

    n = f->size;
    buf = (char *)malloc((size_t)n + 2);
    if (!buf) {
        mr_log("vfs: out of memory reading %s (%ld bytes)", name, n);
        mr_vfs_close(f);
        return NULL;
    }

    n = mr_vfs_read(f, buf, n);
    buf[n] = '\0';
    mr_vfs_close(f);

    if (size_out)
        *size_out = n;
    return buf;
}

static int has_prefix(const char *s, const char *p)
{
    while (*p) {
        unsigned char a = (unsigned char)*s++;
        unsigned char b = (unsigned char)*p++;
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + 32);
        if (a != b)
            return 0;
    }
    return 1;
}

static int has_suffix(const char *s, const char *suf)
{
    size_t ls = strlen(s), lf = strlen(suf);
    if (lf > ls)
        return 0;
    return name_cmp(s + ls - lf, suf) == 0;
}

int mr_vfs_enum(const char *prefix, const char *suffix, int idx,
                char *out, int outsz)
{
    int i, n = 0;

    if (g_dir) {
        for (i = 0; i < g_ndir; i++) {
            if (prefix && !has_prefix(g_dir[i].name, prefix))
                continue;
            if (suffix && !has_suffix(g_dir[i].name, suffix))
                continue;
            if (n == idx) {
                strncpy(out, g_dir[i].name, (size_t)outsz - 1);
                out[outsz - 1] = '\0';
                return 1;
            }
            n++;
        }
        return 0;
    }

#ifdef _WIN32
    /* Loose files on a host where long names work. Enumerated fresh each
     * call, which is slow, but this path only runs during development. */
    if (g_have_loose && prefix) {
        WIN32_FIND_DATA fd;
        HANDLE h;
        char pattern[MR_FSPATH_MAX];

        /* g_loose + '/' + prefix, exactly as the old sprintf built it -- the
         * prefix goes in verbatim, because it is a name fragment and not
         * necessarily a directory. */
        if (!mr_vfs_join(pattern, (int)sizeof(pattern), g_loose, prefix))
            return 0;
        strncat(pattern, "*", sizeof(pattern) - strlen(pattern) - 1);
        if (suffix)
            strncat(pattern, suffix, sizeof(pattern) - strlen(pattern) - 1);
        h = FindFirstFile(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE)
            return 0;

        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            if (n == idx) {
                /* Verbatim, no separator: the pak branch above matches a
                 * prefix rather than a directory, so "images/bg_" is a legal
                 * prefix and the two branches have to agree. Bounded -- this
                 * was an sprintf that truncated *after* writing, and
                 * cFileName is up to 260 bytes. */
                int fit = mr_vfs_cat(out, outsz, prefix, fd.cFileName);
                FindClose(h);
                if (!fit) {
                    mr_log("vfs: enum name too long for a %d byte buffer: %s%s",
                           outsz, prefix, fd.cFileName);
                    return 0;
                }
                return 1;
            }
            n++;
        } while (FindNextFile(h, &fd));

        FindClose(h);
    }
#elif defined(__DOS__) || defined(MR_TARGET_DOS)
    /* The same thing through int 21h AH=4Eh/4Fh. Loose files are a
     * development convenience on DOS as much as on Windows -- what ships is
     * the pak -- but a build that cannot run out of a directory cannot be
     * debugged a line at a time, which is most of what this path is for.
     *
     * Two things are different here and neither is worth working around.
     * Names come back 8.3 and uppercase, which the engine does not mind:
     * every comparison it makes on a name is case-insensitive, and so is
     * DOS when the name is handed back to fopen. And DOS's wildcard is not
     * the shell's -- `bg_*.png` means "starts bg_, extension png", which is
     * exactly the prefix-and-suffix question being asked, but only because
     * the suffix is always an extension. A suffix that was not would match
     * nothing rather than too much.
     */
    if (g_have_loose && prefix) {
        struct find_t f;
        char pattern[MR_FSPATH_MAX];

        if (!mr_vfs_join(pattern, (int)sizeof(pattern), g_loose, prefix))
            return 0;
        strncat(pattern, "*", sizeof(pattern) - strlen(pattern) - 1);
        if (suffix)
            strncat(pattern, suffix, sizeof(pattern) - strlen(pattern) - 1);

        if (_dos_findfirst(pattern, _A_NORMAL | _A_RDONLY | _A_ARCH, &f) != 0)
            return 0;

        do {
            if (f.attrib & _A_SUBDIR)
                continue;
            if (n == idx) {
                /* Verbatim, no separator, exactly as the other two branches
                 * do it: "images/bg_" is a legal prefix and all three have
                 * to agree about what a match is called. */
                int fit = mr_vfs_cat(out, outsz, prefix, f.name);
                _dos_findclose(&f);
                if (!fit) {
                    mr_log("vfs: enum name too long for a %d byte buffer: %s%s",
                           outsz, prefix, f.name);
                    return 0;
                }
                return 1;
            }
            n++;
        } while (_dos_findnext(&f) == 0);

        _dos_findclose(&f);
    }
#else
    /* miwast: the one addition to this copy. The host compiler reads the
     * translations as loose files on a POSIX host, which the engine never
     * needed. `prefix` is a directory followed by a name fragment. */
    if (g_have_loose && prefix) {
        char dir[MR_FSPATH_MAX];
        const char *slash = strrchr(prefix, '/');
        const char *frag = slash ? slash + 1 : prefix;
        int dl = slash ? (int)(slash - prefix) : 0;
        struct dirent *e;
        DIR *d;

        if (strlen(g_loose) + 1 + (size_t)dl + 1 > sizeof(dir))
            return 0;
        sprintf(dir, "%s/%.*s", g_loose, dl, prefix);
        d = opendir(dir);
        if (!d)
            return 0;
        while ((e = readdir(d)) != NULL) {
            if (e->d_type != DT_REG && e->d_type != DT_UNKNOWN)
                continue;
            if (!has_prefix(e->d_name, frag))
                continue;
            if (suffix && !has_suffix(e->d_name, suffix))
                continue;
            if (n == idx) {
                int fit = mr_vfs_cat(out, outsz, prefix, e->d_name + strlen(frag));
                closedir(d);
                return fit;
            }
            n++;
        }
        closedir(d);
    }
#endif
    return 0;
}
