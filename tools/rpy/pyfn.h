/* pyfn.h - the functions a script defines in `init python:`.
 *
 * Ren'Py screens call into Python freely, and this game does it in three
 * places: `default mr = build_music_room()`, `default scores =
 * build_rhythm_highscores()`, and the two actions Confirm() is handed. The
 * bodies involved are a handful of assignments, calls, ifs and a return -- no
 * loops, no comprehensions, no classes -- which is little enough to run
 * directly rather than skipping the block and hardcoding the result.
 *
 * Anything outside that subset is logged and skipped, on the same principle
 * as the rest of the parser: an unrecognised line costs the feature it
 * belongs to, not the game.
 */

#ifndef MR_PYFN_H
#define MR_PYFN_H

#include "expr.h"

/* Forget every definition. Called when a script is loaded. */
void mr_pyfn_reset(void);

/* Define a function. The parser calls begin, then line for each line of the
 * body in order, with the body's own indent column. */
void mr_pyfn_begin(const char *name);
void mr_pyfn_line(const char *text, int indent);

int  mr_pyfn_exists(const char *name);

/* A line of a function's body, or NULL past the end. For a builtin that
 * shadows a script function and still wants to honour part of what the
 * script wrote in it. */
const char *mr_pyfn_body_line(const char *name, int i);

/* Call it. Returns 1 if the function exists and ran, with its return value in
 * *result (None if it fell off the end). */
int  mr_pyfn_call(const char *name, mr_value *result);

#endif /* MR_PYFN_H */
