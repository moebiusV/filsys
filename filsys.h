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

#ifdef __cplusplus
extern "C" {
#endif

/* Edition selectors passed to filsys_open(). */
enum {
    FILSYS_PDP7 = 0,   /* the PDP-7 filesystem (predates V1) */
    FILSYS_V1   = 1,
    FILSYS_V6   = 6,
    FILSYS_V7   = 7,
    FILSYS_32V  = 32
};

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
    char     name[15];           /* 14-char name + NUL */
} filsys_dirent_t;

typedef struct filsys filsys_t;  /* opaque */

/* Open an image.  Returns 0 and *out, or -errno.  uid/gid are the ownership
 * reported by filsys_fill_stat (the "mounting user"). */
int filsys_open(filsys_t **out, int edition, const char *path, int readonly,
                uint64_t offset, int uid, int gid);
/* Flush the superblock (and pending metadata) and close. */
void filsys_close(filsys_t *fs);
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

int filsys_read(filsys_t *fs, const char *path, void *buf, size_t size,
                off_t off);
int filsys_write(filsys_t *fs, const char *path, const void *buf, size_t size,
                 off_t off);

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
int filsys_truncate(filsys_t *fs, const char *path, off_t size);
int filsys_chmod(filsys_t *fs, const char *path, mode_t mode);
int filsys_chown(filsys_t *fs, const char *path, uid_t uid, gid_t gid);
int filsys_utimens(filsys_t *fs, const char *path, const struct timespec tv[2]);
int filsys_statfs(filsys_t *fs, struct statvfs *st);

#ifdef __cplusplus
}
#endif

#endif /* FILSYS_H */
