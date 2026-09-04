/* filsys 1.2.5 - 2026-08-26 - Copyright (C) 2026 David Walther */
/* SPDX-License-Identifier: ISC */
/* v1fs.c - First Edition (V1) Unix filesystem, on-disk access layer.
 *
 * Reads and writes a V1 filesystem image.  The allocator is a pair of bitmaps
 * in the superblock (a free-block map where 1 = free, and an inode map where
 * 0 = free, indexed from inode 41); there is no s_isize -- the i-list size is
 * derived from the inode-map size.  Directory entries are 10 bytes.  See
 * v1fs.h and docs/v1-format.md.
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

int v1fs_open(v1fs_t *fs, const char *path, int readonly, uint64_t offset) {
    memset(fs, 0, sizeof(*fs));
    fs->readonly = readonly;
    fs->base = offset;
    fs->fd = open(path, readonly ? O_RDONLY : O_RDWR);
    if (fs->fd < 0)
        return -errno;

    uint8_t sb[V1_BSIZE * 2];
    if (v1fs_read_block(fs, 0, sb) || v1fs_read_block(fs, 1, sb + V1_BSIZE)) {
        close(fs->fd);
        return -EIO;
    }

    fs->freemap_bytes = v1_get16le(sb + 0);
    uint32_t freemap_off = 2;
    uint32_t inodemap_bytes_off = freemap_off + fs->freemap_bytes;
    fs->inodemap_bytes = v1_get16le(sb + inodemap_bytes_off);
    uint32_t inodemap_off = inodemap_bytes_off + 2;

    /* Reject a superblock whose maps spill past blocks 0+1, or a zero map. */
    if (fs->freemap_bytes == 0 ||
        inodemap_off + fs->inodemap_bytes > V1_BSIZE * 2) {
        close(fs->fd);
        fs->fd = -1;
        return -EINVAL;
    }

    fs->fsize = (uint32_t)fs->freemap_bytes * 8;
    fs->maxino = (uint32_t)fs->inodemap_bytes * 8;

    /* Reject a filesystem that claims more disk than the image holds, or one
     * with no data area (i-list must leave at least one data block). */
    struct stat st;
    if (fstat(fs->fd, &st) != 0 ||
        fs->base + (uint64_t)fs->fsize * V1_BSIZE > (uint64_t)st.st_size ||
        fs->fsize <= v1_data_start(fs->maxino)) {
        close(fs->fd);
        fs->fd = -1;
        return -EINVAL;
    }

    fs->freemap = malloc(fs->freemap_bytes);
    fs->inodemap = malloc(fs->inodemap_bytes);
    if (!fs->freemap || !fs->inodemap) {
        free(fs->freemap);
        free(fs->inodemap);
        close(fs->fd);
        fs->fd = -1;
        return -ENOMEM;
    }
    memcpy(fs->freemap, sb + freemap_off, fs->freemap_bytes);
    memcpy(fs->inodemap, sb + inodemap_off, fs->inodemap_bytes);

    /* Seed the free-space totals for statfs. */
    for (uint32_t b = v1_data_start(fs->maxino); b < fs->fsize; b++)
        if (fs->freemap[b >> 3] & (1u << (b & 7)))
            fs->tfree++;
    for (uint32_t i = V1_ROOTINO; i <= fs->maxino; i++) {
        uint32_t bit = i - V1_ROOTINO;
        if (!(fs->inodemap[bit >> 3] & (1u << (bit & 7))))
            fs->tinode++;
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
    free(fs->freemap);
    free(fs->inodemap);
    fs->freemap = NULL;
    fs->inodemap = NULL;
}

int v1fs_sync(v1fs_t *fs) {
    if (fs->readonly)
        return 0;
    return super_write(fs);
}

/* ---- block io ---------------------------------------------------------- */

int v1fs_read_block(v1fs_t *fs, uint32_t bno, uint8_t *buf) {
    ssize_t n = pread(fs->fd, buf, V1_BSIZE, (off_t)bno * V1_BSIZE + (off_t)fs->base);
    if (n != V1_BSIZE)
        return -EIO;
    return 0;
}

int v1fs_write_block(v1fs_t *fs, uint32_t bno, const uint8_t *buf) {
    if (fs->readonly)
        return -EROFS;
    ssize_t n = pwrite(fs->fd, buf, V1_BSIZE, (off_t)bno * V1_BSIZE + (off_t)fs->base);
    if (n != V1_BSIZE)
        return -EIO;
    return 0;
}

/* ---- superblock persistence --------------------------------------------- */

static int super_write(v1fs_t *fs) {
    if (fs->readonly)
        return 0;
    uint8_t sb[V1_BSIZE * 2];
    if (v1fs_read_block(fs, 0, sb) || v1fs_read_block(fs, 1, sb + V1_BSIZE))
        return -EIO;
    uint32_t freemap_off = 2;
    uint32_t inodemap_off = freemap_off + fs->freemap_bytes + 2;
    memcpy(sb + freemap_off, fs->freemap, fs->freemap_bytes);
    memcpy(sb + inodemap_off, fs->inodemap, fs->inodemap_bytes);
    if (v1fs_write_block(fs, 0, sb) || v1fs_write_block(fs, 1, sb + V1_BSIZE))
        return -EIO;
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
    ip->mode  = v1_get16le(d + 0);
    ip->nlink = (int16_t)d[2];        /* char */
    ip->uid   = (int16_t)d[3];        /* char */
    ip->gid   = 0;                    /* V1 has no gid */
    ip->size  = v1_get16le(d + 4);
    for (int i = 0; i < V1_NIADDR; i++)
        ip->addr[i] = v1_get16le(d + 6 + 2 * i);
    /* V1 stores times as a 60 Hz clock tick count (60ths of a second), not
     * whole seconds like V6/V7; convert to seconds for the POSIX-facing layer.
     * The 32-bit counter wraps every ~2.27 years -- an inherent V1 quirk. */
    ip->ctime = v1_get32me(d + 22) / 60;
    ip->mtime = v1_get32me(d + 26) / 60;
    ip->atime = ip->mtime;            /* V1 has no atime */
    return 0;
}

int v1fs_write_inode(v1fs_t *fs, uint32_t ino, const v1_inode_t *ip) {
    if (ino == 0 || ino > fs->maxino)
        return -EINVAL;
    uint32_t bno = v1_itod(ino);
    uint32_t off = v1_itoo(ino);
    uint8_t raw[V1_BSIZE];
    if (v1fs_read_block(fs, bno, raw))
        return -EIO;
    uint8_t *d = raw + off * V1_INODESZ;
    v1_put16le(d + 0, ip->mode);
    d[2] = (uint8_t)ip->nlink;
    d[3] = (uint8_t)ip->uid;
    v1_put16le(d + 4, (uint16_t)ip->size);
    for (int i = 0; i < V1_NIADDR; i++)
        v1_put16le(d + 6 + 2 * i, (uint16_t)ip->addr[i]);
    v1_put32me(d + 22, ip->ctime * 60u);   /* seconds -> 60ths of a second */
    v1_put32me(d + 26, ip->mtime * 60u);
    return v1fs_write_block(fs, bno, raw);
}

/* ---- allocation (bitmap) ------------------------------------------------ */

int v1fs_balloc(v1fs_t *fs, uint32_t *bno) {
    for (uint32_t b = v1_data_start(fs->maxino); b < fs->fsize; b++) {
        if (fs->freemap[b >> 3] & (1u << (b & 7))) {          /* 1 = free */
            fs->freemap[b >> 3] &= (uint8_t)~(1u << (b & 7)); /* mark used */
            if (fs->tfree) fs->tfree--;
            *bno = b;
            /* Zero the freshly-allocated block so a deleted file's data
             * doesn't leak into a new one (V7's alloc() clrbuf()s). */
            uint8_t z[V1_BSIZE] = {0};
            if (v1fs_write_block(fs, b, z))
                return -EIO;
            return 0;
        }
    }
    return -ENOSPC;
}

void v1fs_bfree(v1fs_t *fs, uint32_t bno) {
    if (bno < v1_data_start(fs->maxino) || bno >= fs->fsize)
        return;
    fs->freemap[bno >> 3] |= (uint8_t)(1u << (bno & 7));
    fs->tfree++;
}

int v1fs_ialloc(v1fs_t *fs, uint32_t *ino) {
    for (uint32_t i = V1_ROOTINO; i <= fs->maxino; i++) {
        uint32_t bit = i - V1_ROOTINO;                        /* first bit = inode 41 */
        if (!(fs->inodemap[bit >> 3] & (1u << (bit & 7)))) {  /* 0 = free */
            fs->inodemap[bit >> 3] |= (uint8_t)(1u << (bit & 7));
            if (fs->tinode) fs->tinode--;
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
    fs->inodemap[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
    fs->tinode++;
}

/* ---- truncate ----------------------------------------------------------- */

static void tloop(v1fs_t *fs, uint32_t blk, int level) {
    uint8_t buf[V1_BSIZE];
    if (v1fs_read_block(fs, blk, buf))
        return;
    for (int i = V1_NINDIR - 1; i >= 0; i--) {
        uint32_t nb = v1_get16le(buf + 2 * i);
        if (nb == 0)
            continue;
        if (level > 0)
            tloop(fs, nb, level - 1);
        else
            v1fs_bfree(fs, nb);
    }
    v1fs_bfree(fs, blk);
}

int v1fs_itrunc(v1fs_t *fs, v1_inode_t *ip) {
    if (ip->mode & V1_ILARG) {
        for (int i = 0; i < V1_NIADDR; i++) {   /* 8 single-indirect slots */
            uint32_t bn = ip->addr[i];
            if (bn) { ip->addr[i] = 0; tloop(fs, bn, 0); }
        }
    } else {
        for (int i = 0; i < V1_NIADDR; i++) {   /* 8 direct blocks */
            uint32_t bn = ip->addr[i];
            if (bn) { ip->addr[i] = 0; v1fs_bfree(fs, bn); }
        }
    }
    ip->size = 0;
    return 0;
}

/* Free leaf blocks [skip, ...) of a single-indirect subtree. */
static void tloop_from(v1fs_t *fs, uint32_t blk, int level, uint32_t skip) {
    uint8_t buf[V1_BSIZE];
    if (v1fs_read_block(fs, blk, buf))
        return;
    uint32_t sub = 1;
    for (int l = 0; l < level; l++) sub *= V1_NINDIR;
    uint32_t se = skip / sub;
    uint32_t sp = skip % sub;
    for (int i = V1_NINDIR - 1; i >= 0; i--) {
        uint32_t nb = v1_get16le(buf + 2 * i);
        if (nb == 0)
            continue;
        if ((uint32_t)i < se)
            continue;
        if ((uint32_t)i == se && sp > 0) {
            tloop_from(fs, nb, level - 1, sp);
        } else {
            if (level == 0) v1fs_bfree(fs, nb);
            else tloop(fs, nb, level - 1);
            v1_put16le(buf + 2 * i, 0);
        }
    }
    v1fs_write_block(fs, blk, buf);
}

/* Free blocks [first_blk, ...); first_blk == 0 == v1fs_itrunc. */
int v1fs_itrunc_from(v1fs_t *fs, v1_inode_t *ip, uint32_t first_blk) {
    if (ip->mode & V1_ILARG) {
        uint32_t slot = first_blk / V1_NINDIR;
        uint32_t within = first_blk % V1_NINDIR;
        for (int i = V1_NIADDR - 1; i > (int)slot; i--) {
            if (ip->addr[i]) { tloop(fs, ip->addr[i], 0); ip->addr[i] = 0; }
        }
        if (ip->addr[slot]) {
            if (within == 0) { tloop(fs, ip->addr[slot], 0); ip->addr[slot] = 0; }
            else tloop_from(fs, ip->addr[slot], 0, within);
        }
    } else {
        for (int i = V1_NIADDR - 1; i >= (int)first_blk; i--) {
            if (ip->addr[i]) { v1fs_bfree(fs, ip->addr[i]); ip->addr[i] = 0; }
        }
    }
    return 0;
}

/* ---- block mapping ------------------------------------------------------ */

/* Single indirect: *slot -> block, entry `idx` (2-byte entries). */
static int ind1(v1fs_t *fs, uint32_t *slot, uint32_t idx, int create, uint32_t *out) {
    uint32_t blk = *slot;
    if (blk == 0) {
        if (!create) { *out = 0; return 0; }
        uint8_t z[V1_BSIZE];
        memset(z, 0, V1_BSIZE);
        if (v1fs_balloc(fs, &blk) || v1fs_write_block(fs, blk, z))
            return -ENOSPC;
        *slot = blk;
    }
    uint8_t buf[V1_BSIZE];
    if (v1fs_read_block(fs, blk, buf))
        return -EIO;
    uint32_t nb = v1_get16le(buf + 2 * idx);
    if (nb == 0 && create) {
        if (v1fs_balloc(fs, &nb))
            return -ENOSPC;
        v1_put16le(buf + 2 * idx, (uint16_t)nb);
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
        if (v1fs_balloc(fs, &iblk))
            return -ENOSPC;
        uint8_t buf[V1_BSIZE] = {0};
        for (int i = 0; i < V1_NDADDR; i++)
            v1_put16le(buf + 2 * i, (uint16_t)ip->addr[i]);
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
        if (v1fs_balloc(fs, &nb))
            return -ENOSPC;
        ip->addr[lbn] = nb;
    }
    *bno = nb;
    return 0;
}

/* ---- file data ---------------------------------------------------------- */

ssize_t v1fs_file_read(v1fs_t *fs, v1_inode_t *ip, uint8_t *buf, size_t size, off_t off) {
    if (off < 0)
        return -EINVAL;
    if ((uint64_t)off >= ip->size)
        return 0;
    uint64_t remaining = ip->size - (uint64_t)off;
    if (size > remaining)
        size = (size_t)remaining;

    size_t done = 0;
    while (done < size) {
        uint32_t lbn  = (uint32_t)((off + (off_t)done) / V1_BSIZE);
        uint32_t boff = (uint32_t)((off + (off_t)done) % V1_BSIZE);
        uint32_t pbn;
        if (v1fs_bmap(fs, ip, lbn, 0, &pbn))
            return -EIO;
        uint8_t blk[V1_BSIZE];
        if (pbn == 0) {
            memset(blk, 0, V1_BSIZE);
        } else if (v1fs_read_block(fs, pbn, blk)) {
            return -EIO;
        }
        size_t n = V1_BSIZE - boff;
        if (n > size - done)
            n = size - done;
        memcpy(buf + done, blk + boff, n);
        done += n;
    }
    return (ssize_t)done;
}

ssize_t v1fs_file_write(v1fs_t *fs, v1_inode_t *ip, const uint8_t *buf, size_t size, off_t off) {
    if (off < 0)
        return -EINVAL;

    size_t done = 0;
    while (done < size) {
        uint32_t lbn  = (uint32_t)((off + (off_t)done) / V1_BSIZE);
        uint32_t boff = (uint32_t)((off + (off_t)done) % V1_BSIZE);
        uint32_t pbn;
        if (v1fs_bmap(fs, ip, lbn, 1, &pbn))
            return -ENOSPC;
        if (pbn == 0)
            return -ENOSPC;

        uint8_t blk[V1_BSIZE];
        if (v1fs_read_block(fs, pbn, blk))
            return -EIO;

        size_t n = V1_BSIZE - boff;
        if (n > size - done)
            n = size - done;
        memcpy(blk + boff, buf + done, n);
        if (v1fs_write_block(fs, pbn, blk))
            return -EIO;
        done += n;
    }
    if ((uint64_t)off + size > ip->size)
        ip->size = (uint32_t)(off + size);
    v1fs_write_inode(fs, ip->ino, ip);
    return (ssize_t)done;
}

/* ---- directories -------------------------------------------------------- */

int v1fs_dir_read(v1fs_t *fs, v1_inode_t *ip, v1_dirent_t **ents, size_t *count) {
    if (!(ip->mode & V1_IFDIR))
        return -ENOTDIR;
    if (ip->size > (uint64_t)(fs->fsize - v1_data_start(fs->maxino)) * V1_BSIZE)
        return -EFBIG;
    size_t cap = ip->size / V1_DIRENTSZ + 1;
    v1_dirent_t *out = calloc(cap, sizeof(v1_dirent_t));
    if (!out)
        return -ENOMEM;

    uint8_t *buf = malloc(ip->size);
    if (!buf) {
        free(out);
        return -ENOMEM;
    }
    ssize_t n = v1fs_file_read(fs, ip, buf, ip->size, 0);
    if (n < 0) {
        free(buf);
        free(out);
        return (int)n;
    }

    size_t cnt = 0;
    for (size_t off = 0; off + V1_DIRENTSZ <= (size_t)n; off += V1_DIRENTSZ) {
        uint16_t ino = v1_get16le(buf + off);
        if (ino == 0)
            continue;
        out[cnt].ino = ino;
        memcpy(out[cnt].name, buf + off + 2, V1_DIRSIZ);
        out[cnt].name[V1_DIRSIZ] = 0;
        cnt++;
    }
    free(buf);
    *ents = out;
    *count = cnt;
    return 0;
}

void v1fs_dirents_free(v1_dirent_t *ents) {
    free(ents);
}

int v1fs_dir_lookup(v1fs_t *fs, v1_inode_t *ip, const char *name, uint32_t *ino) {
    v1_dirent_t *ents = NULL;
    size_t count = 0;
    int rc = v1fs_dir_read(fs, ip, &ents, &count);
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
    v1fs_dirents_free(ents);
    return rc;
}

int v1fs_dir_add(v1fs_t *fs, v1_inode_t *ip, uint32_t ino, const char *name) {
    size_t namelen = strlen(name);
    if (namelen == 0 || namelen > V1_DIRSIZ)
        return -ENAMETOOLONG;
    if (strchr(name, '/'))
        return -EINVAL;

    size_t newsize = ip->size + V1_DIRENTSZ;
    uint8_t *buf = malloc(newsize);
    if (!buf)
        return -ENOMEM;
    memset(buf, 0, newsize);
    ssize_t n = v1fs_file_read(fs, ip, buf, ip->size, 0);
    if (n < 0) {
        free(buf);
        return (int)n;
    }

    size_t slot = SIZE_MAX;
    for (size_t off = 0; off + V1_DIRENTSZ <= (size_t)n; off += V1_DIRENTSZ) {
        if (v1_get16le(buf + off) == 0) {
            slot = off;
            break;
        }
    }
    if (slot == SIZE_MAX) {
        slot = (size_t)n;
        n += V1_DIRENTSZ;
    }

    v1_put16le(buf + slot, (uint16_t)ino);
    memset(buf + slot + 2, 0, V1_DIRSIZ);
    memcpy(buf + slot + 2, name, namelen);

    ssize_t w = v1fs_file_write(fs, ip, buf, (size_t)n, 0);
    free(buf);
    return w < 0 ? (int)w : 0;
}

int v1fs_dir_remove(v1fs_t *fs, v1_inode_t *ip, const char *name) {
    uint8_t *buf = malloc(ip->size);
    if (!buf)
        return -ENOMEM;
    ssize_t n = v1fs_file_read(fs, ip, buf, ip->size, 0);
    if (n < 0) {
        free(buf);
        return (int)n;
    }
    int rc = -ENOENT;
    for (size_t off = 0; off + V1_DIRENTSZ <= (size_t)n; off += V1_DIRENTSZ) {
        if (v1_get16le(buf + off) == 0)
            continue;
        char ent[V1_DIRSIZ + 1];
        memcpy(ent, buf + off + 2, V1_DIRSIZ);
        ent[V1_DIRSIZ] = 0;
        if (strcmp(ent, name) == 0) {
            v1_put16le(buf + off, 0);
            memset(buf + off + 2, 0, V1_DIRSIZ);
            ssize_t w = v1fs_file_write(fs, ip, buf, (size_t)n, 0);
            rc = w < 0 ? (int)w : 0;
            break;
        }
    }
    free(buf);
    return rc;
}

/* ---- path lookup -------------------------------------------------------- */

int v1fs_lookup(v1fs_t *fs, const char *path, uint32_t *ino, v1_inode_t *ip) {
    if (path[0] != '/')
        return -EINVAL;
    uint32_t cur = V1_ROOTINO;
    v1_inode_t dip;
    if (v1fs_read_inode(fs, cur, &dip))
        return -EIO;

    const char *p = path + 1;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (len == 0) {
            p++;
            continue;
        }
        if (len > V1_DIRSIZ)
            return -ENAMETOOLONG;
        char name[V1_DIRSIZ + 1];
        memcpy(name, p, len);
        name[len] = 0;

        if (!(dip.mode & V1_IFDIR))
            return -ENOTDIR;
        uint32_t next;
        int rc = v1fs_dir_lookup(fs, &dip, name, &next);
        if (rc)
            return rc;
        if (v1fs_read_inode(fs, next, &dip))
            return -EIO;
        p = slash ? slash + 1 : p + len;
    }
    *ino = dip.ino;
    if (ip)
        *ip = dip;
    return 0;
}

/* ---- integrity check ---------------------------------------------------- */

/* ---- check / salvage / resolve-dups -------------------------------------- */

typedef struct {
    v1fs_t   *fs;
    uint8_t  *bmap;        /* bit i = data block (dstart + i) */
    uint32_t  nblk;
    uint32_t  used_blocks;
    uint32_t  dup_blocks;
    uint32_t  errors;
    uint32_t  ino;
} v1_chkctx_t;

static int v1_mark_block(v1_chkctx_t *cx, uint32_t bno)
{
    if (bno == 0)
        return 0;
    uint32_t dstart = v1_data_start(cx->fs->maxino);
    if (bno < dstart || bno >= cx->fs->fsize) {
        printf("block %u bad; inode=%u\n", bno, cx->ino);
        cx->errors++;
        return 1;
    }
    uint32_t d = bno - dstart;
    uint8_t  m = (uint8_t)(1u << (d & 7));
    if (cx->bmap[d >> 3] & m) {
        printf("block %u dup; inode=%u\n", bno, cx->ino);
        cx->dup_blocks++;
        cx->errors++;
        return 0;
    }
    cx->bmap[d >> 3] |= m;
    cx->used_blocks++;
    return 0;
}

/* V1 large file: all 8 slots are single-indirect (256 16-bit pointers). */
static void v1_mark_tree(v1_chkctx_t *cx, uint32_t blk)
{
    if (blk == 0)
        return;
    if (v1_mark_block(cx, blk))
        return;
    uint8_t buf[V1_BSIZE];
    if (v1fs_read_block(cx->fs, blk, buf)) {
        printf("cannot read indirect block %u\n", blk);
        cx->errors++;
        return;
    }
    for (int i = 0; i < V1_NINDIR; i++) {
        uint32_t nb = v1_get16le(buf + 2 * i);
        if (nb != 0)
            v1_mark_block(cx, nb);
    }
}

/* Rebuild the free-block map from the usage bitmap (icheck -s).  Only the
 * data area can be free; the superblock, i-list and device slots stay used. */
static void v1fs_makefree(v1fs_t *fs, v1_chkctx_t *cx)
{
    uint32_t dstart = v1_data_start(fs->maxino);
    uint32_t nfree = 0;
    memset(fs->freemap, 0, fs->freemap_bytes);
    for (uint32_t b = dstart; b < fs->fsize; b++) {
        uint32_t off = b - dstart;
        if (!(cx->bmap[off >> 3] & (uint8_t)(1u << (off & 7)))) {
            fs->freemap[b >> 3] |= (uint8_t)(1u << (b & 7));
            nfree++;
        }
    }
    fs->tfree = nfree;
    super_write(fs);
}

int v1fs_check(v1fs_t *fs, v1_check_t *rep, int salvage) {
    memset(rep, 0, sizeof(*rep));
    rep->inodes = fs->maxino;

    if (fs->fsize == 0 || v1_data_start(fs->maxino) >= fs->fsize) {
        printf("bad superblock: fsize=%u maxino=%u\n", fs->fsize, fs->maxino);
        rep->errors++;
    }

    uint32_t dstart = v1_data_start(fs->maxino);
    uint32_t nblk = fs->fsize - dstart;

    v1_chkctx_t cx;
    memset(&cx, 0, sizeof(cx));
    cx.fs = fs;
    cx.nblk = nblk;
    cx.bmap = calloc((nblk + 7) / 8, 1);
    if (!cx.bmap)
        return -ENOMEM;

    /* icheck pass 1: mark every block referenced by an inode. */
    for (uint32_t ino = 1; ino <= fs->maxino; ino++) {
        v1_inode_t ip;
        if (v1fs_read_inode(fs, ino, &ip)) {
            printf("inode %u unreadable\n", ino);
            rep->errors++;
            continue;
        }
        if (ip.mode == 0)
            continue;
        rep->used_inodes++;
        if (ino < V1_ROOTINO)       /* reserved device inode: addr is a device no. */
            continue;
        cx.ino = ino;
        for (int i = 0; i < V1_NIADDR; i++) {
            uint32_t a = ip.addr[i];
            if (a == 0)
                continue;
            if (ip.mode & V1_ILARG)
                v1_mark_tree(&cx, a);
            else
                v1_mark_block(&cx, a);
        }
    }
    rep->errors += cx.errors;
    rep->dup_blocks = cx.dup_blocks;

    if (salvage) {
        v1fs_makefree(fs, &cx);
        printf("salvaged: free map rebuilt (%u free blocks)\n", fs->tfree);
        free(cx.bmap);
        return rep->errors ? -1 : 0;
    }

    /* Compare the free map against the usage map: blocks both used and free,
     * and blocks neither used nor free ("missing"). */
    for (uint32_t b = dstart; b < fs->fsize; b++) {
        uint32_t off = b - dstart;
        int used = cx.bmap[off >> 3] & (uint8_t)(1u << (off & 7));
        int fre  = fs->freemap[b >> 3] & (uint8_t)(1u << (b & 7));
        if (used && fre) {
            printf("block %u used and free\n", b);
            rep->errors++;
        } else if (!used && !fre) {
            printf("block %u missing\n", b);
            rep->missing_blocks++;
            rep->errors++;
        }
        if (fre)
            rep->free_blocks++;
    }

    free(cx.bmap);
    printf("used blocks=%u  free blocks=%u  missing=%u  dup=%u  inodes=%u/%u used  errors=%u\n",
           cx.used_blocks, rep->free_blocks, rep->missing_blocks, rep->dup_blocks,
           rep->used_inodes, rep->inodes, rep->errors);
    return rep->errors ? -1 : 0;
}

int v1fs_resolve_dups(v1fs_t *fs)
{
    uint32_t dstart = v1_data_start(fs->maxino);
    uint32_t nblk = fs->fsize - dstart;

    v1_chkctx_t cx;
    memset(&cx, 0, sizeof(cx));
    cx.fs = fs;
    cx.nblk = nblk;
    cx.bmap = calloc((nblk + 7) / 8, 1);
    if (!cx.bmap)
        return -ENOMEM;

    struct dup { uint32_t blk, ino, idx; };
    struct dup *dups = NULL;
    size_t ndup = 0, cap = 0;

    for (uint32_t ino = 1; ino <= fs->maxino; ino++) {
        v1_inode_t ip;
        if (v1fs_read_inode(fs, ino, &ip))
            continue;
        if (ip.mode == 0)
            continue;
        if (ino < V1_ROOTINO)
            continue;
        cx.ino = ino;
        for (int i = 0; i < V1_NIADDR; i++) {
            uint32_t a = ip.addr[i];
            if (a == 0)
                continue;
            if (ip.mode & V1_ILARG) {
                v1_mark_tree(&cx, a);
                continue;
            }
            if (a < dstart || a >= fs->fsize) {
                printf("block %u bad; inode=%u\n", a, ino);
                continue;
            }
            uint32_t off = a - dstart;
            uint8_t  m = (uint8_t)(1u << (off & 7));
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

    printf("%zu duplicate block(s); rebuilding free map\n", ndup);
    v1fs_makefree(fs, &cx);

    int resolved = 0;
    for (size_t k = 0; k < ndup; k++) {
        uint32_t blk = dups[k].blk, ino = dups[k].ino, idx = dups[k].idx;
        v1_inode_t ip;
        if (v1fs_read_inode(fs, ino, &ip))
            continue;
        if (ip.addr[idx] != blk)
            continue;
        uint32_t nb;
        if (v1fs_balloc(fs, &nb)) {
            printf("block %u dup; inode=%u: out of space\n", blk, ino);
            continue;
        }
        uint8_t buf[V1_BSIZE];
        if (v1fs_read_block(fs, blk, buf) || v1fs_write_block(fs, nb, buf)) {
            printf("block %u dup; inode=%u: copy failed\n", blk, ino);
            continue;
        }
        ip.addr[idx] = nb;
        v1fs_write_inode(fs, ino, &ip);
        uint32_t off = nb - dstart;
        cx.bmap[off >> 3] |= (uint8_t)(1u << (off & 7));
        printf("block %u dup; inode=%u: copied to %u\n", blk, ino, nb);
        resolved++;
    }
    free(dups);

    printf("resolved %d/%zu duplicates; finalizing free map\n", resolved, ndup);
    v1fs_makefree(fs, &cx);
    free(cx.bmap);
    return resolved == (int)ndup ? 0 : -1;
}

/* ---- maintenance: ncheck / clri ---------------------------------------- */

static void v1_ncheck_dir(v1fs_t *fs, uint32_t dirino, const char *prefix,
                          uint32_t target, int *found, int depth)
{
    if (depth > 64)
        return;
    v1_inode_t ip;
    if (v1fs_read_inode(fs, dirino, &ip))
        return;
    if (!(ip.mode & V1_IFDIR))
        return;
    v1_dirent_t *ents = NULL;
    size_t cnt = 0;
    if (v1fs_dir_read(fs, &ip, &ents, &cnt))
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
        v1_inode_t cip;
        if (v1fs_read_inode(fs, eino, &cip) == 0 && (cip.mode & V1_IFDIR))
            v1_ncheck_dir(fs, eino, path, target, found, depth + 1);
    }
    v1fs_dirents_free(ents);
}

int v1fs_ncheck(v1fs_t *fs, uint32_t ino)
{
    int found = 0;
    v1_ncheck_dir(fs, V1_ROOTINO, "/", ino, &found, 0);
    if (!found)
        printf("%u: not found\n", ino);
    return 0;
}

int v1fs_clri(v1fs_t *fs, uint32_t ino)
{
    if (ino == 0 || ino > fs->maxino)
        return -EINVAL;
    v1_inode_t ip;
    memset(&ip, 0, sizeof(ip));
    ip.ino = ino;
    int rc = v1fs_write_inode(fs, ino, &ip);
    if (rc == 0)
        printf("cleared inode %u\n", ino);
    return rc;
}

/* ---- ops table ----------------------------------------------------------
 * Each op takes `void *` (the backend state).  The adapters forward to the
 * typed backend function; the `void *` argument converts implicitly to
 * v1fs_t*, so there is no cast anywhere. */

static int v1fs_open_op(void *fs, const char *path, int readonly, int le,
                        uint64_t offset) {
    (void)le;   /* V1 has no little-endian variant */
    return v1fs_open(fs, path, readonly, offset);
}
static void v1fs_close_op(void *fs) { v1fs_close(fs); }
static int v1fs_sync_op(void *fs) { return v1fs_sync(fs); }
static int v1fs_read_block_op(void *fs, uint32_t bno, uint8_t *buf)
{ return v1fs_read_block(fs, bno, buf); }
static int v1fs_write_block_op(void *fs, uint32_t bno, const uint8_t *buf)
{ return v1fs_write_block(fs, bno, buf); }
static int v1fs_read_inode_op(void *fs, uint32_t ino, filsys_inode_t *ip)
{ return v1fs_read_inode(fs, ino, ip); }
static int v1fs_write_inode_op(void *fs, uint32_t ino, const filsys_inode_t *ip)
{ return v1fs_write_inode(fs, ino, ip); }
static int v1fs_ialloc_op(void *fs, uint32_t *ino)
{ return v1fs_ialloc(fs, ino); }
static void v1fs_ifree_op(void *fs, uint32_t ino) { v1fs_ifree(fs, ino); }
static int v1fs_bmap_op(void *fs, filsys_inode_t *ip, uint32_t lbn, int create, uint32_t *bno)
{ return v1fs_bmap(fs, ip, lbn, create, bno); }
static int v1fs_itrunc_op(void *fs, filsys_inode_t *ip)
{ return v1fs_itrunc(fs, ip); }
static int v1fs_itrunc_from_op(void *fs, filsys_inode_t *ip, uint32_t first_blk)
{ return v1fs_itrunc_from(fs, ip, first_blk); }
static ssize_t v1fs_file_read_op(void *fs, filsys_inode_t *ip, uint8_t *buf, size_t size, off_t off)
{ return v1fs_file_read(fs, ip, buf, size, off); }
static ssize_t v1fs_file_write_op(void *fs, filsys_inode_t *ip, const uint8_t *buf, size_t size, off_t off)
{ return v1fs_file_write(fs, ip, buf, size, off); }
static int v1fs_dir_read_op(void *fs, filsys_inode_t *ip, filsys_dirent_t **ents, size_t *count)
{ return v1fs_dir_read(fs, ip, ents, count); }
static int v1fs_dir_lookup_op(void *fs, filsys_inode_t *ip, const char *name, uint32_t *ino)
{ return v1fs_dir_lookup(fs, ip, name, ino); }
static int v1fs_dir_add_op(void *fs, filsys_inode_t *ip, uint32_t ino, const char *name)
{ return v1fs_dir_add(fs, ip, ino, name); }
static int v1fs_dir_remove_op(void *fs, filsys_inode_t *ip, const char *name)
{ return v1fs_dir_remove(fs, ip, name); }
static int v1fs_lookup_op(void *fs, const char *path, uint32_t *ino, filsys_inode_t *ip)
{ return v1fs_lookup(fs, path, ino, ip); }
static int v1fs_check_op(void *fs) { v1_check_t rep; return v1fs_check(fs, &rep, 0); }
static uint64_t v1fs_max_file_op(void *fs) {
    (void)fs;
    /* The large-file flag can address a megabyte of blocks, but the 16-bit
     * size field caps a file at 65535 bytes (past that it wraps to zero). */
    return (1u << 16) - 1;
}

const struct filsys_ops v1fs_ops = {
    .name        = "v1",
    .blocksize   = V1_BSIZE,
    .open        = v1fs_open_op,
    .close       = v1fs_close_op,
    .sync        = v1fs_sync_op,
    .read_block  = v1fs_read_block_op,
    .write_block = v1fs_write_block_op,
    .read_inode  = v1fs_read_inode_op,
    .write_inode = v1fs_write_inode_op,
    .ialloc      = v1fs_ialloc_op,
    .ifree       = v1fs_ifree_op,
    .bmap        = v1fs_bmap_op,
    .itrunc      = v1fs_itrunc_op,
    .itrunc_from = v1fs_itrunc_from_op,
    .file_read   = v1fs_file_read_op,
    .file_write  = v1fs_file_write_op,
    .dir_read    = v1fs_dir_read_op,
    .dir_lookup  = v1fs_dir_lookup_op,
    .dir_add     = v1fs_dir_add_op,
    .dir_remove  = v1fs_dir_remove_op,
    .lookup      = v1fs_lookup_op,
    .check       = v1fs_check_op,
    .max_file    = v1fs_max_file_op,
};
