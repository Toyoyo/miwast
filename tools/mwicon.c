/* mwicon.c - a GEM resource file holding one monochrome icon, to merge into
 * the desktop's DESKICON.RSC with a resource editor (Interface, ORCS...).
 *
 *   mwicon <icon.png> <caption> <out.rsc>
 *
 * The image is read through ImageMagick as PAM, as mwimg reads the art: a
 * pixel darker than 50% is black. Its width must be a multiple of 16 (the
 * desktop's are 32x32). With alpha, the mask is the pixels at least 50%
 * opaque, so white inside the outline stays white over a dark desktop and
 * the outside lets the desktop through; without, the mask is the whole
 * rectangle. The caption is the text under the icon, at most 12 characters.
 *
 * The file (big-endian; every pointer an offset from the start, which
 * rsrc_load relocates):
 *
 *   RSHDR      18 words
 *   mask       w/16 * h words, then the image the same
 *   caption    NUL-terminated, padded to even
 *   ICONBLK    pmask, pdata, ptext (longs); char, xchar, ychar, xicon,
 *              yicon, wicon, hicon, xtext, ytext, wtext, htext (words)
 *   OBJECT 0   G_BOX, the tree's root, no border, the icon its one child
 *   OBJECT 1   G_ICON, LASTOB, ob_spec the ICONBLK
 *   trindex    one long: object 0
 *
 * Object sizes are in characters (low byte) plus pixels (high byte), which
 * rsrc_obfix turns into pixels at load: a resource editor shows them in
 * whatever font the machine has.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define G_BOX    20
#define G_ICON   31
#define LASTOB   0x20

#define TEXT_W   72             /* 12 characters of the desktop's 6px font */
#define TEXT_H   10

typedef struct pam {
    int w, h, depth;
    unsigned char *px;
} pam;

static int read_pam(const char *png, pam *p)
{
    char cmd[4096], line[256];
    int maxval = 0;
    FILE *f;
    size_t n;

    if (strchr(png, '\'') || strlen(png) > 4000)
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
        sscanf(line, "MAXVAL %d", &maxval);
    }
    if (p->w <= 0 || p->h <= 0 || p->depth < 1 || p->depth > 4 ||
        maxval != 255) {
        pclose(f);
        return 0;
    }
    n = (size_t)p->w * p->h * p->depth;
    p->px = malloc(n);
    if (!p->px || fread(p->px, 1, n, f) != n) {
        pclose(f);
        return 0;
    }
    pclose(f);
    return 1;
}

static unsigned char g_out[65536];
static long g_len;

static void w16(long at, unsigned v)
{
    g_out[at] = (unsigned char)(v >> 8);
    g_out[at + 1] = (unsigned char)v;
}

static void w32(long at, unsigned long v)
{
    w16(at, (unsigned)(v >> 16) & 0xFFFF);
    w16(at + 2, (unsigned)v & 0xFFFF);
}

/* An object's size: characters, plus pixels in the high byte. The system
 * font is 8x16 in high resolution, so counted at that the 72x42 of an
 * icon and its caption come out exact there. */
static unsigned chars(int px, int cell)
{
    return (unsigned)(px / cell) | (unsigned)(px % cell) << 8;
}

int main(int argc, char **argv)
{
    pam p;
    long hdr, mask, data, text, ib, obj, tri;
    long row, x, y;
    int wicon, hicon, xicon, wbox, hbox;
    FILE *f;

    if (argc != 4) {
        fprintf(stderr, "usage: mwicon <icon.png> <caption> <out.rsc>\n");
        return 2;
    }
    if (strlen(argv[2]) > 12) {
        fprintf(stderr, "mwicon: the caption is at most 12 characters\n");
        return 1;
    }
    if (!read_pam(argv[1], &p)) {
        fprintf(stderr, "mwicon: cannot read %s\n", argv[1]);
        return 1;
    }
    if (p.w % 16 || p.w > 64 || p.h > 64) {
        fprintf(stderr, "mwicon: %s is %dx%d: the width must be a multiple "
                "of 16, and 64 at most each way (the desktop's are 32x32)\n",
                argv[1], p.w, p.h);
        return 1;
    }

    wicon = p.w;
    hicon = p.h;
    wbox = wicon > TEXT_W ? wicon : TEXT_W;
    hbox = hicon + TEXT_H;
    xicon = (wbox - wicon) / 2;
    row = p.w / 8;

    hdr = 0;
    mask = 36;
    data = mask + row * p.h;
    text = data + row * p.h;
    ib = text + strlen(argv[2]) + 1;
    ib += ib & 1;
    obj = ib + 34;
    tri = obj + 2 * 24;
    g_len = tri + 4;

    for (y = 0; y < p.h; y++)
        for (x = 0; x < p.w; x++) {
            const unsigned char *c = p.px + (y * p.w + x) * p.depth;
            int grey = p.depth >= 3 ? (c[0] * 299 + c[1] * 587 +
                                       c[2] * 114) / 1000 : c[0];
            int alpha = p.depth == 2 || p.depth == 4 ?
                c[p.depth - 1] : 255;
            unsigned char bit = (unsigned char)(0x80 >> (x & 7));
            if (alpha >= 128) {
                g_out[mask + y * row + x / 8] |= bit;
                if (grey < 128)
                    g_out[data + y * row + x / 8] |= bit;
            }
        }
    memcpy(g_out + text, argv[2], strlen(argv[2]));

    /* RSHDR (aes.h): version, then the offsets, then the counts. Every
     * section the file has none of points at the tree index, past the
     * last thing it holds. */
    w16(hdr + 0, 0);            /* rsh_vrsn */
    w16(hdr + 2, (unsigned)obj);        /* rsh_object */
    w16(hdr + 4, (unsigned)ib);         /* rsh_tedinfo (none) */
    w16(hdr + 6, (unsigned)ib);         /* rsh_iconblk */
    w16(hdr + 8, (unsigned)obj);        /* rsh_bitblk (none) */
    w16(hdr + 10, (unsigned)tri);       /* rsh_frstr (none) */
    w16(hdr + 12, (unsigned)text);      /* rsh_string */
    w16(hdr + 14, (unsigned)mask);      /* rsh_imdata */
    w16(hdr + 16, (unsigned)tri);       /* rsh_frimg (none) */
    w16(hdr + 18, (unsigned)tri);       /* rsh_trindex */
    w16(hdr + 20, 2);           /* rsh_nobs */
    w16(hdr + 22, 1);           /* rsh_ntree */
    w16(hdr + 24, 0);           /* rsh_nted */
    w16(hdr + 26, 1);           /* rsh_nib */
    w16(hdr + 28, 0);           /* rsh_nbb */
    w16(hdr + 30, 0);           /* rsh_nstring */
    w16(hdr + 32, 0);           /* rsh_nimages */
    w16(hdr + 34, (unsigned)g_len);     /* rsh_rssize */

    /* ICONBLK: black on white, no letter in the icon; the icon centred
     * over a caption 72 wide. */
    w32(ib + 0, (unsigned long)mask);
    w32(ib + 4, (unsigned long)data);
    w32(ib + 8, (unsigned long)text);
    w16(ib + 12, 0x1000);       /* ib_char: foreground 1, background 0 */
    w16(ib + 14, 0);            /* ib_xchar */
    w16(ib + 16, 0);            /* ib_ychar */
    w16(ib + 18, (unsigned)xicon);
    w16(ib + 20, 0);            /* ib_yicon */
    w16(ib + 22, (unsigned)wicon);
    w16(ib + 24, (unsigned)hicon);
    w16(ib + 26, 0);            /* ib_xtext */
    w16(ib + 28, (unsigned)hicon);      /* ib_ytext */
    w16(ib + 30, TEXT_W);
    w16(ib + 32, TEXT_H);

    /* The root: a borderless white box the icon's size, as the desktop's
     * own icon trees have. */
    w16(obj + 0, 0xFFFF);       /* ob_next */
    w16(obj + 2, 1);            /* ob_head */
    w16(obj + 4, 1);            /* ob_tail */
    w16(obj + 6, G_BOX);
    w16(obj + 8, 0);            /* ob_flags */
    w16(obj + 10, 0);           /* ob_state */
    w32(obj + 12, 0x00001100UL);        /* ob_spec: no border, white */
    w16(obj + 16, 0);
    w16(obj + 18, 0);
    w16(obj + 20, chars(wbox, 8));
    w16(obj + 22, chars(hbox, 16));

    obj += 24;
    w16(obj + 0, 0);            /* ob_next: the parent */
    w16(obj + 2, 0xFFFF);
    w16(obj + 4, 0xFFFF);
    w16(obj + 6, G_ICON);
    w16(obj + 8, LASTOB);
    w16(obj + 10, 0);
    w32(obj + 12, (unsigned long)ib);
    w16(obj + 16, 0);
    w16(obj + 18, 0);
    w16(obj + 20, chars(wbox, 8));
    w16(obj + 22, chars(hbox, 16));

    w32(tri, (unsigned long)(obj - 24));

    f = fopen(argv[3], "wb");
    if (!f || fwrite(g_out, 1, (size_t)g_len, f) != (size_t)g_len ||
        fclose(f)) {
        fprintf(stderr, "mwicon: cannot write %s\n", argv[3]);
        return 1;
    }
    printf("%s: %dx%d icon \"%s\", %ld bytes\n", argv[3], p.w, p.h,
           argv[2], g_len);
    return 0;
}
