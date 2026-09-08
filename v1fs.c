/* filsys 1.8.0 - 2026-09-06 - Copyright (C) 2026 David Walther */
/* SPDX-License-Identifier: ISC */
/* v1fs.c - First Edition (V1) Unix filesystem, on-disk access layer.
 *
 * Reads and writes a V1 filesystem image.  The allocator is a pair of bitmaps
 * in the superblock (a free-block map where 1 = free, and an inode map where
 * 0 = free, indexed from inode 41); there is no s_isize -- the i-list size is
 * derived from the inode-map size.  Directory entries are 10 bytes.  See
 * v1fs.h.
 */
#include <config.h>
#include "v1fs.h"
#include "filsys_ops.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

static int super_write(v1fs_t *fs);

/* ---- lifecycle --------------------------------------------------------- */

int v1fs_open(v1fs_t *fs, const char *path, int readonly,
              const filsys_edition_t *proto, uint64_t offset) {
    if (fs != proto)
        memcpy(fs, proto, sizeof *fs); /* copy the static descriptor fields */
    fs->readonly = readonly;
    fs->base = offset;
    fs->fd = open(path, readonly ? O_RDONLY : O_RDWR);
    if (fs->fd < 0)
        return -errno;
    fs->io = &filsys_io_file;

    uint8_t sb[V1_BSIZE * 2];
    if (v1fs_read_block(fs, 0, sb) || v1fs_read_block(fs, 1, sb + V1_BSIZE)) {
        close(fs->fd);
        return -EIO;
    }

    fs->bm.freemap_bytes = bo_get16le(sb + 0);
    uint32_t freemap_off = 2;
    uint32_t inodemap_bytes_off = freemap_off + fs->bm.freemap_bytes;
    fs->bm.inodemap_bytes = bo_get16le(sb + inodemap_bytes_off);
    uint32_t inodemap_off = inodemap_bytes_off + 2;

    /* Reject a superblock whose maps spill past blocks 0+1, or a zero map. */
    if (fs->bm.freemap_bytes == 0 ||
        inodemap_off + fs->bm.inodemap_bytes > V1_BSIZE * 2) {
        close(fs->fd);
        fs->fd = -1;
        return -EINVAL;
    }

    fs->fsize = (uint32_t)fs->bm.freemap_bytes * 8;
    fs->maxino = (uint32_t)fs->bm.inodemap_bytes * 8;

    /* Reject a filesystem that claims more disk than the image holds, or one
     * with no data area (i-list must leave at least one data block). */
    uint64_t imgsize;
    if (filsys_dev_size(fs->fd, &imgsize) != 0 ||
        fs->base + (uint64_t)fs->fsize * V1_BSIZE > imgsize ||
        fs->fsize <= v1_data_start(fs->maxino)) {
        close(fs->fd);
        fs->fd = -1;
        return -EINVAL;
    }

    fs->bm.freemap = malloc(fs->bm.freemap_bytes);
    fs->bm.inodemap = malloc(fs->bm.inodemap_bytes);
    if (!fs->bm.freemap || !fs->bm.inodemap) {
        free(fs->bm.freemap);
        free(fs->bm.inodemap);
        close(fs->fd);
        fs->fd = -1;
        return -ENOMEM;
    }
    memcpy(fs->bm.freemap, sb + freemap_off, fs->bm.freemap_bytes);
    memcpy(fs->bm.inodemap, sb + inodemap_off, fs->bm.inodemap_bytes);

    /* Seed the free-space totals for statfs. */
    for (uint32_t b = v1_data_start(fs->maxino); b < fs->fsize; b++)
        if (fs->bm.freemap[b >> 3] & (1u << (b & 7)))
            fs->bm.tfree++;
    for (uint32_t i = V1_ROOTINO; i <= fs->maxino; i++) {
        uint32_t bit = i - V1_ROOTINO;
        if (!(fs->bm.inodemap[bit >> 3] & (1u << (bit & 7))))
            fs->bm.tinode++;
    }
    return 0;
}

void v1fs_close(v1fs_t *fs) {
    if (fs->fd >= 0) {
        if (!fs->readonly)
            super_write(fs);
        close(fs->fd);
        fs->fd = -1;
    }
    free(fs->bm.freemap);
    free(fs->bm.inodemap);
    fs->bm.freemap = NULL;
    fs->bm.inodemap = NULL;
}

int v1fs_sync(v1fs_t *fs) {
    if (fs->readonly)
        return 0;
    return super_write(fs);
}

/* ---- block io ---------------------------------------------------------- */

int v1fs_read_block(v1fs_t *fs, uint32_t bno, uint8_t *buf) {
    return fs->io->read(fs, buf, V1_BSIZE, (off_t)bno * V1_BSIZE + (off_t)fs->base);
}

int v1fs_write_block(v1fs_t *fs, uint32_t bno, const uint8_t *buf) {
    if (fs->readonly)
        return -EROFS;
    return fs->io->write(fs, buf, V1_BSIZE, (off_t)bno * V1_BSIZE + (off_t)fs->base);
}

/* ---- superblock persistence --------------------------------------------- */

static int super_write(v1fs_t *fs) {
    if (fs->readonly)
        return 0;
    uint8_t sb[V1_BSIZE * 2];
    if (v1fs_read_block(fs, 0, sb) || v1fs_read_block(fs, 1, sb + V1_BSIZE))
        return -EIO;
    uint32_t freemap_off = 2;
    uint32_t inodemap_off = freemap_off + fs->bm.freemap_bytes + 2;
    memcpy(sb + freemap_off, fs->bm.freemap, fs->bm.freemap_bytes);
    memcpy(sb + inodemap_off, fs->bm.inodemap, fs->bm.inodemap_bytes);
    if (v1fs_write_block(fs, 0, sb) || v1fs_write_block(fs, 1, sb + V1_BSIZE))
        return -EIO;
    fs->fl_dirty = 0;
    return 0;
}

/* ---- inode io ---------------------------------------------------------- */

int v1fs_read_inode(v1fs_t *fs, uint32_t ino, v1_inode_t *ip) {
    if (ino == 0 || ino > fs->maxino)
        return -EINVAL;
    uint32_t bno = v1_itod(ino);
    uint32_t off = v1_itoo(ino);
    uint8_t raw[V1_BSIZE];
    if (v1fs_read_block(fs, bno, raw))
        return -EIO;
    const uint8_t *d = raw + off * V1_INODESZ;
    memset(ip, 0, sizeof(*ip));
    ip->ino   = ino;
    ip->mode  = bo_get16le(d + 0);
    ip->nlink = (int16_t)d[2];        /* char */
    ip->uid   = (int16_t)d[3];        /* char */
    ip->gid   = 0;                    /* V1 has no gid */
    ip->size  = bo_get16le(d + 4);
    for (int i = 0; i < V1_NIADDR; i++)
        ip->addr[i] = bo_get16le(d + 6 + 2 * i);
    /* V1 stores times as a 60 Hz clock tick count (60ths of a second), not
     * whole seconds like V6/V7; convert to seconds for the POSIX-facing layer.
     * The 32-bit counter wraps every ~2.27 years -- an inherent V1 quirk. */
    ip->ctime = bo_get32me(d + 22) / 60;
    ip->mtime = bo_get32me(d + 26) / 60;
    ip->atime = ip->mtime;            /* V1 has no atime */
    return 0;
}

int v1fs_write_inode(v1fs_t *fs, uint32_t ino, const v1_inode_t *ip) {
    if (ino == 0 || ino > fs->maxino)
        return -EINVAL;
    /* Allocator state before reference: flush the bitmap before this inode's
     * reference to a newly-allocated block/inode, so a crash can't leave it both
     * free and referenced. */
    if (fs->fl_dirty) {
        int rc = super_write(fs);
        if (rc)
            return rc;
    }
    uint32_t bno = v1_itod(ino);
    uint32_t off = v1_itoo(ino);
    uint8_t raw[V1_BSIZE];
    if (v1fs_read_block(fs, bno, raw))
        return -EIO;
    uint8_t *d = raw + off * V1_INODESZ;
    bo_put16le(d + 0, (uint16_t)ip->mode);
    d[2] = (uint8_t)ip->nlink;
    d[3] = (uint8_t)ip->uid;
    bo_put16le(d + 4, (uint16_t)ip->size);
    for (int i = 0; i < V1_NIADDR; i++)
        bo_put16le(d + 6 + 2 * i, (uint16_t)ip->addr[i]);
    bo_put32me(d + 22, ip->ctime * 60u);   /* seconds -> 60ths of a second */
    bo_put32me(d + 26, ip->mtime * 60u);
    return v1fs_write_block(fs, bno, raw);
}

/* ---- allocation (bitmap) ------------------------------------------------ */

int v1fs_balloc(v1fs_t *fs, uint32_t *bno) {
    for (uint32_t b = v1_data_start(fs->maxino); b < fs->fsize; b++) {
        if (fs->bm.freemap[b >> 3] & (1u << (b & 7))) {          /* 1 = free */
            fs->bm.freemap[b >> 3] &= (uint8_t)~(1u << (b & 7)); /* mark used */
            if (fs->bm.tfree) fs->bm.tfree--;
            *bno = b;
            /* Zero the freshly-allocated block so a deleted file's data
             * doesn't leak into a new one (V7's alloc() clrbuf()s). */
            uint8_t z[V1_BSIZE] = {0};
            if (v1fs_write_block(fs, b, z))
                return -EIO;
            fs->fl_dirty = 1;   /* free map changed: flush before reference */
            return 0;
        }
    }
    return -ENOSPC;
}

void v1fs_bfree(v1fs_t *fs, uint32_t bno) {
    if (bno < v1_data_start(fs->maxino) || bno >= fs->fsize)
        return;
    fs->bm.freemap[bno >> 3] |= (uint8_t)(1u << (bno & 7));
    fs->bm.tfree++;
}

int v1fs_ialloc(v1fs_t *fs, uint32_t *ino) {
    for (uint32_t i = V1_ROOTINO; i <= fs->maxino; i++) {
        uint32_t bit = i - V1_ROOTINO;                        /* first bit = inode 41 */
        if (!(fs->bm.inodemap[bit >> 3] & (1u << (bit & 7)))) {  /* 0 = free */
            fs->bm.inodemap[bit >> 3] |= (uint8_t)(1u << (bit & 7));
            if (fs->bm.tinode) fs->bm.tinode--;
            fs->fl_dirty = 1;   /* inode map changed: flush before reference */
            *ino = i;
            return 0;
        }
    }
    return -ENOSPC;
}

void v1fs_ifree(v1fs_t *fs, uint32_t ino) {
    if (ino < V1_ROOTINO || ino > fs->maxino)
        return;
    uint32_t bit = ino - V1_ROOTINO;
    fs->bm.inodemap[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
    fs->bm.tinode++;
}

/* ---- block mapping ------------------------------------------------------ */

/* Single indirect: *slot -> block, entry `idx` (2-byte entries). */
static int ind1(v1fs_t *fs, uint32_t *slot, uint32_t idx, int create, uint32_t *out) {
    uint32_t blk = *slot;
    if (blk == 0) {
        if (!create) { *out = 0; return 0; }
        uint8_t z[V1_BSIZE];
        memset(z, 0, V1_BSIZE);
        int rc = v1fs_balloc(fs, &blk);
        if (rc)
            return rc;   /* -EIO (zeroing write) or -ENOSPC */
        if (v1fs_write_block(fs, blk, z))
            return -EIO;
        *slot = blk;
    }
    uint8_t buf[V1_BSIZE];
    if (v1fs_read_block(fs, blk, buf))
        return -EIO;
    uint32_t nb = bo_get16le(buf + 2 * idx);
    if (nb == 0 && create) {
        int rc = v1fs_balloc(fs, &nb);
        if (rc)
            return rc;
        bo_put16le(buf + 2 * idx, (uint16_t)nb);
        if (v1fs_write_block(fs, blk, buf))
            return -EIO;
    }
    *out = nb;
    return 0;
}

int v1fs_bmap(v1fs_t *fs, v1_inode_t *ip, uint32_t lbn, int create, uint32_t *bno) {
    /* A write past the eight direct slots promotes a small file to a large one:
     * the eight direct block numbers move into the first indirect block. */
    if (!(ip->mode & V1_ILARG) && create && lbn >= V1_NDADDR) {
        uint32_t iblk;
        int rc = v1fs_balloc(fs, &iblk);
        if (rc)
            return rc;
        uint8_t buf[V1_BSIZE] = {0};
        for (int i = 0; i < V1_NDADDR; i++)
            bo_put16le(buf + 2 * i, (uint16_t)ip->addr[i]);
        if (v1fs_write_block(fs, iblk, buf))
            return -EIO;
        for (int i = 0; i < V1_NDADDR; i++)
            ip->addr[i] = 0;
        ip->addr[0] = iblk;
        ip->mode |= V1_ILARG;
    }

    if (ip->mode & V1_ILARG) {
        if (lbn >= V1_NIADDR * V1_NINDIR) {   /* 8 single-indirect slots */
            *bno = 0;
            return create ? -EFBIG : 0;
        }
        return ind1(fs, &ip->addr[lbn >> 8], lbn & (V1_NINDIR - 1), create, bno);
    }
    if (lbn >= V1_NDADDR) {
        *bno = 0;
        return create ? -EFBIG : 0;
    }
    uint32_t nb = ip->addr[lbn];
    if (nb == 0 && create) {
        int rc = v1fs_balloc(fs, &nb);
        if (rc)
            return rc;
        ip->addr[lbn] = nb;
    }
    *bno = nb;
    return 0;
}

/* Rebuild the free-block map from the usage bitmap (icheck -s).  Only the
 * data area can be free; the superblock, i-list and device slots stay used. */
static uint32_t v1fs_makefree(filsys_edition_t *fs, filsys_chkctx_t *cx)
{
    v1fs_t *f = fs;
    uint32_t dstart = v1_data_start(f->maxino);
    uint32_t nfree = 0;
    memset(f->bm.freemap, 0, f->bm.freemap_bytes);
    for (uint32_t b = dstart; b < f->fsize; b++) {
        uint32_t off = b - dstart;
        if (!(cx->bmap[off >> 3] & (uint8_t)(1u << (off & 7)))) {
            f->bm.freemap[b >> 3] |= (uint8_t)(1u << (b & 7));
            nfree++;
        }
    }
    f->bm.tfree = nfree;
    super_write(f);
    return nfree;
}

/* Classify an inode into a checker state.  V1 has only an IFDIR type bit; a
 * device is a reserved i-number (below V1_ROOTINO), not a mode bit, and every
 * other allocated inode is a regular file -- so there is no unknown type. */
static uint8_t v1_inode_state(filsys_edition_t *fs, uint32_t ino, uint32_t mode) {
    (void)fs;
    if (mode == 0)
        return FILSYS_IN_UNALLOC;
    if (ino < V1_ROOTINO)
        return FILSYS_IN_ICHR;   /* reserved device inode */
    if (mode & V1_IFDIR)
        return FILSYS_IN_IDIR;
    return FILSYS_IN_IREG;
}

static uint32_t v1_chk_maxino(filsys_edition_t *fs)     { return ((v1fs_t *)fs)->maxino; }
static uint32_t v1_chk_data_start(filsys_edition_t *fs) { v1fs_t *f = fs; return v1_data_start(f->maxino); }
static uint32_t v1_chk_data_end(filsys_edition_t *fs)   { return ((v1fs_t *)fs)->fsize; }
static int v1_chk_is_clean(filsys_edition_t *fs)        { (void)fs; return 0; }

/* V1's allocator is a bitmap: a block is free iff its freemap bit is set. */
static void v1_chk_walk_free(filsys_edition_t *fs, filsys_chkctx_t *cx, filsys_check_t *rep)
{
    v1fs_t *f = fs;
    uint32_t dstart = v1_data_start(f->maxino);
    for (uint32_t b = dstart; b < f->fsize; b++) {
        int fre = f->bm.freemap[b >> 3] & (uint8_t)(1u << (b & 7));
        if (!fre)
            continue;
        rep->free_blocks++;
        uint32_t off = b - dstart;
        uint8_t m = (uint8_t)(1u << (off & 7));
        if (cx->bmap[off >> 3] & m) {
            printf("block %u used and free\n", b);
            cx->dup_blocks++;
            rep->errors++;
        } else {
            cx->bmap[off >> 3] |= m;
        }
    }
}

int v1fs_check(v1fs_t *fs, v1_check_t *rep, int mode) {
    filsys_edition_t fmt = filsys_getformat(FILSYS_V1);
    return filsys_check_common(&fmt, fs, rep, mode);
}

/* ---- ops table ----------------------------------------------------------
 * Each op takes `void *` (the backend state).  The adapters forward to the
 * typed backend function; the `void *` argument converts implicitly to
 * v1fs_t*, so there is no cast anywhere. */


static uint32_t v1fs_blocksize_op(const filsys_edition_t *fs) { (void)fs; return V1_BSIZE; }


















static int v1fs_check_op(filsys_edition_t *fs) { v1_check_t rep; return v1fs_check(fs, &rep, 0); }
static uint64_t v1fs_max_file_op(filsys_edition_t *fs) {
    (void)fs;
    /* The large-file flag can address a megabyte of blocks, but the 16-bit
     * size field caps a file at 65535 bytes (past that it wraps to zero). */
    return (1u << 16) - 1;
}

static void v1fs_statfs_op(filsys_edition_t *fs, struct statvfs *st) {
    v1fs_t *v1 = fs;
    st->f_blocks = v1->fsize;
    st->f_bfree = st->f_bavail = v1->bm.tfree;
    st->f_files = v1->maxino;
    st->f_ffree = v1->bm.tinode;
}
static const struct filsys_inode_ops inode_v1 = {
    .read_inode  = v1fs_read_inode,
    .write_inode = v1fs_write_inode,
    .bmap        = v1fs_bmap,
    .inode_state = v1_inode_state,
};

const struct filsys_ops v1fs_ops = {
    .name        = "v1",
    .dir         = &dir_fixed,
    .inode       = &inode_v1,
    .blocksize   = v1fs_blocksize_op,
    .open        = v1fs_open,
    .close       = v1fs_close,
    .sync        = v1fs_sync,
    .read_block  = v1fs_read_block,
    .write_block = v1fs_write_block,
    .blk_get     = v1fs_read_block,
    .blk_put     = v1fs_write_block,
    .ialloc      = v1fs_ialloc,
    .ifree       = v1fs_ifree,
    .file_read   = v7fs_file_read,
    .file_write  = v7fs_file_write,
    .dir_lookup  = v7fs_dir_lookup,
    .lookup      = v7fs_lookup,
    .check       = v1fs_check_op,
    .maxino      = v1_chk_maxino,
    .data_start  = v1_chk_data_start,
    .data_end    = v1_chk_data_end,
    .walk_free   = v1_chk_walk_free,
    .makefree    = v1fs_makefree,
    .is_clean    = v1_chk_is_clean,
    .statfs      = v1fs_statfs_op,
    .max_file    = v1fs_max_file_op,
};

/* ---- allocator vtable: V1's dual bitmap -------------------------------- */

const alloc_ops_t bitmap_alloc_ops = {
    .balloc = (int  (*)(void *, uint32_t *))v1fs_balloc,
    .bfree  = (void (*)(void *, uint32_t))v1fs_bfree,
    .ialloc = (int  (*)(void *, uint32_t *))v1fs_ialloc,
    .ifree  = (void (*)(void *, uint32_t))v1fs_ifree,
    .sync   = (int  (*)(void *))v1fs_sync,
};
