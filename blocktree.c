/* blocktree.c - the shared block-map tree walk and truncate path.
 *
 * Every edition's inode addresses resolve through the same two things: the
 * slot topology (filsys_slot_level in v7fs.h -- ndaddr direct slots, or the
 * ILARG large-file reinterpretation) and the indirect-entry codec (the
 * descriptor's ind_get/ind_put, defaulting to v7_ind_get).  This file is the
 * single place that walks a file's blocks -- to mark them (fsck), to free them
 * (truncate), or to free only a tail (truncate past a byte offset) -- so the
 * per-backend mark_tree/tloop/itrunc bodies collapse into it.
 *
 * SPDX-License-Identifier: ISC
 */
#include <config.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "v7fs.h"
#include "filsys_ops.h"

/* The indirect-entry codec: the descriptor's override (PDP-7's 18-bit word) or
 * the daddr_wid-based default (2-byte V6/V1, 4-byte V7/BSD). */
static inline uint32_t ind_get(const filsys_edition_t *fs, const uint8_t *buf, uint32_t i) {
    return fs->ind_get ? fs->ind_get(fs, buf, i) : v7_ind_get(fs, buf, i);
}
static inline void ind_put(const filsys_edition_t *fs, uint8_t *buf, uint32_t i, uint32_t v) {
    if (fs->ind_put) fs->ind_put(fs, buf, i, v);
    else v7_ind_put(fs, buf, i, v);
}

/* True if the inode can carry data blocks (regular/dir/symlink), false for a
 * device inode whose addr[0] is a device number rather than a block. */
static int is_data_inode(const filsys_edition_t *fs, const filsys_inode_t *ip) {
    if (fs->is_device)                      /* V1/PDP-7: device by inode/flag */
        return !fs->is_device(fs, ip);
    uint32_t t = ip->mode & fs->ifmt;
    return t == fs->ifreg || t == fs->ifdir || (fs->iflnk && t == fs->iflnk);
}

/* Mark one data block as accounted-for.  Returns 0 on a first claim, 1 for an
 * out-of-range (bad) block, 2 for a duplicate (already claimed).  Inode 1 is
 * the V7 bad-block inode (fs->badino): its addresses record gone-bad i-list
 * blocks, so a reference into the i-list region is a record, not an error. */
int filsys_mark_block(filsys_edition_t *fs, filsys_chkctx_t *cx, uint32_t bno)
{
    if (bno == 0)
        return 0;
    uint32_t dstart = fs->ops->data_start(fs);
    uint32_t dend   = fs->ops->data_end(fs);
    if (bno < dstart || bno >= dend) {
        if (fs->badino && cx->ino == fs->badino && bno >= 2 && bno < dstart)
            return 0;   /* bad-block record: a gone-bad i-list block */
        printf("block %u bad; inode=%u\n", bno, cx->ino);
        cx->bad_blocks++;
        cx->errors++;
        return 1;
    }
    uint32_t d = bno - dstart;
    uint8_t  m = (uint8_t)(1u << (d & 7));
    if (cx->bmap[d >> 3] & m) {
        if (cx->owner)
            printf("block %u dup; inode=%u (owner %u)\n", bno, cx->ino, cx->owner[d]);
        else
            printf("block %u dup; inode=%u\n", bno, cx->ino);
        cx->dup_blocks++;
        cx->errors++;
        return 2;
    }
    cx->bmap[d >> 3] |= m;
    if (cx->owner)
        cx->owner[d] = cx->ino;
    cx->used_blocks++;
    return 0;
}

/* Mark an indirect block and everything beneath it.  level 0 = single indirect,
 * 1 = double, 2 = triple.  A duplicate indirect block is not chased: its
 * children were already claimed by the first reference, so re-walking them
 * would only cascade spurious "dup" reports (BSD fsck's phase 1). */
void filsys_mark_tree(filsys_edition_t *fs, filsys_chkctx_t *cx, uint32_t blk, int level)
{
    if (blk == 0)
        return;
    if (filsys_mark_block(fs, cx, blk) != 0)
        return;                       /* bad or duplicate: don't chase it */
    uint8_t buf[V7_MAXBSIZE];
    if (fs->ops->read_block(fs, blk, buf)) {
        printf("cannot read indirect block %u\n", blk);
        cx->errors++;
        return;
    }
    for (uint32_t i = 0; i < fs->nindir; i++) {
        uint32_t nb = ind_get(fs, buf, i);
        if (nb == 0)
            continue;
        if (level > 0)
            filsys_mark_tree(fs, cx, nb, level - 1);
        else
            filsys_mark_block(fs, cx, nb);
    }
}

/* Mark every block referenced by an inode. */
void filsys_mark_blocks(filsys_edition_t *fs, const filsys_inode_t *ip, uint32_t ino,
                        filsys_chkctx_t *cx)
{
    (void)ino;   /* the driver set cx->ino */
    for (int i = 0; i < fs->niaddr; i++) {
        uint32_t a = ip->addr[i];
        if (a == 0)
            continue;
        int level = filsys_slot_level(fs, ip->mode, i);
        if (level < 0)
            filsys_mark_block(fs, cx, a);
        else
            filsys_mark_tree(fs, cx, a, level);
    }
}

/* A growable list of block numbers, so a truncate can *collect* the blocks it
 * removes instead of freeing them inline.  The caller persists the unlinked
 * inode (write_inode) first, then filsys_blklist_drain frees the blocks; this
 * closes the window where a block sits on the free list *and* is still
 * referenced by the on-disk inode (aliasing, which nothing repairs).  On a
 * failed truncate the caller discards the list instead, so the blocks leak --
 * recoverable by fsck -- rather than being freed while the inode still points
 * at them. */
struct filsys_blklist {
    uint32_t *blk;
    size_t n, cap;
};

filsys_blklist_t *filsys_blklist_new(void)
{
    filsys_blklist_t *b = malloc(sizeof *b);
    if (b) {
        b->blk = NULL;
        b->n = b->cap = 0;
    }
    return b;
}

static int blklist_push(filsys_blklist_t *b, uint32_t bno)
{
    if (b->n == b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 128;
        uint32_t *nb = realloc(b->blk, ncap * sizeof *nb);
        if (!nb)
            return -ENOMEM;
        b->blk = nb;
        b->cap = ncap;
    }
    b->blk[b->n++] = bno;
    return 0;
}

/* Free every collected block, then the list itself. */
void filsys_blklist_drain(filsys_edition_t *fs, filsys_blklist_t *b)
{
    if (!b)
        return;
    for (size_t i = 0; i < b->n; i++)
        fs->alloc->bfree(fs, b->blk[i]);
    free(b->blk);
    free(b);
}

/* Free the list without freeing the blocks it names (the truncate failed before
 * the inode was persisted, so the blocks must stay referenced). */
void filsys_blklist_discard(filsys_blklist_t *b)
{
    if (!b)
        return;
    free(b->blk);
    free(b);
}

/* Collect an indirect subtree into *b: the children first, the block itself
 * last.  The whole subtree is going away, so its entries are not rewritten. */
static int collect_subtree(filsys_edition_t *fs, uint32_t blk, int level, filsys_blklist_t *b)
{
    if (blk == 0)
        return 0;
    uint8_t buf[V7_MAXBSIZE];
    if (fs->ops->read_block(fs, blk, buf))
        return -EIO;
    for (uint32_t i = 0; i < fs->nindir; i++) {
        uint32_t nb = ind_get(fs, buf, i);
        if (nb == 0)
            continue;
        if (level > 0) {
            if (collect_subtree(fs, nb, level - 1, b))
                return -EIO;
        } else if (blklist_push(b, nb)) {
            return -ENOMEM;
        }
    }
    return blklist_push(b, blk);
}

/* Collect the leaf blocks at indices [skip, ...) of the subtree rooted at `blk`
 * (which has `level` levels of indirection below it), zeroing the dropped
 * entries and writing them back *before* returning, so the block's on-disk
 * entry list stops referencing the collected children before any of them are
 * freed.  Only called for a partial (skip > 0) truncate, so the block itself is
 * always kept. */
static int collect_subtree_from(filsys_edition_t *fs, uint32_t blk, int level,
                                uint32_t skip, filsys_blklist_t *b)
{
    uint8_t buf[V7_MAXBSIZE];
    if (fs->ops->read_block(fs, blk, buf))
        return -EIO;
    uint32_t sub = 1;
    for (int l = 0; l < level; l++)
        sub *= fs->nindir;               /* leaves per entry */
    uint32_t se = skip / sub;            /* whole entries to keep */
    uint32_t sp = skip % sub;            /* partial skip within entry se */
    for (uint32_t i = 0; i < fs->nindir; i++) {
        uint32_t nb = ind_get(fs, buf, i);
        if (nb == 0)
            continue;
        if (i < se)
            continue;                    /* whole entry kept */
        if (i == se && sp > 0) {
            if (collect_subtree_from(fs, nb, level - 1, sp, b))
                return -EIO;             /* partial: entry kept */
        } else {
            if (level == 0) {
                if (blklist_push(b, nb))
                    return -ENOMEM;
            } else if (collect_subtree(fs, nb, level - 1, b)) {
                return -EIO;
            }
            ind_put(fs, buf, i, 0);      /* drop the collected entry */
        }
    }
    return fs->ops->write_block(fs, blk, buf);  /* persist dropped entries */
}

/* The number of logical blocks a slot at `level` covers (level 0 = single =
 * nindir, 1 = nindir^2, ...); direct slots are handled by the caller. */
static uint32_t slot_span(const filsys_edition_t *fs, int level)
{
    uint32_t span = 1;
    for (int l = 0; l <= level; l++)
        span *= fs->nindir;
    return span;
}

/* Collect every block of an inode (truncate to length 0) into *b.  The caller
 * persists the inode (now with a zeroed addr[]), then filsys_blklist_drain. */
int filsys_itrunc(filsys_edition_t *fs, filsys_inode_t *ip, filsys_blklist_t *b)
{
    if (!is_data_inode(fs, ip))
        return 0;
    for (int i = fs->niaddr - 1; i >= 0; i--) {
        uint32_t bn = ip->addr[i];
        if (bn == 0)
            continue;
        ip->addr[i] = 0;
        int level = filsys_slot_level(fs, ip->mode, i);
        if (level < 0) {
            if (blklist_push(b, bn))
                return -ENOMEM;
        } else if (collect_subtree(fs, bn, level, b)) {
            return -EIO;
        }
    }
    ip->size = 0;
    return 0;
}

/* Collect blocks [first_blk, ...) only, leaving the first first_blk blocks in
 * place; first_blk == 0 is equivalent to filsys_itrunc (except that the inode's
 * size is left to the caller). */
int filsys_itrunc_from(filsys_edition_t *fs, filsys_inode_t *ip, uint32_t first_blk,
                       filsys_blklist_t *b)
{
    uint32_t lbn = 0;
    for (int i = 0; i < fs->niaddr; i++) {
        int level = filsys_slot_level(fs, ip->mode, i);
        uint32_t span = (level < 0) ? 1u : slot_span(fs, level);
        uint32_t a = ip->addr[i];
        if (lbn + span <= first_blk) {   /* entirely kept */
            lbn += span;
            continue;
        }
        if (a == 0) {                    /* a hole past the boundary */
            lbn += span;
            continue;
        }
        if (level < 0) {                 /* direct block past first_blk */
            if (blklist_push(b, a))
                return -ENOMEM;
            ip->addr[i] = 0;
        } else if (lbn >= first_blk) {   /* whole subtree past first_blk */
            if (collect_subtree(fs, a, level, b))
                return -EIO;
            ip->addr[i] = 0;
        } else {                         /* straddles first_blk */
            if (collect_subtree_from(fs, a, level, first_blk - lbn, b))
                return -EIO;
        }
        lbn += span;
    }
    return 0;
}
