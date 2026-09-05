/* filsys.c - version-agnostic access layer for Research Unix filesystem
 * images.  Dispatches to the internal v6fs/v7fs backends; this is the library
 * behind both mount.filsys (FUSE) and the standalone tools.
 *
 * SPDX-License-Identifier: ISC
 */
#include <config.h>
#include "filsys.h"
#include "filsys_ops.h"
#include "v1fs.h"
#include "v6fs.h"
#include "v7fs.h"
#include "pdp7fs.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysmacros.h>
#include <time.h>

struct filsys {
    const struct filsys_ops *ops;
    void *fs;                  /* backend state (v6fs_t / v7fs_t / ...) */
    int ver;
    int uid, gid;              /* reported ownership (default: the mounting user) */
};

/* ---- mode conversion (edition-aware) ------------------------------------ */

static mode_t to_posix_mode(int ver, uint32_t mode) {
    if (ver == FILSYS_PDP7) {
        /* PDP-7 has four permission bits (owner r/w, world r/w) and no execute
         * bit; devices are I_SPECIAL inodes, not a type field. */
        mode_t m = (mode & P7_IDIR)  ? S_IFDIR :
                   (mode & P7_ISPEC) ? S_IFCHR : S_IFREG;
        if (mode & P7_IOREAD)  m |= S_IRUSR;
        if (mode & P7_IOWRITE) m |= S_IWUSR;
        if (mode & P7_IWREAD)  m |= S_IRGRP | S_IROTH;
        if (mode & P7_IWWRITE) m |= S_IWGRP | S_IWOTH;
        return m;
    }
    if (ver == FILSYS_V1) {
        /* V1's compact two-class permission model: owner r/w/x, non-owner r/w,
         * setuid, plus a single directory bit.  No char/block type bits --
         * devices are identified by inode number (< 41). */
        mode_t m = (mode & V1_IFDIR) ? S_IFDIR : S_IFREG;
        if (mode & V1_IREAD)  m |= S_IRUSR;
        if (mode & V1_IWRITE) m |= S_IWUSR;
        if (mode & V1_IEXEC)  m |= S_IXUSR | S_IXGRP | S_IXOTH;
        if (mode & V1_OREAD)  m |= S_IRGRP | S_IROTH;
        if (mode & V1_OWRITE) m |= S_IWGRP | S_IWOTH;
        if (mode & V1_ISUID)  m |= S_ISUID;
        return m;
    }
    mode_t m = mode & 07777;
    if (ver == FILSYS_V6) {
        switch (mode & V6_IFMT) {
        case V6_IFDIR: m |= S_IFDIR; break;
        case V6_IFCHR: m |= S_IFCHR; break;
        case V6_IFBLK: m |= S_IFBLK; break;
        default:       m |= S_IFREG; break;   /* regular = type 0 */
        }
    } else {
        switch (mode & V7_IFMT) {
        case V7_IFDIR:            m |= S_IFDIR; break;
        case V7_IFREG:            m |= S_IFREG; break;
        case V7_IFCHR: case V7_IFMPC: m |= S_IFCHR; break;
        case V7_IFBLK: case V7_IFMPB: m |= S_IFBLK; break;
        default:                  m |= S_IFREG; break;
        }
    }
    return m;
}

static uint16_t perm_of(mode_t m) {
    return (uint16_t)(m & 07777);
}

/* POSIX mode -> V1's on-disk flag word.  V1 has a two-class permission model
 * (owner r/w/x, non-owner r/w) with no group or sticky bit, a single universal
 * exec bit, and regular files carry no type bit (only IALLOC).  Group/other
 * read-write collapse onto the non-owner bits; owner-exec drives the universal
 * exec bit. */
static uint16_t to_v1_mode(mode_t m, int isdir) {
    uint16_t f = V1_IALLOC;
    if (isdir) f |= V1_IFDIR;
    if (m & S_IRUSR) f |= V1_IREAD;
    if (m & S_IWUSR) f |= V1_IWRITE;
    if (m & S_IXUSR) f |= V1_IEXEC;
    if ((m & S_IRGRP) || (m & S_IROTH)) f |= V1_OREAD;
    if ((m & S_IWGRP) || (m & S_IWOTH)) f |= V1_OWRITE;
    if (m & S_ISUID) f |= V1_ISUID;
    return f;
}

/* POSIX mode -> PDP-7's on-disk flag word: I_USED + four permission bits
 * (owner r/w, world r/w; no execute, no group). */
static uint32_t to_p7_mode(mode_t m, int isdir) {
    uint32_t f = P7_IUSED;
    if (isdir) f |= P7_IDIR;
    if (m & S_IRUSR) f |= P7_IOREAD;
    if (m & S_IWUSR) f |= P7_IOWRITE;
    if ((m & S_IRGRP) || (m & S_IROTH)) f |= P7_IWREAD;
    if ((m & S_IWGRP) || (m & S_IWOTH)) f |= P7_IWWRITE;
    return f;
}

/* Is this on-disk mode word a directory?  V1 sets V1_IFDIR alongside the
 * always-present IALLOC bit, so masking with V6/V7's IFMT (0170000) would
 * misread it; V6/V7 use the type field. */
static int is_dir(int ver, uint32_t mode) {
    if (ver == FILSYS_PDP7)
        return (mode & P7_IDIR) != 0;
    if (ver == FILSYS_V1)
        return (mode & V1_IFDIR) != 0;
    uint16_t fmask = (ver == FILSYS_V6) ? V6_IFMT : V7_IFMT;
    uint16_t dbit  = (ver == FILSYS_V6) ? V6_IFDIR : V7_IFDIR;
    return (mode & fmask) == dbit;
}

/* ---- dispatch (internal): forward through the backend ops table ---------- */

static int read_inode(filsys_t *fs, uint32_t ino, filsys_inode_t *ip) {
    return fs->ops->read_inode(fs->fs, ino, ip);
}
static int write_inode(filsys_t *fs, uint32_t ino, const filsys_inode_t *ip) {
    return fs->ops->write_inode(fs->fs, ino, ip);
}
static int ialloc(filsys_t *fs, uint32_t *ino) {
    return fs->ops->ialloc(fs->fs, ino);
}
static void ifree(filsys_t *fs, uint32_t ino) {
    fs->ops->ifree(fs->fs, ino);
}
static int itrunc(filsys_t *fs, filsys_inode_t *ip) {
    return fs->ops->itrunc(fs->fs, ip);
}
static int itrunc_from(filsys_t *fs, filsys_inode_t *ip, uint32_t first_blk) {
    return fs->ops->itrunc_from(fs->fs, ip, first_blk);
}
static ssize_t file_read(filsys_t *fs, filsys_inode_t *ip, uint8_t *buf, size_t sz, off_t off) {
    return fs->ops->file_read(fs->fs, ip, buf, sz, off);
}
static ssize_t file_write(filsys_t *fs, filsys_inode_t *ip, const uint8_t *buf, size_t sz, off_t off) {
    return fs->ops->file_write(fs->fs, ip, buf, sz, off);
}
static int dir_read(filsys_t *fs, filsys_inode_t *ip, filsys_dirent_t **e, size_t *n) {
    return fs->ops->dir_read(fs->fs, ip, e, n);
}
static int dir_lookup(filsys_t *fs, filsys_inode_t *ip, const char *name, uint32_t *ino) {
    return fs->ops->dir_lookup(fs->fs, ip, name, ino);
}
static int dir_add(filsys_t *fs, filsys_inode_t *ip, uint32_t ino, const char *name) {
    return fs->ops->dir_add(fs->fs, ip, ino, name);
}
static int dir_remove(filsys_t *fs, filsys_inode_t *ip, const char *name) {
    return fs->ops->dir_remove(fs->fs, ip, name);
}
static int lookup(filsys_t *fs, const char *path, uint32_t *ino, filsys_inode_t *ip) {
    return fs->ops->lookup(fs->fs, path, ino, ip);
}
static int bmap(filsys_t *fs, filsys_inode_t *ip, uint32_t lbn, int create, uint32_t *bno) {
    return fs->ops->bmap(fs->fs, ip, lbn, create, bno);
}

/* Largest file the selected edition can address, in bytes. */
static uint64_t maxfile(const filsys_t *fs) {
    return fs->ops->max_file(fs->fs);
}

/* ---- helpers ------------------------------------------------------------- */

static int split_path(const char *path, char *dir, size_t dirsz,
                      char *name, size_t namesz) {
    const char *slash = strrchr(path, '/');
    if (!slash)
        return -EINVAL;
    size_t dlen = (size_t)(slash - path);
    if (dlen == 0)
        dlen = 1;
    if (dlen >= dirsz)
        return -ENAMETOOLONG;
    memcpy(dir, path, dlen);
    dir[dlen] = 0;
    const char *nm = slash + 1;
    if (!*nm)
        return -EINVAL;
    size_t nlen = strlen(nm);
    if (nlen >= namesz)
        return -ENAMETOOLONG;
    memcpy(name, nm, nlen + 1);
    return 0;
}

/* ---- public API ---------------------------------------------------------- */

int filsys_open(filsys_t **out, int edition, const char *path, int readonly,
                uint64_t offset, int uid, int gid) {
    filsys_t *fs = calloc(1, sizeof(*fs));
    if (!fs)
        return -ENOMEM;
    fs->ver = edition;
    fs->uid = uid;
    fs->gid = gid;
    if (edition == FILSYS_V6) {
        fs->ops = &v6fs_ops;
        fs->fs  = calloc(1, sizeof(v6fs_t));
    } else if (edition == FILSYS_V1) {
        fs->ops = &v1fs_ops;
        fs->fs  = calloc(1, sizeof(v1fs_t));
    } else if (edition == FILSYS_PDP7) {
        fs->ops = &p7fs_ops;
        fs->fs  = calloc(1, sizeof(p7fs_t));
    } else {
        fs->ops = &v7fs_ops;
        fs->fs  = calloc(1, sizeof(v7fs_t));
    }
    if (!fs->fs) {
        free(fs);
        return -ENOMEM;
    }
    /* The open op's 4th arg is a byte-order/layout mode: 0 = V7 (middle-endian,
     * 2-byte), 1 = 32V (little-endian, 4-byte), 2 = Coherent (middle-endian,
     * 2-byte, NICFREE=64).  Only v7fs reads it. */
    int mode = (edition == FILSYS_32V) ? 1 : (edition == FILSYS_COHERENT) ? 2 : 0;
    int rc = fs->ops->open(fs->fs, path, readonly, mode, offset);
    if (rc) {
        free(fs->fs);
        free(fs);
        return rc;
    }
    /* A read-write open is dirty until a clean close clears s_fmod (see the
     * backends' close); stamp it now so a crash before close is flagged. */
    if (!readonly && fs->ops->mark_dirty)
        fs->ops->mark_dirty(fs->fs);
    *out = fs;
    return 0;
}

void filsys_close(filsys_t *fs) {
    if (!fs)
        return;
    if (fs->fs)
        fs->ops->close(fs->fs);
    free(fs->fs);
    free(fs);
}

int filsys_sync(filsys_t *fs) {
    return fs->ops->sync(fs->fs);
}

int filsys_is_readonly(const filsys_t *fs) {
    if (fs->ver == FILSYS_V6) return ((const v6fs_t *)fs->fs)->readonly;
    if (fs->ver == FILSYS_V1) return ((const v1fs_t *)fs->fs)->readonly;
    if (fs->ver == FILSYS_PDP7) return ((const p7fs_t *)fs->fs)->readonly;
    return ((const v7fs_t *)fs->fs)->readonly;
}

int filsys_edition(const filsys_t *fs) {
    return fs->ver;
}

uid_t filsys_uid(const filsys_t *fs) {
    return (uid_t)fs->uid;
}

gid_t filsys_gid(const filsys_t *fs) {
    return (gid_t)fs->gid;
}

int filsys_check(filsys_t *fs) {
    return fs->ops->check(fs->fs);
}

int filsys_lookup(filsys_t *fs, const char *path, uint32_t *ino, filsys_inode_t *ip) {
    return lookup(fs, path, ino, ip);
}

int filsys_read_inode(filsys_t *fs, uint32_t ino, filsys_inode_t *ip) {
    return read_inode(fs, ino, ip);
}

void filsys_fill_stat(filsys_t *fs, const filsys_inode_t *ip, struct stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_ino   = ip->ino;
    st->st_mode  = to_posix_mode(fs->ver, ip->mode);
    st->st_nlink = ip->nlink;
    st->st_uid   = fs->uid;
    st->st_gid   = fs->gid;
    st->st_size  = ip->size;
    int isdev;
    if (fs->ver == FILSYS_V1) {
        isdev = (ip->ino < V1_ROOTINO);   /* V1: devices by inode number */
    } else if (fs->ver == FILSYS_PDP7) {
        isdev = (ip->mode & P7_ISPEC) != 0;   /* PDP-7: I_SPECIAL inode */
    } else {
        uint32_t t = fs->ver == FILSYS_V6 ? (ip->mode & V6_IFMT) : (ip->mode & V7_IFMT);
        isdev = (fs->ver == FILSYS_V6) ? (t == V6_IFCHR || t == V6_IFBLK)
                                       : (t == V7_IFCHR || t == V7_IFBLK ||
                                          t == V7_IFMPC || t == V7_IFMPB);
    }
    if (isdev)
        st->st_rdev = ip->addr[0];
    st->st_atime   = ip->atime;
    st->st_mtime   = ip->mtime;
    st->st_ctime   = ip->ctime;
    st->st_blksize = 512;
    st->st_blocks  = (ip->size + 511) / 512;
}

int filsys_readdir(filsys_t *fs, const char *path, filsys_dirent_t **ents, size_t *count) {
    filsys_inode_t ip;
    uint32_t ino;
    int rc = lookup(fs, path, &ino, &ip);
    if (rc) return rc;
    return dir_read(fs, &ip, ents, count);
}

int filsys_read(filsys_t *fs, const char *path, void *buf, size_t size, off_t off) {
    filsys_inode_t ip;
    uint32_t ino;
    int rc = lookup(fs, path, &ino, &ip);
    if (rc) return rc;
    return (int)file_read(fs, &ip, (uint8_t *)buf, size, off);
}

int filsys_write(filsys_t *fs, const char *path, const void *buf, size_t size, off_t off) {
    filsys_inode_t ip;
    uint32_t ino;
    int rc = lookup(fs, path, &ino, &ip);
    if (rc) return rc;
    if (off < 0)
        return -EINVAL;
    /* A write past the per-format file-size ceiling can never succeed; reject
     * it before the first block is allocated so there is nothing to unwind. */
    if ((uint64_t)off + (uint64_t)size > maxfile(fs))
        return -EFBIG;

    uint32_t oldsize = ip.size;
    ssize_t n = file_write(fs, &ip, (const uint8_t *)buf, size, off);
    if (n < 0) {
        /* A legal-sized write can still fail partway (ENOSPC, EIO): free the
         * blocks it allocated past the original size, then reset the inode. */
        uint32_t bsize = fs->ops->blocksize;
        uint32_t first_blk = oldsize ? (oldsize - 1) / bsize + 1 : 0;
        itrunc_from(fs, &ip, first_blk);
        write_inode(fs, ino, &ip);
    }
    return (int)n;
}

int filsys_create(filsys_t *fs, const char *path, mode_t mode, uid_t uid, gid_t gid) {
    char dir[PATH_MAX], name[15];
    int rc = split_path(path, dir, sizeof(dir), name, sizeof(name));
    if (rc) return rc;
    filsys_inode_t ddir;
    uint32_t dino;
    rc = lookup(fs, dir, &dino, &ddir);
    if (rc) return rc;
    uint32_t nino;
    rc = ialloc(fs, &nino);
    if (rc) return rc;
    filsys_inode_t nip;
    memset(&nip, 0, sizeof(nip));
    nip.ino = nino;
    if (fs->ver == FILSYS_V1)
        nip.mode = to_v1_mode(mode, 0);
    else if (fs->ver == FILSYS_PDP7)
        nip.mode = to_p7_mode(mode, 0);
    else if (fs->ver == FILSYS_V6)
        nip.mode = perm_of(mode);              /* regular file = type 0 */
    else
        nip.mode = perm_of(mode) | V7_IFREG;
    nip.nlink = 1;
    nip.uid = (int16_t)uid;
    nip.gid = (int16_t)gid;
    nip.atime = nip.mtime = nip.ctime = (uint32_t)time(NULL);
    write_inode(fs, nino, &nip);
    rc = dir_add(fs, &ddir, nino, name);
    if (rc) {
        /* The directory entry never landed: put the inode back. */
        nip.mode = 0;
        write_inode(fs, nino, &nip);
        ifree(fs, nino);
    }
    return rc;
}

int filsys_mkdir(filsys_t *fs, const char *path, mode_t mode, uid_t uid, gid_t gid) {
    char dir[PATH_MAX], name[15];
    int rc = split_path(path, dir, sizeof(dir), name, sizeof(name));
    if (rc) return rc;
    filsys_inode_t ddir;
    uint32_t dino;
    rc = lookup(fs, dir, &dino, &ddir);
    if (rc) return rc;
    uint32_t nino;
    rc = ialloc(fs, &nino);
    if (rc) return rc;
    filsys_inode_t nip;
    memset(&nip, 0, sizeof(nip));
    nip.ino = nino;
    if (fs->ver == FILSYS_V1)
        nip.mode = to_v1_mode(mode, 1);
    else if (fs->ver == FILSYS_PDP7)
        nip.mode = to_p7_mode(mode, 1);
    else if (fs->ver == FILSYS_V6)
        nip.mode = perm_of(mode) | V6_IFDIR;
    else
        nip.mode = perm_of(mode) | V7_IFDIR;
    nip.nlink = 2;
    nip.uid = (int16_t)uid;
    nip.gid = (int16_t)gid;
    nip.atime = nip.mtime = nip.ctime = (uint32_t)time(NULL);
    write_inode(fs, nino, &nip);
    dir_add(fs, &nip, nino, ".");
    dir_add(fs, &nip, dino, "..");
    rc = dir_add(fs, &ddir, nino, name);
    if (rc) {
        /* The parent entry never landed: free the new directory's "." and ".."
         * blocks, then its inode, so it isn't left orphaned. */
        itrunc(fs, &nip);
        nip.mode = 0;
        write_inode(fs, nino, &nip);
        ifree(fs, nino);
        return rc;
    }
    ddir.nlink++;
    write_inode(fs, dino, &ddir);
    return 0;
}

int filsys_mknod(filsys_t *fs, const char *path, mode_t mode, dev_t rdev,
                 uid_t uid, gid_t gid) {
    /* V1 has no device type bits: devices are the fixed inodes 1..40, wired up
     * by the kernel at boot (u0.s), not created with mknod(2). */
    if (fs->ver == FILSYS_V1)
        return -EPERM;
    /* V7 has only character and block devices; there is no on-disk type for a
     * FIFO or socket (named pipes arrived in System III).  Reject them up front
     * so mkfifo fails cleanly instead of depositing a bogus char device. */
    int ischr = (mode & S_IFMT) == S_IFCHR;
    int isblk = (mode & S_IFMT) == S_IFBLK;
    if (!ischr && !isblk)
        return -EPERM;
    /* V7 device numbers are 8-bit major + 8-bit minor packed into one word.
     * Reject rather than mask: a modern major like 300 would otherwise
     * silently become 44. */
    if (major(rdev) > 255 || minor(rdev) > 255)
        return -EINVAL;

    char dir[PATH_MAX], name[15];
    int rc = split_path(path, dir, sizeof(dir), name, sizeof(name));
    if (rc) return rc;
    filsys_inode_t ddir;
    uint32_t dino;
    rc = lookup(fs, dir, &dino, &ddir);
    if (rc) return rc;
    uint32_t nino;
    rc = ialloc(fs, &nino);
    if (rc) return rc;
    filsys_inode_t nip;
    memset(&nip, 0, sizeof(nip));
    nip.ino = nino;
    if (fs->ver == FILSYS_V6)
        nip.mode = (isblk ? V6_IFBLK : V6_IFCHR) | perm_of(mode);
    else
        nip.mode = (isblk ? V7_IFBLK : V7_IFCHR) | perm_of(mode);
    nip.nlink = 1;
    nip.uid = (int16_t)uid;
    nip.gid = (int16_t)gid;
    nip.atime = nip.mtime = nip.ctime = (uint32_t)time(NULL);
    /* V7 device number: (major<<8)|minor, stored in di_addr[0]. */
    nip.addr[0] = (uint32_t)((major(rdev) << 8) | minor(rdev));
    write_inode(fs, nino, &nip);
    rc = dir_add(fs, &ddir, nino, name);
    if (rc) {
        /* The directory entry never landed: return the inode to the free list
         * so it isn't left orphaned with nlink=1 for fsck to find. */
        nip.mode = 0;
        write_inode(fs, nino, &nip);
        ifree(fs, nino);
    }
    return rc;
}

static int do_unlink(filsys_t *fs, const char *dirpath, const char *name) {
    filsys_inode_t ddir;
    uint32_t dino;
    int rc = lookup(fs, dirpath, &dino, &ddir);
    if (rc) return rc;
    uint32_t ino;
    rc = dir_lookup(fs, &ddir, name, &ino);
    if (rc) return rc;
    filsys_inode_t ip;
    if (read_inode(fs, ino, &ip)) return -EIO;
    rc = dir_remove(fs, &ddir, name);
    if (rc) return rc;
    ip.nlink--;
    if (ip.nlink <= 0) {
        itrunc(fs, &ip);
        ip.mode = 0;
        ifree(fs, ino);
    }
    write_inode(fs, ino, &ip);
    return 0;
}

int filsys_unlink(filsys_t *fs, const char *path) {
    char dir[PATH_MAX], name[15];
    int rc = split_path(path, dir, sizeof(dir), name, sizeof(name));
    if (rc) return rc;
    filsys_inode_t ip;
    uint32_t ino;
    rc = lookup(fs, path, &ino, &ip);
    if (rc) return rc;
    if (is_dir(fs->ver, ip.mode)) return -EISDIR;
    return do_unlink(fs, dir, name);
}

int filsys_rmdir(filsys_t *fs, const char *path) {
    char dir[PATH_MAX], name[15];
    int rc = split_path(path, dir, sizeof(dir), name, sizeof(name));
    if (rc) return rc;
    filsys_inode_t ddir;
    uint32_t dino;
    rc = lookup(fs, dir, &dino, &ddir);
    if (rc) return rc;
    uint32_t ino;
    rc = dir_lookup(fs, &ddir, name, &ino);
    if (rc) return rc;
    filsys_inode_t tip;
    if (read_inode(fs, ino, &tip)) return -EIO;
    if (!is_dir(fs->ver, tip.mode)) return -ENOTDIR;
    filsys_dirent_t *ents = NULL;
    size_t count = 0;
    if (dir_read(fs, &tip, &ents, &count)) return -EIO;
    for (size_t i = 0; i < count; i++)
        if (strcmp(ents[i].name, ".") && strcmp(ents[i].name, "..")) { free(ents); return -ENOTEMPTY; }
    free(ents);
    rc = dir_remove(fs, &ddir, name);
    if (rc) return rc;
    ddir.nlink--;
    write_inode(fs, dino, &ddir);
    itrunc(fs, &tip);
    tip.mode = 0;
    ifree(fs, ino);
    write_inode(fs, ino, &tip);
    return 0;
}

static int do_link(filsys_t *fs, const char *dst, uint32_t src_ino) {
    char dir[PATH_MAX], name[15];
    int rc = split_path(dst, dir, sizeof(dir), name, sizeof(name));
    if (rc) return rc;
    filsys_inode_t ddir;
    uint32_t dino;
    rc = lookup(fs, dir, &dino, &ddir);
    if (rc) return rc;
    rc = dir_add(fs, &ddir, src_ino, name);
    if (rc) return rc;
    filsys_inode_t ip;
    if (read_inode(fs, src_ino, &ip)) return -EIO;
    ip.nlink++;
    write_inode(fs, src_ino, &ip);
    return 0;
}

int filsys_link(filsys_t *fs, const char *from, const char *to) {
    filsys_inode_t ip;
    uint32_t ino;
    int rc = lookup(fs, from, &ino, &ip);
    if (rc) return rc;
    /* No directory check here, on purpose: V7's link(2) lets the superuser
     * hard-link a directory.  That path is unreachable through .link on Linux
     * -- the kernel's vfs_link() refuses directory links before FUSE is
     * consulted -- but do_link is also rename()'s implementation, so it must
     * keep handling directories. */
    return do_link(fs, to, ino);
}

int filsys_rename(filsys_t *fs, const char *from, const char *to, unsigned int flags) {
    if (flags) return -EINVAL;
    if (!strcmp(from, to)) return 0;
    filsys_inode_t sip;
    uint32_t sino;
    int rc = lookup(fs, from, &sino, &sip);
    if (rc) return rc;

    int isdir = is_dir(fs->ver, sip.mode);

    char fdir[PATH_MAX], fname[15];
    split_path(from, fdir, sizeof(fdir), fname, sizeof(fname));
    char tdir[PATH_MAX], tname[15];
    rc = split_path(to, tdir, sizeof(tdir), tname, sizeof(tname));
    if (rc) return rc;

    filsys_inode_t tdirip;
    uint32_t tdino;
    rc = lookup(fs, tdir, &tdino, &tdirip);
    if (rc) return rc;

    /* Overwrite an existing target: refuse to replace a non-empty directory,
     * and do not let a directory be renamed over one. */
    uint32_t tino;
    if (dir_lookup(fs, &tdirip, tname, &tino) == 0) {
        if (tino == sino) return 0;   /* already there */
        filsys_inode_t tip;
        if (read_inode(fs, tino, &tip)) return -EIO;
        if (is_dir(fs->ver, tip.mode)) {
            filsys_dirent_t *ents = NULL; size_t count = 0;
            if (dir_read(fs, &tip, &ents, &count)) return -EIO;
            for (size_t i = 0; i < count; i++)
                if (strcmp(ents[i].name, ".") && strcmp(ents[i].name, "..")) { free(ents); return -ENOTEMPTY; }
            free(ents);
        }
        do_unlink(fs, tdir, tname);
    }

    do_link(fs, to, sino);
    rc = do_unlink(fs, fdir, fname);
    if (rc) return rc;

    if (isdir) {
        /* Moving a directory: fix the two parents' link counts and rewrite the
         * moved directory's '..' entry to point at its new parent. */
        if (strcmp(fdir, tdir) != 0) {
            filsys_inode_t fddir;
            uint32_t fdino;
            if (lookup(fs, fdir, &fdino, &fddir) == 0) {
                if (fddir.nlink > 1) fddir.nlink--;
                write_inode(fs, fdino, &fddir);
            }
            /* tdirip was read before do_link, which rewrote the target
             * directory's inode (its size grew to hold the new entry).  Re-read
             * it fresh so we bump nlink without clobbering that update. */
            if (read_inode(fs, tdino, &tdirip) == 0) {
                tdirip.nlink++;
                write_inode(fs, tdino, &tdirip);
            }
        }
        filsys_inode_t cip;
        if (read_inode(fs, sino, &cip) == 0) {
            dir_remove(fs, &cip, "..");
            dir_add(fs, &cip, tdino, "..");
        }
    }
    return 0;
}

int filsys_truncate(filsys_t *fs, const char *path, off_t size) {
    filsys_inode_t ip;
    uint32_t ino;
    int rc = lookup(fs, path, &ino, &ip);
    if (rc) return rc;
    /* Reject sizes the format cannot address, rather than wrap a 5 GiB request
     * to 1 GiB via the uint32_t cast below (V7's ceiling is ~1.08 GB). */
    if (size < 0 || (uint64_t)size > maxfile(fs))
        return -EFBIG;
    uint32_t newsize = (uint32_t)size, oldsize = ip.size;
    if (newsize == oldsize) return 0;

    /* Shrink in place: zero the partial tail of the last surviving block, free
     * the blocks strictly past the new end, and leave everything else alone.
     * (Extension is a no-op: holes read back as zero.) */
    if (newsize < oldsize) {
        uint32_t bsize = fs->ops->blocksize;
        uint32_t last  = newsize ? (newsize - 1) / bsize : 0;
        uint32_t blk_end = newsize ? (last + 1) * bsize : 0;
        uint32_t tail = oldsize < blk_end ? oldsize : blk_end;
        /* Zero the partial tail of the last surviving block through the file
         * layer, so word-addressed backends (PDP-7, 128-byte logical blocks)
         * pack the zeros correctly.  Skip it if that block is a hole -- it
         * already reads as zero and file_write would only allocate it. */
        if (newsize < tail) {
            uint32_t bno;
            if (bmap(fs, &ip, last, 0, &bno) == 0 && bno != 0) {
                uint8_t zeros[512] = {0};
                ssize_t w = file_write(fs, &ip, zeros, tail - newsize, newsize);
                if (w < 0) return (int)w;
            }
        }
        rc = itrunc_from(fs, &ip, newsize ? last + 1 : 0);
        if (rc) return rc;
    }

    ip.size = newsize;
    return write_inode(fs, ino, &ip);
}

int filsys_chmod(filsys_t *fs, const char *path, mode_t mode) {
    filsys_inode_t ip;
    uint32_t ino;
    int rc = lookup(fs, path, &ino, &ip);
    if (rc) return rc;
    if (fs->ver == FILSYS_V1) {
        /* V1 chmod replaces the low six permission bits (preserving IALLOC/
         * IFDIR/ILARG) and, like V1's sys/chmod, clears setuid+exec on
         * directories. */
        uint16_t bits = to_v1_mode(mode, 0) & (uint16_t)0077;
        if (ip.mode & V1_IFDIR)
            bits &= (uint16_t)~(V1_ISUID | V1_IEXEC);
        ip.mode = (ip.mode & (uint16_t)~0077) | bits;
    } else if (fs->ver == FILSYS_PDP7) {
        /* PDP-7 chmod replaces the low four permission bits, preserving
         * I_USED/I_LARGE/I_DIRECTORY/I_SPECIAL. */
        uint32_t bits = to_p7_mode(mode, 0) & (uint32_t)017;
        ip.mode = (ip.mode & ~(uint32_t)017) | bits;
    } else {
        uint16_t fmask = fs->ver == FILSYS_V6 ? V6_IFMT : V7_IFMT;
        ip.mode = (ip.mode & fmask) | (uint16_t)(mode & 07777);
    }
    ip.ctime = (uint32_t)time(NULL);
    write_inode(fs, ino, &ip);
    return 0;
}

int filsys_chown(filsys_t *fs, const char *path, uid_t uid, gid_t gid) {
    filsys_inode_t ip;
    uint32_t ino;
    int rc = lookup(fs, path, &ino, &ip);
    if (rc) return rc;
    if (uid != (uid_t)-1) ip.uid = (int16_t)uid;
    if (gid != (gid_t)-1) ip.gid = (int16_t)gid;
    ip.ctime = (uint32_t)time(NULL);
    write_inode(fs, ino, &ip);
    return 0;
}

int filsys_utimens(filsys_t *fs, const char *path, const struct timespec tv[2]) {
    filsys_inode_t ip;
    uint32_t ino;
    int rc = lookup(fs, path, &ino, &ip);
    if (rc) return rc;
    uint32_t now = (uint32_t)time(NULL);
    if (!tv) {
        ip.atime = ip.mtime = now;
    } else {
        if (tv[0].tv_nsec != UTIME_OMIT)
            ip.atime = (tv[0].tv_nsec == UTIME_NOW) ? now : (uint32_t)tv[0].tv_sec;
        if (tv[1].tv_nsec != UTIME_OMIT)
            ip.mtime = (tv[1].tv_nsec == UTIME_NOW) ? now : (uint32_t)tv[1].tv_sec;
    }
    ip.ctime = now;
    write_inode(fs, ino, &ip);
    return 0;
}

int filsys_statfs(filsys_t *fs, struct statvfs *st) {
    memset(st, 0, sizeof(*st));
    st->f_bsize = st->f_frsize = 512;
    if (fs->ver == FILSYS_V6) {
        const v6fs_t *v6 = fs->fs;
        st->f_blocks = v6->fsize;
        st->f_bfree = st->f_bavail = v6->tfree;
        st->f_files = v6_maxino(v6->isize);
        st->f_ffree = v6->tinode;
    } else if (fs->ver == FILSYS_V1) {
        const v1fs_t *v1 = fs->fs;
        st->f_blocks = v1->fsize;
        st->f_bfree = st->f_bavail = v1->tfree;
        st->f_files = v1->maxino;
        st->f_ffree = v1->tinode;
    } else if (fs->ver == FILSYS_PDP7) {
        const p7fs_t *p7 = fs->fs;
        st->f_blocks = P7_NBLOCKS;
        st->f_bfree = st->f_bavail = p7->tfree;
        st->f_files = P7_MAXINO;
        st->f_ffree = 0;   /* not tracked (read-only) */
    } else {
        const v7fs_t *v7 = fs->fs;
        st->f_blocks = v7->fsize;
        st->f_bfree = st->f_bavail = v7->tfree;
        st->f_files = (v7->isize - 2) * 8;
        st->f_ffree = v7->tinode;
    }
    st->f_namemax = fs->ver == FILSYS_V1 ? V1_DIRSIZ :
                    fs->ver == FILSYS_PDP7 ? P7_DIRSIZ : 14;
    return 0;
}
