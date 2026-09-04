/* v1fs.h - First Edition (V1) Unix filesystem, on-disk access layer.
 *
 * The V1 filesystem is byte-addressed with 512-byte blocks.  Unlike V6/V7 its
 * allocator is a pair of bitmaps in the superblock (a free-block map and an
 * inode map), and directory entries are 10 bytes (2-byte i-number + 8-char
 * name).  Device files are identified by inode number (< 41), not by mode bits,
 * and the permission model is a two-class owner/non-owner read/write set rather
 * than V6's rwx-rwx-rwx.  See docs/v1-format.md for the verified layout.
 *
 * SPDX-License-Identifier: ISC
 */
#ifndef V1FS_H
#define V1FS_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#include "filsys.h"

enum {
    V1_BSIZE    = 512,
    V1_INOPB    = 16,          /* inodes per block */
    V1_INODESZ  = 32,          /* sizeof(struct inode on disk) */
    V1_ROOTINO  = 41,
    V1_SUPERB   = 1,           /* superblock spans blocks 0 and 1 */
    V1_NIADDR   = 8,           /* address slots per inode */
    V1_NDADDR   = 8,
    V1_NINDIR   = V1_BSIZE / 2, /* 2-byte addresses per indirect block */
    V1_DIRSIZ   = 8,           /* chars per directory entry name */
    V1_DIRENTSZ = 2 + V1_DIRSIZ /* 10 bytes per on-disk entry */
};

/* i_mode flag bits (sys filesys.5).  No gid, no sticky bit, and no explicit
 * "regular file" type (regular = the directory bit clear). */
enum {
    V1_IALLOC = 0100000,       /* allocated */
    V1_IFDIR  = 0040000,       /* directory */
    V1_ILARG  = 0010000,       /* large file (all 8 slots are indirect) */
    V1_ISUID  = 0000040,       /* set user id on execution */
    V1_IEXEC  = 0000020,       /* execute */
    V1_IREAD  = 0000010,       /* read, owner */
    V1_IWRITE = 0000004,       /* write, owner */
    V1_OREAD  = 0000002,       /* read, non-owner */
    V1_OWRITE = 0000001        /* write, non-owner */
};

/* ---- byte-order primitives --------------------------------------------- */

static inline uint16_t v1_get16le(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline void v1_put16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}
/* 32-bit middle-endian (PDP-11): high word first, each word little-endian. */
static inline uint32_t v1_get32me(const uint8_t *p) {
    uint16_t hi = v1_get16le(p);
    uint16_t lo = v1_get16le(p + 2);
    return ((uint32_t)hi << 16) | lo;
}
static inline void v1_put32me(uint8_t *p, uint32_t v) {
    v1_put16le(p,     (uint16_t)(v >> 16));
    v1_put16le(p + 2, (uint16_t)(v & 0xffff));
}

/* ---- core types -------------------------------------------------------- */

/* Decoded inode/dirent are the public filsys types (no per-backend copy, so
 * the ops table needs no type-punning cast). */
typedef filsys_inode_t  v1_inode_t;
typedef filsys_dirent_t v1_dirent_t;

typedef struct {
    int        fd;
    int        readonly;
    uint64_t   base;
    uint32_t   fsize;          /* total blocks = free-map bytes * 8 */
    uint32_t   maxino;         /* total inode slots = inode-map bytes * 8 */
    uint16_t   freemap_bytes;
    uint16_t   inodemap_bytes;
    uint8_t   *freemap;        /* in-core free-block bitmap (bit=1 free) */
    uint8_t   *inodemap;       /* in-core inode bitmap (bit=0 free, inode 41+) */
    uint32_t   tfree;          /* total free blocks */
    uint32_t   tinode;         /* total free inodes */
} v1fs_t;

/* ---- lifecycle --------------------------------------------------------- */

int v1fs_open(v1fs_t *fs, const char *path, int readonly, uint64_t offset);
void v1fs_close(v1fs_t *fs);
int v1fs_sync(v1fs_t *fs);

/* ---- block / inode io -------------------------------------------------- */

int v1fs_read_block(v1fs_t *fs, uint32_t bno, uint8_t *buf);
int v1fs_write_block(v1fs_t *fs, uint32_t bno, const uint8_t *buf);

static inline uint32_t v1_itod(uint32_t ino) { return (ino + 31) >> 4; }
static inline uint32_t v1_itoo(uint32_t ino) { return (ino + 31) & 15; }

/* First data block: the i-list occupies blocks 2..(maxino+31)/16. */
static inline uint32_t v1_data_start(uint32_t maxino) { return (maxino + 31) / 16 + 1; }

int v1fs_read_inode(v1fs_t *fs, uint32_t ino, v1_inode_t *ip);
int v1fs_write_inode(v1fs_t *fs, uint32_t ino, const v1_inode_t *ip);

/* ---- allocation (bitmap) ------------------------------------------------ */

int v1fs_balloc(v1fs_t *fs, uint32_t *bno);
void v1fs_bfree(v1fs_t *fs, uint32_t bno);
int v1fs_ialloc(v1fs_t *fs, uint32_t *ino);
void v1fs_ifree(v1fs_t *fs, uint32_t ino);
int v1fs_itrunc(v1fs_t *fs, v1_inode_t *ip);
int v1fs_itrunc_from(v1fs_t *fs, v1_inode_t *ip, uint32_t first_blk);

/* ---- file / directory data --------------------------------------------- */

int v1fs_bmap(v1fs_t *fs, v1_inode_t *ip, uint32_t lbn, int create, uint32_t *bno);

ssize_t v1fs_file_read(v1fs_t *fs, v1_inode_t *ip, uint8_t *buf, size_t size, off_t off);
ssize_t v1fs_file_write(v1fs_t *fs, v1_inode_t *ip, const uint8_t *buf, size_t size, off_t off);

int v1fs_dir_read(v1fs_t *fs, v1_inode_t *ip, v1_dirent_t **ents, size_t *count);
void v1fs_dirents_free(v1_dirent_t *ents);
int v1fs_dir_lookup(v1fs_t *fs, v1_inode_t *ip, const char *name, uint32_t *ino);
int v1fs_dir_add(v1fs_t *fs, v1_inode_t *ip, uint32_t ino, const char *name);
int v1fs_dir_remove(v1fs_t *fs, v1_inode_t *ip, const char *name);

int v1fs_lookup(v1fs_t *fs, const char *path, uint32_t *ino, v1_inode_t *ip);

/* ---- integrity check ---------------------------------------------------- */

typedef struct {
    uint32_t free_blocks;
    uint32_t inodes;
    uint32_t used_inodes;
    uint32_t errors;
} v1_check_t;

int v1fs_check(v1fs_t *fs, v1_check_t *rep);

/* Print the full pathname(s) of inode `ino` (ncheck).  Returns 0. */
int v1fs_ncheck(v1fs_t *fs, uint32_t ino);
/* Zero inode `ino` (clri).  Returns 0 or -errno. */
int v1fs_clri(v1fs_t *fs, uint32_t ino);

#endif /* V1FS_H */
