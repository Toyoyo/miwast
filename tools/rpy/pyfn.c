/* pyfn.c - run the small functions a script defines in `init python:`.
 *
 * A body is kept as its source lines with their indent columns; running it
 * walks those lines, handing each to the expression evaluator and using the
 * indent to find the extent of an `if`. Locals live in a scope installed for
 * the duration of the call, so `room = MusicRoom(...)` inside a function does
 * not leave a `room` behind in the variable store -- and so that the screen
 * scope a call was made from is not visible inside it, which is what Python
 * does.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "mr.h"
#include "pyfn.h"
#include "lex.h"

#define MAX_FNS     16
#define MAX_LINES   64
#define MAX_LOCALS  16
#define MAX_DEPTH   4

typedef struct {
    char  name[64];
    char *text[MAX_LINES];
    int   indent[MAX_LINES];
    int   n;
} pyfn;

static pyfn g_fn[MAX_FNS];
static int  g_nfn;
static int  g_cur = -1;

/* Locals, one frame per active call. */
typedef struct {
    char     name[64];
    mr_value v;
} local;

static local g_local[MAX_DEPTH][MAX_LOCALS];
static int   g_nlocal[MAX_DEPTH];
static int   g_depth;

void mr_pyfn_reset(void)
{
    int i, j;

    for (i = 0; i < g_nfn; i++)
        for (j = 0; j < g_fn[i].n; j++)
            free(g_fn[i].text[j]);
    memset(g_fn, 0, sizeof(g_fn));
    g_nfn = 0;
    g_cur = -1;
}

void mr_pyfn_begin(const char *name)
{
    if (g_nfn >= MAX_FNS) {
        mr_log("pyfn: more than %d functions, ignoring %s", MAX_FNS, name);
        g_cur = -1;
        return;
    }
    g_cur = g_nfn++;
    memset(&g_fn[g_cur], 0, sizeof(g_fn[0]));
    strncpy(g_fn[g_cur].name, name, sizeof(g_fn[0].name) - 1);
}

/* How far the brackets are left open by `s`, ignoring quoted text. A list
 * written over several lines -- which is how the high score table's song list
 * is written -- has to be rejoined before it can be evaluated. */
static int bracket_delta(const char *s)
{
    int depth = 0;
    char quote = 0;

    for (; *s; s++) {
        if (quote) {
            if (*s == '\\' && s[1])
                s++;
            else if (*s == quote)
                quote = 0;
            continue;
        }
        if (*s == '"' || *s == '\'')
            quote = *s;
        else if (*s == '[' || *s == '(')
            depth++;
        else if (*s == ']' || *s == ')')
            depth--;
    }
    return depth;
}

void mr_pyfn_line(const char *text, int indent)
{
    pyfn *f;

    if (g_cur < 0)
        return;
    f = &g_fn[g_cur];

    /* Still inside a bracket from the previous line: this is a continuation,
     * so append it to that line rather than starting a new one. Python would
     * do the same; without it `songs = [` evaluates on its own as an
     * unterminated list and the rest is read as separate statements. */
    if (f->n > 0 && bracket_delta(f->text[f->n - 1]) > 0) {
        char *joined;
        const char *add = text;
        size_t have = strlen(f->text[f->n - 1]);

        while (*add == ' ' || *add == '\t')
            add++;

        joined = (char *)malloc(have + strlen(add) + 2);
        if (!joined)
            mr_fatal("out of memory joining a line of %s", f->name);
        strcpy(joined, f->text[f->n - 1]);
        joined[have] = ' ';
        strcpy(joined + have + 1, add);
        free(f->text[f->n - 1]);
        f->text[f->n - 1] = joined;
        return;
    }

    if (f->n >= MAX_LINES) {
        mr_log("pyfn: %s is longer than %d lines, truncated",
               f->name, MAX_LINES);
        return;
    }
    f->text[f->n] = (char *)malloc(strlen(text) + 1);
    if (!f->text[f->n])
        mr_fatal("out of memory recording %s", f->name);
    strcpy(f->text[f->n], text);
    f->indent[f->n] = indent;
    f->n++;
}

static pyfn *find(const char *name)
{
    int i;

    for (i = 0; i < g_nfn; i++)
        if (!strcmp(g_fn[i].name, name))
            return &g_fn[i];
    return NULL;
}

int mr_pyfn_exists(const char *name)
{
    return find(name) != NULL;
}

/* A line of a function's body, or NULL past the end.
 *
 * For the case where a builtin shadows a script function but still wants to
 * honour something the script wrote in it. reset_persistent is the one: its
 * body has to be intercepted, because it ends in renpy.quit(relaunch=True)
 * and a Win32s process cannot relaunch itself -- but the
 * `persistent._clear(progress=...)` line inside it is the script's decision
 * and should stay the script's decision. */
const char *mr_pyfn_body_line(const char *name, int i)
{
    const pyfn *f = find(name);

    if (!f || i < 0 || i >= f->n)
        return NULL;
    return f->text[i];
}

/* --- locals --- */

static int local_get(const char *name, mr_value *out)
{
    int d = g_depth - 1;
    int i;

    if (d < 0)
        return 0;
    for (i = 0; i < g_nlocal[d]; i++) {
        if (!strcmp(g_local[d][i].name, name)) {
            *out = g_local[d][i].v;
            return 1;
        }
    }
    return 0;
}

/* Every assignment inside a function body is a local, which is Python's rule
 * for a name a function assigns to. A dotted name is not: `persistent.x = 1`
 * is an attribute store and belongs in the variable store. */
static int local_set(const char *name, const mr_value *v)
{
    int d = g_depth - 1;
    int i;

    if (d < 0 || strchr(name, '.'))
        return 0;

    for (i = 0; i < g_nlocal[d]; i++) {
        if (!strcmp(g_local[d][i].name, name)) {
            g_local[d][i].v = *v;
            return 1;
        }
    }
    if (g_nlocal[d] >= MAX_LOCALS) {
        mr_log("pyfn: too many locals, %s stays global", name);
        return 0;
    }
    strncpy(g_local[d][i].name, name, sizeof(g_local[0][0].name) - 1);
    g_local[d][i].name[sizeof(g_local[0][0].name) - 1] = '\0';
    g_local[d][i].v = *v;
    g_nlocal[d]++;
    return 1;
}

/* --- execution --- */

/* The condition of an `if`/`elif`, without the trailing colon. */
static void condition(const char *line, char *out, int outsz)
{
    const char *end = strrchr(line, ':');
    int n;

    if (!end)
        end = line + strlen(line);
    n = (int)(end - line);
    if (n > outsz - 1)
        n = outsz - 1;
    memcpy(out, line, (size_t)n);
    out[n] = '\0';
}

/* Run lines [i, end) of `f` at indent `base`. Returns the index one past the
 * last line consumed; sets *returned and *result if a `return` was hit. */
static int exec_block(pyfn *f, int i, int end, int base,
                      int *returned, mr_value *result)
{
    /* Whether the last `if` chain tested true, so `elif`/`else` know. */
    int taken = 0;

    while (i < end && !*returned) {
        const char *line = mr_skip_ws(f->text[i]);
        const char *rest;

        if (f->indent[i] < base)
            break;
        if (f->indent[i] > base) {
            /* A body whose header was skipped; skip it too. */
            i++;
            continue;
        }
        if (!*line || *line == '#') {
            i++;
            continue;
        }

        if ((rest = mr_match_word(line, "return")) != NULL) {
            memset(result, 0, sizeof(*result));
            if (*rest)
                mr_expr_eval(rest, result);
            *returned = 1;
            return i + 1;
        }

        if ((rest = mr_match_word(line, "if")) != NULL ||
            (rest = mr_match_word(line, "elif")) != NULL) {
            char cond[256];
            mr_value v;
            int run;

            /* An `elif` after a branch that ran is not tested at all. */
            if (line[0] == 'e' && taken) {
                run = 0;
            } else {
                condition(rest, cond, sizeof(cond));
                mr_expr_eval(cond, &v);
                run = mr_value_truthy(&v);
            }
            if (line[0] == 'i')
                taken = run;
            else if (run)
                taken = 1;

            i++;
            if (i < end && f->indent[i] > base) {
                int body = f->indent[i];
                if (run)
                    i = exec_block(f, i, end, body, returned, result);
                else
                    while (i < end && f->indent[i] >= body)
                        i++;
            }
            continue;
        }

        if (!strncmp(line, "else", 4)) {
            i++;
            if (i < end && f->indent[i] > base) {
                int body = f->indent[i];
                if (!taken)
                    i = exec_block(f, i, end, body, returned, result);
                else
                    while (i < end && f->indent[i] >= body)
                        i++;
            }
            taken = 0;
            continue;
        }

        /* Anything else is an assignment or a call, which the expression
         * evaluator already handles. */
        mr_expr_exec(line);
        taken = 0;
        i++;
    }

    return i;
}

int mr_pyfn_call(const char *name, mr_value *result)
{
    pyfn *f = find(name);
    mr_scope_get_fn old_get;
    mr_scope_set_fn old_set;
    int returned = 0;

    memset(result, 0, sizeof(*result));
    if (!f)
        return 0;

    if (g_depth >= MAX_DEPTH) {
        mr_log("pyfn: %s nested too deeply, returning None", name);
        return 1;
    }

    mr_expr_get_scope(&old_get, &old_set);
    g_nlocal[g_depth] = 0;
    g_depth++;
    mr_expr_set_scope(local_get, local_set);

    exec_block(f, 0, f->n, f->n ? f->indent[0] : 0, &returned, result);

    g_depth--;
    mr_expr_set_scope(old_get, old_set);
    return 1;
}
