/* bsd211fs.h - 2.11BSD filesystem, on-disk access layer.
 *
 * 2.11BSD keeps V7's free-list superblock but swaps in the "new" BSD inode:
 * 32-bit block addresses, 7 address slots (4 direct + 3 indirect), a di_flags
 * field, and live symbolic links.  Directory entries are variable-length with
 * up to 63-char names.  Block size is 1024 bytes; byte order is the PDP-11's
 * middle-endian (16-bit little-endian, 32-bit most-significant word first).
 *
 * The format is documented in docs/2bsd-format.md; this file mirrors the
 * original 2.11BSD headers (h/{fs.h,inode.h,dir.h,types.h}, machine/machparam.h).
 *
 * SPDX-License-Identifier: ISC
 */
#ifndef BSD211FS_H
#define BSD211FS_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#include "filsys.h"

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

/* di_mode type/mode bits (h/inode.h).  Note IFLNK (symlink) and IFSOCK. */
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

/* ---- byte-order primitives (PDP-11 middle-endian) ------------------------ */

static inline uint16_t bsd211_get16le(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline void bsd211_put16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}
/* 32-bit middle-endian: high word first, each word little-endian. */
static inline uint32_t bsd211_get32me(const uint8_t *p) {
    return ((uint32_t)bsd211_get16le(p) << 16) | bsd211_get16le(p + 2);
}
static inline void bsd211_put32me(uint8_t *p, uint32_t v) {
    bsd211_put16le(p,     (uint16_t)(v >> 16));
    bsd211_put16le(p + 2, (uint16_t)(v & 0xffff));
}

/* ---- superblock field offsets (struct fs, packed, block 1) --------------- */

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

/* ---- core types ---------------------------------------------------------- */

/* Decoded inode/dirent are the public filsys types. */
typedef filsys_inode_t  bsd211_inode_t;
typedef filsys_dirent_t bsd211_dirent_t;

typedef struct {
    int        fd;
    int        readonly;
    uint64_t   base;          /* byte offset of this filesystem within the file */
    /* in-core superblock */
    uint16_t   isize;
    uint32_t   fsize;
    uint16_t   nfree;
    uint32_t   free[BSD211_NICFREE];
    uint16_t   ninode;
    uint16_t   inode[BSD211_NICINOD];
    uint32_t   time;
    uint32_t   tfree;
    uint16_t   tinode;
    int        fmod;
} bsd211fs_t;

/* ---- lifecycle ----------------------------------------------------------- */

int bsd211fs_open(bsd211fs_t *fs, const char *path, int readonly, uint64_t offset);
void bsd211fs_close(bsd211fs_t *fs);
int bsd211fs_sync(bsd211fs_t *fs);
int bsd211fs_mark_dirty(bsd211fs_t *fs);

/* ---- block / inode io ---------------------------------------------------- */

int bsd211fs_read_block(bsd211fs_t *fs, uint32_t bno, uint8_t *buf);
int bsd211fs_write_block(bsd211fs_t *fs, uint32_t bno, const uint8_t *buf);

/* itod / itoo: inode number -> block and offset (16 inodes per block). */
static inline uint32_t bsd211_itod(uint32_t ino) { return 2 + (ino - 1) / BSD211_INOPB; }
static inline uint32_t bsd211_itoo(uint32_t ino) { return (ino - 1) % BSD211_INOPB; }

int bsd211fs_read_inode(bsd211fs_t *fs, uint32_t ino, bsd211_inode_t *ip);
int bsd211fs_write_inode(bsd211fs_t *fs, uint32_t ino, const bsd211_inode_t *ip);

/* ---- allocation ---------------------------------------------------------- */

int bsd211fs_balloc(bsd211fs_t *fs, uint32_t *bno);
void bsd211fs_bfree(bsd211fs_t *fs, uint32_t bno);
int bsd211fs_ialloc(bsd211fs_t *fs, uint32_t *ino);
void bsd211fs_ifree(bsd211fs_t *fs, uint32_t ino);
int bsd211fs_itrunc(bsd211fs_t *fs, bsd211_inode_t *ip);
int bsd211fs_itrunc_from(bsd211fs_t *fs, bsd211_inode_t *ip, uint32_t first_blk);

/* ---- file / directory data ----------------------------------------------- */

int bsd211fs_bmap(bsd211fs_t *fs, bsd211_inode_t *ip, uint32_t lbn, int create, uint32_t *bno);
ssize_t bsd211fs_file_read(bsd211fs_t *fs, bsd211_inode_t *ip, uint8_t *buf, size_t size, off_t off);
ssize_t bsd211fs_file_write(bsd211fs_t *fs, bsd211_inode_t *ip, const uint8_t *buf, size_t size, off_t off);

int bsd211fs_dir_read(bsd211fs_t *fs, bsd211_inode_t *ip, bsd211_dirent_t **ents, size_t *count);
void bsd211fs_dirents_free(bsd211_dirent_t *ents);
int bsd211fs_dir_lookup(bsd211fs_t *fs, bsd211_inode_t *ip, const char *name, uint32_t *ino);
int bsd211fs_dir_add(bsd211fs_t *fs, bsd211_inode_t *ip, uint32_t ino, const char *name);
int bsd211fs_dir_remove(bsd211fs_t *fs, bsd211_inode_t *ip, const char *name);

int bsd211fs_lookup(bsd211fs_t *fs, const char *path, uint32_t *ino, bsd211_inode_t *ip);

/* ---- integrity check ------------------------------------------------------ */

typedef struct {
    uint32_t free_blocks;
    uint32_t used_blocks;
    uint32_t missing_blocks;
    uint32_t dup_blocks;
    uint32_t inodes;
    uint32_t used_inodes;
    uint32_t errors;
} bsd211_check_t;

int bsd211fs_check(bsd211fs_t *fs, bsd211_check_t *rep, int mode);

#endif /* BSD211FS_H */
