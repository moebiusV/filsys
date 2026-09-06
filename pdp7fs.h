/* pdp7fs.h - PDP-7 Unix filesystem, on-disk access layer.
 *
 * The PDP-7 filesystem (the first Unix filesystem, Bell Labs 1969) is
 * word-addressed: 18-bit words, 64-word blocks.  It predates the byte-oriented
 * V1 layout entirely -- no timestamps, no group or execute permission, a link
 * count stored negative, an 8-word directory entry holding two 7-bit ASCII
 * characters per 18-bit word (packed in the low bits of each 9-bit half), and
 * a "dd" (root) directory instead of "/".  The layout is spelled
 * out in pdp7-unix's tools/mkfs7 (and fsck7) as readable constants; this file
 * mirrors them and the McIlroy sysmap (i.flags/i.dskps/i.uid/i.nlks/i.size/
 * i.uniq, d.i/d.name/d.uniq).
 *
 * SPDX-License-Identifier: ISC
 */
#ifndef PDP7FS_H
#define PDP7FS_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#include "filsys.h"
#include "v7fs.h"       /* filsys_edition_t (p7fs_t aliases it) */
#include "check.h"

enum {
    P7_WSIZE      = 64,    /* 18-bit words per block */
    P7_WORDBYTES  = 4,     /* SimH RB09 image: one word per 4-byte LE slot */
    P7_BLOCKBYTES = P7_WSIZE * P7_WORDBYTES,          /* 256 bytes/block */
    P7_NBLOCKS    = 8000,  /* blocks per RB09 surface */
    P7_SURFACE1   = P7_NBLOCKS * P7_BLOCKBYTES,       /* 2048000: fs surface */
    P7_INODESZ    = 12,    /* words per inode */
    P7_INOPB      = P7_WSIZE / P7_INODESZ,            /* 5 inodes/block */
    P7_NIADDR     = 7,     /* block pointers per inode */
    P7_NINDIR     = P7_WSIZE,                          /* words/indirect block */
    P7_DIRENTSZ   = 8,     /* words per directory entry */
    P7_DIRSIZ     = 8,     /* chars per name (2/word x 4 words) */
    P7_FIRSTINOBLK = 2,    /* first inode block (mkfs7) */
    P7_NINOBLKS   = 710,   /* inode blocks (mkfs7; s9.s zeroes 0..709) */
    P7_DATASTART  = P7_FIRSTINOBLK + P7_NINOBLKS,      /* first data block (712) */
    P7_KDATA      = 6400,  /* kernel area starts here (reserved, not freeable) */
    P7_MAXINO     = P7_NINOBLKS * P7_INOPB - 1,        /* 3549; inode 0 unused */
    P7_ROOTINO    = 4,     /* the "dd" (root) directory */
    P7_MAXWORD    = 0777777 /* 18-bit mask */
};

/* i.flags bits (18-bit; the low six are type + permissions). */
enum {
    P7_IUSED   = 0400000,   /* allocated */
    P7_ILARG   = 0200000,   /* large file: all 7 pointers are indirect */
    P7_ISPEC   = 0000040,   /* special (device) file */
    P7_IDIR    = 0000020,   /* directory */
    P7_IOREAD  = 0000010,   /* owner read */
    P7_IOWRITE = 0000004,   /* owner write */
    P7_IWREAD  = 0000002,   /* world read */
    P7_IWWRITE = 0000001    /* world write */
};

/* SimH RB09 word packing: an 18-bit word in a 4-byte little-endian slot
 * (bo_get32le / bo_put32le). */

/* itod / itoo: inode number -> block and word offset (5 inodes per block). */
static inline uint32_t p7_itod(uint32_t ino) { return P7_FIRSTINOBLK + ino / P7_INOPB; }
static inline uint32_t p7_itoo(uint32_t ino) { return P7_INODESZ * (ino % P7_INOPB); }

/* Decoded inode/dirent are the public filsys types. */
typedef filsys_inode_t  p7_inode_t;
typedef filsys_dirent_t p7_dirent_t;

/* PDP-7's backend state is the shared filsys_edition_t: the free-list head
 * lives in .freelist and the free-block count in .fl.tfree, so the file/dir
 * layer above the word-addressed codecs is the shared V7 engine's. */
typedef filsys_edition_t p7fs_t;

/* ---- lifecycle --------------------------------------------------------- */

int p7fs_open(p7fs_t *fs, const char *path, int readonly,
              const filsys_edition_t *proto, uint64_t offset);
void p7fs_close(p7fs_t *fs);
int p7fs_sync(p7fs_t *fs);

/* ---- block / inode io -------------------------------------------------- */

int p7fs_read_block(p7fs_t *fs, uint32_t bno, uint8_t *buf);
int p7fs_write_block(p7fs_t *fs, uint32_t bno, const uint8_t *buf);

int p7fs_read_inode(p7fs_t *fs, uint32_t ino, p7_inode_t *ip);
int p7fs_write_inode(p7fs_t *fs, uint32_t ino, const p7_inode_t *ip);

/* ---- allocation (free list) -------------------------------------------- */

int p7fs_balloc(p7fs_t *fs, uint32_t *bno);
void p7fs_bfree(p7fs_t *fs, uint32_t bno);
int p7fs_ialloc(p7fs_t *fs, uint32_t *ino);
void p7fs_ifree(p7fs_t *fs, uint32_t ino);

/* The allocator vtable wrapping the four functions above (the shared block-map
 * walk calls fs->alloc->bfree/balloc, so PDP-7 needs a row too). */
extern const alloc_ops_t pdp7_alloc_ops;

/* PDP-7's indirect-entry codec (the descriptor's ind_get/ind_put override): an
 * 18-bit word packed in a 4-byte little-endian slot. */
uint32_t p7_ind_get(const filsys_edition_t *fs, const uint8_t *buf, uint32_t i);
void     p7_ind_put(const filsys_edition_t *fs, uint8_t *buf, uint32_t i, uint32_t v);

/* ---- block mapping / directories ---------------------------------------- */

int p7fs_bmap(p7fs_t *fs, p7_inode_t *ip, uint32_t lbn, int create, uint32_t *bno);

/* File data and path lookup are shared with the V7 engine (v7fs_file_read/
 * write, v7fs_lookup, v7fs_dir_lookup); only the word-addressed dirent codec
 * (dir_read/add/remove) and the single-indirect bmap topology stay PDP-7. */
int p7fs_dir_read(p7fs_t *fs, p7_inode_t *ip, p7_dirent_t **ents, size_t *count);
void p7fs_dirents_free(p7_dirent_t *ents);
int p7fs_dir_add(p7fs_t *fs, p7_inode_t *ip, uint32_t ino, const char *name);
int p7fs_dir_remove(p7fs_t *fs, p7_inode_t *ip, const char *name);

/* ---- integrity check ---------------------------------------------------- */

typedef filsys_check_t p7_check_t;

int p7fs_check(p7fs_t *fs, p7_check_t *rep, int mode);

#endif /* PDP7FS_H */
