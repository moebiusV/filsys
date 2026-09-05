/* filsys 1.2.6 - 2026-08-26 - Copyright (C) 2026 David Walther */
/* SPDX-License-Identifier: ISC */
/* fsck.filsys.c - check and repair a Research Unix (PDP-7 through 32V)
 * filesystem on a disk image.
 *
 * Usage:
 *     fsck.filsys [-v <pdp7|v1|v2|v3|v4|v5|v6|v7|32v|coherent>] [-o block] [-s] [-r] [-p] [-f] [-n] image
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
 * byte-identical), v7 (V7), 32v (V7 for the VAX), coherent (Mark Williams Co.).
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
    if (strcmp(s, "1") == 0 || strcmp(s, "2") == 0 || strcmp(s, "3") == 0)
        return FILSYS_V1;   /* V1, V2, V3: one format (10-byte dirents, bitmap, root 41) */
    if (strcmp(s, "4") == 0 || strcmp(s, "5") == 0 || strcmp(s, "6") == 0)
        return FILSYS_V6;
    if (strcmp(s, "7") == 0)
        return FILSYS_V7;
    if (strcmp(s, "32") == 0 || strcmp(s, "32v") == 0)
        return FILSYS_32V;
    if (strcmp(s, "coherent") == 0 || strcmp(s, "coh") == 0 || strcmp(s, "33") == 0)
        return FILSYS_COHERENT;
    return -1;
}

int main(int argc, char **argv)
{
    const char *path;
    uint64_t offblock = 0;
    int edition = FILSYS_V7;   /* default: V7, matching mount.filsys's requirement */
    int salvage = 0, resolve = 0, ncheck = 0, clri = 0, nochange = 0;
    int preen = 0, force = 0, yes = 0, ask = 0;
    uint32_t ino = 0;
    int c;

    while ((c = getopt(argc, argv, "v:o:srpfinN:C:y")) != -1) {
        switch (c) {
        case 'v':
            edition = parse_edition(optarg);
            if (edition < 0) {
                fprintf(stderr, "fsck.filsys: bad edition '%s' (want pdp7|v1|v2|v3|v4|v5|v6|v7|32v|coherent)\n", optarg);
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
            ino = strtoul(optarg, NULL, 0);
            break;
        case 'C':
            clri = 1;
            ino = strtoul(optarg, NULL, 0);
            break;
        default:
            fprintf(stderr,
                "usage: fsck.filsys [-v <pdp7|v1|v2|v3|v4|v5|v6|v7|32v|coherent>] [-o block] [-s] [-r] [-p] [-i] [-y] [-f] [-n] image\n"
                "       fsck.filsys [-v <edition>] [-o block] -N ino image\n"
                "       fsck.filsys [-v <edition>] [-o block] -C ino image\n");
            return 2;
        }
    }
    if (optind >= argc) {
        fprintf(stderr,
            "usage: fsck.filsys [-v <pdp7|v1|v2|v3|v4|v5|v6|v7|32v|coherent>] [-o block] [-s] [-r] [-p] [-i] [-y] [-f] [-n] image\n"
            "       fsck.filsys [-v <edition>] [-o block] -N ino image\n"
            "       fsck.filsys [-v <edition>] [-o block] -C ino image\n");
        return 2;
    }
    path = argv[optind];

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
            err = v6fs_check(&fs, &rep, mode);
        }
        v6fs_close(&fs);
        return err ? 1 : 0;
    }

    if (edition == FILSYS_V1) {
        int readonly = !(salvage || resolve || clri || preen || yes || ask);
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
        else if (resolve)
            err = v1fs_resolve_dups(&fs);
        else {
            v1_check_t rep;
            err = v1fs_check(&fs, &rep, mode);
        }
        v1fs_close(&fs);
        return err ? 1 : 0;
    }

    if (edition == FILSYS_PDP7) {
        int readonly = !(salvage || resolve || clri || preen || yes || ask);
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
        else if (resolve)
            err = p7fs_resolve_dups(&fs);
        else {
            p7_check_t rep;
            err = p7fs_check(&fs, &rep, mode);
        }
        p7fs_close(&fs);
        return err ? 1 : 0;
    }

    int readonly = !(salvage || resolve || clri || preen || yes || ask);
    v7fs_t fs;
    int bomode = (edition == FILSYS_32V) ? 1 : (edition == FILSYS_COHERENT) ? 2 : 0;
    int rc = v7fs_open(&fs, path, readonly, bomode, offblock * V7_BSIZE);
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
        err = v7fs_check(&fs, &rep, mode);
    }
    v7fs_close(&fs);
    return err ? 1 : 0;
}
