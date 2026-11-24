/* stmtid.h - portable names for statements, so a save survives script edits.
 *
 * A position in the story is a statement index (see script.h), and an index is
 * only meaningful against the exact script that produced it. Writing one into
 * a save is therefore a trap: edit a .rpy -- or change the parser, which is
 * how this was found -- and every index after the edit shifts. The save still
 * loads, because the header version only describes the file layout, and the
 * line of dialogue still displays correctly, because a save stores the text
 * verbatim. But the statement it resumes at is some other statement, so the
 * next line and the rollback history belong to a different part of the story.
 *
 * So a save stores names rather than indices, and they are resolved against
 * whatever script is loaded now.
 *
 * Ren'Py's own answer is different: every statement gets a (filename, version,
 * serial) name at parse time, persisted in the .rpyc, and a recompile runs
 * difflib over the old and new statement lists to carry names across an edit
 * (renpy/script.py, assign_names and merge_names). That needs a compiled-script
 * cache we do not have -- we reparse from source every launch -- and a sequence
 * matcher. What the diff-matching actually buys is identity for statements that
 * still match, which is what content addressing gives directly:
 *
 *   T<tlid>          a say statement, named by its translation identifier --
 *                    `<label>_<md5 of the source form>`, Ren'Py's own scheme
 *                    (tl.h). Survives any edit that leaves the line itself
 *                    alone, wherever it is in the file.
 *   T<tlid>+<n>      n statements past that say, for a small n. This is the
 *                    common case for a save: the point a save resumes at is
 *                    the statement *after* the line the player is looking at
 *                    (mr_interp_capture takes g_pc, not the say), and that is
 *                    often not a say itself. Anchoring it to the say keeps it
 *                    as robust as the say is, where the label form below
 *                    would break on any insertion earlier in the label.
 *   L<label>+<n>     anything else: the enclosing label and how many
 *                    statements past it. Survives edits outside that label,
 *                    and is the weakest of the three -- it is what the call
 *                    stack's return addresses get.
 *   $end             one past the last statement.
 *
 * A name that does not resolve returns -1, and the caller rejects the save
 * rather than guessing. That is better than Ren'Py, which does not check: a
 * missing name reaches Script.lookup and raises LabelNotFound
 * (renpy/script.py:1218), and nothing on the load path handles it.
 */

#ifndef MR_STMTID_H
#define MR_STMTID_H

#include "script.h"

/* Longest name: 'T' + a tlid (MR_MAX_NAME + 24) + "+nn" + NUL. */
#define MR_STMTID_MAX 96

/* How far past a say a statement may be and still be named relative to it. */
#define MR_STMTID_NEAR 3

/* Bind the script that names are relative to. Called once, from
 * mr_interp_init. */
void mr_stmtid_init(const mr_script *s);

/* Name `pc`. Always produces something; an out-of-range or unanchored
 * statement gets a name that deliberately fails to resolve, so a save holding
 * one is rejected rather than misread. */
void mr_stmtid_name(long pc, char *out, int outsz);

/* The statement `name` refers to in the script now, or -1.
 *
 * Resolving a say is a binary search over an index of translation identifiers,
 * built on first use. Release it with mr_stmtid_done() once a load has
 * finished: it is tens of kilobytes, it is only wanted for the ~30 names one
 * save contains, and on the target that memory is worth more than the rebuild.
 */
long mr_stmtid_resolve(const char *name);

void mr_stmtid_done(void);

#endif /* MR_STMTID_H */
