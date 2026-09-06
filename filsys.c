/* filsys.c - version-agnostic access layer for Research Unix filesystem
 * images.  Dispatches to the internal v6fs/v7fs backends; this is the library
 * behind both mount.filsys (FUSE) and the standalone tools.
 *
 * An edition is described by a filsys_edition_t (constants + mode conversion,
 * in filsys_format.c) and a filsys_ops vtable (operations).  filsys_getformat()
 * is the single lookup: adding a format is a new descriptor row, not another
 * arm of a dozen `== FILSYS_` switches here.
 *
 * SPDX-License-Identifier: ISC
 */
#include <config.h>
#include "filsys.h"
#include "filsys_ops.h"
#include "v1fs.h"
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
    filsys_edition_t            fmt;      /* the format (by value) */
    void *fs;                  /* backend state (filsys_edition_t / v1fs_t / p7fs_t) */
    int ver;
    int uid, gid;              /* reported ownership (default: the mounting user) */
    int readonly;
};

/* ---- mode conversion ----------------------------------------------------- */

/* The V6/V7/BSD211 family derives every mode operation from the ifmt/ifdir/...
 * constants in the descriptor, so its fn-ptr slots are NULL and these wrappers
 * fall through to the shared logic below.  V1 and PDP-7 supply their own (in
 * filsys_format.c). */
static mode_t mode_to_posix(const filsys_edition_t *f, const filsys_inode_t *ip) {
    if (f->to_posix_mode)
        return f->to_posix_mode(f, ip);
    mode_t m = ip->mode & 07777;
    uint32_t t = ip->mode & f->ifmt;
    if (t == f->ifdir)
        m |= S_IFDIR;
    else if (t == f->ifchr || (f->ifmpc && t == f->ifmpc))
        m |= S_IFCHR;
    else if (t == f->ifblk || (f->ifmpb && t == f->ifmpb))
        m |= S_IFBLK;
    else if (f->iflnk && t == f->iflnk)
        m |= S_IFLNK;
    else if (f->ifsock && t == f->ifsock)
        m |= S_IFSOCK;
    else
        m |= S_IFREG;   /* V6: regular file = type 0 */
    return m;
}
static int mode_is_dir(const filsys_edition_t *f, const filsys_inode_t *ip) {
    if (f->is_dir)
        return f->is_dir(f, ip);
    return (ip->mode & f->ifmt) == f->ifdir;
}
static int mode_is_device(const filsys_edition_t *f, const filsys_inode_t *ip) {
    if (f->is_device)
        return f->is_device(f, ip);
    uint32_t t = ip->mode & f->ifmt;
    return t == f->ifchr || t == f->ifblk ||
           (f->ifmpc && t == f->ifmpc) || (f->ifmpb && t == f->ifmpb);
}
static uint32_t mode_to_disk(const filsys_edition_t *f, mode_t m, int type) {
    if (f->to_disk_mode)
        return f->to_disk_mode(f, m, type);
    uint32_t base = (uint32_t)(m & 07777);
    switch (type) {
    case FILSYS_FT_DIR: return base | f->ifdir;
    case FILSYS_FT_CHR: return base | f->ifchr;
    case FILSYS_FT_BLK: return base | f->ifblk;
    default:            return f->ifreg ? (base | f->ifreg) : base;
    }
}
static uint32_t mode_chmod(const filsys_edition_t *f, uint32_t old, mode_t m) {
    if (f->chmod_mode)
        return f->chmod_mode(f, old, m);
    return (old & f->ifmt) | ((uint32_t)m & 07777);
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
    filsys_edition_t fmt = filsys_getformat(edition);
    if (!fmt.ops)
        return -EINVAL;
    filsys_t *fs = calloc(1, sizeof(*fs));
    if (!fs)
        return -ENOMEM;
    fs->ver = edition;
    fs->fmt = fmt;
    fs->ops = fmt.ops;
    fs->uid = uid;
    fs->gid = gid;
    fs->readonly = readonly;
    fs->fs = calloc(1, fmt.state_size);
    if (!fs->fs) {
        free(fs);
        return -ENOMEM;
    }
    int rc = fs->ops->open(fs->fs, path, readonly, &fmt, offset);
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
    return fs->readonly;
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
    st->st_mode  = mode_to_posix(&fs->fmt, ip);
    st->st_nlink = ip->nlink;
    st->st_uid   = fs->uid;
    st->st_gid   = fs->gid;
    st->st_size  = ip->size;
    if (mode_is_device(&fs->fmt, ip))
        st->st_rdev = ip->addr[0];
    st->st_atime   = ip->atime;
    st->st_mtime   = ip->mtime;
    st->st_ctime   = ip->ctime;
    st->st_blksize = fs->ops->blocksize(fs->fs);
    st->st_blocks  = (ip->size + st->st_blksize - 1) / st->st_blksize;
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
        uint32_t bsize = fs->ops->blocksize(fs->fs);
        uint32_t first_blk = oldsize ? (oldsize - 1) / bsize + 1 : 0;
        itrunc_from(fs, &ip, first_blk);
        write_inode(fs, ino, &ip);
    }
    return (int)n;
}

int filsys_create(filsys_t *fs, const char *path, mode_t mode, uid_t uid, gid_t gid) {
    char dir[PATH_MAX], name[64];
    int rc = split_path(path, dir, sizeof(dir), name, fs->fmt.max_namlen + 1);
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
    nip.mode = mode_to_disk(&fs->fmt, mode, FILSYS_FT_REG);
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
    char dir[PATH_MAX], name[64];
    int rc = split_path(path, dir, sizeof(dir), name, fs->fmt.max_namlen + 1);
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
    nip.mode = mode_to_disk(&fs->fmt, mode, FILSYS_FT_DIR);
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
    /* A FIFO has no on-disk type in any of these editions (named pipes arrived
     * in System III); reject it rather than deposit a bogus char device.  A
     * regular file is accepted: OpenBSD's VFS routes O_CREAT through mknod(2)
     * rather than a separate create callback, and Linux never does, so this is
     * one behaviour for both platforms. */
    int ischr = (mode & S_IFMT) == S_IFCHR;
    int isblk = (mode & S_IFMT) == S_IFBLK;
    int isreg = (mode & S_IFMT) == S_IFREG;
    if (!ischr && !isblk && !isreg)
        return -EPERM;
    if (isreg)
        return filsys_create(fs, path, mode & 07777, uid, gid);
    /* V7 device numbers are 8-bit major + 8-bit minor packed into one word.
     * Reject rather than mask: a modern major like 300 would otherwise
     * silently become 44. */
    if (major(rdev) > 255 || minor(rdev) > 255)
        return -EINVAL;

    char dir[PATH_MAX], name[64];
    int rc = split_path(path, dir, sizeof(dir), name, fs->fmt.max_namlen + 1);
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
    nip.mode = mode_to_disk(&fs->fmt, mode, isblk ? FILSYS_FT_BLK : FILSYS_FT_CHR);
    nip.nlink = 1;
    nip.uid = (int16_t)uid;
    nip.gid = (int16_t)gid;
    nip.atime = nip.mtime = nip.ctime = (uint32_t)time(NULL);
    /* Device number: (major<<8)|minor, stored in di_addr[0]. */
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
    char dir[PATH_MAX], name[64];
    int rc = split_path(path, dir, sizeof(dir), name, fs->fmt.max_namlen + 1);
    if (rc) return rc;
    filsys_inode_t ip;
    uint32_t ino;
    rc = lookup(fs, path, &ino, &ip);
    if (rc) return rc;
    if (mode_is_dir(&fs->fmt, &ip)) return -EISDIR;
    return do_unlink(fs, dir, name);
}

int filsys_rmdir(filsys_t *fs, const char *path) {
    char dir[PATH_MAX], name[64];
    int rc = split_path(path, dir, sizeof(dir), name, fs->fmt.max_namlen + 1);
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
    if (!mode_is_dir(&fs->fmt, &tip)) return -ENOTDIR;
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
    char dir[PATH_MAX], name[64];
    int rc = split_path(dst, dir, sizeof(dir), name, fs->fmt.max_namlen + 1);
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

    int isdir = mode_is_dir(&fs->fmt, &sip);

    char fdir[PATH_MAX], fname[64];
    split_path(from, fdir, sizeof(fdir), fname, fs->fmt.max_namlen + 1);
    char tdir[PATH_MAX], tname[64];
    rc = split_path(to, tdir, sizeof(tdir), tname, fs->fmt.max_namlen + 1);
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
        if (mode_is_dir(&fs->fmt, &tip)) {
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
        uint32_t bsize = fs->ops->blocksize(fs->fs);
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
                uint8_t zeros[V7_MAXBSIZE] = {0};  /* up to the largest block size */
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
    ip.mode = mode_chmod(&fs->fmt, ip.mode, mode);
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
    st->f_bsize = st->f_frsize = fs->ops->blocksize(fs->fs);
    fs->ops->statfs(fs->fs, st);
    st->f_namemax = fs->fmt.max_namlen;
    return 0;
}
