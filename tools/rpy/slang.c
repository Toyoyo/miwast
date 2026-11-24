/* slang.c - screen language tree building and name tables.
 *
 * The two tables below decide how a line inside a screen block is read: a
 * leading word naming a displayable starts a child, a leading word naming a
 * property sets a property on the enclosing displayable. They are transcribed
 * from renpy/sl2/sldisplayables.py and renpy/sl2/slproperties.py so that
 * screens written for real Ren'Py parse the same way here.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "slang.h"

/* From sldisplayables.py. Entries the engine cannot draw are still listed:
 * parsing them correctly and skipping them at render time is far better than
 * mistaking one for a property and corrupting the tree. */
static const char *g_displayables[] = {
    "null", "text", "hbox", "vbox", "fixed", "grid", "side",
    "window", "frame", "key", "timer", "input", "button",
    "imagebutton", "textbutton", "label", "bar", "vbar",
    "viewport", "vpgrid", "imagemap", "hotspot", "hotbar",
    "transform", "add", "drag", "draggroup", "mousearea",
    "on", "nearrect", "dismiss", "areapicker",
    "transclude",        /* marks where a `use`d screen's block is inserted */
    NULL
};

/* Ren'Py builds whole families of properties by prefixing a base name:
 * side_position_properties, text_text_properties, the viewport and scrollbar
 * families, and the button state roles. Rather than enumerate the cross
 * product, strip a known prefix and test the remainder. */
static const char *g_prefixes[] = {
    "text_", "side_", "viewport_", "scrollbar_", "vscrollbar_",
    "idle_", "hover_", "selected_", "insensitive_", "activate_",
    NULL
};

/* position_property_names + box + grid + window + text, from
 * slproperties.py, plus the ui properties that can appear on any element. */
static const char *g_properties[] = {
    /* position */
    "anchor", "xanchor", "yanchor", "pos", "xpos", "ypos",
    "align", "xalign", "yalign", "offset", "xoffset", "yoffset",
    "maximum", "xmaximum", "ymaximum", "minimum", "xminimum", "yminimum",
    "area", "clipping", "xfill", "yfill", "xcenter", "ycenter", "xycenter",
    "xsize", "ysize", "xysize", "alt", "debug",

    /* box */
    "box_align", "box_justify", "box_layout", "box_wrap", "box_wrap_spacing",
    "box_reverse", "justify", "order_reverse", "spacing", "first_spacing",
    "fit_first", "xfit", "yfit",

    /* grid */
    "xspacing", "yspacing",

    /* margins and padding */
    "left_margin", "right_margin", "bottom_margin", "top_margin",
    "xmargin", "ymargin", "margin",
    "left_padding", "right_padding", "top_padding", "bottom_padding",
    "xpadding", "ypadding", "padding",

    /* window */
    "background", "foreground", "size_group", "modal",

    /* text */
    "antialias", "vertical", "black_color", "bold", "color",
    "drop_shadow", "drop_shadow_color", "first_indent", "font",
    "font_features", "size", "italic", "justify", "kerning", "language",
    "layout", "line_leading", "line_overlap_split", "line_spacing",
    "minwidth", "min_width", "newline_indent", "outlines", "outline_scaling",
    "rest_indent", "ruby_style", "slow_cps", "slow_cps_multiplier",
    "slow_abortable", "strikethrough", "textalign", "text_align",
    "substitute", "safe",
    "text_y_fudge", "underline", "hinting", "adjust_spacing", "mipmap",

    /* buttons and bars */
    "action", "hovered", "unhovered", "activate_sound", "hover_sound",
    "keysym", "alternate", "alternate_keysym", "selected", "sensitive",
    "focus_mask", "key_events", "value", "adjustment", "range", "changed",
    "bar_invert", "bar_vertical", "bar_resizing", "unscrollable",
    "released", "pressed",

    /* viewport */
    "scrollbars", "mousewheel", "draggable", "edgescroll", "xadjustment",
    "yadjustment", "xinitial", "yinitial", "arrowkeys", "pagekeys",
    "yinitial_offset",

    /* general ui */
    "style", "style_prefix", "style_group", "style_suffix", "id", "at",
    "tooltip", "prefix", "cols", "rows", "transpose", "properties",
    "auto", "width", "height", "insensitive", "hover", "idle",
    "selected_idle", "selected_hover", "ground", "image",

    /* screen-level keywords, which appear among the body lines */
    "tag", "zorder", "variant", "predict", "layer", "roll_forward",

    NULL
};

static int in_table(const char *const *tab, const char *word)
{
    int i;
    for (i = 0; tab[i]; i++) {
        if (!strcmp(tab[i], word))
            return 1;
    }
    return 0;
}

int sl_is_displayable(const char *word)
{
    return in_table(g_displayables, word);
}

static int is_property_depth(const char *word, int depth)
{
    int i;

    if (in_table(g_properties, word))
        return 1;
    if (depth <= 0)
        return 0;

    for (i = 0; g_prefixes[i]; i++) {
        size_t n = strlen(g_prefixes[i]);
        if (!strncmp(word, g_prefixes[i], n) && word[n])
            return is_property_depth(word + n, depth - 1);
    }
    return 0;
}

int sl_is_property(const char *word)
{
    /* Depth 2 covers the deepest real combinations, such as
     * selected_hover_color and scrollbar_xalign. */
    return is_property_depth(word, 2);
}

/* --- tree building --- */

static char *sl_dup(const char *s)
{
    size_t n;
    char *d;

    if (!s)
        return NULL;
    n = strlen(s) + 1;
    d = (char *)malloc(n);
    if (!d)
        mr_fatal("out of memory copying screen language text");
    memcpy(d, s, n);
    return d;
}

sl_node *sl_node_new(sl_kind kind, int line)
{
    sl_node *n = (sl_node *)calloc(1, sizeof(sl_node));

    if (!n)
        mr_fatal("out of memory allocating a screen node");
    n->kind = kind;
    n->line = line;
    return n;
}

void sl_node_add_kid(sl_node *parent, sl_node *kid)
{
    if (!parent || !kid)
        return;

    if (parent->nkids >= parent->capkids) {
        int nc = parent->capkids ? parent->capkids * 2 : 4;
        void *nv = realloc(parent->kids, (size_t)nc * sizeof(sl_node *));
        if (!nv)
            mr_fatal("out of memory growing a screen node's children");
        parent->kids = nv;
        parent->capkids = nc;
    }
    parent->kids[parent->nkids++] = kid;
}

void sl_node_set_prop(sl_node *n, const char *name, const char *expr)
{
    int i;

    if (!n || !name || !*name)
        return;

    for (i = 0; i < n->nprops; i++) {
        if (!strcmp(n->props[i].name, name)) {
            free(n->props[i].expr);
            n->props[i].expr = sl_dup(expr);
            return;
        }
    }

    if (n->nprops >= n->capprops) {
        int nc = n->capprops ? n->capprops * 2 : 4;
        void *nv = realloc(n->props, (size_t)nc * sizeof(sl_prop));
        if (!nv)
            mr_fatal("out of memory growing a screen node's properties");
        n->props = nv;
        n->capprops = nc;
    }

    i = n->nprops++;
    memset(&n->props[i], 0, sizeof(n->props[i]));
    strncpy(n->props[i].name, name, SL_MAX_PROPNAME - 1);
    n->props[i].expr = sl_dup(expr);
}

const char *sl_node_prop(const sl_node *n, const char *name)
{
    int i;

    if (!n)
        return NULL;
    for (i = 0; i < n->nprops; i++) {
        if (!strcmp(n->props[i].name, name))
            return n->props[i].expr;
    }
    return NULL;
}

void sl_node_free(sl_node *n)
{
    int i;

    if (!n)
        return;

    for (i = 0; i < n->nprops; i++)
        free(n->props[i].expr);
    free(n->props);

    for (i = 0; i < n->nkids; i++)
        sl_node_free(n->kids[i]);
    free(n->kids);

    sl_node_free(n->orelse);

    free(n->arg);
    free(n->arg2);
    free(n->cond);
    free(n);
}

void sl_screen_free(sl_screen *s)
{
    int i;

    if (!s)
        return;
    sl_node_free(s->root);
    s->root = NULL;
    for (i = 0; i < s->nparams; i++) {
        free(s->defaults[i]);
        s->defaults[i] = NULL;
    }
    free(s->zorder);
    s->zorder = NULL;
}
