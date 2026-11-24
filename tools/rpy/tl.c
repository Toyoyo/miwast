/* tl.c - computing translation identifiers, and looking translations up.
 * See tl.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "tl.h"
#include "md5.h"
#include "lex.h"
#include "vfs.h"
#include "expr.h"
#include "script.h"

/* --- identifiers ---
 *
 * Ren'Py digests the statement's *source* form, which it regenerates from the
 * parsed statement rather than quoting the file: speaker, then the dialogue
 * re-escaped, then a trailing `with` clause, joined with single spaces, then
 * CRLF. Reproducing that regeneration is what makes the digests match, so the
 * re-escaping has to be exactly Ren'Py's encode_say_string.
 */

/* Ren'Py's encode_say_string: backslash, newline and quote are escaped, and a
 * space that follows another space becomes `\ ` -- otherwise reparsing would
 * collapse it, since Ren'Py folds runs of whitespace in dialogue. */
static int encode_say_string(const char *s, char *out, int outsz)
{
    int n = 0;
    int prev_space = 0;

#define PUT(ch) do { if (n < outsz - 1) out[n++] = (ch); } while (0)

    PUT('"');
    for (; *s; s++) {
        switch (*s) {
        case '\\': PUT('\\'); PUT('\\'); break;
        case '\n': PUT('\\'); PUT('n');  break;
        case '"':  PUT('\\'); PUT('"');  break;
        case ' ':
            if (prev_space) {
                PUT('\\');
                PUT(' ');
            } else {
                PUT(' ');
            }
            break;
        default:
            PUT(*s);
            break;
        }
        prev_space = (*s == ' ');
    }
    PUT('"');
#undef PUT

    out[n] = '\0';
    return n;
}

void mr_tl_identifier(const char *label, const char *who, const char *what,
                      const char *with_, int nointeract, int dup,
                      char *out, int outsz)
{
    char code[MR_MAX_TEXT * 2 + 160];
    char lab[MR_MAX_NAME];
    char digest[9];
    char suffix[12];
    int  n = 0;

    if (who && *who)
        n += sprintf(code, "%s ", who);
    n += encode_say_string(what ? what : "", code + n,
                           (int)sizeof(code) - n - 48);
    /* Before the `with` clause, which get_code emits last (ast.py:941). */
    if (nointeract)
        n += sprintf(code + n, " nointeract");
    if (with_ && *with_)
        n += sprintf(code + n, " with %.32s", with_);

    /* Ren'Py digests the reconstructed line with a CRLF appended. */
    strcpy(code + n, "\r\n");
    mr_md5_hex(code, (long)(n + 2), digest, 8);

    /* A label with dots in it -- `chap3.part2` -- becomes underscores, since
     * the identifier has to be a bare word. */
    lab[0] = '\0';
    if (label && *label) {
        char *d;
        strncpy(lab, label, sizeof(lab) - 1);
        lab[sizeof(lab) - 1] = '\0';
        for (d = lab; *d; d++)
            if (*d == '.')
                *d = '_';
    }

    if (dup > 0)
        sprintf(suffix, "_%d", dup);
    else
        suffix[0] = '\0';

    if ((int)(strlen(lab) + strlen(digest) + strlen(suffix) + 2) > outsz) {
        mr_log("tl: identifier for label %s does not fit", lab);
        out[0] = '\0';
        return;
    }

    if (lab[0])
        sprintf(out, "%s_%s%s", lab, digest, suffix);
    else
        sprintf(out, "%s%s", digest, suffix);
}

/* --- the catalogue ---
 *
 * Keys are stored as a pair of 32-bit hashes rather than as the key text. The
 * identifiers and the original strings both already exist in memory -- in the
 * script -- and a second copy of the string catalogue's keys would be another
 * quarter megabyte on a machine that has sixteen. Two independent hashes make
 * a collision over a few thousand keys a non-problem, where one would not.
 */

typedef struct {
    mr_u32 h1, h2;
    char  *s;          /* the translation, owned */
} tl_entry;

typedef struct {
    tl_entry *v;
    int       n, cap;
} tl_map;

static tl_map g_dlg;   /* keyed on the translation identifier */
static tl_map g_str;   /* keyed on the original `old` string */

static char g_lang[32];
static int  g_have_lang;

/* Whether a catalogue has been loaded at all, so that selecting the original
 * language for the first time still reads tl/None/ rather than being taken
 * for "already there". */
static int  g_loaded;

/* FNV-1a and djb2. */
static void hash_of(const char *s, mr_u32 *h1, mr_u32 *h2)
{
    mr_u32 a = 2166136261u;
    mr_u32 b = 5381u;

    while (*s) {
        unsigned char c = (unsigned char)*s++;
        a = (a ^ c) * 16777619u;
        b = b * 33u + c;
    }
    *h1 = a;
    *h2 = b;
}

static int entry_cmp(const void *pa, const void *pb)
{
    const tl_entry *a = (const tl_entry *)pa;
    const tl_entry *b = (const tl_entry *)pb;

    if (a->h1 != b->h1)
        return a->h1 < b->h1 ? -1 : 1;
    if (a->h2 != b->h2)
        return a->h2 < b->h2 ? -1 : 1;
    return 0;
}

static void map_add(tl_map *m, const char *key, const char *val)
{
    tl_entry *e;
    size_t n;

    if (!key || !key[0])
        return;

    if (m->n >= m->cap) {
        int nc = m->cap ? m->cap * 2 : 512;
        void *nv = realloc(m->v, (size_t)nc * sizeof(tl_entry));
        if (!nv)
            mr_fatal("out of memory growing the translation catalogue");
        m->v = (tl_entry *)nv;
        m->cap = nc;
    }

    e = &m->v[m->n++];
    hash_of(key, &e->h1, &e->h2);
    n = strlen(val) + 1;
    e->s = (char *)malloc(n);
    if (!e->s)
        mr_fatal("out of memory storing a %lu byte translation",
                 (unsigned long)n);
    memcpy(e->s, val, n);
}

static void map_free(tl_map *m)
{
    int i;
    for (i = 0; i < m->n; i++)
        free(m->v[i].s);
    free(m->v);
    m->v = NULL;
    m->n = m->cap = 0;
}

static const char *map_find(const tl_map *m, const char *key)
{
    mr_u32 h1, h2;
    int lo = 0, hi = m->n - 1;

    if (!m->n || !key || !key[0])
        return NULL;

    hash_of(key, &h1, &h2);

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        const tl_entry *e = &m->v[mid];

        if (e->h1 == h1 && e->h2 == h2)
            return e->s;
        if (e->h1 < h1 || (e->h1 == h1 && e->h2 < h2))
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

/* --- parsing a tl file ---
 *
 * The main lexer is no use here: a translate block's body is a say statement
 * whose speaker is a character variable, which the lexer's parser would try to
 * execute, and the identifier lives on the `translate` line itself. The format
 * is regular enough to read line by line.
 */

/* The text of the say statement in a translate block: an optional speaker,
 * then a quoted string. Escapes are resolved by mr_read_string, the same
 * function the script parser uses, so the result is comparable with the
 * dialogue the script holds. */
static int say_text(const char *line, char *out, int outsz)
{
    const char *p = mr_skip_ws(line);

    /* Through mr_string_quote, so a translator who writes u"..." is read as
     * a string rather than as a speaker called `u` followed by one. */
    if (!mr_string_quote(p, NULL)) {
        char word[64];
        p = mr_skip_ws(mr_next_word(p, word, sizeof(word)));
        if (!mr_string_quote(p, NULL))
            return 0;
    }
    /* Anything after the closing quote -- `nointeract` on a menu caption's
     * line -- is not part of the text and is ignored here. */
    return mr_read_string(p, out, outsz) != NULL;
}

/* `translate english chap0_800d8779:` -> id "chap0_800d8779".
 * Returns 0 if the line is not a translate header for this language. */
static int translate_header(const char *line, const char *lang,
                            char *id, int idsz)
{
    const char *p = mr_match_word(line, "translate");
    char word[64];
    int n;

    if (!p)
        return 0;

    p = mr_next_word(p, word, sizeof(word));
    if (strcmp(word, lang))
        return 0;

    mr_next_word(p, id, idsz);
    n = (int)strlen(id);
    while (n > 0 && (id[n - 1] == ':' || id[n - 1] == ' '))
        id[--n] = '\0';
    return 1;
}

static void load_file(const char *path, const char *lang)
{
    char *buf = mr_vfs_read_all(path, NULL);
    char *p;
    enum { M_NONE, M_DIALOGUE, M_STRINGS } mode = M_NONE;
    char id[MR_MAX_NAME];
    char pending[MR_MAX_TEXT];
    int  before = g_dlg.n + g_str.n;

    if (!buf) {
        mr_log("tl: could not read %s", path);
        return;
    }

    /* These files are shipped with a UTF-8 byte order mark. */
    p = buf;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
        (unsigned char)p[2] == 0xBF)
        p += 3;

    id[0] = '\0';
    pending[0] = '\0';

    while (*p) {
        char *eol = strchr(p, '\n');
        char *line = p;
        const char *t;
        char header[MR_MAX_NAME];
        int  len;

        if (eol) {
            *eol = '\0';
            p = eol + 1;
        } else {
            p = line + strlen(line);
        }

        len = (int)strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ' ||
                           line[len - 1] == '\t'))
            line[--len] = '\0';

        t = mr_skip_ws(line);
        if (!*t)
            continue;

        if (translate_header(t, lang, header, sizeof(header))) {
            if (!strcmp(header, "strings")) {
                mode = M_STRINGS;
                id[0] = '\0';
            } else {
                mode = M_DIALOGUE;
                strcpy(id, header);
            }
            pending[0] = '\0';
            continue;
        }

        /* Comments carry the original text and the source location; neither is
         * needed, since the identifier is the key. */
        if (*t == '#')
            continue;

        if (mode == M_STRINGS) {
            const char *r;
            if ((r = mr_match_word(t, "old")) != NULL) {
                if (!mr_read_string(r, pending, sizeof(pending)))
                    pending[0] = '\0';
            } else if ((r = mr_match_word(t, "new")) != NULL) {
                char val[MR_MAX_TEXT];
                if (pending[0] && mr_read_string(r, val, sizeof(val)))
                    map_add(&g_str, pending, val);
                pending[0] = '\0';
            }
            continue;
        }

        if (mode == M_DIALOGUE && id[0]) {
            char val[MR_MAX_TEXT];
            if (say_text(t, val, sizeof(val)))
                map_add(&g_dlg, id, val);
            /* One say per block; anything further belongs to no identifier. */
            id[0] = '\0';
        }
    }

    free(buf);
    mr_log("tl: %s, %d entries", path, g_dlg.n + g_str.n - before);
}

/* --- language selection --- */

const char *mr_tl_language(void)
{
    return g_have_lang ? g_lang : NULL;
}

/* Read every .rpy and .rpym under tl/<name>/ into the two maps. `.rpym` is
 * included because tl/None/ uses it -- common.rpym is where a game's
 * translations of Ren'Py's own strings live. */
static void load_catalogue(const char *name)
{
    static const char *exts[2] = { ".rpy", ".rpym" };
    char dir[64];
    int  e, i;

    sprintf(dir, "tl/%.32s/", name);
    for (e = 0; e < 2; e++) {
        for (i = 0;; i++) {
            char path[MR_PATH_MAX];
            if (!mr_vfs_enum(dir, exts[e], i, path, sizeof(path)))
                break;
            load_file(path, name);
        }
    }
}

void mr_tl_set_language(const char *lang)
{

    if (lang && (!*lang || !strcmp(lang, "None")))
        lang = NULL;

    if (!lang && !g_have_lang && g_loaded)
        return;
    if (lang && g_have_lang && !strcmp(lang, g_lang))
        return;

    map_free(&g_dlg);
    map_free(&g_str);
    g_have_lang = 0;
    g_lang[0] = '\0';

    g_loaded = 1;

    if (!lang) {
        /* The original language still has a catalogue, and skipping it was a
         * real gap rather than a shortcut.
         *
         * Ren'Py keys string translations by language with None a valid key
         * (translation/__init__.py:615, `strings[language]`), and `tl/None/`
         * is where they go. That is how a game written in French translates
         * Ren'Py's *own* English strings -- the overwrite prompt, "Quick save
         * complete.", the page names -- which are English in the engine
         * whatever the game is written in. This game does exactly that in
         * tl/None/common.rpym.
         *
         * Loading nothing here meant every engine-provided string appeared
         * in English in the French game. The dialogue translations in such a
         * directory, if any, are harmless: a line's own text is already the
         * original.
         *
         * g_have_lang stays 0, because the language in force really is the
         * script's own -- the flag buttons and `_preferences.language` must
         * keep seeing None. */
        load_catalogue("None");
        /* Sorted here as well as below: map_find is a binary search, and
         * returning from this branch without sorting left the catalogue
         * loaded but unsearchable -- every lookup missed and every string
         * came back in English, which looks exactly like not having loaded
         * it at all. */
        qsort(g_dlg.v, (size_t)g_dlg.n, sizeof(tl_entry), entry_cmp);
        qsort(g_str.v, (size_t)g_str.n, sizeof(tl_entry), entry_cmp);
        mr_log("tl: original language, %d strings from tl/None/", g_str.n);
        return;
    }

    strncpy(g_lang, lang, sizeof(g_lang) - 1);
    g_lang[sizeof(g_lang) - 1] = '\0';
    g_have_lang = 1;

    load_catalogue(g_lang);

    if (!g_dlg.n && !g_str.n) {
        mr_log("tl: no translations found under tl/%s/", g_lang);
        g_have_lang = 0;
        g_lang[0] = '\0';
        return;
    }

    /* Sorted once, then only ever searched. */
    qsort(g_dlg.v, (size_t)g_dlg.n, sizeof(tl_entry), entry_cmp);
    qsort(g_str.v, (size_t)g_str.n, sizeof(tl_entry), entry_cmp);

    mr_log("tl: %s loaded, %d lines and %d strings",
           g_lang, g_dlg.n, g_str.n);
}

int mr_tl_sync(void)
{
    mr_value v;
    const char *want = NULL;

    if (mr_var_get("_preferences.language", &v) && v.type == MR_V_STR)
        want = v.s;

    /* `g_loaded` and not just `!g_have_lang`: the original language has a
     * catalogue of its own now (tl/None/), so the very first sync has to
     * reach mr_tl_set_language even when there is no language preference
     * set. Without it the engine's own strings stayed untranslated for
     * exactly the players who never touch the language buttons. */
    if (!want && !g_have_lang && g_loaded)
        return 0;
    if (want && g_have_lang && !strcmp(want, g_lang))
        return 0;

    mr_tl_set_language(want);
    return 1;
}

/* --- lookup --- */

const char *mr_tl_dialogue(const char *tlid, const char *orig)
{
    const char *r;

    if (!g_have_lang || !tlid)
        return orig;

    r = map_find(&g_dlg, tlid);
    return r ? r : orig;
}

/* Ren'Py's `{#...}` tag disambiguates two strings that are identical in the
 * source language but want different translations -- `{#auto_page}A` and
 * `{#quick_page}A`. It is part of the key, and it is not drawn, the text
 * renderer skipping it like any other tag. Strip every occurrence, stopping at
 * a newline: Ren'Py's regex is `\{\#.*?\}`, and `.` does not match one.
 * Returns 0 if there was nothing to strip, so the caller can skip a second
 * lookup that would ask the same question. */
static int strip_hash_tags(const char *s, char *out, int outsz)
{
    int n = 0, found = 0;

    while (*s && n < outsz - 1) {
        if (s[0] == '{' && s[1] == '#') {
            const char *e = s + 2;
            while (*e && *e != '}' && *e != '\n')
                e++;
            if (*e == '}') {
                s = e + 1;
                found = 1;
                continue;
            }
        }
        out[n++] = *s++;
    }
    out[n] = '\0';
    return found;
}

const char *mr_tl_string(const char *orig)
{
    const char *r;
    char notags[MR_MAX_TEXT];

    /* Not gated on a language being selected: the original language has a
     * catalogue too, tl/None/, and that is where a game translates Ren'Py's
     * own English strings into the language it is written in. An empty map
     * simply misses, so the check this replaced bought nothing and cost
     * every engine string its translation for players who never touched the
     * language buttons. */
    if (!orig)
        return orig;

    r = map_find(&g_str, orig);
    if (r)
        return r;

    /* A catalogue may carry the string without the tag the source wrote, so
     * Ren'Py asks a second time with the tags removed before giving up. */
    if (strip_hash_tags(orig, notags, sizeof(notags))) {
        r = map_find(&g_str, notags);
        if (r)
            return r;
    }

    return orig;
}

void mr_tl_shutdown(void)
{
    map_free(&g_dlg);
    map_free(&g_str);
    g_have_lang = 0;
    g_lang[0] = '\0';
}
