/* Copyright (C) 2026 David Walther */
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
#include "instrument.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

/* Flush the allocator state: the free list rides v7fs_super_write; the bitmap forms
 * write their bitmap (in-superblock via v7fs_super_write, out-of-superblock via its
 * blocks) and then the superblock. */
static int flush_fs(filsys_edition_t *fs) {
    if (fs->desc.freemap != V8_FREEMAP_LIST)
        return v8_bitmap_sync(fs);
    return v7fs_super_write(fs);
}

/* ---- lifecycle --------------------------------------------------------- */

int v7fs_open(filsys_edition_t *fs, const char *path, int readonly,
              const filsys_desc_t *proto, uint64_t offset) {
    if (&fs->desc != proto)
        fs->desc = *proto;   /* copy the static descriptor fields */
    fs->readonly = readonly;
    fs->base = offset;
    fs->fd = open(path, readonly ? O_RDONLY : O_RDWR);
    if (fs->fd < 0)
        return -errno;
    fs->io = &filsys_io_file;

    if (fs->desc.dyn_bsize) {
        /* System V: the 512-byte superblock sits at a fixed byte offset (512),
         * not "block 1" -- that only coincides with byte 512 when the block size
         * is 512.  s_type (offset 508) names the block size. */
        uint8_t probe[512];
        if (fs->io->read(fs, probe, sizeof probe, 512 + (off_t)fs->base)) {
            close(fs->fd);
            fs->fd = -1;
            return -EIO;
        }
        if (!fs->desc.ignore_magic &&
            fs->desc.bo->get32(probe + V7_SYSV_MAGIC_OFF) != V7_SYSV_MAGIC) {
            close(fs->fd);
            fs->fd = -1;
            return -EINVAL;
        }
        uint32_t t = fs->desc.bo->get32(probe + V7_SYSV_TYPE_OFF);
        uint32_t bsize = t == V7_SYSV_Fs1b ? 512 : t == V7_SYSV_Fs2b ? 1024 : t == V7_SYSV_Fs4b ? 2048 : 0;
        if (bsize == 0) {
            close(fs->fd);
            fs->fd = -1;
            return -EINVAL;
        }
        /* The logical block size and the indirect-block entry count both follow
         * from s_type; the 4-byte daddr_t is fixed. */
        fs->desc.bsize = bsize;
        fs->desc.nindir = bsize / fs->desc.daddr_wid;
    }

    uint8_t sb[V7_MAXBSIZE];
    if (fs->desc.dyn_bsize) {
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
    if (fs->desc.sb_decode) {
        /* V8-family: the rearranged superblock is decoded by the format's own
         * codec (v8_sb_decode), which also validates the cache counts. */
        if (fs->desc.sb_decode(fs, sb)) {
            close(fs->fd);
            fs->fd = -1;
            return -EINVAL;
        }
    } else if (fs->desc.isize_count) {
        /* V6: 16-bit superblock (s_fsize and the free cache are 2 bytes). */
        fs->isize  = fs->desc.bo->get16(sb + 0);
        fs->fsize  = fs->desc.bo->get16(sb + 2);
        fs->fl.nfree  = fs->desc.bo->get16(sb + 4);
        for (int i = 0; i < fs->desc.nicfree; i++)
            fs->fl.free[i] = fs->desc.bo->get16(sb + 6 + 2 * i);
        fs->fl.ninode = fs->desc.bo->get16(sb + 206);
        for (int i = 0; i < fs->desc.nicinod; i++)
            fs->fl.inode[i] = fs->desc.bo->get16(sb + 208 + 2 * i);
        fs->time   = fs->desc.bo->get32(sb + 412);
        fs->fmod   = sb[410];              /* s_fmod */
    } else {
        fs->isize  = fs->desc.bo->get16(sb + 0);
        fs->fsize  = fs->desc.bo->get32(sb + sb_fsize_off(fs->desc.pack4));
        fs->fl.nfree  = fs->desc.bo->get16(sb + sb_nfree_off(fs->desc.pack4));
        for (int i = 0; i < fs->desc.nicfree; i++)
            fs->fl.free[i] = fs->desc.bo->get32(sb + sb_free_off(fs->desc.pack4) + 4 * i);
        fs->fl.ninode = fs->desc.bo->get16(sb + sb_ninode_off(fs->desc.pack4, fs->desc.nicfree));
        for (int i = 0; i < V7_NICINOD; i++)
            fs->fl.inode[i] = fs->desc.bo->get16(sb + sb_inode_off(fs->desc.pack4, fs->desc.nicfree) + 2 * i);
        fs->time   = fs->desc.bo->get32(sb + sb_time_off(fs->desc.pack4, fs->desc.nicfree));
        fs->fmod   = sb[sb_time_off(fs->desc.pack4, fs->desc.nicfree) - fs->desc.fmod_back];  /* s_fmod */
    }
    if (fs->desc.sb_decode && fs->desc.freemap != V8_FREEMAP_LIST) {
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
    if (fs->fl.nfree > fs->desc.nicfree || fs->fl.ninode > fs->desc.nicinod) {
        close(fs->fd);
        fs->fd = -1;
        return -EINVAL;
    }
    /* s_tfree/s_tinode carry the true free-space totals (v7fs_makefree writes
     * them); the 50/100-entry caches are only the in-core spill.  Read them so
     * statfs can report real free space rather than the cache depth.  V6 has no
     * such fields, so its totals are recomputed from the free list + i-list.
     * The V8-family codec already read them. */
    if (fs->desc.sb_decode)
        ;   /* totals already read by v8_sb_decode */
    else if (fs->desc.isize_count)
        v6_count_free(fs, &fs->fl.tfree, &fs->fl.tinode);
    else if (!fs->desc.pack4 || fs->desc.magic) {
        int toff = sb_tfree_off(fs->desc.pack4, fs->desc.nicfree, fs->desc.has_dinfo);
        fs->fl.tfree  = fs->desc.bo->get32(sb + toff);
        fs->fl.tinode = fs->desc.bo->get16(sb + toff + 4);
    } else {
        /* 32V and System III (pack4, no magic): the s_tfree/s_tinode words sit at
         * the pack4 offset but are unmaintained on historical media, so recompute
         * them from the free list and i-list -- exactly what the V6 branch above
         * does for its layout. */
        v7_count_free(fs, &fs->fl.tfree, &fs->fl.tinode);
    }
    if (fs->desc.interleave) {
        fs->m      = fs->desc.bo->get16(sb + sb_time_off(fs->desc.pack4, fs->desc.nicfree) + 10);
        fs->n      = fs->desc.bo->get16(sb + sb_time_off(fs->desc.pack4, fs->desc.nicfree) + 12);
        fs->unique = fs->desc.bo->get32(sb + sb_time_off(fs->desc.pack4, fs->desc.nicfree) + 26);
    }
    if (!fs->desc.ignore_magic && fs->desc.magic &&
        fs->desc.bo->get32(sb + fs->desc.magic_off) != fs->desc.magic) {
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
        fs->isize < (fs->desc.isize_count ? 1 : 2) ||
        fs->base + (uint64_t)fs->fsize * fs->desc.bsize > imgsize ||
        fs->fsize <= v7_data_first(fs)) {
        close(fs->fd);
        fs->fd = -1;
        return -EINVAL;
    }
    fs->imgsize = imgsize;
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
    uint64_t off = fs->base + (uint64_t)bno * fs->desc.bsize;
    if (filsys_check_range(fs, off, fs->desc.bsize))
        return -EINVAL;
    return fs->io->read(fs, buf, fs->desc.bsize, (off_t)off);
}

int v7fs_write_block(filsys_edition_t *fs, uint32_t bno, const uint8_t *buf) {
    if (fs->readonly)
        return -EROFS;
    uint64_t off = fs->base + (uint64_t)bno * fs->desc.bsize;
    if (filsys_check_range(fs, off, fs->desc.bsize))
        return -EINVAL;
    return fs->io->write(fs, buf, fs->desc.bsize, (off_t)off);
}

/* ---- superblock persistence --------------------------------------------- */

int v7fs_super_write(filsys_edition_t *fs) {
    if (fs->readonly)
        return 0;
    uint8_t sb[V7_MAXBSIZE];
    /* Read the current block to preserve the fields we don't maintain
     * (s_tfree, s_tinode, s_m, s_n, s_fname, s_fpack, ...).  System V keeps the
     * superblock at a fixed byte offset (512), not block 1. */
    if (fs->desc.dyn_bsize
            ? fs->io->read(fs, sb, 512, 512 + (off_t)fs->base)
            : v7fs_read_block(fs, V7_SUPERB, sb))
        return -EIO;
    if (fs->desc.sb_encode) {
        /* V8-family: the rearranged superblock is encoded by its own codec. */
        fs->desc.sb_encode(fs, sb);
    } else if (fs->desc.isize_count) {
        /* V6: a 16-bit superblock (s_fsize and the free cache are 2 bytes), and
         * no s_tfree/s_tinode -- those totals are recomputed on open. */
        fs->desc.bo->put16(sb + 0, fs->isize);
        fs->desc.bo->put16(sb + 2, (uint16_t)fs->fsize);
        fs->desc.bo->put16(sb + 4, fs->fl.nfree);
        for (int i = 0; i < fs->desc.nicfree; i++)
            fs->desc.bo->put16(sb + 6 + 2 * i, (uint16_t)fs->fl.free[i]);
        fs->desc.bo->put16(sb + 206, fs->fl.ninode);
        for (int i = 0; i < fs->desc.nicinod; i++)
            fs->desc.bo->put16(sb + 208 + 2 * i, fs->fl.inode[i]);
        fs->desc.bo->put32(sb + 412, (uint32_t)time(NULL));   /* s_time[2] */
        sb[410] = (uint8_t)(fs->fmod != 0);              /* s_fmod */
    } else {
        fs->desc.bo->put16(sb + 0, fs->isize);
        fs->desc.bo->put32(sb + sb_fsize_off(fs->desc.pack4), fs->fsize);
        fs->desc.bo->put16(sb + sb_nfree_off(fs->desc.pack4), fs->fl.nfree);
        for (int i = 0; i < fs->desc.nicfree; i++)
            fs->desc.bo->put32(sb + sb_free_off(fs->desc.pack4) + 4 * i, fs->fl.free[i]);
        fs->desc.bo->put16(sb + sb_ninode_off(fs->desc.pack4, fs->desc.nicfree), fs->fl.ninode);
        for (int i = 0; i < V7_NICINOD; i++)
            fs->desc.bo->put16(sb + sb_inode_off(fs->desc.pack4, fs->desc.nicfree) + 2 * i, fs->fl.inode[i]);
        uint32_t now = (uint32_t)time(NULL);
        fs->desc.bo->put32(sb + sb_time_off(fs->desc.pack4, fs->desc.nicfree), now);  /* s_time */
        sb[sb_time_off(fs->desc.pack4, fs->desc.nicfree) - fs->desc.fmod_back] = (uint8_t)(fs->fmod != 0); /* s_fmod */
        if (fs->desc.has_state)
            fs->desc.bo->put32(sb + V7_SYSV_STATE_OFF,
                          fs->fmod ? V7_SYSV_STATE_ACTIVE : V7_SYSV_STATE_CLEAN); /* s_state (R4 only) */
        if (!fs->desc.pack4 || fs->desc.magic) {
            int toff = sb_tfree_off(fs->desc.pack4, fs->desc.nicfree, fs->desc.has_dinfo);
            fs->desc.bo->put32(sb + toff, fs->fl.tfree);        /* s_tfree */
            fs->desc.bo->put16(sb + toff + 4, (uint16_t)fs->fl.tinode);   /* s_tinode */
        }
        if (fs->desc.interleave) {
            fs->desc.bo->put16(sb + sb_time_off(fs->desc.pack4, fs->desc.nicfree) + 10, fs->m);
            fs->desc.bo->put16(sb + sb_time_off(fs->desc.pack4, fs->desc.nicfree) + 12, fs->n);
            fs->desc.bo->put32(sb + sb_time_off(fs->desc.pack4, fs->desc.nicfree) + 26, fs->unique);
        }
    }
    if (fs->desc.dyn_bsize
            ? fs->io->write(fs, sb, 512, 512 + (off_t)fs->base)
            : v7fs_write_block(fs, V7_SUPERB, sb))
        return -EIO;
    fs->fl_dirty = 0;   /* the on-disk free list now matches the in-core one */
    return 0;
}

/* The V8-family block allocator (alloc_v8bitmap.c): inode allocation is the
 * shared free-list cache, the block side is the bitmap. */
const alloc_ops_t v8_bitmap_alloc_ops = {
    .balloc = v8_bitmap_balloc,
    .bfree  = v8_bitmap_bfree,
    .ialloc = v7fs_ialloc,
    .ifree  = v7fs_ifree,
    .sync   = v8_bitmap_sync,
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
    const uint8_t *d = raw + off * fs->desc.inode_size;
    memset(ip, 0, sizeof(*ip));
    ip->ino   = ino;
    ip->mode  = fs->desc.bo->get16(d + 0);
    ip->nlink = (int16_t)fs->desc.bo->get16(d + 2);
    ip->uid   = (int16_t)fs->desc.bo->get16(d + 4);
    ip->gid   = (int16_t)fs->desc.bo->get16(d + 6);
    ip->size  = fs->desc.bo->get32(d + 8);
    for (int i = 0; i < fs->desc.niaddr; i++)
        ip->addr[i] = fs->desc.addr_width == 4
                    ? fs->desc.bo->get32(d + 12 + 4 * i)
                    : fs->desc.bo->get24(d + 12 + 3 * i);
    ip->atime = fs->desc.bo->get32(d + 52);
    ip->mtime = fs->desc.bo->get32(d + 56);
    ip->ctime = fs->desc.bo->get32(d + 60);
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
        int rc = v7fs_super_write(fs);
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
    uint8_t *d = raw + off * fs->desc.inode_size;
    fs->desc.bo->put16(d + 0, (uint16_t)ip->mode);
    fs->desc.bo->put16(d + 2, (uint16_t)ip->nlink);
    fs->desc.bo->put16(d + 4, (uint16_t)ip->uid);
    fs->desc.bo->put16(d + 6, (uint16_t)ip->gid);
    fs->desc.bo->put32(d + 8, ip->size);
    for (int i = 0; i < fs->desc.niaddr; i++) {
        if (fs->desc.addr_width == 4)
            fs->desc.bo->put32(d + 12 + 4 * i, ip->addr[i]);
        else
            fs->desc.bo->put24(d + 12 + 3 * i, ip->addr[i]);
    }
    fs->desc.bo->put32(d + 52, ip->atime);
    fs->desc.bo->put32(d + 56, ip->mtime);
    fs->desc.bo->put32(d + 60, ip->ctime);
    return v7fs_write_block(fs, bno, raw);
}

/* ---- file data ---------------------------------------------------------- */

ssize_t filsys_file_read(filsys_edition_t *fs, v7_inode_t *ip, uint8_t *buf, size_t size, off_t off) {
    if (off < 0)
        return -EINVAL;
    if ((uint64_t)off >= ip->size)
        return 0;
    uint64_t remaining = ip->size - (uint64_t)off;
    if (size > remaining)
        size = (size_t)remaining;

    size_t done = 0;
    while (done < size) {
        uint32_t lbn  = (uint32_t)((off + (off_t)done) / fs->desc.bsize);
        uint32_t boff = (uint32_t)((off + (off_t)done) % fs->desc.bsize);
        uint32_t pbn;
        if (fs->desc.ops->inode->bmap(fs, ip, lbn, 0, &pbn))
            return -EIO;
        uint8_t blk[V7_MAXBSIZE];
        if (pbn == 0) {
            memset(blk, 0, fs->desc.bsize);   /* sparse hole */
        } else if (fs->desc.ops->blk_get(fs, pbn, blk)) {
            return -EIO;
        }
        size_t n = fs->desc.bsize - boff;
        if (n > size - done)
            n = size - done;
        memcpy(buf + done, blk + boff, n);
        done += n;
    }
    return (ssize_t)done;
}

ssize_t filsys_file_write(filsys_edition_t *fs, v7_inode_t *ip, const uint8_t *buf, size_t size, off_t off) {
    if (off < 0)
        return -EINVAL;

    size_t done = 0;
    while (done < size) {
        uint32_t lbn  = (uint32_t)((off + (off_t)done) / fs->desc.bsize);
        uint32_t boff = (uint32_t)((off + (off_t)done) % fs->desc.bsize);
        uint32_t pbn;
        int rc = fs->desc.ops->inode->bmap(fs, ip, lbn, 1, &pbn);
        if (rc)
            return rc;   /* -EIO (allocator commit) or -ENOSPC or -EFBIG */
        if (pbn == 0)
            return -ENOSPC;

        uint8_t blk[V7_MAXBSIZE];
        if (fs->desc.ops->blk_get(fs, pbn, blk))
            return -EIO;

        size_t n = fs->desc.bsize - boff;
        if (n > size - done)
            n = size - done;
        memcpy(blk + boff, buf + done, n);
        if (fs->desc.ops->blk_put(fs, pbn, blk))
            return -EIO;
        done += n;
    }
    if ((uint64_t)off + size > ip->size)
        ip->size = (uint32_t)((uint64_t)off + size);
    int rc = fs->desc.ops->inode->write_inode(fs, ip->ino, ip);
    if (rc)
        return rc;   /* the inode write failed: don't report a partial write */
    return (ssize_t)done;
}

/* ---- path lookup -------------------------------------------------------- */

int filsys_path_lookup(filsys_edition_t *fs, const char *path, uint32_t *ino, v7_inode_t *ip) {
    if (path[0] != '/')
        return -EINVAL;
    uint32_t cur = fs->desc.rootino;
    v7_inode_t dip;
    if (fs->desc.ops->inode->read_inode(fs, cur, &dip))
        return -EIO;

    const char *p = path + 1;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (len == 0) {
            p++;
            continue;
        }
        if (len > fs->desc.max_namlen)
            return -ENAMETOOLONG;
        char name[64];
        memcpy(name, p, len);
        name[len] = 0;

        if (!fs_is_dir(fs, &dip))
            return -ENOTDIR;
        uint32_t next;
        int rc = filsys_dir_lookup(fs, &dip, name, &next);
        if (rc)
            return rc;
        if (fs->desc.ops->inode->read_inode(fs, next, &dip))
            return -EIO;
        p = slash ? slash + 1 : p + len;
    }
    *ino = dip.ino;
    if (ip)
        *ip = dip;
    return 0;
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
    return fs->fsize - fs->v8_nblks;
}

int v7fs_check(filsys_edition_t *fs, v7_check_t *rep, int mode) {
    return filsys_check_common(&fs->desc, fs, rep, mode);
}

/* ---- ops table ----------------------------------------------------------
 * Each op takes `void *` (the backend state).  The adapters forward to the
 * typed backend function; the `void *` argument converts implicitly to
 * filsys_edition_t*, so there is no cast anywhere. */


static uint32_t v7fs_blocksize_op(const filsys_desc_t *fs) {
    return ((const filsys_edition_t *)fs)->desc.bsize;
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
    uint16_t isz = fmt->desc.bo->get16(sb);
    if (isz < 3)
        return 0;
    int nfree_off  = sb_nfree_off(fmt->desc.pack4);
    int ninode_off = sb_ninode_off(fmt->desc.pack4, fmt->desc.nicfree);
    int free_off   = sb_free_off(fmt->desc.pack4);
    uint16_t nfree  = fmt->desc.bo->get16(sb + nfree_off);
    uint16_t ninode = fmt->desc.bo->get16(sb + ninode_off);
    if (nfree > fmt->desc.nicfree || ninode > V7_NICINOD)
        return 0;
    uint32_t fsz = fmt->desc.bo->get32(sb + sb_fsize_off(fmt->desc.pack4));
    if (fsz <= isz || base + (uint64_t)fsz * bsize > nbytes)
        return 0;
    for (int i = 0; i < nfree; i++) {
        uint32_t fb = fmt->desc.bo->get32(sb + free_off + 4 * i);
        if (fb != 0 && (fb < isz || fb >= fsz))
            return 0;
    }
    uint32_t head = fmt->desc.bo->get32(sb + free_off);
    int fb_free = fmt->desc.pack4 ? 4 : 2;
    uint32_t s = 0;
    const char *why = NULL;
    if (probe_walk_chain(fmt, io, head, isz, fsz, base, bsize, fmt->desc.nicfree, 4,
                         fmt->desc.bo->get16, fmt->desc.bo->get32, fb_free, &s, &why) < 0) {
        res->why = why; res->isize = isz; res->fsize = fsz; res->segs = s;
        res->class = fmt->desc.nicfree == V7_COH_NICFREE ? "Coherent"
                   : fmt->desc.pack4 ? "32V or sysiii" : "v7, sysiii or sysvr1";
        return 2;
    }
    if (probe_root_ok(fmt, io, base, bsize, fmt->desc.bo->get16, fmt->desc.bo->get32, &why) < 0) {
        res->why = why; res->isize = isz; res->fsize = fsz; res->segs = s;
        res->class = fmt->desc.nicfree == V7_COH_NICFREE ? "Coherent"
                   : fmt->desc.pack4 ? "32V or sysiii" : "v7, sysiii or sysvr1";
        return 2;
    }
    res->class = fmt->desc.nicfree == V7_COH_NICFREE ? "Coherent"
               : fmt->desc.pack4 ? "32V or sysiii" : "v7, sysiii or sysvr1";
    res->isize = isz; res->fsize = fsz; res->segs = s;
    res->blocksize = bsize;
    res->conf = FILSYS_PROBE_STRUCTURAL;
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
    res->conf = FILSYS_PROBE_MAGIC;   /* the magic word matched */

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
    res->conf = FILSYS_PROBE_MAGIC;   /* the magic word matched */
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
    res->conf = FILSYS_PROBE_STRUCTURAL;
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
            res->conf = FILSYS_PROBE_STRUCTURAL;   /* v8_sb_decode validated the layout */
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
    res->conf = FILSYS_PROBE_STRUCTURAL;
    return 1;
}

/* The V7-family dispatcher: route on the descriptor's magic / sb_decode / block
 * size.  One probe serves V7/32V/Coherent/Xenix/2.9BSD/SysIII/SysV/V8/V9/V10. */
static int v7_probe(filsys_edition_t *fmt, const filsys_io_t *io,
                    uint64_t base, int bsize, uint64_t nbytes, filsys_probe_t *res)
{
    memset(res, 0, sizeof *res);
    res->freemap = -1;
    if (fmt->desc.sb_decode)
        return probe_v8(fmt, io, base, bsize, nbytes, res);
    if (fmt->desc.magic == V7_XEN_MAGIC)
        return probe_xenix(fmt, io, base, bsize, nbytes, res);
    if (fmt->desc.magic == V7_SYSV_MAGIC)
        return probe_sysv(fmt, io, base, bsize, nbytes, res);
    if (fmt->desc.bsize == 1024)
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
    .bmap        = filsys_bmap,
    .inode_state = v7_inode_state,
    .allocated_blocks = filsys_allocated_blocks,
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

/* ---- 2.11BSD inode state and check (dirs live in dir_bsd211.c) ------------ */

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
    return filsys_check_common(&fs->desc, fs, rep, mode);
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
    .bmap        = filsys_bmap,
    .inode_state = bsd211_inode_state,
    .allocated_blocks = filsys_allocated_blocks,
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

static int v6_write_inode(filsys_edition_t *fs, uint32_t ino, const v7_inode_t *ip);

/* Recompute the V7-family free-space totals (free blocks + free inodes) by
 * walking the free-list chain and the i-list.  Used for the pack4, magic-free
 * editions (32V and System III), whose on-disk s_tfree/s_tinode are unmaintained
 * and so unreliable on foreign media.  Read-only: never mutates the image. */


int v6_read_inode(filsys_edition_t *fs, uint32_t ino, v7_inode_t *ip) {
    if (ino == 0)
        return -EINVAL;
    uint32_t bno = v7_itod(fs, ino);
    uint32_t off = v7_itoo(fs, ino);
    if (bno >= v6_data_start(fs->isize))   /* i-list is blocks 2..s_isize+1 */
        return -EINVAL;
    uint8_t raw[V6_BSIZE];
    if (v7fs_read_block(fs, bno, raw))
        return -EIO;
    const uint8_t *d = raw + off * fs->desc.inode_size;
    memset(ip, 0, sizeof(*ip));
    ip->ino   = ino;
    ip->mode  = fs->desc.bo->get16(d + 0);
    ip->nlink = (int16_t)d[2];        /* char */
    ip->uid   = (int16_t)d[3];        /* char */
    ip->gid   = (int16_t)d[4];        /* char */
    ip->size  = ((uint32_t)d[5] << 16) | fs->desc.bo->get16(d + 6);
    for (int i = 0; i < fs->desc.niaddr; i++)
        ip->addr[i] = fs->desc.bo->get16(d + 8 + 2 * i);
    ip->atime = fs->desc.bo->get32(d + 24);
    ip->mtime = fs->desc.bo->get32(d + 28);
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
        int rc = v7fs_super_write(fs);
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
    uint8_t *d = raw + off * fs->desc.inode_size;
    fs->desc.bo->put16(d + 0, (uint16_t)ip->mode);
    d[2] = (uint8_t)ip->nlink;
    d[3] = (uint8_t)ip->uid;
    d[4] = (uint8_t)ip->gid;
    d[5] = (uint8_t)((ip->size >> 16) & 0xff);
    fs->desc.bo->put16(d + 6, (uint16_t)(ip->size & 0xffff));
    for (int i = 0; i < fs->desc.niaddr; i++)
        fs->desc.bo->put16(d + 8 + 2 * i, (uint16_t)ip->addr[i]);
    fs->desc.bo->put32(d + 24, ip->atime);
    fs->desc.bo->put32(d + 28, ip->mtime);
    return v7fs_write_block(fs, bno, raw);
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
    v7fs_super_write(f);
    return nfree;
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
    return filsys_check_common(&fs->desc, fs, rep, mode);
}

/* ---- V6 ops table: shared engine + V6 inode/bmap/check ------------------- */






static const struct filsys_inode_ops inode_32 = {
    .read_inode  = v6_read_inode,
    .write_inode = v6_write_inode,
    .bmap        = filsys_bmap,
    .inode_state = v6_inode_state,
    .allocated_blocks = filsys_allocated_blocks,
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
    .balloc = v7fs_balloc,
    .bfree  = v7fs_bfree,
    .ialloc = v7fs_ialloc,
    .ifree  = v7fs_ifree,
    .sync   = v7fs_sync,
};
