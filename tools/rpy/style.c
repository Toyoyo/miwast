/* style.c - the style table and the gui.*_properties expansions.
 *
 * The expansions mirror renpy/common/00gui.rpy. text_properties(kind) probes
 * gui.<kind>_text_<state><property> for every state prefix and every text
 * property name, and button_properties(kind) reads the borders, tile, width
 * and height variables plus the conventional background image paths. Doing
 * this by probing variables, rather than hard-coding what one game's gui.rpy
 * happens to define, is what lets the same code drive another project.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "style.h"
#include "expr.h"
#include "vfs.h"

#define MR_MAX_STYLES 512
#define STYLE_NAME    64
#define PROP_NAME     40

typedef struct {
    char  name[PROP_NAME];
    char *expr;
} st_prop;

typedef struct {
    char     name[STYLE_NAME];
    char     parent[STYLE_NAME];
    int      has_parent;

    st_prop *props;
    int      nprops, capprops;

    char     gui_kind[STYLE_NAME];
    int      gui_accent;
    int      gui_is_text;
    int      has_gui;
} mr_style;

static mr_style g_styles[MR_MAX_STYLES];
static int      g_nstyles;

/* Defined with the resolution cache below; editing the table invalidates it. */
static void style_cache_clear(void);

/* Ren'Py's style prefix search order, from slparser.STYLE_PREFIXES. */
static const char *g_states[] = {
    "", "idle_", "hover_", "selected_", "insensitive_",
    "selected_idle_", "selected_hover_", "selected_insensitive_",
    NULL
};

/* The text properties worth probing. Taken from slproperties.py's
 * text_property_names, trimmed to those this renderer can act on plus the
 * position properties the gui sets on button text. */
static const char *g_text_props[] = {
    "font", "size", "color", "bold", "italic", "underline", "strikethrough",
    "outlines", "kerning", "line_spacing", "line_leading", "justify",
    "text_align", "textalign", "layout", "min_width", "minwidth",
    "xalign", "yalign", "xpos", "ypos", "xoffset", "yoffset",
    "xmaximum", "ymaximum", "xsize", "ysize",
    NULL
};

static char *st_dup(const char *s)
{
    size_t n;
    char *d;

    if (!s)
        return NULL;
    n = strlen(s) + 1;
    d = (char *)malloc(n);
    if (!d)
        mr_fatal("out of memory in the style table");
    memcpy(d, s, n);
    return d;
}

void mr_style_reset(void)
{
    int i, k;

    for (i = 0; i < g_nstyles; i++) {
        for (k = 0; k < g_styles[i].nprops; k++)
            free(g_styles[i].props[k].expr);
        free(g_styles[i].props);
    }
    memset(g_styles, 0, sizeof(g_styles));
    g_nstyles = 0;
    style_cache_clear();
}

static int find_style(const char *name)
{
    int i;
    for (i = 0; i < g_nstyles; i++) {
        if (!strcmp(g_styles[i].name, name))
            return i;
    }
    return -1;
}

/* Which display variants this build claims to be. Ren'Py selects screens and
 * style blocks by variant, and we are a desktop-sized window with a mouse, so
 * "pc" and "large" match and the touch sizes do not. Kept in one place so the
 * parser (which skips `style x: variant "small"` blocks) and `renpy.variant()`
 * cannot drift apart -- they did, and the preferences columns came out the
 * width of a phone's. */
int mr_variant_active(const char *name)
{
    if (!name || !*name)
        return 1;
    return !strcmp(name, "pc") || !strcmp(name, "large");
}

int mr_style_define(const char *name, const char *parent)
{
    int i;

    if (!name || !*name)
        return -1;

    i = find_style(name);
    if (i < 0) {
        if (g_nstyles >= MR_MAX_STYLES) {
            mr_log("style: table full, dropping '%s'", name);
            return -1;
        }
        i = g_nstyles++;
        memset(&g_styles[i], 0, sizeof(g_styles[i]));
        strncpy(g_styles[i].name, name, STYLE_NAME - 1);
    }

    if (parent && *parent) {
        strncpy(g_styles[i].parent, parent, STYLE_NAME - 1);
        g_styles[i].has_parent = 1;
    }
    style_cache_clear();
    return i;
}

static void style_set_idx(int i, const char *prop, const char *expr)
{
    int k;
    mr_style *s;

    if (i < 0 || i >= g_nstyles || !prop || !*prop)
        return;

    style_cache_clear();
    s = &g_styles[i];

    for (k = 0; k < s->nprops; k++) {
        if (!strcmp(s->props[k].name, prop)) {
            free(s->props[k].expr);
            s->props[k].expr = st_dup(expr);
            return;
        }
    }

    if (s->nprops >= s->capprops) {
        int nc = s->capprops ? s->capprops * 2 : 8;
        void *nv = realloc(s->props, (size_t)nc * sizeof(st_prop));
        if (!nv)
            mr_fatal("out of memory growing style '%s'", s->name);
        s->props = nv;
        s->capprops = nc;
    }

    k = s->nprops++;
    memset(&s->props[k], 0, sizeof(s->props[k]));
    strncpy(s->props[k].name, prop, PROP_NAME - 1);
    s->props[k].expr = st_dup(expr);
}

void mr_style_set(const char *name, const char *prop, const char *expr)
{
    style_set_idx(mr_style_define(name, NULL), prop, expr);
}

void mr_style_set_gui(const char *name, const char *kind, int is_text,
                      int accent)
{
    int i = mr_style_define(name, NULL);

    if (i < 0)
        return;
    strncpy(g_styles[i].gui_kind, kind ? kind : "", STYLE_NAME - 1);
    g_styles[i].gui_is_text = is_text;
    g_styles[i].gui_accent = accent;
    g_styles[i].has_gui = 1;
}

/* --- gui.*_properties expansion --- */

/* Copy the value of a gui variable into `out` as expression text. Returns 0
 * if the variable is unset. */
static int gui_var(const char *name, char *out, int outsz)
{
    mr_value v;

    if (!mr_var_get(name, &v) || v.type == MR_V_NONE)
        return 0;

    switch (v.type) {
    case MR_V_INT:   sprintf(out, "%ld", v.i); break;
    case MR_V_FLOAT: sprintf(out, "%f", v.f); break;
    case MR_V_STR:
        if ((int)strlen(v.s) + 3 > outsz)
            return 0;
        sprintf(out, "\"%s\"", v.s);
        break;
    default:
        return 0;
    }
    (void)outsz;
    return 1;
}

static void expand_text(int idx)
{
    const char *kind = g_styles[idx].gui_kind;
    char var[160], val[160];
    int si, pi;

    /* `accent=True` first, so an explicit gui.<kind>_text_color below still
     * wins -- 00gui.rpy:406 sets rv["color"] before the per-kind probes
     * overwrite it. This is what makes the screen titles and the save
     * screen's page label blue instead of white. */
    if (g_styles[idx].gui_accent && gui_var("gui.accent_color", val, sizeof(val)))
        style_set_idx(idx, "color", val);

    for (si = 0; g_states[si]; si++) {
        for (pi = 0; g_text_props[pi]; pi++) {
            char prop[PROP_NAME];

            if ((int)(strlen(g_states[si]) + strlen(g_text_props[pi])) >=
                    PROP_NAME)
                continue;
            sprintf(prop, "%s%s", g_states[si], g_text_props[pi]);

            /* gui.<kind>_text_<state><prop>, or gui.text_<state><prop> when
             * the helper was called without a kind. */
            if (kind[0])
                sprintf(var, "gui.%s_text_%s", kind, prop);
            else
                sprintf(var, "gui.text_%s", prop);

            if (gui_var(var, val, sizeof(val)))
                style_set_idx(idx, prop, val);
        }
    }

    /* text_xalign also drives text_align and textalign. */
    if (kind[0]) {
        sprintf(var, "gui.%s_text_xalign", kind);
        if (gui_var(var, val, sizeof(val))) {
            style_set_idx(idx, "xalign", val);
            style_set_idx(idx, "text_align", val);
            style_set_idx(idx, "textalign", val);
        }
    }
}

static void expand_button(int idx)
{
    const char *kind = g_styles[idx].gui_kind;
    char var[160], val[160];
    int si;

    sprintf(var, "gui.%s_borders", kind);
    if (gui_var(var, val, sizeof(val)))
        style_set_idx(idx, "padding", val);

    sprintf(var, "gui.%s_width", kind);
    if (gui_var(var, val, sizeof(val)))
        style_set_idx(idx, "xsize", val);

    sprintf(var, "gui.%s_height", kind);
    if (gui_var(var, val, sizeof(val)))
        style_set_idx(idx, "ysize", val);

    sprintf(var, "gui.%s_tile", kind);
    if (gui_var(var, val, sizeof(val)))
        style_set_idx(idx, "tile", val);

    /* Backgrounds, one per state. button_properties() in 00gui.rpy builds a
     * list of candidates and lets the Frame take the first that exists:
     *
     *     gui/button/<kind minus its "_button" tail>_[prefix_]background.png
     *     gui/button/[prefix_]background.png
     *
     * Note the truncation: the style kind is "navigation_button" but the file
     * is gui/button/navigation_idle_background.png. Resolving the list here,
     * once, rather than storing it keeps the renderer's job to a single
     * lookup -- and stops it asking the archive for a file that is never
     * going to be there, every button, every frame. */
    {
        char base[STYLE_NAME];
        int klen = (int)strlen(kind);

        if (klen > 7 && !strcmp(kind + klen - 7, "_button"))
            klen -= 7;
        else
            klen = 0;
        if (klen > 0 && klen < (int)sizeof(base)) {
            memcpy(base, kind, (size_t)klen);
            base[klen] = '\0';
        } else {
            base[0] = '\0';
        }

        for (si = 0; g_states[si]; si++) {
            char prop[PROP_NAME];
            char path[STYLE_NAME + 64];
            char name[STYLE_NAME + 48];

            if (!g_states[si][0])
                continue;
            if ((int)strlen(g_states[si]) + 11 >= PROP_NAME)
                continue;

            name[0] = '\0';
            if (base[0]) {
                sprintf(name, "gui/button/%s_%sbackground.png", base,
                        g_states[si]);
                if (!mr_vfs_exists(name))
                    name[0] = '\0';
            }
            if (!name[0]) {
                sprintf(name, "gui/button/%sbackground.png", g_states[si]);
                if (!mr_vfs_exists(name))
                    continue;
            }

            sprintf(prop, "%sbackground", g_states[si]);

            /* button_properties() wraps the image in a Frame with
             * gui.<kind>_borders. Without that the renderer has no border
             * information and falls back to tiling, which for the choice
             * button -- a 790x35 plate stretched to fit its text -- drew the
             * plate twice with a seam across it instead of stretching the
             * middle. Emitted as a reference to the variable rather than its
             * value so it reads like what the .rpy wrote. */
            {
                char borders[STYLE_NAME + 16];
                mr_value bv;

                sprintf(borders, "gui.%s_borders", kind);
                if (mr_var_get(borders, &bv) && bv.type != MR_V_NONE) {
                    char frame[STYLE_NAME + 128];
                    sprintf(frame, "Frame(\"%s\", %s)", name, borders);
                    style_set_idx(idx, prop, frame);
                } else {
                    sprintf(path, "\"%s\"", name);
                    style_set_idx(idx, prop, path);
                }
            }
        }
    }
}

void mr_style_finalize(void)
{
    int i;
    int n = 0;

    for (i = 0; i < g_nstyles; i++) {
        if (!g_styles[i].has_gui)
            continue;
        if (g_styles[i].gui_is_text)
            expand_text(i);
        else
            expand_button(i);
        n++;
    }
    mr_log("style: %d styles, %d gui expansions", g_nstyles, n);
}

/* --- lookup --- */

static const char *own_prop(int i, const char *prop)
{
    int k;
    for (k = 0; k < g_styles[i].nprops; k++) {
        if (!strcmp(g_styles[i].props[k].name, prop))
            return g_styles[i].props[k].expr;
    }
    return NULL;
}

/* The part after the first underscore, which is the implicit parent.
 * A leading underscore is skipped first, so _foo_bar inherits from _bar. */
static const char *implicit_parent(const char *name)
{
    const char *p = name;

    if (*p == '_')
        p++;
    p = strchr(p, '_');
    return p ? p + 1 : NULL;
}

long g_style_lookups, g_style_steps;

/* --- resolution cache -----------------------------------------------------
 *
 * Resolving one property walks the inheritance chain, and every step scans the
 * whole style table for a name. The preferences screen asked for 3950
 * properties per frame, costing 11524 chain steps and something like 600,000
 * string comparisons -- enough on a Pentium III to starve the audio mixer.
 *
 * Styles never change after mr_style_finalize, so a resolution can be
 * remembered for good. Misses are cached too: most lookups are for properties
 * that are not set anywhere, and those are the ones that walk the entire chain
 * before giving up.
 */

#define STYLE_CACHE 4096

typedef struct {
    char        style[STYLE_NAME];
    char        prop[PROP_NAME];
    const char *val;
    int         used;
} style_cache_ent;

static style_cache_ent g_cache[STYLE_CACHE];

/* For memreport. Fixed-size and static, so this is BSS rather than heap and
 * the allocation report cannot see it at all -- which is the point of
 * reporting it: on the target that is real memory in the linker map. `used`
 * is how much of it is occupied, which is what says whether the size is
 * right. */
void mr_style_cache_stats(long *bytes, int *entries, int *capacity)
{
    int i, n = 0;

    for (i = 0; i < STYLE_CACHE; i++) {
        if (g_cache[i].used)
            n++;
    }
    *bytes = (long)sizeof(g_cache);
    *entries = n;
    *capacity = STYLE_CACHE;
}

static void style_cache_clear(void)
{
    memset(g_cache, 0, sizeof(g_cache));
}

static unsigned style_hash(const char *a, const char *b)
{
    unsigned h = 2166136261u;

    while (*a) { h = (h ^ (unsigned char)*a++) * 16777619u; }
    h = (h ^ '/') * 16777619u;
    while (*b) { h = (h ^ (unsigned char)*b++) * 16777619u; }
    return h;
}

static const char *resolve(const char *style, const char *prop);

const char *mr_style_get(const char *style, const char *prop)
{
    unsigned slot;
    style_cache_ent *e;

    g_style_lookups++;

    if (!style || !*style || !prop)
        return NULL;
    if ((int)strlen(style) >= STYLE_NAME || (int)strlen(prop) >= PROP_NAME)
        return resolve(style, prop);

    slot = style_hash(style, prop) & (STYLE_CACHE - 1);
    e = &g_cache[slot];

    if (e->used && !strcmp(e->style, style) && !strcmp(e->prop, prop))
        return e->val;

    /* One entry per slot: a collision simply replaces it. The working set is
     * a few hundred pairs against a thousand slots, so this settles. */
    e->val = resolve(style, prop);
    strcpy(e->style, style);
    strcpy(e->prop, prop);
    e->used = 1;
    return e->val;
}

static const char *resolve(const char *style, const char *prop)
{
    int guard = 0;
    char cur[STYLE_NAME];

    strncpy(cur, style, STYLE_NAME - 1);
    cur[STYLE_NAME - 1] = '\0';

    /* The guard stops a cycle in hand-written style inheritance from
     * hanging the renderer. */
    while (guard++ < 16) {
        int i;
        const char *v;
        const char *next;

        g_style_steps++;
        i = find_style(cur);

        if (i >= 0) {
            v = own_prop(i, prop);
            if (v)
                return v;
        }

        if (i >= 0 && g_styles[i].has_parent) {
            strncpy(cur, g_styles[i].parent, STYLE_NAME - 1);
            continue;
        }

        next = implicit_parent(cur);
        if (!next)
            return NULL;
        memmove(cur, next, strlen(next) + 1);
    }

    mr_log("style: inheritance loop at '%s'", style);
    return NULL;
}

/* Where a prefix looks when it does not define a property itself. Ren'Py
 * gives each prefix a priority rather than a search list, but the effect is
 * this order: a selected button falls back on what `selected_` says, then on
 * the unselected state, then on the style's own value. It is what lets
 * `gui.button_text_selected_color` colour a button whose style only ever
 * mentions idle and hover. */
static const char *const *state_chain(const char *state)
{
    static const char *const sel_idle[]  = { "selected_idle_", "selected_",
                                             "idle_", NULL };
    static const char *const sel_hover[] = { "selected_hover_", "selected_",
                                             "hover_", NULL };
    static const char *const sel[]       = { "selected_", "idle_", NULL };
    static const char *const insens[]    = { "insensitive_", "idle_", NULL };

    if (!strcmp(state, "selected_idle_"))  return sel_idle;
    if (!strcmp(state, "selected_hover_")) return sel_hover;
    if (!strcmp(state, "selected_"))       return sel;
    if (!strcmp(state, "insensitive_"))    return insens;

    /* idle_, hover_ and anything else stand alone. */
    return NULL;
}

const char *mr_style_get_state(const char *style, const char *state,
                               const char *prop)
{
    if (state && *state) {
        const char *const *chain = state_chain(state);
        const char *one[2];
        int i;

        if (!chain) {
            one[0] = state;
            one[1] = NULL;
            chain = one;
        }

        for (i = 0; chain[i]; i++) {
            char full[PROP_NAME];
            const char *v;

            if ((int)(strlen(chain[i]) + strlen(prop)) >= PROP_NAME)
                continue;
            sprintf(full, "%s%s", chain[i], prop);
            v = mr_style_get(style, full);
            if (v)
                return v;
        }
    }
    return mr_style_get(style, prop);
}

int mr_style_count(void)
{
    return g_nstyles;
}
