/* mwimg.c - convert the hand-editable art/ PNGs into the ST's image files.
 *
 *   mwimg <manifest.txt> <art-dir> <out-dir>
 *
 * For each IMG line of the manifest, reads the art file it names (through
 * ImageMagick, as PAM, so any PNG an editor saves will do) and writes the
 * .MBG or .MSP named there, flipping it if the line says so. Formats are in
 * src/mwb.h.
 *
 * The art must still be the size the script was compiled against -- that is
 * what every position in SCRIPT.MWB was resolved with -- so a size mismatch
 * is an error. A pixel that is neither pure black, pure white nor fully
 * transparent is thresholded at 50% and counted, so a stray grey brush
 * stroke is reported rather than silently rounded.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/mwb.h"

static int g_errors;

typedef struct pam {
    int w, h, depth, maxval;
    unsigned char *px;              /* w * h * depth bytes */
} pam;

static int read_pam(const char *png, pam *p)
{
    char cmd[4096], line[256], tupl[64] = "";
    FILE *f;
    size_t n;

    /* The name comes from our own manifest; no quotes in it. */
    if (strchr(png, '\''))
        return 0;
    sprintf(cmd, "convert '%s' -depth 8 pam:- 2>/dev/null", png);
    f = popen(cmd, "r");
    if (!f)
        return 0;
    memset(p, 0, sizeof(*p));
    if (!fgets(line, sizeof(line), f) || strncmp(line, "P7", 2)) {
        pclose(f);
        return 0;
    }
    while (fgets(line, sizeof(line), f) && strncmp(line, "ENDHDR", 6)) {
        sscanf(line, "WIDTH %d", &p->w);
        sscanf(line, "HEIGHT %d", &p->h);
        sscanf(line, "DEPTH %d", &p->depth);
        sscanf(line, "MAXVAL %d", &p->maxval);
        sscanf(line, "TUPLTYPE %63s", tupl);
    }
    if (p->w <= 0 || p->h <= 0 || p->depth < 1 || p->depth > 4 ||
        p->maxval != 255) {
        pclose(f);
        return 0;
    }
    n = (size_t)p->w * p->h * p->depth;
    p->px = malloc(n);
    if (fread(p->px, 1, n, f) != n) {
        pclose(f);
        free(p->px);
        return 0;
    }
    pclose(f);
    return 1;
}

/* The pixel at (x, y): 1 if covered, and *black set if it is black. Gray
 * (depth 1-2) and RGB (3-4) alike; the last channel is alpha when the depth
 * is even. */
static int pixel(const pam *p, int x, int y, int *black, long *odd)
{
    const unsigned char *q = p->px + ((size_t)y * p->w + x) * p->depth;
    int lum, a = 255;

    if (p->depth == 1 || p->depth == 2)
        lum = q[0];
    else
        lum = (q[0] * 299 + q[1] * 587 + q[2] * 114) / 1000;
    if (p->depth == 2 || p->depth == 4)
        a = q[p->depth - 1];

    if ((a != 0 && a != 255) || (a && lum != 0 && lum != 255))
        (*odd)++;
    *black = lum < 128;
    return a >= 128;
}

static void put16(FILE *f, unsigned v)
{
    fputc((v >> 8) & 0xFF, f);
    fputc(v & 0xFF, f);
}

static int convert_one(const char *art, const char *kind, int flip, int w,
                       int h, const char *artfile, const char *outdir,
                       const char *out)
{
    char path[2048], opath[2048];
    unsigned char *mask, *data;
    int wpr, x, y, sx, black, covered, opaque = 1;
    long odd = 0;
    size_t plane;
    pam p;
    FILE *f;

    sprintf(path, "%s/%s", art, artfile);
    if (!read_pam(path, &p)) {
        fprintf(stderr, "error: cannot read %s (run `make images`)\n", path);
        return 0;
    }
    if (p.w != w || p.h != h) {
        fprintf(stderr, "error: %s is %dx%d, the script was compiled for "
                "%dx%d\n", path, p.w, p.h, w, h);
        free(p.px);
        return 0;
    }

    wpr = (w + 15) / 16;
    plane = (size_t)wpr * 2 * h;
    mask = calloc(plane, 1);
    data = calloc(plane, 1);

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            sx = flip ? w - 1 - x : x;
            covered = pixel(&p, sx, y, &black, &odd);
            if (!covered) {
                opaque = 0;
                continue;
            }
            mask[(size_t)y * wpr * 2 + x / 8] |= 0x80 >> (x & 7);
            if (black)
                data[(size_t)y * wpr * 2 + x / 8] |= 0x80 >> (x & 7);
        }
    }
    free(p.px);

    if (odd)
        fprintf(stderr, "warning: %s: %ld pixels are not pure black, white "
                "or transparent; thresholded at 50%%\n", path, odd);

    sprintf(opath, "%s/%s", outdir, out);
    f = fopen(opath, "wb");
    if (!f) {
        perror(opath);
        free(mask);
        free(data);
        return 0;
    }
    if (!strcmp(kind, "bg")) {
        if (w != MWB_STAGE_W || h != MWB_STAGE_H || !opaque) {
            fprintf(stderr, "error: %s: a background must be %dx%d and "
                    "fully opaque\n", path, MWB_STAGE_W, MWB_STAGE_H);
            fclose(f);
            free(mask);
            free(data);
            return 0;
        }
        fwrite(data, 1, plane, f);
    } else {
        put16(f, w);
        put16(f, h);
        put16(f, wpr);
        put16(f, opaque ? MWB_SPR_OPAQUE : 0);
        if (!opaque)
            fwrite(mask, 1, plane, f);
        fwrite(data, 1, plane, f);
    }
    fclose(f);
    free(mask);
    free(data);
    return 1;
}

int main(int argc, char **argv)
{
    char line[2048], tag[8], out[32], kind[16], artfile[1024];
    int flip, w, h, n = 0;
    FILE *m;

    if (argc != 4) {
        fprintf(stderr, "usage: mwimg <manifest.txt> <art-dir> <out-dir>\n");
        return 2;
    }
    m = fopen(argv[1], "r");
    if (!m) {
        perror(argv[1]);
        return 1;
    }
    while (fgets(line, sizeof(line), m)) {
        line[strcspn(line, "\n")] = '\0';
        /* Tab-separated; the art and source names have spaces. */
        if (sscanf(line, "%7[^\t]\t%31[^\t]\t%15[^\t]\t%d\t%d\t%d\t%1023[^\t]",
                   tag, out, kind, &flip, &w, &h, artfile) != 7 ||
            strcmp(tag, "IMG"))
            continue;
        if (convert_one(argv[2], kind, flip, w, h, artfile, argv[3], out))
            n++;
        else
            g_errors++;
    }
    fclose(m);

    printf("data: %d images converted\n", n);
    if (g_errors) {
        fprintf(stderr, "%d error(s)\n", g_errors);
        return 1;
    }
    return 0;
}
