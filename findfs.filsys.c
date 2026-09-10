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
 * V1 (a dual-bitmap superblock in fs blocks 0+1, no boot block) is detected in
 * the same 512-byte scan.  PDP-7 (V0) is word-addressed and sits at a fixed
 * offset -- surface 1 of an RB09 fixed-head disk -- so it is probed separately,
 * once per word container (rb09/packed18/rim).
 *
 * Usage:
 *     findfs.filsys [-s N] [-i] <image>
 *
 * -s N   scan every N-th block (use the disk's blocks-per-cylinder, e.g. 418
 *        for an RP06, to speed up a big image)
 * -i     also trace inode-table runs backwards to their superblocks
 *
 * Each hit is printed as a diagnostic line followed by a ready-to-paste
 * mount command:
 *     fs @ block START  (byte BYTES)  V7  isize=N fsize=M  chain ok (...)
 *       mount.filsys -v v7 -o offset=BYTES <image> /mnt
 * Mount the partition in place by cutting and pasting that second line.
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
#include "v1fs.h"
#include "v7fs.h"
#include "pdp7fs.h"

/* The scanner's own block size: every V1/V6/V7-family superblock sits in a
 * 512-byte block.  Per-format constants come from the backend headers
 * (v1fs.h / v7fs.h / pdp7fs.h), not a local copy. */
enum { BSIZE = 512 };

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
        /* df_nfree is 2 bytes on the PDP-11 (fb_free == 2) and 4 bytes on the
         * VAX/m68k (fb_free == 4); fb_free is the offset of df_free[], so it
         * also names the df_nfree width.  The 4-byte form must be read whole so
         * a big-endian V9 does not read its high (zero) half. */
        uint32_t nfree = fb_free == 4 ? g32(buf + 0) : g16(buf + 0);
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

/* Map a reported class back to its canonical `-v` token -- the earliest member
 * of the equivalence class, except E2 where "v6" is the name in universal use
 * (Lions' *Commentary*).  The class is prose for the reader; the token is what
 * `-v` and the mount line act on. */
static const char *canonical_token(const char *class) {
    if (!strncmp(class, "v1,", 3))         return "v1";
    if (!strncmp(class, "v4,", 3))         return "v6";
    if (!strncmp(class, "v7,", 3))         return "v7";
    if (!strncmp(class, "v8 or v10", 9))   return "v8";
    if (!strncmp(class, "v10,", 4))        return "v10";
    if (!strncmp(class, "sysvr2", 6))      return "sysvr2";
    if (!strncmp(class, "32V", 3))         return "vax32";
    if (!strcmp(class, "Coherent"))        return "coherent";
    if (!strcmp(class, "Xenix"))           return "xenix";
    if (!strcmp(class, "2.9BSD"))          return "bsd29";
    if (!strcmp(class, "2.11BSD"))         return "bsd211";
    if (!strcmp(class, "sysvr4"))          return "sysvr4";
    if (!strcmp(class, "v9"))              return "v9";
    return class;   /* singleton leaf already in -v form */
}

/* Map a reported class to a FILSYS_* selector so `-v` can restrict the scan. */
static int edition_selector(const char *class) {
    return filsys_edition_by_name(canonical_token(class));
}
/* True if the `-v` filter (or "no filter") admits this reported class. */
static int matches(int filter, const char *class) {
    return filter < 0 || edition_selector(class) == filter;
}

/* The canonical FILSYS_* selectors each probe family can report.  A `-v` filter
 * naming none of a probe's editions skips the probe entirely, so the filter
 * reduces work (fewer reads/traversals) instead of only filtering the report.
 * Lists are NULL-terminated with -1.  (PDP-7 is word-addressed, probed apart.) */
static const int fam_v1[]    = { FILSYS_V1, -1 };
static const int fam_v6[]    = { FILSYS_V6, -1 };
static const int fam_v7[]    = { FILSYS_V7, FILSYS_32V, FILSYS_COHERENT, FILSYS_SYSIII, -1 };
static const int fam_sysv[]  = { FILSYS_SVR2, FILSYS_SVR4, -1 };
static const int fam_v8[]    = { FILSYS_V8, FILSYS_V10, -1 };
static const int fam_v9[]    = { FILSYS_V9, -1 };
static const int fam_xenix[] = { FILSYS_XENIX, -1 };
static const int fam_bsd29[] = { FILSYS_BSD29, FILSYS_BSD211, -1 };

static int probe_wanted(int filter, const int *fam) {
    if (filter < 0)
        return 1;
    for (; *fam >= 0; fam++)
        if (*fam == filter)
            return 1;
    return 0;
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
        { 0, 0, "v7 or sysvr1", V7_NICFREE },
        { 1, 0, "32V or sysiii", V7_NICFREE },
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

/* V1 superblock (blocks 0+1, dual bitmaps): no boot block and no free-list
 * chain, so the filesystem starts *at* the candidate block, not before it.  The
 * free map (bit=1 free) begins at byte 2; the inode map (bit=0 free, indexed
 * from inode 41) follows it.  There is no s_isize: the i-list size is derived
 * from the inode-map byte count.  Root is inode 41, a 32-byte inode in block 4.
 * Returns 1 (valid) or 0 (not a candidate). */
static int super_v1(int fd, const uint8_t *sb, uint32_t bno, uint32_t nblocks,
                    uint16_t *isize, uint32_t *fsize, const char **why) {
    uint16_t freemap_bytes = bo_get16le(sb + 0);
    if (freemap_bytes == 0)
        return 0;
    uint32_t inodemap_bytes_off = 2 + freemap_bytes;
    if (inodemap_bytes_off + 2 > BSIZE * 2)
        return 0;
    uint16_t inodemap_bytes = bo_get16le(sb + inodemap_bytes_off);
    uint32_t inodemap_off = inodemap_bytes_off + 2;
    if (inodemap_bytes == 0 || inodemap_off + inodemap_bytes > BSIZE * 2)
        return 0;
    uint32_t fsz = (uint32_t)freemap_bytes * 8;
    uint32_t maxino = (uint32_t)inodemap_bytes * 8;
    uint32_t dstart = (maxino + 31) / 16 + 1;   /* first data block */
    if (fsz <= dstart || (uint64_t)bno + fsz > nblocks)
        return 0;
    /* root inode 41: block 4, slot 8 (32-byte inodes): an allocated directory
     * whose first data block is in the data area. */
    uint8_t blk[BSIZE];
    if (pread(fd, blk, sizeof blk, (off_t)(bno + 4) * BSIZE) != (ssize_t)sizeof blk)
        return 0;
    const uint8_t *d = blk + 8 * 32;
    uint16_t mode = bo_get16le(d + 0);
    if ((mode & (0100000 | 0040000)) != (0100000 | 0040000)) {
        *why = "root inode is not a directory";
        return 0;
    }
    uint16_t a0 = bo_get16le(d + 6);
    if (a0 < dstart || a0 >= fsz) {
        *why = "root directory block out of range";
        return 0;
    }
    *isize = (uint16_t)dstart;
    *fsize = fsz;
    return 1;
}

/* System V Release 4's s_state word (FsOKAY/FsACTIVE at offset 500) tells R4
 * from R2/R3, whose s_fill[12] is left zero.  The one thing the shared s5fs
 * layout alone cannot say. */
static const char *sysv_class(const uint8_t *b, uint32_t (*g32)(const uint8_t *)) {
    uint32_t st = g32(b + V7_SYSV_STATE_OFF);
    return (st == V7_SYSV_STATE_CLEAN || st == V7_SYSV_STATE_ACTIVE)
               ? "sysvr4" : "sysvr2 or sysvr3";
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
    if (bo_get32le(b + 504) == V7_SYSV_MAGIC)      le = 1;
    else if (bo_get32be(b + 504) == V7_SYSV_MAGIC) le = 0;
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
        *edition = sysv_class(b, g32); *note = le ? "LE" : "BE";
        *isize = isz; *fsize = fsz; *segs = s;
        return 2;
    }
    if (root_ok(fd, base, bsz, g16, g32, why) < 0) {
        *edition = sysv_class(b, g32); *note = le ? "LE" : "BE";
        *isize = isz; *fsize = fsz; *segs = s;
        return 2;
    }
    *edition = sysv_class(b, g32);
    *note = le ? "LE" : "BE";
    *isize = isz; *fsize = fsz; *segs = s;
    return 1;
}

/* Xenix superblock (1024-byte blocks, magic 0x2b5544 at offset 1016). */
static int super_xenix(int fd, const uint8_t *b, uint32_t bno, uint32_t nblocks,
                       uint16_t *isize, uint32_t *fsize,
                       const char **why, uint32_t *segs) {
    if (bo_get32le(b + 0x3F8) != V7_XEN_MAGIC)
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

/* ---- V8-family (Eighth/Ninth/Tenth Edition) --------------------------------
 * The rearranged superblock (§5.8) is a positive discriminator against V7: the
 * field *order* differs, so a V7 superblock read as V8 fails at tier 1 and vice
 * versa.  Byte order and free-space form are not read from a single field --
 * the free space is a hypothesis you test by traversing it, and the endianness
 * is tested by walking the i-list.  Each traversal rules out the wrong guess. */

/* Tier 4.3: walk the i-list.  Every allocated inode's 13 three-byte addresses
 * must lie in [isize, fsize) (0 = unused).  Under the wrong byte order the
 * 3-byte addresses come out enormous and fail immediately, so this is the
 * strongest endianness test available.  Returns 0 clean, -1 with *why set. */
static int v8_ilist_ok(int fd, uint64_t base, int bsize, uint32_t isize,
                       uint32_t fsize, int le, uint32_t maxino, const char **why)
{
    uint32_t inopb = (uint32_t)bsize / 64;
    uint32_t niblk = (maxino + inopb - 1) / inopb;
    uint8_t blk[V7_MAXBSIZE];
    uint16_t (*g16)(const uint8_t *) = le ? bo_get16le : bo_get16be;
    uint32_t (*g24)(const uint8_t *) = le ? bo_get24le : bo_get24be;
    for (uint32_t b = 0; b < niblk; b++) {
        if (pread(fd, blk, bsize, (off_t)(base + (uint64_t)(2 + b) * bsize)) != bsize) {
            *why = "cannot read i-list"; return -1;
        }
        for (uint32_t s = 0; s < inopb; s++) {
            uint32_t ino = b * inopb + s + 1;
            if (ino > maxino)
                break;
            const uint8_t *ib = blk + s * 64;
            if (g16(ib + 0) == 0)
                continue;   /* free inode */
            for (int i = 0; i < 13; i++) {
                uint32_t a = g24(ib + 12 + 3 * i);
                if (a != 0 && (a < isize || a >= fsize)) {
                    *why = "inode block address out of range";
                    return -1;
                }
            }
        }
    }
    return 0;
}

/* Popcount the V8-family free-space bitmap (bit i = free).  form is
 * V8_FREEMAP_BITMAP (in-superblock) or V8_FREEMAP_BIGMAP (tail blocks).
 * Returns the count, or -1 with *why set on error. */
static int v8_bitmap_count(int fd, const uint8_t *sb, uint64_t base, int bsize,
                           uint32_t isize, uint32_t fsize, int le, int form,
                           const char **why)
{
    uint32_t (*g32)(const uint8_t *) = le ? bo_get32le : bo_get32be;
    uint32_t nbits, count = 0;
    if (form == V8_FREEMAP_BIGMAP) {
        uint32_t bits_per_blk = (uint32_t)bsize * 8;
        uint32_t nblks = (fsize + bits_per_blk - 1) / bits_per_blk;
        nbits = fsize;
        uint8_t blk[V7_MAXBSIZE];
        for (uint32_t k = 0; k < nblks; k++) {
            uint32_t dblk = fsize - nblks + k;
            if (pread(fd, blk, bsize, (off_t)(base + (uint64_t)dblk * bsize)) != bsize) {
                *why = "bitmap block read error"; return -1;
            }
            uint32_t bytes = bsize;
            if (k == nblks - 1)
                bytes = (nbits - k * bits_per_blk + 7) / 8;
            for (uint32_t j = 0; j < bytes; j++) {
                uint8_t x = blk[j];
                while (x) { count += (x & 1); x >>= 1; }
            }
        }
    } else {
        nbits = fsize - isize;   /* in-superblock: bit i = block isize+i */
        for (uint32_t w = 0; w < (nbits + 31) / 32; w++) {
            uint32_t word = g32(sb + V8_SB_BFREE + 4 * w);
            for (uint32_t bit = 0; bit < 32 && w * 32 + bit < nbits; bit++)
                if (word & (1u << bit))
                    count++;
        }
    }
    return (int)count;
}

/* Test one (byte-order) hypothesis for a V8-family superblock at `base` block 0.
 * Returns 1 (fully validated, *form and *segs set), -1 (near-miss, *why set),
 * or 0 (not a candidate). */
static int v8_try(int fd, const uint8_t *b, uint64_t base, int bsize,
                  uint64_t nbytes, int le, uint16_t *isize, uint32_t *fsize,
                  int *form, uint32_t *segs, const char **why)
{
    uint16_t (*g16)(const uint8_t *) = le ? bo_get16le : bo_get16be;
    uint32_t (*g32)(const uint8_t *) = le ? bo_get32le : bo_get32be;
    uint32_t (*g24)(const uint8_t *) = le ? bo_get24le : bo_get24be;

    /* tier 1: V8-family field order (s_fsize @4, s_ninode @8, s_tfree @220) */
    uint16_t isz = g16(b + V8_SB_ISIZE);
    if (isz < 3)
        return 0;
    uint32_t fsz = g32(b + V8_SB_FSIZE);
    uint16_t ninode = g16(b + V8_SB_NINODE);
    if (ninode > V7_NICINOD)
        return 0;
    uint32_t inopb = (uint32_t)bsize / 64;
    uint32_t maxino = (uint32_t)(isz - 2) * inopb;
    if (maxino == 0 || maxino > 65536)
        return 0;
    for (int i = 0; i < ninode; i++) {
        uint16_t ino = g16(b + V8_SB_INODE + 2 * i);
        if (ino != 0 && ino > maxino)
            return 0;
    }
    if (b[V8_SB_FMOD] > 1)
        return 0;
    uint32_t tfree = g32(b + V8_SB_TFREE);
    uint16_t tinode = g16(b + V8_SB_TINODE);
    if (tfree > fsz - isz || tinode > maxino)
        return 0;
    for (int i = 0; i < 14; i++) {
        uint8_t c = b[V8_SB_FSMNT + i];
        if (c != 0 && (c < 0x20 || c > 0x7e))
            return 0;
    }

    /* tier 2: geometry */
    if (fsz <= isz || fsz > (1u << 24))
        return 0;
    if ((uint64_t)fsz * bsize > nbytes + (uint64_t)bsize)
        return 0;

    /* Past the "not a candidate" gates, every remaining return carries real
     * isize/fsize values -- including the tier-3 near-miss path, so super_v8's
     * "REJECTED:" report shows the true geometry rather than uninitialised 0s. */
    *isize = isz; *fsize = fsz;

    /* tier 3: chase the root (inode 2 = block 2, slot 1) */
    uint8_t ib[128];
    if (pread(fd, ib, sizeof ib, (off_t)(base + 2 * (uint64_t)bsize)) != (ssize_t)sizeof ib) {
        *why = "cannot read i-list"; return -1;
    }
    const uint8_t *d = ib + 64;
    if ((g16(d + 0) & 0170000) != 0040000) { *why = "root inode is not a directory"; return -1; }
    if (g16(d + 2) < 2)                     { *why = "root inode link count < 2"; return -1; }
    uint32_t size = g32(d + 8);
    if (size < 32 || size % 16 != 0)        { *why = "root inode size implausible"; return -1; }
    uint32_t rootblk = g24(d + 12);
    if (rootblk < isz || rootblk >= fsz)    { *why = "root directory block out of range"; return -1; }
    uint8_t rd[64];
    if (pread(fd, rd, sizeof rd, (off_t)(base + (uint64_t)rootblk * bsize)) != (ssize_t)sizeof rd) {
        *why = "cannot read root directory"; return -1;
    }
    if (g16(rd + 0) != 2 || rd[2] != '.' ||
        g16(rd + 16) != 2 || rd[18] != '.' || rd[19] != '.') {
        *why = "root directory entries wrong"; return -1;
    }

    /* tier 4.3: ilist walk (rules out the wrong endianness) */
    if (v8_ilist_ok(fd, base, bsize, isz, fsz, le, maxino, why) < 0)
        return -1;

    /* tier 4.1/4.2: decide free list vs bitmap by traversal.  The free list is
     * tried first: its s_free[] entries are small block numbers, so a bitmap
     * read as a free list fails the range check; a free list read as a bitmap
     * popcounts to something that does not match s_tfree.  One survives. */
    int nicfree = (bsize == 8192) ? V8_NICFREE_LARGE : V8_NICFREE_SMALL;
    uint16_t nfree = g16(b + V8_SB_NFREE);
    uint32_t s = 0;
    const char *why_list = NULL, *why_map = NULL;
    if (nfree <= nicfree) {
        int ok = 1;
        for (int i = 0; i < nfree; i++) {
            uint32_t fb = g32(b + V8_SB_FREE + 4 * i);
            if (fb != 0 && (fb < isz || fb >= fsz)) { ok = 0; break; }
        }
        if (ok && walk_chain(fd, g32(b + V8_SB_FREE), isz, fsz, base, bsize,
                             nicfree, 4, g16, g32, 4, &s, &why_list) == 0) {
            *form = V8_FREEMAP_LIST;
            *isize = isz; *fsize = fsz; *segs = s;
            return 1;
        }
    }
    int fm = (b[V8_SB_FLAG] == 1 && bsize == 4096) ? V8_FREEMAP_BIGMAP : V8_FREEMAP_BITMAP;
    int cnt = v8_bitmap_count(fd, b, base, bsize, isz, fsz, le, fm, &why_map);
    if (cnt >= 0 && (uint32_t)cnt == tfree) {
        *form = fm;
        *isize = isz; *fsize = fsz; *segs = 0;
        return 1;
    }
    *why = why_list ? why_list : (why_map ? why_map : "free space does not traverse");
    return -1;
}

/* V8-family superblock (block 1 of a bsize-byte-block filesystem).  Returns 1
 * (validated), 2 (near-miss), 0 (not a candidate). */
static int super_v8(int fd, const uint8_t *b, uint64_t sb_byte, int bsize,
                    uint64_t nbytes, const char **edition, const char **note,
                    uint16_t *isize, uint32_t *fsize, int *bitmap,
                    const char **why, uint32_t *segs)
{
    static char note_buf[64];
    uint64_t base = sb_byte - bsize;   /* block 0 (boot) of the filesystem */
    const char *miss_why = NULL;
    uint16_t miss_isz = 0; uint32_t miss_fsz = 0;
    for (int le = 1; le >= 0; le--) {   /* LE (VAX) first, then BE (m68k) */
        uint16_t isz; uint32_t fsz; int form; uint32_t s; const char *why_try = NULL;
        int r = v8_try(fd, b, base, bsize, nbytes, le, &isz, &fsz, &form, &s, &why_try);
        if (r == 1) {
            *isize = isz; *fsize = fsz; *segs = s;
            *bitmap = (form != V8_FREEMAP_LIST);
            if (form == V8_FREEMAP_BIGMAP)
                *edition = "v10, bs=4096, bigmap";
            else if (bsize == 8192)
                *edition = "v9";
            else
                *edition = (bsize == 4096) ? "v8 or v10, bs=4096, bitmap"
                                           : "v8 or v10, bs=1024";
            snprintf(note_buf, sizeof note_buf, "%d-byte blocks, %s, %s", bsize,
                     form == V8_FREEMAP_LIST ? "free list" :
                     form == V8_FREEMAP_BITMAP ? "in-superblock bitmap" :
                     "out-of-superblock bitmap", le ? "LE" : "BE");
            *note = note_buf;
            return 1;
        }
        if (r == -1 && !miss_why) {
            miss_why = why_try; miss_isz = isz; miss_fsz = fsz;
        }
    }
    if (miss_why) {
        *why = miss_why; *isize = miss_isz; *fsize = miss_fsz;
        *segs = 0; *bitmap = 0;
        *edition = "v8"; *note = NULL;
        return 2;
    }
    return 0;
}

/* 2.11BSD and 2.9BSD share a superblock (1K blocks, V7-shaped, NICFREE=50) and
 * differ only in directory format: 2.11BSD has variable-length entries
 * (d_ino/d_reclen/d_namlen), 2.9BSD fixed 16-byte entries.  Walk the root
 * directory's entry chain as 2.11BSD and check it is self-consistent: each
 * d_reclen is a multiple of 4, d_namlen is in range, and the "." / ".." entries
 * land.  A 2.9BSD root's byte 2 is '.' (0x2e), so its "reclen" reads 46 -- not
 * a multiple of 4 -- and the walk fails on the first entry.  Returns 1 for
 * 2.11BSD, 0 for 2.9BSD (or indeterminate). */
static int bsd211_root_dir(int fd, uint64_t base, int bsize) {
    uint8_t ib[128];
    if (pread(fd, ib, sizeof ib, (off_t)(base + 2 * (uint64_t)bsize)) != (ssize_t)sizeof ib)
        return 0;
    const uint8_t *d = ib + 64;              /* inode 2 (root) */
    /* di_addr[0] is 32-bit for 2.11BSD (24-bit for 2.9BSD); read it as 32-bit.
     * A 2.9BSD inode's 24-bit [hi,lo,mid] then leaks its next slot's byte into
     * the high 16 bits, giving a huge block number whose read fails -- so only
     * a 2.11BSD inode lands on its actual root directory here. */
    uint32_t rootblk = bo_get32me(d + 12);
    uint8_t rb[1024];
    if (pread(fd, rb, sizeof rb, (off_t)(base + (uint64_t)rootblk * bsize)) != (ssize_t)sizeof rb)
        return 0;

    uint32_t off = 0;
    int dot = 0, dotdot = 0;
    while (off + 6 <= sizeof rb) {
        uint16_t reclen = bo_get16me(rb + off + 2);
        uint16_t namlen = bo_get16me(rb + off + 4);
        if (reclen < 6 || reclen % 4 != 0 || off + reclen > sizeof rb ||
            namlen < 1 || namlen > 63)
            return 0;
        if (namlen == 1 && rb[off + 6] == '.')
            dot = 1;
        else if (namlen == 2 && rb[off + 6] == '.' && rb[off + 7] == '.')
            dotdot = 1;
        off += reclen;
        if (dot && dotdot)
            return 1;                        /* 2.11BSD chain passes */
    }
    return 0;
}

/* PDP-7 (V0) word container: the three codecs and their -o packing= names come
 * from the backend (pdp7fs.c word_rb09/word_packed18/word_rim). */
static const struct { const word_codec_t *codec; const char *name; } p7_containers[] = {
    { &word_rb09,     "rb09" },
    { &word_packed18, "packed18" },
    { &word_rim,      "rim" },
};

/* Read PDP-7 block `bno` (64 words) at byte offset `base` (the filesystem
 * surface) into `words`, decoded through the container codec. */
static int p7_read(int fd, const word_codec_t *wc, uint64_t base, uint32_t bno, uint32_t *words) {
    uint8_t raw[P7_MAXBLOCKBYTES];
    off_t pos = (off_t)base + (off_t)bno * wc->block_bytes;
    if (pread(fd, raw, wc->block_bytes, pos) != (ssize_t)wc->block_bytes)
        return -1;
    for (uint32_t i = 0; i < P7_WSIZE; i++)
        words[i] = wc->get(raw, i);
    return 0;
}

/* PDP-7 probe: no magic; the superblock is block 0 word 0 (the free-list head).
 * Validate the free-list chain (nodes in the data range, each chaining via word
 * 0 and carrying nine free block numbers in words 1..9) and the root "dd"
 * directory (inode 4 at block 2).  Returns 1 (valid) or 0 (not a candidate). */
static int super_pdp7(int fd, const word_codec_t *wc, uint64_t base, uint32_t *segs, const char **why) {
    uint32_t words[P7_WSIZE];
    if (p7_read(fd, wc, base, 0, words) != 0)
        return 0;
    uint32_t head = words[0];
    if (head == 0)
        return 0;                    /* empty fs: not a mounted volume */
    uint8_t seen[P7_NBLOCKS / 8] = {0};
    uint32_t hops = 0, s = 0;
    while (head != 0) {
        if (head < P7_DATASTART || head >= P7_KDATA) { *why = "chain leaves fs"; return 0; }
        if (++hops > P7_NBLOCKS) { *why = "chain too long"; return 0; }
        if (seen[head >> 3] & (1u << (head & 7))) { *why = "chain cycles"; return 0; }
        seen[head >> 3] |= (uint8_t)(1u << (head & 7));
        if (p7_read(fd, wc, base, head, words) != 0) { *why = "chain read error"; return 0; }
        for (int i = 1; i <= 9; i++) {
            uint32_t fb = words[i];
            if (fb != 0 && (fb < P7_DATASTART || fb >= P7_KDATA)) { *why = "chain entry out of range"; return 0; }
        }
        head = words[0];
        s++;
    }
    /* root "dd" directory: inode 4 at block 2, flags word must be an allocated
     * directory. */
    if (p7_read(fd, wc, base, P7_FIRSTINOBLK, words) != 0)
        return 0;
    uint32_t flags = words[P7_INODESZ * (P7_ROOTINO % P7_INOPB)];
    if ((flags & (P7_IUSED | P7_IDIR)) != (P7_IUSED | P7_IDIR)) { *why = "root inode is not a directory"; return 0; }
    *segs = s;
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

/* ---- candidate collection + reporting -------------------------------------- */

/* A candidate superblock found by the scan: either validated (chain ok, root ok)
 * or a rejected near-miss.  Collected during the scan and emitted afterward, so
 * validated hits print above rejected near-misses regardless of the offset order
 * the scan reached them -- a 1K-block filesystem's own bytes can also parse as a
 * 512-byte near-miss at byte 512, which must not print above the real hit. */
struct cand {
    uint32_t blk;                 /* fs-start block (512-byte units) */
    uint64_t byte;                /* fs-start byte offset */
    char     class_desc[48];      /* the whole equivalence class, for the reader */
    char     ed[16];              /* canonical -v token (derived, never printed as prose) */
    char     note[48];            /* extra note, or "" */
    uint16_t isz;
    uint32_t fsz;
    uint32_t segs;
    int      root;                /* root-inode cross-check passed */
    int      bitmap;              /* free space is a bitmap (no chain) */
    int      valid;               /* 1 = validated, 0 = rejected near-miss */
    uint64_t end;                 /* one past the fs (validated only) */
    const char *why;              /* rejection reason (rejected only) */
    int      drop;                /* suppressed: inside a validated fs */
};

enum { MAXCAND = 256 };
static struct cand cands[MAXCAND];
static int ncand;

/* `class` is the human-readable disjunction (e.g. "v7, sysiii or sysvr1");
 * `ed` is derived from it as the canonical -v token.  The two stay separate. */
static void add_cand(uint32_t blk, uint64_t byte, const char *class, const char *note,
                     uint16_t isz, uint32_t fsz, uint32_t segs, int root,
                     int bitmap, int valid, uint64_t end, const char *why) {
    if (ncand >= MAXCAND)
        return;
    struct cand *c = &cands[ncand++];
    c->blk = blk; c->byte = byte; c->isz = isz; c->fsz = fsz;
    c->segs = segs; c->root = root; c->bitmap = bitmap;
    c->valid = valid; c->end = end; c->why = why;
    c->drop = 0;
    snprintf(c->class_desc, sizeof c->class_desc, "%s", class);
    snprintf(c->ed, sizeof c->ed, "%s", canonical_token(class));
    snprintf(c->note, sizeof c->note, "%s", note ? note : "");
}

static void print_cand(const char *image, const struct cand *c) {
    printf("found a %s filesystem", c->class_desc);
    if (c->note[0])
        printf(" (%s)", c->note);
    printf(" at block %u (byte %llu), isize=%u fsize=%u", c->blk,
           (unsigned long long)c->byte, c->isz, c->fsz);
    if (c->bitmap)
        printf(", bitmap free list");   /* bitmap free space, not a chain */
    else
        printf(", chain ok (%u segs)", c->segs);
    if (c->root)
        printf(", root inode ok");
    printf("\n");
    printf("  mount.filsys -v %s -o offset=%llu %s /mnt\n",
           c->ed, (unsigned long long)c->byte, image);
}

static void print_miss(const struct cand *c) {
    printf("fs @ block %u  (byte %llu)  %s%s%s%s  isize=%u fsize=%u  REJECTED: %s\n",
           c->blk, (unsigned long long)c->byte, c->class_desc,
           c->note[0] ? " (" : "", c->note[0] ? c->note : "",
           c->note[0] ? ")" : "", c->isz, c->fsz, c->why);
}

static int cand_cmp(const void *a, const void *b) {
    const struct cand *ca = a, *cb = b;
    if (ca->valid != cb->valid)
        return ca->valid ? -1 : 1;      /* validated before rejected */
    if (ca->byte != cb->byte)
        return ca->byte < cb->byte ? -1 : 1;
    return 0;
}

/* Emit the collected candidates: drop any whose offset falls inside an
 * already-validated filesystem's extent (those bytes are that filesystem's own,
 * not a peer), then print validated hits.  Rejected near-misses (and their
 * reasons) print only under `verbose` -- by default the probe locates the
 * filesystem silently and reports what it found. */
static void emit(const char *image, int verbose) {
    for (int i = 0; i < ncand; i++) {
        if (!cands[i].valid || cands[i].drop)
            continue;
        for (int j = 0; j < ncand; j++) {
            if (j == i)
                continue;
            if (cands[j].byte >= cands[i].byte && cands[j].byte < cands[i].end)
                cands[j].drop = 1;
        }
    }
    qsort(cands, (size_t)ncand, sizeof *cands, cand_cmp);
    for (int i = 0; i < ncand; i++) {
        if (cands[i].drop)
            continue;
        if (cands[i].valid)
            print_cand(image, &cands[i]);
        else if (verbose)
            print_miss(&cands[i]);
    }
}

int main(int argc, char **argv) {
    int stride = 1, backtrace = 0, edition_filter = -1, verbose = 0;
    const char *image = NULL;
    int c;
    while ((c = getopt(argc, argv, "v:s:iV")) != -1) {
        switch (c) {
        case 'v':
            edition_filter = filsys_parse_edition("findfs.filsys", optarg);
            if (edition_filter < 0)
                return 2;
            break;
        case 's': stride = atoi(optarg); break;
        case 'i': backtrace = 1; break;
        case 'V': verbose = 1; break;
        default:
            fprintf(stderr, "usage: findfs.filsys [-v <edition>] [-s N] [-i] [-V] <image>\n"
                            "  editions: %s\n", filsys_editions_usage());
            return 2;
        }
    }
    if (optind >= argc || stride < 1) {
        fprintf(stderr, "usage: findfs.filsys [-v <edition>] [-s N] [-i] [-V] <image>\n"
                        "  editions: %s\n", filsys_editions_usage());
        return 2;
    }
    image = argv[optind];

    int fd = open(image, O_RDONLY);
    if (fd < 0) { perror(image); return 1; }
    uint64_t sz = 0;
    if (filsys_dev_size(fd, &sz) || sz < BSIZE) { fprintf(stderr, "cannot size image\n"); return 1; }
    uint32_t nblocks = (uint32_t)(sz / BSIZE);
    uint8_t *sb_buf = malloc(8192);   /* largest superblock read */
    if (!sb_buf) { fprintf(stderr, "out of memory\n"); return 1; }

    int found = 0;
    /* Blocks below `skip_end` (bytes) belong to an already-validated fs's data
     * area, so their free-list dump blocks are not independent candidates. */
    uint64_t skip_end = 0;

    /* One pass over superblock offsets (512-byte units).  A bsize-byte-block
     * filesystem stores its superblock at block 1 = byte bsize, so every
     * superblock lands at a 512-byte-aligned offset and is probed once here for
     * each bsize whose block 1 could land there (sb % (bsize/512) == 0).  The
     * offset is read once, at the largest applicable block size, and every probe
     * that could report there shares that buffer -- no per-format re-scans. */
    for (uint32_t sb = 0; sb < nblocks; sb += (uint32_t)stride) {
        uint64_t sb_byte = (uint64_t)sb * BSIZE;
        if (sb_byte < skip_end) {
            sb = (uint32_t)(skip_end / BSIZE);
            continue;
        }
        int maxb = BSIZE;               /* V7/SysV/V6 need one 512-byte block */
        if (probe_wanted(edition_filter, fam_v1))
            maxb = 1024;                /* V1's 2-block superblock, every sb */
        if (sb % 2 == 0 && maxb < 1024 &&
            (probe_wanted(edition_filter, fam_v8) ||
             probe_wanted(edition_filter, fam_xenix) ||
             probe_wanted(edition_filter, fam_bsd29)))
            maxb = 1024;
        if (sb % 8 == 0 && maxb < 4096 && probe_wanted(edition_filter, fam_v8))
            maxb = 4096;
        if (sb % 16 == 0 && maxb < 8192 && probe_wanted(edition_filter, fam_v9))
            maxb = 8192;
        if (pread(fd, sb_buf, maxb, (off_t)sb_byte) != maxb)
            continue;

        /* V1: dual-bitmap superblock at fs block 0 -- the fs starts AT sb. */
        if (probe_wanted(edition_filter, fam_v1)) {
            uint16_t isz = 0; uint32_t fsz = 0;
            const char *why = NULL;
            if (super_v1(fd, sb_buf, sb, nblocks, &isz, &fsz, &why) == 1 &&
                matches(edition_filter, "v1, v2 or v3")) {
                uint64_t de = sb_byte + (uint64_t)fsz * BSIZE;
                add_cand(sb, sb_byte, "v1, v2 or v3", NULL, isz, fsz, 0, 1, 1, 1, de, NULL);
                if (de > skip_end)
                    skip_end = de;
                found++;
            }
        }

        /* V7 family (512-byte fs, superblock at block 1 = sb): fs starts at sb-1.
         * System V is checked first: its superblock is V7-shaped but the s_magic
         * at offset 504 is the stronger signal. */
        if (sb >= 1) {
            uint64_t byte = (uint64_t)(sb - 1) * BSIZE;
            uint16_t isz = 0; uint32_t fsz = 0;
            const char *ed = NULL, *note = NULL, *why = NULL;
            uint32_t segs = 0;
            int bs = BSIZE;   /* V7/V6 block size; super_sysv overrides */
            int rc = 0;

            if (probe_wanted(edition_filter, fam_sysv))
                rc = super_sysv(fd, sb_buf, sb, nblocks, &ed, &note, &isz, &fsz, &bs, &why, &segs);
            if (rc == 0 && probe_wanted(edition_filter, fam_v7)) {
                bs = BSIZE;
                rc = super_v7(fd, sb_buf, sb, nblocks, &ed, &isz, &fsz, &why, &segs);
            }
            if (rc == 0 && probe_wanted(edition_filter, fam_v6)) {
                ed = "v4, v5, v6 or usgpg3";
                rc = super_v6(fd, sb_buf, sb, nblocks, &isz, &fsz, &why, &segs);
            }
            if (rc >= 1 && matches(edition_filter, ed)) {
                if (!strcmp(ed, "AFS"))
                    printf("fs @ block %u  (byte %llu)  AFS (bitmap free list, unsupported)\n",
                           (uint32_t)(sb - 1), (unsigned long long)byte);
                else if (rc == 1) {
                    uint64_t de = byte + (uint64_t)fsz * (uint64_t)bs;
                    add_cand((uint32_t)(sb - 1), byte, ed, note, isz, fsz, segs,
                             strcmp(ed, "v4, v5, v6 or usgpg3") != 0, 0, 1, de, NULL);
                    if (de > skip_end)
                        skip_end = de;
                } else
                    add_cand((uint32_t)(sb - 1), byte, ed, note, isz, fsz, 0, 0, 0, 0, 0, why);
                found++;
            }

            /* inode-table backtrace: is sb the first inode block of a filesystem
             * whose superblock is at sb-1?  A damaged superblock can fail its own
             * chain walk here -- that is the damage being recovered -- so a
             * near-miss still reports. */
            if (backtrace && inode_block(sb_buf) && sb >= 2 &&
                pread(fd, sb_buf, BSIZE, (off_t)(uint64_t)(sb - 1) * BSIZE) == BSIZE) {
                const char *ed_bt = NULL, *why_bt = NULL;
                uint16_t isz_bt = 0; uint32_t fsz_bt = 0, segs_bt = 0;
                int r = super_v7(fd, sb_buf, sb - 1, nblocks, &ed_bt, &isz_bt, &fsz_bt, &why_bt, &segs_bt);
                if (r == 0) {
                    ed_bt = "v4, v5, v6 or usgpg3";
                    r = super_v6(fd, sb_buf, sb - 1, nblocks, &isz_bt, &fsz_bt, &why_bt, &segs_bt);
                }
                if (r >= 1 && matches(edition_filter, ed_bt)) {
                    printf("fs @ block %u  (byte %llu)  %s  isize=%u fsize=%u   [via inode backtrace]\n",
                           (uint32_t)(sb - 2), (unsigned long long)(uint64_t)(sb - 2) * BSIZE,
                           ed_bt, isz_bt, fsz_bt);
                    found++;
                }
            }
        }

        /* 1024-byte fs (superblock at block 1 = sb, fs starts at sb-2).  V8-family
         * first: its 1024-byte free-list superblock also parses as a 2.9BSD
         * superblock, so the stronger structural test gets first refusal. */
        if (sb >= 2 && sb % 2 == 0) {
            uint64_t byte = (uint64_t)(sb - 2) * BSIZE;
            uint16_t isz = 0; uint32_t fsz = 0;
            const char *why = NULL, *ed = NULL, *note = NULL;
            uint32_t segs = 0; int bitmap = 0;
            uint32_t blk = sb - 2;
            uint32_t xnblocks = (uint32_t)(sz / 1024);
            int rc = 0;

            if (probe_wanted(edition_filter, fam_v8))
                rc = super_v8(fd, sb_buf, sb_byte, 1024, sz, &ed, &note, &isz, &fsz, &bitmap, &why, &segs);
            if (rc == 0 && probe_wanted(edition_filter, fam_xenix)) {
                ed = "Xenix";
                rc = super_xenix(fd, sb_buf, sb / 2, xnblocks, &isz, &fsz, &why, &segs);
            }
            if (rc == 0 && probe_wanted(edition_filter, fam_bsd29)) {
                ed = "2.9BSD";
                rc = super_bsd29(fd, sb_buf, sb / 2, xnblocks, &isz, &fsz, &why, &segs);
                if (rc == 1 && bsd211_root_dir(fd, (uint64_t)(sb / 2 - 1) * BSIZE, 1024))
                    ed = "2.11BSD";
            }
            if (rc >= 1 && matches(edition_filter, ed)) {
                if (rc == 1) {
                    uint64_t de = byte + (uint64_t)fsz * 1024;
                    add_cand(blk, byte, ed, note, isz, fsz, segs, 1, bitmap, 1, de, NULL);
                    if (de > skip_end)
                        skip_end = de;
                } else
                    add_cand(blk, byte, ed, note, isz, fsz, 0, 0, bitmap, 0, 0, why);
                found++;
            }
        }

        /* V8-family 4096/8192-byte blocks (superblock at block 1 = sb). */
        if (sb >= 8 && sb % 8 == 0 && probe_wanted(edition_filter, fam_v8)) {
            uint64_t byte = (uint64_t)(sb - 8) * BSIZE;
            uint16_t isz = 0; uint32_t fsz = 0;
            const char *why = NULL, *ed = NULL, *note = NULL;
            uint32_t segs = 0; int bitmap = 0;
            int rc = super_v8(fd, sb_buf, sb_byte, 4096, sz, &ed, &note, &isz, &fsz, &bitmap, &why, &segs);
            if (rc >= 1 && matches(edition_filter, ed)) {
                uint32_t blk = (uint32_t)(byte / BSIZE);
                if (rc == 1) {
                    uint64_t de = byte + (uint64_t)fsz * 4096;
                    add_cand(blk, byte, ed, note, isz, fsz, segs, 1, bitmap, 1, de, NULL);
                    if (de > skip_end)
                        skip_end = de;
                } else
                    add_cand(blk, byte, ed, note, isz, fsz, 0, 0, bitmap, 0, 0, why);
                found++;
            }
        }
        if (sb >= 16 && sb % 16 == 0 && probe_wanted(edition_filter, fam_v9)) {
            uint64_t byte = (uint64_t)(sb - 16) * BSIZE;
            uint16_t isz = 0; uint32_t fsz = 0;
            const char *why = NULL, *ed = NULL, *note = NULL;
            uint32_t segs = 0; int bitmap = 0;
            int rc = super_v8(fd, sb_buf, sb_byte, 8192, sz, &ed, &note, &isz, &fsz, &bitmap, &why, &segs);
            if (rc >= 1 && matches(edition_filter, ed)) {
                uint32_t blk = (uint32_t)(byte / BSIZE);
                if (rc == 1) {
                    uint64_t de = byte + (uint64_t)fsz * 8192;
                    add_cand(blk, byte, ed, note, isz, fsz, segs, 1, bitmap, 1, de, NULL);
                    if (de > skip_end)
                        skip_end = de;
                } else
                    add_cand(blk, byte, ed, note, isz, fsz, 0, 0, bitmap, 0, 0, why);
                found++;
            }
        }
    }
    free(sb_buf);

    /* emit the collected candidates: validated before rejected, with in-fs
     * candidates suppressed; rejected reasons print only under -V */
    emit(image, verbose);

    /* PDP-7 (V0): word-addressed, at a fixed offset -- byte 0 of a bare
     * filesystem surface, or surface 1 (P7_NBLOCKS blocks in) of a full
     * two-surface RB09 disk image -- not found by the byte-aligned block scans
     * above.  Probe both offsets, once per word container. */
    for (size_t i = 0; i < sizeof p7_containers / sizeof p7_containers[0]; i++) {
        const word_codec_t *wc = p7_containers[i].codec;
        uint64_t surf = (uint64_t)P7_NBLOCKS * wc->block_bytes;
        uint64_t bases[2] = { 0, surf };
        for (int b = 0; b < 2; b++) {
            uint64_t base = bases[b];
            if (sz < base + (uint64_t)P7_KDATA * wc->block_bytes)
                continue;
            uint32_t segs = 0;
            const char *why = NULL;
            if (super_pdp7(fd, wc, base, &segs, &why) != 1)
                continue;
            if (!matches(edition_filter, "PDP-7"))
                continue;
            printf("fs @ byte %llu  PDP-7 (%s)  chain ok (%u segs), root inode ok\n",
                   (unsigned long long)base, p7_containers[i].name, segs);
            printf("  mount.filsys -v pdp7 -o offset=%llu -o packing=%s %s /mnt\n",
                   (unsigned long long)base, p7_containers[i].name, image);
            found++;
        }
    }

    close(fd);
    return found ? 0 : 1;
}
