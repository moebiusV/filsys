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

int fuse_op_getattr(fuse_ctx_t *c, const char *path, struct stat *st)
{
    filsys_inode_t ip;
    uint32_t ino;
    int rc = filsys_lookup(c->fs, path, &ino, &ip);
    if (rc)
        return rc;
    filsys_fill_stat(c->fs, &ip, st);
    return 0;
}

int fuse_op_readdir(fuse_ctx_t *c, const char *path, fuse_emit_t emit,
                      void *arg)
{
    filsys_dirent_t *ents = NULL;
    size_t count = 0;
    int rc = filsys_readdir(c->fs, path, &ents, &count);
    if (rc)
        return rc;
    for (size_t i = 0; i < count; i++) {
        struct stat st;
        filsys_inode_t eip;
        if (filsys_read_inode(c->fs, ents[i].ino, &eip) == 0)
            filsys_fill_stat(c->fs, &eip, &st);
        else
            memset(&st, 0, sizeof(st));
        if (emit(arg, ents[i].name, &st, i))
            break;
    }
    free(ents);
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
    filsys_fill_stat(c->fs, &ip, &st);
    if (S_ISDIR(st.st_mode))
        return -EISDIR;
    if ((flags & O_ACCMODE) != O_RDONLY && filsys_is_readonly(c->fs))
        return -EROFS;
    *fh = ino;   /* read/write use the ino, so the fd outlives the directory entry */
    return 0;
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

int fuse_op_create(fuse_ctx_t *c, const char *path, mode_t mode)
{
    return filsys_create(c->fs, path, mode, c->uid, c->gid);
}

int fuse_op_mkdir(fuse_ctx_t *c, const char *path, mode_t mode)
{
    return filsys_mkdir(c->fs, path, mode, c->uid, c->gid);
}

int fuse_op_mknod(fuse_ctx_t *c, const char *path, mode_t mode, dev_t rdev)
{
    return filsys_mknod(c->fs, path, mode, rdev, c->uid, c->gid);
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

int fuse_op_utimens(fuse_ctx_t *c, const char *path,
                      const struct timespec tv[2])
{
    return filsys_utimens(c->fs, path, tv);
}

int fuse_op_statfs(fuse_ctx_t *c, struct statvfs *st)
{
    return filsys_statfs(c->fs, st);
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
    /* The whole image is reported as owned by the mounting user (fill_stat
     * st_uid/st_gid), so compare against that, not the on-disk V7 uids. */
    int bits;
    if (c->uid == 0) {
        /* root: R/W always allowed; X_OK needs at least one exec bit set */
        return (mask & X_OK) && !(ip.mode & 0111) ? -EACCES : 0;
    }
    if (c->uid == filsys_uid(k))
        bits = (ip.mode >> 6) & 7;          /* owner */
    else if (c->gid == filsys_gid(k))
        bits = (ip.mode >> 3) & 7;          /* group (primary gid only) */
    else
        bits = ip.mode & 7;                 /* other */
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

int fuse_op_release(fuse_ctx_t *c)
{
    (void)c;
    return 0;   /* no per-open state to release */
}
