/* md5.c - MD5 as specified in RFC 1321. See md5.h for why it is here.
 *
 * Written against the RFC's own description rather than adapted from an
 * implementation, so it is the plain textbook form: four rounds of sixteen
 * operations over a 16-word block. Speed does not matter -- it runs once per
 * say statement at load time, about three thousand times on a few dozen bytes
 * each -- so the round functions are left as written instead of unrolled.
 *
 * Everything is mr_u32, which is 32 bits on both the target and the host; the
 * masking after each addition is what keeps it so regardless.
 */

#include <string.h>

#include "md5.h"

#define F(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define G(x, y, z) (((x) & (z)) | ((y) & ~(z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | ~(z)))

#define ROT(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#define STEP(f, a, b, c, d, x, t, s)                       \
    do {                                                   \
        (a) += f((b), (c), (d)) + (x) + (mr_u32)(t);        \
        (a) = ROT((a), (s));                                \
        (a) += (b);                                         \
    } while (0)

void mr_md5_init(mr_md5 *c)
{
    c->state[0] = 0x67452301u;
    c->state[1] = 0xefcdab89u;
    c->state[2] = 0x98badcfeu;
    c->state[3] = 0x10325476u;
    c->lo = c->hi = 0;
    c->have = 0;
}

/* One 64-byte block. The message is read little-endian, byte by byte, rather
 * than cast to mr_u32 *, because the target's alignment rules and a byte
 * buffer do not otherwise agree. */
static void transform(mr_u32 *state, const unsigned char *p)
{
    mr_u32 x[16];
    mr_u32 a = state[0], b = state[1], c = state[2], d = state[3];
    int i;

    for (i = 0; i < 16; i++)
        x[i] = (mr_u32)p[i * 4] | ((mr_u32)p[i * 4 + 1] << 8) |
               ((mr_u32)p[i * 4 + 2] << 16) | ((mr_u32)p[i * 4 + 3] << 24);

    STEP(F, a, b, c, d, x[0],  0xd76aa478u,  7);
    STEP(F, d, a, b, c, x[1],  0xe8c7b756u, 12);
    STEP(F, c, d, a, b, x[2],  0x242070dbu, 17);
    STEP(F, b, c, d, a, x[3],  0xc1bdceeeu, 22);
    STEP(F, a, b, c, d, x[4],  0xf57c0fafu,  7);
    STEP(F, d, a, b, c, x[5],  0x4787c62au, 12);
    STEP(F, c, d, a, b, x[6],  0xa8304613u, 17);
    STEP(F, b, c, d, a, x[7],  0xfd469501u, 22);
    STEP(F, a, b, c, d, x[8],  0x698098d8u,  7);
    STEP(F, d, a, b, c, x[9],  0x8b44f7afu, 12);
    STEP(F, c, d, a, b, x[10], 0xffff5bb1u, 17);
    STEP(F, b, c, d, a, x[11], 0x895cd7beu, 22);
    STEP(F, a, b, c, d, x[12], 0x6b901122u,  7);
    STEP(F, d, a, b, c, x[13], 0xfd987193u, 12);
    STEP(F, c, d, a, b, x[14], 0xa679438eu, 17);
    STEP(F, b, c, d, a, x[15], 0x49b40821u, 22);

    STEP(G, a, b, c, d, x[1],  0xf61e2562u,  5);
    STEP(G, d, a, b, c, x[6],  0xc040b340u,  9);
    STEP(G, c, d, a, b, x[11], 0x265e5a51u, 14);
    STEP(G, b, c, d, a, x[0],  0xe9b6c7aau, 20);
    STEP(G, a, b, c, d, x[5],  0xd62f105du,  5);
    STEP(G, d, a, b, c, x[10], 0x02441453u,  9);
    STEP(G, c, d, a, b, x[15], 0xd8a1e681u, 14);
    STEP(G, b, c, d, a, x[4],  0xe7d3fbc8u, 20);
    STEP(G, a, b, c, d, x[9],  0x21e1cde6u,  5);
    STEP(G, d, a, b, c, x[14], 0xc33707d6u,  9);
    STEP(G, c, d, a, b, x[3],  0xf4d50d87u, 14);
    STEP(G, b, c, d, a, x[8],  0x455a14edu, 20);
    STEP(G, a, b, c, d, x[13], 0xa9e3e905u,  5);
    STEP(G, d, a, b, c, x[2],  0xfcefa3f8u,  9);
    STEP(G, c, d, a, b, x[7],  0x676f02d9u, 14);
    STEP(G, b, c, d, a, x[12], 0x8d2a4c8au, 20);

    STEP(H, a, b, c, d, x[5],  0xfffa3942u,  4);
    STEP(H, d, a, b, c, x[8],  0x8771f681u, 11);
    STEP(H, c, d, a, b, x[11], 0x6d9d6122u, 16);
    STEP(H, b, c, d, a, x[14], 0xfde5380cu, 23);
    STEP(H, a, b, c, d, x[1],  0xa4beea44u,  4);
    STEP(H, d, a, b, c, x[4],  0x4bdecfa9u, 11);
    STEP(H, c, d, a, b, x[7],  0xf6bb4b60u, 16);
    STEP(H, b, c, d, a, x[10], 0xbebfbc70u, 23);
    STEP(H, a, b, c, d, x[13], 0x289b7ec6u,  4);
    STEP(H, d, a, b, c, x[0],  0xeaa127fau, 11);
    STEP(H, c, d, a, b, x[3],  0xd4ef3085u, 16);
    STEP(H, b, c, d, a, x[6],  0x04881d05u, 23);
    STEP(H, a, b, c, d, x[9],  0xd9d4d039u,  4);
    STEP(H, d, a, b, c, x[12], 0xe6db99e5u, 11);
    STEP(H, c, d, a, b, x[15], 0x1fa27cf8u, 16);
    STEP(H, b, c, d, a, x[2],  0xc4ac5665u, 23);

    STEP(I, a, b, c, d, x[0],  0xf4292244u,  6);
    STEP(I, d, a, b, c, x[7],  0x432aff97u, 10);
    STEP(I, c, d, a, b, x[14], 0xab9423a7u, 15);
    STEP(I, b, c, d, a, x[5],  0xfc93a039u, 21);
    STEP(I, a, b, c, d, x[12], 0x655b59c3u,  6);
    STEP(I, d, a, b, c, x[3],  0x8f0ccc92u, 10);
    STEP(I, c, d, a, b, x[10], 0xffeff47du, 15);
    STEP(I, b, c, d, a, x[1],  0x85845dd1u, 21);
    STEP(I, a, b, c, d, x[8],  0x6fa87e4fu,  6);
    STEP(I, d, a, b, c, x[15], 0xfe2ce6e0u, 10);
    STEP(I, c, d, a, b, x[6],  0xa3014314u, 15);
    STEP(I, b, c, d, a, x[13], 0x4e0811a1u, 21);
    STEP(I, a, b, c, d, x[4],  0xf7537e82u,  6);
    STEP(I, d, a, b, c, x[11], 0xbd3af235u, 10);
    STEP(I, c, d, a, b, x[2],  0x2ad7d2bbu, 15);
    STEP(I, b, c, d, a, x[9],  0xeb86d391u, 21);

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

void mr_md5_update(mr_md5 *c, const void *data, long len)
{
    const unsigned char *p = (const unsigned char *)data;
    mr_u32 n = (mr_u32)len;

    /* Byte count, kept as a 64-bit value in two halves; it becomes a bit
     * count only in mr_md5_final. */
    c->lo += n;
    if (c->lo < n)
        c->hi++;

    while (len > 0) {
        int room = 64 - c->have;
        int take = (len < room) ? (int)len : room;

        memcpy(c->buf + c->have, p, (size_t)take);
        c->have += take;
        p += take;
        len -= take;

        if (c->have == 64) {
            transform(c->state, c->buf);
            c->have = 0;
        }
    }
}

void mr_md5_final(mr_md5 *c, unsigned char out[16])
{
    unsigned char len[8];
    unsigned char pad = 0x80;
    mr_u32 blo = c->lo << 3;
    mr_u32 bhi = (c->hi << 3) | (c->lo >> 29);
    int i;

    for (i = 0; i < 4; i++) {
        len[i]     = (unsigned char)(blo >> (i * 8));
        len[i + 4] = (unsigned char)(bhi >> (i * 8));
    }

    /* 0x80, then zeros until eight bytes short of a block, then the length. */
    mr_md5_update(c, &pad, 1);
    while (c->have != 56) {
        unsigned char zero = 0;
        mr_md5_update(c, &zero, 1);
    }
    /* The length is appended directly: routing it through update would count
     * its own bits. */
    memcpy(c->buf + 56, len, 8);
    transform(c->state, c->buf);
    c->have = 0;

    for (i = 0; i < 16; i++)
        out[i] = (unsigned char)(c->state[i / 4] >> ((i % 4) * 8));
}

void mr_md5_hex(const char *s, long len, char *out, int chars)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char d[16];
    mr_md5 c;
    int i;

    if (chars > 32)
        chars = 32;

    mr_md5_init(&c);
    mr_md5_update(&c, s, len);
    mr_md5_final(&c, d);

    for (i = 0; i < chars; i++)
        out[i] = hex[(i & 1) ? (d[i / 2] & 0xF) : (d[i / 2] >> 4)];
    out[chars] = '\0';
}
