/* bsd211fs.c - 2.11BSD filesystem, on-disk access layer.
 *
 * 2.11BSD keeps V7's free-list superblock but uses the "new" BSD inode (32-bit
 * block addresses, 4 direct + 3 indirect, a di_flags field, symlinks) and
 * variable-length directory entries (up to 63-char names).  See bsd211fs.h and
 * docs/2bsd-format.md.
 *
 * SPDX-License-Identifier: ISC
 */
#include <config.h>
#include "bsd211fs.h"
#include "filsys_ops.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

#define B(x) (bsd211_get16le((uint8_t *)(x)))
#define B32(x) (bsd211_get32me((uint8_t *)(x)))

/* ---- superblock persistence --------------------------------------------- */

static void super_read(bsd211fs_t *fs, const uint8_t *sb) {
    fs->isize  = bsd211_get16le(sb + BSD211_SB_ISIZE);
    fs->fsize  = bsd211_get32me(sb + BSD211_SB_FSIZE);
    fs->nfree  = bsd211_get16le(sb + BSD211_SB_NFREE);
    for (int i = 0; i < BSD211_NICFREE; i++)
        fs->free[i] = bsd211_get32me(sb + BSD211_SB_FREE + 4 * i);
    fs->ninode = bsd211_get16le(sb + BSD211_SB_NINODE);
    for (int i = 0; i < BSD211_NICINOD; i++)
        fs->inode[i] = bsd211_get16le(sb + BSD211_SB_INODE + 2 * i);
    fs->time   = bsd211_get32me(sb + BSD211_SB_TIME);
    fs->tfree  = bsd211_get32me(sb + BSD211_SB_TFREE);
    fs->tinode = bsd211_get16le(sb + BSD211_SB_TINODE);
    fs->fmod   = sb[BSD211_SB_FMOD];
}

static int super_write(bsd211fs_t *fs) {
    if (fs->readonly)
        return 0;
    uint8_t sb[BSD211_BSIZE];
    if (bsd211fs_read_block(fs, BSD211_SUPERB, sb))
        return -EIO;
    bsd211_put16le(sb + BSD211_SB_ISIZE, fs->isize);
    bsd211_put32me(sb + BSD211_SB_FSIZE, fs->fsize);
    bsd211_put16le(sb + BSD211_SB_NFREE, fs->nfree);
    for (int i = 0; i < BSD211_NICFREE; i++)
        bsd211_put32me(sb + BSD211_SB_FREE + 4 * i, fs->free[i]);
    bsd211_put16le(sb + BSD211_SB_NINODE, fs->ninode);
    for (int i = 0; i < BSD211_NICINOD; i++)
        bsd211_put16le(sb + BSD211_SB_INODE + 2 * i, fs->inode[i]);
    bsd211_put32me(sb + BSD211_SB_TIME, (uint32_t)time(NULL));
    bsd211_put32me(sb + BSD211_SB_TFREE, fs->tfree);
    bsd211_put16le(sb + BSD211_SB_TINODE, fs->tinode);
    sb[BSD211_SB_FMOD] = (uint8_t)(fs->fmod != 0);
    return bsd211fs_write_block(fs, BSD211_SUPERB, sb);
}

/* ---- lifecycle ----------------------------------------------------------- */

int bsd211fs_open(bsd211fs_t *fs, const char *path, int readonly, uint64_t offset) {
    memset(fs, 0, sizeof(*fs));
    fs->readonly = readonly;
    fs->base = offset;
    fs->fd = open(path, readonly ? O_RDONLY : O_RDWR);
    if (fs->fd < 0)
        return -errno;

    uint8_t sb[BSD211_BSIZE];
    if (bsd211fs_read_block(fs, BSD211_SUPERB, sb)) {
        close(fs->fd);
        fs->fd = -1;
        return -EIO;
    }
    super_read(fs, sb);

    struct stat st;
    if (fstat(fs->fd, &st) != 0 ||
        fs->isize < 2 ||
        fs->base + (uint64_t)fs->fsize * BSD211_BSIZE > (uint64_t)st.st_size ||
        fs->fsize <= fs->isize) {
        close(fs->fd);
        fs->fd = -1;
        return -EINVAL;
    }
    return 0;
}

void bsd211fs_close(bsd211fs_t *fs) {
    if (fs->fd >= 0) {
        if (!fs->readonly) {
            fs->fmod = 0;
            super_write(fs);
        }
        close(fs->fd);
        fs->fd = -1;
    }
}

int bsd211fs_sync(bsd211fs_t *fs) {
    if (fs->readonly)
        return 0;
    return super_write(fs);
}

int bsd211fs_mark_dirty(bsd211fs_t *fs) {
    if (fs->readonly)
        return 0;
    fs->fmod = 1;
    return super_write(fs);
}

/* ---- block / inode io ---------------------------------------------------- */

int bsd211fs_read_block(bsd211fs_t *fs, uint32_t bno, uint8_t *buf) {
    ssize_t n = pread(fs->fd, buf, BSD211_BSIZE, (off_t)bno * BSD211_BSIZE + (off_t)fs->base);
    if (n != BSD211_BSIZE)
        return -EIO;
    return 0;
}

int bsd211fs_write_block(bsd211fs_t *fs, uint32_t bno, const uint8_t *buf) {
    if (fs->readonly)
        return -EROFS;
    ssize_t n = pwrite(fs->fd, buf, BSD211_BSIZE, (off_t)bno * BSD211_BSIZE + (off_t)fs->base);
    if (n != BSD211_BSIZE)
        return -EIO;
    return 0;
}

int bsd211fs_read_inode(bsd211fs_t *fs, uint32_t ino, bsd211_inode_t *ip) {
    if (ino == 0)
        return -EINVAL;
    uint32_t bno = bsd211_itod(ino);
    uint32_t off = bsd211_itoo(ino);
    if (bno >= fs->isize)
        return -EINVAL;
    uint8_t raw[BSD211_BSIZE];
    if (bsd211fs_read_block(fs, bno, raw))
        return -EIO;
    const uint8_t *d = raw + off * BSD211_INODESZ;
    memset(ip, 0, sizeof(*ip));
    ip->ino   = ino;
    ip->mode  = bsd211_get16le(d + 0);
    ip->nlink = (int16_t)bsd211_get16le(d + 2);
    ip->uid   = (int16_t)bsd211_get16le(d + 4);
    ip->gid   = (int16_t)bsd211_get16le(d + 6);
    ip->size  = bsd211_get32me(d + 8);
    for (int i = 0; i < BSD211_NIADDR; i++)
        ip->addr[i] = bsd211_get32me(d + 12 + 4 * i);
    ip->atime = bsd211_get32me(d + 52);
    ip->mtime = bsd211_get32me(d + 56);
    ip->ctime = bsd211_get32me(d + 60);
    return 0;
}

int bsd211fs_write_inode(bsd211fs_t *fs, uint32_t ino, const bsd211_inode_t *ip) {
    if (ino == 0)
        return -EINVAL;
    uint32_t bno = bsd211_itod(ino);
    uint32_t off = bsd211_itoo(ino);
    if (bno >= fs->isize)
        return -EINVAL;
    uint8_t raw[BSD211_BSIZE];
    if (bsd211fs_read_block(fs, bno, raw))
        return -EIO;
    uint8_t *d = raw + off * BSD211_INODESZ;
    bsd211_put16le(d + 0, ip->mode);
    bsd211_put16le(d + 2, (uint16_t)ip->nlink);
    bsd211_put16le(d + 4, (uint16_t)ip->uid);
    bsd211_put16le(d + 6, (uint16_t)ip->gid);
    bsd211_put32me(d + 8, ip->size);
    for (int i = 0; i < BSD211_NIADDR; i++)
        bsd211_put32me(d + 12 + 4 * i, ip->addr[i]);
    bsd211_put32me(d + 52, ip->atime);
    bsd211_put32me(d + 56, ip->mtime);
    bsd211_put32me(d + 60, ip->ctime);
    /* di_flags (offset 50) is left untouched by the read-modify-write. */
    return bsd211fs_write_block(fs, bno, raw);
}

/* ---- allocation ---------------------------------------------------------- */

int bsd211fs_balloc(bsd211fs_t *fs, uint32_t *bno) {
    if (fs->nfree == 0)
        return -ENOSPC;
    uint32_t blk = fs->free[--fs->nfree];
    if (blk == 0)
        return -ENOSPC;
    if (blk < fs->isize || blk >= fs->fsize)
        return -EIO;
    if (fs->nfree == 0) {
        uint8_t buf[BSD211_BSIZE];
        if (bsd211fs_read_block(fs, blk, buf))
            return -EIO;
        fs->nfree = bsd211_get16le(buf + 0);
        for (int i = 0; i < BSD211_NICFREE; i++)
            fs->free[i] = bsd211_get32me(buf + 2 + 4 * i);
    }
    uint8_t z[BSD211_BSIZE];
    memset(z, 0, sizeof(z));
    if (bsd211fs_write_block(fs, blk, z))
        return -EIO;
    if (fs->tfree) fs->tfree--;
    *bno = blk;
    return 0;
}

void bsd211fs_bfree(bsd211fs_t *fs, uint32_t bno) {
    if (bno < fs->isize || bno >= fs->fsize)
        return;
    if (fs->nfree >= BSD211_NICFREE) {
        uint8_t buf[BSD211_BSIZE];
        memset(buf, 0, sizeof(buf));
        bsd211_put16le(buf + 0, fs->nfree);
        for (int i = 0; i < BSD211_NICFREE; i++)
            bsd211_put32me(buf + 2 + 4 * i, fs->free[i]);
        if (bsd211fs_write_block(fs, bno, buf) == 0)
            fs->nfree = 0;
    }
    fs->free[fs->nfree++] = bno;
    fs->tfree++;
}

int bsd211fs_ialloc(bsd211fs_t *fs, uint32_t *ino) {
    uint32_t maxino = (uint32_t)(fs->isize - 2) * BSD211_INOPB;
    for (;;) {
        if (fs->ninode > 0) {
            uint32_t cand = fs->inode[--fs->ninode];
            bsd211_inode_t ip;
            if (cand >= 2 && cand <= maxino &&
                bsd211fs_read_inode(fs, cand, &ip) == 0 && ip.mode == 0) {
                memset(&ip, 0, sizeof(ip));
                ip.ino = cand;
                bsd211fs_write_inode(fs, cand, &ip);
                if (fs->tinode) fs->tinode--;
                *ino = cand;
                return 0;
            }
            continue;
        }
        fs->ninode = 0;
        for (uint32_t in = 2; in <= maxino && fs->ninode < BSD211_NICINOD; in++) {
            bsd211_inode_t ip;
            if (bsd211fs_read_inode(fs, in, &ip))
                break;
            if (ip.mode == 0)
                fs->inode[fs->ninode++] = (uint16_t)in;
        }
        if (fs->ninode == 0)
            return -ENOSPC;
    }
}

void bsd211fs_ifree(bsd211fs_t *fs, uint32_t ino) {
    if (fs->ninode >= BSD211_NICINOD)
        return;
    fs->inode[fs->ninode++] = (uint16_t)ino;
    fs->tinode++;
}

/* ---- truncate / block mapping ------------------------------------------- */

static void tloop(bsd211fs_t *fs, uint32_t blk, int level) {
    uint8_t buf[BSD211_BSIZE];
    if (bsd211fs_read_block(fs, blk, buf))
        return;
    for (int i = BSD211_NINDIR - 1; i >= 0; i--) {
        uint32_t nb = bsd211_get32me(buf + 4 * i);
        if (nb == 0)
            continue;
        if (level > 0)
            tloop(fs, nb, level - 1);
        else
            bsd211fs_bfree(fs, nb);
    }
    bsd211fs_bfree(fs, blk);
}

int bsd211fs_itrunc(bsd211fs_t *fs, bsd211_inode_t *ip) {
    int t = ip->mode & BSD211_IFMT;
    if (t != BSD211_IFREG && t != BSD211_IFDIR && t != BSD211_IFLNK)
        return 0;
    for (int i = BSD211_NIADDR - 1; i >= 0; i--) {
        uint32_t bn = ip->addr[i];
        if (bn == 0)
            continue;
        ip->addr[i] = 0;
        int level = i - BSD211_NDADDR;   /* 0=single, 1=double, 2=triple */
        if (level >= 0)
            tloop(fs, bn, level);
        else
            bsd211fs_bfree(fs, bn);
    }
    ip->size = 0;
    return 0;
}

int bsd211fs_itrunc_from(bsd211fs_t *fs, bsd211_inode_t *ip, uint32_t first_blk) {
    /* Free blocks [first_blk, ...).  first_blk is in 1024-byte block units. */
    uint32_t rem = first_blk;
    uint32_t nindir = BSD211_NINDIR;

    if (rem < BSD211_NDADDR) {
        for (int i = BSD211_NDADDR - 1; i >= (int)rem; i--) {
            if (ip->addr[i]) { bsd211fs_bfree(fs, ip->addr[i]); ip->addr[i] = 0; }
        }
        rem = 0;
    } else {
        rem -= BSD211_NDADDR;
    }
    if (rem < nindir) {
        if (ip->addr[BSD211_NDADDR]) {
            if (rem == 0) { tloop(fs, ip->addr[BSD211_NDADDR], 0); ip->addr[BSD211_NDADDR] = 0; }
        }
        rem = 0;
    } else {
        rem -= nindir;
    }
    if (rem < (uint32_t)nindir * nindir) {
        if (ip->addr[BSD211_NDADDR + 1]) {
            if (rem == 0) { tloop(fs, ip->addr[BSD211_NDADDR + 1], 1); ip->addr[BSD211_NDADDR + 1] = 0; }
        }
        rem = 0;
    } else {
        rem -= (uint32_t)nindir * nindir;
    }
    if (ip->addr[BSD211_NDADDR + 2]) {
        if (rem == 0) { tloop(fs, ip->addr[BSD211_NDADDR + 2], 2); ip->addr[BSD211_NDADDR + 2] = 0; }
    }
    return 0;
}

/* Follow an indirect chain (levels 1/2/3) from *slot; allocate when create. */
static int ind_follow(bsd211fs_t *fs, uint32_t *slot, int levels,
                      const uint32_t *indices, int create, uint32_t *out) {
    uint32_t blk = *slot;
    for (int L = 0; L < levels; L++) {
        if (blk == 0) {
            if (!create) { *out = 0; return 0; }
            uint8_t z[BSD211_BSIZE];
            memset(z, 0, sizeof(z));
            if (bsd211fs_balloc(fs, &blk) || bsd211fs_write_block(fs, blk, z))
                return -ENOSPC;
            *slot = blk;
        }
        uint8_t buf[BSD211_BSIZE];
        if (bsd211fs_read_block(fs, blk, buf))
            return -EIO;
        uint32_t next = bsd211_get32me(buf + 4 * indices[L]);
        if (L == levels - 1) {
            if (next == 0 && create) {
                if (bsd211fs_balloc(fs, &next))
                    return -ENOSPC;
                bsd211_put32me(buf + 4 * indices[L], next);
                if (bsd211fs_write_block(fs, blk, buf))
                    return -EIO;
            }
            *out = next;
            return 0;
        }
        if (next == 0) {
            if (!create) { *out = 0; return 0; }
            uint8_t z[BSD211_BSIZE];
            memset(z, 0, sizeof(z));
            if (bsd211fs_balloc(fs, &next) || bsd211fs_write_block(fs, next, z))
                return -ENOSPC;
            bsd211_put32me(buf + 4 * indices[L], next);
            if (bsd211fs_write_block(fs, blk, buf))
                return -EIO;
        }
        blk = next;
    }
    return -EIO;
}

int bsd211fs_bmap(bsd211fs_t *fs, bsd211_inode_t *ip, uint32_t lbn, int create, uint32_t *bno) {
    uint32_t nindir = BSD211_NINDIR;
    if (lbn < BSD211_NDADDR) {
        uint32_t nb = ip->addr[lbn];
        if (nb == 0 && create) {
            if (bsd211fs_balloc(fs, &nb))
                return -ENOSPC;
            ip->addr[lbn] = nb;
        }
        *bno = nb;
        return 0;
    }
    uint32_t r = lbn - BSD211_NDADDR;
    if (r < nindir)
        return ind_follow(fs, &ip->addr[BSD211_NDADDR], 1, &r, create, bno);
    r -= nindir;
    if (r < (uint32_t)nindir * nindir) {
        uint32_t idx[2] = { r / nindir, r % nindir };
        return ind_follow(fs, &ip->addr[BSD211_NDADDR + 1], 2, idx, create, bno);
    }
    r -= (uint32_t)nindir * nindir;
    uint32_t idx[3] = { r / (nindir * nindir), (r / nindir) % nindir, r % nindir };
    return ind_follow(fs, &ip->addr[BSD211_NDADDR + 2], 3, idx, create, bno);
}

/* ---- file data ----------------------------------------------------------- */

ssize_t bsd211fs_file_read(bsd211fs_t *fs, bsd211_inode_t *ip, uint8_t *buf, size_t size, off_t off) {
    if (off < 0)
        return -EINVAL;
    if ((uint64_t)off >= ip->size)
        return 0;
    uint64_t remaining = ip->size - (uint64_t)off;
    if (size > remaining)
        size = (size_t)remaining;
    size_t done = 0;
    while (done < size) {
        uint32_t lbn  = (uint32_t)((off + (off_t)done) / BSD211_BSIZE);
        uint32_t boff = (uint32_t)((off + (off_t)done) % BSD211_BSIZE);
        uint32_t pbn;
        if (bsd211fs_bmap(fs, ip, lbn, 0, &pbn))
            return -EIO;
        uint8_t blk[BSD211_BSIZE];
        if (pbn == 0) {
            memset(blk, 0, sizeof(blk));
        } else if (bsd211fs_read_block(fs, pbn, blk)) {
            return -EIO;
        }
        size_t n = BSD211_BSIZE - boff;
        if (n > size - done)
            n = size - done;
        memcpy(buf + done, blk + boff, n);
        done += n;
    }
    return (ssize_t)done;
}

ssize_t bsd211fs_file_write(bsd211fs_t *fs, bsd211_inode_t *ip, const uint8_t *buf, size_t size, off_t off) {
    if (off < 0)
        return -EINVAL;
    size_t done = 0;
    while (done < size) {
        uint32_t lbn  = (uint32_t)((off + (off_t)done) / BSD211_BSIZE);
        uint32_t boff = (uint32_t)((off + (off_t)done) % BSD211_BSIZE);
        uint32_t pbn;
        if (bsd211fs_bmap(fs, ip, lbn, 1, &pbn))
            return -ENOSPC;
        if (pbn == 0)
            return -ENOSPC;
        uint8_t blk[BSD211_BSIZE];
        if (bsd211fs_read_block(fs, pbn, blk))
            return -EIO;
        size_t n = BSD211_BSIZE - boff;
        if (n > size - done)
            n = size - done;
        memcpy(blk + boff, buf + done, n);
        if (bsd211fs_write_block(fs, pbn, blk))
            return -EIO;
        done += n;
    }
    if ((uint64_t)off + size > ip->size)
        ip->size = (uint32_t)(off + size);
    bsd211fs_write_inode(fs, ip->ino, ip);
    return (ssize_t)done;
}

/* ---- directories (variable-length entries) ------------------------------- */

static inline uint32_t dirsiz(uint16_t namlen) {
    return (7u + namlen + 3u) & ~3u;   /* round_up(7 + namlen, 4) */
}

int bsd211fs_dir_read(bsd211fs_t *fs, bsd211_inode_t *ip, bsd211_dirent_t **ents, size_t *count) {
    if ((ip->mode & BSD211_IFMT) != BSD211_IFDIR)
        return -ENOTDIR;
    if (ip->size > (uint64_t)(fs->fsize - fs->isize) * BSD211_BSIZE)
        return -EFBIG;
    size_t cap = ip->size / 12 + 1;
    bsd211_dirent_t *out = calloc(cap, sizeof(*out));
    if (!out)
        return -ENOMEM;
    uint8_t *buf = malloc(ip->size ? ip->size : 1);
    if (!buf) { free(out); return -ENOMEM; }
    ssize_t n = bsd211fs_file_read(fs, ip, buf, ip->size, 0);
    if (n < 0) { free(buf); free(out); return (int)n; }

    size_t cnt = 0;
    for (size_t off = 0; off + 6 <= (size_t)n; ) {
        uint16_t ino    = bsd211_get16le(buf + off);
        uint16_t reclen = bsd211_get16le(buf + off + 2);
        uint16_t namlen = bsd211_get16le(buf + off + 4);
        if (reclen < 6 || off + reclen > (size_t)n)
            break;
        if (ino != 0 && namlen <= BSD211_MAXNAMLEN) {
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

void bsd211fs_dirents_free(bsd211_dirent_t *ents) {
    free(ents);
}

int bsd211fs_dir_lookup(bsd211fs_t *fs, bsd211_inode_t *ip, const char *name, uint32_t *ino) {
    bsd211_dirent_t *ents = NULL;
    size_t count = 0;
    int rc = bsd211fs_dir_read(fs, ip, &ents, &count);
    if (rc)
        return rc;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(ents[i].name, name) == 0) {
            *ino = ents[i].ino;
            free(ents);
            return 0;
        }
    }
    free(ents);
    return -ENOENT;
}

int bsd211fs_dir_add(bsd211fs_t *fs, bsd211_inode_t *ip, uint32_t ino, const char *name) {
    size_t namlen = strlen(name);
    if (namlen > BSD211_MAXNAMLEN)
        return -ENAMETOOLONG;
    uint32_t need = dirsiz((uint16_t)namlen);

    uint8_t *buf = malloc(ip->size ? ip->size : 1);
    if (!buf)
        return -ENOMEM;
    ssize_t n = bsd211fs_file_read(fs, ip, buf, ip->size, 0);
    if (n < 0) { free(buf); return (int)n; }

    /* Reuse a free entry (d_ino == 0) that is big enough. */
    for (size_t off = 0; off + 6 <= (size_t)n; ) {
        uint16_t d_ino    = bsd211_get16le(buf + off);
        uint16_t reclen   = bsd211_get16le(buf + off + 2);
        if (reclen < 6 || off + reclen > (size_t)n)
            break;
        if (d_ino == 0 && reclen >= need) {
            memset(buf + off, 0, reclen);
            bsd211_put16le(buf + off, (uint16_t)ino);
            bsd211_put16le(buf + off + 2, reclen);
            bsd211_put16le(buf + off + 4, (uint16_t)namlen);
            memcpy(buf + off + 6, name, namlen);
            ssize_t w = bsd211fs_file_write(fs, ip, buf, n, 0);
            free(buf);
            return w < 0 ? (int)w : 0;
        }
        off += reclen;
    }

    /* Append a new entry at the end (the directory grows). */
    uint8_t ent[6 + BSD211_MAXNAMLEN + 4];
    memset(ent, 0, sizeof(ent));
    bsd211_put16le(ent, (uint16_t)ino);
    bsd211_put16le(ent + 2, (uint16_t)need);
    bsd211_put16le(ent + 4, (uint16_t)namlen);
    memcpy(ent + 6, name, namlen);
    ssize_t w = bsd211fs_file_write(fs, ip, ent, need, n);
    free(buf);
    return w < 0 ? (int)w : 0;
}

int bsd211fs_dir_remove(bsd211fs_t *fs, bsd211_inode_t *ip, const char *name) {
    uint8_t *buf = malloc(ip->size ? ip->size : 1);
    if (!buf)
        return -ENOMEM;
    ssize_t n = bsd211fs_file_read(fs, ip, buf, ip->size, 0);
    if (n < 0) { free(buf); return (int)n; }

    for (size_t off = 0; off + 6 <= (size_t)n; ) {
        uint16_t d_ino    = bsd211_get16le(buf + off);
        uint16_t reclen   = bsd211_get16le(buf + off + 2);
        uint16_t namlen   = bsd211_get16le(buf + off + 4);
        if (reclen < 6 || off + reclen > (size_t)n)
            break;
        if (d_ino != 0 && namlen == strlen(name) &&
            memcmp(buf + off + 6, name, namlen) == 0) {
            bsd211_put16le(buf + off, 0);   /* mark free */
            ssize_t w = bsd211fs_file_write(fs, ip, buf, n, 0);
            free(buf);
            return w < 0 ? (int)w : 0;
        }
        off += reclen;
    }
    free(buf);
    return -ENOENT;
}

/* ---- path lookup ---------------------------------------------------------- */

int bsd211fs_lookup(bsd211fs_t *fs, const char *path, uint32_t *ino, bsd211_inode_t *ip) {
    uint32_t cur = BSD211_ROOTINO;
    bsd211_inode_t cip;
    if (bsd211fs_read_inode(fs, cur, &cip))
        return -EIO;
    if (*path == '/')
        path++;
    while (*path) {
        if ((cip.mode & BSD211_IFMT) != BSD211_IFDIR)
            return -ENOTDIR;
        const char *slash = strchr(path, '/');
        size_t len = slash ? (size_t)(slash - path) : strlen(path);
        char name[BSD211_MAXNAMLEN + 1];
        if (len == 0) { cur = BSD211_ROOTINO; }
        else {
            if (len > BSD211_MAXNAMLEN)
                return -ENAMETOOLONG;
            memcpy(name, path, len);
            name[len] = 0;
            uint32_t next;
            int rc = bsd211fs_dir_lookup(fs, &cip, name, &next);
            if (rc)
                return rc;
            cur = next;
        }
        if (bsd211fs_read_inode(fs, cur, &cip))
            return -EIO;
        path += len;
        if (*path == '/')
            path++;
    }
    *ino = cur;
    if (ip)
        *ip = cip;
    return 0;
}

/* ---- integrity check ------------------------------------------------------ */

/* Mark one data block as accounted-for.  Returns 0 on a first claim, 1 for an
 * out-of-range (bad) block, 2 for a duplicate (already claimed). */
static int bsd211_mark_block(bsd211fs_t *fs, uint8_t *bmap,
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

/* Mark an indirect block and everything beneath it.  level 0 = single
 * indirect, 1 = double, 2 = triple.  A bad or duplicate indirect block is not
 * chased: its children were already claimed (or are unreachable), so re-walking
 * them would only cascade spurious "dup" reports (BSD fsck phase 1). */
static void bsd211_mark_tree(bsd211fs_t *fs, uint8_t *bmap,
                            uint32_t blk, int level,
                            uint32_t *used, uint32_t *dups, uint32_t *errors)
{
    if (blk == 0)
        return;
    if (bsd211_mark_block(fs, bmap, blk, used, dups, errors) != 0)
        return;
    uint8_t buf[BSD211_BSIZE];
    if (bsd211fs_read_block(fs, blk, buf)) {
        printf("cannot read indirect block %u\n", blk);
        (*errors)++;
        return;
    }
    for (int i = 0; i < BSD211_NINDIR; i++) {
        uint32_t nb = bsd211_get32me(buf + 4 * i);
        if (nb == 0)
            continue;
        if (level > 0)
            bsd211_mark_tree(fs, bmap, nb, level - 1, used, dups, errors);
        else
            bsd211_mark_block(fs, bmap, nb, used, dups, errors);
    }
}

int bsd211fs_check(bsd211fs_t *fs, bsd211_check_t *rep, int mode) {
    (void)mode;
    memset(rep, 0, sizeof(*rep));
    uint32_t nblk = fs->fsize - fs->isize;   /* data blocks */
    uint32_t maxino = (uint32_t)(fs->isize - 2) * BSD211_INOPB;
    uint8_t *bmap = calloc((nblk + 7) / 8, 1);
    if (!bmap)
        return -ENOMEM;

    /* mark data blocks referenced by inodes (direct, then chase indirects) */
    uint32_t used = 0, dups = 0, errors = 0, inodes_used = 0;
    for (uint32_t ino = 1; ino <= maxino; ino++) {
        bsd211_inode_t ip;
        if (bsd211fs_read_inode(fs, ino, &ip))
            continue;
        if (ip.mode == 0)
            continue;
        inodes_used++;
        uint16_t t = ip.mode & BSD211_IFMT;
        if (t == BSD211_IFCHR || t == BSD211_IFBLK || t == BSD211_IFSOCK)
            continue;   /* device/socket inode: addr[0] is a device number, no data blocks */
        if (t != BSD211_IFREG && t != BSD211_IFDIR && t != BSD211_IFLNK) {
            printf("inode %u unknown type 0%o\n", ino, t);
            errors++;
            continue;   /* unknown type: block addresses are untrusted */
        }
        for (int i = 0; i < BSD211_NIADDR; i++) {
            uint32_t bn = ip.addr[i];
            if (bn == 0)
                continue;
            if (i < BSD211_NDADDR)
                bsd211_mark_block(fs, bmap, bn, &used, &dups, &errors);
            else
                bsd211_mark_tree(fs, bmap, bn, i - BSD211_NDADDR, &used, &dups, &errors);
        }
    }

    /* walk the free list */
    uint32_t n = fs->nfree;
    uint32_t cur[BSD211_NICFREE];
    memcpy(cur, fs->free, sizeof(cur));
    uint32_t free_blocks = 0, guard = 0;
    while (n > 0) {
        uint32_t bno = cur[--n];
        if (bno == 0)
            break;
        if (bno < fs->isize || bno >= fs->fsize)
            break;
        uint32_t off = bno - fs->isize;
        if (bmap[off >> 3] & (1u << (off & 7))) { dups++; errors++; }
        else { bmap[off >> 3] |= (uint8_t)(1u << (off & 7)); free_blocks++; }
        if (++guard > fs->fsize + BSD211_NICFREE)
            break;
        if (n == 0) {
            uint8_t blk[BSD211_BSIZE];
            if (bsd211fs_read_block(fs, bno, blk))
                break;
            n = bsd211_get16le(blk);
            for (int i = 0; i < BSD211_NICFREE; i++)
                cur[i] = bsd211_get32me(blk + 2 + 4 * i);
        }
    }

    /* missing blocks: in the data area but neither used nor free. */
    uint32_t missing = 0;
    for (uint32_t off = 0; off < nblk; off++)
        if (!(bmap[off >> 3] & (1u << (off & 7))))
            missing++;
    free(bmap);

    rep->free_blocks = free_blocks;
    rep->used_blocks = used;
    rep->missing_blocks = missing;
    rep->dup_blocks = dups;
    rep->inodes = maxino;
    rep->used_inodes = inodes_used;
    rep->errors = missing + dups + errors;
    printf("used blocks=%u  free blocks=%u  missing=%u  dup=%u  inodes=%u/%u used  errors=%u\n",
           rep->used_blocks, rep->free_blocks, rep->missing_blocks,
           rep->dup_blocks, rep->used_inodes, rep->inodes, rep->errors);
    return rep->errors ? -1 : 0;
}

/* ---- ops table ------------------------------------------------------------ */

static uint32_t bsd211fs_blocksize_op(const void *fs) { (void)fs; return BSD211_BSIZE; }
static int bsd211fs_open_op(void *fs, const char *path, int readonly, int mode,
                           uint64_t offset, uint32_t bsize) {
    (void)mode; (void)bsize;
    return bsd211fs_open(fs, path, readonly, offset);
}
static void bsd211fs_close_op(void *fs) { bsd211fs_close(fs); }
static int bsd211fs_sync_op(void *fs) { return bsd211fs_sync(fs); }
static int bsd211fs_mark_dirty_op(void *fs) { return bsd211fs_mark_dirty(fs); }
static int bsd211fs_read_block_op(void *fs, uint32_t bno, uint8_t *buf)
{ return bsd211fs_read_block(fs, bno, buf); }
static int bsd211fs_write_block_op(void *fs, uint32_t bno, const uint8_t *buf)
{ return bsd211fs_write_block(fs, bno, buf); }
static int bsd211fs_read_inode_op(void *fs, uint32_t ino, filsys_inode_t *ip)
{ return bsd211fs_read_inode(fs, ino, ip); }
static int bsd211fs_write_inode_op(void *fs, uint32_t ino, const filsys_inode_t *ip)
{ return bsd211fs_write_inode(fs, ino, ip); }
static int bsd211fs_ialloc_op(void *fs, uint32_t *ino) { return bsd211fs_ialloc(fs, ino); }
static void bsd211fs_ifree_op(void *fs, uint32_t ino) { bsd211fs_ifree(fs, ino); }
static int bsd211fs_bmap_op(void *fs, filsys_inode_t *ip, uint32_t lbn, int create, uint32_t *bno)
{ return bsd211fs_bmap(fs, ip, lbn, create, bno); }
static int bsd211fs_itrunc_op(void *fs, filsys_inode_t *ip) { return bsd211fs_itrunc(fs, ip); }
static int bsd211fs_itrunc_from_op(void *fs, filsys_inode_t *ip, uint32_t first_blk)
{ return bsd211fs_itrunc_from(fs, ip, first_blk); }
static ssize_t bsd211fs_file_read_op(void *fs, filsys_inode_t *ip, uint8_t *buf, size_t size, off_t off)
{ return bsd211fs_file_read(fs, ip, buf, size, off); }
static ssize_t bsd211fs_file_write_op(void *fs, filsys_inode_t *ip, const uint8_t *buf, size_t size, off_t off)
{ return bsd211fs_file_write(fs, ip, buf, size, off); }
static int bsd211fs_dir_read_op(void *fs, filsys_inode_t *ip, filsys_dirent_t **ents, size_t *count)
{ return bsd211fs_dir_read(fs, ip, ents, count); }
static int bsd211fs_dir_lookup_op(void *fs, filsys_inode_t *ip, const char *name, uint32_t *ino)
{ return bsd211fs_dir_lookup(fs, ip, name, ino); }
static int bsd211fs_dir_add_op(void *fs, filsys_inode_t *ip, uint32_t ino, const char *name)
{ return bsd211fs_dir_add(fs, ip, ino, name); }
static int bsd211fs_dir_remove_op(void *fs, filsys_inode_t *ip, const char *name)
{ return bsd211fs_dir_remove(fs, ip, name); }
static int bsd211fs_lookup_op(void *fs, const char *path, uint32_t *ino, filsys_inode_t *ip)
{ return bsd211fs_lookup(fs, path, ino, ip); }
static int bsd211fs_check_op(void *fs) { bsd211_check_t rep; return bsd211fs_check(fs, &rep, 0); }
static uint64_t bsd211fs_max_file_op(void *fs) {
    (void)fs;
    uint64_t n = BSD211_NINDIR;
    return ((uint64_t)BSD211_NDADDR + n + n * n + n * n * n) * BSD211_BSIZE;
}

const struct filsys_ops bsd211fs_ops = {
    .name        = "bsd211",
    .blocksize   = bsd211fs_blocksize_op,
    .open        = bsd211fs_open_op,
    .close       = bsd211fs_close_op,
    .sync        = bsd211fs_sync_op,
    .mark_dirty  = bsd211fs_mark_dirty_op,
    .read_block  = bsd211fs_read_block_op,
    .write_block = bsd211fs_write_block_op,
    .read_inode  = bsd211fs_read_inode_op,
    .write_inode = bsd211fs_write_inode_op,
    .ialloc      = bsd211fs_ialloc_op,
    .ifree       = bsd211fs_ifree_op,
    .bmap        = bsd211fs_bmap_op,
    .itrunc      = bsd211fs_itrunc_op,
    .itrunc_from = bsd211fs_itrunc_from_op,
    .file_read   = bsd211fs_file_read_op,
    .file_write  = bsd211fs_file_write_op,
    .dir_read    = bsd211fs_dir_read_op,
    .dir_lookup  = bsd211fs_dir_lookup_op,
    .dir_add     = bsd211fs_dir_add_op,
    .dir_remove  = bsd211fs_dir_remove_op,
    .lookup      = bsd211fs_lookup_op,
    .check       = bsd211fs_check_op,
    .max_file    = bsd211fs_max_file_op,
};
