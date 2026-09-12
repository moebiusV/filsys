/* filsys 1.8.0 - 2026-09-06 - Copyright (C) 2026 David Walther */
/* SPDX-License-Identifier: ISC */
/* v7fs.c - Seventh Edition Unix filesystem, on-disk access layer.
 *
 * Reads and writes a V7 filesystem image, and a 32V (VAX) image under the
 * same code with the little-endian byte order selected (see v7fs.h).  The
 * allocation algorithms (balloc/bfree/ialloc/ifree) mirror the V7 kernel's
 * sys/alloc.c so the free list stays interchangeable with what a running
 * kernel expects.
 */
#include <config.h>
#include "v7fs.h"
#include "filsys_ops.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

static int super_write(filsys_edition_t *fs);
static int v8_bitmap_sync(filsys_edition_t *fs);
static void v6_count_free(filsys_edition_t *fs, uint32_t *nblk, uint32_t *nino);
static void v7_count_free(filsys_edition_t *fs, uint32_t *nblk, uint32_t *nino);

/* Flush the allocator state: the free list rides super_write; the bitmap forms
 * write their bitmap (in-superblock via super_write, out-of-superblock via its
 * blocks) and then the superblock. */
static int flush_fs(filsys_edition_t *fs) {
    if (fs->freemap != V8_FREEMAP_LIST)
        return v8_bitmap_sync(fs);
    return super_write(fs);
}

/* ---- lifecycle --------------------------------------------------------- */

int v7fs_open(filsys_edition_t *fs, const char *path, int readonly,
              const filsys_edition_t *proto, uint64_t offset) {
    if (fs != proto)
        memcpy(fs, proto, sizeof *fs); /* copy the static descriptor fields */
    fs->readonly = readonly;
    fs->base = offset;
    fs->fd = open(path, readonly ? O_RDONLY : O_RDWR);
    if (fs->fd < 0)
        return -errno;
    fs->io = &filsys_io_file;

    if (fs->dyn_bsize) {
        /* System V: the 512-byte superblock sits at a fixed byte offset (512),
         * not "block 1" -- that only coincides with byte 512 when the block size
         * is 512.  s_type (offset 508) names the block size. */
        uint8_t probe[512];
        if (fs->io->read(fs, probe, sizeof probe, 512 + (off_t)fs->base)) {
            close(fs->fd);
            fs->fd = -1;
            return -EIO;
        }
        if (!fs->ignore_magic &&
            fs->bo->get32(probe + V7_SYSV_MAGIC_OFF) != V7_SYSV_MAGIC) {
            close(fs->fd);
            fs->fd = -1;
            return -EINVAL;
        }
        uint32_t t = fs->bo->get32(probe + V7_SYSV_TYPE_OFF);
        uint32_t bsize = t == V7_SYSV_Fs1b ? 512 : t == V7_SYSV_Fs2b ? 1024 : t == V7_SYSV_Fs4b ? 2048 : 0;
        if (bsize == 0) {
            close(fs->fd);
            fs->fd = -1;
            return -EINVAL;
        }
        /* The logical block size and the indirect-block entry count both follow
         * from s_type; the 4-byte daddr_t is fixed. */
        fs->bsize = bsize;
        fs->nindir = bsize / fs->daddr_wid;
    }

    uint8_t sb[V7_MAXBSIZE];
    if (fs->dyn_bsize) {
        /* The System V superblock is a 512-byte struct at byte 512, whatever the
         * logical block size (for 1K/2K blocks it lives inside logical block 0). */
        if (fs->io->read(fs, sb, 512, 512 + (off_t)fs->base)) {
            close(fs->fd);
            return -EIO;
        }
    } else if (v7fs_read_block(fs, V7_SUPERB, sb)) {
        close(fs->fd);
        return -EIO;
    }
    if (fs->sb_decode) {
        /* V8-family: the rearranged superblock is decoded by the format's own
         * codec (v8_sb_decode), which also validates the cache counts. */
        if (fs->sb_decode(fs, sb)) {
            close(fs->fd);
            fs->fd = -1;
            return -EINVAL;
        }
    } else if (fs->isize_count) {
        /* V6: 16-bit superblock (s_fsize and the free cache are 2 bytes). */
        fs->isize  = fs->bo->get16(sb + 0);
        fs->fsize  = fs->bo->get16(sb + 2);
        fs->fl.nfree  = fs->bo->get16(sb + 4);
        for (int i = 0; i < fs->nicfree; i++)
            fs->fl.free[i] = fs->bo->get16(sb + 6 + 2 * i);
        fs->fl.ninode = fs->bo->get16(sb + 206);
        for (int i = 0; i < fs->nicinod; i++)
            fs->fl.inode[i] = fs->bo->get16(sb + 208 + 2 * i);
        fs->time   = fs->bo->get32(sb + 412);
        fs->fmod   = sb[410];              /* s_fmod */
    } else {
        fs->isize  = fs->bo->get16(sb + 0);
        fs->fsize  = fs->bo->get32(sb + sb_fsize_off(fs->pack4));
        fs->fl.nfree  = fs->bo->get16(sb + sb_nfree_off(fs->pack4));
        for (int i = 0; i < fs->nicfree; i++)
            fs->fl.free[i] = fs->bo->get32(sb + sb_free_off(fs->pack4) + 4 * i);
        fs->fl.ninode = fs->bo->get16(sb + sb_ninode_off(fs->pack4, fs->nicfree));
        for (int i = 0; i < V7_NICINOD; i++)
            fs->fl.inode[i] = fs->bo->get16(sb + sb_inode_off(fs->pack4, fs->nicfree) + 2 * i);
        fs->time   = fs->bo->get32(sb + sb_time_off(fs->pack4, fs->nicfree));
        fs->fmod   = sb[sb_time_off(fs->pack4, fs->nicfree) - fs->fmod_back];  /* s_fmod */
    }
    if (fs->sb_decode && fs->freemap != V8_FREEMAP_LIST) {
        /* V8-family bitmap forms: load the free-space bitmap into core (from
         * the superblock for the in-superblock form, from its blocks for the
         * out-of-superblock v10 form). */
        if (v8_bitmap_load(fs, sb)) {
            close(fs->fd);
            fs->fd = -1;
            return -EIO;
        }
    }
    /* The free/inode cache counts are bounded by the cache depth.  A value past
     * it means this superblock was mis-decoded (the wrong edition, e.g. a V7
     * volume opened as V6) -- reject before the free-list walk reads cur[nfree]
     * out of bounds. */
    if (fs->fl.nfree > fs->nicfree || fs->fl.ninode > fs->nicinod) {
        close(fs->fd);
        fs->fd = -1;
        return -EINVAL;
    }
    /* s_tfree/s_tinode carry the true free-space totals (v7fs_makefree writes
     * them); the 50/100-entry caches are only the in-core spill.  Read them so
     * statfs can report real free space rather than the cache depth.  V6 has no
     * such fields, so its totals are recomputed from the free list + i-list.
     * The V8-family codec already read them. */
    if (fs->sb_decode)
        ;   /* totals already read by v8_sb_decode */
    else if (fs->isize_count)
        v6_count_free(fs, &fs->fl.tfree, &fs->fl.tinode);
    else if (!fs->pack4 || fs->magic) {
        int toff = sb_tfree_off(fs->pack4, fs->nicfree, fs->has_dinfo);
        fs->fl.tfree  = fs->bo->get32(sb + toff);
        fs->fl.tinode = fs->bo->get16(sb + toff + 4);
    } else {
        /* 32V and System III (pack4, no magic): the s_tfree/s_tinode words sit at
         * the pack4 offset but are unmaintained on historical media, so recompute
         * them from the free list and i-list -- exactly what the V6 branch above
         * does for its layout. */
        v7_count_free(fs, &fs->fl.tfree, &fs->fl.tinode);
    }
    if (fs->interleave) {
        fs->m      = fs->bo->get16(sb + sb_time_off(fs->pack4, fs->nicfree) + 10);
        fs->n      = fs->bo->get16(sb + sb_time_off(fs->pack4, fs->nicfree) + 12);
        fs->unique = fs->bo->get32(sb + sb_time_off(fs->pack4, fs->nicfree) + 26);
    }
    if (!fs->ignore_magic && fs->magic &&
        fs->bo->get32(sb + fs->magic_off) != fs->magic) {
        /* Xenix carries a magic at superblock offset 1016; System V carries one
         * at 504.  Validate it rather than mis-decoding a foreign volume named
         * with this edition. */
        close(fs->fd);
        fs->fd = -1;
        return -EINVAL;
    }

    /* Reject a superblock that claims more disk than the image file actually
     * holds, one with no data area, or an i-list too small to subtract 2 from
     * (`(isize - 2)` underflows when isize is 0 or 1, turning the inode walk
     * into a multi-gigabyte loop).  Without this, a corrupt image can make
     * the checker (and directory readers) allocate gigabytes. */
    uint64_t imgsize;
    if (filsys_dev_size(fs->fd, &imgsize) != 0 ||
        fs->isize < (fs->isize_count ? 1 : 2) ||
        fs->base + (uint64_t)fs->fsize * fs->bsize > imgsize ||
        fs->fsize <= v7_data_first(fs)) {
        close(fs->fd);
        fs->fd = -1;
        return -EINVAL;
    }
    return 0;
}

int v7fs_close(filsys_edition_t *fs) {
    int rc = 0;
    if (fs->fd >= 0) {
        /* The superblock is flushed once here, not per alloc/free: V7's kernel
         * syncs the superblock periodically rather than on every block handoff,
         * and batching avoids one 512-byte pwrite per freed block on truncate.
         * The clean/dirty s_fmod transition is mark_dirty/mark_clean's job, not
         * the close's: a fsck repair opens read-write and must be able to close
         * without clearing s_fmod (a partial repair leaves the image inconsistent
         * and a clean flag would make the next check skip it). */
        if (!fs->readonly)
            rc = flush_fs(fs);
        close(fs->fd);
        fs->fd = -1;
    }
    free(fs->v8_bits);
    fs->v8_bits = NULL;
    return rc;
}

int v7fs_sync(filsys_edition_t *fs) {
    if (fs->readonly)
        return 0;
    return flush_fs(fs);
}

int v7fs_mark_dirty(filsys_edition_t *fs) {
    if (fs->readonly)
        return 0;
    fs->fmod = 1;
    return flush_fs(fs);
}

int v7fs_mark_clean(filsys_edition_t *fs) {
    if (fs->readonly)
        return 0;
    fs->fmod = 0;
    return flush_fs(fs);
}

/* ---- block io ---------------------------------------------------------- */

/* Default transport: positional read/write on the open image.  Every block
 * access routes through fs->io so a test can swap in a fault-injecting
 * implementation without touching the format code. */
static int io_file_read(filsys_edition_t *fs, void *buf, size_t n, off_t off) {
    ssize_t r = pread(fs->fd, buf, n, off);
    return r == (ssize_t)n ? 0 : -EIO;
}
static int io_file_write(filsys_edition_t *fs, const void *buf, size_t n, off_t off) {
    ssize_t r = pwrite(fs->fd, buf, n, off);
    return r == (ssize_t)n ? 0 : -EIO;
}
const filsys_io_t filsys_io_file = { io_file_read, io_file_write };

int v7fs_read_block(filsys_edition_t *fs, uint32_t bno, uint8_t *buf) {
    return fs->io->read(fs, buf, fs->bsize, (off_t)bno * fs->bsize + (off_t)fs->base);
}

int v7fs_write_block(filsys_edition_t *fs, uint32_t bno, const uint8_t *buf) {
    if (fs->readonly)
        return -EROFS;
    return fs->io->write(fs, buf, fs->bsize, (off_t)bno * fs->bsize + (off_t)fs->base);
}

/* ---- superblock persistence --------------------------------------------- */

static int super_write(filsys_edition_t *fs) {
    if (fs->readonly)
        return 0;
    uint8_t sb[V7_MAXBSIZE];
    /* Read the current block to preserve the fields we don't maintain
     * (s_tfree, s_tinode, s_m, s_n, s_fname, s_fpack, ...).  System V keeps the
     * superblock at a fixed byte offset (512), not block 1. */
    if (fs->dyn_bsize
            ? fs->io->read(fs, sb, 512, 512 + (off_t)fs->base)
            : v7fs_read_block(fs, V7_SUPERB, sb))
        return -EIO;
    if (fs->sb_encode) {
        /* V8-family: the rearranged superblock is encoded by its own codec. */
        fs->sb_encode(fs, sb);
    } else if (fs->isize_count) {
        /* V6: a 16-bit superblock (s_fsize and the free cache are 2 bytes), and
         * no s_tfree/s_tinode -- those totals are recomputed on open. */
        fs->bo->put16(sb + 0, fs->isize);
        fs->bo->put16(sb + 2, (uint16_t)fs->fsize);
        fs->bo->put16(sb + 4, fs->fl.nfree);
        for (int i = 0; i < fs->nicfree; i++)
            fs->bo->put16(sb + 6 + 2 * i, (uint16_t)fs->fl.free[i]);
        fs->bo->put16(sb + 206, fs->fl.ninode);
        for (int i = 0; i < fs->nicinod; i++)
            fs->bo->put16(sb + 208 + 2 * i, fs->fl.inode[i]);
        fs->bo->put32(sb + 412, (uint32_t)time(NULL));   /* s_time[2] */
        sb[410] = (uint8_t)(fs->fmod != 0);              /* s_fmod */
    } else {
        fs->bo->put16(sb + 0, fs->isize);
        fs->bo->put32(sb + sb_fsize_off(fs->pack4), fs->fsize);
        fs->bo->put16(sb + sb_nfree_off(fs->pack4), fs->fl.nfree);
        for (int i = 0; i < fs->nicfree; i++)
            fs->bo->put32(sb + sb_free_off(fs->pack4) + 4 * i, fs->fl.free[i]);
        fs->bo->put16(sb + sb_ninode_off(fs->pack4, fs->nicfree), fs->fl.ninode);
        for (int i = 0; i < V7_NICINOD; i++)
            fs->bo->put16(sb + sb_inode_off(fs->pack4, fs->nicfree) + 2 * i, fs->fl.inode[i]);
        uint32_t now = (uint32_t)time(NULL);
        fs->bo->put32(sb + sb_time_off(fs->pack4, fs->nicfree), now);  /* s_time */
        sb[sb_time_off(fs->pack4, fs->nicfree) - fs->fmod_back] = (uint8_t)(fs->fmod != 0); /* s_fmod */
        if (fs->has_state)
            fs->bo->put32(sb + V7_SYSV_STATE_OFF,
                          fs->fmod ? V7_SYSV_STATE_ACTIVE : V7_SYSV_STATE_CLEAN); /* s_state (R4 only) */
        if (!fs->pack4 || fs->magic) {
            int toff = sb_tfree_off(fs->pack4, fs->nicfree, fs->has_dinfo);
            fs->bo->put32(sb + toff, fs->fl.tfree);        /* s_tfree */
            fs->bo->put16(sb + toff + 4, (uint16_t)fs->fl.tinode);   /* s_tinode */
        }
        if (fs->interleave) {
            fs->bo->put16(sb + sb_time_off(fs->pack4, fs->nicfree) + 10, fs->m);
            fs->bo->put16(sb + sb_time_off(fs->pack4, fs->nicfree) + 12, fs->n);
            fs->bo->put32(sb + sb_time_off(fs->pack4, fs->nicfree) + 26, fs->unique);
        }
    }
    if (fs->dyn_bsize
            ? fs->io->write(fs, sb, 512, 512 + (off_t)fs->base)
            : v7fs_write_block(fs, V7_SUPERB, sb))
        return -EIO;
    fs->fl_dirty = 0;   /* the on-disk free list now matches the in-core one */
    return 0;
}

/* ---- V8-family superblock codec (Eighth/Ninth/Tenth Edition) -------------- */

int v8_sb_decode(filsys_edition_t *fs, const uint8_t *sb) {
    fs->isize  = fs->bo->get16(sb + V8_SB_ISIZE);
    fs->fsize  = fs->bo->get32(sb + V8_SB_FSIZE);
    fs->fl.ninode = fs->bo->get16(sb + V8_SB_NINODE);
    for (int i = 0; i < V7_NICINOD; i++)
        fs->fl.inode[i] = fs->bo->get16(sb + V8_SB_INODE + 2 * i);
    fs->time   = fs->bo->get32(sb + V8_SB_TIME);
    fs->fmod   = sb[V8_SB_FMOD];
    fs->fl.tfree  = fs->bo->get32(sb + V8_SB_TFREE);
    fs->fl.tinode = fs->bo->get16(sb + V8_SB_TINODE);
    if (fs->freemap == V8_FREEMAP_LIST) {
        fs->fl.nfree = fs->bo->get16(sb + V8_SB_NFREE);
        for (int i = 0; i < fs->nicfree; i++)
            fs->fl.free[i] = fs->bo->get32(sb + V8_SB_FREE + 4 * i);
    } else {
        fs->fl.nfree = 0;   /* bitmap: no free-list cache */
    }
    if (fs->fl.nfree > fs->nicfree || fs->fl.ninode > fs->nicinod)
        return -EINVAL;
    return 0;
}

int v8_sb_encode(filsys_edition_t *fs, uint8_t *sb) {
    fs->bo->put16(sb + V8_SB_ISIZE, fs->isize);
    fs->bo->put32(sb + V8_SB_FSIZE, fs->fsize);
    fs->bo->put16(sb + V8_SB_NINODE, fs->fl.ninode);
    for (int i = 0; i < V7_NICINOD; i++)
        fs->bo->put16(sb + V8_SB_INODE + 2 * i, fs->fl.inode[i]);
    fs->bo->put32(sb + V8_SB_TIME, (uint32_t)time(NULL));
    sb[V8_SB_FMOD] = (uint8_t)(fs->fmod != 0);
    fs->bo->put32(sb + V8_SB_TFREE, fs->fl.tfree);
    fs->bo->put16(sb + V8_SB_TINODE, (uint16_t)fs->fl.tinode);
    if (fs->freemap == V8_FREEMAP_LIST) {
        fs->bo->put16(sb + V8_SB_NFREE, fs->fl.nfree);
        for (int i = 0; i < fs->nicfree; i++)
            fs->bo->put32(sb + V8_SB_FREE + 4 * i, fs->fl.free[i]);
    } else if (fs->freemap == V8_FREEMAP_BITMAP) {
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
            fs->bo->put32(sb + V8_SB_BFREE + 4 * w, word);
        }
    } else {   /* out-of-superblock bitmap (v10): S_flag=1, S_bsize=BSIZE*8 */
        sb[V8_SB_VALID] = 1;
        sb[V8_SB_FLAG] = 1;
        fs->bo->put32(sb + V8_SB_BSIZE, fs->bsize * 8);
    }
    return 0;
}

/* ---- V8-family bitmap allocator (in-superblock and out-of-superblock) ----- */

/* Load the V8-family bitmap into fs->v8_bits (heap).  sb is the block-1 bytes
 * for the in-superblock form; the out-of-superblock form reads its bitmap
 * blocks from disk.  bit i is set iff the block is free. */
int v8_bitmap_load(filsys_edition_t *fs, const uint8_t *sb)
{
    if (fs->freemap == V8_FREEMAP_BIGMAP) {
        fs->v8_base = 0;
        fs->v8_nbits = fs->fsize;
        uint32_t bits_per_blk = fs->bsize * 8;
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
    fs->v8_bits = calloc((size_t)(fs->v8_nbits + 7) / 8, 1);
    if (!fs->v8_bits)
        return -ENOMEM;
    if (fs->freemap == V8_FREEMAP_BITMAP) {
        for (uint32_t w = 0; w < (fs->v8_nbits + 31) / 32; w++) {
            uint32_t word = fs->bo->get32(sb + V8_SB_BFREE + 4 * w);
            for (uint32_t b = 0; b < 32 && w * 32 + b < fs->v8_nbits; b++)
                if (word & (1u << b))
                    fs->v8_bits[(w * 32 + b) >> 3] |= (uint8_t)(1u << ((w * 32 + b) & 7));
        }
    } else {
        for (uint32_t k = 0; k < fs->v8_nblks; k++) {
            uint8_t blk[V7_MAXBSIZE];
            if (v7fs_read_block(fs, fs->v8_blk_start + k, blk))
                return -EIO;
            uint32_t nbytes = fs->bsize;
            if (k == fs->v8_nblks - 1)
                nbytes = (fs->v8_nbits - k * fs->bsize * 8 + 7) / 8;
            memcpy(fs->v8_bits + k * fs->bsize, blk, nbytes);
        }
    }
    return 0;
}

static int v8_bitmap_balloc(filsys_edition_t *fs, uint32_t *bno)
{
    for (uint32_t i = 0; i < fs->v8_nbits; i++) {
        if (fs->v8_bits[i >> 3] & (uint8_t)(1u << (i & 7))) {
            fs->v8_bits[i >> 3] &= (uint8_t)~(1u << (i & 7));
            uint32_t blk = fs->v8_base + i;
            /* Zero the freshly-allocated block: alloc() clrbuf()s it, so a new
             * file's partial block can't leak the previous file's data. */
            uint8_t z[V7_MAXBSIZE];
            memset(z, 0, fs->bsize);
            if (v7fs_write_block(fs, blk, z))
                return -EIO;
            if (fs->fl.tfree) fs->fl.tfree--;
            fs->fl_dirty = 1;
            *bno = blk;
            return 0;
        }
    }
    return -ENOSPC;
}

static void v8_bitmap_bfree(filsys_edition_t *fs, uint32_t bno)
{
    if (bno < fs->v8_base || bno >= fs->v8_base + fs->v8_nbits)
        return;
    uint32_t i = bno - fs->v8_base;
    fs->v8_bits[i >> 3] |= (uint8_t)(1u << (i & 7));
    fs->fl.tfree++;
    fs->fl_dirty = 1;
}

/* Flush the bitmap and the superblock totals.  The in-superblock form rides
 * super_write (v8_sb_encode writes S_bfree); the out-of-superblock form writes
 * its bitmap blocks then the superblock. */
static int v8_bitmap_sync(filsys_edition_t *fs)
{
    if (fs->readonly)
        return 0;
    if (fs->freemap == V8_FREEMAP_BIGMAP) {
        for (uint32_t k = 0; k < fs->v8_nblks; k++) {
            uint8_t blk[V7_MAXBSIZE] = {0};
            uint32_t nbytes = fs->bsize;
            if (k == fs->v8_nblks - 1)
                nbytes = (fs->v8_nbits - k * fs->bsize * 8 + 7) / 8;
            memcpy(blk, fs->v8_bits + k * fs->bsize, nbytes);
            if (v7fs_write_block(fs, fs->v8_blk_start + k, blk))
                return -EIO;
        }
    }
    return super_write(fs);
}

const alloc_ops_t v8_bitmap_alloc_ops = {
    .balloc = (int  (*)(void *, uint32_t *))v8_bitmap_balloc,
    .bfree  = (void (*)(void *, uint32_t))v8_bitmap_bfree,
    .ialloc = (int  (*)(void *, uint32_t *))v7fs_ialloc,
    .ifree  = (void (*)(void *, uint32_t))v7fs_ifree,
    .sync   = (int  (*)(void *))v8_bitmap_sync,
};

/* ---- inode io ---------------------------------------------------------- */

int v7fs_read_inode(filsys_edition_t *fs, uint32_t ino, v7_inode_t *ip) {
    if (ino == 0)
        return -EINVAL;
    uint32_t bno = v7_itod(fs, ino);
    uint32_t off = v7_itoo(fs, ino);
    if (bno >= fs->isize)   /* i-list lives in blocks 2..s_isize-1 */
        return -EINVAL;
    uint8_t raw[V7_MAXBSIZE];
    if (v7fs_read_block(fs, bno, raw))
        return -EIO;
    const uint8_t *d = raw + off * fs->inode_size;
    memset(ip, 0, sizeof(*ip));
    ip->ino   = ino;
    ip->mode  = fs->bo->get16(d + 0);
    ip->nlink = (int16_t)fs->bo->get16(d + 2);
    ip->uid   = (int16_t)fs->bo->get16(d + 4);
    ip->gid   = (int16_t)fs->bo->get16(d + 6);
    ip->size  = fs->bo->get32(d + 8);
    for (int i = 0; i < fs->niaddr; i++)
        ip->addr[i] = fs->addr_width == 4
                    ? fs->bo->get32(d + 12 + 4 * i)
                    : fs->bo->get24(d + 12 + 3 * i);
    ip->atime = fs->bo->get32(d + 52);
    ip->mtime = fs->bo->get32(d + 56);
    ip->ctime = fs->bo->get32(d + 60);
    return 0;
}

int v7fs_write_inode(filsys_edition_t *fs, uint32_t ino, const v7_inode_t *ip) {
    if (ino == 0)
        return -EINVAL;
    /* Allocator state before reference: a block or inode allocated since the
     * last superblock flush is still listed free on disk.  Flush it first so
     * this inode's reference to it cannot leave it both free and referenced
     * (aliasing, which nothing repairs). */
    if (fs->fl_dirty) {
        int rc = super_write(fs);
        if (rc)
            return rc;
    }
    uint32_t bno = v7_itod(fs, ino);
    uint32_t off = v7_itoo(fs, ino);
    if (bno >= fs->isize)
        return -EINVAL;
    uint8_t raw[V7_MAXBSIZE];
    if (v7fs_read_block(fs, bno, raw))
        return -EIO;
    uint8_t *d = raw + off * fs->inode_size;
    fs->bo->put16(d + 0, (uint16_t)ip->mode);
    fs->bo->put16(d + 2, (uint16_t)ip->nlink);
    fs->bo->put16(d + 4, (uint16_t)ip->uid);
    fs->bo->put16(d + 6, (uint16_t)ip->gid);
    fs->bo->put32(d + 8, ip->size);
    for (int i = 0; i < fs->niaddr; i++) {
        if (fs->addr_width == 4)
            fs->bo->put32(d + 12 + 4 * i, ip->addr[i]);
        else
            fs->bo->put24(d + 12 + 3 * i, ip->addr[i]);
    }
    fs->bo->put32(d + 52, ip->atime);
    fs->bo->put32(d + 56, ip->mtime);
    fs->bo->put32(d + 60, ip->ctime);
    return v7fs_write_block(fs, bno, raw);
}

/* ---- allocation -------------------------------------------------------- */

int v7fs_balloc(filsys_edition_t *fs, uint32_t *bno) {
    if (fs->freemap != V8_FREEMAP_LIST)
        return v8_bitmap_balloc(fs, bno);
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
        fs->fl.nfree = fs->df_nfree_wid == 4 ? fs->bo->get32(buf + 0)
                                             : fs->bo->get16(buf + 0);
        for (int i = 0; i < fs->nicfree; i++)
            fs->fl.free[i] = v7_get_daddr(fs, buf + v7_chain_free_off(fs) + fs->daddr_wid * i);
        /* blk was the on-disk chain head; its contents (the next segment) are
         * now in the cache, and blk is about to become file data.  Commit the
         * allocator state *before* the caller overwrites blk, else a crash would
         * leave the on-disk chain naming a block full of file data -- the seed
         * of aliasing, not a leak.  One flush per NICFREE blocks, not per block. */
        if (super_write(fs))
            return -EIO;
    }
    /* Zero the freshly-allocated block: V7's alloc() clrbuf()s it, and without
     * this the previous file's data leaks into a new file. */
    uint8_t z[V7_MAXBSIZE];
    memset(z, 0, fs->bsize);
    if (v7fs_write_block(fs, blk, z))
        return -EIO;
    if (fs->fl.tfree) fs->fl.tfree--;
    /* A block just left the free list; until the superblock is flushed the
     * on-disk free list still lists it as free.  Record that so write_inode can
     * flush first, closing the "free and referenced" aliasing window. */
    fs->fl_dirty = 1;
    *bno = blk;
    return 0;
}

void v7fs_bfree(filsys_edition_t *fs, uint32_t bno) {
    if (fs->freemap != V8_FREEMAP_LIST) {
        v8_bitmap_bfree(fs, bno);
        return;
    }
    if (bno < v7_data_first(fs) || bno >= fs->fsize)
        return;   /* badblock */
    if (fs->fl.nfree == 0) {
        fs->fl.nfree = 1;
        fs->fl.free[0] = 0;
    }
    if (fs->fl.nfree >= fs->nicfree) {
        uint8_t buf[V7_MAXBSIZE];
        memset(buf, 0, fs->bsize);
        if (fs->df_nfree_wid == 4)
            fs->bo->put32(buf + 0, fs->fl.nfree);
        else
            fs->bo->put16(buf + 0, fs->fl.nfree);
        for (int i = 0; i < fs->nicfree; i++)
            v7_put_daddr(fs, buf + v7_chain_free_off(fs) + fs->daddr_wid * i, fs->fl.free[i]);
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
}

int v7fs_ialloc(filsys_edition_t *fs, uint32_t *ino) {
    uint32_t maxino = v7_maxinode(fs);

    for (;;) {
        if (fs->fl.ninode > 0) {
            uint32_t cand = fs->fl.inode[--fs->fl.ninode];
            if (cand < 2 || cand > maxino)
                continue;   /* bad cache entry: skip */
            v7_inode_t ip;
            if (fs->ops->inode->read_inode(fs, cand, &ip))
                return -EIO;   /* read error, not "in use": don't reclassify */
            if (ip.mode != 0)
                continue;   /* was already allocated; look again */
            memset(&ip, 0, sizeof(ip));
            ip.ino = cand;
            fs->ops->inode->write_inode(fs, cand, &ip);
            if (fs->fl.tinode) fs->fl.tinode--;
            fs->fl_dirty = 1;   /* inode cache changed: flush before reference */
            *ino = cand;
            return 0;
        }
        /* Refill the cache with a linear scan of the i-list. */
        fs->fl.ninode = 0;
        for (uint32_t in = 2; in <= maxino && fs->fl.ninode < fs->nicinod; in++) {
            v7_inode_t ip;
            if (fs->ops->inode->read_inode(fs, in, &ip))
                return -EIO;   /* a mid-scan read error must propagate, not truncate */
            if (ip.mode == 0)
                fs->fl.inode[fs->fl.ninode++] = (uint16_t)in;
        }
        if (fs->fl.ninode == 0)
            return -ENOSPC;
    }
}

void v7fs_ifree(filsys_edition_t *fs, uint32_t ino) {
    if (fs->fl.ninode >= fs->nicinod)
        return;   /* kernel discards beyond the cache */
    fs->fl.inode[fs->fl.ninode++] = (uint16_t)ino;
    fs->fl.tinode++;
}

/* ---- block mapping ------------------------------------------------------ */

/* Follow an indirect chain of `levels` levels (1/2/3) from *slot.
 * indices[0] is the outermost index.  Allocates when create is set. */
static int ind_follow(filsys_edition_t *fs, uint32_t *slot, int levels,
                      const uint32_t *indices, int create, uint32_t *out) {
    uint32_t blk = *slot;
    for (int L = 0; L < levels; L++) {
        if (blk == 0) {
            if (!create) {
                *out = 0;
                return 0;
            }
            uint8_t z[V7_MAXBSIZE];
            memset(z, 0, fs->bsize);
            int rc = v7fs_balloc(fs, &blk);
            if (rc)
                return rc;   /* -EIO (barrier commit) or -ENOSPC */
            if (v7fs_write_block(fs, blk, z))
                return -EIO;
            *slot = blk;
        }
        uint8_t buf[V7_MAXBSIZE];
        if (v7fs_read_block(fs, blk, buf))
            return -EIO;
        uint32_t next = fs->bo->get32(buf + 4 * indices[L]);

        if (L == levels - 1) {
            if (next == 0 && create) {
                int rc = v7fs_balloc(fs, &next);
                if (rc)
                    return rc;   /* -EIO (barrier commit) or -ENOSPC */
                fs->bo->put32(buf + 4 * indices[L], next);
                if (v7fs_write_block(fs, blk, buf))
                    return -EIO;
            }
            *out = next;
            return 0;
        }
        if (next == 0) {
            if (!create) {
                *out = 0;
                return 0;
            }
            uint8_t z[V7_MAXBSIZE];
            memset(z, 0, fs->bsize);
            int rc = v7fs_balloc(fs, &next);
            if (rc)
                return rc;   /* -EIO (barrier commit) or -ENOSPC */
            if (v7fs_write_block(fs, next, z))
                return -EIO;
            fs->bo->put32(buf + 4 * indices[L], next);
            if (v7fs_write_block(fs, blk, buf))
                return -EIO;
        }
        blk = next;
    }
    return -EIO;   /* unreachable */
}

int v7fs_bmap(filsys_edition_t *fs, v7_inode_t *ip, uint32_t lbn, int create, uint32_t *bno) {
    if (lbn < (uint32_t)fs->ndaddr) {
        uint32_t nb = ip->addr[lbn];
        if (nb == 0 && create) {
            int rc = v7fs_balloc(fs, &nb);
            if (rc)
                return rc;   /* -EIO (barrier commit) or -ENOSPC */
            ip->addr[lbn] = nb;
        }
        *bno = nb;
        return 0;
    }
    uint32_t r = lbn - fs->ndaddr;
    if (r < v7_nindir(fs))
        return ind_follow(fs, &ip->addr[fs->ndaddr], 1, &r, create, bno);
    r -= v7_nindir(fs);
    if (r < (uint32_t)v7_nindir(fs) * v7_nindir(fs)) {
        uint32_t idx[2] = { r / v7_nindir(fs), r % v7_nindir(fs) };
        return ind_follow(fs, &ip->addr[fs->ndaddr + 1], 2, idx, create, bno);
    }
    r -= (uint32_t)v7_nindir(fs) * v7_nindir(fs);
    uint32_t idx[3] = { r / (v7_nindir(fs) * v7_nindir(fs)),
                        (r / v7_nindir(fs)) % v7_nindir(fs), r % v7_nindir(fs) };
    return ind_follow(fs, &ip->addr[fs->ndaddr + 2], 3, idx, create, bno);
}

/* Count the blocks in an indirect chain of `levels` levels, including the
 * indirect blocks themselves (they are allocated and do count toward
 * st_blocks).  A zero entry is a hole and contributes nothing. */
static uint64_t v7_ind_count(filsys_edition_t *fs, uint32_t blk, int levels) {
    if (blk == 0)
        return 0;
    uint8_t buf[V7_MAXBSIZE];
    if (v7fs_read_block(fs, blk, buf))
        return 0;   /* unreadable: report nothing rather than fail a stat */
    uint64_t n = 1;   /* the indirect block itself */
    for (uint32_t i = 0; i < v7_nindir(fs); i++) {
        uint32_t sub = fs->bo->get32(buf + 4 * i);
        if (sub == 0)
            continue;
        n += levels == 1 ? 1 : v7_ind_count(fs, sub, levels - 1);
    }
    return n;
}

static uint64_t v7fs_allocated_blocks(filsys_edition_t *fs, const filsys_inode_t *ip) {
    uint64_t n = 0;
    for (int i = 0; i < fs->ndaddr; i++)
        if (ip->addr[i]) n++;
    n += v7_ind_count(fs, ip->addr[fs->ndaddr], 1);      /* single */
    n += v7_ind_count(fs, ip->addr[fs->ndaddr + 1], 2);  /* double */
    n += v7_ind_count(fs, ip->addr[fs->ndaddr + 2], 3);  /* triple */
    return n;
}

/* ---- file data ---------------------------------------------------------- */

ssize_t v7fs_file_read(filsys_edition_t *fs, v7_inode_t *ip, uint8_t *buf, size_t size, off_t off) {
    if (off < 0)
        return -EINVAL;
    if ((uint64_t)off >= ip->size)
        return 0;
    uint64_t remaining = ip->size - (uint64_t)off;
    if (size > remaining)
        size = (size_t)remaining;

    size_t done = 0;
    while (done < size) {
        uint32_t lbn  = (uint32_t)((off + (off_t)done) / fs->bsize);
        uint32_t boff = (uint32_t)((off + (off_t)done) % fs->bsize);
        uint32_t pbn;
        if (fs->ops->inode->bmap(fs, ip, lbn, 0, &pbn))
            return -EIO;
        uint8_t blk[V7_MAXBSIZE];
        if (pbn == 0) {
            memset(blk, 0, fs->bsize);   /* sparse hole */
        } else if (fs->ops->blk_get(fs, pbn, blk)) {
            return -EIO;
        }
        size_t n = fs->bsize - boff;
        if (n > size - done)
            n = size - done;
        memcpy(buf + done, blk + boff, n);
        done += n;
    }
    return (ssize_t)done;
}

ssize_t v7fs_file_write(filsys_edition_t *fs, v7_inode_t *ip, const uint8_t *buf, size_t size, off_t off) {
    if (off < 0)
        return -EINVAL;

    size_t done = 0;
    while (done < size) {
        uint32_t lbn  = (uint32_t)((off + (off_t)done) / fs->bsize);
        uint32_t boff = (uint32_t)((off + (off_t)done) % fs->bsize);
        uint32_t pbn;
        int rc = fs->ops->inode->bmap(fs, ip, lbn, 1, &pbn);
        if (rc)
            return rc;   /* -EIO (allocator commit) or -ENOSPC or -EFBIG */
        if (pbn == 0)
            return -ENOSPC;

        uint8_t blk[V7_MAXBSIZE];
        if (fs->ops->blk_get(fs, pbn, blk))
            return -EIO;

        size_t n = fs->bsize - boff;
        if (n > size - done)
            n = size - done;
        memcpy(blk + boff, buf + done, n);
        if (fs->ops->blk_put(fs, pbn, blk))
            return -EIO;
        done += n;
    }
    if ((uint64_t)off + size > ip->size)
        ip->size = (uint32_t)((uint64_t)off + size);
    int rc = fs->ops->inode->write_inode(fs, ip->ino, ip);
    if (rc)
        return rc;   /* the inode write failed: don't report a partial write */
    return (ssize_t)done;
}

/* ---- directories -------------------------------------------------------- */

int v7fs_dir_read(filsys_edition_t *fs, v7_inode_t *ip, v7_dirent_t **ents, size_t *count) {
    if (!fs_is_dir(fs, ip))
        return -ENOTDIR;
    /* A directory's data cannot exceed the filesystem's data area; reject a
     * corrupt size before the malloc below, else a bogus di_size (up to 4 GiB)
     * turns into a multi-gigabyte allocation. */
    if (ip->size > (uint64_t)(fs->fsize - fs->ops->data_start(fs)) * fs->bsize)
        return -EFBIG;
    size_t cap = ip->size / fs->dirent_size + 1;
    v7_dirent_t *out = calloc(cap, sizeof(v7_dirent_t));
    if (!out)
        return -ENOMEM;

    uint8_t *buf = malloc(ip->size);
    if (!buf) {
        free(out);
        return -ENOMEM;
    }
    ssize_t n = v7fs_file_read(fs, ip, buf, ip->size, 0);
    if (n < 0) {
        free(buf);
        free(out);
        return (int)n;
    }

    size_t cnt = 0;
    for (size_t off = 0; off + fs->dirent_size <= (size_t)n; off += fs->dirent_size) {
        uint16_t ino = fs->bo->get16(buf + off);
        if (ino == 0)
            continue;
        out[cnt].ino = ino;
        memcpy(out[cnt].name, buf + off + 2, fs->max_namlen);
        out[cnt].name[fs->max_namlen] = 0;
        cnt++;
    }
    free(buf);
    *ents = out;
    *count = cnt;
    return 0;
}

void v7fs_dirents_free(v7_dirent_t *ents) {
    free(ents);
}

int v7fs_dir_lookup(filsys_edition_t *fs, v7_inode_t *ip, const char *name, uint32_t *ino) {
    v7_dirent_t *ents = NULL;
    size_t count = 0;
    int rc = fs->ops->dir->dir_read(fs, ip, &ents, &count);
    if (rc)
        return rc;
    rc = -ENOENT;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(ents[i].name, name) == 0) {
            *ino = ents[i].ino;
            rc = 0;
            break;
        }
    }
    v7fs_dirents_free(ents);
    return rc;
}

int v7fs_dir_add(filsys_edition_t *fs, v7_inode_t *ip, uint32_t ino, const char *name) {
    size_t namelen = strlen(name);
    if (namelen == 0 || namelen > fs->max_namlen)
        return -ENAMETOOLONG;
    if (strchr(name, '/'))
        return -EINVAL;

    size_t newsize = ip->size + fs->dirent_size;
    uint8_t *buf = malloc(newsize);
    if (!buf)
        return -ENOMEM;
    memset(buf, 0, newsize);
    ssize_t n = v7fs_file_read(fs, ip, buf, ip->size, 0);
    if (n < 0) {
        free(buf);
        return (int)n;
    }

    /* find an empty slot, else append */
    size_t slot = SIZE_MAX;
    for (size_t off = 0; off + fs->dirent_size <= (size_t)n; off += fs->dirent_size) {
        if (fs->bo->get16(buf + off) == 0) {
            slot = off;
            break;
        }
    }
    if (slot == SIZE_MAX) {
        slot = (size_t)n;
        n += fs->dirent_size;
    }

    fs->bo->put16(buf + slot, (uint16_t)ino);
    memset(buf + slot + 2, 0, fs->max_namlen);
    memcpy(buf + slot + 2, name, namelen);

    ssize_t w = v7fs_file_write(fs, ip, buf, (size_t)n, 0);
    free(buf);
    return w < 0 ? (int)w : 0;
}

int v7fs_dir_remove(filsys_edition_t *fs, v7_inode_t *ip, const char *name) {
    uint8_t *buf = malloc(ip->size);
    if (!buf)
        return -ENOMEM;
    ssize_t n = v7fs_file_read(fs, ip, buf, ip->size, 0);
    if (n < 0) {
        free(buf);
        return (int)n;
    }
    int rc = -ENOENT;
    for (size_t off = 0; off + fs->dirent_size <= (size_t)n; off += fs->dirent_size) {
        if (fs->bo->get16(buf + off) == 0)
            continue;
        char ent[64];
        memcpy(ent, buf + off + 2, fs->max_namlen);
        ent[fs->max_namlen] = 0;
        if (strcmp(ent, name) == 0) {
            fs->bo->put16(buf + off, 0);
            memset(buf + off + 2, 0, fs->max_namlen);
            ssize_t w = v7fs_file_write(fs, ip, buf, (size_t)n, 0);
            rc = w < 0 ? (int)w : 0;
            break;
        }
    }
    free(buf);
    return rc;
}

/* ---- path lookup -------------------------------------------------------- */

int v7fs_lookup(filsys_edition_t *fs, const char *path, uint32_t *ino, v7_inode_t *ip) {
    if (path[0] != '/')
        return -EINVAL;
    uint32_t cur = fs->rootino;
    v7_inode_t dip;
    if (fs->ops->inode->read_inode(fs, cur, &dip))
        return -EIO;

    const char *p = path + 1;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (len == 0) {
            p++;
            continue;
        }
        if (len > fs->max_namlen)
            return -ENAMETOOLONG;
        char name[64];
        memcpy(name, p, len);
        name[len] = 0;

        if (!fs_is_dir(fs, &dip))
            return -ENOTDIR;
        uint32_t next;
        int rc = fs->ops->dir_lookup(fs, &dip, name, &next);
        if (rc)
            return rc;
        if (fs->ops->inode->read_inode(fs, next, &dip))
            return -EIO;
        p = slash ? slash + 1 : p + len;
    }
    *ino = dip.ino;
    if (ip)
        *ip = dip;
    return 0;
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
/* Rebuild the V8-family bitmap free-space from the used-block map.  Used by
 * mkfs (fresh image) and by fsck -s salvage.  bit i is set iff free. */
static uint32_t v8_makefree_bitmap(filsys_edition_t *fs, filsys_chkctx_t *cx)
{
    filsys_edition_t *f = fs;
    uint32_t base, nbits;
    if (f->freemap == V8_FREEMAP_BIGMAP) {
        uint32_t bits_per_blk = f->bsize * 8;
        f->v8_nblks = (f->fsize + bits_per_blk - 1) / bits_per_blk;
        f->v8_blk_start = f->fsize - f->v8_nblks;
        base = 0; nbits = f->fsize;
    } else {
        f->v8_nblks = 0;
        base = f->isize; nbits = f->fsize - f->isize;
    }
    f->v8_base = base;
    f->v8_nbits = nbits;
    free(f->v8_bits);
    f->v8_bits = calloc((size_t)(nbits + 7) / 8, 1);
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

/* Walk the V8-family bitmap as alloc() would, marking free blocks into cx->inode->bmap
 * (a free block already used is a duplicate) and counting free_blocks. */
static void v8_walk_free_bitmap(filsys_edition_t *fs, filsys_chkctx_t *cx,
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

static uint32_t v7fs_makefree(filsys_edition_t *fs, filsys_chkctx_t *cx)
{
    filsys_edition_t *f = fs;
    if (f->freemap != V8_FREEMAP_LIST)
        return v8_makefree_bitmap(fs, cx);
    uint32_t m, n;
    if (f->interleave) {
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
        if (v7fs_read_inode(f, ino, &ip) == 0 && ip.mode != 0)
            used++;
    }
    f->fl.tinode = maxino - used;
    super_write(f);   /* writes f->fl.tfree (= nfree) and f->fl.tinode */
    return nfree;
}

/* Classify an inode's mode into a checker state (the low three type bits). */
static uint8_t v7_inode_state(filsys_edition_t *fs, uint32_t ino, uint32_t mode) {
    (void)fs; (void)ino;
    if (mode == 0)
        return FILSYS_IN_UNALLOC;
    switch (mode & V7_IFMT) {
    case V7_IFDIR: return FILSYS_IN_IDIR;
    case V7_IFREG: case V8_IFLNK: return FILSYS_IN_IREG;
    case V7_IFCHR: case V7_IFMPC: return FILSYS_IN_ICHR;
    case V7_IFBLK: case V7_IFMPB: return FILSYS_IN_IBLK;
    case V7_IFIFO: return FILSYS_IN_IPIPE;
    default: return FILSYS_IN_UNKNOWN;
    }
}

static uint32_t v7_maxino(filsys_edition_t *fs)    { return v7_maxinode(fs); }
static uint32_t v7_data_start(filsys_edition_t *fs) { return v7_data_first(fs); }
static uint32_t v7_data_end(filsys_edition_t *fs) {
    /* The out-of-superblock bitmap's blocks sit at the tail as metadata, not
     * free or data blocks; exclude them from the checker's data band. */
    return ((filsys_edition_t *)fs)->fsize - ((filsys_edition_t *)fs)->v8_nblks;
}
/* Walk the free list exactly as alloc() would, marking free blocks into cx->inode->bmap
 * (a free block already used is a duplicate) and counting free_blocks. */
static void v7_walk_free(filsys_edition_t *fs, filsys_chkctx_t *cx, filsys_check_t *rep)
{
    filsys_edition_t *f = fs;
    if (f->freemap != V8_FREEMAP_LIST) {
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
        if (++guard > f->fsize + f->nicfree) {
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
            n = f->df_nfree_wid == 4 ? f->bo->get32(blk) : f->bo->get16(blk);
            if (n > f->nicfree) {
                printf("free-list block %u has bad count %u\n", bno, n);
                rep->errors++;
                break;
            }
            for (int i = 0; i < f->nicfree; i++)
                cur[i] = v7_get_daddr(f, blk + v7_chain_free_off(f) + f->daddr_wid * i);
        }
    }
    free(seen);
}

int v7fs_check(filsys_edition_t *fs, v7_check_t *rep, int mode) {
    return filsys_check_common(fs, fs, rep, mode);
}

/* ---- ops table ----------------------------------------------------------
 * Each op takes `void *` (the backend state).  The adapters forward to the
 * typed backend function; the `void *` argument converts implicitly to
 * filsys_edition_t*, so there is no cast anywhere. */


static uint32_t v7fs_blocksize_op(const filsys_edition_t *fs) {
    return ((const filsys_edition_t *)fs)->bsize;
}



















static void v7fs_statfs_op(filsys_edition_t *fs, struct statvfs *st) {
    filsys_edition_t *v7 = fs;
    st->f_blocks = v7->fsize;
    st->f_bfree = st->f_bavail = v7->fl.tfree;
    st->f_files = (v7->isize - 2) * v7_inopb(v7);
    st->f_ffree = v7->fl.tinode;
}
/* ---- read-only superblock probes (Fold 1) ---------------------------------
 * Moved from findfs.filsys.c and reworked to read through `io` and to derive
 * the layout from the edition descriptor (`fmt->pack4`/`magic`/`bsize`/`bo`/
 * `nicfree`/`sb_decode`) and the shared sb_*_off() helpers, so the probe and
 * the superblock reader in v7fs_open agree on every field offset by
 * construction rather than by hand-copied constants.  Each probe returns 1
 * (validated, res->class set), 2 (near-miss, res->why set) or 0 (not this
 * format). */

static int probe_walk_chain(filsys_edition_t *fmt, const filsys_io_t *io,
                            uint32_t head, uint32_t isize, uint32_t fsize,
                            uint64_t base, int bsize, int nicfree, int wid,
                            uint16_t (*g16)(const uint8_t *),
                            uint32_t (*g32)(const uint8_t *), int fb_free,
                            uint32_t *segs, const char **why)
{
    uint32_t hops = 0, next = 0;
    uint8_t *buf = malloc((size_t)bsize);
    uint8_t *seen = calloc((size_t)((fsize + 7) / 8), 1);
    *segs = 0;
    *why = NULL;
    if (!buf || !seen) {
        *why = "out of memory";
        free(buf); free(seen);
        return -1;
    }
    while (head != 0) {
        if (head < isize || head >= fsize)        { *why = "chain leaves fs"; break; }
        if (hops++ > (uint32_t)(fsize / nicfree)) { *why = "chain too long"; break; }
        if (seen[head >> 3] & (1u << (head & 7))) { *why = "chain cycles"; break; }
        seen[head >> 3] |= (uint8_t)(1u << (head & 7));
        if (io->read(fmt, buf, bsize, (off_t)(base + (uint64_t)head * bsize)) != 0) {
            *why = "chain read error"; break;
        }
        uint32_t nfree = fb_free == 4 ? g32(buf + 0) : g16(buf + 0);
        if (nfree < 1 || nfree > (uint32_t)nicfree) { *why = "bad segment nfree"; break; }
        int bad = 0;
        next = 0;
        for (uint32_t i = 0; i < nfree; i++) {
            uint32_t blk = wid == 2 ? g16(buf + fb_free + 2 * i)
                                    : g32(buf + fb_free + 4 * i);
            if (i == 0)
                next = blk;
            else if (blk != 0 && (blk < isize || blk >= fsize)) { bad = 1; break; }
        }
        if (bad) { *why = "chain entry out of range"; break; }
        head = next;
        (*segs)++;
    }
    int rc = *why ? -1 : 0;
    free(buf);
    free(seen);
    return rc;
}

static int probe_root_ok(filsys_edition_t *fmt, const filsys_io_t *io,
                         uint64_t base, int bsize,
                         uint16_t (*g16)(const uint8_t *),
                         uint32_t (*g32)(const uint8_t *), const char **why)
{
    uint8_t ib[128];
    if (io->read(fmt, ib, sizeof ib, (off_t)(base + (uint64_t)2 * bsize)) != 0) {
        *why = "cannot read i-list"; return -1;
    }
    const uint8_t *d = ib + 64;
    uint16_t mode = g16(d + 0);
    if ((mode & 0170000) != 0040000) { *why = "root inode is not a directory"; return -1; }
    uint32_t size = g32(d + 8);
    if (size == 0)                    { *why = "root inode size 0"; return -1; }
    if (size % 16 != 0)               { *why = "root inode size not dirent-aligned"; return -1; }
    return 0;
}

/* ---- V8-family traversal helpers (Eighth/Ninth/Tenth Edition) ------------- */

static int probe_v8_ilist_ok(filsys_edition_t *fmt, const filsys_io_t *io,
                             uint64_t base, int bsize, uint32_t isize,
                             uint32_t fsize, int le, uint32_t maxino, const char **why)
{
    uint32_t inopb = (uint32_t)bsize / 64;
    uint32_t niblk = (maxino + inopb - 1) / inopb;
    uint8_t blk[V7_MAXBSIZE];
    uint16_t (*g16)(const uint8_t *) = le ? bo_get16le : bo_get16be;
    uint32_t (*g24)(const uint8_t *) = le ? bo_get24le : bo_get24be;
    for (uint32_t b = 0; b < niblk; b++) {
        if (io->read(fmt, blk, bsize, (off_t)(base + (uint64_t)(2 + b) * bsize)) != 0) {
            *why = "cannot read i-list"; return -1;
        }
        for (uint32_t s = 0; s < inopb; s++) {
            uint32_t ino = b * inopb + s + 1;
            if (ino > maxino)
                break;
            const uint8_t *ib = blk + s * 64;
            if (g16(ib + 0) == 0)
                continue;
            for (int i = 0; i < 13; i++) {
                uint32_t a = g24(ib + 12 + 3 * i);
                if (a != 0 && (a < isize || a >= fsize)) {
                    *why = "inode block address out of range";
                    return -1;
                }
            }
        }
    }
    return 0;
}

static int probe_v8_bitmap_count(filsys_edition_t *fmt, const filsys_io_t *io,
                                 const uint8_t *sb, uint64_t base, int bsize,
                                 uint32_t isize, uint32_t fsize, int le, int form,
                                 const char **why)
{
    uint32_t (*g32)(const uint8_t *) = le ? bo_get32le : bo_get32be;
    uint32_t nbits, count = 0;
    if (form == V8_FREEMAP_BIGMAP) {
        uint32_t bits_per_blk = (uint32_t)bsize * 8;
        uint32_t nblks = (fsize + bits_per_blk - 1) / bits_per_blk;
        nbits = fsize;
        uint8_t blk[V7_MAXBSIZE];
        for (uint32_t k = 0; k < nblks; k++) {
            uint32_t dblk = fsize - nblks + k;
            if (io->read(fmt, blk, bsize, (off_t)(base + (uint64_t)dblk * bsize)) != 0) {
                *why = "bitmap block read error"; return -1;
            }
            uint32_t bytes = bsize;
            if (k == nblks - 1)
                bytes = (nbits - k * bits_per_blk + 7) / 8;
            for (uint32_t j = 0; j < bytes; j++) {
                uint8_t x = blk[j];
                while (x) { count += (x & 1); x >>= 1; }
            }
        }
    } else {
        nbits = fsize - isize;
        for (uint32_t w = 0; w < (nbits + 31) / 32; w++) {
            uint32_t word = g32(sb + V8_SB_BFREE + 4 * w);
            for (uint32_t bit = 0; bit < 32 && w * 32 + bit < nbits; bit++)
                if (word & (1u << bit))
                    count++;
        }
    }
    return (int)count;
}

static int probe_v8_try(filsys_edition_t *fmt, const filsys_io_t *io,
                        const uint8_t *b, uint64_t base, int bsize,
                        uint64_t nbytes, int le, uint16_t *isize, uint32_t *fsize,
                        int *form, uint32_t *segs, const char **why)
{
    uint16_t (*g16)(const uint8_t *) = le ? bo_get16le : bo_get16be;
    uint32_t (*g32)(const uint8_t *) = le ? bo_get32le : bo_get32be;
    uint32_t (*g24)(const uint8_t *) = le ? bo_get24le : bo_get24be;

    uint16_t isz = g16(b + V8_SB_ISIZE);
    if (isz < 3)
        return 0;
    uint32_t fsz = g32(b + V8_SB_FSIZE);
    uint16_t ninode = g16(b + V8_SB_NINODE);
    if (ninode > V7_NICINOD)
        return 0;
    uint32_t inopb = (uint32_t)bsize / 64;
    uint32_t maxino = (uint32_t)(isz - 2) * inopb;
    if (maxino == 0 || maxino > 65536)
        return 0;
    for (int i = 0; i < ninode; i++) {
        uint16_t ino = g16(b + V8_SB_INODE + 2 * i);
        if (ino != 0 && ino > maxino)
            return 0;
    }
    if (b[V8_SB_FMOD] > 1)
        return 0;
    uint32_t tfree = g32(b + V8_SB_TFREE);
    uint16_t tinode = g16(b + V8_SB_TINODE);
    if (tfree > fsz - isz || tinode > maxino)
        return 0;
    for (int i = 0; i < 14; i++) {
        uint8_t c = b[V8_SB_FSMNT + i];
        if (c != 0 && (c < 0x20 || c > 0x7e))
            return 0;
    }

    if (fsz <= isz || fsz > (1u << 24))
        return 0;
    if ((uint64_t)fsz * bsize > nbytes + (uint64_t)bsize)
        return 0;

    *isize = isz; *fsize = fsz;

    uint8_t ib[128];
    if (io->read(fmt, ib, sizeof ib, (off_t)(base + 2 * (uint64_t)bsize)) != 0) {
        *why = "cannot read i-list"; return -1;
    }
    const uint8_t *d = ib + 64;
    if ((g16(d + 0) & 0170000) != 0040000) { *why = "root inode is not a directory"; return -1; }
    if (g16(d + 2) < 2)                     { *why = "root inode link count < 2"; return -1; }
    uint32_t size = g32(d + 8);
    if (size < 32 || size % 16 != 0)        { *why = "root inode size implausible"; return -1; }
    uint32_t rootblk = g24(d + 12);
    if (rootblk < isz || rootblk >= fsz)    { *why = "root directory block out of range"; return -1; }
    uint8_t rd[64];
    if (io->read(fmt, rd, sizeof rd, (off_t)(base + (uint64_t)rootblk * bsize)) != 0) {
        *why = "cannot read root directory"; return -1;
    }
    if (g16(rd + 0) != 2 || rd[2] != '.' ||
        g16(rd + 16) != 2 || rd[18] != '.' || rd[19] != '.') {
        *why = "root directory entries wrong"; return -1;
    }
    if (probe_v8_ilist_ok(fmt, io, base, bsize, isz, fsz, le, maxino, why) < 0)
        return -1;

    int nicfree = (bsize == 8192) ? V8_NICFREE_LARGE : V8_NICFREE_SMALL;
    uint16_t nfree = g16(b + V8_SB_NFREE);
    uint32_t s = 0;
    const char *why_list = NULL, *why_map = NULL;
    if (nfree <= nicfree) {
        int ok = 1;
        for (int i = 0; i < nfree; i++) {
            uint32_t fb = g32(b + V8_SB_FREE + 4 * i);
            if (fb != 0 && (fb < isz || fb >= fsz)) { ok = 0; break; }
        }
        if (ok && probe_walk_chain(fmt, io, g32(b + V8_SB_FREE), isz, fsz, base, bsize,
                                   nicfree, 4, g16, g32, 4, &s, &why_list) == 0) {
            *form = V8_FREEMAP_LIST;
            *isize = isz; *fsize = fsz; *segs = s;
            return 1;
        }
    }
    int fm = (b[V8_SB_FLAG] == 1 && bsize == 4096) ? V8_FREEMAP_BIGMAP : V8_FREEMAP_BITMAP;
    int cnt = probe_v8_bitmap_count(fmt, io, b, base, bsize, isz, fsz, le, fm, &why_map);
    if (cnt >= 0 && (uint32_t)cnt == tfree) {
        *form = fm;
        *isize = isz; *fsize = fsz; *segs = 0;
        return 1;
    }
    *why = why_list ? why_list : (why_map ? why_map : "free space does not traverse");
    return -1;
}

static int probe_bsd211_root_dir(filsys_edition_t *fmt, const filsys_io_t *io,
                                 uint64_t base, int bsize)
{
    uint8_t ib[128];
    if (io->read(fmt, ib, sizeof ib, (off_t)(base + 2 * (uint64_t)bsize)) != 0)
        return 0;
    const uint8_t *d = ib + 64;
    uint32_t rootblk = bo_get32me(d + 12);
    uint8_t rb[1024];
    if (io->read(fmt, rb, sizeof rb, (off_t)(base + (uint64_t)rootblk * bsize)) != 0)
        return 0;

    uint32_t off = 0;
    int dot = 0, dotdot = 0;
    while (off + 6 <= sizeof rb) {
        uint16_t reclen = bo_get16me(rb + off + 2);
        uint16_t namlen = bo_get16me(rb + off + 4);
        if (reclen < 6 || reclen % 4 != 0 || off + reclen > sizeof rb ||
            namlen < 1 || namlen > 63)
            return 0;
        if (namlen == 1 && rb[off + 6] == '.')
            dot = 1;
        else if (namlen == 2 && rb[off + 6] == '.' && rb[off + 7] == '.')
            dotdot = 1;
        off += reclen;
        if (dot && dotdot)
            return 1;
    }
    return 0;
}

static const char *probe_sysv_class(const uint8_t *b, uint32_t (*g32)(const uint8_t *)) {
    uint32_t st = g32(b + V7_SYSV_STATE_OFF);
    return (st == V7_SYSV_STATE_CLEAN || st == V7_SYSV_STATE_ACTIVE)
               ? "sysvr4" : "sysvr2 or sysvr3";
}

/* ---- family probes ------------------------------------------------------- */

/* V7/32V/Coherent/System III (no magic, no sb_decode, 512-byte blocks): the
 * layout is entirely `fmt` -- pack4 shifts the 4-byte fields, nicfree names the
 * cache depth (50 vs Coherent's 64). */
static int probe_v7_family(filsys_edition_t *fmt, const filsys_io_t *io,
                           uint64_t base, int bsize, uint64_t nbytes, filsys_probe_t *res)
{
    uint8_t sb[V7_MAXBSIZE];
    if (io->read(fmt, sb, bsize, (off_t)(base + (uint64_t)bsize)) != 0)
        return 0;
    uint16_t isz = fmt->bo->get16(sb);
    if (isz < 3)
        return 0;
    int nfree_off  = sb_nfree_off(fmt->pack4);
    int ninode_off = sb_ninode_off(fmt->pack4, fmt->nicfree);
    int free_off   = sb_free_off(fmt->pack4);
    uint16_t nfree  = fmt->bo->get16(sb + nfree_off);
    uint16_t ninode = fmt->bo->get16(sb + ninode_off);
    if (nfree > fmt->nicfree || ninode > V7_NICINOD)
        return 0;
    uint32_t fsz = fmt->bo->get32(sb + sb_fsize_off(fmt->pack4));
    if (fsz <= isz || base + (uint64_t)fsz * bsize > nbytes)
        return 0;
    for (int i = 0; i < nfree; i++) {
        uint32_t fb = fmt->bo->get32(sb + free_off + 4 * i);
        if (fb != 0 && (fb < isz || fb >= fsz))
            return 0;
    }
    uint32_t head = fmt->bo->get32(sb + free_off);
    int fb_free = fmt->pack4 ? 4 : 2;
    uint32_t s = 0;
    const char *why = NULL;
    if (probe_walk_chain(fmt, io, head, isz, fsz, base, bsize, fmt->nicfree, 4,
                         fmt->bo->get16, fmt->bo->get32, fb_free, &s, &why) < 0) {
        res->why = why; res->isize = isz; res->fsize = fsz; res->segs = s;
        res->class = fmt->nicfree == V7_COH_NICFREE ? "Coherent"
                   : fmt->pack4 ? "32V or sysiii" : "v7, sysiii or sysvr1";
        return 2;
    }
    if (probe_root_ok(fmt, io, base, bsize, fmt->bo->get16, fmt->bo->get32, &why) < 0) {
        res->why = why; res->isize = isz; res->fsize = fsz; res->segs = s;
        res->class = fmt->nicfree == V7_COH_NICFREE ? "Coherent"
                   : fmt->pack4 ? "32V or sysiii" : "v7, sysiii or sysvr1";
        return 2;
    }
    res->class = fmt->nicfree == V7_COH_NICFREE ? "Coherent"
               : fmt->pack4 ? "32V or sysiii" : "v7, sysiii or sysvr1";
    res->isize = isz; res->fsize = fsz; res->segs = s;
    res->blocksize = bsize;
    return 1;
}

/* System V s5fs (magic 0xfd187e20): byte order from the magic, s_type names the
 * block size.  AFS signals itself with s_nfree == 0xffff. */
static int probe_sysv(filsys_edition_t *fmt, const filsys_io_t *io,
                      uint64_t base, int bsize, uint64_t nbytes, filsys_probe_t *res)
{
    (void)bsize;
    uint8_t sb[V7_MAXBSIZE];
    if (io->read(fmt, sb, 512, (off_t)(base + 512)) != 0)
        return 0;
    int le;
    if (bo_get32le(sb + 504) == V7_SYSV_MAGIC)      le = 1;
    else if (bo_get32be(sb + 504) == V7_SYSV_MAGIC) le = 0;
    else return 0;

    uint16_t (*g16)(const uint8_t *) = le ? bo_get16le : bo_get16be;
    uint32_t (*g32)(const uint8_t *) = le ? bo_get32le : bo_get32be;

    if (g16(sb + 8) == 0xffff) {
        res->class = "AFS";
        return 1;
    }
    uint32_t t = g32(sb + 508);
    int bsz = t == 1 ? 512 : t == 2 ? 1024 : t == 3 ? 2048 : 0;
    if (bsz == 0)
        return 0;
    uint16_t isz = g16(sb + 0);
    uint16_t nfree = g16(sb + 8), ninode = g16(sb + 212);
    if (nfree > V7_NICFREE || ninode > V7_NICINOD)
        return 0;
    uint32_t fsz = g32(sb + 4);
    if (fsz <= isz || base + (uint64_t)fsz * bsz > nbytes)
        return 0;
    for (int i = 0; i < nfree; i++) {
        uint32_t fb = g32(sb + 12 + 4 * i);
        if (fb != 0 && (fb < isz || fb >= fsz))
            return 0;
    }
    uint32_t s = 0;
    const char *why = NULL;
    if (probe_walk_chain(fmt, io, g32(sb + 12), isz, fsz, base, bsz, V7_NICFREE, 4,
                         g16, g32, 4, &s, &why) < 0) {
        res->class = probe_sysv_class(sb, g32); res->why = why;
        res->isize = isz; res->fsize = fsz; res->segs = s; res->blocksize = bsz;
        return 2;
    }
    if (probe_root_ok(fmt, io, base, bsz, g16, g32, &why) < 0) {
        res->class = probe_sysv_class(sb, g32); res->why = why;
        res->isize = isz; res->fsize = fsz; res->segs = s; res->blocksize = bsz;
        return 2;
    }
    res->class = probe_sysv_class(sb, g32);
    res->isize = isz; res->fsize = fsz; res->segs = s; res->blocksize = bsz;
    res->byteorder = le ? "le" : "be";
    res->note = le ? "LE" : "BE";
    return 1;
}

/* Xenix (magic 0x2b5544 at 1016, 1024-byte blocks). */
static int probe_xenix(filsys_edition_t *fmt, const filsys_io_t *io,
                       uint64_t base, int bsize, uint64_t nbytes, filsys_probe_t *res)
{
    (void)bsize;
    uint8_t sb[V7_MAXBSIZE];
    if (io->read(fmt, sb, 1024, (off_t)(base + 1024)) != 0)
        return 0;
    if (bo_get32le(sb + 0x3F8) != V7_XEN_MAGIC)
        return 0;
    uint16_t isz = bo_get16le(sb + 0);
    uint32_t fsz = bo_get32le(sb + 2);
    uint16_t nfree = bo_get16le(sb + 6);
    uint16_t ninode = bo_get16le(sb + 0x198);
    if (isz < 3 || fsz <= isz || nfree > V7_XEN_NICFREE || ninode > V7_NICINOD)
        return 0;
    if (base + (uint64_t)fsz * 1024 > nbytes)
        return 0;
    for (int i = 0; i < nfree; i++) {
        uint32_t fb = bo_get32le(sb + 8 + 4 * i);
        if (fb != 0 && (fb < isz || fb >= fsz))
            return 0;
    }
    uint32_t s = 0;
    const char *why = NULL;
    if (probe_walk_chain(fmt, io, bo_get32le(sb + 8), isz, fsz, base, 1024,
                         V7_XEN_NICFREE, 4, bo_get16le, bo_get32le, 2, &s, &why) < 0) {
        res->class = "Xenix"; res->why = why;
        res->isize = isz; res->fsize = fsz; res->segs = s; res->blocksize = 1024;
        return 2;
    }
    if (probe_root_ok(fmt, io, base, 1024, bo_get16le, bo_get32le, &why) < 0) {
        res->class = "Xenix"; res->why = why;
        res->isize = isz; res->fsize = fsz; res->segs = s; res->blocksize = 1024;
        return 2;
    }
    res->class = "Xenix";
    res->isize = isz; res->fsize = fsz; res->segs = s; res->blocksize = 1024;
    res->byteorder = "le";
    return 1;
}

/* 2.9BSD / 2.11BSD (1024-byte blocks, V7-shaped, no magic): they share a
 * superblock and differ only in directory format, so probe_bsd211_root_dir
 * disambiguates. */
static int probe_bsd29(filsys_edition_t *fmt, const filsys_io_t *io,
                       uint64_t base, int bsize, uint64_t nbytes, filsys_probe_t *res)
{
    (void)bsize;
    uint8_t sb[V7_MAXBSIZE];
    if (io->read(fmt, sb, 1024, (off_t)(base + 1024)) != 0)
        return 0;
    uint16_t isz = bo_get16le(sb);
    if (isz < 3)
        return 0;
    uint16_t nfree = bo_get16le(sb + 6);
    uint16_t ninode = bo_get16le(sb + 208);
    if (nfree > V7_NICFREE || ninode > V7_NICINOD)
        return 0;
    uint32_t fsz = bo_get32me(sb + 2);
    if (fsz <= isz || base + (uint64_t)fsz * 1024 > nbytes)
        return 0;
    for (int i = 0; i < nfree; i++) {
        uint32_t fb = bo_get32me(sb + 8 + 4 * i);
        if (fb != 0 && (fb < isz || fb >= fsz))
            return 0;
    }
    uint32_t s = 0;
    const char *why = NULL;
    if (probe_walk_chain(fmt, io, bo_get32me(sb + 8), isz, fsz, base, 1024,
                         V7_NICFREE, 4, bo_get16le, bo_get32me, 2, &s, &why) < 0) {
        res->class = "2.9BSD"; res->why = why;
        res->isize = isz; res->fsize = fsz; res->segs = s; res->blocksize = 1024;
        return 2;
    }
    if (probe_root_ok(fmt, io, base, 1024, bo_get16le, bo_get32me, &why) < 0) {
        res->class = "2.9BSD"; res->why = why;
        res->isize = isz; res->fsize = fsz; res->segs = s; res->blocksize = 1024;
        return 2;
    }
    res->class = probe_bsd211_root_dir(fmt, io, base, 1024) ? "2.11BSD" : "2.9BSD";
    res->isize = isz; res->fsize = fsz; res->segs = s; res->blocksize = 1024;
    return 1;
}

/* V8/V9/V10 (rearranged superblock via v8_sb_decode). */
static int probe_v8(filsys_edition_t *fmt, const filsys_io_t *io,
                    uint64_t base, int bsize, uint64_t nbytes, filsys_probe_t *res)
{
    uint8_t sb[V7_MAXBSIZE];
    if (io->read(fmt, sb, bsize, (off_t)(base + (uint64_t)bsize)) != 0)
        return 0;
    const char *miss_why = NULL;
    uint16_t miss_isz = 0; uint32_t miss_fsz = 0;
    for (int le = 1; le >= 0; le--) {
        uint16_t isz; uint32_t fsz; int form; uint32_t s; const char *why_try = NULL;
        int r = probe_v8_try(fmt, io, sb, base, bsize, nbytes, le, &isz, &fsz, &form, &s, &why_try);
        if (r == 1) {
            static char note_buf[64];
            res->isize = isz; res->fsize = fsz; res->segs = s;
            res->freemap = form;
            res->blocksize = bsize;
            res->byteorder = le ? "le" : "be";
            res->bitmap = (form != V8_FREEMAP_LIST);
            if (form == V8_FREEMAP_BIGMAP)
                res->class = "v10, bs=4096, bigmap";
            else if (bsize == 8192)
                res->class = "v9";
            else
                res->class = (bsize == 4096) ? "v8 or v10, bs=4096, bitmap"
                                             : "v8 or v10, bs=1024";
            snprintf(note_buf, sizeof note_buf, "%d-byte blocks, %s, %s", bsize,
                     form == V8_FREEMAP_LIST ? "free list" :
                     form == V8_FREEMAP_BITMAP ? "in-superblock bitmap" :
                     "out-of-superblock bitmap", le ? "LE" : "BE");
            res->note = note_buf;
            return 1;
        }
        if (r == -1 && !miss_why) {
            miss_why = why_try; miss_isz = isz; miss_fsz = fsz;
        }
    }
    if (miss_why) {
        res->why = miss_why; res->isize = miss_isz; res->fsize = miss_fsz;
        res->segs = 0; res->class = "v8";
        return 2;
    }
    return 0;
}

/* V6 (16-bit fields; isize_count).  Root-inode cross-check is absent here: V6's
 * root is inode 1 in a 32-byte inode, a different layout, so the chain walk is
 * the structural test. */
static int probe_v6(filsys_edition_t *fmt, const filsys_io_t *io,
                    uint64_t base, int bsize, uint64_t nbytes, filsys_probe_t *res)
{
    (void)bsize;
    uint8_t sb[V7_MAXBSIZE];
    if (io->read(fmt, sb, 512, (off_t)(base + 512)) != 0)
        return 0;
    uint16_t isz = bo_get16le(sb);
    if (isz < 3)
        return 0;
    uint16_t fsz = bo_get16le(sb + 2);
    uint16_t nfree = bo_get16le(sb + 4);
    uint16_t ninode = bo_get16le(sb + 206);
    if (fsz <= isz || nfree > V6_NICFREE || ninode > V6_NICINOD ||
        base + (uint64_t)fsz * 512 > nbytes)
        return 0;
    for (int i = 0; i < nfree; i++) {
        uint16_t fb = bo_get16le(sb + 6 + 2 * i);
        if (fb != 0 && (fb < isz || fb >= fsz))
            return 0;
    }
    uint32_t s = 0;
    const char *why = NULL;
    if (probe_walk_chain(fmt, io, bo_get16le(sb + 6), isz, fsz, base, 512,
                         V6_NICFREE, 2, bo_get16le, bo_get32le, 2, &s, &why) < 0) {
        res->class = "v4, v5, v6 or usgpg3"; res->why = why;
        res->isize = isz; res->fsize = fsz; res->segs = s; res->blocksize = 512;
        return 2;
    }
    res->class = "v4, v5, v6 or usgpg3";
    res->isize = isz; res->fsize = fsz; res->segs = s; res->blocksize = 512;
    return 1;
}

/* The V7-family dispatcher: route on the descriptor's magic / sb_decode / block
 * size.  One probe serves V7/32V/Coherent/Xenix/2.9BSD/SysIII/SysV/V8/V9/V10. */
static int v7_probe(filsys_edition_t *fmt, const filsys_io_t *io,
                    uint64_t base, int bsize, uint64_t nbytes, filsys_probe_t *res)
{
    memset(res, 0, sizeof *res);
    res->freemap = -1;
    if (fmt->sb_decode)
        return probe_v8(fmt, io, base, bsize, nbytes, res);
    if (fmt->magic == V7_XEN_MAGIC)
        return probe_xenix(fmt, io, base, bsize, nbytes, res);
    if (fmt->magic == V7_SYSV_MAGIC)
        return probe_sysv(fmt, io, base, bsize, nbytes, res);
    if (fmt->bsize == 1024)
        return probe_bsd29(fmt, io, base, bsize, nbytes, res);
    return probe_v7_family(fmt, io, base, bsize, nbytes, res);
}

const struct filsys_dir_ops dir_fixed = {
    .dir_read   = v7fs_dir_read,
    .dir_add    = v7fs_dir_add,
    .dir_remove = v7fs_dir_remove,
};
static const struct filsys_inode_ops inode_64 = {
    .read_inode  = v7fs_read_inode,
    .write_inode = v7fs_write_inode,
    .bmap        = v7fs_bmap,
    .inode_state = v7_inode_state,
    .allocated_blocks = v7fs_allocated_blocks,
};

const struct filsys_ops v7fs_ops = {
    .name        = "v7",
    .dir         = &dir_fixed,
    .inode       = &inode_64,
    .blocksize   = v7fs_blocksize_op,
    .probe       = v7_probe,
    .open        = v7fs_open,
    .close       = v7fs_close,
    .sync        = v7fs_sync,
    .mark_dirty  = v7fs_mark_dirty,
    .mark_clean  = v7fs_mark_clean,
    .read_block  = v7fs_read_block,
    .write_block = v7fs_write_block,
    .blk_get     = v7fs_read_block,
    .blk_put     = v7fs_write_block,
    .ialloc      = v7fs_ialloc,
    .ifree       = v7fs_ifree,
    .file_read   = v7fs_file_read,
    .file_write  = v7fs_file_write,
    .dir_lookup  = v7fs_dir_lookup,
    .lookup      = v7fs_lookup,
    .check       = filsys_check_op,
    .maxino      = v7_maxino,
    .data_start  = v7_data_start,
    .data_end    = v7_data_end,
    .walk_free   = v7_walk_free,
    .makefree    = v7fs_makefree,
    .is_clean    = filsys_is_clean,
    .statfs      = v7fs_statfs_op,
    .max_file    = filsys_max_file_op,
};

/* ---- 2.11BSD directories (variable-length entries) ------------------------ */

static inline uint32_t bsd211_dirsiz(uint16_t namlen) {
    return (7u + namlen + 3u) & ~3u;   /* round_up(7 + namlen, 4) */
}

static int bsd211_dir_read(filsys_edition_t *fs, v7_inode_t *ip, v7_dirent_t **ents, size_t *count) {
    if ((ip->mode & fs->ifmt) != fs->ifdir)
        return -ENOTDIR;
    if (ip->size > (uint64_t)(fs->fsize - fs->isize) * fs->bsize)
        return -EFBIG;
    size_t cap = ip->size / 12 + 1;
    v7_dirent_t *out = calloc(cap, sizeof(*out));
    if (!out)
        return -ENOMEM;
    uint8_t *buf = malloc(ip->size ? ip->size : 1);
    if (!buf) { free(out); return -ENOMEM; }
    ssize_t n = v7fs_file_read(fs, ip, buf, ip->size, 0);
    if (n < 0) { free(buf); free(out); return (int)n; }

    size_t cnt = 0;
    for (size_t off = 0; off + 6 <= (size_t)n; ) {
        uint16_t ino    = fs->bo->get16(buf + off);
        uint16_t reclen = fs->bo->get16(buf + off + 2);
        uint16_t namlen = fs->bo->get16(buf + off + 4);
        if (reclen < 6 || off + reclen > (size_t)n)
            break;
        if (ino != 0 && namlen <= fs->max_namlen) {
            out[cnt].ino = ino;
            memcpy(out[cnt].name, buf + off + 6, namlen);
            out[cnt].name[namlen] = 0;
            cnt++;
        }
        off += reclen;
    }
    free(buf);
    *ents = out;
    *count = cnt;
    return 0;
}

static int bsd211_dir_add(filsys_edition_t *fs, v7_inode_t *ip, uint32_t ino, const char *name) {
    size_t namlen = strlen(name);
    if (namlen > fs->max_namlen)
        return -ENAMETOOLONG;
    uint32_t need = bsd211_dirsiz((uint16_t)namlen);

    uint8_t *buf = malloc(ip->size ? ip->size : 1);
    if (!buf)
        return -ENOMEM;
    ssize_t n = v7fs_file_read(fs, ip, buf, ip->size, 0);
    if (n < 0) { free(buf); return (int)n; }

    /* Reuse a free entry (d_ino == 0) that is big enough. */
    for (size_t off = 0; off + 6 <= (size_t)n; ) {
        uint16_t d_ino    = fs->bo->get16(buf + off);
        uint16_t reclen   = fs->bo->get16(buf + off + 2);
        if (reclen < 6 || off + reclen > (size_t)n)
            break;
        if (d_ino == 0 && reclen >= need) {
            memset(buf + off, 0, reclen);
            fs->bo->put16(buf + off, (uint16_t)ino);
            fs->bo->put16(buf + off + 2, reclen);
            fs->bo->put16(buf + off + 4, (uint16_t)namlen);
            memcpy(buf + off + 6, name, namlen);
            ssize_t w = v7fs_file_write(fs, ip, buf, ip->size, 0);
            free(buf);
            return w < 0 ? (int)w : 0;
        }
        off += reclen;
    }

    /* Append a new entry at the end (the directory grows). */
    uint8_t ent[80];
    memset(ent, 0, sizeof(ent));
    fs->bo->put16(ent, (uint16_t)ino);
    fs->bo->put16(ent + 2, (uint16_t)need);
    fs->bo->put16(ent + 4, (uint16_t)namlen);
    memcpy(ent + 6, name, namlen);
    ssize_t w = v7fs_file_write(fs, ip, ent, need, n);
    free(buf);
    return w < 0 ? (int)w : 0;
}

static int bsd211_dir_remove(filsys_edition_t *fs, v7_inode_t *ip, const char *name) {
    uint8_t *buf = malloc(ip->size ? ip->size : 1);
    if (!buf)
        return -ENOMEM;
    ssize_t n = v7fs_file_read(fs, ip, buf, ip->size, 0);
    if (n < 0) { free(buf); return (int)n; }

    for (size_t off = 0; off + 6 <= (size_t)n; ) {
        uint16_t d_ino    = fs->bo->get16(buf + off);
        uint16_t reclen   = fs->bo->get16(buf + off + 2);
        uint16_t namlen   = fs->bo->get16(buf + off + 4);
        if (reclen < 6 || off + reclen > (size_t)n)
            break;
        if (d_ino != 0 && namlen == strlen(name) &&
            memcmp(buf + off + 6, name, namlen) == 0) {
            fs->bo->put16(buf + off, 0);   /* mark free */
            ssize_t w = v7fs_file_write(fs, ip, buf, ip->size, 0);
            free(buf);
            return w < 0 ? (int)w : 0;
        }
        off += reclen;
    }
    free(buf);
    return -ENOENT;
}

static uint8_t bsd211_inode_state(filsys_edition_t *fs, uint32_t ino, uint32_t mode) {
    (void)fs; (void)ino;
    if (mode == 0)
        return FILSYS_IN_UNALLOC;
    switch (mode & BSD211_IFMT) {
    case BSD211_IFDIR: return FILSYS_IN_IDIR;
    case BSD211_IFREG: case BSD211_IFLNK: return FILSYS_IN_IREG;
    case BSD211_IFCHR: case BSD211_IFSOCK: return FILSYS_IN_ICHR;
    case BSD211_IFBLK: return FILSYS_IN_IBLK;
    default: return FILSYS_IN_UNKNOWN;
    }
}

int bsd211_check(filsys_edition_t *fs, v7_check_t *rep, int mode) {
    return filsys_check_common(fs, fs, rep, mode);
}

/* ---- 2.11BSD ops table: the shared V7 engine + variable-length dirents ---- */




static const struct filsys_dir_ops dir_variable = {
    .dir_read   = bsd211_dir_read,
    .dir_add    = bsd211_dir_add,
    .dir_remove = bsd211_dir_remove,
};
static const struct filsys_inode_ops inode_64_32addr = {
    .read_inode  = v7fs_read_inode,
    .write_inode = v7fs_write_inode,
    .bmap        = v7fs_bmap,
    .inode_state = bsd211_inode_state,
    .allocated_blocks = v7fs_allocated_blocks,
};

const struct filsys_ops bsd211fs_ops = {
    .name        = "bsd211",
    .dir         = &dir_variable,
    .probe       = v7_probe,
    .inode       = &inode_64_32addr,
    .blocksize   = v7fs_blocksize_op,
    .open        = v7fs_open,
    .close       = v7fs_close,
    .sync        = v7fs_sync,
    .mark_dirty  = v7fs_mark_dirty,
    .mark_clean  = v7fs_mark_clean,
    .read_block  = v7fs_read_block,
    .write_block = v7fs_write_block,
    .blk_get     = v7fs_read_block,
    .blk_put     = v7fs_write_block,
    .ialloc      = v7fs_ialloc,
    .ifree       = v7fs_ifree,
    .file_read   = v7fs_file_read,
    .file_write  = v7fs_file_write,
    .dir_lookup  = v7fs_dir_lookup,
    .lookup      = v7fs_lookup,
    .check       = filsys_check_op,
    .maxino      = v7_maxino,
    .data_start  = v7_data_start,
    .data_end    = v7_data_end,
    .walk_free   = v7_walk_free,
    .makefree    = v7fs_makefree,
    .is_clean    = filsys_is_clean,
    .statfs      = v7fs_statfs_op,
    .max_file    = filsys_max_file_op,
};

/* ======================= Sixth Edition (V6) ============================== *
 * V6 keeps V7's free-list superblock but with 16-bit block numbers (2-byte
 * free-list and indirect entries), s_isize as the *number* of i-list blocks
 * (first data block = isize+2, maxino = isize*16), a 32-byte inode, and the
 * ILARG large-file layout (7 single + 1 double indirect).  These are semantic
 * differences, so the inode/alloc/bmap/check bodies stay V6-specific and the
 * dir/file/lookup/block-IO come from the shared engine above. */

static int v6_read_inode(filsys_edition_t *fs, uint32_t ino, v7_inode_t *ip);
static int v6_write_inode(filsys_edition_t *fs, uint32_t ino, const v7_inode_t *ip);

/* Recompute the V7-family free-space totals (free blocks + free inodes) by
 * walking the free-list chain and the i-list.  Used for the pack4, magic-free
 * editions (32V and System III), whose on-disk s_tfree/s_tinode are unmaintained
 * and so unreliable on foreign media.  Read-only: never mutates the image. */
static void v7_count_free(filsys_edition_t *fs, uint32_t *nblk, uint32_t *nino) {
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
            n = fs->df_nfree_wid == 4 ? fs->bo->get32(blk) : fs->bo->get16(blk);
            if (n > fs->nicfree)
                break;
            for (int i = 0; i < fs->nicfree; i++)
                cur[i] = v7_get_daddr(fs, blk + v7_chain_free_off(fs) + fs->daddr_wid * i);
        }
        if (++guard > fs->fsize + fs->nicfree)
            break;
    }
    *nblk = blocks;

    uint32_t maxino = v7_maxinode(fs), used = 0;
    for (uint32_t ino = 1; ino <= maxino; ino++) {
        v7_inode_t ip;
        if (v7fs_read_inode(fs, ino, &ip) == 0 && ip.mode != 0)
            used++;
    }
    *nino = maxino - used;
}

static void v6_count_free(filsys_edition_t *fs, uint32_t *nblk, uint32_t *nino) {
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
            n = fs->bo->get16(blk + 0);
            for (int i = 0; i < fs->nicfree; i++)
                cur[i] = fs->bo->get16(blk + 2 + 2 * i);
        }
        if (++guard > fs->fsize + fs->nicfree)
            break;
    }
    *nblk = blocks;

    uint32_t maxino = v6_maxino(fs->isize), used = 0;
    for (uint32_t ino = 1; ino <= maxino; ino++) {
        v7_inode_t ip;
        if (v6_read_inode(fs, ino, &ip) == 0 && (ip.mode & V6_IALLOC))
            used++;
    }
    *nino = maxino - used;
}

static int v6_read_inode(filsys_edition_t *fs, uint32_t ino, v7_inode_t *ip) {
    if (ino == 0)
        return -EINVAL;
    uint32_t bno = v7_itod(fs, ino);
    uint32_t off = v7_itoo(fs, ino);
    if (bno >= v6_data_start(fs->isize))   /* i-list is blocks 2..s_isize+1 */
        return -EINVAL;
    uint8_t raw[V6_BSIZE];
    if (v7fs_read_block(fs, bno, raw))
        return -EIO;
    const uint8_t *d = raw + off * fs->inode_size;
    memset(ip, 0, sizeof(*ip));
    ip->ino   = ino;
    ip->mode  = fs->bo->get16(d + 0);
    ip->nlink = (int16_t)d[2];        /* char */
    ip->uid   = (int16_t)d[3];        /* char */
    ip->gid   = (int16_t)d[4];        /* char */
    ip->size  = ((uint32_t)d[5] << 16) | fs->bo->get16(d + 6);
    for (int i = 0; i < fs->niaddr; i++)
        ip->addr[i] = fs->bo->get16(d + 8 + 2 * i);
    ip->atime = fs->bo->get32(d + 24);
    ip->mtime = fs->bo->get32(d + 28);
    ip->ctime = ip->mtime;            /* V6 has no ctime */
    return 0;
}

static int v6_write_inode(filsys_edition_t *fs, uint32_t ino, const v7_inode_t *ip) {
    if (ino == 0)
        return -EINVAL;
    /* Allocator state before reference: flush the superblock before this
     * inode's reference to a newly-allocated block/inode (same rule as
     * v7fs_write_inode). */
    if (fs->fl_dirty) {
        int rc = super_write(fs);
        if (rc)
            return rc;
    }
    uint32_t bno = v7_itod(fs, ino);
    uint32_t off = v7_itoo(fs, ino);
    if (bno >= v6_data_start(fs->isize))
        return -EINVAL;
    uint8_t raw[V6_BSIZE];
    if (v7fs_read_block(fs, bno, raw))
        return -EIO;
    uint8_t *d = raw + off * fs->inode_size;
    fs->bo->put16(d + 0, (uint16_t)ip->mode);
    d[2] = (uint8_t)ip->nlink;
    d[3] = (uint8_t)ip->uid;
    d[4] = (uint8_t)ip->gid;
    d[5] = (uint8_t)((ip->size >> 16) & 0xff);
    fs->bo->put16(d + 6, (uint16_t)(ip->size & 0xffff));
    for (int i = 0; i < fs->niaddr; i++)
        fs->bo->put16(d + 8 + 2 * i, (uint16_t)ip->addr[i]);
    fs->bo->put32(d + 24, ip->atime);
    fs->bo->put32(d + 28, ip->mtime);
    return v7fs_write_block(fs, bno, raw);
}

static int v6_ind1(filsys_edition_t *fs, uint32_t *slot, uint32_t idx, int create, uint32_t *out) {
    uint32_t blk = *slot;
    if (blk == 0) {
        if (!create) { *out = 0; return 0; }
        uint8_t z[V6_BSIZE];
        memset(z, 0, V6_BSIZE);
        int rc = v7fs_balloc(fs, &blk);
        if (rc)
            return rc;   /* -EIO (barrier commit) or -ENOSPC */
        if (v7fs_write_block(fs, blk, z))
            return -EIO;
        *slot = blk;
    }
    uint8_t buf[V6_BSIZE];
    if (v7fs_read_block(fs, blk, buf))
        return -EIO;
    uint32_t nb = fs->bo->get16(buf + 2 * idx);
    if (nb == 0 && create) {
        int rc = v7fs_balloc(fs, &nb);
        if (rc)
            return rc;   /* -EIO (barrier commit) or -ENOSPC */
        fs->bo->put16(buf + 2 * idx, (uint16_t)nb);
        if (v7fs_write_block(fs, blk, buf))
            return -EIO;
    }
    *out = nb;
    return 0;
}

static int v6_ind2(filsys_edition_t *fs, uint32_t *slot, uint32_t o, uint32_t i, int create, uint32_t *out) {
    uint32_t blk = *slot;
    if (blk == 0) {
        if (!create) { *out = 0; return 0; }
        uint8_t z[V6_BSIZE];
        memset(z, 0, V6_BSIZE);
        int rc = v7fs_balloc(fs, &blk);
        if (rc)
            return rc;   /* -EIO (barrier commit) or -ENOSPC */
        if (v7fs_write_block(fs, blk, z))
            return -EIO;
        *slot = blk;
    }
    uint8_t buf[V6_BSIZE];
    if (v7fs_read_block(fs, blk, buf))
        return -EIO;
    uint32_t sub = fs->bo->get16(buf + 2 * o);
    if (sub == 0 && create) {
        uint8_t z[V6_BSIZE];
        memset(z, 0, V6_BSIZE);
        int rc = v7fs_balloc(fs, &sub);
        if (rc)
            return rc;   /* -EIO (barrier commit) or -ENOSPC */
        if (v7fs_write_block(fs, sub, z))
            return -EIO;
        fs->bo->put16(buf + 2 * o, (uint16_t)sub);
        if (v7fs_write_block(fs, blk, buf))
            return -EIO;
    }
    if (sub == 0) { *out = 0; return 0; }
    return v6_ind1(fs, &sub, i, create, out);
}

static int v6_bmap(filsys_edition_t *fs, v7_inode_t *ip, uint32_t lbn, int create, uint32_t *bno) {
    if (!(ip->mode & V6_ILARG) && create && lbn >= V6_NDADDR) {
        uint32_t iblk;
        int rc = v7fs_balloc(fs, &iblk);
        if (rc)
            return rc;   /* -EIO (barrier commit) or -ENOSPC */
        uint8_t buf[V6_BSIZE] = {0};
        for (int i = 0; i < V6_NDADDR; i++)
            fs->bo->put16(buf + 2 * i, (uint16_t)ip->addr[i]);
        if (v7fs_write_block(fs, iblk, buf))
            return -EIO;
        for (int i = 0; i < V6_NDADDR; i++)
            ip->addr[i] = 0;
        ip->addr[0] = iblk;
        ip->mode |= V6_ILARG;
    }

    if (ip->mode & V6_ILARG) {
        if (lbn < 7 * V6_NINDIR)
            return v6_ind1(fs, &ip->addr[lbn >> 8], lbn & (V6_NINDIR - 1), create, bno);
        uint32_t r = lbn - 7 * V6_NINDIR;
        return v6_ind2(fs, &ip->addr[7], r >> 8, r & (V6_NINDIR - 1), create, bno);
    }
    if (lbn >= V6_NDADDR) {
        *bno = 0;
        return create ? -EFBIG : 0;
    }
    uint32_t nb = ip->addr[lbn];
    if (nb == 0 && create) {
        int rc = v7fs_balloc(fs, &nb);
        if (rc)
            return rc;   /* -EIO (barrier commit) or -ENOSPC */
        ip->addr[lbn] = nb;
    }
    *bno = nb;
    return 0;
}

static uint32_t v6_makefree(filsys_edition_t *fs, filsys_chkctx_t *cx) {
    filsys_edition_t *f = fs;
    uint32_t m = 3, n = 100;
    uint32_t adr[100];
    uint8_t flg[100] = {0};
    uint32_t i, j;
    i = 0;
    for (j = 0; j < n; j++) {
        while (flg[i])
            i = (i + 1) % n;
        adr[j] = i + 1;
        flg[i]++;
        i = (i + m) % n;
    }

    f->fl.nfree = 0;
    f->fl.ninode = 0;
    uint32_t dstart = v6_data_start(f->isize);
    uint32_t nfree = 0;
    uint32_t d = f->fsize - 1;
    while (d % n)
        d++;
    for (; d > 0; d -= n) {
        for (i = 0; i < n; i++) {
            int64_t b = (int64_t)d - adr[i];
            if (b < (int64_t)dstart || b >= (int64_t)f->fsize)
                continue;
            uint32_t off = (uint32_t)b - dstart;
            if (!(cx->bmap[off >> 3] & (uint8_t)(1u << (off & 7)))) {
                v7fs_bfree(f, (uint32_t)b);
                nfree++;
            }
        }
    }
    super_write(f);
    return nfree;
}

static uint64_t v6_ind_count(filsys_edition_t *fs, uint32_t blk, int levels) {
    if (blk == 0)
        return 0;
    uint8_t buf[V6_BSIZE];
    if (v7fs_read_block(fs, blk, buf))
        return 0;
    uint64_t n = 1;   /* the indirect block itself */
    for (uint32_t i = 0; i < V6_NINDIR; i++) {
        uint32_t sub = fs->bo->get16(buf + 2 * i);
        if (sub == 0)
            continue;
        n += levels == 1 ? 1 : v6_ind_count(fs, sub, levels - 1);
    }
    return n;
}

static uint64_t v6_allocated_blocks(filsys_edition_t *fs, const filsys_inode_t *ip) {
    uint64_t n = 0;
    if (ip->mode & V6_ILARG) {
        for (int i = 0; i < V6_NDADDR - 1; i++)        /* 7 single-indirect slots */
            n += v6_ind_count(fs, ip->addr[i], 1);
        n += v6_ind_count(fs, ip->addr[V6_NDADDR - 1], 2);  /* 1 double-indirect */
    } else {
        for (int i = 0; i < V6_NDADDR; i++)
            if (ip->addr[i]) n++;
    }
    return n;
}

static uint8_t v6_inode_state(filsys_edition_t *fs, uint32_t ino, uint32_t mode) {
    (void)fs; (void)ino;
    if (mode == 0)
        return FILSYS_IN_UNALLOC;
    switch (mode & V6_IFMT) {
    case V6_IFDIR: return FILSYS_IN_IDIR;
    case V6_IFCHR: return FILSYS_IN_ICHR;
    case V6_IFBLK: return FILSYS_IN_IBLK;
    case 0: return FILSYS_IN_IREG;   /* regular: no type bits set */
    default: return FILSYS_IN_UNKNOWN;
    }
}

int v6_check(filsys_edition_t *fs, v7_check_t *rep, int mode) {
    return filsys_check_common(fs, fs, rep, mode);
}

/* ---- V6 ops table: shared engine + V6 inode/bmap/check ------------------- */






static const struct filsys_inode_ops inode_32 = {
    .read_inode  = v6_read_inode,
    .write_inode = v6_write_inode,
    .bmap        = v6_bmap,
    .inode_state = v6_inode_state,
    .allocated_blocks = v6_allocated_blocks,
};

const struct filsys_ops v6fs_ops = {
    .name        = "v6",
    .dir         = &dir_fixed,
    .probe       = probe_v6,
    .inode       = &inode_32,
    .blocksize   = v7fs_blocksize_op,
    .open        = v7fs_open,
    .close       = v7fs_close,
    .sync        = v7fs_sync,
    .mark_dirty  = v7fs_mark_dirty,
    .mark_clean  = v7fs_mark_clean,
    .read_block  = v7fs_read_block,
    .write_block = v7fs_write_block,
    .blk_get     = v7fs_read_block,
    .blk_put     = v7fs_write_block,
    .ialloc      = v7fs_ialloc,
    .ifree       = v7fs_ifree,
    .file_read   = v7fs_file_read,
    .file_write  = v7fs_file_write,
    .dir_lookup  = v7fs_dir_lookup,
    .lookup      = v7fs_lookup,
    .check       = filsys_check_op,
    .maxino      = v7_maxino,
    .data_start  = v7_data_start,
    .data_end    = v7_data_end,
    .walk_free   = v7_walk_free,
    .makefree    = v6_makefree,
    .is_clean    = filsys_is_clean,
    .statfs      = v7fs_statfs_op,
    .max_file    = filsys_max_file_op,
};

/* ---- allocator vtable: the free-list cache (V6/V7/BSD211) ---------------- */

const alloc_ops_t freelist_alloc_ops = {
    .balloc = (int  (*)(void *, uint32_t *))v7fs_balloc,
    .bfree  = (void (*)(void *, uint32_t))v7fs_bfree,
    .ialloc = (int  (*)(void *, uint32_t *))v7fs_ialloc,
    .ifree  = (void (*)(void *, uint32_t))v7fs_ifree,
    .sync   = (int  (*)(void *))v7fs_sync,
};
