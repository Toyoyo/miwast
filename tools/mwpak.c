/* mwpak.c - pack the compiled script, images and music into the ST's pak.
 *
 *   mwpak <out.pak> <SCRIPT.MWB> <manifest.txt> <data-dir> <game-dir>
 *
 * What goes in is exactly what the manifest names: SCRIPT.MWB and the
 * TXT and BTM entries beside it (one .MWT per language, the rhythm game's
 * beatmaps), every IMG
 * entry's converted file from <data-dir>, every MUS entry's SNDH from
 * <game-dir> under its 8.3 name. So a pak cannot lack an image the script
 * uses, nor carry one it does not. Format in src/mwb.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/mwb.h"

typedef struct entry {
    char name[14];
    char path[1024];
    unsigned long size, off;
} entry;

static entry g_e[1024];
static int   g_n;

static int add(const char *name, const char *path)
{
    FILE *f;
    int i;

    for (i = 0; i < g_n; i++)
        if (!strcmp(g_e[i].name, name))
            return 1;               /* the same file named twice */
    if (strlen(name) > 12 || g_n == 1024) {
        fprintf(stderr, "error: cannot add %s\n", name);
        return 0;
    }
    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: missing %s (for %s)\n", path, name);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    g_e[g_n].size = (unsigned long)ftell(f);
    fclose(f);
    strcpy(g_e[g_n].name, name);
    strcpy(g_e[g_n].path, path);
    g_n++;
    return 1;
}

static int cmp(const void *a, const void *b)
{
    return strcmp(((const entry *)a)->name, ((const entry *)b)->name);
}

static void put16(FILE *f, unsigned v)
{
    fputc((v >> 8) & 0xFF, f);
    fputc(v & 0xFF, f);
}

static void put32(FILE *f, unsigned long v)
{
    put16(f, (unsigned)(v >> 16) & 0xFFFF);
    put16(f, (unsigned)v & 0xFFFF);
}

int main(int argc, char **argv)
{
    char line[2048], tag[8], name[32], rest[1024], path[2048];
    char buf[65536];
    unsigned long off, total = 0;
    int i, ok = 1, j;
    FILE *m, *f, *in;
    size_t n;

    if (argc != 6) {
        fprintf(stderr, "usage: mwpak <out.pak> <SCRIPT.MWB> <manifest.txt> "
                "<data-dir> <game-dir>\n");
        return 2;
    }
    ok &= add("SCRIPT.MWB", argv[2]);

    m = fopen(argv[3], "r");
    if (!m) {
        perror(argv[3]);
        return 1;
    }
    while (fgets(line, sizeof(line), m)) {
        line[strcspn(line, "\n")] = '\0';
        if (sscanf(line, "%7s %31s %1023[^\n]", tag, name, rest) != 3)
            continue;
        if (!strcmp(tag, "IMG")) {
            sprintf(path, "%s/%s", argv[4], name);
            ok &= add(name, path);
        } else if (!strcmp(tag, "MUS")) {
            sprintf(path, "%s/%s", argv[5], rest);
            ok &= add(name, path);
        } else if (!strcmp(tag, "TXT") || !strcmp(tag, "BTM")) {
            /* Beside the script. */
            const char *slash = strrchr(argv[2], '/');
            int dl = slash ? (int)(slash - argv[2]) : 1;
            sprintf(path, "%.*s/%s", dl, slash ? argv[2] : ".", name);
            ok &= add(name, path);
        }
    }
    fclose(m);
    if (!ok)
        return 1;

    qsort(g_e, g_n, sizeof(g_e[0]), cmp);
    off = MWB_PAK_HEADER_SIZE + (unsigned long)MWB_PAK_ENTRY_SIZE * g_n;
    for (i = 0; i < g_n; i++) {
        g_e[i].off = off;
        off += (g_e[i].size + 1) & ~1UL;
    }

    f = fopen(argv[1], "wb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    put32(f, MWB_PAK_MAGIC);
    put16(f, g_n);
    put16(f, 0);
    for (i = 0; i < g_n; i++) {
        for (j = 0; j < 14; j++)
            fputc(j < (int)strlen(g_e[i].name) ? g_e[i].name[j] : 0, f);
        put32(f, g_e[i].off);
        put32(f, g_e[i].size);
        put16(f, 0);
    }
    for (i = 0; i < g_n; i++) {
        in = fopen(g_e[i].path, "rb");
        if (!in || ftell(f) != (long)g_e[i].off) {
            fprintf(stderr, "error: layout or read failure at %s\n",
                    g_e[i].name);
            return 1;
        }
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
            fwrite(buf, 1, n, f);
        fclose(in);
        if (g_e[i].size & 1)
            fputc(0, f);
        total += g_e[i].size;
    }
    fclose(f);
    printf("pak: %d files, %lu bytes of data\n", g_n, total);
    return 0;
}
