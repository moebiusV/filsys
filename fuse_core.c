/* fuse_core.c - the FUSE-free callback bodies (see fuse_core.h).
 *
 * SPDX-License-Identifier: ISC */
#include <config.h>
#include "fuse_core.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef HAVE_SYS_SYSMACROS_H
#include <sys/sysmacros.h>
#endif

/* Fill a POSIX struct stat from the engine's plain-integer attributes. */
static int fill_stat(filsys_t *fs, const filsys_inode_t *ip, struct stat *st)
{
    filsys_stat_t s;
    int rc = filsys_stat_inode(fs, ip, &s);
    if (rc)
        return rc;
    memset(st, 0, sizeof(*st));
    st->st_ino   = s.ino;
    st->st_mode  = s.mode;
    st->st_nlink = s.nlink;
    st->st_uid   = s.uid;
    st->st_gid   = s.gid;
    st->st_size  = s.size;
    if (s.rdev)
        st->st_rdev = makedev(s.rdev >> 8, s.rdev & 0xff);
    st->st_atime   = s.atime;
    st->st_mtime   = s.mtime;
    st->st_ctime   = s.ctime;
    st->st_blksize = s.blksize;
    st->st_blocks  = (s.bytes + 511) / 512;
    return 0;
}

static int fill_stat_ino(filsys_t *fs, uint32_t ino, struct stat *st)
{
    filsys_inode_t ip;
    int rc = filsys_read_inode(fs, ino, &ip);
    if (rc)
        return rc;
    return fill_stat(fs, &ip, st);
}

int fuse_op_getattr(fuse_ctx_t *c, const char *path, struct stat *st)
{
    filsys_inode_t ip;
    uint32_t ino;
    int rc = filsys_lookup(c->fs, path, &ino, &ip);
    if (rc)
        return rc;
    return fill_stat(c->fs, &ip, st);
}

int fuse_op_getattr_ino(fuse_ctx_t *c, uint64_t fh, struct stat *st)
{
    return fill_stat_ino(c->fs, (uint32_t)fh, st);
}

int fuse_op_readdir(fuse_ctx_t *c, const char *path, fuse_emit_t emit,
                      void *arg)
{
    uint32_t ino;
    int rc = filsys_lookup(c->fs, path, &ino, NULL);
    if (rc)
        return rc;
    filsys_iter_t it;
    rc = filsys_dir_seek(c->fs, ino, 0, &it);
    if (rc)
        return rc;
    size_t index = 0;
    uint32_t eino;
    const char *name;
    uint16_t namlen;
    uint64_t next_off;
    while ((rc = filsys_dir_next(&it, &eino, &name, &namlen, &next_off)) == 1) {
        struct stat st;
        filsys_inode_t eip;
        if (filsys_read_inode(c->fs, eino, &eip) == 0)
            fill_stat(c->fs, &eip, &st);
        else
            memset(&st, 0, sizeof(st));
        char namebuf[64];
        memcpy(namebuf, name, namlen);
        namebuf[namlen] = 0;
        if (emit(arg, namebuf, &st, index))
            break;
        index++;
    }
    filsys_dir_release(&it);
    return 0;
}

int fuse_op_open(fuse_ctx_t *c, const char *path, int flags, uint64_t *fh)
{
    filsys_inode_t ip;
    uint32_t ino;
    int rc = filsys_lookup(c->fs, path, &ino, &ip);
    if (rc)
        return rc;
    struct stat st;
    fill_stat(c->fs, &ip, &st);
    if (S_ISDIR(st.st_mode))
        return -EISDIR;
    if ((flags & O_ACCMODE) != O_RDONLY && filsys_is_readonly(c->fs))
        return -EROFS;
    *fh = ino;   /* read/write use the ino, so the fd outlives the directory entry */
    return filsys_open_ino(c->fs, ino);   /* track the handle for hard_remove */
}

int fuse_op_read(fuse_ctx_t *c, uint64_t fh, char *buf, size_t size,
                   off_t off)
{
    return (int)filsys_read_ino(c->fs, (uint32_t)fh, buf, size, off);
}

int fuse_op_write(fuse_ctx_t *c, uint64_t fh, const char *buf,
                    size_t size, off_t off)
{
    return (int)filsys_write_ino(c->fs, (uint32_t)fh, buf, size, off);
}

int fuse_op_readlink(fuse_ctx_t *c, const char *path, char *buf, size_t size)
{
    ssize_t n = filsys_readlink(c->fs, path, buf, size);
    if (n < 0)
        return (int)n;
    if (n < (ssize_t)size)
        buf[n] = '\0';   /* filsys_readlink returns the target without a NUL */
    return 0;
}

int fuse_op_create(fuse_ctx_t *c, const char *path, mode_t mode, uint64_t *fh)
{
    /* The kernel's create is a combined open: return the new inode so the
     * adapter can stash it in fi->fh for the read/write that follow. */
    uint32_t ino;
    int rc = filsys_create(c->fs, path, mode, c->uid, c->gid, &ino);
    if (rc)
        return rc;
    *fh = ino;
    return filsys_open_ino(c->fs, ino);   /* track the handle for hard_remove */
}

int fuse_op_mkdir(fuse_ctx_t *c, const char *path, mode_t mode)
{
    return filsys_mkdir(c->fs, path, mode, c->uid, c->gid);
}

int fuse_op_mknod(fuse_ctx_t *c, const char *path, mode_t mode, dev_t rdev)
{
    /* Pack the host dev_t into V7's (major<<8 | minor) word.  Reject rather
     * than mask: a modern major like 300 would otherwise silently become 44. */
    if (major(rdev) > 255 || minor(rdev) > 255)
        return -EINVAL;
    uint32_t packed = (uint32_t)((major(rdev) << 8) | minor(rdev));
    return filsys_mknod(c->fs, path, mode, packed, c->uid, c->gid);
}

int fuse_op_symlink(fuse_ctx_t *c, const char *target, const char *linkpath)
{
    return filsys_symlink(c->fs, target, linkpath);
}

int fuse_op_unlink(fuse_ctx_t *c, const char *path)
{
    return filsys_unlink(c->fs, path);
}

int fuse_op_rmdir(fuse_ctx_t *c, const char *path)
{
    return filsys_rmdir(c->fs, path);
}

int fuse_op_link(fuse_ctx_t *c, const char *from, const char *to)
{
    return filsys_link(c->fs, from, to);
}

int fuse_op_rename(fuse_ctx_t *c, const char *from, const char *to,
                     unsigned int flags)
{
    return filsys_rename(c->fs, from, to, flags);
}

int fuse_op_chmod(fuse_ctx_t *c, const char *path, mode_t mode)
{
    return filsys_chmod(c->fs, path, mode);
}

int fuse_op_chown(fuse_ctx_t *c, const char *path, uid_t uid, gid_t gid)
{
    return filsys_chown(c->fs, path, uid, gid);
}

int fuse_op_truncate(fuse_ctx_t *c, const char *path, off_t size)
{
    return filsys_truncate(c->fs, path, size);
}

int fuse_op_truncate_ino(fuse_ctx_t *c, uint64_t fh, off_t size)
{
    return filsys_truncate_ino(c->fs, (uint32_t)fh, size);
}

int fuse_op_utimens(fuse_ctx_t *c, const char *path,
                      const struct timespec tv[2])
{
    int64_t t[2];
    if (!tv)
        return filsys_utimens(c->fs, path, NULL);
    for (int i = 0; i < 2; i++) {
        if (tv[i].tv_nsec == UTIME_NOW)
            t[i] = FILSYS_UTIME_NOW;
        else if (tv[i].tv_nsec == UTIME_OMIT)
            t[i] = FILSYS_UTIME_OMIT;
        else
            t[i] = (int64_t)tv[i].tv_sec;
    }
    return filsys_utimens(c->fs, path, t);
}

int fuse_op_statfs(fuse_ctx_t *c, struct statvfs *st)
{
    filsys_statfs_t s;
    int rc = filsys_statfs(c->fs, &s);
    if (rc)
        return rc;
    memset(st, 0, sizeof(*st));
    st->f_bsize = st->f_frsize = s.bsize;
    st->f_blocks = s.blocks;
    st->f_bfree = s.bfree;
    st->f_bavail = s.bavail;
    st->f_files = s.files;
    st->f_ffree = s.ffree;
    st->f_namemax = s.namemax;
    return 0;
}

int fuse_op_access(fuse_ctx_t *c, const char *path, int mask)
{
    filsys_t *k = c->fs;
    filsys_inode_t ip;
    uint32_t ino;
    int rc = filsys_lookup(k, path, &ino, &ip);
    if (rc)
        return rc;
    if ((mask & W_OK) && filsys_is_readonly(k))
        return -EROFS;
    /* Check the same POSIX mode stat() reports, not the raw on-disk word: V1
     * and PDP-7 lay their permission bits out differently (V1's owner-read is
     * 010, not 0400), so shifting ip.mode made access() contradict stat(). */
    struct stat st;
    rc = fill_stat(k, &ip, &st);
    if (rc)
        return rc;
    mode_t pm = st.st_mode;
    /* The whole image is reported as owned by the mounting user (fill_stat
     * st_uid/st_gid), so compare against that, not the on-disk V7 uids. */
    int bits;
    if (c->uid == 0) {
        /* root: R/W always allowed; X_OK needs at least one exec bit set */
        return (mask & X_OK) && !(pm & 0111) ? -EACCES : 0;
    }
    if (c->uid == filsys_uid(k))
        bits = (pm >> 6) & 7;               /* owner */
    else if (c->gid == filsys_gid(k))
        bits = (pm >> 3) & 7;               /* group (primary gid only) */
    else
        bits = pm & 7;                      /* other */
    if ((mask & R_OK) && !(bits & 4)) return -EACCES;
    if ((mask & W_OK) && !(bits & 2)) return -EACCES;
    if ((mask & X_OK) && !(bits & 1)) return -EACCES;
    return 0;
}

int fuse_op_flush(fuse_ctx_t *c)
{
    /* Data blocks go straight to the image, but the free list and superblock
     * are batched; flush on close(2) to persist them. */
    return filsys_sync(c->fs);
}

int fuse_op_fsync(fuse_ctx_t *c)
{
    return filsys_sync(c->fs);
}

int fuse_op_release(fuse_ctx_t *c, uint64_t fh)
{
    return filsys_close_ino(c->fs, (uint32_t)fh);
}
