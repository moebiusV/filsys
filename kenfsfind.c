/* kenfs 0.9.0 — 2026-08-26 — Copyright (C) 2026 David Walther */
/* SPDX-License-Identifier: ISC */
/* kenfsfind.c — locate filesystem superblocks on a raw disk image.
 *
 * A V7 (and V4/V5/V6/32V) disk is a set of *partitions* in one file, and the
 * partition table is compiled into the kernel, not stored on the disk.  This
 * tool finds the filesystems by looking for their superblocks directly, and —
 * with -i — by scanning for inode-table runs and tracing backwards to the
 * superblock (block 2 of a filesystem is its first inode-table block, so the
 * block just before an inode-table run is the superblock).  The latter finds
 * filesystems even when a damaged superblock defeats the direct scan.
 *
 * Usage:
 *     kenfsfind [-c N] [-i] <image>
 *
 * -c N   scan every N-th block (use the disk's blocks-per-cylinder, e.g. 418
 *        for an RP06, to speed up a big image)
 * -i     also trace inode-table runs backwards to their superblocks
 *
 * Each hit is printed as:
 *     fs @ block START  (byte BYTES)  V7  isize=N fsize=M
 * Mount the partition in place with kenfsmount -o offset=BYTES.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define BSIZE 512
#define V6_NICFREE 100
#define V6_NICINOD 100
#define V7_NICFREE 50
#define V7_NICINOD 100

static uint16_t get16le(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t get32me(const uint8_t *p) {   /* PDP-11 middle-endian */
    return ((uint32_t)get16le(p) << 16) | get16le(p + 2);
}
static uint32_t get32le(const uint8_t *p) {   /* VAX little-endian (32V) */
    return (uint32_t)get16le(p) | ((uint32_t)get16le(p + 2) << 16);
}

/* Does block b (a candidate superblock) describe a V7 filesystem whose
 * superblock sits at absolute block `bno` (so the fs starts at bno-1)?
 * The free-list entries are checked to be in-range, which rejects the vast
 * majority of random data that merely *looks* superblock-shaped. */
static int super_v7(const uint8_t *b, uint32_t bno, uint32_t nblocks,
                    const char **edition, uint16_t *isize, uint32_t *fsize) {
    uint16_t isz = get16le(b);
    if (isz < 3)
        return 0;
    uint16_t nfree = get16le(b + 6);
    uint16_t ninode = get16le(b + 208);
    if (nfree > V7_NICFREE || ninode > V7_NICINOD)
        return 0;
    for (int le = 0; le < 2; le++) {   /* 0 = PDP-11 ME (V7), 1 = VAX LE (32V) */
        uint32_t fsz = le ? get32le(b + 2) : get32me(b + 2);
        if (fsz <= isz || (uint64_t)bno - 1 + fsz > nblocks)
            continue;
        int ok = 1;
        for (int i = 0; i < nfree; i++) {
            uint32_t fb = le ? get32le(b + 8 + 4 * i) : get32me(b + 8 + 4 * i);
            if (fb != 0 && (fb < isz || fb >= fsz)) { ok = 0; break; }
        }
        if (ok) {
            *edition = le ? "32V" : "V7";
            *isize = isz; *fsize = fsz;
            return 1;
        }
    }
    return 0;
}

/* V6 superblock (all 16-bit fields), same free-list validation. */
static int super_v6(const uint8_t *b, uint32_t bno, uint32_t nblocks,
                    uint16_t *isize, uint32_t *fsize) {
    uint16_t isz = get16le(b);
    if (isz < 3)
        return 0;
    uint16_t fsz = get16le(b + 2);
    uint16_t nfree = get16le(b + 4);
    uint16_t ninode = get16le(b + 206);
    if (fsz <= isz || nfree > V6_NICFREE || ninode > V6_NICINOD ||
        (uint64_t)bno - 1 + fsz > nblocks)
        return 0;
    for (int i = 0; i < nfree; i++) {
        uint16_t fb = get16le(b + 6 + 2 * i);
        if (fb != 0 && (fb < isz || fb >= fsz))
            return 0;
    }
    *isize = isz; *fsize = fsz;
    return 1;
}

/* Is one 64-byte on-disk inode plausible (free, or a valid type with small
 * counts)? */
static int inode_ok(const uint8_t *d) {
    uint16_t mode = get16le(d);
    if (mode == 0)
        return 1;   /* free inode */
    uint16_t t = mode & 0170000;
    if (t != 0100000 && t != 0040000 && t != 0020000 && t != 0060000)
        return 0;
    return get16le(d + 2) < 128 && get16le(d + 4) < 256 && get16le(d + 6) < 256;
}

/* Is block b an inode-table block (8 plausible inodes, at least one used)? */
static int inode_block(const uint8_t *b) {
    int ok = 0, used = 0;
    for (int i = 0; i < 8; i++) {
        const uint8_t *d = b + i * 64;
        if (inode_ok(d)) ok++;
        if (get16le(d) != 0) used++;
    }
    return ok >= 6 && used > 0;
}

int main(int argc, char **argv) {
    int stride = 1, backtrace = 0;
    const char *image = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) { stride = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "-i")) { backtrace = 1; }
        else if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "usage: kenfsfind [-c N] [-i] <image>\n");
            return 2;
        } else { image = argv[i]; }
    }
    if (!image || stride < 1) {
        fprintf(stderr, "usage: kenfsfind [-c N] [-i] <image>\n");
        return 2;
    }

    int fd = open(image, O_RDONLY);
    if (fd < 0) { perror(image); return 1; }
    struct stat st;
    if (fstat(fd, &st) || st.st_size < BSIZE) { fprintf(stderr, "cannot size image\n"); return 1; }
    uint32_t nblocks = (uint32_t)(st.st_size / BSIZE);
    uint8_t buf[BSIZE];

    int found = 0;
    for (uint32_t bno = 1; bno < nblocks; bno += (uint32_t)stride) {
        if (pread(fd, buf, BSIZE, (off_t)bno * BSIZE) != BSIZE)
            continue;

        /* direct superblock detection */
        const char *ed = NULL; uint16_t isz = 0; uint32_t fsz = 0;
        if (super_v7(buf, bno, nblocks, &ed, &isz, &fsz) || super_v6(buf, bno, nblocks, &isz, &fsz)) {
            if (!ed) ed = "V6";
            printf("fs @ block %u  (byte %llu)  %s  isize=%u fsize=%u\n",
                   bno - 1, (unsigned long long)(bno - 1) * BSIZE, ed, isz, fsz);
            found++;
        }

        /* inode-table backtrace: is bno the first inode block of a filesystem
         * whose superblock is at bno-1? */
        if (backtrace && inode_block(buf)) {
            const char *ed = NULL; uint16_t isz = 0; uint32_t fsz = 0;
            if (bno >= 2 && pread(fd, buf, BSIZE, (off_t)(bno - 1) * BSIZE) == BSIZE &&
                (super_v7(buf, bno - 1, nblocks, &ed, &isz, &fsz) ||
                 super_v6(buf, bno - 1, nblocks, &isz, &fsz))) {
                if (!ed) ed = "V6";
                printf("fs @ block %u  (byte %llu)  %s  isize=%u fsize=%u   [via inode backtrace]\n",
                       bno - 2, (unsigned long long)(bno - 2) * BSIZE, ed, isz, fsz);
                found++;
            }
        }
    }
    close(fd);
    return found ? 0 : 1;
}
