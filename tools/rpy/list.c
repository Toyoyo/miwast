/* list.c - the list pool. See list.h. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "mr.h"
#include "list.h"

typedef struct {
    int      used;
    int      n;
    mr_value v[MR_LIST_ITEMS];
} mr_listobj;

static mr_listobj g_list[MR_LIST_POOL];
static int        g_next;

/* How hard the pool is actually worked, for memreport. The size of the pool
 * and the cost of clearing a slot only matter in proportion to how many
 * lists a session creates, and that was guessed at twice before it was
 * counted. `g_hi_slot` is how much of the 2.03MB a run ever reaches. */
static long g_created;
static int  g_hi_slot;

void mr_list_stats(long *created, int *slots_reached, int *pool,
                   long *slot_bytes)
{
    *created = g_created;
    *slots_reached = g_hi_slot;
    *pool = MR_LIST_POOL;
    *slot_bytes = (long)sizeof(g_list[0]);
}

/* "\001L<n>". The \001 cannot appear in anything the parser produces, so a
 * handle is never mistaken for an ordinary string. */
#define LIST_TAG '\001'

/* Clearing the headers, not the pool.
 *
 * g_list is 2.03MB -- 32 slots of 64 mr_values, and an mr_value is 1040 bytes
 * because MR_VSTR_MAX is 1024. A memset over the whole thing writes every one
 * of those pages, and a page that has been written is in the process's
 * working set for good. Windows' "active private working set" is what the
 * memory figure on the target is read from, so this one line was worth 2MB of
 * it. Touching `used` and `n` per slot touches 32 pages instead of 496.
 *
 * Nothing is lost by not clearing `v`: the two places that reach it are the
 * read at mr_list_get, guarded by `i >= n`, and the append below, which
 * overwrites the whole value. Stale bytes past `n` are unreachable. */
void mr_list_reset(void)
{
    int i;

    for (i = 0; i < MR_LIST_POOL; i++) {
        g_list[i].used = 0;
        g_list[i].n = 0;
    }
    g_next = 0;
}

const char *mr_list_new(void)
{
    static char handle[16];
    int id = g_next;

    g_next = (g_next + 1) % MR_LIST_POOL;

    /* Was a memset of the whole slot: 66,560 bytes, per list created, and
     * the callers are expression sites (expr.c:614, :671, :725) that run
     * while a screen draws. So it was both a per-frame cost on the PIII and
     * the thing that pulled all 2.03MB resident within the first 32 lists.
     * The appended values arrive fully initialised -- mr_value_set_* memset
     * their own argument -- so only the header needs resetting. */
    g_list[id].n = 0;
    g_list[id].used = 1;
    g_created++;
    if (id + 1 > g_hi_slot)
        g_hi_slot = id + 1;

    sprintf(handle, "%cL%d", LIST_TAG, id);
    return handle;
}

int mr_list_id_str(const char *s)
{
    int id;

    if (!s || s[0] != LIST_TAG || s[1] != 'L')
        return -1;
    id = atoi(s + 2);
    if (id < 0 || id >= MR_LIST_POOL || !g_list[id].used)
        return -1;
    return id;
}

int mr_list_id(const mr_value *v)
{
    if (!v || v->type != MR_V_STR)
        return -1;
    return mr_list_id_str(v->s);
}

int mr_list_len(int id)
{
    if (id < 0 || id >= MR_LIST_POOL || !g_list[id].used)
        return 0;
    return g_list[id].n;
}

int mr_list_get(int id, int i, mr_value *out)
{
    if (out)
        memset(out, 0, sizeof(*out));
    if (id < 0 || id >= MR_LIST_POOL || !g_list[id].used)
        return 0;
    if (i < 0 || i >= g_list[id].n)
        return 0;
    if (out)
        *out = g_list[id].v[i];
    return 1;
}

int mr_list_append(int id, const mr_value *v)
{
    if (id < 0 || id >= MR_LIST_POOL || !g_list[id].used || !v)
        return 0;
    if (g_list[id].n >= MR_LIST_ITEMS) {
        mr_log("list: %d is full at %d items, dropping one",
               id, MR_LIST_ITEMS);
        return 0;
    }
    g_list[id].v[g_list[id].n++] = *v;
    return 1;
}
