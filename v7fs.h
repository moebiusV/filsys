/* filsys 1.2.7 - 2026-09-04 - Copyright (C) 2026 David Walther */
/* SPDX-License-Identifier: ISC */
/* v7fs.h - Seventh Edition (V7) Unix filesystem, on-disk access layer.
 *
 * The V7 filesystem lives on a PDP-11 disk image.  Block size is 512 bytes:
 *
 *     block 0             boot block
 *     block 1             superblock (struct filsys)
 *     blocks 2..s_isize-1 i-list (s_isize-2 blocks, 8 inodes each)
 *     blocks s_isize..    data blocks
 *
 * Byte order is the PDP-11's "middle-endian" convention:
 *   - 16-bit quantities are little-endian (single word).
 *   - 32-bit quantities (daddr_t / off_t / time_t) are stored as two words,
 *     most-significant word first, each word little-endian.
 *   - the 3-byte block numbers packed into di_addr are the low three bytes of
 *     that middle-endian 32-bit layout: [ hi, lo, mid ].
 *   - indirect blocks store full 4-byte middle-endian daddr_t entries.
 */
#ifndef V7FS_H
#define V7FS_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#include "filsys.h"

enum {
    V7_BSIZE    = 512,
    V7_INOPB    = 8,           /* inodes per block */
    V7_INODESZ  = 64,          /* sizeof(struct dinode) */
    V7_NICFREE  = 50,          /* superblock free-block cache size (V7/32V) */
    V7_COH_NICFREE = 64,       /* Coherent free-block cache size */
    V7_COH_MAXINTN  = 255,     /* Coherent interleave bound (fsck MAXINTN) */
    V7_NICINOD  = 100,         /* superblock free-inode cache size */
    V7_ROOTINO  = 2,
    V7_BADFIN   = 1,           /* the bad-block inode (records bad i-list blocks) */
    V7_SUPERB   = 1,           /* block number of superblock */
    V7_NDADDR   = 10,          /* direct addresses per inode */
    V7_NIADDR   = 13,          /* total address slots per inode */
    V7_NINDIR   = V7_BSIZE / 4, /* 4-byte addresses per indirect block */
    V7_DIRSIZ   = 14,          /* chars per directory entry name */
    V7_DIRENTSZ = 2 + V7_DIRSIZ /* bytes per on-disk directory entry */
};

/* di_mode type/mode bits (sys/ino.h) */
enum {
    V7_IFMT   = 0170000,
    V7_IFCHR  = 0020000,
    V7_IFDIR  = 0040000,
    V7_IFBLK  = 0060000,
    V7_IFREG  = 0100000,
    V7_IFMPC  = 0030000,
    V7_IFMPB  = 0070000,
    V7_IFIFO  = 0010000,   /* Coherent: pipe inode (V7 has no such type) */
    V7_ISUID  = 0004000,
    V7_ISGID  = 0002000,
    V7_ISVTX  = 0001000,
    V7_IREAD  = 0000400,
    V7_IWRITE = 0000200,
    V7_IEXEC  = 0000100
};

/* ---- byte-order primitives --------------------------------------------- */

static inline uint16_t v7_get16le(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline void v7_put16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}
/* 32-bit middle-endian: high word first, each word little-endian. */
static inline uint32_t v7_get32me(const uint8_t *p) {
    uint16_t hi = v7_get16le(p);
    uint16_t lo = v7_get16le(p + 2);
    return ((uint32_t)hi << 16) | lo;
}
static inline void v7_put32me(uint8_t *p, uint32_t v) {
    v7_put16le(p,     (uint16_t)(v >> 16));
    v7_put16le(p + 2, (uint16_t)(v & 0xffff));
}
/* 24-bit block number packed as [ hi, lo, mid ]. */
static inline uint32_t v7_get24me(const uint8_t *p) {
    return (uint32_t)p[1] | ((uint32_t)p[2] << 8) | ((uint32_t)p[0] << 16);
}
static inline void v7_put24me(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 16) & 0xff);  /* hi  */
    p[1] = (uint8_t)(v & 0xff);          /* lo  */
    p[2] = (uint8_t)((v >> 8) & 0xff);   /* mid */
}

/* 32V (VAX) stores the same fields little-endian: 32-bit values low word
 * first, and the 3-byte di_addr bytes as [ lo, mid, hi ].  The PDP-11 (V7)
 * layout above is middle-endian.  These dispatch on the edition's byte order,
 * so the rest of the code can stay edition-neutral. */
static inline uint32_t v7_get32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void v7_put32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}
static inline uint32_t v7_get32(const uint8_t *p, int le) {
    return le ? v7_get32le(p) : v7_get32me(p);
}
static inline void v7_put32(uint8_t *p, int le, uint32_t v) {
    if (le) v7_put32le(p, v); else v7_put32me(p, v);
}
static inline uint32_t v7_get24(const uint8_t *p, int le) {
    return le ? (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
              : v7_get24me(p);
}
static inline void v7_put24(uint8_t *p, int le, uint32_t v) {
    if (le) {
        p[0] = (uint8_t)(v & 0xff);
        p[1] = (uint8_t)((v >> 8) & 0xff);
        p[2] = (uint8_t)((v >> 16) & 0xff);
    } else {
        v7_put24me(p, v);
    }
}

/* 32V (VAX) aligns daddr_t/time_t to 4 bytes, so in the superblock and the
 * free-list dump block the fields that follow such a type sit 2 bytes later
 * than they do in V7 (PDP-11, 2-byte alignment).  The inode is unaffected
 * (di_size already lands on a 4-byte boundary).  Coherent keeps the 2-byte
 * packing (8086 heritage) and widens the free cache to 64 entries, but its
 * on-disk byte order is the PDP-11's middle-endian, not 32V's little-endian:
 * the format was fixed on the PDP-11 and Coherent preserved it verbatim on
 * x86.  `coh` selects that layout, `le` the byte order. */
static inline int sb_nicfree(int coh)       { return coh ? V7_COH_NICFREE : V7_NICFREE; }
static inline int sb_fsize_off(int le, int coh)  { return (le && !coh) ? 4 : 2; }
static inline int sb_nfree_off(int le, int coh)  { return (le && !coh) ? 8 : 6; }
static inline int sb_free_off(int le, int coh)   { return (le && !coh) ? 12 : 8; }
static inline int sb_ninode_off(int le, int coh) { return sb_free_off(le, coh) + 4 * sb_nicfree(coh); }
static inline int sb_inode_off(int le, int coh)  { return sb_ninode_off(le, coh) + 2; }
static inline int sb_time_off(int le, int coh)   { return sb_inode_off(le, coh) + 2*V7_NICINOD + 4 + ((le && !coh) ? 2 : 0); }
static inline int fb_free_off(int le, int coh)   { return (le && !coh) ? 4 : 2; }

/* ---- core types -------------------------------------------------------- */

/* Decoded inode/dirent are the public filsys types (no per-backend copy, so
 * the ops table needs no type-punning cast). */
typedef filsys_inode_t  v7_inode_t;
typedef filsys_dirent_t v7_dirent_t;

typedef struct {
    int        fd;             /* open disk image */
    int        readonly;
    int        le;             /* 0 = PDP-11 middle-endian (V7/Coherent), 1 = little-endian (32V) */
    int        coherent;       /* Coherent: 2-byte-packed superblock, NICFREE=64, s_unique */
    uint64_t   base;           /* byte offset of this filesystem within the file */
    /* in-core superblock (kept in sync with block 1) */
    uint16_t   isize;
    uint32_t   fsize;
    uint16_t   nfree;
    uint32_t   free[V7_COH_NICFREE];
    uint16_t   ninode;
    uint16_t   inode[V7_NICINOD];
    uint32_t   time;           /* last superblock update */
    uint32_t   tfree;          /* total free blocks (s_tfree) */
    uint32_t   tinode;         /* total free inodes (s_tinode) */
    uint16_t   m;              /* s_m interleave factor (coherent) */
    uint16_t   n;              /* s_n interleave factor (coherent) */
    uint32_t   unique;         /* s_unique (coherent) */
    int        fmod;           /* s_fmod: superblock modified flag (dirty) */
} v7fs_t;

/* ---- lifecycle --------------------------------------------------------- */

/* Open a disk image.  Returns 0, or -errno.  little_endian selects the 32V
 * (VAX) byte order for 32-bit fields and 3-byte block addresses; 0 selects
 * the PDP-11 (V7) middle-endian order.  offset is the byte offset of the
 * filesystem within the file (0 = it starts at block 0 of the file). */
int v7fs_open(v7fs_t *fs, const char *path, int readonly, int mode,
              uint64_t offset);
/* Flush the superblock and close. */
void v7fs_close(v7fs_t *fs);
/* Flush the superblock (and pending metadata) to the image without closing. */
int v7fs_sync(v7fs_t *fs);
/* Mark the filesystem dirty (s_fmod) and flush: a read-write mount is dirty
 * until a clean close clears it, so a crash leaves the image flagged for fsck. */
int v7fs_mark_dirty(v7fs_t *fs);

/* ---- block / inode io -------------------------------------------------- */

int v7fs_read_block(v7fs_t *fs, uint32_t bno, uint8_t *buf);
int v7fs_write_block(v7fs_t *fs, uint32_t bno, const uint8_t *buf);

/* itod / itoo: inode number -> block and offset. */
static inline uint32_t v7_itod(uint32_t ino) { return (ino + 15) >> 3; }
static inline uint32_t v7_itoo(uint32_t ino) { return (ino + 15) & 7; }

int v7fs_read_inode(v7fs_t *fs, uint32_t ino, v7_inode_t *ip);
int v7fs_write_inode(v7fs_t *fs, uint32_t ino, const v7_inode_t *ip);

/* ---- allocation -------------------------------------------------------- */

/* Allocate a free data block into *bno (0 = absent/sparse). */
int v7fs_balloc(v7fs_t *fs, uint32_t *bno);
void v7fs_bfree(v7fs_t *fs, uint32_t bno);
/* Allocate a free inode into *ino. */
int v7fs_ialloc(v7fs_t *fs, uint32_t *ino);
void v7fs_ifree(v7fs_t *fs, uint32_t ino);
/* Free every data block referenced by an inode (truncate to 0). */
int v7fs_itrunc(v7fs_t *fs, v7_inode_t *ip);
/* Free blocks [first_blk, ...) only; first_blk == 0 == v7fs_itrunc. */
int v7fs_itrunc_from(v7fs_t *fs, v7_inode_t *ip, uint32_t first_blk);

/* ---- file / directory data --------------------------------------------- */

/* Map a logical block of an inode to a physical block (allocate if create). */
int v7fs_bmap(v7fs_t *fs, v7_inode_t *ip, uint32_t lbn, int create, uint32_t *bno);

ssize_t v7fs_file_read(v7fs_t *fs, v7_inode_t *ip, uint8_t *buf, size_t size, off_t off);
ssize_t v7fs_file_write(v7fs_t *fs, v7_inode_t *ip, const uint8_t *buf, size_t size, off_t off);

/* Read a directory's entries.  Caller frees with v7fs_dirents_free. */
int v7fs_dir_read(v7fs_t *fs, v7_inode_t *ip, v7_dirent_t **ents, size_t *count);
void v7fs_dirents_free(v7_dirent_t *ents);

/* Look up name in a directory; returns 0 and *ino, or -ENOENT. */
int v7fs_dir_lookup(v7fs_t *fs, v7_inode_t *ip, const char *name, uint32_t *ino);
/* Add an entry (name must be <= V7_DIRSIZ, no '/'); 0 or -errno. */
int v7fs_dir_add(v7fs_t *fs, v7_inode_t *ip, uint32_t ino, const char *name);
/* Remove an entry; 0 or -errno. */
int v7fs_dir_remove(v7fs_t *fs, v7_inode_t *ip, const char *name);

/* Resolve a path into an inode number.  0 or -errno. */
int v7fs_lookup(v7fs_t *fs, const char *path, uint32_t *ino, v7_inode_t *ip);

/* ---- integrity check ---------------------------------------------------- */

typedef struct {
    uint32_t free_blocks;    /* free blocks found by walking the free list */
    uint32_t used_blocks;    /* data blocks referenced by inodes */
    uint32_t missing_blocks; /* blocks in the data area referenced by neither */
    uint32_t dup_blocks;     /* blocks referenced twice, or used + free */
    uint32_t inodes;         /* total inode slots */
    uint32_t used_inodes;    /* inodes with a non-zero mode */
    uint32_t errors;         /* number of integrity problems found */
} v7_check_t;

/* Run the V7 equivalent of icheck(8) + dcheck(8): walk the inode table marking
 * every referenced block, walk the free list, detect duplicates and missing
 * blocks, and check directory link counts.  If mode has FILSYS_CK_SALVAGE set, rebuild the
 * free list from the block-usage map (icheck -s) instead of checking it; the
 * filesystem must have been opened read-write.  Reports to stdout; returns 0
 * if no errors were found, -1 otherwise. */
int v7fs_check(v7fs_t *fs, v7_check_t *rep, int mode);

/* ---- maintenance (V7's ncheck / clri / salv -a) ------------------------ */

/* Print the full pathname(s) of inode `ino` (ncheck).  Returns 0. */
int v7fs_ncheck(v7fs_t *fs, uint32_t ino);
/* Zero inode `ino` (clri).  Returns 0 or -errno. */
int v7fs_clri(v7fs_t *fs, uint32_t ino);
/* Resolve duplicate blocks (salv -a): give each second reference a private
 * copy of the block, then rebuild the free list.  Returns 0 or -errno. */
int v7fs_resolve_dups(v7fs_t *fs);

#endif /* V7FS_H */
