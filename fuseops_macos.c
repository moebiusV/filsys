/* fuseops_macos.c - the FUSE3 adapter for macFUSE (>= 5.2).
 *
 * macFUSE's libfuse3 API is NOT Linux/BSD fuse3: its getattr/readdir carry a
 * macFUSE-specific `struct fuse_darwin_attr` (a superset of struct stat with
 * btimespec/bkuptimespec/flags) instead of `struct stat`, and its statfs takes
 * `struct statfs` instead of `struct statvfs`.  The `FUSE_DARWIN_EXTEND_OPERATION`
 * macro in macFUSE's <fuse.h> swaps those three signatures under __APPLE__ while
 * keeping the op-table field names (getattr/readdir/statfs) unchanged.  Everything
 * else is the shared trampolines in fuseops_common.c plus the handful of divergent
 * callbacks here, translated through two field-conversion helpers.
 *
 * SPDX-License-Identifier: ISC */
#include <config.h>
#include "fuse_core.h"
#include "fuseops_common.h"

#include <errno.h>
#include <string.h>
#include <sys/mount.h>   /* struct statfs */
#include <sys/stat.h>

#include <fuse.h>

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
    struct stat st;
    fuse_ctx_t c = fuse_ctx();
    int rc = (fi && fi->fh)
        ? fuse_op_getattr_ino(&c, fi->fh, &st)
        : fuse_op_getattr(&c, path, &st);
    if (rc)
        return rc;
    stat_to_darwin_attr(attr, &st);
    return 0;
}

struct rd_bridge {
    void *buf;
    fuse_darwin_fill_dir_t filler;
    uint64_t off;   /* resume offset: entries with next_off <= off were already sent */
};

static int rd_emit(void *arg, const char *name, const struct stat *st,
                   uint64_t off)
{
    struct rd_bridge *b = arg;
    if (off <= b->off)
        return 0;                       /* skip past the resume point */
    struct fuse_darwin_attr attr;
    stat_to_darwin_attr(&attr, st);
    return b->filler(b->buf, name, &attr, (off_t)off, 0);
}

static int fuse_readdir(const char *path, void *buf, fuse_darwin_fill_dir_t filler,
                        off_t off, struct fuse_file_info *fi,
                        enum fuse_readdir_flags fl)
{
    (void)fi; (void)fl;
    /* macFUSE calls readdir with non-zero offsets even for the first (and only)
     * pass, so the resume offset must be honoured rather than assumed zero. */
    struct rd_bridge b = { buf, filler, off < 0 ? 0 : (uint64_t)off };
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

static int fuse_statfs(const char *path, struct statfs *st)
{
    (void)path;
    struct statvfs sv;
    fuse_ctx_t c = fuse_ctx();
    int rc = fuse_op_statfs(&c, &sv);
    if (rc)
        return rc;
    statvfs_to_statfs(st, &sv);
    return 0;
}

static void *fuse_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void)conn;
    cfg->kernel_cache = 0;   /* backing store is a plain file; don't cache pages */
    cfg->use_ino = 1;        /* stable inode numbers are reported (see fill_stat) */
    cfg->hard_remove = 1;    /* forced, not chosen; see fuseops_common.h */
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

static const fuse_quirks_t fuse_quirks = { .use_ino_mount_opt = 0 };

int fuse_run(fuse_ctx_t *ctx, const fuse_mount_opts_t *opts)
{
    return fuse_run_common(ctx, opts, &fuse_ops, &fuse_quirks);
}
