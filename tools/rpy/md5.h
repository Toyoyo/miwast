/* md5.h - MD5, needed only to reproduce Ren'Py's translation identifiers.
 *
 * Ren'Py names each translatable block `<label>_<digest>`, where the digest is
 * the first eight hex characters of the MD5 of the statement's source form.
 * Looking a translation up therefore means computing the same digest, which is
 * the whole reason this file exists; nothing else in the engine hashes.
 */

#ifndef MR_MD5_H
#define MR_MD5_H

#include "mr.h"

typedef struct {
    mr_u32        state[4];
    mr_u32        lo, hi;      /* message length in bits, split for 32-bit */
    unsigned char buf[64];
    int           have;        /* bytes buffered */
} mr_md5;

void mr_md5_init(mr_md5 *c);
void mr_md5_update(mr_md5 *c, const void *data, long len);
void mr_md5_final(mr_md5 *c, unsigned char out[16]);

/* The convenience form the identifier code wants: the first `chars` hex digits
 * of the digest of `s`, NUL terminated. `out` needs chars+1 bytes. */
void mr_md5_hex(const char *s, long len, char *out, int chars);

#endif /* MR_MD5_H */
