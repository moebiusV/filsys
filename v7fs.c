/* filsys 1.4.0 - 2026-09-06 - Copyright (C) 2026 David Walther */
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

    uint8_t sb[V7_MAXBSIZE];
    if (v7fs_read_block(fs, V7_SUPERB, sb)) {
        close(fs->fd);
        return -EIO;
    }
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
    /* s_tfree/s_tinode carry the true free-space totals (v7fs_makefree writes
     * them); the 50/100-entry caches are only the in-core spill.  Read them so
     * statfs can report real free space rather than the cache depth.  They sit
     * immediately after s_time, whose offset already accounts for the layout. */
    if (!fs->pack4 || fs->magic) {
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
        fs->isize < 2 ||
        fs->base + (uint64_t)fs->fsize * fs->bsize > (uint64_t)st.st_size ||
        fs->fsize <= fs->isize) {
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

int v7fs_read_block(filsys_edition_t *fs, uint32_t bno, uint8_t *buf) {
    ssize_t n = pread(fs->fd, buf, fs->bsize, (off_t)bno * fs->bsize + (off_t)fs->base);
    if (n != fs->bsize)
        return -EIO;
    return 0;
}

int v7fs_write_block(filsys_edition_t *fs, uint32_t bno, const uint8_t *buf) {
    if (fs->readonly)
        return -EROFS;
    ssize_t n = pwrite(fs->fd, buf, fs->bsize, (off_t)bno * fs->bsize + (off_t)fs->base);
    if (n != fs->bsize)
        return -EIO;
    return 0;
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
    return v7fs_write_block(fs, V7_SUPERB, sb);
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
    uint32_t bno = v7_itod(fs, ino);
    uint32_t off = v7_itoo(fs, ino);
    if (bno >= fs->isize)
        return -EINVAL;
    uint8_t raw[V7_MAXBSIZE];
    if (v7fs_read_block(fs, bno, raw))
        return -EIO;
    uint8_t *d = raw + off * fs->inode_size;
    bo_put16le(d + 0, ip->mode);
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
    if (blk < fs->isize || blk >= fs->fsize)
        return -EIO;   /* badblock: refuse garbage */

    if (fs->fl.nfree == 0) {
        /* Just popped the bottom of the stack: it is a free-list block. */
        uint8_t buf[V7_MAXBSIZE];
        if (v7fs_read_block(fs, blk, buf))
            return -EIO;
        fs->fl.nfree = bo_get16le(buf + 0);
        for (int i = 0; i < fs->nicfree; i++)
            fs->fl.free[i] = fs->bo->get32(buf + fb_free_off(fs->pack4) + 4 * i);
    }
    /* Zero the freshly-allocated block: V7's alloc() clrbuf()s it, and without
     * this the previous file's data leaks into a new file. */
    uint8_t z[V7_MAXBSIZE];
    memset(z, 0, fs->bsize);
    if (v7fs_write_block(fs, blk, z))
        return -EIO;
    if (fs->fl.tfree) fs->fl.tfree--;
    *bno = blk;
    return 0;
}

void v7fs_bfree(filsys_edition_t *fs, uint32_t bno) {
    if (bno < fs->isize || bno >= fs->fsize)
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
            fs->bo->put32(buf + fb_free_off(fs->pack4) + 4 * i, fs->fl.free[i]);
        if (v7fs_write_block(fs, bno, buf) == 0)
            fs->fl.nfree = 0;
    }
    fs->fl.free[fs->fl.nfree++] = bno;
    fs->fl.tfree++;
}

int v7fs_ialloc(filsys_edition_t *fs, uint32_t *ino) {
    uint32_t maxino = (uint32_t)(fs->isize - 2) * v7_inopb(fs);

    for (;;) {
        if (fs->fl.ninode > 0) {
            uint32_t cand = fs->fl.inode[--fs->fl.ninode];
            v7_inode_t ip;
            if (cand >= 2 && cand <= maxino &&
                v7fs_read_inode(fs, cand, &ip) == 0 && ip.mode == 0) {
                memset(&ip, 0, sizeof(ip));
                ip.ino = cand;
                v7fs_write_inode(fs, cand, &ip);
                if (fs->fl.tinode) fs->fl.tinode--;
                *ino = cand;
                return 0;
            }
            continue;   /* was already allocated; look again */
        }
        /* Refill the cache with a linear scan of the i-list. */
        fs->fl.ninode = 0;
        for (uint32_t in = 2; in <= maxino && fs->fl.ninode < V7_NICINOD; in++) {
            v7_inode_t ip;
            if (v7fs_read_inode(fs, in, &ip))
                break;
            if (ip.mode == 0)
                fs->fl.inode[fs->fl.ninode++] = (uint16_t)in;
        }
        if (fs->fl.ninode == 0)
            return -ENOSPC;
    }
}

void v7fs_ifree(filsys_edition_t *fs, uint32_t ino) {
    if (fs->fl.ninode >= V7_NICINOD)
        return;   /* kernel discards beyond the cache */
    fs->fl.inode[fs->fl.ninode++] = (uint16_t)ino;
    fs->fl.tinode++;
}

/* ---- truncate ----------------------------------------------------------- */

static void tloop(filsys_edition_t *fs, uint32_t blk, int level) {
    uint8_t buf[V7_MAXBSIZE];
    if (v7fs_read_block(fs, blk, buf))
        return;
    for (int i = v7_nindir(fs) - 1; i >= 0; i--) {
        uint32_t nb = fs->bo->get32(buf + 4 * i);
        if (nb == 0)
            continue;
        if (level > 0)
            tloop(fs, nb, level - 1);
        else
            v7fs_bfree(fs, nb);
    }
    v7fs_bfree(fs, blk);
}

int v7fs_itrunc(filsys_edition_t *fs, v7_inode_t *ip) {
    int t = ip->mode & fs->ifmt;
    if (t != fs->ifreg && t != fs->ifdir && !(fs->iflnk && t == fs->iflnk))
        return 0;
    for (int i = fs->niaddr - 1; i >= 0; i--) {
        uint32_t bn = ip->addr[i];
        if (bn == 0)
            continue;
        ip->addr[i] = 0;
        int level = i - fs->ndaddr;   /* 0=single, 1=double, 2=triple; <0=direct */
        if (level >= 0)
            tloop(fs, bn, level);
        else
            v7fs_bfree(fs, bn);
    }
    ip->size = 0;
    return 0;
}

/* Free the leaf blocks at indices [skip, ...) of the subtree rooted at `blk`
 * (which has `level` levels of indirection below it).  When skip == 0 the whole
 * subtree and `blk` itself are freed. */
static void tloop_from(filsys_edition_t *fs, uint32_t blk, int level, uint32_t skip) {
    uint8_t buf[V7_MAXBSIZE];
    if (v7fs_read_block(fs, blk, buf))
        return;
    uint32_t sub = 1;
    for (int l = 0; l < level; l++) sub *= v7_nindir(fs);   /* leaves per entry */
    uint32_t se = skip / sub;                            /* whole entries to skip */
    uint32_t sp = skip % sub;                            /* partial skip within entry se */
    for (int i = v7_nindir(fs) - 1; i >= 0; i--) {
        uint32_t nb = fs->bo->get32(buf + 4 * i);
        if (nb == 0)
            continue;
        if ((uint32_t)i < se)
            continue;                                    /* whole entry kept */
        if ((uint32_t)i == se && sp > 0) {
            tloop_from(fs, nb, level - 1, sp);           /* partial: entry kept */
        } else {
            if (level == 0) v7fs_bfree(fs, nb);
            else tloop(fs, nb, level - 1);
            fs->bo->put32(buf + 4 * i, 0);            /* drop the freed entry */
        }
    }
    /* skip > 0 by construction: itrunc_from calls tloop_from only for a partial
     * (non-zero) count, and recursion fires only when sp > 0.  The whole-block
     * case is handled by tloop() at the call sites. */
    v7fs_write_block(fs, blk, buf);                      /* persist dropped entries */
}

/* Free every block from logical block `first_blk` onwards, leaving the first
 * `first_blk` blocks in place.  `first_blk == 0` is equivalent to itrunc. */
int v7fs_itrunc_from(filsys_edition_t *fs, v7_inode_t *ip, uint32_t first_blk) {
    uint32_t rem = first_blk;

    /* Direct blocks [0, ndaddr) */
    if (rem < (uint32_t)fs->ndaddr) {
        for (int i = fs->ndaddr - 1; i >= (int)rem; i--) {
            if (ip->addr[i]) { v7fs_bfree(fs, ip->addr[i]); ip->addr[i] = 0; }
        }
        rem = 0;
    } else {
        rem -= fs->ndaddr;
    }

    /* Single indirect [ndaddr, ndaddr + nindir) */
    if (rem < v7_nindir(fs)) {
        if (ip->addr[fs->ndaddr]) {
            if (rem == 0) { tloop(fs, ip->addr[fs->ndaddr], 0); ip->addr[fs->ndaddr] = 0; }
            else tloop_from(fs, ip->addr[fs->ndaddr], 0, rem);
        }
        rem = 0;
    } else {
        rem -= v7_nindir(fs);
    }

    /* Double indirect [ndaddr + nindir, ... + nindir^2) */
    if (rem < (uint32_t)v7_nindir(fs) * v7_nindir(fs)) {
        if (ip->addr[fs->ndaddr + 1]) {
            if (rem == 0) { tloop(fs, ip->addr[fs->ndaddr + 1], 1); ip->addr[fs->ndaddr + 1] = 0; }
            else tloop_from(fs, ip->addr[fs->ndaddr + 1], 1, rem);
        }
        rem = 0;
    } else {
        rem -= (uint32_t)v7_nindir(fs) * v7_nindir(fs);
    }

    /* Triple indirect [ndaddr + nindir + nindir^2, ...) */
    if (ip->addr[fs->ndaddr + 2]) {
        if (rem == 0) { tloop(fs, ip->addr[fs->ndaddr + 2], 2); ip->addr[fs->ndaddr + 2] = 0; }
        else tloop_from(fs, ip->addr[fs->ndaddr + 2], 2, rem);
    }
    return 0;
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
        ip->size = (uint32_t)(off + size);
    fs->ops->write_inode(fs, ip->ino, ip);
    return (ssize_t)done;
}

/* ---- directories -------------------------------------------------------- */

int v7fs_dir_read(filsys_edition_t *fs, v7_inode_t *ip, v7_dirent_t **ents, size_t *count) {
    if ((ip->mode & fs->ifmt) != fs->ifdir)
        return -ENOTDIR;
    /* A directory's data cannot exceed the filesystem's data area; reject a
     * corrupt size before the malloc below, else a bogus di_size (up to 4 GiB)
     * turns into a multi-gigabyte allocation. */
    if (ip->size > (uint64_t)(fs->fsize - fs->isize) * fs->bsize)
        return -EFBIG;
    size_t cap = ip->size / 16 + 1;
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
    for (size_t off = 0; off + 16 <= (size_t)n; off += 16) {
        uint16_t ino = bo_get16le(buf + off);
        if (ino == 0)
            continue;
        out[cnt].ino = ino;
        memcpy(out[cnt].name, buf + off + 2, V7_DIRSIZ);
        out[cnt].name[V7_DIRSIZ] = 0;
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
    if (namelen == 0 || namelen > V7_DIRSIZ)
        return -ENAMETOOLONG;
    if (strchr(name, '/'))
        return -EINVAL;

    size_t newsize = ip->size + 16;
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
    for (size_t off = 0; off + 16 <= (size_t)n; off += 16) {
        if (bo_get16le(buf + off) == 0) {
            slot = off;
            break;
        }
    }
    if (slot == SIZE_MAX) {
        slot = (size_t)n;
        n += 16;
    }

    bo_put16le(buf + slot, (uint16_t)ino);
    memset(buf + slot + 2, 0, V7_DIRSIZ);
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
    for (size_t off = 0; off + 16 <= (size_t)n; off += 16) {
        if (bo_get16le(buf + off) == 0)
            continue;
        char ent[V7_DIRSIZ + 1];
        memcpy(ent, buf + off + 2, V7_DIRSIZ);
        ent[V7_DIRSIZ] = 0;
        if (strcmp(ent, name) == 0) {
            bo_put16le(buf + off, 0);
            memset(buf + off + 2, 0, V7_DIRSIZ);
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

        if ((dip.mode & fs->ifmt) != fs->ifdir)
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

/* ---- integrity check ---------------------------------------------------- */

/* Mark one data block as accounted-for.  Returns 0 on a first claim, 1 for an
 * out-of-range (bad) block, 2 for a duplicate (already claimed).  Inode 1 is
 * the bad-block inode (V7_BADFIN): its addresses record bad blocks, so a
 * reference into the i-list region [2, isize) is a record, not an error. */
static int v7_mark_block(filsys_edition_t *fs, filsys_chkctx_t *cx, uint32_t bno)
{
    if (bno == 0)
        return 0;
    if (bno < fs->isize || bno >= fs->fsize) {
        if (cx->ino == V7_BADFIN && bno >= 2 && bno < fs->isize)
            return 0;   /* bad-block record: a gone-bad i-list block */
        printf("block %u bad; inode=%u\n", bno, cx->ino);
        cx->bad_blocks++;
        cx->errors++;
        return 1;
    }
    uint32_t d = bno - fs->isize;
    uint8_t  m = (uint8_t)(1u << (d & 7));
    if (cx->bmap[d >> 3] & m) {
        if (cx->owner)
            printf("block %u dup; inode=%u (owner %u)\n",
                   bno, cx->ino, cx->owner[d]);
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

/* Mark an indirect block and everything beneath it.  level 0 = single
 * indirect, 1 = double, 2 = triple.  A duplicate indirect block is not chased:
 * its children were already claimed by the first reference, so re-walking them
 * would only cascade spurious "dup" reports (BSD fsck's phase 1). */
static void v7_mark_tree(filsys_edition_t *fs, filsys_chkctx_t *cx, uint32_t blk, int level)
{
    if (blk == 0)
        return;
    if (v7_mark_block(fs, cx, blk) != 0)
        return;                       /* bad or duplicate: don't chase it */
    uint8_t buf[V7_MAXBSIZE];
    if (v7fs_read_block(fs, blk, buf)) {
        printf("cannot read indirect block %u\n", blk);
        cx->errors++;
        return;
    }
    for (int i = 0; i < (int)v7_nindir(fs); i++) {
        uint32_t nb = fs->bo->get32(buf + 4 * i);
        if (nb == 0)
            continue;
        if (level > 0)
            v7_mark_tree(fs, cx, nb, level - 1);
        else
            v7_mark_block(fs, cx, nb);
    }
}

/* The vtable mark_blocks seam: mark every block referenced by an inode. */
static void v7_mark_blocks(void *fs, const filsys_inode_t *ip, uint32_t ino,
                           filsys_chkctx_t *cx)
{
    filsys_edition_t *f = fs;
    (void)ino;   /* the driver set cx->ino */
    for (int i = 0; i < f->ndaddr; i++) {
        uint32_t a = ip->addr[i];
        if (a)
            v7_mark_block(f, cx, a);
    }
    for (int i = f->ndaddr; i < f->niaddr; i++) {
        uint32_t a = ip->addr[i];
        if (a)
            v7_mark_tree(f, cx, a, i - f->ndaddr);
    }
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
static int v7fs_makefree(void *fs, filsys_chkctx_t *cx)
{
    filsys_edition_t *f = fs;
    int m, n;
    if (f->interleave) {
        m = f->m;
        n = f->n;
        if (n < 1 || n > V7_COH_MAXINTN || m < 1 || m > n || n % m != 0) {
            printf("invalid interleave factors in superblock (m=%d n=%d); defaulting\n", m, n);
            m = 1;
            n = 1;
        }
    } else {
        m = 1;
        n = 1;   /* V7/32V: no interleave */
    }

    int maptab[V7_COH_MAXINTN];
    int ratio = n / m;
    for (int i = 0; i < n; i++)
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
    int nfree = 0;
    for (uint32_t bn = f->isize; bn < f->fsize; bn++) {
        uint32_t blk = bn;
        if (bn >= mapbot && bn < maptop)
            blk = (bn / n) * n + (uint32_t)maptab[bn % n];
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
static uint8_t v7_inode_state(void *fs, uint32_t ino, uint32_t mode) {
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

static uint32_t v7_maxino(void *fs) {
    filsys_edition_t *f = fs;
    return (uint32_t)(f->isize - 2) * v7_inopb(f);
}
static uint32_t v7_data_start(void *fs) { return ((filsys_edition_t *)fs)->isize; }
static uint32_t v7_data_end(void *fs)   { return ((filsys_edition_t *)fs)->fsize; }
static int v7_is_clean(void *fs)        { return ((filsys_edition_t *)fs)->fmod == 0; }

/* Walk the free list exactly as alloc() would, marking free blocks into cx->bmap
 * (a free block already used is a duplicate) and counting free_blocks. */
static void v7_walk_free(void *fs, filsys_chkctx_t *cx, filsys_check_t *rep)
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
        if (bno < f->isize || bno >= f->fsize) {
            printf("free block %u out of range [%u,%u)\n", bno, f->isize, f->fsize);
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
        uint32_t off = bno - f->isize;
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
                cur[i] = f->bo->get32(blk + fb_free_off(f->pack4) + 4 * i);
        }
    }
    free(seen);
}

static void v7fs_preen(void *fs, const uint8_t *ecount, const uint8_t *state,
                       uint32_t maxino, int mode)
{
    v7_inode_t root;
    uint32_t lf_ino = 0;
    if (v7fs_read_inode(fs, V7_ROOTINO, &root) == 0 &&
        v7fs_dir_lookup(fs, &root, "lost+found", &lf_ino) != 0) {
        v7_inode_t lf;
        if (v7fs_ialloc(fs, &lf_ino) == 0) {
            memset(&lf, 0, sizeof(lf));
            lf.ino = lf_ino;
            lf.mode = 0777 | V7_IFDIR;
            lf.nlink = 2;
            v7fs_write_inode(fs, lf_ino, &lf);
            v7fs_dir_add(fs, &lf, lf_ino, ".");
            v7fs_dir_add(fs, &lf, V7_ROOTINO, "..");
            v7fs_dir_add(fs, &root, lf_ino, "lost+found");
            root.nlink++;
            v7fs_write_inode(fs, V7_ROOTINO, &root);
            printf("created lost+found (inode %u)\n", lf_ino);
        }
    }

    for (uint32_t ino = 1; ino <= maxino; ino++) {
        int cnt = ecount[ino] & 0377;
        v7_inode_t ip;
        if (v7fs_read_inode(fs, ino, &ip) != 0)
            continue;
        int allocd = filsys_in_allocated(state[ino]);
        if (!allocd) {
            if (cnt != 0 && filsys_query(mode, "clear free-but-referenced inode %u", ino)) {
                ip.mode = 0;
                ip.nlink = 0;
                v7fs_write_inode(fs, ino, &ip);
                printf("cleared free-but-referenced inode %u\n", ino);
            }
            continue;
        }
        if (cnt == ip.nlink)
            continue;
        if (ino == V7_ROOTINO || ino == lf_ino)
            continue;   /* nlink just set by lost+found creation; ecount is stale */
        if (cnt == 0) {
            if (lf_ino == 0)
                continue;
            if ((ip.mode & V7_IFMT) != V7_IFREG)   /* dir or device: leave it */
                continue;
            v7_inode_t lf;
            char name[16];
            snprintf(name, sizeof(name), "%u", ino);
            if (filsys_query(mode, "reconnect inode %u to lost+found", ino) &&
                v7fs_read_inode(fs, lf_ino, &lf) == 0 &&
                v7fs_dir_add(fs, &lf, ino, name) == 0) {
                ip.nlink = 1;
                v7fs_write_inode(fs, ino, &ip);
                v7fs_write_inode(fs, lf_ino, &lf);
                printf("reconnected inode %u to lost+found\n", ino);
            }
        } else {
            int old = ip.nlink;
            if (filsys_query(mode, "fix link count of inode %u from %d to %d",
                             ino, old, cnt)) {
                ip.nlink = (int16_t)(cnt & 0377);
                v7fs_write_inode(fs, ino, &ip);
                printf("inode %u link count %d -> %d\n", ino, old, ip.nlink);
            }
        }
    }
}

int v7fs_check(filsys_edition_t *fs, v7_check_t *rep, int mode) {
    return filsys_check_common(fs, fs, rep, mode);
}

/* ---- maintenance: ncheck / clri / salv -a ------------------------------ */

/* Recursively walk the directory tree from `dirino`, printing the pathname(s)
 * of `target` and descending into subdirectories. */
static void ncheck_dir(filsys_edition_t *fs, uint32_t dirino, const char *prefix,
                       uint32_t target, int *found, int depth)
{
    if (depth > 64)
        return;                       /* guard against a directory cycle */
    v7_inode_t ip;
    if (v7fs_read_inode(fs, dirino, &ip))
        return;
    if ((ip.mode & V7_IFMT) != V7_IFDIR)
        return;
    v7_dirent_t *ents = NULL;
    size_t cnt = 0;
    if (v7fs_dir_read(fs, &ip, &ents, &cnt))
        return;
    for (size_t i = 0; i < cnt; i++) {
        uint32_t eino = ents[i].ino;
        if (eino == 0)
            continue;
        if (ents[i].name[0] == '.' &&
            (ents[i].name[1] == 0 ||
             (ents[i].name[1] == '.' && ents[i].name[2] == 0)))
            continue;                 /* skip "." and ".." */
        char path[1024];
        if (prefix[1] == 0)           /* prefix is "/" */
            snprintf(path, sizeof(path), "/%s", ents[i].name);
        else
            snprintf(path, sizeof(path), "%s/%s", prefix, ents[i].name);
        if (eino == target) {
            printf("%u\t%s\n", target, path);
            *found = 1;
        }
        v7_inode_t cip;
        if (v7fs_read_inode(fs, eino, &cip) == 0 &&
            (cip.mode & V7_IFMT) == V7_IFDIR)
            ncheck_dir(fs, eino, path, target, found, depth + 1);
    }
    v7fs_dirents_free(ents);
}

int v7fs_ncheck(filsys_edition_t *fs, uint32_t ino)
{
    int found = 0;
    ncheck_dir(fs, V7_ROOTINO, "/", ino, &found, 0);
    if (!found)
        printf("%u: not found\n", ino);
    return 0;
}

int v7fs_clri(filsys_edition_t *fs, uint32_t ino)
{
    uint32_t maxino = (uint32_t)(fs->isize - 2) * v7_inopb(fs);
    if (ino == 0 || ino > maxino)
        return -EINVAL;
    v7_inode_t ip;
    memset(&ip, 0, sizeof(ip));
    ip.ino = ino;
    int rc = v7fs_write_inode(fs, ino, &ip);
    if (rc == 0)
        printf("cleared inode %u\n", ino);
    return rc;
}

int v7fs_resolve_dups(filsys_edition_t *fs)
{
    uint32_t maxino = (uint32_t)(fs->isize - 2) * v7_inopb(fs);
    uint32_t nblk = fs->fsize - fs->isize;

    filsys_chkctx_t cx;
    memset(&cx, 0, sizeof(cx));
    cx.nblk = nblk;
    cx.bmap = calloc((nblk + 7) / 8, 1);
    if (!cx.bmap)
        return -ENOMEM;

    struct dup { uint32_t blk, ino, idx; };
    struct dup *dups = NULL;
    size_t ndup = 0, cap = 0;

    for (uint32_t ino = 1; ino <= maxino; ino++) {
        v7_inode_t ip;
        if (v7fs_read_inode(fs, ino, &ip))
            continue;
        if (ip.mode == 0)
            continue;
        uint16_t fmt = ip.mode & V7_IFMT;
        if (fmt == V7_IFCHR || fmt == V7_IFBLK ||
            fmt == V7_IFMPC || fmt == V7_IFMPB)
            continue;   /* device inode: addr[0] is a device number, not a block */
        for (int i = 0; i < fs->niaddr; i++) {
            uint32_t a = ip.addr[i];
            if (a == 0)
                continue;
            if (i < fs->ndaddr) {
                if (a < fs->isize || a >= fs->fsize) {
                    printf("block %u bad; inode=%u\n", a, ino);
                    continue;
                }
                uint32_t off = a - fs->isize;
                uint8_t m = (uint8_t)(1u << (off & 7));
                if (cx.bmap[off >> 3] & m) {
                    if (ndup == cap) {
                        cap = cap ? cap * 2 : 16;
                        dups = realloc(dups, cap * sizeof(*dups));
                    }
                    dups[ndup].blk = a;
                    dups[ndup].ino = ino;
                    dups[ndup].idx = i;
                    ndup++;
                } else {
                    cx.bmap[off >> 3] |= m;
                    cx.used_blocks++;
                }
            } else {
                cx.ino = ino;
                v7_mark_tree(fs, &cx, a, i - fs->ndaddr);
            }
        }
    }

    if (ndup == 0) {
        printf("no duplicate blocks\n");
        free(cx.bmap);
        return 0;
    }

    printf("%zu duplicate block(s); rebuilding free list\n", ndup);
    v7fs_makefree(fs, &cx);

    int resolved = 0;
    for (size_t k = 0; k < ndup; k++) {
        uint32_t blk = dups[k].blk, ino = dups[k].ino, idx = dups[k].idx;
        v7_inode_t ip;
        if (v7fs_read_inode(fs, ino, &ip))
            continue;
        if (ip.addr[idx] != blk)
            continue;
        uint32_t nb;
        if (v7fs_balloc(fs, &nb)) {
            printf("block %u dup; inode=%u: out of space\n", blk, ino);
            continue;
        }
        uint8_t buf[V7_MAXBSIZE];
        if (v7fs_read_block(fs, blk, buf) || v7fs_write_block(fs, nb, buf)) {
            printf("block %u dup; inode=%u: copy failed\n", blk, ino);
            continue;
        }
        ip.addr[idx] = nb;
        v7fs_write_inode(fs, ino, &ip);
        uint32_t off = nb - fs->isize;
        cx.bmap[off >> 3] |= (uint8_t)(1u << (off & 7));
        printf("block %u dup; inode=%u: copied to %u\n", blk, ino, nb);
        resolved++;
    }
    free(dups);

    printf("resolved %d/%zu duplicates; finalizing free list\n", resolved, ndup);
    v7fs_makefree(fs, &cx);
    free(cx.bmap);
    return resolved == (int)ndup ? 0 : -1;
}

/* ---- ops table ----------------------------------------------------------
 * Each op takes `void *` (the backend state).  The adapters forward to the
 * typed backend function; the `void *` argument converts implicitly to
 * filsys_edition_t*, so there is no cast anywhere. */

static int v7fs_open_op(void *fs, const char *path, int readonly,
                        const filsys_edition_t *proto, uint64_t offset) {
    return v7fs_open(fs, path, readonly, proto, offset);
}
static uint32_t v7fs_blocksize_op(const void *fs) {
    return ((const filsys_edition_t *)fs)->bsize;
}
static void v7fs_close_op(void *fs) { v7fs_close(fs); }
static int v7fs_sync_op(void *fs) { return v7fs_sync(fs); }
static int v7fs_mark_dirty_op(void *fs) { return v7fs_mark_dirty(fs); }
static int v7fs_read_block_op(void *fs, uint32_t bno, uint8_t *buf)
{ return v7fs_read_block(fs, bno, buf); }
static int v7fs_write_block_op(void *fs, uint32_t bno, const uint8_t *buf)
{ return v7fs_write_block(fs, bno, buf); }
static int v7fs_read_inode_op(void *fs, uint32_t ino, filsys_inode_t *ip)
{ return v7fs_read_inode(fs, ino, ip); }
static int v7fs_write_inode_op(void *fs, uint32_t ino, const filsys_inode_t *ip)
{ return v7fs_write_inode(fs, ino, ip); }
static int v7fs_ialloc_op(void *fs, uint32_t *ino)
{ return v7fs_ialloc(fs, ino); }
static void v7fs_ifree_op(void *fs, uint32_t ino) { v7fs_ifree(fs, ino); }
static int v7fs_bmap_op(void *fs, filsys_inode_t *ip, uint32_t lbn, int create, uint32_t *bno)
{ return v7fs_bmap(fs, ip, lbn, create, bno); }
static int v7fs_itrunc_op(void *fs, filsys_inode_t *ip)
{ return v7fs_itrunc(fs, ip); }
static int v7fs_itrunc_from_op(void *fs, filsys_inode_t *ip, uint32_t first_blk)
{ return v7fs_itrunc_from(fs, ip, first_blk); }
static ssize_t v7fs_file_read_op(void *fs, filsys_inode_t *ip, uint8_t *buf, size_t size, off_t off)
{ return v7fs_file_read(fs, ip, buf, size, off); }
static ssize_t v7fs_file_write_op(void *fs, filsys_inode_t *ip, const uint8_t *buf, size_t size, off_t off)
{ return v7fs_file_write(fs, ip, buf, size, off); }
static int v7fs_dir_read_op(void *fs, filsys_inode_t *ip, filsys_dirent_t **ents, size_t *count)
{ return v7fs_dir_read(fs, ip, ents, count); }
static int v7fs_dir_lookup_op(void *fs, filsys_inode_t *ip, const char *name, uint32_t *ino)
{ return v7fs_dir_lookup(fs, ip, name, ino); }
static int v7fs_dir_add_op(void *fs, filsys_inode_t *ip, uint32_t ino, const char *name)
{ return v7fs_dir_add(fs, ip, ino, name); }
static int v7fs_dir_remove_op(void *fs, filsys_inode_t *ip, const char *name)
{ return v7fs_dir_remove(fs, ip, name); }
static int v7fs_lookup_op(void *fs, const char *path, uint32_t *ino, filsys_inode_t *ip)
{ return v7fs_lookup(fs, path, ino, ip); }
static int v7fs_check_op(void *fs) { v7_check_t rep; return v7fs_check(fs, &rep, 0); }
static uint64_t v7fs_max_file_op(void *fs) {
    const filsys_edition_t *v7 = fs;
    uint64_t n = v7_nindir(v7);
    return ((uint64_t)v7->ndaddr + n + n * n + n * n * n) * v7->bsize;
}

static void v7fs_statfs_op(void *fs, struct statvfs *st) {
    filsys_edition_t *v7 = fs;
    st->f_blocks = v7->fsize;
    st->f_bfree = st->f_bavail = v7->fl.tfree;
    st->f_files = (v7->isize - 2) * v7_inopb(v7);
    st->f_ffree = v7->fl.tinode;
}
const struct filsys_ops v7fs_ops = {
    .name        = "v7",
    .blocksize   = v7fs_blocksize_op,
    .open        = v7fs_open_op,
    .close       = v7fs_close_op,
    .sync        = v7fs_sync_op,
    .mark_dirty  = v7fs_mark_dirty_op,
    .read_block  = v7fs_read_block_op,
    .write_block = v7fs_write_block_op,
    .blk_get     = v7fs_read_block_op,
    .blk_put     = v7fs_write_block_op,
    .read_inode  = v7fs_read_inode_op,
    .write_inode = v7fs_write_inode_op,
    .ialloc      = v7fs_ialloc_op,
    .ifree       = v7fs_ifree_op,
    .bmap        = v7fs_bmap_op,
    .itrunc      = v7fs_itrunc_op,
    .itrunc_from = v7fs_itrunc_from_op,
    .file_read   = v7fs_file_read_op,
    .file_write  = v7fs_file_write_op,
    .dir_read    = v7fs_dir_read_op,
    .dir_lookup  = v7fs_dir_lookup_op,
    .dir_add     = v7fs_dir_add_op,
    .dir_remove  = v7fs_dir_remove_op,
    .lookup      = v7fs_lookup_op,
    .check       = v7fs_check_op,
    .maxino      = v7_maxino,
    .data_start  = v7_data_start,
    .data_end    = v7_data_end,
    .inode_state = v7_inode_state,
    .mark_blocks = v7_mark_blocks,
    .walk_free   = v7_walk_free,
    .makefree    = v7fs_makefree,
    .preen       = v7fs_preen,
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
            ssize_t w = v7fs_file_write(fs, ip, buf, n, 0);
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
            ssize_t w = v7fs_file_write(fs, ip, buf, n, 0);
            free(buf);
            return w < 0 ? (int)w : 0;
        }
        off += reclen;
    }
    free(buf);
    return -ENOENT;
}

/* ---- 2.11BSD integrity check ---------------------------------------------- */

static int bsd211_mark_block(filsys_edition_t *fs, uint8_t *bmap,
                             uint32_t bno, uint32_t *used, uint32_t *dups, uint32_t *errors)
{
    if (bno == 0)
        return 0;
    if (bno < fs->isize || bno >= fs->fsize) {
        printf("block %u bad\n", bno);
        (*errors)++;
        return 1;
    }
    uint32_t d = bno - fs->isize;
    uint8_t  m = (uint8_t)(1u << (d & 7));
    if (bmap[d >> 3] & m) {
        (*dups)++;
        (*errors)++;
        return 2;
    }
    bmap[d >> 3] |= m;
    (*used)++;
    return 0;
}

static void bsd211_mark_tree(filsys_edition_t *fs, uint8_t *bmap,
                             uint32_t blk, int level,
                             uint32_t *used, uint32_t *dups, uint32_t *errors)
{
    if (blk == 0)
        return;
    if (bsd211_mark_block(fs, bmap, blk, used, dups, errors) != 0)
        return;
    uint8_t buf[V7_MAXBSIZE];
    if (v7fs_read_block(fs, blk, buf)) {
        printf("cannot read indirect block %u\n", blk);
        (*errors)++;
        return;
    }
    for (int i = 0; i < (int)v7_nindir(fs); i++) {
        uint32_t nb = fs->bo->get32(buf + 4 * i);
        if (nb == 0)
            continue;
        if (level > 0)
            bsd211_mark_tree(fs, bmap, nb, level - 1, used, dups, errors);
        else
            bsd211_mark_block(fs, bmap, nb, used, dups, errors);
    }
}

static uint8_t bsd211_inode_state(void *fs, uint32_t ino, uint32_t mode) {
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

static void bsd211_mark_blocks(void *fs, const filsys_inode_t *ip, uint32_t ino,
                               filsys_chkctx_t *cx)
{
    filsys_edition_t *f = fs;
    (void)ino;
    for (int i = 0; i < f->ndaddr; i++) {
        uint32_t a = ip->addr[i];
        if (a)
            bsd211_mark_block(f, cx->bmap, a, &cx->used_blocks, &cx->dup_blocks, &cx->errors);
    }
    for (int i = f->ndaddr; i < f->niaddr; i++) {
        uint32_t a = ip->addr[i];
        if (a)
            bsd211_mark_tree(f, cx->bmap, a, i - f->ndaddr, &cx->used_blocks, &cx->dup_blocks, &cx->errors);
    }
}

static int bsd211_is_clean(void *fs) { (void)fs; return 0; }

int bsd211_check(filsys_edition_t *fs, v7_check_t *rep, int mode) {
    (void)mode;   /* 2.11BSD: check only, no salvage/preen */
    return filsys_check_common(fs, fs, rep, 0);
}

/* ---- 2.11BSD ops table: the shared V7 engine + variable-length dirents ---- */

static int bsd211_dir_read_op(void *fs, filsys_inode_t *ip, filsys_dirent_t **ents, size_t *count)
{ return bsd211_dir_read(fs, ip, ents, count); }
static int bsd211_dir_add_op(void *fs, filsys_inode_t *ip, uint32_t ino, const char *name)
{ return bsd211_dir_add(fs, ip, ino, name); }
static int bsd211_dir_remove_op(void *fs, filsys_inode_t *ip, const char *name)
{ return bsd211_dir_remove(fs, ip, name); }
static int bsd211_check_op(void *fs) { v7_check_t rep; return bsd211_check(fs, &rep, 0); }

const struct filsys_ops bsd211fs_ops = {
    .name        = "bsd211",
    .blocksize   = v7fs_blocksize_op,
    .open        = v7fs_open_op,
    .close       = v7fs_close_op,
    .sync        = v7fs_sync_op,
    .mark_dirty  = v7fs_mark_dirty_op,
    .read_block  = v7fs_read_block_op,
    .write_block = v7fs_write_block_op,
    .blk_get     = v7fs_read_block_op,
    .blk_put     = v7fs_write_block_op,
    .read_inode  = v7fs_read_inode_op,
    .write_inode = v7fs_write_inode_op,
    .ialloc      = v7fs_ialloc_op,
    .ifree       = v7fs_ifree_op,
    .bmap        = v7fs_bmap_op,
    .itrunc      = v7fs_itrunc_op,
    .itrunc_from = v7fs_itrunc_from_op,
    .file_read   = v7fs_file_read_op,
    .file_write  = v7fs_file_write_op,
    .dir_read    = bsd211_dir_read_op,
    .dir_lookup  = v7fs_dir_lookup_op,
    .dir_add     = bsd211_dir_add_op,
    .dir_remove  = bsd211_dir_remove_op,
    .lookup      = v7fs_lookup_op,
    .check       = bsd211_check_op,
    .maxino      = v7_maxino,
    .data_start  = v7_data_start,
    .data_end    = v7_data_end,
    .inode_state = bsd211_inode_state,
    .mark_blocks = bsd211_mark_blocks,
    .walk_free   = v7_walk_free,
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

static void v6_count_free(filsys_edition_t *fs, uint32_t *nblk, uint32_t *nino);
static int v6_read_inode(filsys_edition_t *fs, uint32_t ino, v7_inode_t *ip);
static int v6_write_inode(filsys_edition_t *fs, uint32_t ino, const v7_inode_t *ip);

static int v6_open(filsys_edition_t *fs, const char *path, int readonly,
                   const filsys_edition_t *proto, uint64_t offset) {
    if (fs != proto)
        memcpy(fs, proto, sizeof *fs);
    fs->readonly = readonly;
    fs->base = offset;
    fs->fd = open(path, readonly ? O_RDONLY : O_RDWR);
    if (fs->fd < 0)
        return -errno;

    uint8_t sb[V6_BSIZE];
    if (v7fs_read_block(fs, V6_SUPERB, sb)) {
        close(fs->fd);
        fs->fd = -1;
        return -EIO;
    }
    /* V6 superblock: 16-bit fields, s_free[100]/s_inode[100], s_time[2]. */
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

    struct stat st;
    if (fstat(fs->fd, &st) != 0 ||
        fs->isize < 1 ||
        fs->base + (uint64_t)fs->fsize * fs->bsize > (uint64_t)st.st_size ||
        fs->fsize <= v6_data_start(fs->isize)) {
        close(fs->fd);
        fs->fd = -1;
        return -EINVAL;
    }
    /* V6's superblock has no s_tfree/s_tinode; compute the totals here. */
    v6_count_free(fs, &fs->fl.tfree, &fs->fl.tinode);
    return 0;
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

static int v6_super_write(filsys_edition_t *fs) {
    if (fs->readonly)
        return 0;
    uint8_t sb[V6_BSIZE];
    if (v7fs_read_block(fs, V6_SUPERB, sb))
        return -EIO;
    bo_put16le(sb + 0, fs->isize);
    bo_put16le(sb + 2, fs->fsize);
    bo_put16le(sb + 4, fs->fl.nfree);
    for (int i = 0; i < fs->nicfree; i++)
        bo_put16le(sb + 6 + 2 * i, (uint16_t)fs->fl.free[i]);
    bo_put16le(sb + 206, fs->fl.ninode);
    for (int i = 0; i < fs->nicinod; i++)
        bo_put16le(sb + 208 + 2 * i, fs->fl.inode[i]);
    fs->bo->put32(sb + 412, (uint32_t)time(NULL));   /* s_time[2] */
    sb[410] = (uint8_t)(fs->fmod != 0);              /* s_fmod */
    return v7fs_write_block(fs, V6_SUPERB, sb);
}

static void v6_close(filsys_edition_t *fs) {
    if (fs->fd >= 0) {
        if (!fs->readonly) {
            fs->fmod = 0;
            v6_super_write(fs);
        }
        close(fs->fd);
        fs->fd = -1;
    }
}

static int v6_sync(filsys_edition_t *fs) {
    if (fs->readonly)
        return 0;
    return v6_super_write(fs);
}

static int v6_mark_dirty(filsys_edition_t *fs) {
    if (fs->readonly)
        return 0;
    fs->fmod = 1;
    return v6_super_write(fs);
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
    uint32_t bno = v7_itod(fs, ino);
    uint32_t off = v7_itoo(fs, ino);
    if (bno >= v6_data_start(fs->isize))
        return -EINVAL;
    uint8_t raw[V6_BSIZE];
    if (v7fs_read_block(fs, bno, raw))
        return -EIO;
    uint8_t *d = raw + off * fs->inode_size;
    bo_put16le(d + 0, ip->mode);
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

static int v6_balloc(filsys_edition_t *fs, uint32_t *bno) {
    if (fs->fl.nfree == 0)
        return -ENOSPC;

    uint32_t blk = fs->fl.free[--fs->fl.nfree];
    if (blk == 0)
        return -ENOSPC;
    if (blk < v6_data_start(fs->isize) || blk >= fs->fsize)
        return -EIO;

    if (fs->fl.nfree == 0) {
        uint8_t buf[V6_BSIZE];
        if (v7fs_read_block(fs, blk, buf))
            return -EIO;
        fs->fl.nfree = bo_get16le(buf + 0);
        for (int i = 0; i < fs->nicfree; i++)
            fs->fl.free[i] = bo_get16le(buf + 2 + 2 * i);
    }
    uint8_t z[V6_BSIZE];
    memset(z, 0, V6_BSIZE);
    if (v7fs_write_block(fs, blk, z))
        return -EIO;
    if (fs->fl.tfree) fs->fl.tfree--;
    *bno = blk;
    return 0;
}

static void v6_bfree(filsys_edition_t *fs, uint32_t bno) {
    if (bno < v6_data_start(fs->isize) || bno >= fs->fsize)
        return;
    if (fs->fl.nfree == 0) {
        fs->fl.nfree = 1;
        fs->fl.free[0] = 0;
    }
    if (fs->fl.nfree >= fs->nicfree) {
        uint8_t buf[V6_BSIZE];
        memset(buf, 0, V6_BSIZE);
        bo_put16le(buf + 0, fs->fl.nfree);
        for (int i = 0; i < fs->nicfree; i++)
            bo_put16le(buf + 2 + 2 * i, (uint16_t)fs->fl.free[i]);
        if (v7fs_write_block(fs, bno, buf) == 0)
            fs->fl.nfree = 0;
    }
    fs->fl.free[fs->fl.nfree++] = bno;
    fs->fl.tfree++;
}

static int v6_ialloc(filsys_edition_t *fs, uint32_t *ino) {
    uint32_t maxino = v6_maxino(fs->isize);

    for (;;) {
        if (fs->fl.ninode > 0) {
            uint32_t cand = fs->fl.inode[--fs->fl.ninode];
            v7_inode_t ip;
            if (cand >= 2 && cand <= maxino &&
                v6_read_inode(fs, cand, &ip) == 0 && ip.mode == 0) {
                memset(&ip, 0, sizeof(ip));
                ip.ino = cand;
                v6_write_inode(fs, cand, &ip);
                if (fs->fl.tinode) fs->fl.tinode--;
                *ino = cand;
                return 0;
            }
            continue;
        }
        fs->fl.ninode = 0;
        for (uint32_t in = 2; in <= maxino && fs->fl.ninode < fs->nicinod; in++) {
            v7_inode_t ip;
            if (v6_read_inode(fs, in, &ip))
                break;
            if (ip.mode == 0)
                fs->fl.inode[fs->fl.ninode++] = (uint16_t)in;
        }
        if (fs->fl.ninode == 0)
            return -ENOSPC;
    }
}

static void v6_ifree(filsys_edition_t *fs, uint32_t ino) {
    if (fs->fl.ninode >= fs->nicinod)
        return;
    fs->fl.inode[fs->fl.ninode++] = (uint16_t)ino;
    fs->fl.tinode++;
}

static void v6_tloop(filsys_edition_t *fs, uint32_t blk, int level) {
    uint8_t buf[V6_BSIZE];
    if (v7fs_read_block(fs, blk, buf))
        return;
    for (int i = V6_NINDIR - 1; i >= 0; i--) {
        uint32_t nb = bo_get16le(buf + 2 * i);
        if (nb == 0)
            continue;
        if (level > 0)
            v6_tloop(fs, nb, level - 1);
        else
            v6_bfree(fs, nb);
    }
    v6_bfree(fs, blk);
}

static int v6_itrunc(filsys_edition_t *fs, v7_inode_t *ip) {
    int t = ip->mode & V6_IFMT;
    if (t != 0 && t != V6_IFDIR)   /* regular (type 0) and directory only */
        return 0;
    if (ip->mode & V6_ILARG) {
        for (int i = 0; i < 7; i++) {          /* 7 single-indirect slots */
            uint32_t bn = ip->addr[i];
            if (bn) { ip->addr[i] = 0; v6_tloop(fs, bn, 0); }
        }
        uint32_t bn = ip->addr[7];             /* 1 double-indirect slot */
        if (bn) { ip->addr[7] = 0; v6_tloop(fs, bn, 1); }
    } else {
        for (int i = 0; i < fs->niaddr; i++) { /* 8 direct blocks */
            uint32_t bn = ip->addr[i];
            if (bn) { ip->addr[i] = 0; v6_bfree(fs, bn); }
        }
    }
    ip->size = 0;
    return 0;
}

static void v6_tloop_from(filsys_edition_t *fs, uint32_t blk, int level, uint32_t skip) {
    uint8_t buf[V6_BSIZE];
    if (v7fs_read_block(fs, blk, buf))
        return;
    uint32_t sub = 1;
    for (int l = 0; l < level; l++) sub *= V6_NINDIR;
    uint32_t se = skip / sub;
    uint32_t sp = skip % sub;
    for (int i = V6_NINDIR - 1; i >= 0; i--) {
        uint32_t nb = bo_get16le(buf + 2 * i);
        if (nb == 0)
            continue;
        if ((uint32_t)i < se)
            continue;
        if ((uint32_t)i == se && sp > 0) {
            v6_tloop_from(fs, nb, level - 1, sp);          /* partial: entry kept */
        } else {
            if (level == 0) v6_bfree(fs, nb);
            else v6_tloop(fs, nb, level - 1);
            bo_put16le(buf + 2 * i, 0);                    /* drop the freed entry */
        }
    }
    v7fs_write_block(fs, blk, buf);                        /* persist dropped entries */
}

static int v6_itrunc_from(filsys_edition_t *fs, v7_inode_t *ip, uint32_t first_blk) {
    if (ip->mode & V6_ILARG) {
        uint32_t rem = first_blk;
        uint32_t slot = rem / V6_NINDIR;
        uint32_t within = rem % V6_NINDIR;
        if (slot < 7) {
            for (int i = 6; i > (int)slot; i--) {
                if (ip->addr[i]) { v6_tloop(fs, ip->addr[i], 0); ip->addr[i] = 0; }
            }
            if (ip->addr[slot]) {
                if (within == 0) { v6_tloop(fs, ip->addr[slot], 0); ip->addr[slot] = 0; }
                else v6_tloop_from(fs, ip->addr[slot], 0, within);
            }
            if (ip->addr[7]) { v6_tloop(fs, ip->addr[7], 1); ip->addr[7] = 0; }
        } else {
            uint32_t drem = rem - 7 * V6_NINDIR;
            if (ip->addr[7]) {
                if (drem == 0) { v6_tloop(fs, ip->addr[7], 1); ip->addr[7] = 0; }
                else v6_tloop_from(fs, ip->addr[7], 1, drem);
            }
        }
    } else {
        for (int i = fs->niaddr - 1; i >= (int)first_blk; i--) {
            if (ip->addr[i]) { v6_bfree(fs, ip->addr[i]); ip->addr[i] = 0; }
        }
    }
    return 0;
}

static int v6_ind1(filsys_edition_t *fs, uint32_t *slot, uint32_t idx, int create, uint32_t *out) {
    uint32_t blk = *slot;
    if (blk == 0) {
        if (!create) { *out = 0; return 0; }
        uint8_t z[V6_BSIZE];
        memset(z, 0, V6_BSIZE);
        if (v6_balloc(fs, &blk) || v7fs_write_block(fs, blk, z))
            return -ENOSPC;
        *slot = blk;
    }
    uint8_t buf[V6_BSIZE];
    if (v7fs_read_block(fs, blk, buf))
        return -EIO;
    uint32_t nb = bo_get16le(buf + 2 * idx);
    if (nb == 0 && create) {
        if (v6_balloc(fs, &nb))
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
        if (v6_balloc(fs, &blk) || v7fs_write_block(fs, blk, z))
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
        if (v6_balloc(fs, &sub) || v7fs_write_block(fs, sub, z))
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
        if (v6_balloc(fs, &iblk))
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
        if (v6_balloc(fs, &nb))
            return -ENOSPC;
        ip->addr[lbn] = nb;
    }
    *bno = nb;
    return 0;
}

/* ---- V6 integrity check -------------------------------------------------- */

static int v6_mark_block(filsys_edition_t *fs, filsys_chkctx_t *cx, uint32_t bno) {
    if (bno == 0)
        return 0;
    uint32_t dstart = v6_data_start(fs->isize);
    if (bno < dstart || bno >= fs->fsize) {
        printf("block %u bad; inode=%u\n", bno, cx->ino);
        cx->bad_blocks++;
        cx->errors++;
        return 1;
    }
    uint32_t d = bno - dstart;
    uint8_t  m = (uint8_t)(1u << (d & 7));
    if (cx->bmap[d >> 3] & m) {
        printf("block %u dup; inode=%u\n", bno, cx->ino);
        cx->dup_blocks++;
        cx->errors++;
        return 2;
    }
    cx->bmap[d >> 3] |= m;
    cx->used_blocks++;
    return 0;
}

static void v6_mark_tree(filsys_edition_t *fs, filsys_chkctx_t *cx, uint32_t blk) {
    if (blk == 0)
        return;
    if (v6_mark_block(fs, cx, blk) != 0)
        return;
    uint8_t buf[V6_BSIZE];
    if (v7fs_read_block(fs, blk, buf)) {
        printf("cannot read indirect block %u\n", blk);
        cx->errors++;
        return;
    }
    for (int i = 0; i < V6_NINDIR; i++) {
        uint32_t nb = bo_get16le(buf + 2 * i);
        if (nb != 0)
            v6_mark_block(fs, cx, nb);
    }
}

/* The vtable mark_blocks seam: V6's ILARG layout (all slots single-indirect). */
static void v6_mark_blocks(void *fs, const filsys_inode_t *ip, uint32_t ino,
                           filsys_chkctx_t *cx)
{
    filsys_edition_t *f = fs;
    (void)ino;
    for (int i = 0; i < f->niaddr; i++) {
        uint32_t a = ip->addr[i];
        if (a == 0)
            continue;
        if (ip->mode & V6_ILARG)
            v6_mark_tree(f, cx, a);
        else
            v6_mark_block(f, cx, a);
    }
}

static int v6_makefree(void *fs, filsys_chkctx_t *cx) {
    filsys_edition_t *f = fs;
    int m = 3, n = 100;
    int adr[100];
    uint8_t flg[100] = {0};
    int i, j;
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
    int nfree = 0;
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
                v6_bfree(f, (uint32_t)b);
                nfree++;
            }
        }
    }
    v6_super_write(f);
    return nfree;
}

static uint8_t v6_inode_state(void *fs, uint32_t ino, uint32_t mode) {
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

static void v6_preen(void *fs, const uint8_t *ecount, const uint8_t *state,
                     uint32_t maxino, int mode) {
    filsys_edition_t *f = fs;
    v7_inode_t root;
    uint32_t lf_ino = 0;
    if (v6_read_inode(fs, f->rootino, &root) == 0 &&
        v7fs_dir_lookup(fs, &root, "lost+found", &lf_ino) != 0) {
        v7_inode_t lf;
        if (v6_ialloc(fs, &lf_ino) == 0) {
            memset(&lf, 0, sizeof(lf));
            lf.ino = lf_ino;
            lf.mode = 0777 | V6_IFDIR;
            lf.nlink = 2;
            v6_write_inode(fs, lf_ino, &lf);
            v7fs_dir_add(fs, &lf, lf_ino, ".");
            v7fs_dir_add(fs, &lf, f->rootino, "..");
            v7fs_dir_add(fs, &root, lf_ino, "lost+found");
            root.nlink++;
            v6_write_inode(fs, f->rootino, &root);
            printf("created lost+found (inode %u)\n", lf_ino);
        }
    }

    for (uint32_t ino = 1; ino <= maxino; ino++) {
        int cnt = ecount[ino] & 0377;
        v7_inode_t ip;
        if (v6_read_inode(fs, ino, &ip) != 0)
            continue;
        int allocd = filsys_in_allocated(state[ino]);
        if (!allocd) {
            if (cnt != 0 && filsys_query(mode, "clear free-but-referenced inode %u", ino)) {
                ip.mode = 0;
                ip.nlink = 0;
                v6_write_inode(fs, ino, &ip);
                printf("cleared free-but-referenced inode %u\n", ino);
            }
            continue;
        }
        if (cnt == ip.nlink)
            continue;
        if (ino == f->rootino || ino == lf_ino)
            continue;
        if (cnt == 0) {
            if (lf_ino == 0)
                continue;
            if ((ip.mode & V6_IFMT) != 0)   /* dir or device: leave it */
                continue;
            v7_inode_t lf;
            char name[16];
            snprintf(name, sizeof(name), "%u", ino);
            if (filsys_query(mode, "reconnect inode %u to lost+found", ino) &&
                v6_read_inode(fs, lf_ino, &lf) == 0 &&
                v7fs_dir_add(fs, &lf, ino, name) == 0) {
                ip.nlink = 1;
                v6_write_inode(fs, ino, &ip);
                v6_write_inode(fs, lf_ino, &lf);
                printf("reconnected inode %u to lost+found\n", ino);
            }
        } else {
            int old = ip.nlink;
            if (filsys_query(mode, "fix link count of inode %u from %d to %d",
                             ino, old, cnt)) {
                ip.nlink = (int16_t)(cnt & 0377);
                v6_write_inode(fs, ino, &ip);
                printf("inode %u link count %d -> %d\n", ino, old, ip.nlink);
            }
        }
    }
}

static uint32_t v6_chk_maxino(void *fs) {
    filsys_edition_t *f = fs;
    return v6_maxino(f->isize);
}
static uint32_t v6_chk_data_start(void *fs) {
    filsys_edition_t *f = fs;
    return v6_data_start(f->isize);
}
static uint32_t v6_chk_data_end(void *fs)   { return ((filsys_edition_t *)fs)->fsize; }
static int v6_chk_is_clean(void *fs)        { return ((filsys_edition_t *)fs)->fmod == 0; }

/* Walk the V6 free list (2-byte entries) marking free blocks into cx->bmap. */
static void v6_chk_walk_free(void *fs, filsys_chkctx_t *cx, filsys_check_t *rep)
{
    filsys_edition_t *f = fs;
    uint32_t dstart = v6_data_start(f->isize);
    uint8_t *seen = calloc(f->fsize ? f->fsize : 1, 1);
    if (!seen)
        return;
    uint16_t n = f->fl.nfree;
    uint32_t cur[V6_NICFREE];
    memcpy(cur, f->fl.free, sizeof(cur));
    uint32_t guard = 0;
    while (n > 0) {
        uint32_t bno = cur[--n];
        if (bno == 0)
            break;
        if (bno < dstart || bno >= f->fsize) {
            printf("free block %u out of range [%u,%u)\n", bno, dstart, f->fsize);
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
        uint32_t off = bno - dstart;
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
            uint8_t blk[V6_BSIZE];
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
                cur[i] = bo_get16le(blk + 2 + 2 * i);
        }
    }
    free(seen);
}

int v6_check(filsys_edition_t *fs, v7_check_t *rep, int mode) {
    return filsys_check_common(fs, fs, rep, mode);
}

/* ---- V6 maintenance: ncheck / clri / salv -a ---------------------------- */

static void v6_ncheck_dir(filsys_edition_t *fs, uint32_t dirino, const char *prefix,
                          uint32_t target, int *found, int depth) {
    if (depth > 64)
        return;
    v7_inode_t ip;
    if (v6_read_inode(fs, dirino, &ip))
        return;
    if ((ip.mode & V6_IFMT) != V6_IFDIR)
        return;
    v7_dirent_t *ents = NULL;
    size_t cnt = 0;
    if (v7fs_dir_read(fs, &ip, &ents, &cnt))
        return;
    for (size_t i = 0; i < cnt; i++) {
        uint32_t eino = ents[i].ino;
        if (eino == 0)
            continue;
        if (ents[i].name[0] == '.' &&
            (ents[i].name[1] == 0 ||
             (ents[i].name[1] == '.' && ents[i].name[2] == 0)))
            continue;
        char path[1024];
        if (prefix[1] == 0)
            snprintf(path, sizeof(path), "/%s", ents[i].name);
        else
            snprintf(path, sizeof(path), "%s/%s", prefix, ents[i].name);
        if (eino == target) {
            printf("%u\t%s\n", target, path);
            *found = 1;
        }
        v7_inode_t cip;
        if (v6_read_inode(fs, eino, &cip) == 0 &&
            (cip.mode & V6_IFMT) == V6_IFDIR)
            v6_ncheck_dir(fs, eino, path, target, found, depth + 1);
    }
    v7fs_dirents_free(ents);
}

int v6_ncheck(filsys_edition_t *fs, uint32_t ino) {
    int found = 0;
    v6_ncheck_dir(fs, fs->rootino, "/", ino, &found, 0);
    if (!found)
        printf("%u: not found\n", ino);
    return 0;
}

int v6_clri(filsys_edition_t *fs, uint32_t ino) {
    uint32_t maxino = v6_maxino(fs->isize);
    if (ino == 0 || ino > maxino)
        return -EINVAL;
    v7_inode_t ip;
    memset(&ip, 0, sizeof(ip));
    ip.ino = ino;
    int rc = v6_write_inode(fs, ino, &ip);
    if (rc == 0)
        printf("cleared inode %u\n", ino);
    return rc;
}

int v6_resolve_dups(filsys_edition_t *fs) {
    uint32_t maxino = v6_maxino(fs->isize);
    uint32_t dstart = v6_data_start(fs->isize);
    uint32_t nblk = fs->fsize - dstart;

    filsys_chkctx_t cx;
    memset(&cx, 0, sizeof(cx));
    cx.nblk = nblk;
    cx.bmap = calloc((nblk + 7) / 8, 1);
    if (!cx.bmap)
        return -ENOMEM;

    struct dup { uint32_t blk, ino, idx; };
    struct dup *dups = NULL;
    size_t ndup = 0, cap = 0;

    for (uint32_t ino = 1; ino <= maxino; ino++) {
        v7_inode_t ip;
        if (v6_read_inode(fs, ino, &ip))
            continue;
        if (ip.mode == 0)
            continue;
        int t = ip.mode & V6_IFMT;
        if (t == V6_IFCHR || t == V6_IFBLK)
            continue;
        cx.ino = ino;
        for (int i = 0; i < fs->niaddr; i++) {
            uint32_t a = ip.addr[i];
            if (a == 0)
                continue;
            if (ip.mode & V6_ILARG) {
                v6_mark_tree(fs, &cx, a);
                continue;
            }
            if (a < dstart || a >= fs->fsize) {
                printf("block %u bad; inode=%u\n", a, ino);
                continue;
            }
            uint32_t off = a - dstart;
            uint8_t m = (uint8_t)(1u << (off & 7));
            if (cx.bmap[off >> 3] & m) {
                if (ndup == cap) {
                    cap = cap ? cap * 2 : 16;
                    dups = realloc(dups, cap * sizeof(*dups));
                }
                dups[ndup].blk = a;
                dups[ndup].ino = ino;
                dups[ndup].idx = i;
                ndup++;
            } else {
                cx.bmap[off >> 3] |= m;
                cx.used_blocks++;
            }
        }
    }

    if (ndup == 0) {
        printf("no duplicate blocks\n");
        free(cx.bmap);
        return 0;
    }

    printf("%zu duplicate block(s); rebuilding free list\n", ndup);
    v6_makefree(fs, &cx);

    int resolved = 0;
    for (size_t k = 0; k < ndup; k++) {
        uint32_t blk = dups[k].blk, ino = dups[k].ino, idx = dups[k].idx;
        v7_inode_t ip;
        if (v6_read_inode(fs, ino, &ip))
            continue;
        if (ip.addr[idx] != blk)
            continue;
        uint32_t nb;
        if (v6_balloc(fs, &nb)) {
            printf("block %u dup; inode=%u: out of space\n", blk, ino);
            continue;
        }
        uint8_t buf[V6_BSIZE];
        if (v7fs_read_block(fs, blk, buf) || v7fs_write_block(fs, nb, buf)) {
            printf("block %u dup; inode=%u: copy failed\n", blk, ino);
            continue;
        }
        ip.addr[idx] = nb;
        v6_write_inode(fs, ino, &ip);
        uint32_t off = nb - dstart;
        cx.bmap[off >> 3] |= (uint8_t)(1u << (off & 7));
        printf("block %u dup; inode=%u: copied to %u\n", blk, ino, nb);
        resolved++;
    }
    free(dups);

    printf("resolved %d/%zu duplicates; finalizing free list\n", resolved, ndup);
    v6_makefree(fs, &cx);
    free(cx.bmap);
    return resolved == (int)ndup ? 0 : -1;
}

/* ---- V6 ops table: shared engine + V6 inode/alloc/bmap/check ------------- */

static int v6_open_op(void *fs, const char *path, int readonly,
                      const filsys_edition_t *proto, uint64_t offset)
{ return v6_open(fs, path, readonly, proto, offset); }
static void v6_close_op(void *fs) { v6_close(fs); }
static int v6_sync_op(void *fs) { return v6_sync(fs); }
static int v6_mark_dirty_op(void *fs) { return v6_mark_dirty(fs); }
static int v6_read_inode_op(void *fs, uint32_t ino, filsys_inode_t *ip)
{ return v6_read_inode(fs, ino, ip); }
static int v6_write_inode_op(void *fs, uint32_t ino, const filsys_inode_t *ip)
{ return v6_write_inode(fs, ino, ip); }
static int v6_ialloc_op(void *fs, uint32_t *ino) { return v6_ialloc(fs, ino); }
static void v6_ifree_op(void *fs, uint32_t ino) { v6_ifree(fs, ino); }
static int v6_bmap_op(void *fs, filsys_inode_t *ip, uint32_t lbn, int create, uint32_t *bno)
{ return v6_bmap(fs, ip, lbn, create, bno); }
static int v6_itrunc_op(void *fs, filsys_inode_t *ip) { return v6_itrunc(fs, ip); }
static int v6_itrunc_from_op(void *fs, filsys_inode_t *ip, uint32_t first_blk)
{ return v6_itrunc_from(fs, ip, first_blk); }
static int v6_check_op(void *fs) { v7_check_t rep; return v6_check(fs, &rep, 0); }
static uint64_t v6_max_file_op(void *fs) {
    (void)fs;
    /* The large-file layout can address ~32 MB, but the 24-bit size field
     * caps a file at 16777215 bytes. */
    return (1u << 24) - 1;
}

static void v6_statfs_op(void *fs, struct statvfs *st) {
    filsys_edition_t *v6 = fs;
    st->f_blocks = v6->fsize;
    st->f_bfree = st->f_bavail = v6->fl.tfree;
    st->f_files = v6_maxino(v6->isize);
    st->f_ffree = v6->fl.tinode;
}

const struct filsys_ops v6fs_ops = {
    .name        = "v6",
    .blocksize   = v7fs_blocksize_op,
    .open        = v6_open_op,
    .close       = v6_close_op,
    .sync        = v6_sync_op,
    .mark_dirty  = v6_mark_dirty_op,
    .read_block  = v7fs_read_block_op,
    .write_block = v7fs_write_block_op,
    .blk_get     = v7fs_read_block_op,
    .blk_put     = v7fs_write_block_op,
    .read_inode  = v6_read_inode_op,
    .write_inode = v6_write_inode_op,
    .ialloc      = v6_ialloc_op,
    .ifree       = v6_ifree_op,
    .bmap        = v6_bmap_op,
    .itrunc      = v6_itrunc_op,
    .itrunc_from = v6_itrunc_from_op,
    .file_read   = v7fs_file_read_op,
    .file_write  = v7fs_file_write_op,
    .dir_read    = v7fs_dir_read_op,
    .dir_lookup  = v7fs_dir_lookup_op,
    .dir_add     = v7fs_dir_add_op,
    .dir_remove  = v7fs_dir_remove_op,
    .lookup      = v7fs_lookup_op,
    .check       = v6_check_op,
    .maxino      = v6_chk_maxino,
    .data_start  = v6_chk_data_start,
    .data_end    = v6_chk_data_end,
    .inode_state = v6_inode_state,
    .mark_blocks = v6_mark_blocks,
    .walk_free   = v6_chk_walk_free,
    .makefree    = v6_makefree,
    .preen       = v6_preen,
    .is_clean    = v6_chk_is_clean,
    .statfs      = v6_statfs_op,
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
