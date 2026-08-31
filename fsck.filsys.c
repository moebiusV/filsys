/* filsys 1.0.0 - 2026-08-26 - Copyright (C) 2026 David Walther */
/* SPDX-License-Identifier: ISC */
/* fsck.filsys.c - check a Research Unix (V4 through V7) filesystem on a disk
 * image.
 *
 * Usage:
 *     fsck.filsys [-v <4|5|6|7>] [-o block] image
 *
 * A Research Unix disk image holds one or more filesystems ("partitions"); the
 * partition table is compiled into the kernel, not stored on the disk.  So by
 * default fsck.filsys checks the filesystem at block 0 (the first partition).
 * -o selects a filesystem at a different block offset within the image.
 *
 * -v selects the edition: 4, 5 and 6 are byte-identical (the V6 format) and
 * are checked with v6fs_check(); 7 is V7, checked with v7fs_check().  The
 * default is 7.  (V7 had no single fsck -- it used icheck(8) + dcheck(8);
 * see fsck.filsys.1.)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>

#include "v6fs.h"
#include "v7fs.h"

/* Parse the -v edition argument.  Returns 6 (V4/V5/V6) or 7 (V7), or -1 on
 * error.  Accepts a leading 'v' (v6, v7) and rejects everything else. */
static int parse_edition(const char *s)
{
    if (s[0] == 'v' || s[0] == 'V')
        s++;
    if (strcmp(s, "4") == 0 || strcmp(s, "5") == 0 || strcmp(s, "6") == 0)
        return 6;
    if (strcmp(s, "7") == 0)
        return 7;
    return -1;
}

int main(int argc, char **argv)
{
    const char *path;
    uint64_t offblock = 0;
    int edition = 7;   /* default: V7, matching mount.filsys's requirement */
    int c;

    while ((c = getopt(argc, argv, "v:o:")) != -1) {
        switch (c) {
        case 'v':
            edition = parse_edition(optarg);
            if (edition < 0) {
                fprintf(stderr, "fsck.filsys: bad edition '%s' (want 4|5|6|7)\n", optarg);
                return 2;
            }
            break;
        case 'o':
            offblock = strtoull(optarg, NULL, 0);
            break;
        default:
            fprintf(stderr, "usage: fsck.filsys [-v <4|5|6|7>] [-o block] image\n");
            return 2;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "usage: fsck.filsys [-v <4|5|6|7>] [-o block] image\n");
        return 2;
    }
    path = argv[optind];

    printf("%s:", path);
    if (offblock)
        printf(" (offset block %llu)", (unsigned long long)offblock);
    printf("\n");

    int err;

    if (edition == 6) {
        v6fs_t fs;
        int rc = v6fs_open(&fs, path, 1 /*readonly*/, offblock * V6_BSIZE);
        if (rc < 0) {
            fprintf(stderr, "%s: %s\n", path, strerror(-rc));
            return 1;
        }
        v6_check_t rep;
        err = v6fs_check(&fs, &rep);
        v6fs_close(&fs);
    } else {
        v7fs_t fs;
        int rc = v7fs_open(&fs, path, 1 /*readonly*/, 0 /*V7 middle-endian*/,
                           offblock * V7_BSIZE);
        if (rc < 0) {
            fprintf(stderr, "%s: %s\n", path, strerror(-rc));
            return 1;
        }
        v7_check_t rep;
        err = v7fs_check(&fs, &rep);
        v7fs_close(&fs);
    }

    return err ? 1 : 0;
}
