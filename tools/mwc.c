/* mwc.c - compile a Ren'Py game into the ST's .mwb script.
 *
 *   mwc <game-dir> <out.mwb> <manifest.txt>
 *
 * The C89 engine's parser (tools/rpy/, copied unmodified from
 * minirenpy-video/src) reads the .rpy files into its statement array. This
 * walks that array from `label start` in execution order and simulates the
 * scene list, so that each `show` comes out as a slot, an image and a final
 * pixel position at 640x360. Nothing the ST does needs a float or a name.
 *
 * jump, menu and `if` on the story's variables branch the walk: each branch
 * is walked with the state it is reached with, and where two paths come to
 * the same statement with the same state they share the code from there
 * (compile_from). The path check then plays every path through the result
 * against the source (check_paths). `call` is followed in line, its
 * return place part of the walk's state (cstate.ret): the ST has no call
 * stack. A statement the runtime cannot honour stops the build, not the
 * player.
 *
 * The manifest lists every image and music file the script uses, with the
 * 8.3 name it must have on the ST, for the asset converters.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>

#include "mr.h"
#include "script.h"
#include "vfs.h"
#include "tl.h"
#include "expr.h"
#include "lex.h"
#include "../src/mwb.h"

/* ---- platform stubs the parser needs ---------------------------------- */

/* The path check walks the script again, path after path: what it would
 * say has been said by the compile. */
static int g_quiet;

void mr_log(const char *fmt, ...)
{
    va_list ap;
    if (g_quiet)
        return;
    va_start(ap, fmt);
    fputs("  log: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void mr_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("FATAL: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

void mr_plat_watch_write(const void *addr)
{
    (void)addr;
}

static const mr_script *g_s;
static const char      *g_dir;
static int              g_errors;

/* An error names the source line and lets the walk carry on, so one build
 * reports every problem rather than the first. */
static void err(const mr_stmt *st, const char *fmt, ...)
{
    va_list ap;
    if (g_quiet)
        return;
    fprintf(stderr, "%s:%d: error: ",
            st ? g_s->files[st->file] : "?", st ? st->line : 0);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    g_errors++;
}

static void warn(const mr_stmt *st, const char *fmt, ...)
{
    va_list ap;
    if (g_quiet)
        return;
    fprintf(stderr, "%s:%d: warning: ",
            st ? g_s->files[st->file] : "?", st ? st->line : 0);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static int name_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++, b++;
    }
    return *a == *b;
}

/* ---- the image registry ------------------------------------------------ */

/* What `image` statements and images/ declare: a name, a file and the flip
 * flag. Explicit statements first, then every images/*.png under its
 * basename unless the name is taken (image.c:540 mr_image_autodefine_dir). */
typedef struct regimg {
    char name[MR_MAX_NAME];
    char file[MR_PATH_MAX];         /* game-relative */
    int  flip;
    int  w, h;                      /* full scale, from the PNG header */
    int  zoom;                      /* percent, 0 for none: the rhythm
                                     * game's notes at 1.25x, an image of
                                     * their own (reg_zoomed) */
} regimg;

static regimg *g_reg;
static int     g_nreg, g_capreg;

static int reg_find(const char *name)
{
    int i;
    for (i = 0; i < g_nreg; i++)
        if (name_eq(g_reg[i].name, name))
            return i;
    return -1;
}

static int png_size(const char *rel, int *w, int *h)
{
    char path[1024];
    unsigned char b[24];
    FILE *f;

    sprintf(path, "%s/%s", g_dir, rel);
    f = fopen(path, "rb");
    if (!f)
        return 0;
    if (fread(b, 1, 24, f) != 24 || memcmp(b + 12, "IHDR", 4)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    *w = (b[16] << 24) | (b[17] << 16) | (b[18] << 8) | b[19];
    *h = (b[20] << 24) | (b[21] << 16) | (b[22] << 8) | b[23];
    return 1;
}

static void reg_define(const char *name, const char *file, int flip)
{
    int i = reg_find(name);

    /* A redefinition replaces, as Ren'Py's does. */
    if (i < 0) {
        if (g_nreg == g_capreg) {
            g_capreg = g_capreg ? g_capreg * 2 : 64;
            g_reg = realloc(g_reg, g_capreg * sizeof(*g_reg));
        }
        i = g_nreg++;
    }
    memset(&g_reg[i], 0, sizeof(g_reg[i]));
    strncpy(g_reg[i].name, name, MR_MAX_NAME - 1);
    strncpy(g_reg[i].file, file, MR_PATH_MAX - 1);
    g_reg[i].flip = flip;
    if (!png_size(file, &g_reg[i].w, &g_reg[i].h)) {
        fprintf(stderr, "error: image '%s': cannot read %s\n", name, file);
        g_errors++;
    }
}

/* An image no `image` statement names -- the rhythm game's -- at `zoom`
 * percent (0: as it is), registered under a name no script can say. Its
 * size is the zoomed one, so everything downstream sees that. */
static int reg_zoomed(const char *file, int zoom)
{
    char name[MR_MAX_NAME];
    int i;

    snprintf(name, sizeof(name), ":%s@%d", file, zoom);
    i = reg_find(name);
    if (i >= 0)
        return i;
    reg_define(name, file, 0);
    i = reg_find(name);
    if (zoom) {
        g_reg[i].zoom = zoom;
        g_reg[i].w = (g_reg[i].w * zoom + 50) / 100;
        g_reg[i].h = (g_reg[i].h * zoom + 50) / 100;
    }
    return i;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void reg_autodefine(void)
{
    char path[1024];
    char *names[1024];
    int n = 0, i;
    DIR *d;
    struct dirent *e;

    sprintf(path, "%s/images", g_dir);
    d = opendir(path);
    if (!d) {
        fprintf(stderr, "error: no %s\n", path);
        g_errors++;
        return;
    }
    while ((e = readdir(d)) != NULL && n < 1024) {
        size_t l = strlen(e->d_name);
        if (l > 4 && name_eq(e->d_name + l - 4, ".png"))
            names[n++] = strdup(e->d_name);
    }
    closedir(d);
    /* Sorted, so the build does not depend on directory order. */
    qsort(names, n, sizeof(names[0]), cmp_str);

    for (i = 0; i < n; i++) {
        char name[MR_MAX_NAME];
        char file[MR_PATH_MAX];
        strncpy(name, names[i], sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        name[strlen(name) - 4] = '\0';
        sprintf(file, "images/%s", names[i]);
        if (reg_find(name) < 0)
            reg_define(name, file, 0);
        free(names[i]);
    }
}

/* Ren'Py's attribute matching, as image.c:417 mr_image_choose implements it:
 * an exact name wins; otherwise the candidate with the same tag that carries
 * every attribute asked for, nothing outside what was asked for or is
 * already worn, and the most attributes. */
#define MAX_ATTRS 8
#define ATTR_LEN  20

typedef struct attrset {
    char w[MAX_ATTRS][ATTR_LEN];
    int  n;
} attrset;

static void split_attrs(const char *s, attrset *a)
{
    int k;
    a->n = 0;
    while (*s && a->n < MAX_ATTRS) {
        while (*s == ' ')
            s++;
        if (!*s)
            break;
        k = 0;
        while (*s && *s != ' ') {
            if (k < ATTR_LEN - 1)
                a->w[a->n][k++] = *s;
            s++;
        }
        a->w[a->n][k] = '\0';
        a->n++;
    }
}

static int attr_in(const attrset *a, int from, const char *w)
{
    int i;
    for (i = from; i < a->n; i++)
        if (!strcmp(a->w[i], w))
            return 1;
    return 0;
}

static void attr_add(attrset *a, const char *w)
{
    if (attr_in(a, 0, w) || a->n >= MAX_ATTRS)
        return;
    strncpy(a->w[a->n], w, ATTR_LEN - 1);
    a->w[a->n][ATTR_LEN - 1] = '\0';
    a->n++;
}

static void attr_del(attrset *a, const char *w)
{
    int i;
    for (i = 0; i < a->n; i++) {
        if (!strcmp(a->w[i], w)) {
            memmove(&a->w[i], &a->w[i + 1], (a->n - i - 1) * sizeof(a->w[0]));
            a->n--;
            return;
        }
    }
}

static int reg_choose(const mr_stmt *st, const char *name, const char *current)
{
    attrset want, cur, req, opt, cand;
    int i, k, ok, best = -1, bestlen = -1, nmatch = 0;

    i = reg_find(name);
    if (i >= 0)
        return i;

    split_attrs(name, &want);
    if (want.n == 0)
        return -1;

    opt.n = 0;
    if (current && current[0]) {
        split_attrs(current, &cur);
        if (cur.n > 0 && !strcmp(cur.w[0], want.w[0]))
            for (k = 1; k < cur.n; k++)
                attr_add(&opt, cur.w[k]);
    }

    req.n = 0;
    for (k = 1; k < want.n; k++) {
        if (want.w[k][0] == '-') {
            attr_del(&opt, want.w[k] + 1);
            attr_del(&req, want.w[k] + 1);
        } else {
            attr_add(&req, want.w[k]);
        }
    }

    for (i = 0; i < g_nreg; i++) {
        split_attrs(g_reg[i].name, &cand);
        if (cand.n == 0 || strcmp(cand.w[0], want.w[0]))
            continue;
        ok = 1;
        for (k = 1; k < cand.n && ok; k++)
            if (!attr_in(&req, 0, cand.w[k]) && !attr_in(&opt, 0, cand.w[k]))
                ok = 0;
        for (k = 0; k < req.n && ok; k++)
            if (!attr_in(&cand, 1, req.w[k]))
                ok = 0;
        if (!ok)
            continue;
        if (cand.n - 1 > bestlen) {
            bestlen = cand.n - 1;
            best = i;
            nmatch = 1;
        } else if (cand.n - 1 == bestlen) {
            nmatch++;
        }
    }

    /* Ren'Py raises on a tie (image.py:1060); so does the build. */
    if (best >= 0 && nmatch > 1)
        err(st, "'%s' matches %d images equally well", name, nmatch);
    return best;
}

/* ---- placement --------------------------------------------------------- */

/* scene.c:113 place_merge: a show statement updates the tag's transform
 * rather than replacing it, and align and pos exclude each other. */
static void place_merge(mr_place *dst, const mr_place *src)
{
    if (src->has_xalign) {
        dst->has_xalign = 1; dst->xalign = src->xalign; dst->has_xpos = 0;
    }
    if (src->has_xpos) {
        dst->has_xpos = 1; dst->xpos = src->xpos; dst->xpos_px = src->xpos_px;
        dst->has_xalign = 0;
    }
    if (src->has_yalign) {
        dst->has_yalign = 1; dst->yalign = src->yalign; dst->has_ypos = 0;
    }
    if (src->has_ypos) {
        dst->has_ypos = 1; dst->ypos = src->ypos; dst->ypos_px = src->ypos_px;
        dst->has_yalign = 0;
    }
    if (src->has_xoffset) { dst->has_xoffset = 1; dst->xoffset = src->xoffset; }
    if (src->has_yoffset) { dst->has_yoffset = 1; dst->yoffset = src->yoffset; }
    if (src->has_xanchor) {
        dst->has_xanchor = 1; dst->xanchor = src->xanchor;
        dst->xanchor_px = src->xanchor_px;
    }
    if (src->has_yanchor) {
        dst->has_yanchor = 1; dst->yanchor = src->yanchor;
        dst->yanchor_px = src->yanchor_px;
    }
    if (src->has_alpha) { dst->has_alpha = 1; dst->alpha = src->alpha; }
    if (src->has_xzoom) { dst->has_xzoom = 1; dst->xzoom = src->xzoom; }
    if (src->has_yzoom) { dst->has_yzoom = 1; dst->yzoom = src->yzoom; }
    if (src->has_zoom)  { dst->has_zoom = 1;  dst->zoom = src->zoom; }
}

/* scene.c:873 mr_place_resolve, at the 1280x720 stage. With no placement at
 * all Ren'Py's default transform stands the image bottom-centre; with one,
 * whatever it does not mention is 0. */
static void place_resolve(const mr_place *pl, int has_place,
                          int img_w, int img_h, int *x, int *y)
{
    float xa = has_place ? 0.0f : 0.5f;
    float ya = has_place ? 0.0f : 1.0f;

    if (has_place && pl->has_xalign)
        xa = pl->xalign;
    if (has_place && pl->has_yalign)
        ya = pl->yalign;

    if (has_place && pl->has_xpos) {
        float fx = pl->xpos_px ? pl->xpos : pl->xpos * (float)MR_STAGE_W;
        if (pl->has_xanchor)
            fx -= pl->xanchor_px ? pl->xanchor : pl->xanchor * (float)img_w;
        else if (pl->has_xalign)
            fx -= pl->xalign * (float)img_w;
        *x = (int)fx;
    } else {
        *x = (int)(xa * (float)(MR_STAGE_W - img_w));
    }

    if (has_place && pl->has_ypos) {
        float fy = pl->ypos_px ? pl->ypos : pl->ypos * (float)MR_STAGE_H;
        if (pl->has_yanchor)
            fy -= pl->yanchor_px ? pl->yanchor : pl->yanchor * (float)img_h;
        else if (pl->has_yalign)
            fy -= pl->yalign * (float)img_h;
        *y = (int)fy;
    } else {
        *y = (int)(ya * (float)(MR_STAGE_H - img_h));
    }

    if (has_place) {
        *x += pl->xoffset;
        *y += pl->yoffset;
    }
}

/* Full-scale stage coordinates to the ST's half scale. Rounds toward minus
 * infinity, so an image half off the left edge stays half off. */
static int half(int v)
{
    return v >= 0 ? v / 2 : -((-v + 1) / 2);
}

static int half_size(int v)
{
    return (v + 1) / 2;
}

static void check_place(const mr_stmt *st, const mr_place *pl)
{
    if (pl->has_alpha || pl->has_xzoom || pl->has_yzoom || pl->has_zoom)
        err(st, "alpha/zoom are not supported on the ST");
}

/* ---- output tables ----------------------------------------------------- */

/* An image as the ST sees it: one file per (source, flip, kind), because a
 * flipped sprite is converted flipped and a background and a sprite are
 * different formats. */
typedef struct outimg {
    int  reg;
    int  kind;
} outimg;

static outimg g_img[512];
static int    g_nimg;

static int img_id(const mr_stmt *st, int reg, int kind)
{
    int i;
    for (i = 0; i < g_nimg; i++) {
        if (!strcmp(g_reg[g_img[i].reg].file, g_reg[reg].file) &&
            g_reg[g_img[i].reg].flip == g_reg[reg].flip &&
            g_reg[g_img[i].reg].zoom == g_reg[reg].zoom &&
            g_img[i].kind == kind)
            return i;
    }
    if (g_nimg == 512) {
        err(st, "too many images");
        return 0;
    }
    g_img[g_nimg].reg = reg;
    g_img[g_nimg].kind = kind;
    return g_nimg++;
}

static char g_tag[64][MR_MAX_NAME];
static int  g_ntag;

static int tag_id(const char *tag)
{
    int i;
    for (i = 0; i < g_ntag; i++)
        if (!strcmp(g_tag[i], tag))
            return i;
    if (g_ntag == 64)
        mr_fatal("too many tags");
    strcpy(g_tag[g_ntag], tag);
    return g_ntag++;
}

static char g_mus[32][MR_PATH_MAX];     /* game-relative .snd path */
static int  g_nmus;

static int mus_id(const mr_stmt *st, const char *file)
{
    char base[MR_PATH_MAX], path[MR_PATH_MAX], full[1024];
    const char *slash = strrchr(file, '/');
    char *dot;
    FILE *f;
    int i;

    /* The tracks are converted by hand to SNDH and sit beside the originals
     * as audio/<name>.snd. */
    strcpy(base, slash ? slash + 1 : file);
    dot = strrchr(base, '.');
    if (dot)
        *dot = '\0';
    sprintf(path, "audio/%s.snd", base);

    for (i = 0; i < g_nmus; i++)
        if (!strcmp(g_mus[i], path))
            return i;

    sprintf(full, "%s/%s", g_dir, path);
    f = fopen(full, "rb");
    if (!f)
        err(st, "no SNDH conversion of %s (wanted %s)", file, path);
    else
        fclose(f);
    if (g_nmus == 32)
        mr_fatal("too many music files");
    strcpy(g_mus[g_nmus], path);
    return g_nmus++;
}

/* The rhythm game's beatmaps, read here into what the ST plays (mwb.h,
 * "beatmaps"): RhythmGameDisplayable.read_beatmap_file and __init__
 * (game/00-renpythm/rhythm_game_displayable.rpy:880-900, :375-393), done
 * once at build time instead of per run. */
typedef struct btm {
    char           file[MR_PATH_MAX];   /* game-relative */
    int            stride;
    unsigned long  key;                 /* the high score's, see btm_id */
    unsigned long  end;                 /* the tune's end, ms (snd_ms) */
    unsigned long *note;                /* (track << 30) | ms */
    int            n;
    unsigned char  hit[4];              /* MIDI note a track, see hit_notes */
} btm;

static btm g_btm[32];
static int g_nbtm;

static unsigned long fnv(unsigned long h, const void *p, size_t n);

/* rhythm_game_score_key (.rpy:72): the bare file name. */
static const char *score_key(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* persistent.rhythm_game_highscores[key] (.rpy:96-112), kept in the
 * persistent file under this name's hash, as a variable would be. */
static unsigned long score_hash(const char *path)
{
    char key[MR_PATH_MAX + 64];

    snprintf(key, sizeof(key), "persistent.rhythm_game_highscores[%s]",
             score_key(path));
    return fnv(2166136261UL, key, strlen(key));
}

/* How long a SNDH conversion plays, in ms. The tracks were converted to
 * 4-bit ADPCM, two samples a byte, at the STE's 12.5kHz (the init's
 * `move.b #$81,$ff8921`: mono, 12517Hz, i.e. 8010613/640), and the init
 * stores the data's length with `move.l #len,8(a6)` (2D7C len 0008): all
 * nine tracks come out within 30ms of their .wav. A tune without that
 * falls back on the TIME tag, whole seconds rounded up. */
static unsigned long snd_ms(const mr_stmt *st, const char *snd)
{
    char full[1024];
    unsigned char b[1024];
    size_t n, i;
    FILE *f;

    snprintf(full, sizeof(full), "%s/%s", g_dir, snd);
    f = fopen(full, "rb");
    if (!f)
        return 0;
    n = fread(b, 1, sizeof(b), f);
    fclose(f);
    for (i = 0; i + 8 <= n; i += 2)
        if (b[i] == 0x2D && b[i + 1] == 0x7C && b[i + 6] == 0 &&
            b[i + 7] == 8) {
            unsigned long len = (unsigned long)b[i + 2] << 24 |
                                (unsigned long)b[i + 3] << 16 |
                                (unsigned long)b[i + 4] << 8 | b[i + 5];
            return (unsigned long)(len * 2 * 1000.0 * 640 / 8010613 + 0.5);
        }
    for (i = 0; i + 6 <= n; i++)
        if (!memcmp(b + i, "TIME", 4)) {
            warn(st, "%s: no data length found, ending it at its TIME tag",
                 snd);
            return ((unsigned long)b[i + 4] << 8 | b[i + 5]) * 1000UL;
        }
    err(st, "%s: cannot tell how long it plays", snd);
    return 0;
}

/* The hit sounds' notes. Ren'Py plays one sample a track: the song's own
 * '<base>.hit0.wav'..'hit3.wav' if all four are there, else
 * RHYTHM_HIT_SOUND, do mi sol la (C3 E3 G3 A3) (.rpy:20-27, :89-101,
 * :337-354). The ST plays a note on the YM instead (rhythm.c, "hit
 * sounds"), and this is its pitch.
 *
 * A sample's note is its fundamental, by YIN (de Cheveigne & Kawahara
 * 2002) over 30ms windows from 30% to 70% of the sample, each snapped to a
 * semitone, the most voted kept. The game's are a plucked string whose
 * attack is mostly an upper partial (2 octaves and a fourth up): a window
 * in it reads that or a sub-octave, and the decay reads the fundamental
 * to the cent. Measured, 8 windows of 9 agree on each of the six samples
 * the nine songs use: Bb2 C3 Eb3 F3 G3 Ab3, the samples being one
 * resampled 0, +2, +5, +7, +9 and +10 semitones (their lengths say so to
 * the frame). */
static const char *const g_note_name[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

/* 16-bit PCM, channels averaged, as doubles; NULL if not that. */
static double *wav_read(const char *path, long *n, long *rate)
{
    unsigned char h[8], fmt[16];
    unsigned long len;
    int ch = 0, bits = 0, got_fmt = 0;
    double *x = NULL;
    FILE *f = fopen(path, "rb");

    if (!f)
        return NULL;
    if (fread(h, 1, 8, f) != 8 || memcmp(h, "RIFF", 4) ||
        fread(h, 1, 4, f) != 4 || memcmp(h, "WAVE", 4))
        goto out;
    while (fread(h, 1, 8, f) == 8) {
        len = h[4] | (unsigned long)h[5] << 8 | (unsigned long)h[6] << 16 |
              (unsigned long)h[7] << 24;
        if (!memcmp(h, "fmt ", 4) && len >= 16) {
            if (fread(fmt, 1, 16, f) != 16)
                goto out;
            ch = fmt[2] | fmt[3] << 8;
            *rate = fmt[4] | (long)fmt[5] << 8 | (long)fmt[6] << 16 |
                    (long)fmt[7] << 24;
            bits = fmt[14] | fmt[15] << 8;
            got_fmt = (fmt[0] | fmt[1] << 8) == 1 && bits == 16 && ch > 0;
            fseek(f, (long)(len - 16 + (len & 1)), SEEK_CUR);
        } else if (!memcmp(h, "data", 4) && got_fmt) {
            long i, c;
            *n = (long)(len / (2UL * ch));
            x = malloc((*n ? *n : 1) * sizeof(*x));
            for (i = 0; i < *n; i++) {
                double s = 0;
                for (c = 0; c < ch; c++) {
                    int lo = getc(f), hi = getc(f);
                    s += (short)(lo | hi << 8);
                }
                x[i] = s / ch;
            }
            break;
        } else {
            fseek(f, (long)(len + (len & 1)), SEEK_CUR);
        }
    }
out:
    fclose(f);
    return x;
}

/* YIN's period at x[s..], window w, lags lo..hi: the first dip of the
 * cumulative mean normalized difference under 0.1 (else the least),
 * refined by a parabola. 0 if the window runs off the end. */
static double yin_period(const double *x, long n, long s, long w, long lo,
                         long hi)
{
    double *d, sum = 0, a, b, c, p;
    long t, k;

    if (s + w + hi + 1 > n)
        return 0;
    d = malloc((hi + 2) * sizeof(*d));
    d[0] = 1;
    for (t = 1; t <= hi + 1; t++) {
        double e = 0;
        for (k = 0; k < w; k++) {
            double v = x[s + k] - x[s + k + t];
            e += v * v;
        }
        sum += e;
        d[t] = sum > 0 ? e * t / sum : 1;
    }
    for (t = lo; t < hi && d[t] > 0.1; t++)
        ;
    if (t == hi)
        for (t = lo, k = lo; k < hi; k++)
            if (d[k] < d[t])
                t = k;
    while (t + 1 < hi && d[t + 1] < d[t])
        t++;
    a = d[t - 1];
    b = d[t];
    c = d[t + 1];
    p = a - 2 * b + c != 0 ? t + 0.5 * (a - c) / (a - 2 * b + c) : t;
    free(d);
    return p;
}

/* The MIDI note nearest f, equal temperament at A440; -1 off 0..127. */
static int midi_of(double f)
{
    double fm = 8.175798915643707;      /* note 0 */
    int m;

    for (m = 0; m < 128; m++, fm *= 1.0594630943592953)
        if (f < fm * 1.0293022366434921)    /* half a semitone up */
            return f < fm / 1.0293022366434921 ? -1 : m;
    return -1;
}

/* The sample's note, or -1. */
static int wav_note(const mr_stmt *st, const char *path)
{
    long n = 0, rate = 0, pc;
    int votes[128] = { 0 }, best = -1, m, nwin = 0;
    double *x = wav_read(path, &n, &rate);

    if (!x) {
        warn(st, "%s: not a 16-bit PCM wav", path);
        return -1;
    }
    for (pc = 30; pc <= 70; pc += 5) {
        double p = yin_period(x, n, n * pc / 100, rate * 3 / 100,
                              rate / 1000, rate / 60);
        if (p > 0 && (m = midi_of(rate / p)) >= 0) {
            votes[m]++;
            nwin++;
        }
    }
    free(x);
    for (m = 0; m < 128; m++)
        if (votes[m] && (best < 0 || votes[m] > votes[best]))
            best = m;
    if (best < 0 || votes[best] * 2 <= nwin)
        warn(st, "%s: no clear pitch (%d windows of %d agree)", path,
             best < 0 ? 0 : votes[best], nwin);
    return best;
}

static void hit_notes(const mr_stmt *st, const char *file,
                      unsigned char hit[4])
{
    static const unsigned char fallback[4] = { 48, 52, 55, 57 };
    char base[MR_PATH_MAX], path[1024];
    size_t len = strlen(file), sfx = strlen(".beatmap.txt");
    int k, got = 0;
    FILE *f;

    snprintf(base, sizeof(base), "%.*s", (int)(len > sfx &&
             !strcmp(file + len - sfx, ".beatmap.txt") ? len - sfx : len),
             file);
    for (k = 0; k < 4; k++) {
        snprintf(path, sizeof(path), "%s/%s.hit%d.wav", g_dir, base, k);
        if ((f = fopen(path, "rb")) != NULL) {
            fclose(f);
            got++;
        }
    }
    for (k = 0; k < 4; k++) {
        int m = -1;
        if (got == 4) {
            snprintf(path, sizeof(path), "%s/%s.hit%d.wav", g_dir, base, k);
            m = wav_note(st, path);
        }
        hit[k] = (unsigned char)(m >= 0 ? m : fallback[k]);
    }
    if (!g_quiet)
        printf("%s: hit notes %s%d %s%d %s%d %s%d%s\n", file,
               g_note_name[hit[0] % 12], hit[0] / 12 - 1,
               g_note_name[hit[1] % 12], hit[1] / 12 - 1,
               g_note_name[hit[2] % 12], hit[2] / 12 - 1,
               g_note_name[hit[3] % 12], hit[3] / 12 - 1,
               got == 4 ? "" : ", do mi sol la (no .hit0-3.wav)");
}

static int btm_id(const mr_stmt *st, const char *file, int stride,
                  const char *snd)
{
    char full[1024], line[256];
    double *t = NULL;
    int *tr = NULL, n = 0, cap = 0, ntr = 0, i, k;
    btm *b;
    FILE *f;

    for (i = 0; i < g_nbtm; i++)
        if (!strcmp(g_btm[i].file, file) && g_btm[i].stride == stride)
            return i;
    if (g_nbtm == 32)
        mr_fatal("too many beatmaps");

    snprintf(full, sizeof(full), "%s/%s", g_dir, file);
    f = fopen(full, "r");
    if (!f) {
        err(st, "cannot read the beatmap %s", file);
        return 0;
    }
    /* One onset a line, "seconds" or "seconds<whitespace>track"; blank
     * lines skipped (.rpy:886-893). */
    while (fgets(line, sizeof(line), f)) {
        double s;
        int trk, got = sscanf(line, "%lf %d", &s, &trk);
        if (got < 1)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 1024;
            t = realloc(t, cap * sizeof(*t));
            tr = realloc(tr, cap * sizeof(*tr));
        }
        t[n] = s;
        tr[n] = got == 2 ? ((trk % 4) + 4) % 4 : -1;
        if (got == 2)
            ntr++;
        n++;
    }
    fclose(f);

    b = &g_btm[g_nbtm];
    memset(b, 0, sizeof(*b));
    snprintf(b->file, sizeof(b->file), "%s", file);
    b->stride = stride;
    b->note = malloc(((n + stride - 1) / stride + 1) * sizeof(*b->note));
    /* The old format, onsets only: the tracks from the C version's PRNG,
     * seeded with the djb2 hash of the score key (RhythmGameRandom,
     * .rpy:76-94), drawn once per note kept. Tracks are used only when
     * every onset has one (.rpy:897). */
    {
        unsigned long seed = 5381;
        const unsigned char *p = (const unsigned char *)score_key(file);
        for (; *p; p++)
            seed = (seed * 33 + *p) & 0xFFFFFFFFUL;
        for (i = 0, k = 0; i < n; i += stride, k++) {
            unsigned long ms = (unsigned long)(t[i] * 1000.0 + 0.5);
            int trk = tr[i];
            if (ntr != n) {
                seed = (seed * 1103515245UL + 12345) & 0xFFFFFFFFUL;
                trk = (int)((seed >> 16) % 4);
            }
            if (ms > MWB_BTM_MS_MASK)
                err(st, "%s: an onset past the 30-bit clock", file);
            b->note[k] = ((unsigned long)trk << MWB_BTM_TRACK_SHIFT) |
                         (ms & MWB_BTM_MS_MASK);
        }
        b->n = k;
    }
    free(t);
    free(tr);
    if (!b->n)
        err(st, "the beatmap %s has no onsets", file);
    b->key = score_hash(file);
    b->end = snd_ms(st, snd);
    hit_notes(st, file, b->hit);
    return g_nbtm++;
}

/* The strings: numbered, each with its text in every language, written out
 * as one .MWT per language. A string identical in every language to one
 * already numbered gets that number. */
static const char *const g_lang_name[MWB_NLANG] = { NULL, "english" };
static const char *const g_lang_file[MWB_NLANG] = {
    "TEXT_FR.MWT", "TEXT_EN.MWT"
};

static char        *g_tx[MWB_NLANG];           /* each language's text */
static size_t       g_ntx[MWB_NLANG], g_captx[MWB_NLANG];
static unsigned long *g_soff;                  /* [id * MWB_NLANG + lang] */
static unsigned     g_nsid, g_capsid;
static int          g_lang;                    /* being converted, for errors */
static int          g_untranslated;
static int          g_in_lang;                 /* walking a language branch */

static unsigned str_add(char *const v[MWB_NLANG])
{
    unsigned id;
    int l;

    for (id = 0; id < g_nsid; id++) {
        for (l = 0; l < MWB_NLANG; l++)
            if (strcmp(g_tx[l] + g_soff[id * MWB_NLANG + l], v[l]))
                break;
        if (l == MWB_NLANG)
            return id;
    }
    if (g_nsid == 0xFFFF)
        mr_fatal("too many strings");
    if (g_nsid == g_capsid) {
        g_capsid = g_capsid ? g_capsid * 2 : 1024;
        g_soff = realloc(g_soff, g_capsid * MWB_NLANG * sizeof(*g_soff));
    }
    for (l = 0; l < MWB_NLANG; l++) {
        size_t n = strlen(v[l]) + 1;
        if (g_ntx[l] + n > g_captx[l]) {
            g_captx[l] = (g_captx[l] + n) * 2;
            g_tx[l] = realloc(g_tx[l], g_captx[l]);
        }
        memcpy(g_tx[l] + g_ntx[l], v[l], n);
        g_soff[g_nsid * MWB_NLANG + l] = (unsigned long)g_ntx[l];
        g_ntx[l] += n;
    }
    return g_nsid++;
}

static unsigned char *g_code;
static size_t         g_ncode, g_capcode;

static void w16(unsigned v)
{
    if (g_ncode + 2 > g_capcode) {
        g_capcode = g_capcode ? g_capcode * 2 : 4096;
        g_code = realloc(g_code, g_capcode);
    }
    g_code[g_ncode++] = (unsigned char)(v >> 8);
    g_code[g_ncode++] = (unsigned char)v;
}

static void w32(unsigned long v)
{
    w16((unsigned)(v >> 16) & 0xFFFF);
    w16((unsigned)v & 0xFFFF);
}

static unsigned long g_ncmd[MWB_NCMD];

static void cs_note(unsigned op);

static void cmd(unsigned op)
{
    g_ncmd[op]++;
    cs_note(op);
    w16(op);
}

/* ---- variables and expressions ----------------------------------------- */

static unsigned long fnv(unsigned long h, const void *p, size_t n);

/* A value as the ST holds one: MWB_V_* and a 16-bit number. */
typedef struct val {
    int type;
    int v;
} val;

/* The value strings (mwb.h MWB_V_STR): each literal an expression names,
 * once. `text` is the ST's (Atari charset, inline codes); `src` is the
 * same with its tags already codes but still UTF-8, for a constant
 * printed into a line here. */
#define VSTR_MAX 256
static struct {
    char text[MWB_VSTR_MAX * 2 + 1];
    char src[MWB_VSTR_MAX * 4 + 1];
} g_vstr[VSTR_MAX];
static int g_nvstr;

/* The most characters a value prints: -32768, or the longest string. */
static int g_vwide = 6;

static int strip_tags(const mr_stmt *st, const char *in, char *out,
                      int outsz, int interp);
static int to_atari(const mr_stmt *st, const char *in, char *out, int outsz);
static int is_code(char c);

/* The value string for UTF-8 `s` as the script spells it. Its tags are
 * read as a say's are, since Ren'Py reads them after putting the value
 * in; its brackets are not, as the value is put in only once. -1 on an
 * error, reported. */
static int vstr_add(const mr_stmt *st, const char *s)
{
    char src[MWB_VSTR_MAX * 4 + 1], text[MWB_VSTR_MAX * 2 + 1];
    int i, w = 0;

    if (!strip_tags(st, s, src, sizeof(src), 0) ||
        !to_atari(st, src, text, sizeof(text)))
        return -1;
    for (i = 0; text[i]; i++)
        if (!is_code(text[i]))
            w++;
    if (w > MWB_VSTR_MAX) {
        err(st, "a string of %d characters, the ST holds %d", w,
            MWB_VSTR_MAX);
        return -1;
    }
    for (i = 0; i < g_nvstr; i++)
        if (!strcmp(g_vstr[i].text, text))
            return i;
    if (g_nvstr == VSTR_MAX)
        mr_fatal("more than %d strings in expressions", VSTR_MAX);
    strcpy(g_vstr[g_nvstr].text, text);
    strcpy(g_vstr[g_nvstr].src, src);
    if (w > g_vwide)
        g_vwide = w;
    return g_nvstr++;
}

static int v_truthy(val a)
{
    if (a.type == MWB_V_STR)
        return g_vstr[a.v].text[0] != '\0';
    return a.type != MWB_V_NONE && a.v != 0;
}

/* Python's operators on these values. A bool is an int (True == 1), None
 * equals only None and a string only the same string, and ordering None
 * against anything or a string against a non-string raises -- *bad is
 * set, and the result is False. Strings order by their bytes. */
static val v_op(int op, val a, val b, int *bad)
{
    val r;
    int none = a.type == MWB_V_NONE || b.type == MWB_V_NONE;
    int str = a.type == MWB_V_STR || b.type == MWB_V_STR;
    int c;

    r.type = MWB_V_BOOL;
    r.v = 0;
    switch (op) {
    case MWB_EX_AND:
        return v_truthy(a) ? b : a;
    case MWB_EX_OR:
        return v_truthy(a) ? a : b;
    case MWB_EX_EQ:
    case MWB_EX_NE:
        r.v = none || str ? a.type == b.type && a.v == b.v : a.v == b.v;
        if (op == MWB_EX_NE)
            r.v = !r.v;
        return r;
    }
    if (none || (str && a.type != b.type)) {
        *bad = 1;
        return r;
    }
    if (str) {
        c = strcmp(g_vstr[a.v].text, g_vstr[b.v].text);
        a.v = c;
        b.v = 0;
    }
    switch (op) {
    case MWB_EX_LT: r.v = a.v < b.v; break;
    case MWB_EX_LE: r.v = a.v <= b.v; break;
    case MWB_EX_GT: r.v = a.v > b.v; break;
    case MWB_EX_GE: r.v = a.v >= b.v; break;
    }
    return r;
}

/* The variables: every name a `$` line assigns, and every `persistent.`
 * name. Numbered in the order met. */
typedef struct var {
    char          name[MR_MAX_NAME];
    int           persistent;
    val           init;
    unsigned long key;              /* the file's name for it; 0: the name's */
} var;

static var g_var[MWB_TXT_MAXVAR];
static int g_nvar;

/* Names a `$` line may assign that are not story variables: the engine's
 * own, which do_python translates or ignores. */
static int engine_name(const char *n)
{
    static const char *const prefix[] = {
        "_", "config.", "preferences.", "gui.", "renpy.", "build.", NULL
    };
    int i;

    if (!strcmp(n, "title") || !strcmp(n, "quick_menu"))
        return 1;
    for (i = 0; prefix[i]; i++)
        if (!strncmp(n, prefix[i], strlen(prefix[i])))
            return 1;
    return 0;
}

/* rhythm_game_get_highscore(path): read as the variable named
 * "rhythm_game_get_highscore:<file name>", see ex_atom. */
#define HS_FUNC "rhythm_game_get_highscore"

static int var_find(const char *name)
{
    int i;
    for (i = 0; i < g_nvar; i++)
        if (!strcmp(g_var[i].name, name))
            return i;
    return -1;
}

/* `name = rhs`, with name a dotted identifier: the target and where the
 * right-hand side starts. Not `==`, `+=`, a tuple or a call. */
static int assign_split(const char *t, char *name, const char **rhs)
{
    int n = 0;

    while (*t == ' ')
        t++;
    if (!isalpha((unsigned char)*t) && *t != '_')
        return 0;
    while ((isalnum((unsigned char)*t) || *t == '_' || *t == '.') &&
           n < MR_MAX_NAME - 1)
        name[n++] = *t++;
    name[n] = '\0';
    while (*t == ' ')
        t++;
    if (t[0] != '=' || t[1] == '=')
        return 0;
    t++;
    while (*t == ' ')
        t++;
    *rhs = t;
    return *t != '\0';
}

/* The INIT statement (`define` or `default`) that sets `name`, or NULL. */
static const mr_stmt *init_of(const char *name, const char **rhs)
{
    char n[MR_MAX_NAME];
    int i;

    for (i = 0; i < g_s->n; i++) {
        const mr_stmt *st = &g_s->st[i];
        if (st->kind == MR_ST_INIT && st->text &&
            assign_split(st->text, n, rhs) && !strcmp(n, name))
            return st;
    }
    return NULL;
}

static int compile_expr(const mr_stmt *st, const char *src, unsigned *off,
                        val *c);

static int var_add(const mr_stmt *st, const char *name)
{
    const mr_stmt *in;
    const char *rhs;
    unsigned off;
    int i = var_find(name);

    if (i >= 0)
        return i;
    if (g_nvar == MWB_TXT_MAXVAR)
        mr_fatal("more than %d variables", MWB_TXT_MAXVAR);
    i = g_nvar++;
    memset(&g_var[i], 0, sizeof(g_var[i]));
    strcpy(g_var[i].name, name);
    g_var[i].persistent = !strncmp(name, "persistent.", 11);
    g_var[i].init.type = MWB_V_NONE;
    /* `default x = ...`: the value at the start of a game, which is the
     * value of an unset `persistent.` field too (None). Anything else
     * starts as None, where Ren'Py would raise on reading it. */
    in = init_of(name, &rhs);
    if (in && compile_expr(in, rhs, &off, &g_var[i].init) != 0)
        err(in ? in : st, "the initial value of %s is not a constant", name);
    return i;
}

/* Constants: names a `define` gives a literal and nothing assigns. The
 * path check gives them to the reference evaluator too. */
static char g_const[64][MR_MAX_NAME];
static int  g_nconst;

/* The expression table, and the expression being built. */
static unsigned short *g_expr;
static size_t          g_nexpr, g_capexpr;
static unsigned short  g_eb[256];
static int             g_neb;

typedef struct exparse {
    const mr_stmt *st;
    const char    *p;
    int            bad;
} exparse;

/* An operand: its value if the compiler knows it, and where its words
 * start in g_eb. */
typedef struct opd {
    int known;
    val c;
    int start;
} opd;

static void eb(exparse *e, unsigned w)
{
    if (g_neb == (int)(sizeof(g_eb) / sizeof(g_eb[0]))) {
        e->bad = 1;
        return;
    }
    g_eb[g_neb++] = (unsigned short)w;
}

static opd ex_const(exparse *e, int start, val c)
{
    opd r;

    g_neb = start;
    if (c.type == MWB_V_NONE) {
        eb(e, MWB_EX_NONE);
    } else if (c.type == MWB_V_BOOL) {
        eb(e, c.v ? MWB_EX_TRUE : MWB_EX_FALSE);
    } else if (c.type == MWB_V_STR) {
        eb(e, MWB_EX_STR);
        eb(e, (unsigned)c.v);
    } else if (c.v >= -32768 && c.v <= 32767) {
        eb(e, MWB_EX_INT);
        eb(e, (unsigned)c.v & 0xFFFF);
    } else {
        eb(e, MWB_EX_LONG);
        eb(e, ((unsigned long)c.v >> 16) & 0xFFFF);
        eb(e, (unsigned)c.v & 0xFFFF);
    }
    r.known = 1;
    r.c = c;
    r.start = start;
    return r;
}

static opd ex_binary(exparse *e, int op, opd a, opd b)
{
    if (a.known && b.known) {
        int bad = 0;
        val r = v_op(op, a.c, b.c, &bad);
        if (bad) {
            err(e->st, "ordering None, or a string against another type, "
                "in `%s`", e->st->text);
            e->bad = 1;
        }
        return ex_const(e, a.start, r);
    }
    eb(e, (unsigned)op);
    a.known = 0;
    return a;
}

static void ex_ws(exparse *e)
{
    while (*e->p == ' ' || *e->p == '\t')
        e->p++;
}

/* The keyword `w` next, as a whole word? Consumed if so. */
static int ex_word(exparse *e, const char *w)
{
    size_t n = strlen(w);
    ex_ws(e);
    if (strncmp(e->p, w, n) || isalnum((unsigned char)e->p[n]) ||
        e->p[n] == '_' || e->p[n] == '.')
        return 0;
    e->p += n;
    return 1;
}

static opd ex_or(exparse *e);

static opd ex_atom(exparse *e)
{
    opd r;
    val c;
    char name[MR_MAX_NAME];
    int n = 0, start = g_neb;

    r.known = 0;
    r.start = start;
    c.type = MWB_V_NONE;
    c.v = 0;
    ex_ws(e);
    if (*e->p == '(') {
        e->p++;
        r = ex_or(e);
        ex_ws(e);
        if (*e->p != ')')
            e->bad = 1;
        else
            e->p++;
        return r;
    }
    if (isdigit((unsigned char)*e->p) ||
        (*e->p == '-' && isdigit((unsigned char)e->p[1]))) {
        long v = strtol(e->p, (char **)&e->p, 10);
        if (v < -2147483647L - 1 || v > 2147483647L) {
            err(e->st, "%ld does not fit the ST's 32 bits", v);
            e->bad = 1;
        }
        c.type = MWB_V_INT;
        c.v = (int)v;
        return ex_const(e, start, c);
    }
    if (*e->p == '"' || *e->p == '\'') {
        /* A string literal: Python's escapes for quotes, backslash and
         * line break; no prefixes or triple quotes. */
        char s[MWB_VSTR_MAX * 4 + 1], q = *e->p++;
        int k = 0, id;

        for (; *e->p && *e->p != q; e->p++) {
            char ch = *e->p;
            if (ch == '\\') {
                ch = *++e->p;
                if (ch == 'n')
                    ch = '\n';
                else if (ch != '\\' && ch != '"' && ch != '\'') {
                    err(e->st, "the escape \\%c is not supported in `%s`",
                        ch ? ch : ' ', e->st->text);
                    e->bad = 1;
                    return r;
                }
            }
            if (k == (int)sizeof(s) - 1) {
                err(e->st, "a string longer than %d characters in `%s`",
                    MWB_VSTR_MAX, e->st->text);
                e->bad = 1;
                return r;
            }
            s[k++] = ch;
        }
        if (*e->p != q) {
            e->bad = 1;
            return r;
        }
        e->p++;
        s[k] = '\0';
        id = vstr_add(e->st, s);
        if (id < 0) {
            e->bad = 1;
            return r;
        }
        c.type = MWB_V_STR;
        c.v = id;
        return ex_const(e, start, c);
    }
    if (!isalpha((unsigned char)*e->p) && *e->p != '_') {
        e->bad = 1;
        return r;
    }
    while ((isalnum((unsigned char)*e->p) || *e->p == '_' || *e->p == '.') &&
           n < MR_MAX_NAME - 1)
        name[n++] = *e->p++;
    name[n] = '\0';
    ex_ws(e);
    if (*e->p == '(' && !strcmp(name, HS_FUNC)) {
        /* A song's high score (rhythm_game_displayable.rpy:103-108): the
         * persistent entry the game keeps it in, 0 until it is played. */
        char path[MR_PATH_MAX], q;
        int k = 0, i;

        e->p++;
        ex_ws(e);
        q = *e->p;
        if (q == '"' || q == '\'')
            for (e->p++; *e->p && *e->p != q && k < MR_PATH_MAX - 1; e->p++)
                path[k++] = *e->p;
        path[k] = '\0';
        if ((q != '"' && q != '\'') || *e->p != q) {
            err(e->st, "%s() takes a beatmap's path, a plain string", name);
            e->bad = 1;
            return r;
        }
        e->p++;
        ex_ws(e);
        if (*e->p != ')') {
            err(e->st, "%s() takes a beatmap's path, a plain string", name);
            e->bad = 1;
            return r;
        }
        e->p++;
        if (snprintf(name, sizeof(name), HS_FUNC ":%s", score_key(path)) >=
            (int)sizeof(name))
            err(e->st, "%s: a file name too long", path);
        if ((i = var_find(name)) < 0) {
            i = var_add(e->st, name);
            g_var[i].persistent = 1;
            g_var[i].key = score_hash(path);
            g_var[i].init.type = MWB_V_INT;
            g_var[i].init.v = 0;
        }
        eb(e, MWB_EX_VAR);
        eb(e, (unsigned)i);
        return r;
    }
    if (*e->p == '(' && !strcmp(name, "preferences.get_mute")) {
        /* No mixer on the ST, so nothing muted: False, as Ren'Py's is for
         * a mixer it has not muted (preferences.py:377-381). */
        int depth = 0;
        do {
            if (*e->p == '(')
                depth++;
            else if (*e->p == ')')
                depth--;
            else if (!*e->p) {
                e->bad = 1;
                return r;
            }
            e->p++;
        } while (depth);
        c.type = MWB_V_BOOL;
        c.v = 0;
        return ex_const(e, start, c);
    }
    if (*e->p == '(') {
        err(e->st, "`%s(...)` cannot be evaluated on the ST", name);
        e->bad = 1;
        return r;
    }
    if (!strcmp(name, "True") || !strcmp(name, "False")) {
        c.type = MWB_V_BOOL;
        c.v = name[0] == 'T';
        return ex_const(e, start, c);
    }
    if (!strcmp(name, "None"))
        return ex_const(e, start, c);
    if (!strcmp(name, "main_menu")) {
        /* True only under the main menu (00start.rpy:322); the ST has none,
         * the story is all it runs (defaultstore.py:72). */
        c.type = MWB_V_BOOL;
        c.v = 0;
        return ex_const(e, start, c);
    }
    if (var_find(name) >= 0 || !strncmp(name, "persistent.", 11)) {
        eb(e, MWB_EX_VAR);
        eb(e, (unsigned)var_add(e->st, name));
        return r;
    }
    {
        /* A `define`d constant. */
        const char *rhs;
        const mr_stmt *in = init_of(name, &rhs);
        unsigned off;
        int i;

        if (in && compile_expr(in, rhs, &off, &c) == 0) {
            for (i = 0; i < g_nconst; i++)
                if (!strcmp(g_const[i], name))
                    break;
            if (i == g_nconst && g_nconst < 64)
                strcpy(g_const[g_nconst++], name);
            g_neb = start;
            return ex_const(e, start, c);
        }
    }
    err(e->st, "`%s` is not a variable or a constant", name);
    e->bad = 1;
    return r;
}

static opd ex_cmp(exparse *e)
{
    static const struct { const char *s; int op; } ops[] = {
        { "==", MWB_EX_EQ }, { "!=", MWB_EX_NE }, { "<=", MWB_EX_LE },
        { ">=", MWB_EX_GE }, { "<", MWB_EX_LT }, { ">", MWB_EX_GT }
    };
    opd a = ex_atom(e);
    int i, cmps = 0;

    for (;;) {
        ex_ws(e);
        for (i = 0; i < 6; i++)
            if (!strncmp(e->p, ops[i].s, strlen(ops[i].s)))
                break;
        if (i == 6)
            break;
        if (cmps++) {
            err(e->st, "chained comparisons are not supported");
            e->bad = 1;
        }
        e->p += strlen(ops[i].s);
        a = ex_binary(e, ops[i].op, a, ex_atom(e));
    }
    return a;
}

static opd ex_not(exparse *e)
{
    if (ex_word(e, "not")) {
        int start = g_neb;
        opd a = ex_not(e);
        if (a.known) {
            val c;
            c.type = MWB_V_BOOL;
            c.v = !v_truthy(a.c);
            return ex_const(e, start, c);
        }
        eb(e, MWB_EX_NOT);
        return a;
    }
    return ex_cmp(e);
}

static opd ex_and(exparse *e)
{
    opd a = ex_not(e);
    while (ex_word(e, "and"))
        a = ex_binary(e, MWB_EX_AND, a, ex_not(e));
    return a;
}

static opd ex_or(exparse *e)
{
    opd a = ex_and(e);
    while (ex_word(e, "or"))
        a = ex_binary(e, MWB_EX_OR, a, ex_and(e));
    return a;
}

/* A word sequence into the table, sharing an identical one. */
static unsigned expr_store(const unsigned short *w, int n)
{
    size_t i, at;

    for (i = 0; i + (size_t)n <= g_nexpr; i++)
        if ((i == 0 || g_expr[i - 1] == MWB_EX_END) &&
            !memcmp(&g_expr[i], w, n * sizeof(*w)))
            return (unsigned)i;
    if (g_nexpr + n > g_capexpr) {
        g_capexpr = (g_capexpr + n) * 2;
        g_expr = realloc(g_expr, g_capexpr * sizeof(*g_expr));
    }
    at = g_nexpr;
    memcpy(&g_expr[at], w, n * sizeof(*w));
    g_nexpr += n;
    return (unsigned)at;
}

/* Compile `src`. 0: the compiler knows its value (*c). 1: it needs the
 * ST, at *off in the expression table. -1: an error, reported. */
static int compile_expr(const mr_stmt *st, const char *src, unsigned *off,
                        val *c)
{
    exparse e;
    opd r;
    /* Re-entered when a name's `default` is compiled halfway through
     * another expression: that one's words are put back after. */
    unsigned short outer[256], w[256];
    int save = g_neb, n;

    memcpy(outer, g_eb, save * sizeof(outer[0]));
    e.st = st;
    e.p = src;
    e.bad = 0;
    g_neb = 0;
    r = ex_or(&e);
    ex_ws(&e);
    if (*e.p && !e.bad) {
        err(st, "cannot read `%s`", src);
        e.bad = 1;
    }
    n = g_neb;
    memcpy(w, g_eb, n * sizeof(w[0]));
    memcpy(g_eb, outer, save * sizeof(outer[0]));
    g_neb = save;
    if (e.bad)
        return -1;
    if (r.known) {
        *c = r.c;
        return 0;
    }
    w[n++] = MWB_EX_END;
    *off = expr_store(w, n);
    return 1;
}

/* An expression that is just a value, for SET. */
static unsigned expr_value(val c)
{
    unsigned short w[4];
    int n = 0;

    if (c.type == MWB_V_NONE)
        w[n++] = MWB_EX_NONE;
    else if (c.type == MWB_V_BOOL)
        w[n++] = c.v ? MWB_EX_TRUE : MWB_EX_FALSE;
    else if (c.type == MWB_V_STR) {
        w[n++] = MWB_EX_STR;
        w[n++] = (unsigned short)c.v;
    } else if (c.v >= -32768 && c.v <= 32767) {
        w[n++] = MWB_EX_INT;
        w[n++] = (unsigned short)(c.v & 0xFFFF);
    } else {
        w[n++] = MWB_EX_LONG;
        w[n++] = (unsigned short)(((unsigned long)c.v >> 16) & 0xFFFF);
        w[n++] = (unsigned short)(c.v & 0xFFFF);
    }
    w[n++] = MWB_EX_END;
    return expr_store(w, n);
}

/* Evaluate table expression `off` against `vars`: what the ST will do. */
static val expr_eval(unsigned off, const val *vars)
{
    val st[32];
    int sp = 0, bad = 0;
    const unsigned short *w = g_expr + off;

    for (;;) {
        val r;
        r.type = MWB_V_NONE;
        r.v = 0;
        switch (*w++) {
        case MWB_EX_END:
            return sp ? st[sp - 1] : r;
        case MWB_EX_NONE:
            st[sp++] = r;
            break;
        case MWB_EX_FALSE:
        case MWB_EX_TRUE:
            r.type = MWB_V_BOOL;
            r.v = w[-1] == MWB_EX_TRUE;
            st[sp++] = r;
            break;
        case MWB_EX_INT:
            r.type = MWB_V_INT;
            r.v = (short)*w++;
            st[sp++] = r;
            break;
        case MWB_EX_LONG:
            r.type = MWB_V_INT;
            r.v = (int)(((unsigned long)w[0] << 16) | w[1]);
            w += 2;
            st[sp++] = r;
            break;
        case MWB_EX_STR:
            r.type = MWB_V_STR;
            r.v = *w++;
            st[sp++] = r;
            break;
        case MWB_EX_VAR:
            st[sp++] = vars[*w++];
            break;
        case MWB_EX_RET:
            /* The rhythm game's results are the player's: the path check
             * takes every one as 0, the source walk too (orun). */
            r.type = MWB_V_INT;
            w++;
            st[sp++] = r;
            break;
        case MWB_EX_NOT:
            r.type = MWB_V_BOOL;
            r.v = !v_truthy(st[sp - 1]);
            st[sp - 1] = r;
            break;
        default:
            st[sp - 2] = v_op(w[-1], st[sp - 2], st[sp - 1], &bad);
            sp--;
            break;
        }
    }
}

/* ---- text -------------------------------------------------------------- */

/* A no-break space, while wrapping: a glyph the wrapper will not break at.
 * It becomes an ordinary space once the lines are settled. */
#define NBSP 0x1F

/* UTF-8 to the Atari ST character set, which has every accented letter the
 * French script uses. Anything else stops the build, except what has no
 * Atari glyph and a plain stand-in: ō and ī (Japanese names), and the
 * curly quotes of the English translation. */
static const struct { unsigned cp; unsigned char st; } g_charmap[] = {
    { 0xC7, 0x80 }, { 0xFC, 0x81 }, { 0xE9, 0x82 }, { 0xE2, 0x83 },
    { 0xE4, 0x84 }, { 0xE0, 0x85 }, { 0xE5, 0x86 }, { 0xE7, 0x87 },
    { 0xEA, 0x88 }, { 0xEB, 0x89 }, { 0xE8, 0x8A }, { 0xEF, 0x8B },
    { 0xEE, 0x8C }, { 0xEC, 0x8D }, { 0xC4, 0x8E }, { 0xC5, 0x8F },
    { 0xC9, 0x90 }, { 0xE6, 0x91 }, { 0xC6, 0x92 }, { 0xF4, 0x93 },
    { 0xF6, 0x94 }, { 0xF2, 0x95 }, { 0xFB, 0x96 }, { 0xF9, 0x97 },
    { 0xFF, 0x98 }, { 0xD6, 0x99 }, { 0xDC, 0x9A }, { 0x153, 0xB4 },
    { 0x152, 0xB5 }, { 0xC0, 0xB6 }, { 0x14D, 'o' }, { 0x12B, 'i' },
    { 0x2018, '\'' }, { 0x2019, '\'' }, { 0x201C, '"' }, { 0x201D, '"' },
    { 0xAB, 0xAE }, { 0xBB, 0xAF }, { 0xA0, NBSP }
};

static int to_atari(const mr_stmt *st, const char *in, char *out, int outsz)
{
    const unsigned char *p = (const unsigned char *)in;
    int n = 0;

    while (*p) {
        unsigned cp;
        int extra, i;

        if (*p < 0x80) {
            cp = *p++;
            extra = -1;
        } else if ((*p & 0xE0) == 0xC0) {
            cp = *p++ & 0x1F; extra = 1;
        } else if ((*p & 0xF0) == 0xE0) {
            cp = *p++ & 0x0F; extra = 2;
        } else {
            cp = *p++ & 0x07; extra = 3;
        }
        for (i = 0; i < extra; i++) {
            if ((*p & 0xC0) != 0x80) {
                err(st, "bad UTF-8");
                return 0;
            }
            cp = (cp << 6) | (*p++ & 0x3F);
        }
        if (cp == 0xFEFF)
            continue;
        if (extra >= 0) {
            unsigned c = 0;
            for (i = 0; i < (int)(sizeof(g_charmap) / sizeof(g_charmap[0])); i++)
                if (g_charmap[i].cp == cp)
                    c = g_charmap[i].st;
            if (!c) {
                err(st, "no Atari glyph for U+%04X", cp);
                c = '?';
            }
            cp = c;
        }
        if (n >= outsz - 1) {
            err(st, "text too long");
            return 0;
        }
        out[n++] = (char)cp;
    }
    out[n] = '\0';
    return 1;
}

/* Ren'Py text tags to the runtime's inline codes: italic, bold and {fast};
 * any other tag stops the build. With `interp`,
 * `[name]` is interpolated too (a line); without, brackets are text (a
 * value, which goes in as it is). */
static int strip_tags(const mr_stmt *st, const char *in, char *out,
                      int outsz, int interp)
{
    int n = 0;

    while (*in) {
        if (!interp && *in == '[') {
            out[n++] = *in++;
            continue;
        }
        if (in[0] == '{' && in[1] == '{') {
            out[n++] = '{';
            in += 2;
        } else if (*in == '{') {
            const char *e = strchr(in, '}');
            char tag[16];
            int l;
            if (!e || e - in - 1 >= (int)sizeof(tag)) {
                err(st, "bad text tag");
                return 0;
            }
            l = (int)(e - in - 1);
            memcpy(tag, in + 1, l);
            tag[l] = '\0';
            if (!strcmp(tag, "i"))       out[n++] = MWB_TXT_ITALIC_ON;
            else if (!strcmp(tag, "/i")) out[n++] = MWB_TXT_ITALIC_OFF;
            else if (!strcmp(tag, "b"))  out[n++] = MWB_TXT_BOLD_ON;
            else if (!strcmp(tag, "/b")) out[n++] = MWB_TXT_BOLD_OFF;
            /* typing starts here (character.py:163) */
            else if (!strcmp(tag, "fast")) out[n++] = MWB_TXT_FAST;
            else err(st, "text tag {%s} is not supported", tag);
            in = e + 1;
        } else if (in[0] == '[' && in[1] == '[') {
            out[n++] = '[';
            in += 2;
        } else if (*in == '[') {
            /* `[name]`: a variable is left to the ST, which prints it as
             * Ren'Py's format(value, "") does (substitutions.py:100); a
             * constant is printed here the same way. */
            const char *e = strchr(in, ']');
            char name[MR_MAX_NAME];
            unsigned off;
            val c;
            int l;

            if (!e || e - in - 1 >= (int)sizeof(name)) {
                err(st, "bad interpolation");
                return 0;
            }
            l = (int)(e - in - 1);
            memcpy(name, in + 1, l);
            name[l] = '\0';
            if (strchr(name, '!') || strchr(name, ':')) {
                err(st, "[%s]: conversions and formats are not supported",
                    name);
                return 0;
            }
            if (var_find(name) >= 0 || !strncmp(name, "persistent.", 11)) {
                out[n++] = MWB_TXT_VAR;
                out[n++] = (char)(MWB_TXT_VAR_BASE + var_add(st, name));
            } else if (compile_expr(st, name, &off, &c) == 0) {
                if (c.type == MWB_V_NONE)
                    n += sprintf(out + n, "None");
                else if (c.type == MWB_V_BOOL)
                    n += sprintf(out + n, c.v ? "True" : "False");
                else if (c.type == MWB_V_STR) {
                    if (n + strlen(g_vstr[c.v].src) >= (size_t)outsz) {
                        err(st, "text too long");
                        return 0;
                    }
                    n += sprintf(out + n, "%s", g_vstr[c.v].src);
                } else
                    n += sprintf(out + n, "%d", c.v);
            } else {
                return 0;
            }
            in = e + 1;
        } else {
            out[n++] = *in++;
        }
        if (n >= outsz - 1) {
            err(st, "text too long");
            return 0;
        }
    }
    out[n] = '\0';
    return 1;
}

static int is_code(char c)
{
    return c >= MWB_TXT_ITALIC_ON && c <= MWB_TXT_CODE_LAST;
}

/* Greedy word wrap at `cols` visible characters. Returns the line count. */
static int wrap(char *s, int cols)
{
    int lines = 1, col = 0, lastsp = -1, i;

    for (i = 0; s[i]; i++) {
        if (is_code(s[i]))
            continue;
        if (s[i] == '\n') {
            lines++;
            col = 0;
            lastsp = -1;
            continue;
        }
        if (s[i] == ' ')
            lastsp = i;
        if (++col > cols) {
            if (lastsp < 0)
                return -1;          /* a word longer than a line */
            s[lastsp] = '\n';
            lines++;
            /* Recount from the break. */
            col = 0;
            for (lastsp++; lastsp <= i; lastsp++)
                if (!is_code(s[lastsp]))
                    col++;
            lastsp = -1;
        }
    }
    return lines;
}

/* Text that shows a value: only the ST knows how wide the value is, so it
 * wraps a say line itself. Each has to fit with its values at the widest
 * one prints, which is known once every string is (g_vwide): they are
 * checked at the end. `rows` is the box's for a say, 0 for an option. */
typedef struct wideline {
    const mr_stmt *st;
    char          *text;
    int            rows, lang;
} wideline;
static wideline *g_wl;
static int       g_nwl, g_capwl;

static void wide_add(const mr_stmt *st, const char *text, int rows, int lang)
{
    if (g_quiet)
        return;
    if (g_nwl == g_capwl) {
        g_capwl = g_capwl ? g_capwl * 2 : 64;
        g_wl = realloc(g_wl, g_capwl * sizeof(*g_wl));
    }
    g_wl[g_nwl].st = st;
    g_wl[g_nwl].text = strdup(text);
    g_wl[g_nwl].rows = rows;
    g_wl[g_nwl].lang = lang;
    g_nwl++;
}

static void wide_check(void)
{
    char w[MR_MAX_TEXT * 4];
    int j;

    for (j = 0; j < g_nwl; j++) {
        const wideline *l = &g_wl[j];
        const char *lang = g_lang_name[l->lang] ? g_lang_name[l->lang]
                                                : "French";
        int i, k = 0, lines, cols = 0;

        for (i = 0; l->text[i] && k < (int)sizeof(w) - g_vwide - 1; i++) {
            if (l->text[i] == MWB_TXT_VAR && l->text[i + 1]) {
                memset(w + k, 'x', g_vwide);
                k += g_vwide;
                i++;
            } else {
                w[k++] = l->text[i];
            }
        }
        w[k] = '\0';
        if (l->rows) {
            lines = wrap(w, MWB_TEXT_COLS);
            if (lines < 0 || lines > l->rows)
                err(l->st, "%s text needs %d lines with its values at their "
                    "widest (%d characters), the box has %d", lang, lines,
                    g_vwide, l->rows);
            continue;
        }
        for (i = 0; w[i]; i++)
            if (!is_code(w[i]))
                cols++;
        if (cols > MWB_OPT_COLS)
            err(l->st, "%s option is %d characters with its values at their "
                "widest (%d), a button holds %d", lang, cols, g_vwide,
                MWB_OPT_COLS);
    }
}

/* One language's text: tags to codes, the Atari charset, wrapped to `rows`
 * lines of the box when it is say text. */
static void text_conv(const mr_stmt *st, const char *utf8, int rows,
                      char *out, int outsz)
{
    char a[MR_MAX_TEXT * 2];
    char *p;
    int lines;

    out[0] = '\0';
    if (!strip_tags(st, utf8, a, sizeof(a), 1) ||
        !to_atari(st, a, out, outsz))
        return;
    if (rows && strchr(out, MWB_TXT_VAR)) {
        wide_add(st, out, rows, g_lang);
    } else if (rows) {
        lines = wrap(out, MWB_TEXT_COLS);
        if (lines < 0 || lines > rows)
            err(st, "%s text needs %d lines, the box has %d",
                g_lang ? g_lang_name[g_lang] : "French", lines, rows);
    }
    for (p = out; *p; p++)
        if (*p == NBSP)
            *p = ' ';
}

/* A string in every language: `utf8[l]` is language l's text. */
static unsigned text_str(const mr_stmt *st, const char *const utf8[MWB_NLANG],
                         int rows)
{
    static char b[MWB_NLANG][MR_MAX_TEXT * 2];
    char *v[MWB_NLANG];

    for (g_lang = 0; g_lang < MWB_NLANG; g_lang++) {
        text_conv(st, utf8[g_lang], rows, b[g_lang], sizeof(b[0]));
        v[g_lang] = b[g_lang];
    }
    g_lang = 0;
    return str_add(v);
}

/* A `_()` string or a `!t` interpolation, as Ren'Py's string translation
 * has it: by the original text, falling back to it. The catalogue loaded
 * is the English one; the French is the original. */
static unsigned tl_str(const mr_stmt *st, const char *orig, int rows)
{
    const char *v[MWB_NLANG];

    v[MWB_LANG_FR] = orig;
    v[MWB_LANG_EN] = mr_tl_string(orig);
    return text_str(st, v, rows);
}

/* ---- source lines ------------------------------------------------------ */

/* The text of a statement's source line. Needed only where the parser keeps
 * less than the ST wants: Fade's three times, which mr_transition sums. */
static int source_line(const mr_stmt *st, char *out, int outsz)
{
    char path[1024];
    FILE *f;
    int n = 0;

    sprintf(path, "%s/%s", g_dir, g_s->files[st->file]);
    f = fopen(path, "r");
    if (!f)
        return 0;
    while (fgets(out, outsz, f))
        if (++n == st->line) {
            fclose(f);
            return 1;
        }
    fclose(f);
    return 0;
}

/* ---- the walk ---------------------------------------------------------- */

/* The scene list as the runtime will hold it, so that shows can be resolved
 * against what the tag is already wearing and where it already is. */
typedef struct slot {
    int      tag;
    int      reg;
    mr_place place;
    int      has_place;
    int      img, x, y;             /* what the ST was told: its image and
                                     * half-scale position */
} slot;

/* Everything the walk knows at a point: the ST's display state (vm.c's
 * vstate, field for field), and what the compiler needs to settle what
 * comes next. What follows a statement is a function of the statement and
 * this, which is what lets paths that arrive with the same one share code.
 * Compared as bytes, so it is only ever copied whole and its strings are
 * zero-filled: a stray byte can only cost a needless copy of some code,
 * never a wrong merge. */
#define CALL_DEPTH 8                /* calls inside calls */

typedef struct cstate {
    slot     slot[16];
    int      nslot;
    int      bg;                    /* image id, or -1 for black */
    unsigned title_str;             /* the card shown, or 0xFFFF */
    unsigned chapter_str;           /* the last card shown, or 0xFFFF */
    int      music;                 /* track id, or -1 */
    int      window;
    int      skip_ok;
    int      rollback_ok;
    int      title_up;              /* the title screen is up */
    char     title[MR_MAX_TEXT];    /* the `title` variable */
    /* The compiler's alone: */
    int      bg_reg;                /* the backdrop's image, or -1; its tag
                                     * is the name's first word, numbered
                                     * only if it becomes a slot */
    int      dirty;                 /* the stage changed since the last
                                     * interaction (the transition baseline) */
    /* `call`: where each `return` goes back to, innermost last, entries
     * past nret 0. Part of the state, so a label called from two places
     * is compiled once for each: what follows its `return` differs. */
    int      ret[CALL_DEPTH];
    int      nret;
} cstate;

static cstate g_cs;

/* vm.c state_reset. */
static void cs_reset(cstate *cs)
{
    memset(cs, 0, sizeof(*cs));
    cs->bg = -1;
    cs->bg_reg = -1;
    cs->title_str = cs->chapter_str = 0xFFFF;
    cs->music = -1;
    cs->window = 1;
    cs->skip_ok = 1;
    cs->rollback_ok = 1;
}

/* What a command does to the baseline: an interaction (vm.c frame_n,
 * choice_frame, with_none, transition) makes the stage it shows the
 * baseline; a stage change makes the stage differ from it. */
static void cs_note(unsigned op)
{
    switch (op) {
    case MWB_SAY: case MWB_PAUSE: case MWB_WITH: case MWB_MENU:
    case MWB_RHYTHM:                /* it ends with a `with None` (vm.c) */
        g_cs.dirty = 0;
        break;
    case MWB_SCENE: case MWB_SHOW: case MWB_SCROLL: case MWB_HIDE:
    case MWB_TITLE: case MWB_UNTITLE:
        g_cs.dirty = 1;
        break;
    }
}

static int find_slot(int tag)
{
    int i;
    for (i = 0; i < g_cs.nslot; i++)
        if (g_cs.slot[i].tag == tag)
            return i;
    return -1;
}

/* scene.c:384 in_taglist: is `tag` a whole entry of the comma list? */
static int in_taglist(const char *list, const char *tag)
{
    int n = (int)strlen(tag);

    if (!list)
        return 0;
    while (*list) {
        const char *e = list;
        while (*e && *e != ',')
            e++;
        if (e - list == n && !strncmp(list, tag, (size_t)n))
            return 1;
        list = *e ? e + 1 : e;
    }
    return 0;
}

static void tag_of(const char *name, char *out)
{
    int n = 0;
    while (name[n] && name[n] != ' ' && n < MR_MAX_NAME - 1) {
        out[n] = name[n];
        n++;
    }
    out[n] = '\0';
}

/* Transition `tr`, statement st's, as MWB_WITH's four words; 0 if
 * unsupported. */
static int with_args(const mr_stmt *st, const mr_transition *tr,
                     unsigned w[4])
{
    unsigned a = 0, b = 0, c = 0, kind;

    switch (tr->kind) {
    case MR_TRANS_NONE:         kind = MWB_T_NONE; break;
    case MR_TRANS_DISSOLVE:     kind = MWB_T_DISSOLVE; break;
    case MR_TRANS_FADE:         kind = MWB_T_FADE; break;
    case MR_TRANS_MOVE:         kind = MWB_T_MOVE; break;
    case MR_TRANS_MOVEINLEFT:   kind = MWB_T_MOVEINLEFT; break;
    case MR_TRANS_MOVEINRIGHT:  kind = MWB_T_MOVEINRIGHT; break;
    case MR_TRANS_MOVEOUTLEFT:  kind = MWB_T_MOVEOUTLEFT; break;
    case MR_TRANS_MOVEOUTRIGHT: kind = MWB_T_MOVEOUTRIGHT; break;
    default:
        err(st, "transition %d is not supported", tr->kind);
        return 0;
    }

    if (kind == MWB_T_FADE) {
        /* fade is Fade(.5, 0, .5) (00definitions.rpy:327). A Fade(...) call
         * keeps its hold time, which the parser's total cannot express. */
        char line[1024];
        const char *f = NULL;
        a = 500; b = 0; c = 500;
        /* The `with` that trails `show X with Fade(...)` carries the line
         * after the show's in the C89 parser, so the show's line is tried
         * too. */
        if (source_line(st, line, sizeof(line)))
            f = strstr(line, "Fade(");
        if (!f && st > g_s->st && source_line(st - 1, line, sizeof(line)))
            f = strstr(line, "Fade(");
        if (f) {
            double x = 0, y = 0, z = 0;
            if (sscanf(f, "Fade(%lf ,%lf ,%lf", &x, &y, &z) != 3) {
                err(st, "cannot read Fade's three times");
            } else {
                a = (unsigned)(x * 1000.0 + 0.5);
                b = (unsigned)(y * 1000.0 + 0.5);
                c = (unsigned)(z * 1000.0 + 0.5);
            }
        } else if (tr->ms != 1000) {
            err(st, "fade of %ldms with no Fade( on the line", tr->ms);
        }
    } else {
        a = (unsigned)tr->ms;
    }
    w[0] = kind;
    w[1] = a;
    w[2] = b;
    w[3] = c;
    return 1;
}

static void do_with(const mr_stmt *st)
{
    unsigned w[4];
    int i;

    if (!with_args(st, &st->trans, w))
        return;
    cmd(MWB_WITH);
    for (i = 0; i < 4; i++)
        w16(w[i]);
}

/* `call screen rhythm_game(audio, beatmap, background, beatmap_stride=2)
 * with fade` (script_game.rpy): the screen's arguments (.rpy:117), the
 * track, the beatmap, the backdrop and the note images settled here. No
 * display state changes: the screen is gone when the call returns. */
static void do_rhythm(const mr_stmt *st)
{
    static const char *const notes[4] = {
        /* note_drawables, tracks 0-3 (.rpy:478-483) */
        "00-renpythm/images/note_left.png", "00-renpythm/images/note_up.png",
        "00-renpythm/images/note_down.png", "00-renpythm/images/note_right.png"
    };
    char arg[3][MR_PATH_MAX];
    const char *p = st->text ? strchr(st->text, '(') : NULL, *s;
    unsigned w[4] = { MWB_T_NONE, 0, 0, 0 };
    int na = 0, stride = 2, i, reg, bg, note = 0, id;

    if (strcmp(st->name, "rhythm_game")) {
        err(st, "screen %s is not supported", st->name);
        return;
    }
    /* The three positional strings, then any keywords. */
    while (p && na < 3 && (p = strpbrk(p, "'\"")) != NULL) {
        const char *e = strchr(p + 1, *p);
        if (!e || e - p - 1 >= MR_PATH_MAX)
            break;
        memcpy(arg[na], p + 1, e - p - 1);
        arg[na][e - p - 1] = '\0';
        na++;
        p = e + 1;
    }
    if (na < 3) {
        err(st, "rhythm_game needs audio, beatmap and background");
        return;
    }
    /* beatmap_stride=None is 2 (.rpy:369-370). */
    if ((s = strstr(p, "beatmap_stride")) != NULL &&
        sscanf(s, "beatmap_stride = %d", &stride) != 1 &&
        sscanf(s, "beatmap_stride=%d", &stride) != 1)
        err(st, "cannot read beatmap_stride");
    if (stride < 1)
        stride = 1;
    if (strstr(p, "hit_sound"))
        warn(st, "hit_sound is not read: the ST's hit notes are the "
             "song's .hit0-3.wav, else do mi sol la (hit_notes)");

    /* The backdrop: `add background_path`, a full-stage image. */
    reg = -1;
    for (i = 0; i < g_nreg && reg < 0; i++)
        if (!strcmp(g_reg[i].file, arg[2]) && !g_reg[i].flip &&
            !g_reg[i].zoom)
            reg = i;
    if (reg < 0)
        reg = reg_zoomed(arg[2], 0);
    if (g_reg[reg].w != MR_STAGE_W || g_reg[reg].h != MR_STAGE_H) {
        err(st, "the rhythm game's background %s is not %dx%d", arg[2],
            MR_STAGE_W, MR_STAGE_H);
        return;
    }
    bg = img_id(st, reg, MWB_IMG_BG);

    /* The notes, then the same at zoom_scale 1.25 (.rpy:296, :485-490):
     * eight ids in a row, which they are when they are made together. */
    for (i = 0; i < 8; i++) {
        id = img_id(st, reg_zoomed(notes[i & 3], i < 4 ? 0 : 125),
                    MWB_IMG_SPRITE);
        if (i == 0)
            note = id;
        else if (id != note + i)
            err(st, "the note images are not in a row");
    }

    /* `with` after the arguments: the shared parser looks for it right
     * after the screen's name (parse.c:2104), so it is read here. */
    {
        const char *close = strrchr(st->text, ')');
        const char *wt = close ? strstr(close, " with ") : NULL;
        mr_transition tr;
        char word[MR_MAX_NAME];
        int k = 0;

        if (wt) {
            wt += 6;
            while (*wt == ' ')
                wt++;
            while (*wt && *wt != ' ' && *wt != '(' && k < MR_MAX_NAME - 1)
                word[k++] = *wt++;
            word[k] = '\0';
            mr_trans_parse(&tr, word);
            if (tr.kind == MR_TRANS_NONE && strcmp(word, "None")) {
                err(st, "transition %s is not supported", word);
                return;
            }
            if (!with_args(st, &tr, w))
                return;
        }
    }

    i = mus_id(st, arg[0]);
    cmd(MWB_RHYTHM);
    w16(i);
    w16(btm_id(st, arg[1], stride, g_mus[i]));
    w16(bg);
    w16(note);
    for (i = 0; i < 4; i++)
        w16(w[i]);
}

/* scene.c:974 bg_pos: a scene image stands where an unplaced image does.
 * Only a full-stage one there can be the ST's backdrop (MWB_IMG_BG, a
 * screen copied whole); any other is a sprite slot. */
static int is_backdrop(int reg)
{
    int x, y;

    place_resolve(NULL, 0, g_reg[reg].w, g_reg[reg].h, &x, &y);
    return x == 0 && y == 0 && g_reg[reg].w == MR_STAGE_W &&
           g_reg[reg].h == MR_STAGE_H;
}

/* Is `tagname` the backdrop's tag? */
static int bg_is(const char *tagname)
{
    char t[MR_MAX_NAME];

    if (g_cs.bg_reg < 0 || !tagname)
        return 0;
    tag_of(g_reg[g_cs.bg_reg].name, t);
    return !strcmp(t, tagname);
}

/* Tell the ST the stage again from the backdrop up: each slot where it
 * already is, so nothing moves. */
static void restage(void)
{
    int k;

    cmd(MWB_SCENE);
    w16(g_cs.bg < 0 ? 0xFFFF : (unsigned)g_cs.bg);
    for (k = 0; k < g_cs.nslot; k++) {
        cmd(MWB_SHOW);
        w16(g_cs.slot[k].tag);
        w16(g_cs.slot[k].img);
        w16((unsigned)g_cs.slot[k].x & 0xFFFF);
        w16((unsigned)g_cs.slot[k].y & 0xFFFF);
        w16(k);
    }
}

/* The backdrop is the bottom image of the master layer, under its tag
 * (Ren'Py's scene is a show after a clear: ast.py:1484-1487). A
 * statement that names the tag as more than a backdrop -- placed, behind,
 * or an image that cannot be one -- needs it as what it is in Ren'Py: the
 * bottom slot, unplaced (bottom-centre, which for a full-stage image is
 * where it already is). It becomes one under the last interaction's
 * baseline, as the ST's move only glides a tag that was a slot there
 * (vm.c move_plan). */
static void bg_to_slot(const mr_stmt *st)
{
    char tagname[MR_MAX_NAME];
    slot *sl;
    int x, y, was_dirty;

    if (g_cs.nslot == 16) {
        err(st, "more than 16 images shown");
        return;
    }
    memmove(&g_cs.slot[1], &g_cs.slot[0], g_cs.nslot * sizeof(slot));
    g_cs.nslot++;
    sl = &g_cs.slot[0];
    memset(sl, 0, sizeof(*sl));
    tag_of(g_reg[g_cs.bg_reg].name, tagname);
    sl->tag = tag_id(tagname);
    sl->reg = g_cs.bg_reg;
    sl->img = img_id(st, sl->reg, MWB_IMG_SPRITE);
    place_resolve(NULL, 0, g_reg[sl->reg].w, g_reg[sl->reg].h, &x, &y);
    sl->x = half(x);
    sl->y = half(y);
    g_cs.bg = g_cs.bg_reg = -1;
    was_dirty = g_cs.dirty;
    restage();
    if (was_dirty) {
        /* A `with None` here would take those changes out of the
         * transition. */
        warn(st, "'%s' stops being the backdrop after other changes to the "
             "stage: a `with move` will not glide it (show it first)",
             g_reg[sl->reg].name);
        return;
    }
    cmd(MWB_WITH);
    w16(MWB_T_NONE);
    w16(0);
    w16(0);
    w16(0);
}

static void do_show(const mr_stmt *st)
{
    char tagname[MR_MAX_NAME];
    int tag, i, at, reg, x, y, img;
    slot *sl, moved;

    tag_of(st->name, tagname);
    tag = tag_id(tagname);

    if (g_cs.bg_reg >= 0) {
        char bgtag[MR_MAX_NAME];
        tag_of(g_reg[g_cs.bg_reg].name, bgtag);
        if (in_taglist(st->behind, bgtag))
            bg_to_slot(st);
    }
    if (bg_is(tagname)) {
        reg = reg_choose(st, st->name, g_reg[g_cs.bg_reg].name);
        if (reg < 0) {
            err(st, "no image '%s'", st->name);
            return;
        }
        if (!st->has_place && !st->atl && is_backdrop(reg)) {
            /* An unplaced re-show keeps the default transform's place
             * (scenelists.py:445): still the backdrop. */
            g_cs.bg = img_id(st, reg, MWB_IMG_BG);
            g_cs.bg_reg = reg;
            restage();
            return;
        }
        bg_to_slot(st);
    }
    i = find_slot(tag);

    reg = reg_choose(st, st->name, i >= 0 ? g_reg[g_cs.slot[i].reg].name : NULL);
    if (reg < 0) {
        err(st, "no image '%s'", st->name);
        return;
    }

    /* Where the tag lands in the z-order: scene.c:412 find_add_index, which
     * is Ren'Py's find_index (scenelists.py:313) with every zorder equal --
     * the first entry whose tag is this one or is named by `behind`, else
     * the top. The old entry is taken out and the new one put there
     * (scenelists.py:424-455); the target is never after the old slot, so
     * the index means the same before and after the removal. */
    for (at = 0; at < g_cs.nslot; at++)
        if (g_cs.slot[at].tag == tag ||
            in_taglist(st->behind, g_tag[g_cs.slot[at].tag]))
            break;

    if (i >= 0) {
        memcpy(&moved, &g_cs.slot[i], sizeof(slot));
        memmove(&g_cs.slot[i], &g_cs.slot[i + 1], (g_cs.nslot - i - 1) * sizeof(slot));
        g_cs.nslot--;
    } else {
        if (g_cs.nslot == 16) {
            err(st, "more than 16 images shown");
            return;
        }
        memset(&moved, 0, sizeof(moved));
        moved.tag = tag;
    }
    memmove(&g_cs.slot[at + 1], &g_cs.slot[at], (g_cs.nslot - at) * sizeof(slot));
    memcpy(&g_cs.slot[at], &moved, sizeof(slot));
    g_cs.nslot++;
    sl = &g_cs.slot[at];
    sl->reg = reg;
    /* scenelists.py:445 -- a re-show that says nothing keeps the old
     * placement, so has_place is sticky. */
    if (st->has_place) {
        /* A re-show takes the state of the transform it replaces
         * (scenelists.py:444, :306 take_state); a tag first shown with no
         * placement had the `default` transform (scenelists.py:451,
         * 00definitions.rpy:64-67), bottom-centre. `show park:` +
         * `yoffset 400` moves it down from there, not from the corner. */
        if (i >= 0 && !sl->has_place) {
            memset(&sl->place, 0, sizeof(sl->place));
            sl->place.has_xpos = sl->place.has_ypos = 1;
            sl->place.xpos = 0.5f;
            sl->place.ypos = 1.0f;
            sl->place.has_xanchor = sl->place.has_yanchor = 1;
            sl->place.xanchor = 0.5f;
            sl->place.yanchor = 1.0f;
        }
        check_place(st, &st->place);
        place_merge(&sl->place, &st->place);
        sl->has_place = 1;
    }

    img = img_id(st, reg, MWB_IMG_SPRITE);
    place_resolve(&sl->place, sl->has_place, g_reg[reg].w, g_reg[reg].h,
                  &x, &y);

    if (st->atl) {
        /* The one ATL block in the game is the credits roll: a single linear
         * step on y. Anything richer is refused. */
        mr_place end = sl->place;
        int x1, y1;
        long ms;

        if (st->atl->n != 1 || st->atl->repeat ||
            st->atl->v[0].warp != MR_WARP_LINEAR) {
            err(st, "only a single linear ATL step is supported");
            return;
        }
        check_place(st, &st->atl->v[0].place);
        place_merge(&end, &st->atl->v[0].place);
        place_resolve(&end, 1, g_reg[reg].w, g_reg[reg].h, &x1, &y1);
        if (x1 != x)
            err(st, "ATL may only move vertically");
        ms = st->atl->v[0].ms;
        memcpy(&sl->place, &end, sizeof(end));
        sl->img = img;
        sl->x = half(x);
        sl->y = half(y1);

        cmd(MWB_SCROLL);
        w16(tag);
        w16(img);
        w16((unsigned)half(x) & 0xFFFF);
        w16((unsigned)half(y) & 0xFFFF);
        w16(at);
        w16((unsigned)half(y1) & 0xFFFF);
        w32((unsigned long)ms);
        return;
    }

    sl->img = img;
    sl->x = half(x);
    sl->y = half(y);
    cmd(MWB_SHOW);
    w16(tag);
    w16(img);
    w16((unsigned)half(x) & 0xFFFF);
    w16((unsigned)half(y) & 0xFFFF);
    w16(at);
}

static void do_scene(const mr_stmt *st)
{
    char tagname[MR_MAX_NAME];
    int reg, x, y;

    g_cs.nslot = 0;
    g_cs.bg = g_cs.bg_reg = -1;
    if (st->has_place)
        err(st, "scene with a placement is not supported");
    if (!st->name[0]) {
        restage();
        return;
    }
    reg = reg_find(st->name);
    if (reg < 0) {
        err(st, "no image '%s'", st->name);
        return;
    }
    if (is_backdrop(reg)) {
        g_cs.bg = img_id(st, reg, MWB_IMG_BG);
        g_cs.bg_reg = reg;
        restage();
        return;
    }
    /* Anything else is the bottom slot over black, where an unplaced
     * image stands (scene.c:974 bg_pos). */
    tag_of(st->name, tagname);
    g_cs.nslot = 1;
    memset(&g_cs.slot[0], 0, sizeof(slot));
    g_cs.slot[0].tag = tag_id(tagname);
    g_cs.slot[0].reg = reg;
    g_cs.slot[0].img = img_id(st, reg, MWB_IMG_SPRITE);
    place_resolve(NULL, 0, g_reg[reg].w, g_reg[reg].h, &x, &y);
    g_cs.slot[0].x = half(x);
    g_cs.slot[0].y = half(y);
    restage();
}

static void do_hide(const mr_stmt *st)
{
    char tagname[MR_MAX_NAME];
    int tag, i;

    tag_of(st->name, tagname);
    tag = tag_id(tagname);
    if (bg_is(tagname)) {
        /* The scene image is hidden by its tag like any other. */
        g_cs.bg = g_cs.bg_reg = -1;
        restage();
        return;
    }
    i = find_slot(tag);
    if (i < 0) {
        /* Hiding what is not shown is a no-op in Ren'Py. */
        warn(st, "hide %s: not shown", st->name);
        return;
    }
    memmove(&g_cs.slot[i], &g_cs.slot[i + 1], (g_cs.nslot - i - 1) * sizeof(slot));
    g_cs.nslot--;
    cmd(MWB_HIDE);
    w16(tag);
}

static unsigned who_of(const mr_stmt *st);

/* Whether character i is in Ren'Py's centered window (00definitions.rpy
 * :440, 00style.rpy:317), said in the stage's middle (MWB_WHO_CENTERED).
 * The parser knows only the game's characters (rpy/parse.c:1711), so a
 * game using `centered` defines it itself, as Ren'Py does; and it keeps
 * none of a Character's styles (parse.c:731), so the define lines are
 * read here, once. */
static int char_centered(int i)
{
    static signed char *c;
    int f, k, j;

    if (!c) {
        c = calloc((size_t)g_s->nchars + 1, 1);
        if (!c)
            mr_fatal("out of memory");
        for (f = 0; f < g_s->nfiles; f++) {
            mr_lines *l = mr_lex_file(g_s->files[f]);
            if (!l)
                continue;
            for (k = 0; k < l->n; k++) {
                const char *t = l->v[k].text, *r;
                char name[MR_MAX_NAME];
                if (strncmp(t, "define ", 7) ||
                    !strstr(t, "window_style=\"centered_window\"") ||
                    !assign_split(t + 7, name, &r) ||
                    strncmp(r, "Character(", 10))
                    continue;
                for (j = 0; j < g_s->nchars; j++)
                    if (!strcmp(g_s->chars[j].var, name))
                        c[j] = 1;
            }
            mr_lex_free(l);
        }
    }
    return c[i];
}

static void do_say(const mr_stmt *st)
{
    unsigned who = who_of(st);
    const char *v[MWB_NLANG];

    if (g_in_lang)
        err(st, "a say inside a language branch: checkpoints must be the "
            "same in every language");
    /* The English is the line's dialogue translation, by its identifier
     * (tl.h), falling back to the original as Ren'Py does. */
    v[MWB_LANG_FR] = st->text ? st->text : "";
    v[MWB_LANG_EN] = mr_tl_dialogue(st->tlid, v[MWB_LANG_FR]);
    if (v[MWB_LANG_EN] == v[MWB_LANG_FR] && *v[MWB_LANG_FR]) {
        /* Once per line, however many paths compile it. */
        static unsigned char *told;
        if (!told)
            told = calloc(g_s->n, 1);
        if (!told[st - g_s->st]++ && g_untranslated++ < 10)
            warn(st, "no English for this line (%s)",
                 st->tlid ? st->tlid : "no identifier");
    }
    cmd(MWB_SAY);
    w16(who);
    w16(text_str(st, v, who == MWB_WHO_CENTERED ? MWB_CENTER_ROWS
                                                 : MWB_TEXT_ROWS));
}

static void pause_cmd(unsigned long ms, int hard)
{
    if (g_in_lang && !ms)
        mr_fatal("an untimed pause inside a language branch: checkpoints "
                 "must be the same in every language");
    cmd(MWB_PAUSE);
    w32(ms);
    w16(hard ? 1 : 0);
}

/* `a, b, c = _return`: the names, up to 3; how many, or 0 if `t` is not
 * that. */
static int unpack_return(const char *t, char name[3][MR_MAX_NAME])
{
    int n = 0, k;

    for (;;) {
        while (*t == ' ')
            t++;
        k = 0;
        while ((isalnum((unsigned char)*t) || *t == '_') &&
               k < MR_MAX_NAME - 1)
            name[n][k++] = *t++;
        name[n][k] = '\0';
        if (!k || n == 3)
            return 0;
        n++;
        while (*t == ' ')
            t++;
        if (*t == ',') {
            t++;
            continue;
        }
        if (*t != '=' || t[1] == '=' || n < 2)
            return 0;
        t++;
        while (*t == ' ')
            t++;
        return !strcmp(t, "_return") ? n : 0;
    }
}

/* `$` lines. The game's Python is a handful of fixed idioms; each is either
 * translated or listed here as having no meaning on the ST. */
static void do_python(const mr_stmt *st)
{
    const char *t = st->text ? st->text : "";
    /* The rhythm game's (script_game.rpy): the ST has no mixer to mute or
     * turn down, and its checkpoints are its says, menus and untimed
     * pauses, of which this is none -- with rollback off around it, a
     * checkpoint there holds nothing to roll back to anyway. */
    static const char *const ignored[] = {
        "renpy.save_persistent()", "quick_menu", "renpy.movie_cutscene(",
        "renpy.checkpoint()", "preferences.set_volume(",
        "preferences.set_mute(",
        NULL
    };
    char name[MR_MAX_NAME], names[3][MR_MAX_NAME];
    const char *rhs;
    int i, n;
    double secs;

    for (i = 0; ignored[i]; i++)
        if (!strncmp(t, ignored[i], strlen(ignored[i])))
            return;

    if ((n = unpack_return(t, names)) > 0) {
        /* The rhythm game's (hits, notes, score) (.rpy:830-837). */
        for (i = 0; i < n; i++) {
            unsigned short w[3];
            w[0] = MWB_EX_RET;
            w[1] = (unsigned short)i;
            w[2] = MWB_EX_END;
            cmd(MWB_SET);
            w16((unsigned)var_add(st, names[i]));
            w16(expr_store(w, 3));
        }
    } else if (assign_split(t, name, &rhs) && !engine_name(name)) {
        /* A story variable. The ST saves `persistent.` ones as soon as
         * they are set, so renpy.save_persistent() has nothing to do. */
        unsigned off;
        val c;
        int v = var_add(st, name), r = compile_expr(st, rhs, &off, &c);
        if (r < 0)
            return;
        cmd(MWB_SET);
        w16((unsigned)v);
        w16(r ? off : expr_value(c));
    } else if (!strcmp(t, "_skipping = False") ||
               !strcmp(t, "_skipping = True")) {
        g_cs.skip_ok = t[12] == 'T';
        cmd(MWB_SKIPPING);
        w16(g_cs.skip_ok);
    } else if (!strcmp(t, "config.rollback_enabled = False") ||
               !strcmp(t, "config.rollback_enabled = True")) {
        g_cs.rollback_ok = t[26] == 'T';
        cmd(MWB_ROLLBACK);
        w16(g_cs.rollback_ok);
    } else if (!strcmp(t, "renpy.block_rollback()")) {
        cmd(MWB_FENCE);
    } else if (!strcmp(t, "renpy.pause()")) {
        pause_cmd(0, 0);
    } else if (sscanf(t, "renpy.pause ( %lf", &secs) == 1 ||
               sscanf(t, "renpy.pause( %lf", &secs) == 1) {
        pause_cmd((unsigned long)(secs * 1000.0 + 0.5),
                  strstr(t, "hard=True") != NULL);
    } else if (!strncmp(t, "title = ", 8)) {
        const char *q = strchr(t, '"');
        const char *e = q ? strrchr(t, '"') : NULL;
        if (!q || e == q) {
            err(st, "cannot read the title string");
            return;
        }
        memset(g_cs.title, 0, sizeof(g_cs.title));
        memcpy(g_cs.title, q + 1, e - q - 1);
    } else if (!strncmp(t, "renpy.music.play(", 17)) {
        const char *q = strchr(t, '"');
        const char *e = q ? strchr(q + 1, '"') : NULL;
        char file[MR_PATH_MAX];
        unsigned flags = 0;
        if (!q || !e) {
            err(st, "cannot read renpy.music.play's file");
            return;
        }
        memcpy(file, q + 1, e - q - 1);
        file[e - q - 1] = '\0';
        if (strstr(t, "loop=True"))
            flags |= MWB_MUS_LOOP;
        if (strstr(t, "if_changed=True"))
            flags |= MWB_MUS_IFCHANGED;
        g_cs.music = mus_id(st, file);
        cmd(MWB_MUSIC);
        w16(g_cs.music);
        w16(flags);
    } else {
        err(st, "python `%s` is not supported", t);
    }
}

/* What a statement does, for every statement that is not control flow:
 * its commands, and its effect on g_cs. 0 for control flow, which the
 * caller handles. */
static int stmt(const mr_stmt *st)
{
    switch (st->kind) {
    case MR_ST_LABEL:
    case MR_ST_NOP:
    case MR_ST_IMAGE:
    case MR_ST_INIT:
        break;
    case MR_ST_SAY:
        do_say(st);
        break;
    case MR_ST_SCENE:
        do_scene(st);
        break;
    case MR_ST_SHOW:
        do_show(st);
        break;
    case MR_ST_HIDE:
        do_hide(st);
        break;
    case MR_ST_WITH:
        do_with(st);
        break;
    case MR_ST_SHOWSCREEN:
        if (strcmp(st->name, "title")) {
            err(st, "screen %s is not supported", st->name);
            break;
        }
        g_cs.title_up = 1;
        g_cs.title_str = g_cs.chapter_str =
            tl_str(st, g_cs.title, 0);          /* "[title!t]" */
        cmd(MWB_TITLE);
        w16(g_cs.title_str);
        break;
    case MR_ST_CALLSCREEN:
        do_rhythm(st);
        break;
    case MR_ST_HIDESCREEN:
        if (!g_cs.title_up) {
            warn(st, "hide screen %s: not shown", st->name);
            break;
        }
        g_cs.title_up = 0;
        g_cs.title_str = 0xFFFF;
        cmd(MWB_UNTITLE);
        break;
    case MR_ST_PLAY:
        if (st->aux != MR_CH_MUSIC)
            break;              /* sound effects are dropped */
        g_cs.music = mus_id(st, st->text);
        cmd(MWB_MUSIC);
        w16(g_cs.music);
        w16(st->loop ? MWB_MUS_LOOP : 0);
        break;
    case MR_ST_QUEUE:
        if (st->aux == MR_CH_MUSIC)
            err(st, "queue music is not supported");
        break;
    case MR_ST_STOP:
        if (st->aux == MR_CH_MUSIC) {
            g_cs.music = -1;
            cmd(MWB_STOPMUSIC);
        }
        break;
    case MR_ST_WINDOW:
        g_cs.window = st->aux ? 1 : 0;
        cmd(MWB_WINDOW);
        w16(g_cs.window);
        break;
    case MR_ST_PAUSE:
        pause_cmd(st->ms > 0 ? (unsigned long)st->ms : 0, 0);
        break;
    case MR_ST_PYTHON:
        do_python(st);
        break;
    default:
        return 0;
    }
    return 1;
}

/* `if` on the language: which one, or -1 if the condition is not that. */
static int lang_of(const mr_stmt *st)
{
    const char *t = st->text ? st->text : "";
    char want[64];
    int l;

    if (!strcmp(t, "_preferences.language == None"))
        return MWB_LANG_FR;
    for (l = 1; l < MWB_NLANG; l++) {
        sprintf(want, "_preferences.language == \"%s\"", g_lang_name[l]);
        if (!strcmp(t, want))
            return l;
    }
    return -1;
}

/* Language branches, compiled as IFLANG: every branch is in the code, and
 * the ST runs the one for its language. The state simulated here has to be
 * each language's in turn, so a run of consecutive language `if`s is a
 * group: each branch starts from the state as it was before the group, and
 * after it the state is the French branch's. Branches must leave the same
 * number of images, or the `at` index of anything shown later would be
 * right in one language only. */
static cstate g_lang_pre, g_lang_def;
static int    g_lang_group_end = -1, g_lang_nres, g_lang_has_def;

static unsigned compile_from(int pc, int end);

static int lang_branch(int pc, int l)
{
    const mr_stmt *st = &g_s->st[pc];
    int end = st->target;
    size_t at;

    if (end - 1 <= pc || g_s->st[end - 1].kind != MR_ST_GOTO ||
        g_s->st[end - 1].target != end) {
        err(st, "only a lone `if` on the language is supported");
        return end;
    }
    if (pc != g_lang_group_end) {
        memcpy(&g_lang_pre, &g_cs, sizeof(g_cs));
        g_lang_has_def = 0;
        g_lang_nres = -1;
    } else {
        memcpy(&g_cs, &g_lang_pre, sizeof(g_cs));
    }

    cmd(MWB_IFLANG);
    w16((unsigned)l);
    at = g_ncode;
    w16(0);
    g_in_lang++;
    compile_from(pc + 1, end - 1);  /* the GOTO at end - 1 ends the chain */
    g_in_lang--;
    g_code[at] = (unsigned char)((g_ncode - at - 2) / 2 >> 8);
    g_code[at + 1] = (unsigned char)((g_ncode - at - 2) / 2);

    if (g_lang_nres >= 0 && g_cs.nslot != g_lang_nres)
        err(st, "language branches leave different numbers of images");
    g_lang_nres = g_cs.nslot;
    if (l == MWB_LANG_FR) {
        memcpy(&g_lang_def, &g_cs, sizeof(g_cs));
        g_lang_has_def = 1;
    } else if (g_lang_has_def) {
        memcpy(&g_cs, &g_lang_def, sizeof(g_cs));
    } else {
        memcpy(&g_cs, &g_lang_pre, sizeof(g_cs));
    }
    g_lang_group_end = end;
    return end;
}

/* Code already compiled, by (statement, state): where paths meet. States
 * are interned, so the table holds a number for each. */
#define HBITS 16
#define HSIZE (1 << HBITS)

static cstate      **g_csv;
static unsigned long *g_csh;
static int          *g_csnext;
static int           g_ncsv, g_capcsv;
static int           g_csbucket[HSIZE];

typedef struct seen {
    int      pc, cs;
    unsigned off;                   /* word offset of its code */
    int      next;
} seen;

static seen *g_seen;
static int   g_nseen, g_capseen;
static int   g_seenbucket[HSIZE];

static int cs_intern(const cstate *in)
{
    static cstate c;
    const cstate *cs = &c;
    unsigned long h;
    int b, i;

    /* The slots past the last are left over from what a scene or a hide
     * took away: not part of the state. */
    memcpy(&c, in, sizeof(c));
    memset(&c.slot[c.nslot], 0, (16 - c.nslot) * sizeof(slot));
    h = fnv(2166136261UL, cs, sizeof(*cs));
    b = (int)(h & (HSIZE - 1));

    for (i = g_csbucket[b] - 1; i >= 0; i = g_csnext[i] - 1)
        if (g_csh[i] == h && !memcmp(g_csv[i], cs, sizeof(*cs)))
            return i;
    if (g_ncsv == g_capcsv) {
        g_capcsv = g_capcsv ? g_capcsv * 2 : 1024;
        g_csv = realloc(g_csv, g_capcsv * sizeof(*g_csv));
        g_csh = realloc(g_csh, g_capcsv * sizeof(*g_csh));
        g_csnext = realloc(g_csnext, g_capcsv * sizeof(*g_csnext));
    }
    i = g_ncsv++;
    g_csv[i] = malloc(sizeof(*cs));
    memcpy(g_csv[i], cs, sizeof(*cs));
    g_csh[i] = h;
    g_csnext[i] = g_csbucket[b];
    g_csbucket[b] = i + 1;
    return i;
}

static int seen_find(int pc, int cs, unsigned *off)
{
    int b = (int)(((unsigned)pc * 40503u + (unsigned)cs) & (HSIZE - 1)), i;

    for (i = g_seenbucket[b] - 1; i >= 0; i = g_seen[i].next - 1)
        if (g_seen[i].pc == pc && g_seen[i].cs == cs) {
            *off = g_seen[i].off;
            return 1;
        }
    return 0;
}

static void seen_add(int pc, int cs, unsigned off)
{
    int b = (int)(((unsigned)pc * 40503u + (unsigned)cs) & (HSIZE - 1));

    if (g_nseen == g_capseen) {
        g_capseen = g_capseen ? g_capseen * 2 : 4096;
        g_seen = realloc(g_seen, g_capseen * sizeof(*g_seen));
    }
    g_seen[g_nseen].pc = pc;
    g_seen[g_nseen].cs = cs;
    g_seen[g_nseen].off = off;
    g_seen[g_nseen].next = g_seenbucket[b];
    g_seenbucket[b] = ++g_nseen;
}

/* Branches still to compile: a statement, the state it is reached with,
 * and what to fill in with its code's offset -- a word of the code, or an
 * option's pc. */
typedef struct pend {
    int     pc;
    cstate *cs;
    size_t  fix;                    /* byte offset in g_code, or */
    int     opt;                    /* an option, or -1 */
} pend;

static pend *g_pend;
static int   g_npend, g_cappend, g_pendhead;

static void pend_add(int pc, size_t fix, int opt)
{
    if (g_npend == g_cappend) {
        g_cappend = g_cappend ? g_cappend * 2 : 64;
        g_pend = realloc(g_pend, g_cappend * sizeof(*g_pend));
    }
    g_pend[g_npend].pc = pc;
    g_pend[g_npend].cs = malloc(sizeof(cstate));
    memcpy(g_pend[g_npend].cs, &g_cs, sizeof(cstate));
    g_pend[g_npend].fix = fix;
    g_pend[g_npend].opt = opt;
    g_npend++;
}

/* A placeholder word for a pc, and its branch to compile. */
static void w16_pc(int pc)
{
    pend_add(pc, g_ncode, -1);
    w16(0);
}

/* The menu options, as the ST gets them. */
typedef struct option {
    unsigned str, cond, pc;
} option;

static option *g_opt;
static int     g_nopt, g_capopt;

/* The character a say names: its index, or 0xFFFF. */
static unsigned who_of(const mr_stmt *st)
{
    int i;

    if (!st->name[0])
        return 0xFFFF;
    for (i = 0; i < g_s->nchars; i++)
        if (!strcmp(g_s->chars[i].var, st->name))
            return char_centered(i) ? MWB_WHO_CENTERED : (unsigned)i;
    err(st, "unknown character '%s'", st->name);
    return 0xFFFF;
}

/* A menu's caption in every language. A `who "..."` caption is a say, and
 * has a translation identifier (with `nointeract` in it); a bare string is
 * a menu item, translated through the string table (ast.py:1880). */
static unsigned menu_caption(const mr_stmt *st)
{
    const char *v[MWB_NLANG];

    if (!st->text)
        return 0xFFFF;
    v[MWB_LANG_FR] = st->text;
    v[MWB_LANG_EN] = st->tlid ? mr_tl_dialogue(st->tlid, st->text)
                              : mr_tl_string(st->text);
    return text_str(st, v, MWB_TEXT_ROWS);
}

/* An option is one line of its button, in every language, with any value
 * in it at its widest (wide_check, at the end). */
static void opt_width(const mr_stmt *st, unsigned id)
{
    int l;

    for (l = 0; l < MWB_NLANG; l++) {
        const char *s = g_tx[l] + g_soff[id * MWB_NLANG + l];
        int w = 0;
        if (strchr(s, MWB_TXT_VAR)) {
            wide_add(st, s, 0, l);
            continue;
        }
        for (; *s; s++)
            if (!is_code(*s))
                w++;
        if (w > MWB_OPT_COLS)
            err(st, "%s option is %d characters, a button holds %d",
                g_lang_name[l] ? g_lang_name[l] : "French", w, MWB_OPT_COLS);
    }
}

/* A menu, the MENU statement at pc: the options a condition does not
 * exclude for good, each branch queued. Ren'Py hides an option whose
 * condition is false (config.menu_include_disabled, config.py:985;
 * menuexports.py:101) and goes on past the menu when none is left. */
static void do_menu(int pc)
{
    const mr_stmt *st = &g_s->st[pc];
    unsigned who = who_of(st), str, first = (unsigned)g_nopt;
    int i, n = 0;

    if (g_in_lang)
        err(st, "a menu inside a language branch");
    if (st->trans.kind != MR_TRANS_NONE)
        err(st, "a menu with a transition is not supported");
    str = menu_caption(st);
    for (i = 0; i < st->aux; i++) {
        const mr_stmt *c = &g_s->st[pc + 1 + i];
        unsigned off = 0xFFFF;
        val k;

        if (c->kind != MR_ST_CHOICE) {
            err(st, "menu: statement %d is not an option", i);
            break;
        }
        if (c->cond) {
            int r = compile_expr(c, c->cond, &off, &k);
            if (r < 0 || (r == 0 && !v_truthy(k)))
                continue;           /* never offered */
            if (r == 0)
                off = 0xFFFF;
        }
        if (g_nopt == g_capopt) {
            g_capopt = g_capopt ? g_capopt * 2 : 32;
            g_opt = realloc(g_opt, g_capopt * sizeof(*g_opt));
        }
        if (n == MWB_MENU_MAXOPT)
            err(c, "a menu of more than %d options", MWB_MENU_MAXOPT);
        g_opt[g_nopt].str = tl_str(c, c->text, 1);
        opt_width(c, g_opt[g_nopt].str);
        g_opt[g_nopt].cond = off;
        g_opt[g_nopt].pc = 0;
        pend_add(c->target, 0, g_nopt);
        g_nopt++;
        n++;
    }
    cmd(MWB_MENU);
    w16(who);
    w16(str);
    w16(first);
    w16((unsigned)n);
    w16_pc(st->target);
}

/* Compile from statement pc, with the state in g_cs, until the path ends,
 * leaves [pc, end) (a language branch), or comes to a statement already
 * compiled with this same state -- where a GOTO joins it. Every other
 * branch the path passes is queued. Returns the word offset of the path's
 * code. */
static unsigned compile_from(int pc, int end)
{
    long guard = 0;
    long start = -1;
    unsigned off;

    while (pc < end) {
        const mr_stmt *st = &g_s->st[pc];

        if (++guard > 1000000L)
            mr_fatal("the walk does not terminate");

        /* Not inside a language branch, whose code is skipped in the other
         * language, nor between two `if`s of a language group. */
        if (!g_in_lang && pc != g_lang_group_end) {
            int cs = cs_intern(&g_cs);
            if (seen_find(pc, cs, &off)) {
                if (start < 0)
                    return off;
                cmd(MWB_GOTO);
                w16(off);
                return (unsigned)start;
            }
            seen_add(pc, cs, (unsigned)(g_ncode / 2));
        }
        if (start < 0)
            start = (long)(g_ncode / 2);

        if (stmt(st)) {
            pc++;
            continue;
        }
        switch (st->kind) {
        case MR_ST_IF: {
            int l = lang_of(st), r;
            val c;
            if (l >= 0) {
                if (g_in_lang)
                    err(st, "an `if` inside a language branch");
                pc = lang_branch(pc, l);
                continue;
            }
            if (g_in_lang)
                err(st, "an `if` inside a language branch");
            r = compile_expr(st, st->text ? st->text : "", &off, &c);
            if (r <= 0) {
                /* Known here: only the branch taken is compiled. */
                pc = r == 0 && v_truthy(c) ? pc + 1 : st->target;
                continue;
            }
            cmd(MWB_IF);
            w16(off);
            w16_pc(st->target);
            pc++;
            continue;
        }
        case MR_ST_GOTO:
            if (st->target >= end)
                return (unsigned)start;
            pc = st->target;
            continue;
        case MR_ST_JUMP: {
            int t = mr_script_find_label(g_s, st->name);
            if (g_in_lang)
                err(st, "a jump inside a language branch");
            if (t < 0) {
                err(st, "jump to unknown label '%s'", st->name);
                cmd(MWB_END);
                return (unsigned)start;
            }
            pc = t;
            continue;
        }
        case MR_ST_MENU:
            do_menu(pc);
            return (unsigned)start;
        case MR_ST_CALL: {
            /* Followed as a jump that remembers where it came from: the
             * ST gets the label's code in line, once per caller. */
            int t = mr_script_find_label(g_s, st->name);
            if (g_in_lang)
                err(st, "a call inside a language branch");
            if (t < 0) {
                err(st, "call to unknown label '%s'", st->name);
                cmd(MWB_END);
                return (unsigned)start;
            }
            if (g_cs.nret == CALL_DEPTH) {
                err(st, "calls nested more than %d deep (recursion?)",
                    CALL_DEPTH);
                cmd(MWB_END);
                return (unsigned)start;
            }
            g_cs.ret[g_cs.nret++] = pc + 1;
            pc = t;
            continue;
        }
        case MR_ST_RETURN:
            /* Back after the last `call`; with none, the end of the game,
             * as Ren'Py's: an empty return stack makes lookup_return None
             * (execution.py:790, ast.py:1710) and the game ends. */
            if (g_in_lang)
                err(st, "`return` inside a language branch");
            if (g_cs.nret > 0) {
                pc = g_cs.ret[--g_cs.nret];
                g_cs.ret[g_cs.nret] = 0;
                continue;
            }
            cmd(MWB_END);
            return (unsigned)start;
        default:
            err(st, "statement kind %d is not supported", st->kind);
            break;
        }
        pc++;
    }
    if (!g_in_lang)
        cmd(MWB_END);
    return (unsigned)(start < 0 ? (long)(g_ncode / 2) : start);
}

/* The whole game, from `start`: the first path, then every branch it and
 * the others queued, in the order met. */
static void compile_all(int start)
{
    cs_reset(&g_cs);
    compile_from(start, g_s->n);
    while (g_pendhead < g_npend) {
        pend *p = &g_pend[g_pendhead++];
        unsigned off;

        memcpy(&g_cs, p->cs, sizeof(cstate));
        free(p->cs);
        p->cs = NULL;
        off = compile_from(p->pc, g_s->n);
        p = &g_pend[g_pendhead - 1];        /* compile_from may realloc */
        if (p->opt >= 0) {
            g_opt[p->opt].pc = off;
        } else {
            g_code[p->fix] = (unsigned char)(off >> 8);
            g_code[p->fix + 1] = (unsigned char)off;
        }
    }
    if (g_ncode / 2 > 0xFFFF)
        mr_fatal("the code is %lu words; offsets are 16-bit",
                 (unsigned long)(g_ncode / 2));
}

/* ---- the path check ----------------------------------------------------
 *
 * The compiled code shares what paths have in common, and that sharing is
 * the part that can go wrong without any single line looking wrong. So
 * every path through the game -- every sequence of menu answers, in every
 * language -- is played twice and the two are compared line by line:
 *
 *   - the compiled code, run the way the ST runs it (xrun), and
 *   - the source, walked down that one path with nothing shared (orun),
 *     its conditions and assignments done by the C89 engine's own
 *     evaluator (expr.c), as minirenpy runs them.
 *
 * At every say, menu and end both print what is said and the whole display
 * state. Values print as numbers on both sides: expr.c holds True as 1,
 * where the ST keeps the bool and prints "True" as Ren'Py does. */

typedef struct trace {
    char **l;
    int    n, cap;
} trace;

static void tr_add(trace *t, const char *s)
{
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 256;
        t->l = realloc(t->l, t->cap * sizeof(*t->l));
    }
    t->l[t->n++] = strdup(s);
}

static void tr_free(trace *t)
{
    int i;
    for (i = 0; i < t->n; i++)
        free(t->l[i]);
    free(t->l);
    memset(t, 0, sizeof(*t));
}

/* MWC_TRACE: one path's compiled-side trace printed the way the ST's
 * NatFeats build prints it (vm.c path_say), to diff the two -- tags by
 * number, and booleans as Ren'Py prints them. */
static int g_st_trace;

#define TRACE_MAX  40000            /* lines: a loop that never ends */
#define MENU_MAX   64               /* answers on one path */

/* The display state as the ST holds it. */
typedef struct xstate {
    int bg, nslot;
    int tag[16], img[16], x[16], y[16];
    int title, chapter, music, window, skip_ok, rollback_ok;
} xstate;

static void xs_reset(xstate *s)
{
    memset(s, 0, sizeof(*s));
    s->bg = -1;
    s->title = s->chapter = 0xFFFF;
    s->music = -1;
    s->window = 1;
    s->skip_ok = s->rollback_ok = 1;
}

static void xs_of_cs(xstate *s, const cstate *cs)
{
    int i;

    s->bg = cs->bg;
    s->nslot = cs->nslot;
    for (i = 0; i < cs->nslot; i++) {
        s->tag[i] = cs->slot[i].tag;
        s->img[i] = cs->slot[i].img;
        s->x[i] = cs->slot[i].x;
        s->y[i] = cs->slot[i].y;
    }
    s->title = (int)cs->title_str;
    s->chapter = (int)cs->chapter_str;
    s->music = cs->music;
    s->window = cs->window;
    s->skip_ok = cs->skip_ok;
    s->rollback_ok = cs->rollback_ok;
}

/* MWC_TRACE's lines end with the story variables (not the persistent ones,
 * which a rollback or a load leaves as they are): what the ST's rollback
 * log restores. */
static const val *g_tr_vars;

/* "<what> | <state>", the line both sides print. */
static void tr_line(trace *t, const char *what, const xstate *s)
{
    char b[MR_MAX_TEXT * 4 + 1024];
    int n, i;

    n = snprintf(b, sizeof(b) - 512, "%s | bg %d;", what, s->bg);
    for (i = 0; i < s->nslot && n < (int)sizeof(b) - 64; i++) {
        if (g_st_trace)
            n += sprintf(b + n, " %d", s->tag[i]);
        else
            n += sprintf(b + n, " %s", g_tag[s->tag[i]]);
        n += sprintf(b + n, ":%d@%d,%d", s->img[i], s->x[i], s->y[i]);
    }
    n += sprintf(b + n, "; title %d ch %d mus %d win %d skip %d rb %d",
                 s->title, s->chapter, s->music, s->window, s->skip_ok,
                 s->rollback_ok);
    if (g_st_trace && g_tr_vars) {
        n += sprintf(b + n, "; vars");
        for (i = 0; i < g_nvar && n < (int)sizeof(b) - 16; i++) {
            val v = g_tr_vars[i];
            if (g_var[i].persistent)
                continue;
            if (v.type == MWB_V_NONE)
                n += sprintf(b + n, " None");
            else if (v.type == MWB_V_BOOL)
                n += sprintf(b + n, v.v ? " True" : " False");
            else if (v.type == MWB_V_STR)
                n += snprintf(b + n, sizeof(b) - n, " %s", g_vstr[v.v].text);
            else
                n += sprintf(b + n, " %d", v.v);
        }
    }
    tr_add(t, b);
}

/* Text for a trace: line breaks are wrapping, which is not under test. */
static void tr_text(char *out, const char *in, int outsz)
{
    int n = 0;
    for (; *in && n < outsz - 1; in++)
        out[n++] = *in == '\n' ? ' ' : *in;
    out[n] = '\0';
}

/* --- the compiled code, run --- */

static unsigned cw(unsigned pc, int i)
{
    return (unsigned)g_code[2 * (pc + i)] << 8 | g_code[2 * (pc + i) + 1];
}

/* String `id` in language l, its variables printed from `vars`. */
static void x_str(char *out, int outsz, unsigned id, int l, const val *vars)
{
    const char *s = g_tx[l] + g_soff[id * MWB_NLANG + l];
    char b[MR_MAX_TEXT * 2];
    int n = 0;

    for (; *s && n < (int)sizeof(b) - MWB_VSTR_MAX * 2 - 2; s++) {
        if (*s == MWB_TXT_VAR && s[1]) {
            val v = vars[*++s - MWB_TXT_VAR_BASE];
            if (v.type == MWB_V_NONE)
                n += sprintf(b + n, "None");
            else if (v.type == MWB_V_BOOL && g_st_trace)
                n += sprintf(b + n, v.v ? "True" : "False");
            else if (v.type == MWB_V_STR)
                n += sprintf(b + n, "%s", g_vstr[v.v].text);
            else
                n += sprintf(b + n, "%d", v.v);
        } else {
            b[n++] = *s;
        }
    }
    b[n] = '\0';
    tr_text(out, b, outsz);
}

static void x_show(xstate *s, unsigned pc)
{
    int tag = (int)cw(pc, 1), at = (int)cw(pc, 5), i;

    for (i = 0; i < s->nslot; i++)
        if (s->tag[i] == tag) {
            memmove(&s->tag[i], &s->tag[i + 1], (s->nslot - i - 1) * sizeof(int));
            memmove(&s->img[i], &s->img[i + 1], (s->nslot - i - 1) * sizeof(int));
            memmove(&s->x[i], &s->x[i + 1], (s->nslot - i - 1) * sizeof(int));
            memmove(&s->y[i], &s->y[i + 1], (s->nslot - i - 1) * sizeof(int));
            s->nslot--;
            break;
        }
    if (s->nslot == 16 || at > s->nslot)
        return;
    memmove(&s->tag[at + 1], &s->tag[at], (s->nslot - at) * sizeof(int));
    memmove(&s->img[at + 1], &s->img[at], (s->nslot - at) * sizeof(int));
    memmove(&s->x[at + 1], &s->x[at], (s->nslot - at) * sizeof(int));
    memmove(&s->y[at + 1], &s->y[at], (s->nslot - at) * sizeof(int));
    s->tag[at] = tag;
    s->img[at] = (int)cw(pc, 2);
    s->x[at] = (short)cw(pc, 3);
    s->y[at] = (short)cw(pc, 4);
    s->nslot++;
}

/* The display state each command is reached with, in one language, over
 * every path: what the ST's replay assumes is one state per command. */
static xstate        *g_at;
static unsigned char *g_at_seen;
static int            g_at_bad;

static void at_check(unsigned pc, const xstate *s, int l)
{
    if (!g_at_seen[pc]) {
        g_at_seen[pc] = 1;
        g_at[pc] = *s;
        return;
    }
    if (!memcmp(&g_at[pc], s, sizeof(*s)) || g_at_bad >= 3)
        return;
    {
        trace t;
        memset(&t, 0, sizeof(t));
        tr_line(&t, "one path", &g_at[pc]);
        tr_line(&t, "another ", s);
        fprintf(stderr, "path check, %s: code offset %u is reached with two "
                "display states\n  %s\n  %s\n",
                g_lang_name[l] ? g_lang_name[l] : "French", 2 * pc,
                t.l[0], t.l[1]);
        tr_free(&t);
        g_at_bad++;
    }
}

/* Joins: where a path reaches a menu as an earlier one did, everything
 * after it has been checked already, so the path stops there ("joined").
 * Without this a loop is played round to MENU_MAX on every path, and two
 * ways back double the paths at every lap.
 *
 * "As an earlier one did" is decided on the compiled side: the menu's code
 * offset, which names the compile's state too, and the variables and the
 * display state. The source side has to agree: its statement, its
 * variables and its walk state at that menu are recorded the first time
 * and must be the same at every join -- else the two sides do not mean
 * the same thing by that state, which is a difference like any other.
 *
 * A key is first seen at a menu the path chose freely (past its given
 * answers), and every answer to that menu is queued from there, so what
 * follows a key is always explored once in full. */
typedef struct blob {
    unsigned char *b;
    size_t         n, cap;
} blob;

static void blob_add(blob *k, const void *p, size_t n)
{
    if (k->n + n > k->cap) {
        k->cap = (k->n + n) * 2;
        k->b = realloc(k->b, k->cap);
    }
    memcpy(k->b + k->n, p, n);
    k->n += n;
}

typedef struct join {
    blob xk, ok;                    /* ok.b NULL until the source side ran */
    int  next;
} join;

static int   g_join;                /* joining paths: check_paths only */
static join *g_jn;
static int   g_njn, g_capjn;
static int   g_jbucket[HSIZE];
static blob  g_xk[MENU_MAX + 1], g_ok[MENU_MAX + 1];
static int   g_joined;              /* menu index the compiled side joined */

static void join_reset(void)
{
    int i;
    for (i = 0; i < g_njn; i++) {
        free(g_jn[i].xk.b);
        free(g_jn[i].ok.b);
    }
    g_njn = 0;
    memset(g_jbucket, 0, sizeof(g_jbucket));
}

static int join_find(const blob *k)
{
    int b = (int)(fnv(2166136261UL, k->b, k->n) & (HSIZE - 1)), i;

    for (i = g_jbucket[b] - 1; i >= 0; i = g_jn[i].next - 1)
        if (g_jn[i].xk.n == k->n && !memcmp(g_jn[i].xk.b, k->b, k->n))
            return i;
    return -1;
}

static void join_add(const blob *k)
{
    int b = (int)(fnv(2166136261UL, k->b, k->n) & (HSIZE - 1));

    if (g_njn == g_capjn) {
        g_capjn = g_capjn ? g_capjn * 2 : 1024;
        g_jn = realloc(g_jn, g_capjn * sizeof(*g_jn));
    }
    memset(&g_jn[g_njn], 0, sizeof(*g_jn));
    blob_add(&g_jn[g_njn].xk, k->b, k->n);
    g_jn[g_njn].next = g_jbucket[b];
    g_jbucket[b] = ++g_njn;
}

/* The compiled side's key at a menu. */
static void xkey(blob *k, unsigned pc, const val *vars, const xstate *s)
{
    k->n = 0;
    blob_add(k, &pc, sizeof(pc));
    blob_add(k, vars, g_nvar * sizeof(*vars));
    blob_add(k, s, sizeof(*s));
}

/* The source side's: its statement, the reference store's value of every
 * variable, and the walk's state (cs_intern's normal form). */
static void okey(blob *k, int pc)
{
    static cstate c;
    int i;

    k->n = 0;
    blob_add(k, &pc, sizeof(pc));
    for (i = 0; i < g_nvar; i++) {
        mr_value v;
        memset(&v, 0, sizeof(v));
        if (!mr_var_get(g_var[i].name, &v))
            v.type = MR_V_NONE;
        blob_add(k, &v.type, sizeof(v.type));
        if (v.type == MR_V_STR)
            blob_add(k, v.s, strlen(v.s) + 1);
        else if (v.type == MR_V_FLOAT)
            blob_add(k, &v.f, sizeof(v.f));
        else if (v.type == MR_V_INT)
            blob_add(k, &v.i, sizeof(v.i));
    }
    memcpy(&c, &g_cs, sizeof(c));
    memset(&c.slot[c.nslot], 0, (16 - c.nslot) * sizeof(slot));
    blob_add(k, &c, sizeof(c));
}

/* Play the code in language l, answering menu k with ans[k] (0 past
 * nans). The visible options of each menu met go in nvis[]. With g_join,
 * a menu past the given answers that has been reached as before ends the
 * path ("joined"), its index in g_joined (-1 if none). */
static void xrun(int l, const int *ans, int nans, int *nvis, int *nmenus,
                 trace *t)
{
    static const unsigned char words[MWB_NCMD] = MWB_CMD_WORDS;
    char b[MR_MAX_TEXT * 4], s1[MR_MAX_TEXT * 2];
    val vars[MWB_TXT_MAXVAR];
    xstate s;
    unsigned pc = 0;
    long steps = 0;
    int i;

    for (i = 0; i < g_nvar; i++)
        vars[i] = g_var[i].init;
    g_tr_vars = vars;
    xs_reset(&s);
    *nmenus = 0;
    g_joined = -1;
    for (;;) {
        unsigned op = cw(pc, 0);

        /* Compared as bytes: the slots past the last are not state. */
        for (i = s.nslot; i < 16; i++)
            s.tag[i] = s.img[i] = s.x[i] = s.y[i] = 0;
        at_check(pc, &s, l);
        if (++steps > 20000000L) {
            tr_line(t, "runaway", &s);
            return;
        }
        if (t->n >= TRACE_MAX) {
            tr_line(t, "cut", &s);
            return;
        }
        switch (op) {
        case MWB_END:
            tr_line(t, "end", &s);
            return;
        case MWB_SAY:
            x_str(s1, sizeof(s1), cw(pc, 2), l, vars);
            snprintf(b, sizeof(b), "say %d %s", (short)cw(pc, 1), s1);
            tr_line(t, b, &s);
            break;
        case MWB_PAUSE:
            /* The ST's trace has its other checkpoint too: what a
             * rollback there counts. */
            if (g_st_trace && !cw(pc, 1) && !cw(pc, 2))
                tr_line(t, "pause", &s);
            break;
        case MWB_SCENE:
            s.bg = cw(pc, 1) == 0xFFFF ? -1 : (int)cw(pc, 1);
            s.nslot = 0;
            break;
        case MWB_SHOW:
            x_show(&s, pc);
            break;
        case MWB_SCROLL:
            x_show(&s, pc);
            for (i = 0; i < s.nslot; i++)
                if (s.tag[i] == (int)cw(pc, 1))
                    s.y[i] = (short)cw(pc, 6);
            break;
        case MWB_HIDE:
            for (i = 0; i < s.nslot; i++)
                if (s.tag[i] == (int)cw(pc, 1)) {
                    int k;
                    for (k = i; k < s.nslot - 1; k++) {
                        s.tag[k] = s.tag[k + 1];
                        s.img[k] = s.img[k + 1];
                        s.x[k] = s.x[k + 1];
                        s.y[k] = s.y[k + 1];
                    }
                    s.nslot--;
                    break;
                }
            break;
        case MWB_TITLE:
            s.title = s.chapter = (int)cw(pc, 1);
            break;
        case MWB_UNTITLE:
            s.title = 0xFFFF;
            break;
        case MWB_MUSIC:
            s.music = (int)cw(pc, 1);
            break;
        case MWB_STOPMUSIC:
            s.music = -1;
            break;
        case MWB_WINDOW:
            s.window = (int)cw(pc, 1);
            break;
        case MWB_SKIPPING:
            s.skip_ok = (int)cw(pc, 1);
            break;
        case MWB_ROLLBACK:
            s.rollback_ok = (int)cw(pc, 1);
            break;
        case MWB_IFLANG:
            if ((int)cw(pc, 1) != l)
                pc += cw(pc, 2);
            break;
        case MWB_GOTO:
            pc = cw(pc, 1);
            continue;
        case MWB_IF:
            if (!v_truthy(expr_eval(cw(pc, 1), vars))) {
                pc = cw(pc, 2);
                continue;
            }
            break;
        case MWB_SET:
            vars[cw(pc, 1)] = expr_eval(cw(pc, 2), vars);
            break;
        case MWB_MENU: {
            unsigned first = cw(pc, 3), n = cw(pc, 4), k;
            int vis[64], nv = 0, a;

            if (*nmenus >= MENU_MAX) {
                tr_line(t, "cut", &s);
                return;
            }
            if (cw(pc, 2) == 0xFFFF)
                s1[0] = '\0';
            else
                x_str(s1, sizeof(s1), cw(pc, 2), l, vars);
            snprintf(b, sizeof(b), "menu %d %s [", (short)cw(pc, 1), s1);
            for (k = 0; k < n && nv < 64; k++) {
                const option *o = &g_opt[first + k];
                if (o->cond != 0xFFFF && !v_truthy(expr_eval(o->cond, vars)))
                    continue;
                vis[nv++] = (int)(first + k);
                x_str(s1, sizeof(s1), o->str, l, vars);
                strncat(b, s1, sizeof(b) - strlen(b) - 4);
                strcat(b, " /");
            }
            strcat(b, "]");
            tr_line(t, b, &s);
            if (!nv) {
                pc = cw(pc, 5);
                continue;
            }
            if (g_join) {
                xkey(&g_xk[*nmenus], pc, vars, &s);
                if (*nmenus >= nans) {
                    if (join_find(&g_xk[*nmenus]) >= 0) {
                        g_joined = *nmenus;
                        tr_line(t, "joined", &s);
                        return;
                    }
                    join_add(&g_xk[*nmenus]);
                }
            }
            a = *nmenus < nans ? ans[*nmenus] : 0;
            nvis[(*nmenus)++] = nv;
            pc = g_opt[vis[a < nv ? a : 0]].pc;
            continue;
        }
        }
        pc += words[op];
    }
}

/* --- the source, walked --- */

static int stub_call(const char *name, mr_value *argv, const char **argname,
                     int argc, mr_value *result)
{
    (void)name; (void)argv; (void)argname; (void)argc;
    result->type = MR_V_NONE;
    return 0;
}

/* `text` in the reference store: variables printed as numbers; constants
 * are left for text_conv, as the compile leaves them. */
static void o_subst(char *out, int outsz, const char *text)
{
    int n = 0;

    while (*text && n < outsz - 32) {
        const char *e = text[0] == '[' && text[1] != '[' ?
                        strchr(text, ']') : NULL;
        char name[MR_MAX_NAME];
        mr_value v;

        if (text[0] == '[' && text[1] == '[') {
            out[n++] = *text++;
            out[n++] = *text++;
            continue;
        }
        if (e && e - text - 1 < (int)sizeof(name)) {
            memcpy(name, text + 1, e - text - 1);
            name[e - text - 1] = '\0';
            if (var_find(name) >= 0) {
                if (!mr_var_get(name, &v) || v.type == MR_V_NONE) {
                    n += sprintf(out + n, "None");
                } else if (v.type == MR_V_STR) {
                    /* As it is: its brackets are text, so they go on
                     * doubled to the conversion that reads them. */
                    const char *c;
                    for (c = v.s; *c && n < outsz - 34; c++) {
                        if (*c == '[')
                            out[n++] = '[';
                        out[n++] = *c;
                    }
                } else {
                    n += sprintf(out + n, "%ld", v.i);
                }
                text = e + 1;
                continue;
            }
        }
        out[n++] = *text++;
    }
    out[n] = '\0';
}

/* A say's or caption's text in language l, as the trace prints it. */
static void o_text(char *out, int outsz, const mr_stmt *st, const char *orig,
                   int dialogue, int l)
{
    char a[MR_MAX_TEXT * 2], b[MR_MAX_TEXT * 2];
    const char *t = orig;

    if (l == MWB_LANG_EN)
        t = dialogue ? mr_tl_dialogue(st->tlid, orig) : mr_tl_string(orig);
    o_subst(a, sizeof(a), t);
    g_lang = l;
    text_conv(st, a, 0, b, sizeof(b));
    g_lang = 0;
    tr_text(out, b, outsz);
}

/* The source walked the same way; `joined` is the menu index the compiled
 * side stopped at (-1 for none), where this stops too. Each menu's key goes
 * in g_ok[] when joining. */
static void orun(int l, const int *ans, int nans, int start, int joined,
                 trace *t)
{
    char b[MR_MAX_TEXT * 4], s1[MR_MAX_TEXT * 2];
    xstate s;
    int pc = start, i, nmenus = 0, ret[CALL_DEPTH], nret = 0;
    long steps = 0;
    mr_value lang;

    /* A new game: the defaults and constants, and the language. */
    mr_expr_init(stub_call);
    for (i = 0; i < g_s->n; i++) {
        const mr_stmt *st = &g_s->st[i];
        char name[MR_MAX_NAME];
        const char *rhs;
        int k;
        if (st->kind != MR_ST_INIT || !st->text ||
            !assign_split(st->text, name, &rhs))
            continue;
        for (k = 0; k < g_nconst; k++)
            if (!strcmp(g_const[k], name))
                break;
        if (var_find(name) >= 0 || k < g_nconst)
            mr_expr_exec(st->text);
    }
    memset(&lang, 0, sizeof(lang));
    lang.type = MR_V_NONE;
    if (g_lang_name[l])
        mr_value_set_str(&lang, g_lang_name[l]);
    mr_var_set("_preferences.language", &lang);
    cs_reset(&g_cs);

    for (;;) {
        const mr_stmt *st;

        if (pc >= g_s->n) {
            xs_of_cs(&s, &g_cs);
            tr_line(t, "end", &s);
            return;
        }
        st = &g_s->st[pc];
        if (++steps > 20000000L) {
            xs_of_cs(&s, &g_cs);
            tr_line(t, "runaway", &s);
            return;
        }
        if (t->n >= TRACE_MAX) {
            xs_of_cs(&s, &g_cs);
            tr_line(t, "cut", &s);
            return;
        }
        if (stmt(st)) {
            char name[MR_MAX_NAME], names[3][MR_MAX_NAME], set[80];
            const char *rhs;
            int n;
            if (st->kind == MR_ST_PYTHON && st->text &&
                assign_split(st->text, name, &rhs) && var_find(name) >= 0) {
                /* A high score: 0, the file being empty here. */
                if (!strncmp(rhs, HS_FUNC "(", strlen(HS_FUNC) + 1)) {
                    snprintf(set, sizeof(set), "%s = 0", name);
                    mr_expr_exec(set);
                } else
                    mr_expr_exec(st->text);
            }
            /* The rhythm game's results, 0 as xrun has them. */
            if (st->kind == MR_ST_PYTHON && st->text &&
                (n = unpack_return(st->text, names)) > 0)
                while (n--) {
                    snprintf(set, sizeof(set), "%s = 0", names[n]);
                    mr_expr_exec(set);
                }
            if (st->kind == MR_ST_SAY) {
                o_text(s1, sizeof(s1), st, st->text ? st->text : "", 1, l);
                snprintf(b, sizeof(b), "say %d %s", (short)who_of(st), s1);
                xs_of_cs(&s, &g_cs);
                tr_line(t, b, &s);
            }
            pc++;
            continue;
        }
        switch (st->kind) {
        case MR_ST_IF: {
            mr_value v;
            mr_expr_eval(st->text ? st->text : "", &v);
            pc = mr_value_truthy(&v) ? pc + 1 : st->target;
            continue;
        }
        case MR_ST_GOTO:
            pc = st->target;
            continue;
        case MR_ST_JUMP:
            pc = mr_script_find_label(g_s, st->name);
            if (pc < 0)
                pc = g_s->n;
            continue;
        case MR_ST_MENU: {
            int vis[64], nv = 0, a;

            if (nmenus >= MENU_MAX) {
                xs_of_cs(&s, &g_cs);
                tr_line(t, "cut", &s);
                return;
            }
            if (st->text)
                o_text(s1, sizeof(s1), st, st->text, st->tlid != NULL, l);
            else
                s1[0] = '\0';
            snprintf(b, sizeof(b), "menu %d %s [", (short)who_of(st), s1);
            for (i = 0; i < st->aux && nv < 64; i++) {
                const mr_stmt *c = &g_s->st[pc + 1 + i];
                if (c->cond) {
                    mr_value v;
                    mr_expr_eval(c->cond, &v);
                    if (!mr_value_truthy(&v))
                        continue;
                }
                vis[nv++] = pc + 1 + i;
                o_text(s1, sizeof(s1), c, c->text ? c->text : "", 0, l);
                strncat(b, s1, sizeof(b) - strlen(b) - 4);
                strcat(b, " /");
            }
            strcat(b, "]");
            xs_of_cs(&s, &g_cs);
            tr_line(t, b, &s);
            if (!nv) {
                pc = st->target;
                continue;
            }
            if (g_join) {
                okey(&g_ok[nmenus], pc);
                if (nmenus == joined) {
                    tr_line(t, "joined", &s);
                    return;
                }
            }
            a = nmenus < nans ? ans[nmenus] : 0;
            nmenus++;
            pc = g_s->st[vis[a < nv ? a : 0]].target;
            continue;
        }
        case MR_ST_CALL:
            if (nret == CALL_DEPTH) {
                xs_of_cs(&s, &g_cs);
                tr_line(t, "unsupported", &s);
                return;
            }
            ret[nret++] = pc + 1;
            pc = mr_script_find_label(g_s, st->name);
            if (pc < 0)
                pc = g_s->n;
            continue;
        case MR_ST_RETURN:
            if (nret > 0) {
                pc = ret[--nret];
                continue;
            }
            xs_of_cs(&s, &g_cs);
            tr_line(t, "end", &s);
            return;
        default:
            xs_of_cs(&s, &g_cs);
            tr_line(t, "unsupported", &s);
            return;
        }
    }
}

/* Every path, both ways. Returns the number that disagree. */
static int check_paths(int start)
{
    unsigned char *code = g_code;
    size_t ncode = g_ncode, capcode = g_capcode;
    unsigned long ncmd[MWB_NCMD];
    unsigned nsid = g_nsid;
    int nimg = g_nimg, ntag = g_ntag, nmus = g_nmus, nvar = g_nvar;
    int untrans = g_untranslated, bad = 0, paths = 0, nvstr = g_nvstr, l;
    size_t nexpr = g_nexpr;

    memcpy(ncmd, g_ncmd, sizeof(ncmd));
    g_quiet = 1;
    g_at_bad = 0;
    for (l = 0; l < MWB_NLANG && bad < 3; l++) {
        int **stack = NULL, *slen = NULL, nstack = 0, cap = 0;

        free(g_at);
        free(g_at_seen);
        g_at = malloc((ncode / 2 + 1) * sizeof(*g_at));
        g_at_seen = calloc(ncode / 2 + 1, 1);
        join_reset();
        g_join = 1;
        stack = malloc(sizeof(*stack));
        slen = malloc(sizeof(*slen));
        stack[0] = NULL;
        slen[0] = 0;
        nstack = cap = 1;
        while (nstack && bad < 3) {
            int *p = stack[--nstack], np = slen[nstack];
            int nvis[MENU_MAX + 1], nm, full[MENU_MAX + 1], j, i;
            trace xt, ot;

            memset(&xt, 0, sizeof(xt));
            memset(&ot, 0, sizeof(ot));
            /* The compiled side reads g_code; the walk writes its
             * commands somewhere else. */
            g_code = code;
            g_ncode = ncode;
            xrun(l, p, np, nvis, &nm, &xt);
            for (j = 0; j < nm; j++)
                full[j] = j < np ? p[j] : 0;
            g_code = NULL;
            g_ncode = g_capcode = 0;
            orun(l, full, nm, start, g_joined, &ot);
            free(g_code);
            g_code = code;
            g_ncode = ncode;
            g_capcode = capcode;
            paths++;

            /* The source side's key where this path met a menu first, and
             * at its join the one met there before. A key first met here
             * has its source key from this run, even if the join is to an
             * earlier menu of this same path. */
            for (j = np; j < nm; j++) {
                join *e = &g_jn[join_find(&g_xk[j])];
                if (!e->ok.b)
                    blob_add(&e->ok, g_ok[j].b, g_ok[j].n);
            }
            if (g_joined >= 0) {
                join *e = &g_jn[join_find(&g_xk[g_joined])];
                if (!e->ok.b || e->ok.n != g_ok[g_joined].n ||
                    memcmp(e->ok.b, g_ok[g_joined].b, e->ok.n)) {
                    fprintf(stderr, "path check, %s, answers",
                            g_lang_name[l] ? g_lang_name[l] : "French");
                    for (j = 0; j < nm; j++)
                        fprintf(stderr, " %d", full[j] + 1);
                    fprintf(stderr, ": menu %d is reached as before by the "
                            "compiled code, but not by the source\n",
                            g_joined + 1);
                    bad++;
                }
            }

            for (i = 0; i < xt.n || i < ot.n; i++) {
                const char *a = i < xt.n ? xt.l[i] : "(nothing)";
                const char *o = i < ot.n ? ot.l[i] : "(nothing)";
                if (strcmp(a, o)) {
                    fprintf(stderr, "path check, %s, answers",
                            g_lang_name[l] ? g_lang_name[l] : "French");
                    for (j = 0; j < nm; j++)
                        fprintf(stderr, " %d", full[j] + 1);
                    fprintf(stderr, ": line %d differs\n  compiled: %s\n"
                            "  source:   %s\n", i + 1, a, o);
                    bad++;
                    break;
                }
            }
            tr_free(&xt);
            tr_free(&ot);

            /* Every other answer to every menu past the given ones. */
            for (j = np; j < nm; j++)
                for (i = 1; i < nvis[j]; i++) {
                    int *q = malloc((j + 1) * sizeof(int));
                    memcpy(q, full, j * sizeof(int));
                    q[j] = i;
                    if (nstack == cap) {
                        cap *= 2;
                        stack = realloc(stack, cap * sizeof(*stack));
                        slen = realloc(slen, cap * sizeof(*slen));
                    }
                    stack[nstack] = q;
                    slen[nstack++] = j + 1;
                }
            free(p);
            if (paths > 200000) {
                fprintf(stderr, "path check: stopped after %d paths\n", paths);
                bad++;
            }
        }
        while (nstack)
            free(stack[--nstack]);
        free(stack);
        free(slen);
    }
    g_quiet = 0;
    g_join = 0;
    join_reset();
    bad += g_at_bad;
    free(g_at);
    free(g_at_seen);
    g_at = NULL;
    g_at_seen = NULL;
    memcpy(g_ncmd, ncmd, sizeof(ncmd));
    g_untranslated = untrans;
    if (g_nsid != nsid || g_nimg != nimg || g_ntag != ntag ||
        g_nmus != nmus || g_nvar != nvar || g_nexpr != nexpr ||
        g_nvstr != nvstr) {
        fprintf(stderr, "path check: the walk met something the compile "
                "did not\n");
        bad++;
    }
    if (!bad)
        printf("path check: %d paths in %d languages agree\n",
               paths, MWB_NLANG);
    return bad;
}

/* ---- output ------------------------------------------------------------ */

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

static void putname(FILE *f, const char *s, int n)
{
    int i, l = (int)strlen(s);
    for (i = 0; i < n; i++)
        fputc(i < l ? s[i] : 0, f);
}

static void img_filename(int i, char *out)
{
    sprintf(out, "%c%03d.%s", g_img[i].kind == MWB_IMG_BG ? 'B' : 'S', i,
            g_img[i].kind == MWB_IMG_BG ? "MBG" : "MSP");
}

/* One language's strings, beside the script. */
static int write_mwt(const char *dir, int l)
{
    char path[1024];
    unsigned long base = 8UL + 4UL * g_nsid, end = base + g_ntx[l];
    unsigned i;
    FILE *f;

    sprintf(path, "%s/%s", dir, g_lang_file[l]);
    f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return 0;
    }
    put32(f, MWB_MWT_MAGIC);
    put16(f, g_nsid);
    put16(f, (unsigned)l);
    for (i = 0; i < g_nsid; i++)
        put32(f, base + g_soff[i * MWB_NLANG + l]);
    fwrite(g_tx[l], 1, g_ntx[l], f);
    if (end & 1)
        fputc(0, f);                /* pak entries are even anyway */
    fclose(f);
    printf("%s: %u strings, %lu bytes of text\n", g_lang_file[l], g_nsid,
           (unsigned long)g_ntx[l]);
    return 1;
}

static unsigned g_char_str[256];

static unsigned long fnv(unsigned long h, const void *p, size_t n)
{
    const unsigned char *b = p;

    while (n--)
        h = ((h ^ *b++) * 16777619UL) & 0xFFFFFFFFUL;
    return h;
}

static unsigned long fnv_str(unsigned long h, unsigned id)
{
    const char *s = g_tx[MWB_LANG_FR] + g_soff[id * MWB_NLANG + MWB_LANG_FR];

    return fnv(h, s, strlen(s) + 1);
}

/* The header's save_sig: the checkpoints, in order, as the runtime counts
 * them (vm.c is_checkpoint). A say is its French speaker and text, which
 * is what tells the player where a save puts them; a menu is its caption
 * and its options. */
static unsigned long save_sig(void)
{
    static const unsigned char words[MWB_NCMD] = MWB_CMD_WORDS;
    unsigned long h = 2166136261UL;
    size_t pc;

    for (pc = 0; pc < g_ncode; pc += 2UL * words[g_code[pc + 1]]) {
        const unsigned char *c = g_code + pc;
        unsigned a1 = (unsigned)c[2] << 8 | c[3];
        unsigned a2 = (unsigned)c[4] << 8 | c[5];

        if (c[1] == MWB_SAY) {
            h = fnv(h, "S", 1);
            if (a1 != 0xFFFF)
                h = fnv_str(h, g_char_str[a1]);
            h = fnv_str(h, a2);
        } else if (c[1] == MWB_PAUSE && a1 == 0 && a2 == 0) {
            h = fnv(h, "P", 1);
        } else if (c[1] == MWB_MENU) {
            unsigned first = (unsigned)c[6] << 8 | c[7];
            unsigned n = (unsigned)c[8] << 8 | c[9], i;
            h = fnv(h, "M", 1);
            if (a1 != 0xFFFF)
                h = fnv_str(h, g_char_str[a1]);
            if (a2 != 0xFFFF)
                h = fnv_str(h, a2);
            for (i = 0; i < n; i++)
                h = fnv_str(h, g_opt[first + i].str);
        }
    }
    return h;
}

static unsigned long g_mwb_size;

/* Beatmap i, beside the script (mwb.h, "beatmaps"). */
static int write_btm(const char *dir, int i)
{
    char path[1024];
    const btm *b = &g_btm[i];
    FILE *f;
    int k;

    snprintf(path, sizeof(path), "%s/R%02d.BTM", dir, i);
    f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return 0;
    }
    put32(f, b->key);
    put32(f, b->end);
    put16(f, (unsigned)b->n);
    put16(f, 0);
    for (k = 0; k < 4; k++)
        putc(b->hit[k], f);
    for (k = 0; k < b->n; k++)
        put32(f, b->note[k]);
    fclose(f);
    return 1;
}

static int write_mwb(const char *path)
{
    unsigned long img_off, char_off, mus_off, var_off, expr_off, opt_off;
    unsigned long vstr_off, vstr_text, code_off, end;
    char name[16], dir[1024], *slash;
    FILE *f;
    int i;

    img_off  = MWB_HEADER_SIZE;
    char_off = img_off + 20UL * g_nimg;
    mus_off  = char_off + 2UL * g_s->nchars;
    var_off  = mus_off + 16UL * g_nmus;
    expr_off = var_off + (unsigned long)MWB_VAR_SIZE * g_nvar;
    opt_off  = expr_off + 2UL * g_nexpr;
    vstr_off = opt_off + 6UL * g_nopt;
    vstr_text = vstr_off + 8UL * g_nvstr;
    code_off = vstr_text;
    for (i = 0; i < g_nvstr; i++)
        code_off += strlen(g_vstr[i].text) + 1;
    code_off = (code_off + 1) & ~1UL;
    end      = code_off + g_ncode;
    g_mwb_size = end;

    f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return 0;
    }
    put32(f, MWB_MAGIC);
    put16(f, MWB_VERSION);
    put16(f, g_nimg);
    put16(f, g_s->nchars);
    put16(f, g_nmus);
    put16(f, g_ntag);
    put16(f, 0);
    put32(f, img_off);
    put32(f, char_off);
    put32(f, mus_off);
    put32(f, code_off);
    put32(f, g_ncode);
    put32(f, g_nsid);
    put32(f, MWB_NLANG);
    put32(f, save_sig());
    put32(f, var_off);
    put32(f, g_nvar);
    put32(f, expr_off);
    put32(f, g_nexpr);
    put32(f, opt_off);
    put32(f, g_nopt);
    put32(f, vstr_off);
    put32(f, g_nvstr);

    for (i = 0; i < g_nimg; i++) {
        const regimg *r = &g_reg[g_img[i].reg];
        put16(f, half_size(r->w));
        put16(f, half_size(r->h));
        put16(f, g_img[i].kind);
        img_filename(i, name);
        putname(f, name, 14);
    }
    for (i = 0; i < g_s->nchars; i++)
        put16(f, g_char_str[i]);
    for (i = 0; i < g_nmus; i++) {
        sprintf(name, "M%02d.SND", i);
        putname(f, name, 14);
        put16(f, 0);
    }
    for (i = 0; i < g_nvar; i++) {
        put32(f, g_var[i].key ? g_var[i].key
                              : fnv(2166136261UL, g_var[i].name,
                                    strlen(g_var[i].name)));
        fputc(g_var[i].persistent ? MWB_VAR_PERSISTENT : 0, f);
        fputc(g_var[i].init.type, f);
        put16(f, 0);
        put32(f, (unsigned long)g_var[i].init.v & 0xFFFFFFFFUL);
    }
    for (i = 0; i < (int)g_nexpr; i++)
        put16(f, g_expr[i]);
    for (i = 0; i < g_nopt; i++) {
        put16(f, g_opt[i].str);
        put16(f, g_opt[i].cond);
        put16(f, g_opt[i].pc);
    }
    for (i = 0; i < g_nvstr; i++) {
        put32(f, fnv(2166136261UL, g_vstr[i].text, strlen(g_vstr[i].text)));
        put32(f, vstr_text);
        vstr_text += strlen(g_vstr[i].text) + 1;
    }
    for (i = 0; i < g_nvstr; i++)
        fwrite(g_vstr[i].text, 1, strlen(g_vstr[i].text) + 1, f);
    if (ftell(f) & 1)
        fputc(0, f);
    fwrite(g_code, 1, g_ncode, f);
    if (ftell(f) != (long)end)
        mr_fatal("layout mismatch: wrote %ld, expected %lu", ftell(f), end);
    fclose(f);

    strcpy(dir, path);
    slash = strrchr(dir, '/');
    if (slash)
        *slash = '\0';
    else
        strcpy(dir, ".");
    for (i = 0; i < MWB_NLANG; i++)
        if (!write_mwt(dir, i))
            return 0;
    for (i = 0; i < g_nbtm; i++)
        if (!write_btm(dir, i))
            return 0;
    return 1;
}

static int write_manifest(const char *path)
{
    char name[16];
    FILE *f = fopen(path, "w");
    int i;

    if (!f) {
        perror(path);
        return 0;
    }
    /* Tab-separated, because the names have spaces:
     *
     *   IMG <out> <kind> <flip> <w> <h> <art> <source>
     *   MUS <out> <source>
     *   BTM <out> <source>        (written beside the script)
     *   TXT <out> <language>      (written beside the script)
     *
     * Sizes are at half scale. <art> is the hand-editable file under art/
     * that the image is converted from: one per source, whatever the flips,
     * so an edit shows up both ways round. A source that is shown as a
     * backdrop anywhere has its art under bg/ even where it is also shown
     * as a sprite (a full-stage `show`), so that it is one file too. */
    for (i = 0; i < g_nimg; i++) {
        const regimg *r = &g_reg[g_img[i].reg];
        const char *base = strrchr(r->file, '/');
        char art[MR_PATH_MAX];
        int j, as_bg = g_img[i].kind == MWB_IMG_BG;

        for (j = 0; j < g_nimg && !as_bg; j++)
            if (g_img[j].kind == MWB_IMG_BG &&
                !strcmp(g_reg[g_img[j].reg].file, r->file))
                as_bg = 1;
        img_filename(i, name);
        snprintf(art, sizeof(art), "%s", base ? base + 1 : r->file);
        /* A zoomed copy is art of its own: foo.png at 125% is
         * foo_x125.png. */
        if (r->zoom) {
            char *dot = strrchr(art, '.');
            if (dot)
                *dot = '\0';
            snprintf(art + strlen(art), sizeof(art) - strlen(art),
                     "_x%d.png", r->zoom);
        }
        fprintf(f, "IMG\t%s\t%s\t%d\t%d\t%d\t%s/%s\t%s\n", name,
                g_img[i].kind == MWB_IMG_BG ? "bg" : "sprite", r->flip,
                half_size(r->w), half_size(r->h), as_bg ? "bg" : "sprite",
                art, r->file);
    }
    for (i = 0; i < g_nmus; i++) {
        sprintf(name, "M%02d.SND", i);
        fprintf(f, "MUS\t%s\t%s\n", name, g_mus[i]);
    }
    /*   BTM <out> <source>        beside the script */
    for (i = 0; i < g_nbtm; i++) {
        sprintf(name, "R%02d.BTM", i);
        fprintf(f, "BTM\t%s\t%s\n", name, g_btm[i].file);
    }
    /*   TXT <out> <language>   beside the script */
    for (i = 0; i < MWB_NLANG; i++)
        fprintf(f, "TXT\t%s\t%s\n", g_lang_file[i],
                g_lang_name[i] ? g_lang_name[i] : "None");
    fclose(f);
    return 1;
}

/* A line indented under one that does not end with a colon: Ren'Py refuses
 * the script (lexer.py:930 expect_noblock, called by every statement that
 * takes no block, e.g. parser.py:818 for `show`), the shared parser skips
 * the line (parse.c:2619). `show park` over `yoffset 400` lost the offset
 * without a word. Python blocks are Python's, not Ren'Py's statements. */
static void colon_check(void)
{
    int f, i;

    for (f = 0; f < g_s->nfiles; f++) {
        mr_lines *l = mr_lex_file(g_s->files[f]);
        int py = -1;    /* indent of the `python:` line we are under */

        if (!l)
            continue;
        for (i = 0; i < l->n; i++) {
            const mr_line *ln = &l->v[i];
            const mr_line *pv = i > 0 ? &l->v[i - 1] : NULL;
            size_t n;

            if (py >= 0 && ln->indent > py)
                continue;
            py = -1;
            if (pv && ln->indent > pv->indent) {
                n = strlen(pv->text);
                if (n == 0 || pv->text[n - 1] != ':') {
                    fprintf(stderr, "%s:%d: error: line is indented, but "
                            "the line before does not take a block "
                            "(forgotten a colon?)\n",
                            g_s->files[f], ln->line);
                    g_errors++;
                }
            }
            n = strlen(ln->text);
            if (n > 0 && ln->text[n - 1] == ':' &&
                (!strncmp(ln->text, "python", 6) ||
                 (!strncmp(ln->text, "init", 4) &&
                  strstr(ln->text, " python"))))
                py = ln->indent;
        }
        mr_lex_free(l);
    }
}

int main(int argc, char **argv)
{
    static const char *const cmdname[MWB_NCMD] = {
        "end", "say", "scene", "show", "hide", "with", "title", "untitle",
        "music", "stopmusic", "window", "pause", "scroll", "skipping",
        "rollback", "fence", "iflang", "goto", "if", "set", "menu", "rhythm"
    };
    mr_script *s;
    int i, start;

    if (argc != 4) {
        fprintf(stderr, "usage: mwc <game-dir> <out.mwb> <manifest.txt>\n");
        return 2;
    }
    g_dir = argv[1];

    if (!mr_vfs_init(NULL, g_dir)) {
        fprintf(stderr, "cannot mount %s\n", g_dir);
        return 1;
    }
    s = mr_script_load(g_dir);
    if (!s) {
        fprintf(stderr, "parse failed\n");
        return 1;
    }
    g_s = s;
    colon_check();

    /* `image` statements are declarations: all of them count, wherever they
     * sit, before the auto-registered files (image.c:540). */
    for (i = 0; i < s->n; i++) {
        const mr_stmt *st = &s->st[i];
        if (st->kind != MR_ST_IMAGE)
            continue;
        if (!st->text || st->anim || st->blur_x != 0.0f ||
            st->blur_y != 0.0f || st->scale_x != 0.0f || st->scale_y != 0.0f)
            err(st, "image '%s': only a file, optionally flipped, is supported",
                st->name);
        else
            reg_define(st->name, st->text, st->aux != 0);
    }
    reg_autodefine();

    start = mr_script_find_label(s, "start");
    if (start < 0)
        mr_fatal("no label start");
    /* Every string is made in both languages as the walk meets it: the
     * English catalogue is the one loaded, the French is the original. */
    mr_tl_set_language(g_lang_name[MWB_LANG_EN]);
    if (!mr_tl_language())
        mr_fatal("no translations under %s/tl/%s", g_dir,
                 g_lang_name[MWB_LANG_EN]);
    /* The story variables first, so that an `if` met before the line that
     * assigns its variable knows it for one. */
    for (i = 0; i < s->n; i++) {
        char name[MR_MAX_NAME];
        const char *rhs;
        if (s->st[i].kind == MR_ST_PYTHON && s->st[i].text &&
            assign_split(s->st[i].text, name, &rhs) && !engine_name(name))
            var_add(&s->st[i], name);
    }
    compile_all(start);

    /* Name plates are `_()` strings (script_declares.rpy). */
    for (i = 0; i < s->nchars && i < 256; i++)
        g_char_str[i] = tl_str(NULL, s->chars[i].who, 0);
    wide_check();

    if (g_errors) {
        fprintf(stderr, "%d error(s)\n", g_errors);
        return 1;
    }
    if (check_paths(start))
        return 1;
    if (g_untranslated)
        printf("%d lines have no English\n", g_untranslated);
    if (!write_mwb(argv[2]) || !write_manifest(argv[3]))
        return 1;

    printf("%d images, %d characters, %d tracks, %d tags\n",
           g_nimg, s->nchars, g_nmus, g_ntag);
    printf("script %lu bytes, code %lu\n", g_mwb_size,
           (unsigned long)g_ncode);
    printf("%d variables, %d strings in them, %lu expression words, "
           "%d options, %d states, %d statements compiled\n",
           g_nvar, g_nvstr, (unsigned long)g_nexpr, g_nopt, g_ncsv, g_nseen);
    for (i = 0; i < MWB_NCMD; i++)
        if (g_ncmd[i])
            printf("  %-10s %lu\n", cmdname[i], g_ncmd[i]);

    /* MWC_TRACE="<lang> [a,b,...]": the path those menu answers take, as
     * the ST's ANSWERS= build traces it. */
    if (getenv("MWC_TRACE")) {
        const char *p = getenv("MWC_TRACE");
        int ans[MENU_MAX], nans = 0, nvis[MENU_MAX + 1], nm, l = atoi(p);
        trace t;

        p = strchr(p, ' ');
        while (p && *p && nans < MENU_MAX) {
            ans[nans++] = atoi(p + 1);
            p = strchr(p + 1, ',');
        }
        memset(&t, 0, sizeof(t));
        g_st_trace = 1;
        g_quiet = 1;
        g_at = malloc((g_ncode / 2 + 1) * sizeof(*g_at));
        g_at_seen = calloc(g_ncode / 2 + 1, 1);
        xrun(l, ans, nans, nvis, &nm, &t);
        for (i = 0; i < t.n; i++)
            printf("path: %s\n", t.l[i]);
        tr_free(&t);
    }
    return 0;
}
