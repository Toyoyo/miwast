/* pak.c - read files out of MIWA.PAK (format in mwb.h).
 *
 * The pak is opened once and its directory kept in memory; a read is one
 * Fseek and one Fread. Entries are sorted by name, so lookup is a binary
 * search.
 */

#include <stdlib.h>
#include <string.h>
#include <mint/osbind.h>

#include "st.h"

typedef struct pentry {
    char name[14];
    u32  off;
    u32  size;
} pentry;

static short   g_fd = -1;
static pentry *g_dir;
static int     g_n;

static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

int pak_open(const char *path)
{
    u8 hdr[MWB_PAK_HEADER_SIZE];
    u8 *raw;
    long fd;
    int i;

    fd = Fopen(path, 0);
    if (fd < 0)
        return 0;
    g_fd = (short)fd;
    if (Fread(g_fd, MWB_PAK_HEADER_SIZE, hdr) != MWB_PAK_HEADER_SIZE ||
        be32(hdr) != MWB_PAK_MAGIC)
        return 0;
    g_n = (hdr[4] << 8) | hdr[5];

    raw = malloc((u32)g_n * MWB_PAK_ENTRY_SIZE);
    g_dir = malloc((u32)g_n * sizeof(pentry));
    if (!raw || !g_dir)
        return 0;
    if (Fread(g_fd, (u32)g_n * MWB_PAK_ENTRY_SIZE, raw) !=
        (long)g_n * MWB_PAK_ENTRY_SIZE)
        return 0;
    for (i = 0; i < g_n; i++) {
        const u8 *e = raw + (u32)i * MWB_PAK_ENTRY_SIZE;
        memcpy(g_dir[i].name, e, 14);
        g_dir[i].name[13] = '\0';
        g_dir[i].off = be32(e + 14);
        g_dir[i].size = be32(e + 18);
    }
    free(raw);
    trace("pak: %d entries", g_n);
    return 1;
}

void pak_close(void)
{
    if (g_fd >= 0)
        Fclose(g_fd);
    g_fd = -1;
}

static const pentry *find(const char *name)
{
    int lo = 0, hi = g_n - 1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(name, g_dir[mid].name);
        if (c == 0)
            return &g_dir[mid];
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return NULL;
}

s32 pak_size(const char *name)
{
    const pentry *e = find(name);
    return e ? (s32)e->size : -1;
}

s32 pak_max_size(const char *prefix)
{
    s32 m = -1;
    size_t l = strlen(prefix);
    int i;

    for (i = 0; i < g_n; i++)
        if (!strncmp(g_dir[i].name, prefix, l) && (s32)g_dir[i].size > m)
            m = (s32)g_dir[i].size;
    return m;
}

int pak_read(const char *name, void *buf, u32 max)
{
    const pentry *e = find(name);

    if (!e || e->size > max)
        return 0;
    if (Fseek(e->off, g_fd, 0) != (long)e->off)
        return 0;
    return Fread(g_fd, e->size, buf) == (long)e->size;
}

void *pak_load(const char *name, u32 *size)
{
    const pentry *e = find(name);
    void *p;

    if (!e)
        return NULL;
    p = malloc(e->size);
    if (!p)
        return NULL;
    if (!pak_read(name, p, e->size)) {
        free(p);
        return NULL;
    }
    if (size)
        *size = e->size;
    return p;
}
