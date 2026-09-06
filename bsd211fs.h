/* bsd211fs.h - 2.11BSD on-disk constants (see docs/2bsd-format.md).
 *
 * 2.11BSD is served by the V7 engine (v7fs.c): it keeps V7's free-list
 * superblock but swaps in the "new" BSD inode (32-bit block addresses, 4 direct
 * + 3 indirect, di_flags, symlinks) and variable-length directory entries.  The
 * engine is parameterized by the filsys_edition_t descriptor; this header holds
 * only the raw 2.11BSD on-disk numbers, used by the descriptor row
 * (filsys_format.c) and by mkfs.filsys.c's self-contained 2.11BSD mkfs path.
 *
 * SPDX-License-Identifier: ISC
 */
#ifndef BSD211FS_H
#define BSD211FS_H

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

#endif /* BSD211FS_H */
