/* fuseops_openbsd.c - the FUSE2 adapter for OpenBSD's base-libfuse (2.6-era).
 *
 * Translates the FUSE2 callback signatures onto the FUSE-free fuse_core.c
 * bodies.  This is the *permanent* OpenBSD path, not a transitional shim:
 * OpenBSD ships its own ISC-licensed libfuse implementing the 2.6-era API, and
 * there is no fuse3 there.  It is also the path for older macFUSE 4.x / FUSE-T,
 * both of which are 2.x-only.
 *
 * The invariant trampolines live in fuseops_common.c; this file holds only the
 * callbacks whose FUSE2 signature differs from FUSE3 (getattr, readdir, rename,
 * chmod, chown, truncate, utimens, statfs, init) plus the ops table and
 * fuse_run.
 *
 * Two differences from the FUSE3 adapter are not mere argument shuffling:
 *   - FUSE2 `init` has no `struct fuse_config *`, so `hard_remove`/`use_ino`
 *     are high-level *mount options* parsed by fuse_new, not fields set in init.
 *     fuse_run() injects them into the args before fuse_main.
 *   - The op-table stays at the 2.6 intersection: no write_buf/read_buf/flock/
 *     fallocate/ioctl/poll, which a 2.6 header does not declare.
 *
 * SPDX-License-Identifier: ISC */
#include <config.h>
#include "fuse_core.h"
#include "fuseops_common.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#include <fuse.h>

/* FUSE2 getattr carries no struct fuse_file_info *.  Consequence: unlike the
 * FUSE3/macFUSE adapters, fstat() on an unlinked-but-still-open descriptor
 * cannot be routed through the handle and returns ENOENT on OpenBSD -- a
 * limitation of the 2.6 callback surface, not a filsys bug. */
static int fuse_getattr(const char *path, struct stat *st)
{
    fuse_ctx_t c = fuse_ctx();
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
    return b->filler(b->buf, name, st, (off_t)index + 1);   /* FUSE2: 4 args */
}

static int fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t off, struct fuse_file_info *fi)
{
    (void)fi;
    struct rd_bridge b = { buf, filler, off < 0 ? 0 : (size_t)off };
    fuse_ctx_t c = fuse_ctx();
    return fuse_op_readdir(&c, path, rd_emit, &b);
}

/* FUSE2 rename has no flags argument; filsys needs none of RENAME_NOREPLACE /
 * RENAME_EXCHANGE. */
static int fuse_rename(const char *from, const char *to)
{
    fuse_ctx_t c = fuse_ctx();
    return fuse_op_rename(&c, from, to, 0);
}

static int fuse_chmod(const char *path, mode_t mode)
{
    fuse_ctx_t c = fuse_ctx();
    return fuse_op_chmod(&c, path, mode);
}

static int fuse_chown(const char *path, uid_t uid, gid_t gid)
{
    fuse_ctx_t c = fuse_ctx();
    return fuse_op_chown(&c, path, uid, gid);
}

static int fuse_truncate(const char *path, off_t size)
{
    fuse_ctx_t c = fuse_ctx();
    return fuse_op_truncate(&c, path, size);
}

static int fuse_utimens(const char *path, const struct timespec tv[2])
{
    fuse_ctx_t c = fuse_ctx();
    return fuse_op_utimens(&c, path, tv);
}

static int fuse_statfs(const char *path, struct statvfs *st)
{
    (void)path;
    fuse_ctx_t c = fuse_ctx();
    return fuse_op_statfs(&c, st);
}

/* FUSE2 init has no struct fuse_config *: the mount options are injected in
 * fuse_run() below.  Return the handle we were given (already in
 * fuse_get_context()->private_data). */
static void *fuse_init(struct fuse_conn_info *conn)
{
    (void)conn;
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
    /* FUSE2: no fuse_config in init, so use_ino (honour the stable st_ino our
     * fill_stat reports) is a mount option injected before fuse_main.  Note the
     * OpenBSD option parser is the one cgofuse flags as not fully compatible;
     * if it rejects this on target, drop it (the kernel then synthesises node
     * numbers, which are still stable) — see docs/fuse-ports3.md.  OpenBSD
     * already defaults to hard_remove (no silly-rename), so no option is needed
     * there; that is the only option anyway, since the 28-char ".fuse_hidden"
     * fallback name cannot fit a V7 14-byte (or V1/PDP-7 8-byte) dirent.
     * filsys_open_ino still defers the unlink free to release. */
    fuse_opt_add_arg(&args, "-o");
    fuse_opt_add_arg(&args, "use_ino");
    if (opts->fuse_opts && opts->fuse_opts[0]) {
        fuse_opt_add_arg(&args, "-o");
        fuse_opt_add_arg(&args, opts->fuse_opts);
    }
    int rc = fuse_main(args.argc, args.argv, &fuse_ops, ctx->fs);
    fuse_opt_free_args(&args);
    return rc;
}
