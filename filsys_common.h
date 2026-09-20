/* Copyright (C) 2026 David Walther */
/* SPDX-License-Identifier: ISC */
/* filsys_common.h - the version-agnostic engine types: byte transport, probe
 * result, format descriptor and per-mount runtime state, shared by every
 * on-disk format backend (V1 through V10, PDP-7, BSD, System V).
 *
 * This is the "one authoritative descriptor" half of the split: the descriptor
 * and the state that embeds it live here, below the V7-family disk-layout
 * constants (which stay in v7fs.h).  A backend header (v7fs.h, v1fs.h,
 * pdp7fs.h) includes this and adds its own on-disk layout.
 */
#ifndef FILSYS_COMMON_H
#define FILSYS_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <errno.h>

#include "filsys_engine.h"  /* on-disk types: filsys_inode_t, filsys_dirent_t, ... */
#include "byteorder.h"    /* byte_order_ops_t, bo_* */
#include "check.h"        /* filsys_check_t, alloc_ops_t, bitmap_state, ... */

struct filsys_ops;       /* forward: the per-backend vtable (see filsys_ops.h) */
struct word_codec;       /* forward: PDP-7's 18-bit-word container codec (pdp7fs.h) */
struct filsys_edition;   /* forward: the per-mount state (defined below) */

/* Engine-wide bounds the in-core types are sized by.  The names keep the V7/V8
 * prefix they inherited (they were the largest member of each format's cache
 * depth); here they are the in-core maxima the union-sized freelist_state is
 * built from, not a per-format on-disk value. */
enum {
    V7_MAXBSIZE      = 8192,  /* largest block size the engine supports (512..8192; V9's 8K) */
    V7_NICINOD       = 100,   /* free-inode cache depth (V7 family) */
    V8_NICFREE_LARGE = 946,   /* deepest free-block cache (V9's 8K superblock) */
};

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

/* Result of a read-only superblock probe (the edition vtable's `probe` method).
 * `class` is the human-readable equivalence class the probe matched ("v7, sysiii
 * or sysvr1", "v8 or v10, bs=1024", ...); the resolver (filsys_detect) maps it
 * to a FILSYS_* edition + geometry.  isize/fsize/segs/why feed findfs's report:
 * why is set on a near-miss, NULL on a validated hit. */
typedef struct {
    const char *class;      /* equivalence class, NULL = not this format */
    uint32_t    blocksize;  /* detected block size */
    int         freemap;    /* FILSYS_FREEMAP_* or -1 (V8-family only) */
    const char *byteorder;  /* "le"/"be", or NULL = edition default */
    const char *packing;    /* PDP-7 word container codec name, or NULL */
    const char *note;       /* prose geometry note (findfs report), or NULL */
    int         bitmap;     /* 1 = free space is a bitmap (no chain), 0 = free list */
    uint16_t    isize;      /* i-list size in blocks (findfs report) */
    uint32_t    fsize;      /* volume size in blocks (findfs report) */
    uint32_t    segs;       /* free-list segments followed (findfs report) */
    const char *why;        /* near-miss rejection reason, or NULL */
    uint64_t    base;       /* matched fs-start byte (PDP-7 surface; 0 elsewhere) */
    filsys_probe_conf_t conf; /* how confident this match is (MAGIC/STRUCTURAL/...) */
} filsys_probe_t;

/* Decoded inode/dirent are the public filsys types (no per-backend copy, so
 * the ops table needs no type-punning cast).  The "v7_" prefix is historical:
 * every backend decodes into these. */
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

typedef struct filsys_desc {
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
    uint8_t     has_dinfo;          /* s_dinfo[4] sits before s_tfree (System III/V) */
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
    uint32_t    iallocated;         /* mode bit every live inode carries (0 = none).
                                     * V6's IALLOC: a created inode without it reads
                                     * back as free.  V1/PDP-7 set theirs inside their
                                     * own to_disk_mode. */
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
} filsys_desc_t;

typedef struct filsys_edition {
    filsys_desc_t desc;   /* the static format descriptor (a mutable per-mount copy) */

    /* ---- runtime (filled by v7fs_open) ---- */
    int        fd;             /* open disk image */
    int        readonly;
    const filsys_io_t *io;     /* raw byte-slice transport (default: filsys_io_file) */
    uint64_t   base;           /* byte offset of this filesystem within the file */
    uint64_t   imgsize;        /* backing file size (0 = unknown: mkfs/detect) */
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

#endif /* FILSYS_COMMON_H */
