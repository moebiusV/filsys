/* alloc_v8bitmap.c - the V8-family bitmap allocator and superblock codec (split from v7fs.c along its allocator/dir seams).
 *
 * SPDX-License-Identifier: ISC */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "v7fs.h"
#include "filsys_ops.h"
#include "check.h"
#include "instrument.h"

/* Decode the rearranged V8-family superblock into the shared in-core fields. */
int v8_sb_decode(filsys_edition_t *fs, const uint8_t *sb) {
    uint16_t (*get16)(const uint8_t *) = fs->desc.bo->get16;
    uint32_t (*get32)(const uint8_t *) = fs->desc.bo->get32;
    fs->isize  = get16(sb + V8_SB_ISIZE);
    fs->fsize  = get32(sb + V8_SB_FSIZE);
    fs->fl.ninode = get16(sb + V8_SB_NINODE);
    for (int i = 0; i < V7_NICINOD; i++)
        fs->fl.inode[i] = get16(sb + V8_SB_INODE + 2 * i);
    fs->time   = get32(sb + V8_SB_TIME);
    fs->fmod   = sb[V8_SB_FMOD];
    fs->fl.tfree  = get32(sb + V8_SB_TFREE);
    fs->fl.tinode = get16(sb + V8_SB_TINODE);
    if (fs->desc.freemap == V8_FREEMAP_LIST) {
        fs->fl.nfree = get16(sb + V8_SB_NFREE);
        for (int i = 0; i < fs->desc.nicfree; i++)
            fs->fl.free[i] = get32(sb + V8_SB_FREE + 4 * i);
    } else {
        fs->fl.nfree = 0;   /* bitmap: no free-list cache */
    }
    if (fs->fl.nfree > fs->desc.nicfree || fs->fl.ninode > fs->desc.nicinod)
        return -EINVAL;
    return 0;
}

int v8_sb_encode(filsys_edition_t *fs, uint8_t *sb) {
    void (*put16)(uint8_t *, uint16_t) = fs->desc.bo->put16;
    void (*put32)(uint8_t *, uint32_t) = fs->desc.bo->put32;
    put16(sb + V8_SB_ISIZE, fs->isize);
    put32(sb + V8_SB_FSIZE, fs->fsize);
    put16(sb + V8_SB_NINODE, fs->fl.ninode);
    for (int i = 0; i < V7_NICINOD; i++)
        put16(sb + V8_SB_INODE + 2 * i, fs->fl.inode[i]);
    put32(sb + V8_SB_TIME, (uint32_t)time(NULL));
    sb[V8_SB_FMOD] = (uint8_t)(fs->fmod != 0);
    put32(sb + V8_SB_TFREE, fs->fl.tfree);
    put16(sb + V8_SB_TINODE, (uint16_t)fs->fl.tinode);
    if (fs->desc.freemap == V8_FREEMAP_LIST) {
        put16(sb + V8_SB_NFREE, fs->fl.nfree);
        for (int i = 0; i < fs->desc.nicfree; i++)
            put32(sb + V8_SB_FREE + 4 * i, fs->fl.free[i]);
    } else if (fs->desc.freemap == V8_FREEMAP_BITMAP) {
        /* in-superblock bitmap: S_valid=1 and S_bfree[] from the in-core bits */
        sb[V8_SB_VALID] = 1;
        for (uint32_t w = 0; w < V8_BITMAP; w++) {
            uint32_t word = 0;
            for (uint32_t b = 0; b < 32; b++) {
                uint32_t i = w * 32 + b;
                if (i >= fs->v8_nbits)
                    break;
                if (fs->v8_bits[i >> 3] & (uint8_t)(1u << (i & 7)))
                    word |= (1u << b);
            }
            put32(sb + V8_SB_BFREE + 4 * w, word);
        }
    } else {   /* out-of-superblock bitmap (v10): S_flag=1, S_bsize=BSIZE*8 */
        sb[V8_SB_VALID] = 1;
        sb[V8_SB_FLAG] = 1;
        put32(sb + V8_SB_BSIZE, fs->desc.bsize * 8);
    }
    return 0;
}

/* Load the V8-family bitmap into fs->v8_bits (heap).  sb is the block-1 bytes
 * for the in-superblock form; the out-of-superblock form reads its bitmap
 * blocks from disk.  bit i is set iff the block is free. */
int v8_bitmap_load(filsys_edition_t *fs, const uint8_t *sb)
{
    if (fs->desc.freemap == V8_FREEMAP_BIGMAP) {
        fs->v8_base = 0;
        fs->v8_nbits = fs->fsize;
        uint32_t bits_per_blk = fs->desc.bsize * 8;
        fs->v8_nblks = (fs->fsize + bits_per_blk - 1) / bits_per_blk;
        fs->v8_blk_start = fs->fsize - fs->v8_nblks;
    } else {
        fs->v8_base = fs->isize;
        fs->v8_nbits = fs->fsize - fs->isize;
        fs->v8_nblks = 0;
        /* The in-superblock bitmap holds V8_BITMAP_BITS (30752) blocks; a data
         * area past that cannot be described there and the superblock read would
         * walk off the valid bytes into garbage.  A volume this large must use
         * the out-of-superblock bigmap. */
        if (fs->v8_nbits > V8_BITMAP_BITS)
            return -E2BIG;
    }
    fs->v8_bits = filsys_alloc(FILSYS_AL_MOUNT, (size_t)(fs->v8_nbits + 7) / 8, 1);
    if (!fs->v8_bits)
        return -ENOMEM;
    if (fs->desc.freemap == V8_FREEMAP_BITMAP) {
        for (uint32_t w = 0; w < (fs->v8_nbits + 31) / 32; w++) {
            uint32_t word = fs->desc.bo->get32(sb + V8_SB_BFREE + 4 * w);
            for (uint32_t b = 0; b < 32 && w * 32 + b < fs->v8_nbits; b++)
                if (word & (1u << b))
                    fs->v8_bits[(w * 32 + b) >> 3] |= (uint8_t)(1u << ((w * 32 + b) & 7));
        }
    } else {
        for (uint32_t k = 0; k < fs->v8_nblks; k++) {
            uint8_t blk[V7_MAXBSIZE];
            if (v7fs_read_block(fs, fs->v8_blk_start + k, blk))
                return -EIO;
            uint32_t nbytes = fs->desc.bsize;
            if (k == fs->v8_nblks - 1)
                nbytes = (fs->v8_nbits - k * fs->desc.bsize * 8 + 7) / 8;
            memcpy(fs->v8_bits + k * fs->desc.bsize, blk, nbytes);
        }
    }
    return 0;
}

int v8_bitmap_balloc(filsys_edition_t *fs, uint32_t *bno)
{
    for (uint32_t i = 0; i < fs->v8_nbits; i++) {
        if (fs->v8_bits[i >> 3] & (uint8_t)(1u << (i & 7))) {
            fs->v8_bits[i >> 3] &= (uint8_t)~(1u << (i & 7));
            uint32_t blk = fs->v8_base + i;
            /* Zero the freshly-allocated block: alloc() clrbuf()s it, so a new
             * file's partial block can't leak the previous file's data. */
            uint8_t z[V7_MAXBSIZE];
            memset(z, 0, fs->desc.bsize);
            if (v7fs_write_block(fs, blk, z))
                return -EIO;
            if (fs->fl.tfree) fs->fl.tfree--;
            fs->fl_dirty = 1;
            filsys_instr_balloc(blk);
            *bno = blk;
            return 0;
        }
    }
    return -ENOSPC;
}

void v8_bitmap_bfree(filsys_edition_t *fs, uint32_t bno)
{
    if (bno < fs->v8_base || bno >= fs->v8_base + fs->v8_nbits)
        return;
    uint32_t i = bno - fs->v8_base;
    fs->v8_bits[i >> 3] |= (uint8_t)(1u << (i & 7));
    fs->fl.tfree++;
    fs->fl_dirty = 1;
    filsys_instr_bfree(bno);
}

/* Flush the bitmap and the superblock totals.  The in-superblock form rides
 * v7fs_super_write (v8_sb_encode writes S_bfree); the out-of-superblock form
 * writes its bitmap blocks then the superblock. */
int v8_bitmap_sync(filsys_edition_t *fs)
{
    if (fs->readonly)
        return 0;
    if (fs->desc.freemap == V8_FREEMAP_BIGMAP) {
        for (uint32_t k = 0; k < fs->v8_nblks; k++) {
            uint8_t blk[V7_MAXBSIZE] = {0};
            uint32_t nbytes = fs->desc.bsize;
            if (k == fs->v8_nblks - 1)
                nbytes = (fs->v8_nbits - k * fs->desc.bsize * 8 + 7) / 8;
            memcpy(blk, fs->v8_bits + k * fs->desc.bsize, nbytes);
            if (v7fs_write_block(fs, fs->v8_blk_start + k, blk))
                return -EIO;
        }
    }
    return v7fs_super_write(fs);
}

/* Rebuild the V8-family bitmap free-space from the used-block map.  Used by
 * mkfs (fresh image) and by fsck -s salvage.  bit i is set iff free. */
uint32_t v8_makefree_bitmap(filsys_edition_t *fs, filsys_chkctx_t *cx)
{
    filsys_edition_t *f = fs;
    uint32_t base, nbits;
    if (f->desc.freemap == V8_FREEMAP_BIGMAP) {
        uint32_t bits_per_blk = f->desc.bsize * 8;
        f->v8_nblks = (f->fsize + bits_per_blk - 1) / bits_per_blk;
        f->v8_blk_start = f->fsize - f->v8_nblks;
        base = 0; nbits = f->fsize;
    } else {
        f->v8_nblks = 0;
        base = f->isize; nbits = f->fsize - f->isize;
    }
    f->v8_base = base;
    filsys_free(FILSYS_AL_MOUNT, f->v8_bits, (size_t)(f->v8_nbits + 7) / 8);
    f->v8_nbits = nbits;
    f->v8_bits = filsys_alloc(FILSYS_AL_MOUNT, (size_t)(nbits + 7) / 8, 1);
    if (!f->v8_bits)
        return 0;
    f->fl.nfree = 0;
    f->fl.ninode = 0;
    f->fl.tfree = 0;
    f->fl.tinode = 0;
    uint32_t nfree = 0;
    for (uint32_t blk = f->isize; blk < f->fsize; blk++) {
        uint32_t off = blk - f->isize;
        if (off >= cx->nblk)
            continue;
        if (cx->bmap[off >> 3] & (uint8_t)(1u << (off & 7)))
            continue;   /* used */
        uint32_t i = blk - base;
        f->v8_bits[i >> 3] |= (uint8_t)(1u << (i & 7));
        nfree++;
    }
    /* The out-of-superblock bitmap's own blocks sit past cx->nblk (data_end
     * excludes them), so they were never marked free above -- their bits stay 0. */
    f->fl.tfree = nfree;
    v8_bitmap_sync(f);
    return nfree;
}

/* Walk the V8-family bitmap as alloc() would, marking free blocks into
 * cx->inode->bmap (a free block already used is a duplicate) and counting
 * free_blocks. */
void v8_walk_free_bitmap(filsys_edition_t *fs, filsys_chkctx_t *cx,
                                filsys_check_t *rep)
{
    filsys_edition_t *f = fs;
    for (uint32_t i = 0; i < f->v8_nbits; i++) {
        if (!(f->v8_bits[i >> 3] & (uint8_t)(1u << (i & 7))))
            continue;
        uint32_t blk = f->v8_base + i;
        if (blk < v7_data_first(f) || blk >= f->fsize)
            continue;   /* boot/superblock/i-list: reserved, never free */
        uint32_t off = blk - v7_data_first(f);
        uint8_t m = (uint8_t)(1u << (off & 7));
        if (cx->bmap[off >> 3] & m) {
            printf("block %u dup; bitmap\n", blk);
            cx->dup_blocks++;
            rep->errors++;
        } else {
            cx->bmap[off >> 3] |= m;
            rep->free_blocks++;   /* a duplicate is used, not free */
        }
    }
}
