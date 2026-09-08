/* fuse_core.h - the FUSE-free callback bodies shared by the FUSE3 and FUSE2
 * adapters.
 *
 * Nothing here includes <fuse.h>; the adapters (fuseops.c for FUSE3,
 * fuseops_openbsd.c for OpenBSD's base-libfuse 2.6) translate their own
 * callback signatures onto these.  Ownership of new inodes is the *requesting*
 * process, carried in fuse_ctx_t and filled by the adapter (from
 * fuse_get_context() where that exists, else geteuid()/getegid()), so the core
 * never depends on fuse_context_t. */
#ifndef FUSE_CORE_H
#define FUSE_CORE_H

#include "filsys.h"

typedef struct {
    filsys_t *fs;
    uid_t     uid;   /* requesting process (per-call) */
    gid_t     gid;
} fuse_ctx_t;

/* readdir emits one entry per call; `index` is the entry's 0-based position in
 * the directory (stable across calls), which the adapter turns into a FUSE
 * resume offset.  A non-zero return stops the walk. */
typedef int (*fuse_emit_t)(void *arg, const char *name, const struct stat *st,
                           size_t index);

int fuse_op_getattr(fuse_ctx_t *c, const char *path, struct stat *st);
int fuse_op_readdir(fuse_ctx_t *c, const char *path, fuse_emit_t emit, void *arg);
int fuse_op_open(fuse_ctx_t *c, const char *path, int flags);
int fuse_op_read(fuse_ctx_t *c, const char *path, char *buf, size_t size,
                 off_t off);
int fuse_op_write(fuse_ctx_t *c, const char *path, const char *buf, size_t size,
                  off_t off);
int fuse_op_create(fuse_ctx_t *c, const char *path, mode_t mode);
int fuse_op_mkdir(fuse_ctx_t *c, const char *path, mode_t mode);
int fuse_op_mknod(fuse_ctx_t *c, const char *path, mode_t mode, dev_t rdev);
int fuse_op_unlink(fuse_ctx_t *c, const char *path);
int fuse_op_rmdir(fuse_ctx_t *c, const char *path);
int fuse_op_link(fuse_ctx_t *c, const char *from, const char *to);
int fuse_op_rename(fuse_ctx_t *c, const char *from, const char *to,
                   unsigned int flags);
int fuse_op_chmod(fuse_ctx_t *c, const char *path, mode_t mode);
int fuse_op_chown(fuse_ctx_t *c, const char *path, uid_t uid, gid_t gid);
int fuse_op_truncate(fuse_ctx_t *c, const char *path, off_t size);
int fuse_op_utimens(fuse_ctx_t *c, const char *path, const struct timespec tv[2]);
int fuse_op_statfs(fuse_ctx_t *c, struct statvfs *st);
int fuse_op_access(fuse_ctx_t *c, const char *path, int mask);
int fuse_op_flush(fuse_ctx_t *c);
int fuse_op_fsync(fuse_ctx_t *c);
int fuse_op_release(fuse_ctx_t *c);

/* Mount options collected by main() and handed to the adapter's fuse_run().
 * Nothing FUSE-typed leaks into the shared main(). */
typedef struct {
    const char *argv0;
    const char *mountpoint;
    int         foreground;
    int         debug;
    const char *fuse_opts;   /* the -o payload (allow_other,ro,default_permissions) */
} fuse_mount_opts_t;

/* Build the FUSE args, inject version-specific mount options, and run
 * fuse_main.  Defined by the selected adapter (fuseops.c for FUSE3,
 * fuseops_openbsd.c for OpenBSD's base-libfuse 2.6). */
int fuse_run(fuse_ctx_t *ctx, const fuse_mount_opts_t *opts);

#endif /* FUSE_CORE_H */
