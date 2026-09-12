/* fuseops.c - the FUSE3 adapter: translate FUSE3 callbacks onto the FUSE-free
 * fuse_core.c bodies.
 *
 * The invariant trampolines live in fuseops_common.c; this file holds only the
 * callbacks whose FUSE3 signature differs from FUSE2/macFUSE (getattr, readdir,
 * rename, chmod, chown, truncate, utimens, statfs, init) plus the ops table and
 * fuse_run.
 *
 * SPDX-License-Identifier: ISC */
#include <config.h>
#include "fuse_core.h"
#include "fuseops_common.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#include <fuse.h>

static int fuse_getattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
    fuse_ctx_t c = fuse_ctx();
    if (fi && fi->fh)   /* open (possibly unlinked) fd: stat the inode, not the path */
        return fuse_op_getattr_ino(&c, fi->fh, st);
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
    fuse_ctx_t c = fuse_ctx();
    return fuse_op_readdir(&c, path, rd_emit, &b);
}

static int fuse_rename(const char *from, const char *to, unsigned int flags)
{
    fuse_ctx_t c = fuse_ctx();
    return fuse_op_rename(&c, from, to, flags);
}

static int fuse_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void)fi;
    fuse_ctx_t c = fuse_ctx();
    return fuse_op_chmod(&c, path, mode);
}

static int fuse_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi)
{
    (void)fi;
    fuse_ctx_t c = fuse_ctx();
    return fuse_op_chown(&c, path, uid, gid);
}

static int fuse_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    fuse_ctx_t c = fuse_ctx();
    if (fi && fi->fh)
        return fuse_op_truncate_ino(&c, fi->fh, size);
    return fuse_op_truncate(&c, path, size);
}

static int fuse_utimens(const char *path, const struct timespec tv[2],
                        struct fuse_file_info *fi)
{
    (void)fi;
    fuse_ctx_t c = fuse_ctx();
    return fuse_op_utimens(&c, path, tv);
}

static int fuse_statfs(const char *path, struct statvfs *st)
{
    (void)path;
    fuse_ctx_t c = fuse_ctx();
    return fuse_op_statfs(&c, st);
}

static void *fuse_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void)conn;
    cfg->kernel_cache = 0;   /* backing store is a plain file; don't cache pages */
    cfg->use_ino = 1;        /* stable inode numbers are reported (see fill_stat) */
    cfg->hard_remove = 1;    /* no silly-rename: libfuse's fallback writes a 28-char
                              * ".fuse_hidden" name, which cannot fit a V7 14-byte (or
                              * V1/PDP-7 8-byte) dirent -- every hidden file would
                              * truncate to the same name and collide.  hard_remove is
                              * therefore forced, not chosen.
                              *
                              * (2.11BSD's 63-byte names *could* hold the silly-rename
                              * name, so the length argument doesn't bind there; hard_remove
                              * is forced uniformly anyway.  The deferred-free lifecycle
                              * filsys_open_ino/free_deferred_ino is built around hard_remove
                              * semantics -- the name is gone and nlink drops to 0 on the
                              * first unlink -- and letting one edition fall back to a
                              * hidden rename would leave an untested path.) */
    /* Return the handle we were given; private_data in later callbacks comes
     * from this, and fuse_get_context()->private_data already holds it. */
    return fuse_get_context()->private_data;
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
