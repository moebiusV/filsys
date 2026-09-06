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
    uint32_t errors;         /* number of integrity problems found */
} filsys_check_t;

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
