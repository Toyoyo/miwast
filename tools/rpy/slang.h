/* slang.h - Ren'Py screen language: syntax tree and parser.
 *
 * This is a general implementation rather than one shaped around a single
 * game's screens, so the engine can run other Ren'Py projects. The name
 * tables come from the reference implementation (renpy/sl2/sldisplayables.py
 * and renpy/sl2/slproperties.py) rather than from what this game happens to
 * use.
 *
 * Screen language lines look like
 *
 *     <displayable> [positional...] [property value]... [:]
 *
 * and an indented block under a displayable holds both further properties
 * (written as bare `name value` lines) and child displayables. The two are
 * told apart exactly as Ren'Py tells them apart: by whether the leading word
 * names a known displayable or a known property.
 *
 * Property values are kept as unevaluated expression text. Ren'Py evaluates
 * them as Python each time a screen is shown, and actions in particular must
 * not be evaluated until clicked, so deferring is both simpler and correct.
 */

#ifndef MR_SLANG_H
#define MR_SLANG_H

#include "mr.h"

#define SL_MAX_PROPNAME 32
#define SL_MAX_TYPE     24
#define SL_MAX_PARAMS   8

typedef enum {
    SL_DISPLAYABLE = 0, /* text, vbox, button, add, ... */
    SL_IF,              /* cond + children; chained through `orelse` */
    SL_FOR,             /* `for <var> in <expr>:` */
    SL_PYTHON,          /* a `$` line inside a screen */
    SL_DEFAULT,         /* `default name = expr` */
    SL_USE              /* `use other_screen(args)` */
} sl_kind;

typedef struct sl_prop {
    char  name[SL_MAX_PROPNAME];
    char *expr;                  /* unevaluated */
} sl_prop;

typedef struct sl_node {
    sl_kind kind;
    int     line;

    char    type[SL_MAX_TYPE];   /* displayable name, or the `use` target */
    char   *arg;                 /* first positional argument, unevaluated */
    char   *arg2;                /* second, for `grid cols rows` */

    sl_prop        *props;
    int             nprops, capprops;

    struct sl_node **kids;
    int              nkids, capkids;

    /* control flow */
    char   *cond;                /* if condition, or the for iterable */
    char    var[SL_MAX_PROPNAME];/* for loop variable */
    struct sl_node *orelse;      /* elif/else chain for SL_IF */
} sl_node;

typedef struct sl_screen {
    char     name[64];
    char     params[SL_MAX_PARAMS][SL_MAX_PROPNAME];
    char    *defaults[SL_MAX_PARAMS];  /* default value expression, or NULL */
    int      nparams;

    sl_node *root;               /* synthetic `fixed` holding the body */
    char     tag[64];
    char    *zorder;
    int      modal;
} sl_screen;

/* Classification, exported so the parser and any tooling agree. */
int sl_is_displayable(const char *word);
int sl_is_property(const char *word);

/* Tree building. */
sl_node *sl_node_new(sl_kind kind, int line);
void     sl_node_free(sl_node *n);
void     sl_node_add_kid(sl_node *parent, sl_node *kid);
void     sl_node_set_prop(sl_node *n, const char *name, const char *expr);
const char *sl_node_prop(const sl_node *n, const char *name);

void sl_screen_free(sl_screen *s);

#endif /* MR_SLANG_H */
