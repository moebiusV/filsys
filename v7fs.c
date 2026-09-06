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
static void v6_count_free(filsys_edition_t *fs, uint32_t *nblk, uint32_t *nino);

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

    uint8_t sb[V7_MAXBSIZE];
    if (v7fs_read_block(fs, V7_SUPERB, sb)) {
        close(fs->fd);
        return -EIO;
    }
    if (fs->isize_count) {
        /* V6: 16-bit superblock (s_fsize and the free cache are 2 bytes). */
        fs->isize  = bo_get16le(sb + 0);
        fs->fsize  = bo_get16le(sb + 2);
        fs->fl.nfree  = bo_get16le(sb + 4);
        for (int i = 0; i < fs->nicfree; i++)
            fs->fl.free[i] = bo_get16le(sb + 6 + 2 * i);
        fs->fl.ninode = bo_get16le(sb + 206);
        for (int i = 0; i < fs->nicinod; i++)
            fs->fl.inode[i] = bo_get16le(sb + 208 + 2 * i);
        fs->time   = fs->bo->get32(sb + 412);
        fs->fmod   = sb[410];              /* s_fmod */
    } else {
        fs->isize  = bo_get16le(sb + 0);
        fs->fsize  = fs->bo->get32(sb + sb_fsize_off(fs->pack4));
        fs->fl.nfree  = bo_get16le(sb + sb_nfree_off(fs->pack4));
        for (int i = 0; i < fs->nicfree; i++)
            fs->fl.free[i] = fs->bo->get32(sb + sb_free_off(fs->pack4) + 4 * i);
        fs->fl.ninode = bo_get16le(sb + sb_ninode_off(fs->pack4, fs->nicfree));
        for (int i = 0; i < V7_NICINOD; i++)
            fs->fl.inode[i] = bo_get16le(sb + sb_inode_off(fs->pack4, fs->nicfree) + 2 * i);
        fs->time   = fs->bo->get32(sb + sb_time_off(fs->pack4, fs->nicfree));
        fs->fmod   = sb[sb_time_off(fs->pack4, fs->nicfree) - fs->fmod_back];  /* s_fmod */
    }
    /* s_tfree/s_tinode carry the true free-space totals (v7fs_makefree writes
     * them); the 50/100-entry caches are only the in-core spill.  Read them so
     * statfs can report real free space rather than the cache depth.  V6 has no
     * such fields, so its totals are recomputed from the free list + i-list. */
    if (fs->isize_count)
        v6_count_free(fs, &fs->fl.tfree, &fs->fl.tinode);
    else if (!fs->pack4 || fs->magic) {
        int toff = sb_time_off(fs->pack4, fs->nicfree);
        fs->fl.tfree  = fs->bo->get32(sb + toff + 4);
        fs->fl.tinode = bo_get16le(sb + toff + 8);
    }
    if (fs->interleave) {
        fs->m      = bo_get16le(sb + sb_time_off(fs->pack4, fs->nicfree) + 10);
        fs->n      = bo_get16le(sb + sb_time_off(fs->pack4, fs->nicfree) + 12);
        fs->unique = fs->bo->get32(sb + sb_time_off(fs->pack4, fs->nicfree) + 26);
    }
    if (fs->magic && bo_get32le(sb + 0x3F8) != V7_XEN_MAGIC) {
        /* Xenix carries a magic at superblock offset 1016; validate it rather
         * than mis-decoding a non-Xenix volume named -v xenix. */
        close(fs->fd);
        fs->fd = -1;
        return -EINVAL;
    }

    /* Reject a superblock that claims more disk than the image file actually
     * holds, one with no data area, or an i-list too small to subtract 2 from
     * (`(isize - 2)` underflows when isize is 0 or 1, turning the inode walk
     * into a multi-gigabyte loop).  Without this, a corrupt image can make
     * the checker (and directory readers) allocate gigabytes. */
    struct stat st;
    if (fstat(fs->fd, &st) != 0 ||
        fs->isize < (fs->isize_count ? 1 : 2) ||
        fs->base + (uint64_t)fs->fsize * fs->bsize > (uint64_t)st.st_size ||
        fs->fsize <= v7_data_first(fs)) {
        close(fs->fd);
        fs->fd = -1;
        return -EINVAL;
    }
    return 0;
}

void v7fs_close(filsys_edition_t *fs) {
    if (fs->fd >= 0) {
        /* The superblock is flushed once here, not per alloc/free: V7's kernel
         * syncs the superblock periodically rather than on every block handoff,
         * and batching avoids one 512-byte pwrite per freed block on truncate.
         * A clean close clears s_fmod: the image is now consistent. */
        if (!fs->readonly) {
            fs->fmod = 0;
            super_write(fs);
        }
        close(fs->fd);
        fs->fd = -1;
    }
}

int v7fs_sync(filsys_edition_t *fs) {
    if (fs->readonly)
        return 0;
    return super_write(fs);
}

int v7fs_mark_dirty(filsys_edition_t *fs) {
    if (fs->readonly)
        return 0;
    fs->fmod = 1;
    return super_write(fs);
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
     * (s_tfree, s_tinode, s_m, s_n, s_fname, s_fpack, ...). */
    if (v7fs_read_block(fs, V7_SUPERB, sb))
        return -EIO;
    if (fs->isize_count) {
        /* V6: a 16-bit superblock (s_fsize and the free cache are 2 bytes), and
         * no s_tfree/s_tinode -- those totals are recomputed on open. */
        bo_put16le(sb + 0, fs->isize);
        bo_put16le(sb + 2, (uint16_t)fs->fsize);
        bo_put16le(sb + 4, fs->fl.nfree);
        for (int i = 0; i < fs->nicfree; i++)
            bo_put16le(sb + 6 + 2 * i, (uint16_t)fs->fl.free[i]);
        bo_put16le(sb + 206, fs->fl.ninode);
        for (int i = 0; i < fs->nicinod; i++)
            bo_put16le(sb + 208 + 2 * i, fs->fl.inode[i]);
        fs->bo->put32(sb + 412, (uint32_t)time(NULL));   /* s_time[2] */
        sb[410] = (uint8_t)(fs->fmod != 0);              /* s_fmod */
    } else {
        bo_put16le(sb + 0, fs->isize);
        fs->bo->put32(sb + sb_fsize_off(fs->pack4), fs->fsize);
        bo_put16le(sb + sb_nfree_off(fs->pack4), fs->fl.nfree);
        for (int i = 0; i < fs->nicfree; i++)
            fs->bo->put32(sb + sb_free_off(fs->pack4) + 4 * i, fs->fl.free[i]);
        bo_put16le(sb + sb_ninode_off(fs->pack4, fs->nicfree), fs->fl.ninode);
        for (int i = 0; i < V7_NICINOD; i++)
            bo_put16le(sb + sb_inode_off(fs->pack4, fs->nicfree) + 2 * i, fs->fl.inode[i]);
        fs->bo->put32(sb + sb_time_off(fs->pack4, fs->nicfree), (uint32_t)time(NULL));  /* s_time */
        sb[sb_time_off(fs->pack4, fs->nicfree) - fs->fmod_back] = (uint8_t)(fs->fmod != 0); /* s_fmod */
        if (!fs->pack4 || fs->magic) {
            int toff = sb_time_off(fs->pack4, fs->nicfree);
            fs->bo->put32(sb + toff + 4, fs->fl.tfree);        /* s_tfree */
            bo_put16le(sb + toff + 8, (uint16_t)fs->fl.tinode);   /* s_tinode */
        }
        if (fs->interleave) {
            bo_put16le(sb + sb_time_off(fs->pack4, fs->nicfree) + 10, fs->m);
            bo_put16le(sb + sb_time_off(fs->pack4, fs->nicfree) + 12, fs->n);
            fs->bo->put32(sb + sb_time_off(fs->pack4, fs->nicfree) + 26, fs->unique);
        }
    }
    if (v7fs_write_block(fs, V7_SUPERB, sb))
        return -EIO;
    fs->fl_dirty = 0;   /* the on-disk free list now matches the in-core one */
    return 0;
}

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
    ip->mode  = bo_get16le(d + 0);
    ip->nlink = (int16_t)bo_get16le(d + 2);
    ip->uid   = (int16_t)bo_get16le(d + 4);
    ip->gid   = (int16_t)bo_get16le(d + 6);
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
    bo_put16le(d + 0, (uint16_t)ip->mode);
    bo_put16le(d + 2, (uint16_t)ip->nlink);
    bo_put16le(d + 4, (uint16_t)ip->uid);
    bo_put16le(d + 6, (uint16_t)ip->gid);
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
        fs->fl.nfree = bo_get16le(buf + 0);
        for (int i = 0; i < fs->nicfree; i++)
            fs->fl.free[i] = v7_get_daddr(fs, buf + fb_free_off(fs->pack4) + fs->daddr_wid * i);
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
    if (bno < v7_data_first(fs) || bno >= fs->fsize)
        return;   /* badblock */
    if (fs->fl.nfree == 0) {
        fs->fl.nfree = 1;
        fs->fl.free[0] = 0;
    }
    if (fs->fl.nfree >= fs->nicfree) {
        uint8_t buf[V7_MAXBSIZE];
        memset(buf, 0, fs->bsize);
        bo_put16le(buf + 0, fs->fl.nfree);
        for (int i = 0; i < fs->nicfree; i++)
            v7_put_daddr(fs, buf + fb_free_off(fs->pack4) + fs->daddr_wid * i, fs->fl.free[i]);
        if (v7fs_write_block(fs, bno, buf) == 0)
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
            v7_inode_t ip;
            if (cand >= 2 && cand <= maxino &&
                fs->ops->read_inode(fs, cand, &ip) == 0 && ip.mode == 0) {
                memset(&ip, 0, sizeof(ip));
                ip.ino = cand;
                fs->ops->write_inode(fs, cand, &ip);
                if (fs->fl.tinode) fs->fl.tinode--;
                fs->fl_dirty = 1;   /* inode cache changed: flush before reference */
                *ino = cand;
                return 0;
            }
            continue;   /* was already allocated; look again */
        }
        /* Refill the cache with a linear scan of the i-list. */
        fs->fl.ninode = 0;
        for (uint32_t in = 2; in <= maxino && fs->fl.ninode < fs->nicinod; in++) {
            v7_inode_t ip;
            if (fs->ops->read_inode(fs, in, &ip))
                break;
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
            if (v7fs_balloc(fs, &blk) || v7fs_write_block(fs, blk, z))
                return -ENOSPC;
            *slot = blk;
        }
        uint8_t buf[V7_MAXBSIZE];
        if (v7fs_read_block(fs, blk, buf))
            return -EIO;
        uint32_t next = fs->bo->get32(buf + 4 * indices[L]);

        if (L == levels - 1) {
            if (next == 0 && create) {
                if (v7fs_balloc(fs, &next))
                    return -ENOSPC;
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
            if (v7fs_balloc(fs, &next) || v7fs_write_block(fs, next, z))
                return -ENOSPC;
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
            if (v7fs_balloc(fs, &nb))
                return -ENOSPC;
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
        if (fs->ops->bmap(fs, ip, lbn, 0, &pbn))
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
        if (fs->ops->bmap(fs, ip, lbn, 1, &pbn))
            return -ENOSPC;
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
    fs->ops->write_inode(fs, ip->ino, ip);
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
        uint16_t ino = bo_get16le(buf + off);
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
    int rc = fs->ops->dir_read(fs, ip, &ents, &count);
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
        if (bo_get16le(buf + off) == 0) {
            slot = off;
            break;
        }
    }
    if (slot == SIZE_MAX) {
        slot = (size_t)n;
        n += fs->dirent_size;
    }

    bo_put16le(buf + slot, (uint16_t)ino);
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
        if (bo_get16le(buf + off) == 0)
            continue;
        char ent[64];
        memcpy(ent, buf + off + 2, fs->max_namlen);
        ent[fs->max_namlen] = 0;
        if (strcmp(ent, name) == 0) {
            bo_put16le(buf + off, 0);
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
    if (fs->ops->read_inode(fs, cur, &dip))
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
        if (fs->ops->read_inode(fs, next, &dip))
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
static uint32_t v7fs_makefree(filsys_edition_t *fs, filsys_chkctx_t *cx)
{
    filsys_edition_t *f = fs;
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
    super_write(f);   /* writes f->fl.tfree (= nfree) and f->fl.tinode (= 0) */
    return nfree;
}

/* Classify an inode's mode into a checker state (the low three type bits). */
static uint8_t v7_inode_state(filsys_edition_t *fs, uint32_t ino, uint32_t mode) {
    (void)fs; (void)ino;
    if (mode == 0)
        return FILSYS_IN_UNALLOC;
    switch (mode & V7_IFMT) {
    case V7_IFDIR: return FILSYS_IN_IDIR;
    case V7_IFREG: return FILSYS_IN_IREG;
    case V7_IFCHR: case V7_IFMPC: return FILSYS_IN_ICHR;
    case V7_IFBLK: case V7_IFMPB: return FILSYS_IN_IBLK;
    case V7_IFIFO: return FILSYS_IN_IPIPE;
    default: return FILSYS_IN_UNKNOWN;
    }
}

static uint32_t v7_maxino(filsys_edition_t *fs)    { return v7_maxinode(fs); }
static uint32_t v7_data_start(filsys_edition_t *fs) { return v7_data_first(fs); }
static uint32_t v7_data_end(filsys_edition_t *fs)   { return ((filsys_edition_t *)fs)->fsize; }
static int v7_is_clean(filsys_edition_t *fs)        { return ((filsys_edition_t *)fs)->fmod == 0; }

/* Walk the free list exactly as alloc() would, marking free blocks into cx->bmap
 * (a free block already used is a duplicate) and counting free_blocks. */
static void v7_walk_free(filsys_edition_t *fs, filsys_chkctx_t *cx, filsys_check_t *rep)
{
    filsys_edition_t *f = fs;
    uint8_t *seen = calloc(f->fsize ? f->fsize : 1, 1);
    if (!seen)
        return;
    uint16_t n = f->fl.nfree;
    uint32_t cur[V7_XEN_NICFREE];
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
        rep->free_blocks++;
        uint32_t off = bno - v7_data_first(f);
        uint8_t m = (uint8_t)(1u << (off & 7));
        if (cx->bmap[off >> 3] & m) {
            printf("block %u dup; free-list\n", bno);
            cx->dup_blocks++;
            rep->errors++;
        } else {
            cx->bmap[off >> 3] |= m;
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
            n = bo_get16le(blk);
            if (n > f->nicfree) {
                printf("free-list block %u has bad count %u\n", bno, n);
                rep->errors++;
                break;
            }
            for (int i = 0; i < f->nicfree; i++)
                cur[i] = v7_get_daddr(f, blk + fb_free_off(f->pack4) + f->daddr_wid * i);
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



















static int v7fs_check_op(filsys_edition_t *fs) { v7_check_t rep; return v7fs_check(fs, &rep, 0); }
static uint64_t v7fs_max_file_op(filsys_edition_t *fs) {
    const filsys_edition_t *v7 = fs;
    uint64_t n = v7_nindir(v7);
    return ((uint64_t)v7->ndaddr + n + n * n + n * n * n) * v7->bsize;
}

static void v7fs_statfs_op(filsys_edition_t *fs, struct statvfs *st) {
    filsys_edition_t *v7 = fs;
    st->f_blocks = v7->fsize;
    st->f_bfree = st->f_bavail = v7->fl.tfree;
    st->f_files = (v7->isize - 2) * v7_inopb(v7);
    st->f_ffree = v7->fl.tinode;
}
const struct filsys_ops v7fs_ops = {
    .name        = "v7",
    .blocksize   = v7fs_blocksize_op,
    .open        = v7fs_open,
    .close       = v7fs_close,
    .sync        = v7fs_sync,
    .mark_dirty  = v7fs_mark_dirty,
    .read_block  = v7fs_read_block,
    .write_block = v7fs_write_block,
    .blk_get     = v7fs_read_block,
    .blk_put     = v7fs_write_block,
    .read_inode  = v7fs_read_inode,
    .write_inode = v7fs_write_inode,
    .ialloc      = v7fs_ialloc,
    .ifree       = v7fs_ifree,
    .bmap        = v7fs_bmap,
    .file_read   = v7fs_file_read,
    .file_write  = v7fs_file_write,
    .dir_read    = v7fs_dir_read,
    .dir_lookup  = v7fs_dir_lookup,
    .dir_add     = v7fs_dir_add,
    .dir_remove  = v7fs_dir_remove,
    .lookup      = v7fs_lookup,
    .check       = v7fs_check_op,
    .maxino      = v7_maxino,
    .data_start  = v7_data_start,
    .data_end    = v7_data_end,
    .inode_state = v7_inode_state,
    .walk_free   = v7_walk_free,
    .makefree    = v7fs_makefree,
    .is_clean    = v7_is_clean,
    .statfs      = v7fs_statfs_op,
    .max_file    = v7fs_max_file_op,
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
        uint16_t ino    = bo_get16le(buf + off);
        uint16_t reclen = bo_get16le(buf + off + 2);
        uint16_t namlen = bo_get16le(buf + off + 4);
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
        uint16_t d_ino    = bo_get16le(buf + off);
        uint16_t reclen   = bo_get16le(buf + off + 2);
        if (reclen < 6 || off + reclen > (size_t)n)
            break;
        if (d_ino == 0 && reclen >= need) {
            memset(buf + off, 0, reclen);
            bo_put16le(buf + off, (uint16_t)ino);
            bo_put16le(buf + off + 2, reclen);
            bo_put16le(buf + off + 4, (uint16_t)namlen);
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
    bo_put16le(ent, (uint16_t)ino);
    bo_put16le(ent + 2, (uint16_t)need);
    bo_put16le(ent + 4, (uint16_t)namlen);
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
        uint16_t d_ino    = bo_get16le(buf + off);
        uint16_t reclen   = bo_get16le(buf + off + 2);
        uint16_t namlen   = bo_get16le(buf + off + 4);
        if (reclen < 6 || off + reclen > (size_t)n)
            break;
        if (d_ino != 0 && namlen == strlen(name) &&
            memcmp(buf + off + 6, name, namlen) == 0) {
            bo_put16le(buf + off, 0);   /* mark free */
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

static int bsd211_is_clean(filsys_edition_t *fs) { (void)fs; return 0; }

int bsd211_check(filsys_edition_t *fs, v7_check_t *rep, int mode) {
    (void)mode;   /* 2.11BSD: check only, no salvage/preen */
    return filsys_check_common(fs, fs, rep, 0);
}

/* ---- 2.11BSD ops table: the shared V7 engine + variable-length dirents ---- */




static int bsd211_check_op(filsys_edition_t *fs) { v7_check_t rep; return bsd211_check(fs, &rep, 0); }

const struct filsys_ops bsd211fs_ops = {
    .name        = "bsd211",
    .blocksize   = v7fs_blocksize_op,
    .open        = v7fs_open,
    .close       = v7fs_close,
    .sync        = v7fs_sync,
    .mark_dirty  = v7fs_mark_dirty,
    .read_block  = v7fs_read_block,
    .write_block = v7fs_write_block,
    .blk_get     = v7fs_read_block,
    .blk_put     = v7fs_write_block,
    .read_inode  = v7fs_read_inode,
    .write_inode = v7fs_write_inode,
    .ialloc      = v7fs_ialloc,
    .ifree       = v7fs_ifree,
    .bmap        = v7fs_bmap,
    .file_read   = v7fs_file_read,
    .file_write  = v7fs_file_write,
    .dir_read    = bsd211_dir_read,
    .dir_lookup  = v7fs_dir_lookup,
    .dir_add     = bsd211_dir_add,
    .dir_remove  = bsd211_dir_remove,
    .lookup      = v7fs_lookup,
    .check       = bsd211_check_op,
    .maxino      = v7_maxino,
    .data_start  = v7_data_start,
    .data_end    = v7_data_end,
    .inode_state = bsd211_inode_state,
    .walk_free   = v7_walk_free,
    .makefree    = v7fs_makefree,
    .is_clean    = bsd211_is_clean,
    .statfs      = v7fs_statfs_op,
    .max_file    = v7fs_max_file_op,
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
            n = bo_get16le(blk + 0);
            for (int i = 0; i < fs->nicfree; i++)
                cur[i] = bo_get16le(blk + 2 + 2 * i);
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
    ip->mode  = bo_get16le(d + 0);
    ip->nlink = (int16_t)d[2];        /* char */
    ip->uid   = (int16_t)d[3];        /* char */
    ip->gid   = (int16_t)d[4];        /* char */
    ip->size  = ((uint32_t)d[5] << 16) | bo_get16le(d + 6);
    for (int i = 0; i < fs->niaddr; i++)
        ip->addr[i] = bo_get16le(d + 8 + 2 * i);
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
    bo_put16le(d + 0, (uint16_t)ip->mode);
    d[2] = (uint8_t)ip->nlink;
    d[3] = (uint8_t)ip->uid;
    d[4] = (uint8_t)ip->gid;
    d[5] = (uint8_t)((ip->size >> 16) & 0xff);
    bo_put16le(d + 6, (uint16_t)(ip->size & 0xffff));
    for (int i = 0; i < fs->niaddr; i++)
        bo_put16le(d + 8 + 2 * i, (uint16_t)ip->addr[i]);
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
        if (v7fs_balloc(fs, &blk) || v7fs_write_block(fs, blk, z))
            return -ENOSPC;
        *slot = blk;
    }
    uint8_t buf[V6_BSIZE];
    if (v7fs_read_block(fs, blk, buf))
        return -EIO;
    uint32_t nb = bo_get16le(buf + 2 * idx);
    if (nb == 0 && create) {
        if (v7fs_balloc(fs, &nb))
            return -ENOSPC;
        bo_put16le(buf + 2 * idx, (uint16_t)nb);
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
        if (v7fs_balloc(fs, &blk) || v7fs_write_block(fs, blk, z))
            return -ENOSPC;
        *slot = blk;
    }
    uint8_t buf[V6_BSIZE];
    if (v7fs_read_block(fs, blk, buf))
        return -EIO;
    uint32_t sub = bo_get16le(buf + 2 * o);
    if (sub == 0 && create) {
        uint8_t z[V6_BSIZE];
        memset(z, 0, V6_BSIZE);
        if (v7fs_balloc(fs, &sub) || v7fs_write_block(fs, sub, z))
            return -ENOSPC;
        bo_put16le(buf + 2 * o, (uint16_t)sub);
        if (v7fs_write_block(fs, blk, buf))
            return -EIO;
    }
    if (sub == 0) { *out = 0; return 0; }
    return v6_ind1(fs, &sub, i, create, out);
}

static int v6_bmap(filsys_edition_t *fs, v7_inode_t *ip, uint32_t lbn, int create, uint32_t *bno) {
    if (!(ip->mode & V6_ILARG) && create && lbn >= V6_NDADDR) {
        uint32_t iblk;
        if (v7fs_balloc(fs, &iblk))
            return -ENOSPC;
        uint8_t buf[V6_BSIZE] = {0};
        for (int i = 0; i < V6_NDADDR; i++)
            bo_put16le(buf + 2 * i, (uint16_t)ip->addr[i]);
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
        if (v7fs_balloc(fs, &nb))
            return -ENOSPC;
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






static int v6_check_op(filsys_edition_t *fs) { v7_check_t rep; return v6_check(fs, &rep, 0); }
static uint64_t v6_max_file_op(filsys_edition_t *fs) {
    (void)fs;
    /* The large-file layout can address ~32 MB, but the 24-bit size field
     * caps a file at 16777215 bytes. */
    return (1u << 24) - 1;
}

const struct filsys_ops v6fs_ops = {
    .name        = "v6",
    .blocksize   = v7fs_blocksize_op,
    .open        = v7fs_open,
    .close       = v7fs_close,
    .sync        = v7fs_sync,
    .mark_dirty  = v7fs_mark_dirty,
    .read_block  = v7fs_read_block,
    .write_block = v7fs_write_block,
    .blk_get     = v7fs_read_block,
    .blk_put     = v7fs_write_block,
    .read_inode  = v6_read_inode,
    .write_inode = v6_write_inode,
    .ialloc      = v7fs_ialloc,
    .ifree       = v7fs_ifree,
    .bmap        = v6_bmap,
    .file_read   = v7fs_file_read,
    .file_write  = v7fs_file_write,
    .dir_read    = v7fs_dir_read,
    .dir_lookup  = v7fs_dir_lookup,
    .dir_add     = v7fs_dir_add,
    .dir_remove  = v7fs_dir_remove,
    .lookup      = v7fs_lookup,
    .check       = v6_check_op,
    .maxino      = v7_maxino,
    .data_start  = v7_data_start,
    .data_end    = v7_data_end,
    .inode_state = v6_inode_state,
    .walk_free   = v7_walk_free,
    .makefree    = v6_makefree,
    .is_clean    = v7_is_clean,
    .statfs      = v7fs_statfs_op,
    .max_file    = v6_max_file_op,
};

/* ---- allocator vtable: the free-list cache (V6/V7/BSD211) ---------------- */

const alloc_ops_t freelist_alloc_ops = {
    .balloc = (int  (*)(void *, uint32_t *))v7fs_balloc,
    .bfree  = (void (*)(void *, uint32_t))v7fs_bfree,
    .ialloc = (int  (*)(void *, uint32_t *))v7fs_ialloc,
    .ifree  = (void (*)(void *, uint32_t))v7fs_ifree,
    .sync   = (int  (*)(void *))v7fs_sync,
};
