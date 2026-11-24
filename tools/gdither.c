/*
 * gdither - error diffusion to black and white with the knobs mkart.sh's
 * ffmpeg dither lacks, for the alternatives tools/jarvisdither.sh offers.
 * Not part of the build: art/ stays as mkart.sh made it unless an image is
 * redone by hand.
 *
 *   gdither [options] <in.ppm> <out.pgm>
 *
 *   -K atkinson|jarvis  the kernel (default jarvis)
 *   -k keep     the share of the error passed on (default 1); Atkinson
 *               passes 0.75, which is where its contrast comes from
 *   -r          raster: every row left to right, as ffmpeg's paletteuse
 *               does (default serpentine: odd rows right to left, mirrored)
 *   -n noise    threshold jitter at mid-grey, in 1/255ths of lightness either
 *               side, fading to none at black and white (default 0)
 *   -f fade     how fast the jitter fades away from mid-grey: it is scaled
 *               by (1 - |2L - 1|)^fade (default 1, linear). Higher keeps the
 *               light areas clean, where jitter turns near-whites black
 *   -c calm     jitter only where the image is flat: it fades with the local
 *               gradient (in levels per pixel, smoothed over ~5 pixels) and
 *               is gone at calm, so edges stay as crisp as without it
 *               (default 0, everywhere)
 *   -u sharp    unsharp mask before dithering, for readability: each pixel
 *               pushed away from its blurred surroundings by sharp times
 *               the difference (default 0, off)
 *   -R radius   the blur that mask uses, in passes of the 5-tap binomial:
 *               about sqrt(radius) pixels (default 2)
 *   -i          whole numbers: each neighbour's share of the error is cut
 *               toward zero, as C's integer division does
 *   -s steer    gradient steering (default 0, off)
 *   -e edge     the gradient that counts as a full edge (default 64)
 *
 * It dithers the way ffmpeg's paletteuse does, so that -K atkinson -k 0.75
 * -r -i is mkart.sh's dither in tone (not pixel for pixel: a dither drifts
 * apart on the smallest difference): in colour, the error spread over each
 * of R, G and B and each kept within 0..255, and black or white chosen by
 * which is nearer in OkLab. That is white when the OkLab lightness is over
 * 0.5, which for a grey is about sRGB 99, not 128: where art/'s light look
 * comes from. Its clean whites come from -i: a near-white's error is too
 * small to survive the integer division, so nothing darkens its neighbours.
 * Measured on miwaclass, white pixels overall, in its light and middle
 * areas, and solid-white 8x8 blocks all agree with ffmpeg's within 0.1%.
 *
 * Raster order and a small equal-weight kernel are what make Atkinson fall
 * into short cycles on flat mid-tones: the regular "\\\" stripes. Serpentine
 * scanning takes away the one-way push that picks the diagonal, and the
 * jitter breaks the cycles that remain, at the cost of a little grain.
 *
 * Steering bends each pixel's weights by the local structure: where the
 * image has a clear edge or line, the error goes along it rather than across
 * it. The structure is the smoothed structure tensor of the Sobel gradient
 * of the luma: its main eigenvector is the gradient direction g, and its
 * coherence says whether this is an edge or just texture. A neighbour at
 * unit offset d gets
 *
 *   w = kernel(d) * exp(-steer * s * (d.g)^2)
 *
 * with s = coherence * min(1, |gradient| / edge) in 0..1, then the weights
 * are normalised. Flat or busy areas (s near 0) dither with the plain kernel.
 * Strong steering leaves streaks; mild steering on strong edges only is
 * nearly plain.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    int dx, dy, w;
} tap;

/* Forward halves: dx, dy, weight. */
static const tap jarvis[] = {
    { 1, 0, 7 }, { 2, 0, 5 },
    { -2, 1, 3 }, { -1, 1, 5 }, { 0, 1, 7 }, { 1, 1, 5 }, { 2, 1, 3 },
    { -2, 2, 1 }, { -1, 2, 3 }, { 0, 2, 5 }, { 1, 2, 3 }, { 2, 2, 1 },
    { 0, 0, 0 }
};
static const tap atkinson[] = {
    { 1, 0, 1 }, { 2, 0, 1 },
    { -1, 1, 1 }, { 0, 1, 1 }, { 1, 1, 1 },
    { 0, 2, 1 },
    { 0, 0, 0 }
};

static unsigned char *read_ppm(const char *path, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    char magic[3];
    int max;
    size_t n;
    unsigned char *px;

    if (!f || fscanf(f, "%2s %d %d %d", magic, w, h, &max) != 4 ||
        magic[0] != 'P' || magic[1] != '6' || max != 255) {
        fprintf(stderr, "%s: not an 8-bit P6 PPM\n", path);
        exit(1);
    }
    fgetc(f);
    n = (size_t)*w * *h * 3;
    px = malloc(n);
    if (fread(px, 1, n, f) != n) {
        fprintf(stderr, "%s: short\n", path);
        exit(1);
    }
    fclose(f);
    return px;
}

static float linear(float c)
{
    c /= 255;
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

/* OkLab lightness of an sRGB colour, 0..1. */
static float oklab_l(float r, float g, float b)
{
    float lr = linear(r), lg = linear(g), lb = linear(b);
    float l = 0.4122214708f * lr + 0.5363325363f * lg + 0.0514459929f * lb;
    float m = 0.2119034982f * lr + 0.6806995451f * lg + 0.1073969566f * lb;
    float s = 0.0883024619f * lr + 0.2817188376f * lg + 0.6299787005f * lb;

    return 0.2104542553f * cbrtf(l) + 0.7936177850f * cbrtf(m) -
           0.0040720468f * cbrtf(s);
}

static float at(const float *p, int w, int h, int x, int y)
{
    x = x < 0 ? 0 : x >= w ? w - 1 : x;
    y = y < 0 ? 0 : y >= h ? h - 1 : y;
    return p[y * w + x];
}

/* 5-tap binomial blur, separable, in place. */
static void blur(float *p, int w, int h)
{
    static const float k[5] = { 1 / 16.f, 4 / 16.f, 6 / 16.f, 4 / 16.f,
                                1 / 16.f };
    float *t = malloc(sizeof *t * w * h);
    int x, y, i;

    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            float s = 0;
            for (i = -2; i <= 2; i++)
                s += k[i + 2] * at(p, w, h, x + i, y);
            t[y * w + x] = s;
        }
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            float s = 0;
            for (i = -2; i <= 2; i++)
                s += k[i + 2] * at(t, w, h, x, y + i);
            p[y * w + x] = s;
        }
    free(t);
}

/* Unsharp mask on the interleaved RGB in v, each channel kept in 0..255. */
static void unsharp(float *v, int w, int h, float amount, int passes)
{
    int n = w * h, i, k, j;
    float *ch = malloc(sizeof *ch * n);

    for (k = 0; k < 3; k++) {
        for (i = 0; i < n; i++)
            ch[i] = v[i * 3 + k];
        for (j = 0; j < passes; j++)
            blur(ch, w, h);
        for (i = 0; i < n; i++) {
            float *q = &v[i * 3 + k];

            *q += amount * (*q - ch[i]);
            *q = *q < 0 ? 0 : *q > 255 ? 255 : *q;
        }
    }
    free(ch);
}

/* A fixed hash of the position, -0.5..0.5: the same image dithers the same
 * way every time. */
static float jitter(int x, int y)
{
    uint32_t v = (uint32_t)x * 0x9E3779B1u ^ (uint32_t)y * 0x85EBCA77u;

    v ^= v >> 15;
    v *= 0x2C1B3C6Du;
    v ^= v >> 12;
    v *= 0x297A2D39u;
    v ^= v >> 15;
    return (v >> 8) / 16777216.f - 0.5f;
}

/* The structure tensor of the Sobel gradient, smoothed so a one-pixel line
 * (two opposite gradients side by side) reads as one orientation: the
 * gradient direction into gx, gy, the edge strength into s and the
 * gradient's size, whatever its direction, into mg. */
static void structure(const float *v, int w, int h, float edge, float *gx,
                      float *gy, float *s, float *mg)
{
    int n = w * h, x, y, i;
    float *jxx = malloc(sizeof *jxx * n);
    float *jxy = malloc(sizeof *jxy * n);
    float *jyy = malloc(sizeof *jyy * n);

    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            float dx = (at(v, w, h, x + 1, y - 1) + 2 * at(v, w, h, x + 1, y) +
                        at(v, w, h, x + 1, y + 1) - at(v, w, h, x - 1, y - 1) -
                        2 * at(v, w, h, x - 1, y) - at(v, w, h, x - 1, y + 1)) / 4;
            float dy = (at(v, w, h, x - 1, y + 1) + 2 * at(v, w, h, x, y + 1) +
                        at(v, w, h, x + 1, y + 1) - at(v, w, h, x - 1, y - 1) -
                        2 * at(v, w, h, x, y - 1) - at(v, w, h, x + 1, y - 1)) / 4;
            jxx[y * w + x] = dx * dx;
            jxy[y * w + x] = dx * dy;
            jyy[y * w + x] = dy * dy;
        }
    blur(jxx, w, h);
    blur(jxy, w, h);
    blur(jyy, w, h);
    for (i = 0; i < n; i++) {
        float a = (jxx[i] + jyy[i]) / 2;
        float b = sqrtf(((jxx[i] - jyy[i]) / 2) * ((jxx[i] - jyy[i]) / 2) +
                        jxy[i] * jxy[i]);
        float l1 = a + b, l2 = a - b;
        float th = 0.5f * atan2f(2 * jxy[i], jxx[i] - jyy[i]);
        float coh = l1 + l2 > 1e-6f ? (l1 - l2) / (l1 + l2) : 0;
        float mag = sqrtf(l1 > 0 ? l1 : 0) / edge;

        gx[i] = cosf(th);
        gy[i] = sinf(th);
        s[i] = coh * (mag < 1 ? mag : 1);
        mg[i] = sqrtf(l1 + l2 > 0 ? l1 + l2 : 0);
    }
    free(jxx);
    free(jxy);
    free(jyy);
}

static void usage(void)
{
    fprintf(stderr, "usage: gdither [-K atkinson|jarvis] [-k keep] [-r] "
            "[-n noise] [-f fade] [-c calm] [-i] [-u sharp] [-R radius] "
            "[-s steer] [-e edge] "
            "<in.ppm> <out.pgm>\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const tap *nb = jarvis;
    float steer = 0, edge = 64, keep = 1, noise = 0, fade = 1, calm = 0;
    float sharp = 0;
    int radius = 2;
    int raster = 0, whole = 0, ntap, c, w, h, x, y, i, k, n;
    unsigned char *in, *out;
    float *v, *l0, *gx = NULL, *gy = NULL, *s = NULL, *mg = NULL;
    FILE *f;

    while ((c = getopt(argc, argv, "K:k:rn:f:c:s:e:iu:R:")) != -1)
        switch (c) {
        case 'K':
            if (!strcmp(optarg, "atkinson"))
                nb = atkinson;
            else if (!strcmp(optarg, "jarvis"))
                nb = jarvis;
            else
                usage();
            break;
        case 'k': keep = atof(optarg); break;
        case 'r': raster = 1; break;
        case 'n': noise = atof(optarg); break;
        case 's': steer = atof(optarg); break;
        case 'e': edge = atof(optarg); break;
        case 'f': fade = atof(optarg); break;
        case 'i': whole = 1; break;
        case 'c': calm = atof(optarg); break;
        case 'u': sharp = atof(optarg); break;
        case 'R': radius = atoi(optarg); break;
        default: usage();
        }
    if (argc - optind != 2)
        usage();
    for (ntap = 0; nb[ntap].w; ntap++)
        ;

    in = read_ppm(argv[optind], &w, &h);
    n = w * h;
    v = malloc(sizeof *v * n * 3);
    l0 = malloc(sizeof *l0 * n);
    out = malloc(n);
    for (i = 0; i < n * 3; i++)
        v[i] = in[i];
    if (sharp > 0)
        unsharp(v, w, h, sharp, radius);
    for (i = 0; i < n; i++)
        l0[i] = oklab_l(v[i * 3], v[i * 3 + 1], v[i * 3 + 2]);
    if (steer > 0 || calm > 0) {
        float *luma = malloc(sizeof *luma * n);

        for (i = 0; i < n; i++)
            luma[i] = 0.299f * v[i * 3] + 0.587f * v[i * 3 + 1] +
                      0.114f * v[i * 3 + 2];
        gx = malloc(sizeof *gx * n);
        gy = malloc(sizeof *gy * n);
        s = malloc(sizeof *s * n);
        mg = malloc(sizeof *mg * n);
        structure(luma, w, h, edge, gx, gy, s, mg);
        free(luma);
    }

    for (y = 0; y < h; y++) {
        int dir = raster || !(y & 1) ? 1 : -1;
        for (x = dir > 0 ? 0 : w - 1; x >= 0 && x < w; x += dir) {
            int p = y * w + x;
            /* Jitter is strongest at mid-grey, where the cycles form, and
             * gone at the ends, so black and white stay clean; and only on
             * flat areas, the only ones the cycles form on, so edges stay
             * as crisp as without it. */
            float mid = powf(1 - fabsf(2 * l0[p] - 1), fade);

            if (calm > 0)
                mid *= mg[p] < calm ? 1 - mg[p] / calm : 0;
            float t = 0.5f + noise / 255 * mid * 2 * jitter(x, y);
            float *px = &v[p * 3], e[3], wt[12], sum = 0;

            out[p] = oklab_l(px[0], px[1], px[2]) > t ? 255 : 0;
            for (k = 0; k < 3; k++)
                e[k] = (px[k] - out[p]) * keep;
            for (i = 0; i < ntap; i++) {
                float dx = nb[i].dx * dir, dy = nb[i].dy;

                wt[i] = nb[i].w;
                if (steer > 0) {
                    float d = (dx * gx[p] + dy * gy[p]) /
                              sqrtf(dx * dx + dy * dy);
                    wt[i] *= expf(-steer * s[p] * d * d);
                }
                sum += wt[i];
            }
            /* Error for a neighbour outside the image is dropped, as the
             * plain kernels do, not piled onto the ones inside. Each channel
             * stays within 0..255, as ffmpeg keeps it in a byte. */
            for (i = 0; i < ntap; i++) {
                int nx = x + nb[i].dx * dir, ny = y + nb[i].dy;

                if (nx < 0 || nx >= w || ny >= h)
                    continue;
                for (k = 0; k < 3; k++) {
                    float *q = &v[(ny * w + nx) * 3 + k];

                    float d = e[k] * wt[i] / sum;

                    /* The nudge keeps 30/6 from being 4.99999. */
                    *q += whole ? truncf(d + (d > 0 ? 1e-4f : -1e-4f)) : d;
                    *q = *q < 0 ? 0 : *q > 255 ? 255 : *q;
                }
            }
        }
    }

    f = fopen(argv[optind + 1], "wb");
    if (!f) {
        perror(argv[optind + 1]);
        return 1;
    }
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    fwrite(out, 1, n, f);
    fclose(f);
    return 0;
}
