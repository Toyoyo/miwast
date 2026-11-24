/* slparse.c - parsing `screen` blocks into a screen language tree.
 *
 * A screen body is indentation-structured like the rest of Ren'Py. Within a
 * displayable's block, a line is a property when its leading word names one
 * and a child displayable when it names one of those; that is the same rule
 * the reference parser uses, and it is why slang.c carries the full name
 * tables rather than only the names this game happens to use.
 *
 * Property values are taken as raw expression text up to the next property
 * name on the same line, so `text "[x]" xalign 0.5 size 30` splits correctly
 * without needing a Python expression parser at parse time.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "mr.h"
#include "lex.h"
#include "slang.h"
#include "slparse.h"

typedef struct {
    mr_lines *l;
    int       i;
    const char *filename;
} slp;

static char *sl_dup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (!d)
        mr_fatal("out of memory in the screen parser");
    memcpy(d, s, n);
    return d;
}

/* Copy text, trimming whitespace at both ends. */
static char *dup_trim(const char *s, int len)
{
    char *d;
    int start = 0;

    while (start < len && (s[start] == ' ' || s[start] == '\t'))
        start++;
    while (len > start && (s[len - 1] == ' ' || s[len - 1] == '\t'))
        len--;

    d = (char *)malloc((size_t)(len - start) + 1);
    if (!d)
        mr_fatal("out of memory in the screen parser");
    memcpy(d, s + start, (size_t)(len - start));
    d[len - start] = '\0';
    return d;
}

/* Scan forward to the start of the next top-level property name, i.e. a bare
 * word that names a property and sits outside quotes and brackets. Returns
 * the offset of that word, or the length of the string. */
static int next_prop_offset(const char *p)
{
    int depth = 0;
    int instr = 0;
    int i = 0;
    int word_start = -1;

    while (p[i]) {
        char c = p[i];

        if (instr) {
            if (c == '\\' && p[i + 1]) { i += 2; continue; }
            if (c == instr) instr = 0;
            i++;
            continue;
        }

        if (c == '"' || c == '\'') { instr = c; word_start = -1; i++; continue; }
        if (c == '(' || c == '[' || c == '{') { depth++; word_start = -1; i++; continue; }
        if (c == ')' || c == ']' || c == '}') { if (depth) depth--; word_start = -1; i++; continue; }

        if (depth == 0) {
            if (isalpha((unsigned char)c) || c == '_') {
                if (word_start < 0)
                    word_start = i;
            } else if (isalnum((unsigned char)c)) {
                /* digits continue a word but cannot start one */
                if (word_start < 0)
                    word_start = -2;
            } else {
                if (word_start >= 0) {
                    char w[SL_MAX_PROPNAME];
                    int n = i - word_start;
                    if (n < SL_MAX_PROPNAME) {
                        memcpy(w, p + word_start, (size_t)n);
                        w[n] = '\0';
                        /* A property must not be an attribute access like
                         * `a.b`. It may sit at offset 0, which is the usual
                         * case for displayables with no positional argument:
                         * `imagebutton auto "..."`, `null width 10`. */
                        if (sl_is_property(w) &&
                            (word_start == 0 || p[word_start - 1] != '.'))
                            return word_start;
                    }
                }
                word_start = (c == '.') ? -2 : -1;
            }
        }
        i++;
    }

    /* A trailing word at the very end cannot be a property (no value). */
    return i;
}

/* Read `name value` pairs from `p` onto the node. */
static void parse_properties(sl_node *n, const char *p)
{
    char word[SL_MAX_PROPNAME];

    for (;;) {
        const char *after;
        int voff;

        p = mr_skip_ws(p);
        if (!*p)
            return;

        after = mr_next_word(p, word, sizeof(word));
        if (!word[0])
            return;

        if (!sl_is_property(word)) {
            /* Not a property: skip a token and try again rather than
             * silently dropping the rest of the line. */
            if (after == p)
                return;
            p = after;
            continue;
        }

        after = mr_skip_ws(after);
        voff = next_prop_offset(after);

        {
            char *val = dup_trim(after, voff);
            sl_node_set_prop(n, word, val);
            free(val);
        }
        p = after + voff;
    }
}

/* Split a positional argument off the front, returning where it ends. A
 * positional is everything up to the first property name. */
static int positional_len(const char *p)
{
    return next_prop_offset(p);
}

static void parse_block(slp *s, sl_node *parent, int indent);

/* One line, plus any block it opens. */
static void parse_line(slp *s, sl_node *parent, int indent)
{
    const char *line = s->l->v[s->i].text;
    int         srcline = s->l->v[s->i].line;
    char        word[SL_MAX_PROPNAME];
    const char *q;
    int         opens_block;

    q = mr_next_word(line, word, sizeof(word));
    {
        int n = (int)strlen(word);
        while (n > 0 && (word[n - 1] == ':' )) word[--n] = '\0';
    }

    /* Does this line open an indented block? */
    {
        int n = (int)strlen(line);
        while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t')) n--;
        opens_block = (n > 0 && line[n - 1] == ':');
    }

    /* ---- control flow ---- */
    if (!strcmp(word, "if") || !strcmp(word, "elif") ||
        !strcmp(word, "showif")) {
        sl_node *n = sl_node_new(SL_IF, srcline);
        int len = (int)strlen(q);
        while (len > 0 && (q[len - 1] == ':' || q[len - 1] == ' ')) len--;
        n->cond = dup_trim(q, len);
        s->i++;
        parse_block(s, n, indent);

        /* Chain any elif/else that follows at the same indent. */
        if (s->i < s->l->n && s->l->v[s->i].indent == indent) {
            char nw[SL_MAX_PROPNAME];
            mr_next_word(s->l->v[s->i].text, nw, sizeof(nw));
            {
                int k = (int)strlen(nw);
                while (k > 0 && nw[k - 1] == ':') nw[--k] = '\0';
            }
            if (!strcmp(nw, "elif") || !strcmp(nw, "else")) {
                sl_node *save_parent = sl_node_new(SL_DISPLAYABLE, srcline);
                parse_line(s, save_parent, indent);
                if (save_parent->nkids == 1) {
                    n->orelse = save_parent->kids[0];
                    save_parent->nkids = 0;
                }
                sl_node_free(save_parent);
            }
        }
        sl_node_add_kid(parent, n);
        return;
    }

    if (!strcmp(word, "else")) {
        /* Represented as an if with no condition. */
        sl_node *n = sl_node_new(SL_IF, srcline);
        s->i++;
        parse_block(s, n, indent);
        sl_node_add_kid(parent, n);
        return;
    }

    if (!strcmp(word, "for")) {
        sl_node *n = sl_node_new(SL_FOR, srcline);
        const char *in = strstr(q, " in ");
        if (in) {
            int vlen = (int)(in - q);
            char *v = dup_trim(q, vlen);
            int len;
            strncpy(n->var, v, SL_MAX_PROPNAME - 1);
            free(v);
            in += 4;
            len = (int)strlen(in);
            while (len > 0 && (in[len - 1] == ':' || in[len - 1] == ' ')) len--;
            n->cond = dup_trim(in, len);
        }
        s->i++;
        parse_block(s, n, indent);
        sl_node_add_kid(parent, n);
        return;
    }

    if (word[0] == '$' || !strcmp(word, "python")) {
        sl_node *n = sl_node_new(SL_PYTHON, srcline);
        n->arg = sl_dup(mr_skip_ws(line + 1));
        s->i++;
        if (opens_block)
            parse_block(s, n, indent);
        sl_node_add_kid(parent, n);
        return;
    }

    if (!strcmp(word, "default")) {
        sl_node *n = sl_node_new(SL_DEFAULT, srcline);
        n->arg = sl_dup(q);
        s->i++;
        sl_node_add_kid(parent, n);
        return;
    }

    if (!strcmp(word, "use")) {
        sl_node *n = sl_node_new(SL_USE, srcline);
        char name[SL_MAX_TYPE];
        const char *ap = mr_next_word(q, name, sizeof(name));
        char *paren = strchr(name, '(');
        if (paren) *paren = '\0';
        {
            int k = (int)strlen(name);
            while (k > 0 && name[k - 1] == ':') name[--k] = '\0';
        }
        strncpy(n->type, name, SL_MAX_TYPE - 1);
        n->arg = sl_dup(q);
        (void)ap;
        s->i++;
        if (opens_block)
            parse_block(s, n, indent);
        sl_node_add_kid(parent, n);
        return;
    }

    /* `has vbox` turns the enclosing one-child displayable into that
     * container, with the following siblings as its children. */
    if (!strcmp(word, "has")) {
        char name[SL_MAX_TYPE];
        const char *ap = mr_next_word(q, name, sizeof(name));
        sl_node *n;
        {
            int k = (int)strlen(name);
            while (k > 0 && name[k - 1] == ':') name[--k] = '\0';
        }
        n = sl_node_new(SL_DISPLAYABLE, srcline);
        strncpy(n->type, name, SL_MAX_TYPE - 1);
        parse_properties(n, ap);
        s->i++;

        /* `has vbox:` can open a block of its own properties. */
        if (opens_block)
            parse_block(s, n, indent);

        /* Then every following sibling at this indent becomes its child.
         * Going through parse_block rather than parse_line keeps the
         * property-versus-child rule applied. */
        parse_block(s, n, indent - 1);

        sl_node_add_kid(parent, n);
        return;
    }

    /* ---- a displayable ---- */
    {
        sl_node *n = sl_node_new(SL_DISPLAYABLE, srcline);
        const char *rest;
        int plen;

        strncpy(n->type, word, SL_MAX_TYPE - 1);

        rest = mr_skip_ws(q);
        {
            /* Trim the block colon before looking for arguments. */
            int rl = (int)strlen(rest);
            while (rl > 0 && (rest[rl - 1] == ' ' || rest[rl - 1] == '\t'))
                rl--;
            if (rl > 0 && rest[rl - 1] == ':')
                rl--;

            plen = positional_len(rest);
            if (plen > rl)
                plen = rl;

            if (plen > 0) {
                n->arg = dup_trim(rest, plen);
                if (!n->arg[0]) {
                    free(n->arg);
                    n->arg = NULL;
                }
            }

            /* `grid cols rows` takes two positionals. */
            if (n->arg && !strcmp(n->type, "grid")) {
                char *sp = strchr(n->arg, ' ');
                if (sp) {
                    *sp = '\0';
                    n->arg2 = sl_dup(mr_skip_ws(sp + 1));
                }
            }

            if (plen < rl)
                parse_properties(n, rest + plen);
        }

        s->i++;
        if (opens_block)
            parse_block(s, n, indent);

        sl_node_add_kid(parent, n);
    }
}

/* Consume every line indented deeper than `indent`. */
static void parse_block(slp *s, sl_node *parent, int indent)
{
    int block_indent = -1;

    while (s->i < s->l->n) {
        int mi = s->l->v[s->i].indent;

        if (mi <= indent)
            return;
        if (block_indent < 0)
            block_indent = mi;
        if (mi < block_indent)
            return;
        if (mi > block_indent) {
            /* Deeper than expected: attach to the last child if we can. */
            if (parent->nkids > 0)
                parse_block(s, parent->kids[parent->nkids - 1], block_indent);
            else
                s->i++;
            continue;
        }

        {
            int before = s->i;
            const char *line = s->l->v[s->i].text;
            char word[SL_MAX_PROPNAME];

            mr_next_word(line, word, sizeof(word));
            {
                int k = (int)strlen(word);
                while (k > 0 && word[k - 1] == ':') word[--k] = '\0';
            }

            /* A bare `name value` line whose name is a property belongs to
             * the enclosing displayable, not to a new child. */
            if (!sl_is_displayable(word) && sl_is_property(word)) {
                parse_properties(parent, line);
                s->i++;
                continue;
            }

            parse_line(s, parent, block_indent);
            if (s->i == before)
                s->i++;         /* never spin */
        }
    }
}

int mr_sl_parse_screen(mr_lines *l, int *idx, int indent, sl_screen *out)
{
    slp s;
    const char *line = l->v[*idx].text;
    const char *q;
    char name[64];

    memset(out, 0, sizeof(*out));

    q = mr_match_word(line, "screen");
    if (!q)
        return 0;

    /* name and parameter list */
    {
        int n = 0;
        while (q[n] && q[n] != '(' && q[n] != ':' && q[n] != ' ')
            n++;
        if (n > 63)
            n = 63;
        memcpy(name, q, (size_t)n);
        name[n] = '\0';
        strncpy(out->name, name, sizeof(out->name) - 1);
        q += n;
    }

    q = mr_skip_ws(q);
    if (*q == '(') {
        const char *e = ++q;
        while (*e && *e != ')') {
            const char *comma;
            const char *eq;
            int len;

            while (*e == ' ' || *e == ',')
                e++;
            comma = e;
            {
                int depth = 0;
                while (*comma && (depth > 0 || (*comma != ',' && *comma != ')'))) {
                    if (*comma == '(') depth++;
                    else if (*comma == ')') depth--;
                    comma++;
                }
            }
            len = (int)(comma - e);
            if (len > 0 && out->nparams < SL_MAX_PARAMS) {
                eq = memchr(e, '=', (size_t)len);
                if (eq) {
                    char *pn = dup_trim(e, (int)(eq - e));
                    strncpy(out->params[out->nparams], pn,
                            SL_MAX_PROPNAME - 1);
                    free(pn);
                    out->defaults[out->nparams] =
                        dup_trim(eq + 1, (int)(comma - eq - 1));
                } else {
                    char *pn = dup_trim(e, len);
                    strncpy(out->params[out->nparams], pn,
                            SL_MAX_PROPNAME - 1);
                    free(pn);
                }
                out->nparams++;
            }
            e = comma;
            if (*e == ',')
                e++;
        }
    }

    out->root = sl_node_new(SL_DISPLAYABLE, l->v[*idx].line);
    strncpy(out->root->type, "fixed", SL_MAX_TYPE - 1);

    s.l = l;
    s.i = *idx + 1;
    s.filename = NULL;

    parse_block(&s, out->root, indent);

    /* Screen-level keywords sit among the body lines. */
    {
        const char *t = sl_node_prop(out->root, "tag");
        const char *z = sl_node_prop(out->root, "zorder");
        const char *m = sl_node_prop(out->root, "modal");
        if (t) strncpy(out->tag, t, sizeof(out->tag) - 1);
        if (z) out->zorder = sl_dup(z);
        if (m) out->modal = 1;
    }

    *idx = s.i;
    return 1;
}
