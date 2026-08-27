/* kenfs 1.0.0 - 2026-08-26 - Copyright (C) 2026 David Walther */
/* SPDX-License-Identifier: ISC */
/* v6fs.c - Sixth Edition Unix filesystem, on-disk access layer.
 *
 * Reads and writes a V6 filesystem image.  All multi-byte fields follow the
 * PDP-11 middle-endian convention (see v6fs.h).  The allocation algorithms
 * (balloc/bfree/ialloc/ifree) mirror the V6 kernel's sys/alloc.c so the free
 * list stays interchangeable with what a running V6 kernel expects.
 */
#include "v6fs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

static int super_write(v6fs_t *fs);

/* ---- lifecycle --------------------------------------------------------- */

int v6fs_open(v6fs_t *fs, const char *path, int readonly, uint64_t offset) {
    memset(fs, 0, sizeof(*fs));
    fs->readonly = readonly;
    fs->base = offset;
    fs->fd = open(path, readonly ? O_RDONLY : O_RDWR);
    if (fs->fd < 0)
        return -errno;

    uint8_t sb[V6_BSIZE];
    if (v6fs_read_block(fs, V6_SUPERB, sb)) {
        close(fs->fd);
        return -EIO;
    }
    /* V6 superblock: 16-bit fields, s_free[100]/s_inode[100], s_time[2]. */
    fs->isize  = v6_get16le(sb + 0);
    fs->fsize  = v6_get16le(sb + 2);
    fs->nfree  = v6_get16le(sb + 4);
    for (int i = 0; i < V6_NICFREE; i++)
        fs->free[i] = v6_get16le(sb + 6 + 2 * i);
    fs->ninode = v6_get16le(sb + 206);
    for (int i = 0; i < V6_NICINOD; i++)
        fs->inode[i] = v6_get16le(sb + 208 + 2 * i);
    fs->time   = v6_get32me(sb + 412);

    /* Reject a superblock that claims more disk than the image file actually
     * holds, or one with no data area (see the same check in v7fs_open). */
    struct stat st;
    if (fstat(fs->fd, &st) == 0 &&
        (fs->base + (uint64_t)fs->fsize * V6_BSIZE > (uint64_t)st.st_size ||
         fs->fsize <= fs->isize)) {
        close(fs->fd);
        fs->fd = -1;
        return -EINVAL;
    }
    return 0;
}

void v6fs_close(v6fs_t *fs) {
    if (fs->fd >= 0) {
        if (!fs->readonly)
            super_write(fs);
        close(fs->fd);
        fs->fd = -1;
    }
}

/* ---- block io ---------------------------------------------------------- */

int v6fs_read_block(v6fs_t *fs, uint32_t bno, uint8_t *buf) {
    ssize_t n = pread(fs->fd, buf, V6_BSIZE, (off_t)bno * V6_BSIZE + (off_t)fs->base);
    if (n != V6_BSIZE)
        return -EIO;
    return 0;
}

int v6fs_write_block(v6fs_t *fs, uint32_t bno, const uint8_t *buf) {
    if (fs->readonly)
        return -EROFS;
    ssize_t n = pwrite(fs->fd, buf, V6_BSIZE, (off_t)bno * V6_BSIZE + (off_t)fs->base);
    if (n != V6_BSIZE)
        return -EIO;
    return 0;
}

/* ---- superblock persistence --------------------------------------------- */

static int super_write(v6fs_t *fs) {
    if (fs->readonly)
        return 0;
    uint8_t sb[V6_BSIZE];
    /* Read the current block to preserve the fields we don't maintain
     * (s_flock/s_ilock/s_fmod/s_ronly, the pad words, ...). */
    if (v6fs_read_block(fs, V6_SUPERB, sb))
        return -EIO;
    v6_put16le(sb + 0, fs->isize);
    v6_put16le(sb + 2, fs->fsize);
    v6_put16le(sb + 4, fs->nfree);
    for (int i = 0; i < V6_NICFREE; i++)
        v6_put16le(sb + 6 + 2 * i, (uint16_t)fs->free[i]);
    v6_put16le(sb + 206, fs->ninode);
    for (int i = 0; i < V6_NICINOD; i++)
        v6_put16le(sb + 208 + 2 * i, fs->inode[i]);
    v6_put32me(sb + 412, (uint32_t)time(NULL));  /* s_time[2] */
    return v6fs_write_block(fs, V6_SUPERB, sb);
}

/* ---- inode io ---------------------------------------------------------- */

int v6fs_read_inode(v6fs_t *fs, uint32_t ino, v6_inode_t *ip) {
    if (ino == 0)
        return -EINVAL;
    uint32_t bno = v6_itod(ino);
    uint32_t off = v6_itoo(ino);
    if (bno >= fs->isize)   /* i-list lives in blocks 2..s_isize-1 */
        return -EINVAL;
    uint8_t raw[V6_BSIZE];
    if (v6fs_read_block(fs, bno, raw))
        return -EIO;
    const uint8_t *d = raw + off * V6_INODESZ;
    memset(ip, 0, sizeof(*ip));
    ip->ino   = ino;
    ip->mode  = v6_get16le(d + 0);
    ip->nlink = (int16_t)d[2];        /* char */
    ip->uid   = (int16_t)d[3];        /* char */
    ip->gid   = (int16_t)d[4];        /* char */
    /* 24-bit size: i_size0 @5 is the HIGH byte (bits 16..23),
     * i_size1 @6..7 is the LOW word (bits 0..15). */
    ip->size  = ((uint32_t)d[5] << 16) | v6_get16le(d + 6);
    for (int i = 0; i < V6_NIADDR; i++)
        ip->addr[i] = v6_get16le(d + 8 + 2 * i);
    ip->atime = v6_get32me(d + 24);
    ip->mtime = v6_get32me(d + 28);
    ip->ctime = ip->mtime;            /* V6 has no ctime */
    return 0;
}

int v6fs_write_inode(v6fs_t *fs, uint32_t ino, const v6_inode_t *ip) {
    if (ino == 0)
        return -EINVAL;
    uint32_t bno = v6_itod(ino);
    uint32_t off = v6_itoo(ino);
    if (bno >= fs->isize)
        return -EINVAL;
    uint8_t raw[V6_BSIZE];
    if (v6fs_read_block(fs, bno, raw))
        return -EIO;
    uint8_t *d = raw + off * V6_INODESZ;
    v6_put16le(d + 0, ip->mode);
    d[2] = (uint8_t)ip->nlink;
    d[3] = (uint8_t)ip->uid;
    d[4] = (uint8_t)ip->gid;
    /* 24-bit size: i_size0 @5 = high byte, i_size1 @6..7 = low word */
    d[5] = (uint8_t)((ip->size >> 16) & 0xff);
    v6_put16le(d + 6, (uint16_t)(ip->size & 0xffff));
    for (int i = 0; i < V6_NIADDR; i++)
        v6_put16le(d + 8 + 2 * i, (uint16_t)ip->addr[i]);
    v6_put32me(d + 24, ip->atime);
    v6_put32me(d + 28, ip->mtime);
    return v6fs_write_block(fs, bno, raw);
}

/* ---- allocation -------------------------------------------------------- */

int v6fs_balloc(v6fs_t *fs, uint32_t *bno) {
    if (fs->nfree == 0)
        return -ENOSPC;   /* no cached blocks and no dump block to reload */

    uint32_t blk = fs->free[--fs->nfree];
    if (blk == 0)
        return -ENOSPC;

    if (fs->nfree == 0) {
        /* Just popped the bottom of the stack: it is a free-list block. */
        uint8_t buf[V6_BSIZE];
        if (v6fs_read_block(fs, blk, buf))
            return -EIO;
        fs->nfree = v6_get16le(buf + 0);
        for (int i = 0; i < V6_NICFREE; i++)
            fs->free[i] = v6_get16le(buf + 2 + 2 * i);
    }
    if (blk < fs->isize || blk >= fs->fsize)
        return -EIO;   /* badblock: refuse garbage */
    super_write(fs);
    *bno = blk;
    return 0;
}

void v6fs_bfree(v6fs_t *fs, uint32_t bno) {
    if (bno < fs->isize || bno >= fs->fsize)
        return;   /* badblock */
    if (fs->nfree == 0) {
        fs->nfree = 1;
        fs->free[0] = 0;
    }
    if (fs->nfree >= V6_NICFREE) {
        uint8_t buf[V6_BSIZE];
        memset(buf, 0, V6_BSIZE);
        v6_put16le(buf + 0, fs->nfree);
        for (int i = 0; i < V6_NICFREE; i++)
            v6_put16le(buf + 2 + 2 * i, (uint16_t)fs->free[i]);
        if (v6fs_write_block(fs, bno, buf) == 0)
            fs->nfree = 0;
    }
    fs->free[fs->nfree++] = bno;
    super_write(fs);
}

int v6fs_ialloc(v6fs_t *fs, uint32_t *ino) {
    uint32_t maxino = (uint32_t)(fs->isize - 2) * V6_INOPB;

    for (;;) {
        if (fs->ninode > 0) {
            uint32_t cand = fs->inode[--fs->ninode];
            v6_inode_t ip;
            if (cand >= 2 && cand <= maxino &&
                v6fs_read_inode(fs, cand, &ip) == 0 && ip.mode == 0) {
                memset(&ip, 0, sizeof(ip));
                ip.ino = cand;
                v6fs_write_inode(fs, cand, &ip);
                super_write(fs);
                *ino = cand;
                return 0;
            }
            continue;   /* was already allocated; look again */
        }
        /* Refill the cache with a linear scan of the i-list. */
        fs->ninode = 0;
        for (uint32_t in = 2; in <= maxino && fs->ninode < V6_NICINOD; in++) {
            v6_inode_t ip;
            if (v6fs_read_inode(fs, in, &ip))
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

void v6fs_ifree(v6fs_t *fs, uint32_t ino) {
    if (fs->ninode >= V6_NICINOD)
        return;   /* kernel discards beyond the cache */
    fs->inode[fs->ninode++] = (uint16_t)ino;
    super_write(fs);
}

/* ---- truncate ----------------------------------------------------------- */

static void tloop(v6fs_t *fs, uint32_t blk, int level) {
    uint8_t buf[V6_BSIZE];
    if (v6fs_read_block(fs, blk, buf))
        return;
    for (int i = V6_NINDIR - 1; i >= 0; i--) {
        uint32_t nb = v6_get16le(buf + 2 * i);
        if (nb == 0)
            continue;
        if (level > 0)
            tloop(fs, nb, level - 1);
        else
            v6fs_bfree(fs, nb);
    }
    v6fs_bfree(fs, blk);
}

int v6fs_itrunc(v6fs_t *fs, v6_inode_t *ip) {
    int t = ip->mode & V6_IFMT;
    if (t != 0 && t != V6_IFDIR)   /* regular (type 0) and directory only */
        return 0;
    if (ip->mode & V6_ILARG) {
        for (int i = 0; i < 7; i++) {          /* 7 single-indirect slots */
            uint32_t bn = ip->addr[i];
            if (bn) { ip->addr[i] = 0; tloop(fs, bn, 0); }
        }
        uint32_t bn = ip->addr[7];             /* 1 double-indirect slot */
        if (bn) { ip->addr[7] = 0; tloop(fs, bn, 1); }
    } else {
        for (int i = 0; i < V6_NIADDR; i++) {  /* 8 direct blocks */
            uint32_t bn = ip->addr[i];
            if (bn) { ip->addr[i] = 0; v6fs_bfree(fs, bn); }
        }
    }
    ip->size = 0;
    return 0;
}

/* ---- block mapping ------------------------------------------------------ */

/* Single indirect: *slot -> block, index `idx` (16-bit entries). */
static int ind1(v6fs_t *fs, uint32_t *slot, uint32_t idx, int create, uint32_t *out) {
    uint32_t blk = *slot;
    if (blk == 0) {
        if (!create) { *out = 0; return 0; }
        uint8_t z[V6_BSIZE];
        memset(z, 0, V6_BSIZE);
        if (v6fs_balloc(fs, &blk) || v6fs_write_block(fs, blk, z))
            return -ENOSPC;
        *slot = blk;
    }
    uint8_t buf[V6_BSIZE];
    if (v6fs_read_block(fs, blk, buf))
        return -EIO;
    uint32_t nb = v6_get16le(buf + 2 * idx);
    if (nb == 0 && create) {
        if (v6fs_balloc(fs, &nb))
            return -ENOSPC;
        v6_put16le(buf + 2 * idx, (uint16_t)nb);
        if (v6fs_write_block(fs, blk, buf))
            return -EIO;
    }
    *out = nb;
    return 0;
}

/* Double indirect: *slot -> block, outer `o` -> block, inner `i`. */
static int ind2(v6fs_t *fs, uint32_t *slot, uint32_t o, uint32_t i, int create, uint32_t *out) {
    uint32_t blk = *slot;
    if (blk == 0) {
        if (!create) { *out = 0; return 0; }
        uint8_t z[V6_BSIZE];
        memset(z, 0, V6_BSIZE);
        if (v6fs_balloc(fs, &blk) || v6fs_write_block(fs, blk, z))
            return -ENOSPC;
        *slot = blk;
    }
    uint8_t buf[V6_BSIZE];
    if (v6fs_read_block(fs, blk, buf))
        return -EIO;
    uint32_t sub = v6_get16le(buf + 2 * o);
    if (sub == 0 && create) {
        uint8_t z[V6_BSIZE];
        memset(z, 0, V6_BSIZE);
        if (v6fs_balloc(fs, &sub) || v6fs_write_block(fs, sub, z))
            return -ENOSPC;
        v6_put16le(buf + 2 * o, (uint16_t)sub);
        if (v6fs_write_block(fs, blk, buf))
            return -EIO;
    }
    if (sub == 0) { *out = 0; return 0; }
    return ind1(fs, &sub, i, create, out);
}

int v6fs_bmap(v6fs_t *fs, v6_inode_t *ip, uint32_t lbn, int create, uint32_t *bno) {
    if (ip->mode & V6_ILARG) {
        /* large file: i_addr[0..6] single-indirect, i_addr[7] double-indirect */
        if (lbn < 7 * V6_NINDIR)
            return ind1(fs, &ip->addr[lbn >> 8], lbn & (V6_NINDIR - 1), create, bno);
        uint32_t r = lbn - 7 * V6_NINDIR;
        return ind2(fs, &ip->addr[7], r >> 8, r & (V6_NINDIR - 1), create, bno);
    }
    /* small file: 8 direct blocks */
    if (lbn >= V6_NDADDR) {
        *bno = 0;
        return create ? -EFBIG : 0;
    }
    uint32_t nb = ip->addr[lbn];
    if (nb == 0 && create) {
        if (v6fs_balloc(fs, &nb))
            return -ENOSPC;
        ip->addr[lbn] = nb;
    }
    *bno = nb;
    return 0;
}

/* ---- file data ---------------------------------------------------------- */

ssize_t v6fs_file_read(v6fs_t *fs, v6_inode_t *ip, uint8_t *buf, size_t size, off_t off) {
    if (off < 0)
        return -EINVAL;
    if ((uint64_t)off >= ip->size)
        return 0;
    uint64_t remaining = ip->size - (uint64_t)off;
    if (size > remaining)
        size = (size_t)remaining;

    size_t done = 0;
    while (done < size) {
        uint32_t lbn  = (uint32_t)((off + (off_t)done) / V6_BSIZE);
        uint32_t boff = (uint32_t)((off + (off_t)done) % V6_BSIZE);
        uint32_t pbn;
        if (v6fs_bmap(fs, ip, lbn, 0, &pbn))
            return -EIO;
        uint8_t blk[V6_BSIZE];
        if (pbn == 0) {
            memset(blk, 0, V6_BSIZE);   /* sparse hole */
        } else if (v6fs_read_block(fs, pbn, blk)) {
            return -EIO;
        }
        size_t n = V6_BSIZE - boff;
        if (n > size - done)
            n = size - done;
        memcpy(buf + done, blk + boff, n);
        done += n;
    }
    return (ssize_t)done;
}

ssize_t v6fs_file_write(v6fs_t *fs, v6_inode_t *ip, const uint8_t *buf, size_t size, off_t off) {
    if (off < 0)
        return -EINVAL;

    size_t done = 0;
    while (done < size) {
        uint32_t lbn  = (uint32_t)((off + (off_t)done) / V6_BSIZE);
        uint32_t boff = (uint32_t)((off + (off_t)done) % V6_BSIZE);
        uint32_t pbn;
        if (v6fs_bmap(fs, ip, lbn, 1, &pbn))
            return -ENOSPC;
        if (pbn == 0)
            return -ENOSPC;

        uint8_t blk[V6_BSIZE];
        if (v6fs_read_block(fs, pbn, blk))
            return -EIO;

        size_t n = V6_BSIZE - boff;
        if (n > size - done)
            n = size - done;
        memcpy(blk + boff, buf + done, n);
        if (v6fs_write_block(fs, pbn, blk))
            return -EIO;
        done += n;
    }
    if ((uint64_t)off + size > ip->size)
        ip->size = (uint32_t)(off + size);
    v6fs_write_inode(fs, ip->ino, ip);
    return (ssize_t)done;
}

/* ---- directories -------------------------------------------------------- */

int v6fs_dir_read(v6fs_t *fs, v6_inode_t *ip, v6_dirent_t **ents, size_t *count) {
    if ((ip->mode & V6_IFMT) != V6_IFDIR)
        return -ENOTDIR;
    /* Bound the directory size to the data area before allocating (a corrupt
     * 24-bit size could otherwise reach 16 MiB *beyond* the real data area). */
    if (ip->size > (uint64_t)(fs->fsize - fs->isize) * V6_BSIZE)
        return -EFBIG;
    size_t cap = ip->size / 16 + 1;
    v6_dirent_t *out = calloc(cap, sizeof(v6_dirent_t));
    if (!out)
        return -ENOMEM;

    uint8_t *buf = malloc(ip->size);
    if (!buf) {
        free(out);
        return -ENOMEM;
    }
    ssize_t n = v6fs_file_read(fs, ip, buf, ip->size, 0);
    if (n < 0) {
        free(buf);
        free(out);
        return (int)n;
    }

    size_t cnt = 0;
    for (size_t off = 0; off + 16 <= (size_t)n; off += 16) {
        uint16_t ino = v6_get16le(buf + off);
        if (ino == 0)
            continue;
        out[cnt].ino = ino;
        memcpy(out[cnt].name, buf + off + 2, V6_DIRSIZ);
        out[cnt].name[V6_DIRSIZ] = 0;
        cnt++;
    }
    free(buf);
    *ents = out;
    *count = cnt;
    return 0;
}

void v6fs_dirents_free(v6_dirent_t *ents) {
    free(ents);
}

int v6fs_dir_lookup(v6fs_t *fs, v6_inode_t *ip, const char *name, uint32_t *ino) {
    v6_dirent_t *ents = NULL;
    size_t count = 0;
    int rc = v6fs_dir_read(fs, ip, &ents, &count);
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
    v6fs_dirents_free(ents);
    return rc;
}

int v6fs_dir_add(v6fs_t *fs, v6_inode_t *ip, uint32_t ino, const char *name) {
    size_t namelen = strlen(name);
    if (namelen == 0 || namelen > V6_DIRSIZ)
        return -ENAMETOOLONG;
    if (strchr(name, '/'))
        return -EINVAL;

    size_t newsize = ip->size + 16;
    uint8_t *buf = malloc(newsize);
    if (!buf)
        return -ENOMEM;
    memset(buf, 0, newsize);
    ssize_t n = v6fs_file_read(fs, ip, buf, ip->size, 0);
    if (n < 0) {
        free(buf);
        return (int)n;
    }

    /* find an empty slot, else append */
    size_t slot = SIZE_MAX;
    for (size_t off = 0; off + 16 <= (size_t)n; off += 16) {
        if (v6_get16le(buf + off) == 0) {
            slot = off;
            break;
        }
    }
    if (slot == SIZE_MAX) {
        slot = (size_t)n;
        n += 16;
    }

    v6_put16le(buf + slot, (uint16_t)ino);
    memset(buf + slot + 2, 0, V6_DIRSIZ);
    memcpy(buf + slot + 2, name, namelen);

    ssize_t w = v6fs_file_write(fs, ip, buf, (size_t)n, 0);
    free(buf);
    return w < 0 ? (int)w : 0;
}

int v6fs_dir_remove(v6fs_t *fs, v6_inode_t *ip, const char *name) {
    uint8_t *buf = malloc(ip->size);
    if (!buf)
        return -ENOMEM;
    ssize_t n = v6fs_file_read(fs, ip, buf, ip->size, 0);
    if (n < 0) {
        free(buf);
        return (int)n;
    }
    int rc = -ENOENT;
    for (size_t off = 0; off + 16 <= (size_t)n; off += 16) {
        if (v6_get16le(buf + off) == 0)
            continue;
        char ent[V6_DIRSIZ + 1];
        memcpy(ent, buf + off + 2, V6_DIRSIZ);
        ent[V6_DIRSIZ] = 0;
        if (strcmp(ent, name) == 0) {
            v6_put16le(buf + off, 0);
            memset(buf + off + 2, 0, V6_DIRSIZ);
            ssize_t w = v6fs_file_write(fs, ip, buf, (size_t)n, 0);
            rc = w < 0 ? (int)w : 0;
            break;
        }
    }
    free(buf);
    return rc;
}

/* ---- path lookup -------------------------------------------------------- */

int v6fs_lookup(v6fs_t *fs, const char *path, uint32_t *ino, v6_inode_t *ip) {
    if (path[0] != '/')
        return -EINVAL;
    uint32_t cur = V6_ROOTINO;
    v6_inode_t dip;
    if (v6fs_read_inode(fs, cur, &dip))
        return -EIO;

    const char *p = path + 1;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (len == 0) {
            p++;
            continue;
        }
        if (len > V6_DIRSIZ)
            return -ENAMETOOLONG;
        char name[V6_DIRSIZ + 1];
        memcpy(name, p, len);
        name[len] = 0;

        if ((dip.mode & V6_IFMT) != V6_IFDIR)
            return -ENOTDIR;
        uint32_t next;
        int rc = v6fs_dir_lookup(fs, &dip, name, &next);
        if (rc)
            return rc;
        if (v6fs_read_inode(fs, next, &dip))
            return -EIO;
        p = slash ? slash + 1 : p + len;
    }
    *ino = dip.ino;
    if (ip)
        *ip = dip;
    return 0;
}

/* ---- integrity check ---------------------------------------------------- */

int v6fs_check(v6fs_t *fs, v6_check_t *rep) {
    memset(rep, 0, sizeof(*rep));

    /* 1. superblock sanity */
    if (fs->isize >= fs->fsize || fs->fsize == 0) {
        printf("bad superblock: isize=%u fsize=%u\n", fs->isize, fs->fsize);
        rep->errors++;
    }
    if (fs->nfree > V6_NICFREE) {
        printf("bad nfree=%u (>%d)\n", fs->nfree, V6_NICFREE);
        rep->errors++;
    }
    if (fs->ninode > V6_NICINOD) {
        printf("bad ninode=%u (>%d)\n", fs->ninode, V6_NICINOD);
        rep->errors++;
    }

    /* 2. walk the free list exactly as alloc() would, checking integrity */
    uint8_t *seen = calloc(fs->fsize ? fs->fsize : 1, 1);
    if (!seen)
        return -ENOMEM;
    uint16_t n = fs->nfree;
    uint32_t cur[V6_NICFREE];
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
        if (++guard > fs->fsize + V6_NICFREE) {
            printf("free list does not terminate\n");
            rep->errors++;
            break;
        }
        if (n == 0) {
            /* just popped the bottom: it is a dump block holding the next batch */
            uint8_t blk[V6_BSIZE];
            if (v6fs_read_block(fs, bno, blk)) {
                printf("cannot read free-list block %u\n", bno);
                rep->errors++;
                break;
            }
            n = v6_get16le(blk);
            if (n > V6_NICFREE) {
                printf("free-list block %u has bad count %u\n", bno, n);
                rep->errors++;
                break;
            }
            for (int i = 0; i < V6_NICFREE; i++)
                cur[i] = v6_get16le(blk + 2 + 2 * i);
        }
    }
    free(seen);

    /* 3. inode table walk */
    uint32_t maxino = (uint32_t)(fs->isize - 2) * V6_INOPB;
    rep->inodes = maxino;
    for (uint32_t ino = 1; ino <= maxino; ino++) {
        v6_inode_t ip;
        if (v6fs_read_inode(fs, ino, &ip)) {
            printf("inode %u unreadable\n", ino);
            rep->errors++;
            continue;
        }
        if (ip.mode == 0)
            continue;
        rep->used_inodes++;
        for (int i = 0; i < V6_NIADDR; i++) {
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
