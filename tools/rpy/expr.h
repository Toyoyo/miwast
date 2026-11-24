/* expr.h - the variable store and the small expression evaluator.
 *
 * The game's Python usage is narrow: 191 `$` lines and 56 `if` conditions,
 * which between them do assignments, comparisons against small integers and
 * booleans, and calls to a short list of renpy/preferences functions. That is
 * what this evaluates. It is deliberately not a Python interpreter; anything
 * unrecognised evaluates to None and is logged rather than aborting the game.
 */

#ifndef MR_EXPR_H
#define MR_EXPR_H

#include "mr.h"

typedef enum {
    MR_V_NONE = 0,
    MR_V_INT,
    MR_V_FLOAT,
    MR_V_STR
} mr_vtype;

/* The longest string a value can hold.
 *
 * 128 was too small for engine prose. Ren'Py's own confirm messages reach it:
 * the French `layout.LOADING` is exactly 128 bytes, so it stored 127 and lost
 * its final "?" -- see NOTES. 26 strings in this game's catalogue are 128
 * bytes or longer, the longest 350, so a cap here is felt by any game with a
 * translation, not just this one.
 *
 * Costs about 64KB of static table (g_vars and g_base are 256 entries each)
 * and 128 bytes per variable in a save, which is why it is not simply huge:
 * every value is passed and stored by value. The truncation warning in
 * expr.c stays -- it is what found this. */
#define MR_VSTR_MAX 1024

/* What versions 5 and 6 of the save format wrote: a fixed-width field of
 * this many bytes. Version 7 length-prefixes instead, so the cap above is
 * free to grow without touching a save's size. The bytes already on a
 * player's disk do not change, so these stay. */
#define MR_VSTR_V5  128
#define MR_VSTR_V6  256

typedef struct mr_value {
    mr_vtype type;
    long     i;
    double   f;
    char     s[MR_VSTR_MAX];
} mr_value;

/* Python truthiness: 0, 0.0, "" and None are false. */
int  mr_value_truthy(const mr_value *v);
void mr_value_set_int(mr_value *v, long i);
void mr_value_set_float(mr_value *v, double f);
void mr_value_set_str(mr_value *v, const char *s);

/* A builtin call. `name` is the dotted function name, argv the evaluated
 * positional arguments (keyword arguments are evaluated and passed too, with
 * their names in argname). Return 1 if handled. The interpreter installs this
 * so that calls can reach the audio and scene layers. */
typedef int (*mr_call_fn)(const char *name, mr_value *argv,
                          const char **argname, int argc, mr_value *result);

void mr_expr_init(mr_call_fn call);
void mr_expr_shutdown(void);

/* Variables. Names are dotted and stored whole, so `persistent.ending` and
 * `config.rollback_enabled` are just names with dots in them. */
void mr_var_set(const char *name, const mr_value *v);
int  mr_var_get(const char *name, mr_value *out);   /* 0 if unset */
void mr_var_set_int(const char *name, long i);
long mr_var_get_int(const char *name, long dflt);

/* A local scope, owned by whoever installs it. `get` returns 1 if it resolved
 * the name, `set` returns 1 if it consumed the assignment; a 0 from either
 * falls through to the global store. The screen renderer uses this for `use`
 * parameters, `for` loop variables and `$` lines, all of which are local to a
 * screen in Ren'Py rather than global. */
typedef int (*mr_scope_get_fn)(const char *name, mr_value *out);
typedef int (*mr_scope_set_fn)(const char *name, const mr_value *v);
void mr_expr_set_scope(mr_scope_get_fn get, mr_scope_set_fn set);

/* Read the installed scope back, so a caller that has to swap one in
 * temporarily -- a function body, which has locals of its own -- can put the
 * previous one back afterwards. */
void mr_expr_get_scope(mr_scope_get_fn *get, mr_scope_set_fn *set);

/* The rollback log listens here: before a global is overwritten it is told
 * the old value, or that there was none. */
typedef void (*mr_journal_fn)(const char *name, const mr_value *old,
                              int existed);
void mr_expr_set_journal(mr_journal_fn fn);

/* A name the engine supplies as a list rather than a variable; returns its
 * length, or -1 if the name is not one. Lets `if not _history_list` work. */
typedef int (*mr_listlen_fn)(const char *name);
void mr_expr_set_listlen(mr_listlen_fn fn);

/* Variables that are not part of a playthrough and so take no part in the
 * baseline below: the persistent data. Whoever knows the rule installs it;
 * the interpreter passes mr_persist_is_persistent. Without it the baseline
 * behaves as it always did, which is right for a build that has no notion of
 * persistent data at all. */
typedef int (*mr_exempt_fn)(const char *name);
void mr_var_set_baseline_exempt(mr_exempt_fn fn);

/* Remember the variables the init phase established, and restore them. A
 * load resets to this baseline before applying the save, so the loaded game
 * does not inherit variables from wherever the player was.
 *
 * Exempt variables are left out of the baseline entirely and keep their
 * current values across a reset. The baseline is taken after the persistent
 * file has been read, so including them meant that clearing the persistent
 * data and then starting a new game brought every unlock straight back. */
void mr_var_mark_baseline(void);
void mr_var_reset_to_baseline(void);

/* Put a value back without journalling it, and remove one entirely. Rollback
 * uses these to undo; an undo must not be recorded as a change. */
void mr_var_set_no_journal(const char *name, const mr_value *v);
void mr_var_unset(const char *name);

/* Evaluate an expression. Returns 0 and sets *out to None on a parse error. */
int mr_expr_eval(const char *src, mr_value *out);

/* Execute a statement: an assignment, a tuple unpack, or a bare call. */
void mr_expr_exec(const char *src);

/* Iterate the variable store, for saving. Returns 0 when i is out of range. */
int mr_var_enumerate(int i, const char **name, const mr_value **v);

/* Substitute `[name]` interpolations from the variable store, dropping
 * Ren'Py's `!conversion` flags.
 *
 * Ren'Py's substitutions.substitute(), which *every* Text displayable runs its
 * string through -- so it applies to dialogue exactly as it does to a screen's
 * `text`. It lived in slrender.c as a static for a long time, which meant
 * interpolation worked everywhere except in the one place a writer is most
 * likely to use it: `m "lalala [selchoice] lalala"` displayed the brackets
 * verbatim. Here because it reads the store, and so that there is one
 * implementation rather than a second one for dialogue.
 *
 * Substitute *after* translating, not before: the translated line is the one
 * with the brackets in it. */
void mr_expr_substitute(const char *src, char *out, int outsz);

#endif /* MR_EXPR_H */
