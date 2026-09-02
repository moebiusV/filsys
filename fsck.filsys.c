/* filsys 1.0.0 - 2026-08-26 - Copyright (C) 2026 David Walther */
/* SPDX-License-Identifier: ISC */
/* fsck.filsys.c - check and repair a Research Unix (V4 through V7) filesystem
 * on a disk image.
 *
 * Usage:
 *     fsck.filsys [-v <4|5|6|7>] [-o block] [-s] [-r] [-n] image
 *     fsck.filsys [-v 7] [-o block] -N ino image
 *     fsck.filsys [-v 7] [-o block] -C ino image
 *
 * A Research Unix disk image holds one or more filesystems ("partitions"); the
 * partition table is compiled into the kernel, not stored on the disk.  So by
 * default fsck.filsys checks the filesystem at block 0.  -o selects a different
 * block offset.
 *
 * -v selects the edition: 4, 5 and 6 are byte-identical (the V6 format) and
 * are checked with v6fs_check(); 7 is V7, checked with v7fs_check().  The
 * default is 7.  (V7 had no single fsck -- it used icheck(8) + dcheck(8);
 * v7fs_check() folds both in.)
 *
 * -s rebuilds the free list from the block scan (icheck -s), the standard
 * repair after restor(8), which does not rebuild the free list.
 * -r resolves duplicate blocks (salv -a): each duplicate is copied to a fresh
 * block and the second reference re-pointed, then the free list is rebuilt.
 * -n report only, change nothing (the default; explicit, and refused together
 * with the modifying flags -s/-r/-C).
 * -N ino prints the pathname(s) of an inode (ncheck).
 * -C ino zeroes an inode (clri).
 * The -s/-r/-N/-C maintenance modes are V7 only.
 */
#include <config.h>
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
    int salvage = 0, resolve = 0, ncheck = 0, clri = 0, nochange = 0;
    uint32_t ino = 0;
    int c;

    while ((c = getopt(argc, argv, "v:o:srnN:C:")) != -1) {
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
        case 's':
            salvage = 1;
            break;
        case 'r':
            resolve = 1;
            break;
        case 'n':
            nochange = 1;
            break;
        case 'N':
            ncheck = 1;
            ino = strtoul(optarg, NULL, 0);
            break;
        case 'C':
            clri = 1;
            ino = strtoul(optarg, NULL, 0);
            break;
        default:
            fprintf(stderr,
                "usage: fsck.filsys [-v <4|5|6|7>] [-o block] [-s] [-r] [-n] image\n"
                "       fsck.filsys [-v 7] [-o block] -N ino image\n"
                "       fsck.filsys [-v 7] [-o block] -C ino image\n");
            return 2;
        }
    }
    if (optind >= argc) {
        fprintf(stderr,
            "usage: fsck.filsys [-v <4|5|6|7>] [-o block] [-s] [-r] image\n"
            "       fsck.filsys [-v 7] [-o block] -n ino image\n"
            "       fsck.filsys [-v 7] [-o block] -c ino image\n");
        return 2;
    }
    path = argv[optind];

    /* -s/-r/-N/-C are V7-only maintenance modes */
    if (edition == 6 && (salvage || resolve || ncheck || clri)) {
        fprintf(stderr, "fsck.filsys: -s/-r/-N/-C are V7 only\n");
        return 2;
    }
    if (nochange && (salvage || resolve || clri)) {
        fprintf(stderr, "fsck.filsys: -n (no change) conflicts with -s/-r/-C\n");
        return 2;
    }

    printf("%s:", path);
    if (offblock)
        printf(" (offset block %llu)", (unsigned long long)offblock);
    printf("\n");

    if (edition == 6) {
        v6fs_t fs;
        int rc = v6fs_open(&fs, path, 1 /*readonly*/, offblock * V6_BSIZE);
        if (rc < 0) {
            fprintf(stderr, "%s: %s\n", path, strerror(-rc));
            return 1;
        }
        v6_check_t rep;
        int err = v6fs_check(&fs, &rep);
        v6fs_close(&fs);
        return err ? 1 : 0;
    }

    int readonly = !(salvage || resolve || clri);
    v7fs_t fs;
    int rc = v7fs_open(&fs, path, readonly, 0 /*V7 middle-endian*/,
                       offblock * V7_BSIZE);
    if (rc < 0) {
        fprintf(stderr, "%s: %s\n", path, strerror(-rc));
        return 1;
    }

    int err = 0;
    if (ncheck) {
        err = v7fs_ncheck(&fs, ino);
    } else if (clri) {
        err = v7fs_clri(&fs, ino);
    } else if (resolve) {
        err = v7fs_resolve_dups(&fs);
    } else {
        v7_check_t rep;
        err = v7fs_check(&fs, &rep, salvage);
    }
    v7fs_close(&fs);
    return err ? 1 : 0;
}
