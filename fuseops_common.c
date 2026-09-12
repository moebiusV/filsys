/* fuseops_common.c - the FUSE callback trampolines shared by the FUSE3, macFUSE
 * and FUSE2 adapters.
 *
 * Each trampoline is a one-line translation of a <fuse.h> callback signature
 * onto the FUSE-free fuse_core.c body, with the per-call context built by
 * fuse_ctx().  The three adapters used to carry byte-identical copies of these;
 * they are folded here so a fix to the common path is made once.
 *
 * SPDX-License-Identifier: ISC */
#include <config.h>
#include "fuseops_common.h"

#include <errno.h>
#include <sys/stat.h>

#include <fuse.h>

fuse_ctx_t fuse_ctx(void)
{
    const struct fuse_context *fc = fuse_get_context();
    fuse_ctx_t c;
    c.fs = (filsys_t *)fc->private_data;
    c.uid = fc->uid;
    c.gid = fc->gid;
    return c;
}

int fuse_open(const char *path, struct fuse_file_info *fi)
{
    fuse_ctx_t c = fuse_ctx();
    uint64_t fh = 0;
    int rc = fuse_op_open(&c, path, fi->flags, &fh);
    if (rc)
        return rc;
    fi->fh = fh;   /* stash the inode number; read/write use it, not the path */
    return 0;
}

int fuse_read(const char *path, char *buf, size_t size, off_t off,
              struct fuse_file_info *fi)
{
    (void)path;
    fuse_ctx_t c = fuse_ctx();
    return fuse_op_read(&c, fi->fh, buf, size, off);
}

int fuse_write(const char *path, const char *buf, size_t size, off_t off,
               struct fuse_file_info *fi)
{
    (void)path;
    fuse_ctx_t c = fuse_ctx();
    return fuse_op_write(&c, fi->fh, buf, size, off);
}

int fuse_readlink(const char *path, char *buf, size_t size)
{
    fuse_ctx_t c = fuse_ctx();
    return fuse_op_readlink(&c, path, buf, size);
}

int fuse_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    fuse_ctx_t c = fuse_ctx();
    uint64_t fh = 0;
    int rc = fuse_op_create(&c, path, mode, &fh);
    if (rc)
        return rc;
    fi->fh = fh;   /* the write that follows create() must reach the new inode */
    return 0;
}

int fuse_mkdir(const char *path, mode_t mode)
{
    fuse_ctx_t c = fuse_ctx();
    return fuse_op_mkdir(&c, path, mode);
}

int fuse_mknod(const char *path, mode_t mode, dev_t rdev)
{
    fuse_ctx_t c = fuse_ctx();
    return fuse_op_mknod(&c, path, mode, rdev);
}

int fuse_symlink(const char *target, const char *linkpath)
{
    fuse_ctx_t c = fuse_ctx();
    return fuse_op_symlink(&c, target, linkpath);
}

int fuse_unlink(const char *path) { fuse_ctx_t c = fuse_ctx(); return fuse_op_unlink(&c, path); }
int fuse_rmdir(const char *path)  { fuse_ctx_t c = fuse_ctx(); return fuse_op_rmdir(&c, path); }
int fuse_link(const char *from, const char *to) { fuse_ctx_t c = fuse_ctx(); return fuse_op_link(&c, from, to); }

int fuse_access(const char *path, int mask)
{
    fuse_ctx_t c = fuse_ctx();
    return fuse_op_access(&c, path, mask);
}

int fuse_flush(const char *path, struct fuse_file_info *fi)
{
    (void)path; (void)fi;
    fuse_ctx_t c = fuse_ctx();
    return fuse_op_flush(&c);
}

int fuse_fsync(const char *path, int isdatasync, struct fuse_file_info *fi)
{
    (void)path; (void)isdatasync; (void)fi;
    fuse_ctx_t c = fuse_ctx();
    return fuse_op_fsync(&c);
}

int fuse_release(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    fuse_ctx_t c = fuse_ctx();
    return fuse_op_release(&c, fi->fh);
}

void fuse_op_destroy(void *private_data)
{
    filsys_sync((filsys_t *)private_data);   /* flush on unmount; main() closes */
}
