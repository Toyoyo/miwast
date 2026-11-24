/* style.h - Ren'Py's style table.
 *
 * A style is a named bag of properties with a parent it falls back to. Style
 * names are hierarchical by convention rather than by structure: a style
 * called `navigation_button` inherits from `button` unless told otherwise,
 * which is renpy/style.pyx's `name.partition("_")` rule.
 *
 * States (idle, hover, selected...) are not separate styles. They are encoded
 * as a prefix on the property name, so a hovering button asks for
 * `hover_background` and falls back to `background`. That is how the gui
 * helpers emit them and how the reference resolves them.
 */

#ifndef MR_STYLE_H
#define MR_STYLE_H

#include "mr.h"

void mr_style_reset(void);

/* Create or fetch a style. `parent` may be NULL, in which case it is inferred
 * from the name at lookup time. */
/* Does this build match the named Ren'Py display variant? An empty or NULL
 * name means "no variant given", which always matches. */
int  mr_variant_active(const char *name);

int  mr_style_define(const char *name, const char *parent);

/* Set one property. The value stays as unevaluated expression text. */
void mr_style_set(const char *name, const char *prop, const char *expr);

/* Record a `properties gui.text_properties("kind")` style directive. These
 * cannot be expanded at parse time because they read gui.* variables that the
 * init phase has not yet assigned; mr_style_finalize does it afterwards. */
void mr_style_set_gui(const char *name, const char *kind, int is_text,
                      int accent);

/* Expand every recorded gui.*_properties directive. Call once, after the
 * init-phase assignments have run. */
void mr_style_finalize(void);

/* Look a property up, following the parent chain. Returns the unevaluated
 * expression text, or NULL. */
const char *mr_style_get(const char *style, const char *prop);

/* As above, but tries `<state>_<prop>` before `<prop>`, which is Ren'Py's
 * style prefix search. `state` may be NULL or "". */
const char *mr_style_get_state(const char *style, const char *state,
                               const char *prop);

int mr_style_count(void);

/* Size of the resolution cache, how many slots are occupied, and how many
 * there are. For memreport; the bytes are static, not heap. */
void mr_style_cache_stats(long *bytes, int *entries, int *capacity);

#endif /* MR_STYLE_H */
