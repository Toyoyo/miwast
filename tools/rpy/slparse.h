#ifndef MR_SLPARSE_H
#define MR_SLPARSE_H

#include "lex.h"
#include "slang.h"

/* Parse a `screen ...:` block starting at l->v[*idx]. On success fills *out
 * and advances *idx past the block. Returns 0 if the line is not a screen. */
int mr_sl_parse_screen(mr_lines *l, int *idx, int indent, sl_screen *out);

#endif
