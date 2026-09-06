/* v6fs.h - Sixth Edition (V6) Unix on-disk constants.
 *
 * V6 is served by the V7 engine (v7fs.c): it keeps V7's free-list superblock
 * but with 16-bit block numbers, s_isize as the *number* of i-list blocks, a
 * 32-byte inode, and the ILARG large-file layout.  The engine is parameterized
 * by the filsys_edition_t descriptor; this header holds the raw V6 on-disk
 * numbers plus the isize-semantics helpers (v6_data_start / v6_maxino), used by
 * the V6 code in v7fs.c, the descriptor row (filsys_format.c), and mkfs.
 *
 * SPDX-License-Identifier: ISC
 */
#ifndef V6FS_H
#define V6FS_H

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

/* i_mode type/mode bits (sys/ino.h).  V6 has no IFREG (regular = type 0)
 * and no IFMPC/IFMPB; bit 010000 is the ILARG large-file flag. */
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

#endif /* V6FS_H */
