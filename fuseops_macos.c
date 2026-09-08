/* fuseops_macos.c - the FUSE3 adapter for macFUSE (>= 5.2).
 *
 * macFUSE's libfuse3 API is NOT Linux/BSD fuse3: its getattr/readdir carry a
 * macFUSE-specific `struct fuse_darwin_attr` (a superset of struct stat with
 * btimespec/bkuptimespec/flags) instead of `struct stat`, and its statfs takes
 * `struct statfs` instead of `struct statvfs`.  The `FUSE_DARWIN_EXTEND_OPERATION`
 * macro in macFUSE's <fuse.h> swaps those three signatures under __APPLE__ while
 * keeping the op-table field names (getattr/readdir/statfs) unchanged.  Everything
 * else is byte-identical to fuseops.c, so this file is fuseops.c with those three
 * callbacks translated through two field-conversion helpers.
 *
 * SPDX-License-Identifier: ISC */
#include <config.h>
#include "fuse_core.h"

#include <errno.h>
#include <string.h>
#include <sys/mount.h>   /* struct statfs */
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

/* struct stat -> struct fuse_darwin_attr.  The core fills st_atime/st_mtime/
 * st_ctime/st_birthtime (on macOS these are aliases for the st_*timespec.tv_sec
 * halves); macFUSE's attr carries the whole timespec, so the sub-second halves
 * are left zero. */
static void stat_to_darwin_attr(struct fuse_darwin_attr *a, const struct stat *st)
{
    memset(a, 0, sizeof *a);
    a->ino    = st->st_ino;
    a->mode   = st->st_mode;
    a->nlink  = st->st_nlink;
    a->uid    = st->st_uid;
    a->gid    = st->st_gid;
    a->rdev   = st->st_rdev;
    a->atimespec.tv_sec = st->st_atime;
    a->mtimespec.tv_sec = st->st_mtime;
    a->ctimespec.tv_sec = st->st_ctime;
    a->btimespec.tv_sec = st->st_birthtime;
    a->size   = st->st_size;
    a->blocks = st->st_blocks;
    a->blksize = st->st_blksize;
    a->flags  = st->st_flags;
}

/* struct statvfs -> struct statfs (macFUSE's statfs takes the BSD struct). */
static void statvfs_to_statfs(struct statfs *out, const struct statvfs *sv)
{
    memset(out, 0, sizeof *out);
    out->f_bsize  = (uint32_t)sv->f_bsize;
    out->f_iosize = (int32_t)sv->f_frsize;
    out->f_blocks = sv->f_blocks;
    out->f_bfree  = sv->f_bfree;
    out->f_bavail = sv->f_bavail;
    out->f_files  = sv->f_files;
    out->f_ffree  = sv->f_ffree;
    strncpy(out->f_fstypename, "filsys", sizeof out->f_fstypename - 1);
}

static int fuse_getattr(const char *path, struct fuse_darwin_attr *attr,
                        struct fuse_file_info *fi)
{
    (void)fi;
    struct stat st;
    fuse_ctx_t c = C();
    int rc = fuse_op_getattr(&c, path, &st);
    if (rc)
        return rc;
    stat_to_darwin_attr(attr, &st);
    return 0;
}

struct rd_bridge {
    void *buf;
    fuse_darwin_fill_dir_t filler;
    size_t off;   /* resume offset: entries with index < off were already sent */
};

static int rd_emit(void *arg, const char *name, const struct stat *st,
                   size_t index)
{
    struct rd_bridge *b = arg;
    if (index < b->off)
        return 0;                       /* skip past the resume point */
    struct fuse_darwin_attr attr;
    stat_to_darwin_attr(&attr, st);
    return b->filler(b->buf, name, &attr, (off_t)index + 1, 0);
}

static int fuse_readdir(const char *path, void *buf, fuse_darwin_fill_dir_t filler,
                        off_t off, struct fuse_file_info *fi,
                        enum fuse_readdir_flags fl)
{
    (void)fi; (void)fl;
    /* macFUSE calls readdir with non-zero offsets even for the first (and only)
     * pass, so the resume offset must be honoured rather than assumed zero. */
    struct rd_bridge b = { buf, filler, off < 0 ? 0 : (size_t)off };
    fuse_ctx_t c = C();
    return fuse_op_readdir(&c, path, rd_emit, &b);
}

static int fuse_open(const char *path, struct fuse_file_info *fi)
{
    fuse_ctx_t c = C();
    uint64_t fh = 0;
    int rc = fuse_op_open(&c, path, fi->flags, &fh);
    if (rc)
        return rc;
    fi->fh = fh;
    return 0;
}

static int fuse_read(const char *path, char *buf, size_t size, off_t off,
                     struct fuse_file_info *fi)
{
    (void)path;
    fuse_ctx_t c = C();
    return fuse_op_read(&c, fi->fh, buf, size, off);
}

static int fuse_write(const char *path, const char *buf, size_t size, off_t off,
                      struct fuse_file_info *fi)
{
    (void)path;
    fuse_ctx_t c = C();
    return fuse_op_write(&c, fi->fh, buf, size, off);
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

static int fuse_statfs(const char *path, struct statfs *st)
{
    (void)path;
    struct statvfs sv;
    fuse_ctx_t c = C();
    int rc = fuse_op_statfs(&c, &sv);
    if (rc)
        return rc;
    statvfs_to_statfs(st, &sv);
    return 0;
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
