/* mwb.h - the compiled script format, shared by the host compiler
 * (tools/mwc.c) and the ST runtime.
 *
 * The .rpy files are parsed on the host by the C89 engine's own parser and
 * walked in execution order. Everything that needs floats, strings or the
 * scene semantics -- image attribute resolution, placement, `if` on known
 * constants, text wrapping, UTF-8 -- is settled there. What the ST gets is a
 * list of commands whose arguments are already final 640x360 pixel
 * positions and table indices, with jumps where the story branches (see
 * "branching" below).
 *
 * Everything is big-endian, and every command and table entry is a whole
 * number of 16-bit words so the 68000 never reads a word at an odd address.
 *
 * A save stores a checkpoint -- the nth say, menu or click-to-continue
 * pause -- and a load replays the script up to it. So a save outlives any
 * rebuild that keeps the checkpoints: the header's save_sig hashes just
 * those (each one's kind, a say's French speaker and text, a menu's caption
 * and options), and staging -- images, positions, transitions, music,
 * English text -- is left out of it.
 *
 * Text is not in the script: commands name strings by number, and each
 * language has a file of them (.MWT, below), of which the ST holds one.
 */

#ifndef MWB_H
#define MWB_H

#define MWB_MAGIC   0x4D574231UL    /* "MWB1" */
#define MWB_VERSION 10

/* Stage geometry on the ST: the top 360 lines are the 1280x720 stage at half
 * scale, the 40 below it the text area (a name line and four text lines in
 * the 8x8 font). */
#define MWB_STAGE_W   640
#define MWB_STAGE_H   360
#define MWB_TEXT_COLS 78            /* one character of margin each side */
#define MWB_TEXT_ROWS 4

/* A say's `who` for Ren'Py's `centered` (00definitions.rpy:440): no name
 * and no text window, the lines centered on the stage (00style.rpy:317-
 * 328). The ST boxes them, for the dithered backdrops; more rows fit. */
#define MWB_WHO_CENTERED  0xFFFE
#define MWB_CENTER_ROWS   16

/* Header, at offset 0. All offsets are from the start of the file. */
typedef struct mwb_header {
    unsigned long  magic;
    unsigned short version;
    unsigned short nimg;            /* entries in the image table */
    unsigned short nchar;           /* entries in the character table */
    unsigned short nmus;            /* entries in the music table */
    unsigned short ntag;            /* distinct image tags: runtime slot ids */
    unsigned short pad;
    unsigned long  img_off;         /* mwb_image[nimg] */
    unsigned long  char_off;        /* unsigned short name_str[nchar] */
    unsigned long  mus_off;         /* mwb_music[nmus] */
    unsigned long  code_off;        /* the command stream */
    unsigned long  code_len;
    unsigned long  nstr;            /* strings in every .MWT */
    unsigned long  nlang;           /* languages: MWB_LANG_* */
    unsigned long  save_sig;        /* which saves are good for this build */
    unsigned long  var_off;         /* mwb_var[nvar] */
    unsigned long  nvar;
    unsigned long  expr_off;        /* unsigned short expr[expr_words] */
    unsigned long  expr_words;
    unsigned long  opt_off;         /* mwb_option[nopt] */
    unsigned long  nopt;
    unsigned long  vstr_off;        /* the value strings, see MWB_V_STR */
    unsigned long  nvstr;
} mwb_header;

/* Its size in the file. Not sizeof, which is the compiler's business. */
#define MWB_HEADER_SIZE 80

#define MWB_IMG_BG     0            /* opaque, 1bpp, 640 wide */
#define MWB_IMG_SPRITE 1            /* mask + data words */

/* An image as the ST loads it. `file` is an 8.3 GEMDOS name. */
typedef struct mwb_image {
    unsigned short w, h;            /* at half scale */
    unsigned short kind;            /* MWB_IMG_* */
    char           file[14];
} mwb_image;                        /* 20 bytes */

typedef struct mwb_music {
    char file[14];
    unsigned short pad;
} mwb_music;                        /* 16 bytes */

/* Commands. Each is an opcode word followed by its argument words; the
 * comments give the arguments in order. "str" is a string number, the same
 * in every language's .MWT. "none" for an index is 0xFFFF. */
enum {
    MWB_END = 0,        /* -- end of the game */
    MWB_SAY,            /* who, str: who is a character index, none, or
                         * MWB_WHO_CENTERED */
    MWB_SCENE,          /* img: clears every tag; img none is black */
    MWB_SHOW,           /* tag, img, x, y, at: takes the tag out of the
                         * list if it is there, then inserts it at index
                         * `at` -- its old place, the top, or behind
                         * another tag, all settled by the compiler */
    MWB_HIDE,           /* tag */
    MWB_WITH,           /* kind, a_ms, b_ms, c_ms: transition from what is on
                         * screen to the state built since the last WITH */
    MWB_TITLE,          /* str: the `title` screen, a centred card on black */
    MWB_UNTITLE,        /* -- */
    MWB_MUSIC,          /* mus, flags */
    MWB_STOPMUSIC,      /* -- */
    MWB_WINDOW,         /* shown: 0 hides the text box */
    MWB_PAUSE,          /* ms_hi, ms_lo, hard: ms 0 waits for a click */
    MWB_SCROLL,         /* tag, img, x, y0, at, y1, ms_hi, ms_lo: a SHOW at
                         * y0 that then moves linearly to y1 (the credits) */
    MWB_SKIPPING,       /* allowed: `$ _skipping = ...`, a fence the skip
                         * cannot cross, not a cancel of it */
    MWB_ROLLBACK,       /* enabled: `$ config.rollback_enabled = ...` */
    MWB_FENCE,          /* -- `renpy.block_rollback()`: no rolling back
                         * past here */
    MWB_IFLANG,         /* lang, words: `if _preferences.language == ...`;
                         * in any other language the next `words` words
                         * are skipped. Never holds a say or an untimed
                         * pause: checkpoints are the same in every
                         * language */
    MWB_GOTO,           /* pc: carry on at word offset pc */
    MWB_IF,             /* expr, pc: carry on at pc if expr is false */
    MWB_SET,            /* var, expr: a `$` assignment */
    MWB_MENU,           /* who, str, opt, nopt, pc: a choice menu, with
                         * options opt..opt+nopt-1 of the option table. str
                         * is its caption or none, said by who (a character
                         * or none) without waiting. If no option's
                         * condition holds, carry on at pc */
    MWB_RHYTHM,         /* mus, btm, bg, note, kind, a_ms, b_ms, c_ms:
                         * `call screen rhythm_game(...)`, the minigame,
                         * played to track mus with beatmap R<btm>.BTM over
                         * backdrop image bg; note is the first of 8 image
                         * ids, the notes left, up, down, right, then the
                         * same at 1.25x. kind..c_ms are the `with`, as
                         * MWB_WITH's, into its first frame. Leaves (hits,
                         * notes, score) for MWB_EX_RET. Live only: a
                         * replay passes over it */
    MWB_NCMD
};

/* Each command's length in words, opcode included, indexed by opcode. */
#define MWB_CMD_WORDS { 1, 3, 2, 6, 2, 5, 2, 1, 3, 1, 2, 4, 9, 2, 2, 1, 3, \
                        2, 3, 3, 6, 9 }

/* ---- branching ----------------------------------------------------------
 *
 * The display state -- backdrop, images, title card, music, window, the
 * skip and rollback flags -- is still a function of the position: where
 * paths that left the stage differently meet, the compiler compiles what
 * follows once per version of the stage until they agree again (usually
 * at the next `scene`). Only variables differ between two visits of the
 * same command.
 *
 * Variables: Ren'Py's store and persistent names the script assigns. A
 * value is a type and a 32-bit number: Python's ints are not bounded, and
 * the rhythm game's score passes 16 bits. */
#define MWB_V_NONE 0
#define MWB_V_BOOL 1
#define MWB_V_INT  2
#define MWB_V_STR  3                /* the number is a value string's */

/* The value strings: every string literal an expression names, apart from
 * the .MWT text because Ren'Py does not translate one -- `[name]` prints
 * the value as it is (substitutions.py:100; only `!t` translates, :282).
 * Atari charset, `{i}`/`{b}` as the inline codes, each text once, so two
 * are equal when their numbers are. At vstr_off:
 *
 *     nvstr x { u32 hash, u32 offset (from the file's start) }, then the
 *     NUL-terminated texts, padded to a word.
 *
 * The hash is FNV-1a of the text: what a save or the persistent file
 * keeps a string as, since a rebuild renumbers them. */
#define MWB_VSTR_MAX 70             /* characters in one */

typedef struct mwb_var {
    unsigned long  name;            /* FNV-1a of the name: what a persistent
                                     * value is kept under across builds */
    unsigned char  flags;           /* MWB_VAR_* */
    unsigned char  type;            /* the initial value */
    unsigned short pad;
    long           value;
} mwb_var;
#define MWB_VAR_SIZE       12       /* in the file */
#define MWB_VAR_PERSISTENT 1

/* Expressions, in reverse Polish, ending with EX_END. IF, SET and an
 * option's condition name one by its word offset in the expression table.
 * Python's semantics: `and`/`or` give an operand, comparisons a bool,
 * True == 1, a string equals only the same string and orders only against
 * a string (by its bytes: Python's code point order for the ASCII ones),
 * and "" is false. */
enum {
    MWB_EX_END = 0,
    MWB_EX_NONE, MWB_EX_FALSE, MWB_EX_TRUE,
    MWB_EX_INT,                     /* value: -32768..32767 */
    MWB_EX_VAR,                     /* var */
    MWB_EX_NOT,
    MWB_EX_EQ, MWB_EX_NE, MWB_EX_LT, MWB_EX_LE, MWB_EX_GT, MWB_EX_GE,
    MWB_EX_AND, MWB_EX_OR,
    MWB_EX_STR,                     /* value string */
    MWB_EX_RET,                     /* i: _return[i], an int, from the
                                     * last MWB_RHYTHM (0 before any) */
    MWB_EX_LONG                     /* hi, lo: an int past 16 bits */
};

/* ---- beatmaps ----------------------------------------------------------
 *
 * R<nn>.BTM, beside the script: a Ren'Py beatmap ("seconds[<TAB>track]" a
 * line) with its stride taken and its tracks settled by the compiler:
 *
 *     u32 key, u32 end, u16 count, u16 pad, u8 hit[4],
 *     then count x u32: (track << 30) | ms
 *
 * ms is the onset from the start of the tune, in order. key is the FNV-1a
 * of "persistent.rhythm_game_highscores[<beatmap file name>]": the song's
 * high score is kept in the persistent file under it, a 32-bit int. end is
 * where the tune ends, in ms: Ren'Py ends the run when the music stops,
 * and the SNDH conversions play on into noise, so the ST stops them
 * there. hit is each track's hit sound, a MIDI note (mwc.c, hit_notes). */
#define MWB_BTM_HEADER_SIZE 16
#define MWB_BTM_HIT         12      /* hit[4]'s offset */
#define MWB_BTM_TRACK_SHIFT 30
#define MWB_BTM_MS_MASK     0x3FFFFFFFUL

typedef struct mwb_option {
    unsigned short str;             /* the caption */
    unsigned short cond;            /* expr, or none: always offered */
    unsigned short pc;              /* where choosing it goes */
} mwb_option;                       /* 6 bytes */

/* The choice screen: a column of one-line buttons over the stage. */
#define MWB_MENU_MAXOPT 12          /* options in one menu */
#define MWB_OPT_COLS    70          /* characters in an option */

/* Languages: Ren'Py's `_preferences.language`, None being the script's own. */
#define MWB_LANG_FR 0               /* None */
#define MWB_LANG_EN 1               /* "english" */
#define MWB_NLANG   2

/* ---- text files --------------------------------------------------------
 *
 * TEXT_FR.MWT, TEXT_EN.MWT: every string the script names, in one language.
 *
 *     u32 magic, u16 nstr, u16 lang, u32 offset[nstr] (from the file's
 *     start), then the strings: NUL-terminated, Atari charset, say lines
 *     pre-wrapped for this language's words.
 */
#define MWB_MWT_MAGIC 0x4D575431UL  /* "MWT1" */

/* MWB_WITH kinds. The a/b/c words are what each one needs:
 * dissolve a = duration; fade a/b/c = out/hold/in; the moves a = duration. */
enum {
    MWB_T_NONE = 0,
    MWB_T_DISSOLVE,
    MWB_T_FADE,
    MWB_T_MOVE,
    MWB_T_MOVEINLEFT,
    MWB_T_MOVEINRIGHT,
    MWB_T_MOVEOUTLEFT,
    MWB_T_MOVEOUTRIGHT
};

#define MWB_MUS_LOOP      1
#define MWB_MUS_IFCHANGED 2         /* do nothing if this track is playing */

/* Inline codes in say text. Lines are pre-wrapped by the compiler and
 * separated by '\n' -- except a line that shows a variable, which only the
 * ST can wrap. */
#define MWB_TXT_ITALIC_ON  0x01
#define MWB_TXT_ITALIC_OFF 0x02
#define MWB_TXT_BOLD_ON    0x03
#define MWB_TXT_BOLD_OFF   0x04
#define MWB_TXT_FAST       0x05     /* {fast}: what is before it is not
                                     * typed (Ren'Py's textsupport) */
#define MWB_TXT_CODE_LAST  0x05     /* 1..this: codes, shown as nothing */
#define MWB_TXT_VAR        0x06     /* then MWB_TXT_VAR_BASE + var: `[var]` */
#define MWB_TXT_VAR_BASE   0x30
#define MWB_TXT_MAXVAR     (0x80 - MWB_TXT_VAR_BASE)

/* ---- image files ------------------------------------------------------
 *
 * Pixels are the ST's own: 1 is black, most significant bit leftmost, rows of
 * 16-bit words. They are stored uncompressed -- from a hard disk, reading the
 * raw bytes is faster than an 8MHz 68000 can decompress them.
 *
 * .MBG, a background: 360 rows of 80 bytes, exactly what goes into the top
 * of the screen. 28800 bytes, no header.
 *
 * .MSP, a sprite: a header, then the mask plane (unless MWB_SPR_OPAQUE),
 * then the data plane, each `wpr * h` words. A mask bit is 1 where the
 * sprite covers; data bits are only ever set inside the mask. Drawing is
 * two blits: screen &= ~mask, then screen |= data. An opaque sprite has no
 * mask plane and is a single copy.
 */
#define MWB_BG_BYTES (MWB_STAGE_W / 8 * MWB_STAGE_H)

typedef struct mwb_sprite {
    unsigned short w, h;
    unsigned short wpr;             /* words per row, (w + 15) / 16 */
    unsigned short flags;           /* MWB_SPR_* */
} mwb_sprite;
#define MWB_SPRITE_HEADER_SIZE 8
#define MWB_SPR_OPAQUE 1

/* ---- the pak -----------------------------------------------------------
 *
 * One file holding everything the game reads, so the ST opens one file and
 * seeks rather than going through GEMDOS's directory search per image:
 *
 *     u32 magic, u16 count, u16 pad, then `count` entries of
 *     char name[14] (8.3, NUL-padded), u32 offset, u32 size, u16 pad
 *
 * Entries are sorted by name, and every offset is even.
 */
#define MWB_PAK_MAGIC       0x4D57504BUL    /* "MWPK" */
#define MWB_PAK_HEADER_SIZE 8
#define MWB_PAK_ENTRY_SIZE  24

#endif
