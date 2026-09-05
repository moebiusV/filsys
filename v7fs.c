/* filsys 1.2.6 - 2026-08-26 - Copyright (C) 2026 David Walther */
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

static int super_write(v7fs_t *fs);

/* ---- lifecycle --------------------------------------------------------- */

int v7fs_open(v7fs_t *fs, const char *path, int readonly, int mode,
              uint64_t offset) {
    memset(fs, 0, sizeof(*fs));
    fs->readonly = readonly;
    fs->le = (mode == 1);            /* only 32V is little-endian */
    fs->coherent = (mode == 2);      /* Coherent: middle-endian like V7, NICFREE=64 */
    fs->base = offset;
    fs->fd = open(path, readonly ? O_RDONLY : O_RDWR);
    if (fs->fd < 0)
        return -errno;

    uint8_t sb[V7_BSIZE];
    if (v7fs_read_block(fs, V7_SUPERB, sb)) {
        close(fs->fd);
        return -EIO;
    }
    fs->isize  = v7_get16le(sb + 0);
    fs->fsize  = v7_get32(sb + sb_fsize_off(fs->le, fs->coherent), fs->le);
    fs->nfree  = v7_get16le(sb + sb_nfree_off(fs->le, fs->coherent));
    for (int i = 0; i < sb_nicfree(fs->coherent); i++)
        fs->free[i] = v7_get32(sb + sb_free_off(fs->le, fs->coherent) + 4 * i, fs->le);
    fs->ninode = v7_get16le(sb + sb_ninode_off(fs->le, fs->coherent));
    for (int i = 0; i < V7_NICINOD; i++)
        fs->inode[i] = v7_get16le(sb + sb_inode_off(fs->le, fs->coherent) + 2 * i);
    fs->time   = v7_get32(sb + sb_time_off(fs->le, fs->coherent), fs->le);
    fs->fmod   = sb[sb_time_off(fs->le, fs->coherent) - 2];  /* s_fmod */
    /* s_tfree/s_tinode carry the true free-space totals (v7fs_makefree writes
     * them); the 50/100-entry caches are only the in-core spill.  Read them so
     * statfs can report real free space rather than the cache depth.  They sit
     * immediately after s_time, whose offset already accounts for the layout. */
    if (fs->le == 0) {
        int toff = sb_time_off(fs->le, fs->coherent);
        fs->tfree  = v7_get32(sb + toff + 4, fs->le);
        fs->tinode = v7_get16le(sb + toff + 8);
    }
    if (fs->coherent) {
        fs->m      = v7_get16le(sb + sb_time_off(fs->le, fs->coherent) + 10);
        fs->n      = v7_get16le(sb + sb_time_off(fs->le, fs->coherent) + 12);
        fs->unique = v7_get32(sb + sb_time_off(fs->le, fs->coherent) + 26, fs->le);
    }

    /* Reject a superblock that claims more disk than the image file actually
     * holds, one with no data area, or an i-list too small to subtract 2 from
     * (`(isize - 2)` underflows when isize is 0 or 1, turning the inode walk
     * into a multi-gigabyte loop).  Without this, a corrupt image can make
     * the checker (and directory readers) allocate gigabytes. */
    struct stat st;
    if (fstat(fs->fd, &st) != 0 ||
        fs->isize < 2 ||
        fs->base + (uint64_t)fs->fsize * V7_BSIZE > (uint64_t)st.st_size ||
        fs->fsize <= fs->isize) {
        close(fs->fd);
        fs->fd = -1;
        return -EINVAL;
    }
    return 0;
}

void v7fs_close(v7fs_t *fs) {
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

int v7fs_sync(v7fs_t *fs) {
    if (fs->readonly)
        return 0;
    return super_write(fs);
}

int v7fs_mark_dirty(v7fs_t *fs) {
    if (fs->readonly)
        return 0;
    fs->fmod = 1;
    return super_write(fs);
}

/* ---- block io ---------------------------------------------------------- */

int v7fs_read_block(v7fs_t *fs, uint32_t bno, uint8_t *buf) {
    ssize_t n = pread(fs->fd, buf, V7_BSIZE, (off_t)bno * V7_BSIZE + (off_t)fs->base);
    if (n != V7_BSIZE)
        return -EIO;
    return 0;
}

int v7fs_write_block(v7fs_t *fs, uint32_t bno, const uint8_t *buf) {
    if (fs->readonly)
        return -EROFS;
    ssize_t n = pwrite(fs->fd, buf, V7_BSIZE, (off_t)bno * V7_BSIZE + (off_t)fs->base);
    if (n != V7_BSIZE)
        return -EIO;
    return 0;
}

/* ---- superblock persistence --------------------------------------------- */

static int super_write(v7fs_t *fs) {
    if (fs->readonly)
        return 0;
    uint8_t sb[V7_BSIZE];
    /* Read the current block to preserve the fields we don't maintain
     * (s_tfree, s_tinode, s_m, s_n, s_fname, s_fpack, ...). */
    if (v7fs_read_block(fs, V7_SUPERB, sb))
        return -EIO;
    v7_put16le(sb + 0, fs->isize);
    v7_put32(sb + sb_fsize_off(fs->le, fs->coherent), fs->le, fs->fsize);
    v7_put16le(sb + sb_nfree_off(fs->le, fs->coherent), fs->nfree);
    for (int i = 0; i < sb_nicfree(fs->coherent); i++)
        v7_put32(sb + sb_free_off(fs->le, fs->coherent) + 4 * i, fs->le, fs->free[i]);
    v7_put16le(sb + sb_ninode_off(fs->le, fs->coherent), fs->ninode);
    for (int i = 0; i < V7_NICINOD; i++)
        v7_put16le(sb + sb_inode_off(fs->le, fs->coherent) + 2 * i, fs->inode[i]);
    v7_put32(sb + sb_time_off(fs->le, fs->coherent), fs->le, (uint32_t)time(NULL));  /* s_time */
    sb[sb_time_off(fs->le, fs->coherent) - 2] = (uint8_t)(fs->fmod != 0);            /* s_fmod */
    if (fs->le == 0) {
        int toff = sb_time_off(fs->le, fs->coherent);
        v7_put32(sb + toff + 4, fs->le, fs->tfree);        /* s_tfree */
        v7_put16le(sb + toff + 8, (uint16_t)fs->tinode);   /* s_tinode */
    }
    if (fs->coherent) {
        v7_put16le(sb + sb_time_off(fs->le, fs->coherent) + 10, fs->m);
        v7_put16le(sb + sb_time_off(fs->le, fs->coherent) + 12, fs->n);
        v7_put32(sb + sb_time_off(fs->le, fs->coherent) + 26, fs->le, fs->unique);
    }
    return v7fs_write_block(fs, V7_SUPERB, sb);
}

/* ---- inode io ---------------------------------------------------------- */

int v7fs_read_inode(v7fs_t *fs, uint32_t ino, v7_inode_t *ip) {
    if (ino == 0)
        return -EINVAL;
    uint32_t bno = v7_itod(ino);
    uint32_t off = v7_itoo(ino);
    if (bno >= fs->isize)   /* i-list lives in blocks 2..s_isize-1 */
        return -EINVAL;
    uint8_t raw[V7_BSIZE];
    if (v7fs_read_block(fs, bno, raw))
        return -EIO;
    const uint8_t *d = raw + off * V7_INODESZ;
    memset(ip, 0, sizeof(*ip));
    ip->ino   = ino;
    ip->mode  = v7_get16le(d + 0);
    ip->nlink = (int16_t)v7_get16le(d + 2);
    ip->uid   = (int16_t)v7_get16le(d + 4);
    ip->gid   = (int16_t)v7_get16le(d + 6);
    ip->size  = v7_get32(d + 8, fs->le);
    for (int i = 0; i < V7_NIADDR; i++)
        ip->addr[i] = v7_get24(d + 12 + 3 * i, fs->le);
    ip->atime = v7_get32(d + 52, fs->le);
    ip->mtime = v7_get32(d + 56, fs->le);
    ip->ctime = v7_get32(d + 60, fs->le);
    return 0;
}

int v7fs_write_inode(v7fs_t *fs, uint32_t ino, const v7_inode_t *ip) {
    if (ino == 0)
        return -EINVAL;
    uint32_t bno = v7_itod(ino);
    uint32_t off = v7_itoo(ino);
    if (bno >= fs->isize)
        return -EINVAL;
    uint8_t raw[V7_BSIZE];
    if (v7fs_read_block(fs, bno, raw))
        return -EIO;
    uint8_t *d = raw + off * V7_INODESZ;
    v7_put16le(d + 0, ip->mode);
    v7_put16le(d + 2, (uint16_t)ip->nlink);
    v7_put16le(d + 4, (uint16_t)ip->uid);
    v7_put16le(d + 6, (uint16_t)ip->gid);
    v7_put32(d + 8, fs->le, ip->size);
    for (int i = 0; i < V7_NIADDR; i++)
        v7_put24(d + 12 + 3 * i, fs->le, ip->addr[i]);
    v7_put32(d + 52, fs->le, ip->atime);
    v7_put32(d + 56, fs->le, ip->mtime);
    v7_put32(d + 60, fs->le, ip->ctime);
    return v7fs_write_block(fs, bno, raw);
}

/* ---- allocation -------------------------------------------------------- */

int v7fs_balloc(v7fs_t *fs, uint32_t *bno) {
    if (fs->nfree == 0)
        return -ENOSPC;   /* no cached blocks and no dump block to reload */

    uint32_t blk = fs->free[--fs->nfree];
    if (blk == 0)
        return -ENOSPC;

    /* Range-check before touching the free list: on a corrupt image a bogus
     * block number must be rejected here, not after it has been used to reload
     * the in-core free list with garbage. */
    if (blk < fs->isize || blk >= fs->fsize)
        return -EIO;   /* badblock: refuse garbage */

    if (fs->nfree == 0) {
        /* Just popped the bottom of the stack: it is a free-list block. */
        uint8_t buf[V7_BSIZE];
        if (v7fs_read_block(fs, blk, buf))
            return -EIO;
        fs->nfree = v7_get16le(buf + 0);
        for (int i = 0; i < sb_nicfree(fs->coherent); i++)
            fs->free[i] = v7_get32(buf + fb_free_off(fs->le, fs->coherent) + 4 * i, fs->le);
    }
    /* Zero the freshly-allocated block: V7's alloc() clrbuf()s it, and without
     * this the previous file's data leaks into a new file. */
    uint8_t z[V7_BSIZE];
    memset(z, 0, V7_BSIZE);
    if (v7fs_write_block(fs, blk, z))
        return -EIO;
    if (fs->tfree) fs->tfree--;
    *bno = blk;
    return 0;
}

void v7fs_bfree(v7fs_t *fs, uint32_t bno) {
    if (bno < fs->isize || bno >= fs->fsize)
        return;   /* badblock */
    if (fs->nfree == 0) {
        fs->nfree = 1;
        fs->free[0] = 0;
    }
    if (fs->nfree >= sb_nicfree(fs->coherent)) {
        uint8_t buf[V7_BSIZE];
        memset(buf, 0, V7_BSIZE);
        v7_put16le(buf + 0, fs->nfree);
        for (int i = 0; i < sb_nicfree(fs->coherent); i++)
            v7_put32(buf + fb_free_off(fs->le, fs->coherent) + 4 * i, fs->le, fs->free[i]);
        if (v7fs_write_block(fs, bno, buf) == 0)
            fs->nfree = 0;
    }
    fs->free[fs->nfree++] = bno;
    fs->tfree++;
}

int v7fs_ialloc(v7fs_t *fs, uint32_t *ino) {
    uint32_t maxino = (uint32_t)(fs->isize - 2) * V7_INOPB;

    for (;;) {
        if (fs->ninode > 0) {
            uint32_t cand = fs->inode[--fs->ninode];
            v7_inode_t ip;
            if (cand >= 2 && cand <= maxino &&
                v7fs_read_inode(fs, cand, &ip) == 0 && ip.mode == 0) {
                memset(&ip, 0, sizeof(ip));
                ip.ino = cand;
                v7fs_write_inode(fs, cand, &ip);
                if (fs->tinode) fs->tinode--;
                *ino = cand;
                return 0;
            }
            continue;   /* was already allocated; look again */
        }
        /* Refill the cache with a linear scan of the i-list. */
        fs->ninode = 0;
        for (uint32_t in = 2; in <= maxino && fs->ninode < V7_NICINOD; in++) {
            v7_inode_t ip;
            if (v7fs_read_inode(fs, in, &ip))
                break;
            if (ip.mode == 0)
                fs->inode[fs->ninode++] = (uint16_t)in;
        }
        if (fs->ninode == 0)
            return -ENOSPC;
    }
}

void v7fs_ifree(v7fs_t *fs, uint32_t ino) {
    if (fs->ninode >= V7_NICINOD)
        return;   /* kernel discards beyond the cache */
    fs->inode[fs->ninode++] = (uint16_t)ino;
    fs->tinode++;
}

/* ---- truncate ----------------------------------------------------------- */

static void tloop(v7fs_t *fs, uint32_t blk, int level) {
    uint8_t buf[V7_BSIZE];
    if (v7fs_read_block(fs, blk, buf))
        return;
    for (int i = V7_NINDIR - 1; i >= 0; i--) {
        uint32_t nb = v7_get32(buf + 4 * i, fs->le);
        if (nb == 0)
            continue;
        if (level > 0)
            tloop(fs, nb, level - 1);
        else
            v7fs_bfree(fs, nb);
    }
    v7fs_bfree(fs, blk);
}

int v7fs_itrunc(v7fs_t *fs, v7_inode_t *ip) {
    int t = ip->mode & V7_IFMT;
    if (t != V7_IFREG && t != V7_IFDIR)
        return 0;
    for (int i = V7_NIADDR - 1; i >= 0; i--) {
        uint32_t bn = ip->addr[i];
        if (bn == 0)
            continue;
        ip->addr[i] = 0;
        switch (i) {
        case V7_NIADDR - 1: tloop(fs, bn, 2); break;   /* triple */
        case V7_NIADDR - 2: tloop(fs, bn, 1); break;   /* double */
        case V7_NIADDR - 3: tloop(fs, bn, 0); break;   /* single */
        default:            v7fs_bfree(fs, bn); break; /* direct */
        }
    }
    ip->size = 0;
    return 0;
}

/* Free the leaf blocks at indices [skip, ...) of the subtree rooted at `blk`
 * (which has `level` levels of indirection below it).  When skip == 0 the whole
 * subtree and `blk` itself are freed. */
static void tloop_from(v7fs_t *fs, uint32_t blk, int level, uint32_t skip) {
    uint8_t buf[V7_BSIZE];
    if (v7fs_read_block(fs, blk, buf))
        return;
    uint32_t sub = 1;
    for (int l = 0; l < level; l++) sub *= V7_NINDIR;   /* leaves per entry */
    uint32_t se = skip / sub;                            /* whole entries to skip */
    uint32_t sp = skip % sub;                            /* partial skip within entry se */
    for (int i = V7_NINDIR - 1; i >= 0; i--) {
        uint32_t nb = v7_get32(buf + 4 * i, fs->le);
        if (nb == 0)
            continue;
        if ((uint32_t)i < se)
            continue;                                    /* whole entry kept */
        if ((uint32_t)i == se && sp > 0) {
            tloop_from(fs, nb, level - 1, sp);           /* partial: entry kept */
        } else {
            if (level == 0) v7fs_bfree(fs, nb);
            else tloop(fs, nb, level - 1);
            v7_put32(buf + 4 * i, fs->le, 0);            /* drop the freed entry */
        }
    }
    /* skip > 0 by construction: itrunc_from calls tloop_from only for a partial
     * (non-zero) count, and recursion fires only when sp > 0.  The whole-block
     * case is handled by tloop() at the call sites. */
    v7fs_write_block(fs, blk, buf);                      /* persist dropped entries */
}

/* Free every block from logical block `first_blk` onwards, leaving the first
 * `first_blk` blocks in place.  `first_blk == 0` is equivalent to itrunc. */
int v7fs_itrunc_from(v7fs_t *fs, v7_inode_t *ip, uint32_t first_blk) {
    uint32_t rem = first_blk;

    /* Direct blocks [0, V7_NDADDR) */
    if (rem < V7_NDADDR) {
        for (int i = V7_NDADDR - 1; i >= (int)rem; i--) {
            if (ip->addr[i]) { v7fs_bfree(fs, ip->addr[i]); ip->addr[i] = 0; }
        }
        rem = 0;
    } else {
        rem -= V7_NDADDR;
    }

    /* Single indirect [V7_NDADDR, V7_NDADDR+V7_NINDIR) */
    if (rem < V7_NINDIR) {
        if (ip->addr[V7_NDADDR]) {
            if (rem == 0) { tloop(fs, ip->addr[V7_NDADDR], 0); ip->addr[V7_NDADDR] = 0; }
            else tloop_from(fs, ip->addr[V7_NDADDR], 0, rem);
        }
        rem = 0;
    } else {
        rem -= V7_NINDIR;
    }

    /* Double indirect [V7_NDADDR+V7_NINDIR, ...+V7_NINDIR^2) */
    if (rem < (uint32_t)V7_NINDIR * V7_NINDIR) {
        if (ip->addr[V7_NDADDR + 1]) {
            if (rem == 0) { tloop(fs, ip->addr[V7_NDADDR + 1], 1); ip->addr[V7_NDADDR + 1] = 0; }
            else tloop_from(fs, ip->addr[V7_NDADDR + 1], 1, rem);
        }
        rem = 0;
    } else {
        rem -= (uint32_t)V7_NINDIR * V7_NINDIR;
    }

    /* Triple indirect [V7_NDADDR+V7_NINDIR+V7_NINDIR^2, ...) */
    if (ip->addr[V7_NDADDR + 2]) {
        if (rem == 0) { tloop(fs, ip->addr[V7_NDADDR + 2], 2); ip->addr[V7_NDADDR + 2] = 0; }
        else tloop_from(fs, ip->addr[V7_NDADDR + 2], 2, rem);
    }
    return 0;
}

/* ---- block mapping ------------------------------------------------------ */

/* Follow an indirect chain of `levels` levels (1/2/3) from *slot.
 * indices[0] is the outermost index.  Allocates when create is set. */
static int ind_follow(v7fs_t *fs, uint32_t *slot, int levels,
                      const uint32_t *indices, int create, uint32_t *out) {
    uint32_t blk = *slot;
    for (int L = 0; L < levels; L++) {
        if (blk == 0) {
            if (!create) {
                *out = 0;
                return 0;
            }
            uint8_t z[V7_BSIZE];
            memset(z, 0, V7_BSIZE);
            if (v7fs_balloc(fs, &blk) || v7fs_write_block(fs, blk, z))
                return -ENOSPC;
            *slot = blk;
        }
        uint8_t buf[V7_BSIZE];
        if (v7fs_read_block(fs, blk, buf))
            return -EIO;
        uint32_t next = v7_get32(buf + 4 * indices[L], fs->le);

        if (L == levels - 1) {
            if (next == 0 && create) {
                if (v7fs_balloc(fs, &next))
                    return -ENOSPC;
                v7_put32(buf + 4 * indices[L], fs->le, next);
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
            uint8_t z[V7_BSIZE];
            memset(z, 0, V7_BSIZE);
            if (v7fs_balloc(fs, &next) || v7fs_write_block(fs, next, z))
                return -ENOSPC;
            v7_put32(buf + 4 * indices[L], fs->le, next);
            if (v7fs_write_block(fs, blk, buf))
                return -EIO;
        }
        blk = next;
    }
    return -EIO;   /* unreachable */
}

int v7fs_bmap(v7fs_t *fs, v7_inode_t *ip, uint32_t lbn, int create, uint32_t *bno) {
    if (lbn < V7_NDADDR) {
        uint32_t nb = ip->addr[lbn];
        if (nb == 0 && create) {
            if (v7fs_balloc(fs, &nb))
                return -ENOSPC;
            ip->addr[lbn] = nb;
        }
        *bno = nb;
        return 0;
    }
    uint32_t r = lbn - V7_NDADDR;
    if (r < V7_NINDIR)
        return ind_follow(fs, &ip->addr[10], 1, &r, create, bno);
    r -= V7_NINDIR;
    if (r < (uint32_t)V7_NINDIR * V7_NINDIR) {
        uint32_t idx[2] = { r / V7_NINDIR, r % V7_NINDIR };
        return ind_follow(fs, &ip->addr[11], 2, idx, create, bno);
    }
    r -= (uint32_t)V7_NINDIR * V7_NINDIR;
    uint32_t idx[3] = { r / (V7_NINDIR * V7_NINDIR),
                        (r / V7_NINDIR) % V7_NINDIR, r % V7_NINDIR };
    return ind_follow(fs, &ip->addr[12], 3, idx, create, bno);
}

/* ---- file data ---------------------------------------------------------- */

ssize_t v7fs_file_read(v7fs_t *fs, v7_inode_t *ip, uint8_t *buf, size_t size, off_t off) {
    if (off < 0)
        return -EINVAL;
    if ((uint64_t)off >= ip->size)
        return 0;
    uint64_t remaining = ip->size - (uint64_t)off;
    if (size > remaining)
        size = (size_t)remaining;

    size_t done = 0;
    while (done < size) {
        uint32_t lbn  = (uint32_t)((off + (off_t)done) / V7_BSIZE);
        uint32_t boff = (uint32_t)((off + (off_t)done) % V7_BSIZE);
        uint32_t pbn;
        if (v7fs_bmap(fs, ip, lbn, 0, &pbn))
            return -EIO;
        uint8_t blk[V7_BSIZE];
        if (pbn == 0) {
            memset(blk, 0, V7_BSIZE);   /* sparse hole */
        } else if (v7fs_read_block(fs, pbn, blk)) {
            return -EIO;
        }
        size_t n = V7_BSIZE - boff;
        if (n > size - done)
            n = size - done;
        memcpy(buf + done, blk + boff, n);
        done += n;
    }
    return (ssize_t)done;
}

ssize_t v7fs_file_write(v7fs_t *fs, v7_inode_t *ip, const uint8_t *buf, size_t size, off_t off) {
    if (off < 0)
        return -EINVAL;

    size_t done = 0;
    while (done < size) {
        uint32_t lbn  = (uint32_t)((off + (off_t)done) / V7_BSIZE);
        uint32_t boff = (uint32_t)((off + (off_t)done) % V7_BSIZE);
        uint32_t pbn;
        if (v7fs_bmap(fs, ip, lbn, 1, &pbn))
            return -ENOSPC;
        if (pbn == 0)
            return -ENOSPC;

        uint8_t blk[V7_BSIZE];
        if (v7fs_read_block(fs, pbn, blk))
            return -EIO;

        size_t n = V7_BSIZE - boff;
        if (n > size - done)
            n = size - done;
        memcpy(blk + boff, buf + done, n);
        if (v7fs_write_block(fs, pbn, blk))
            return -EIO;
        done += n;
    }
    if ((uint64_t)off + size > ip->size)
        ip->size = (uint32_t)(off + size);
    v7fs_write_inode(fs, ip->ino, ip);
    return (ssize_t)done;
}

/* ---- directories -------------------------------------------------------- */

int v7fs_dir_read(v7fs_t *fs, v7_inode_t *ip, v7_dirent_t **ents, size_t *count) {
    if ((ip->mode & V7_IFMT) != V7_IFDIR)
        return -ENOTDIR;
    /* A directory's data cannot exceed the filesystem's data area; reject a
     * corrupt size before the malloc below, else a bogus di_size (up to 4 GiB)
     * turns into a multi-gigabyte allocation. */
    if (ip->size > (uint64_t)(fs->fsize - fs->isize) * V7_BSIZE)
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
        uint16_t ino = v7_get16le(buf + off);
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

int v7fs_dir_lookup(v7fs_t *fs, v7_inode_t *ip, const char *name, uint32_t *ino) {
    v7_dirent_t *ents = NULL;
    size_t count = 0;
    int rc = v7fs_dir_read(fs, ip, &ents, &count);
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

int v7fs_dir_add(v7fs_t *fs, v7_inode_t *ip, uint32_t ino, const char *name) {
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
        if (v7_get16le(buf + off) == 0) {
            slot = off;
            break;
        }
    }
    if (slot == SIZE_MAX) {
        slot = (size_t)n;
        n += 16;
    }

    v7_put16le(buf + slot, (uint16_t)ino);
    memset(buf + slot + 2, 0, V7_DIRSIZ);
    memcpy(buf + slot + 2, name, namelen);

    ssize_t w = v7fs_file_write(fs, ip, buf, (size_t)n, 0);
    free(buf);
    return w < 0 ? (int)w : 0;
}

int v7fs_dir_remove(v7fs_t *fs, v7_inode_t *ip, const char *name) {
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
        if (v7_get16le(buf + off) == 0)
            continue;
        char ent[V7_DIRSIZ + 1];
        memcpy(ent, buf + off + 2, V7_DIRSIZ);
        ent[V7_DIRSIZ] = 0;
        if (strcmp(ent, name) == 0) {
            v7_put16le(buf + off, 0);
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

int v7fs_lookup(v7fs_t *fs, const char *path, uint32_t *ino, v7_inode_t *ip) {
    if (path[0] != '/')
        return -EINVAL;
    uint32_t cur = V7_ROOTINO;
    v7_inode_t dip;
    if (v7fs_read_inode(fs, cur, &dip))
        return -EIO;

    const char *p = path + 1;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (len == 0) {
            p++;
            continue;
        }
        if (len > V7_DIRSIZ)
            return -ENAMETOOLONG;
        char name[V7_DIRSIZ + 1];
        memcpy(name, p, len);
        name[len] = 0;

        if ((dip.mode & V7_IFMT) != V7_IFDIR)
            return -ENOTDIR;
        uint32_t next;
        int rc = v7fs_dir_lookup(fs, &dip, name, &next);
        if (rc)
            return rc;
        if (v7fs_read_inode(fs, next, &dip))
            return -EIO;
        p = slash ? slash + 1 : p + len;
    }
    *ino = dip.ino;
    if (ip)
        *ip = dip;
    return 0;
}

/* ---- integrity check ---------------------------------------------------- */

/* Context for the icheck(8) block-usage pass.  One bit per data block records
 * whether the block is accounted for (referenced by an inode, or in the free
 * list); a block reached twice is a duplicate, a block never reached is
 * "missing". */
typedef struct {
    v7fs_t  *fs;
    uint8_t *bmap;        /* bit i = data block (isize + i) */
    uint32_t *owner;      /* owner[block - isize] = first inode to claim it (or NULL) */
    uint32_t nblk;        /* number of data blocks (fsize - isize) */
    uint32_t used_blocks;
    uint32_t dup_blocks;
    uint32_t bad_blocks;  /* out-of-range block references */
    uint32_t errors;
    uint32_t ino;         /* current inode, for messages */
} v7_chkctx_t;

/* Mark one data block as accounted-for.  Returns 0 on a first claim, 1 for an
 * out-of-range (bad) block, 2 for a duplicate (already claimed).  Inode 1 is
 * the bad-block inode (V7_BADFIN): its addresses record bad blocks, so a
 * reference into the i-list region [2, isize) is a record, not an error. */
static int v7_mark_block(v7_chkctx_t *cx, uint32_t bno)
{
    if (bno == 0)
        return 0;
    if (bno < cx->fs->isize || bno >= cx->fs->fsize) {
        if (cx->ino == V7_BADFIN && bno >= 2 && bno < cx->fs->isize)
            return 0;   /* bad-block record: a gone-bad i-list block */
        printf("block %u bad; inode=%u\n", bno, cx->ino);
        cx->bad_blocks++;
        cx->errors++;
        return 1;
    }
    uint32_t d = bno - cx->fs->isize;
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
static void v7_mark_tree(v7_chkctx_t *cx, uint32_t blk, int level)
{
    if (blk == 0)
        return;
    if (v7_mark_block(cx, blk) != 0)
        return;                       /* bad or duplicate: don't chase it */
    uint8_t buf[V7_BSIZE];
    if (v7fs_read_block(cx->fs, blk, buf)) {
        printf("cannot read indirect block %u\n", blk);
        cx->errors++;
        return;
    }
    for (int i = 0; i < V7_NINDIR; i++) {
        uint32_t nb = v7_get32(buf + 4 * i, cx->fs->le);
        if (nb == 0)
            continue;
        if (level > 0)
            v7_mark_tree(cx, nb, level - 1);
        else
            v7_mark_block(cx, nb);
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
static int v7fs_makefree(v7fs_t *fs, v7_chkctx_t *cx)
{
    int m, n;
    if (fs->coherent) {
        m = fs->m;
        n = fs->n;
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
    uint32_t mapbot = ((fs->isize + n - 1) / n) * n;
    uint32_t maptop = (fs->fsize / n) * n;

    /* v7fs_bfree increments fs->tfree as it goes, so reset the running totals
     * first -- otherwise the rebuilt count is added on top of the old one. */
    fs->nfree = 0;
    fs->ninode = 0;
    fs->tfree = 0;
    fs->tinode = 0;
    int nfree = 0;
    for (uint32_t bn = fs->isize; bn < fs->fsize; bn++) {
        uint32_t blk = bn;
        if (bn >= mapbot && bn < maptop)
            blk = (bn / n) * n + (uint32_t)maptab[bn % n];
        uint32_t off = blk - fs->isize;
        if (off >= cx->nblk)
            continue;   /* interleave stays within [isize, fsize), but be safe */
        if (!(cx->bmap[off >> 3] & (uint8_t)(1u << (off & 7)))) {
            v7fs_bfree(fs, blk);
            nfree++;
        }
    }
    super_write(fs);   /* writes fs->tfree (= nfree) and fs->tinode (= 0) */
    return nfree;
}

/* Classify an inode's mode into a checker state (the low three type bits). */
static uint8_t v7_inode_state(uint16_t mode) {
    switch (mode & V7_IFMT) {
    case V7_IFDIR: return FILSYS_IN_IDIR;
    case V7_IFREG: return FILSYS_IN_IREG;
    case V7_IFCHR: case V7_IFMPC: return FILSYS_IN_ICHR;
    case V7_IFBLK: case V7_IFMPB: return FILSYS_IN_IBLK;
    case V7_IFIFO: return FILSYS_IN_IPIPE;
    default: return FILSYS_IN_UNKNOWN;
    }
}

static void v7fs_preen(v7fs_t *fs, const uint8_t *ecount, const uint8_t *state,
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

int v7fs_check(v7fs_t *fs, v7_check_t *rep, int mode) {
    memset(rep, 0, sizeof(*rep));

    if (fs->fmod == 0 && !(mode & (FILSYS_CK_SALVAGE | FILSYS_CK_FORCE))) {
        printf("filesystem clean; skipped (use -f to force)\n");
        return 0;
    }

    /* 1. superblock sanity.  A bad superblock makes everything downstream
     * meaningless, so it sets the Coherent-style errflag and bails before the
     * i-list and block walks (nblk = fsize - isize would underflow). */
    int errflag = 0;
    if (fs->isize >= fs->fsize || fs->fsize == 0) {
        printf("bad superblock: isize=%u fsize=%u\n", fs->isize, fs->fsize);
        rep->errors++;
        errflag = 1;
    }
    if (fs->nfree > sb_nicfree(fs->coherent)) {
        printf("bad nfree=%u (>%d)\n", fs->nfree, sb_nicfree(fs->coherent));
        rep->errors++;
    }
    if (fs->ninode > V7_NICINOD) {
        printf("bad ninode=%u (>%d)\n", fs->ninode, V7_NICINOD);
        rep->errors++;
    }
    if (errflag) {
        printf("superblock unusable; skipping check\n");
        return -1;
    }

    uint32_t maxino = (uint32_t)(fs->isize - 2) * V7_INOPB;
    rep->inodes = maxino;

    uint32_t nblk = fs->fsize - fs->isize;
    v7_chkctx_t cx;
    memset(&cx, 0, sizeof(cx));
    cx.fs = fs;
    cx.nblk = nblk;
    cx.bmap = calloc((nblk + 7) / 8, 1);
    if (!cx.bmap)
        return -ENOMEM;

    /* owner table: name the inode that first claimed each block, so a duplicate
     * report can say who already owns it (BSD fsck's phase-1b rescan). */
    cx.owner = calloc(nblk + 1, sizeof(uint32_t));
    if (!cx.owner) {
        free(cx.bmap);
        return -ENOMEM;
    }

    uint8_t *state = calloc(maxino + 1, 1);
    if (!state) {
        free(cx.owner);
        free(cx.bmap);
        return -ENOMEM;
    }

    /* 2. icheck pass 1: mark every block referenced by an inode. */
    for (uint32_t ino = 1; ino <= maxino; ino++) {
        v7_inode_t ip;
        if (v7fs_read_inode(fs, ino, &ip)) {
            printf("inode %u unreadable\n", ino);
            rep->errors++;
            continue;
        }
        if (ip.mode == 0)
            continue;   /* state[ino] stays UNALLOC */
        rep->used_inodes++;
        state[ino] = v7_inode_state(ip.mode);
        if (state[ino] == FILSYS_IN_UNKNOWN) {
            printf("inode %u unknown type 0%o\n", ino, ip.mode & V7_IFMT);
            rep->errors++;
            continue;   /* unknown type: block addresses are untrusted */
        }
        if (state[ino] == FILSYS_IN_ICHR || state[ino] == FILSYS_IN_IBLK)
            continue;   /* device inode: addr[0] is a device number, not a block */
        cx.ino = ino;
        for (int i = 0; i < V7_NIADDR; i++) {
            uint32_t a = ip.addr[i];
            if (a == 0)
                continue;
            if (i < V7_NDADDR)
                v7_mark_block(&cx, a);
            else
                v7_mark_tree(&cx, a, i - V7_NDADDR);
        }
    }

    /* fold the mark-phase findings (dup/bad blocks) into the report */
    rep->errors += cx.errors;

    /* Too many bad blocks leave the block accounting untrustworthy, so skip the
     * free-list and directory phases rather than report phantom "missing"
     * blocks (BSD fsck's errflag on excessive bad blocks). */
    if (cx.bad_blocks > FILSYS_MAXBADOK) {
        printf("too many bad blocks (%u); skipping free-list and directory checks\n",
               cx.bad_blocks);
        free(state);
        free(cx.owner);
        free(cx.bmap);
        return -1;
    }

    if (mode & FILSYS_CK_SALVAGE) {
        int nf = v7fs_makefree(fs, &cx);
        printf("salvaged: free list rebuilt (%d free blocks)\n", nf);
        free(state);
        free(cx.owner);
        free(cx.bmap);
        return rep->errors ? -1 : 0;
    }

    /* 3. walk the free list exactly as alloc() would, marking free blocks. */
    uint8_t *seen = calloc(fs->fsize ? fs->fsize : 1, 1);
    if (!seen) {
        free(state);
        free(cx.owner);
        free(cx.bmap);
        return -ENOMEM;
    }
    uint16_t n = fs->nfree;
    uint32_t cur[V7_COH_NICFREE];
    memcpy(cur, fs->free, sizeof(cur));
    uint32_t guard = 0;
    while (n > 0) {
        uint32_t bno = cur[--n];
        if (bno == 0)
            break;                       /* sentinel: end of chain */
        if (bno < fs->isize || bno >= fs->fsize) {
            printf("free block %u out of range [%u,%u)\n",
                   bno, fs->isize, fs->fsize);
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
        /* a free block that is also referenced by an inode is a duplicate */
        uint32_t off = bno - fs->isize;
        uint8_t m = (uint8_t)(1u << (off & 7));
        if (cx.bmap[off >> 3] & m) {
            printf("block %u dup; free-list\n", bno);
            cx.dup_blocks++;
            rep->errors++;
        } else {
            cx.bmap[off >> 3] |= m;
        }
        if (++guard > fs->fsize + sb_nicfree(fs->coherent)) {
            printf("free list does not terminate\n");
            rep->errors++;
            break;
        }
        if (n == 0) {
            uint8_t blk[V7_BSIZE];
            if (v7fs_read_block(fs, bno, blk)) {
                printf("cannot read free-list block %u\n", bno);
                rep->errors++;
                break;
            }
            n = v7_get16le(blk);
            if (n > sb_nicfree(fs->coherent)) {
                printf("free-list block %u has bad count %u\n", bno, n);
                rep->errors++;
                break;
            }
            for (int i = 0; i < sb_nicfree(fs->coherent); i++)
                cur[i] = v7_get32(blk + fb_free_off(fs->le, fs->coherent) + 4 * i, fs->le);
        }
    }
    free(seen);

    /* 4. missing blocks: in the data area but neither used nor free. */
    for (uint32_t off = 0; off < nblk; off++)
        if (!(cx.bmap[off >> 3] & (uint8_t)(1u << (off & 7)))) {
            rep->missing_blocks++;
            rep->errors++;
        }

    rep->used_blocks = cx.used_blocks;
    rep->dup_blocks = cx.dup_blocks;

    /* 5. dcheck: directory link counts. */
    uint8_t *ecount = calloc(maxino + 1, 1);
    if (ecount) {
        for (uint32_t ino = 1; ino <= maxino; ino++) {
            v7_inode_t ip;
            if (v7fs_read_inode(fs, ino, &ip))
                continue;
            if ((ip.mode & V7_IFMT) != V7_IFDIR)
                continue;
            v7_dirent_t *ents = NULL;
            size_t cnt = 0;
            if (v7fs_dir_read(fs, &ip, &ents, &cnt) == 0) {
                for (size_t e = 0; e < cnt; e++) {
                    uint32_t dno = ents[e].ino;
                    if (dno == 0)
                        continue;
                    if (dno > maxino || dno <= 1) {
                        printf("%u bad; %u/%.*s\n", dno, ino, V7_DIRSIZ, ents[e].name);
                        rep->errors++;
                        continue;
                    }
                    if (!filsys_in_allocated(state[dno])) {
                        printf("dir %u references free inode %u\n", ino, dno);
                        rep->errors++;
                    }
                    ecount[dno]++;
                    if (ecount[dno] == 0)
                        ecount[dno] = 0377;
                }
                v7fs_dirents_free(ents);
            }
        }
        for (uint32_t ino = 1; ino <= maxino; ino++) {
            v7_inode_t ip;
            if (v7fs_read_inode(fs, ino, &ip))
                continue;
            int cnt = ecount[ino] & 0377;
            if (cnt == ip.nlink)
                continue;
            if ((ip.mode & V7_IFMT) == 0 && cnt == 0)
                continue;
            printf("%u entries=%d link=%d\n", ino, cnt, ip.nlink);
            rep->errors++;
        }
        if (mode & (FILSYS_CK_PREEN | FILSYS_CK_YES | FILSYS_CK_ASK))
            v7fs_preen(fs, ecount, state, maxino, mode);
        free(ecount);
    }

    free(state);
    free(cx.bmap);

    printf("used blocks=%u  free blocks=%u  missing=%u  dup=%u  inodes=%u/%u used  errors=%u\n",
           rep->used_blocks, rep->free_blocks, rep->missing_blocks,
           rep->dup_blocks, rep->used_inodes, rep->inodes, rep->errors);
    return rep->errors ? -1 : 0;
}

/* ---- maintenance: ncheck / clri / salv -a ------------------------------ */

/* Recursively walk the directory tree from `dirino`, printing the pathname(s)
 * of `target` and descending into subdirectories. */
static void ncheck_dir(v7fs_t *fs, uint32_t dirino, const char *prefix,
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

int v7fs_ncheck(v7fs_t *fs, uint32_t ino)
{
    int found = 0;
    ncheck_dir(fs, V7_ROOTINO, "/", ino, &found, 0);
    if (!found)
        printf("%u: not found\n", ino);
    return 0;
}

int v7fs_clri(v7fs_t *fs, uint32_t ino)
{
    uint32_t maxino = (uint32_t)(fs->isize - 2) * V7_INOPB;
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

int v7fs_resolve_dups(v7fs_t *fs)
{
    uint32_t maxino = (uint32_t)(fs->isize - 2) * V7_INOPB;
    uint32_t nblk = fs->fsize - fs->isize;

    v7_chkctx_t cx;
    memset(&cx, 0, sizeof(cx));
    cx.fs = fs;
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
        for (int i = 0; i < V7_NIADDR; i++) {
            uint32_t a = ip.addr[i];
            if (a == 0)
                continue;
            if (i < V7_NDADDR) {
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
                v7_mark_tree(&cx, a, i - V7_NDADDR);
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
        uint8_t buf[V7_BSIZE];
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
 * v7fs_t*, so there is no cast anywhere. */

static int v7fs_open_op(void *fs, const char *path, int readonly, int mode,
                        uint64_t offset) {
    return v7fs_open(fs, path, readonly, mode, offset);
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
    (void)fs;
    uint64_t n = V7_NINDIR;
    return ((uint64_t)V7_NDADDR + n + n * n + n * n * n) * V7_BSIZE;
}

const struct filsys_ops v7fs_ops = {
    .name        = "v7",
    .blocksize   = V7_BSIZE,
    .open        = v7fs_open_op,
    .close       = v7fs_close_op,
    .sync        = v7fs_sync_op,
    .mark_dirty  = v7fs_mark_dirty_op,
    .read_block  = v7fs_read_block_op,
    .write_block = v7fs_write_block_op,
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
    .max_file    = v7fs_max_file_op,
};
