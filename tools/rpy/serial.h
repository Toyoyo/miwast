/* serial.h - the primitives a save file is written with.
 *
 * Everything is explicit little-endian, field by field, never a struct dump:
 * the host harnesses are 64-bit and the target is 32, and a save written by
 * one has to be readable by the other. Doubles go out as 16.16 fixed point
 * for the same reason -- the values involved are volumes, text speeds and
 * alignment fractions, none of which need more.
 */

#ifndef MR_SERIAL_H
#define MR_SERIAL_H

#include <stdio.h>
#include "mr.h"
#include "script.h"

void   mr_put32(FILE *f, mr_u32 v);
mr_u32 mr_get32(FILE *f);

void   mr_put_fixed(FILE *f, double v);
double mr_get_fixed(FILE *f);

/* Fixed-width string: exactly n bytes, padded with zeros. */
void mr_put_str(FILE *f, const char *s, int n);
void mr_get_str(FILE *f, char *s, int n);

/* Length-prefixed string, for text whose typical length is far below its
 * maximum -- history lines especially, where fixed width would multiply a
 * save's size several times over. */
void mr_put_vstr(FILE *f, const char *s);
void mr_get_vstr(FILE *f, char *s, int max);

/* A repeat count read from a file, clamped to what the reader can actually
 * hold. A count is the one field that must never be taken on trust: every
 * other kind of corruption costs a wrong value, while a corrupt count costs
 * a loop that does not end. `what` names the section for the log. */
long mr_get_count(FILE *f, long max, const char *what);

void mr_put_place(FILE *f, const mr_place *p);
void mr_get_place(FILE *f, mr_place *p);

/* As above, with alpha and the zooms. See the comment on mr_put_place_full:
 * the short form cannot be widened without moving fields in existing saves,
 * so new sections use this one. */
void mr_put_place_full(FILE *f, const mr_place *p);
void mr_get_place_full(FILE *f, mr_place *p);

/* A transform block, by value. */
void mr_put_atl(FILE *f, const mr_atl *a);
void mr_get_atl(FILE *f, mr_atl *a);

#endif /* MR_SERIAL_H */
