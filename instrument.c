/* instrument.c - test-build block-ownership table and mutation trace.
 *
 * See instrument.h.  Everything here is compiled only when FILSYS_INSTRUMENT
 * is defined; the no-op stubs in the header make the hooks free otherwise.
 *
 * SPDX-License-Identifier: ISC
 */
#include <config.h>

#ifdef FILSYS_INSTRUMENT

#include "instrument.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A block not yet assigned to an inode (just handed out by the allocator). */
#define INO_ALLOC 0xffffffffu

typedef struct {
    uint32_t ino;    /* 0 free, INO_ALLOC allocated-unassigned, else owner */
    uint32_t lbn;    /* logical block within the owner (0 when unassigned) */
    uint8_t  kind;   /* 0 free, 1 data, 2 indirect, 3 allocator */
} block_owner_t;

static block_owner_t *g_owner;
static uint32_t g_nblk;

/* A ring of sequence-numbered trace entries.  The seq is monotonic; the ring
 * keeps the most recent TRACE_MAX events, which is what a debugging session
 * cares about when it fails. */
enum { TRACE_MAX = 4096 };
typedef struct {
    uint64_t seq;
    char     what[64];
} trace_ent_t;
static trace_ent_t g_trace[TRACE_MAX];
static uint64_t g_seq;
static int g_violations;

void filsys_instr_reset(uint32_t nblk) {
    free(g_owner);
    g_owner = calloc(nblk, sizeof *g_owner);
    g_nblk = nblk;
    g_seq = 0;
    g_violations = 0;
    memset(g_trace, 0, sizeof g_trace);
}

static void trace(const char *fmt, ...) {
    trace_ent_t *e = &g_trace[g_seq % TRACE_MAX];
    e->seq = g_seq;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->what, sizeof e->what, fmt, ap);
    va_end(ap);
    g_seq++;
}

void filsys_instr_dump(void) {
    uint64_t first = g_seq > TRACE_MAX ? g_seq - TRACE_MAX : 0;
    fprintf(stderr, "INSTRUMENT: mutation trace (last %" PRIu64 " events):\n",
            g_seq < TRACE_MAX ? g_seq : (uint64_t)TRACE_MAX);
    for (uint64_t i = first; i < g_seq; i++)
        fprintf(stderr, "  %6" PRIu64 " %s\n", g_trace[i % TRACE_MAX].seq,
                g_trace[i % TRACE_MAX].what);
}

static void owner_violation(const char *what, uint32_t bno) {
    const block_owner_t *o = &g_owner[bno];
    const char *state = o->ino == 0 ? "free"
                      : o->ino == INO_ALLOC ? "allocated-unassigned"
                      : "owned";
    fprintf(stderr, "INSTRUMENT: %s block %u: found %s (ino=%u lbn=%u kind=%u)\n",
            what, bno, state, o->ino, o->lbn, o->kind);
    g_violations++;
    filsys_instr_dump();
}

void filsys_instr_balloc(uint32_t bno) {
    if (bno >= g_nblk)
        return;   /* out of the data area: the allocator already rejects it */
    if (g_owner[bno].ino != 0)
        owner_violation("ALLOC_BLOCK (claimed twice)", bno);
    g_owner[bno].ino = INO_ALLOC;
    g_owner[bno].lbn = 0;
    g_owner[bno].kind = 3;
    trace("ALLOC_BLOCK %u", bno);
}

void filsys_instr_bfree(uint32_t bno) {
    if (bno >= g_nblk)
        return;
    if (g_owner[bno].ino != INO_ALLOC)
        owner_violation(g_owner[bno].ino == 0
                        ? "FREE_BLOCK (already free)"
                        : "FREE_BLOCK (still owned by a live inode)", bno);
    g_owner[bno].ino = 0;
    g_owner[bno].lbn = 0;
    g_owner[bno].kind = 0;
    trace("FREE_BLOCK %u", bno);
}

void filsys_instr_assign(uint32_t ino, uint32_t lbn, uint32_t bno, int kind) {
    if (bno >= g_nblk)
        return;
    if (g_owner[bno].ino != INO_ALLOC)
        owner_violation("ASSIGN (block not allocated-unassigned)", bno);
    g_owner[bno].ino = ino;
    g_owner[bno].lbn = lbn;
    g_owner[bno].kind = (uint8_t)kind;
    trace("ASSIGN ino=%u lbn=%u blk=%u kind=%d", ino, lbn, bno, kind);
}

void filsys_instr_release(uint32_t bno) {
    if (bno >= g_nblk)
        return;
    if (g_owner[bno].ino != 0 && g_owner[bno].ino != INO_ALLOC)
        g_owner[bno].ino = INO_ALLOC;   /* owned -> released (inode persisted) */
    trace("RELEASE_BLOCK %u", bno);
}

void filsys_instr_ialloc(uint32_t ino) {
    trace("ALLOC_INODE %u", ino);
}

void filsys_instr_ifree(uint32_t ino) {
    trace("FREE_INODE %u", ino);
}

void filsys_instr_dir_add(uint32_t parent, const char *name, uint32_t ino) {
    trace("DIR_ADD parent=%u \"%s\" ino=%u", parent, name, ino);
}

void filsys_instr_dir_remove(uint32_t parent, const char *name) {
    trace("DIR_REMOVE parent=%u \"%s\"", parent, name);
}

void filsys_instr_link(uint32_t ino, uint32_t from, uint32_t to) {
    trace("LINK ino=%u %u->%u", ino, from, to);
}

void filsys_instr_truncate(uint32_t ino) {
    trace("TRUNCATE ino=%u", ino);
}

int filsys_instr_violations(void) {
    return g_violations;
}

#endif /* FILSYS_INSTRUMENT */
