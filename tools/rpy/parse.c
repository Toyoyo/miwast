/* parse.c - .rpy logical lines to a flat statement array.
 *
 * The grammar handled here is a subset of Ren'Py's, and a small one: say,
 * scene/show/hide with positions and transitions, menu, if/elif/else,
 * jump/call/return, the audio statements, window, pause, `$` lines, and the
 * define/image declarations.
 *
 * It began as "exactly what Miwa no Utsuroi uses", surveyed from its 9000
 * lines, and each game since has added to it -- so treat the list above as
 * what has been needed so far rather than as a boundary anyone chose. The
 * fixtures in tools/harness are built from the *grammar* each parse_* accepts
 * rather than from the usage that prompted it, for that reason.
 * Anything else is recorded as a NOP with a log line rather than being treated
 * as a fatal error, so an unrecognised statement costs a cosmetic glitch
 * instead of refusing to run the game.
 *
 * Indentation drives block structure. Each parse_block call consumes every
 * line at or deeper than its own column, and control flow is resolved by
 * emitting jumps with placeholder targets that get patched once the extent of
 * the block is known.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "mr.h"
#include "lex.h"
#include "script.h"
#include "style.h"
#include "slparse.h"
#include "tl.h"
#include "pyfn.h"
#include "list.h"
#include "plat.h"    /* mr_plat_watch_write, for scriptcheck */

/* The watched copy of the statement array; see script.h and
 * mr_script_snapshot below. Declared up here because mr_script_load, which
 * is much further down, checks at every file boundary. */
static mr_stmt *g_snap;
static int      g_snap_n;
static int      g_check_each_file;

typedef struct parser {
    mr_script *s;
    mr_lines  *l;
    int        i;       /* current line */
    int        file;    /* index into the script's file name table */

    /* Translation identifiers. `tl_label` is the enclosing label, which names
     * the identifier; Ren'Py disambiguates two statements that produce the
     * same identifier with a numeric suffix, and its scope for doing so is one
     * file, so `tl_seen` is reset per file. Only the hashes are kept: a
     * genuine duplicate is the same label and the same dialogue, so a hash
     * match is confirmed against the identifier before counting it. */
    char       tl_label[MR_MAX_NAME];
    mr_u32    *tl_seen;
    int        tl_nseen, tl_capseen;
} parser;

/* How many statements already in this file carry this identifier. */
static int tl_dup_count(parser *p, const char *id)
{
    mr_u32 h = 2166136261u;
    const char *s = id;
    int i, n = 0;

    while (*s)
        h = (h ^ (mr_u32)(unsigned char)*s++) * 16777619u;

    for (i = 0; i < p->tl_nseen; i++)
        if (p->tl_seen[i] == h)
            n++;

    if (p->tl_nseen >= p->tl_capseen) {
        int nc = p->tl_capseen ? p->tl_capseen * 2 : 1024;
        void *nv = realloc(p->tl_seen, (size_t)nc * sizeof(mr_u32));
        if (!nv)
            mr_fatal("out of memory tracking translation identifiers");
        p->tl_seen = (mr_u32 *)nv;
        p->tl_capseen = nc;
    }
    p->tl_seen[p->tl_nseen++] = h;
    return n;
}

/* --- statement emission --- */

/* Grow the statement array by hand rather than with realloc.
 *
 * Not a style preference. On FreeDOS with JEMMEX or JEMM386, `realloc`
 * across a megabyte returns a block that does not hold what it was given:
 * `probe/memwalk.c` grows a block by doubling and loses 556,819 of
 * 557,056 bytes at the 557,056 -> 1,114,112 step, with no engine
 * involved. That step is exactly this array going from 2048 to 4096
 * statements of 272 bytes, so the parse completed with correct counts and
 * the init pass then read a copy of the program image. See NOTES, "Not
 * with Jemm".
 *
 * It is the engine's own arithmetic that lands on the broken size, so the
 * engine can step around it. The cost is nil and arguably negative:
 *
 * - realloc does this same allocate-copy-free whenever it cannot extend
 *   in place, and it cannot extend this one. Every doubling allocates a
 *   dup_text string per statement in between, so the space after the
 *   array is always taken by the time it needs to grow.
 * - This copies `used` statements; realloc copies the whole old block.
 *   Fewer bytes, not more. The whole parse moves about 4MB in total,
 *   which is tens of milliseconds on the target, once, at startup.
 * - Peak memory is the same either way: old and new both exist during
 *   the copy.
 *
 * Fragmentation is the one real difference, and it is small. Where
 * realloc could have extended in place, this leaves the old block as a
 * hole -- but that hole is immediately useful, because what follows is
 * thousands of small script-text allocations that fit in it. A hole of
 * exactly the previous array size, in an allocator that is about to be
 * asked for a great many small blocks, is close to the best case.
 *
 * Only this array is treated this way. Every other growing table in the
 * engine stays on realloc because none of them can reach the size where
 * the fault lives: the lexer's line table is 16 bytes an entry (82KB for
 * the longest script here), the persistent seen-set 4 bytes, and the
 * screen, style, character and label tables are smaller still. If one of
 * them ever grows into megabytes, it wants this too.
 */
static mr_stmt *grow_stmts(mr_stmt *old, int used, int newcap)
{
    mr_stmt *nv = (mr_stmt *)malloc((size_t)newcap * sizeof(mr_stmt));

    if (!nv)
        return NULL;

    if (old) {
        memcpy(nv, old, (size_t)used * sizeof(mr_stmt));
        free(old);
    }
    return nv;
}

/* Every statement so far, checked for a pointer that no allocator could
 * have produced. Only the alignment is tested, which sounds weak and is
 * not: malloc returns storage aligned for any type, so a non-NULL pointer
 * with its low bits set was written by something that was not malloc.
 * That is a true invariant on every target, costs five compares per
 * statement, and needs no knowledge of where the heap is.
 *
 * Run from emit every 128 statements under `scriptcheck`, which is what
 * makes it useful: it localises damage to within 128 statements of when it
 * happened, including inside the first file's parse, where the per-file
 * checks in mr_script_load were still one boundary too late to see
 * anything. Reports once and then disarms, since the first report is the
 * informative one and the rest would be the same damage found again.
 */
static int g_align_reported;

static void check_pointers(const mr_script *s, int at)
{
    int i;

    if (g_align_reported)
        return;

    for (i = 0; i < s->n; i++) {
        const mr_stmt *st = &s->st[i];
        const void *bad = NULL;
        const char *which = NULL;

        if ((unsigned long)st->text & 3UL)  { bad = st->text; which = "text"; }
        else if ((unsigned long)st->tlid & 3UL)
                                            { bad = st->tlid; which = "tlid"; }
        else if ((unsigned long)st->anim & 3UL)
                                            { bad = st->anim; which = "anim"; }
        else if ((unsigned long)st->atl & 3UL)
                                            { bad = st->atl;  which = "atl"; }

        if (!bad)
            continue;

        mr_log("scriptcheck: statement %d has a misaligned %s (%p), found "
               "while emitting statement %d -- it was written by something "
               "that is not malloc", i, which, bad, at);
        g_align_reported = 1;
        return;
    }
}

static mr_stmt *emit(parser *p, mr_st_kind kind)
{
    mr_stmt *st;

    if (g_check_each_file && p->s->n && (p->s->n & 127) == 0)
        check_pointers(p->s, p->s->n);

    if (p->s->n >= p->s->cap) {
        int nc = p->s->cap ? p->s->cap * 2 : 1024;
        mr_stmt *nv = grow_stmts(p->s->st, p->s->n, nc);
        if (!nv)
            mr_fatal("out of memory growing the script to %d statements", nc);
        p->s->st = nv;
        p->s->cap = nc;

        /* The array just moved, so the watchpoint has to follow it. Four
         * bytes at the base: that is where the damage lands -- statement
         * 0, byte 0 -- and it is written exactly once legitimately, by the
         * very first emit. Every write after that is the one being hunted.
         * See plat.h. */
        if (g_check_each_file)
            mr_plat_watch_write(p->s->st);
    }

    st = &p->s->st[p->s->n++];
    memset(st, 0, sizeof(*st));
    st->kind = kind;
    st->target = -1;
    st->volume = 1.0f;
    st->file = p->file;
    st->line = (p->i < p->l->n) ? p->l->v[p->i].line : 0;
    return st;
}

/* Block-introducing keywords are written with the colon stuck to them:
 * `else:`, `menu:`, `python:`. mr_next_word splits on whitespace only, so the
 * colon has to be removed before any keyword comparison. */
static void strip_colon(char *w)
{
    int n = (int)strlen(w);
    while (n > 0 && (w[n - 1] == ':' || w[n - 1] == ' '))
        w[--n] = '\0';
}

static char *dup_text(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (!d)
        mr_fatal("out of memory copying %lu bytes of script text",
                 (unsigned long)n);
    /* Under `scriptcheck`: is the allocator itself handing back nonsense?
     * malloc returns suitably aligned storage by definition, so a
     * misaligned return is not a pointer at all. Worth asking because
     * every garbage value seen in a damaged statement so far -- 8d004a7e,
     * 8b02e2c1, c70a7e3f -- was misaligned, and "something wrote over the
     * struct" and "the struct was filled in with a bad pointer" are
     * opposite bugs that look identical afterwards. */
    if (g_check_each_file && ((unsigned long)d & 3UL))
        mr_log("scriptcheck: malloc(%lu) returned %p, which is not aligned",
               (unsigned long)n, (void *)d);
    memcpy(d, s, n);
    return d;
}

static void add_label(parser *p, const char *name, int stmt)
{
    if (p->s->nlabels >= p->s->caplabels) {
        int nc = p->s->caplabels ? p->s->caplabels * 2 : 128;
        void *nv = realloc(p->s->labels, (size_t)nc * sizeof(*p->s->labels));
        if (!nv)
            mr_fatal("out of memory growing the label table");
        p->s->labels = nv;
        p->s->caplabels = nc;
    }
    strncpy(p->s->labels[p->s->nlabels].name, name, MR_MAX_NAME - 1);
    p->s->labels[p->s->nlabels].name[MR_MAX_NAME - 1] = '\0';
    p->s->labels[p->s->nlabels].stmt = stmt;
    p->s->nlabels++;
}

/* --- transitions --- */

void mr_trans_parse(mr_transition *t, const char *name)
{
    memset(t, 0, sizeof(*t));
    t->color = MR_RGB(0, 0, 0);

    /* Ren'Py's stock timings, which the game never overrides except through
     * Fade(...). dissolve is 0.5s, fade is 0.5s out plus 0.5s in, and the
     * move family is 0.5s. */
    if (!strcmp(name, "dissolve")) {
        t->kind = MR_TRANS_DISSOLVE;
        t->ms = 500;
    } else if (!strcmp(name, "fade") || !strncmp(name, "Fade", 4)) {
        t->kind = MR_TRANS_FADE;
        t->ms = 1000;
    } else if (!strcmp(name, "move")) {
        t->kind = MR_TRANS_MOVE;
        t->ms = 500;
    } else if (!strcmp(name, "moveinleft")) {
        t->kind = MR_TRANS_MOVEINLEFT;
        t->ms = 500;
    } else if (!strcmp(name, "moveinright")) {
        t->kind = MR_TRANS_MOVEINRIGHT;
        t->ms = 500;
    } else if (!strcmp(name, "moveoutleft")) {
        t->kind = MR_TRANS_MOVEOUTLEFT;
        t->ms = 500;
    } else if (!strcmp(name, "moveoutright")) {
        t->kind = MR_TRANS_MOVEOUTRIGHT;
        t->ms = 500;
    } else if (!strcmp(name, "moveintop")) {
        t->kind = MR_TRANS_MOVEINTOP;
        t->ms = 500;
    } else if (!strcmp(name, "moveinbottom")) {
        t->kind = MR_TRANS_MOVEINBOTTOM;
        t->ms = 500;
    } else if (!strcmp(name, "moveouttop")) {
        t->kind = MR_TRANS_MOVEOUTTOP;
        t->ms = 500;
    } else if (!strcmp(name, "moveoutbottom")) {
        t->kind = MR_TRANS_MOVEOUTBOTTOM;
        t->ms = 500;
    } else if (!strcmp(name, "MoveTransition")) {
        /* The constructor the whole move family is built from
         * (00definitions.rpy:250). Written out longhand it is what a script
         * reaches for when it wants a duration other than the stock 0.5s:
         *
         *     with MoveTransition(2.0, enter=None, leave=None, old=False,
         *                         layers=['master'], time_warp=None, ...)
         *
         * Every keyword there is Ren'Py's own default (movetransition.py:457),
         * so that is `move` with a 2 second delay -- and the delay is picked
         * up from the first argument by the caller. Falling through to the
         * `else` below made it a dissolve, which is what it looked like.
         *
         * A non-default enter=/leave=/old= is not honoured; see read_with,
         * which says so in the log rather than quietly doing the wrong
         * thing. */
        t->kind = MR_TRANS_MOVE;
        t->ms = 500;
    } else if (!strcmp(name, "None") || !strcmp(name, "none")) {
        t->kind = MR_TRANS_NONE;
        t->ms = 0;
    } else {
        /* `unscrollable` is a custom transition defined by this game; treating
         * an unknown transition as a dissolve is always visually safe. */
        t->kind = MR_TRANS_DISSOLVE;
        t->ms = 500;
    }
}

/* Read `with <name>` if present, leaving *t as NONE otherwise. */
static const char *read_with(const char *q, mr_transition *t, int *has)
{
    const char *r;
    char word[MR_MAX_NAME];

    memset(t, 0, sizeof(*t));
    *has = 0;

    r = mr_match_word(q, "with");
    if (!r)
        return q;

    /* Fade(1.0, 0.0, 1.0) and friends arrive as a single token with parens. */
    {
        const char *e = r;
        int n = 0;
        while (*e && *e != ' ' && *e != '(' && n < MR_MAX_NAME - 1) {
            word[n++] = *e++;
        }
        word[n] = '\0';

        mr_trans_parse(t, word);

        /* If it is a call, take the durations from its arguments:
         * Fade(out_time, hold_time, in_time) in seconds. */
        if (*e == '(') {
            double a = 0, b = 0, c = 0;
            const char *ap = e + 1;
            const char *np;
            np = mr_read_number(ap, &a);
            if (np) {
                ap = mr_skip_ws(np);
                if (*ap == ',') {
                    np = mr_read_number(ap + 1, &b);
                    if (np) {
                        ap = mr_skip_ws(np);
                        if (*ap == ',')
                            np = mr_read_number(ap + 1, &c);
                    }
                }
            }
            if (a > 0 || b > 0 || c > 0)
                t->ms = (long)((a + b + c) * 1000.0);

            /* The keywords we cannot honour. A MoveTransition with an
             * enter= or leave= transform animates images that appear or
             * disappear, and old=True moves the outgoing copy rather than
             * the incoming one -- none of which render_move does. Said out
             * loud, because the alternative is a transition that looks
             * subtly wrong with nothing to connect it to a cause. The
             * defaults are None, None and False, so the common longhand
             * call is silent. */
            {
                const char *k;
                for (k = ap; *k && *k != ')'; k++) {
                    if ((!strncmp(k, "enter=", 6) &&
                         strncmp(k + 6, "None", 4)) ||
                        (!strncmp(k, "leave=", 6) &&
                         strncmp(k + 6, "None", 4)) ||
                        (!strncmp(k, "old=", 4) &&
                         !strncmp(k + 4, "True", 4))) {
                        mr_log("parse: %s: enter/leave/old are not honoured "
                               "(%.60s)", word, ap);
                        break;
                    }
                }
            }

            while (*e && *e != ')')
                e++;
            if (*e == ')')
                e++;
        }
        *has = 1;
        return mr_skip_ws(e);
    }
}

/* --- positions --- */

/* Returns 0 if the name is not one of the stock positions, so the caller can
 * tell "not a position" from "a position that happens to set nothing". The
 * game may define a transform with any of these names and expect its own --
 * they are store variables in Ren'Py (00definitions.rpy:34-48) and a later
 * `transform center:` simply overwrites one -- so a named transform is looked
 * up first, in the `at` handling, and this is only the fallback. */
static int place_from_name(mr_place *pl, const char *name)
{
    /* Ren'Py's stock positions, and they name *both* axes
     * (00definitions.rpy:34-48):
     *
     *     transform left:   xpos 0.0 xanchor 0.0 ypos 1.0 yanchor 1.0
     *     transform right:  xpos 1.0 xanchor 1.0 ypos 1.0 yanchor 1.0
     *     transform center: xpos 0.5 xanchor 0.5 ypos 1.0 yanchor 1.0
     *
     * Only x used to be set here, with y left to fall through to the
     * resolver's "stand on the bottom" default. That gave the right answer
     * for the wrong reason, and the reason stopped being harmless once the
     * resolver learned that a placement *given* means unspecified axes are
     * zero, not defaulted -- see mr_place_resolve. Saying ypos 1.0 keeps
     * `at center` exactly where it has always been. */
    if (!strcmp(name, "left")) {
        pl->has_xalign = 1;
        pl->xalign = 0.0f;
        pl->has_yalign = 1;
        pl->yalign = 1.0f;
    } else if (!strcmp(name, "right")) {
        pl->has_xalign = 1;
        pl->xalign = 1.0f;
        pl->has_yalign = 1;
        pl->yalign = 1.0f;
    } else if (!strcmp(name, "center")) {
        pl->has_xalign = 1;
        pl->xalign = 0.5f;
        pl->has_yalign = 1;
        pl->yalign = 1.0f;
    } else if (!strcmp(name, "truecenter")) {
        pl->has_xalign = 1;
        pl->has_yalign = 1;
        pl->xalign = 0.5f;
        pl->yalign = 0.5f;
    } else if (!strcmp(name, "topleft")) {
        pl->has_xalign = pl->has_yalign = 1;
        pl->xalign = 0.0f;
        pl->yalign = 0.0f;
    } else if (!strcmp(name, "topright")) {
        pl->has_xalign = pl->has_yalign = 1;
        pl->xalign = 1.0f;
        pl->yalign = 0.0f;
    } else {
        return 0;
    }
    return 1;
}

/* Read a number and report whether it was written as an integer, since that is
 * what separates a pixel count from a fraction of the stage. */
static const char *read_num_kind(const char *p, double *out, int *is_px)
{
    const char *start = mr_skip_ws(p);
    const char *end = mr_read_number(start, out);
    const char *s;

    if (!end)
        return NULL;

    *is_px = 1;
    for (s = start; s < end; s++) {
        if (*s == '.' || *s == 'e' || *s == 'E') {
            *is_px = 0;
            break;
        }
    }
    return end;
}

/* Set one property on a placement. */
static void set_place_prop(mr_place *pl, const char *word, double v, int is_px)
{
    if (!strcmp(word, "xoffset"))      { pl->has_xoffset = 1; pl->xoffset = (int)v; }
    else if (!strcmp(word, "yoffset")) { pl->has_yoffset = 1; pl->yoffset = (int)v; }
    /* align is sugar: Ren'Py's set_xalign does `xpos = v; xanchor = v`, and
     * the int/float rule then applies to both. `yalign 1` is therefore *one
     * pixel*, not the bottom of the stage -- the anchor one pixel down from
     * the image's top sits one pixel down the screen, which puts the image
     * at the top. `yalign 1.0` is the bottom. Expanding here rather than
     * keeping an align of our own is what makes that fall out. */
    else if (!strcmp(word, "xalign"))  { pl->has_xpos = 1; pl->xpos = (float)v; pl->xpos_px = is_px;
                                         pl->has_xanchor = 1; pl->xanchor = (float)v; pl->xanchor_px = is_px; }
    else if (!strcmp(word, "yalign"))  { pl->has_ypos = 1; pl->ypos = (float)v; pl->ypos_px = is_px;
                                         pl->has_yanchor = 1; pl->yanchor = (float)v; pl->yanchor_px = is_px; }
    /* Not a position, but it is set by the same syntax in the same block.
     * Ren'Py's default is 1.0, so `alpha 1.0` and "no alpha given" mean the
     * same thing and has_alpha only decides whether the blitter takes its
     * fast path. */
    else if (!strcmp(word, "alpha"))   { pl->has_alpha = 1; pl->alpha = (float)v; }
    /* Scale factors, and always factors: `xzoom 2` is float(2) in Ren'Py, not
     * two pixels, so is_px is deliberately ignored here where every position
     * property above consults it. Negative is legal and mirrors. */
    else if (!strcmp(word, "xzoom"))   { pl->has_xzoom = 1; pl->xzoom = (float)v; }
    else if (!strcmp(word, "yzoom"))   { pl->has_yzoom = 1; pl->yzoom = (float)v; }
    /* Kept apart from the two above rather than multiplied in here: see the
     * note in script.h. */
    else if (!strcmp(word, "zoom"))    { pl->has_zoom = 1; pl->zoom = (float)v; }
    else if (!strcmp(word, "xpos"))    { pl->has_xpos = 1; pl->xpos = (float)v; pl->xpos_px = is_px; }
    else if (!strcmp(word, "ypos"))    { pl->has_ypos = 1; pl->ypos = (float)v; pl->ypos_px = is_px; }
    else if (!strcmp(word, "xanchor")) { pl->has_xanchor = 1; pl->xanchor = (float)v; pl->xanchor_px = is_px; }
    else if (!strcmp(word, "yanchor")) { pl->has_yanchor = 1; pl->yanchor = (float)v; pl->yanchor_px = is_px; }
    else if (!strcmp(word, "xcenter")) { pl->has_xpos = 1; pl->xpos = (float)v; pl->xpos_px = is_px;
                                         pl->has_xanchor = 1; pl->xanchor = 0.5f; pl->xanchor_px = 0; }
    else if (!strcmp(word, "ycenter")) { pl->has_ypos = 1; pl->ypos = (float)v; pl->ypos_px = is_px;
                                         pl->has_yanchor = 1; pl->yanchor = 0.5f; pl->yanchor_px = 0; }
    else
        mr_log("parse: ignoring transform property '%s'", word);
}

/* One line of an inline transform block. Either a bare position name, a
 * property assignment, or a timed step: `linear 90 yanchor 1.0 ypos 0`, which
 * interpolates toward that placement over ninety seconds.
 *
 * `atl` collects the timed steps; it may be NULL where animation is not
 * supported, in which case a step is applied as an immediate jump. */
static void place_property(mr_place *pl, mr_atl *atl, const char *line)
{
    char word[MR_MAX_NAME];
    const char *q;
    double v;
    int is_px;

    q = mr_next_word(line, word, sizeof(word));

    if (!strcmp(word, "linear") || !strcmp(word, "ease") ||
        !strcmp(word, "easein") || !strcmp(word, "easeout")) {
        mr_place *target = pl;
        double secs;

        if (!read_num_kind(q, &secs, &is_px))
            return;
        q = read_num_kind(q, &secs, &is_px);

        if (atl && atl->n < MR_MAX_ATL) {
            mr_atl_step *st = &atl->v[atl->n++];
            memset(st, 0, sizeof(*st));
            st->ms = (long)(secs * 1000.0);
            st->warp = strcmp(word, "linear") ? MR_WARP_EASE : MR_WARP_LINEAR;
            target = &st->place;
        } else if (atl) {
            /* Out of step slots. Said out loud because the alternative is an
             * animation that silently plays only its first MR_MAX_ATL steps,
             * and that presents as "the transform looks wrong" with nothing
             * to connect it to a limit. A NULL atl is different and not
             * warned about: that is a caller that does not support animation
             * at all, where a step is applied as an immediate jump by
             * design. */
            mr_log("parse: transform already has %d timed steps, the most "
                   "MR_MAX_ATL allows; dropping '%s'", MR_MAX_ATL, line);
        }

        while (*q) {
            q = mr_next_word(q, word, sizeof(word));
            if (!word[0])
                break;
            {
                const char *nq = read_num_kind(q, &v, &is_px);
                if (!nq) {
                    /* No number after it, so it is a bare position name:
                     * `linear 0.5 right`, which is as legal in a timed step
                     * as `xalign 1.0` is. The untimed branch below has always
                     * handled this; the timed one used to stop here, which
                     * left the step with an empty target and the image
                     * standing still for the whole of its duration.
                     *
                     * Anything else is a property we do not know, and
                     * stopping is the only safe answer -- the rest of the
                     * line cannot be read without knowing whether that word
                     * took an argument. */
                    if (!place_from_name(target, word))
                        break;
                    continue;
                }
                q = nq;
            }
            set_place_prop(target, word, v, is_px);
        }
        return;
    }

    if (!strcmp(word, "repeat")) {
        if (atl)
            atl->repeat = 1;
        return;
    }

    if (!strcmp(word, "pause")) {
        if (read_num_kind(q, &v, &is_px) && atl) {
            if (atl->n < MR_MAX_ATL) {
                mr_atl_step *st = &atl->v[atl->n++];
                memset(st, 0, sizeof(*st));
                st->ms = (long)(v * 1000.0);
            } else {
                mr_log("parse: transform already has %d timed steps, the "
                       "most MR_MAX_ATL allows; dropping '%s'",
                       MR_MAX_ATL, line);
            }
        }
        return;
    }

    /* Several properties may share a line: `ypos 720 yanchor 0.0 xalign 0.5`.
     * Once a timed step has been seen, later properties belong to it. */
    {
        mr_place *target = (atl && atl->n > 0) ? &atl->v[atl->n - 1].place : pl;

        while (word[0]) {
            const char *nq = read_num_kind(q, &v, &is_px);
            if (!nq) {
                /* A bare position name, e.g. just `left` or `center`. */
                place_from_name(target, word);
            } else {
                set_place_prop(target, word, v, is_px);
                q = nq;
            }
            q = mr_next_word(q, word, sizeof(word));
        }
    }
}

/* --- audio statement tails --- */

static void read_audio_opts(mr_stmt *st, const char *q)
{
    char word[MR_MAX_NAME];
    double v;

    while (*q) {
        const char *next = mr_next_word(q, word, sizeof(word));
        if (!word[0])
            break;
        q = next;

        if (!strcmp(word, "loop")) {
            st->loop = 1;
        } else if (!strcmp(word, "noloop")) {
            st->loop = 0;
        } else if (!strcmp(word, "fadein")) {
            const char *nq = mr_read_number(q, &v);
            if (nq) { st->fadein = (long)(v * 1000.0); q = nq; }
        } else if (!strcmp(word, "fadeout")) {
            const char *nq = mr_read_number(q, &v);
            if (nq) { st->fadeout = (long)(v * 1000.0); q = nq; }
        } else if (!strcmp(word, "volume")) {
            const char *nq = mr_read_number(q, &v);
            if (nq) { st->volume = (float)v; q = nq; }
        }
    }
}

/* --- declarations --- */

static mr_u32 parse_hex_color(const char *s)
{
    unsigned r = 0, g = 0, b = 0;

    if (*s == '#')
        s++;
    if (strlen(s) >= 6) {
        sscanf(s, "%2x%2x%2x", &r, &g, &b);
        return MR_RGB(r, g, b);
    }
    return MR_RGB(255, 255, 255);
}

/* `define m = Character("Miwa", color="#204e89", ...)`
 * and `define config.main_menu_music = "audio/..."`. */
static void parse_define(parser *p, const char *q)
{
    char var[MR_MAX_NAME];
    const char *eq;
    const char *r;

    q = mr_next_word(q, var, sizeof(var));
    eq = mr_skip_ws(q);
    if (*eq != '=') {
        mr_log("parse: malformed define at line %d", p->l->v[p->i].line);
        return;
    }
    eq = mr_skip_ws(eq + 1);

    r = mr_match_word(eq, "Character");
    if (!r || *r != '(') {
        /* Not a character. Config and story values become init-time
         * assignments evaluated before the story starts.
         *
         * The buffer is sized from the operands rather than fixed: the lexer
         * joins bracket-continued lines, so a `define` spanning several
         * physical lines arrives here as one very long logical line. */
        mr_stmt *st = emit(p, MR_ST_INIT);
        size_t need = strlen(var) + strlen(eq) + 4;
        char *joined = (char *)malloc(need);

        if (!joined)
            mr_fatal("out of memory joining a define of %lu bytes",
                     (unsigned long)need);
        sprintf(joined, "%s = %s", var, eq);
        st->text = joined;
        return;
    }

    /* Character("Name", color="#rrggbb", ...) */
    {
        mr_character *c;
        const char *ap = r + 1;
        char who[MR_MAX_NAME];

        if (p->s->nchars >= p->s->capchars) {
            int nc = p->s->capchars ? p->s->capchars * 2 : 32;
            void *nv = realloc(p->s->chars, (size_t)nc * sizeof(mr_character));
            if (!nv)
                mr_fatal("out of memory growing the character table");
            p->s->chars = nv;
            p->s->capchars = nc;
        }
        c = &p->s->chars[p->s->nchars++];
        memset(c, 0, sizeof(*c));
        strncpy(c->var, var, MR_MAX_NAME - 1);
        c->color = MR_RGB(255, 255, 255);

        /* The displayed name is the first argument, sometimes wrapped in the
         * translation marker _("..."). */
        ap = mr_skip_ws(ap);
        if (*ap == '_') {
            ap++;
            ap = mr_skip_ws(ap);
            if (*ap == '(')
                ap++;
        }
        if (mr_read_string(ap, who, sizeof(who)))
            strncpy(c->who, who, MR_MAX_NAME - 1);

        /* colour, wherever it appears in the argument list */
        {
            const char *cp = strstr(r, "color=");
            if (cp) {
                char col[32];
                if (mr_read_string(cp + 6, col, sizeof(col)))
                    c->color = parse_hex_color(col);
            }
        }
    }
}

/* Paths in the scripts are relative to the game directory, but a bare
 * filename means images/ -- the convention every `image` form shares. */
static void image_path(char *out, int outsz, const char *file)
{
    /* Deliberately strcpy/strncat rather than sprintf with a `%.*s`
     * precision. The precision form is correct C, but this is one of the few
     * functions on the path every `image` declaration in every game takes,
     * and the Watcom runtime is the thing that has to agree about it -- the
     * harnesses build with gcc and cannot speak for it. Nothing here needs
     * formatting, so nothing here uses a formatter. */
    if (strchr(file, '/') || strchr(file, '\\')) {
        strncpy(out, file, (size_t)outsz - 1);
        out[outsz - 1] = '\0';
    } else {
        strcpy(out, "images/");
        strncat(out, file, (size_t)outsz - strlen("images/") - 1);
    }
}

/* The block form of `image`: an animation.
 *
 *     image fireworks animated:
 *         "images/fireworks0.png" with dissolve
 *         pause 2.2
 *         ...
 *         repeat
 *
 * Each quoted line is a frame and each `pause` is how long the frame *before*
 * it stays up -- see the note on mr_anim in script.h for why that way round.
 * A `with` on a frame line is the cross-fade into that frame.
 *
 * Returns a freshly allocated mr_anim, or NULL if the block had no frames.
 * Allocated rather than filled on the stack because an mr_anim is about four
 * kilobytes, which is not something to put on a Win32s stack. */
static mr_anim *parse_anim_block(parser *p, int outer_indent)
{
    mr_anim *a;
    int      i;

    a = (mr_anim *)malloc(sizeof(*a));
    if (!a) {
        /* Still consume the block, or its lines get parsed as statements. */
        while (p->i < p->l->n && p->l->v[p->i].indent > outer_indent)
            p->i++;
        return NULL;
    }
    memset(a, 0, sizeof(*a));

    while (p->i < p->l->n && p->l->v[p->i].indent > outer_indent) {
        const char *ln = mr_skip_ws(p->l->v[p->i].text);
        const char *r;
        int         srcline = p->l->v[p->i].line;

        p->i++;

        if (mr_string_quote(ln, NULL)) {
            char          file[MR_PATH_MAX];
            mr_transition tr;
            int           has_with;

            r = mr_read_string(ln, file, sizeof(file));
            if (!r) {
                mr_log("parse: unreadable animation frame at line %d", srcline);
                continue;
            }
            if (a->n >= MR_MAX_ANIM) {
                /* Said out loud for the same reason the MR_MAX_ATL overflow
                 * is: the symptom of dropping it quietly is an animation that
                 * plays part of itself, which looks like a timing bug and has
                 * nothing pointing at a limit. */
                mr_log("parse: animation already has %d frames, the most "
                       "MR_MAX_ANIM allows; dropping the one at line %d",
                       MR_MAX_ANIM, srcline);
                continue;
            }

            image_path(a->v[a->n].file, MR_PATH_MAX, file);
            a->v[a->n].ms = 0;
            a->v[a->n].trans_ms = 0;

            r = read_with(mr_skip_ws(r), &tr, &has_with);
            if (has_with && tr.kind != MR_TRANS_NONE) {
                /* Only a cross-fade is meaningful between two frames of one
                 * image: the move family slides a whole display list around,
                 * and there is no display list here. A fade would flash to
                 * black inside the image, which no script means by it.
                 * Anything else is treated as the dissolve it almost always
                 * is, with its own duration kept. */
                a->v[a->n].trans_ms = tr.ms;
            }
            a->n++;
            continue;
        }

        r = mr_match_word(ln, "pause");
        if (r) {
            double v;
            if (!mr_read_number(r, &v)) {
                mr_log("parse: animation pause without a duration at line %d",
                       srcline);
            } else if (a->n == 0) {
                /* A pause before any frame. Ren'Py would hold the previous
                 * child, and there is none, so there is nothing for it to
                 * hold. Dropped, and said so. */
                mr_log("parse: animation pause before its first frame at "
                       "line %d; ignored", srcline);
            } else {
                a->v[a->n - 1].ms += (long)(v * 1000.0);
            }
            continue;
        }

        if (mr_match_word(ln, "repeat")) {
            a->repeat = 1;
            continue;
        }

        mr_log("parse: unsupported line in animation block at line %d (%s)",
               srcline, ln);
    }

    if (a->n == 0) {
        mr_log("parse: animation block with no frames");
        free(a);
        return NULL;
    }

    for (i = 0; i < a->n; i++)
        a->total_ms += a->v[i].ms;

    return a;
}

/* `image miwa flip = im.Flip("miwa.png", horizontal="True")`
 * `image bg blurred = im.Blur("bg.png", 2.0)`
 * `image x = "file.png"`
 *
 * The `=` forms. Does not touch p->i: every failure path here returns early,
 * and making each of them remember to advance is how a parser ends up in an
 * infinite loop on one malformed line. parse_image does the advancing. */
static void parse_image_assign(parser *p, const char *q)
{
    char name[MR_MAX_NAME];
    char file[MR_PATH_MAX];
    const char *eq;
    int n;
    int flip;
    float xrad, yrad;
    float xscale, yscale;

    /* mr_next_word leaves the caller just past the keyword, so the name is
     * preceded by the separating space; without this the registered name would
     * be " miwa flip" and nothing would ever match it. */
    q = mr_skip_ws(q);

    /* The name runs up to the '='. */
    eq = strchr(q, '=');
    if (!eq) {
        mr_log("parse: image without '=' at line %d", p->l->v[p->i].line);
        return;
    }

    n = (int)(eq - q);
    while (n > 0 && (q[n - 1] == ' ' || q[n - 1] == '\t'))
        n--;
    if (n > MR_MAX_NAME - 1)
        n = MR_MAX_NAME - 1;
    memcpy(name, q, (size_t)n);
    name[n] = '\0';

    eq = mr_skip_ws(eq + 1);
    flip = 0;
    xrad = 0.0f;
    yrad = 0.0f;
    xscale = 0.0f;      /* zero is "not scaled"; see mr_stmt's note */
    yscale = 0.0f;

    if (!strncmp(eq, "im.Flip", 7)) {
        const char *ap = strchr(eq, '(');
        if (!ap)
            return;
        if (!mr_read_string(ap + 1, file, sizeof(file)))
            return;
        /* The game only ever flips horizontally. */
        flip = 1;
    } else if (!strncmp(eq, "im.Blur", 7)) {
        /* im.Blur(im, xrad, yrad=None): one radius means a circular kernel
         * (im.py:1666, `self.ry = xrad if yrad is None else yrad`). The
         * child may itself be a manipulator; im.Flip is the only one this
         * engine knows, and the two compose. */
        const char *ap = strchr(eq, '(');
        const char *np;
        double d;

        if (!ap)
            return;
        ap = mr_skip_ws(ap + 1);

        if (!strncmp(ap, "im.Flip", 7)) {
            const char *fp = strchr(ap, '(');
            if (!fp)
                return;
            flip = 1;
            ap = fp + 1;
        }

        /* Reading the filename also yields the position just past it, which
         * is where the radii have to be looked for: a filename may contain a
         * comma of its own, so the first comma in the line is not it. */
        np = mr_read_string(ap, file, sizeof(file));
        if (!np)
            return;

        /* A nested im.Flip brings its own arguments, so the radii start after
         * its closing paren rather than after the filename. */
        if (flip) {
            np = strchr(np, ')');
            if (!np)
                return;
            np++;
        }

        np = strchr(np, ',');
        if (!np || !(np = mr_read_number(np + 1, &d))) {
            mr_log("parse: im.Blur without a radius for '%s'", name);
            return;
        }
        xrad = (float)d;
        yrad = xrad;

        np = mr_skip_ws(np);
        if (*np == ',' && mr_read_number(np + 1, &d))
            yrad = (float)d;

        if (xrad < 0.0f || yrad < 0.0f) {
            mr_log("parse: im.Blur with a negative radius for '%s'", name);
            return;
        }
    } else if (!strncmp(eq, "im.FactorScale", 14)) {
        /* im.FactorScale(im, width, height=None, bilinear=True): the second
         * factor defaults to the first (im.py:1313-1314), and the size is
         * `int(w * factor)` -- truncated, not rounded (im.py:1334-1335).
         *
         * `bilinear=False` would mean nearest neighbour. It is read and
         * ignored: the resampler has no nearest path, and no game passes it.
         *
         * Azami writes this 76 times and 74 of them are a factor of exactly
         * 1.0, under the comment "Duplications with dummy FactorScale" -- a
         * way of getting several distinct image *names* for one file, so that
         * five vampires can be on screen at once. Those 74 names did not
         * exist at all until this branch did: the declaration was logged as
         * unsupported and dropped, and every `show vampire_dup3` with it. */
        const char *ap = strchr(eq, '(');
        const char *np;
        double d;

        if (!ap)
            return;
        ap = mr_skip_ws(ap + 1);

        if (!strncmp(ap, "im.Flip", 7)) {
            const char *fp = strchr(ap, '(');
            if (!fp)
                return;
            flip = 1;
            ap = fp + 1;
        }

        np = mr_read_string(ap, file, sizeof(file));
        if (!np)
            return;
        if (flip) {
            np = strchr(np, ')');
            if (!np)
                return;
            np++;
        }

        np = strchr(np, ',');
        if (!np || !(np = mr_read_number(np + 1, &d))) {
            mr_log("parse: im.FactorScale without a factor for '%s'", name);
            return;
        }
        xscale = (float)d;
        yscale = xscale;

        np = mr_skip_ws(np);
        if (*np == ',' && mr_read_number(np + 1, &d))
            yscale = (float)d;

        /* A factor of zero or less has no image to produce. Dropped with a
         * line rather than clamped, because clamping would hide a typo as a
         * one-pixel sprite. */
        if (xscale <= 0.0f || yscale <= 0.0f) {
            mr_log("parse: im.FactorScale with a factor of %.3f/%.3f for "
                   "'%s'", xscale, yscale, name);
            return;
        }
    } else if (mr_string_quote(eq, NULL)) {
        if (!mr_read_string(eq, file, sizeof(file)))
            return;
    } else {
        mr_log("parse: unsupported image definition for '%s' (%s)", name, eq);
        return;
    }

    /* The interpreter registers every one of these with the image module in a
     * single pass before execution starts. */
    {
        mr_stmt *st = emit(p, MR_ST_IMAGE);
        char buf[MR_PATH_MAX];

        strncpy(st->name, name, MR_MAX_NAME - 1);
        image_path(buf, (int)sizeof(buf), file);
        st->text = dup_text(buf);
        st->aux = flip;
        st->blur_x = xrad;
        st->blur_y = yrad;
        st->scale_x = xscale;
        st->scale_y = yscale;
    }
}

static void parse_image(parser *p, const char *q, int indent)
{
    const char *line = p->l->v[p->i].text;
    int         n;

    /* A trailing colon introduces an animation block. Tested on the whole
     * logical line rather than at the parse position, for the same reason the
     * show statement does it that way: the colon ends up attached to whatever
     * word precedes it, so looking for it as a token misses it. */
    n = (int)strlen(line);
    while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t'))
        n--;

    if (n > 0 && line[n - 1] == ':') {
        char     name[MR_MAX_NAME];
        mr_anim *anim;
        int      len;

        /* The name runs from after `image` up to the colon. */
        q = mr_skip_ws(q);
        len = (int)(line + n - 1 - q);
        while (len > 0 && (q[len - 1] == ' ' || q[len - 1] == '\t'))
            len--;
        if (len <= 0) {
            mr_log("parse: image block without a name at line %d",
                   p->l->v[p->i].line);
            p->i++;
            return;
        }
        if (len > MR_MAX_NAME - 1)
            len = MR_MAX_NAME - 1;
        memcpy(name, q, (size_t)len);
        name[len] = '\0';

        p->i++;
        anim = parse_anim_block(p, indent);
        if (anim) {
            mr_stmt *st = emit(p, MR_ST_IMAGE);
            strncpy(st->name, name, MR_MAX_NAME - 1);
            st->anim = anim;
        }
        return;
    }

    parse_image_assign(p, q);
    p->i++;
}

/* --- screens --- */

static void parse_screen(parser *p, int indent)
{
    sl_screen sc;

    if (!mr_sl_parse_screen(p->l, &p->i, indent, &sc)) {
        p->i++;
        return;
    }

    if (p->s->nscreens >= p->s->capscreens) {
        int nc = p->s->capscreens ? p->s->capscreens * 2 : 32;
        void *nv = realloc(p->s->screens, (size_t)nc * sizeof(sl_screen));
        if (!nv)
            mr_fatal("out of memory growing the screen table");
        p->s->screens = nv;
        p->s->capscreens = nc;
    }
    p->s->screens[p->s->nscreens++] = sc;
}

/* --- python blocks --- */

/* `init python:` and `python:` blocks.
 *
 * Function definitions are picked out and kept, because screens call them --
 * `default mr = build_music_room()`.
 *
 * Everything else at the top level of the block is emitted as an MR_ST_INIT
 * statement, i.e. evaluated in the init phase alongside `define` and
 * `default`. It used to be skipped wholesale, which was fine while the only
 * such lines set gui.* values that a `define` set as well -- and stopped
 * being fine at `config.overlay_screens.append("quick_menu")`, the one line
 * that puts the quick menu on the screen. Skipping it meant the overlay
 * never appeared and nothing said why.
 *
 * The whole game has three such lines, all `config.*`. A line the evaluator
 * cannot make sense of -- `config.keymap['screenshot'] = [...]`, which needs
 * subscript assignment -- logs and does nothing, which is what it did before
 * when the entire block was ignored.
 *
 * Only the top level: the body of a def belongs to the def.
 */
static void parse_python(parser *p, int indent)
{
    int block_indent = -1;

    p->i++;

    while (p->i < p->l->n && p->l->v[p->i].indent > indent) {
        const char *line = mr_skip_ws(p->l->v[p->i].text);
        const char *rest = mr_match_word(line, "def");
        int def_indent = p->l->v[p->i].indent;

        if (block_indent < 0)
            block_indent = def_indent;

        if (!rest || !(isalpha((unsigned char)*rest) || *rest == '_')) {
            if (def_indent == block_indent && *line && *line != '#') {
                mr_stmt *st = emit(p, MR_ST_INIT);
                st->text = dup_text(line);
            }
            p->i++;
            continue;
        }

        {
            char name[64];
            int n = 0;

            while ((isalnum((unsigned char)*rest) || *rest == '_') &&
                   n < (int)sizeof(name) - 1)
                name[n++] = *rest++;
            name[n] = '\0';

            /* Only a no-argument function can be called from a screen the way
             * this engine calls one, and every definition here is one. */
            mr_pyfn_begin(name);
            p->i++;

            while (p->i < p->l->n && p->l->v[p->i].indent > def_indent) {
                mr_pyfn_line(p->l->v[p->i].text, p->l->v[p->i].indent);
                p->i++;
            }
        }
    }
}

/* --- styles --- */

/* `style navigation_button is gui_button` and/or
 *
 *     style navigation_button:
 *         size_group "navigation"
 *         properties gui.button_properties("navigation_button")
 *
 * Both forms may appear for the same style; the second adds to the first.
 */
static void parse_style(parser *p, int indent)
{
    char name[64], parent[64];
    const char *q;

    q = mr_match_word(p->l->v[p->i].text, "style");
    if (!q) {
        p->i++;
        return;
    }

    q = mr_next_word(q, name, sizeof(name));
    strip_colon(name);
    parent[0] = '\0';

    {
        const char *r = mr_match_word(q, "is");
        if (r) {
            mr_next_word(r, parent, sizeof(parent));
            strip_colon(parent);
        }
    }

    p->i++;

    /* A style block may be guarded by `variant "small"` and so on, and the
     * guard can appear anywhere in the block, so it has to be found before
     * any of the block is applied. The phone and tablet blocks come last in
     * screens.rpy, so applying them regardless silently wins every conflict:
     * that is how the preferences columns ended up 400px wide instead of
     * 225. */
    {
        int start = p->i;
        int skip = 0;

        while (p->i < p->l->n && p->l->v[p->i].indent > indent) {
            char prop[64];
            const char *v = mr_next_word(p->l->v[p->i].text, prop,
                                         sizeof(prop));
            strip_colon(prop);
            if (!strcmp(prop, "variant")) {
                char want[64];
                if (mr_read_string(mr_skip_ws(v), want, sizeof(want)))
                    skip = !mr_variant_active(want);
            }
            p->i++;
        }

        if (skip)
            return;
        p->i = start;
    }

    mr_style_define(name, parent[0] ? parent : NULL);

    while (p->i < p->l->n && p->l->v[p->i].indent > indent) {
        const char *line = p->l->v[p->i].text;
        char prop[64];
        const char *v;

        v = mr_next_word(line, prop, sizeof(prop));
        strip_colon(prop);
        v = mr_skip_ws(v);

        if (!strcmp(prop, "properties")) {
            /* properties gui.<kind>_properties("name"[, accent=True]) */
            const char *open = strchr(v, '(');
            int is_text = (strstr(v, "text_properties") != NULL);
            /* `accent=True` colours the text with gui.accent_color
             * (00gui.rpy:406). It is what makes the screen titles and the
             * save screen's page label blue rather than white. */
            int accent = (strstr(v, "accent=True") != NULL);
            char kind[64];

            if (open && mr_read_string(open + 1, kind, sizeof(kind)))
                mr_style_set_gui(name, kind, is_text, accent);
            else if (open)
                mr_style_set_gui(name, "", is_text, accent);
        } else if (prop[0]) {
            mr_style_set(name, prop, v);
        }
        p->i++;
    }
}

/* --- statements --- */

static void parse_block(parser *p, int indent);

/* Consume an indented transform block attached to a show/scene statement.
 * Any timed steps go into a freshly allocated mr_atl, which is handed back
 * through *atl_out (left NULL when the block does not animate). */
static void parse_place_block(parser *p, int outer_indent, mr_place *pl,
                              mr_atl **atl_out)
{
    mr_atl atl;

    memset(&atl, 0, sizeof(atl));

    while (p->i < p->l->n && p->l->v[p->i].indent > outer_indent) {
        place_property(pl, &atl, p->l->v[p->i].text);
        p->i++;
    }

    if (atl_out && atl.n > 0) {
        *atl_out = (mr_atl *)malloc(sizeof(atl));
        if (*atl_out)
            **atl_out = atl;
    }
}

/* --- named transforms ---
 *
 *     transform left640:
 *         xoffset 640.0
 *         yalign 1.0
 *
 *     transform leftground:
 *         xoffset 0.0
 *         yalign 1.0
 *         linear 0.5 yoffset 300
 *
 * and then `show eari uniform at left640`.
 *
 * In Ren'Py this is an ordinary store variable holding an ATLTransform
 * (parser.py:1047, ast.py:1323-1329), which is why a game can redefine `left`
 * or `truecenter` and why the body is the same ATL grammar an inline `show x:`
 * block uses. Both of those fall out here: the body goes through
 * place_property, the same function the inline block uses, and a lookup
 * beats the stock positions in place_from_name.
 *
 * The statement is wrapped in an Init (parser.py:1052-1053), so every
 * transform in the game exists before a single line of story runs -- a
 * transform may be used above the line that defines it, and in another file
 * entirely. We parse in one pass over the files, so a name not yet seen is
 * recorded as a fixup and resolved once every file is in; see resolve_ats.
 *
 * What is *not* supported is the rest of ATL: rotate, blur, crop, size,
 * parallel/choice/contains blocks, `time`, event handlers, and transforms
 * with parameters (atl.py:1034-1050 is only the position aliases).
 * Positions, `alpha` and `zoom`/`xzoom`/`yzoom` are.
 * An unsupported property is logged by set_place_prop and skipped, which
 * leaves the placement it did understand rather than dropping the transform.
 */
typedef struct named_transform {
    char     name[MR_MAX_NAME];
    mr_place place;
    mr_atl   atl;              /* n == 0 when the transform does not animate */
} named_transform;

static named_transform *g_tf;
static int              g_ntf;
static int              g_captf;

/* A use of `at <name>` that named a transform we had not read yet. The
 * statement is held by index, not by pointer: emit() reallocs the statement
 * array as the script grows, and a pointer taken now would dangle long before
 * the fixups are applied. */
typedef struct at_fixup {
    int  stmt;
    char name[MR_MAX_NAME];
    int  file;                 /* for the message if it stays unresolved */
    int  line;
} at_fixup;

static at_fixup *g_fix;
static int       g_nfix;
static int       g_capfix;

static void transforms_reset(void)
{
    free(g_tf);
    g_tf = NULL;
    g_ntf = g_captf = 0;
    free(g_fix);
    g_fix = NULL;
    g_nfix = g_capfix = 0;
}

static const named_transform *find_transform(const char *name)
{
    int i;

    for (i = 0; i < g_ntf; i++) {
        if (!strcmp(g_tf[i].name, name))
            return &g_tf[i];
    }
    return NULL;
}

/* `transform <name>:` and its block. A redefinition replaces the earlier one,
 * as assigning the store variable twice would. */
static void parse_transform(parser *p, int indent)
{
    char name[MR_MAX_NAME];
    const char *q;
    named_transform *t;
    int myline = p->i;

    q = mr_next_word(p->l->v[p->i].text, name, sizeof(name));   /* "transform" */
    q = mr_next_word(q, name, sizeof(name));
    strip_colon(name);

    /* `transform delayed_blink(delay, cycle):` takes arguments, which means
     * its body reads values we have no way to supply. Skipped whole rather
     * than registered with the arguments ignored: a half-applied parametrised
     * transform puts the image somewhere nobody asked for, which is harder to
     * recognise than its absence. */
    if (!name[0] || strchr(name, '(')) {
        if (name[0]) {
            char *paren = strchr(name, '(');
            if (paren)
                *paren = '\0';
            mr_log("parse: transform '%s' takes parameters, skipping it "
                   "(%s line %d)", name, p->s->files[p->file],
                   p->l->v[myline].line);
        }
        p->i++;
        while (p->i < p->l->n && p->l->v[p->i].indent > indent)
            p->i++;
        return;
    }

    t = (named_transform *)find_transform(name);
    if (!t) {
        if (g_ntf >= g_captf) {
            int nc = g_captf ? g_captf * 2 : 32;
            named_transform *nv = (named_transform *)realloc(
                g_tf, (size_t)nc * sizeof(named_transform));
            if (!nv)
                mr_fatal("out of memory growing the transform table to %d", nc);
            g_tf = nv;
            g_captf = nc;
        }
        t = &g_tf[g_ntf++];
    }

    memset(t, 0, sizeof(*t));
    strncpy(t->name, name, MR_MAX_NAME - 1);

    p->i++;
    while (p->i < p->l->n && p->l->v[p->i].indent > indent) {
        char word[MR_MAX_NAME];

        mr_next_word(p->l->v[p->i].text, word, sizeof(word));
        strip_colon(word);

        /* `on show:` / `on hide:` are event handlers (atl.py's RawEvent), and
         * their bodies run only when that event fires. Miwa's and Hinata's
         * `transform notify_appear:` is one. Everything above an `on` is the
         * transform's base state and is kept; the handlers are dropped,
         * because applying a handler's body unconditionally would put the
         * image wherever the *hide* animation ends and call it a placement.
         * The rest of the block goes with it -- a second `on` is still a
         * handler, and a property after one belongs to it. */
        if (!strcmp(word, "on")) {
            mr_log("parse: transform '%s' has an `on` block, which needs "
                   "events we do not have; keeping its base placement only "
                   "(%s line %d)", t->name, p->s->files[p->file],
                   p->l->v[p->i].line);
            while (p->i < p->l->n && p->l->v[p->i].indent > indent)
                p->i++;
            break;
        }

        place_property(&t->place, &t->atl, p->l->v[p->i].text);
        p->i++;
    }
}

/* Apply a transform's placement to one a statement is building.
 *
 * `under` false is the normal direction, src on top: `at a, b` applies a then
 * b and Ren'Py's later transform wins. `under` true is for a fixup settled
 * after the statement was read, where the statement's own inline block is
 * already in dst and must survive.
 *
 * Alignment and absolute position are alternatives for the same axis, so
 * setting one clears the other -- the same rule as scene.c's place_merge,
 * which this deliberately mirrors rather than shares: that one is static to
 * the display layer, and parse.c is linked without it by dumpscript and by
 * every parser-only harness. If the rule changes it has to change in both,
 * which is the cost of keeping those link lines short. */
static void place_apply(mr_place *dst, const mr_place *src, int under)
{
#define APPLY_F(has, field) \
    if (src->has && !(under && dst->has)) \
        { dst->has = 1; dst->field = src->field; }
#define APPLY_P(has, field, px) \
    if (src->has && !(under && dst->has)) \
        { dst->has = 1; dst->field = src->field; dst->px = src->px; }
    APPLY_F(has_xoffset, xoffset)
    APPLY_F(has_yoffset, yoffset)
    APPLY_F(has_alpha, alpha)
    APPLY_F(has_xzoom, xzoom)
    APPLY_F(has_yzoom, yzoom)
    APPLY_F(has_zoom, zoom)
    APPLY_P(has_xanchor, xanchor, xanchor_px)
    APPLY_P(has_yanchor, yanchor, yanchor_px)

    /* The exclusive pairs, after the rest so that clearing the other form
     * cannot be undone by a later line of this function. */
    if (src->has_xalign && !(under && (dst->has_xalign || dst->has_xpos))) {
        dst->has_xalign = 1;
        dst->xalign = src->xalign;
        dst->has_xpos = 0;
    }
    if (src->has_xpos && !(under && (dst->has_xpos || dst->has_xalign))) {
        dst->has_xpos = 1;
        dst->xpos = src->xpos;
        dst->xpos_px = src->xpos_px;
        dst->has_xalign = 0;
    }
    if (src->has_yalign && !(under && (dst->has_yalign || dst->has_ypos))) {
        dst->has_yalign = 1;
        dst->yalign = src->yalign;
        dst->has_ypos = 0;
    }
    if (src->has_ypos && !(under && (dst->has_ypos || dst->has_yalign))) {
        dst->has_ypos = 1;
        dst->ypos = src->ypos;
        dst->ypos_px = src->ypos_px;
        dst->has_yalign = 0;
    }
#undef APPLY_F
#undef APPLY_P
}

/* Apply a named transform to a statement being built. Returns 0 if no
 * transform of that name is known, which is the caller's cue to fall back to
 * the stock positions or to record a fixup. */
static int apply_transform(const char *name, mr_place *pl, mr_atl **atl_out,
                           int under)
{
    const named_transform *t = find_transform(name);

    if (!t)
        return 0;

    /* Straight over the top on the immediate path: `at a, b` applies a then
     * b, and Ren'Py's later transform wins, which is what overwriting gives.
     * Underneath only when this is a fixup being settled after the fact, by
     * which time the statement's own inline block is already in `pl` and must
     * not be undone. The two differ for a deferred `at a, b` where both are
     * forward references and both set the same property -- nothing in any of
     * these games does that, and it is called out here rather than silently
     * got wrong. */
    place_apply(pl, &t->place, under);

    /* The timed steps come along with the placement: `at leftground` is what
     * makes the sprite rise from below, and dropping them would leave it
     * standing still with no sign that anything was lost. An inline block on
     * the same statement has already produced an mr_atl and wins -- it is
     * applied after the transform in Ren'Py, and nothing in these games does
     * both. */
    if (t->atl.n > 0 && atl_out && !*atl_out) {
        *atl_out = (mr_atl *)malloc(sizeof(mr_atl));
        if (*atl_out)
            **atl_out = t->atl;
    }
    return 1;
}

/* Read the argument of a show clause.
 *
 * `list` is for `behind keiko,azami`, which takes a comma-separated list; the
 * others take one name. When `out` is non-NULL the argument is collected into
 * it, normalised to `a,b` with no spaces so the consumer can split on commas
 * alone; otherwise it is simply stepped over, which is the point for the
 * clauses we parse but do not act on -- without that the loop never reaches
 * the `at` sitting behind them.
 *
 * `clause` is only for the one-time log; pass NULL for a clause that is
 * acted on. Azami has 338 `behind`s, so a line apiece would bury the log the
 * way the image table's drop messages did. */
static const char *skip_clause_arg(const char *r, int list, const char *clause,
                                   char *out, int outsz)
{
    int outn = 0;

    if (out && outsz > 0)
        out[0] = '\0';

    if (clause) {
        static char told[4][12];
        int i;

        for (i = 0; i < 4; i++) {
            if (!told[i][0]) {
                strncpy(told[i], clause, sizeof(told[i]) - 1);
                mr_log("parse: `%s` on a show statement is parsed but not "
                       "acted on", clause);
                break;
            }
            if (!strcmp(told[i], clause))
                break;
        }
    }

    for (;;) {
        char w[MR_MAX_NAME];
        const char *next = mr_next_word(r, w, sizeof(w));
        int n;

        if (!w[0])
            return r;
        r = next;

        /* A trailing colon ends the statement line, not just the clause. */
        strip_colon(w);

        /* Collected as `a,b`: one separator, no spaces, so the consumer
         * splits on commas and nothing else. An over-long list is truncated
         * rather than reallocated -- `behind` names a handful of tags, and a
         * buffer that grows with the script is the worse answer. */
        if (out) {
            const char *s = w;

            if (outn > 0 && outn < outsz - 1 && out[outn - 1] != ',')
                out[outn++] = ',';
            while (*s && outn < outsz - 1) {
                if (*s != ' ')
                    out[outn++] = *s;
                s++;
            }
            out[outn] = '\0';
        }

        if (!list)
            return r;

        /* `behind a, b`, `behind a,b` and `behind a ,b` are all one list.
         * Only a comma at the very *end* of the word continues it -- `a,b`
         * is already whole, and treating any embedded comma as "more to
         * come" would swallow the `at` that follows. */
        n = (int)strlen(w);
        if (n > 0 && w[n - 1] == ',')
            continue;
        r = mr_skip_ws(r);
        if (*r == ',') {
            r++;
            continue;
        }
        return r;
    }
}

/* Is this line a `say`? Either a bare string, or a known-ish variable name
 * followed by a string. */
static int looks_like_say(parser *p, const char *line, char *who, int whosz,
                          const char **textpos)
{
    const char *q;
    char word[MR_MAX_NAME];

    q = mr_skip_ws(line);
    if (mr_string_quote(q, NULL)) {
        who[0] = '\0';
        *textpos = q;
        return 1;
    }

    q = mr_next_word(q, word, sizeof(word));
    q = mr_skip_ws(q);
    if (*q != '"' && *q != '\'')
        return 0;

    /* A word followed by a string is a say only if the word names a character;
     * otherwise it is something like `play music "..."` that we have already
     * failed to match, and guessing wrong would silently eat a statement. */
    if (!mr_script_find_char(p->s, word))
        return 0;

    strncpy(who, word, (size_t)whosz - 1);
    who[whosz - 1] = '\0';
    *textpos = q;
    return 1;
}

/* --- the lines inside a `menu` block ---
 *
 * Not all of them are choices, and that is the whole reason this exists as a
 * function instead of an inline quote test. A menu's body can hold
 *
 *     menu:
 *         h "So, was it a date or not?"     <- a say menuitem: the caption
 *         "Just some text"                  <- a caption, narrated
 *         with dissolve                     <- a transition for the menu
 *         set seen_choices                  <- a set to filter against
 *         "Yes...":                         <- an actual choice
 *             ...
 *
 * and the two passes below have to agree exactly on which is which. They did
 * not: pass one emitted a CHOICE only for a line starting with a quote, while
 * pass two treated *every* line at the body indent as an option and handed it
 * the next CHOICE in sequence. So one caption shifted every option's target
 * by one -- the first choice jumped to the caption's (empty) block and ran
 * straight past the menu, the second ran the first's block, and the last
 * option's block became unreachable. Reported from play, on a menu whose
 * caption was a say menuitem. One classifier, called from both passes, is
 * what makes that class of bug impossible rather than fixed. */
typedef enum {
    ML_CHOICE,     /* "caption" [if cond]: with a block */
    ML_CAPTION,    /* a bare string on a line of its own */
    ML_SAY,        /* <character> "..." */
    ML_WITH,
    ML_SET,
    ML_UNKNOWN
} menu_line;

/* Classify one line of a menu body.
 *
 * For the three kinds that carry a string -- a choice, a caption, a say
 * menuitem -- `text` receives that string and `rest` points at whatever
 * follows it, which for a choice is the `if` clause and the colon. The string
 * is handed back rather than left to the caller to re-read: the first version
 * of this returned a position whose meaning differed by kind, and the caption
 * branch read from the wrong end of it and came out empty. */
static menu_line classify_menu_line(parser *p, const char *ml, char *who,
                                    int whosz, char *text, int textsz,
                                    const char **rest)
{
    const char *after;
    const char *textpos;

    who[0] = '\0';
    text[0] = '\0';
    *rest = NULL;

    if (mr_match_word(ml, "with"))
        return ML_WITH;
    if (mr_match_word(ml, "set"))
        return ML_SET;

    /* A string first: a choice if anything follows it, a caption if not.
     * Ren'Py decides it the same way round -- read the label, then
     * `if l.eol()` makes it a caption (parser.py:279). */
    if (mr_string_quote(ml, NULL)) {
        after = mr_read_string(ml, text, textsz);
        if (!after)
            return ML_UNKNOWN;
        *rest = after;
        return *after ? ML_CHOICE : ML_CAPTION;
    }

    /* Otherwise the only other thing it can be is a say menuitem, which
     * looks_like_say recognises -- including its requirement that the word be
     * a declared character, so `play music "x"` indented into a menu is not
     * mistaken for one. */
    if (looks_like_say(p, ml, who, whosz, &textpos) && who[0]) {
        after = mr_read_string(textpos, text, textsz);
        if (!after)
            return ML_UNKNOWN;
        *rest = after;
        return ML_SAY;
    }

    return ML_UNKNOWN;
}

static void parse_statement(parser *p, int indent)
{
    const char *line = p->l->v[p->i].text;
    int         myline = p->i;
    char        word[MR_MAX_NAME];
    const char *q;

    q = mr_next_word(line, word, sizeof(word));
    strip_colon(word);

    /* ---- label ---- */
    if (!strcmp(word, "label")) {
        char name[MR_MAX_NAME];
        mr_stmt *st;
        mr_read_imagename(q, name, sizeof(name));
        /* Strip the trailing colon and any parameter list. */
        {
            char *c = strchr(name, ':');
            if (c) *c = '\0';
            c = strchr(name, '(');
            if (c) *c = '\0';
            while (*name && name[strlen(name) - 1] == ' ')
                name[strlen(name) - 1] = '\0';
        }
        st = emit(p, MR_ST_LABEL);
        strncpy(st->name, name, MR_MAX_NAME - 1);
        add_label(p, name, p->s->n - 1);

        /* Translation identifiers are named after the enclosing label, but
         * Ren'Py ignores labels beginning with an underscore for this purpose:
         * those are its own internal entry points, and a say inside one still
         * belongs to the story label that contains it. */
        if (name[0] != '_') {
            strncpy(p->tl_label, name, MR_MAX_NAME - 1);
            p->tl_label[MR_MAX_NAME - 1] = '\0';
        }
        p->i++;
        parse_block(p, indent + 1);
        return;
    }

    /* ---- show/hide screen ---- */
    if (!strcmp(word, "show") || !strcmp(word, "hide")) {
        const char *r = mr_match_word(q, "screen");
        if (r) {
            mr_stmt *st;
            char sname[MR_MAX_NAME];
            mr_transition tr;
            int has;
            const char *ap = mr_next_word(r, sname, sizeof(sname));

            strip_colon(sname);
            st = emit(p, !strcmp(word, "show") ? MR_ST_SHOWSCREEN
                                               : MR_ST_HIDESCREEN);
            strncpy(st->name, sname, MR_MAX_NAME - 1);

            read_with(ap, &tr, &has);
            p->i++;
            if (has) {
                mr_stmt *w = emit(p, MR_ST_WITH);
                w->trans = tr;
            }
            return;
        }
    }

    /* ---- scene / show / hide ---- */
    if (!strcmp(word, "scene") || !strcmp(word, "show") ||
        !strcmp(word, "hide")) {
        int is_scene = !strcmp(word, "scene");
        int is_hide  = !strcmp(word, "hide");
        mr_stmt *st;
        char name[MR_MAX_NAME];
        mr_place pl;
        int has_place = 0;
        mr_transition tr;
        int has_with = 0;
        int ends_block;
        /* `at` names that were not known when this statement was read. They
         * are resolved after every file is in; see resolve_ats. Several only
         * because `at a, b` is legal, not because anything uses it. */
        char pending[4][MR_MAX_NAME];
        int  npending = 0;
        /* The timed steps, which may come from `at <transform>` as well as
         * from an inline block, so they are collected here and attached once
         * the statement exists. */
        mr_atl *st_atl = NULL;
        /* `behind keiko,azami`, normalised. A longer list is truncated,
         * which costs the tail of an ordering rather than anything
         * structural. */
        char behind[MR_MAX_BEHIND];

        behind[0] = '\0';
        memset(&pl, 0, sizeof(pl));

        /* `call screen ...` is not a show; `show screen ...` we ignore. */
        q = mr_read_imagename(q, name, sizeof(name));

        /* `show credits:` opens a transform block, and the colon must not end
         * up as part of the image name. */
        strip_colon(name);

        /* Ren'Py's image specifier is a *loop* over clauses in any order --
         * onlayer, at, as, zorder, behind (parser.py:140-185). We looked for
         * `at` immediately after the image name and nowhere else, so
         *
         *     show eari uniform flip behind keiko at right:
         *         xoffset 100
         *
         * lost its `at` entirely: the sprite drew at the top-left corner with
         * only the block's xoffset, because a placement that was *given* gets
         * no default on the axes it does not name. 133 of Azami's show
         * statements put a clause before the `at`. */
        for (;;) {
            const char *r;

            if ((r = mr_match_word(q, "behind")) != NULL) {
                q = skip_clause_arg(r, 1, NULL, behind, sizeof(behind));
                continue;
            }
            if ((r = mr_match_word(q, "as")) != NULL) {
                q = skip_clause_arg(r, 0, "as", NULL, 0);
                continue;
            }
            if ((r = mr_match_word(q, "onlayer")) != NULL) {
                q = skip_clause_arg(r, 0, "onlayer", NULL, 0);
                continue;
            }
            if ((r = mr_match_word(q, "zorder")) != NULL) {
                q = skip_clause_arg(r, 0, "zorder", NULL, 0);
                continue;
            }

            r = mr_match_word(q, "at");
            if (!r)
                break;
            {
                /* One or more comma-separated positions/transforms. */
                for (;;) {
                    char pos[MR_MAX_NAME];
                    r = mr_next_word(r, pos, sizeof(pos));
                    {
                        char *c = strchr(pos, ',');
                        if (c) *c = '\0';
                    }
                    /* `show x at left:` puts the colon on the position. */
                    strip_colon(pos);
                    if (pos[0]) {
                        /* A transform the game defined wins over the stock
                         * position of the same name: in Ren'Py both are store
                         * variables and the game's `transform truecenter:`
                         * overwrites 00definitions.rpy's. */
                        if (!apply_transform(pos, &pl, &st_atl, 0) &&
                            !place_from_name(&pl, pos) &&
                            npending < 4) {
                            strncpy(pending[npending], pos, MR_MAX_NAME - 1);
                            pending[npending][MR_MAX_NAME - 1] = '\0';
                            npending++;
                        }
                        has_place = 1;
                    }
                    r = mr_skip_ws(r);
                    if (*r == ',') { r++; continue; }
                    break;
                }
                q = r;
            }
        }

        q = read_with(q, &tr, &has_with);

        /* A trailing colon introduces an inline transform block. It has to be
         * detected on the whole line rather than at the parse position,
         * because `at left:` leaves the colon attached to the position word. */
        ends_block = 0;
        {
            int n = (int)strlen(line);
            while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t'))
                n--;
            if (n > 0 && line[n - 1] == ':')
                ends_block = 1;
        }

        /* A `with` written on the statement itself is three nodes in Ren'Py,
         * not two: parse_with returns
         *
         *     [With(None, paired=expr), node, With(expr)]
         *
         * (parser.py:205). The leading `with None` is a commit, and it moves
         * the transition's baseline to *just before this statement* -- so a
         * `show ... with dissolve` transitions only its own change, and
         * whatever else was shown earlier in the same batch is already on the
         * old side.
         *
         * Without it,
         *
         *     show saza at moveleft
         *     show azami pout flip with dissolve
         *
         * dissolved from a baseline that predated both: saza appeared twice
         * at once, half-transparent at the right where she started and
         * half-transparent in the middle where her move had got to.
         * Reported from play as a sprite that moves and dissolves at the
         * same time.
         *
         * A `with` on a line of its own is a different node and gets no
         * pairing (parser.py:893), which is what keeps the common
         *
         *     scene bg room
         *     show a
         *     show b
         *     with fade
         *
         * fading all four changes together. */
        if (has_with)
            emit(p, MR_ST_WITH);        /* zeroed, so MR_TRANS_NONE */

        st = emit(p, is_scene ? MR_ST_SCENE : (is_hide ? MR_ST_HIDE : MR_ST_SHOW));
        strncpy(st->name, name, MR_MAX_NAME - 1);

        p->i++;
        if (ends_block) {
            /* The inline block is applied after the `at` transform in Ren'Py,
             * so where both animate the block's steps are the ones that
             * survive. */
            mr_atl *blk = NULL;

            parse_place_block(p, indent, &pl, &blk);
            if (blk) {
                free(st_atl);
                st_atl = blk;
            }
            has_place = 1;
        }

        st->place = pl;
        st->has_place = has_place;
        st->atl = st_atl;
        if (behind[0])
            st->behind = dup_text(behind);

        /* Held by index: emit() above may have grown -- and moved -- the
         * statement array, and will again before the fixups are applied. */
        while (npending > 0) {
            npending--;
            if (g_nfix >= g_capfix) {
                int nc = g_capfix ? g_capfix * 2 : 64;
                at_fixup *nv = (at_fixup *)realloc(
                    g_fix, (size_t)nc * sizeof(at_fixup));
                if (!nv)
                    mr_fatal("out of memory growing the `at` fixup list to %d",
                             nc);
                g_fix = nv;
                g_capfix = nc;
            }
            g_fix[g_nfix].stmt = (int)(st - p->s->st);
            g_fix[g_nfix].file = p->file;
            g_fix[g_nfix].line = p->l->v[myline].line;
            strcpy(g_fix[g_nfix].name, pending[npending]);
            g_nfix++;
        }

        if (has_with) {
            mr_stmt *w = emit(p, MR_ST_WITH);
            w->trans = tr;
        }
        return;
    }

    /* ---- with ---- */
    if (!strcmp(word, "with")) {
        mr_transition tr;
        int has;
        mr_stmt *st;
        read_with(line, &tr, &has);
        st = emit(p, MR_ST_WITH);
        st->trans = tr;
        p->i++;
        return;
    }

    /* ---- jump / call / return ---- */
    if (!strcmp(word, "jump") || !strcmp(word, "call")) {
        int is_call = !strcmp(word, "call");
        const char *r;

        /* `call screen rhythm_game(...)` */
        r = mr_match_word(q, "screen");
        if (is_call && r) {
            mr_stmt *st = emit(p, MR_ST_CALLSCREEN);
            mr_transition tr;
            int has;
            char name[MR_MAX_NAME];
            const char *ap;

            ap = mr_next_word(r, name, sizeof(name));
            {
                char *c = strchr(name, '(');
                if (c) *c = '\0';
            }
            strncpy(st->name, name, MR_MAX_NAME - 1);
            /* Keep the whole argument text; the screen handler parses it. */
            st->text = dup_text(r);

            read_with(ap, &tr, &has);
            if (has)
                st->trans = tr;

            p->i++;
            return;
        }

        {
            mr_stmt *st = emit(p, is_call ? MR_ST_CALL : MR_ST_JUMP);
            char name[MR_MAX_NAME];
            const char *ap = mr_next_word(q, name, sizeof(name));
            (void)ap;
            /* `call foo from _call_foo_1` - drop the from clause. */
            strncpy(st->name, name, MR_MAX_NAME - 1);
            p->i++;
            return;
        }
    }

    if (!strcmp(word, "return")) {
        emit(p, MR_ST_RETURN);
        p->i++;
        return;
    }

    /* ---- if / elif / else ---- */
    if (!strcmp(word, "if")) {
        /* Emit the chain, collecting the GOTOs that leave each branch so they
         * can be pointed at the statement after the whole chain. */
        int exits[32];
        int nexits = 0;

        for (;;) {
            const char *cond = p->l->v[p->i].text;
            char kw[MR_MAX_NAME];
            const char *cq = mr_next_word(cond, kw, sizeof(kw));
            int is_else;
            int test = -1;

            strip_colon(kw);
            is_else = !strcmp(kw, "else");

            if (!is_else) {
                char expr[MR_MAX_TEXT];
                int n = 0;
                const char *e = mr_skip_ws(cq);
                while (*e && n < MR_MAX_TEXT - 1)
                    expr[n++] = *e++;
                expr[n] = '\0';
                /* Drop the trailing colon. */
                while (n > 0 && (expr[n - 1] == ' ' || expr[n - 1] == ':'))
                    expr[--n] = '\0';

                {
                    mr_stmt *st = emit(p, MR_ST_IF);
                    st->text = dup_text(expr);
                    test = p->s->n - 1;
                }
            }

            p->i++;
            parse_block(p, indent + 1);

            if (!is_else) {
                /* Branch taken: skip the rest of the chain. */
                mr_stmt *g = emit(p, MR_ST_GOTO);
                (void)g;
                if (nexits < 32)
                    exits[nexits++] = p->s->n - 1;
                /* Condition false: continue at the next test. */
                p->s->st[test].target = p->s->n;
            }

            /* Another arm? */
            if (p->i < p->l->n && p->l->v[p->i].indent == indent) {
                char nw[MR_MAX_NAME];
                mr_next_word(p->l->v[p->i].text, nw, sizeof(nw));
                strip_colon(nw);
                if (!strcmp(nw, "elif") || !strcmp(nw, "else"))
                    continue;
            }
            break;
        }

        {
            int i;
            for (i = 0; i < nexits; i++)
                p->s->st[exits[i]].target = p->s->n;
        }
        return;
    }

    /* ---- menu ---- */
    if (!strcmp(word, "menu")) {
        /* Layout: MENU, then one CHOICE per option, then the option blocks,
         * each terminated by a GOTO past the whole menu. */
        int menu_idx;
        int choice_idx[16];
        int nchoices = 0;
        int exits[16];
        int nexits = 0;
        int body_indent = -1;
        int first_body;
        int k;

        emit(p, MR_ST_MENU);
        menu_idx = p->s->n - 1;
        p->i++;

        /* Pass one: scan the option lines at the menu's body indent so all the
         * CHOICE statements sit together, which lets the interpreter read them
         * as a contiguous run. */
        first_body = p->i;
        while (p->i < p->l->n) {
            const char *ml = p->l->v[p->i].text;
            int mi = p->l->v[p->i].indent;

            if (mi <= indent)
                break;
            if (body_indent < 0)
                body_indent = mi;
            if (mi > body_indent) {
                p->i++;       /* part of an option's block; skipped in pass one */
                continue;
            }

            {
                char        who[MR_MAX_NAME];
                char        text[MR_MAX_TEXT];
                const char *rest;
                menu_line   kind = classify_menu_line(p, ml, who, sizeof(who),
                                                      text, sizeof(text),
                                                      &rest);

                switch (kind) {
                case ML_CHOICE: {
                    mr_stmt *c = emit(p, MR_ST_CHOICE);
                    const char *cq;

                    c->text = dup_text(text);

                    /* `"caption" if <expr>:` -- the option is only offered
                     * when the expression is true. Everything between `if`
                     * and the line's trailing colon is the expression; it is
                     * kept as source and evaluated each time the menu is
                     * reached, which is what lets the same menu offer
                     * different options on a second visit. */
                    cq = mr_match_word(rest, "if");
                    if (cq) {
                        char cond[MR_MAX_TEXT];
                        int cn = 0;

                        while (*cq && cn < (int)sizeof(cond) - 1)
                            cond[cn++] = *cq++;
                        /* The block colon is punctuation, not expression. */
                        while (cn > 0 && (cond[cn - 1] == ' ' ||
                                          cond[cn - 1] == '\t' ||
                                          cond[cn - 1] == ':'))
                            cn--;
                        cond[cn] = '\0';
                        if (cn > 0)
                            c->cond = dup_text(cond);
                        else
                            mr_log("parse: menu choice with an empty `if` at "
                                   "line %d", p->l->v[p->i].line);
                    }

                    if (nchoices < 16)
                        choice_idx[nchoices++] = p->s->n - 1;
                    break;
                }

                case ML_SAY:
                case ML_CAPTION:
                    /* The caption. Ren'Py emits a say node ahead of the Menu
                     * for the `who "..."` form, with interact=False so it
                     * does not wait for a click (parser.py:259), and turns a
                     * bare string into narration said the same way
                     * (ast.py:1801). Both come out as a line in the window
                     * with the menu up over it and nothing to dismiss, which
                     * is what carrying it on the MENU statement reproduces --
                     * and it keeps the statement array's shape, where the
                     * CHOICEs must follow the MENU immediately. */
                    if (p->s->st[menu_idx].text) {
                        /* Ren'Py rejects a second one outright ("Only one say
                         * menuitem may exist per menu"), and joins multiple
                         * bare captions with a newline. Joining both ways is
                         * the lenient reading, and refusing to parse a script
                         * is not something this engine does anywhere. */
                        char *joined = (char *)malloc(
                            strlen(p->s->st[menu_idx].text) +
                            strlen(text) + 2);
                        if (joined) {
                            sprintf(joined, "%s\n%s",
                                    p->s->st[menu_idx].text, text);
                            free(p->s->st[menu_idx].text);
                            p->s->st[menu_idx].text = joined;
                        }
                    } else {
                        char base[MR_MAX_NAME + 16];
                        char id[MR_MAX_NAME + 24];

                        p->s->st[menu_idx].text = dup_text(text);
                        strncpy(p->s->st[menu_idx].name, who,
                                MR_MAX_NAME - 1);

                        /* An identifier only for the say-menuitem form.
                         *
                         * The two forms go to different catalogues in
                         * Ren'Py. `h "..."` is a real Say node, digested into
                         * an identifier -- with `nointeract` in the digest,
                         * because the caption say carries interact=False and
                         * get_code writes the word into the code it hashes
                         * (ast.py:932). The generated tl/<lang>/script.rpy
                         * says so on the line itself:
                         *
                         *     # h "So, was it a date or not?" nointeract
                         *     h "Alors, était-ce un rendez-vous ?" nointeract
                         *
                         * A *bare* caption is not a Say node. It is a Menu
                         * item, and Menu.get_translation_strings hands every
                         * item caption to the string catalogue
                         * (ast.py:1880), the same place the choice captions
                         * go. Giving it an identifier would be worse than
                         * useless: it would be a well-formed id that no
                         * catalogue can contain, so the lookup could never
                         * hit and the caption would stay in the source
                         * language. No identifier is how the interpreter
                         * knows to ask the string catalogue instead; see
                         * say_line(). */
                        if (who[0]) {
                            mr_tl_identifier(p->tl_label, who, text, NULL,
                                             1, 0, base, sizeof(base));
                            mr_tl_identifier(p->tl_label, who, text, NULL, 1,
                                             tl_dup_count(p, base),
                                             id, sizeof(id));
                            p->s->st[menu_idx].tlid = dup_text(id);
                        }
                    }
                    break;

                case ML_WITH: {
                    /* `menu: with dissolve`. Ren'Py sets it as the
                     * interaction's transition (say_menu_with), so it plays
                     * as the menu appears. */
                    mr_transition tr;
                    int has;
                    read_with(ml, &tr, &has);
                    if (has)
                        p->s->st[menu_idx].trans = tr;
                    break;
                }

                case ML_SET:
                    /* `menu set <expr>`: the options already in the set are
                     * filtered out, and the chosen one is appended to it.
                     * Recognised so it cannot be counted as an option --
                     * which is the bug this classifier exists for -- but not
                     * implemented. */
                    mr_log("parse: `menu set` is not implemented, line %d",
                           p->l->v[p->i].line);
                    break;

                case ML_UNKNOWN:
                default:
                    mr_log("parse: unrecognised line in a menu at line %d: "
                           "%.40s", p->l->v[p->i].line, ml);
                    break;
                }
            }
            p->i++;
        }

        /* Through the index, not the pointer emit returned: pass one emits a
         * CHOICE per option and emit() reallocs the statement array when it
         * grows, so that pointer can be dangling by now. It survived because
         * realloc usually extends in place and the array starts at 1024
         * entries -- a menu that happened to straddle a growth would have
         * written its choice count into freed memory. */
        p->s->st[menu_idx].aux = nchoices;

        /* Pass two: parse each option's block, in order, recording where it
         * starts so the CHOICE can point at it. */
        p->i = first_body;
        k = 0;
        while (p->i < p->l->n) {
            int mi = p->l->v[p->i].indent;

            if (mi <= indent)
                break;
            if (mi > body_indent) {
                /* Should not happen: option blocks are consumed below. */
                p->i++;
                continue;
            }

            /* Only a choice has a block, and only a choice consumes one of
             * the CHOICE statements pass one emitted. Classified by the same
             * function pass one used rather than by "it is at the body
             * indent", which is what let a caption take an option's place. */
            {
                char        who[MR_MAX_NAME];
                char        text[MR_MAX_TEXT];
                const char *rest;

                if (classify_menu_line(p, p->l->v[p->i].text, who,
                                       sizeof(who), text, sizeof(text),
                                       &rest) != ML_CHOICE) {
                    p->i++;
                    continue;
                }
            }

            /* This is an option line. Its block, if any, follows. */
            p->i++;
            if (k < nchoices)
                p->s->st[choice_idx[k]].target = p->s->n;
            parse_block(p, body_indent + 1);

            {
                mr_stmt *g = emit(p, MR_ST_GOTO);
                (void)g;
                if (nexits < 16)
                    exits[nexits++] = p->s->n - 1;
            }
            k++;
        }

        {
            int i;
            for (i = 0; i < nexits; i++)
                p->s->st[exits[i]].target = p->s->n;
        }
        p->s->st[menu_idx].target = p->s->n;
        return;
    }

    /* ---- audio ---- */
    if (!strcmp(word, "play") || !strcmp(word, "queue")) {
        int is_queue = !strcmp(word, "queue");
        char chan[MR_MAX_NAME];
        char file[MR_PATH_MAX];
        mr_stmt *st;

        q = mr_next_word(q, chan, sizeof(chan));
        if (!mr_read_string(q, file, sizeof(file))) {
            mr_log("parse: %s without a filename at line %d", word,
                   p->l->v[p->i].line);
            p->i++;
            return;
        }

        st = emit(p, is_queue ? MR_ST_QUEUE : MR_ST_PLAY);
        st->aux = !strcmp(chan, "music") ? MR_CH_MUSIC : MR_CH_SOUND;
        st->text = dup_text(file);
        read_audio_opts(st, mr_read_string(q, file, sizeof(file)));
        p->i++;
        return;
    }

    if (!strcmp(word, "stop")) {
        char chan[MR_MAX_NAME];
        mr_stmt *st;
        q = mr_next_word(q, chan, sizeof(chan));
        st = emit(p, MR_ST_STOP);
        st->aux = !strcmp(chan, "music") ? MR_CH_MUSIC : MR_CH_SOUND;
        read_audio_opts(st, q);
        p->i++;
        return;
    }

    /* ---- window ---- */
    if (!strcmp(word, "window")) {
        char mode[MR_MAX_NAME];
        mr_stmt *st;
        mr_next_word(q, mode, sizeof(mode));
        st = emit(p, MR_ST_WINDOW);
        st->aux = !strcmp(mode, "hide") ? 0 : 1;
        p->i++;
        return;
    }

    /* ---- pause ---- */
    if (!strcmp(word, "pause")) {
        mr_stmt *st = emit(p, MR_ST_PAUSE);
        double v;
        if (mr_read_number(q, &v))
            st->ms = (long)(v * 1000.0);
        else
            st->ms = -1;    /* wait for a click */
        p->i++;
        return;
    }

    /* ---- declarations ---- */
    if (!strcmp(word, "define") || !strcmp(word, "default")) {
        parse_define(p, q);
        p->i++;
        return;
    }

    if (!strcmp(word, "image")) {
        /* Advances p->i itself: the block form has to consume its block. */
        parse_image(p, q, indent);
        return;
    }

    /* ---- python one-liner ---- */
    if (word[0] == '$') {
        mr_stmt *st = emit(p, MR_ST_PYTHON);
        st->text = dup_text(mr_skip_ws(line + 1));
        p->i++;
        return;
    }

    /* ---- say ---- */
    {
        char who[MR_MAX_NAME];
        const char *textpos;

        if (looks_like_say(p, line, who, sizeof(who), &textpos)) {
            char text[MR_MAX_TEXT];
            mr_stmt *st;
            const char *after = mr_read_string(textpos, text, sizeof(text));

            /* A say can carry its own transition, as in `m "..." with
             * dissolve`. The clause's source text is part of what Ren'Py
             * digests for the translation identifier, so it is needed as
             * written and not only as a parsed transition. */
            const char *withtext = after ? mr_match_word(after, "with") : NULL;
            char base[MR_MAX_NAME + 16];
            char id[MR_MAX_NAME + 24];

            st = emit(p, MR_ST_SAY);
            strncpy(st->name, who, MR_MAX_NAME - 1);
            st->text = dup_text(text);

            mr_tl_identifier(p->tl_label, who, text, withtext, 0, 0,
                             base, sizeof(base));
            mr_tl_identifier(p->tl_label, who, text, withtext, 0,
                             tl_dup_count(p, base), id, sizeof(id));
            st->tlid = dup_text(id);

            if (after) {
                mr_transition tr;
                int has;
                read_with(after, &tr, &has);
                if (has) {
                    mr_stmt *w = emit(p, MR_ST_WITH);
                    w->trans = tr;
                }
            }
            p->i++;
            return;
        }
    }

    /* ---- anything else ---- */
    /* Screens, styles, transforms, translate blocks and python blocks all land
     * here. They are skipped along with their indented bodies. */
    if (!strcmp(word, "screen")) {
        parse_screen(p, indent);
        return;
    }

    if (!strcmp(word, "style")) {
        parse_style(p, indent);
        return;
    }

    if (!strcmp(word, "python") || !strcmp(word, "init")) {
        parse_python(p, indent);
        return;
    }

    if (!strcmp(word, "transform")) {
        parse_transform(p, indent);
        return;
    }

    if (
        !strcmp(word, "translate") ||
        !strcmp(word, "layeredimage")) {
        p->i++;
        while (p->i < p->l->n && p->l->v[p->i].indent > indent)
            p->i++;
        return;
    }

    mr_log("parse: skipping unrecognised statement at %s line %d: %.60s",
           p->s->files[p->file], p->l->v[myline].line, line);
    p->i++;
}

static void parse_block(parser *p, int indent)
{
    int block_indent = -1;

    while (p->i < p->l->n) {
        int mi = p->l->v[p->i].indent;

        /* A block ends at the first line indented less than its own body. */
        if (block_indent < 0) {
            if (mi < indent)
                return;
            block_indent = mi;
        }
        if (mi < block_indent)
            return;
        if (mi > block_indent) {
            /* Unexpected deeper indent: an unhandled construct's body. Skip. */
            p->i++;
            continue;
        }

        parse_statement(p, block_indent);
    }
}

/* --- lookups --- */

int mr_script_find_label(const mr_script *s, const char *name)
{
    int i;
    for (i = 0; i < s->nlabels; i++) {
        if (!strcmp(s->labels[i].name, name))
            return s->labels[i].stmt;
    }
    return -1;
}

const sl_screen *mr_script_find_screen(const mr_script *s, const char *name)
{
    int i;
    for (i = 0; i < s->nscreens; i++) {
        if (!strcmp(s->screens[i].name, name))
            return &s->screens[i];
    }
    return NULL;
}

const mr_character *mr_script_find_char(const mr_script *s, const char *var)
{
    int i;
    for (i = 0; i < s->nchars; i++) {
        if (!strcmp(s->chars[i].var, var))
            return &s->chars[i];
    }
    return NULL;
}

/* --- loading --- */

/* The files that carry the story, in the order Ren'Py would run their
 * declarations. script_declares.rpy has to be parsed before script.rpy so the
 * character and image definitions exist when the story references them. */
/* Ren'Py loads every .rpy in the game directory. gui.rpy must come first
 * because its `define gui.*` values are what the style expansions read, and
 * screens.rpy follows so its styles can refer to them. */
static const char *g_script_files[] = {
    "gui.rpy",
    "options.rpy",
    "screens.rpy",
    "script_declares.rpy",
    "script.rpy",
    "script_game.rpy",
    NULL
};

static int add_file(mr_script *s, const char *name)
{
    char **nv = (char **)realloc(s->files, (size_t)(s->nfiles + 1) * sizeof(char *));
    if (!nv)
        mr_fatal("out of memory growing the file table");
    s->files = nv;
    s->files[s->nfiles] = dup_text(name);
    return s->nfiles++;
}

/* Apply every `at <name>` that named a transform we had not read yet.
 *
 * Ren'Py wraps each `transform` statement in an Init (parser.py:1052-1053),
 * so all of them exist before any story line runs and a `show x at foo` above
 * -- or in a file before -- `transform foo:` is perfectly legal. We parse the
 * files in one pass because script.rpy is 2MB and lexing it twice is not
 * something to spend on a Pentium III, so the backward references are
 * collected and settled here instead.
 *
 * The transform goes *under* whatever the statement already carries, which is
 * the same order the immediate path produces: the `at` transform first, the
 * statement's own inline block on top. */
static void resolve_ats(mr_script *s)
{
    int i;

    for (i = 0; i < g_nfix; i++) {
        mr_stmt *st = &s->st[g_fix[i].stmt];

        if (!apply_transform(g_fix[i].name, &st->place, &st->atl, 1)) {
            mr_log("parse: unknown position or transform '%s' at %s line %d, "
                   "leaving the image where it was",
                   g_fix[i].name, s->files[g_fix[i].file], g_fix[i].line);
            continue;
        }
        st->has_place = 1;
    }
}

mr_script *mr_script_load(const char *game_dir)
{
    mr_script *s;
    parser p;
    int f;

    (void)game_dir;   /* names are archive-relative; the VFS resolves them */

    s = (mr_script *)calloc(1, sizeof(mr_script));
    if (!s)
        return NULL;

    mr_pyfn_reset();
    mr_list_reset();
    transforms_reset();

    for (f = 0; g_script_files[f]; f++) {
        char path[320];
        mr_lines *l;

        sprintf(path, "%s", g_script_files[f]);
        l = mr_lex_file(path);
        if (!l) {
            mr_log("script: could not read %s", path);
            continue;
        }

        memset(&p, 0, sizeof(p));
        p.s = s;
        p.l = l;
        p.i = 0;
        p.file = add_file(s, g_script_files[f]);

        /* Who lives next to the statement array. A block of data landed on
         * statement 0 during one of these files, and the cheapest
         * explanation is a neighbour overrunning: the array moves every
         * time it doubles, and whatever was allocated just below its
         * current base is the suspect. Printing the addresses makes
         * adjacency visible instead of hypothetical. */
        if (g_check_each_file)
            mr_log("scriptcheck: %.40s: lines at %p, text buffer at %p, "
                   "%d lines; statements at %p, %d of %d used",
                   g_script_files[f], (const void *)l->v, (const void *)l->buf,
                   l->n, (const void *)s->st, s->n, s->cap);

        /* Top level: every construct sits at column 0. */
        while (p.i < l->n) {
            int before = p.i;
            parse_statement(&p, p.l->v[p.i].indent);
            if (p.i == before) {
                /* Defensive: never spin on a line we failed to advance past. */
                p.i++;
            }
        }

        /* The files are parsed into one statement array, so without this the
         * last label of script.rpy would fall straight into the first
         * statement of script_game.rpy. Running off the end of a file ends
         * the game instead, as it does in Ren'Py. */
        emit(&p, MR_ST_RETURN);

        /* Identifier uniqueness is scoped to one file, and so is the table. */
        free(p.tl_seen);
        p.tl_seen = NULL;
        p.tl_nseen = p.tl_capseen = 0;

        mr_lex_free(l);

        /* Under `scriptcheck`, after every file: the statements parsed so
         * far are compared against what they were at the previous file
         * boundary. A DOS fault had the array already damaged by the time
         * parsing finished, so the question narrowed to which file's parse
         * contains the writer -- or whether it is no file in particular,
         * which would say the writer is not the parser at all. */
        if (g_check_each_file) {
            char what[80];
            sprintf(what, "parsing %.40s", g_script_files[f]);
            mr_script_verify(s, what);
            mr_script_snapshot(s);
        }
    }

    resolve_ats(s);
    if (g_check_each_file) {
        mr_script_verify(s, "resolve_ats");
        mr_script_snapshot(s);
    }

    mr_log("script: %d statements, %d labels, %d characters, "
           "%d transforms from %d files",
           s->n, s->nlabels, s->nchars, g_ntf, s->nfiles);

    /* The tables are parse-time only: every transform that was used has been
     * copied into the statement that uses it. */
    transforms_reset();
    return s;
}

/* --- the statement array, watched ---------------------------------------
 *
 * See script.h for why this exists. The copy is of the array only, not of
 * what its pointers point at: the question is whether the structs change,
 * and a deep copy would both cost far more and hide the interesting case,
 * which is a pointer field turning into something that was never a
 * pointer.
 */
void mr_script_check_enable(int on)
{
    g_check_each_file = on;
}

void mr_script_snapshot(const mr_script *s)
{
    /* Whether to do this at all is the caller's question, not this file's:
     * reading the ini here would make parse.c depend on config.c, which
     * three harnesses link without. */
    if (!s || !s->n)
        return;

    free(g_snap);
    g_snap_n = 0;
    g_snap = (mr_stmt *)malloc((size_t)s->n * sizeof(mr_stmt));
    if (!g_snap) {
        mr_log("scriptcheck: no memory for a %ld-byte snapshot; off",
               (long)s->n * (long)sizeof(mr_stmt));
        return;
    }

    memcpy(g_snap, s->st, (size_t)s->n * sizeof(mr_stmt));
    g_snap_n = s->n;
    mr_log("scriptcheck: watching %d statements, %ld bytes at %p",
           s->n, (long)s->n * (long)sizeof(mr_stmt), (const void *)s->st);
}

/* Hex and ASCII, 16 bytes a line, to the log. */
static void dump_bytes(const char *tag, const unsigned char *p, int n)
{
    char line[80];
    int i, j;

    for (i = 0; i < n; i += 16) {
        int m = n - i < 16 ? n - i : 16;
        char *o = line;

        for (j = 0; j < m; j++) {
            sprintf(o, "%02x ", p[i + j]);
            o += 3;
        }
        for (; j < 16; j++) {
            strcpy(o, "   ");
            o += 3;
        }
        *o++ = ' ';
        for (j = 0; j < m; j++)
            *o++ = (p[i + j] >= 32 && p[i + j] < 127) ? (char)p[i + j] : '.';
        *o = '\0';
        mr_log("%s +%02d  %s", tag, i, line);
    }
}

/* Is this heap address the same physical memory as the engine's own code?
 *
 * The damaged statements come back holding a byte-exact copy of mr_log's
 * machine code, and nothing in the engine copies code anywhere. Two
 * explanations remain and they are opposites: either something memcpy'd
 * out of the code segment into the heap, or the host handed out a heap
 * block that is mapped onto the image -- two linear addresses, one
 * physical page -- in which case no copy ever happened and the "damage"
 * is the code being read through the other address.
 *
 * Telling them apart takes one write. Put a sentinel in the heap and look
 * at the code: if the code changed, they are the same memory. Both are
 * restored immediately, and the window is a few instructions with no
 * interrupt handler that calls mr_log.
 *
 * `MR_ST_INIT` is logged next to it for an unrelated reason: the init pass
 * evaluated 2 statements where the script has 193, which is what it would
 * look like if two translation units disagreed about the *value* of the
 * kind enum. The stride check cannot see that -- adding an enumerator does
 * not change sizeof -- so the value is printed from here, to be read
 * against interp.c's.
 */
static void probe_aliasing(const void *heap)
{
    const unsigned char *code = (const unsigned char *)(const void *)mr_log;
    volatile unsigned long *target = (volatile unsigned long *)heap;
    unsigned long saved = *target;
    unsigned char before[8];
    int i, same = 1;

    mr_log("scriptcheck: MR_ST_INIT is %d in parse.c; mr_log is at %p",
           (int)MR_ST_INIT, (const void *)code);

    for (i = 0; i < 8; i++)
        before[i] = code[0x29 + i];

    *target = 0xFEEDFACEUL;

    for (i = 0; i < 8; i++)
        if (code[0x29 + i] != before[i])
            same = 0;

    *target = saved;

    if (!same)
        mr_log("scriptcheck: %p and the code at mr_log+29 are THE SAME "
               "MEMORY -- the host gave out a heap block mapped onto the "
               "image; nothing copied anything", heap);
    else
        mr_log("scriptcheck: %p is not aliased onto the code; the bytes "
               "there were copied, by something that is not in this "
               "source", heap);
}

int mr_script_verify(const mr_script *s, const char *when)
{
    int i, n;

    if (!g_snap || !s)
        return 1;

    /* Only the statements that existed when the snapshot was taken. The
     * array grows while the files are parsed, and comparing a prefix is
     * what lets this be called at each file boundary rather than only
     * once at the end. A *shrunk* array is not a thing the parser does,
     * so that is reported rather than compared. */
    n = s->n < g_snap_n ? s->n : g_snap_n;
    if (s->n < g_snap_n) {
        mr_log("scriptcheck: the array shrank from %d to %d by '%s'",
               g_snap_n, s->n, when);
        return 0;
    }

    if (!memcmp(g_snap, s->st, (size_t)n * sizeof(mr_stmt)))
        return 1;

    /* Something moved. Find the first statement that differs and the first
     * byte within it, and print both values: a field is recognisable from
     * its offset and sizeof(mr_stmt) is in the log above. */
    for (i = 0; i < n; i++) {
        const unsigned char *a = (const unsigned char *)&g_snap[i];
        const unsigned char *b = (const unsigned char *)&s->st[i];
        size_t k;

        for (k = 0; k < sizeof(mr_stmt); k++) {
            if (a[k] == b[k])
                continue;

            mr_log("scriptcheck: CHANGED by '%s' -- statement %d, byte %lu "
                   "of %lu: was %02x now %02x",
                   when, i, (unsigned long)k, (unsigned long)sizeof(mr_stmt),
                   a[k], b[k]);
            /* The whole word, which is what a mangled pointer looks like. */
            if (k + 4 <= sizeof(mr_stmt)) {
                /* Zeroed first: `long` is four bytes under Watcom and
                 * eight on the harness host, and copying four into an
                 * uninitialised one prints four bytes of stack above the
                 * answer. Harmless on the target, confusing in a log. */
                unsigned long wa = 0, wb = 0;
                memcpy(&wa, a + (k & ~3u), 4);
                memcpy(&wb, b + (k & ~3u), 4);
                mr_log("scriptcheck:   word at %lu: was %08lx now %08lx",
                       (unsigned long)(k & ~3u), wa, wb);
            }
            /* And what the damage *is*. Several fields of one statement
             * came back wrong at once, so this is a block of something
             * landing on the struct rather than one bad assignment, and
             * the bytes say what: script text reads as ASCII, a run of
             * pointers reads as addresses, pixels read as neither. Four
             * lines of 16 bytes, both versions, which is enough to
             * recognise any of those. */
            dump_bytes("scriptcheck:   was", a, 64);
            dump_bytes("scriptcheck:   now", b, 64);
            probe_aliasing(&s->st[i]);

            /* How far the damage runs. The report above names the first
             * differing byte and nothing else, which read as "one statement
             * was hit" when the init pass had in fact lost 191 of its 193
             * statements. Count them. */
            {
                int nbad = 0, first = i, last = i, j;
                for (j = i; j < n; j++)
                    if (memcmp(&g_snap[j], &s->st[j], sizeof(mr_stmt))) {
                        nbad++;
                        last = j;
                    }
                mr_log("scriptcheck:   %d of statements %d..%d differ "
                       "(%ld bytes of array)", nbad, first, last,
                       (long)(last - first + 1) * (long)sizeof(mr_stmt));
            }
            /* Resynchronise, so a second call reports the next change
             * rather than this one again. */
            memcpy(g_snap, s->st, (size_t)n * sizeof(mr_stmt));
            return 0;
        }
    }

    return 1;
}

void mr_script_free(mr_script *s)
{
    int i;

    if (!s)
        return;
    for (i = 0; i < s->n; i++) {
        free(s->st[i].text);
        free(s->st[i].tlid);
        free(s->st[i].atl);
        /* The registry keeps its own copy, so this is not a shared pointer. */
        free(s->st[i].anim);
        free(s->st[i].behind);
    }
    free(s->st);
    free(s->labels);
    free(s->chars);
    for (i = 0; i < s->nscreens; i++)
        sl_screen_free(&s->screens[i]);
    free(s->screens);
    for (i = 0; i < s->nfiles; i++)
        free(s->files[i]);
    free(s->files);
    free(s);
}
