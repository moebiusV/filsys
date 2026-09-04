/* filsys 1.0.0 - 2026-08-26 - Copyright (C) 2026 David Walther */
/* SPDX-License-Identifier: ISC */
/* fsck.filsys.c - check and repair a Research Unix (PDP-7 through V7)
 * filesystem on a disk image.
 *
 * Usage:
 *     fsck.filsys [-v <v0|1|4|5|6|7|32v>] [-o block] [-s] [-r] [-n] image
 *     fsck.filsys [-v <edition>] [-o block] -N ino image
 *     fsck.filsys [-v <edition>] [-o block] -C ino image
 *
 * A Research Unix disk image holds one or more filesystems ("partitions"); the
 * partition table is compiled into the kernel, not stored on the disk.  So by
 * default fsck.filsys checks the filesystem at block 0.  -o selects a different
 * block offset.
 *
 * -v selects the edition: v0 (PDP-7), 1 (First Edition), 4/5/6 (the V6
 * format, byte-identical), 7 (V7), 32v.  The default is 7.  (V7 had no single
 * fsck -- it used icheck(8) + dcheck(8); the check folds both in.)
 *
 * -s rebuilds the free list from the block scan (icheck -s), the standard
 * repair after restor(8), which does not rebuild the free list.  V7/V6 only
 * (V1 has a bitmap allocator, and PDP-7's rebuild is not implemented).
 * -r resolves duplicate blocks (salv -a): each duplicate is copied to a fresh
 * block and the second reference re-pointed, then the free list is rebuilt.
 * V7/V6 only.
 * -n report only, change nothing (the default; explicit, and refused together
 * with the modifying flags -s/-r/-C).
 * -N ino prints the pathname(s) of an inode (ncheck).  Every edition.
 * -C ino zeroes an inode (clri).  Every edition.
 */
#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>

#include "filsys.h"
#include "v1fs.h"
#include "v6fs.h"
#include "v7fs.h"
#include "pdp7fs.h"

/* Parse the -v edition argument.  Returns a FILSYS_* selector, or -1 on error.
 * Accepts a leading 'v' (v6, v7) and rejects everything else. */
static int parse_edition(const char *s)
{
    if (s[0] == 'v' || s[0] == 'V')
        s++;
    if (strcmp(s, "v0") == 0 || strcmp(s, "0") == 0 ||
        strcmp(s, "pdp7") == 0 || strcmp(s, "p7") == 0)
        return FILSYS_PDP7;
    if (strcmp(s, "1") == 0)
        return FILSYS_V1;
    if (strcmp(s, "4") == 0 || strcmp(s, "5") == 0 || strcmp(s, "6") == 0)
        return FILSYS_V6;
    if (strcmp(s, "7") == 0)
        return FILSYS_V7;
    if (strcmp(s, "32") == 0 || strcmp(s, "32v") == 0)
        return FILSYS_32V;
    return -1;
}

int main(int argc, char **argv)
{
    const char *path;
    uint64_t offblock = 0;
    int edition = FILSYS_V7;   /* default: V7, matching mount.filsys's requirement */
    int salvage = 0, resolve = 0, ncheck = 0, clri = 0, nochange = 0;
    uint32_t ino = 0;
    int c;

    while ((c = getopt(argc, argv, "v:o:srnN:C:")) != -1) {
        switch (c) {
        case 'v':
            edition = parse_edition(optarg);
            if (edition < 0) {
                fprintf(stderr, "fsck.filsys: bad edition '%s' (want v0|1|4|5|6|7|32v)\n", optarg);
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
                "usage: fsck.filsys [-v <v0|1|4|5|6|7|32v>] [-o block] [-s] [-r] [-n] image\n"
                "       fsck.filsys [-v <edition>] [-o block] -N ino image\n"
                "       fsck.filsys [-v <edition>] [-o block] -C ino image\n");
            return 2;
        }
    }
    if (optind >= argc) {
        fprintf(stderr,
            "usage: fsck.filsys [-v <v0|1|4|5|6|7|32v>] [-o block] [-s] [-r] [-n] image\n"
            "       fsck.filsys [-v <edition>] [-o block] -N ino image\n"
            "       fsck.filsys [-v <edition>] [-o block] -C ino image\n");
        return 2;
    }
    path = argv[optind];

    /* -s (salvage) and -r (resolve dups) are V7/V6-only: V1 uses a bitmap
     * allocator and PDP-7's free-list rebuild is not implemented.  -N (ncheck)
     * and -C (clri) work on every edition. */
    if ((edition == FILSYS_V1 || edition == FILSYS_PDP7) && (salvage || resolve)) {
        fprintf(stderr, "fsck.filsys: -s/-r are V7/V6 only\n");
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

    if (edition == FILSYS_V6) {
        int readonly = !(salvage || resolve || clri);
        v6fs_t fs;
        int rc = v6fs_open(&fs, path, readonly, offblock * V6_BSIZE);
        if (rc < 0) {
            fprintf(stderr, "%s: %s\n", path, strerror(-rc));
            return 1;
        }
        int err;
        if (ncheck)
            err = v6fs_ncheck(&fs, ino);
        else if (clri)
            err = v6fs_clri(&fs, ino);
        else if (resolve)
            err = v6fs_resolve_dups(&fs);
        else {
            v6_check_t rep;
            err = v6fs_check(&fs, &rep, salvage);
        }
        v6fs_close(&fs);
        return err ? 1 : 0;
    }

    if (edition == FILSYS_V1) {
        int readonly = !clri;
        v1fs_t fs;
        int rc = v1fs_open(&fs, path, readonly, offblock * V1_BSIZE);
        if (rc < 0) {
            fprintf(stderr, "%s: %s\n", path, strerror(-rc));
            return 1;
        }
        int err;
        if (ncheck)
            err = v1fs_ncheck(&fs, ino);
        else if (clri)
            err = v1fs_clri(&fs, ino);
        else {
            v1_check_t rep;
            err = v1fs_check(&fs, &rep);
        }
        v1fs_close(&fs);
        return err ? 1 : 0;
    }

    if (edition == FILSYS_PDP7) {
        int readonly = !clri;
        p7fs_t fs;
        int rc = p7fs_open(&fs, path, readonly, offblock * P7_BLOCKBYTES);
        if (rc < 0) {
            fprintf(stderr, "%s: %s\n", path, strerror(-rc));
            return 1;
        }
        int err;
        if (ncheck)
            err = p7fs_ncheck(&fs, ino);
        else if (clri)
            err = p7fs_clri(&fs, ino);
        else {
            p7_check_t rep;
            err = p7fs_check(&fs, &rep);
        }
        p7fs_close(&fs);
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
