/* filsys 1.7.0 - 2026-09-06 - Copyright (C) 2026 David Walther */
/* SPDX-License-Identifier: ISC */
/* fsck.filsys.c - check and repair a Research Unix (PDP-7 through 32V)
 * filesystem on a disk image.
 *
 * Usage:
 *     fsck.filsys [-v <pdp7|v1|v6|v7|vax32|coherent|xenix|bsd29|bsd211>] [-o block] [-s] [-r] [-p] [-f] [-n] image
 *     fsck.filsys [-v <edition>] [-o block] -N ino image
 *     fsck.filsys [-v <edition>] [-o block] -C ino image
 *
 * A Research Unix disk image holds one or more filesystems ("partitions"); the
 * partition table is compiled into the kernel, not stored on the disk.  So by
 * default fsck.filsys checks the filesystem at block 0.  -o selects a different
 * block offset.
 *
 * -v selects the edition: pdp7 (the word-addressed PDP-7), v1/v2/v3 (First
 * through Third Edition, one bitmap format), v4/v5/v6 (the V6 format,
 * byte-identical), v7 (V7), vax32 (V7 for the VAX), coherent (Mark Williams Co.).
 * The default is v7.  (V7 had no single fsck -- it used icheck(8) +
 * dcheck(8); the check folds both in.)
 *
 * -s rebuilds the free list from the block scan (icheck -s), the standard
 * repair after restor(8), which does not rebuild the free list.  V7/V6 and
 * PDP-7 rebuild their free list; V1 rebuilds its free map.
 * -r resolves duplicate blocks (salv -a): each duplicate is copied to a fresh
 * block and the second reference re-pointed, then the free list is rebuilt.
 * Every edition.
 * -p preens: fixes the safe subset without prompting (unreferenced inodes are
 * reconnected to lost+found, link counts corrected, blocks missing from or
 * doubly-listed in the free map reconciled).  Every edition.
 * -y answers yes to every repair question (like -p, but for the whole safe set).
 * -i prompts on standard input before each repair (interactive; otherwise fsck
 * is a batch check).  Every edition.
 * -f forces a check even when the superblock is marked clean (V6/V7 only).
 * -n report only, change nothing (the default; explicit, and refused together
 * with the modifying flags -s/-r/-p/-i/-y/-C).
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
#include "filsys_ops.h"
#include "v1fs.h"
#include "v7fs.h"
#include "pdp7fs.h"

int main(int argc, char **argv)
{
    const char *path;
    uint64_t offblock = 0;
    int edition = -1;   /* no default: the version must be named explicitly */
    int salvage = 0, resolve = 0, ncheck = 0, clri = 0, nochange = 0;
    int preen = 0, force = 0, yes = 0, ask = 0;
    uint32_t ino = 0;
    int c;

    while ((c = getopt(argc, argv, "v:o:srpfinN:C:y")) != -1) {
        switch (c) {
        case 'v':
            edition = filsys_edition_by_name(optarg);
            if (edition < 0) {
                fprintf(stderr, "fsck.filsys: bad edition '%s'\n", optarg);
                return 2;
            }
            break;
        case 'o':
            offblock = strtoull(optarg, NULL, 0);
            break;
        case 's':
            salvage = 1;
            break;
        case 'p':
            preen = 1;
            break;
        case 'f':
            force = 1;
            break;
        case 'i':
            ask = 1;
            break;
        case 'n':
            nochange = 1;
            break;
        case 'y':
            yes = 1;
            break;
        case 'N':
            ncheck = 1;
            ino = (uint32_t)strtoul(optarg, NULL, 0);
            break;
        case 'C':
            clri = 1;
            ino = (uint32_t)strtoul(optarg, NULL, 0);
            break;
        default:
            fprintf(stderr,
                "usage: fsck.filsys -v <edition> [-o block] [-s] [-r] [-p] [-i] [-y] [-f] [-n] image\n"
                "       fsck.filsys -v <edition> [-o block] -N ino image\n"
                "       fsck.filsys -v <edition> [-o block] -C ino image\n"
                "  editions: %s\n", filsys_editions_usage());
            return 2;
        }
    }
    if (optind >= argc) {
        fprintf(stderr,
            "usage: fsck.filsys -v <edition> [-o block] [-s] [-r] [-p] [-i] [-y] [-f] [-n] image\n"
            "       fsck.filsys -v <edition> [-o block] -N ino image\n"
            "       fsck.filsys -v <edition> [-o block] -C ino image\n"
            "  editions: %s\n", filsys_editions_usage());
        return 2;
    }
    path = argv[optind];

    if (edition < 0) {
        fprintf(stderr, "fsck.filsys: no filesystem version given; use -v <edition>\n");
        return 2;
    }

    /* -s (salvage), -r (resolve dups), -N (ncheck) and -C (clri) all work on
     * every edition: V1 rebuilds its free map, PDP-7 its free list.  -i and -y
     * are repair modes, so they conflict with -n (report only). */
    if (nochange && (salvage || resolve || clri || preen || yes || ask)) {
        fprintf(stderr, "fsck.filsys: -n (no change) conflicts with -s/-r/-p/-i/-y/-C\n");
        return 2;
    }

    int mode = 0;
    if (salvage) mode |= FILSYS_CK_SALVAGE;
    if (preen)   mode |= FILSYS_CK_PREEN;
    if (force)   mode |= FILSYS_CK_FORCE;
    if (yes)     mode |= FILSYS_CK_YES;
    if (ask)     mode |= FILSYS_CK_ASK;

    printf("%s:", path);
    if (offblock)
        printf(" (offset block %llu)", (unsigned long long)offblock);
    printf("\n");

    if (edition == FILSYS_V6) {
        int readonly = !(salvage || resolve || clri || preen || yes || ask);
        filsys_edition_t fs = filsys_getformat(edition);
        int rc = fs.ops->open(&fs, path, readonly, &fs, offblock * fs.bsize);
        if (rc < 0) {
            fprintf(stderr, "%s: %s\n", path, strerror(-rc));
            return 1;
        }
        int err;
        if (ncheck)
            err = filsys_ncheck(&fs, ino);
        else if (clri)
            err = filsys_clri(&fs, ino);
        else if (resolve)
            err = filsys_resolve_dups(&fs);
        else {
            v7_check_t rep;
            err = v6_check(&fs, &rep, mode);
        }
        fs.ops->close(&fs);
        return err ? 1 : 0;
    }

    if (edition == FILSYS_V1) {
        int readonly = !(salvage || resolve || clri || preen || yes || ask);
        filsys_edition_t fs = filsys_getformat(edition);
        int rc = v1fs_open(&fs, path, readonly, &fs, offblock * V1_BSIZE);
        if (rc < 0) {
            fprintf(stderr, "%s: %s\n", path, strerror(-rc));
            return 1;
        }
        int err;
        if (ncheck)
            err = filsys_ncheck(&fs, ino);
        else if (clri)
            err = filsys_clri(&fs, ino);
        else if (resolve)
            err = filsys_resolve_dups(&fs);
        else {
            v1_check_t rep;
            err = v1fs_check(&fs, &rep, mode);
        }
        v1fs_close(&fs);
        return err ? 1 : 0;
    }

    if (edition == FILSYS_PDP7) {
        int readonly = !(salvage || resolve || clri || preen || yes || ask);
        filsys_edition_t fs = filsys_getformat(edition);
        int rc = p7fs_open(&fs, path, readonly, &fs, offblock * P7_BLOCKBYTES);
        if (rc < 0) {
            fprintf(stderr, "%s: %s\n", path, strerror(-rc));
            return 1;
        }
        int err;
        if (ncheck)
            err = filsys_ncheck(&fs, ino);
        else if (clri)
            err = filsys_clri(&fs, ino);
        else if (resolve)
            err = filsys_resolve_dups(&fs);
        else {
            p7_check_t rep;
            err = p7fs_check(&fs, &rep, mode);
        }
        p7fs_close(&fs);
        return err ? 1 : 0;
    }

    int readonly = !(salvage || resolve || clri || preen || yes || ask);
    filsys_edition_t fs = filsys_getformat(edition);
    int rc = v7fs_open(&fs, path, readonly, &fs, offblock * fs.bsize);
    if (rc < 0) {
        fprintf(stderr, "%s: %s\n", path, strerror(-rc));
        return 1;
    }

    int err = 0;
    if (edition == FILSYS_BSD211) {
        /* 2.11BSD: check only (no ncheck/clri/salvage maintenance ops). */
        v7_check_t rep;
        err = bsd211_check(&fs, &rep, mode);
    } else if (ncheck) {
        err = filsys_ncheck(&fs, ino);
    } else if (clri) {
        err = filsys_clri(&fs, ino);
    } else if (resolve) {
        err = filsys_resolve_dups(&fs);
    } else {
        v7_check_t rep;
        err = v7fs_check(&fs, &rep, mode);
    }
    v7fs_close(&fs);
    return err ? 1 : 0;
}
