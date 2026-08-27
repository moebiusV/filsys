/* filsys 1.0.0 - 2026-08-26 - Copyright (C) 2026 David Walther */
/* SPDX-License-Identifier: ISC */
/* v6fs.h - Sixth Edition (V6) Unix filesystem, on-disk access layer.
 *
 * The V6 filesystem lives on a PDP-11 disk image.  Block size is 512 bytes:
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
#ifndef V6FS_H
#define V6FS_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#define V6_BSIZE    512
#define V6_INOPB    16         /* inodes per block */
#define V6_INODESZ  32         /* sizeof(struct inode on disk) */
#define V6_NICFREE  100        /* superblock free-block cache size */
#define V6_NICINOD  100        /* superblock free-inode cache size */
#define V6_ROOTINO  1
#define V6_SUPERB   1          /* block number of superblock */
#define V6_NIADDR   8          /* total address slots per inode */
#define V6_NDADDR   8          /* direct blocks in a small file */
#define V6_NINDIR   (V6_BSIZE / 2)  /* 2-byte addresses per indirect block */
#define V6_DIRSIZ   14         /* chars per directory entry name */

/* i_mode type/mode bits (sys/ino.h).  V6 has no IFREG (regular = type 0)
 * and no IFMPC/IFMPB; bit 010000 is the ILARG large-file flag. */
#define V6_IFMT   0060000
#define V6_IFCHR  0020000
#define V6_IFDIR  0040000
#define V6_IFBLK  0060000
#define V6_ILARG  0010000
#define V6_ISUID  0004000
#define V6_ISGID  0002000
#define V6_ISVTX  0001000
#define V6_IREAD  0000400
#define V6_IWRITE 0000200
#define V6_IEXEC  0000100

/* ---- byte-order primitives --------------------------------------------- */

static inline uint16_t v6_get16le(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline void v6_put16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}
/* 32-bit middle-endian: high word first, each word little-endian. */
static inline uint32_t v6_get32me(const uint8_t *p) {
    uint16_t hi = v6_get16le(p);
    uint16_t lo = v6_get16le(p + 2);
    return ((uint32_t)hi << 16) | lo;
}
static inline void v6_put32me(uint8_t *p, uint32_t v) {
    v6_put16le(p,     (uint16_t)(v >> 16));
    v6_put16le(p + 2, (uint16_t)(v & 0xffff));
}
/* 24-bit block number packed as [ hi, lo, mid ]. */
static inline uint32_t v6_get24me(const uint8_t *p) {
    return (uint32_t)p[1] | ((uint32_t)p[2] << 8) | ((uint32_t)p[0] << 16);
}
static inline void v6_put24me(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 16) & 0xff);  /* hi  */
    p[1] = (uint8_t)(v & 0xff);          /* lo  */
    p[2] = (uint8_t)((v >> 8) & 0xff);   /* mid */
}

/* ---- core types -------------------------------------------------------- */

/* Decoded on-disk inode. */
typedef struct {
    uint32_t ino;              /* inode number (0 = not yet bound) */
    uint16_t mode;
    int16_t  nlink;
    int16_t  uid;
    int16_t  gid;
    uint32_t size;
    uint32_t addr[13];         /* block numbers (V6 uses 8); 13 for V6 parity */
    uint32_t atime, mtime, ctime;
} v6_inode_t;

typedef struct {
    uint16_t ino;
    char     name[V6_DIRSIZ + 1];
} v6_dirent_t;

typedef struct {
    int        fd;             /* open disk image */
    int        readonly;
    uint64_t   base;           /* byte offset of this filesystem within the file */
    /* in-core superblock (kept in sync with block 1) */
    uint16_t   isize;
    uint32_t   fsize;
    uint16_t   nfree;
    uint32_t   free[V6_NICFREE];
    uint16_t   ninode;
    uint16_t   inode[V6_NICINOD];
    uint32_t   time;           /* last superblock update */
} v6fs_t;

/* ---- lifecycle --------------------------------------------------------- */

/* Open a disk image.  Returns 0, or -errno.  offset is the byte offset of the
 * filesystem within the file (0 = it starts at block 0 of the file). */
int v6fs_open(v6fs_t *fs, const char *path, int readonly, uint64_t offset);
/* Flush the superblock and close. */
void v6fs_close(v6fs_t *fs);

/* ---- block / inode io -------------------------------------------------- */

int v6fs_read_block(v6fs_t *fs, uint32_t bno, uint8_t *buf);
int v6fs_write_block(v6fs_t *fs, uint32_t bno, const uint8_t *buf);

/* itod / itoo: inode number -> block and offset (16 inodes per block). */
static inline uint32_t v6_itod(uint32_t ino) { return (ino + 31) >> 4; }
static inline uint32_t v6_itoo(uint32_t ino) { return (ino + 31) & 15; }

int v6fs_read_inode(v6fs_t *fs, uint32_t ino, v6_inode_t *ip);
int v6fs_write_inode(v6fs_t *fs, uint32_t ino, const v6_inode_t *ip);

/* ---- allocation -------------------------------------------------------- */

/* Allocate a free data block into *bno (0 = absent/sparse). */
int v6fs_balloc(v6fs_t *fs, uint32_t *bno);
void v6fs_bfree(v6fs_t *fs, uint32_t bno);
/* Allocate a free inode into *ino. */
int v6fs_ialloc(v6fs_t *fs, uint32_t *ino);
void v6fs_ifree(v6fs_t *fs, uint32_t ino);
/* Free every data block referenced by an inode (truncate to 0). */
int v6fs_itrunc(v6fs_t *fs, v6_inode_t *ip);

/* ---- file / directory data --------------------------------------------- */

/* Map a logical block of an inode to a physical block (allocate if create). */
int v6fs_bmap(v6fs_t *fs, v6_inode_t *ip, uint32_t lbn, int create, uint32_t *bno);

ssize_t v6fs_file_read(v6fs_t *fs, v6_inode_t *ip, uint8_t *buf, size_t size, off_t off);
ssize_t v6fs_file_write(v6fs_t *fs, v6_inode_t *ip, const uint8_t *buf, size_t size, off_t off);

/* Read a directory's entries.  Caller frees with v6fs_dirents_free. */
int v6fs_dir_read(v6fs_t *fs, v6_inode_t *ip, v6_dirent_t **ents, size_t *count);
void v6fs_dirents_free(v6_dirent_t *ents);

/* Look up name in a directory; returns 0 and *ino, or -ENOENT. */
int v6fs_dir_lookup(v6fs_t *fs, v6_inode_t *ip, const char *name, uint32_t *ino);
/* Add an entry (name must be <= V6_DIRSIZ, no '/'); 0 or -errno. */
int v6fs_dir_add(v6fs_t *fs, v6_inode_t *ip, uint32_t ino, const char *name);
/* Remove an entry; 0 or -errno. */
int v6fs_dir_remove(v6fs_t *fs, v6_inode_t *ip, const char *name);

/* Resolve a path into an inode number.  0 or -errno. */
int v6fs_lookup(v6fs_t *fs, const char *path, uint32_t *ino, v6_inode_t *ip);

/* ---- integrity check ---------------------------------------------------- */

typedef struct {
    uint32_t free_blocks;    /* free blocks found by walking the free list */
    uint32_t inodes;         /* total inode slots */
    uint32_t used_inodes;    /* inodes with a non-zero mode */
    uint32_t errors;         /* number of integrity problems found */
} v6_check_t;

/* Walk the free list and the inode table; report findings to stdout.
 * Returns 0 if no errors were found, -1 otherwise. */
int v6fs_check(v6fs_t *fs, v6_check_t *rep);

#endif /* V6FS_H */
