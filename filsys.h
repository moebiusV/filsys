/* filsys.h - public API for the filsys library.
 *
 * A version-agnostic access layer for Research Unix (V4/V5/V6/V7/32V)
 * filesystem images.  The on-disk backends (v6fs/v7fs) are internal; this
 * header is the stable surface.  Edition-specific details live behind the
 * opaque `filsys_t`, so a future vtable/parameter unification of the backends
 * won't change this API.
 *
 * SPDX-License-Identifier: ISC
 */
#ifndef FILSYS_H
#define FILSYS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* Size in bytes of the image backing fd.  A regular file reports its st_size;
 * a block device reports 0 in st_size but its real size via lseek(SEEK_END),
 * so the device path falls through to the seek.  Returns 0 with *sz set, or -1
 * if the size can't be determined.  All library I/O is positional (pread/
 * pwrite), so the seek's offset side effect is harmless. */
static inline int filsys_dev_size(int fd, uint64_t *sz)
{
    struct stat st;
    if (fstat(fd, &st) != 0)
        return -1;
    if (S_ISREG(st.st_mode)) {
        *sz = (uint64_t)st.st_size;
        return 0;
    }
    off_t end = lseek(fd, 0, SEEK_END);
    if (end < 0)
        return -1;
    *sz = (uint64_t)end;
    return 0;
}

#ifdef __cplusplus
extern "C" {
#endif

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
    FILSYS_SYSIII = 37,   /* System III: V7 superblock (no magic), middle-endian */
    FILSYS_SVR2   = 38,   /* System V Release 2 s5fs: 2-byte-aligned; byte order from -o arch */
    FILSYS_SVR4   = 39    /* System V Release 4 s5fs: 4-byte-aligned + s_pad2; byte order from -o arch */
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

/* Iterate the edition name table (returns NULL past the end). */
const filsys_format_t *filsys_format_nth(size_t i);
/* Resolve a canonical name or alias to a FILSYS_* selector, or -1.  Accepts a
 * leading "v"/"V" (so "v7" == "7") as the tools always have. */
int filsys_edition_by_name(const char *name);
/* Resolve the -v argument and, on failure, print "<tool>: bad edition '<arg>'".
 * Returns the FILSYS_* selector or -1.  One shared entry point so the four
 * tools report the same error. */
int filsys_parse_edition(const char *tool, const char *name);
/* The canonical edition names joined by "|", for usage strings. */
const char *filsys_editions_usage(void);

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
    uint32_t size;
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
    const char *byteorder;      /* "le"/"be", or NULL = edition default */
} filsys_geom_t;

/* filsys_geom_t.freemap values: the V8-family free-space representation. */
enum {
    FILSYS_FREEMAP_LIST   = 0,  /* free list */
    FILSYS_FREEMAP_BITMAP = 1,  /* in-superblock bitmap */
    FILSYS_FREEMAP_BIGMAP = 2,  /* out-of-superblock bitmap (V10 only) */
};

/* Open an image.  Returns 0 and *out, or -errno.  uid/gid are the ownership
 * reported by filsys_fill_stat (the "mounting user").  packing selects the
 * PDP-7 word container codec ("rb09", "packed18", "rim"); NULL = the edition's
 * default, and it is ignored for byte-addressed editions. */
int filsys_open(filsys_t **out, int edition, const char *path, int readonly,
                uint64_t offset, uid_t uid, gid_t gid, const char *packing);
/* Like filsys_open, but `arch` names the CPU ("vax"/"x86" little-endian,
 * "3b2"/"68k" big-endian).  For magic-bearing editions the byte order is
 * detected from the superblock magic, so a big-endian volume opens without an
 * arch; `arch` is then checked against it.  `force` opens despite a magic word
 * that matches neither byte order (using `arch`).  On failure *errmsg (if
 * non-NULL) receives a static description of a byte-order problem, else NULL. */
int filsys_open_arch(filsys_t **out, int edition, const char *path, int readonly,
                     uint64_t offset, uid_t uid, gid_t gid, const char *packing,
                     const char *arch, int force, const filsys_geom_t *geom,
                     const char **errmsg);
/* Close and flush.  Returns the final sync result (0 on success, or a negative
 * errno if the last flush failed); the caller should report it and set a
 * non-zero exit status -- the final write is the one a silent failure loses
 * the most. */
int filsys_close(filsys_t *fs);
/* Flush the superblock (and pending metadata) without closing. */
int filsys_sync(filsys_t *fs);
int filsys_is_readonly(const filsys_t *fs);
int filsys_edition(const filsys_t *fs);
/* The ownership filsys_fill_stat reports for every file (the "mounting
 * user").  Permission checks compare the caller against these, not the on-disk
 * V7 uids, which are meaningless on the host. */
uid_t filsys_uid(const filsys_t *fs);
gid_t filsys_gid(const filsys_t *fs);
/* Run the integrity check (icheck+dcheck); reports to stdout.  Returns 0 if
 * clean, -1 if problems were found. */
int filsys_check(filsys_t *fs);

/* ---- inspection ---------------------------------------------------------- */

int filsys_lookup(filsys_t *fs, const char *path, uint32_t *ino,
                  filsys_inode_t *ip);
int filsys_read_inode(filsys_t *fs, uint32_t ino, filsys_inode_t *ip);
void filsys_fill_stat(filsys_t *fs, const filsys_inode_t *ip, struct stat *st);

/* Read a directory's entries into *ents (malloc'd; free() it).  Returns the
 * entry count in *count, or -errno. */
int filsys_readdir(filsys_t *fs, const char *path, filsys_dirent_t **ents,
                   size_t *count);

/* ---- file data ----------------------------------------------------------- */

ssize_t filsys_read(filsys_t *fs, const char *path, void *buf, size_t size,
                    off_t off);
ssize_t filsys_write(filsys_t *fs, const char *path, const void *buf, size_t size,
                     off_t off);
/* Same, but by inode number rather than path -- lets an open descriptor stay
 * independent of the directory entry (unlink/rename of the path don't move it). */
ssize_t filsys_read_ino(filsys_t *fs, uint32_t ino, void *buf, size_t size, off_t off);
ssize_t filsys_write_ino(filsys_t *fs, uint32_t ino, const void *buf, size_t size, off_t off);
/* Read a symlink's target (no trailing NUL) into buf; returns the byte count.
 * -ENOSYS if the edition predates symlinks, -EINVAL if path is not a symlink. */
ssize_t filsys_readlink(filsys_t *fs, const char *path, char *buf, size_t size);

/* ---- mutation (uid/gid are used when a new inode is created) ------------- */

int filsys_create(filsys_t *fs, const char *path, mode_t mode, uid_t uid,
                  gid_t gid);
int filsys_mkdir(filsys_t *fs, const char *path, mode_t mode, uid_t uid,
                 gid_t gid);
int filsys_mknod(filsys_t *fs, const char *path, mode_t mode, dev_t rdev,
                 uid_t uid, gid_t gid);
int filsys_unlink(filsys_t *fs, const char *path);
int filsys_rmdir(filsys_t *fs, const char *path);
int filsys_link(filsys_t *fs, const char *from, const char *to);
int filsys_rename(filsys_t *fs, const char *from, const char *to,
                  unsigned int flags);
int filsys_symlink(filsys_t *fs, const char *target, const char *linkpath);
int filsys_truncate(filsys_t *fs, const char *path, off_t size);
int filsys_chmod(filsys_t *fs, const char *path, mode_t mode);
int filsys_chown(filsys_t *fs, const char *path, uid_t uid, gid_t gid);
int filsys_utimens(filsys_t *fs, const char *path, const struct timespec tv[2]);
int filsys_statfs(filsys_t *fs, struct statvfs *st);

#ifdef __cplusplus
}
#endif

#endif /* FILSYS_H */
