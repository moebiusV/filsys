/* fuse-conformance.c - a standalone minimal FUSE filesystem that measures
 * handle fidelity: for each callback it logs whether `fi` was passed and what
 * `fi->fh` carried, so a driver can diff the observed transcript against an
 * expected one and answer the questions that decide whether filsys's
 * fi->fh-based deferred-free lifecycle is supportable on a given platform.
 *
 * It has no filsys dependency: it serves one empty file "/f" and prints a
 * deterministic, timestamp-free line per callback to stderr.  The file handle
 * is a simple incrementing counter so the driver can tell whether the kernel
 * echoed back the exact value `open` returned.
 *
 * Questions this answers (see docs/fuse-conformance.md):
 *   1. does getattr receive fi, and is fi->fh what open returned?
 *   2. same for truncate?
 *   3. does release fire at last close(2), or later at vnode reclaim?
 *   4. is open deduplicated per (vnode, mode), and do open/release balance?
 *   5. do two processes holding one file get distinct fh values?
 *   6. does unlink of an open file reach the FS (hard_remove), or is it
 *      silly-renamed by libfuse?
 *   7. does truncate carry fi->fh to an *unlinked* open fd (the tmpfile shape
 *      the deferred-free lifecycle needs for ftruncate)?
 *
 * Build: cc -D_FILE_OFFSET_BITS=64 fuse-conformance.c $(pkg-config --cflags --libs fuse3)
 *        (or fuse2 where that is the base libfuse).
 *
 * SPDX-License-Identifier: ISC */
#define FUSE_USE_VERSION 31
#include <fuse.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint64_t g_next_fh = 1;

static void L(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* "fi" may be NULL for a path-based call; report it and the handle it carried. */
static void fi_str(const struct fuse_file_info *fi, char *out, size_t n)
{
    if (!fi)
        snprintf(out, n, "fi=0");
    else
        snprintf(out, n, "fi=1 fh=%llu", (unsigned long long)fi->fh);
}

static int c_getattr(const char *path, struct stat *st,
                     struct fuse_file_info *fi)
{
    char f[64];
    fi_str(fi, f, sizeof f);
    L("GETATTR %s %s", path ? path : "(null)", f);
    memset(st, 0, sizeof *st);
    if (path && strcmp(path, "/") == 0) {
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 2;
    } else if (!path || strcmp(path, "/f") == 0) {
        /* NULL path: getattr on an unlinked-but-open fd (the Q7 shape). */
        st->st_mode = S_IFREG | 0644;
        st->st_nlink = 1;
        st->st_size = 0;
    } else {
        return -ENOENT;
    }
    return 0;
}

static int c_open(const char *path, struct fuse_file_info *fi)
{
    L("OPEN %s flags=0x%x", path, fi->flags);
    if ((fi->flags & O_ACCMODE) != O_RDONLY &&
        (fi->flags & O_ACCMODE) != O_WRONLY &&
        (fi->flags & O_ACCMODE) != O_RDWR)
        return -EACCES;
    fi->fh = g_next_fh++;
    return 0;
}

static int c_read(const char *path, char *buf, size_t size, off_t off,
                  struct fuse_file_info *fi)
{
    char f[64];
    fi_str(fi, f, sizeof f);
    L("READ %s off=%lld size=%zu %s", path, (long long)off, size, f);
    memset(buf, 0, size);
    return (int)size;
}

static int c_write(const char *path, const char *buf, size_t size, off_t off,
                   struct fuse_file_info *fi)
{
    char f[64];
    fi_str(fi, f, sizeof f);
    L("WRITE %s off=%lld size=%zu %s", path, (long long)off, size, f);
    return (int)size;
}

static int c_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    char f[64];
    fi_str(fi, f, sizeof f);
    L("TRUNCATE %s size=%lld %s", path, (long long)size, f);
    return 0;
}

static int c_flush(const char *path, struct fuse_file_info *fi)
{
    char f[64];
    fi_str(fi, f, sizeof f);
    L("FLUSH %s %s", path, f);
    return 0;
}

static int c_release(const char *path, struct fuse_file_info *fi)
{
    char f[64];
    fi_str(fi, f, sizeof f);
    L("RELEASE %s %s", path, f);
    return 0;
}

static int c_unlink(const char *path)
{
    L("UNLINK %s", path);
    return 0;
}

static int c_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    L("CREATE %s mode=0%o flags=0x%x", path, mode, fi->flags);
    fi->fh = g_next_fh++;
    return 0;
}

static int c_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                     off_t off, struct fuse_file_info *fi,
                     enum fuse_readdir_flags flags)
{
    (void)off; (void)fi; (void)flags;
    if (strcmp(path, "/") != 0)
        return -ENOENT;
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    filler(buf, "f", NULL, 0, 0);
    return 0;
}

static void *c_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void)conn;
    cfg->hard_remove = 1;   /* the property filsys relies on */
    cfg->use_ino = 1;
    L("INIT hard_remove=1");
    return NULL;
}

static const struct fuse_operations ops = {
    .getattr  = c_getattr,
    .open     = c_open,
    .read     = c_read,
    .write    = c_write,
    .truncate = c_truncate,
    .flush    = c_flush,
    .release  = c_release,
    .unlink   = c_unlink,
    .create   = c_create,
    .readdir  = c_readdir,
    .init     = c_init,
};

int main(int argc, char **argv)
{
    return fuse_main(argc, argv, &ops, NULL);
}
