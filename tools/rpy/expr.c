/* expr.c - variable store and recursive-descent expression evaluator.
 *
 * Grammar, in precedence order:
 *
 *   or_expr   := and_expr ( 'or' and_expr )*
 *   and_expr  := not_expr ( 'and' not_expr )*
 *   not_expr  := 'not' not_expr | comparison
 *   comparison:= sum ( ('=='|'!='|'<'|'<='|'>'|'>=') sum )?
 *   sum       := term ( ('+'|'-') term )*
 *   term      := unary ( ('*'|'/'|'%') unary )*
 *   unary     := '-' unary | atom
 *   atom      := number | string | name [ '(' args ')' ] | '(' or_expr ')'
 *              | 'True' | 'False' | 'None'
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "mr.h"
#include "expr.h"
#include "list.h"
#include "lex.h"      /* mr_match_word, for the `for`/`in` of a comprehension */
#include "tl.h"       /* mr_tl_string, for the `!t` conversion */

#define MR_MAX_VARS 256

typedef struct mr_var {
    char     name[64];
    mr_value v;
} mr_var;

static mr_var    g_vars[MR_MAX_VARS];
static int       g_nvars;

/* The variable table as it stood once the init phase had run: the gui.*,
 * config.* and `define` values, which belong to the game rather than to a
 * playthrough. Loading a save restores this and then applies the save on top,
 * so that nothing from wherever the player happened to be leaks into the
 * loaded game -- a `config.rollback_enabled = False` from the ending once
 * survived a load and silently disabled rollback. */
static mr_var    g_base[MR_MAX_VARS];
static int       g_nbase;
static mr_call_fn g_call;

/* --- values --- */

int mr_value_truthy(const mr_value *v)
{
    switch (v->type) {
    case MR_V_INT:   return v->i != 0;
    case MR_V_FLOAT: return v->f != 0.0;
    case MR_V_STR:   return v->s[0] != '\0';
    default:         return 0;
    }
}

void mr_value_set_int(mr_value *v, long i)
{
    memset(v, 0, sizeof(*v));
    v->type = MR_V_INT;
    v->i = i;
}

void mr_value_set_float(mr_value *v, double f)
{
    memset(v, 0, sizeof(*v));
    v->type = MR_V_FLOAT;
    v->f = f;
    v->i = (long)f;
}

/* A string value that did not fit.
 *
 * Reported rather than dropped quietly, because the failure looks like
 * anything but a length limit: a name or a label comes out with its tail
 * missing and the game carries on. Unlike the parse-time limits this can be
 * reached every frame -- a screen building a string per button -- so it is
 * latched after a few, which is enough to diagnose without filling log.txt. */
static void vstr_truncated(const char *s)
{
    static int said;

    if (said >= 4)
        return;
    said++;
    mr_log("expr: string longer than %d bytes, truncated: '%.40s...'%s",
           MR_VSTR_MAX - 1, s,
           said == 4 ? " (further ones not reported)" : "");
}

void mr_value_set_str(mr_value *v, const char *s)
{
    memset(v, 0, sizeof(*v));
    v->type = MR_V_STR;
    strncpy(v->s, s, MR_VSTR_MAX - 1);

    /* memchr rather than strlen: bounded, and all that is being asked is
     * whether a terminator appears within the space available. */
    if (!memchr(s, '\0', MR_VSTR_MAX))
        vstr_truncated(s);
}

static double value_num(const mr_value *v)
{
    if (v->type == MR_V_INT)
        return (double)v->i;
    if (v->type == MR_V_FLOAT)
        return v->f;
    return 0.0;
}

/* --- variables --- */

void mr_expr_init(mr_call_fn call)
{
    g_nvars = 0;
    g_call = call;
}

void mr_expr_shutdown(void)
{
    g_nvars = 0;
    g_call = NULL;
}

/* `preferences` and `_preferences` are the same object in Ren'Py, and scripts
 * use both spellings for the same setting -- options.rpy writes
 * `preferences.text_cps` while the screens read `_preferences.text_cps`. The
 * store has no notion of objects, so the two spellings are folded together
 * here rather than silently becoming two unrelated variables. */
static const char *normalise(const char *name, char *buf, int bufsz)
{
    if (!strncmp(name, "preferences.", 12)) {
        if ((int)strlen(name) + 2 < bufsz) {
            buf[0] = '_';
            strcpy(buf + 1, name);
            return buf;
        }
    }
    return name;
}

/* Names that are not part of a playthrough, and so take no part in the
 * baseline. The interpreter installs mr_persist_is_persistent here; expr.c
 * does not call it directly because not every harness links persist.c. */
static mr_exempt_fn g_exempt;

void mr_var_set_baseline_exempt(mr_exempt_fn fn)
{
    g_exempt = fn;
}

void mr_var_mark_baseline(void)
{
    int i;

    /* Persistent data is deliberately left out.
     *
     * It used to be copied in wholesale, and that quietly resurrected it: the
     * baseline is taken at init, *after* the persistent file has been read,
     * so it held whatever was unlocked at startup. Clearing the persistent
     * data from the preferences emptied the live store but not the baseline,
     * and the next `Start()` -- which resets to the baseline -- brought every
     * unlock back. It looked as though setting one flag set another, because
     * the second was already back before the first was ever assigned. */
    g_nbase = 0;
    for (i = 0; i < g_nvars; i++) {
        if (g_exempt && g_exempt(g_vars[i].name))
            continue;
        g_base[g_nbase++] = g_vars[i];
    }
}

void mr_var_reset_to_baseline(void)
{
    int i, n = 0;

    if (g_nbase <= 0)
        return;

    /* Keep the exempt variables at their *current* values -- an extra
     * unlocked this session must survive starting a new game, and one cleared
     * this session must stay cleared. */
    if (g_exempt) {
        for (i = 0; i < g_nvars; i++)
            if (g_exempt(g_vars[i].name))
                g_vars[n++] = g_vars[i];
    }
    g_nvars = n;

    /* Then lay the baseline back on top. It holds no exempt names, so there
     * is nothing to collide with. */
    for (i = 0; i < g_nbase && g_nvars < MR_MAX_VARS; i++)
        g_vars[g_nvars++] = g_base[i];
}

static int var_index(const char *name)
{
    int i;
    for (i = 0; i < g_nvars; i++) {
        if (!strcmp(g_vars[i].name, name))
            return i;
    }
    return -1;
}

/* Screens introduce a local scope: `use` parameters, `for` loop variables and
 * `$` assignments inside a screen are scope-local in Ren'Py, not global. The
 * screen renderer owns that scope, so it installs a pair of hooks here rather
 * than this module knowing anything about screens. */
static mr_scope_get_fn g_scope_get;
static mr_scope_set_fn g_scope_set;

void mr_expr_set_scope(mr_scope_get_fn get, mr_scope_set_fn set)
{
    g_scope_get = get;
    g_scope_set = set;
}

void mr_expr_get_scope(mr_scope_get_fn *get, mr_scope_set_fn *set)
{
    *get = g_scope_get;
    *set = g_scope_set;
}

/* The rollback log watches globals being overwritten; see mr_expr_set_journal. */
static mr_journal_fn g_journal;

/* Engine-provided lists are not in the variable store, but scripts test them
 * with `if not _history_list`, so their length stands in for them. */
static mr_listlen_fn g_listlen;

void mr_expr_set_listlen(mr_listlen_fn fn)
{
    g_listlen = fn;
}

void mr_expr_set_journal(mr_journal_fn fn)
{
    g_journal = fn;
}

/* Assign without telling the journal, which is how rollback puts old values
 * back: undoing a change must not itself be recorded as a change. */
void mr_var_set_no_journal(const char *name, const mr_value *v)
{
    char buf[80];
    int i;

    name = normalise(name, buf, sizeof(buf));
    i = var_index(name);

    if (i < 0) {
        if (g_nvars >= MR_MAX_VARS)
            return;
        i = g_nvars++;
        memset(&g_vars[i], 0, sizeof(g_vars[i]));
        strncpy(g_vars[i].name, name, sizeof(g_vars[i].name) - 1);
    }
    g_vars[i].v = *v;
}

void mr_var_unset(const char *name)
{
    char buf[80];
    int i;

    name = normalise(name, buf, sizeof(buf));
    i = var_index(name);
    if (i < 0)
        return;

    /* Compact: the table is scanned linearly, so order does not matter. */
    g_vars[i] = g_vars[--g_nvars];
}

void mr_var_set(const char *name, const mr_value *v)
{
    char buf[80];
    int i;

    name = normalise(name, buf, sizeof(buf));

    if (g_scope_set && g_scope_set(name, v))
        return;

    i = var_index(name);

    /* Tell the rollback log what this is about to replace. Screen-local
     * assignments never get here: they are handled by the scope above, and
     * they do not survive the frame anyway. */
    if (g_journal)
        g_journal(name, i >= 0 ? &g_vars[i].v : NULL, i >= 0);

    if (i < 0) {
        if (g_nvars >= MR_MAX_VARS) {
            mr_log("expr: variable table full, dropping '%s'", name);
            return;
        }
        i = g_nvars++;
        memset(&g_vars[i], 0, sizeof(g_vars[i]));
        strncpy(g_vars[i].name, name, sizeof(g_vars[i].name) - 1);
    }
    g_vars[i].v = *v;
}

int mr_var_get(const char *name, mr_value *out)
{
    char buf[80];
    int i;

    name = normalise(name, buf, sizeof(buf));

    if (g_scope_get && g_scope_get(name, out))
        return 1;

    i = var_index(name);

    if (i < 0) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    *out = g_vars[i].v;
    return 1;
}

void mr_var_set_int(const char *name, long i)
{
    mr_value v;
    mr_value_set_int(&v, i);
    mr_var_set(name, &v);
}

long mr_var_get_int(const char *name, long dflt)
{
    mr_value v;
    if (!mr_var_get(name, &v))
        return dflt;
    if (v.type == MR_V_INT)
        return v.i;
    if (v.type == MR_V_FLOAT)
        return (long)v.f;
    return dflt;
}

int mr_var_enumerate(int i, const char **name, const mr_value **v)
{
    if (i < 0 || i >= g_nvars)
        return 0;
    *name = g_vars[i].name;
    *v = &g_vars[i].v;
    return 1;
}

/* --- tokenizer --- */

typedef struct {
    const char *p;
    int         ok;
} estate;

static void skip(estate *e)
{
    while (*e->p == ' ' || *e->p == '\t')
        e->p++;
}

/* Match a literal operator and consume it. */
static int eat(estate *e, const char *op)
{
    size_t n = strlen(op);
    skip(e);
    if (strncmp(e->p, op, n) != 0)
        return 0;
    /* A word operator must not be a prefix of a longer identifier. */
    if (isalpha((unsigned char)op[0])) {
        char c = e->p[n];
        if (isalnum((unsigned char)c) || c == '_' || c == '.')
            return 0;
    }
    e->p += n;
    skip(e);
    return 1;
}

static int read_name(estate *e, char *out, int outsz)
{
    int n = 0;

    skip(e);
    if (!isalpha((unsigned char)*e->p) && *e->p != '_')
        return 0;

    while (isalnum((unsigned char)*e->p) || *e->p == '_' || *e->p == '.') {
        if (n < outsz - 1)
            out[n++] = *e->p;
        e->p++;
    }
    out[n] = '\0';
    return 1;
}

static void eval_or(estate *e, mr_value *out);

/* --- lists ---------------------------------------------------------------
 *
 * A list literal, a tuple and a comprehension all produce a list handle (see
 * list.h). A tuple is simply a list, which is what makes a list of tuples --
 * the shape the high score table is built from -- fall out for free.
 */

/* Bindings a comprehension's `for` target introduces, chained in front of
 * whatever scope was installed (a screen's, or a function's locals). */
typedef struct {
    char     name[64];
    mr_value v;
} compbind;

static compbind g_cbind[8];
static int      g_ncbind;
static mr_scope_get_fn g_comp_prev_get;
static mr_scope_set_fn g_comp_prev_set;

static int comp_scope_get(const char *name, mr_value *out)
{
    int i;
    for (i = g_ncbind - 1; i >= 0; i--)
        if (!strcmp(g_cbind[i].name, name)) {
            *out = g_cbind[i].v;
            return 1;
        }
    return g_comp_prev_get ? g_comp_prev_get(name, out) : 0;
}

static int comp_scope_set(const char *name, const mr_value *v)
{
    return g_comp_prev_set ? g_comp_prev_set(name, v) : 0;
}

/* Walk to the matching close bracket, honouring nesting and quotes. Returns
 * the position of the closer, or NULL. If `want` is non-zero, also reports
 * the first top-level occurrence of that keyword through `at`. */
static const char *scan_bracket(const char *p, const char *want,
                                const char **at)
{
    const char *start = p;
    int depth = 0;
    char quote = 0;

    if (at)
        *at = NULL;

    for (; *p; p++) {
        if (quote) {
            if (*p == '\\' && p[1])
                p++;
            else if (*p == quote)
                quote = 0;
            continue;
        }
        if (*p == '"' || *p == '\'') {
            quote = *p;
            continue;
        }
        if (*p == '[' || *p == '(') {
            depth++;
            continue;
        }
        if (*p == ')') {
            depth--;
            continue;
        }
        if (*p == ']') {
            depth--;
            if (depth == 0)
                return p;
            continue;
        }
        /* A bare word at the top level of the brackets, so `for` inside a
         * string or a nested list is not mistaken for the comprehension's. */
        if (depth == 1 && want && at && !*at && p > start &&
            (p[-1] == ' ' || p[-1] == '\t') &&
            mr_match_word(p, want))
            *at = p;
    }
    return NULL;
}

/* Evaluate `src` (a bounded span) as one expression. */
static void eval_span(const char *src, int len, mr_value *out)
{
    char buf[512];

    if (len < 0)
        len = 0;
    if (len > (int)sizeof(buf) - 1)
        len = (int)sizeof(buf) - 1;
    memcpy(buf, src, (size_t)len);
    buf[len] = '\0';
    mr_expr_eval(buf, out);
}

/* `a, b` -> two targets. Returns how many were found. */
static int split_targets(const char *src, int len, char t[][64], int max)
{
    int n = 0, i = 0, w = 0;

    while (i < len && n < max) {
        char c = src[i++];
        if (c == ',') {
            t[n][w] = '\0';
            if (w)
                n++;
            w = 0;
            continue;
        }
        if (c == ' ' || c == '\t')
            continue;
        if (w < 62)
            t[n][w++] = c;
    }
    t[n][w] = '\0';
    if (w)
        n++;
    return n;
}

/* Bind one item of the iterated list to the loop targets: element-wise when
 * the item is itself a list and there is more than one target, otherwise the
 * whole item to the single target. */
static void bind_targets(char t[][64], int nt, const mr_value *item)
{
    int sub = mr_list_id(item);
    int i;

    if (nt > 1 && sub >= 0) {
        for (i = 0; i < nt && g_ncbind < 8; i++) {
            mr_value ev;
            memset(&ev, 0, sizeof(ev));
            mr_list_get(sub, i, &ev);
            strncpy(g_cbind[g_ncbind].name, t[i], 63);
            g_cbind[g_ncbind].name[63] = '\0';
            g_cbind[g_ncbind].v = ev;
            g_ncbind++;
        }
        return;
    }

    if (g_ncbind < 8) {
        strncpy(g_cbind[g_ncbind].name, t[0], 63);
        g_cbind[g_ncbind].name[63] = '\0';
        g_cbind[g_ncbind].v = *item;
        g_ncbind++;
    }
}

/* `[ <head> for <targets> in <iterable> ]`, already located. */
static void eval_comprehension(const char *head, int head_len,
                               const char *rest, const char *close,
                               mr_value *out)
{
    char        targets[4][64];
    int         nt;
    const char *in;
    mr_value    src;
    int         list_id, result, n, i;
    int         save_ncbind = g_ncbind;
    mr_scope_get_fn sg;
    mr_scope_set_fn ss;

    memset(out, 0, sizeof(*out));

    in = NULL;
    {
        const char *q = rest;
        int depth = 0;
        char quote = 0;
        for (; q < close; q++) {
            if (quote) {
                if (*q == '\\' && q[1]) q++;
                else if (*q == quote) quote = 0;
                continue;
            }
            if (*q == '"' || *q == '\'') { quote = *q; continue; }
            if (*q == '[' || *q == '(') { depth++; continue; }
            if (*q == ']' || *q == ')') { depth--; continue; }
            if (depth == 0 && (q == rest || q[-1] == ' ' || q[-1] == '\t') &&
                mr_match_word(q, "in")) {
                in = q;
                break;
            }
        }
    }
    if (!in) {
        mr_log("expr: comprehension without `in`");
        return;
    }

    nt = split_targets(rest, (int)(in - rest), targets, 4);
    if (!nt) {
        mr_log("expr: comprehension without a target");
        return;
    }

    eval_span(mr_match_word(in, "in"),
              (int)(close - mr_match_word(in, "in")), &src);
    list_id = mr_list_id(&src);
    if (list_id < 0) {
        mr_log("expr: comprehension over something that is not a list");
        return;
    }

    result = mr_list_id_str(mr_list_new());
    n = mr_list_len(list_id);

    /* The head is re-evaluated per item with the targets bound in front of
     * whatever scope is already installed. */
    mr_expr_get_scope(&sg, &ss);
    g_comp_prev_get = sg;
    g_comp_prev_set = ss;
    mr_expr_set_scope(comp_scope_get, comp_scope_set);

    for (i = 0; i < n; i++) {
        mr_value item, v;

        mr_list_get(list_id, i, &item);
        g_ncbind = save_ncbind;
        bind_targets(targets, nt, &item);

        eval_span(head, head_len, &v);
        mr_list_append(result, &v);
    }

    g_ncbind = save_ncbind;
    mr_expr_set_scope(sg, ss);

    mr_value_set_str(out, "");
    sprintf(out->s, "%cL%d", '\001', result);
    out->type = MR_V_STR;
}

static void eval_atom(estate *e, mr_value *out)
{
    char name[64];

    memset(out, 0, sizeof(*out));
    skip(e);

    /* `[ ... ]`: a list literal, or a comprehension. */
    if (*e->p == '[') {
        const char *close, *forat;

        close = scan_bracket(e->p, "for", &forat);
        if (!close) {
            mr_log("expr: unterminated list");
            e->ok = 0;
            if (*e->p)
                e->p++;
            return;
        }

        if (forat) {
            eval_comprehension(e->p + 1, (int)(forat - (e->p + 1)),
                               mr_match_word(forat, "for"), close, out);
            e->p = close + 1;
            return;
        }

        {
            int id = mr_list_id_str(mr_list_new());
            const char *q = e->p + 1;

            while (q < close) {
                mr_value item;
                const char *start;
                int depth = 0;
                char quote = 0;

                while (q < close && (*q == ' ' || *q == '\t' || *q == ','))
                    q++;
                if (q >= close)
                    break;

                start = q;
                while (q < close) {
                    if (quote) {
                        if (*q == '\\' && q[1]) q++;
                        else if (*q == quote) quote = 0;
                    } else if (*q == '"' || *q == '\'') {
                        quote = *q;
                    } else if (*q == '[' || *q == '(') {
                        depth++;
                    } else if (*q == ']' || *q == ')') {
                        depth--;
                    } else if (*q == ',' && depth == 0) {
                        break;
                    }
                    q++;
                }

                eval_span(start, (int)(q - start), &item);
                mr_list_append(id, &item);
            }

            e->p = close + 1;
            mr_value_set_str(out, "");
            sprintf(out->s, "%cL%d", '\001', id);
            out->type = MR_V_STR;
            return;
        }
    }

    if (*e->p == '(') {
        /* A parenthesised expression, or a tuple if a comma follows at this
         * level. A tuple is a list, which is how `[(a, b), (c, d)]` and
         * `for title, score in ...` meet in the middle. */
        const char *save = e->p;

        e->p++;
        eval_or(e, out);
        skip(e);

        if (*e->p == ',') {
            int id = mr_list_id_str(mr_list_new());

            e->p = save + 1;
            while (*e->p) {
                mr_value item;
                skip(e);
                if (*e->p == ')')
                    break;
                eval_or(e, &item);
                mr_list_append(id, &item);
                skip(e);
                if (*e->p == ',')
                    e->p++;
                else
                    break;
            }
            if (*e->p == ')')
                e->p++;

            mr_value_set_str(out, "");
            sprintf(out->s, "%cL%d", '\001', id);
            out->type = MR_V_STR;
            return;
        }

        if (*e->p == ')')
            e->p++;
        return;
    }

    if (mr_string_quote(e->p, NULL)) {
        /* Through mr_read_string, which resolves the escapes. Reading the
         * literal here used to skip the backslash and keep the character
         * after it, so "\n" arrived as the letter n -- visible on the About
         * screen, where `text _("Version [config.version!t]\n")` ran into
         * the line below it. Escapes belong in one place, and that is the
         * place the say statement already uses. */
        const char *after = mr_read_string(e->p, out->s, MR_VSTR_MAX);
        if (!after) {
            out->s[0] = '\0';
            out->type = MR_V_STR;
            return;
        }
        e->p = after;
        out->type = MR_V_STR;
        return;
    }

    if (isdigit((unsigned char)*e->p) ||
        (*e->p == '.' && isdigit((unsigned char)e->p[1]))) {
        char *endp;
        double d = strtod(e->p, &endp);
        /* An integer literal stays an integer so that `== 1` compares
         * exactly and save files round-trip cleanly. */
        if (memchr(e->p, '.', (size_t)(endp - e->p)) == NULL) {
            out->type = MR_V_INT;
            out->i = (long)d;
        } else {
            out->type = MR_V_FLOAT;
            out->f = d;
        }
        e->p = endp;
        return;
    }

    if (!read_name(e, name, sizeof(name))) {
        /* Unparseable: consume a character so the caller cannot spin. */
        if (*e->p)
            e->p++;
        e->ok = 0;
        return;
    }

    if (!strcmp(name, "True"))  { out->type = MR_V_INT; out->i = 1; return; }
    if (!strcmp(name, "False")) { out->type = MR_V_INT; out->i = 0; return; }
    if (!strcmp(name, "None"))  { out->type = MR_V_NONE; return; }

    skip(e);
    if (*e->p == '(') {
        /* A call. Evaluate the arguments, then hand it to the interpreter. */
        mr_value    argv[8];
        const char *argname[8];
        char        namebuf[8][32];
        int         argc = 0;

        e->p++;
        skip(e);
        while (*e->p && *e->p != ')') {
            const char *save;

            if (argc >= 8) {
                /* Too many arguments: skip to the closing paren. */
                int depth = 1;
                while (*e->p && depth > 0) {
                    if (*e->p == '(') depth++;
                    else if (*e->p == ')') depth--;
                    if (depth > 0) e->p++;
                }
                break;
            }

            argname[argc] = NULL;

            /* Detect `keyword=value`, but not `==`. */
            save = e->p;
            if (read_name(e, namebuf[argc], sizeof(namebuf[0]))) {
                skip(e);
                if (*e->p == '=' && e->p[1] != '=') {
                    e->p++;
                    argname[argc] = namebuf[argc];
                } else {
                    e->p = save;
                }
            } else {
                e->p = save;
            }

            eval_or(e, &argv[argc]);
            argc++;

            skip(e);
            if (*e->p == ',') {
                e->p++;
                skip(e);
            }
        }
        if (*e->p == ')')
            e->p++;

        memset(out, 0, sizeof(*out));

        /* `len(x)` on a list, and `x.append(v)`. Handled here rather than in
         * the interpreter's builtin table because they are properties of the
         * value, not of the game. */
        /* `str(x)`, which the high score table formats its numbers with. An
         * integer must not come out as "120.0", which is why this is not
         * simply a printf of the numeric value. */
        if (!strcmp(name, "str") && argc == 1) {
            char buf[MR_VSTR_MAX];

            switch (argv[0].type) {
            case MR_V_STR:
                mr_value_set_str(out, argv[0].s);
                return;
            case MR_V_INT:
                sprintf(buf, "%ld", argv[0].i);
                mr_value_set_str(out, buf);
                return;
            case MR_V_FLOAT:
                sprintf(buf, "%g", argv[0].f);
                mr_value_set_str(out, buf);
                return;
            default:
                mr_value_set_str(out, "None");
                return;
            }
        }

        if (!strcmp(name, "len") && argc == 1) {
            int id = mr_list_id(&argv[0]);
            if (id >= 0) {
                mr_value_set_int(out, mr_list_len(id));
                return;
            }
            if (argv[0].type == MR_V_STR) {
                mr_value_set_int(out, (long)strlen(argv[0].s));
                return;
            }
        }
        {
            int n = (int)strlen(name);
            if (n > 7 && !strcmp(name + n - 7, ".append") && argc == 1) {
                char     base[64];
                mr_value lv;
                int      id;

                memcpy(base, name, (size_t)(n - 7));
                base[n - 7] = '\0';
                if (mr_var_get(base, &lv) &&
                    (id = mr_list_id(&lv)) >= 0) {
                    mr_list_append(id, &argv[0]);
                    return;                 /* append returns None */
                }
            }
        }

        if (!g_call || !g_call(name, argv, argname, argc, out))
            mr_log("expr: unhandled call %s(), result None", name);
        return;
    }

    /* A plain variable. Unset names are None, which is falsey -- the same
     * behaviour the game relies on for flags it never initialises. */
    /* A subscript reads a dotted name: h.who_args["color"] is
     * h.who_args.color, which is how the loop bound it. */
    skip(e);
    if (*e->p == '[') {
        const char *save = e->p;
        mr_value key;

        e->p++;
        eval_or(e, &key);
        skip(e);
        if (*e->p == ']') {
            e->p++;
            if (key.type == MR_V_STR && key.s[0] &&
                (int)(strlen(name) + strlen(key.s) + 2) < (int)sizeof(name)) {
                strcat(name, ".");
                strcat(name, key.s);
            }
        } else {
            e->p = save;
        }
    }

    if (!mr_var_get(name, out) && g_listlen) {
        int n = g_listlen(name);
        if (n >= 0)
            mr_value_set_int(out, n);
    }
}

static void eval_unary(estate *e, mr_value *out)
{
    skip(e);
    if (*e->p == '-' && e->p[1] != '-') {
        e->p++;
        eval_unary(e, out);
        if (out->type == MR_V_FLOAT)
            out->f = -out->f;
        else {
            out->type = MR_V_INT;
            out->i = -out->i;
        }
        return;
    }
    eval_atom(e, out);
}

static void eval_term(estate *e, mr_value *out)
{
    mr_value rhs;

    eval_unary(e, out);
    for (;;) {
        int op;
        skip(e);
        if (*e->p == '*')      op = '*';
        else if (*e->p == '/') op = '/';
        else if (*e->p == '%') op = '%';
        else return;
        e->p++;
        eval_unary(e, &rhs);

        if (out->type == MR_V_FLOAT || rhs.type == MR_V_FLOAT) {
            double a = value_num(out), b = value_num(&rhs);
            out->type = MR_V_FLOAT;
            out->f = (op == '*') ? a * b : (b != 0.0 ? (op == '/' ? a / b : 0.0) : 0.0);
        } else {
            long a = out->i, b = rhs.i;
            out->type = MR_V_INT;
            if (op == '*') out->i = a * b;
            else if (b != 0) out->i = (op == '/') ? a / b : a % b;
            else out->i = 0;
        }
    }
}

static void eval_sum(estate *e, mr_value *out)
{
    mr_value rhs;

    eval_term(e, out);
    for (;;) {
        int op;
        skip(e);
        /* Do not mistake `->` or a negative literal in the next statement. */
        if (*e->p == '+')      op = '+';
        else if (*e->p == '-') op = '-';
        else return;
        e->p++;
        eval_term(e, &rhs);

        if (out->type == MR_V_STR && rhs.type == MR_V_STR && op == '+') {
            size_t have = strlen(out->s);
            size_t room = MR_VSTR_MAX - 1 - have;

            strncat(out->s, rhs.s, room);
            if (strlen(rhs.s) > room)
                vstr_truncated(out->s);
        } else if (out->type == MR_V_FLOAT || rhs.type == MR_V_FLOAT) {
            double a = value_num(out), b = value_num(&rhs);
            out->type = MR_V_FLOAT;
            out->f = (op == '+') ? a + b : a - b;
        } else {
            long a = out->i, b = rhs.i;
            out->type = MR_V_INT;
            out->i = (op == '+') ? a + b : a - b;
        }
    }
}

static int compare(const mr_value *a, const mr_value *b)
{
    if (a->type == MR_V_STR || b->type == MR_V_STR) {
        const char *x = a->type == MR_V_STR ? a->s : "";
        const char *y = b->type == MR_V_STR ? b->s : "";
        return strcmp(x, y);
    }
    if (a->type == MR_V_NONE || b->type == MR_V_NONE) {
        /* None equals only None. */
        if (a->type == b->type)
            return 0;
        return a->type == MR_V_NONE ? -1 : 1;
    }
    {
        double x = value_num(a), y = value_num(b);
        return (x < y) ? -1 : (x > y ? 1 : 0);
    }
}

static void eval_cmp(estate *e, mr_value *out)
{
    mr_value rhs;
    int c;

    eval_sum(e, out);
    skip(e);

    /* Order matters: test the two-character operators first. */
    if (eat(e, "==")) {
        eval_sum(e, &rhs);
        c = compare(out, &rhs);
        mr_value_set_int(out, c == 0);
    } else if (eat(e, "!=")) {
        eval_sum(e, &rhs);
        c = compare(out, &rhs);
        mr_value_set_int(out, c != 0);
    } else if (eat(e, "<=")) {
        eval_sum(e, &rhs);
        c = compare(out, &rhs);
        mr_value_set_int(out, c <= 0);
    } else if (eat(e, ">=")) {
        eval_sum(e, &rhs);
        c = compare(out, &rhs);
        mr_value_set_int(out, c >= 0);
    } else if (eat(e, "<")) {
        eval_sum(e, &rhs);
        c = compare(out, &rhs);
        mr_value_set_int(out, c < 0);
    } else if (eat(e, ">")) {
        eval_sum(e, &rhs);
        c = compare(out, &rhs);
        mr_value_set_int(out, c > 0);
    } else if (eat(e, "in")) {
        /* `"color" in h.who_args`. There are no dicts here, but the store
         * keeps dotted names whole, so membership is simply whether
         * h.who_args.color exists. */
        char key[MR_VSTR_MAX];
        char full[160];
        mr_value probe;

        strncpy(key, out->type == MR_V_STR ? out->s : "", sizeof(key) - 1);
        key[sizeof(key) - 1] = '\0';

        skip(e);
        if (read_name(e, full, sizeof(full)) && key[0] &&
            (int)(strlen(full) + strlen(key) + 2) < (int)sizeof(full)) {
            strcat(full, ".");
            strcat(full, key);
            mr_value_set_int(out, mr_var_get(full, &probe));
        } else {
            mr_value_set_int(out, 0);
        }
    }
}

static void eval_not(estate *e, mr_value *out)
{
    if (eat(e, "not")) {
        eval_not(e, out);
        mr_value_set_int(out, !mr_value_truthy(out));
        return;
    }
    eval_cmp(e, out);
}

static void eval_and(estate *e, mr_value *out)
{
    mr_value rhs;

    eval_not(e, out);
    while (eat(e, "and")) {
        eval_not(e, &rhs);
        /* Evaluated eagerly: none of the game's conditions have side effects
         * in their right-hand operand. */
        mr_value_set_int(out, mr_value_truthy(out) && mr_value_truthy(&rhs));
    }
}

static void eval_or(estate *e, mr_value *out)
{
    mr_value rhs;

    eval_and(e, out);
    while (eat(e, "or")) {
        eval_and(e, &rhs);
        mr_value_set_int(out, mr_value_truthy(out) || mr_value_truthy(&rhs));
    }
}

int mr_expr_eval(const char *src, mr_value *out)
{
    estate e;

    e.p = src;
    e.ok = 1;
    eval_or(&e, out);

    if (!e.ok)
        mr_log("expr: could not fully evaluate '%.60s'", src);
    return e.ok;
}

/* --- statements --- */

void mr_expr_exec(const char *src)
{
    const char *eq;
    const char *p;
    int depth;
    int instr;

    /* Find a top-level '=' that is not part of ==, !=, <= or >=. Scanning
     * rather than parsing keeps `renpy.music.play(..., if_changed=True)` from
     * looking like an assignment. */
    eq = NULL;
    depth = 0;
    instr = 0;
    for (p = src; *p; p++) {
        if (instr) {
            if (*p == '\\' && p[1]) { p++; continue; }
            if (*p == instr) instr = 0;
            continue;
        }
        if (*p == '"' || *p == '\'') { instr = *p; continue; }
        if (*p == '(' || *p == '[') depth++;
        else if (*p == ')' || *p == ']') depth--;
        else if (*p == '=' && depth == 0) {
            if (p[1] == '=') { p++; continue; }
            if (p > src && (p[-1] == '!' || p[-1] == '<' || p[-1] == '>'))
                continue;
            eq = p;
            break;
        }
    }

    if (!eq) {
        /* A bare expression, evaluated for its side effects. */
        mr_value v;
        mr_expr_eval(src, &v);
        return;
    }

    /* Left-hand side: one name, or a comma-separated tuple. Both ends are
     * trimmed -- a `default x = ...` line arrives with the space after the
     * keyword still attached, and a name of " x" is not the same variable
     * as "x". */
    {
        char lhs[192];
        const char *start = src;
        int n;
        mr_value v;

        while (*start == ' ' || *start == '\t')
            start++;
        n = (int)(eq - start);
        if (n < 0)
            n = 0;
        if (n > (int)sizeof(lhs) - 1)
            n = (int)sizeof(lhs) - 1;
        memcpy(lhs, start, (size_t)n);
        lhs[n] = '\0';
        while (n > 0 && (lhs[n - 1] == ' ' || lhs[n - 1] == '\t'))
            lhs[--n] = '\0';

        mr_expr_eval(eq + 1, &v);

        if (strchr(lhs, ',')) {
            /* Tuple unpack. The one case in this game is
             * `num_hits, num_notes, rhythm_score = _return`, where the right
             * side is the rhythm minigame's (hits, notes, score).
             *
             * There is no tuple type here. A producer that has several values
             * to return publishes them as `<name>.0`, `<name>.1`, ... beside
             * `<name>` itself, and an unpack binds the targets to those in
             * order. Anything without indexed parts falls back to giving
             * every target the same scalar, which is what this did before and
             * is still right for `a, b = 0`. */
            char base[MR_VSTR_MAX];
            char *tok;
            int  k = 0;
            int  n;

            {
                const char *r = eq + 1;
                while (*r == ' ' || *r == '\t')
                    r++;
                n = (int)strlen(r);
                while (n > 0 && (r[n - 1] == ' ' || r[n - 1] == '\t' ||
                                 r[n - 1] == '\r' || r[n - 1] == '\n'))
                    n--;
                if (n > (int)sizeof(base) - 1)
                    n = (int)sizeof(base) - 1;
                memcpy(base, r, (size_t)n);
                base[n] = '\0';
            }

            tok = strtok(lhs, ",");
            while (tok) {
                while (*tok == ' ') tok++;
                {
                    int m = (int)strlen(tok);
                    while (m > 0 && tok[m - 1] == ' ')
                        tok[--m] = '\0';
                }
                if (*tok) {
                    char     part[MR_VSTR_MAX + 24];
                    mr_value pv;

                    sprintf(part, "%s.%d", base, k);
                    if (mr_var_get(part, &pv))
                        mr_var_set(tok, &pv);
                    else
                        mr_var_set(tok, &v);
                }
                k++;
                tok = strtok(NULL, ",");
            }
            return;
        }

        mr_var_set(lhs, &v);
    }
}

void mr_expr_substitute(const char *src, char *out, int outsz)
{
    int n = 0;

    if (!out || outsz <= 0)
        return;
    if (!src) { out[0] = '\0'; return; }

    while (*src && n < outsz - 1) {
        if (*src == '[') {
            const char *close = strchr(src, ']');
            if (close) {
                char var[96];
                int vl = (int)(close - src - 1);
                char *bang;
                const char *conv = "";
                mr_value v;

                if (vl > (int)sizeof(var) - 1)
                    vl = (int)sizeof(var) - 1;
                memcpy(var, src + 1, (size_t)vl);
                var[vl] = '\0';

                /* `[title!t]` - the conversion flags, after the name. */
                bang = strchr(var, '!');
                if (bang) {
                    *bang = '\0';
                    conv = bang + 1;
                }

                if (mr_var_get(var, &v)) {
                    char tmp[160];
                    const char *rep = tmp;
                    if (v.type == MR_V_STR)        rep = v.s;
                    else if (v.type == MR_V_INT)   sprintf(tmp, "%ld", v.i);
                    else if (v.type == MR_V_FLOAT) sprintf(tmp, "%g", v.f);
                    else                           tmp[0] = '\0';

                    /* `!t` translates the *substituted* value, which is the
                     * one conversion that cannot be done anywhere else.
                     * Ren'Py: `if "t" in conv: value =
                     * renpy.translation.translate_string(value)`
                     * (substitutions.py:282-283).
                     *
                     * The text around a substitution is translated as a
                     * template, before interpolation -- so a screen that says
                     * `text "[title]"` looks up the literal `[title]` and the
                     * variable's contents are never a key. That is Ren'Py's
                     * order too, and it is why a chapter title assigned under
                     * one language stays in that language when the player
                     * switches: the `$ title = "..."` does not re-run, and a
                     * save carries the string it computed. `!t` is the
                     * provided way out, and the only one that survives a
                     * load.
                     *
                     * Keyed on the value, through the `_("...")` string table
                     * -- so the assigned text has to exist as a translation
                     * *source*, i.e. in the script's own language.
                     *
                     * The other flags stay unimplemented: `r`, `s`, `q`, `u`,
                     * `l`, `c`, `i`. Each is reachable here the same way if a
                     * script ever needs one. Order matters when they combine
                     * (Ren'Py's is r/s, t, i, q, u, l, c) and `t` is first of
                     * the ones below the string conversions, so applying it
                     * alone cannot be made wrong by a later addition. */
                    if (strchr(conv, 't'))
                        rep = mr_tl_string(rep);

                    while (*rep && n < outsz - 1)
                        out[n++] = *rep++;
                }
                src = close + 1;
                continue;
            }
        }
        out[n++] = *src++;
    }
    out[n] = '\0';
}
