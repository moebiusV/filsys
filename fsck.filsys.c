/* filsys 1.0.0 - 2026-08-26 - Copyright (C) 2026 David Walther */
/* SPDX-License-Identifier: ISC */
/* fsck.filsys.c - check a V7 (PDP-11) filesystem on a raw disk image.
 *
 * Usage:
 *     fsck.filsys [-o block] image
 *
 * A Research Unix disk image holds one or more filesystems ("partitions"); the
 * partition table is compiled into the kernel, not stored on the disk.  So by
 * default fsck.filsys checks the filesystem at block 0 (the first partition).
 * -o selects a filesystem at a different block offset within the image.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>

#include "v7fs.h"

int main(int argc, char **argv)
{
    const char *path;
    uint64_t offblock = 0;
    int c;

    while ((c = getopt(argc, argv, "o:")) != -1) {
        switch (c) {
        case 'o': offblock = strtoull(optarg, NULL, 0); break;
        default:
            fprintf(stderr, "usage: fsck.filsys [-o block] image\n");
            return 2;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "usage: fsck.filsys [-o block] image\n");
        return 2;
    }
    path = argv[optind];

    v7fs_t fs;
    int rc = v7fs_open(&fs, path, 1 /*readonly*/, 0 /*V7 middle-endian*/,
                       offblock * V7_BSIZE);
    if (rc < 0) {
        fprintf(stderr, "%s: %s\n", path, strerror(-rc));
        return 1;
    }

    printf("%s:", path);
    if (offblock)
        printf(" (offset block %llu)", (unsigned long long)offblock);
    printf("\n");

    v7_check_t rep;
    int err = v7fs_check(&fs, &rep);
    v7fs_close(&fs);

    return err ? 1 : 0;
}
