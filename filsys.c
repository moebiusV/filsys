/* filsys.c - version-agnostic access layer for Research Unix filesystem
 * images.  Dispatches to the internal v6fs/v7fs backends; this is the library
 * behind both mount.filsys (FUSE) and the standalone tools.
 *
 * SPDX-License-Identifier: ISC
 */
#include <config.h>
#include "filsys.h"
#include "v6fs.h"
#include "v7fs.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysmacros.h>
#include <time.h>

struct filsys {
    int ver;
    int uid, gid;              /* reported ownership (default: the mounting user) */
    union { v6fs_t v6; v7fs_t v7; } u;
};

/* ---- mode conversion (edition-aware) ------------------------------------ */

static mode_t to_posix_mode(int ver, uint16_t mode) {
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

/* ---- dispatch wrappers (internal) ---------------------------------------- */

static int read_inode(filsys_t *fs, uint32_t ino, filsys_inode_t *ip) {
    if (fs->ver == FILSYS_V6) return v6fs_read_inode(&fs->u.v6, ino, (v6_inode_t *)ip);
    return v7fs_read_inode(&fs->u.v7, ino, (v7_inode_t *)ip);
}
static int write_inode(filsys_t *fs, uint32_t ino, const filsys_inode_t *ip) {
    if (fs->ver == FILSYS_V6) return v6fs_write_inode(&fs->u.v6, ino, (const v6_inode_t *)ip);
    return v7fs_write_inode(&fs->u.v7, ino, (const v7_inode_t *)ip);
}
static int ialloc(filsys_t *fs, uint32_t *ino) {
    if (fs->ver == FILSYS_V6) return v6fs_ialloc(&fs->u.v6, ino);
    return v7fs_ialloc(&fs->u.v7, ino);
}
static void ifree(filsys_t *fs, uint32_t ino) {
    if (fs->ver == FILSYS_V6) v6fs_ifree(&fs->u.v6, ino);
    else v7fs_ifree(&fs->u.v7, ino);
}
static int itrunc(filsys_t *fs, filsys_inode_t *ip) {
    if (fs->ver == FILSYS_V6) return v6fs_itrunc(&fs->u.v6, (v6_inode_t *)ip);
    return v7fs_itrunc(&fs->u.v7, (v7_inode_t *)ip);
}
static int itrunc_from(filsys_t *fs, filsys_inode_t *ip, uint32_t first_blk) {
    if (fs->ver == FILSYS_V6) return v6fs_itrunc_from(&fs->u.v6, (v6_inode_t *)ip, first_blk);
    return v7fs_itrunc_from(&fs->u.v7, (v7_inode_t *)ip, first_blk);
}
static ssize_t file_read(filsys_t *fs, filsys_inode_t *ip, uint8_t *buf, size_t sz, off_t off) {
    if (fs->ver == FILSYS_V6) return v6fs_file_read(&fs->u.v6, (v6_inode_t *)ip, buf, sz, off);
    return v7fs_file_read(&fs->u.v7, (v7_inode_t *)ip, buf, sz, off);
}
static ssize_t file_write(filsys_t *fs, filsys_inode_t *ip, const uint8_t *buf, size_t sz, off_t off) {
    if (fs->ver == FILSYS_V6) return v6fs_file_write(&fs->u.v6, (v6_inode_t *)ip, buf, sz, off);
    return v7fs_file_write(&fs->u.v7, (v7_inode_t *)ip, buf, sz, off);
}
static int dir_read(filsys_t *fs, filsys_inode_t *ip, filsys_dirent_t **e, size_t *n) {
    if (fs->ver == FILSYS_V6) return v6fs_dir_read(&fs->u.v6, (v6_inode_t *)ip, (v6_dirent_t **)e, n);
    return v7fs_dir_read(&fs->u.v7, (v7_inode_t *)ip, (v7_dirent_t **)e, n);
}
static int dir_lookup(filsys_t *fs, filsys_inode_t *ip, const char *name, uint32_t *ino) {
    if (fs->ver == FILSYS_V6) return v6fs_dir_lookup(&fs->u.v6, (v6_inode_t *)ip, name, ino);
    return v7fs_dir_lookup(&fs->u.v7, (v7_inode_t *)ip, name, ino);
}
static int dir_add(filsys_t *fs, filsys_inode_t *ip, uint32_t ino, const char *name) {
    if (fs->ver == FILSYS_V6) return v6fs_dir_add(&fs->u.v6, (v6_inode_t *)ip, ino, name);
    return v7fs_dir_add(&fs->u.v7, (v7_inode_t *)ip, ino, name);
}
static int dir_remove(filsys_t *fs, filsys_inode_t *ip, const char *name) {
    if (fs->ver == FILSYS_V6) return v6fs_dir_remove(&fs->u.v6, (v6_inode_t *)ip, name);
    return v7fs_dir_remove(&fs->u.v7, (v7_inode_t *)ip, name);
}
static int lookup(filsys_t *fs, const char *path, uint32_t *ino, filsys_inode_t *ip) {
    if (fs->ver == FILSYS_V6) return v6fs_lookup(&fs->u.v6, path, ino, (v6_inode_t *)ip);
    return v7fs_lookup(&fs->u.v7, path, ino, (v7_inode_t *)ip);
}
static int bmap(filsys_t *fs, filsys_inode_t *ip, uint32_t lbn, int create, uint32_t *bno) {
    if (fs->ver == FILSYS_V6) return v6fs_bmap(&fs->u.v6, (v6_inode_t *)ip, lbn, create, bno);
    return v7fs_bmap(&fs->u.v7, (v7_inode_t *)ip, lbn, create, bno);
}
static int read_block(filsys_t *fs, uint32_t bno, uint8_t *buf) {
    if (fs->ver == FILSYS_V6) return v6fs_read_block(&fs->u.v6, bno, buf);
    return v7fs_read_block(&fs->u.v7, bno, buf);
}
static int write_block(filsys_t *fs, uint32_t bno, const uint8_t *buf) {
    if (fs->ver == FILSYS_V6) return v6fs_write_block(&fs->u.v6, bno, buf);
    return v7fs_write_block(&fs->u.v7, bno, buf);
}

/* Largest file the selected edition can address, in bytes.  V6 (ILARG): 7
 * single-indirect (256 each) + 1 double-indirect (256^2).  V7/32V: 10 direct
 * + single + double + triple indirect (128 each). */
static uint64_t maxfile(const filsys_t *fs) {
    if (fs->ver == FILSYS_V6) {
        uint64_t n = V6_NINDIR;
        return (7u * n + n * n) * V6_BSIZE;
    }
    uint64_t n = V7_NINDIR;
    return ((uint64_t)V7_NDADDR + n + n*n + n*n*n) * V7_BSIZE;
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
    int rc;
    if (edition == FILSYS_V6)
        rc = v6fs_open(&fs->u.v6, path, readonly, offset);
    else
        rc = v7fs_open(&fs->u.v7, path, readonly, edition == FILSYS_32V, offset);
    if (rc) {
        free(fs);
        return rc;
    }
    *out = fs;
    return 0;
}

void filsys_close(filsys_t *fs) {
    if (!fs)
        return;
    if (fs->ver == FILSYS_V6) v6fs_close(&fs->u.v6);
    else v7fs_close(&fs->u.v7);
    free(fs);
}

int filsys_sync(filsys_t *fs) {
    if (fs->ver == FILSYS_V6) return v6fs_sync(&fs->u.v6);
    return v7fs_sync(&fs->u.v7);
}

int filsys_is_readonly(const filsys_t *fs) {
    return fs->ver == FILSYS_V6 ? fs->u.v6.readonly : fs->u.v7.readonly;
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
    if (fs->ver == FILSYS_V6) {
        v6_check_t rep;
        return v6fs_check(&fs->u.v6, &rep) ? -1 : 0;
    }
    v7_check_t rep;
    return v7fs_check(&fs->u.v7, &rep, 0) ? -1 : 0;
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
    uint16_t t = fs->ver == FILSYS_V6 ? (ip->mode & V6_IFMT) : (ip->mode & V7_IFMT);
    int isdev = (fs->ver == FILSYS_V6) ? (t == V6_IFCHR || t == V6_IFBLK)
                                   : (t == V7_IFCHR || t == V7_IFBLK ||
                                      t == V7_IFMPC || t == V7_IFMPB);
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
    return (int)file_write(fs, &ip, (const uint8_t *)buf, size, off);
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
    nip.mode = perm_of(mode) | (fs->ver == FILSYS_V6 ? 0 : V7_IFREG);
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
    nip.mode = perm_of(mode) | (fs->ver == FILSYS_V6 ? V6_IFDIR : V7_IFDIR);
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
    uint16_t t = fs->ver == FILSYS_V6 ? (ip.mode & V6_IFMT) : (ip.mode & V7_IFMT);
    int isdir = (fs->ver == FILSYS_V6) ? (t == V6_IFDIR) : (t == V7_IFDIR);
    if (isdir) return -EISDIR;
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
    uint16_t t = fs->ver == FILSYS_V6 ? (tip.mode & V6_IFMT) : (tip.mode & V7_IFMT);
    int isdir = (fs->ver == FILSYS_V6) ? (t == V6_IFDIR) : (t == V7_IFDIR);
    if (!isdir) return -ENOTDIR;
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

    uint16_t fmask = fs->ver == FILSYS_V6 ? V6_IFMT : V7_IFMT;
    uint16_t dirmode = fs->ver == FILSYS_V6 ? V6_IFDIR : V7_IFDIR;
    int isdir = (sip.mode & fmask) == dirmode;

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
        if ((tip.mode & fmask) == dirmode) {
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
        uint32_t last = newsize ? (newsize - 1) / 512 : 0;
        uint32_t off  = newsize % 512;
        if (off) {
            /* Zero the partial tail through the block layer, not file_write:
             * file_write would bump ip.size past the old EOF and, if the tail
             * block is a hole, allocate a block only to discard it. */
            uint32_t bno;
            rc = bmap(fs, &ip, last, 0, &bno);
            if (rc) return rc;
            if (bno) {
                uint8_t blk[512];
                rc = read_block(fs, bno, blk);
                if (rc) return rc;
                memset(blk + off, 0, 512 - off);
                rc = write_block(fs, bno, blk);
                if (rc) return rc;
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
    uint16_t fmask = fs->ver == FILSYS_V6 ? V6_IFMT : V7_IFMT;
    ip.mode = (ip.mode & fmask) | (uint16_t)(mode & 07777);
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
        st->f_blocks = fs->u.v6.fsize;
        st->f_bfree = st->f_bavail = fs->u.v6.tfree;
        st->f_files = v6_maxino(fs->u.v6.isize);
        st->f_ffree = fs->u.v6.tinode;
    } else {
        st->f_blocks = fs->u.v7.fsize;
        st->f_bfree = st->f_bavail = fs->u.v7.tfree;
        st->f_files = (fs->u.v7.isize - 2) * 8;
        st->f_ffree = fs->u.v7.tinode;
    }
    st->f_namemax = 14;
    return 0;
}
