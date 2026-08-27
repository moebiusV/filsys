/* filsys 1.0.0 - 2026-08-26 - Copyright (C) 2026 David Walther */
/* SPDX-License-Identifier: ISC */
/* filsysmount.c - mount a Research Unix filesystem image (PDP-11) as a FUSE
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
 *     filsysmount -v <4|5|6|7|32> [-o offset=N[,uid=N,gid=N,...]] [-r] [-f] [-d] <image> <mountpoint>
 *     filsysmount -v <4|5|6|7|32> [-o offset=N] -c <image>   # integrity check
 *
 * `-o offset=N` mounts a filesystem that lives at byte offset N within the
 * file (a partition of a larger disk image), instead of one at block 0.
 * `-o uid=N,gid=N` override the reported ownership (default: the mounting
 * user, so a nested mount point is writable).  Any other -o option is passed
 * through to FUSE (e.g. allow_other).
 */
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
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <fuse.h>

#define KFS_V6 6
#define KFS_V7 7
#define KFS_32V 32

/* v6_inode_t and v7_inode_t share the same decoded layout (both carry
 * addr[13]); use the v7 type as the common in-core inode. */
typedef v7_inode_t  kfs_inode_t;
typedef v7_dirent_t kfs_dirent_t;

typedef struct {
    int ver;
    int uid, gid;              /* reported ownership (default: the mounting user) */
    union { v6fs_t v6; v7fs_t v7; } u;
} kfs_t;

static kfs_t *KFS(void) {
    return (kfs_t *)fuse_get_context()->private_data;
}

/* ---- mode conversion (edition-aware) ------------------------------------ */

static mode_t kfs_to_posix_mode(int ver, uint16_t mode) {
    mode_t m = mode & 07777;
    if (ver == KFS_V6) {
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

static uint16_t kfs_perm_of(mode_t m) {
    return (uint16_t)(m & 07777);
}

/* ---- dispatch wrappers --------------------------------------------------- */

static int kfs_fs_open(kfs_t *k, const char *path, int readonly, uint64_t offset) {
    if (k->ver == KFS_V6) return v6fs_open(&k->u.v6, path, readonly, offset);
    return v7fs_open(&k->u.v7, path, readonly, k->ver == KFS_32V, offset);
}
static void kfs_close(kfs_t *k) {
    if (k->ver == KFS_V6) v6fs_close(&k->u.v6);
    else v7fs_close(&k->u.v7);
}
static int kfs_read_inode(kfs_t *k, uint32_t ino, kfs_inode_t *ip) {
    if (k->ver == KFS_V6) return v6fs_read_inode(&k->u.v6, ino, (v6_inode_t *)ip);
    return v7fs_read_inode(&k->u.v7, ino, ip);
}
static int kfs_write_inode(kfs_t *k, uint32_t ino, const kfs_inode_t *ip) {
    if (k->ver == KFS_V6) return v6fs_write_inode(&k->u.v6, ino, (const v6_inode_t *)ip);
    return v7fs_write_inode(&k->u.v7, ino, ip);
}
static int kfs_bmap(kfs_t *k, kfs_inode_t *ip, uint32_t lbn, int create, uint32_t *bno) {
    if (k->ver == KFS_V6) return v6fs_bmap(&k->u.v6, (v6_inode_t *)ip, lbn, create, bno);
    return v7fs_bmap(&k->u.v7, ip, lbn, create, bno);
}
static int kfs_balloc(kfs_t *k, uint32_t *bno) {
    if (k->ver == KFS_V6) return v6fs_balloc(&k->u.v6, bno);
    return v7fs_balloc(&k->u.v7, bno);
}
static void kfs_bfree(kfs_t *k, uint32_t bno) {
    if (k->ver == KFS_V6) v6fs_bfree(&k->u.v6, bno);
    else v7fs_bfree(&k->u.v7, bno);
}
static int kfs_ialloc(kfs_t *k, uint32_t *ino) {
    if (k->ver == KFS_V6) return v6fs_ialloc(&k->u.v6, ino);
    return v7fs_ialloc(&k->u.v7, ino);
}
static void kfs_ifree(kfs_t *k, uint32_t ino) {
    if (k->ver == KFS_V6) v6fs_ifree(&k->u.v6, ino);
    else v7fs_ifree(&k->u.v7, ino);
}
static int kfs_itrunc(kfs_t *k, kfs_inode_t *ip) {
    if (k->ver == KFS_V6) return v6fs_itrunc(&k->u.v6, (v6_inode_t *)ip);
    return v7fs_itrunc(&k->u.v7, ip);
}
static ssize_t kfs_file_read(kfs_t *k, kfs_inode_t *ip, uint8_t *buf, size_t sz, off_t off) {
    if (k->ver == KFS_V6) return v6fs_file_read(&k->u.v6, (v6_inode_t *)ip, buf, sz, off);
    return v7fs_file_read(&k->u.v7, ip, buf, sz, off);
}
static ssize_t kfs_file_write(kfs_t *k, kfs_inode_t *ip, const uint8_t *buf, size_t sz, off_t off) {
    if (k->ver == KFS_V6) return v6fs_file_write(&k->u.v6, (v6_inode_t *)ip, buf, sz, off);
    return v7fs_file_write(&k->u.v7, ip, buf, sz, off);
}
static int kfs_dir_read(kfs_t *k, kfs_inode_t *ip, kfs_dirent_t **e, size_t *n) {
    if (k->ver == KFS_V6) return v6fs_dir_read(&k->u.v6, (v6_inode_t *)ip, (v6_dirent_t **)e, n);
    return v7fs_dir_read(&k->u.v7, ip, e, n);
}
static int kfs_dir_lookup(kfs_t *k, kfs_inode_t *ip, const char *name, uint32_t *ino) {
    if (k->ver == KFS_V6) return v6fs_dir_lookup(&k->u.v6, (v6_inode_t *)ip, name, ino);
    return v7fs_dir_lookup(&k->u.v7, ip, name, ino);
}
static int kfs_dir_add(kfs_t *k, kfs_inode_t *ip, uint32_t ino, const char *name) {
    if (k->ver == KFS_V6) return v6fs_dir_add(&k->u.v6, (v6_inode_t *)ip, ino, name);
    return v7fs_dir_add(&k->u.v7, ip, ino, name);
}
static int kfs_dir_remove(kfs_t *k, kfs_inode_t *ip, const char *name) {
    if (k->ver == KFS_V6) return v6fs_dir_remove(&k->u.v6, (v6_inode_t *)ip, name);
    return v7fs_dir_remove(&k->u.v7, ip, name);
}
static int kfs_lookup(kfs_t *k, const char *path, uint32_t *ino, kfs_inode_t *ip) {
    if (k->ver == KFS_V6) return v6fs_lookup(&k->u.v6, path, ino, (v6_inode_t *)ip);
    return v7fs_lookup(&k->u.v7, path, ino, ip);
}
static int kfs_check(kfs_t *k, v6_check_t *rep) {
    if (k->ver == KFS_V6) return v6fs_check(&k->u.v6, rep);
    return v7fs_check(&k->u.v7, (v7_check_t *)rep);   /* identical layout */
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

static void fill_stat(kfs_t *k, const kfs_inode_t *ip, struct stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_ino   = ip->ino;
    st->st_mode  = kfs_to_posix_mode(k->ver, ip->mode);
    st->st_nlink = ip->nlink;
    st->st_uid   = k->uid;
    st->st_gid   = k->gid;
    st->st_size  = ip->size;
    uint16_t t = k->ver == KFS_V6 ? (ip->mode & V6_IFMT) : (ip->mode & V7_IFMT);
    int isdev = (k->ver == KFS_V6) ? (t == V6_IFCHR || t == V6_IFBLK)
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

static int kfs_getattr(const char *path, struct stat *st, struct fuse_file_info *fi) {
    (void)fi;
    kfs_t *k = KFS();
    kfs_inode_t ip;
    uint32_t ino;
    int rc = kfs_lookup(k, path, &ino, &ip);
    if (rc) return rc;
    fill_stat(k, &ip, st);
    return 0;
}

static int kfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t off, struct fuse_file_info *fi, enum fuse_readdir_flags fl) {
    (void)off; (void)fi; (void)fl;
    kfs_t *k = KFS();
    kfs_inode_t ip;
    uint32_t ino;
    int rc = kfs_lookup(k, path, &ino, &ip);
    if (rc) return rc;
    kfs_dirent_t *ents = NULL;
    size_t count = 0;
    rc = kfs_dir_read(k, &ip, &ents, &count);
    if (rc) return rc;
    for (size_t i = 0; i < count; i++) {
        struct stat st;
        kfs_inode_t eip;
        if (kfs_read_inode(k, ents[i].ino, &eip) == 0)
            fill_stat(k, &eip, &st);
        else
            memset(&st, 0, sizeof(st));
        if (filler(buf, ents[i].name, &st, 0, 0))
            break;
    }
    free(ents);
    return 0;
}

static int kfs_open(const char *path, struct fuse_file_info *fi) {
    kfs_t *k = KFS();
    kfs_inode_t ip;
    uint32_t ino;
    int rc = kfs_lookup(k, path, &ino, &ip);
    if (rc) return rc;
    uint16_t t = k->ver == KFS_V6 ? (ip.mode & V6_IFMT) : (ip.mode & V7_IFMT);
    int isdir = (k->ver == KFS_V6) ? (t == V6_IFDIR) : (t == V7_IFDIR);
    if (isdir) return -EISDIR;
    if ((fi->flags & O_ACCMODE) != O_RDONLY && (k->ver == KFS_V6 ? k->u.v6.readonly : k->u.v7.readonly))
        return -EROFS;
    return 0;
}

static int kfs_read(const char *path, char *buf, size_t size, off_t off, struct fuse_file_info *fi) {
    (void)fi;
    kfs_t *k = KFS();
    kfs_inode_t ip;
    uint32_t ino;
    int rc = kfs_lookup(k, path, &ino, &ip);
    if (rc) return rc;
    return (int)kfs_file_read(k, &ip, (uint8_t *)buf, size, off);
}

static int kfs_write(const char *path, const char *buf, size_t size, off_t off, struct fuse_file_info *fi) {
    (void)fi;
    kfs_t *k = KFS();
    kfs_inode_t ip;
    uint32_t ino;
    int rc = kfs_lookup(k, path, &ino, &ip);
    if (rc) return rc;
    return (int)kfs_file_write(k, &ip, (const uint8_t *)buf, size, off);
}

static int kfs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)fi;
    kfs_t *k = KFS();
    char dir[PATH_MAX], name[15];
    int rc = split_path(path, dir, sizeof(dir), name, sizeof(name));
    if (rc) return rc;
    kfs_inode_t ddir;
    uint32_t dino;
    rc = kfs_lookup(k, dir, &dino, &ddir);
    if (rc) return rc;
    uint32_t nino;
    rc = kfs_ialloc(k, &nino);
    if (rc) return rc;
    const struct fuse_context *ctx = fuse_get_context();
    kfs_inode_t nip;
    memset(&nip, 0, sizeof(nip));
    nip.ino = nino;
    nip.mode = kfs_perm_of(mode) | (k->ver == KFS_V6 ? 0 : V7_IFREG);
    nip.nlink = 1;
    nip.uid = (int16_t)ctx->uid;
    nip.gid = (int16_t)ctx->gid;
    nip.atime = nip.mtime = nip.ctime = (uint32_t)time(NULL);
    kfs_write_inode(k, nino, &nip);
    return kfs_dir_add(k, &ddir, nino, name);
}

static int kfs_mkdir(const char *path, mode_t mode) {
    kfs_t *k = KFS();
    char dir[PATH_MAX], name[15];
    int rc = split_path(path, dir, sizeof(dir), name, sizeof(name));
    if (rc) return rc;
    kfs_inode_t ddir;
    uint32_t dino;
    rc = kfs_lookup(k, dir, &dino, &ddir);
    if (rc) return rc;
    uint32_t nino;
    rc = kfs_ialloc(k, &nino);
    if (rc) return rc;
    const struct fuse_context *ctx = fuse_get_context();
    kfs_inode_t nip;
    memset(&nip, 0, sizeof(nip));
    nip.ino = nino;
    nip.mode = kfs_perm_of(mode) | (k->ver == KFS_V6 ? V6_IFDIR : V7_IFDIR);
    nip.nlink = 2;
    nip.uid = (int16_t)ctx->uid;
    nip.gid = (int16_t)ctx->gid;
    nip.atime = nip.mtime = nip.ctime = (uint32_t)time(NULL);
    kfs_write_inode(k, nino, &nip);
    kfs_dir_add(k, &nip, nino, ".");
    kfs_dir_add(k, &nip, dino, "..");
    rc = kfs_dir_add(k, &ddir, nino, name);
    if (rc) return rc;
    ddir.nlink++;
    kfs_write_inode(k, dino, &ddir);
    return 0;
}

static int kfs_do_unlink(kfs_t *k, const char *dirpath, const char *name) {
    kfs_inode_t ddir;
    uint32_t dino;
    int rc = kfs_lookup(k, dirpath, &dino, &ddir);
    if (rc) return rc;
    uint32_t ino;
    rc = kfs_dir_lookup(k, &ddir, name, &ino);
    if (rc) return rc;
    kfs_inode_t ip;
    if (kfs_read_inode(k, ino, &ip)) return -EIO;
    rc = kfs_dir_remove(k, &ddir, name);
    if (rc) return rc;
    ip.nlink--;
    if (ip.nlink <= 0) {
        kfs_itrunc(k, &ip);
        ip.mode = 0;
        kfs_ifree(k, ino);
    }
    kfs_write_inode(k, ino, &ip);
    return 0;
}

static int kfs_unlink(const char *path) {
    kfs_t *k = KFS();
    char dir[PATH_MAX], name[15];
    int rc = split_path(path, dir, sizeof(dir), name, sizeof(name));
    if (rc) return rc;
    kfs_inode_t ip;
    uint32_t ino;
    rc = kfs_lookup(k, path, &ino, &ip);
    if (rc) return rc;
    uint16_t t = k->ver == KFS_V6 ? (ip.mode & V6_IFMT) : (ip.mode & V7_IFMT);
    int isdir = (k->ver == KFS_V6) ? (t == V6_IFDIR) : (t == V7_IFDIR);
    if (isdir) return -EISDIR;
    return kfs_do_unlink(k, dir, name);
}

static int kfs_rmdir(const char *path) {
    kfs_t *k = KFS();
    char dir[PATH_MAX], name[15];
    int rc = split_path(path, dir, sizeof(dir), name, sizeof(name));
    if (rc) return rc;
    kfs_inode_t ddir;
    uint32_t dino;
    rc = kfs_lookup(k, dir, &dino, &ddir);
    if (rc) return rc;
    uint32_t ino;
    rc = kfs_dir_lookup(k, &ddir, name, &ino);
    if (rc) return rc;
    kfs_inode_t tip;
    if (kfs_read_inode(k, ino, &tip)) return -EIO;
    uint16_t t = k->ver == KFS_V6 ? (tip.mode & V6_IFMT) : (tip.mode & V7_IFMT);
    int isdir = (k->ver == KFS_V6) ? (t == V6_IFDIR) : (t == V7_IFDIR);
    if (!isdir) return -ENOTDIR;
    kfs_dirent_t *ents = NULL;
    size_t count = 0;
    if (kfs_dir_read(k, &tip, &ents, &count)) return -EIO;
    for (size_t i = 0; i < count; i++)
        if (strcmp(ents[i].name, ".") && strcmp(ents[i].name, "..")) { free(ents); return -ENOTEMPTY; }
    free(ents);
    rc = kfs_dir_remove(k, &ddir, name);
    if (rc) return rc;
    ddir.nlink--;
    kfs_write_inode(k, dino, &ddir);
    kfs_itrunc(k, &tip);
    tip.mode = 0;
    kfs_ifree(k, ino);
    kfs_write_inode(k, ino, &tip);
    return 0;
}

static int kfs_do_link(kfs_t *k, const char *dst, uint32_t src_ino) {
    char dir[PATH_MAX], name[15];
    int rc = split_path(dst, dir, sizeof(dir), name, sizeof(name));
    if (rc) return rc;
    kfs_inode_t ddir;
    uint32_t dino;
    rc = kfs_lookup(k, dir, &dino, &ddir);
    if (rc) return rc;
    rc = kfs_dir_add(k, &ddir, src_ino, name);
    if (rc) return rc;
    kfs_inode_t ip;
    if (kfs_read_inode(k, src_ino, &ip)) return -EIO;
    ip.nlink++;
    kfs_write_inode(k, src_ino, &ip);
    return 0;
}

static int kfs_rename(const char *from, const char *to, unsigned int flags) {
    if (flags) return -EINVAL;
    if (!strcmp(from, to)) return 0;
    kfs_t *k = KFS();
    kfs_inode_t sip;
    uint32_t sino;
    int rc = kfs_lookup(k, from, &sino, &sip);
    if (rc) return rc;
    char tdir[PATH_MAX], tname[15];
    rc = split_path(to, tdir, sizeof(tdir), tname, sizeof(tname));
    if (rc) return rc;
    kfs_inode_t tdirip;
    uint32_t tdino;
    rc = kfs_lookup(k, tdir, &tdino, &tdirip);
    if (rc) return rc;
    uint32_t tino;
    if (kfs_dir_lookup(k, &tdirip, tname, &tino) == 0)
        kfs_do_unlink(k, tdir, tname);
    kfs_do_link(k, to, sino);
    char fdir[PATH_MAX], fname[15];
    split_path(from, fdir, sizeof(fdir), fname, sizeof(fname));
    return kfs_do_unlink(k, fdir, fname);
}

static int kfs_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)fi;
    kfs_t *k = KFS();
    kfs_inode_t ip;
    uint32_t ino;
    int rc = kfs_lookup(k, path, &ino, &ip);
    if (rc) return rc;
    uint16_t fmask = k->ver == KFS_V6 ? V6_IFMT : V7_IFMT;
    ip.mode = (ip.mode & fmask) | (uint16_t)(mode & 07777);
    ip.ctime = (uint32_t)time(NULL);
    kfs_write_inode(k, ino, &ip);
    return 0;
}

static int kfs_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi) {
    (void)fi;
    kfs_t *k = KFS();
    kfs_inode_t ip;
    uint32_t ino;
    int rc = kfs_lookup(k, path, &ino, &ip);
    if (rc) return rc;
    if (uid != (uid_t)-1) ip.uid = (int16_t)uid;
    if (gid != (gid_t)-1) ip.gid = (int16_t)gid;
    ip.ctime = (uint32_t)time(NULL);
    kfs_write_inode(k, ino, &ip);
    return 0;
}

static int kfs_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
    (void)fi;
    kfs_t *k = KFS();
    kfs_inode_t ip;
    uint32_t ino;
    int rc = kfs_lookup(k, path, &ino, &ip);
    if (rc) return rc;
    uint32_t newsize = (uint32_t)size, oldsize = ip.size;
    if (newsize == oldsize) return 0;
    uint32_t keep = newsize < oldsize ? newsize : oldsize;
    uint8_t *data = malloc(keep ? keep : 1);
    if (!data) return -ENOMEM;
    if (keep) kfs_file_read(k, &ip, data, keep, 0);
    kfs_itrunc(k, &ip);
    if (keep) kfs_file_write(k, &ip, data, keep, 0);
    if (newsize != keep) { ip.size = newsize; kfs_write_inode(k, ino, &ip); }
    free(data);
    return 0;
}

static int kfs_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi) {
    (void)fi;
    kfs_t *k = KFS();
    kfs_inode_t ip;
    uint32_t ino;
    int rc = kfs_lookup(k, path, &ino, &ip);
    if (rc) return rc;
    uint32_t now = (uint32_t)time(NULL);
    ip.atime = tv ? (uint32_t)tv[0].tv_sec : now;
    ip.mtime = tv ? (uint32_t)tv[1].tv_sec : now;
    ip.ctime = now;
    kfs_write_inode(k, ino, &ip);
    return 0;
}

static int kfs_statfs(const char *path, struct statvfs *st) {
    (void)path;
    kfs_t *k = KFS();
    memset(st, 0, sizeof(*st));
    st->f_bsize = st->f_frsize = 512;
    if (k->ver == KFS_V6) {
        st->f_blocks = k->u.v6.fsize;
        st->f_bfree = st->f_bavail = k->u.v6.nfree;
        st->f_files = (k->u.v6.isize - 2) * 16;
        st->f_ffree = k->u.v6.ninode;
    } else {
        st->f_blocks = k->u.v7.fsize;
        st->f_bfree = st->f_bavail = k->u.v7.nfree;
        st->f_files = (k->u.v7.isize - 2) * 8;
        st->f_ffree = k->u.v7.ninode;
    }
    st->f_namemax = 14;
    return 0;
}

static struct fuse_operations kfs_ops = {
    .getattr  = kfs_getattr,
    .readdir  = kfs_readdir,
    .open     = kfs_open,
    .read     = kfs_read,
    .write    = kfs_write,
    .create   = kfs_create,
    .mkdir    = kfs_mkdir,
    .unlink   = kfs_unlink,
    .rmdir    = kfs_rmdir,
    .rename   = kfs_rename,
    .chmod    = kfs_chmod,
    .chown    = kfs_chown,
    .truncate = kfs_truncate,
    .utimens  = kfs_utimens,
    .statfs   = kfs_statfs,
};

/* ---- main ---------------------------------------------------------------- */

static void usage(const char *p) {
    fprintf(stderr,
            "usage: %s -v <4|5|6|7|32> [-o offset=N] [-r] [-f] [-d] <image> <mountpoint>\n"
            "       %s -v <4|5|6|7|32> [-o offset=N] -c <image>   # integrity check\n",
            p, p);
}

int main(int argc, char *argv[]) {
    int ver = KFS_V7, readonly = 0, foreground = 0, debug = 0, check = 0;
    uint64_t offset = 0;
    int uid = -1, gid = -1;   /* -1 = report as the mounting user */
    char fuse_opts[512] = ""; /* -o options passed through to FUSE (allow_other, ...) */
    int ai = 1;
    for (; ai < argc && argv[ai][0] == '-'; ai++) {
        const char *a = argv[ai];
        if (!strcmp(a, "-v")) {
            if (ai + 1 >= argc) { usage(argv[0]); return 2; }
            const char *v = argv[++ai];
            if (v[0] == 'v' || v[0] == 'V') v++;   /* accept "v7" and "7" alike */
            if (!strcmp(v, "4") || !strcmp(v, "5") || !strcmp(v, "6"))
                ver = KFS_V6;
            else if (!strcmp(v, "7"))
                ver = KFS_V7;
            else if (!strcmp(v, "32") || !strcmp(v, "32v"))
                ver = KFS_32V;
            else {
                fprintf(stderr, "%s: unknown Unix version \"%s\" (4, 5, 6, 7, 32)\n", argv[0], v);
                usage(argv[0]);
                return 2;
            }
            continue;
        }
        if (!strcmp(a, "-o")) {
            if (ai + 1 >= argc) { usage(argv[0]); return 2; }
            char *opts = strdup(argv[++ai]);
            int bad = 0;
            for (char *tok = strtok(opts, ","); tok; tok = strtok(NULL, ",")) {
                if (!strncmp(tok, "offset=", 7)) {
                    char *end = NULL;
                    offset = strtoull(tok + 7, &end, 0);
                    if (!end || *end) { bad = 1; break; }
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
            continue;
        }
        for (const char *p = a + 1; *p; p++) {
            switch (*p) {
            case 'r': readonly = 1; break;
            case 'f': foreground = 1; break;
            case 'd': debug = 1; break;
            case 'c': check = 1; break;
            default: usage(argv[0]); return 2;
            }
        }
    }
    if (check && (argc - ai != 1)) { fprintf(stderr, "usage: %s -c <image>\n", argv[0]); return 2; }
    if (!check && (argc - ai != 2)) { usage(argv[0]); return 2; }
    const char *image = argv[ai];
    const char *mountpoint = check ? NULL : argv[ai + 1];

    kfs_t k;
    memset(&k, 0, sizeof(k));
    k.ver = ver;
    k.uid = uid >= 0 ? uid : (int)getuid();
    k.gid = gid >= 0 ? gid : (int)getgid();
    int rc = kfs_fs_open(&k, image, readonly || check, offset);
    if (rc) {
        fprintf(stderr, "filsys: cannot open %s: %s\n", image, strerror(-rc));
        return 1;
    }

    if (check) {
        v6_check_t rep;
        int crc = kfs_check(&k, &rep);
        kfs_close(&k);
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

    rc = fuse_main(args.argc, args.argv, &kfs_ops, &k);
    fuse_opt_free_args(&args);

    kfs_close(&k);
    return rc;
}
