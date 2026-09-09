/* fuse-conformance-fuse2.c - the FUSE2 (2.6-era) build of the conformance
 * probe, for OpenBSD's base libfuse and any other 2.x-only stack.
 *
 * Identical to fuse-conformance.c except for the callbacks that FUSE2 declares
 * without a struct fuse_file_info *: getattr and truncate have no fi, so those
 * two questions (and Q7, the unlinked-truncate case) are answered structurally
 * ("no fi in the signature") rather than by measurement.  open/flush/release/
 * unlink still carry fi, so the release-timing, dedup, two-process and
 * hard_remove questions are unchanged.
 *
 * Build: cc -D_FILE_OFFSET_BITS=64 fuse-conformance-fuse2.c \
 *            $(pkg-config --cflags --libs fuse)
 *        (OpenBSD's base libfuse ships fuse.pc, so the header is found through
 *        pkg-config; a bare -lfuse cannot see fuse.h.)
 *
 * SPDX-License-Identifier: ISC */
#define FUSE_USE_VERSION 26
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

static void fi_str(const struct fuse_file_info *fi, char *out, size_t n)
{
    if (!fi)
        snprintf(out, n, "fi=0");
    else
        snprintf(out, n, "fi=1 fh=%llu", (unsigned long long)fi->fh);
}

static int c_getattr(const char *path, struct stat *st)
{
    L("GETATTR %s (fuse2: no fi in signature)", path ? path : "(null)");
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

static int c_truncate(const char *path, off_t size)
{
    L("TRUNCATE %s size=%lld (fuse2: no fi in signature)", path, (long long)size);
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
                     off_t off, struct fuse_file_info *fi)
{
    (void)off; (void)fi;
    if (strcmp(path, "/") != 0)
        return -ENOENT;
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    filler(buf, "f", NULL, 0);
    return 0;
}

static void *c_init(struct fuse_conn_info *conn)
{
    (void)conn;
    L("INIT hard_remove=1 (mount option)");
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
