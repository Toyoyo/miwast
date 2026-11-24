/* musicroom.h - MusicRoom, the object behind the jukebox screen.
 *
 * `00musicroom.rpy` defines MusicRoom as a Python class the game builds in
 * `init python` and stores in a screen variable, then calls methods on from
 * button actions: `mr.Play(file)`, `mr.Next()`, `mr.Previous()`. There is no
 * Python here, so the class lives in C and a room is referred to by a handle
 * that fits in an mr_value string -- which is what lets it be stored in a
 * screen variable and captured into a button's scope like any other value.
 */

#ifndef MR_MUSICROOM_H
#define MR_MUSICROOM_H

#include "mr.h"
#include "script.h"    /* mr_channel */
#include "expr.h"

/* Create a room, returning its id. Ids come from a small pool that wraps. */
int  mr_room_new(mr_channel ch, long fadeout_ms, long fadein_ms,
                 int loop, int single_track);

/* Write the handle for `id` into a value, so the script can hold onto it. */
void mr_room_handle(int id, mr_value *out);

/* The id a handle refers to, or -1 if the value is not one. */
int  mr_room_from_value(const mr_value *v);

void mr_room_add(int id, const char *file, int always_unlocked);

/* Ren'Py's play(filename, offset): start from `file` (or from whatever is
 * playing, or the first track), then step `offset` places along the unlocked
 * playlist. */
void mr_room_play(int id, const char *file, int offset);
void mr_room_next(int id);
void mr_room_previous(int id);
void mr_room_stop(int id);

int  mr_room_is_unlocked(int id, const char *file);

/* Is this the track the room's channel is on? `Play(file)` reports itself as
 * selected when it is, which is what draws the playing track in the selected
 * colour rather than the idle one. */
int  mr_room_is_playing(int id, const char *file);

/* Ren'Py unlocks a track once the player has heard it (renpy.seen_audio).
 * The interpreter reports every track it starts here. */
void mr_room_note_played(const char *file);

/* Forget every room. The rooms are rebuilt each time the jukebox opens, so
 * without this they would accumulate. */
void mr_room_reset(void);

#endif /* MR_MUSICROOM_H */
