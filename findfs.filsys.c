/* filsys 1.7.0 - 2026-09-06 - Copyright (C) 2026 David Walther */
/* SPDX-License-Identifier: ISC */
/* findfs.filsys.c - locate filesystem superblocks on a raw disk image.
 *
 * A V7 (and V4/V5/V6/32V/Coherent) disk is a set of *partitions* in one file, and the
 * partition table is compiled into the kernel, not stored on the disk.  This
 * tool finds the filesystems by looking for their superblocks directly, and -
 * with -i - by scanning for inode-table runs and tracing backwards to the
 * superblock (block 2 of a filesystem is its first inode-table block, so the
 * block just before an inode-table run is the superblock).  The latter finds
 * filesystems even when a damaged superblock defeats the direct scan.
 *
 * Usage:
 *     findfs.filsys [-s N] [-i] <image>
 *
 * -s N   scan every N-th block (use the disk's blocks-per-cylinder, e.g. 418
 *        for an RP06, to speed up a big image)
 * -i     also trace inode-table runs backwards to their superblocks
 *
 * Each hit is printed as:
 *     fs @ block START  (byte BYTES)  V7  isize=N fsize=M   (or 32V/Coherent/V6)
 * Mount the partition in place with mount.filsys -o offset=BYTES.
 */
#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "byteorder.h"

enum {
    BSIZE      = 512,
    V6_NICFREE = 100,
    V6_NICINOD = 100,
    V7_NICFREE = 50,
    V7_COH_NICFREE = 64,   /* Coherent free-list cache depth */
    V7_XEN_NICFREE = 100,  /* Xenix free-list cache depth */
    XENIX_MAGIC = 0x2b5544,/* Xenix superblock magic (offset 1016) */
    V7_NICINOD = 100
};

/* Does block b (a candidate superblock) describe a V7 filesystem whose
 * superblock sits at absolute block `bno` (so the fs starts at bno-1)?
 * The free-list entries are checked to be in-range, which rejects the vast
 * majority of random data that merely *looks* superblock-shaped. */
static int super_v7(const uint8_t *b, uint32_t bno, uint32_t nblocks,
                    const char **edition, uint16_t *isize, uint32_t *fsize) {
    uint16_t isz = bo_get16le(b);
    if (isz < 3)
        return 0;
    /* Three V7-family layouts: V7 (PDP-11 middle-endian, 2-byte), 32V (VAX
     * little-endian, 4-byte aligned), and Coherent (middle-endian, 2-byte, a
     * 64-entry free cache pushing s_ninode to +264). */
    static const struct {
        int le, coh;
        const char *name;
        int nicfree;
    } L[] = {
        { 0, 0, "V7",       V7_NICFREE },
        { 1, 0, "32V",      V7_NICFREE },
        { 0, 1, "Coherent", V7_COH_NICFREE },
    };
    for (int k = 0; k < 3; k++) {
        int le  = L[k].le, coh = L[k].coh;
        /* 32V aligns daddr_t to 4 bytes, so s_fsize/s_nfree/s_free/s_ninode
         * sit 2 bytes later than in V7; Coherent keeps the 2-byte packing. */
        int nfree_off  = (le && !coh) ? 8 : 6;
        int ninode_off = coh ? 264 : (le ? 212 : 208);
        int free_off   = (le && !coh) ? 12 : 8;
        uint16_t nfree  = bo_get16le(b + nfree_off);
        uint16_t ninode = bo_get16le(b + ninode_off);
        if (nfree > L[k].nicfree || ninode > V7_NICINOD)
            continue;
        uint32_t fsz = le ? bo_get32le(b + 4) : bo_get32me(b + 2);
        if (fsz <= isz || (uint64_t)bno - 1 + fsz > nblocks)
            continue;
        int ok = 1;
        for (int i = 0; i < nfree; i++) {
            uint32_t fb = le ? bo_get32le(b + free_off + 4 * i) : bo_get32me(b + free_off + 4 * i);
            if (fb != 0 && (fb < isz || fb >= fsz)) { ok = 0; break; }
        }
        if (ok) {
            *edition = L[k].name;
            *isize = isz; *fsize = fsz;
            return 1;
        }
    }
    return 0;
}

/* V6 superblock (all 16-bit fields), same free-list validation. */
static int super_v6(const uint8_t *b, uint32_t bno, uint32_t nblocks,
                    uint16_t *isize, uint32_t *fsize) {
    uint16_t isz = bo_get16le(b);
    if (isz < 3)
        return 0;
    uint16_t fsz = bo_get16le(b + 2);
    uint16_t nfree = bo_get16le(b + 4);
    uint16_t ninode = bo_get16le(b + 206);
    if (fsz <= isz || nfree > V6_NICFREE || ninode > V6_NICINOD ||
        (uint64_t)bno - 1 + fsz > nblocks)
        return 0;
    for (int i = 0; i < nfree; i++) {
        uint16_t fb = bo_get16le(b + 6 + 2 * i);
        if (fb != 0 && (fb < isz || fb >= fsz))
            return 0;
    }
    *isize = isz; *fsize = fsz;
    return 1;
}

/* Xenix superblock (1024-byte blocks, magic 0x2b5544 at offset 1016). */
static int super_xenix(const uint8_t *b, uint32_t bno, uint32_t nblocks,
                       uint16_t *isize, uint32_t *fsize) {
    if (bo_get32le(b + 0x3F8) != XENIX_MAGIC)
        return 0;
    uint16_t isz = bo_get16le(b + 0);
    uint32_t fsz = bo_get32le(b + 2);
    uint16_t nfree = bo_get16le(b + 6);
    uint16_t ninode = bo_get16le(b + 0x198);
    if (isz < 3 || fsz <= isz || nfree > V7_XEN_NICFREE || ninode > V7_NICINOD)
        return 0;
    if ((uint64_t)bno - 1 + fsz > nblocks)
        return 0;
    for (int i = 0; i < nfree; i++) {
        uint32_t fb = bo_get32le(b + 8 + 4 * i);
        if (fb != 0 && (fb < isz || fb >= fsz))
            return 0;
    }
    *isize = isz; *fsize = fsz;
    return 1;
}

/* 2.9BSD superblock (1024-byte blocks, V7-shaped, no magic): middle-endian,
 * 2-byte packing, NICFREE=50. */
static int super_bsd29(const uint8_t *b, uint32_t bno, uint32_t nblocks,
                       uint16_t *isize, uint32_t *fsize) {
    uint16_t isz = bo_get16le(b);
    if (isz < 3)
        return 0;
    uint16_t nfree = bo_get16le(b + 6);
    uint16_t ninode = bo_get16le(b + 208);
    if (nfree > V7_NICFREE || ninode > V7_NICINOD)
        return 0;
    uint32_t fsz = bo_get32me(b + 2);
    if (fsz <= isz || (uint64_t)bno - 1 + fsz > nblocks)
        return 0;
    for (int i = 0; i < nfree; i++) {
        uint32_t fb = bo_get32me(b + 8 + 4 * i);
        if (fb != 0 && (fb < isz || fb >= fsz))
            return 0;
    }
    *isize = isz; *fsize = fsz;
    return 1;
}

/* Is one 64-byte on-disk inode plausible (free, or a valid type with small
 * counts)? */
static int inode_ok(const uint8_t *d) {
    uint16_t mode = bo_get16le(d);
    if (mode == 0)
        return 1;   /* free inode */
    uint16_t t = mode & 0170000;
    if (t != 0100000 && t != 0040000 && t != 0020000 && t != 0060000)
        return 0;
    return bo_get16le(d + 2) < 128 && bo_get16le(d + 4) < 256 && bo_get16le(d + 6) < 256;
}

/* Is block b an inode-table block (8 plausible inodes, at least one used)? */
static int inode_block(const uint8_t *b) {
    int ok = 0, used = 0;
    for (int i = 0; i < 8; i++) {
        const uint8_t *d = b + i * 64;
        if (inode_ok(d)) ok++;
        if (bo_get16le(d) != 0) used++;
    }
    return ok >= 6 && used > 0;
}

int main(int argc, char **argv) {
    int stride = 1, backtrace = 0;
    const char *image = NULL;
    int c;
    while ((c = getopt(argc, argv, "s:i")) != -1) {
        switch (c) {
        case 's': stride = atoi(optarg); break;
        case 'i': backtrace = 1; break;
        default:
            fprintf(stderr, "usage: findfs.filsys [-s N] [-i] <image>\n");
            return 2;
        }
    }
    if (optind >= argc || stride < 1) {
        fprintf(stderr, "usage: findfs.filsys [-s N] [-i] <image>\n");
        return 2;
    }
    image = argv[optind];

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
            const char *ed_bt = NULL; uint16_t isz_bt = 0; uint32_t fsz_bt = 0;
            if (bno >= 2 && pread(fd, buf, BSIZE, (off_t)(bno - 1) * BSIZE) == BSIZE &&
                (super_v7(buf, bno - 1, nblocks, &ed_bt, &isz_bt, &fsz_bt) ||
                 super_v6(buf, bno - 1, nblocks, &isz_bt, &fsz_bt))) {
                if (!ed_bt) ed_bt = "V6";
                printf("fs @ block %u  (byte %llu)  %s  isize=%u fsize=%u   [via inode backtrace]\n",
                       bno - 2, (unsigned long long)(bno - 2) * BSIZE, ed_bt, isz_bt, fsz_bt);
                found++;
            }
        }
    }
    /* 1024-byte-block scan: Xenix (magic at offset 1016) and 2.9BSD
     * (V7-shaped, no magic -- so only tried when Xenix's magic is absent). */
    uint8_t xb[1024];
    uint32_t xnblocks = (uint32_t)(st.st_size / 1024);
    for (uint32_t xbno = 1; xbno < xnblocks; xbno += (uint32_t)stride) {
        if (pread(fd, xb, 1024, (off_t)xbno * 1024) != 1024)
            continue;
        uint16_t isz = 0; uint32_t fsz = 0;
        if (super_xenix(xb, xbno, xnblocks, &isz, &fsz)) {
            printf("fs @ block %u  (byte %llu)  Xenix  isize=%u fsize=%u\n",
                   (xbno - 1) * 2, (unsigned long long)(xbno - 1) * 1024, isz, fsz);
            found++;
        } else if (super_bsd29(xb, xbno, xnblocks, &isz, &fsz)) {
            printf("fs @ block %u  (byte %llu)  2.9BSD  isize=%u fsize=%u\n",
                   (xbno - 1) * 2, (unsigned long long)(xbno - 1) * 1024, isz, fsz);
            found++;
        }
    }

    close(fd);
    return found ? 0 : 1;
}
