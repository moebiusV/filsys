/* filsys.h - public API for the filsys library.
 *
 * A version-agnostic access layer for Research Unix filesystem images (PDP-7
 * through V10, plus 32V, Coherent, Xenix, 2.9/2.11BSD, System III and System V
 * R2/R4).  The on-disk backends are internal; this header is the stable surface.
 * Edition-specific details live behind the opaque `filsys_t` and the per-edition
 * ops vtable, so a backend change never reaches this API.
 *
 * SPDX-License-Identifier: ISC
 */
#ifndef FILSYS_H
#define FILSYS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "filsys_engine.h"   /* on-disk types: filsys_inode_t, filsys_iter_t, ... */

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

/* Autodetect the filesystem whose block 0 sits at byte `offset`.  `fd` is an
 * open read-only descriptor, `size` the image's total byte size, and `filter`
 * the edition to accept (FILSYS_UNIX = any).  Returns 0 and fills *out, or -1
 * with *why (a static description) when nothing matches.  Read-only. */
int filsys_detect(filsys_detect_t *out, int fd, uint64_t offset, uint64_t size,
                  int filter, const char **why);

/* Open an image.  Returns 0 and *out, or -errno.  uid/gid are the ownership
 * reported by filsys_stat_inode (the "mounting user").  packing selects the
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
                     int no_lock, const char **errmsg);
/* Close and flush.  Returns the final sync result (0 on success, or a negative
 * errno if the last flush failed); the caller should report it and set a
 * non-zero exit status -- the final write is the one a silent failure loses
 * the most. */
int filsys_close(filsys_t *fs);
/* Track an open handle on an inode (hard_remove): the kernel unlinks an open
 * file's name directly, so the inode and its blocks must outlive the name until
 * the last handle closes.  filsys_open_ino bumps the refcount; filsys_close_ino
 * drops it and, if the inode was unlinked while open, frees it then. */
int filsys_open_ino(filsys_t *fs, uint32_t ino);
int filsys_close_ino(filsys_t *fs, uint32_t ino);
/* Flush the superblock (and pending metadata) without closing. */
int filsys_sync(filsys_t *fs);
int filsys_is_readonly(const filsys_t *fs);
int filsys_edition(const filsys_t *fs);
/* The ownership filsys_stat_inode reports for every file (the "mounting
 * user").  Permission checks compare the caller against these, not the on-disk
 * V7 uids, which are meaningless on the host. */
uid_t filsys_uid(const filsys_t *fs);
gid_t filsys_gid(const filsys_t *fs);
/* Run the integrity check (icheck+dcheck); reports to stdout.  Returns 0 if
 * clean, -1 if problems were found. */
int filsys_check(filsys_t *fs);

/* ---- inspection ---------------------------------------------------------- */

/* Resolve one path component: look up `name` in directory `ino` and return the
 * child's inode number.  This is the node-anchored core primitive; a frontend
 * resolves a path by walking it one component at a time from the root inode.
 * Returns 0, or -errno (ENOTDIR if `ino` is not a directory, ENAMETOOLONG if
 * `name` exceeds the edition's limit). */
int filsys_walk(filsys_t *fs, uint32_t ino, const char *name, uint32_t *out);

int filsys_lookup(filsys_t *fs, const char *path, uint32_t *ino,
                  filsys_inode_t *ip);
int filsys_read_inode(filsys_t *fs, uint32_t ino, filsys_inode_t *ip);
/* The root directory inode number (the start of a path walk). */
uint32_t filsys_rootino(const filsys_t *fs);
/* The longest legal filename (component) this edition stores. */
uint32_t filsys_namemax(const filsys_t *fs);
/* Is *ip a directory?  (the descriptor's per-edition type test). */
int filsys_is_dir(const filsys_t *fs, const filsys_inode_t *ip);
/* Fill plain-integer attributes from *ip: the POSIX mode (type bits +
 * permissions), the mounting-user ownership, the raw device word (major<<8 |
 * minor) for device files, the logical block size, and the allocated byte
 * count.  Returns 0, or -errno if the block count cannot be computed (an
 * unreadable indirect block); the caller should treat the stat as failed rather
 * than report a silently wrong count.  A frontend fills its own type (struct
 * stat, struct vattr) from this. */
int filsys_stat_inode(filsys_t *fs, const filsys_inode_t *ip, filsys_stat_t *st);

/* Read a directory entry-by-entry (offset-resumable).  filsys_dir_seek
 * positions at byte offset `off` (0 to start); each filsys_dir_next returns one
 * entry.  *name points into the iterator's buffer and is NOT NUL-terminated:
 * use *namlen, not strcmp.  *next_off is the resume token to hand back to
 * filsys_dir_seek.  Returns 1 (entry), 0 (end), or -errno.  Call
 * filsys_dir_release to free the iterator. */
int filsys_dir_seek(filsys_t *fs, uint32_t ino, uint64_t off, filsys_iter_t *it);
int filsys_dir_next(filsys_iter_t *it, uint32_t *ino, const char **name,
                    uint16_t *namlen, uint64_t *next_off);
int filsys_dir_release(filsys_iter_t *it);

/* ---- file data ----------------------------------------------------------- */

ssize_t filsys_read(filsys_t *fs, const char *path, void *buf, size_t size,
                    off_t off);
ssize_t filsys_write(filsys_t *fs, const char *path, const void *buf, size_t size,
                     off_t off);
/* Same, but by inode number rather than path -- lets an open descriptor stay
 * independent of the directory entry (unlink/rename of the path don't move it). */
ssize_t filsys_read_ino(filsys_t *fs, uint32_t ino, void *buf, size_t size, off_t off);
ssize_t filsys_write_ino(filsys_t *fs, uint32_t ino, const void *buf, size_t size, off_t off);
/* Stat / truncate an open inode by number, for a handle whose name was unlinked
 * while open (hard_remove): the directory entry is gone but the handle still
 * names the inode, so these bypass the path lookup that would return ENOENT. */
int filsys_stat_ino(filsys_t *fs, uint32_t ino, filsys_stat_t *st);
int filsys_truncate_ino(filsys_t *fs, uint32_t ino, off_t size);
/* filsys_bmap_ino's *type: the mapped extent is a hole (reads back as zero) or
 * a physical run. */
enum {
    FILSYS_BMAP_HOLE   = 0,
    FILSYS_BMAP_MAPPED = 1,
};
/* Map a logical byte range [off, off+len) to a physical byte extent, in iomap's
 * shape.  *paddr / *plen are the physical address and length of the contiguous
 * run, both in bytes (OpenBSD's vop_bmap divides by DEV_BSIZE for a block
 * number); *plen is clamped to len and to where the physical mapping stops
 * being contiguous, so readahead gets the longest safe run.  Returns 0, or
 * -errno (an unreadable indirect block). */
int filsys_bmap_ino(filsys_t *fs, uint32_t ino, uint64_t off, uint64_t len,
                    uint64_t *paddr, uint64_t *plen, int *type);
/* Read a symlink's target (no trailing NUL) into buf; returns the byte count.
 * -ENOSYS if the edition predates symlinks, -EINVAL if path is not a symlink. */
ssize_t filsys_readlink(filsys_t *fs, const char *path, char *buf, size_t size);

/* ---- mutation (uid/gid are used when a new inode is created) ------------- */

int filsys_create(filsys_t *fs, const char *path, mode_t mode, uid_t uid,
                  gid_t gid, uint32_t *ino);
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
/* Sentinels for filsys_utimens: a frontend maps the POSIX UTIME_NOW / UTIME_OMIT
 * special nsec values onto these plain-second marks. */
enum {
    FILSYS_UTIME_NOW  = -1,  /* set this timestamp to the current time */
    FILSYS_UTIME_OMIT = -2,  /* leave this timestamp unchanged */
};
/* Set atime/mtime on `path`.  tv may be NULL (set both to now); each entry is a
 * second count since the epoch, or FILSYS_UTIME_NOW / FILSYS_UTIME_OMIT.  ctime
 * is always set to the current time. */
int filsys_utimens(filsys_t *fs, const char *path, const int64_t tv[2]);
int filsys_statfs(filsys_t *fs, filsys_statfs_t *st);

/* ---- node-anchored mutation primitives ------------------------------------
 *
 * The path-taking functions above resolve a path (split + walk) and then do an
 * inode-anchored operation.  These are that operation exposed directly, for a
 * frontend (the kernel, plan9) that has already resolved the directory inode or
 * the inode number itself.  The `_ino` forms act on a single inode; the `_in`
 * forms act on a name within a directory inode. */

int filsys_chmod_ino(filsys_t *fs, uint32_t ino, mode_t mode);
int filsys_chown_ino(filsys_t *fs, uint32_t ino, uid_t uid, gid_t gid);
int filsys_utimens_ino(filsys_t *fs, uint32_t ino, const int64_t tv[2]);
ssize_t filsys_readlink_ino(filsys_t *fs, uint32_t ino, char *buf, size_t size);

int filsys_create_in(filsys_t *fs, uint32_t dir, const char *name, mode_t mode,
                     uid_t uid, gid_t gid, uint32_t *ino);
int filsys_mkdir_in(filsys_t *fs, uint32_t dir, const char *name, mode_t mode,
                    uid_t uid, gid_t gid);
int filsys_mknod_in(filsys_t *fs, uint32_t dir, const char *name, mode_t mode,
                    dev_t rdev, uid_t uid, gid_t gid);
int filsys_symlink_in(filsys_t *fs, const char *target, uint32_t dir,
                      const char *name);
int filsys_unlink_in(filsys_t *fs, uint32_t dir, const char *name);
int filsys_rmdir_in(filsys_t *fs, uint32_t dir, const char *name);
int filsys_link_in(filsys_t *fs, uint32_t src_ino, uint32_t dir, const char *name);
int filsys_rename_in(filsys_t *fs, uint32_t sdir, const char *sname,
                     uint32_t tdir, const char *tname, unsigned int flags);

#ifdef __cplusplus
}
#endif

#endif /* FILSYS_H */
