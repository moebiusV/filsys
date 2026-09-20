/* filsys_engine.h - the POSIX-free engine surface.
 *
 * The on-disk types and edition selectors, shared by the engine and every
 * frontend.  This header is deliberately free of <sys/stat.h>, <sys/statvfs.h>,
 * <time.h> and <unistd.h>, so a kernel driver (or a plan9/QNX server) can
 * compile against the engine with only <stdint.h> and <stddef.h>.  The
 * POSIX-typed userspace API (path resolution, struct stat/statvfs filling, and
 * the mutating calls) lives in filsys.h, which includes this header.
 *
 * SPDX-License-Identifier: ISC
 */
#ifndef FILSYS_ENGINE_H
#define FILSYS_ENGINE_H

#include <stddef.h>
#include <stdint.h>

/* Edition selectors passed to filsys_open(). */
enum {
    FILSYS_PDP7 = 0,   /* the PDP-7 filesystem (predates V1) */
    FILSYS_V1   = 1,
    FILSYS_V6   = 6,
    FILSYS_V7   = 7,
    FILSYS_V8   = 8,    /* Eighth Edition: V7 inode/dir, V8-family superblock, 1K list / 4K bitmap */
    FILSYS_V9   = 9,    /* Ninth Edition: Sun-3 port, 8K blocks, big-endian */
    FILSYS_V10  = 10,   /* Tenth Edition: V8-family + S_flag + out-of-superblock bitmap */
    FILSYS_32V  = 32,
    FILSYS_COHERENT = 33, /* Coherent (Mark Williams): V7-family, middle-endian, NICFREE=64 */
    FILSYS_XENIX = 34,   /* Xenix (SCO): V7-family, little-endian 2-byte, NICFREE=100 */
    FILSYS_BSD29 = 35,   /* 2.9BSD: V7 inode, 1024-byte blocks, 4 direct + 3 indirect */
    FILSYS_BSD211 = 36,   /* 2.11BSD: 32-bit-address inode, variable 63-char dirs */
    FILSYS_SYSIII = 37,   /* System III s5fs: VAX (little-endian, 4-byte-aligned) by default; -o arch=pdp11 for the packed PDP-11 form */
    FILSYS_SVR2   = 38,   /* System V Release 2 s5fs: 2-byte-aligned; byte order from -o arch */
    FILSYS_SVR4   = 39,   /* System V Release 4 s5fs: 4-byte-aligned + s_pad2; byte order from -o arch */
    FILSYS_UNIX   = 40    /* not a format: autodetect across every edition (a -v mode) */
};

/* One row of the edition name table (defined in filsys_format.c): the canonical
 * "-v" spelling, its alternates, and the FILSYS_* selector.  The tools and the
 * test matrix resolve and list editions through this table rather than their own
 * strcmp chains, so the spellings live in one place and cannot drift. */
typedef struct {
    int         edition;           /* FILSYS_* selector */
    const char *name;              /* canonical spelling: "pdp7", "v1", ... */
    const char *const *aliases;    /* NULL-terminated alternates: "0", "p7", ... */
} filsys_format_t;

/* Decoded inode -- one shape for every edition (V1, V6, V7/32V, PDP-7).  Each
 * backend decodes its own on-disk inode (V1/V6 32 bytes, V7 64 bytes, PDP-7
 * 12 words) field-by-field into this struct.  mode is 32 bits so PDP-7's
 * 18-bit flag word fits (V1/V6/V7 modes are only 16 bits). */
typedef struct {
    uint32_t ino;
    uint32_t mode;               /* on-disk mode bits (edition-specific type) */
    int16_t  nlink;
    int16_t  uid;
    int16_t  gid;
    uint32_t size;               /* bytes; 32-bit by decision -- every edition's
                                  * on-disk size field is <= 32 bits (V7-family 32,
                                  * V6 24, V1 16), so a wider field would never fill */
    uint32_t addr[13];           /* block numbers (device number in addr[0]) */
    uint32_t atime, mtime, ctime;
} filsys_inode_t;

/* One directory entry. */
typedef struct {
    uint16_t ino;
    char     name[64];           /* name + NUL (14 for V6/V7; 63 for 2.11BSD) */
} filsys_dirent_t;

typedef struct filsys filsys_t;  /* opaque */

/* Geometry overrides (the V8-family's `-o` options).  All zero/-1/NULL means
 * "use the edition's default".  Applies only to the V8/V9/V10 editions. */
typedef struct {
    uint32_t    blocksize;      /* 0 = edition default (1024/4096/8192) */
    int         freemap;        /* -1 = derive; else a FILSYS_FREEMAP_* value */
    char        *byteorder;      /* "le"/"be", or NULL = edition default (owned) */
} filsys_geom_t;

/* filsys_geom_t.freemap values: the V8-family free-space representation. */
enum {
    FILSYS_FREEMAP_LIST   = 0,  /* free list */
    FILSYS_FREEMAP_BITMAP = 1,  /* in-superblock bitmap */
    FILSYS_FREEMAP_BIGMAP = 2,  /* out-of-superblock bitmap (V10 only) */
};

/* Confidence of a format probe result: how a detection was made.  MAGIC means
 * a superblock magic word matched (byte order resolved by the word); STRUCTURAL
 * means a full structural validation passed (free-list/bitmap, root inode) with
 * no magic; HEURISTIC is a weaker, partial match. */
typedef enum {
    FILSYS_PROBE_NONE = 0,
    FILSYS_PROBE_HEURISTIC,
    FILSYS_PROBE_STRUCTURAL,
    FILSYS_PROBE_MAGIC,
} filsys_probe_conf_t;

/* The result of filsys_detect(): a resolved edition and its geometry. */
typedef struct {
    int         edition;      /* FILSYS_* selector (never FILSYS_UNIX) */
    uint32_t    blocksize;    /* 0 = edition default */
    int         freemap;      /* -1 = derive; else a FILSYS_FREEMAP_* value */
    const char *byteorder;    /* "le"/"be", or NULL = edition default */
    const char *packing;      /* PDP-7 word container codec name, or NULL */
    filsys_probe_conf_t conf; /* how the winning probe matched */
} filsys_detect_t;

/* One file's attributes as plain integers, the POSIX-free stand-in for struct
 * stat.  A frontend fills its own type from this (struct stat for FUSE, struct
 * vattr for a kernel).  rdev is the raw on-disk device word (major<<8 | minor)
 * so the frontend re-encodes it with its own makedev, and bytes is the
 * allocated byte count so the frontend converts to 512-byte st_blocks units. */
typedef struct {
    uint32_t ino;
    uint32_t mode;      /* POSIX mode: type bits + permissions */
    uint32_t nlink;
    uint32_t uid, gid;  /* the mounting user */
    uint64_t size;
    uint32_t rdev;      /* raw device word (major<<8|minor), 0 if not a device */
    uint64_t atime, mtime, ctime;
    uint32_t blksize;
    uint64_t bytes;     /* allocated bytes (holes not counted) */
} filsys_stat_t;

/* Plain-integer filesystem totals, the POSIX-free stand-in for struct statvfs.
 * The frontend fills its own type from this (struct statvfs for FUSE, struct
 * statfs for a kernel).  bsize doubles as frsize; bavail equals bfree here
 * (every frontend treats the mounting user as privileged). */
typedef struct {
    uint64_t bsize;    /* filesystem block size (f_bsize / f_frsize) */
    uint64_t blocks;   /* total blocks */
    uint64_t bfree;    /* free blocks */
    uint64_t bavail;   /* free blocks available to a non-root user */
    uint64_t files;    /* total inodes */
    uint64_t ffree;    /* free inodes */
    uint32_t namemax;  /* longest filename */
} filsys_statfs_t;

#endif /* FILSYS_ENGINE_H */
