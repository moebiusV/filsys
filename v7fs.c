/* filsys 1.0.0 - 2026-08-26 - Copyright (C) 2026 David Walther */
/* SPDX-License-Identifier: ISC */
/* v7fs.c - Seventh Edition Unix filesystem, on-disk access layer.
 *
 * Reads and writes a V7 filesystem image, and a 32V (VAX) image under the
 * same code with the little-endian byte order selected (see v7fs.h).  The
 * allocation algorithms (balloc/bfree/ialloc/ifree) mirror the V7 kernel's
 * sys/alloc.c so the free list stays interchangeable with what a running
 * kernel expects.
 */
#include "v7fs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

static int super_write(v7fs_t *fs);

/* 32V (VAX) aligns daddr_t/time_t to 4 bytes, so in the superblock and the
 * free-list dump block the fields that follow such a type sit 2 bytes later
 * than they do in V7 (PDP-11, 2-byte alignment).  The inode is unaffected
 * (di_size already lands on a 4-byte boundary). */
static inline int sb_fsize_off(int le)  { return le ? 4 : 2; }
static inline int sb_nfree_off(int le)  { return le ? 8 : 6; }
static inline int sb_free_off(int le)   { return le ? 12 : 8; }
static inline int sb_ninode_off(int le) { return le ? 212 : 208; }
static inline int sb_inode_off(int le)  { return le ? 214 : 210; }
static inline int sb_time_off(int le)   { return le ? 420 : 414; }
static inline int fb_free_off(int le)   { return le ? 4 : 2; }

/* ---- lifecycle --------------------------------------------------------- */

int v7fs_open(v7fs_t *fs, const char *path, int readonly, int little_endian,
              uint64_t offset) {
    memset(fs, 0, sizeof(*fs));
    fs->readonly = readonly;
    fs->le = little_endian;
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
    fs->fsize  = v7_get32(sb + sb_fsize_off(fs->le), fs->le);
    fs->nfree  = v7_get16le(sb + sb_nfree_off(fs->le));
    for (int i = 0; i < V7_NICFREE; i++)
        fs->free[i] = v7_get32(sb + sb_free_off(fs->le) + 4 * i, fs->le);
    fs->ninode = v7_get16le(sb + sb_ninode_off(fs->le));
    for (int i = 0; i < V7_NICINOD; i++)
        fs->inode[i] = v7_get16le(sb + sb_inode_off(fs->le) + 2 * i);
    fs->time   = v7_get32(sb + sb_time_off(fs->le), fs->le);

    /* Reject a superblock that claims more disk than the image file actually
     * holds, or one with no data area.  Without this, a corrupt image can make
     * the checker (and directory readers) allocate gigabytes. */
    struct stat st;
    if (fstat(fs->fd, &st) == 0 &&
        (fs->base + (uint64_t)fs->fsize * V7_BSIZE > (uint64_t)st.st_size ||
         fs->fsize <= fs->isize)) {
        close(fs->fd);
        fs->fd = -1;
        return -EINVAL;
    }
    return 0;
}

void v7fs_close(v7fs_t *fs) {
    if (fs->fd >= 0) {
        if (!fs->readonly)
            super_write(fs);
        close(fs->fd);
        fs->fd = -1;
    }
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
    v7_put32(sb + sb_fsize_off(fs->le), fs->le, fs->fsize);
    v7_put16le(sb + sb_nfree_off(fs->le), fs->nfree);
    for (int i = 0; i < V7_NICFREE; i++)
        v7_put32(sb + sb_free_off(fs->le) + 4 * i, fs->le, fs->free[i]);
    v7_put16le(sb + sb_ninode_off(fs->le), fs->ninode);
    for (int i = 0; i < V7_NICINOD; i++)
        v7_put16le(sb + sb_inode_off(fs->le) + 2 * i, fs->inode[i]);
    v7_put32(sb + sb_time_off(fs->le), fs->le, (uint32_t)time(NULL));  /* s_time */
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

    if (fs->nfree == 0) {
        /* Just popped the bottom of the stack: it is a free-list block. */
        uint8_t buf[V7_BSIZE];
        if (v7fs_read_block(fs, blk, buf))
            return -EIO;
        fs->nfree = v7_get16le(buf + 0);
        for (int i = 0; i < V7_NICFREE; i++)
            fs->free[i] = v7_get32(buf + fb_free_off(fs->le) + 4 * i, fs->le);
    }
    if (blk < fs->isize || blk >= fs->fsize)
        return -EIO;   /* badblock: refuse garbage */
    super_write(fs);
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
    if (fs->nfree >= V7_NICFREE) {
        uint8_t buf[V7_BSIZE];
        memset(buf, 0, V7_BSIZE);
        v7_put16le(buf + 0, fs->nfree);
        for (int i = 0; i < V7_NICFREE; i++)
            v7_put32(buf + fb_free_off(fs->le) + 4 * i, fs->le, fs->free[i]);
        if (v7fs_write_block(fs, bno, buf) == 0)
            fs->nfree = 0;
    }
    fs->free[fs->nfree++] = bno;
    super_write(fs);
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
                super_write(fs);
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
        if (fs->ninode == 0) {
            super_write(fs);
            return -ENOSPC;
        }
    }
}

void v7fs_ifree(v7fs_t *fs, uint32_t ino) {
    if (fs->ninode >= V7_NICINOD)
        return;   /* kernel discards beyond the cache */
    fs->inode[fs->ninode++] = (uint16_t)ino;
    super_write(fs);
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

int v7fs_check(v7fs_t *fs, v7_check_t *rep) {
    memset(rep, 0, sizeof(*rep));

    /* 1. superblock sanity */
    if (fs->isize >= fs->fsize || fs->fsize == 0) {
        printf("bad superblock: isize=%u fsize=%u\n", fs->isize, fs->fsize);
        rep->errors++;
    }
    if (fs->nfree > V7_NICFREE) {
        printf("bad nfree=%u (>%d)\n", fs->nfree, V7_NICFREE);
        rep->errors++;
    }
    if (fs->ninode > V7_NICINOD) {
        printf("bad ninode=%u (>%d)\n", fs->ninode, V7_NICINOD);
        rep->errors++;
    }

    /* 2. walk the free list exactly as alloc() would, checking integrity */
    uint8_t *seen = calloc(fs->fsize ? fs->fsize : 1, 1);
    if (!seen)
        return -ENOMEM;
    uint16_t n = fs->nfree;
    uint32_t cur[V7_NICFREE];
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
        if (++guard > fs->fsize + V7_NICFREE) {
            printf("free list does not terminate\n");
            rep->errors++;
            break;
        }
        if (n == 0) {
            /* just popped the bottom: it is a dump block holding the next batch */
            uint8_t blk[V7_BSIZE];
            if (v7fs_read_block(fs, bno, blk)) {
                printf("cannot read free-list block %u\n", bno);
                rep->errors++;
                break;
            }
            n = v7_get16le(blk);
            if (n > V7_NICFREE) {
                printf("free-list block %u has bad count %u\n", bno, n);
                rep->errors++;
                break;
            }
            for (int i = 0; i < V7_NICFREE; i++)
                cur[i] = v7_get32(blk + fb_free_off(fs->le) + 4 * i, fs->le);
        }
    }
    free(seen);

    /* 3. inode table walk */
    uint32_t maxino = (uint32_t)(fs->isize - 2) * V7_INOPB;
    rep->inodes = maxino;
    for (uint32_t ino = 1; ino <= maxino; ino++) {
        v7_inode_t ip;
        if (v7fs_read_inode(fs, ino, &ip)) {
            printf("inode %u unreadable\n", ino);
            rep->errors++;
            continue;
        }
        if (ip.mode == 0)
            continue;
        rep->used_inodes++;
        for (int i = 0; i < V7_NIADDR; i++) {
            uint32_t a = ip.addr[i];
            if (a != 0 && a >= fs->fsize) {
                printf("inode %u addr[%d]=%u out of range\n", ino, i, a);
                rep->errors++;
            }
        }
    }

    printf("free blocks=%u  inodes=%u/%u used  errors=%u\n",
           rep->free_blocks, rep->used_inodes, rep->inodes, rep->errors);
    return rep->errors ? -1 : 0;
}
