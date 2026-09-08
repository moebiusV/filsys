/* fuseops.c - the FUSE3 adapter: translate FUSE3 callbacks onto the FUSE-free
 * fuse_core.c bodies.
 *
 * SPDX-License-Identifier: ISC */
#include <config.h>
#include "fuse_core.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#include <fuse.h>

/* Build the per-call context: the mount-wide filesystem handle lives in
 * private_data (set by fuse_run and returned by init); uid/gid are the
 * requesting process. */
static fuse_ctx_t C(void)
{
    const struct fuse_context *fc = fuse_get_context();
    fuse_ctx_t c;
    c.fs = (filsys_t *)fc->private_data;
    c.uid = fc->uid;
    c.gid = fc->gid;
    return c;
}

static int fuse_getattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
    (void)fi;
    fuse_ctx_t c = C();
    return fuse_op_getattr(&c, path, st);
}

struct rd_bridge {
    void *buf;
    fuse_fill_dir_t filler;
    size_t off;   /* resume offset: entries with index < off were already sent */
};

static int rd_emit(void *arg, const char *name, const struct stat *st,
                   size_t index)
{
    struct rd_bridge *b = arg;
    if (index < b->off)
        return 0;                       /* skip past the resume point */
    return b->filler(b->buf, name, st, (off_t)index + 1, 0);
}

static int fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t off, struct fuse_file_info *fi,
                        enum fuse_readdir_flags fl)
{
    (void)fi; (void)fl;
    /* macFUSE >= 5.3 calls readdir with non-zero offsets even for the first
     * (and only) pass, so the resume offset must be honoured rather than
     * assumed zero. */
    struct rd_bridge b = { buf, filler, off < 0 ? 0 : (size_t)off };
    fuse_ctx_t c = C();
    return fuse_op_readdir(&c, path, rd_emit, &b);
}

static int fuse_open(const char *path, struct fuse_file_info *fi)
{
    fuse_ctx_t c = C();
    return fuse_op_open(&c, path, fi->flags);
}

static int fuse_read(const char *path, char *buf, size_t size, off_t off,
                     struct fuse_file_info *fi)
{
    (void)fi;
    fuse_ctx_t c = C();
    return fuse_op_read(&c, path, buf, size, off);
}

static int fuse_write(const char *path, const char *buf, size_t size, off_t off,
                      struct fuse_file_info *fi)
{
    (void)fi;
    fuse_ctx_t c = C();
    return fuse_op_write(&c, path, buf, size, off);
}

static int fuse_readlink(const char *path, char *buf, size_t size)
{
    fuse_ctx_t c = C();
    return fuse_op_readlink(&c, path, buf, size);
}

static int fuse_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void)fi;
    fuse_ctx_t c = C();
    return fuse_op_create(&c, path, mode);
}

static int fuse_mkdir(const char *path, mode_t mode)
{
    fuse_ctx_t c = C();
    return fuse_op_mkdir(&c, path, mode);
}

static int fuse_mknod(const char *path, mode_t mode, dev_t rdev)
{
    fuse_ctx_t c = C();
    return fuse_op_mknod(&c, path, mode, rdev);
}

static int fuse_symlink(const char *target, const char *linkpath)
{
    fuse_ctx_t c = C();
    return fuse_op_symlink(&c, target, linkpath);
}

static int fuse_unlink(const char *path) { fuse_ctx_t c = C(); return fuse_op_unlink(&c, path); }
static int fuse_rmdir(const char *path)  { fuse_ctx_t c = C(); return fuse_op_rmdir(&c, path); }
static int fuse_link(const char *from, const char *to) { fuse_ctx_t c = C(); return fuse_op_link(&c, from, to); }

static int fuse_rename(const char *from, const char *to, unsigned int flags)
{
    fuse_ctx_t c = C();
    return fuse_op_rename(&c, from, to, flags);
}

static int fuse_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void)fi;
    fuse_ctx_t c = C();
    return fuse_op_chmod(&c, path, mode);
}

static int fuse_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi)
{
    (void)fi;
    fuse_ctx_t c = C();
    return fuse_op_chown(&c, path, uid, gid);
}

static int fuse_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    (void)fi;
    fuse_ctx_t c = C();
    return fuse_op_truncate(&c, path, size);
}

static int fuse_utimens(const char *path, const struct timespec tv[2],
                        struct fuse_file_info *fi)
{
    (void)fi;
    fuse_ctx_t c = C();
    return fuse_op_utimens(&c, path, tv);
}

static int fuse_statfs(const char *path, struct statvfs *st)
{
    (void)path;
    fuse_ctx_t c = C();
    return fuse_op_statfs(&c, st);
}

static int fuse_access(const char *path, int mask)
{
    fuse_ctx_t c = C();
    return fuse_op_access(&c, path, mask);
}

static int fuse_flush(const char *path, struct fuse_file_info *fi)
{
    (void)path; (void)fi;
    fuse_ctx_t c = C();
    return fuse_op_flush(&c);
}

static int fuse_fsync(const char *path, int isdatasync, struct fuse_file_info *fi)
{
    (void)path; (void)isdatasync; (void)fi;
    fuse_ctx_t c = C();
    return fuse_op_fsync(&c);
}

static int fuse_release(const char *path, struct fuse_file_info *fi)
{
    (void)path; (void)fi;
    fuse_ctx_t c = C();
    return fuse_op_release(&c);
}

static void *fuse_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void)conn;
    cfg->kernel_cache = 0;   /* backing store is a plain file; don't cache pages */
    cfg->use_ino = 1;        /* stable inode numbers are reported (see fill_stat) */
    /* Return the handle we were given; private_data in later callbacks comes
     * from this, and fuse_get_context()->private_data already holds it. */
    return fuse_get_context()->private_data;
}

static void fuse_op_destroy(void *private_data)
{
    filsys_sync((filsys_t *)private_data);   /* flush on unmount; main() closes */
}

static struct fuse_operations fuse_ops = {
    .getattr  = fuse_getattr,
    .readdir  = fuse_readdir,
    .open     = fuse_open,
    .read     = fuse_read,
    .write    = fuse_write,
    .readlink = fuse_readlink,
    .create   = fuse_create,
    .link     = fuse_link,
    .symlink  = fuse_symlink,
    .mkdir    = fuse_mkdir,
    .mknod    = fuse_mknod,
    .unlink   = fuse_unlink,
    .rmdir    = fuse_rmdir,
    .rename   = fuse_rename,
    .chmod    = fuse_chmod,
    .chown    = fuse_chown,
    .truncate = fuse_truncate,
    .utimens  = fuse_utimens,
    .statfs   = fuse_statfs,
    .access   = fuse_access,
    .flush    = fuse_flush,
    .fsync    = fuse_fsync,
    .release  = fuse_release,
    .init     = fuse_init,
    .destroy  = fuse_op_destroy,
};

int fuse_run(fuse_ctx_t *ctx, const fuse_mount_opts_t *opts)
{
    struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
    fuse_opt_add_arg(&args, opts->argv0);
    fuse_opt_add_arg(&args, opts->mountpoint);
    fuse_opt_add_arg(&args, "-s");
    if (opts->foreground) fuse_opt_add_arg(&args, "-f");
    if (opts->debug)      fuse_opt_add_arg(&args, "-d");
    if (opts->fuse_opts && opts->fuse_opts[0]) {
        fuse_opt_add_arg(&args, "-o");
        fuse_opt_add_arg(&args, opts->fuse_opts);
    }
    int rc = fuse_main(args.argc, args.argv, &fuse_ops, ctx->fs);
    fuse_opt_free_args(&args);
    return rc;
}
