/* script.h - the compiled representation of the game's .rpy files.
 *
 * Ren'Py scripts are indentation-structured, but executing a tree is awkward,
 * so the parser flattens everything into one array of statements with explicit
 * jump targets. Blocks survive only as index ranges. This is also what makes
 * saving cheap: a position in the game is just a statement index plus a call
 * stack of statement indices.
 */

#ifndef MR_SCRIPT_H
#define MR_SCRIPT_H

#include "mr.h"

/* Transitions the game actually uses. Counted from the scripts:
 * dissolve 677, fade 199, move 140, moveinleft 33, moveinright 30,
 * Fade(...) 7, moveoutright 1, None 1, plus one custom `unscrollable`. */
typedef enum {
    MR_TRANS_NONE = 0,
    MR_TRANS_DISSOLVE,
    MR_TRANS_FADE,        /* out to black, then in */
    MR_TRANS_MOVE,        /* interpolate positions of shown images */
    MR_TRANS_MOVEINLEFT,
    MR_TRANS_MOVEINRIGHT,
    MR_TRANS_MOVEOUTLEFT,
    MR_TRANS_MOVEOUTRIGHT,
    /* The vertical half of the same family, which a second game uses.
     * 00definitions.rpy:250-300 builds all eight the same way: MoveTransition
     * plus an enter_factory (the `in` ones) or a leave_factory (the `out`
     * ones) that slides the image from, or to, one screen edge. The tuple is
     * (xpos, ypos, xanchor, yanchor), so `inbottom` is (None, 1.0, None, 0.0)
     * -- the image's top edge starts at the bottom of the screen. */
    MR_TRANS_MOVEINTOP,
    MR_TRANS_MOVEINBOTTOM,
    MR_TRANS_MOVEOUTTOP,
    MR_TRANS_MOVEOUTBOTTOM
} mr_trans_kind;

typedef struct mr_transition {
    mr_trans_kind kind;
    long          ms;      /* total duration */
    mr_u32        color;   /* for fade */
} mr_transition;

typedef enum {
    MR_ST_NOP = 0,
    MR_ST_LABEL,
    MR_ST_SAY,
    MR_ST_SCENE,
    MR_ST_SHOW,
    MR_ST_HIDE,
    MR_ST_WITH,
    MR_ST_JUMP,
    MR_ST_CALL,
    MR_ST_RETURN,
    MR_ST_IF,          /* jumps to `target` when the condition is false */
    MR_ST_GOTO,        /* unconditional internal jump, ends an if branch */
    MR_ST_MENU,        /* followed by MR_ST_CHOICE entries */
    MR_ST_CHOICE,
    MR_ST_PLAY,
    MR_ST_QUEUE,
    MR_ST_STOP,
    MR_ST_WINDOW,
    MR_ST_PAUSE,
    MR_ST_PYTHON,      /* a `$` line */
    MR_ST_CALLSCREEN,  /* `call screen ...`, notably the rhythm minigame */

    /* Declarations. These sit outside any label, so execution never reaches
     * them; the interpreter runs them all once before starting the story,
     * which is what Ren'Py's init phase does. */
    MR_ST_IMAGE,       /* name = file, aux is the horizontal flip flag and
                        * blur_x/blur_y the im.Blur radii */
    MR_ST_INIT,        /* an init-time assignment from `define`/`default` */

    MR_ST_SHOWSCREEN,  /* `show screen title` */
    MR_ST_HIDESCREEN   /* `hide screen title` */
} mr_st_kind;

/* Audio channels. Ren'Py's `music` and `sound`, plus the game's use of
 * `play sound` for one-shot effects.
 *
 * The four hit channels are the rhythm game's, and mirror what the script
 * registers for it:
 *
 *     define RHYTHM_HIT_CHANNELS = ['rhythm_hit_0', ... 'rhythm_hit_3']
 *
 * A hit sound is ~0.46s long and a beatmap puts notes closer together than
 * that, so hits are dealt round-robin across the four rather than cutting
 * each other off on one. They follow the sfx volume, as `mixer='sfx'` in the
 * register_channel call asks for.
 *
 * MR_CH_MOVIE carries a cutscene's soundtrack, and Ren'Py has it too -- it is
 * a real channel there rather than part of the video player
 * (common/00mixers.rpy:56):
 *
 *     renpy.music.register_channel("movie", "music", False,
 *                                  stop_on_mute=False, movie=True)
 *
 * Three things in that line are implemented here and are worth reading off it
 * rather than guessing:
 *
 *   mixer="music"        so the *music* volume slider controls a cutscene's
 *                        sound. There is no separate movie slider in the
 *                        preferences screen and there should not be one.
 *                        chan_mix maps this channel onto MR_CH_MUSIC's volume
 *                        and mute for exactly this reason.
 *   loop=False           a cutscene's audio never loops. `loops` in
 *                        movie_cutscene repeats the whole movie, which is a
 *                        different thing and is handled by replaying it.
 *   stop_on_mute=False   muting music silences a cutscene but does not *end*
 *                        it. Free here -- nothing in this mixer stops a
 *                        channel on mute -- but it decides what "mute during
 *                        a cutscene" does, so it is written down.
 *
 * It is a separate channel from MR_CH_MUSIC and not a reuse of it, which is
 * what makes `stop_music` work: the music is stopped and the cutscene's own
 * audio plays, and the two never contend for one channel's state.
 *
 * Added at the end on purpose. The values are not stored in a save -- nothing
 * outside audio.c reads MR_CH_COUNT -- but appending costs nothing and
 * renumbering a channel that a persistent preference happened to index would
 * be a silent fault. */
typedef enum {
    MR_CH_MUSIC = 0,
    MR_CH_SOUND,
    MR_CH_HIT0,
    MR_CH_HIT1,
    MR_CH_HIT2,
    MR_CH_HIT3,
    MR_CH_MOVIE,
    MR_CH_COUNT
} mr_channel;

/* How many hit channels there are, for the round-robin. */
#define MR_CH_HIT_COUNT 4

/* Where a shown image sits. Ren'Py positions are an alignment plus an offset;
 * `at left` is xalign 0.0, `right` 1.0, `center`/`truecenter` 0.5. */
/* Each property carries a flag saying whether the statement actually set it.
 * This matters because showing an image that is already on screen *updates*
 * its transform rather than replacing it: properties the new statement does
 * not mention keep their current values. So
 *
 *     show tamae flip at right:
 *         yoffset 150
 *     show tamae flip at left:
 *         xoffset 300
 *
 * leaves the yoffset of 150 in force. Without the flags there is no way to
 * tell "xoffset 0" from "xoffset not mentioned". */
/* A placement. Ren'Py's rule, which the credits depend on: a position written
 * as a float is a fraction of the stage, one written as an integer is a pixel
 * count. `ypos 720` is the bottom edge, `ypos 0.5` is halfway down. The
 * anchor says which point of the image lands on that position. */
typedef struct mr_place {
    int   has_xalign;
    int   has_yalign;
    float xalign;
    float yalign;
    int   has_xoffset;
    int   has_yoffset;
    int   xoffset;
    int   yoffset;
    int   has_xpos;
    int   has_ypos;
    int   xpos_px;          /* the value was an integer: pixels, not fraction */
    int   ypos_px;
    float xpos;
    float ypos;
    int   has_xanchor;
    int   has_yanchor;
    int   xanchor_px;
    int   yanchor_px;
    float xanchor;
    float yanchor;

    /* Ren'Py's Transform `alpha`: a 0..1 multiplier on the image's own alpha
     * (`add_property("alpha", float, 1.0)`, transform.py:1351), applied to
     * the composited result. Azami uses it 182 times in the script alone,
     * always as `show X at <pos> with dissolve:` + `alpha 0.3` for a ghost.
     *
     * Not a placement, but it lives here because this is the struct a show
     * statement's transform block fills in, and because carrying it here is
     * what gets it interpolated by place_lerp for free -- `linear .5 alpha
     * 0.0` is a fade-out and eight of those exist across the games.
     *
     * Appended last, for the reason the note on mr_stmt gives: a field
     * inserted above this moves every offset after it, and a half-stale
     * object file then reads the wrong one. */
    int   has_alpha;
    float alpha;

    /* Ren'Py's Transform `xzoom`/`yzoom`: per-axis scale factors, default 1.0
     * (`add_property("xzoom", float, 1.0)`, transform.py:1392).
     *
     * accelerator.pyx:555-585 is the whole of the behaviour. There is no
     * clamp, so 2.0 doubles the image; the size the placement then works from
     * is the scaled one (`width *= xzoom`); and a negative value mirrors --
     * the size takes the absolute value and the sign stays in the matrix,
     * which is what the im.Flip deprecation note means by "set xzoom to a
     * negative value". The filtering is the texture's, and both GL filters are
     * GL_LINEAR (gl2texture.pyx:298-301), so it is bilinear in both
     * directions.
     *
     * Appended last, for the reason the note above gives. */
    int   has_xzoom;
    int   has_yzoom;
    float xzoom;
    float yzoom;

    /* Ren'Py's plain `zoom` (`add_property("zoom", float, 1.0)`,
     * transform.py:1382), which multiplies into both axes:
     *
     *     zoom = state.zoom
     *     xzoom = zoom * state.xzoom      (accelerator.pyx:555-557)
     *     yzoom = zoom * state.yzoom
     *
     * A third independent factor, and it has to stay a separate field rather
     * than being folded into the two above at parse time. They are separate
     * state in Ren'Py, so `zoom 2.0` followed by a later `xzoom 0.5` is 1.0
     * on x and still 2.0 on y; folding them makes the second statement
     * silently cancel the first.
     *
     * The signs multiply too, which is the other reason it cannot be folded:
     * `zoom -1.0` with `xzoom -1.0` is +1 on x and -1 on y, i.e. a vertical
     * flip and no horizontal one.
     *
     * config.zoom_zaxis (`rzdz = zoom`) has no meaning here: it only applies
     * under perspective, and there is no z in a 2D software compositor. */
    int   has_zoom;
    float zoom;
} mr_place;

/* An ATL block: a starting placement, then timed steps interpolating toward
 * further placements. The credits roll is one 90-second linear step. */
#define MR_MAX_ATL 6

typedef enum {
    MR_WARP_LINEAR = 0,
    MR_WARP_EASE            /* smoothstep; `ease`, `easein`, `easeout` */
} mr_warper;

typedef struct mr_atl_step {
    long      ms;           /* 0 means snap */
    mr_warper warp;
    mr_place  place;        /* merged onto the running placement */
} mr_atl_step;

typedef struct mr_atl {
    mr_atl_step v[MR_MAX_ATL];
    int         n;
    int         repeat;
} mr_atl;

/* An animated image: a name whose pixels change with time.
 *
 *     image fireworks animated:
 *         "images/fireworks0.png" with dissolve
 *         pause 2.2
 *         ...
 *         repeat
 *
 * In Ren'Py this is an ATL block whose statements are children rather than
 * interpolations (atl.py:1303 RawChild, :1333 Child). Two things that shape
 * everything below come from reading that:
 *
 *  - A frame's duration is the `pause` that *follows* it, not the one before.
 *    A Child swaps the displayable and returns immediately; the pause after it
 *    is what holds it on screen.
 *  - `with dissolve` belongs to the child it is written on, and Child.execute
 *    wraps the arriving child in that transition against the departing one.
 *    It is a cross-fade *inside* the displayable, between two frames -- not a
 *    scene transition, and nothing else on the stage takes part.
 *
 * This is parsed script data, like mr_atl, but unlike mr_atl it is handed to
 * the image registry rather than to the display list: an animated image is
 * part of what a *name* means, the same way im.Flip and im.Blur are. See the
 * note in image.h for what that buys and what it costs.
 */
/* The longest animation declared in any game looked at so far is Hinata's
 * five-frame `fireworks animated`, so this is a bit over three times the
 * headroom. (Written when there were two games; the figure has not been
 * re-measured since, and a game that overflows it says so in the log.) It is
 * not a cheap constant: a frame is 272 bytes and almost all of that is the
 * path buffer, so an mr_anim is about 4.3KB whatever it actually holds. That
 * is why one is malloc'd rather than built on the stack -- a 4KB local is not
 * something to put on a Win32s stack -- and why mr_stmt holds a pointer to it
 * rather than the struct.
 *
 * Raising it is cheap in practice: the allocation is per *declaration*, and a
 * game has a handful of those, so each extra slot costs 272 bytes times the
 * number of animated images. If it ever needs to be large, the thing to fix
 * first is the 256-byte path per frame holding a ~25-character name, not the
 * frame count.
 *
 * Overflowing it is reported rather than silently truncated (see parse.c),
 * for the same reason MR_MAX_ATL's overflow is: an animation that plays only
 * its first sixteen frames presents as a timing bug, with nothing pointing at
 * a limit. */
#define MR_MAX_ANIM 16

typedef struct mr_animframe {
    /* The frame's file, resolved to a game-relative path at parse time. Held
     * as a fixed buffer rather than a pointer because an mr_anim is copied by
     * value into the image registry, which outlives the script's text. */
    char file[MR_PATH_MAX];
    long ms;          /* how long this frame is held */
    long trans_ms;    /* cross-fade into this frame; 0 is a hard cut */
} mr_animframe;

typedef struct mr_anim {
    mr_animframe v[MR_MAX_ANIM];
    int  n;
    int  repeat;
    long total_ms;    /* sum of the frame durations; filled in at define time */
} mr_anim;

#define MR_MAX_TEXT 1024
#define MR_MAX_NAME 64

/* How much of a `behind a,b,c` list a show statement can carry. A tag is one
 * word -- the first word of an image name -- and the longest in any of these
 * games is nine characters, so this holds about eight of them. Sized well
 * under MR_MAX_NAME times four on purpose: the buffer sits on
 * parse_statement's frame, and that frame is already the deepest one in the
 * parser. */
#define MR_MAX_BEHIND 96

typedef struct mr_stmt {
    mr_st_kind kind;
    int        line;        /* source line, for diagnostics */
    int        file;        /* index into the file name table */

    char       name[MR_MAX_NAME];   /* label / image / character / channel */
    char      *text;               /* dialogue, expression, or filename */
    /* On MR_ST_MENU those two are the menu's caption and who says it, NULL
     * and empty when it has none: `menu:` may open with a say menuitem or a
     * bare string, which Ren'Py displays as a line of dialogue with the menu
     * up over it and nothing to dismiss. */

    /* Ren'Py's translation identifier, on say statements only; NULL elsewhere.
     * A pointer rather than an array because only one statement in three is a
     * say and the identifiers are short, so the array would cost several
     * hundred kilobytes to hold mostly nothing. See tl.h. */
    char      *tlid;

    /* A menu choice's condition -- the `if` tail of
     *
     *     "Then, so be it" if lifechoice1 == True:
     *
     * NULL when the option is unconditional, which is almost all of them. A
     * pointer for the same reason tlid is one: it would otherwise be a dead
     * array on every statement in the script. Ren'Py stores the string "True"
     * instead of a null (ast.py:1792), and evaluates it every time the menu
     * is reached; NULL here means the same thing without the eval. */
    char      *cond;

    mr_transition trans;    /* for scene/show/hide/with, and menu choices */
    mr_place      place;
    int           has_place;
    mr_atl       *atl;      /* only allocated when the block animates */
    int        target;      /* jump/call/if/goto destination statement index */
    int        aux;         /* channel, loop flag, choice count, ... */

    /* im.Blur radii on MR_ST_IMAGE, 0 elsewhere. These are two more floats on
     * every statement for the sake of the handful that are image
     * declarations, which is worth about 80KB on a script the size of Miwa's;
     * the alternative was packing them into aux, and a blur radius encoded as
     * fixed point in a bitfield is not something anyone should have to read.
     * See also tlid and cond, which went the other way because they are
     * pointers and could. */
    float      blur_x, blur_y;
    long       ms;          /* pause duration, fade in/out lengths */
    float      volume;
    int        loop;
    long       fadein;
    long       fadeout;

    /* On MR_ST_IMAGE, the block form of `image`. NULL for every other
     * statement and for the `image x = "file"` forms -- which is all but a
     * handful, so this is a pointer for the same reason tlid and cond are:
     * an mr_anim is about four kilobytes and inlining it here would cost
     * tens of megabytes on a script the size of Miwa's to hold nothing. The
     * pointer itself is another word on every statement, roughly 40KB.
     *
     * Deliberately *last*. Put between `atl` and `target`, as it first was,
     * it shifts the offset of every field after it -- `target` among them,
     * which is where an `if` keeps its destination. Any object file not
     * rebuilt against the new header then reads a statement's fields at the
     * wrong offsets, and an `if` jumps somewhere it should not: the build
     * does not fail, most of the game still works, and one conditional block
     * quietly stops executing. Appending instead leaves every pre-existing
     * offset exactly where it was, so a half-stale build degrades to not
     * seeing animations rather than to mis-branching. */
    mr_anim      *anim;

    /* `show eari uniform flip behind keiko,azami`: the tags this image is to
     * be drawn under, comma-separated and NULL on every statement that does
     * not say so. A pointer for the same reason `anim` and `cond` are -- the
     * list is at most a few dozen bytes but there are 48,000 statements, and
     * Azami carries 338 behind clauses among them.
     *
     * Appended for the reason the note above gives: a field inserted here
     * moves every offset after it, and a half-stale object file then reads
     * `target` from the wrong place. */
    char         *behind;

    /* im.FactorScale's two factors on MR_ST_IMAGE, 0 everywhere else -- and
     * zero means "not scaled", not "scale to nothing", because every
     * statement is zeroed when it is emitted and only a handful are image
     * declarations. mr_image_define_manip reads anything at or below zero as
     * 1.0 for that reason.
     *
     * Appended, for the reason the two notes above give. */
    float      scale_x, scale_y;
} mr_stmt;

/* A character declared with `define x = Character("Name", color="#rrggbb")`.
 * The game declares 16 of them and uses only the name and colour, plus a
 * one-pixel black outline that every one of them specifies identically. */
/* Screens are held as screen language trees; see slang.h. */
#include "slang.h"

typedef struct mr_character {
    char   var[MR_MAX_NAME];   /* the script variable, e.g. "m" */
    char   who[MR_MAX_NAME];   /* the displayed name, e.g. "Miwa" */
    mr_u32 color;
} mr_character;

typedef struct mr_script {
    mr_stmt *st;
    int      n;
    int      cap;

    mr_character *chars;
    int           nchars;
    int           capchars;

    sl_screen *screens;
    int        nscreens;
    int        capscreens;

    /* Label table. */
    struct mr_label {
        char name[MR_MAX_NAME];
        int  stmt;
    }       *labels;
    int      nlabels;
    int      caplabels;

    char   **files;
    int      nfiles;
} mr_script;

/* Parse every .rpy the game needs, in Ren'Py's load order (alphabetical, with
 * the 00- prefixed files first). Returns NULL and logs on failure. */
mr_script *mr_script_load(const char *game_dir);
void       mr_script_free(mr_script *s);

/* Did anything write into the statement array behind our back?
 *
 * `mrpy.ini`'s `scriptcheck=1` is what turns it on, and the *caller* reads
 * that key -- parse.c does not, so that the harnesses which link it
 * without config.c still build. With no snapshot taken, every verify
 * returns 1 and costs a compare. With one, it costs a copy of the array --
 * about 2MB for a 6466-statement game -- and a memcmp per checkpoint.
 *
 * It exists because of a DOS fault where the array parsed correctly, the
 * counts in the log were right, and the init pass then read pointer fields
 * holding values around 2.3GB -- in one case `anim`, which this game never
 * sets at all. Valgrind over the same sequence on the host reports nothing,
 * so whatever does it is in code valgrind cannot see, and the first
 * question is *when*: snapshot after parsing, verify at each later stage,
 * and the stage that reports a difference is the one that contains the
 * writer. mr_script_verify names the first statement that changed, the
 * byte offset within it, and the before and after values, which is enough
 * to recognise the field.
 *
 * It answers a second question for free: if a checkpoint immediately
 * before a read passes and the read still fetches rubbish, the memory is
 * not stable and the fault is below the engine rather than in it. */
void mr_script_snapshot(const mr_script *s);
int  mr_script_verify(const mr_script *s, const char *when);

/* Check at every file boundary inside mr_script_load as well, which is the
 * only way to see a parse that damages what an earlier parse wrote. Set it
 * before loading; the caller reads the ini key, as above. */
void mr_script_check_enable(int on);

/* -1 if the label is unknown. */
int mr_script_find_label(const mr_script *s, const char *name);

/* NULL if no character is defined with that variable name. */
const mr_character *mr_script_find_char(const mr_script *s, const char *var);

/* NULL if no screen of that name was parsed. */
const sl_screen *mr_script_find_screen(const mr_script *s, const char *name);

/* Parse a transition name as written after `with`. */
void mr_trans_parse(mr_transition *t, const char *name);

#endif /* MR_SCRIPT_H */
