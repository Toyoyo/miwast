/* lex.c - .rpy files to logical lines, plus the small token readers the
 * parser needs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "mr.h"
#include "lex.h"
#include "vfs.h"

static int lines_push(mr_lines *l, char *text, int indent, int line)
{
    if (l->n >= l->cap) {
        int nc = l->cap ? l->cap * 2 : 512;
        mr_line *nv = (mr_line *)realloc(l->v, (size_t)nc * sizeof(mr_line));
        if (!nv)
            return 0;
        l->v = nv;
        l->cap = nc;
    }
    l->v[l->n].text = text;
    l->v[l->n].indent = indent;
    l->v[l->n].line = line;
    l->n++;
    return 1;
}

mr_lines *mr_lex_file(const char *path)
{
    long size;
    char *buf;
    mr_lines *l;
    char *p, *end;
    int lineno;

    buf = mr_vfs_read_all(path, &size);
    if (!buf) {
        mr_log("lex: cannot open %s", path);
        return NULL;
    }

    l = (mr_lines *)calloc(1, sizeof(mr_lines));
    if (!l) {
        free(buf);
        return NULL;
    }
    l->buf = buf;

    p = buf;
    end = buf + size;

    /* A UTF-8 BOM opens script.rpy in this game. */
    if (size >= 3 && (unsigned char)p[0] == 0xEF &&
        (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
        p += 3;

    lineno = 1;

    while (p < end) {
        char *linestart;
        char *out;
        int indent;
        int startline;
        int depth;     /* unclosed ( [ { */
        int instr;     /* quote character we are inside, else 0 */

        /* Measure the indent. */
        indent = 0;
        while (p < end && (*p == ' ' || *p == '\t')) {
            indent += (*p == '\t') ? 8 : 1;
            p++;
        }

        startline = lineno;
        linestart = p;
        out = p;        /* we rewrite in place; output never outruns input */
        depth = 0;
        instr = 0;

        for (;;) {
            if (p >= end)
                break;

            if (*p == '\r') {
                p++;
                continue;
            }

            if (*p == '\n') {
                lineno++;
                p++;
                /* Inside brackets, or after a trailing backslash, the logical
                 * line continues. Ren'Py only needs this for the multi-line
                 * Character() and define statements. */
                if (depth > 0) {
                    /* Collapse the newline and the next line's indent into a
                     * single space so tokens do not run together. */
                    *out++ = ' ';
                    while (p < end && (*p == ' ' || *p == '\t'))
                        p++;
                    continue;
                }
                break;
            }

            if (instr) {
                if (*p == '\\' && p + 1 < end) {
                    /* Keep escapes intact for mr_read_string to resolve. */
                    *out++ = *p++;
                    *out++ = *p++;
                    continue;
                }
                if (*p == instr)
                    instr = 0;
                *out++ = *p++;
                continue;
            }

            if (*p == '"' || *p == '\'') {
                instr = *p;
                *out++ = *p++;
                continue;
            }

            if (*p == '#') {
                /* Comment to end of line. Do not consume the newline here;
                 * the loop above handles it and keeps lineno correct. */
                while (p < end && *p != '\n')
                    p++;
                continue;
            }

            if (*p == '(' || *p == '[' || *p == '{')
                depth++;
            else if (*p == ')' || *p == ']' || *p == '}') {
                if (depth > 0)
                    depth--;
            } else if (*p == '\\' && p + 1 < end && p[1] == '\n') {
                /* Explicit continuation. */
                p += 2;
                lineno++;
                *out++ = ' ';
                while (p < end && (*p == ' ' || *p == '\t'))
                    p++;
                continue;
            }

            *out++ = *p++;
        }

        /* Trim trailing whitespace and terminate. */
        while (out > linestart && (out[-1] == ' ' || out[-1] == '\t'))
            out--;
        *out = '\0';

        if (*linestart) {
            if (!lines_push(l, linestart, indent, startline)) {
                mr_lex_free(l);
                return NULL;
            }
        }
    }

    return l;
}

void mr_lex_free(mr_lines *l)
{
    if (!l)
        return;
    free(l->v);
    free(l->buf);
    free(l);
}

/* --- token helpers --- */

const char *mr_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

const char *mr_next_word(const char *p, char *out, int outsz)
{
    const char *start;
    int n;

    p = mr_skip_ws(p);
    start = p;
    while (*p && *p != ' ' && *p != '\t')
        p++;

    n = (int)(p - start);
    if (n >= outsz)
        n = outsz - 1;
    memcpy(out, start, (size_t)n);
    out[n] = '\0';
    return p;
}

static int word_char(int c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '.';
}

const char *mr_match_word(const char *p, const char *word)
{
    size_t n;

    p = mr_skip_ws(p);
    n = strlen(word);
    if (strncmp(p, word, n) != 0)
        return NULL;
    /* Must be a whole word, so `at` does not match inside `attack`. */
    if (word_char((unsigned char)p[n]))
        return NULL;
    return mr_skip_ws(p + n);
}

const char *mr_string_quote(const char *p, int *raw)
{
    int n;

    if (raw)
        *raw = 0;
    p = mr_skip_ws(p);

    /* At most two prefix letters -- u, b, r, f and the pairs rb/br/fr/rf --
     * and no space between them or before the quote, so this cannot swallow
     * an identifier that merely starts with one of the letters. */
    for (n = 0; n < 2; n++) {
        char c = *p;
        if (c != 'u' && c != 'U' && c != 'b' && c != 'B' &&
            c != 'r' && c != 'R' && c != 'f' && c != 'F')
            break;
        if (p[1] != '"' && p[1] != '\'' &&
            p[1] != 'u' && p[1] != 'U' && p[1] != 'b' && p[1] != 'B' &&
            p[1] != 'r' && p[1] != 'R' && p[1] != 'f' && p[1] != 'F')
            break;
        if (raw && (c == 'r' || c == 'R'))
            *raw = 1;
        p++;
    }

    if (*p != '"' && *p != '\'') {
        if (raw)
            *raw = 0;
        return NULL;
    }
    return p;
}

const char *mr_read_string(const char *p, char *out, int outsz)
{
    char quote;
    int n;
    int truncated = 0;
    int raw;

    p = mr_string_quote(p, &raw);
    if (!p)
        return NULL;
    quote = *p++;

    n = 0;
    while (*p && *p != quote) {
        /* A raw string keeps its backslashes, which is the whole reason the
         * prefix exists; only the quote-escape still ends up consuming one. */
        if (raw && *p == '\\' && p[1]) {
            if (n < outsz - 2) { out[n++] = '\\'; out[n++] = p[1]; }
            p += 2;
            continue;
        }
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n':  if (n < outsz - 1) out[n++] = '\n'; break;
            case 't':  if (n < outsz - 1) out[n++] = '\t'; break;
            case '\\': if (n < outsz - 1) out[n++] = '\\'; break;
            case '"':  if (n < outsz - 1) out[n++] = '"';  break;
            case '\'': if (n < outsz - 1) out[n++] = '\''; break;
            case ' ':  if (n < outsz - 1) out[n++] = ' ';  break;
            default:
                /* Ren'Py leaves unknown escapes as the literal character. */
                if (n < outsz - 1) out[n++] = *p;
                break;
            }
            p++;
            continue;
        }
        if (n < outsz - 1)
            out[n++] = *p;
        else
            truncated = 1;
        p++;
    }
    out[n] = '\0';

    /* Say so. Every caller passes a fixed buffer -- MR_MAX_TEXT for dialogue,
     * MR_VSTR_MAX for a string value -- and dropping the tail quietly means a
     * long line of dialogue simply ends early on screen with nothing to
     * connect it to a limit. This runs at parse time, so it cannot flood. */
    if (truncated)
        mr_log("lex: string longer than %d bytes, truncated to '%.40s...'",
               outsz - 1, out);

    if (*p == quote)
        p++;
    return mr_skip_ws(p);
}

/* Words that end an image name in a show/scene/hide statement. */
static int is_terminator(const char *w)
{
    return !strcmp(w, "at") || !strcmp(w, "with") || !strcmp(w, "behind") ||
           !strcmp(w, "as") || !strcmp(w, "onlayer") || !strcmp(w, "zorder");
}

const char *mr_read_imagename(const char *p, char *out, int outsz)
{
    char word[64];
    const char *next;
    int n;

    p = mr_skip_ws(p);
    n = 0;
    out[0] = '\0';

    /* A quoted name is taken whole. */
    if (*p == '"' || *p == '\'')
        return mr_read_string(p, out, outsz);

    for (;;) {
        if (!*p || *p == ':')
            break;

        next = mr_next_word(p, word, sizeof(word));
        if (!word[0])
            break;
        if (is_terminator(word))
            break;

        if (n > 0 && n < outsz - 1)
            out[n++] = ' ';
        {
            int wl = (int)strlen(word);
            if (n + wl > outsz - 1)
                wl = outsz - 1 - n;
            memcpy(out + n, word, (size_t)wl);
            n += wl;
        }
        p = next;
    }

    out[n] = '\0';
    return mr_skip_ws(p);
}

const char *mr_read_number(const char *p, double *out)
{
    char *endp;
    double v;

    p = mr_skip_ws(p);
    v = strtod(p, &endp);
    if (endp == p)
        return NULL;
    *out = v;
    return mr_skip_ws(endp);
}

const char *mr_read_arg(const char *p, char *out, int outsz)
{
    int depth = 0, instr = 0, n = 0;

    p = mr_skip_ws(p);
    while (*p) {
        char c = *p;

        if (instr) {
            if (c == '\\' && p[1]) {
                if (n < outsz - 2) { out[n++] = c; out[n++] = p[1]; }
                p += 2;
                continue;
            }
            if (c == instr)
                instr = 0;
        } else {
            if (c == '"' || c == '\'') instr = c;
            else if (c == '(' || c == '[' || c == '{') depth++;
            else if (c == ')' || c == ']' || c == '}') {
                if (depth == 0)
                    break;
                depth--;
            } else if (c == ',' && depth == 0) {
                break;
            }
        }
        if (n < outsz - 1)
            out[n++] = c;
        p++;
    }

    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t'))
        n--;
    out[n] = '\0';

    if (*p == ',')
        p++;
    return mr_skip_ws(p);
}
