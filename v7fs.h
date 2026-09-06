/* filsys 1.3.2 - 2026-09-05 - Copyright (C) 2026 David Walther */
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
#include "bo.h"

struct filsys_ops;   /* forward: the per-backend vtable (see filsys_ops.h) */

/* The type a to_disk_mode caller is encoding (create / mkdir / mknod). */
enum {
    FILSYS_FT_REG = 0,
    FILSYS_FT_DIR,
    FILSYS_FT_CHR,
    FILSYS_FT_BLK
};

enum {
    V7_BSIZE    = 512,          /* default / legacy block size */
    V7_MAXBSIZE = 2048,         /* largest block size the engine supports (512/1024/2048) */
    V7_INOPB    = 8,            /* inodes per block (at V7_BSIZE) */
    V7_INODESZ  = 64,           /* sizeof(struct dinode) */
    V7_NICFREE  = 50,          /* superblock free-block cache size (V7/32V) */
    V7_COH_NICFREE = 64,       /* Coherent free-block cache size */
    V7_XEN_NICFREE = 100,      /* Xenix free-block cache size */
    V7_XEN_MAGIC   = 0x2b5544, /* Xenix superblock magic (offset 1016) */
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

/* Byte order lives in bo.h (bo_get16le/bo_get32me/...); v7fs reads and writes
 * through fs->bo (a byte_order_ops_t chosen by the format descriptor). */

/* Superblock field offsets.  32V (VAX) aligns daddr_t/time_t to 4 bytes, so
 * the fields after such a type sit 2 bytes later than in V7; Coherent and
 * Xenix keep the 2-byte packing.  pack4 and nicfree come from the descriptor. */
static inline int sb_fsize_off(int pack4)  { return pack4 ? 4 : 2; }
static inline int sb_nfree_off(int pack4)  { return pack4 ? 8 : 6; }
static inline int sb_free_off(int pack4)   { return pack4 ? 12 : 8; }
static inline int sb_ninode_off(int pack4, int nicfree) { return sb_free_off(pack4) + 4 * nicfree; }
static inline int sb_inode_off(int pack4, int nicfree)  { return sb_ninode_off(pack4, nicfree) + 2; }
static inline int sb_time_off(int pack4, int nicfree)   { return sb_inode_off(pack4, nicfree) + 2*V7_NICINOD + 4 + (pack4 ? 2 : 0); }
static inline int fb_free_off(int pack4)   { return pack4 ? 4 : 2; }

/* ---- core types -------------------------------------------------------- */

/* Decoded inode/dirent are the public filsys types (no per-backend copy, so
 * the ops table needs no type-punning cast). */
typedef filsys_inode_t  v7_inode_t;
typedef filsys_dirent_t v7_dirent_t;

typedef struct filsys_edition {
    /* ---- format descriptor (static; set by filsys_getformat) ---- */
    const struct filsys_ops *ops;   /* backend vtable */
    size_t      state_size;         /* sizeof the backend state struct */
    const char *name;               /* "-v" spelling */
    const byte_order_ops_t *bo;     /* byte-order ops (bo_le / bo_be / bo_me) */
    uint32_t    bsize;              /* logical block size (512 / 1024) */
    uint16_t    nicfree;            /* free-block cache depth (50 / 64 / 100) */
    uint16_t    nicinod;            /* free-inode cache depth */
    uint8_t     pack4;              /* 4-byte-aligned superblock fields (32V) */
    uint8_t     interleave;         /* Coherent s_m/s_n interleave */
    uint32_t    magic;              /* superblock magic word (0 = none) */
    int         magic_off;          /* byte offset of magic in the superblock */
    uint8_t     fmod_back;          /* s_fmod sits N bytes before s_time (V7 2, 2.11BSD 3) */
    uint16_t    rootino;            /* root directory inode (1 / 2 / 41) */
    uint8_t     inode_size;         /* bytes per on-disk inode */
    uint8_t     ndaddr;             /* direct block addresses per inode */
    uint8_t     niaddr;             /* total block addresses per inode */
    uint8_t     addr_width;         /* bytes per on-disk di_addr entry (2/3/4) */
    uint8_t     max_namlen;         /* longest entry name (8 / 14 / 63) */
    uint8_t     dirent_size;        /* bytes per fixed entry (0 = variable) */
    /* on-disk type field (V6/V7/BSD211 family); 0 for V1/PDP-7 */
    uint16_t    ifmt, ifdir, ifreg, ifchr, ifblk, iflnk, ifsock, ifmpc, ifmpb;
    /* mode conversion (NULL = derive from the constants above) */
    mode_t   (*to_posix_mode)(const struct filsys_edition *, const filsys_inode_t *);
    int      (*is_dir)(const struct filsys_edition *, const filsys_inode_t *);
    int      (*is_device)(const struct filsys_edition *, const filsys_inode_t *);
    uint32_t (*to_disk_mode)(const struct filsys_edition *, mode_t m, int type);
    uint32_t (*chmod_mode)(const struct filsys_edition *, uint32_t old_mode, mode_t m);

    /* ---- runtime (filled by v7fs_open) ---- */
    int        fd;             /* open disk image */
    int        readonly;
    uint64_t   base;           /* byte offset of this filesystem within the file */
    /* in-core superblock (kept in sync with block 1) */
    uint16_t   isize;
    uint32_t   fsize;
    uint16_t   nfree;
    uint32_t   free[V7_XEN_NICFREE];   /* max free-cache depth: 50/64/100 */
    uint16_t   ninode;
    uint16_t   inode[V7_NICINOD];
    uint32_t   time;           /* last superblock update */
    uint32_t   tfree;          /* total free blocks (s_tfree) */
    uint32_t   tinode;         /* total free inodes (s_tinode) */
    uint16_t   m;              /* s_m interleave factor (coherent) */
    uint16_t   n;              /* s_n interleave factor (coherent) */
    uint32_t   unique;         /* s_unique (coherent) */
    int        fmod;           /* s_fmod: superblock modified flag (dirty) */
} filsys_edition_t;

/* The descriptor for an edition: a copy of the V7 default with the edition's
 * overrides.  Returns a zeroed descriptor (ops == NULL) for an unknown
 * edition. */
filsys_edition_t filsys_getformat(int edition);

/* ---- lifecycle --------------------------------------------------------- */

/* Open a disk image.  Returns 0, or -errno.  proto is the format descriptor
 * returned by filsys_getformat() (byte order, block size, free-cache width,
 * superblock packing); v7fs_open copies it into *fs and fills the runtime
 * fields.  offset is the byte offset of the filesystem within the file (0 =
 * block 0 of the file). */
int v7fs_open(filsys_edition_t *fs, const char *path, int readonly,
              const filsys_edition_t *proto, uint64_t offset);
/* Flush the superblock and close. */
void v7fs_close(filsys_edition_t *fs);
/* Flush the superblock (and pending metadata) to the image without closing. */
int v7fs_sync(filsys_edition_t *fs);
/* Mark the filesystem dirty (s_fmod) and flush: a read-write mount is dirty
 * until a clean close clears it, so a crash leaves the image flagged for fsck. */
int v7fs_mark_dirty(filsys_edition_t *fs);

/* ---- block / inode io -------------------------------------------------- */

int v7fs_read_block(filsys_edition_t *fs, uint32_t bno, uint8_t *buf);
int v7fs_write_block(filsys_edition_t *fs, uint32_t bno, const uint8_t *buf);

/* Block-size-dependent quantities: the logical block size is fs->bsize, not the
 * compile-time V7_BSIZE.  4-byte daddr_t per indirect, 64-byte inodes. */
static inline uint32_t v7_nindir(const filsys_edition_t *fs) { return fs->bsize / 4; }
static inline uint32_t v7_inopb(const filsys_edition_t *fs)  { return fs->bsize / fs->inode_size; }
/* itod / itoo: inode number -> block and offset. */
static inline uint32_t v7_itod(const filsys_edition_t *fs, uint32_t ino) { return 2 + (ino - 1) / v7_inopb(fs); }
static inline uint32_t v7_itoo(const filsys_edition_t *fs, uint32_t ino) { return (ino - 1) % v7_inopb(fs); }

int v7fs_read_inode(filsys_edition_t *fs, uint32_t ino, v7_inode_t *ip);
int v7fs_write_inode(filsys_edition_t *fs, uint32_t ino, const v7_inode_t *ip);

/* ---- allocation -------------------------------------------------------- */

/* Allocate a free data block into *bno (0 = absent/sparse). */
int v7fs_balloc(filsys_edition_t *fs, uint32_t *bno);
void v7fs_bfree(filsys_edition_t *fs, uint32_t bno);
/* Allocate a free inode into *ino. */
int v7fs_ialloc(filsys_edition_t *fs, uint32_t *ino);
void v7fs_ifree(filsys_edition_t *fs, uint32_t ino);
/* Free every data block referenced by an inode (truncate to 0). */
int v7fs_itrunc(filsys_edition_t *fs, v7_inode_t *ip);
/* Free blocks [first_blk, ...) only; first_blk == 0 == v7fs_itrunc. */
int v7fs_itrunc_from(filsys_edition_t *fs, v7_inode_t *ip, uint32_t first_blk);

/* ---- file / directory data --------------------------------------------- */

/* Map a logical block of an inode to a physical block (allocate if create). */
int v7fs_bmap(filsys_edition_t *fs, v7_inode_t *ip, uint32_t lbn, int create, uint32_t *bno);

ssize_t v7fs_file_read(filsys_edition_t *fs, v7_inode_t *ip, uint8_t *buf, size_t size, off_t off);
ssize_t v7fs_file_write(filsys_edition_t *fs, v7_inode_t *ip, const uint8_t *buf, size_t size, off_t off);

/* Read a directory's entries.  Caller frees with v7fs_dirents_free. */
int v7fs_dir_read(filsys_edition_t *fs, v7_inode_t *ip, v7_dirent_t **ents, size_t *count);
void v7fs_dirents_free(v7_dirent_t *ents);

/* Look up name in a directory; returns 0 and *ino, or -ENOENT. */
int v7fs_dir_lookup(filsys_edition_t *fs, v7_inode_t *ip, const char *name, uint32_t *ino);
/* Add an entry (name must be <= V7_DIRSIZ, no '/'); 0 or -errno. */
int v7fs_dir_add(filsys_edition_t *fs, v7_inode_t *ip, uint32_t ino, const char *name);
/* Remove an entry; 0 or -errno. */
int v7fs_dir_remove(filsys_edition_t *fs, v7_inode_t *ip, const char *name);

/* Resolve a path into an inode number.  0 or -errno. */
int v7fs_lookup(filsys_edition_t *fs, const char *path, uint32_t *ino, v7_inode_t *ip);

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
int v7fs_check(filsys_edition_t *fs, v7_check_t *rep, int mode);
/* 2.11BSD's check: icheck+dcheck without the V7 repair modes (mode is ignored).
 * The on-disk layout is the V7 engine's, so it shares filsys_edition_t. */
int bsd211_check(filsys_edition_t *fs, v7_check_t *rep, int mode);
/* V6's check/maintenance: its isize semantics (s_isize = i-list block count),
 * ILARG bmap and 32-byte inode make these V6-specific, but they operate on the
 * shared filsys_edition_t. */
int v6_check(filsys_edition_t *fs, v7_check_t *rep, int mode);
int v6_ncheck(filsys_edition_t *fs, uint32_t ino);
int v6_clri(filsys_edition_t *fs, uint32_t ino);
int v6_resolve_dups(filsys_edition_t *fs);

/* ---- maintenance (V7's ncheck / clri / salv -a) ------------------------ */

/* Print the full pathname(s) of inode `ino` (ncheck).  Returns 0. */
int v7fs_ncheck(filsys_edition_t *fs, uint32_t ino);
/* Zero inode `ino` (clri).  Returns 0 or -errno. */
int v7fs_clri(filsys_edition_t *fs, uint32_t ino);
/* Resolve duplicate blocks (salv -a): give each second reference a private
 * copy of the block, then rebuild the free list.  Returns 0 or -errno. */
int v7fs_resolve_dups(filsys_edition_t *fs);

#endif /* V7FS_H */
