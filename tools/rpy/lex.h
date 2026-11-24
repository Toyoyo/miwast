/* lex.h - turning a .rpy file into logical lines.
 *
 * Ren'Py's syntax is line and indentation oriented, so the first pass reduces a
 * file to a list of (indent, text) pairs with comments removed and bracketed
 * continuations joined. The parser then only ever looks at one logical line at
 * a time plus the indent column, which is enough to recover the block
 * structure.
 */

#ifndef MR_LEX_H
#define MR_LEX_H

typedef struct mr_line {
    char *text;    /* comment-stripped, trailing whitespace removed */
    int   indent;  /* leading space columns, tabs counted as 8 */
    int   line;    /* 1-based source line of the first physical line */
} mr_line;

typedef struct mr_lines {
    mr_line *v;
    int      n;
    int      cap;
    char    *buf;  /* owns all the text the mr_line entries point into */
} mr_lines;

mr_lines *mr_lex_file(const char *path);
void      mr_lex_free(mr_lines *l);

/* --- token helpers shared by the parser --- */

/* Skip spaces and tabs. */
const char *mr_skip_ws(const char *p);

/* Copy the next whitespace-delimited word into out. Returns the position just
 * past it, or p unchanged if there is no word. */
const char *mr_next_word(const char *p, char *out, int outsz);

/* Match a bare word at p. Returns the position after it (and after following
 * whitespace) if it matches, else NULL. */
const char *mr_match_word(const char *p, const char *word);

/* The opening quote of a string literal at p, skipping whitespace and any
 * Python string prefix, or NULL if there is no string there. *raw is set when
 * the prefix contains r or R, the one prefix that changes what the literal
 * means; pass NULL if that does not matter.
 *
 * Needed because Ren'Py scripts do write the prefix: Hinata's gui.rpy spells
 * every colour u'#c184ff'. Read as an expression, that is the name `u`
 * followed by a string -- so the define came out as an unset variable. */
const char *mr_string_quote(const char *p, int *raw);

/* Read a quoted string, resolving Ren'Py's backslash escapes, into out. The
 * result stays UTF-8. Returns the position after the closing quote, or NULL.
 * Accepts a prefix, as mr_string_quote does. */
const char *mr_read_string(const char *p, char *out, int outsz);

/* Read a bare or quoted image/label name up to a terminator keyword such as
 * `at`, `with`, `behind`, `as`, or end of line. Multi-word names like
 * "miwa biwa talk" are kept whole with single spaces. */
const char *mr_read_imagename(const char *p, char *out, int outsz);

/* Copy one argument out of a call's argument list, stopping at a comma that
 * is not inside brackets or a string, or at the closing paren. Returns the
 * position after it, past the comma. For constructs whose arguments must be
 * kept as source -- Confirm's two actions, a Frame's image list -- rather
 * than evaluated. */
const char *mr_read_arg(const char *p, char *out, int outsz);

/* Parse a number, integer or float. */
const char *mr_read_number(const char *p, double *out);

#endif /* MR_LEX_H */
