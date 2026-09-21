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
#include <sys/statvfs.h>

/* One open-handle-table entry: an inode with open file descriptions.  refs
 * counts them, so the last release is the point to free an inode that was
 * unlinked-while-open (hard_remove). */
typedef struct {
    uint32_t ino;
    int      refs;
} fuse_open_t;

/* Mount-wide FUSE state, passed as libfuse's private_data.  The open-handle
 * table moved here from the engine: only FUSE has no kernel handle lifetime, so
 * only FUSE needs to pin an inode across unlink-while-open. */
typedef struct {
    filsys_t    *fs;
    fuse_open_t *open;
    int          nopen, nopen_cap;
} fuse_mount_t;

typedef struct {
    filsys_t     *fs;
    fuse_mount_t *mount;   /* the open table lives here, not in the engine */
    uid_t         uid;     /* requesting process (per-call) */
    gid_t         gid;
} fuse_ctx_t;

/* Open-handle table: track an open inode, release it (freeing the inode on the
 * last close if it was unlinked while open), report its refcount, and drain it
 * at unmount. */
int  fuse_open_track(fuse_mount_t *m, uint32_t ino);
int  fuse_open_release(fuse_mount_t *m, uint32_t ino);
int  fuse_open_refs(const fuse_mount_t *m, uint32_t ino);
void fuse_open_drain(fuse_mount_t *m);

/* readdir emits one entry per call; `off` is the engine's resume token
 * (filsys_dir_next's next_off, the offset of the following entry), which the
 * adapter hands to the FUSE filler as that entry's offset.  A non-zero return
 * stops the walk. */
typedef int (*fuse_emit_t)(void *arg, const char *name, const struct stat *st,
                           uint64_t off);

int fuse_op_getattr(fuse_ctx_t *c, const char *path, struct stat *st);
/* Stat an open handle (fi->fh = ino): an unlinked-open fd has no path. */
int fuse_op_getattr_ino(fuse_ctx_t *c, uint64_t fh, struct stat *st);
int fuse_op_readdir(fuse_ctx_t *c, const char *path, fuse_emit_t emit, void *arg);
int fuse_op_open(fuse_ctx_t *c, const char *path, int flags, uint64_t *fh);
int fuse_op_read(fuse_ctx_t *c, uint64_t fh, char *buf, size_t size,
                 off_t off);
int fuse_op_write(fuse_ctx_t *c, uint64_t fh, const char *buf, size_t size,
                  off_t off);
int fuse_op_readlink(fuse_ctx_t *c, const char *path, char *buf, size_t size);
int fuse_op_create(fuse_ctx_t *c, const char *path, mode_t mode, uint64_t *fh);
int fuse_op_mkdir(fuse_ctx_t *c, const char *path, mode_t mode);
int fuse_op_mknod(fuse_ctx_t *c, const char *path, mode_t mode, dev_t rdev);
int fuse_op_symlink(fuse_ctx_t *c, const char *target, const char *linkpath);
int fuse_op_unlink(fuse_ctx_t *c, const char *path);
int fuse_op_rmdir(fuse_ctx_t *c, const char *path);
int fuse_op_link(fuse_ctx_t *c, const char *from, const char *to);
int fuse_op_rename(fuse_ctx_t *c, const char *from, const char *to,
                   unsigned int flags);
int fuse_op_chmod(fuse_ctx_t *c, const char *path, mode_t mode);
int fuse_op_chown(fuse_ctx_t *c, const char *path, uid_t uid, gid_t gid);
int fuse_op_truncate(fuse_ctx_t *c, const char *path, off_t size);
int fuse_op_truncate_ino(fuse_ctx_t *c, uint64_t fh, off_t size);
int fuse_op_utimens(fuse_ctx_t *c, const char *path, const struct timespec tv[2]);
int fuse_op_statfs(fuse_ctx_t *c, struct statvfs *st);
int fuse_op_access(fuse_ctx_t *c, const char *path, int mask);
int fuse_op_flush(fuse_ctx_t *c);
int fuse_op_fsync(fuse_ctx_t *c);
int fuse_op_release(fuse_ctx_t *c, uint64_t fh);

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
