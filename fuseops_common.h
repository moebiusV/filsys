/* fuseops_common.h - the FUSE callback trampolines shared by the FUSE3, macFUSE
 * and FUSE2 adapters.
 *
 * These 15 callbacks have identical signatures and bodies across all three
 * adapters, so they are compiled once (fuseops_common.c) and referenced by each
 * adapter's ops table.  The divergent callbacks (getattr, readdir, rename,
 * chmod, chown, truncate, utimens, statfs, init, run) stay per-platform.
 *
 * Unlike fuse_core.h, this header is FUSE-typed: the trampolines translate
 * <fuse.h> signatures onto the FUSE-free fuse_core bodies, so they necessarily
 * name struct fuse_file_info.  Only the adapters include it.
 *
 * SPDX-License-Identifier: ISC */
#ifndef FUSEOPS_COMMON_H
#define FUSEOPS_COMMON_H

#include <fuse.h>

#include "fuse_core.h"

/* Build the per-call context (was the adapters' static C()). */
fuse_ctx_t fuse_ctx(void);

int fuse_open(const char *path, struct fuse_file_info *fi);
int fuse_read(const char *path, char *buf, size_t size, off_t off,
              struct fuse_file_info *fi);
int fuse_write(const char *path, const char *buf, size_t size, off_t off,
               struct fuse_file_info *fi);
int fuse_readlink(const char *path, char *buf, size_t size);
int fuse_create(const char *path, mode_t mode, struct fuse_file_info *fi);
int fuse_mkdir(const char *path, mode_t mode);
int fuse_mknod(const char *path, mode_t mode, dev_t rdev);
int fuse_symlink(const char *target, const char *linkpath);
int fuse_unlink(const char *path);
int fuse_rmdir(const char *path);
int fuse_link(const char *from, const char *to);
int fuse_access(const char *path, int mask);
int fuse_flush(const char *path, struct fuse_file_info *fi);
int fuse_fsync(const char *path, int isdatasync, struct fuse_file_info *fi);
int fuse_release(const char *path, struct fuse_file_info *fi);
void fuse_op_destroy(void *private_data);

#endif /* FUSEOPS_COMMON_H */
