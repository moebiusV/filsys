/* filsys 1.8.0 - 2026-09-06 - Copyright (C) 2026 David Walther */
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
#include "byteorder.h"
#include "check.h"

struct filsys_ops;       /* forward: the per-backend vtable (see filsys_ops.h) */
struct word_codec;       /* forward: PDP-7's 18-bit-word container codec (pdp7fs.h) */
struct filsys_edition;   /* forward: the unified descriptor+state struct (defined below) */

/* Raw byte-slice transport: read/write exactly `n` bytes at absolute image
 * offset `off`.  filsys_io_file is the default (pread/pwrite on fs->fd); a test
 * backend can swap fs->io for a fault-injecting implementation, which is how the
 * mutator/fsck regression suite drives block I/O failures without touching the
 * format code.  read/write return 0 on a full transfer, -EIO on a short one. */
typedef struct filsys_io {
    int (*read)(struct filsys_edition *fs, void *buf, size_t n, off_t off);
    int (*write)(struct filsys_edition *fs, const void *buf, size_t n, off_t off);
} filsys_io_t;

extern const filsys_io_t filsys_io_file;

/* Map a CPU architecture name ("vax", "3b2", "68k", "pdp11", ...) to the byte
 * order it stored multi-byte fields in, or NULL for an unknown arch.  The arch
 * (not the edition) selects the byte order: System V ran on both endiannesses. */
const byte_order_ops_t *filsys_arch_bo(const char *arch);

/* The type a to_disk_mode caller is encoding (create / mkdir / mknod / symlink). */
enum {
    FILSYS_FT_REG = 0,
    FILSYS_FT_DIR,
    FILSYS_FT_CHR,
    FILSYS_FT_BLK,
    FILSYS_FT_LNK
};

enum {
    V7_BSIZE    = 512,          /* default / legacy block size */
    V7_MAXBSIZE = 8192,         /* largest block size the engine supports (512..8192; V9's 8K) */
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

/* System V s5fs: s_magic / s_type sit in the last 8 bytes of the 512-byte
 * superblock (offsets 504 and 508).  s_type names the block size.  The magic
 * lives in its own enum so a 32-bit value that overflows int does not widen the
 * type of the small V7 constants above (a C enum shares one underlying type). */
enum {
    V7_SYSV_MAGIC    = 0xfd187e20,
    V7_SYSV_MAGIC_OFF = 504,
    V7_SYSV_TYPE_OFF  = 508,
    V7_SYSV_STATE_OFF = 500,          /* s_state (R4; s_fill[12] in R2/R3) */
    V7_SYSV_STATE_CLEAN = 0x7c269d38, /* FsOKAY: cleanly unmounted */
    V7_SYSV_STATE_ACTIVE = 0x5e72d81a,/* FsACTIVE: mounted (dirty) */
    V7_SYSV_Fs1b      = 1,   /* 512-byte blocks */
    V7_SYSV_Fs2b      = 2,   /* 1024-byte blocks */
    V7_SYSV_Fs4b      = 3    /* 2048-byte blocks */
};

/* Superblock field offsets.  32V (VAX) aligns daddr_t/time_t to 4 bytes, so
 * the fields after such a type sit 2 bytes later than in V7; Coherent and
 * Xenix keep the 2-byte packing.  pack4 and nicfree come from the descriptor. */
static inline int sb_fsize_off(int pack4)  { return pack4 ? 4 : 2; }
static inline int sb_nfree_off(int pack4)  { return pack4 ? 8 : 6; }
static inline int sb_free_off(int pack4)   { return pack4 ? 12 : 8; }
static inline int sb_ninode_off(int pack4, int nicfree) { return sb_free_off(pack4) + 4 * nicfree; }
static inline int sb_inode_off(int pack4, int nicfree)  { return sb_ninode_off(pack4, nicfree) + 2; }
static inline int sb_time_off(int pack4, int nicfree)   { return sb_inode_off(pack4, nicfree) + 2*V7_NICINOD + 4 + (pack4 ? 2 : 0); }
/* s_tfree sits right after s_time in V7/32V/Xenix, but System V inserts
 * s_dinfo[4] (8 bytes of device info) first, so its totals are 8 bytes later. */
static inline int sb_tfree_off(int pack4, int nicfree, int dyn_bsize) {
    return sb_time_off(pack4, nicfree) + 4 + (dyn_bsize ? 8 : 0);
}
static inline int fb_free_off(int pack4)   { return pack4 ? 4 : 2; }

/* ---- V6 on-disk constants (the V6 backend runs on this engine) ---------- */

/* V6 keeps V7's free-list superblock but with 16-bit block numbers, s_isize as
 * the *number* of i-list blocks, a 32-byte inode, and the ILARG large-file
 * layout.  These are the raw on-disk numbers plus the isize-semantics helpers,
 * shared by the V6 code in v7fs.c, the descriptor row, and mkfs. */
enum {
    V6_BSIZE   = 512,
    V6_INOPB   = 16,           /* inodes per block */
    V6_INODESZ = 32,           /* sizeof(struct inode on disk) */
    V6_NICFREE = 100,          /* superblock free-block cache size */
    V6_NICINOD = 100,          /* superblock free-inode cache size */
    V6_ROOTINO = 1,
    V6_SUPERB  = 1,            /* block number of superblock */
    V6_NIADDR  = 8,            /* total address slots per inode */
    V6_NDADDR  = 8,            /* direct blocks in a small file */
    V6_NINDIR  = V6_BSIZE / 2, /* 2-byte addresses per indirect block */
    V6_DIRSIZ  = 14            /* chars per directory entry name */
};

/* i_mode type/mode bits.  V6 has no IFREG (regular = type 0) and no
 * IFMPC/IFMPB; bit 010000 is the ILARG large-file flag. */
enum {
    V6_IALLOC = 0100000,   /* allocated bit (set in every live inode) */
    V6_IFMT   = 0060000,
    V6_IFCHR  = 0020000,
    V6_IFDIR  = 0040000,
    V6_IFBLK  = 0060000,
    V6_ILARG  = 0010000,
    V6_ISUID  = 0004000,
    V6_ISGID  = 0002000,
    V6_ISVTX  = 0001000,
    V6_IREAD  = 0000400,
    V6_IWRITE = 0000200,
    V6_IEXEC  = 0000100
};

/* V6 stores the NUMBER of i-list blocks in s_isize (unlike V7, which stores
 * the first data block).  The i-list occupies blocks 2..s_isize+1, so the
 * first data block is s_isize+2 and the inode count is s_isize*16. */
static inline uint32_t v6_data_start(uint32_t isize) { return isize + 2; }
static inline uint32_t v6_maxino(uint32_t isize) { return (uint32_t)isize * V6_INOPB; }

/* itod / itoo: inode number -> block and offset (16 inodes per block). */
static inline uint32_t v6_itod(uint32_t ino) { return (ino + 31) >> 4; }
static inline uint32_t v6_itoo(uint32_t ino) { return (ino + 31) & 15; }

/* ---- 2.11BSD on-disk constants (see docs/2bsd-format.md) ---------------- */

/* 2.11BSD keeps V7's free-list superblock but swaps in the "new" BSD inode
 * (32-bit block addresses, 4 direct + 3 indirect, di_flags, symlinks) and
 * variable-length directory entries.  Raw on-disk numbers, shared by the
 * descriptor row and mkfs's self-contained 2.11BSD path. */
enum {
    BSD211_BSIZE      = 1024,
    BSD211_INODESZ    = 64,     /* sizeof(struct dinode) */
    BSD211_INOPB      = 16,     /* inodes per block (1024/64) */
    BSD211_NICFREE    = 50,     /* superblock free-block cache size */
    BSD211_NICINOD    = 100,    /* superblock free-inode cache size */
    BSD211_ROOTINO    = 2,
    BSD211_LOSTFOUNDINO = 3,
    BSD211_SUPERB     = 1,      /* block number of superblock */
    BSD211_NDADDR     = 4,      /* direct addresses per inode */
    BSD211_NIADDR     = 7,      /* total address slots (4 direct + 3 indirect) */
    BSD211_NINDIR     = 256,    /* 4-byte addresses per indirect block (1024/4) */
    BSD211_MAXNAMLEN  = 63,
    BSD211_DIRBLKSIZ  = 512,    /* directory framing (still 512 inside 1024 blocks) */
};

/* di_mode type/mode bits.  Note IFLNK (symlink) and IFSOCK. */
enum {
    BSD211_IFMT   = 0170000,
    BSD211_IFCHR  = 0020000,
    BSD211_IFDIR  = 0040000,
    BSD211_IFBLK  = 0060000,
    BSD211_IFREG  = 0100000,
    BSD211_IFLNK  = 0120000,   /* symbolic link */
    BSD211_IFSOCK = 0140000,   /* socket */
    BSD211_ISUID  = 0004000,
    BSD211_ISGID  = 0002000,
    BSD211_ISVTX  = 0001000,
    BSD211_IREAD  = 0000400,
    BSD211_IWRITE = 0000200,
    BSD211_IEXEC  = 0000100
};

/* Superblock field offsets (struct fs, packed, block 1). */
enum {
    BSD211_SB_ISIZE   = 0,     /* u16: first block after i-list */
    BSD211_SB_FSIZE   = 2,     /* u32: size of entire volume in blocks */
    BSD211_SB_NFREE   = 6,     /* u16 */
    BSD211_SB_FREE    = 8,     /* u32 x NICFREE */
    BSD211_SB_NINODE  = 208,   /* u16 */
    BSD211_SB_INODE   = 210,   /* u16 x NICINOD */
    BSD211_SB_FMOD    = 411,   /* u8: superblock modified flag */
    BSD211_SB_TIME    = 414,   /* u32 */
    BSD211_SB_TFREE   = 418,   /* u32: total free blocks */
    BSD211_SB_TINODE  = 422,   /* u16: total free inodes */
    BSD211_SB_STEP    = 424,   /* u16: interleave m */
    BSD211_SB_CYL     = 426,   /* u16: interleave n */
};

/* ---- V8-family (Eighth/Ninth/Tenth Edition) on-disk constants -------------- */
/* See docs/impl-v10fs.md §5.  The inode (64 bytes, 13 three-byte addresses) and
 * the 16-byte directory entry are byte-identical to V7's, so the V7 engine's
 * inode/dir/bmap code is reused unchanged; only the superblock is rearranged.
 * V7 puts the free-list cache (s_nfree/s_free) right after s_fsize and
 * s_fname/s_fpack at the tail; the V8 family moves the free-list/bitmap into a
 * union at the *end* and adds s_fsmnt/s_lasti/s_nbehind. */
enum {
    V8_SB_ISIZE    = 0,     /* u16: first data block number (not a count) */
    V8_SB_FSIZE    = 4,     /* u32: total blocks (4-byte aligned) */
    V8_SB_NINODE   = 8,     /* u16: valid entries in s_inode */
    V8_SB_INODE    = 10,    /* u16 x NICINOD */
    V8_SB_FMOD     = 212,   /* u8: superblock-modified flag */
    V8_SB_TIME     = 216,   /* u32 */
    V8_SB_TFREE    = 220,   /* u32: total free blocks */
    V8_SB_TINODE   = 224,   /* u16: total free inodes */
    V8_SB_DINFO    = 226,   /* s_m @226, s_n @228 (V7 interleave / V10 cylsize+aspace) */
    V8_SB_FSMNT    = 230,   /* 14-byte mount name */
    V8_SB_LASTI    = 244,   /* u16: inode allocation rotor */
    V8_SB_NBEHIND  = 246,   /* u16: est. free inodes below s_lasti */
    V8_SB_UNION    = 248,   /* free-list or bitmap (below) */
    /* free-list union arm (§5.9a): s_nfree is a 2-byte short here */
    V8_SB_NFREE    = 248,
    V8_SB_FREE     = 252,   /* u32 x NICFREE */
    /* in-superblock bitmap arm (§5.9b) */
    V8_SB_VALID    = 248,   /* u8: S_valid (1 on disk = valid) */
    V8_SB_FLAG     = 249,   /* u8: S_flag (V10 only; padding in V8/V9) */
    V8_SB_BFREE    = 252,   /* u32 x V8_BITMAP */
    /* out-of-superblock bitmap arm (§5.9c, v10 only) */
    V8_SB_BSIZE    = 252,   /* u32: bits per bitmap block */
    /* free-space forms (freemap descriptor field; = the public FILSYS_FREEMAP_*) */
    V8_FREEMAP_LIST   = FILSYS_FREEMAP_LIST,
    V8_FREEMAP_BITMAP = FILSYS_FREEMAP_BITMAP,
    V8_FREEMAP_BIGMAP = FILSYS_FREEMAP_BIGMAP,
    /* geometry constants */
    V8_NICFREE_SMALL = 178, /* free-list cache depth (1K/4K blocks) */
    V8_NICFREE_LARGE = 946, /* V9's 8K free-list cache depth */
    V8_BITMAP        = 961, /* in-superblock bitmap longwords */
    V8_BITCELL       = 32,  /* bits per bitmap longword */
    V8_BITMAP_BITS   = V8_BITMAP * V8_BITCELL, /* 30752: data blocks the
                           in-superblock bitmap can describe (the bigmap
                           threshold) */
    V8_IFLNK         = 0120000, /* symbolic link (present in V8) */
    /* V9/V10 concurrency bits: ICONC overlaps the sticky bit, ICCTYP overlaps
     * setuid/setgid (§5.7).  V8 has ISVTX (V7_ISVTX) instead. */
    V8_ICONC         = 0001000,
    V8_ICCTYP        = 0007000,
    V8_ISYNC         = 0001000,
    V8_IEXCL         = 0003000,
};

/* ---- core types -------------------------------------------------------- */

/* Decoded inode/dirent are the public filsys types (no per-backend copy, so
 * the ops table needs no type-punning cast). */
typedef filsys_inode_t  v7_inode_t;
typedef filsys_dirent_t v7_dirent_t;

/* In-core free-list allocator state (V6/V7/BSD211; the PDP-7 keeps its own
 * on-disk head, V1 a bitmap).  This was the free-list cache living directly in
 * filsys_edition_t; it moves here so the generic descriptor no longer carries
 * one allocator's runtime state. */
typedef struct {
    uint16_t nfree;
    uint32_t free[V8_NICFREE_LARGE]; /* max free-cache depth: 50/64/100/178/946 */
    uint16_t ninode;
    uint16_t inode[V7_NICINOD];
    uint32_t tfree;                  /* total free blocks (s_tfree) */
    uint32_t tinode;                 /* total free inodes (s_tinode) */
} freelist_state;

extern const alloc_ops_t freelist_alloc_ops;
extern const alloc_ops_t v8_bitmap_alloc_ops;

typedef struct filsys_edition {
    /* ---- format descriptor (static; set by filsys_getformat) ---- */
    const struct filsys_ops *ops;   /* backend vtable */
    const alloc_ops_t *alloc;       /* allocator (free-list or bitmap) */
    size_t      state_size;         /* sizeof the backend state struct */
    const char *name;               /* "-v" spelling */
    const byte_order_ops_t *bo;     /* byte-order ops (bo_le / bo_be / bo_me) */
    uint32_t    bsize;              /* logical block size (128 / 512 / 1024) */
    uint16_t    nicfree;            /* free-block cache depth (50 / 64 / 100) */
    uint16_t    nicinod;            /* free-inode cache depth */
    uint8_t     pack4;              /* 4-byte-aligned superblock fields (32V) */
    uint8_t     interleave;         /* Coherent s_m/s_n interleave */
    uint32_t    magic;              /* superblock magic word (0 = none) */
    int         magic_off;          /* byte offset of magic in the superblock */
    uint8_t     dyn_bsize;          /* block size comes from s_type (System V) */
    uint8_t     has_state;          /* s_state field present (SysV R4; R2/R3 leave s_fill[12]) */
    uint8_t     ignore_magic;       /* -F: open despite a magic that matches neither order */
    uint8_t     fmod_back;          /* s_fmod sits N bytes before s_time (V7 2, 2.11BSD 3) */
    uint8_t     has_fmod;           /* s_fmod is a reliable clean flag (V6/V7 family; not V1/PDP-7/2.11BSD) */
    uint8_t     size_bits;          /* on-disk size-field width when it caps max_file (V6 24, V1 16; 0 = block map is the limit) */
    uint16_t    rootino;            /* root directory inode (1 / 2 / 41) */
    uint8_t     inode_size;         /* bytes per on-disk inode */
    uint8_t     ndaddr;             /* direct block addresses per inode */
    uint8_t     niaddr;             /* total block addresses per inode */
    uint8_t     addr_width;         /* bytes per on-disk di_addr entry (2/3/4) */
    uint8_t     daddr_wid;          /* bytes per block address in free list & indirect blocks (2/4) */
    uint8_t     df_nfree_wid;       /* bytes per df_nfree in a free-list *chain* block (2 V7 / 4 V8-family) */
    uint8_t     isize_count;        /* s_isize counts i-list blocks (V6) vs first data block (V7) */
    uint32_t    nindir;             /* block pointers per indirect block */
    uint32_t    ilarg_mask;         /* mode bit marking a "large" file (0 = none) */
    uint8_t     large_single;       /* single-indirect slots in the large layout */
    uint8_t     large_double;       /* double-indirect slots in the large layout (0/1) */
    uint8_t     badino;             /* bad-block inode (records bad i-list blocks; 0 = none).
                                     * A descriptor whose mkfs reserves a nameless inode must
                                     * set this, or fsck reports it as an unreferenced orphan. */
    uint32_t (*ind_get)(const struct filsys_edition *, const uint8_t *buf, uint32_t i);
    void     (*ind_put)(const struct filsys_edition *, uint8_t *buf, uint32_t i, uint32_t v);
    const struct word_codec *word;  /* PDP-7 word container codec (NULL = byte-addressed) */
    uint8_t     max_namlen;         /* longest entry name (8 / 14 / 63) */
    uint8_t     dirent_size;        /* bytes per fixed entry (0 = variable) */
    uint8_t     synth_dot;          /* dir_read synthesizes "." / ".." (PDP-7) */
    uint8_t     freemap;            /* V8-family free-space form (V8_FREEMAP_*); 0 = free list */
    uint8_t     nomkfs;             /* 1 = mkfs.filsys cannot create this format yet (read-only) */
    uint8_t     noprobe;            /* 1 = findfs cannot detect this format yet (probe pending) */
    /* on-disk type field (V6/V7/BSD211 family); 0 for V1/PDP-7 */
    uint16_t    ifmt, ifdir, ifreg, ifchr, ifblk, iflnk, ifsock, ifmpc, ifmpb;
    /* mode conversion (NULL = derive from the constants above) */
    mode_t   (*to_posix_mode)(const struct filsys_edition *, const filsys_inode_t *);
    int      (*is_dir)(const struct filsys_edition *, const filsys_inode_t *);
    int      (*is_device)(const struct filsys_edition *, const filsys_inode_t *);
    uint32_t (*to_disk_mode)(const struct filsys_edition *, mode_t m, int type);
    uint32_t (*chmod_mode)(const struct filsys_edition *, uint32_t old_mode, mode_t m);
    /* Superblock codec: decode/encode the block-1 bytes into/from the in-core
     * fields (isize, fsize, free-list cache, totals, fmod, time).  NULL = the
     * V7-family inline codec; the V8-family sets these to its own rearranged
     * layout (v8_sb_decode/v8_sb_encode). */
    int  (*sb_decode)(struct filsys_edition *fs, const uint8_t *sb);
    int  (*sb_encode)(struct filsys_edition *fs, uint8_t *sb);

    /* ---- runtime (filled by v7fs_open) ---- */
    int        fd;             /* open disk image */
    int        readonly;
    const filsys_io_t *io;     /* raw byte-slice transport (default: filsys_io_file) */
    uint64_t   base;           /* byte offset of this filesystem within the file */
    /* in-core superblock (kept in sync with block 1) */
    uint16_t   isize;
    uint32_t   fsize;
    uint32_t   maxino;         /* inode slots (V1 reads it; V7/V6 derive it) */
    uint32_t   freelist;       /* PDP-7 on-disk free-list head (block 0 word 0) */
    union {                     /* allocator state: free-list cache or bitmap */
        freelist_state fl;
        bitmap_state   bm;
    };
    uint32_t   time;           /* last superblock update */
    uint16_t   m;              /* s_m interleave factor (coherent) */
    uint16_t   n;              /* s_n interleave factor (coherent) */
    uint32_t   unique;         /* s_unique (coherent) */
    int        fmod;           /* s_fmod: superblock modified flag (dirty) */
    int        fl_dirty;       /* the free list has unflushed allocations: the
                                  superblock must be flushed before an inode that
                                  references a newly-allocated block (else a crash
                                  leaves the block both free and referenced) */
    /* V8-family bitmap state (heap-backed; NULL for free-list forms).  bit i is
     * set iff the block is free; v8_base maps bit 0 to its block number
     * (s_isize for the in-superblock bitmap, 0 for the out-of-superblock one). */
    uint8_t  *v8_bits;
    uint32_t  v8_nbits;
    uint32_t  v8_base;
    uint32_t  v8_nblks;        /* on-disk bitmap blocks (0 = in-superblock) */
    uint32_t  v8_blk_start;    /* first on-disk bitmap block (out-of-superblock) */
} filsys_edition_t;

/* The descriptor for an edition: a copy of the V7 default with the edition's
 * overrides.  Returns a zeroed descriptor (ops == NULL) for an unknown
 * edition. */
filsys_edition_t filsys_getformat(int edition);

/* Resolve a magic-bearing edition's byte order from its on-disk magic word
 * (and, if given, check it against `arch`).  Sets fmt->bo; returns 0 or a
 * negative errno with *errmsg (if non-NULL) set to a static description.
 * `force` proceeds despite a magic that matches neither byte order. */
int filsys_resolve_byteorder(filsys_edition_t *fmt, const char *path,
                             uint64_t offset, const char *arch, int force,
                             const char **errmsg);

/* ---- lifecycle --------------------------------------------------------- */

/* Open a disk image.  Returns 0, or -errno.  proto is the format descriptor
 * returned by filsys_getformat() (byte order, block size, free-cache width,
 * superblock packing); v7fs_open copies it into *fs and fills the runtime
 * fields.  offset is the byte offset of the filesystem within the file (0 =
 * block 0 of the file). */
int v7fs_open(filsys_edition_t *fs, const char *path, int readonly,
              const filsys_edition_t *proto, uint64_t offset);
/* Flush the superblock and close. */
int v7fs_close(filsys_edition_t *fs);
/* Flush the superblock (and pending metadata) to the image without closing. */
int v7fs_sync(filsys_edition_t *fs);
/* Mark the filesystem dirty (s_fmod) and flush: a read-write mount is dirty
 * until a clean close clears it, so a crash leaves the image flagged for fsck. */
int v7fs_mark_dirty(filsys_edition_t *fs);

/* V8-family superblock codec (Eighth/Ninth/Tenth Edition, docs/impl-v10fs.md
 * §5.8).  The on-disk superblock is rearranged relative to V7's, so these
 * decode/encode it into/from the shared in-core fields.  Wired into the
 * descriptor's sb_decode/sb_encode slots; v8_sb_decode returns 0, or -EINVAL if
 * the free-list/inode cache counts exceed their depths (mis-decoded block 1). */
int v8_sb_decode(filsys_edition_t *fs, const uint8_t *sb);
int v8_sb_encode(filsys_edition_t *fs, uint8_t *sb);
/* Load the V8-family free-space bitmap into fs->v8_bits (bit i set = free). */
int v8_bitmap_load(filsys_edition_t *fs, const uint8_t *sb);

/* Apply the V8-family `-o` geometry overrides (blocksize/freemap/byteorder) to a
 * descriptor, enforcing the one-bit-selects-both rule the kernels honour.  ed is
 * the FILSYS_* selector.  Returns 0, or -EINVAL with *errmsg set. */
int filsys_apply_geom(filsys_edition_t *fmt, int ed, const filsys_geom_t *g,
                      const char **errmsg);

/* ---- block / inode io -------------------------------------------------- */

int v7fs_read_block(filsys_edition_t *fs, uint32_t bno, uint8_t *buf);
int v7fs_write_block(filsys_edition_t *fs, uint32_t bno, const uint8_t *buf);

/* Block-size-dependent quantities: the logical block size is fs->bsize, not the
 * compile-time V7_BSIZE.  The per-indirect-block entry count lives in
 * fs->nindir (it is not always bsize/daddr_wid: PDP-7's container block is 256
 * bytes of 4-byte words against a 128-byte logical block). */
static inline uint32_t v7_nindir(const filsys_edition_t *fs) { return fs->nindir; }
static inline uint32_t v7_inopb(const filsys_edition_t *fs)  { return fs->bsize / fs->inode_size; }
/* itod / itoo: inode number -> block and offset. */
static inline uint32_t v7_itod(const filsys_edition_t *fs, uint32_t ino) { return 2 + (ino - 1) / v7_inopb(fs); }
static inline uint32_t v7_itoo(const filsys_edition_t *fs, uint32_t ino) { return (ino - 1) % v7_inopb(fs); }

/* First data block and last inode: V6's s_isize counts i-list blocks (data
 * starts at isize+2, max inode = isize * inodes/block); V7's s_isize is itself
 * the first data block (max inode = (isize-2) * inodes/block). */
static inline uint32_t v7_data_first(const filsys_edition_t *fs) {
    return fs->isize + (fs->isize_count ? 2u : 0u);
}
static inline uint32_t v7_maxinode(const filsys_edition_t *fs) {
    return (uint32_t)(fs->isize - (fs->isize_count ? 0u : 2u)) * v7_inopb(fs);
}

/* An on-disk block address in a free-list or indirect block: 2-byte LE for V6,
 * 4-byte in the descriptor's byte order otherwise. */
static inline uint32_t v7_get_daddr(const filsys_edition_t *fs, const uint8_t *p) {
    return fs->daddr_wid == 2 ? (uint32_t)bo_get16le(p) : fs->bo->get32(p);
}
static inline void v7_put_daddr(const filsys_edition_t *fs, uint8_t *p, uint32_t v) {
    if (fs->daddr_wid == 2)
        bo_put16le(p, v & 0xFFFFu);
    else
        fs->bo->put32(p, v);
}

/* The offset of df_free[] within a free-list *chain* block: after df_nfree
 * (4 bytes in the V8-family, 2 elsewhere).  V7/32V's 2-byte df_nfree is padded
 * to 4 on 32V, which fb_free_off(pack4) encodes. */
static inline int v7_chain_free_off(const filsys_edition_t *fs) {
    return fs->df_nfree_wid == 4 ? 4 : fb_free_off(fs->pack4);
}

/* The default indirect-entry codec: 2-byte LE (V6/V1) or 4-byte in the
 * descriptor's byte order (V7/2.11BSD), indexed by entry.  PDP-7 overrides the
 * descriptor's ind_get/ind_put with its 18-bit-word codec. */
static inline uint32_t v7_ind_get(const filsys_edition_t *fs, const uint8_t *buf, uint32_t i) {
    return fs->daddr_wid == 2 ? (uint32_t)bo_get16le(buf + 2 * i) : fs->bo->get32(buf + 4 * i);
}
static inline void v7_ind_put(const filsys_edition_t *fs, uint8_t *buf, uint32_t i, uint32_t v) {
    if (fs->daddr_wid == 2)
        bo_put16le(buf + 2 * i, v & 0xFFFFu);
    else
        fs->bo->put32(buf + 4 * i, v);
}

/* The level of address slot `slot` for an inode whose mode is `mode`:
 * -1 = direct, 0 = single indirect, 1 = double, 2 = triple.  The V7-family
 * layout is "ndaddr direct slots then levels 0,1,2 in order"; the V6/V1/PDP-7
 * layouts reinterpret the slots under the ILARG mode bit (large_single + one
 * optional large_double slot), all of which this describes as data. */
static inline int filsys_slot_level(const filsys_edition_t *fs, uint32_t mode, int slot) {
    if (fs->ilarg_mask && (mode & fs->ilarg_mask)) {
        if (slot < (int)fs->large_single)
            return 0;
        if (slot < (int)(fs->large_single + fs->large_double))
            return 1;
        return -1;   /* no slot beyond the large layout */
    }
    if (slot < (int)fs->ndaddr)
        return -1;
    return slot - (int)fs->ndaddr;   /* 0, 1, 2, ... */
}

/* Directory test: V1/PDP-7 carry an is_dir callback (their type bits don't fit
 * the V6/V7 ifmt/ifdir pair); the V6/V7 family derives it from ifmt. */
static inline int fs_is_dir(const filsys_edition_t *fs, const filsys_inode_t *ip) {
    if (fs->is_dir)
        return fs->is_dir(fs, ip);
    return (ip->mode & fs->ifmt) == fs->ifdir;
}

int v7fs_read_inode(filsys_edition_t *fs, uint32_t ino, v7_inode_t *ip);
int v7fs_write_inode(filsys_edition_t *fs, uint32_t ino, const v7_inode_t *ip);

/* ---- allocation -------------------------------------------------------- */

/* Allocate a free data block into *bno (0 = absent/sparse). */
int v7fs_balloc(filsys_edition_t *fs, uint32_t *bno);
void v7fs_bfree(filsys_edition_t *fs, uint32_t bno);
/* Allocate a free inode into *ino. */
int v7fs_ialloc(filsys_edition_t *fs, uint32_t *ino);
void v7fs_ifree(filsys_edition_t *fs, uint32_t ino);

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

typedef filsys_check_t v7_check_t;

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
/* V6's check: its isize semantics (s_isize = i-list block count), ILARG bmap
 * and 32-byte inode make the codec V6-specific, but it operates on the shared
 * filsys_edition_t. */
int v6_check(filsys_edition_t *fs, v7_check_t *rep, int mode);

/* ---- shared block-map walk + truncate (blocktree.c) ---------------------- */

/* Mark one data block as accounted-for (0 = first, 1 = bad, 2 = duplicate). */
int filsys_mark_block(filsys_edition_t *fs, filsys_chkctx_t *cx, uint32_t bno);
/* Mark an indirect block and everything beneath it (level 0 = single, 1 =
 * double, 2 = triple); a duplicate indirect block is not chased. */
void filsys_mark_tree(filsys_edition_t *fs, filsys_chkctx_t *cx, uint32_t blk, int level);
/* Mark every block referenced by an inode. */
void filsys_mark_blocks(filsys_edition_t *fs, const filsys_inode_t *ip, uint32_t ino,
                        filsys_chkctx_t *cx);

/* A deferred block-free list: filsys_itrunc/_from collect the blocks they
 * remove into one of these instead of freeing inline, so the caller can persist
 * the unlinked inode (write_inode) *before* any block returns to the free list. */
typedef struct filsys_blklist filsys_blklist_t;
filsys_blklist_t *filsys_blklist_new(void);
void filsys_blklist_drain(filsys_edition_t *fs, filsys_blklist_t *b);   /* bfree all + free list */
void filsys_blklist_discard(filsys_blklist_t *b);                        /* free list, keep blocks */

/* Collect every block of an inode (truncate to length 0) into *b. */
int filsys_itrunc(filsys_edition_t *fs, filsys_inode_t *ip, filsys_blklist_t *b);
/* Collect blocks [first_blk, ...) only; first_blk == 0 == filsys_itrunc. */
int filsys_itrunc_from(filsys_edition_t *fs, filsys_inode_t *ip, uint32_t first_blk,
                       filsys_blklist_t *b);

/* ---- shared maintenance (check.c): ncheck / clri / preen / resolve-dups -- */

int filsys_ncheck(filsys_edition_t *fs, uint32_t ino);
int filsys_clri(filsys_edition_t *fs, uint32_t ino);
void filsys_preen(filsys_edition_t *fs, const uint8_t *ecount, const uint8_t *state,
                  uint32_t maxino, int mode);
int filsys_resolve_dups(filsys_edition_t *fs);

#endif /* V7FS_H */
