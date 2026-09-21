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

/* Why hard_remove is forced, not chosen.  libfuse's default fallback (silly-
 * rename) writes a 28-char ".fuse_hidden" name, which cannot fit a V7 14-byte
 * (or V1/PDP-7 8-byte) dirent -- every hidden file would truncate to the same
 * name and collide.  hard_remove is therefore forced uniformly: the FUSE3/
 * macFUSE init sets cfg->hard_remove = 1, and OpenBSD's FUSE2 already defaults
 * to it.  (2.11BSD's 63-byte names *could* hold the silly-rename name, so the
 * length argument doesn't bind there; hard_remove is forced uniformly anyway.
 * The deferred-free lifecycle filsys_open_ino/free_deferred_ino is built around
 * hard_remove semantics -- the name is gone and nlink drops to 0 on the first
 * unlink -- and letting one edition fall back to a hidden rename would leave an
 * untested path.) */

/* Behavioural differences between the FUSE2 and FUSE3 families, the "data" the
 * one shared fuse_run body reads.  Everything else the adapters differ in is a
 * callback signature, which cannot be data, so the signature-specific callbacks
 * and the ops table stay per-adapter. */
typedef struct {
    /* FUSE2 has no struct fuse_config in init, so stable inode numbers are a
     * high-level mount option rather than a cfg field; inject "-o use_ino".
     * (OpenBSD's option parser is the one cgofuse flags as not fully compatible;
     * if it rejects this on target, drop it -- the kernel then synthesises node
     * numbers, which are still stable.) */
    int use_ino_mount_opt;
} fuse_quirks_t;

/* The shared fuse_run body: build the args (injecting the quirks' mount
 * options), call fuse_main, free the args.  Each adapter's fuse_run is a
 * one-liner passing its ops table and quirks. */
int fuse_run_common(fuse_ctx_t *ctx, const fuse_mount_opts_t *opts,
                    const struct fuse_operations *ops, const fuse_quirks_t *q);

#endif /* FUSEOPS_COMMON_H */
