/* list.h - list values for the small expression evaluator.
 *
 * The scripts build lists in `init python:` and screens iterate them:
 *
 *     songs = [("Chichi no Ito", "audio/miwa-chichi_no_ito.beatmap.txt"), ...]
 *     if persistent.watashi:
 *         songs.append(("Watashi no Oto", "audio/watashi-no-oto.beatmap.txt"))
 *     return [(title, rhythm_game_get_highscore(b)) for title, b in songs]
 *
 * and then `for title, score in scores:`. That is the extras menu's high
 * score table, and none of it ran before: `mr_value` is a scalar.
 *
 * Rather than widen mr_value -- which is copied into save slots and the
 * persistent file by value, and is the wrong place to grow a pointer -- a
 * list lives in a fixed pool here and is referred to by a handle string that
 * fits in one. Same device as the music rooms: `\001L3`. A handle can
 * therefore sit in a variable, a screen variable, a function local or a
 * button's captured scope without any of those knowing what it is.
 *
 * Lists are one-dimensional and their items are ordinary values -- including
 * other handles, which is what makes a list of tuples work: `("a", 1)` is
 * itself a two-item list. Unpacking `for title, score in ...` binds the
 * targets to the elements of each item.
 *
 * The pool wraps when it is exhausted, exactly as the music rooms do. A
 * screen rebuilds its lists every time it opens, so the live set is small;
 * what the pool must not do is fail an allocation and leave a screen with
 * nothing to iterate.
 */

#ifndef MR_LIST_H
#define MR_LIST_H

#include "expr.h"

#define MR_LIST_POOL   32     /* lists alive at once */
#define MR_LIST_ITEMS  64     /* items in one list */

/* Allocate an empty list. Returns its handle, valid until the pool wraps
 * around to it again. Never NULL. */
const char *mr_list_new(void);

/* The list a value refers to, or -1 if it is not a list handle. */
int mr_list_id(const mr_value *v);

/* As above, straight from a string. */
int mr_list_id_str(const char *s);

int mr_list_len(int id);
int mr_list_get(int id, int i, mr_value *out);
int mr_list_append(int id, const mr_value *v);

/* Drop every list. Called when the script is reloaded. */
void mr_list_reset(void);

/* How many lists a run has created, the highest slot it reached, the pool
 * size and the size of one slot. For memreport: the pool is 2.03MB of BSS
 * and whether that matters depends entirely on these numbers. */
void mr_list_stats(long *created, int *slots_reached, int *pool,
                   long *slot_bytes);

#endif /* MR_LIST_H */
