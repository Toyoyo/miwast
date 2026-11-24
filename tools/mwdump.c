/* mwdump.c - print a compiled .mwb the way the ST will read it.
 *
 *   mwdump <file.mwb>
 *
 * Deliberately independent of mwc.c: it knows only mwb.h and the bytes, so
 * a disagreement between the two about the format shows up here rather than
 * on the ST. Text is printed with the Atari accents mapped back to UTF-8 and
 * the inline codes as {i} {/i} {b} {/b}, line breaks as |.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/mwb.h"

static unsigned char *g_f;
static long           g_len;

static unsigned r16(unsigned long o)
{
    if ((long)o + 2 > g_len) {
        fprintf(stderr, "read past the end at %lu\n", o);
        exit(1);
    }
    return (g_f[o] << 8) | g_f[o + 1];
}

static unsigned long r32(unsigned long o)
{
    return ((unsigned long)r16(o) << 16) | r16(o + 2);
}

static int s16(unsigned v)
{
    return v >= 0x8000 ? (int)v - 0x10000 : (int)v;
}

/* The .MWT files named after the script, each read whole. */
static unsigned char *g_mwt[MWB_NLANG];
static long           g_mwt_len[MWB_NLANG];
static int            g_nmwt;
static unsigned long  g_nstr;

static const char *const g_hi[] = {
    /* 0x80 */ "Ç", "ü", "é", "â", "ä", "à", "å", "ç",
    /* 0x88 */ "ê", "ë", "è", "ï", "î", "ì", "Ä", "Å",
    /* 0x90 */ "É", "æ", "Æ", "ô", "ö", "ò", "û", "ù",
    /* 0x98 */ "ÿ", "Ö", "Ü"
};

static unsigned long rt32(const unsigned char *b, unsigned long o)
{
    return ((unsigned long)b[o] << 24) | ((unsigned long)b[o + 1] << 16) |
           ((unsigned long)b[o + 2] << 8) | b[o + 3];
}

static void ptext(const unsigned char *p);

/* String `id` in the l-th text file. */
static void pstr_in(int l, unsigned id)
{
    unsigned long so;

    if (l >= g_nmwt) {
        printf("#%u", id);
        return;
    }
    so = id < g_nstr ? rt32(g_mwt[l], 8 + 4UL * id) : 0;
    if (!so || so >= (unsigned long)g_mwt_len[l]) {
        printf("<bad string %u>", id);
        return;
    }
    ptext(g_mwt[l] + so);
}

/* Text in the Atari charset, quoted, as UTF-8 with its codes spelled. */
static void ptext(const unsigned char *p)
{
    putchar('"');
    for (; *p; p++) {
        if (*p == MWB_TXT_ITALIC_ON)       fputs("{i}", stdout);
        else if (*p == MWB_TXT_ITALIC_OFF) fputs("{/i}", stdout);
        else if (*p == MWB_TXT_BOLD_ON)    fputs("{b}", stdout);
        else if (*p == MWB_TXT_BOLD_OFF)   fputs("{/b}", stdout);
        else if (*p == MWB_TXT_FAST)       fputs("{fast}", stdout);
        else if (*p == MWB_TXT_VAR && p[1])
            printf("[v%d]", *++p - MWB_TXT_VAR_BASE);
        else if (*p == '\n')               putchar('|');
        else if (*p >= 0x80 && *p <= 0x9A) fputs(g_hi[*p - 0x80], stdout);
        else if (*p == 0xAE)               fputs("«", stdout);
        else if (*p == 0xAF)               fputs("»", stdout);
        else if (*p == 0xB4)               fputs("œ", stdout);
        else if (*p == 0xB5)               fputs("Œ", stdout);
        else if (*p == 0xB6)               fputs("À", stdout);
        else if (*p < 0x20 || *p >= 0x80)  printf("<%02X>", *p);
        else                               putchar(*p);
    }
    putchar('"');
}

/* The first language's text, and the others' on lines of their own. */
static void pstr(unsigned id)
{
    int l;

    pstr_in(0, id);
    for (l = 1; l < g_nmwt; l++) {
        printf("\n%6s   %s ", "", "  +");
        pstr_in(l, id);
    }
}

/* The value strings: `s<n>` in expressions and values, listed with their
 * text once. */
static unsigned long g_vstr_off, g_nvstr;

/* Expression `w` (a word offset into the table at expr_off), in infix. */
static unsigned long g_expr_off, g_expr_words;

static void pexpr(unsigned w)
{
    static const char *const opname[] = {
        "", "", "", "", "", "", "not", "==", "!=", "<", "<=", ">", ">=",
        "and", "or"
    };
    char st[16][160], t[160];
    int sp = 0;

    for (;;) {
        unsigned op;
        if (w >= g_expr_words || sp >= 16) {
            printf("<bad expression>");
            return;
        }
        op = r16(g_expr_off + 2UL * w++);
        switch (op) {
        case MWB_EX_END:
            printf("%s", sp == 1 ? st[0] : "<bad expression>");
            return;
        case MWB_EX_NONE:  strcpy(st[sp++], "None"); break;
        case MWB_EX_FALSE: strcpy(st[sp++], "False"); break;
        case MWB_EX_TRUE:  strcpy(st[sp++], "True"); break;
        case MWB_EX_INT:
            sprintf(st[sp++], "%d", s16(r16(g_expr_off + 2UL * w++)));
            break;
        case MWB_EX_VAR:
            sprintf(st[sp++], "v%u", r16(g_expr_off + 2UL * w++));
            break;
        case MWB_EX_STR:
            sprintf(st[sp++], "s%u", r16(g_expr_off + 2UL * w++));
            break;
        case MWB_EX_RET:
            sprintf(st[sp++], "_return[%u]", r16(g_expr_off + 2UL * w++));
            break;
        case MWB_EX_LONG:
            sprintf(st[sp++], "%ld",
                    (long)(int)r32(g_expr_off + 2UL * w));
            w += 2;
            break;
        case MWB_EX_NOT:
            if (sp < 1)
                break;
            sprintf(t, "not %.140s", st[sp - 1]);
            strcpy(st[sp - 1], t);
            break;
        default:
            if (op > MWB_EX_OR || sp < 2) {
                printf("<bad expression>");
                return;
            }
            sprintf(t, "(%.70s %s %.70s)", st[sp - 2], opname[op], st[sp - 1]);
            strcpy(st[--sp - 1], t);
            break;
        }
    }
}

static void pval(unsigned type, unsigned long v)
{
    if (type == MWB_V_NONE)       printf("None");
    else if (type == MWB_V_BOOL)  printf(v ? "True" : "False");
    else if (type == MWB_V_STR)   printf("s%lu", v);
    else                          printf("%ld", (long)(int)v);
}

/* `mwdump -i in.MBG|in.MSP out.pam`: decode an image file into a PAM with
 * alpha, black/white/transparent, for comparing with the art it came from. */
static int decode_image(const char *in, const char *out)
{
    unsigned w, h, wpr, flags, x, y;
    unsigned long mask_off, data_off, plane;
    FILE *f;
    size_t l = strlen(in);

    if (l > 4 && !strcmp(in + l - 4, ".MBG")) {
        if (g_len != MWB_BG_BYTES) {
            fprintf(stderr, "%s: %ld bytes, want %d\n", in, g_len,
                    MWB_BG_BYTES);
            return 1;
        }
        w = MWB_STAGE_W;
        h = MWB_STAGE_H;
        wpr = w / 16;
        flags = MWB_SPR_OPAQUE;
        data_off = 0;
        mask_off = 0;
    } else {
        w = r16(0);
        h = r16(2);
        wpr = r16(4);
        flags = r16(6);
        plane = 2UL * wpr * h;
        if (wpr != (w + 15) / 16 ||
            (unsigned long)g_len != MWB_SPRITE_HEADER_SIZE +
                plane * ((flags & MWB_SPR_OPAQUE) ? 1 : 2)) {
            fprintf(stderr, "%s: bad sprite layout\n", in);
            return 1;
        }
        mask_off = MWB_SPRITE_HEADER_SIZE;
        data_off = mask_off + ((flags & MWB_SPR_OPAQUE) ? 0 : plane);
    }

    f = fopen(out, "wb");
    if (!f) {
        perror(out);
        return 1;
    }
    fprintf(f, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 2\nMAXVAL 255\n"
            "TUPLTYPE GRAYSCALE_ALPHA\nENDHDR\n", w, h);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            unsigned long byte = (unsigned long)y * wpr * 2 + x / 8;
            int bit = 0x80 >> (x & 7);
            int on = (flags & MWB_SPR_OPAQUE) || (g_f[mask_off + byte] & bit);
            int black = g_f[data_off + byte] & bit;
            if (black && !on) {
                fprintf(stderr, "%s: data bit outside the mask at %u,%u\n",
                        in, x, y);
                fclose(f);
                return 1;
            }
            fputc(on ? (black ? 0 : 255) : 0, f);
            fputc(on ? 255 : 0, f);
        }
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    static const char *const tname[] = {
        "none", "dissolve", "fade", "move", "moveinleft", "moveinright",
        "moveoutleft", "moveoutright"
    };
    unsigned long img_off, char_off, mus_off, code_off, code_len, o, end;
    unsigned long var_off, opt_off;
    unsigned nimg, nchar, nmus, nvar, nopt, i, last = MWB_END;
    FILE *f;

    const char *in;
    int image = argc == 4 && !strcmp(argv[1], "-i");

    if ((argc < 2 || argc > 2 + MWB_NLANG) && !image) {
        fprintf(stderr, "usage: mwdump <file.mwb> [<text.MWT>...]\n"
                        "       mwdump -i <file.MBG|file.MSP> <out.pam>\n");
        return 2;
    }
    for (i = 0; !image && (int)i + 2 < argc; i++) {
        f = fopen(argv[i + 2], "rb");
        if (!f) {
            perror(argv[i + 2]);
            return 1;
        }
        fseek(f, 0, SEEK_END);
        g_mwt_len[i] = ftell(f);
        fseek(f, 0, SEEK_SET);
        g_mwt[i] = malloc(g_mwt_len[i]);
        if (fread(g_mwt[i], 1, g_mwt_len[i], f) != (size_t)g_mwt_len[i] ||
            rt32(g_mwt[i], 0) != MWB_MWT_MAGIC) {
            fprintf(stderr, "%s: not a text file\n", argv[i + 2]);
            return 1;
        }
        fclose(f);
        g_nmwt++;
    }
    in = image ? argv[2] : argv[1];
    f = fopen(in, "rb");
    if (!f) {
        perror(in);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    g_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_f = malloc(g_len);
    if (fread(g_f, 1, g_len, f) != (size_t)g_len)
        return 1;
    fclose(f);

    if (image)
        return decode_image(in, argv[3]);

    if (r32(0) != MWB_MAGIC || r16(4) != MWB_VERSION) {
        fprintf(stderr, "not an MWB%d file\n", MWB_VERSION);
        return 1;
    }
    nimg = r16(6);
    nchar = r16(8);
    nmus = r16(10);
    img_off = r32(16);
    char_off = r32(20);
    mus_off = r32(24);
    code_off = r32(28);
    code_len = r32(32);
    g_nstr = r32(36);
    var_off = r32(48);
    nvar = (unsigned)r32(52);
    g_expr_off = r32(56);
    g_expr_words = r32(60);
    opt_off = r32(64);
    nopt = (unsigned)r32(68);
    g_vstr_off = r32(72);
    g_nvstr = r32(76);
    if (img_off != MWB_HEADER_SIZE ||
        var_off != mus_off + 16UL * nmus ||
        g_expr_off != var_off + (unsigned long)MWB_VAR_SIZE * nvar ||
        opt_off != g_expr_off + 2UL * g_expr_words ||
        g_vstr_off != opt_off + 6UL * nopt ||
        code_off < g_vstr_off + 8UL * g_nvstr ||
        code_off + code_len != (unsigned long)g_len || (code_off & 1)) {
        fprintf(stderr, "bad layout\n");
        return 1;
    }
    /* Each value string inside the table's text, the text ending at the
     * code, and its hash what the hash says. */
    o = g_vstr_off + 8UL * g_nvstr;
    for (i = 0; i < g_nvstr; i++) {
        unsigned long h = 2166136261UL, at = r32(g_vstr_off + 8UL * i + 4);
        const unsigned char *p;
        if (at != o || at >= code_off) {
            fprintf(stderr, "bad value string %u\n", i);
            return 1;
        }
        for (p = g_f + at; *p && p < g_f + code_off; p++)
            h = ((h ^ *p) * 16777619UL) & 0xFFFFFFFFUL;
        if (p == g_f + code_off || h != r32(g_vstr_off + 8UL * i)) {
            fprintf(stderr, "bad value string %u\n", i);
            return 1;
        }
        o = (unsigned long)(p - g_f) + 1;
    }
    if (((o + 1) & ~1UL) != code_off) {
        fprintf(stderr, "bad layout\n");
        return 1;
    }
    for (i = 0; (int)i < g_nmwt; i++)
        if (((unsigned long)g_mwt[i][4] << 8 | g_mwt[i][5]) != g_nstr) {
            fprintf(stderr, "text file %u does not belong to this script\n",
                    i + 1);
            return 1;
        }

    printf("# %u images, %u characters, %u tracks, %u tags, saves %08lx\n",
           nimg, nchar, nmus, r16(12), r32(44));
    for (i = 0; i < nimg; i++) {
        o = img_off + 20UL * i;
        printf("img %u %-14s %s %ux%u\n", i, (char *)g_f + o + 6,
               r16(o + 4) == MWB_IMG_BG ? "bg    " : "sprite",
               r16(o), r16(o + 2));
    }
    for (i = 0; i < nchar; i++) {
        printf("char %u ", i);
        pstr(r16(char_off + 2UL * i));
        putchar('\n');
    }
    for (i = 0; i < nmus; i++)
        printf("mus %u %s\n", i, (char *)g_f + mus_off + 16UL * i);
    for (i = 0; i < g_nvstr; i++) {
        printf("str s%u %08lx ", i, r32(g_vstr_off + 8UL * i));
        ptext(g_f + r32(g_vstr_off + 8UL * i + 4));
        putchar('\n');
    }
    for (i = 0; i < nvar; i++) {
        o = var_off + (unsigned long)MWB_VAR_SIZE * i;
        printf("var v%u %08lx%s = ", i, r32(o),
               g_f[o + 4] & MWB_VAR_PERSISTENT ? " persistent" : "");
        pval(g_f[o + 5], r32(o + 8));
        putchar('\n');
    }
    for (i = 0; i < nopt; i++) {
        o = opt_off + 6UL * i;
        printf("opt %u -> %u", i, 2 * r16(o + 4));
        if (r16(o + 2) != 0xFFFF) {
            printf(" if ");
            pexpr(r16(o + 2));
        }
        putchar(' ');
        pstr(r16(o));
        putchar('\n');
    }

    o = code_off;
    end = code_off + code_len;
    while (o < end) {
        unsigned op = r16(o);
        printf("%6lu ", o - code_off);
        switch (op) {
        case MWB_END:
            printf("end\n");
            o += 2;
            if (o == end)
                return 0;
            last = op;
            continue;
        case MWB_GOTO:
            printf("goto %u\n", 2 * r16(o + 2));
            o += 4;
            break;
        case MWB_IF:
            printf("if not ");
            pexpr(r16(o + 2));
            printf(" goto %u\n", 2 * r16(o + 4));
            o += 6;
            break;
        case MWB_SET:
            printf("set v%u = ", r16(o + 2));
            pexpr(r16(o + 4));
            putchar('\n');
            o += 6;
            break;
        case MWB_MENU:
            printf("menu opts %u..%u, none offered goto %u, ", r16(o + 6),
                   r16(o + 6) + r16(o + 8) - 1, 2 * r16(o + 10));
            if (r16(o + 4) == 0xFFFF) {
                printf("no caption\n");
            } else {
                if (r16(o + 2) == 0xFFFF)
                    printf("caption - ");
                else
                    printf("caption %u ", r16(o + 2));
                pstr(r16(o + 4));
                putchar('\n');
            }
            o += 12;
            break;
        case MWB_SAY:
            if (r16(o + 2) == 0xFFFF)
                printf("say - ");
            else if (r16(o + 2) == MWB_WHO_CENTERED)
                printf("say centered ");
            else
                printf("say %u ", r16(o + 2));
            pstr(r16(o + 4));
            putchar('\n');
            o += 6;
            break;
        case MWB_SCENE:
            if (r16(o + 2) == 0xFFFF)
                printf("scene black\n");
            else
                printf("scene img %u\n", r16(o + 2));
            o += 4;
            break;
        case MWB_SHOW:
            printf("show tag %u img %u at %d,%d z %u\n", r16(o + 2),
                   r16(o + 4), s16(r16(o + 6)), s16(r16(o + 8)), r16(o + 10));
            o += 12;
            break;
        case MWB_HIDE:
            printf("hide tag %u\n", r16(o + 2));
            o += 4;
            break;
        case MWB_WITH:
            printf("with %s %u %u %u\n",
                   r16(o + 2) < 8 ? tname[r16(o + 2)] : "?",
                   r16(o + 4), r16(o + 6), r16(o + 8));
            o += 10;
            break;
        case MWB_TITLE:
            printf("title ");
            pstr(r16(o + 2));
            putchar('\n');
            o += 4;
            break;
        case MWB_UNTITLE:
            printf("untitle\n");
            o += 2;
            break;
        case MWB_MUSIC:
            printf("music %u flags %u\n", r16(o + 2), r16(o + 4));
            o += 6;
            break;
        case MWB_STOPMUSIC:
            printf("stopmusic\n");
            o += 2;
            break;
        case MWB_WINDOW:
            printf("window %s\n", r16(o + 2) ? "show" : "hide");
            o += 4;
            break;
        case MWB_PAUSE:
            printf("pause %lums%s\n", r32(o + 2), r16(o + 6) ? " hard" : "");
            o += 8;
            break;
        case MWB_SCROLL:
            printf("scroll tag %u img %u x %d y %d z %u to y %d over %lums\n",
                   r16(o + 2), r16(o + 4), s16(r16(o + 6)), s16(r16(o + 8)),
                   r16(o + 10), s16(r16(o + 12)), r32(o + 14));
            o += 18;
            break;
        case MWB_SKIPPING:
            printf("skipping %s\n", r16(o + 2) ? "allowed" : "fenced");
            o += 4;
            break;
        case MWB_ROLLBACK:
            printf("rollback %s\n", r16(o + 2) ? "enabled" : "disabled");
            o += 4;
            break;
        case MWB_FENCE:
            printf("fence\n");
            o += 2;
            break;
        case MWB_IFLANG:
            printf("iflang %u, else skip %u words\n", r16(o + 2), r16(o + 4));
            o += 6;
            break;
        case MWB_RHYTHM:
            printf("rhythm music %u beatmap R%02u.BTM bg img %u notes img "
                   "%u.. with %s %u %u %u\n", r16(o + 2), r16(o + 4),
                   r16(o + 6), r16(o + 8),
                   r16(o + 10) < 8 ? tname[r16(o + 10)] : "?",
                   r16(o + 12), r16(o + 14), r16(o + 16));
            o += 18;
            break;
        default:
            printf("bad opcode %u\n", op);
            return 1;
        }
        last = op;
    }
    /* Branches put code after an `end`; the code may finish on a goto,
     * but nothing may run off the end. */
    if (last != MWB_GOTO) {
        printf("# the last command runs off the end of the code\n");
        return 1;
    }
    return 0;
}
