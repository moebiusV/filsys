/* alloc_freelist.c - the free-list block/inode allocator and its check driver (split from v7fs.c along its allocator/dir seams).
 *
 * SPDX-License-Identifier: ISC */
#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "v7fs.h"
#include "filsys_ops.h"
#include "check.h"
#include "instrument.h"

int v7fs_balloc(filsys_edition_t *fs, uint32_t *bno) {
    if (fs->fl.nfree == 0)
        return -ENOSPC;   /* no cached blocks and no dump block to reload */

    uint32_t blk = fs->fl.free[--fs->fl.nfree];
    if (blk == 0)
        return -ENOSPC;

    /* Range-check before touching the free list: on a corrupt image a bogus
     * block number must be rejected here, not after it has been used to reload
     * the in-core free list with garbage. */
    if (blk < v7_data_first(fs) || blk >= fs->fsize)
        return -EIO;   /* badblock: refuse garbage */

    if (fs->fl.nfree == 0) {
        /* Just popped the bottom of the stack: it is a free-list block. */
        uint8_t buf[V7_MAXBSIZE];
        if (v7fs_read_block(fs, blk, buf))
            return -EIO;
        fs->fl.nfree = fs->desc.df_nfree_wid == 4 ? fs->desc.bo->get32(buf + 0)
                                             : fs->desc.bo->get16(buf + 0);
        for (int i = 0; i < fs->desc.nicfree; i++)
            fs->fl.free[i] = v7_get_daddr(fs, buf + v7_chain_free_off(fs) + fs->desc.daddr_wid * i);
        /* blk was the on-disk chain head; its contents (the next segment) are
         * now in the cache, and blk is about to become file data.  Commit the
         * allocator state *before* the caller overwrites blk, else a crash would
         * leave the on-disk chain naming a block full of file data -- the seed
         * of aliasing, not a leak.  One flush per NICFREE blocks, not per block. */
        if (v7fs_super_write(fs))
            return -EIO;
    }
    /* Zero the freshly-allocated block: V7's alloc() clrbuf()s it, and without
     * this the previous file's data leaks into a new file. */
    uint8_t z[V7_MAXBSIZE];
    memset(z, 0, fs->desc.bsize);
    if (v7fs_write_block(fs, blk, z))
        return -EIO;
    if (fs->fl.tfree) fs->fl.tfree--;
    /* A block just left the free list; until the superblock is flushed the
     * on-disk free list still lists it as free.  Record that so write_inode can
     * flush first, closing the "free and referenced" aliasing window. */
    fs->fl_dirty = 1;
    filsys_instr_balloc(blk);
    *bno = blk;
    return 0;
}

void v7fs_bfree(filsys_edition_t *fs, uint32_t bno) {
    if (bno < v7_data_first(fs) || bno >= fs->fsize)
        return;   /* badblock */
    if (fs->fl.nfree == 0) {
        fs->fl.nfree = 1;
        fs->fl.free[0] = 0;
    }
    if (fs->fl.nfree >= fs->desc.nicfree) {
        uint8_t buf[V7_MAXBSIZE];
        memset(buf, 0, fs->desc.bsize);
        if (fs->desc.df_nfree_wid == 4)
            fs->desc.bo->put32(buf + 0, fs->fl.nfree);
        else
            fs->desc.bo->put16(buf + 0, fs->fl.nfree);
        for (int i = 0; i < fs->desc.nicfree; i++)
            v7_put_daddr(fs, buf + v7_chain_free_off(fs) + fs->desc.daddr_wid * i, fs->fl.free[i]);
        /* The dump block must reach disk before the cache is reset, else the
         * freed blocks are lost; and a failed dump must not fall through to
         * free[nfree++] with nfree == nicfree -- that overflows free[].  On a
         * failed dump, leak bno (salvage recovers it) rather than corrupt the
         * cache. */
        if (v7fs_write_block(fs, bno, buf) != 0)
            return;
        fs->fl.nfree = 0;
    }
    fs->fl.free[fs->fl.nfree++] = bno;
    fs->fl.tfree++;
    filsys_instr_bfree(bno);
}

/* A live inode: the edition's "allocated" bit is set if it has one (V6's
 * IALLOC -- a freed V6 inode keeps its stale mode bits but has IALLOC clear),
 * else the mode word is non-zero (V7/V8 free inodes read back as 0).  The
 * allocator and the free-inode counters must agree here, or an inode one side
 * treats as free is counted as used by the other. */
static int inode_live(const filsys_edition_t *fs, uint16_t mode) {
    return fs->desc.iallocated ? (mode & fs->desc.iallocated) != 0 : mode != 0;
}

int v7fs_ialloc(filsys_edition_t *fs, uint32_t *ino) {
    uint32_t maxino = v7_maxinode(fs);

    for (;;) {
        if (fs->fl.ninode > 0) {
            uint32_t cand = fs->fl.inode[--fs->fl.ninode];
            if (cand < 2 || cand > maxino)
                continue;   /* bad cache entry: skip */
            v7_inode_t ip;
            if (fs->desc.ops->inode->read_inode(fs, cand, &ip))
                return -EIO;   /* read error, not "in use": don't reclassify */
            if (inode_live(fs, ip.mode))
                continue;   /* was already allocated; look again */
            memset(&ip, 0, sizeof(ip));
            ip.ino = cand;
            fs->desc.ops->inode->write_inode(fs, cand, &ip);
            if (fs->fl.tinode) fs->fl.tinode--;
            fs->fl_dirty = 1;   /* inode cache changed: flush before reference */
            filsys_instr_ialloc(cand);
            *ino = cand;
            return 0;
        }
        /* Refill the cache with a linear scan of the i-list. */
        fs->fl.ninode = 0;
        for (uint32_t in = 2; in <= maxino && fs->fl.ninode < fs->desc.nicinod; in++) {
            v7_inode_t ip;
            if (fs->desc.ops->inode->read_inode(fs, in, &ip))
                return -EIO;   /* a mid-scan read error must propagate, not truncate */
            if (!inode_live(fs, ip.mode))
                fs->fl.inode[fs->fl.ninode++] = (uint16_t)in;
        }
        if (fs->fl.ninode == 0)
            return -ENOSPC;
    }
}

void v7fs_ifree(filsys_edition_t *fs, uint32_t ino) {
    if (fs->fl.ninode >= fs->desc.nicinod)
        return;   /* kernel discards beyond the cache */
    fs->fl.inode[fs->fl.ninode++] = (uint16_t)ino;
    fs->fl.tinode++;
    filsys_instr_ifree(ino);
}

/* Rebuild the free list from the block-usage map (icheck -s).  Returns the
 * number of free blocks, or -1 if the superblock could not be read.
 *
 * Coherent interleaves the free list so that sequential allocation walks
 * blocks around the cylinder: a logical block bn maps to the physical block
 * (bn/n)*n + maptab[bn%n], where maptab[i] = (i/ratio) + (i%ratio)*m and
 * ratio = n/m (s_m, s_n).  The free list and inode addresses store the
 * resulting physical (interleaved) numbers, so the read/write path uses them
 * directly -- no runtime mapping.  V7/32V have no interleave: s_m = s_n = 1,
 * the identity map. */
uint32_t v7fs_makefree(filsys_edition_t *fs, filsys_chkctx_t *cx)
{
    filsys_edition_t *f = fs;
    if (f->desc.freemap != V8_FREEMAP_LIST)
        return v8_makefree_bitmap(fs, cx);
    uint32_t m, n;
    if (f->desc.interleave) {
        m = f->m;
        n = f->n;
        if (n < 1 || n > V7_COH_MAXINTN || m < 1 || m > n || n % m != 0) {
            printf("invalid interleave factors in superblock (m=%u n=%u); defaulting\n", m, n);
            m = 1;
            n = 1;
        }
    } else {
        m = 1;
        n = 1;   /* V7/32V: no interleave */
    }

    uint32_t maptab[V7_COH_MAXINTN];
    uint32_t ratio = n / m;
    for (uint32_t i = 0; i < n; i++)
        maptab[i] = (i / ratio) + (i % ratio) * m;

    /* Interleave only within the data band, aligned to n blocks (phase6's
     * mapbot/maptop). */
    uint32_t mapbot = ((f->isize + n - 1) / n) * n;
    uint32_t maptop = (f->fsize / n) * n;

    /* v7fs_bfree increments f->fl.tfree as it goes, so reset the running totals
     * first -- otherwise the rebuilt count is added on top of the old one. */
    f->fl.nfree = 0;
    f->fl.ninode = 0;
    f->fl.tfree = 0;
    f->fl.tinode = 0;
    uint32_t nfree = 0;
    for (uint32_t bn = f->isize; bn < f->fsize; bn++) {
        uint32_t blk = bn;
        if (bn >= mapbot && bn < maptop)
            blk = (bn / n) * n + maptab[bn % n];
        uint32_t off = blk - f->isize;
        if (off >= cx->nblk)
            continue;   /* interleave stays within [isize, fsize), but be safe */
        if (!(cx->bmap[off >> 3] & (uint8_t)(1u << (off & 7)))) {
            v7fs_bfree(f, blk);
            nfree++;
        }
    }
    /* The free-inode total is s_tinode, not the 50-entry cache: rebuild it from
     * the i-list so a salvage writes the true count rather than 0 (System III's
     * fsck reports a wrong s_tinode otherwise). */
    uint32_t maxino = v7_maxinode(f), used = 0;
    for (uint32_t ino = 1; ino <= maxino; ino++) {
        v7_inode_t ip;
        if (v7fs_read_inode(f, ino, &ip) == 0 && inode_live(f, ip.mode))
            used++;
    }
    f->fl.tinode = maxino - used;
    v7fs_super_write(f);   /* writes f->fl.tfree (= nfree) and f->fl.tinode */
    return nfree;
}

/* Walk the free list exactly as alloc() would, marking free blocks into
 * cx->inode->bmap (a free block already used is a duplicate) and counting
 * free_blocks. */
void v7_walk_free(filsys_edition_t *fs, filsys_chkctx_t *cx, filsys_check_t *rep)
{
    filsys_edition_t *f = fs;
    if (f->desc.freemap != V8_FREEMAP_LIST) {
        v8_walk_free_bitmap(fs, cx, rep);
        return;
    }
    uint8_t *seen = calloc(f->fsize ? f->fsize : 1, 1);
    if (!seen)
        return;
    uint16_t n = f->fl.nfree;
    uint32_t cur[V8_NICFREE_LARGE];   /* the V8-family cache holds up to 946 */
    memcpy(cur, f->fl.free, sizeof(cur));
    uint32_t guard = 0;
    while (n > 0) {
        uint32_t bno = cur[--n];
        if (bno == 0)
            break;                       /* sentinel: end of chain */
        if (bno < v7_data_first(f) || bno >= f->fsize) {
            printf("free block %u out of range [%u,%u)\n", bno, v7_data_first(f), f->fsize);
            rep->errors++;
            break;
        }
        if (seen[bno]) {
            printf("free block %u listed twice (cycle?)\n", bno);
            rep->errors++;
            break;
        }
        seen[bno] = 1;
        uint32_t off = bno - v7_data_first(f);
        uint8_t m = (uint8_t)(1u << (off & 7));
        if (cx->bmap[off >> 3] & m) {
            printf("block %u dup; free-list\n", bno);
            cx->dup_blocks++;
            rep->errors++;
        } else {
            cx->bmap[off >> 3] |= m;
            rep->free_blocks++;   /* a duplicate is used, not free */
        }
        if (++guard > f->fsize + f->desc.nicfree) {
            printf("free list does not terminate\n");
            rep->errors++;
            break;
        }
        if (n == 0) {
            uint8_t blk[V7_MAXBSIZE];
            if (v7fs_read_block(f, bno, blk)) {
                printf("cannot read free-list block %u\n", bno);
                rep->errors++;
                break;
            }
            n = f->desc.df_nfree_wid == 4 ? f->desc.bo->get32(blk) : f->desc.bo->get16(blk);
            if (n > f->desc.nicfree) {
                printf("free-list block %u has bad count %u\n", bno, n);
                rep->errors++;
                break;
            }
            for (int i = 0; i < f->desc.nicfree; i++)
                cur[i] = v7_get_daddr(f, blk + v7_chain_free_off(f) + f->desc.daddr_wid * i);
        }
    }
    free(seen);
}

void v7_count_free(filsys_edition_t *fs, uint32_t *nblk, uint32_t *nino) {
    uint32_t n = fs->fl.nfree;
    uint32_t cur[V8_NICFREE_LARGE];   /* the largest free-cache depth (V9) */
    memcpy(cur, fs->fl.free, sizeof(cur));
    uint32_t blocks = 0, guard = 0;
    while (n > 0) {
        uint32_t bno = cur[--n];
        if (bno == 0)
            break;                       /* sentinel: end of chain */
        blocks++;
        if (n == 0) {
            uint8_t blk[V7_MAXBSIZE];
            if (v7fs_read_block(fs, bno, blk))
                break;
            n = fs->desc.df_nfree_wid == 4 ? fs->desc.bo->get32(blk) : fs->desc.bo->get16(blk);
            if (n > fs->desc.nicfree)
                break;
            for (int i = 0; i < fs->desc.nicfree; i++)
                cur[i] = v7_get_daddr(fs, blk + v7_chain_free_off(fs) + fs->desc.daddr_wid * i);
        }
        if (++guard > fs->fsize + fs->desc.nicfree)
            break;
    }
    *nblk = blocks;

    uint32_t maxino = v7_maxinode(fs), used = 0;
    for (uint32_t ino = 1; ino <= maxino; ino++) {
        v7_inode_t ip;
        if (v7fs_read_inode(fs, ino, &ip) == 0 && inode_live(fs, ip.mode))
            used++;
    }
    *nino = maxino - used;
}

void v6_count_free(filsys_edition_t *fs, uint32_t *nblk, uint32_t *nino) {
    uint32_t n = fs->fl.nfree;
    uint32_t cur[V6_NICFREE];
    memcpy(cur, fs->fl.free, sizeof(cur));
    uint32_t blocks = 0, guard = 0;
    while (n > 0) {
        uint32_t bno = cur[--n];
        if (bno == 0)
            break;
        blocks++;
        if (n == 0) {
            uint8_t blk[V6_BSIZE];
            if (v7fs_read_block(fs, bno, blk))
                break;
            n = fs->desc.bo->get16(blk + 0);
            for (int i = 0; i < fs->desc.nicfree; i++)
                cur[i] = fs->desc.bo->get16(blk + 2 + 2 * i);
        }
        if (++guard > fs->fsize + fs->desc.nicfree)
            break;
    }
    *nblk = blocks;

    uint32_t maxino = v6_maxino(fs->isize), used = 0;
    for (uint32_t ino = 1; ino <= maxino; ino++) {
        v7_inode_t ip;
        if (v6_read_inode(fs, ino, &ip) == 0 && inode_live(fs, ip.mode))
            used++;
    }
    *nino = maxino - used;
}

