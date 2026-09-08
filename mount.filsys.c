/* filsys 1.8.0 - 2026-09-06 - Copyright (C) 2026 David Walther */
/* SPDX-License-Identifier: ISC */
/* mount.filsys.c - mount a Research Unix filesystem image (PDP-11) as a FUSE
 * filesystem, selecting the on-disk edition at run time.
 *
 * This file is main(): argument parsing, image opening, and the integrity
 * check, shared by both FUSE adapters.  The callback bodies live in the
 * FUSE-free fuse_core.c; the adapters (fuseops.c for FUSE3, fuseops_openbsd.c
 * for OpenBSD's base-libfuse 2.6) translate their callback signatures onto it.
 *
 * One binary, every edition we care about (canonical -v names):
 *
 *     pdp7 v1 v6 v7 vax32 coherent xenix bsd29 bsd211
 *
 * pdp7 is the word-addressed PDP-7; v1/v2/v3 share one bitmap format; v4/v5/v6
 * are byte-identical (the V6 format); v7 is V7; vax32 is 32V (VAX, little-endian);
 * coherent is Mark Williams Coherent (V7 middle-endian, 64-entry free cache).
 *
 * The on-disk layout is the 1969 Thompson/Canaday/Ritchie design - a flat
 * i-list at a fixed offset, directories as ordinary files of 16-byte entries,
 * and device files - carried essentially unchanged from the PDP-7 through V6,
 * then widened in V7 (64-byte inode, 24-bit block numbers).  See filsys.5.
 *
 * Usage:
 *     mount.filsys -v <pdp7|v1|v6|v7|vax32|coherent|xenix|bsd29|bsd211> [-o offset=N[,version=N][,uid=N,gid=N,...]]
 *                    [-r] [-f] [-d] <image> <mountpoint>
 *     mount.filsys -v <pdp7|v1|v6|v7|vax32|coherent|xenix|bsd29|bsd211> [-o offset=N] -c <image>   # integrity check
 *
 * `-o offset=N` mounts a filesystem that lives at byte offset N within the
 * file (a partition of a larger disk image), instead of one at block 0.
 * `-o version=N` selects the edition (so `mount -t filsys` can pass it in `-o`
 * rather than `-v`, which mount(8) has no generic way to supply).
 * `-o uid=N,gid=N` override the reported ownership (default: the mounting
 * user, so a nested mount point is writable).  `-o packing=NAME` selects the
 * PDP-7 word container codec (rb09|packed18|rim) when mounting a non-simh
 * dump.  Any other -o option is passed through to FUSE (e.g. allow_other).
 * Installed as sbin/mount.filsys, so `mount -t filsys device dir -o
 * version=7,offset=N` works.
 */
#include <config.h>
#include "filsys.h"
#include "fuse_core.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *p) {
    fprintf(stderr,
            "usage: %s -v <%s> [-o offset=N[,version=N][,packing=NAME][,arch=NAME][,uid=N][,gid=N]]\n"
            "                [-r] [-f] [-d] [-F] <image> <mountpoint>\n"
            "       %s -v <edition> [-o offset=N] -c <image>   # integrity check\n",
            p, filsys_editions_usage(), p);
}

int main(int argc, char *argv[]) {
    int ver = -1, readonly = 0, foreground = 0, debug = 0, check = 0, force = 0;
    int no_lock = 0;
    uint64_t offset = 0;
    int uid = -1, gid = -1;   /* -1 = report as the mounting user */
    const char *packing = NULL; /* PDP-7 word container codec (rb09|packed18|rim) */
    const char *arch = NULL;    /* CPU arch: overrides the edition's byte order (3b2/68k = BE) */
    filsys_geom_t geom = {0, -1, NULL}; /* V8-family -o blocksize/freemap/byteorder overrides */
    char fuse_opts[512] = ""; /* -o options passed through to FUSE (allow_other, ...) */
    int c;

    /* getopt, not a hand-rolled loop, so the mount(8) argument order
     * `mount.filsys image dir -o opts` parses (GNU getopt permutes operands to
     * the front, so trailing options are fine). */
    while ((c = getopt(argc, argv, "v:o:rfdcF")) != -1) {
        switch (c) {
        case 'v': {
            int v = filsys_parse_edition(argv[0], optarg);
            if (v < 0) {
                usage(argv[0]);
                return 2;
            }
            ver = v;
            break;
        }
        case 'o': {
            char *opts = strdup(optarg);
            int bad = 0;
            for (char *tok = strtok(opts, ","); tok; tok = strtok(NULL, ",")) {
                if (!strncmp(tok, "offset=", 7)) {
                    char *end = NULL;
                    offset = strtoull(tok + 7, &end, 0);
                    if (!end || *end) { bad = 1; break; }
                } else if (!strncmp(tok, "version=", 8)) {
                    int v = filsys_edition_by_name(tok + 8);
                    if (v < 0) bad = 1; else ver = v;
                } else if (!strncmp(tok, "uid=", 4)) {
                    uid = atoi(tok + 4);
                } else if (!strncmp(tok, "gid=", 4)) {
                    gid = atoi(tok + 4);
                } else if (!strncmp(tok, "packing=", 8)) {
                    packing = strdup(tok + 8);   /* copy: opts is freed below */
                } else if (!strncmp(tok, "arch=", 5)) {
                    arch = strdup(tok + 5);      /* copy: opts is freed below */
                } else if (!strncmp(tok, "blocksize=", 10)) {
                    char *end = NULL;
                    geom.blocksize = (uint32_t)strtoul(tok + 10, &end, 0);
                    if (!end || *end) bad = 1;
                } else if (!strncmp(tok, "freemap=", 8)) {
                    if (!strcmp(tok + 8, "list"))       geom.freemap = FILSYS_FREEMAP_LIST;
                    else if (!strcmp(tok + 8, "bitmap")) geom.freemap = FILSYS_FREEMAP_BITMAP;
                    else bad = 1;
                } else if (!strncmp(tok, "bitmap=", 7)) {
                    if (!strcmp(tok + 7, "super"))      geom.freemap = FILSYS_FREEMAP_BITMAP;
                    else if (!strcmp(tok + 7, "blocks")) geom.freemap = FILSYS_FREEMAP_BIGMAP;
                    else bad = 1;
                } else if (!strncmp(tok, "byteorder=", 10)) {
                    geom.byteorder = strdup(tok + 10); /* copy: opts is freed below */
                } else if (!strcmp(tok, "no_lock")) {
                    no_lock = 1;   /* skip the advisory RW lock (debugging) */
                } else {
                    /* pass anything else through to FUSE (allow_other, ...) */
                    if (fuse_opts[0]) strncat(fuse_opts, ",", sizeof(fuse_opts) - strlen(fuse_opts) - 1);
                    strncat(fuse_opts, tok, sizeof(fuse_opts) - strlen(fuse_opts) - 1);
                }
            }
            free(opts);
            if (bad) { usage(argv[0]); return 2; }
            break;
        }
        case 'r': readonly = 1; break;
        case 'f': foreground = 1; break;
        case 'd': debug = 1; break;
        case 'c': check = 1; break;
        case 'F': force = 1; break;   /* open despite a bad superblock magic */
        default: usage(argv[0]); return 2;
        }
    }
    if (ver < 0) {
        fprintf(stderr, "%s: no filesystem version given; use -v <edition>\n", argv[0]);
        usage(argv[0]);
        return 2;
    }
    if (check && (argc - optind != 1)) { fprintf(stderr, "usage: %s -c <image>\n", argv[0]); return 2; }
    if (!check && (argc - optind != 2)) { usage(argv[0]); return 2; }
    const char *image = argv[optind];
    const char *mountpoint = check ? NULL : argv[optind + 1];

    filsys_t *k = NULL;
    const char *errmsg = NULL;
    int rc = filsys_open_arch(&k, ver, image, readonly || check, offset,
                              uid >= 0 ? (uid_t)uid : getuid(),
                              gid >= 0 ? (gid_t)gid : getgid(), packing, arch,
                              force, &geom, no_lock, &errmsg);
    free((void *)packing);
    free((void *)arch);
    free((void *)geom.byteorder);
    if (rc) {
        fprintf(stderr, "filsys: cannot open %s: %s\n", image,
                errmsg ? errmsg : strerror(-rc));
        return 1;
    }

    if (check) {
        int crc = filsys_check(k);
        int cl = filsys_close(k);
        if (cl)
            fprintf(stderr, "filsys: final flush failed: %s\n", strerror(-cl));
        return (crc == 0 && cl == 0) ? 0 : 1;
    }

    if (readonly) {
        if (fuse_opts[0]) strncat(fuse_opts, ",", sizeof(fuse_opts) - strlen(fuse_opts) - 1);
        strncat(fuse_opts, "ro", sizeof(fuse_opts) - strlen(fuse_opts) - 1);
    }
    /* Let the kernel enforce the mode bits returned by getattr, so -o allow_other
     * honours them instead of granting blanket access.  A mount option, not a
     * fuse_config field, so it works on both FUSE2 and FUSE3. */
    if (fuse_opts[0]) strncat(fuse_opts, ",", sizeof(fuse_opts) - strlen(fuse_opts) - 1);
    strncat(fuse_opts, "default_permissions", sizeof(fuse_opts) - strlen(fuse_opts) - 1);

    fuse_ctx_t ctx;
    ctx.fs = k;
    ctx.uid = uid >= 0 ? (uid_t)uid : getuid();
    ctx.gid = gid >= 0 ? (gid_t)gid : getgid();

    fuse_mount_opts_t mopts;
    mopts.argv0 = argv[0];
    mopts.mountpoint = mountpoint;
    mopts.foreground = foreground;
    mopts.debug = debug;
    mopts.fuse_opts = fuse_opts;

    rc = fuse_run(&ctx, &mopts);

    int cl = filsys_close(k);
    if (cl) {
        fprintf(stderr, "filsys: final flush failed: %s\n", strerror(-cl));
        if (rc == 0)
            rc = 1;
    }
    return rc;
}
