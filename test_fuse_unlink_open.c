/* test_fuse_unlink_open.c - the FUSE-boundary regression test for ftruncate on
 * an unlinked-but-still-open descriptor.
 *
 * The bug this guards (open / unlink / truncate resolved the *path*, which is
 * gone after unlink, and returned ENOENT) lived in the FUSE adapter, not the
 * library: fuseops.c and fuseops_macos.c route truncate through fi->fh when a
 * file handle is present.  It runs under test.sh while the V7 image is mounted,
 * which is the layer the original defect actually occupied.
 *
 * fstat immediately after unlink is deliberately *not* asserted: with
 * hard_remove the Linux FUSE kernel answers fstat of an unlinked inode with
 * -ESTALE before getattr is ever called, so there is nothing filsys can route.
 * (fstat works again once ftruncate has refreshed the inode, as verified below.)
 *
 * Exit: 0 = pass, 77 = skip (FUSE2: truncate has no fi, so it is ENOENT), 1 = fail.
 *
 * SPDX-License-Identifier: ISC */
#define _POSIX_C_SOURCE 200809L   /* ftruncate, pread under -std=c17 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <path-under-mount>\n", argv[0]);
        return 1;
    }
    const char *path = argv[1];
    const char payload[] = "hello world";
    const size_t plen = sizeof payload - 1;

    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { perror("open"); return 1; }
    if (write(fd, payload, plen) != (ssize_t)plen) { perror("write"); close(fd); return 1; }
    if (unlink(path) != 0) { perror("unlink"); close(fd); return 1; }

    /* The handle must reach the inode without the (now gone) path.  Where the
     * backend cannot route truncate by handle on an unlinked fd (FUSE2 has no fi
     * in truncate; NetBSD's librefuse drops fi->fh once the name is unlinked)
     * ENOENT is the expected skip; where truncate carries fi->fh through the
     * handle (real fuse3) an ENOENT is exactly the path-based regression this
     * test guards.  FILSYS_TRUNC_FI is exported by configure/AM_TESTS_ENVIRONMENT. */
    int trunc_fi = getenv("FILSYS_TRUNC_FI") != NULL;
    if (ftruncate(fd, 5) != 0) {
        if (errno == ENOENT && !trunc_fi) { close(fd); return 77; }
        perror("ftruncate"); close(fd); return 1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) { perror("fstat"); close(fd); return 1; }
    if (st.st_size != 5) {
        fprintf(stderr, "post-ftruncate size %lld != 5\n", (long long)st.st_size);
        close(fd); return 1;
    }

    char buf[8] = {0};
    ssize_t n = pread(fd, buf, sizeof buf - 1, 0);
    if (n != 5 || strncmp(buf, "hello", 5) != 0) {
        fprintf(stderr, "read %zd bytes: '%s', wanted 5 bytes 'hello'\n", n, buf);
        close(fd); return 1;
    }

    close(fd);
    return 0;
}
