/* filsys 1.8.0 - 2026-09-06 - Copyright (C) 2026 David Walther */
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
#include "filsys.h"

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
/* System V s_magic (offset 504) lives in its own enum: the 32-bit value does
 * not fit int, so it would widen the small constants above (C shares one
 * underlying enum type). */
enum { SYSV_MAGIC = 0xfd187e20 };

/* ---- free-list chain walk + i-list cross-check ---------------------------- */

/* Walk a V7-family free-list chain.  `head` is s_free[0]: 0 is an empty chain,
 * anything else is the address of a free-list ("dump") block.  Each dump block
 * is a 16-bit `nfree` at offset 0 followed by `nfree` daddr_t entries; entry 0
 * chains to the next dump block (0 terminates), the rest are free block
 * numbers.  A real superblock's chain is self-consistent and rooted at its own
 * offset; a stray free-list dump block interpreted as a superblock reads entries
 * relative to the *real* geometry, so its chain almost immediately leaves the
 * candidate's [isize, fsize) range, cycles, or runs off the media.
 *
 * Returns 0 on a clean terminating chain, else -1 with *why set.  *segs counts
 * the dump segments followed. */
static int walk_chain(int fd, uint32_t head, uint32_t isize, uint32_t fsize,
                      uint64_t base, int bsize, int nicfree, int wid,
                      uint16_t (*g16)(const uint8_t *),
                      uint32_t (*g32)(const uint8_t *), int fb_free,
                      uint32_t *segs, const char **why)
{
    uint32_t hops = 0, next = 0;
    uint8_t *buf = malloc((size_t)bsize);
    uint8_t *seen = calloc((size_t)((fsize + 7) / 8), 1);
    *segs = 0;
    *why = NULL;
    if (!buf || !seen) {
        *why = "out of memory";
        free(buf); free(seen);
        return -1;
    }

    while (head != 0) {
        if (head < isize || head >= fsize)        { *why = "chain leaves fs"; break; }
        if (hops++ > (uint32_t)(fsize / nicfree)) { *why = "chain too long"; break; }
        if (seen[head >> 3] & (1u << (head & 7))) { *why = "chain cycles"; break; }
        seen[head >> 3] |= (uint8_t)(1u << (head & 7));
        if (pread(fd, buf, bsize, (off_t)(base + (uint64_t)head * bsize)) != bsize) {
            *why = "chain read error"; break;
        }
        uint32_t nfree = g16(buf + 0);
        if (nfree < 1 || nfree > (uint32_t)nicfree) { *why = "bad segment nfree"; break; }
        int bad = 0;
        next = 0;
        for (uint32_t i = 0; i < nfree; i++) {
            uint32_t blk = wid == 2 ? g16(buf + fb_free + 2 * i)
                                    : g32(buf + fb_free + 4 * i);
            if (i == 0)
                next = blk;
            else if (blk != 0 && (blk < isize || blk >= fsize)) { bad = 1; break; }
        }
        if (bad) { *why = "chain entry out of range"; break; }
        head = next;
        (*segs)++;
    }

    int rc = *why ? -1 : 0;
    free(buf);
    free(seen);
    return rc;
}

/* Cross-check the i-list: inode 2 (the root) must look like a directory with a
 * non-zero, dirent-aligned size.  `g16`/`g32` decode the inode's mode/size
 * fields (V7-family inode layout: mode at 0, size at 8, 64-byte inodes). */
static int root_ok(int fd, uint64_t base, int bsize,
                   uint16_t (*g16)(const uint8_t *),
                   uint32_t (*g32)(const uint8_t *), const char **why)
{
    uint8_t ib[128];
    if (pread(fd, ib, sizeof ib, (off_t)(base + (uint64_t)2 * bsize)) != (ssize_t)sizeof ib) {
        *why = "cannot read i-list"; return -1;
    }
    const uint8_t *d = ib + 64;   /* inode 2 (inode 1 at offset 0, 64-byte inodes) */
    uint16_t mode = g16(d + 0);
    if ((mode & 0170000) != 0040000) { *why = "root inode is not a directory"; return -1; }
    uint32_t size = g32(d + 8);
    if (size == 0)                    { *why = "root inode size 0"; return -1; }
    if (size % 16 != 0)               { *why = "root inode size not dirent-aligned"; return -1; }
    return 0;
}

/* Map one of findfs's reported edition names back to a FILSYS_* selector so
 * `-v` can restrict the scan to a single edition. */
static int edition_selector(const char *name) {
    if (!strcmp(name, "V7"))       return FILSYS_V7;
    if (!strcmp(name, "32V"))      return FILSYS_32V;
    if (!strcmp(name, "Coherent")) return FILSYS_COHERENT;
    if (!strcmp(name, "V6"))       return FILSYS_V6;
    if (!strcmp(name, "Xenix"))    return FILSYS_XENIX;
    if (!strcmp(name, "2.9BSD"))   return FILSYS_BSD29;
    if (!strcmp(name, "sysvr2"))   return FILSYS_SVR2;
    if (!strcmp(name, "sysvr4"))   return FILSYS_SVR4;
    return -1;
}
/* True if the `-v` filter (or "no filter") admits this reported edition. */
static int matches(int filter, const char *name) {
    return filter < 0 || edition_selector(name) == filter;
}

/* Does block b (a candidate superblock) describe a V7 filesystem whose
 * superblock sits at absolute block `bno` (so the fs starts at bno-1)?
 * The free-list entries are checked to be in-range, which rejects the vast
 * majority of random data that merely *looks* superblock-shaped.  A candidate
 * that passes the field check is then cross-checked structurally: its free-list
 * chain must terminate cleanly and its root inode must look like a directory.
 * Returns 1 (valid), 2 (near-miss, *why set) or 0 (not a candidate). */
static int super_v7(int fd, const uint8_t *b, uint32_t bno, uint32_t nblocks,
                    const char **edition, uint16_t *isize, uint32_t *fsize,
                    const char **why, uint32_t *segs) {
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
        if (!ok)
            continue;
        /* Field-range passes: a candidate.  Walk its free-list chain and check
         * its root inode -- the two structural tests that a free-list dump block
         * (which has no i-list and whose entries were written relative to the
         * real fs's geometry) cannot also pass. */
        uint32_t (*g32)(const uint8_t *) = le ? bo_get32le : bo_get32me;
        uint32_t head = g32(b + free_off);   /* s_free[0] is the chain head */
        int fb_free = (le && !coh) ? 4 : 2;  /* dump-block free[] offset */
        uint32_t s = 0;
        uint64_t base = (uint64_t)(bno - 1) * BSIZE;
        if (walk_chain(fd, head, isz, fsz, base, BSIZE, L[k].nicfree, 4,
                       bo_get16le, g32, fb_free, &s, why) < 0) {
            *edition = L[k].name; *isize = isz; *fsize = fsz; *segs = s;
            return 2;
        }
        if (root_ok(fd, base, BSIZE, bo_get16le, g32, why) < 0) {
            *edition = L[k].name; *isize = isz; *fsize = fsz; *segs = s;
            return 2;
        }
        *edition = L[k].name;
        *isize = isz; *fsize = fsz; *segs = s;
        return 1;
    }
    return 0;
}

/* V6 superblock (all 16-bit fields): 16-bit free-list entries (wid=2).  V6 has
 * no separate root-inode cross-check here -- its root is inode 1 in a 32-byte
 * inode, a different layout -- so the chain walk is the structural test. */
static int super_v6(int fd, const uint8_t *b, uint32_t bno, uint32_t nblocks,
                    uint16_t *isize, uint32_t *fsize,
                    const char **why, uint32_t *segs) {
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
    uint64_t base = (uint64_t)(bno - 1) * BSIZE;
    uint32_t s = 0;
    if (walk_chain(fd, bo_get16le(b + 6), isz, fsz, base, BSIZE, V6_NICFREE, 2,
                   bo_get16le, bo_get32le, 2, &s, why) < 0) {
        *isize = isz; *fsize = fsz; *segs = s;
        return 2;
    }
    *isize = isz; *fsize = fsz; *segs = s;
    return 1;
}

/* System V s5fs superblock: s_magic 0xfd187e20 at offset 504 (LE or BE), s_type
 * at 508 names the block size.  The superblock sits at a fixed byte offset
 * (512), which the 512-byte scan reaches at bno == 1.  AFS signals itself with
 * s_nfree == 0xffff -- an impossible free-list count ("bitmap, not free list"). */
static int super_sysv(int fd, const uint8_t *b, uint32_t bno, uint32_t nblocks,
                      const char **edition, const char **note,
                      uint16_t *isize, uint32_t *fsize, int *bs,
                      const char **why, uint32_t *segs) {
    int le;
    *bs = 0;
    if (bo_get32le(b + 504) == SYSV_MAGIC)      le = 1;
    else if (bo_get32be(b + 504) == SYSV_MAGIC) le = 0;
    else return 0;

    uint16_t (*g16)(const uint8_t *) = le ? bo_get16le : bo_get16be;
    uint32_t (*g32)(const uint8_t *) = le ? bo_get32le : bo_get32be;

    /* AFS (Acer Fast File System) signals itself with s_nfree == 0xffff -- an
     * impossible free-list count ("bitmap, not free list"). */
    if (g16(b + 8) == 0xffff) {
        *edition = "AFS";
        *note = "bitmap free list, unsupported";
        return 1;
    }
    uint32_t t = g32(b + 508);
    int bsz = t == 1 ? 512 : t == 2 ? 1024 : t == 3 ? 2048 : 0;
    if (bsz == 0)
        return 0;
    *bs = bsz;
    uint16_t isz = g16(b + 0);

    /* One s5fs layout: daddr_t/time_t are 4-byte aligned (nfree@8, free@12,
     * ninode@212, inode@214); R2, R3 and R4 all share it (R4 only adds s_state
     * at 500, which this scanner does not need to tell them apart).  Validate
     * the free-list cache and take it if self-consistent. */
    uint16_t nfree = g16(b + 8), ninode = g16(b + 212);
    if (nfree > V7_NICFREE || ninode > V7_NICINOD)
        return 0;
    uint32_t fsz = g32(b + 4);
    if (fsz <= isz || (uint64_t)bno - 1 + fsz > nblocks)
        return 0;
    for (int i = 0; i < nfree; i++) {
        uint32_t fb = g32(b + 12 + 4 * i);
        if (fb != 0 && (fb < isz || fb >= fsz))
            return 0;
    }

    /* candidate: walk the chain and check the root inode */
    uint64_t base = (uint64_t)(bno - 1) * BSIZE;
    uint32_t s = 0;
    if (walk_chain(fd, g32(b + 12), isz, fsz, base, bsz, V7_NICFREE, 4,
                   g16, g32, 4, &s, why) < 0) {
        *edition = "sysvr2"; *note = le ? "LE" : "BE";
        *isize = isz; *fsize = fsz; *segs = s;
        return 2;
    }
    if (root_ok(fd, base, bsz, g16, g32, why) < 0) {
        *edition = "sysvr2"; *note = le ? "LE" : "BE";
        *isize = isz; *fsize = fsz; *segs = s;
        return 2;
    }
    *edition = "sysvr2";
    *note = le ? "LE" : "BE";
    *isize = isz; *fsize = fsz; *segs = s;
    return 1;
}

/* Xenix superblock (1024-byte blocks, magic 0x2b5544 at offset 1016). */
static int super_xenix(int fd, const uint8_t *b, uint32_t bno, uint32_t nblocks,
                       uint16_t *isize, uint32_t *fsize,
                       const char **why, uint32_t *segs) {
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
    uint64_t base = (uint64_t)(bno - 1) * BSIZE;
    uint32_t s = 0;
    if (walk_chain(fd, bo_get32le(b + 8), isz, fsz, base, 1024, V7_XEN_NICFREE, 4,
                   bo_get16le, bo_get32le, 2, &s, why) < 0) {
        *isize = isz; *fsize = fsz; *segs = s;
        return 2;
    }
    if (root_ok(fd, base, 1024, bo_get16le, bo_get32le, why) < 0) {
        *isize = isz; *fsize = fsz; *segs = s;
        return 2;
    }
    *isize = isz; *fsize = fsz; *segs = s;
    return 1;
}

/* 2.9BSD superblock (1024-byte blocks, V7-shaped, no magic): middle-endian,
 * 2-byte packing, NICFREE=50. */
static int super_bsd29(int fd, const uint8_t *b, uint32_t bno, uint32_t nblocks,
                       uint16_t *isize, uint32_t *fsize,
                       const char **why, uint32_t *segs) {
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
    uint64_t base = (uint64_t)(bno - 1) * BSIZE;
    uint32_t s = 0;
    if (walk_chain(fd, bo_get32me(b + 8), isz, fsz, base, 1024, V7_NICFREE, 4,
                   bo_get16le, bo_get32me, 2, &s, why) < 0) {
        *isize = isz; *fsize = fsz; *segs = s;
        return 2;
    }
    if (root_ok(fd, base, 1024, bo_get16le, bo_get32me, why) < 0) {
        *isize = isz; *fsize = fsz; *segs = s;
        return 2;
    }
    *isize = isz; *fsize = fsz; *segs = s;
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

/* Print one validated or near-miss candidate.  `root` is 1 when the root-inode
 * cross-check ran and passed, 0 when it was skipped (V6 has a different layout). */
static void report(uint32_t blk, uint64_t byte, const char *ed, const char *note,
                   uint16_t isz, uint32_t fsz, uint32_t segs, int root) {
    printf("fs @ block %u  (byte %llu)  %s%s%s%s  isize=%u fsize=%u  chain ok (%u segs)",
           blk, (unsigned long long)byte, ed,
           note ? " (" : "", note ? note : "", note ? ")" : "", isz, fsz, segs);
    if (root)
        printf(", root inode ok");
    printf("\n");
}
static void report_miss(uint32_t blk, uint64_t byte, const char *ed, const char *note,
                        uint16_t isz, uint32_t fsz, const char *why) {
    printf("fs @ block %u  (byte %llu)  %s%s%s%s  isize=%u fsize=%u  REJECTED: %s\n",
           blk, (unsigned long long)byte, ed,
           note ? " (" : "", note ? note : "", note ? ")" : "", isz, fsz, why);
}

int main(int argc, char **argv) {
    int stride = 1, backtrace = 0, edition_filter = -1;
    const char *image = NULL;
    int c;
    while ((c = getopt(argc, argv, "v:s:i")) != -1) {
        switch (c) {
        case 'v':
            edition_filter = filsys_parse_edition("findfs.filsys", optarg);
            if (edition_filter < 0)
                return 2;
            break;
        case 's': stride = atoi(optarg); break;
        case 'i': backtrace = 1; break;
        default:
            fprintf(stderr, "usage: findfs.filsys [-v <edition>] [-s N] [-i] <image>\n"
                            "  editions: %s\n", filsys_editions_usage());
            return 2;
        }
    }
    if (optind >= argc || stride < 1) {
        fprintf(stderr, "usage: findfs.filsys [-v <edition>] [-s N] [-i] <image>\n"
                        "  editions: %s\n", filsys_editions_usage());
        return 2;
    }
    image = argv[optind];

    int fd = open(image, O_RDONLY);
    if (fd < 0) { perror(image); return 1; }
    uint64_t sz = 0;
    if (filsys_dev_size(fd, &sz) || sz < BSIZE) { fprintf(stderr, "cannot size image\n"); return 1; }
    uint32_t nblocks = (uint32_t)(sz / BSIZE);
    uint8_t buf[BSIZE];

    int found = 0;
    /* Blocks below `skip_end` (bytes) belong to an already-validated fs's data
     * area, so their free-list dump blocks are not independent candidates. */
    uint64_t skip_end = 0;
    for (uint32_t bno = 1; bno < nblocks; bno += (uint32_t)stride) {
        if ((uint64_t)bno * BSIZE < skip_end)
            continue;
        if (pread(fd, buf, BSIZE, (off_t)bno * BSIZE) != BSIZE)
            continue;

        /* direct superblock detection.  System V is checked first: its superblock
         * is V7-shaped, but the s_magic at offset 504 is the stronger signal, so
         * a sysvr2 volume reports as sysvr2 rather than a false "V7". */
        uint16_t isz = 0; uint32_t fsz = 0;
        const char *ed = NULL, *note = NULL, *why = NULL;
        uint32_t segs = 0;
        uint64_t byte = (uint64_t)(bno - 1) * BSIZE;
        int bs = BSIZE;   /* V7/V6 block size; super_sysv overrides */

        int rc = super_sysv(fd, buf, bno, nblocks, &ed, &note, &isz, &fsz, &bs, &why, &segs);
        if (rc == 0) {
            bs = BSIZE;
            rc = super_v7(fd, buf, bno, nblocks, &ed, &isz, &fsz, &why, &segs);
            if (rc == 0) {
                ed = "V6";
                rc = super_v6(fd, buf, bno, nblocks, &isz, &fsz, &why, &segs);
            }
        }
        if (rc >= 1 && matches(edition_filter, ed)) {
            if (!strcmp(ed, "AFS"))
                printf("fs @ block %u  (byte %llu)  AFS (bitmap free list, unsupported)\n",
                       bno - 1, (unsigned long long)byte);
            else if (rc == 1) {
                report(bno - 1, byte, ed, note, isz, fsz, segs, strcmp(ed, "V6") != 0);
                uint64_t de = (uint64_t)(bno - 1) * BSIZE + (uint64_t)fsz * (uint64_t)bs;
                if (de > skip_end)
                    skip_end = de;
            } else
                report_miss(bno - 1, byte, ed, note, isz, fsz, why);
            found++;
        }

        /* inode-table backtrace: is bno the first inode block of a filesystem
         * whose superblock is at bno-1?  A damaged superblock can fail its own
         * chain walk here -- that is the damage being recovered -- so a near-miss
         * still reports. */
        if (backtrace && inode_block(buf) && bno >= 2 &&
            pread(fd, buf, BSIZE, (off_t)(bno - 1) * BSIZE) == BSIZE) {
            const char *ed_bt = NULL, *why_bt = NULL;
            uint16_t isz_bt = 0; uint32_t fsz_bt = 0, segs_bt = 0;
            int r = super_v7(fd, buf, bno - 1, nblocks, &ed_bt, &isz_bt, &fsz_bt, &why_bt, &segs_bt);
            if (r == 0) {
                ed_bt = "V6";
                r = super_v6(fd, buf, bno - 1, nblocks, &isz_bt, &fsz_bt, &why_bt, &segs_bt);
            }
            if (r >= 1 && matches(edition_filter, ed_bt)) {
                printf("fs @ block %u  (byte %llu)  %s  isize=%u fsize=%u   [via inode backtrace]\n",
                       bno - 2, (unsigned long long)(bno - 2) * BSIZE, ed_bt, isz_bt, fsz_bt);
                found++;
            }
        }
    }
    /* 1024-byte-block scan: Xenix (magic at offset 1016) and 2.9BSD
     * (V7-shaped, no magic -- so only tried when Xenix's magic is absent). */
    uint8_t xb[1024];
    uint32_t xnblocks = (uint32_t)(sz / 1024);
    for (uint32_t xbno = 1; xbno < xnblocks; xbno += (uint32_t)stride) {
        if ((uint64_t)xbno * 1024 < skip_end)
            continue;
        if (pread(fd, xb, 1024, (off_t)xbno * 1024) != 1024)
            continue;
        uint16_t isz = 0; uint32_t fsz = 0;
        const char *why = NULL, *ed = "Xenix";
        uint32_t segs = 0;
        uint64_t byte = (uint64_t)(xbno - 1) * 1024;
        uint32_t blk = (xbno - 1) * 2;

        int rc = super_xenix(fd, xb, xbno, xnblocks, &isz, &fsz, &why, &segs);
        if (rc == 0) {
            ed = "2.9BSD";
            rc = super_bsd29(fd, xb, xbno, xnblocks, &isz, &fsz, &why, &segs);
        }
        if (rc >= 1 && matches(edition_filter, ed)) {
            if (rc == 1) {
                report(blk, byte, ed, NULL, isz, fsz, segs, 1);
                uint64_t de = (uint64_t)(xbno - 1) * 1024 + (uint64_t)fsz * 1024;
                if (de > skip_end)
                    skip_end = de;
            } else
                report_miss(blk, byte, ed, NULL, isz, fsz, why);
            found++;
        }
    }

    close(fd);
    return found ? 0 : 1;
}
