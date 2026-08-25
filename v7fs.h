/* v7fs.h — Seventh Edition (V7) Unix filesystem, on-disk access layer.
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

#define V7_BSIZE    512
#define V7_INOPB    8          /* inodes per block */
#define V7_INODESZ  64         /* sizeof(struct dinode) */
#define V7_NICFREE  50         /* superblock free-block cache size */
#define V7_NICINOD  100        /* superblock free-inode cache size */
#define V7_ROOTINO  2
#define V7_SUPERB   1          /* block number of superblock */
#define V7_NDADDR   10         /* direct addresses per inode */
#define V7_NIADDR   13         /* total address slots per inode */
#define V7_NINDIR   (V7_BSIZE / 4)  /* 4-byte addresses per indirect block */
#define V7_DIRSIZ   14         /* chars per directory entry name */

/* di_mode type/mode bits (sys/ino.h) */
#define V7_IFMT   0170000
#define V7_IFCHR  0020000
#define V7_IFDIR  0040000
#define V7_IFBLK  0060000
#define V7_IFREG  0100000
#define V7_IFMPC  0030000
#define V7_IFMPB  0070000
#define V7_ISUID  0004000
#define V7_ISGID  0002000
#define V7_ISVTX  0001000
#define V7_IREAD  0000400
#define V7_IWRITE 0000200
#define V7_IEXEC  0000100

/* ---- byte-order primitives --------------------------------------------- */

static inline uint16_t v7_get16le(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline void v7_put16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}
/* 32-bit middle-endian: high word first, each word little-endian. */
static inline uint32_t v7_get32me(const uint8_t *p) {
    uint16_t hi = v7_get16le(p);
    uint16_t lo = v7_get16le(p + 2);
    return ((uint32_t)hi << 16) | lo;
}
static inline void v7_put32me(uint8_t *p, uint32_t v) {
    v7_put16le(p,     (uint16_t)(v >> 16));
    v7_put16le(p + 2, (uint16_t)(v & 0xffff));
}
/* 24-bit block number packed as [ hi, lo, mid ]. */
static inline uint32_t v7_get24me(const uint8_t *p) {
    return (uint32_t)p[1] | ((uint32_t)p[2] << 8) | ((uint32_t)p[0] << 16);
}
static inline void v7_put24me(uint8_t *p, uint32_t v) {
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
    uint32_t addr[V7_NIADDR];  /* block numbers, 0 = absent */
    uint32_t atime, mtime, ctime;
} v7_inode_t;

typedef struct {
    uint16_t ino;
    char     name[V7_DIRSIZ + 1];
} v7_dirent_t;

typedef struct {
    int        fd;             /* open disk image */
    int        readonly;
    /* in-core superblock (kept in sync with block 1) */
    uint16_t   isize;
    uint32_t   fsize;
    uint16_t   nfree;
    uint32_t   free[V7_NICFREE];
    uint16_t   ninode;
    uint16_t   inode[V7_NICINOD];
    uint32_t   time;           /* last superblock update */
} v7fs_t;

/* ---- lifecycle --------------------------------------------------------- */

/* Open a disk image.  Returns 0, or -errno. */
int v7fs_open(v7fs_t *fs, const char *path, int readonly);
/* Flush the superblock and close. */
void v7fs_close(v7fs_t *fs);

/* ---- block / inode io -------------------------------------------------- */

int v7fs_read_block(v7fs_t *fs, uint32_t bno, uint8_t *buf);
int v7fs_write_block(v7fs_t *fs, uint32_t bno, const uint8_t *buf);

/* itod / itoo: inode number -> block and offset. */
static inline uint32_t v7_itod(uint32_t ino) { return (ino + 15) >> 3; }
static inline uint32_t v7_itoo(uint32_t ino) { return (ino + 15) & 7; }

int v7fs_read_inode(v7fs_t *fs, uint32_t ino, v7_inode_t *ip);
int v7fs_write_inode(v7fs_t *fs, uint32_t ino, const v7_inode_t *ip);

/* ---- allocation -------------------------------------------------------- */

/* Allocate a free data block into *bno (0 = absent/sparse). */
int v7fs_balloc(v7fs_t *fs, uint32_t *bno);
void v7fs_bfree(v7fs_t *fs, uint32_t bno);
/* Allocate a free inode into *ino. */
int v7fs_ialloc(v7fs_t *fs, uint32_t *ino);
void v7fs_ifree(v7fs_t *fs, uint32_t ino);
/* Free every data block referenced by an inode (truncate to 0). */
int v7fs_itrunc(v7fs_t *fs, v7_inode_t *ip);

/* ---- file / directory data --------------------------------------------- */

/* Map a logical block of an inode to a physical block (allocate if create). */
int v7fs_bmap(v7fs_t *fs, v7_inode_t *ip, uint32_t lbn, int create, uint32_t *bno);

ssize_t v7fs_file_read(v7fs_t *fs, v7_inode_t *ip, uint8_t *buf, size_t size, off_t off);
ssize_t v7fs_file_write(v7fs_t *fs, v7_inode_t *ip, const uint8_t *buf, size_t size, off_t off);

/* Read a directory's entries.  Caller frees with v7fs_dirents_free. */
int v7fs_dir_read(v7fs_t *fs, v7_inode_t *ip, v7_dirent_t **ents, size_t *count);
void v7fs_dirents_free(v7_dirent_t *ents);

/* Look up name in a directory; returns 0 and *ino, or -ENOENT. */
int v7fs_dir_lookup(v7fs_t *fs, v7_inode_t *ip, const char *name, uint32_t *ino);
/* Add an entry (name must be <= V7_DIRSIZ, no '/'); 0 or -errno. */
int v7fs_dir_add(v7fs_t *fs, v7_inode_t *ip, uint32_t ino, const char *name);
/* Remove an entry; 0 or -errno. */
int v7fs_dir_remove(v7fs_t *fs, v7_inode_t *ip, const char *name);

/* Resolve a path into an inode number.  0 or -errno. */
int v7fs_lookup(v7fs_t *fs, const char *path, uint32_t *ino, v7_inode_t *ip);

/* ---- integrity check ---------------------------------------------------- */

typedef struct {
    uint32_t free_blocks;    /* free blocks found by walking the free list */
    uint32_t inodes;         /* total inode slots */
    uint32_t used_inodes;    /* inodes with a non-zero mode */
    uint32_t errors;         /* number of integrity problems found */
} v7_check_t;

/* Walk the free list and the inode table; report findings to stdout.
 * Returns 0 if no errors were found, -1 otherwise. */
int v7fs_check(v7fs_t *fs, v7_check_t *rep);

#endif /* V7FS_H */
