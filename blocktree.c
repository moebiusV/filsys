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
#include <stdio.h>

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

/* Free an indirect subtree: the children first, the block itself last (freeing
 * the block first would let the free-list allocator overwrite the entry list we
 * still need to read). */
static void free_subtree(filsys_edition_t *fs, uint32_t blk, int level)
{
    if (blk == 0)
        return;
    uint8_t buf[V7_MAXBSIZE];
    if (fs->ops->read_block(fs, blk, buf))
        return;
    for (uint32_t i = 0; i < fs->nindir; i++) {
        uint32_t nb = ind_get(fs, buf, i);
        if (nb == 0)
            continue;
        if (level > 0)
            free_subtree(fs, nb, level - 1);
        else
            fs->alloc->bfree(fs, nb);
    }
    fs->alloc->bfree(fs, blk);
}

/* Free the leaf blocks at indices [skip, ...) of the subtree rooted at `blk`
 * (which has `level` levels of indirection below it), zeroing the dropped
 * entries.  Only called for a partial (skip > 0) truncate, so the block itself
 * is always kept. */
static void free_subtree_from(filsys_edition_t *fs, uint32_t blk, int level, uint32_t skip)
{
    uint8_t buf[V7_MAXBSIZE];
    if (fs->ops->read_block(fs, blk, buf))
        return;
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
            free_subtree_from(fs, nb, level - 1, sp);   /* partial: entry kept */
        } else {
            if (level == 0) fs->alloc->bfree(fs, nb);
            else free_subtree(fs, nb, level - 1);
            ind_put(fs, buf, i, 0);      /* drop the freed entry */
        }
    }
    fs->ops->write_block(fs, blk, buf);  /* persist dropped entries */
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

/* Free every block of an inode (truncate to length 0). */
int filsys_itrunc(filsys_edition_t *fs, filsys_inode_t *ip)
{
    if (!is_data_inode(fs, ip))
        return 0;
    for (int i = fs->niaddr - 1; i >= 0; i--) {
        uint32_t bn = ip->addr[i];
        if (bn == 0)
            continue;
        ip->addr[i] = 0;
        int level = filsys_slot_level(fs, ip->mode, i);
        if (level < 0)
            fs->alloc->bfree(fs, bn);
        else
            free_subtree(fs, bn, level);
    }
    ip->size = 0;
    return 0;
}

/* Free blocks [first_blk, ...) only, leaving the first first_blk blocks in
 * place; first_blk == 0 is equivalent to filsys_itrunc (except that the inode's
 * size is left to the caller). */
int filsys_itrunc_from(filsys_edition_t *fs, filsys_inode_t *ip, uint32_t first_blk)
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
            fs->alloc->bfree(fs, a);
            ip->addr[i] = 0;
        } else if (lbn >= first_blk) {   /* whole subtree past first_blk */
            free_subtree(fs, a, level);
            ip->addr[i] = 0;
        } else {                         /* straddles first_blk */
            free_subtree_from(fs, a, level, first_blk - lbn);
        }
        lbn += span;
    }
    return 0;
}
