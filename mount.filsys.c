/* filsys 1.0.0 - 2026-08-26 - Copyright (C) 2026 David Walther */
/* SPDX-License-Identifier: ISC */
/* mount.filsys.c - mount a Research Unix filesystem image (PDP-11) as a FUSE
 * filesystem, selecting the on-disk edition at run time.
 *
 * One binary, every edition we care about:
 *
 *   -v 4     V4 format (byte-identical to V5/V6)
 *   -v 5     V5 format (byte-identical to V4/V6)
 *   -v 6     V6 format
 *   -v 7     V7 format
 *   -v 32    32V format (V7 for the VAX; little-endian 32-bit fields + addresses)
 *
 * The on-disk layout is the 1969 Thompson/Canaday/Ritchie design - a flat
 * i-list at a fixed offset, directories as ordinary files of 16-byte entries,
 * and device files - carried essentially unchanged from the PDP-7 through V6,
 * then widened in V7 (64-byte inode, 24-bit block numbers).  See filsys.5.
 *
 * Usage:
 *     mount.filsys -v <4|5|6|7|32> [-o offset=N[,version=N][,uid=N,gid=N,...]]
 *                    [-r] [-f] [-d] <image> <mountpoint>
 *     mount.filsys -v <4|5|6|7|32> [-o offset=N] -c <image>   # integrity check
 *
 * `-o offset=N` mounts a filesystem that lives at byte offset N within the
 * file (a partition of a larger disk image), instead of one at block 0.
 * `-o version=N` selects the edition (so `mount -t filsys` can pass it in `-o`
 * rather than `-v`, which mount(8) has no generic way to supply).
 * `-o uid=N,gid=N` override the reported ownership (default: the mounting
 * user, so a nested mount point is writable).  Any other -o option is passed
 * through to FUSE (e.g. allow_other).  Installed as sbin/mount.filsys, so
 * `mount -t filsys device dir -o version=7,offset=N` works.
 */
#include <config.h>
#include "v6fs.h"
#include "v7fs.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <fuse.h>

#define FILSYS_V6 6
#define FILSYS_V7 7
#define FILSYS_32V 32

/* v6_inode_t and v7_inode_t share the same decoded layout (both carry
 * addr[13]); use the v7 type as the common in-core inode. */
typedef v7_inode_t  filsys_inode_t;
typedef v7_dirent_t filsys_dirent_t;

typedef struct {
    int ver;
    int uid, gid;              /* reported ownership (default: the mounting user) */
    union { v6fs_t v6; v7fs_t v7; } u;
} filsys_t;

static filsys_t *FILSYS(void) {
    return (filsys_t *)fuse_get_context()->private_data;
}

/* ---- mode conversion (edition-aware) ------------------------------------ */

static mode_t filsys_to_posix_mode(int ver, uint16_t mode) {
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

static uint16_t filsys_perm_of(mode_t m) {
    return (uint16_t)(m & 07777);
}

/* ---- dispatch wrappers --------------------------------------------------- */

static int filsys_fs_open(filsys_t *k, const char *path, int readonly, uint64_t offset) {
    if (k->ver == FILSYS_V6) return v6fs_open(&k->u.v6, path, readonly, offset);
    return v7fs_open(&k->u.v7, path, readonly, k->ver == FILSYS_32V, offset);
}
static void filsys_close(filsys_t *k) {
    if (k->ver == FILSYS_V6) v6fs_close(&k->u.v6);
    else v7fs_close(&k->u.v7);
}
static int filsys_read_inode(filsys_t *k, uint32_t ino, filsys_inode_t *ip) {
    if (k->ver == FILSYS_V6) return v6fs_read_inode(&k->u.v6, ino, (v6_inode_t *)ip);
    return v7fs_read_inode(&k->u.v7, ino, ip);
}
static int filsys_write_inode(filsys_t *k, uint32_t ino, const filsys_inode_t *ip) {
    if (k->ver == FILSYS_V6) return v6fs_write_inode(&k->u.v6, ino, (const v6_inode_t *)ip);
    return v7fs_write_inode(&k->u.v7, ino, ip);
}
static int filsys_ialloc(filsys_t *k, uint32_t *ino) {
    if (k->ver == FILSYS_V6) return v6fs_ialloc(&k->u.v6, ino);
    return v7fs_ialloc(&k->u.v7, ino);
}
static void filsys_ifree(filsys_t *k, uint32_t ino) {
    if (k->ver == FILSYS_V6) v6fs_ifree(&k->u.v6, ino);
    else v7fs_ifree(&k->u.v7, ino);
}
static int filsys_itrunc(filsys_t *k, filsys_inode_t *ip) {
    if (k->ver == FILSYS_V6) return v6fs_itrunc(&k->u.v6, (v6_inode_t *)ip);
    return v7fs_itrunc(&k->u.v7, ip);
}
static int filsys_itrunc_from(filsys_t *k, filsys_inode_t *ip, uint32_t first_blk) {
    if (k->ver == FILSYS_V6) return v6fs_itrunc_from(&k->u.v6, (v6_inode_t *)ip, first_blk);
    return v7fs_itrunc_from(&k->u.v7, ip, first_blk);
}
static ssize_t filsys_file_read(filsys_t *k, filsys_inode_t *ip, uint8_t *buf, size_t sz, off_t off) {
    if (k->ver == FILSYS_V6) return v6fs_file_read(&k->u.v6, (v6_inode_t *)ip, buf, sz, off);
    return v7fs_file_read(&k->u.v7, ip, buf, sz, off);
}
static ssize_t filsys_file_write(filsys_t *k, filsys_inode_t *ip, const uint8_t *buf, size_t sz, off_t off) {
    if (k->ver == FILSYS_V6) return v6fs_file_write(&k->u.v6, (v6_inode_t *)ip, buf, sz, off);
    return v7fs_file_write(&k->u.v7, ip, buf, sz, off);
}
static int filsys_dir_read(filsys_t *k, filsys_inode_t *ip, filsys_dirent_t **e, size_t *n) {
    if (k->ver == FILSYS_V6) return v6fs_dir_read(&k->u.v6, (v6_inode_t *)ip, (v6_dirent_t **)e, n);
    return v7fs_dir_read(&k->u.v7, ip, e, n);
}
static int filsys_dir_lookup(filsys_t *k, filsys_inode_t *ip, const char *name, uint32_t *ino) {
    if (k->ver == FILSYS_V6) return v6fs_dir_lookup(&k->u.v6, (v6_inode_t *)ip, name, ino);
    return v7fs_dir_lookup(&k->u.v7, ip, name, ino);
}
static int filsys_dir_add(filsys_t *k, filsys_inode_t *ip, uint32_t ino, const char *name) {
    if (k->ver == FILSYS_V6) return v6fs_dir_add(&k->u.v6, (v6_inode_t *)ip, ino, name);
    return v7fs_dir_add(&k->u.v7, ip, ino, name);
}
static int filsys_dir_remove(filsys_t *k, filsys_inode_t *ip, const char *name) {
    if (k->ver == FILSYS_V6) return v6fs_dir_remove(&k->u.v6, (v6_inode_t *)ip, name);
    return v7fs_dir_remove(&k->u.v7, ip, name);
}
static int filsys_lookup(filsys_t *k, const char *path, uint32_t *ino, filsys_inode_t *ip) {
    if (k->ver == FILSYS_V6) return v6fs_lookup(&k->u.v6, path, ino, (v6_inode_t *)ip);
    return v7fs_lookup(&k->u.v7, path, ino, ip);
}
static int filsys_check(filsys_t *k, v6_check_t *rep) {
    if (k->ver == FILSYS_V6) return v6fs_check(&k->u.v6, rep);
    v7_check_t r;
    return v7fs_check(&k->u.v7, &r, 0);   /* no salvage from the mount tool */
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

static void fill_stat(filsys_t *k, const filsys_inode_t *ip, struct stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_ino   = ip->ino;
    st->st_mode  = filsys_to_posix_mode(k->ver, ip->mode);
    st->st_nlink = ip->nlink;
    st->st_uid   = k->uid;
    st->st_gid   = k->gid;
    st->st_size  = ip->size;
    uint16_t t = k->ver == FILSYS_V6 ? (ip->mode & V6_IFMT) : (ip->mode & V7_IFMT);
    int isdev = (k->ver == FILSYS_V6) ? (t == V6_IFCHR || t == V6_IFBLK)
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

/* ---- callbacks ----------------------------------------------------------- */

static int filsys_getattr(const char *path, struct stat *st, struct fuse_file_info *fi) {
    (void)fi;
    filsys_t *k = FILSYS();
    filsys_inode_t ip;
    uint32_t ino;
    int rc = filsys_lookup(k, path, &ino, &ip);
    if (rc) return rc;
    fill_stat(k, &ip, st);
    return 0;
}

static int filsys_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t off, struct fuse_file_info *fi, enum fuse_readdir_flags fl) {
    (void)off; (void)fi; (void)fl;
    filsys_t *k = FILSYS();
    filsys_inode_t ip;
    uint32_t ino;
    int rc = filsys_lookup(k, path, &ino, &ip);
    if (rc) return rc;
    filsys_dirent_t *ents = NULL;
    size_t count = 0;
    rc = filsys_dir_read(k, &ip, &ents, &count);
    if (rc) return rc;
    for (size_t i = 0; i < count; i++) {
        struct stat st;
        filsys_inode_t eip;
        if (filsys_read_inode(k, ents[i].ino, &eip) == 0)
            fill_stat(k, &eip, &st);
        else
            memset(&st, 0, sizeof(st));
        if (filler(buf, ents[i].name, &st, 0, 0))
            break;
    }
    free(ents);
    return 0;
}

static int filsys_open(const char *path, struct fuse_file_info *fi) {
    filsys_t *k = FILSYS();
    filsys_inode_t ip;
    uint32_t ino;
    int rc = filsys_lookup(k, path, &ino, &ip);
    if (rc) return rc;
    uint16_t t = k->ver == FILSYS_V6 ? (ip.mode & V6_IFMT) : (ip.mode & V7_IFMT);
    int isdir = (k->ver == FILSYS_V6) ? (t == V6_IFDIR) : (t == V7_IFDIR);
    if (isdir) return -EISDIR;
    if ((fi->flags & O_ACCMODE) != O_RDONLY && (k->ver == FILSYS_V6 ? k->u.v6.readonly : k->u.v7.readonly))
        return -EROFS;
    return 0;
}

static int filsys_read(const char *path, char *buf, size_t size, off_t off, struct fuse_file_info *fi) {
    (void)fi;
    filsys_t *k = FILSYS();
    filsys_inode_t ip;
    uint32_t ino;
    int rc = filsys_lookup(k, path, &ino, &ip);
    if (rc) return rc;
    return (int)filsys_file_read(k, &ip, (uint8_t *)buf, size, off);
}

static int filsys_write(const char *path, const char *buf, size_t size, off_t off, struct fuse_file_info *fi) {
    (void)fi;
    filsys_t *k = FILSYS();
    filsys_inode_t ip;
    uint32_t ino;
    int rc = filsys_lookup(k, path, &ino, &ip);
    if (rc) return rc;
    return (int)filsys_file_write(k, &ip, (const uint8_t *)buf, size, off);
}

static int filsys_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)fi;
    filsys_t *k = FILSYS();
    char dir[PATH_MAX], name[15];
    int rc = split_path(path, dir, sizeof(dir), name, sizeof(name));
    if (rc) return rc;
    filsys_inode_t ddir;
    uint32_t dino;
    rc = filsys_lookup(k, dir, &dino, &ddir);
    if (rc) return rc;
    uint32_t nino;
    rc = filsys_ialloc(k, &nino);
    if (rc) return rc;
    const struct fuse_context *ctx = fuse_get_context();
    filsys_inode_t nip;
    memset(&nip, 0, sizeof(nip));
    nip.ino = nino;
    nip.mode = filsys_perm_of(mode) | (k->ver == FILSYS_V6 ? 0 : V7_IFREG);
    nip.nlink = 1;
    nip.uid = (int16_t)ctx->uid;
    nip.gid = (int16_t)ctx->gid;
    nip.atime = nip.mtime = nip.ctime = (uint32_t)time(NULL);
    filsys_write_inode(k, nino, &nip);
    return filsys_dir_add(k, &ddir, nino, name);
}

static int filsys_mkdir(const char *path, mode_t mode) {
    filsys_t *k = FILSYS();
    char dir[PATH_MAX], name[15];
    int rc = split_path(path, dir, sizeof(dir), name, sizeof(name));
    if (rc) return rc;
    filsys_inode_t ddir;
    uint32_t dino;
    rc = filsys_lookup(k, dir, &dino, &ddir);
    if (rc) return rc;
    uint32_t nino;
    rc = filsys_ialloc(k, &nino);
    if (rc) return rc;
    const struct fuse_context *ctx = fuse_get_context();
    filsys_inode_t nip;
    memset(&nip, 0, sizeof(nip));
    nip.ino = nino;
    nip.mode = filsys_perm_of(mode) | (k->ver == FILSYS_V6 ? V6_IFDIR : V7_IFDIR);
    nip.nlink = 2;
    nip.uid = (int16_t)ctx->uid;
    nip.gid = (int16_t)ctx->gid;
    nip.atime = nip.mtime = nip.ctime = (uint32_t)time(NULL);
    filsys_write_inode(k, nino, &nip);
    filsys_dir_add(k, &nip, nino, ".");
    filsys_dir_add(k, &nip, dino, "..");
    rc = filsys_dir_add(k, &ddir, nino, name);
    if (rc) return rc;
    ddir.nlink++;
    filsys_write_inode(k, dino, &ddir);
    return 0;
}

static int filsys_mknod(const char *path, mode_t mode, dev_t rdev) {
    filsys_t *k = FILSYS();
    char dir[PATH_MAX], name[15];
    int rc = split_path(path, dir, sizeof(dir), name, sizeof(name));
    if (rc) return rc;
    filsys_inode_t ddir;
    uint32_t dino;
    rc = filsys_lookup(k, dir, &dino, &ddir);
    if (rc) return rc;
    uint32_t nino;
    rc = filsys_ialloc(k, &nino);
    if (rc) return rc;
    const struct fuse_context *ctx = fuse_get_context();
    filsys_inode_t nip;
    memset(&nip, 0, sizeof(nip));
    nip.ino = nino;
    int isblk = (mode & S_IFMT) == S_IFBLK;
    if (k->ver == FILSYS_V6)
        nip.mode = (isblk ? V6_IFBLK : V6_IFCHR) | filsys_perm_of(mode);
    else
        nip.mode = (isblk ? V7_IFBLK : V7_IFCHR) | filsys_perm_of(mode);
    nip.nlink = 1;
    nip.uid = (int16_t)ctx->uid;
    nip.gid = (int16_t)ctx->gid;
    nip.atime = nip.mtime = nip.ctime = (uint32_t)time(NULL);
    /* V7 device number: (major<<8)|minor (8-bit each), stored in di_addr[0]. */
    nip.addr[0] = (uint32_t)(((major(rdev) & 0xff) << 8) | (minor(rdev) & 0xff));
    filsys_write_inode(k, nino, &nip);
    return filsys_dir_add(k, &ddir, nino, name);
}

static int filsys_do_unlink(filsys_t *k, const char *dirpath, const char *name) {
    filsys_inode_t ddir;
    uint32_t dino;
    int rc = filsys_lookup(k, dirpath, &dino, &ddir);
    if (rc) return rc;
    uint32_t ino;
    rc = filsys_dir_lookup(k, &ddir, name, &ino);
    if (rc) return rc;
    filsys_inode_t ip;
    if (filsys_read_inode(k, ino, &ip)) return -EIO;
    rc = filsys_dir_remove(k, &ddir, name);
    if (rc) return rc;
    ip.nlink--;
    if (ip.nlink <= 0) {
        filsys_itrunc(k, &ip);
        ip.mode = 0;
        filsys_ifree(k, ino);
    }
    filsys_write_inode(k, ino, &ip);
    return 0;
}

static int filsys_unlink(const char *path) {
    filsys_t *k = FILSYS();
    char dir[PATH_MAX], name[15];
    int rc = split_path(path, dir, sizeof(dir), name, sizeof(name));
    if (rc) return rc;
    filsys_inode_t ip;
    uint32_t ino;
    rc = filsys_lookup(k, path, &ino, &ip);
    if (rc) return rc;
    uint16_t t = k->ver == FILSYS_V6 ? (ip.mode & V6_IFMT) : (ip.mode & V7_IFMT);
    int isdir = (k->ver == FILSYS_V6) ? (t == V6_IFDIR) : (t == V7_IFDIR);
    if (isdir) return -EISDIR;
    return filsys_do_unlink(k, dir, name);
}

static int filsys_rmdir(const char *path) {
    filsys_t *k = FILSYS();
    char dir[PATH_MAX], name[15];
    int rc = split_path(path, dir, sizeof(dir), name, sizeof(name));
    if (rc) return rc;
    filsys_inode_t ddir;
    uint32_t dino;
    rc = filsys_lookup(k, dir, &dino, &ddir);
    if (rc) return rc;
    uint32_t ino;
    rc = filsys_dir_lookup(k, &ddir, name, &ino);
    if (rc) return rc;
    filsys_inode_t tip;
    if (filsys_read_inode(k, ino, &tip)) return -EIO;
    uint16_t t = k->ver == FILSYS_V6 ? (tip.mode & V6_IFMT) : (tip.mode & V7_IFMT);
    int isdir = (k->ver == FILSYS_V6) ? (t == V6_IFDIR) : (t == V7_IFDIR);
    if (!isdir) return -ENOTDIR;
    filsys_dirent_t *ents = NULL;
    size_t count = 0;
    if (filsys_dir_read(k, &tip, &ents, &count)) return -EIO;
    for (size_t i = 0; i < count; i++)
        if (strcmp(ents[i].name, ".") && strcmp(ents[i].name, "..")) { free(ents); return -ENOTEMPTY; }
    free(ents);
    rc = filsys_dir_remove(k, &ddir, name);
    if (rc) return rc;
    ddir.nlink--;
    filsys_write_inode(k, dino, &ddir);
    filsys_itrunc(k, &tip);
    tip.mode = 0;
    filsys_ifree(k, ino);
    filsys_write_inode(k, ino, &tip);
    return 0;
}

static int filsys_do_link(filsys_t *k, const char *dst, uint32_t src_ino) {
    char dir[PATH_MAX], name[15];
    int rc = split_path(dst, dir, sizeof(dir), name, sizeof(name));
    if (rc) return rc;
    filsys_inode_t ddir;
    uint32_t dino;
    rc = filsys_lookup(k, dir, &dino, &ddir);
    if (rc) return rc;
    rc = filsys_dir_add(k, &ddir, src_ino, name);
    if (rc) return rc;
    filsys_inode_t ip;
    if (filsys_read_inode(k, src_ino, &ip)) return -EIO;
    ip.nlink++;
    filsys_write_inode(k, src_ino, &ip);
    return 0;
}

static int filsys_link(const char *from, const char *to) {
    filsys_t *k = FILSYS();
    filsys_inode_t ip;
    uint32_t ino;
    int rc = filsys_lookup(k, from, &ino, &ip);
    if (rc) return rc;
    /* V7's link(2) lets the superuser hard-link a directory, and the staging
     * tool's mounting user is that superuser.  filsys_do_link only bumps the
     * directory's nlink; its ".." still points at the original parent, so a
     * cycle is the caller's own foot to shoot -- faithful to V7. */
    return filsys_do_link(k, to, ino);
}

static int filsys_rename(const char *from, const char *to, unsigned int flags) {
    if (flags) return -EINVAL;
    if (!strcmp(from, to)) return 0;
    filsys_t *k = FILSYS();
    filsys_inode_t sip;
    uint32_t sino;
    int rc = filsys_lookup(k, from, &sino, &sip);
    if (rc) return rc;

    uint16_t fmask = k->ver == FILSYS_V6 ? V6_IFMT : V7_IFMT;
    uint16_t dirmode = k->ver == FILSYS_V6 ? V6_IFDIR : V7_IFDIR;
    int isdir = (sip.mode & fmask) == dirmode;

    char fdir[PATH_MAX], fname[15];
    split_path(from, fdir, sizeof(fdir), fname, sizeof(fname));
    char tdir[PATH_MAX], tname[15];
    rc = split_path(to, tdir, sizeof(tdir), tname, sizeof(tname));
    if (rc) return rc;

    filsys_inode_t tdirip;
    uint32_t tdino;
    rc = filsys_lookup(k, tdir, &tdino, &tdirip);
    if (rc) return rc;

    /* Overwrite an existing target: refuse to replace a non-empty directory,
     * and do not let a directory be renamed over one. */
    uint32_t tino;
    if (filsys_dir_lookup(k, &tdirip, tname, &tino) == 0) {
        if (tino == sino) return 0;   /* already there */
        filsys_inode_t tip;
        if (filsys_read_inode(k, tino, &tip)) return -EIO;
        if ((tip.mode & fmask) == dirmode) {
            filsys_dirent_t *ents = NULL; size_t count = 0;
            if (filsys_dir_read(k, &tip, &ents, &count)) return -EIO;
            for (size_t i = 0; i < count; i++)
                if (strcmp(ents[i].name, ".") && strcmp(ents[i].name, "..")) { free(ents); return -ENOTEMPTY; }
            free(ents);
        }
        filsys_do_unlink(k, tdir, tname);
    }

    filsys_do_link(k, to, sino);
    rc = filsys_do_unlink(k, fdir, fname);
    if (rc) return rc;

    if (isdir) {
        /* Moving a directory: fix the two parents' link counts and rewrite the
         * moved directory's '..' entry to point at its new parent. */
        if (strcmp(fdir, tdir) != 0) {
            filsys_inode_t fddir;
            uint32_t fdino;
            if (filsys_lookup(k, fdir, &fdino, &fddir) == 0) {
                if (fddir.nlink > 1) fddir.nlink--;
                filsys_write_inode(k, fdino, &fddir);
            }
            /* tdirip was read before filsys_do_link, which rewrote the target
             * directory's inode (its size grew to hold the new entry).  Re-read
             * it fresh so we bump nlink without clobbering that update. */
            if (filsys_read_inode(k, tdino, &tdirip) == 0) {
                tdirip.nlink++;
                filsys_write_inode(k, tdino, &tdirip);
            }
        }
        filsys_inode_t cip;
        if (filsys_read_inode(k, sino, &cip) == 0) {
            filsys_dir_remove(k, &cip, "..");
            filsys_dir_add(k, &cip, tdino, "..");
        }
    }
    return 0;
}

static int filsys_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)fi;
    filsys_t *k = FILSYS();
    filsys_inode_t ip;
    uint32_t ino;
    int rc = filsys_lookup(k, path, &ino, &ip);
    if (rc) return rc;
    uint16_t fmask = k->ver == FILSYS_V6 ? V6_IFMT : V7_IFMT;
    ip.mode = (ip.mode & fmask) | (uint16_t)(mode & 07777);
    ip.ctime = (uint32_t)time(NULL);
    filsys_write_inode(k, ino, &ip);
    return 0;
}

static int filsys_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi) {
    (void)fi;
    filsys_t *k = FILSYS();
    filsys_inode_t ip;
    uint32_t ino;
    int rc = filsys_lookup(k, path, &ino, &ip);
    if (rc) return rc;
    if (uid != (uid_t)-1) ip.uid = (int16_t)uid;
    if (gid != (gid_t)-1) ip.gid = (int16_t)gid;
    ip.ctime = (uint32_t)time(NULL);
    filsys_write_inode(k, ino, &ip);
    return 0;
}

static int filsys_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
    (void)fi;
    filsys_t *k = FILSYS();
    filsys_inode_t ip;
    uint32_t ino;
    int rc = filsys_lookup(k, path, &ino, &ip);
    if (rc) return rc;
    uint32_t newsize = (uint32_t)size, oldsize = ip.size;
    if (newsize == oldsize) return 0;

    /* Shrink in place: zero the partial tail of the last surviving block, free
     * the blocks strictly past the new end, and leave everything else alone.
     * (Extension is a no-op: holes read back as zero.) */
    if (newsize < oldsize) {
        uint32_t last = newsize ? (newsize - 1) / 512 : 0;
        uint32_t off  = newsize % 512;
        if (off) {
            uint8_t z[512];
            memset(z, 0, sizeof z);
            rc = filsys_file_write(k, &ip, z, 512 - off, (off_t)newsize);
            if (rc < 0) return rc;
        }
        rc = filsys_itrunc_from(k, &ip, newsize ? last + 1 : 0);
        if (rc) return rc;
    }

    ip.size = newsize;
    return filsys_write_inode(k, ino, &ip);
}

static int filsys_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi) {
    (void)fi;
    filsys_t *k = FILSYS();
    filsys_inode_t ip;
    uint32_t ino;
    int rc = filsys_lookup(k, path, &ino, &ip);
    if (rc) return rc;
    uint32_t now = (uint32_t)time(NULL);
    ip.atime = tv ? (uint32_t)tv[0].tv_sec : now;
    ip.mtime = tv ? (uint32_t)tv[1].tv_sec : now;
    ip.ctime = now;
    filsys_write_inode(k, ino, &ip);
    return 0;
}

static int filsys_statfs(const char *path, struct statvfs *st) {
    (void)path;
    filsys_t *k = FILSYS();
    memset(st, 0, sizeof(*st));
    st->f_bsize = st->f_frsize = 512;
    if (k->ver == FILSYS_V6) {
        st->f_blocks = k->u.v6.fsize;
        st->f_bfree = st->f_bavail = k->u.v6.tfree;
        st->f_files = v6_maxino(k->u.v6.isize);
        st->f_ffree = k->u.v6.tinode;
    } else {
        st->f_blocks = k->u.v7.fsize;
        st->f_bfree = st->f_bavail = k->u.v7.tfree;
        st->f_files = (k->u.v7.isize - 2) * 8;
        st->f_ffree = k->u.v7.tinode;
    }
    st->f_namemax = 14;
    return 0;
}

static struct fuse_operations filsys_ops = {
    .getattr  = filsys_getattr,
    .readdir  = filsys_readdir,
    .open     = filsys_open,
    .read     = filsys_read,
    .write    = filsys_write,
    .create   = filsys_create,
    .link     = filsys_link,
    .mkdir    = filsys_mkdir,
    .mknod    = filsys_mknod,
    .unlink   = filsys_unlink,
    .rmdir    = filsys_rmdir,
    .rename   = filsys_rename,
    .chmod    = filsys_chmod,
    .chown    = filsys_chown,
    .truncate = filsys_truncate,
    .utimens  = filsys_utimens,
    .statfs   = filsys_statfs,
};

/* ---- main ---------------------------------------------------------------- */

static void usage(const char *p) {
    fprintf(stderr,
            "usage: %s -v <4|5|6|7|32> [-o offset=N[,version=N][,uid=N][,gid=N]]\n"
            "                [-r] [-f] [-d] <image> <mountpoint>\n"
            "       %s -v <4|5|6|7|32> [-o offset=N] -c <image>   # integrity check\n",
            p, p);
}

/* Parse a -v / -o version= argument.  Returns FILSYS_V6/V7/32V, or -1. */
static int parse_version(const char *s) {
    if (s[0] == 'v' || s[0] == 'V') s++;   /* accept "v7" and "7" alike */
    if (!strcmp(s, "4") || !strcmp(s, "5") || !strcmp(s, "6"))
        return FILSYS_V6;
    if (!strcmp(s, "7"))
        return FILSYS_V7;
    if (!strcmp(s, "32") || !strcmp(s, "32v"))
        return FILSYS_32V;
    return -1;
}

int main(int argc, char *argv[]) {
    int ver = FILSYS_V7, readonly = 0, foreground = 0, debug = 0, check = 0;
    uint64_t offset = 0;
    int uid = -1, gid = -1;   /* -1 = report as the mounting user */
    char fuse_opts[512] = ""; /* -o options passed through to FUSE (allow_other, ...) */
    int c;

    /* getopt, not a hand-rolled loop, so the mount(8) argument order
     * `mount.filsys image dir -o opts` parses (GNU getopt permutes operands to
     * the front, so trailing options are fine). */
    while ((c = getopt(argc, argv, "v:o:rfdc")) != -1) {
        switch (c) {
        case 'v': {
            int v = parse_version(optarg);
            if (v < 0) {
                fprintf(stderr, "%s: unknown Unix version \"%s\" (4, 5, 6, 7, 32)\n",
                        argv[0], optarg);
                usage(argv[0]);
                return 2;
            }
            ver = v;
            break;
        }
        case 'o': {
            char *opts = strdup(optarg);
            int bad = 0;
            for (char *tok = strtok(opts, ","); tok; tok = strtok(NULL, ",")) {
                if (!strncmp(tok, "offset=", 7)) {
                    char *end = NULL;
                    offset = strtoull(tok + 7, &end, 0);
                    if (!end || *end) { bad = 1; break; }
                } else if (!strncmp(tok, "version=", 8)) {
                    int v = parse_version(tok + 8);
                    if (v < 0) bad = 1; else ver = v;
                } else if (!strncmp(tok, "uid=", 4)) {
                    uid = atoi(tok + 4);
                } else if (!strncmp(tok, "gid=", 4)) {
                    gid = atoi(tok + 4);
                } else {
                    /* pass anything else through to FUSE (allow_other, ...) */
                    if (fuse_opts[0]) strncat(fuse_opts, ",", sizeof(fuse_opts) - strlen(fuse_opts) - 1);
                    strncat(fuse_opts, tok, sizeof(fuse_opts) - strlen(fuse_opts) - 1);
                }
            }
            free(opts);
            if (bad) { usage(argv[0]); return 2; }
            break;
        }
        case 'r': readonly = 1; break;
        case 'f': foreground = 1; break;
        case 'd': debug = 1; break;
        case 'c': check = 1; break;
        default: usage(argv[0]); return 2;
        }
    }
    if (check && (argc - optind != 1)) { fprintf(stderr, "usage: %s -c <image>\n", argv[0]); return 2; }
    if (!check && (argc - optind != 2)) { usage(argv[0]); return 2; }
    const char *image = argv[optind];
    const char *mountpoint = check ? NULL : argv[optind + 1];

    filsys_t k;
    memset(&k, 0, sizeof(k));
    k.ver = ver;
    k.uid = uid >= 0 ? uid : (int)getuid();
    k.gid = gid >= 0 ? gid : (int)getgid();
    int rc = filsys_fs_open(&k, image, readonly || check, offset);
    if (rc) {
        fprintf(stderr, "filsys: cannot open %s: %s\n", image, strerror(-rc));
        return 1;
    }

    if (check) {
        v6_check_t rep;
        int crc = filsys_check(&k, &rep);
        filsys_close(&k);
        return crc == 0 ? 0 : 1;
    }

    struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
    fuse_opt_add_arg(&args, argv[0]);
    fuse_opt_add_arg(&args, mountpoint);
    fuse_opt_add_arg(&args, "-s");
    if (foreground) fuse_opt_add_arg(&args, "-f");
    if (debug)      fuse_opt_add_arg(&args, "-d");
    if (readonly) {
        if (fuse_opts[0]) strncat(fuse_opts, ",", sizeof(fuse_opts) - strlen(fuse_opts) - 1);
        strncat(fuse_opts, "ro", sizeof(fuse_opts) - strlen(fuse_opts) - 1);
    }
    if (fuse_opts[0]) { fuse_opt_add_arg(&args, "-o"); fuse_opt_add_arg(&args, fuse_opts); }

    rc = fuse_main(args.argc, args.argv, &filsys_ops, &k);
    fuse_opt_free_args(&args);

    filsys_close(&k);
    return rc;
}
