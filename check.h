/* check.h - types shared by the integrity-check driver and every backend.
 *
 * The report (filsys_check_t) and the block-usage context (filsys_chkctx_t)
 * live here, below the per-format headers, so v7_check_t / v1_check_t /
 * p7_check_t can alias the one report type instead of re-declaring it.
 *
 * SPDX-License-Identifier: ISC
 */
#ifndef CHECK_H
#define CHECK_H

#include <stdint.h>

/* Unified fsck report: v7_check_t / v1_check_t / p7_check_t merged. */
typedef struct {
    uint32_t free_blocks;    /* free blocks found by walking the allocator */
    uint32_t used_blocks;    /* data blocks referenced by inodes */
    uint32_t missing_blocks; /* blocks in the data area referenced by neither */
    uint32_t dup_blocks;     /* blocks referenced twice, or used + free */
    uint32_t inodes;         /* total inode slots */
    uint32_t used_inodes;    /* inodes with a non-zero mode */
    uint32_t orphaned;       /* allocated non-directory inodes with no dir entry */
    uint32_t errors;         /* number of integrity problems found */
} filsys_check_t;

/* The failure class of a check report, from cleanest to most severe.  The
 * integrity contract is "never D": a block claimed twice (aliasing).  B is a
 * leak (a missing block, or an allocated-but-unreferenced inode -- the state
 * hard_remove's deferred free leaves on purpose, so it is a leak class, not an
 * error class); C is a recoverable inconsistency (link counts, superblock
 * totals); A is clean. */
typedef enum {
    FILSYS_CK_A = 0,   /* clean */
    FILSYS_CK_C,       /* recoverable inconsistency */
    FILSYS_CK_B,       /* recoverable leak (missing block / orphaned inode) */
    FILSYS_CK_D,       /* corruption: a block claimed twice (aliasing) */
} filsys_fail_class_t;

/* Classify a check report by its worst finding.  The severity order (A < C < B
 * < D) matches the enum, so "class > whitelist" reads as "worse than allowed". */
static inline filsys_fail_class_t filsys_classify(const filsys_check_t *rep) {
    if (rep->dup_blocks > 0)
        return FILSYS_CK_D;
    if (rep->missing_blocks > 0 || rep->orphaned > 0)
        return FILSYS_CK_B;
    if (rep->errors > 0)
        return FILSYS_CK_C;
    return FILSYS_CK_A;
}

/* The allocator vtable: the free-list cache (V6/V7/BSD211) and V1's dual
 * bitmap are two implementations.  `fs` is the backend state (filsys_edition_t
 * for the free-list editions, v1fs_t for V1); the allocator reads its state
 * from the embedded freelist_state / bitmap_state.  Every backend's state is
 * the per-mount filsys_edition_t (v1fs_t and p7fs_t alias it), which embeds the
 * shared filsys_desc_t descriptor, so the vtable types it directly -- not
 * `void *` -- which keeps the calls compatible with each implementation (no
 * function-pointer cast, which UBSan flags). */
struct filsys_edition;
typedef struct {
    int  (*balloc)(struct filsys_edition *fs, uint32_t *bno);
    void (*bfree)(struct filsys_edition *fs, uint32_t bno);
    int  (*ialloc)(struct filsys_edition *fs, uint32_t *ino);
    void (*ifree)(struct filsys_edition *fs, uint32_t ino);
    int  (*sync)(struct filsys_edition *fs);          /* flush the allocator state */
} alloc_ops_t;

/* V1's dual-bitmap allocator state (the other allocator beside freelist_state). */
typedef struct {
    uint16_t   freemap_bytes;
    uint16_t   inodemap_bytes;
    uint8_t   *freemap;        /* in-core free-block bitmap (bit=1 free) */
    uint8_t   *inodemap;       /* in-core inode bitmap (bit=0 free, inode 41+) */
    uint32_t   tfree;          /* total free blocks */
    uint32_t   tinode;         /* total free inodes */
} bitmap_state;

/* Block-usage context for the shared check driver (icheck's bitmaps + counters).
 * The backend state is passed to each op separately; only the format-neutral
 * accounting lives here, so one context serves every edition. */
typedef struct {
    uint8_t  *bmap;          /* bit i = data block (data_start + i) */
    uint32_t *owner;         /* owner[i] = first inode to claim data block i */
    uint32_t  nblk;          /* data blocks (data_end - data_start) */
    uint32_t  used_blocks;
    uint32_t  dup_blocks;
    uint32_t  bad_blocks;    /* out-of-range block references */
    uint32_t  errors;
    uint32_t  ino;           /* current inode, for messages */
} filsys_chkctx_t;

#endif /* CHECK_H */
