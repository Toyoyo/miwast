/* vm.c - run SCRIPT.MWB.
 *
 * Commands change the pending display state; an interaction (say, pause, a
 * transition) composes it into a frame and waits. This is Ren'Py's model:
 * nothing is seen between interactions, which is why `show a`, `show b`
 * with nothing between them appear together.
 *
 * The text strip below the stage shows the line being said during a say
 * and is blank otherwise, which is what Ren'Py's window looks like then --
 * shown empty or hidden, both read as a blank strip in this layout.
 *
 * ---- Transitions ----
 *
 * A `with` goes from the baseline -- the state as last shown, or as it
 * stood at the last `with None` (core.py:1482, :2724) -- to the state now.
 * The C89 engine learned the hard way that `show x with dissolve` is three
 * statements, `with None`, the show, `with dissolve` (parser.py:205); the
 * compiler emits them that way, and here `with None` moves the baseline.
 *
 * Dissolve and fade go through the blitter's halftone. The difference of
 * the two images, from XOR to, is computed once; each step XORs in the
 * part of it whose 16x16 Bayer threshold the clock has passed since that
 * screen was last drawn. A step is one blitter pass over the stage, and a
 * slow step only means the next one jumps further. A fade is two of those
 * through black with the hold between.
 *
 * Moves recompose the stage every frame: the backdrop, then each sprite at
 * its interpolated position, all by the blitter.
 *
 * A click, Space or Return ends a transition at once (core.py:2643, its
 * SayBehavior's dismiss); skipping drops it (displayexports.py:687).
 *
 * ---- Rollback is a replay ----
 *
 * The display state at any command is a function of the position alone:
 * variables choose the way at a branch, but the compiler compiles what
 * follows once per display state it can be reached with (mwb.h,
 * "branching"). So a rollback does not keep a log of display states to
 * restore. It starts from the keyframe before the target, runs the
 * commands from there with nothing drawn, waited for or loaded, stops at
 * the target checkpoint, and puts the music right. The replay is the same
 * apply() the live game runs, so it cannot drift from it, and a step is
 * finished before the next key is read -- the C89 engine's "two rollbacks
 * in one frame" fault has no half-finished step to land in.
 *
 * What the position does not give is the variables, and the way the game
 * went: those are the rollback log (Ren'Py's RollbackLog, rollback.py), an
 * entry per checkpoint reached -- where, the story variables there, and
 * how it was answered. A rollback restores an entry's variables; the
 * entries rolled over become the future.
 *
 * Checkpoints are where Ren'Py takes hard ones: every say, every untimed
 * pause (statementexports.py:151-191, which is also why a chapter title
 * card -- `show screen title` then `renpy.pause()` -- can be rolled back
 * to), and every menu (menuexports.py:432). They are found once, at load,
 * by going through the code.
 *
 * The limit is Ren'Py's rollback_limit (rollback.py:756, :953): each hard
 * checkpoint reached gains a step, up to config.hard_rollback_limit (100);
 * a rollback pops the current line's checkpoint and the previous one's,
 * and re-running the previous one gains one back. block_rollback() zeroes
 * it (rollback.py:812).
 *
 * Rollforward is Ren'Py's (roll_forward_core, rollbackexports.py:43): the
 * line or menu being waited at is answered as it was the last time, and
 * the game runs on from there -- live, so its own `if`s and `$` lines take
 * it where they take it -- to the next checkpoint, with the transitions
 * and the timed pauses on the way dropped. The future survives only a
 * rollforward (or an untimed pause clicked through as before): a click, a
 * skip, a different answer, or the same answer clicked, ends it
 * (rollback.py:787).
 *
 * A save is the whole log, future too, and the rollback limit; a load
 * restores it and replays to its last entry, so it resumes *into* the menu
 * or line saved at. The log goes in a file of its own per slot (log_write).
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <mint/osbind.h>

#include "st.h"

#define HARD_ROLLBACK_LIMIT 100     /* config.hard_rollback_limit */

static u16 be16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static u32 be32(const u8 *p) { return ((u32)be16(p) << 16) | be16(p + 2); }

static u8        *g_mwb;
static u8        *g_text;           /* the language's .MWT */
static u32        g_nstr;
static const u16 *g_code;
static u32        g_code_words;
static const u8  *g_imgtab;
static const u8  *g_chartab;
static u16        g_nimg, g_nchar;

u32 g_script_sig;

static const u8 g_cmd_words[MWB_NCMD] = MWB_CMD_WORDS;

/* ---- variables ----
 *
 * The story's variables and the expressions on them (mwb.h). Evaluated
 * exactly as the compiler's path check does (mwc.c v_op, expr_eval), which
 * is what makes that check say anything about the ST. */

typedef struct val {
    s16 type;                       /* MWB_V_* */
    s32 v;
} val;

/* Variable i's entry in the table (mwb.h): its name hash at +0, flags +4,
 * type +5, initial value +8. */
#define VAR_ENT(i) (g_vartab + (u32)MWB_VAR_SIZE * (i))

static const u8  *g_vartab;
static u16        g_nvar;
static const u16 *g_expr;
static const u8  *g_opttab;         /* mwb_option[] */
static const u8  *g_vstrtab;        /* { u32 hash, u32 offset }[] */
static u16        g_nvstr;
static val       *g_var;

/* Value string `id`'s text. */
static const u8 *vstr(u16 id)
{
    return g_mwb + be32(g_vstrtab + 8 * (u32)id + 4);
}

static int truthy(val a)
{
    if (a.type == MWB_V_STR)
        return *vstr((u16)a.v) != 0;
    return a.type != MWB_V_NONE && a.v != 0;
}

static val v_op(u16 op, val a, val b)
{
    val r;
    int none = a.type == MWB_V_NONE || b.type == MWB_V_NONE;
    int str = a.type == MWB_V_STR || b.type == MWB_V_STR;

    r.type = MWB_V_BOOL;
    r.v = 0;
    switch (op) {
    case MWB_EX_AND:
        return truthy(a) ? b : a;
    case MWB_EX_OR:
        return truthy(a) ? a : b;
    case MWB_EX_EQ:
    case MWB_EX_NE:
        r.v = none || str ? a.type == b.type && a.v == b.v : a.v == b.v;
        if (op == MWB_EX_NE)
            r.v = !r.v;
        return r;
    }
    if (none || (str && a.type != b.type))
        return r;                   /* Python raises; the compiler warns */
    if (str) {
        /* By their bytes, as strcmp does. */
        const u8 *p = vstr((u16)a.v), *q = vstr((u16)b.v);
        while (*p && *p == *q)
            p++, q++;
        a.v = (s16)*p - (s16)*q;
        b.v = 0;
    }
    switch (op) {
    case MWB_EX_LT: r.v = a.v < b.v; break;
    case MWB_EX_LE: r.v = a.v <= b.v; break;
    case MWB_EX_GT: r.v = a.v > b.v; break;
    case MWB_EX_GE: r.v = a.v >= b.v; break;
    }
    return r;
}

/* `_return` after the rhythm game: (hits, notes, score). */
static s32 g_ret[3];

static val eval(u16 off)
{
    val st[16];
    int sp = 0;
    const u16 *w = g_expr + off;

    for (;;) {
        val r;
        r.type = MWB_V_NONE;
        r.v = 0;
        switch (*w++) {
        case MWB_EX_END:
            return sp ? st[sp - 1] : r;
        case MWB_EX_NONE:
            st[sp++] = r;
            break;
        case MWB_EX_FALSE:
        case MWB_EX_TRUE:
            r.type = MWB_V_BOOL;
            r.v = w[-1] == MWB_EX_TRUE;
            st[sp++] = r;
            break;
        case MWB_EX_INT:
            r.type = MWB_V_INT;
            r.v = (s16)*w++;
            st[sp++] = r;
            break;
        case MWB_EX_STR:
            r.type = MWB_V_STR;
            r.v = (s16)*w++;
            st[sp++] = r;
            break;
        case MWB_EX_VAR:
            st[sp++] = g_var[*w++];
            break;
        case MWB_EX_RET:
            r.type = MWB_V_INT;
            r.v = *w < 3 ? g_ret[*w] : 0;
            w++;
            st[sp++] = r;
            break;
        case MWB_EX_LONG:
            r.type = MWB_V_INT;
            r.v = (s32)(((u32)w[0] << 16) | w[1]);
            w += 2;
            st[sp++] = r;
            break;
        case MWB_EX_NOT:
            r.type = MWB_V_BOOL;
            r.v = !truthy(st[sp - 1]);
            st[sp - 1] = r;
            break;
        default:
            st[sp - 2] = v_op(w[-1], st[sp - 2], st[sp - 1]);
            sp--;
            break;
        }
    }
}

/* A new game: every variable at its `default`, except the persistent
 * ones, which outlive it. */
static void vars_reset(int all)
{
    u16 i;
    for (i = 0; i < g_nvar; i++) {
        const u8 *e = VAR_ENT(i);
        if (!all && (e[4] & MWB_VAR_PERSISTENT))
            continue;
        g_var[i].type = e[5];
        g_var[i].v = (s32)be32(e + 8);
    }
}

/* A value as Ren'Py prints it, format(value, "") (substitutions.py:100):
 * None, True, False, the number, or the string as it is. At most `room`
 * bytes (a string is cut there). Returns its length. */
static int val_fmt(val a, u8 *out, int room)
{
    const char *w = a.type == MWB_V_NONE ? "None" :
                    a.type == MWB_V_BOOL ? (a.v ? "True" : "False") :
                    a.type == MWB_V_STR ? (const char *)vstr((u16)a.v) : NULL;
    char d[12];
    u32 v = a.v < 0 ? 0 - (u32)a.v : (u32)a.v;
    int n = 0, k = 0;

    if (room < 6)
        return 0;
    if (w) {
        while (*w && n < room)
            out[n++] = (u8)*w++;
        return n;
    }
    if (room < 11)                  /* "-2147483648" */
        return 0;
    if (a.v < 0)
        out[n++] = '-';
    do {
        d[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (k)
        out[n++] = (u8)d[--k];
    return n;
}

/* A value as a file keeps it, in 6 bytes: the type, then a 32-bit number
 * -- for a string its hash, as a rebuild renumbers the strings. */
#define VAL_BYTES 6

static void put16(u8 *p, u16 v)
{
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}

static void val_put(u8 *p, val a)
{
    u32 v = a.type == MWB_V_STR ? be32(g_vstrtab + 8 * (u32)a.v)
                                : (u32)(s32)a.v;
    p[0] = (u8)((u16)a.type >> 8);
    p[1] = (u8)a.type;
    p[2] = (u8)(v >> 24);
    p[3] = (u8)(v >> 16);
    p[4] = (u8)(v >> 8);
    p[5] = (u8)v;
}

/* 0 if it is a string this build does not have, or a type it does not
 * know: what that holds then is up to the caller. */
static int val_get(const u8 *p, val *a)
{
    u16 type = be16(p);
    u32 v = be32(p + 2);
    u16 i;

    if (type == MWB_V_STR) {
        for (i = 0; i < g_nvstr; i++)
            if (be32(g_vstrtab + 8 * (u32)i) == v) {
                a->type = MWB_V_STR;
                a->v = (s16)i;
                return 1;
            }
        return 0;
    }
    if (type > MWB_V_STR)
        return 0;
    a->type = (s16)type;
    a->v = (s32)v;
    return 1;
}

/* ---- persistent ----
 *
 * Ren'Py's persistent data: apart from the saves and from the build of the
 * script. Its own file, MIWA.PER, of entries filed by the name's hash, as
 * the variable table has it:
 *
 *     u32 magic, u32 count, then count x { u32 name, value (val_put) }
 *
 * An entry no variable of this build names is kept as it is, as Ren'Py
 * keeps every field of persistent whether the script still uses it or not;
 * so is one holding a string this build does not have, its variable
 * starting at its `default`. A file from before strings ("MWPS": u32 name,
 * u8 0, u8 type, s16) is read as well, and written back in this form.
 *
 * Written when a persistent value has changed, at the next interaction.
 * Ren'Py writes it at a save and at quit (loadsave.py:172, main.py:608);
 * an ST is switched off rather than quit, which is why Ren'Py also writes
 * it when a mobile app goes to the background (core.py:1903). */
#define PERSIST_FILE  "MIWA.PER"
#define PERSIST_MAGIC 0x4D575032UL  /* "MWP2" */
#define PERSIST_OLD   0x4D575053UL  /* "MWPS" */
#define PERSIST_MAX   128
#define PERSIST_ENT   (4 + VAL_BYTES)

static int g_persist_dirty;
static u8 *g_pkeep;                 /* [var]: the file's value is kept */

/* The file's entries into `buf` (PERSIST_MAX * PERSIST_ENT bytes); returns
 * how many. */
static int persist_read(u8 *buf)
{
    u32 hdr[2];
    long fd = Fopen(PERSIST_FILE, 0), n = 0;
    int k;

    if (fd < 0)
        return 0;
    if (Fread((short)fd, 8, hdr) == 8 && hdr[1] <= PERSIST_MAX) {
        if (hdr[0] == PERSIST_MAGIC) {
            n = Fread((short)fd, (long)hdr[1] * PERSIST_ENT, buf) /
                PERSIST_ENT;
        } else if (hdr[0] == PERSIST_OLD) {
            /* Read to the top of the buffer, then spread out. */
            u8 *old = buf + PERSIST_MAX * (PERSIST_ENT - 8);
            n = Fread((short)fd, (long)hdr[1] * 8, old) / 8;
            for (k = 0; k < n; k++) {
                u8 *f = buf + PERSIST_ENT * k;
                const u8 *o = old + 8 * k;
                val v;
                v.type = o[5];
                v.v = (s16)be16(o + 6);
                memmove(f, o, 4);
                put16(f + 4, (u16)v.type);
                f[6] = f[7] = v.v < 0 ? 0xFF : 0;
                put16(f + 8, (u16)v.v);
            }
        }
    }
    Fclose((short)fd);
    return n < 0 ? 0 : (int)n;
}

static u8 *persist_find(u8 *buf, int n, const u8 *name)
{
    int k;
    for (k = 0; k < n; k++)
        if (!memcmp(buf + PERSIST_ENT * k, name, 4))
            return buf + PERSIST_ENT * k;
    return NULL;
}

/* At start: the persistent variables from the file. */
static void persist_load(void)
{
    u8 *buf = malloc(PERSIST_MAX * PERSIST_ENT);
    int n = buf ? persist_read(buf) : 0;
    u16 i;

    for (i = 0; i < g_nvar; i++) {
        const u8 *e = VAR_ENT(i), *f;
        if ((e[4] & MWB_VAR_PERSISTENT) && (f = persist_find(buf, n, e)) &&
            !val_get(f + 4, &g_var[i])) {
            /* Its `default` here, and kept in the file until the story
             * sets it, so another build can still read it. */
            g_pkeep[i] = 1;
            trace("vm: persistent %08lx holds a string this build lacks",
                  (unsigned long)be32(e));
        }
    }
    trace("vm: %d persistent values in the file", n);
    free(buf);
}

static void persist_save(void)
{
    u8 *buf = malloc(PERSIST_MAX * PERSIST_ENT);
    u32 hdr[2];
    long fd;
    int n;
    u16 i;

    g_persist_dirty = 0;
    if (!buf)
        return;
    n = persist_read(buf);
    for (i = 0; i < g_nvar; i++) {
        const u8 *e = VAR_ENT(i);
        u8 *f;
        if (!(e[4] & MWB_VAR_PERSISTENT))
            continue;
        f = persist_find(buf, n, e);
        if (f && g_pkeep[i])
            continue;
        if (!f) {
            if (n == PERSIST_MAX)
                continue;
            f = buf + PERSIST_ENT * n++;
            memcpy(f, e, 4);
        }
        val_put(f + 4, g_var[i]);
    }
    hdr[0] = PERSIST_MAGIC;
    hdr[1] = (u32)n;
    fd = Fcreate(PERSIST_FILE, 0);
    if (fd < 0 || Fwrite((short)fd, 8, hdr) != 8 ||
        Fwrite((short)fd, (long)n * PERSIST_ENT, buf) !=
            (long)n * PERSIST_ENT)
        trace("vm: cannot write %s", PERSIST_FILE);
    if (fd >= 0)
        Fclose((short)fd);
    trace("vm: %d persistent values written", n);
    free(buf);
}

/* rhythm_game_set_highscore (rhythm_game_displayable.rpy:101-112): a
 * song's high score, an int entry of the file under the key the compiler
 * made (mwb.h, "beatmaps"), written here and into the variable the
 * script reads it through, if any (rhythm_game_get_highscore). 1 if
 * `score` beat it. */
int persist_record(u32 key, s32 score)
{
    u8 *buf = malloc(PERSIST_MAX * PERSIST_ENT), *f, name[4];
    u32 hdr[2];
    long fd;
    s32 old = 0;
    int n;
    u16 i;

    if (!buf)
        return 0;
    n = persist_read(buf);
    name[0] = (u8)(key >> 24);
    name[1] = (u8)(key >> 16);
    name[2] = (u8)(key >> 8);
    name[3] = (u8)key;
    f = persist_find(buf, n, name);
    if (f && be16(f + 4) == MWB_V_INT)
        old = (s32)be32(f + 6);
    if (score <= old || (!f && n == PERSIST_MAX)) {
        free(buf);
        return 0;
    }
    for (i = 0; i < g_nvar; i++)
        if ((VAR_ENT(i)[4] & MWB_VAR_PERSISTENT) && be32(VAR_ENT(i)) == key) {
            g_var[i].type = MWB_V_INT;
            g_var[i].v = score;
            g_pkeep[i] = 0;
        }
    if (!f) {
        f = buf + PERSIST_ENT * n++;
        memcpy(f, name, 4);
    }
    put16(f + 4, MWB_V_INT);
    put16(f + 6, (u16)((u32)score >> 16));
    put16(f + 8, (u16)score);
    hdr[0] = PERSIST_MAGIC;
    hdr[1] = (u32)n;
    fd = Fcreate(PERSIST_FILE, 0);
    if (fd < 0 || Fwrite((short)fd, 8, hdr) != 8 ||
        Fwrite((short)fd, (long)n * PERSIST_ENT, buf) !=
            (long)n * PERSIST_ENT)
        trace("vm: cannot write %s", PERSIST_FILE);
    if (fd >= 0)
        Fclose((short)fd);
    trace("vm: high score %08lx %ld, was %ld", (unsigned long)key,
          (long)score, (long)old);
    free(buf);
    return 1;
}

/* `$ var = expr`. Live only: a replay does not redo it. */
static void set_var(u16 i, val v)
{
    if ((VAR_ENT(i)[4] & MWB_VAR_PERSISTENT) &&
        (g_pkeep[i] || v.type != g_var[i].type || v.v != g_var[i].v)) {
        g_persist_dirty = 1;
        g_pkeep[i] = 0;
    }
    g_var[i] = v;
}

/* ---- the rollback log ----
 *
 * An entry per checkpoint reached, oldest first, in a ring: g_nback of
 * them behind (the last is the checkpoint the game is at, or last passed),
 * then g_nfwd rolled back over -- the future. Each holds the checkpoint's
 * word offset, how it was answered (ANS_NONE while it is being waited at
 * the first time; 0 for a line or a pause; a menu's option, counted from
 * its first in the table), and the story variables as they were there.
 * The persistent ones are not in it: Ren'Py's persistent is not part of
 * the store the log restores, and an unlock is not taken back by a
 * rollback. LOG_MAX is config.rollback_length (config.py), more than the
 * hard_rollback_limit of steps there can be to go back. */
#define LOG_MAX  128
#define ANS_NONE 0xFFFF

typedef struct logent {
    u16 pc;
    u16 answer;
} logent;

static logent *g_log;
static val    *g_logvar;            /* LOG_MAX x g_nrb */
static u16    *g_rbvar;             /* the story variables: not persistent */
static u16     g_nrb;
static int     g_lfirst, g_nback, g_nfwd;

/* The next checkpoint is the log's last entry, re-run: after a rollback, a
 * load, or a change of language. Not a new one. */
static int g_resume;

/* Rolling forward: running to the next checkpoint. */
static int g_rolling;

static logent *log_at(int k)
{
    return &g_log[(g_lfirst + k) % LOG_MAX];
}

static val *log_vars(int k)
{
    return g_logvar + (u32)((g_lfirst + k) % LOG_MAX) * g_nrb;
}

static void log_vars_put(int k)
{
    val *v = log_vars(k);
    u16 i;
    for (i = 0; i < g_nrb; i++)
        v[i] = g_var[g_rbvar[i]];
}

static void log_vars_get(int k)
{
    const val *v = log_vars(k);
    u16 i;
    for (i = 0; i < g_nrb; i++)
        g_var[g_rbvar[i]] = v[i];
}

/* The checkpoint the game is at or last passed, or NULL at the start. */
static logent *log_cur(void)
{
    return g_nback ? log_at(g_nback - 1) : NULL;
}

/* The checkpoint being waited at is done with: answered `answer`, by a
 * rollforward or not. Ren'Py keeps the future only if this went the way it
 * went before and the player rolled forward (or it is a pause, which keeps
 * it on its own, keep_rollback; rollback.py:787, statementexports.py:196). */
static void log_done(u16 answer, int keep)
{
    logent *e = log_cur();

    if (!keep)
        g_nfwd = 0;
    if (e)
        e->answer = answer;
}

static void log_reset(void)
{
    g_lfirst = g_nback = g_nfwd = 0;
    g_resume = g_rolling = 0;
}

/* ---- the state ---- */

typedef struct slot {
    u16 tag, img;
    s16 x, y;
} slot;

#define MAX_SLOTS 16
#define NO_STR    0xFFFF

typedef struct vstate {
    s16        bg;                  /* image id, or -1 for black */
    s16        nslot;
    slot       slot[MAX_SLOTS];
    u16        title;               /* the title screen's string, or NO_STR */
    u16        chapter;             /* the last title shown, for saves */
    s16        window;
    s16        music;               /* the track that should be playing */
    s16        skip_ok;             /* _skipping */
    s16        rollback_ok;         /* config.rollback_enabled */
} vstate;

static vstate g_st;
static u32    g_pc;                 /* word index into g_code */
static int    g_stage_dirty = 1;

/* The transition baseline, and the stage generation it was composed as
 * when it was last shown (0: never composed, or not since it changed). */
static vstate g_prev;
static u32    g_prev_gen;

/* Checkpoints, as word offsets of the commands, in script order: what a
 * save names them by (staging edits move the offsets, not these). */
static u16 *g_cp;
static int  g_ncp;
static int  g_rb_limit;             /* Ren'Py's rollback_limit */
static int  g_branches;             /* GOTOs, IFs and MENUs: 0 = linear */

static u8 *g_stage;                 /* composed stage, STAGE_BYTES */
static u8 *g_bgbuf;                 /* the current backdrop's pixels */
static int g_bgbuf_img = -1;
static u8 *g_diff;                  /* a transition's from XOR to */

static int g_menu_pending;          /* Esc during a transition */

/* The credits roll: a SCROLL's image moving from y0 to y1, from the
 * moment it is first shown. The state has it at y1, where it ends. */
static struct {
    int on;
    u16 tag;
    s16 y0, y1, y;
    u32 ms, start;
} g_scroll;

/* ---- the sprite cache ----
 *
 * Loaded sprites stay until memory is wanted; then the least recently used
 * one not on the stage -- or on the baseline, which a transition draws --
 * goes.
 *
 * They live in one block, the arena, taken whole at load and managed here.
 * Through malloc they did not: libcmini carves small requests out of shared
 * 32KB hunks and gives a hunk back to TOS only once all of it is free, so a
 * long run left TOS's memory in pieces, and the credits roll's 115KB sprite
 * found no piece big enough with plenty free in all -- seen after a whole
 * game played through, never after a load near the end. In the arena a gap
 * too small is closed up by sliding the sprites down. */
static u8  **g_spr;
static u32  *g_spr_used;
static u32   g_spr_bytes, g_spr_budget, g_clock;
static u8   *g_arena;                   /* g_spr_budget bytes */
static u32   g_block;                   /* the arena's most: no tune loaded */

/* ---- text ----
 *
 * Strings are numbered; the current language's .MWT holds them all. Only
 * the number is ever kept -- in the state, in a keyframe -- and the text
 * looked up when it is drawn, so a change of language reaches everything
 * without a pointer to go stale. */
static const char *const g_text_file[MWB_NLANG] = {
    "TEXT_FR.MWT", "TEXT_EN.MWT"
};

static const u8 *str(u16 id)
{
    if (id >= g_nstr)
        return (const u8 *)"";
    return g_text + be32(g_text + 8 + (u32)id * 4);
}

static const u8 *char_name(int who)
{
    return str(be16(g_chartab + (u32)who * 2));
}

/* A string with a `[var]` in it comes unwrapped from the compiler (mwb.h):
 * the values are printed into it here and a say line is wrapped as the
 * compiler wraps the others (mwc.c wrap), greedily at MWB_TEXT_COLS. The
 * compiler made sure it fits the box with every value at its widest, and
 * a narrower value never takes a greedy wrap to more lines. */
#define VTEXT_MAX 400
static u8 g_vtext[VTEXT_MAX];       /* the say or the menu's caption */

/* An inline code: a style or {fast}, shown as nothing. */
static int is_style(u8 c)
{
    return c >= MWB_TXT_ITALIC_ON && c <= MWB_TXT_CODE_LAST;
}

static void wrap(u8 *s, int cols)
{
    int col = 0, lastsp = -1, i;

    for (i = 0; s[i]; i++) {
        if (is_style(s[i]))
            continue;
        if (s[i] == '\n') {
            col = 0;
            lastsp = -1;
            continue;
        }
        if (s[i] == ' ')
            lastsp = i;
        if (++col > cols && lastsp >= 0) {
            s[lastsp] = '\n';
            col = 0;
            for (lastsp++; lastsp <= i; lastsp++)
                if (!is_style(s[lastsp]))
                    col++;
            lastsp = -1;
        }
    }
}

/* String `id`, or -- if it shows variables -- it with their values printed
 * into `out` (`size` bytes) and wrapped at `cols` if that is not 0. */
static const u8 *text_vars(u16 id, u8 *out, int size, int cols)
{
    const u8 *s = str(id);
    int n = 0;

    if (!strchr((const char *)s, MWB_TXT_VAR))
        return s;
    for (; *s && n < size - 8; s++) {
        if (*s == MWB_TXT_VAR && s[1]) {
            u16 v = (u16)(*++s - MWB_TXT_VAR_BASE);
            if (v < g_nvar)
                n += val_fmt(g_var[v], out + n, size - 1 - n);
        } else {
            out[n++] = *s;
        }
    }
    out[n] = 0;
    if (cols)
        wrap(out, cols);
    return out;
}

/* Read language `lang`'s strings into the buffer made for the larger. */
static int text_load(int lang)
{
    const char *f = g_text_file[lang];

    if (!pak_read(f, g_text, (u32)pak_size(f)) ||
        be32(g_text) != MWB_MWT_MAGIC || be16(g_text + 4) != g_nstr) {
        trace("vm: bad %s", f);
        return 0;
    }
    trace("vm: text %s", f);
    return 1;
}

static const char *img_file(int id)
{
    return (const char *)g_imgtab + (u32)id * 20 + 6;
}

static int img_w(int id)
{
    return be16(g_imgtab + (u32)id * 20);
}

static int img_h(int id)
{
    return be16(g_imgtab + (u32)id * 20 + 2);
}

/* Where a slot is drawn: the credits roll's live position while it rolls. */
static int slot_y(const slot *e)
{
    return g_scroll.on && e->tag == g_scroll.tag ? g_scroll.y : e->y;
}

/* Whether any of a slot's image is on the stage. One wholly off it is
 * neither drawn nor kept in the cache: the credits, rolled up past the
 * top, are 115KB the end card's fade needs for the black. */
static int slot_visible(const slot *e)
{
    int y = slot_y(e);

    return e->x < MWB_STAGE_W && e->x + img_w(e->img) > 0 &&
           y < MWB_STAGE_H && y + img_h(e->img) > 0;
}

static int in_state(const vstate *s, int id)
{
    int i;
    for (i = 0; i < s->nslot; i++)
        if (s->slot[i].img == id && slot_visible(&s->slot[i]))
            return 1;
    return 0;
}

/* What a sprite takes in the arena: its file, rounded to a long. */
static u32 spr_size(int id)
{
    return ((u32)pak_size(img_file(id)) + 3) & ~3UL;
}

static int evict_one(void)
{
    int i, best = -1;

    for (i = 0; i < g_nimg; i++)
        if (g_spr[i] && !in_state(&g_st, i) && !in_state(&g_prev, i) &&
            (best < 0 || g_spr_used[i] < g_spr_used[best]))
            best = i;
    if (best < 0)
        return 0;
    g_spr_bytes -= spr_size(best);
    g_spr[best] = NULL;
    return 1;
}

/* The loaded sprites in address order; returns how many. */
static int *g_order;

static int arena_order(void)
{
    int i, j, n = 0;

    for (i = 0; i < g_nimg; i++) {
        if (!g_spr[i])
            continue;
        for (j = n; j > 0 && g_spr[g_order[j - 1]] > g_spr[i]; j--)
            g_order[j] = g_order[j - 1];
        g_order[j] = i;
        n++;
    }
    return n;
}

/* The first gap of `size` bytes between the n sprites in g_order, or NULL. */
static u8 *arena_fit(u32 size, int n)
{
    u8 *at = g_arena;
    int k;

    for (k = 0; k < n; k++) {
        if ((u32)(g_spr[g_order[k]] - at) >= size)
            return at;
        at = g_spr[g_order[k]] + spr_size(g_order[k]);
    }
    return (u32)(g_arena + g_spr_budget - at) >= size ? at : NULL;
}

/* Every sprite slid down to the bottom: the free space is one gap, at
 * g_arena + g_spr_bytes. */
static void arena_compact(int n)
{
    u32 t0 = now_ms();
    u8 *at = g_arena;
    int k;

    (void)t0;                       /* only the trace reads it */
    for (k = 0; k < n; k++) {
        int id = g_order[k];
        u32 s = spr_size(id);
        if (g_spr[id] != at) {
            memmove(at, g_spr[id], s);
            g_spr[id] = at;
        }
        at += s;
    }
    trace("vm: sprites compacted, %lu bytes, in %lums",
          (unsigned long)g_spr_bytes, (unsigned long)(now_ms() - t0));
}

/* The tune's room (music.c): the arena's block is the sprites' and the
 * tune's, the tune at the top. The arena ends where the tune starts, so a
 * shorter tune leaves the sprites more; a longer one moves the end down
 * first -- the sprites slid down, the least recently used dropped if they
 * must be. `bytes` 0 gives it all back. NULL when the stage's own sprites
 * leave no room (the tune is then not played). */
u8 *arena_cede(u32 bytes)
{
    u32 budget;
    int n;

    if (!g_arena || bytes > g_block)
        return NULL;
    budget = (g_block - bytes) & ~3UL;
    while (g_spr_bytes > budget)
        if (!evict_one()) {
            trace("vm: no room for a %lu byte tune: %lu of sprites in use",
                  (unsigned long)bytes, (unsigned long)g_spr_bytes);
            return NULL;
        }
    n = arena_order();
    if (n && g_spr[g_order[n - 1]] + spr_size(g_order[n - 1]) >
                 g_arena + budget)
        arena_compact(n);
    g_spr_budget = budget;
    return g_arena + budget;
}

/* A sprite's pixels. Only good until the next call: loading one may evict
 * or move another, so a pointer is never held across a second lookup. */
static const u8 *sprite(int id)
{
    s32 size;
    u32 need;
    u8 *p;

    g_spr_used[id] = ++g_clock;
    if (g_spr[id])
        return g_spr[id];
    size = pak_size(img_file(id));
    if (size < 0) {
        trace("vm: no %s", img_file(id));
        return NULL;
    }
    need = spr_size(id);
    for (;;) {
        int n = arena_order();
        p = arena_fit(need, n);
        if (!p && g_spr_bytes + need <= g_spr_budget) {
            arena_compact(n);
            p = g_arena + g_spr_bytes;
        }
        if (p || !evict_one())
            break;
    }
    if (!p) {
        trace("vm: no room for %s (%lu bytes): %lu in use", img_file(id),
              (unsigned long)need, (unsigned long)g_spr_bytes);
        return NULL;
    }
    if (!pak_read(img_file(id), p, (u32)size)) {
        trace("vm: cannot read %s", img_file(id));
        return NULL;
    }
    g_spr[id] = p;
    g_spr_bytes += need;
    return p;
}

static void draw_sprite(u8 *dst, int img, int x, int y)
{
    const u8 *s = sprite(img);
    if (s)
        gfx_sprite(dst, s, x, y);
}

/* ---- composing ---- */

static void draw_bg(const vstate *s, u8 *dst)
{
    if (s->bg < 0) {
        gfx_fill_stage(dst, 0xFF);
        return;
    }
    if (g_bgbuf_img != s->bg) {
        if (!pak_read(img_file(s->bg), g_bgbuf, STAGE_BYTES))
            trace("vm: cannot read %s", img_file(s->bg));
        g_bgbuf_img = s->bg;
    }
    gfx_copy_stage(dst, g_bgbuf);
}

/* A state's stage, into `dst` -- g_stage, or a screen directly. */
static void compose(const vstate *s, u8 *dst)
{
    int i;

    if (s->title != NO_STR) {
        /* A screen is drawn over every image, and this one is opaque. */
        gfx_title(dst, str(s->title));
        return;
    }
    draw_bg(s, dst);
    for (i = 0; i < s->nslot; i++) {
        const slot *e = &s->slot[i];
        if (slot_visible(e))
            draw_sprite(dst, e->img, e->x, slot_y(e));
    }
}

/* Which composition of the stage each screen holds. Most frames change
 * only the text, and the back screen then already has the stage from two
 * frames ago -- copying it again was 28.8KB per line for nothing.
 *
 * Anything that draws into a screen's stage area other than frame() -- a
 * transition, the credits, a menu -- must zero that screen's entry. */
static u32 g_stage_gen = 1;
static u32 g_screen_gen[2];

static void update_stage(void)
{
    u32 t0 = now_ms();

    (void)t0;                       /* only the trace reads it */
    if (!g_stage_dirty)
        return;
    compose(&g_st, g_stage);
    g_stage_dirty = 0;
    g_stage_gen++;
    trace("compose %lums, %d sprites", (unsigned long)(now_ms() - t0),
          g_st.nslot);
}

static void forget_screens(void)
{
    g_screen_gen[0] = g_screen_gen[1] = 0;
}

/* Put a frame up: the stage, and the say text if there is one. What it
 * shows becomes the transition baseline (core.py:2724). */
/* A say line's characters, as the typewriter counts them: everything but
 * the style codes. A wrap's '\n' counts, being the space it replaced --
 * Ren'Py counts the text it shows, and minirenpy learned that counting
 * anything else leaves a line "typing" after its last letter is up. */
static int say_len(const u8 *text)
{
    int n = 0;

    for (; *text; text++)
        if (!is_style(*text))
            n++;
    return n;
}

/* ---- `centered` (MWB_WHO_CENTERED) ----
 *
 * Ren'Py's centered character puts its lines in the middle of the screen,
 * each centered, with no window (00style.rpy:317-328). Here they are in a
 * white box with a black border, the menus' look: bare text cannot be read
 * on a dithered backdrop. The box is drawn over the stage of the screen,
 * so that screen's stage is then not the composition (g_screen_gen 0). */
#define WHO_CENTERED (-2)
#define CEN_STEP     10             /* pixels from one row to the next */

/* A command's `who`: a character, -1 for none, or WHO_CENTERED. */
static int who_word(u16 w)
{
    return w == 0xFFFF ? -1 : w == MWB_WHO_CENTERED ? WHO_CENTERED : (int)w;
}

/* The characters of the line starting at t. */
static int line_len(const u8 *t)
{
    int n = 0;

    for (; *t && *t != '\n'; t++)
        if (!is_style(*t))
            n++;
    return n;
}

/* The box's rows, widest row in characters, and top-left character cell
 * (the text starts one cell in) and pixel row. */
static void cen_box(const u8 *text, int *rows, int *cols, int *c0, int *y0)
{
    const u8 *t = text;
    int r = 0, w = 0;

    for (;;) {
        int n = line_len(t);
        if (n > w)
            w = n;
        r++;
        while (*t && *t != '\n')
            t++;
        if (!*t || r == MWB_CENTER_ROWS)
            break;
        t++;
    }
    *rows = r;
    *cols = w;
    *c0 = (MWB_STAGE_W / 8 - w - 2) / 2;
    *y0 = (MWB_STAGE_H - (r * CEN_STEP + 10)) / 2;
}

/* Draw characters [from, to) of a say line into the strip -- or, centered,
 * its box -- each where it falls in the whole line, in the style in force
 * there. Drawing is ORed, so a line can be drawn in pieces. */
static void say_chars(u8 *scr, int who, const u8 *text, int from, int to)
{
    int i = 0, row = 0, col = 1, style = 0, rows = MWB_TEXT_ROWS;
    int cols = 0, c0 = 0, y0 = MWB_STAGE_H + 8, step = 8;
    int center = who == WHO_CENTERED;

    if (center) {
        cen_box(text, &rows, &cols, &c0, &y0);
        c0++;                       /* the box's margin */
        y0 += 6;
        step = CEN_STEP;
        col = c0 + (cols - line_len(text)) / 2;
    }
    for (; *text && i < to && row < rows; text++) {
        u8 c = *text;
        if (c == MWB_TXT_ITALIC_ON)       { style |= GFX_ITALIC;  continue; }
        if (c == MWB_TXT_ITALIC_OFF)      { style &= ~GFX_ITALIC; continue; }
        if (c == MWB_TXT_BOLD_ON)         { style |= GFX_BOLD;    continue; }
        if (c == MWB_TXT_BOLD_OFF)        { style &= ~GFX_BOLD;   continue; }
        if (c == MWB_TXT_FAST)
            continue;
        if (c == '\n') {
            row++;
            col = center ? c0 + (cols - line_len(text + 1)) / 2 : 1;
        } else {
            if (i >= from)
                gfx_text(scr, col, y0 + row * step, text, 1, style);
            col++;
        }
        i++;
    }
}

/* The strip for a say: cleared, the name, and the first `n` characters --
 * centered, the box over the stage instead, the strip left empty. */
static void say_strip(u8 *scr, int who, const u8 *text, int n)
{
    gfx_text_clear(scr);
    if (!text)
        return;
    if (who == WHO_CENTERED) {
        int rows, cols, c0, y0;
        cen_box(text, &rows, &cols, &c0, &y0);
        gfx_box(scr, c0 * 8, y0, (c0 + cols + 2) * 8,
                y0 + rows * CEN_STEP + 10);
        g_screen_gen[scr == gfx_back() ? gfx_back_index()
                                        : gfx_back_index() ^ 1] = 0;
    } else if (who >= 0) {
        const u8 *name = char_name(who);
        gfx_text(scr, 1, MWB_STAGE_H, name, (int)strlen((const char *)name),
                 GFX_BOLD);
    }
    say_chars(scr, who, text, 0, n);
}

/* The back screen's stage brought up to date. */
static void stage_to_back(void)
{
    int b = gfx_back_index();

    update_stage();
    if (g_screen_gen[b] != g_stage_gen) {
        gfx_copy_stage(gfx_back(), g_stage);
        g_screen_gen[b] = g_stage_gen;
    }
}

/* Put a frame up: the stage, and the first `n` characters of the say text
 * if there is one (all of them if n < 0). What it shows becomes the
 * transition baseline (core.py:2724). */
static void frame_n(int who, const u8 *text, int n)
{
    stage_to_back();
    say_strip(gfx_back(), who, text, n < 0 ? 0x7FFF : n);
    gfx_flip();
    g_prev = g_st;
    g_prev_gen = g_stage_gen;
}

static void frame(int who, const u8 *text)
{
    frame_n(who, text, -1);
}

/* ---- the typewriter ----
 *
 * preferences.text_cps (options.rpy:128 sets 50; the menu changes it, 0 is
 * instant). Each screen remembers how much of the line it has, -1 for none
 * of it yet -- its strip still holds the line before -- and is brought up
 * to the clock when it is next the back screen, so a step draws only the
 * glyphs that are new to it. The line is complete when the screen being
 * shown has all of it: the rule minirenpy arrived at after losing a line's
 * last glyph twice. */
static struct {
    int        on;
    int        who;
    const u8  *text;
    int        len;
    int        fast;                /* up at once: before the last {fast} */
    int        shown[2];
    int        all;                 /* a click: the rest at once */
    u32        start;
} g_type;

static int g_after_roll;            /* renpy.game.after_rollback */

static int type_due(void)
{
    u32 t = now_ms() - g_type.start;
    s32 n;

    if (!g_text_cps || g_type.all)
        return g_type.len;
    n = g_type.fast + (s32)(t * (u32)g_text_cps / 1000);
    return n > g_type.len ? g_type.len : (int)n;
}

static int type_done(void)
{
    return !g_type.on || g_type.shown[gfx_back_index() ^ 1] >= g_type.len;
}

/* Bring the back screen up to the clock and show it. Returns 0 if there
 * was nothing new to draw. */
static int type_step(void)
{
    int b = gfx_back_index(), n = type_due();

    if (n <= g_type.shown[b] || type_done())
        return 0;
    stage_to_back();
    /* A centered box went with the stage stage_to_back put back: whole. */
    if (g_type.shown[b] < 0 || g_type.who == WHO_CENTERED)
        say_strip(gfx_back(), g_type.who, g_type.text, n);
    else
        say_chars(gfx_back(), g_type.who, g_type.text, g_type.shown[b], n);
    g_type.shown[b] = n;
    gfx_flip();
    if (n == g_type.len)
        trace("type: %d chars up at %lums", n,
              (unsigned long)(now_ms() - g_type.start));
    return 1;
}

/* The first frame of a say, typing or whole. */
/* Where typing starts: at the last {fast} (character.py:163-172). */
static int fast_start(const u8 *text)
{
    int i = 0, fast = 0;

    for (; *text; text++)
        if (*text == MWB_TXT_FAST)
            fast = i;
        else if (!is_style(*text))
            i++;
    return fast;
}

static void type_begin(int who, const u8 *text)
{
    int instant = !text || !g_text_cps || g_autoplay || g_after_roll;

    if (!instant) {
        g_type.len = say_len(text);
        g_type.fast = fast_start(text);
        instant = g_type.fast >= g_type.len;
    }
    g_type.on = !instant;
    g_type.all = 0;
    g_after_roll = 0;
    if (instant) {
        frame(who, text);
        return;
    }
    g_type.who = who;
    g_type.text = text;
    trace("type: %d chars", g_type.len);
    frame_n(who, text, g_type.fast);
    g_type.shown[gfx_back_index() ^ 1] = g_type.fast;
    g_type.shown[gfx_back_index()] = -1;
    g_type.start = now_ms();
}

/* `with None`: the state now is the baseline, shown or not. */
static void with_none(void)
{
    g_prev = g_st;
    g_prev_gen = g_stage_dirty ? 0 : g_stage_gen;
}

/* ---- the display list ---- */

static void slot_remove(int tag)
{
    int i;
    for (i = 0; i < g_st.nslot; i++) {
        if (g_st.slot[i].tag == tag) {
            memmove(&g_st.slot[i], &g_st.slot[i + 1],
                    (g_st.nslot - i - 1) * sizeof(slot));
            g_st.nslot--;
            return;
        }
    }
}

static void slot_show(const u16 *a)
{
    u16 at = a[4];

    slot_remove(a[0]);
    if (g_st.nslot == MAX_SLOTS || at > g_st.nslot) {
        trace("vm: bad show of tag %u at %u", a[0], at);
        return;
    }
    memmove(&g_st.slot[at + 1], &g_st.slot[at],
            (g_st.nslot - at) * sizeof(slot));
    g_st.slot[at].tag = a[0];
    g_st.slot[at].img = a[1];
    g_st.slot[at].x = (s16)a[2];
    g_st.slot[at].y = (s16)a[3];
    g_st.nslot++;
}

static int slot_find(const vstate *s, int tag)
{
    int i;
    for (i = 0; i < s->nslot; i++)
        if (s->slot[i].tag == tag)
            return i;
    return -1;
}

/* ---- the state machine ---- */

static void state_reset(void)
{
    memset(&g_st, 0, sizeof(g_st));
    g_st.bg = -1;
    g_st.window = 1;
    g_st.music = -1;
    g_st.title = g_st.chapter = NO_STR;
    g_st.skip_ok = 1;
    g_st.rollback_ok = 1;           /* config.rollback_enabled's default */
}

/* The command after the one at `pc`. A language branch not for the language
 * in force is stepped over whole. */
static u32 next_pc(u32 pc)
{
    const u16 *c = g_code + pc;

    pc += g_cmd_words[c[0]];
    if (c[0] == MWB_IFLANG && c[1] != (u16)g_lang)
        pc += c[2];
    return pc;
}

/* What a command does to the state -- all of it, for the commands that
 * have a state effect, and nothing else. Shared by the live game and the
 * replay, which is what makes a replay arrive at the state the game was
 * in. */
static void apply(u16 op, const u16 *a)
{
    switch (op) {
    case MWB_SCENE:
        g_st.bg = a[0] == 0xFFFF ? -1 : (s16)a[0];
        g_st.nslot = 0;
        break;
    case MWB_SHOW:
        slot_show(a);
        break;
    case MWB_SCROLL: {
        /* Where it ends: the roll itself is vm_run's, and only live. */
        int i;
        slot_show(a);
        i = slot_find(&g_st, a[0]);
        if (i >= 0)
            g_st.slot[i].y = (s16)a[5];
        break;
    }
    case MWB_HIDE:
        slot_remove(a[0]);
        break;
    case MWB_TITLE:
        g_st.title = g_st.chapter = a[0];
        break;
    case MWB_UNTITLE:
        g_st.title = NO_STR;
        break;
    case MWB_MUSIC:
        g_st.music = (s16)a[0];
        break;
    case MWB_STOPMUSIC:
        g_st.music = -1;
        break;
    case MWB_WINDOW:
        g_st.window = (s16)a[0];
        break;
    case MWB_SKIPPING:
        g_st.skip_ok = (s16)a[0];
        break;
    case MWB_ROLLBACK:
        g_st.rollback_ok = (s16)a[0];
        break;
    default:
        return;
    }
    switch (op) {
    case MWB_SCENE: case MWB_SHOW: case MWB_SCROLL: case MWB_HIDE:
    case MWB_TITLE: case MWB_UNTITLE:
        g_stage_dirty = 1;
        break;
    }
}

/* As the compiler counts them for the save signature (mwc.c save_sig). */
static int is_checkpoint(const u16 *c)
{
    return c[0] == MWB_SAY || c[0] == MWB_MENU ||
           (c[0] == MWB_PAUSE && c[1] == 0 && c[2] == 0);
}

/* The index of the last checkpoint at or before word offset `pc`, or -1. */
static int cp_at_or_before(u32 pc)
{
    int lo = 0, hi = g_ncp - 1, r = -1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_cp[mid] <= pc) {
            r = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return r;
}

/* Keyframes: the display state at chosen commands, made at load, so that a
 * replay starts close to its target. Replaying from the start was 635ms at
 * the end of the game on an 8MHz STE.
 *
 * They are made by following the code the way it can run -- every branch
 * of an IF, every option of a MENU, every GOTO -- not by sweeping it in
 * file order, which would carry one path's state into the next path's
 * code. There is one at every place a jump lands, and one at every
 * KEY_EVERY-th checkpoint. So the code between a keyframe and the next is
 * straight: a replay from the last keyframe before its target runs into no
 * GOTO or MENU on the way, and goes on through an IF -- whatever comes
 * after it that a jump does not reach is reached by not jumping. Made by
 * the same apply() as everything else, so starting from one arrives at the
 * same state as starting from the top. */
#define KEY_EVERY 64

typedef struct keyframe {
    u32    pc;
    vstate st;
} keyframe;

static keyframe *g_key;             /* sorted by pc */
static int       g_nkey;
static int       g_capkey;          /* the most there can be: vm_load */

/* While they are made: which commands have one, which are every
 * KEY_EVERY-th checkpoint, and which keyframes have had the code after
 * them followed. */
static u8 *g_kmark;
static u8 *g_kperiod;
static u8 *g_kwalked;

static int key_add(u32 pc)
{
    if (g_kmark[pc >> 3] & (1 << (pc & 7)))
        return -1;
    if (g_nkey == g_capkey) {
        trace("vm: more keyframes than the %d counted", g_capkey);
        return -1;
    }
    g_kmark[pc >> 3] |= (u8)(1 << (pc & 7));
    g_key[g_nkey].pc = pc;
    g_key[g_nkey].st = g_st;
    g_kwalked[g_nkey] = 0;
    return g_nkey++;
}

/* Made again when the language changes, since a language branch can change
 * the state. The live state is left as it was. */
static void make_keyframes(void)
{
    u32 t0 = now_ms();
    vstate live = g_st;
    int i, dirty = g_stage_dirty;

    (void)t0;                       /* only the trace reads it */

    if (!g_key) {
        g_key = malloc((u32)g_capkey * sizeof(keyframe) + 1);
        if (!g_key) {
            g_capkey = 0;
            return;
        }
    }
    g_kmark = calloc(g_code_words / 8 + 1, 1);
    g_kperiod = calloc(g_code_words / 8 + 1, 1);
    g_kwalked = malloc((u32)g_capkey + 1);
    g_nkey = 0;
    if (!g_kmark || !g_kperiod || !g_kwalked) {
        free(g_kmark);
        free(g_kperiod);
        free(g_kwalked);
        return;
    }
    for (i = 0; i < g_ncp; i += KEY_EVERY)
        g_kperiod[g_cp[i] >> 3] |= (u8)(1 << (g_cp[i] & 7));

    state_reset();
    key_add(0);
    for (i = 0; i < g_nkey; i++) {
        u32 pc = g_key[i].pc;

        if (g_kwalked[i])
            continue;
        g_kwalked[i] = 1;
        g_st = g_key[i].st;
        for (;;) {
            const u16 *c = g_code + pc;
            int k;

            /* Most bytes of both maps are 0: the bit is looked for only
             * when they are not (the shifts cost the 68000 more than the
             * rest of the walk). */
            if (pc != g_key[i].pc &&
                (g_kmark[pc >> 3] | g_kperiod[pc >> 3])) {
                u8 bit = (u8)(1 << (pc & 7));
                /* A keyframe's code is its own walk's. */
                if (g_kmark[pc >> 3] & bit)
                    break;
                if ((g_kperiod[pc >> 3] & bit) && (k = key_add(pc)) >= 0)
                    g_kwalked[k] = 1;
            }
            if (c[0] == MWB_END)
                break;
            if (c[0] == MWB_GOTO) {
                key_add(c[1]);
                break;
            }
            if (c[0] == MWB_IF)
                key_add(c[2]);
            if (c[0] == MWB_MENU) {
                u16 o;
                for (o = 0; o < c[4]; o++)
                    key_add(be16(g_opttab + 6 * (u32)(c[3] + o) + 4));
                key_add(c[5]);
                break;
            }
            apply(c[0], c + 1);
            pc = next_pc(pc);
        }
    }

    /* In code order, for replay_to's search. */
    for (i = 1; i < g_nkey; i++) {
        keyframe k = g_key[i];
        int j;
        for (j = i; j > 0 && g_key[j - 1].pc > k.pc; j--)
            g_key[j] = g_key[j - 1];
        g_key[j] = k;
    }
    free(g_kmark);
    free(g_kperiod);
    free(g_kwalked);
    g_kmark = g_kperiod = g_kwalked = NULL;

    g_st = live;
    g_stage_dirty = dirty;
    trace("vm: %d keyframes, %lu bytes, in %lums", g_nkey,
          (unsigned long)g_nkey * sizeof(keyframe),
          (unsigned long)(now_ms() - t0));
}

/* Run to word offset `target` with nothing drawn, waited for or loaded,
 * from the last keyframe at or before it, then put the music right. */
static void replay_to(u32 target)
{
    u32 t0 = now_ms();
    u32 pc = 0;
    int lo = 0, hi = g_nkey - 1, k = -1;

    (void)t0;                       /* only the trace reads it */

    state_reset();
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_key[mid].pc <= target) {
            k = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (k >= 0) {
        g_st = g_key[k].st;
        pc = g_key[k].pc;
    }
    while (pc < target) {
        u16 op = g_code[pc];
        if (op == MWB_GOTO || op == MWB_MENU || op == MWB_END) {
            /* Not with the keyframes made right: see make_keyframes. */
            trace("vm: the replay to %lu met opcode %u at %lu",
                  (unsigned long)target * 2, op, (unsigned long)pc * 2);
            break;
        }
        apply(op, g_code + pc + 1);
        pc = next_pc(pc);
    }
    g_pc = target;
    g_stage_dirty = 1;
    g_scroll.on = 0;
    g_menu_pending = 0;
    g_after_roll = 1;               /* the line comes up whole */
    with_none();
    trace("replay to %lu in %lums", (unsigned long)target * 2,
          (unsigned long)(now_ms() - t0));
    music_sync(g_st.music);
}

/* ---- the path, traced ----
 *
 * NatFeats builds print every say, untimed pause, menu and the end with
 * the whole display state and the story variables, exactly as the compiler
 * prints the compiled code's walk of the same menu answers (MWC_TRACE,
 * mwc.c tr_line): the two are diffed to show that the ST goes where the
 * path check says the code goes. What moves in the log is printed too --
 * "back", "fwd", "pick <option shown>", "save <slot>", "load <slot>" -- so
 * that check-st can take out what was rolled over and diff the rest. */
#ifdef MW_NATFEATS
#include <stdio.h>

static char g_path[2048];

static void path_line(int n)
{
    int i;

    n += sprintf(g_path + n, " | bg %d;", g_st.bg);
    for (i = 0; i < g_st.nslot; i++)
        n += sprintf(g_path + n, " %u:%u@%d,%d", g_st.slot[i].tag,
                     g_st.slot[i].img, g_st.slot[i].x, g_st.slot[i].y);
    n += sprintf(g_path + n, "; title %u ch %u mus %d win %d skip %d rb %d",
                 g_st.title, g_st.chapter, g_st.music, g_st.window,
                 g_st.skip_ok, g_st.rollback_ok);
    n += sprintf(g_path + n, "; vars");
    for (i = 0; i < (int)g_nrb && n < (int)sizeof(g_path) - 8; i++) {
        g_path[n++] = ' ';
        n += val_fmt(g_var[g_rbvar[i]], (u8 *)g_path + n,
                     (int)sizeof(g_path) - 1 - n);
    }
    g_path[n] = 0;
    trace("path: %s", g_path);
}

/* Text as the trace has it: a wrap is the space it replaced. */
static int path_text(int n, const u8 *s)
{
    for (; s && *s && n < (int)sizeof(g_path) - 300; s++)
        g_path[n++] = *s == '\n' ? ' ' : (char)*s;
    g_path[n] = 0;
    return n;
}

static void path_say(int who, const u8 *text)
{
    path_line(path_text(sprintf(g_path, "say %d ", who), text));
}

static void path_menu(int who, const u8 *cap, const u8 *const *label, int n)
{
    int k = path_text(sprintf(g_path, "menu %d ", who), cap), i;

    k += sprintf(g_path + k, " [");
    for (i = 0; i < n; i++) {
        k = path_text(k, label[i]);
        k += sprintf(g_path + k, " /");
    }
    path_line(k + sprintf(g_path + k, "]"));
}

/* "end", "pause", or "cut" where a test build gives up on a looping
 * path; and the log's moves, "back" and the rest, with a number or -1. */
static void path_end(const char *what)
{
    path_line(sprintf(g_path, "%s", what));
}

static void path_note(const char *what, int n)
{
    if (n < 0)
        sprintf(g_path, "%s", what);
    else
        sprintf(g_path, "%s %d", what, n);
    trace("path: %s", g_path);
}
#else
#define path_say(who, text)          ((void)0)
#define path_menu(who, cap, l, n)    ((void)0)
#define path_end(what)               ((void)0)
#define path_note(what, n)           ((void)0)
#endif

/* A rollback or rollforward asked for at `here`, the word offset of the
 * say, pause, menu or transition being waited at. Returns 1 if it is done:
 * back, with g_pc on the checkpoint to re-run and the variables as they
 * were there; forward, with g_rolling set and g_pc left where the game
 * goes on from -- a menu sets it itself, to the option answered. */
static int roll(int dir, u32 here)
{
    logent *cur = log_cur();
    int at_cp = cur && cur->pc == here;

    if (dir < 0) {
        /* Waiting at a checkpoint, going back pops it and the one before;
         * waiting anywhere else (a timed pause) pops only the last one. */
        int pops = at_cp ? 2 : 1, t = g_nback - pops;
        if (!g_st.rollback_ok || g_rb_limit < pops || t < 0)
            return 0;
        g_rb_limit -= pops;
        g_nfwd += g_nback - (t + 1);
        g_nback = t + 1;
        log_vars_get(t);
        replay_to(log_at(t)->pc);
        g_resume = 1;
        path_note("back", -1);
        return 1;
    }
    /* Ren'Py's roll_forward_info (rollback.py:723): only where this
     * checkpoint was answered before, i.e. was rolled back to. */
    if (!at_cp || cur->answer == ANS_NONE)
        return 0;
    g_rolling = 1;
    path_note("fwd", -1);
    return 1;
}

/* A hard checkpoint reached in play: one more step may be rolled back,
 * and an entry in the log -- unless it is the entry re-run, or the next
 * one of the future, reached by rolling forward. */
static void checkpoint(u32 here)
{
    if (g_rb_limit < HARD_ROLLBACK_LIMIT)
        g_rb_limit++;
    if (g_rolling) {
        g_rolling = 0;
        g_after_roll = 1;           /* the line comes up whole */
    }
    if (g_resume) {
        g_resume = 0;
        if (log_cur() && log_cur()->pc == here)
            return;
        trace("vm: resumed at %lu, the log is at %lu",
              (unsigned long)here * 2,
              (unsigned long)(log_cur() ? log_cur()->pc * 2UL : 0));
    }
    if (g_nfwd && log_at(g_nback)->pc == here) {
#ifdef MW_NATFEATS
        /* The way forward ran live from where the log says it was: it
         * should arrive with the variables the log has here. */
        const val *v = log_vars(g_nback);
        u16 i;
        for (i = 0; i < g_nrb; i++)
            if (memcmp(&v[i], &g_var[g_rbvar[i]], sizeof(val)))
                trace("vm: rolled forward to %lu, variable %u differs",
                      (unsigned long)here * 2, g_rbvar[i]);
#endif
        g_nback++;
        g_nfwd--;
        log_vars_put(g_nback - 1);
        return;
    }
    /* Somewhere new: the future, if any, is not this way. */
    g_nfwd = 0;
    if (g_nback == LOG_MAX) {
        g_lfirst = (g_lfirst + 1) % LOG_MAX;
        g_nback--;
    }
    g_nback++;
    log_at(g_nback - 1)->pc = (u16)here;
    log_at(g_nback - 1)->answer = ANS_NONE;
    log_vars_put(g_nback - 1);
}

/* A menu ends the skip (choice_for_skipping, menuexports.py:153, with
 * preferences.skip_after_choices False, preferences.py:134), and Ren'Py
 * starts one only on Ctrl going down (behavior.py:348): one still held
 * from before the menu does not skip again until it has come up. */
static int g_skip_cut;

/* May Ctrl skip now? Not where the script fences it, nor after a menu
 * until it is let go. */
static int skip_ok(void)
{
    if (!skip_held())
        g_skip_cut = 0;
    return g_st.skip_ok && !g_skip_cut;
}

/* Is the player skipping right now? */
static int skipping(void)
{
    return skip_ok() && skip_held();
}

/* ---- transitions ---- */

/* The 16x16 Bayer matrix: each pixel's threshold, 0..255, spread so that
 * every level is an even sprinkle. Built at load. */
static u8 g_bayer[16][16];

static void make_bayer(void)
{
    int n, r, c;

    g_bayer[0][0] = 0;
    for (n = 1; n < 16; n *= 2)
        for (r = 0; r < n; r++)
            for (c = 0; c < n; c++) {
                u8 v = (u8)(g_bayer[r][c] * 4);
                g_bayer[r][c] = v;
                g_bayer[r][c + n] = (u8)(v + 2);
                g_bayer[r + n][c] = (u8)(v + 3);
                g_bayer[r + n][c + n] = (u8)(v + 1);
            }
}

/* The halftone of the pixels whose threshold is in [from, to). */
static void halftone(int from, int to, u16 *ht)
{
    int r, c;

    for (r = 0; r < 16; r++) {
        u16 w = 0;
        for (c = 0; c < 16; c++) {
            int t = g_bayer[r][c];
            if (t >= from && t < to)
                w |= (u16)(0x8000 >> c);
        }
        ht[r] = w;
    }
}

/* What the player did during a transition. Ending it or opening the menu
 * finish it at once (the menu then opens at the next interaction); a roll
 * leaves it where it is. Returns IN_NONE to carry on, IN_ADVANCE to
 * finish, IN_ROLLED if g_pc has moved. */
#define IN_ROLLED 100
static int trans_input(u32 here)
{
    int r = poll_input(skip_ok() ? W_SKIPOK : 0);

    switch (r) {
    case IN_NONE:
        return IN_NONE;
    case IN_BACK:
    case IN_FORWARD:
        return roll(r == IN_BACK ? -1 : 1, here) ? IN_ROLLED : IN_NONE;
    case IN_MENU:
        g_menu_pending = 1;
        return IN_ADVANCE;
    case IN_QUIT:                   /* autoplay's Esc */
        g_menu_pending = 1;
        return IN_ADVANCE;
    default:
        return IN_ADVANCE;
    }
}

/* Both screens' stages hold `from`; g_diff holds from XOR to. Step the
 * halftone from 0 to 256 over `ms` from t0 -- taken before the setup, so
 * the setup's ~100ms is part of the transition's time, as a slow first
 * frame is in Ren'Py, and not added to it. Returns IN_NONE with the shown
 * screen exactly `to`. */
static int xfade(u32 t0, u32 ms, u32 here)
{
    int lev[2] = { 0, 0 };
    u16 ht[16];

    for (;;) {
        u32 t = now_ms() - t0;
        int lv = (!ms || t >= ms) ? 256 : (int)(t * 256 / ms);
        int b = gfx_back_index();
        int r = trans_input(here);

        if (r != IN_NONE)
            return r;
        if (lv > lev[b]) {
            halftone(lev[b], lv, ht);
            gfx_blit_stage(gfx_back(), g_diff, BLT_HOP_SRC_AND_HT,
                           BLT_OP_S_XOR_D, ht);
            gfx_text_clear(gfx_back());
            lev[b] = lv;
            gfx_flip();
            if (lv == 256)
                return IN_NONE;
        } else {
            Vsync();
        }
    }
}

/* Wait `ms` on a still screen, for the hold of a fade. */
static int hold(u32 ms, u32 here)
{
    u32 t0 = now_ms();

    while (now_ms() - t0 < ms) {
        int r = trans_input(here);
        if (r != IN_NONE)
            return r;
        Vsync();
    }
    return IN_NONE;
}

/* Make both screens show the baseline. If it was never shown -- changes
 * made before a `with None` and not displayed -- this is a cut to it, which
 * is what Ren'Py shows too: its transition starts from that scene. */
static void baseline_on_screens(void)
{
    int f = gfx_back_index() ^ 1;

    if (!g_prev_gen || g_screen_gen[f] != g_prev_gen) {
        trace("with: baseline not on screen, cut to it");
        compose(&g_prev, gfx_back());
        gfx_text_clear(gfx_back());
        gfx_flip();
    }
    gfx_copy_stage(gfx_back(), gfx_front());
    gfx_text_clear(gfx_back());
}

/* The move family (movetransition.py:561-716): tags in both lists glide
 * from the old position to the new; a tag only in the new list enters from
 * the side the transition names, or -- plain `move`, or an `out` one --
 * is not drawn until it is over (`entering`: "if not enter: return"); one
 * only in the old list leaves by its side, or is gone at once. The order
 * is Ren'Py's merge of the two lists. */
typedef struct mover {
    u16 img;
    s16 x0, y0, x1, y1;
} mover;

static int move_plan(int kind, mover *m)
{
    int enter = kind == MWB_T_MOVEINLEFT ? -1 :
                kind == MWB_T_MOVEINRIGHT ? 1 : 0;
    int leave = kind == MWB_T_MOVEOUTLEFT ? -1 :
                kind == MWB_T_MOVEOUTRIGHT ? 1 : 0;
    u8 done[MAX_SLOTS];             /* old entries already placed */
    int oi = 0, ni = 0, n = 0;

    memset(done, 0, sizeof(done));
    while (oi < g_prev.nslot || ni < g_st.nslot) {
        if (oi < g_prev.nslot) {
            const slot *o = &g_prev.slot[oi];
            if (done[oi]) {
                oi++;
                continue;
            }
            if (slot_find(&g_st, o->tag) < 0) {
                /* leaving */
                if (leave) {
                    m[n].img = o->img;
                    m[n].x0 = o->x;
                    m[n].y0 = m[n].y1 = o->y;
                    m[n].x1 = (s16)(leave < 0 ? -img_w(o->img)
                                              : MWB_STAGE_W);
                    n++;
                }
                done[oi++] = 1;
                continue;
            }
        }
        {
            const slot *e = &g_st.slot[ni++];
            int j = slot_find(&g_prev, e->tag);

            m[n].img = e->img;
            m[n].x1 = e->x;
            m[n].y1 = e->y;
            if (j >= 0) {
                /* moving */
                m[n].x0 = g_prev.slot[j].x;
                m[n].y0 = g_prev.slot[j].y;
                done[j] = 1;
                n++;
            } else if (enter) {
                m[n].x0 = (s16)(enter < 0 ? -img_w(e->img) : MWB_STAGE_W);
                m[n].y0 = e->y;
                n++;
            }
        }
    }
    return n;
}

static int move(int kind, u32 ms, u32 here)
{
    mover m[2 * MAX_SLOTS];
    int n = move_plan(kind, m);
    u32 t0 = now_ms();

    for (;;) {
        u32 t = now_ms() - t0;
        u8 *scr = gfx_back();
        int i, r = trans_input(here);

        if (r != IN_NONE)
            return r;
        if (t >= ms)
            return IN_NONE;
        draw_bg(&g_st, scr);
        for (i = 0; i < n; i++)
            draw_sprite(scr, m[i].img,
                        m[i].x0 + (int)((s32)(m[i].x1 - m[i].x0) *
                                        (s32)t / (s32)ms),
                        m[i].y0 + (int)((s32)(m[i].y1 - m[i].y0) *
                                        (s32)t / (s32)ms));
        gfx_text_clear(scr);
        gfx_flip();
    }
}

/* A dissolve or a fade (a: MWB_WITH's words) from the baseline to what is
 * in g_stage, its clock from t0. Returns as xfade does; *exact is whether
 * the shown screen was left holding g_stage exactly. */
static int stage_fade(const u16 *a, u32 here, u32 t0, int *exact)
{
    int r = IN_NONE;
    u32 t1 = t0;

    *exact = 0;
    if (a[0] == MWB_T_DISSOLVE) {
        baseline_on_screens();
        gfx_blit_stage(g_diff, gfx_front(), BLT_HOP_SRC, BLT_OP_S, NULL);
        gfx_blit_stage(g_diff, g_stage, BLT_HOP_SRC, BLT_OP_S_XOR_D, NULL);
        r = xfade(t1, a[1], here);
        *exact = r == IN_NONE;
    } else if (a[0] == MWB_T_FADE) {
        /* Fade(out, hold, in): to black, black, then the new scene. */
        baseline_on_screens();
        gfx_blit_stage(g_diff, gfx_front(), BLT_HOP_SRC, BLT_OP_NOTS, NULL);
        r = xfade(t1, a[1], here);
        if (r != IN_NONE)
            return r;
        gfx_fill_stage(gfx_back(), 0xFF);
        gfx_text_clear(gfx_back());
        r = hold(a[2], here);
        if (r != IN_NONE)
            return r;
        t1 = now_ms();
        gfx_blit_stage(g_diff, g_stage, BLT_HOP_SRC, BLT_OP_NOTS, NULL);
        r = xfade(t1, a[3], here);
        *exact = r == IN_NONE;
    }
    return r;
}

/* A `with` that is not None: from the baseline to the state now. Returns
 * IN_ROLLED if the player rolled back out of it, else IN_NONE. Either way
 * the screens' stages are no longer what g_screen_gen says. */
static int transition(const u16 *a, u32 here)
{
    int kind = a[0], r = IN_NONE, exact = 0;
    u32 t0 = now_ms();
#ifdef MW_SLOW
    u16 slow[4];
    slow[0] = a[0];
    slow[1] = (u16)(a[1] * MW_SLOW);
    slow[2] = (u16)(a[2] * MW_SLOW);
    slow[3] = (u16)(a[3] * MW_SLOW);
    a = slow;                       /* test builds: slow motion */
#endif

    (void)t0;                       /* only the trace reads it */
    update_stage();

    switch (kind) {
    case MWB_T_DISSOLVE:
    case MWB_T_FADE:
        r = stage_fade(a, here, t0, &exact);
        break;

    case MWB_T_MOVE:
    case MWB_T_MOVEINLEFT:
    case MWB_T_MOVEINRIGHT:
    case MWB_T_MOVEOUTLEFT:
    case MWB_T_MOVEOUTRIGHT:
        if (g_st.title != NO_STR || g_prev.title != NO_STR)
            break;                  /* nothing on a title card moves */
        r = move(kind, a[1], here);
        break;
    }
    forget_screens();               /* whatever happened, they are drawn over */
    trace("with %d: %lums", kind, (unsigned long)(now_ms() - t0));
    if (r == IN_ROLLED)
        return r;

    /* A dissolve or fade that ran its course left `to` on the screen; a
     * move, or a click, needs it put up. */
    if (exact) {
        g_screen_gen[gfx_back_index() ^ 1] = g_stage_gen;
    } else {
        int b = gfx_back_index();
        gfx_copy_stage(gfx_back(), g_stage);
        gfx_text_clear(gfx_back());
        g_screen_gen[b] = g_stage_gen;
        gfx_flip();
    }
    g_prev = g_st;
    g_prev_gen = g_stage_gen;
    return IN_NONE;
}

/* ---- the rhythm game ---- */

/* MWB_RHYTHM: mus, btm, bg, note, then a `with` (mwb.h). The minigame is
 * rhythm.c's; here it gets its pictures and data, a transition into its
 * first frame, and the screens back afterwards. Its images are read from
 * the pak into buffers of its own, not the sprite arena, which moves what
 * it holds. The test build's autoplay does not play it: it gives back a
 * run with no hits. */
static int rhythm(const u16 *a, u32 here)
{
    rhythm_in in;
    u8 *own[9];
    char name[16];
    int i, ok = 1, exact;
    u32 size;

    memset(own, 0, sizeof(own));
    if (g_autoplay) {
        u8 *b;
        sprintf(name, "R%02u.BTM", a[1]);
        b = pak_load(name, &size);
        g_ret[0] = g_ret[2] = 0;
        g_ret[1] = b ? (s32)be16(b + 8) : 0;
        free(b);
        trace("vm: rhythm skipped (autoplay)");
        return IN_NONE;
    }
    music_stop();
    g_st.music = -1;
    sprintf(name, "R%02u.BTM", a[1]);
    own[8] = pak_load(name, &size);
    for (i = 0; i < 8; i++) {
        own[i] = pak_load(img_file(a[3] + i), &size);
        ok &= own[i] != NULL;
    }
    ok &= own[8] != NULL;
    if (ok && pak_read(img_file(a[2]), g_bgbuf, STAGE_BYTES))
        g_bgbuf_img = a[2];
    else
        ok = 0;
    for (i = 0; i < 8; i++)
        in.note[i] = own[i];
    in.btm = own[8];
    in.bg = g_bgbuf;
    in.mus = a[0];
    in.work = g_diff;               /* free once the fade into it is over */
    ok = ok && music_load(a[0]) && rhythm_begin(&in);
    if (ok) {
        int r;
        /* Into its first frame: the stage buffer holds it for the
         * transition, and is the state's again afterwards. */
        rhythm_first(g_stage);
        g_stage_dirty = 1;
        r = a[4] == MWB_T_NONE || skipping() ? IN_NONE :
            stage_fade(a + 4, here, now_ms(), &exact);
        /* Esc during the fade is the game's, not the menu's. */
        g_menu_pending = 0;
        (void)r;
        /* A fade that had to put its baseline up first (after a choice
         * menu, say) composed it, reloading the scene's backdrop over the
         * game's: the game's layer is made from this buffer. */
        if (g_bgbuf_img != a[2]) {
            if (pak_read(img_file(a[2]), g_bgbuf, STAGE_BYTES))
                g_bgbuf_img = a[2];
            else
                trace("vm: rhythm: cannot reload its backdrop");
        }
        rhythm_run(g_ret);
        /* The click that closed its summary is the game's: still held, it
         * would read as a new one here and skip the next transition. */
        input_reset();
        trace("vm: rhythm (%ld, %ld, %ld)", (long)g_ret[0], (long)g_ret[1],
              (long)g_ret[2]);
    } else {
        trace("vm: rhythm: cannot load what it needs");
        g_ret[0] = g_ret[1] = g_ret[2] = 0;
    }
    rhythm_end();
    for (i = 0; i < 9; i++)
        free(own[i]);
    forget_screens();
    with_none();
    if (ok) {
        /* The next `with` starts from the game's last frame, still on the
         * shown screen: Ren'Py's starts from the last scene displayed
         * (core.py:2416, 2724), which held the screen `call screen` showed
         * (statementexports.py:327) -- nothing interacts in between. A
         * generation of its own marks it the baseline; g_stage, dirty, is
         * composed afresh under a later one. */
        g_stage_dirty = 1;
        g_prev_gen = ++g_stage_gen;
        g_screen_gen[gfx_back_index() ^ 1] = g_prev_gen;
    }
    return IN_NONE;
}

/* ---- the credits roll ---- */

/* Put the scroll where the clock says, drawing straight into the back
 * screen: the stage buffer would cost one more 28.8KB copy per step. The
 * roll runs under a pause, which has no text -- but the back screen still
 * has the line said two frames ago, so the strip is cleared too. */
static void scroll_step(void)
{
    u32 t = now_ms() - g_scroll.start;
    s16 y = t >= g_scroll.ms ? g_scroll.y1 :
            (s16)(g_scroll.y0 + (s32)(g_scroll.y1 - g_scroll.y0) *
                  (s32)t / (s32)g_scroll.ms);

    if (y == g_scroll.y)
        return;
    g_scroll.y = y;
    compose(&g_st, gfx_back());
    gfx_text_clear(gfx_back());
    g_screen_gen[gfx_back_index()] = 0;
    g_stage_dirty = 1;
    gfx_flip();
    if (t >= g_scroll.ms)
        g_scroll.on = 0;
}

/* ---- the game menu ---- */

/* Fill in a save of the position at `here`: the checkpoint at or before
 * it, which a load re-runs, and what the slot shows -- the line, or the
 * chapter, and the picture on the screen. */
static void make_save(save_rec *s, int who, const u8 *text, u32 here)
{
    int cp = cp_at_or_before(log_cur() ? log_cur()->pc : here);
    const u8 *p = NULL;
    int n = 0;

    memset(s, 0, sizeof(*s));
    s->used = 1;
    s->cp = (u16)(cp < 0 ? 0 : cp);
    s->rb_limit = (u16)g_rb_limit;
    if (text) {
        if (who >= 0) {
            const u8 *name = char_name(who);
            while (*name && n < SAVE_TEXT - 3)
                s->text[n++] = *name++;
            s->text[n++] = ':';
            s->text[n++] = ' ';
        }
        p = text;
    } else if (g_st.chapter != NO_STR) {
        p = str(g_st.chapter);
    }
    while (p && *p && n < SAVE_TEXT - 1) {
        u8 c = *p++;
        if (c == '\n')
            c = ' ';
        if (is_style(c))
            continue;
        s->text[n++] = c;
    }
    s->text[n] = 0;
    gfx_thumb(gfx_front(), s->thumb);
}

/* A slot's rollback log: MIWAnn.LOG beside MIWA.SAV, so that the slots and
 * the preferences there keep the layout they had, and a slot saved before
 * there were logs still loads (from its checkpoint, as it did then).
 *
 *     u32 magic, u32 script signature, u16 date, u16 time (the slot's:
 *     a log is good only for the save it was written with), u16 nvar,
 *     u16 entries, u16 behind, u16 0, nvar x u32 variable name hash,
 *     entries x { u16 checkpoint number, u16 answer, nvar x value }
 *
 * A value is as val_put has it. The variables are filed by name, as Ren'Py's
 * pickled store is: one this build does not have is dropped, one the save
 * does not have gets its `default` -- and so does one holding a string
 * this build lacks. Checkpoints are by number, as the slot's own is: what
 * the save signature says is the same. */
#define LOG_MAGIC 0x4D574C32UL      /* "MWL2" */
#define LOG_HDR   20

static void log_file(char *name, int slot)
{
    memcpy(name, "MIWA00.LOG", 11);
    name[4] = (char)('0' + slot / 10);
    name[5] = (char)('0' + slot % 10);
}

int log_write(int slot, const save_rec *r)
{
    int nent = g_nback + g_nfwd, k;
    u32 ent = 4 + VAL_BYTES * (u32)g_nrb, size = LOG_HDR + 4 * (u32)g_nrb +
                                                 (u32)nent * ent;
    u8 *b = malloc(size), *p;
    char name[16];
    long fd;
    u16 i;

    if (!b)
        return 0;
    put16(b, (u16)(LOG_MAGIC >> 16));
    put16(b + 2, (u16)LOG_MAGIC);
    put16(b + 4, (u16)(g_script_sig >> 16));
    put16(b + 6, (u16)g_script_sig);
    put16(b + 8, r->date);
    put16(b + 10, r->time);
    put16(b + 12, g_nrb);
    put16(b + 14, (u16)nent);
    put16(b + 16, (u16)g_nback);
    put16(b + 18, 0);
    p = b + LOG_HDR;
    for (i = 0; i < g_nrb; i++, p += 4)
        memcpy(p, VAR_ENT(g_rbvar[i]), 4);
    for (k = 0; k < nent; k++, p += ent) {
        const val *v = log_vars(k);
        put16(p, (u16)cp_at_or_before(log_at(k)->pc));
        put16(p + 2, log_at(k)->answer);
        for (i = 0; i < g_nrb; i++)
            val_put(p + 4 + VAL_BYTES * i, v[i]);
    }
    log_file(name, slot);
    fd = Fcreate(name, 0);
    k = fd >= 0 && Fwrite((short)fd, (long)size, b) == (long)size;
    if (fd >= 0)
        Fclose((short)fd);
    if (!k)
        trace("vm: cannot write %s", name);
    free(b);
    return k;
}

void log_delete(int slot)
{
    char name[16];
    log_file(name, slot);
    Fdelete(name);
}

/* Slot `slot`'s log, saved with `r`, into the log; 0 if there is none for
 * it, or it is not for this script. */
static int log_read(int slot, const save_rec *r)
{
    long fd, size;
    u8 *b = NULL, *p;
    char name[16];
    int ok = 0, nent, nback, k;
    u16 nv, i, j;
    u32 ent;

    log_file(name, slot);
    fd = Fopen(name, 0);
    if (fd < 0)
        return 0;
    size = Fseek(0, (short)fd, 2);
    Fseek(0, (short)fd, 0);
    if (size >= LOG_HDR && (b = malloc((u32)size)) != NULL &&
        Fread((short)fd, size, b) == size) {
        nv = be16(b + 12);
        nent = be16(b + 14);
        nback = be16(b + 16);
        ent = 4 + VAL_BYTES * (u32)nv;
        ok = be32(b) == LOG_MAGIC && be32(b + 4) == g_script_sig &&
             be16(b + 8) == r->date && be16(b + 10) == r->time &&
             nback >= 1 && nback <= nent && nent <= LOG_MAX &&
             (u32)size == LOG_HDR + 4 * (u32)nv + (u32)nent * ent;
        p = b + LOG_HDR + 4 * (u32)nv;
        for (k = 0; ok && k < nent; k++)
            ok = be16(p + k * ent) < (u16)g_ncp;
        if (ok) {
            log_reset();
            g_nback = nback;
            g_nfwd = nent - nback;
            for (k = 0; k < nent; k++, p += ent) {
                val *v = log_vars(k);
                log_at(k)->pc = g_cp[be16(p)];
                log_at(k)->answer = be16(p + 2);
                for (i = 0; i < g_nrb; i++) {
                    const u8 *e = VAR_ENT(g_rbvar[i]);
                    v[i].type = e[5];
                    v[i].v = (s32)be32(e + 8);
                    for (j = 0; j < nv; j++)
                        if (!memcmp(b + LOG_HDR + 4 * (u32)j, e, 4)) {
                            if (!val_get(p + 4 + VAL_BYTES * j, &v[i])) {
                                v[i].type = e[5];
                                v[i].v = (s32)be32(e + 8);
                            }
                            break;
                        }
                }
            }
        }
    }
    Fclose((short)fd);
    free(b);
    if (!ok)
        trace("vm: %s is not for this save", name);
    return ok;
}

/* Load slot `slot`, saved as `r`: its log, or with none, its checkpoint
 * and the variables at their defaults; then the replay to the checkpoint
 * it was saved at, which is re-run -- a menu is put up again. */
static void load_slot(int slot, const save_rec *r)
{
    if (!log_read(slot, r)) {
        /* With a script that never branches (Miwa's, whose slots predate
         * the logs), what was played before the checkpoint is the
         * checkpoints before it in the file, as many as the rollback
         * limit can reach; otherwise just the checkpoint. */
        int cp = r->cp < g_ncp ? r->cp : 0, k;
        int behind = g_branches ? 0 : r->rb_limit < cp ? r->rb_limit : cp;
        log_reset();
        vars_reset(0);
        if (behind > LOG_MAX - 1)
            behind = LOG_MAX - 1;
        g_nback = behind + 1;
        for (k = 0; k <= behind; k++) {
            log_at(k)->pc = g_cp[cp - behind + k];
            log_at(k)->answer = k < behind ? 0 : ANS_NONE;
            log_vars_put(k);
        }
    }
    trace("load: slot %d, checkpoint %u, %d behind, %d ahead, rollback %u",
          slot, r->cp, g_nback, g_nfwd, r->rb_limit);
    log_vars_get(g_nback - 1);
    replay_to(log_cur()->pc);
    g_resume = 1;
    /* The re-run gains its step back, as after a rollback. */
    g_rb_limit = r->rb_limit ? r->rb_limit - 1 : 0;
    path_note("load", slot);
}

/* The menu changed g_lang: Ren'Py's change_language. The text is read
 * again, the keyframes are made again (a language branch may differ), and
 * rollback is blocked (translation/__init__.py:857). What is on the stage
 * stays: the images a branch already chose are not chosen again. */
static void language_changed(void)
{
    u32 t0 = now_ms();

    (void)t0;                       /* only the trace reads it */
    if (!text_load(g_lang))
        fatal("cannot read the text");
    make_keyframes();
    g_rb_limit = 0;
    trace("vm: language %d in %lums", g_lang,
          (unsigned long)(now_ms() - t0));
}

/* A new game from the top of the script and its title card, as Ren'Py's
 * main menu and Start: at the end of `start`, and the menu's Restart. The
 * variables are at their defaults and there is nothing to roll back into;
 * the persistent ones stay. The baseline stays what is on the screen, so
 * the opening fade starts from the last picture. */
static void new_game(void)
{
    state_reset();
    vars_reset(0);
    log_reset();
    g_pc = 0;
    g_stage_dirty = 1;
    g_rb_limit = 0;
    g_after_roll = 0;
    g_scroll.on = 0;
    g_menu_pending = 0;
    music_sync(g_st.music);
}

/* Esc at an interaction. Returns IN_QUIT, IN_ROLLED after a load, a
 * restart or a change of language at a say or a menu, or IN_NONE with the
 * frame put back -- by the caller, at a menu. */
static int game_menu(int who, const u8 *text, u32 here, int choice)
{
    save_rec *cur = malloc(sizeof(save_rec));
    save_rec *ld = malloc(sizeof(save_rec));
    int r = IN_NONE, m, lang0 = g_lang, slot = 0;

    if (!cur || !ld) {
        free(cur);
        free(ld);
        return IN_NONE;
    }
    trace("menu: open, %ld free", (long)Malloc(-1L));
    make_save(cur, who, text, here);
    trace("menu: save made");
    m = menu_run(cur, ld, &slot);
    trace("menu: %d", m);
    forget_screens();
    if (g_lang != lang0)
        language_changed();
    if (m == MENU_QUIT) {
        r = IN_QUIT;
    } else if (m == MENU_RESTART) {
        trace("vm: restart");
        path_note("restart", -1);
        new_game();
        r = IN_ROLLED;
    } else if (m == MENU_LOAD && ld->cp < g_ncp) {
        load_slot(slot, ld);
        r = IN_ROLLED;
    } else if (g_lang != lang0 && (text || choice)) {
        /* check_language (translation/__init__.py:864): the say starts
         * again in the new language, typed afresh; a menu is put up again
         * in it. The same checkpoint, not a new one. */
        g_pc = here;
        g_after_roll = 0;
        g_resume = 1;
        r = IN_ROLLED;
    } else if (choice) {
        /* The choice screen puts itself back. */
    } else if (g_lang != lang0) {
        /* A pause: a title card is drawn again in the new language. Not
         * run again, so the checkpoint it is waited at keeps its step:
         * Ren'Py takes a pause's checkpoint after its interaction
         * (statementexports.py:195), after block_rollback zeroed the
         * limit, so the next line can roll back to it -- not past it.
         * Here it was counted on arrival and language_changed zeroed it. A
         * say or a menu run again counts its own (checkpoint). */
        logent *cur = log_cur();
        if (cur && cur->pc == here)
            g_rb_limit = 1;
        if (g_st.title != NO_STR)
            g_stage_dirty = 1;
        frame(who, text);
    } else if (g_type.on) {
        /* Back as it was: as much of the line as had been shown. */
        int n = g_type.shown[gfx_back_index() ^ 1];
        frame_n(who, text, n < 0 ? 0 : n);
        g_type.shown[gfx_back_index() ^ 1] = n < 0 ? 0 : n;
        g_type.shown[gfx_back_index()] = -1;
    } else {
        frame(who, text);
    }
    free(cur);
    free(ld);
    return r;
}

/* ---- the test build's player ----
 *
 * ANSWERS= builds answer the menus from a list: an entry is the option, 0
 * being the first one shown (and past the list's end, 0). A negative one
 * is something done at that menu before the next entry answers it:
 *
 *     -k        roll back k times: there, and at each checkpoint after
 *     -100-k    roll forward k times
 *     -200-k    roll back k times, then forward k times
 *     -300-s    save in slot s
 *     -400-s    load slot s
 *
 * They go through roll(), save_put and load_slot as the keys and the
 * menus do, and are traced for check-st. */
#ifdef MW_ANSWERS
static const int g_answer[] = { MW_ANSWERS };
static int       g_nanswer;         /* entries used */
static int       g_npick;           /* menus answered */
static int       g_test_back, g_test_fwd;

/* At a line or an untimed pause: the rolls still to do. One that cannot
 * be done is traced too: a test may expect it, or not. */
static int test_roll(u32 here)
{
    if (g_test_back > 0) {
        g_test_back--;
        if (roll(-1, here))
            return IN_BACK;
        path_note("refused back", -1);
    } else if (g_test_fwd > 0) {
        g_test_fwd--;
        if (roll(1, here))
            return IN_FORWARD;
        path_note("refused fwd", -1);
    }
    return IN_NONE;
}
#endif

/* ---- interactions ---- */

/* An interaction: the frame, then the wait, with the skip decided once for
 * both. Deciding it twice would let Ctrl come up in between, and the game
 * would wait on a line it had not drawn, the previous one still showing.
 *
 * Skipping, every line still gets its frame -- Ren'Py draws the screen
 * before the say behaviour can dismiss it, and that waits for
 * config.skip_delay (behavior.py:811-815, core.py:2837; 5ms, config.py:123)
 * -- and the wait is skip_delay: the next tick of the 200Hz counter. The
 * flip already waits a VBL, so a line is up for at least 14ms. Otherwise the frame goes up, the music a skip or a replay deferred
 * is put right -- this is where the player is going to listen -- and the
 * wait is the player's. Returns IN_*, with IN_ROLLED meaning g_pc has been
 * moved. */
static int interact(int who, const u8 *text, u32 here, u32 ms, int flags)
{
    u32 until;
    int r;

    if (g_rolling) {
        /* Rolling forward: a timed pause on the way is not waited for --
         * the replay a rollforward used to be never waited either. */
        g_scroll.on = 0;
        return IN_TIMEOUT;
    }
    if (g_persist_dirty)
        persist_save();
    if (skipping()) {
        u32 t;
        g_type.on = 0;
        frame(who, text);
        t = now_ms();
        while (now_ms() == t)
            ;
        g_scroll.on = 0;
        return IN_SKIP;
    }
    type_begin(who, text);
    music_sync(g_st.music);
    if (g_autoplay) {
        g_scroll.on = 0;
        if (g_menu_pending)
            return IN_QUIT;         /* Esc during a transition */
#ifdef MW_ANSWERS
        if (!ms && (r = test_roll(here)) != IN_NONE)
            return r;
#endif
        return wait_input(ms, flags);
    }

    if (g_scroll.on && !g_scroll.start)
        g_scroll.start = now_ms();
    until = now_ms() + ms;
    input_reset();
    for (;;) {
        if (g_menu_pending) {
            g_menu_pending = 0;
            r = IN_MENU;
        } else {
            r = poll_input(flags | (skip_ok() ? W_SKIPOK : 0));
        }
        if (r == IN_ADVANCE && !type_done()) {
            /* A click finishes the line and does nothing else
             * (text/text.py:2661). */
            g_type.all = 1;
            continue;
        }
        if (r == IN_BACK || r == IN_FORWARD) {
            if (roll(r == IN_BACK ? -1 : 1, here))
                break;
            continue;               /* nowhere to go: keep waiting */
        }
        if (r == IN_MENU) {
            /* The menu's time is not the pause's, nor the roll's. */
            u32 t = now_ms();
            r = game_menu(who, text, here, 0);
            if (r != IN_NONE)
                break;
            until += now_ms() - t;
            g_scroll.start += now_ms() - t;
            if (g_type.on && g_text_cps) {
                /* Typing resumes from what is shown, at the speed the
                 * menu may have just changed. */
                int n = g_type.shown[gfx_back_index() ^ 1];
                g_type.start = now_ms() -
                               (u32)n * 1000 / (u32)g_text_cps;
            }
            input_reset();
            continue;
        }
        if (r != IN_NONE)
            break;
        if (g_scroll.on)
            scroll_step();
        if (ms && (long)(now_ms() - until) >= 0) {
            r = IN_TIMEOUT;
            break;
        }
        if (!type_step())           /* a step waits its own VBL */
            Vsync();
    }
    g_scroll.on = 0;
    g_type.on = 0;
    return r;
}

/* ---- the choice menu (MENU) ----
 *
 * Ren'Py's menu: the options whose condition holds (an option with a false
 * one is left out, config.menu_include_disabled; menuexports.py:101), and
 * if none does, no menu at all -- the game goes on past it
 * (menuexports.py:116). Otherwise the choice screen over the stage
 * (menu.c), the caption in the text strip, typed as a say is: a press
 * finishes it and does nothing else (text/text.py:2661) -- unless it is on
 * a button, which is above the say screen and takes it first.
 *
 * Keys: the arrows move the focus (Ren'Py's focus_up/focus_down; at a menu
 * they are not rollback, Undo is), Return chooses the focused option or,
 * with none focused, finishes the caption, as Space does. The pointer
 * focuses what it is over. Undo rolls back; Help rolls forward -- Ren'Py's
 * Page Down, which the ST does not have, and Down is the focus here. */
static u8 g_optext[MWB_MENU_MAXOPT][MWB_OPT_COLS + 16];

#define SC_UP   0x48
#define SC_DOWN 0x50
#define SC_UNDO 0x61
#define SC_HELP 0x62

/* The option shown that a rollforward answers with -- the one answered
 * last time, if it is still offered (menuexports.py:306-309) -- or -1. */
static int fwd_pick(const u16 *opt, int n, u32 here)
{
    logent *e = log_cur();
    int i;

    if (!e || e->pc != here || e->answer == ANS_NONE)
        return -1;
    for (i = 0; i < n; i++)
        if (opt[i] == e->answer)
            return i;
    return -1;
}

/* The screen with the stage, the buttons and the first `chars` of the
 * caption, up. Its stage is not g_stage's any more. */
static void choice_frame(int who, const u8 *cap, int chars,
                         const u8 *const *label, int n, int sel)
{
    stage_to_back();
    choice_draw(gfx_back(), label, n, sel);
    g_screen_gen[gfx_back_index()] = 0;
    say_strip(gfx_back(), who, cap, chars);
    gfx_pointer(0);
    gfx_flip();
    gfx_pointer(1);
}

/* Option `k` shown taken, `fwd` if by rolling forward: logged, and g_pc
 * where it goes. */
static int choose(const u16 *a, const u16 *opt, int k, int fwd)
{
    if (fwd)
        g_rolling = 1;
    log_done(opt[k], fwd);
    path_note("pick", k);
    g_pc = be16(g_opttab + 6 * (u32)(a[2] + opt[k]) + 4);
    return IN_ADVANCE;
}

/* The MENU at `here`. Returns IN_ADVANCE with g_pc where the answer goes,
 * or IN_QUIT, or IN_ROLLED. */
static int choice(const u16 *a, u32 here)
{
    const u8 *label[MWB_MENU_MAXOPT];
    u16 opt[MWB_MENU_MAXOPT];       /* each one shown, from the first */
    int who = who_word(a[0]);
    const u8 *cap = NULL;
    int n = 0, i, sel = -1, shown = -1, shown_sel = -2, buttons, mx, my;
    int r = IN_NONE;

    if (a[1] != NO_STR)
        cap = text_vars(a[1], g_vtext, VTEXT_MAX, MWB_TEXT_COLS);
    for (i = 0; i < a[3] && n < MWB_MENU_MAXOPT; i++) {
        const u8 *o = g_opttab + 6 * (u32)(a[2] + i);
        u16 cond = be16(o + 2);
        if (cond != 0xFFFF && !truthy(eval(cond)))
            continue;
        label[n] = text_vars(be16(o), g_optext[n], sizeof(g_optext[0]), 0);
        opt[n++] = (u16)i;
    }
#ifdef MW_ANSWERS
    /* A path the answers keep in a loop ends as the compiler's walk of it
     * does: 64 menus answered (mwc.c MENU_MAX), then "cut". */
    if (g_autoplay && g_npick >= 64) {
        path_end("cut");
        return IN_QUIT;
    }
#endif
    path_menu(who, cap, label, n);
    if (!n) {
        g_pc = a[4];
        return IN_ADVANCE;
    }

    if (skip_held())
        g_skip_cut = 1;
    if (g_persist_dirty)
        persist_save();
    checkpoint(here);
    g_type.on = cap && g_text_cps && !g_autoplay && !g_after_roll;
    g_type.all = 0;
    g_type.len = cap ? say_len(cap) : 0;
    g_type.start = now_ms();
    g_after_roll = 0;
    g_prev = g_st;                  /* what a transition after it is from */
    g_prev_gen = g_stage_gen;

    if (g_autoplay) {
        int k = 0;
        choice_frame(who, cap, 0x7FFF, label, n, -1);
        gfx_pointer(0);
        forget_screens();
        g_type.on = 0;
        music_sync(g_st.music);
        if (wait_input(1, 0) == IN_QUIT)
            return IN_QUIT;
#ifdef MW_ANSWERS
        for (;;) {
            int t;
            if (g_test_back > 0) {
                g_test_back--;
                if (roll(-1, here))
                    return IN_ROLLED;
                path_note("refused back", -1);
                continue;
            }
            if (g_test_fwd > 0) {
                g_test_fwd--;
                if ((k = fwd_pick(opt, n, here)) >= 0) {
                    path_note("fwd", -1);
                    g_npick++;
                    return choose(a, opt, k, 1);
                }
                path_note("refused fwd", -1);
                k = 0;
                continue;
            }
            if (g_nanswer >= (int)(sizeof(g_answer) / sizeof(g_answer[0])))
                break;
            t = g_answer[g_nanswer++];
            if (t >= 0) {
                k = t;
                break;
            }
            if (t > -100) {
                g_test_back = -t;
            } else if (t > -200) {
                g_test_fwd = -t - 100;
            } else if (t > -300) {
                g_test_back = g_test_fwd = -t - 200;
            } else if (t > -400) {
                save_rec *s = malloc(sizeof(save_rec));
                if (s) {
                    make_save(s, who, cap, here);
                    if (save_put(-t - 300, s))
                        path_note("save", -t - 300);
                    free(s);
                }
            } else {
                save_rec *s = malloc(sizeof(save_rec));
                if (s) {
                    save_get(-t - 400, s);
                    if (s->used && s->cp < g_ncp) {
                        load_slot(-t - 400, s);
                        free(s);
                        return IN_ROLLED;
                    }
                    free(s);
                }
            }
        }
        g_npick++;
#endif
        return choose(a, opt, k < n ? k : 0, 0);
    }

    buttons = gfx_mouse_buttons();
    gfx_mouse_pos(&mx, &my);
    sel = choice_at(mx, my, label, n);
    shown = g_type.on ? 0 : 0x7FFF;
    choice_frame(who, cap, shown, label, n, sel);
    shown_sel = sel;
    music_sync(g_st.music);
    for (;;) {
        long key = read_key();
        int c = (int)(key & 0xFF), sc = (int)((key >> 16) & 0xFF);
        int b = gfx_mouse_buttons(), pressed = b & ~buttons, x, y;
        int chars, pick = -1;

        buttons = b;
        gfx_mouse_pos(&x, &y);
        if (g_menu_pending) {
            g_menu_pending = 0;
            c = 0x1B;
        }
        if (c == 0x1B || (pressed & 2)) {
            u32 t = now_ms();
            gfx_pointer(0);
            r = game_menu(who, cap, here, 1);
            if (r != IN_NONE)
                break;
            if (g_type.on && g_text_cps)
                g_type.start += now_ms() - t;
            shown_sel = -2;         /* put the screen back */
            buttons = gfx_mouse_buttons();
            continue;
        }
        if (sc == SC_UNDO) {
            gfx_pointer(0);
            if (roll(-1, here)) {
                r = IN_ROLLED;
                break;
            }
            gfx_pointer(1);
        } else if (sc == SC_HELP) {
            int k = fwd_pick(opt, n, here);
            if (k >= 0) {
                path_note("fwd", -1);
                r = choose(a, opt, k, 1);
                break;
            }
        } else if (sc == SC_UP || sc == SC_DOWN) {
            sel = sel < 0 ? (sc == SC_UP ? n - 1 : 0)
                          : (sel + (sc == SC_UP ? n - 1 : 1)) % n;
        } else if (c == '\r' && sel >= 0) {
            pick = sel;
        } else if (c == '\r' || c == ' ') {
            g_type.all = 1;
        } else if (pressed & 1) {
            pick = choice_at(x, y, label, n);
            if (pick < 0)
                g_type.all = 1;
        } else if (x != mx || y != my) {
            sel = choice_at(x, y, label, n);
        }
        mx = x;
        my = y;
        if (pick >= 0) {
            r = choose(a, opt, pick, 0);
            break;
        }
        chars = g_type.on ? type_due() : 0x7FFF;
        if (chars != shown || sel != shown_sel) {
            choice_frame(who, cap, chars, label, n, sel);
            shown = chars;
            shown_sel = sel;
        } else {
            Vsync();
        }
    }
    gfx_pointer(0);
    forget_screens();
    g_type.on = 0;
    return r;
}

/* ---- loading ---- */

/* Whether a command's jumps land inside the code and its options inside
 * the table: a script out of step with this runtime fails at load. */
static int jumps_ok(const u16 *c)
{
    u32 nopt = be32(g_mwb + 68);
    u16 o;

    switch (c[0]) {
    case MWB_GOTO:
        return c[1] < g_code_words;
    case MWB_IF:
        return c[2] < g_code_words;
    case MWB_MENU:
        if (c[4] > MWB_MENU_MAXOPT || (u32)c[3] + c[4] > nopt ||
            c[5] >= g_code_words)
            return 0;
        for (o = 0; o < c[4]; o++)
            if (be16(g_opttab + 6 * (u32)(c[3] + o) + 4) >= g_code_words)
                return 0;
        return 1;
    }
    return 1;
}

int vm_load(void)
{
    u32 size, free_now, pc;
    int n;

    g_mwb = pak_load("SCRIPT.MWB", &size);
    if (!g_mwb || be32(g_mwb) != MWB_MAGIC || be16(g_mwb + 4) != MWB_VERSION)
        return 0;
    g_nimg = be16(g_mwb + 6);
    g_nchar = be16(g_mwb + 8);
    g_imgtab = g_mwb + be32(g_mwb + 16);
    g_chartab = g_mwb + be32(g_mwb + 20);
    g_code = (const u16 *)(g_mwb + be32(g_mwb + 28));
    g_code_words = be32(g_mwb + 32) / 2;
    g_nstr = be32(g_mwb + 36);
    if (g_code_words > 0xFFFFUL || be32(g_mwb + 40) != MWB_NLANG)
        return 0;                   /* checkpoints are 16-bit offsets */
    g_vartab = g_mwb + be32(g_mwb + 48);
    g_nvar = (u16)be32(g_mwb + 52);
    g_expr = (const u16 *)(g_mwb + be32(g_mwb + 56));
    g_opttab = g_mwb + be32(g_mwb + 64);
    g_vstrtab = g_mwb + be32(g_mwb + 72);
    g_nvstr = (u16)be32(g_mwb + 76);
    for (n = 0; n < (int)g_nvstr; n++)
        if (be32(g_vstrtab + 8 * (u32)n + 4) >= be32(g_mwb + 28))
            return 0;               /* the texts end where the code starts */
    g_var = malloc((u32)g_nvar * sizeof(val) + 1);
    g_rbvar = malloc((u32)g_nvar * sizeof(u16) + 1);
    g_pkeep = calloc((u32)g_nvar + 1, 1);
    if (!g_var || !g_rbvar || !g_pkeep)
        return 0;
    vars_reset(1);
    persist_load();
    for (n = 0; n < (int)g_nvar; n++)
        if (!(VAR_ENT(n)[4] & MWB_VAR_PERSISTENT))
            g_rbvar[g_nrb++] = (u16)n;
    g_log = malloc(LOG_MAX * sizeof(logent));
    g_logvar = malloc(LOG_MAX * (u32)g_nrb * sizeof(val) + 1);
    if (!g_log || !g_logvar)
        return 0;

    /* One language's text at a time, in a buffer that holds either. */
    g_text = malloc((u32)pak_max_size("TEXT_"));
    if (!g_text)
        return 0;

    /* Find the checkpoints, and check on the way that the commands tile
     * the code exactly -- a length table out of step with the compiler
     * would otherwise make every replay land somewhere wrong. */
    for (n = 0, pc = 0; pc < g_code_words; pc += g_cmd_words[g_code[pc]]) {
        u16 op = g_code[pc];
        if (op >= MWB_NCMD || !jumps_ok(g_code + pc))
            return 0;
        if (is_checkpoint(g_code + pc))
            n++;
        /* Keyframes (make_keyframes): one per place a jump lands. */
        if (op == MWB_GOTO || op == MWB_IF)
            g_capkey++;
        else if (op == MWB_MENU)
            g_capkey += g_code[pc + 4] + 1;
        if (op == MWB_GOTO || op == MWB_IF || op == MWB_MENU)
            g_branches++;
    }
    /* ... one per KEY_EVERY checkpoints, and the top. */
    g_capkey += 2 + n / KEY_EVERY;
    if (pc != g_code_words)
        return 0;
    g_cp = malloc((u32)n * sizeof(*g_cp) + 2);
    if (!g_cp)
        return 0;
    for (g_ncp = 0, pc = 0; pc < g_code_words; pc += g_cmd_words[g_code[pc]])
        if (is_checkpoint(g_code + pc))
            g_cp[g_ncp++] = (u16)pc;
    /* The keyframes' table, now: taken after the sprite budget it would
     * come out of the reserve the menus need (27KB with the rhythm menus,
     * which left the save screen no 32KB hunk for its page of slots). */
    g_key = malloc((u32)g_capkey * sizeof(keyframe) + 1);
    if (!g_key)
        return 0;

    /* What a save is good for: any build with these checkpoints (mwb.h). */
    g_script_sig = be32(g_mwb + 44);

    g_stage = malloc(STAGE_BYTES);
    g_bgbuf = malloc(STAGE_BYTES);
    g_diff = malloc(STAGE_BYTES);
    g_spr = calloc(g_nimg, sizeof(*g_spr));
    g_spr_used = calloc(g_nimg, sizeof(*g_spr_used));
    g_order = calloc(g_nimg, sizeof(*g_order));
    if (!g_stage || !g_bgbuf || !g_diff || !g_spr || !g_spr_used || !g_order)
        return 0;
    make_bayer();

    /* Sprites and the tune may use what is free now, less a reserve for
     * the C library, TOS and the menus -- all of it now, in one piece (the
     * arena, the tune at its top: arena_cede). Straight from TOS, not
     * through libcmini's heap. */
    free_now = (u32)Malloc(-1L);
    g_block = free_now > 64000UL ? (free_now - 64000UL) & ~3UL : 0;
#ifdef MW_SPRBUDGET
    /* a smaller machine: the sprites' part with the longest tune */
    g_block = ((u32)(MW_SPRBUDGET) + (u32)music_max()) & ~3UL;
#endif
    if (g_block < (u32)music_max() + 32000UL)
        return 0;
    g_arena = (u8 *)Malloc(g_block);
    if (!g_arena)
        return 0;
    g_spr_budget = g_block;
    trace("vm: %lu byte script, %u images, %d checkpoints, %lu free, "
          "sprite budget %lu, %lu with the longest tune", (unsigned long)size,
          g_nimg, g_ncp, (unsigned long)free_now, (unsigned long)g_block,
          (unsigned long)(g_block - (u32)music_max()));
    return 1;
}

#ifdef MW_SELFTEST
/* The blitter's sprites against the CPU's, pixel for pixel, at every
 * shift and clipped on every side. Traces each mismatch. */
static void selftest(void)
{
    static const int ids[] = { 1, 2, 34, 44, 58 };
    static const int xs[] = { 0, 1, 7, 15, 16, 17, 100, 333, -1, -15, -16,
                              -17, -60, -330, 539, 560, 600, 625, 639 };
    static const int ys[] = { 0, 87, -20, 300 };
    u8 *a = malloc(STAGE_BYTES), *b = malloc(STAGE_BYTES);
    u8 *t = malloc(STAGE_BYTES);
    unsigned i, j, k;
    int bad = 0, runs = 0;
    u32 p;

    if (!a || !b || !t)
        return;
    /* A pattern underneath, so a wrong mask shows. */
    for (p = 0; p < STAGE_BYTES; p++)
        t[p] = (u8)(p * 7 + (p >> 7));
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        if (ids[i] >= g_nimg || be16(g_imgtab + ids[i] * 20 + 4) != 1)
            continue;
        for (j = 0; j < sizeof(xs) / sizeof(xs[0]); j++)
            for (k = 0; k < sizeof(ys) / sizeof(ys[0]); k++) {
                const u8 *s;
                memcpy(a, t, STAGE_BYTES);
                memcpy(b, t, STAGE_BYTES);
                s = sprite(ids[i]);
                if (!s)
                    continue;
                gfx_sprite(a, s, xs[j], ys[k]);
                gfx_sprite_cpu(b, s, xs[j], ys[k]);
                runs++;
                for (p = 0; p < STAGE_BYTES; p++)
                    if (a[p] != b[p])
                        break;
                if (p < STAGE_BYTES) {
                    bad++;
                    trace("selftest: img %d at %d,%d differs at row %lu "
                          "byte %lu: blt %02x cpu %02x", ids[i], xs[j],
                          ys[k], (unsigned long)p / 80,
                          (unsigned long)p % 80, a[p], b[p]);
                }
            }
    }
    trace("selftest: %d of %d sprite blits differ", bad, runs);
    free(a);
    free(b);
    free(t);
}
#endif


#ifdef MW_SELFTEST
/* Dissolves into a buffer, with the music playing, checked against the
 * target: whether the blitter ever leaves a pass unfinished. */
static void xfade_stress(void)
{
    u8 *a = malloc(STAGE_BYTES), *x = malloc(STAGE_BYTES);
    u16 ht[16];
    u32 seed = 12345;
    int run, bad = 0, steps = 0;

    if (!a || !x)
        return;
    pak_read(img_file(0), g_stage, STAGE_BYTES);      /* to */
    for (run = 0; run < 300; run++) {
        int lev = 0;
        u32 p;
        if (run == 150) {
            trace("stress: %d of 150 wrong without music", bad);
            music_play(0, MWB_MUS_LOOP);
        }
        for (p = 0; p < STAGE_BYTES; p++)
            a[p] = (u8)(p * 13 + run);                  /* from */
        gfx_copy_stage(x, a);
        gfx_copy_stage(g_diff, a);
        gfx_blit_stage(g_diff, g_stage, BLT_HOP_SRC, BLT_OP_S_XOR_D, NULL);
        while (lev < 256) {
            int lv;
            seed = seed * 1103515245UL + 12345;
            lv = lev + 1 + (int)((seed >> 16) % 40);
            if (lv > 256)
                lv = 256;
            halftone(lev, lv, ht);
            gfx_blit_stage(x, g_diff, BLT_HOP_SRC_AND_HT, BLT_OP_S_XOR_D, ht);
            lev = lv;
            steps++;
        }
        if (memcmp(x, g_stage, STAGE_BYTES)) {
            for (p = 0; p < STAGE_BYTES && x[p] == g_stage[p]; p++)
                ;
            bad++;
            trace("stress: run %d wrong from row %lu", run,
                  (unsigned long)p / 80);
        }
    }
    trace("stress: %d of 300 dissolves wrong, %d passes", bad, steps);
    music_stop();
    free(a);
    free(x);
}
#endif

/* ---- running ---- */

void vm_run(void)
{
#ifdef MW_SELFTEST
    selftest();
    xfade_stress();
#endif
    /* The language is a preference, read by menu_init. */
    if (!text_load(g_lang))
        fatal("cannot read the text");
    make_keyframes();
    state_reset();
    log_reset();
    with_none();
    g_pc = 0;

    while (g_pc < g_code_words) {
        u32 here = g_pc;
        const u16 *c = g_code + here;
        const u16 *a = c + 1;
        u16 op = c[0];
        int r = IN_ADVANCE;

#ifdef MW_STOPAT
        /* Autoplay hands over to the player here, for looking at one
         * frame deep in the script. */
        if (g_autoplay && here * 2 >= (u32)(MW_STOPAT))
            g_autoplay = 0;
#endif
        g_pc += g_cmd_words[op];

        switch (op) {
        case MWB_END:
            /* The end of `start`: Ren'Py goes back to the main menu at
             * once -- here a new game (new_game). The test build stops
             * instead. */
            trace("vm: end");
            path_end("end");
            if (g_autoplay)
                return;
            new_game();
            break;

        case MWB_SAY: {
            int who = who_word(a[0]);
            const u8 *text = text_vars(a[1], g_vtext, VTEXT_MAX,
                                       MWB_TEXT_COLS);
            path_say(who, text);
            checkpoint(here);
            trace("say @%lu sprites %lu bytes", (unsigned long)here * 2,
                  (unsigned long)g_spr_bytes);
            r = interact(who, text, here, 0, 0);
            if (r == IN_FORWARD)
                log_done(0, 1);
            else if (r != IN_BACK && r != IN_ROLLED && r != IN_QUIT)
                log_done(0, 0);
            break;
        }

        case MWB_PAUSE: {
            u32 ms = ((u32)a[0] << 16) | a[1];
            if (!ms) {
                path_end("pause");
                checkpoint(here);
            }
            r = interact(-1, NULL, here, ms, a[2] ? W_HARD : 0);
            /* An untimed pause keeps the future when it is ended as
             * before, however: keep_rollback (statementexports.py:196). */
            if (!ms && r != IN_BACK && r != IN_ROLLED && r != IN_QUIT)
                log_done(0, r == IN_FORWARD || log_cur()->answer == 0);
            break;
        }

        case MWB_WITH:
            /* Skipping drops transitions (displayexports.py:687), and a
             * dropped one is a `with None` (core.py:1473). */
            if (a[0] == MWB_T_NONE || skipping() || g_rolling)
                with_none();
#ifdef MW_STOPAT
            else if (g_autoplay)
                with_none();        /* travelling to STOPAT */
#endif
            else
                r = transition(a, here);
            break;

        case MWB_SCROLL:
            apply(op, a);
            g_scroll.on = 1;
            g_scroll.tag = a[0];
            g_scroll.y0 = g_scroll.y = (s16)a[3];
            g_scroll.y1 = (s16)a[5];
            g_scroll.ms = ((u32)a[6] << 16) | a[7];
            g_scroll.start = 0;
            break;

        case MWB_MUSIC:
            g_st.music = (s16)a[0];
            /* Played at once, restart and all -- unless skipping, when
             * loading a 1.3MB track to hear a frame of it would stall the
             * skip for seconds. The skip's end puts the right one on
             * (interact). A divergence from Ren'Py, which plays each
             * track it skips over. Rolling forward is the same. */
            if (!skipping() && !g_rolling)
                music_play(a[0], a[1]);
            break;

        case MWB_STOPMUSIC:
            g_st.music = -1;
            music_stop();
            break;

        case MWB_FENCE:
            g_rb_limit = 0;
            break;

        case MWB_IFLANG:
            if (a[0] != (u16)g_lang)
                g_pc += a[1];
            break;

        case MWB_SET:
            set_var(a[0], eval(a[1]));
            break;

        case MWB_GOTO:
            g_pc = a[0];
            break;

        case MWB_IF:
            if (!truthy(eval(a[0])))
                g_pc = a[1];
            break;

        case MWB_MENU:
            r = choice(a, here);
            break;

        case MWB_RHYTHM:
            r = rhythm(a, here);
            break;

        default:
            if (op >= MWB_NCMD) {
                trace("vm: bad opcode %u at %lu", op, (unsigned long)here * 2);
                fatal("bad script");
                return;
            }
            apply(op, a);
            break;
        }

        if (r == IN_QUIT)
            return;
    }
}
