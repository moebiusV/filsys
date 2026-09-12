/* filsys.c - version-agnostic access layer for Research Unix filesystem
 * images.  Dispatches through the per-edition filsys_ops vtable; this is the
 * behind both mount.filsys (FUSE) and the standalone tools.
 *
 * An edition is described by a filsys_edition_t (constants + mode conversion,
 * in filsys_format.c) and a filsys_ops vtable (operations).  filsys_getformat()
 * is the single lookup: adding a format is a new descriptor row, not another
 * arm of a dozen `== FILSYS_` switches here.
 *
 * SPDX-License-Identifier: ISC
 */
#include <config.h>
#include "filsys.h"
#include "filsys_ops.h"
#include "v1fs.h"
#include "v7fs.h"
#include "pdp7fs.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* major()/minor(): glibc >= 2.28 moved them from <sys/types.h> (which filsys.h
 * pulls in) into <sys/sysmacros.h>; the BSDs and macOS still declare them in
 * <sys/types.h>. */
#ifdef HAVE_SYS_SYSMACROS_H
#include <sys/sysmacros.h>
#endif

/* Open-handle tracking for hard_remove: with hard_remove the kernel unlinks an
 * open file's name directly (no silly-rename to a hidden name), so the inode and
 * its blocks must outlive the name until the last handle closes.  Each open
 * inode carries a refcount; an unlink that drops nlink to 0 marks it pending and
 * defers the block/inode free to the final release.
 *
 * Only regular files (and symlinks) ever enter this table: the FUSE adapters
 * define no opendir/releasedir, so a directory is never "open" and rmdir /
 * directory-rename cannot reach the deferred-free path.  That is the reason
 * filsys_rmdir can free an inode unconditionally.  If opendir is ever added
 * (e.g. for a readdir-offset cache), that invariant has to be re-established,
 * or a directory can strand its blocks the same way.
 *
 * Why the table is a fixed 1024 entries (rather than grown on demand): a
 * hypothesis worth recording and NOT re-chasing.  On some platforms the FUSE
 * `release` callback can fire at vnode *reclaim* instead of at last close,
 * which would leave entries live long after close and let 1024 distinct inodes
 * overflow the table as -ENFILE (and defer hard_remove frees so statfs
 * under-reports).  Measured on OpenBSD 7.9 (base libfuse 2.6) through a live
 * FUSE2 mount -- test-openbsd-release.sh, "finding A" -- 2000 distinct files
 * were created and re-opened with zero -ENFILE failures and statfs recovered
 * to baseline.  So release fires at last close there and the fixed 1024 table
 * is not the binding limit; no reclaim-time-release workaround is needed. */
enum { FILSYS_OPEN_MAX = 1024 };
struct open_handle {
    uint32_t ino;
    int      refs;
    int      pending;   /* nlink hit 0 while still open: free on last close */
};

struct filsys {
    const struct filsys_ops *ops;
    filsys_edition_t            fmt;      /* the format (by value) */
    filsys_edition_t *fs;                  /* backend state (filsys_edition_t / v1fs_t / p7fs_t) */
    int ver;
    uid_t uid;                 /* reported ownership (default: the mounting user) */
    gid_t gid;
    int readonly;
    struct open_handle opens[FILSYS_OPEN_MAX];
    int nopen;
};

/* ---- mode conversion ----------------------------------------------------- */

/* The V6/V7/BSD211 family derives every mode operation from the ifmt/ifdir/...
 * constants in the descriptor, so its fn-ptr slots are NULL and these wrappers
 * fall through to the shared logic below.  V1 and PDP-7 supply their own (in
 * filsys_format.c). */
static mode_t mode_to_posix(const filsys_edition_t *f, const filsys_inode_t *ip) {
    if (f->to_posix_mode)
        return f->to_posix_mode(f, ip);
    mode_t m = ip->mode & 07777;
    uint32_t t = ip->mode & f->ifmt;
    if (t == f->ifdir)
        m |= S_IFDIR;
    else if (t == f->ifchr || (f->ifmpc && t == f->ifmpc))
        m |= S_IFCHR;
    else if (t == f->ifblk || (f->ifmpb && t == f->ifmpb))
        m |= S_IFBLK;
    else if (f->iflnk && t == f->iflnk)
        m |= S_IFLNK;
    else if (f->ifsock && t == f->ifsock)
        m |= S_IFSOCK;
    else
        m |= S_IFREG;   /* V6: regular file = type 0 */
    return m;
}
static int mode_is_dir(const filsys_edition_t *f, const filsys_inode_t *ip) {
    if (f->is_dir)
        return f->is_dir(f, ip);
    return (ip->mode & f->ifmt) == f->ifdir;
}
static int mode_is_device(const filsys_edition_t *f, const filsys_inode_t *ip) {
    if (f->is_device)
        return f->is_device(f, ip);
    uint32_t t = ip->mode & f->ifmt;
    return t == f->ifchr || t == f->ifblk ||
           (f->ifmpc && t == f->ifmpc) || (f->ifmpb && t == f->ifmpb);
}
static uint32_t mode_to_disk(const filsys_edition_t *f, mode_t m, int type) {
    if (f->to_disk_mode)
        return f->to_disk_mode(f, m, type);
    uint32_t base = (uint32_t)(m & 07777);
    switch (type) {
    case FILSYS_FT_DIR: return base | f->ifdir;
    case FILSYS_FT_CHR: return base | f->ifchr;
    case FILSYS_FT_BLK: return base | f->ifblk;
    case FILSYS_FT_LNK: return base | f->iflnk;
    default:            return f->ifreg ? (base | f->ifreg) : base;
    }
}
static uint32_t mode_chmod(const filsys_edition_t *f, uint32_t old, mode_t m) {
    if (f->chmod_mode)
        return f->chmod_mode(f, old, m);
    return (old & f->ifmt) | ((uint32_t)m & 07777);
}

/* The on-disk uid/gid is 16-bit; a modern uid like 70000 would silently wrap
 * to 4464 on create.  Reject rather than truncate (mknod already does this for
 * major/minor). */
static int uidgid_fit(uid_t uid, gid_t gid) {
    return (uid > INT16_MAX || gid > INT16_MAX) ? -EINVAL : 0;
}

/* ---- dispatch (internal): forward through the backend ops table ---------- */

static int read_inode(filsys_t *fs, uint32_t ino, filsys_inode_t *ip) {
    return fs->ops->inode->read_inode(fs->fs, ino, ip);
}
static int write_inode(filsys_t *fs, uint32_t ino, const filsys_inode_t *ip) {
    return fs->ops->inode->write_inode(fs->fs, ino, ip);
}
static int ialloc(filsys_t *fs, uint32_t *ino) {
    return fs->ops->ialloc(fs->fs, ino);
}
static void ifree(filsys_t *fs, uint32_t ino) {
    fs->ops->ifree(fs->fs, ino);
}
static int itrunc(filsys_t *fs, filsys_inode_t *ip, filsys_blklist_t **bl) {
    *bl = filsys_blklist_new();
    if (!*bl)
        return -ENOMEM;
    return filsys_itrunc(fs->fs, ip, *bl);
}
static int itrunc_from(filsys_t *fs, filsys_inode_t *ip, uint32_t first_blk,
                       filsys_blklist_t **bl) {
    *bl = filsys_blklist_new();
    if (!*bl)
        return -ENOMEM;
    return filsys_itrunc_from(fs->fs, ip, first_blk, *bl);
}
static ssize_t file_read(filsys_t *fs, filsys_inode_t *ip, uint8_t *buf, size_t sz, off_t off) {
    return fs->ops->file_read(fs->fs, ip, buf, sz, off);
}
static ssize_t file_write(filsys_t *fs, filsys_inode_t *ip, const uint8_t *buf, size_t sz, off_t off) {
    return fs->ops->file_write(fs->fs, ip, buf, sz, off);
}
static int dir_read(filsys_t *fs, filsys_inode_t *ip, filsys_dirent_t **e, size_t *n) {
    return fs->ops->dir->dir_read(fs->fs, ip, e, n);
}
static int dir_lookup(filsys_t *fs, filsys_inode_t *ip, const char *name, uint32_t *ino) {
    return fs->ops->dir_lookup(fs->fs, ip, name, ino);
}
static int dir_add(filsys_t *fs, filsys_inode_t *ip, uint32_t ino, const char *name) {
    return fs->ops->dir->dir_add(fs->fs, ip, ino, name);
}
static int dir_remove(filsys_t *fs, filsys_inode_t *ip, const char *name) {
    return fs->ops->dir->dir_remove(fs->fs, ip, name);
}
static int lookup(filsys_t *fs, const char *path, uint32_t *ino, filsys_inode_t *ip) {
    return fs->ops->lookup(fs->fs, path, ino, ip);
}
static int bmap(filsys_t *fs, filsys_inode_t *ip, uint32_t lbn, int create, uint32_t *bno) {
    return fs->ops->inode->bmap(fs->fs, ip, lbn, create, bno);
}

/* Largest file the selected edition can address, in bytes. */
static uint64_t maxfile(const filsys_t *fs) {
    return fs->ops->max_file(fs->fs);
}

/* Free a deferred (unlink/rename-while-open) inode: its name is gone and nlink
 * is 0, but an open handle still pins its blocks.  Re-read it by number, drop
 * its blocks, clear it, and return it to the free list -- only after the clear
 * is durable, so a crash never leaves a free inode still referenced. */
static int free_deferred_ino(filsys_t *fs, uint32_t ino) {
    filsys_inode_t ip;
    if (read_inode(fs, ino, &ip))
        return -EIO;
    filsys_blklist_t *bl = NULL;
    int rc = itrunc(fs, &ip, &bl);
    if (rc) { filsys_blklist_discard(bl); return rc; }
    ip.mode = 0;
    rc = write_inode(fs, ino, &ip);
    if (rc) { filsys_blklist_discard(bl); return rc; }
    filsys_blklist_drain(fs->fs, bl);
    ifree(fs, ino);
    return 0;
}

/* ---- helpers ------------------------------------------------------------- */

static int split_path(const char *path, char *dir, size_t dirsz,
                      char *name, size_t namesz) {
    const char *slash = strrchr(path, '/');
    if (!slash)
        return -EINVAL;
    size_t dlen = (size_t)(slash - path);
    if (dlen == 0)
        dlen = 1;
    if (dlen >= dirsz)
        return -ENAMETOOLONG;
    memcpy(dir, path, dlen);
    dir[dlen] = 0;
    const char *nm = slash + 1;
    if (!*nm)
        return -EINVAL;
    size_t nlen = strlen(nm);
    if (nlen >= namesz)
        return -ENAMETOOLONG;
    memcpy(name, nm, nlen + 1);
    return 0;
}

/* ---- public API ---------------------------------------------------------- */

/* Advisory byte-range lock over one filesystem's extent within the image, so
 * two read-write mounts of the same extent can't hand out the same block (the
 * silent, persistent corruption case).  Only a read-write mount takes a lock
 * (an OFD write lock); a read-only mount takes none but warns if a write lock
 * is already held there.  Nested mounts (root at offset 0, /usr at offset N)
 * hold disjoint extents and coexist.  OFD, not POSIX record locks, so the lock
 * isn't dropped when any descriptor of the file is closed.  Advisory: it does
 * not stop simh, which does not participate. */
static int lock_image(filsys_t *fs, const char *path, uint64_t offset) {
    uint64_t len = (uint64_t)fs->fs->fsize * fs->fs->bsize;
    struct flock lk = { .l_type = F_WRLCK, .l_whence = SEEK_SET,
                        .l_start = (off_t)offset, .l_len = (off_t)len };
    int fd = fs->fs->fd;
    /* OFD locks are a Linux-only feature (per-open-file-description); POSIX
     * record locks (the BSDs/macOS/illumos) are per-process and drop on any
     * close of the file -- the best those hosts offer.  Solaris/illumos declare
     * F_OFD_SETLK for Linux binary compatibility but its kernel rejects it with
     * EINVAL, so the OFD path is gated on __linux__, not just the decl. */
#if defined(__linux__) && HAVE_DECL_F_OFD_SETLK
    int setlk = F_OFD_SETLK, getlk = F_OFD_GETLK;
#else
    int setlk = F_SETLK, getlk = F_GETLK;
#endif
    if (fs->readonly) {
        if (fcntl(fd, getlk, &lk) == 0 && lk.l_type == F_WRLCK)
            fprintf(stderr, "filsys: warning: %s is open read-write elsewhere; "
                    "this mount won't see those writes\n", path);
        return 0;
    }
    if (fcntl(fd, setlk, &lk) == 0)
        return 0;
    fprintf(stderr, "filsys: %s is already open read-write (lock: %s; "
            "use -o no_lock to override)\n", path, strerror(errno));
    return -EBUSY;
}

int filsys_open_arch(filsys_t **out, int edition, const char *path, int readonly,
                     uint64_t offset, uid_t uid, gid_t gid, const char *packing,
                     const char *arch, int force, const filsys_geom_t *geom,
                     int no_lock, const char **errmsg) {
    filsys_edition_t fmt = filsys_getformat(edition);
    if (!fmt.ops)
        return -EINVAL;
    if (geom && (edition == FILSYS_V8 || edition == FILSYS_V9 || edition == FILSYS_V10)) {
        int rc = filsys_apply_geom(&fmt, edition, geom, errmsg);
        if (rc)
            return rc;
    }
    int rc = filsys_resolve_byteorder(&fmt, path, offset, arch, force, errmsg);
    if (rc)
        return rc;
    if (packing && fmt.word) {   /* packing applies only to word-addressed editions */
        const word_codec_t *wc = filsys_word_codec_by_name(packing);
        if (!wc)
            return -EINVAL;
        fmt.word = wc;
    }
    filsys_t *fs = calloc(1, sizeof(*fs));
    if (!fs)
        return -ENOMEM;
    fs->ver = edition;
    fs->fmt = fmt;
    fs->ops = fmt.ops;
    fs->uid = uid;
    fs->gid = gid;
    fs->readonly = readonly;
    fs->fs = calloc(1, fmt.state_size);
    if (!fs->fs) {
        free(fs);
        return -ENOMEM;
    }
    rc = fs->ops->open(fs->fs, path, readonly, &fmt, offset);
    if (rc) {
        free(fs->fs);
        free(fs);
        return rc;
    }
    if (!no_lock) {
        rc = lock_image(fs, path, offset);
        if (rc) {
            fs->ops->close(fs->fs);
            free(fs->fs);
            free(fs);
            return rc;
        }
    }
    /* A read-write open is dirty until a clean close clears s_fmod (see the
     * backends' close); stamp it now so a crash before close is flagged.  If the
     * dirty stamp itself can't be persisted, refuse the open -- otherwise a crash
     * before the first flush would leave s_fmod clear and fsck -p would skip the
     * damaged filesystem. */
    if (!readonly && fs->ops->mark_dirty) {
        rc = fs->ops->mark_dirty(fs->fs);
        if (rc) {
            fs->ops->close(fs->fs);
            free(fs->fs);
            free(fs);
            return rc;
        }
    }
    *out = fs;
    return 0;
}

int filsys_open(filsys_t **out, int edition, const char *path, int readonly,
                uint64_t offset, uid_t uid, gid_t gid, const char *packing) {
    return filsys_open_arch(out, edition, path, readonly, offset, uid, gid, packing, NULL, 0, NULL, 0, NULL);
}

int filsys_close(filsys_t *fs) {
    if (!fs)
        return 0;
    /* Drain any deferred (unlink-while-open) frees *before* the final flush, so
     * they land inside that flush rather than after it.  A forced unmount (or
     * the OpenBSD ifuse_try_unmount fork) may not send release for every open
     * fd; draining here is the only path that frees those orphans.
     *
     * A process killed by SIGKILL never reaches filsys_close, so its in-memory
     * open table is simply gone and the nlink==0 inode stays on disk; such
     * persistent orphans must therefore remain detectable and recoverable by
     * fsck (check.c) -- this drain cannot address them.
     *
     * Error contract: filsys_close must never return 0 if any step failed.  The
     * final flush's error wins over a deferred-free error, because a failed
     * flush can leave the whole image inconsistent whereas a failed deferred
     * free is a single recoverable orphan. */
    int rc = 0;
    for (int i = 0; i < fs->nopen; i++) {
        if (fs->opens[i].pending) {
            int r = free_deferred_ino(fs, fs->opens[i].ino);
            if (r && !rc)
                rc = r;                 /* first deferred-free failure */
        }
    }
    if (fs->fs) {
        /* A clean unmount clears s_fmod (the image is now consistent).  This
         * happens on the mount path, not in the backend close: fsck opens the
         * same backend read-write to repair, and a partial repair must not
         * clear s_fmod (a clean flag would make the next check skip it). */
        if (!fs->readonly && fs->ops->mark_clean) {
            int r = fs->ops->mark_clean(fs->fs);
            if (r && !rc)
                rc = r;
        }
        int r = fs->ops->close(fs->fs);
        if (r)
            rc = r;                     /* flush failure dominates */
    }
    free(fs->fs);
    free(fs);
    return rc;
}

int filsys_open_ino(filsys_t *fs, uint32_t ino) {
    for (int i = 0; i < fs->nopen; i++)
        if (fs->opens[i].ino == ino) {
            fs->opens[i].refs++;
            return 0;
        }
    if (fs->nopen >= FILSYS_OPEN_MAX)
        return -ENFILE;
    fs->opens[fs->nopen].ino = ino;
    fs->opens[fs->nopen].refs = 1;
    fs->opens[fs->nopen].pending = 0;
    fs->nopen++;
    return 0;
}

int filsys_close_ino(filsys_t *fs, uint32_t ino) {
    for (int i = 0; i < fs->nopen; i++) {
        if (fs->opens[i].ino != ino)
            continue;
        if (--fs->opens[i].refs > 0)
            return 0;
        if (!fs->opens[i].pending) {
            fs->opens[i] = fs->opens[fs->nopen - 1];   /* drop the entry */
            fs->nopen--;
            return 0;
        }
        /* Unlinked while open: free the inode and its blocks now.  Keep the
         * entry until the free commits -- if a fault-injected I/O fails midway,
         * the pending marker survives so filsys_close can retry, rather than
         * stranding a half-truncated inode nothing can find. */
        int rc = free_deferred_ino(fs, ino);
        if (rc == 0) {
            fs->opens[i] = fs->opens[fs->nopen - 1];
            fs->nopen--;
        }
        return rc;
    }
    return 0;   /* not tracked: nothing to release */
}

/* Test hook: swap the byte-slice transport (fault injection).  Internal, used
 * by the fault-injection regression test to drive block I/O failures. */
void filsys_set_io(filsys_t *fs, const filsys_io_t *io) {
    fs->fs->io = io;
}

int filsys_sync(filsys_t *fs) {
    int rc = fs->ops->sync(fs->fs);
    /* ops->sync pushed the superblock and pending metadata into the host page
     * cache via pwrite.  fsync the backing fd so the FUSE fsync path -- and an
     * application's fsync(2) on a mounted file -- actually reaches the device
     * rather than stopping at the cache: without this the guarantee ends at
     * "process-crash durable" and says nothing about host crash / power loss. */
    if (rc == 0 && !fs->readonly && fs->fs->fd >= 0 && fsync(fs->fs->fd) != 0)
        rc = -errno;
    return rc;
}

int filsys_is_readonly(const filsys_t *fs) {
    return fs->readonly;
}

int filsys_edition(const filsys_t *fs) {
    return fs->ver;
}

uid_t filsys_uid(const filsys_t *fs) {
    return fs->uid;
}

gid_t filsys_gid(const filsys_t *fs) {
    return fs->gid;
}

int filsys_check(filsys_t *fs) {
    return fs->ops->check(fs->fs);
}

int filsys_invariants(filsys_t *fs, filsys_check_t *rep) {
    /* Every backend state is a filsys_edition_t (v1fs_t and p7fs_t alias it),
     * so passing the state as its own format descriptor is correct here -- the
     * same `fs, fs` pairing filsys_check_op uses.  FORCE bypasses the
     * is_clean short-circuit; QUIET drops the always-on chatter. */
    return filsys_check_common(fs->fs, fs->fs, rep, FILSYS_CK_FORCE | FILSYS_CK_QUIET);
}

int filsys_lookup(filsys_t *fs, const char *path, uint32_t *ino, filsys_inode_t *ip) {
    return lookup(fs, path, ino, ip);
}

int filsys_read_inode(filsys_t *fs, uint32_t ino, filsys_inode_t *ip) {
    return read_inode(fs, ino, ip);
}

void filsys_fill_stat(filsys_t *fs, const filsys_inode_t *ip, struct stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_ino   = ip->ino;
    st->st_mode  = mode_to_posix(&fs->fmt, ip);
    st->st_nlink = (nlink_t)ip->nlink;
    st->st_uid   = fs->uid;
    st->st_gid   = fs->gid;
    st->st_size  = ip->size;
    if (mode_is_device(&fs->fmt, ip))
        /* V7 packs 8-bit major + 8-bit minor into one word; makedev() re-encodes
         * into the host's dev_t layout (Linux's is the same, the BSDs'/macOS's
         * differ). */
        st->st_rdev = makedev(ip->addr[0] >> 8, ip->addr[0] & 0xff);
    st->st_atime   = ip->atime;
    st->st_mtime   = ip->mtime;
    st->st_ctime   = ip->ctime;
    uint32_t blksize = fs->ops->blocksize(fs->fs);
    st->st_blksize = blksize;
    /* POSIX: st_blocks counts 512-byte units, not filesystem blocks, and only
     * blocks actually allocated -- a hole (zero block address) is not counted,
     * so a sparse file reports less than st_size implies. */
    st->st_blocks  = (fs->ops->inode->allocated_blocks(fs->fs, ip) * blksize + 511) / 512;
}

int filsys_readdir(filsys_t *fs, const char *path, filsys_dirent_t **ents, size_t *count) {
    filsys_inode_t ip;
    uint32_t ino;
    int rc = lookup(fs, path, &ino, &ip);
    if (rc) return rc;
    return dir_read(fs, &ip, ents, count);
}

ssize_t filsys_read(filsys_t *fs, const char *path, void *buf, size_t size, off_t off) {
    filsys_inode_t ip;
    uint32_t ino;
    int rc = lookup(fs, path, &ino, &ip);
    if (rc) return rc;
    return file_read(fs, &ip, (uint8_t *)buf, size, off);
}

ssize_t filsys_read_ino(filsys_t *fs, uint32_t ino, void *buf, size_t size, off_t off) {
    filsys_inode_t ip;
    int rc = read_inode(fs, ino, &ip);
    if (rc) return rc;
    return file_read(fs, &ip, (uint8_t *)buf, size, off);
}

ssize_t filsys_write(filsys_t *fs, const char *path, const void *buf, size_t size, off_t off) {
    filsys_inode_t ip;
    uint32_t ino;
    int rc = lookup(fs, path, &ino, &ip);
    if (rc) return rc;
    return filsys_write_ino(fs, ino, buf, size, off);
}

ssize_t filsys_write_ino(filsys_t *fs, uint32_t ino, const void *buf, size_t size, off_t off) {
    filsys_inode_t ip;
    int rc = read_inode(fs, ino, &ip);
    if (rc) return rc;
    if (off < 0)
        return -EINVAL;
    /* A write past the per-format file-size ceiling can never succeed; reject
     * it before the first block is allocated so there is nothing to unwind. */
    if ((uint64_t)off + (uint64_t)size > maxfile(fs))
        return -EFBIG;

    uint32_t oldsize = ip.size;
    ssize_t n = file_write(fs, &ip, (const uint8_t *)buf, size, off);
    if (n < 0) {
        /* A legal-sized write can still fail partway (ENOSPC, EIO): collect the
         * blocks it allocated past the original size, reset the inode, then free
         * the collected blocks.  The reset is persisted before the free, so a
         * failed write leaves leaks (recoverable) rather than aliasing. */
        uint32_t bsize = fs->ops->blocksize(fs->fs);
        uint32_t first_blk = oldsize ? (oldsize - 1) / bsize + 1 : 0;
        filsys_blklist_t *bl = NULL;
        rc = itrunc_from(fs, &ip, first_blk, &bl);
        if (rc == 0)
            rc = write_inode(fs, ino, &ip);
        /* A rollback failure is more severe than the write error it was
         * cleaning up: the inode would now hold dangling block pointers. */
        if (rc) {
            filsys_blklist_discard(bl);
            return rc;
        }
        filsys_blklist_drain(fs->fs, bl);
    }
    return n;
}

int filsys_create(filsys_t *fs, const char *path, mode_t mode, uid_t uid, gid_t gid) {
    int rc = uidgid_fit(uid, gid);
    if (rc) return rc;
    char dir[PATH_MAX], name[64];
    rc = split_path(path, dir, sizeof(dir), name, fs->fmt.max_namlen + 1);
    if (rc) return rc;
    filsys_inode_t ddir;
    uint32_t dino;
    rc = lookup(fs, dir, &dino, &ddir);
    if (rc) return rc;
    uint32_t nino;
    rc = ialloc(fs, &nino);
    if (rc) return rc;
    filsys_inode_t nip;
    memset(&nip, 0, sizeof(nip));
    nip.ino = nino;
    nip.mode = mode_to_disk(&fs->fmt, mode, FILSYS_FT_REG);
    nip.nlink = 1;
    nip.uid = (int16_t)uid;
    nip.gid = (int16_t)gid;
    nip.atime = nip.mtime = nip.ctime = (uint32_t)time(NULL);
    rc = write_inode(fs, nino, &nip);
    if (rc) {
        ifree(fs, nino);
        return rc;
    }
    rc = dir_add(fs, &ddir, nino, name);
    if (rc) {
        /* The directory entry never landed: clear the inode, then return it to
         * the free list -- and only if the clear persisted.  If the clear write
         * fails, leave the inode allocated: a recoverable leak beats freeing
         * live on-disk state into the allocator. */
        nip.mode = 0;
        if (write_inode(fs, nino, &nip) == 0)
            ifree(fs, nino);
    }
    return rc;
}

int filsys_mkdir(filsys_t *fs, const char *path, mode_t mode, uid_t uid, gid_t gid) {
    int rc = uidgid_fit(uid, gid);
    if (rc) return rc;
    char dir[PATH_MAX], name[64];
    rc = split_path(path, dir, sizeof(dir), name, fs->fmt.max_namlen + 1);
    if (rc) return rc;
    filsys_inode_t ddir;
    uint32_t dino;
    rc = lookup(fs, dir, &dino, &ddir);
    if (rc) return rc;
    uint32_t nino;
    rc = ialloc(fs, &nino);
    if (rc) return rc;
    filsys_inode_t nip;
    memset(&nip, 0, sizeof(nip));
    nip.ino = nino;
    nip.mode = mode_to_disk(&fs->fmt, mode, FILSYS_FT_DIR);
    nip.nlink = 2;
    nip.uid = (int16_t)uid;
    nip.gid = (int16_t)gid;
    nip.atime = nip.mtime = nip.ctime = (uint32_t)time(NULL);
    rc = write_inode(fs, nino, &nip);
    if (rc) { ifree(fs, nino); return rc; }
    rc = dir_add(fs, &nip, nino, ".");
    if (rc) goto fail;
    rc = dir_add(fs, &nip, dino, "..");
    if (rc) goto fail;
    rc = dir_add(fs, &ddir, nino, name);
    if (rc) goto fail;
    ddir.nlink++;
    rc = write_inode(fs, dino, &ddir);
    if (rc) {
        /* The bump didn't persist, but dir_remove rewrites the parent's inode
         * from the in-memory ddir (whose nlink is now bumped).  Undo the bump
         * first so the parent doesn't count a child that's about to be freed. */
        ddir.nlink--;
        dir_remove(fs, &ddir, name);
        goto fail;
    }
    return 0;
fail:
    /* The parent entry never landed (or its update failed): free the new
     * directory's "." and ".." blocks, then its inode, so it isn't orphaned.
     * Only free the inode once the cleared state has persisted; otherwise leave
     * it allocated -- a recoverable leak, not live state returned to the free
     * list. */
    {
        filsys_blklist_t *bl = NULL;
        itrunc(fs, &nip, &bl);          /* best-effort; the dir is freshly made */
        nip.mode = 0;
        if (write_inode(fs, nino, &nip) == 0) {
            filsys_blklist_drain(fs->fs, bl);
            ifree(fs, nino);
        } else {
            filsys_blklist_discard(bl);
        }
    }
    return rc;
}

int filsys_mknod(filsys_t *fs, const char *path, mode_t mode, dev_t rdev,
                 uid_t uid, gid_t gid) {
    if (uidgid_fit(uid, gid))
        return -EINVAL;
    /* V1 has no device type bits: devices are the fixed inodes 1..40, wired up
     * by the kernel at boot (u0.s), not created with mknod(2). */
    if (fs->ver == FILSYS_V1)
        return -EPERM;
    /* A FIFO has no on-disk type in any of these editions (named pipes arrived
     * in System III); reject it rather than deposit a bogus char device.  A
     * regular file is accepted: OpenBSD's VFS routes O_CREAT through mknod(2)
     * rather than a separate create callback, and Linux never does, so this is
     * one behaviour for both platforms. */
    int ischr = (mode & S_IFMT) == S_IFCHR;
    int isblk = (mode & S_IFMT) == S_IFBLK;
    int isreg = (mode & S_IFMT) == S_IFREG;
    if (!ischr && !isblk && !isreg)
        return -EPERM;
    if (isreg)
        return filsys_create(fs, path, mode & 07777, uid, gid);
    /* V7 device numbers are 8-bit major + 8-bit minor packed into one word.
     * Reject rather than mask: a modern major like 300 would otherwise
     * silently become 44. */
    if (major(rdev) > 255 || minor(rdev) > 255)
        return -EINVAL;

    char dir[PATH_MAX], name[64];
    int rc = split_path(path, dir, sizeof(dir), name, fs->fmt.max_namlen + 1);
    if (rc) return rc;
    filsys_inode_t ddir;
    uint32_t dino;
    rc = lookup(fs, dir, &dino, &ddir);
    if (rc) return rc;
    uint32_t nino;
    rc = ialloc(fs, &nino);
    if (rc) return rc;
    filsys_inode_t nip;
    memset(&nip, 0, sizeof(nip));
    nip.ino = nino;
    nip.mode = mode_to_disk(&fs->fmt, mode, isblk ? FILSYS_FT_BLK : FILSYS_FT_CHR);
    nip.nlink = 1;
    nip.uid = (int16_t)uid;
    nip.gid = (int16_t)gid;
    nip.atime = nip.mtime = nip.ctime = (uint32_t)time(NULL);
    /* Device number: (major<<8)|minor, stored in di_addr[0]. */
    nip.addr[0] = (uint32_t)((major(rdev) << 8) | minor(rdev));
    rc = write_inode(fs, nino, &nip);
    if (rc) { ifree(fs, nino); return rc; }
    rc = dir_add(fs, &ddir, nino, name);
    if (rc) {
        /* The directory entry never landed: clear the inode, then return it to
         * the free list -- and only if the clear persisted (see filsys_create). */
        nip.mode = 0;
        if (write_inode(fs, nino, &nip) == 0)
            ifree(fs, nino);
    }
    return rc;
}

static int open_refs(filsys_t *fs, uint32_t ino) {
    for (int i = 0; i < fs->nopen; i++)
        if (fs->opens[i].ino == ino)
            return fs->opens[i].refs;
    return 0;
}
static void mark_pending(filsys_t *fs, uint32_t ino) {
    for (int i = 0; i < fs->nopen; i++)
        if (fs->opens[i].ino == ino)
            fs->opens[i].pending = 1;
}

static int do_unlink(filsys_t *fs, const char *dirpath, const char *name) {
    filsys_inode_t ddir;
    uint32_t dino;
    int rc = lookup(fs, dirpath, &dino, &ddir);
    if (rc) return rc;
    uint32_t ino;
    rc = dir_lookup(fs, &ddir, name, &ino);
    if (rc) return rc;
    filsys_inode_t ip;
    if (read_inode(fs, ino, &ip)) return -EIO;
    rc = dir_remove(fs, &ddir, name);
    if (rc) return rc;
    ip.nlink--;
    filsys_blklist_t *bl = NULL;
    int unlinked = ip.nlink <= 0;
    /* hard_remove: the name is gone but an open handle still holds the inode;
     * keep its blocks and defer the free to the final close. */
    int deferred = unlinked && open_refs(fs, ino) > 0;
    if (unlinked && !deferred) {
        rc = itrunc(fs, &ip, &bl);
        if (rc) {
            filsys_blklist_discard(bl);
            return rc;
        }
        ip.mode = 0;
    }
    rc = write_inode(fs, ino, &ip);
    if (rc) {
        filsys_blklist_discard(bl);     /* inode not persisted: blocks stay referenced */
        return rc;
    }
    filsys_blklist_drain(fs->fs, bl);
    if (unlinked && !deferred)
        ifree(fs, ino);                 /* only after the cleared inode is durable */
    else if (deferred)
        mark_pending(fs, ino);
    return 0;
}

int filsys_unlink(filsys_t *fs, const char *path) {
    char dir[PATH_MAX], name[64];
    int rc = split_path(path, dir, sizeof(dir), name, fs->fmt.max_namlen + 1);
    if (rc) return rc;
    filsys_inode_t ip;
    uint32_t ino;
    rc = lookup(fs, path, &ino, &ip);
    if (rc) return rc;
    if (mode_is_dir(&fs->fmt, &ip)) return -EISDIR;
    return do_unlink(fs, dir, name);
}

int filsys_rmdir(filsys_t *fs, const char *path) {
    char dir[PATH_MAX], name[64];
    int rc = split_path(path, dir, sizeof(dir), name, fs->fmt.max_namlen + 1);
    if (rc) return rc;
    filsys_inode_t ddir;
    uint32_t dino;
    rc = lookup(fs, dir, &dino, &ddir);
    if (rc) return rc;
    uint32_t ino;
    rc = dir_lookup(fs, &ddir, name, &ino);
    if (rc) return rc;
    filsys_inode_t tip;
    if (read_inode(fs, ino, &tip)) return -EIO;
    if (!mode_is_dir(&fs->fmt, &tip)) return -ENOTDIR;
    filsys_dirent_t *ents = NULL;
    size_t count = 0;
    if (dir_read(fs, &tip, &ents, &count)) return -EIO;
    for (size_t i = 0; i < count; i++)
        if (strcmp(ents[i].name, ".") && strcmp(ents[i].name, "..")) { free(ents); return -ENOTEMPTY; }
    free(ents);
    /* One fewer name points at the directory now.  Its nlink counts its own "."
     * plus each parent entry (a historical directory may be hard-linked); it is
     * orphaned -- and freed -- only when "." alone remains (nlink <= 1). */
    tip.nlink--;
    if (tip.nlink > 1) {
        /* Still linked elsewhere: drop just this name and persist the child's
         * decremented link count.  The parent's nlink is unchanged -- the
         * child's ".." entry survives. */
        rc = dir_remove(fs, &ddir, name);
        if (rc) return rc;
        return write_inode(fs, ino, &tip);
    }
    /* Orphaned: removing it also drops the child's ".." from the parent. */
    ddir.nlink--;
    rc = dir_remove(fs, &ddir, name);
    if (rc) {
        ddir.nlink++;   /* the entry wasn't removed: undo the decrement */
        return rc;
    }
    filsys_blklist_t *bl = NULL;
    rc = itrunc(fs, &tip, &bl);
    if (rc) {
        filsys_blklist_discard(bl);
        return rc;
    }
    tip.mode = 0;
    rc = write_inode(fs, ino, &tip);
    if (rc) {
        filsys_blklist_discard(bl);     /* inode not persisted: blocks stay referenced */
        return rc;
    }
    filsys_blklist_drain(fs->fs, bl);
    ifree(fs, ino);                     /* only after the cleared inode is durable */
    return 0;
}

/* Is `anc_ino` an ancestor of directory `dir_ino` (does dir_ino sit in
 * anc_ino's subtree)?  Walk ".." from dir_ino up to the root.  A directory
 * linked or renamed into its own subtree would make a cycle that lookup and
 * fsck would then chase forever, so both operations refuse it.
 *
 * Returns 1 if anc_ino is an ancestor, 0 if dir_ino provably reaches the root
 * without meeting it, or a negative errno when the walk cannot be completed.
 * Callers must fail closed: a negative result is not "safe", it is "unknown". */
static int dir_ancestor(filsys_t *fs, uint32_t anc_ino, uint32_t dir_ino) {
    uint32_t cur = dir_ino, guard = 0;
    while (cur != (uint32_t)fs->fmt.rootino && cur != 0 && guard++ < 65536) {
        if (cur == anc_ino)
            return 1;
        filsys_inode_t ip;
        uint32_t next;
        int rc = read_inode(fs, cur, &ip);
        if (rc)
            return rc;
        rc = dir_lookup(fs, &ip, "..", &next);
        if (rc)
            return rc;
        cur = next;
    }
    if (guard >= 65536)
        return -ELOOP;   /* parent chain loops: cannot establish safety */
    return 0;
}

static int do_link(filsys_t *fs, const char *dst, uint32_t src_ino) {
    char dir[PATH_MAX], name[64];
    int rc = split_path(dst, dir, sizeof(dir), name, fs->fmt.max_namlen + 1);
    if (rc) return rc;
    filsys_inode_t ddir;
    uint32_t dino;
    rc = lookup(fs, dir, &dino, &ddir);
    if (rc) return rc;
    filsys_inode_t ip;
    if (read_inode(fs, src_ino, &ip)) return -EIO;
    /* A directory may be hard-linked (V7 lets the superuser do it), but never
     * into its own subtree -- that would form a cycle. */
    if (mode_is_dir(&fs->fmt, &ip)) {
        int a = dir_ancestor(fs, src_ino, dino);
        if (a == 1) return -EINVAL;   /* would form a cycle */
        if (a < 0) return a;          /* cannot verify: fail closed */
    }
    rc = dir_add(fs, &ddir, src_ino, name);
    if (rc) return rc;
    ip.nlink++;
    rc = write_inode(fs, src_ino, &ip);
    if (rc)
        dir_remove(fs, &ddir, name);   /* the link count didn't persist */
    return rc;
}

int filsys_link(filsys_t *fs, const char *from, const char *to) {
    filsys_inode_t ip;
    uint32_t ino;
    int rc = lookup(fs, from, &ino, &ip);
    if (rc) return rc;
    /* No directory check here, on purpose: V7's link(2) lets the superuser
     * hard-link a directory.  That path is unreachable through .link on Linux
     * -- the kernel's vfs_link() refuses directory links before FUSE is
     * consulted -- but do_link is also rename()'s implementation, so it must
     * keep handling directories. */
    return do_link(fs, to, ino);
}

/* rename(2) flags (the Linux renameat2 ABI; frozen values).  FUSE3 passes them
 * on Linux; FUSE2 (OpenBSD) passes none.  filsys supports only NOREPLACE --
 * the kernel's silly-rename for unlink-of-open-file uses it, so the descriptor
 * (now ino-keyed) can outlive the directory entry. */
enum { FS_RENAME_NOREPLACE = 1 };

int filsys_rename(filsys_t *fs, const char *from, const char *to, unsigned int flags) {
    if (flags & ~FS_RENAME_NOREPLACE) return -EINVAL;   /* EXCHANGE/unknown unsupported */
    if (!strcmp(from, to)) return 0;
    filsys_inode_t sip;
    uint32_t sino;
    int rc = lookup(fs, from, &sino, &sip);
    if (rc) return rc;

    int isdir = mode_is_dir(&fs->fmt, &sip);

    char fdir[PATH_MAX], fname[64];
    split_path(from, fdir, sizeof(fdir), fname, fs->fmt.max_namlen + 1);
    char tdir[PATH_MAX], tname[64];
    rc = split_path(to, tdir, sizeof(tdir), tname, fs->fmt.max_namlen + 1);
    if (rc) return rc;

    filsys_inode_t tdirip;
    uint32_t tdino;
    rc = lookup(fs, tdir, &tdino, &tdirip);
    if (rc) return rc;

    /* Every check that can fail without mutating goes here, before the first
     * persistent change: a directory must not be renamed into its own subtree
     * (that would form a cycle). */
    if (isdir) {
        int a = dir_ancestor(fs, sino, tdino);
        if (a == 1) return -EINVAL;   /* would form a cycle */
        if (a < 0) return a;          /* cannot verify: fail closed */
    }

    /* Overwrite an existing target.  Enforce the POSIX type rules: a directory
     * may only replace another directory (and then only an empty one), and a
     * non-directory may only replace a non-directory. */
    uint32_t tino = 0;
    filsys_inode_t tip_saved;          /* snapshot of the removed target */
    int had_target = 0;
    int free_target = 0;               /* orphaned target to free at commit */
    if (dir_lookup(fs, &tdirip, tname, &tino) == 0) {
        if (flags & FS_RENAME_NOREPLACE) return -EEXIST;   /* don't clobber */
        if (tino == sino) return 0;   /* already there */
        filsys_inode_t tip;
        if (read_inode(fs, tino, &tip)) return -EIO;
        int tisdir = mode_is_dir(&fs->fmt, &tip);
        if (isdir && !tisdir) return -ENOTDIR;   /* directory over a file */
        if (!isdir && tisdir) return -EISDIR;    /* file over a directory */
        if (tisdir) {
            filsys_dirent_t *ents = NULL; size_t count = 0;
            if (dir_read(fs, &tip, &ents, &count)) return -EIO;
            for (size_t i = 0; i < count; i++)
                if (strcmp(ents[i].name, ".") && strcmp(ents[i].name, "..")) { free(ents); return -ENOTEMPTY; }
            free(ents);
        }
        tip_saved = tip;
        had_target = 1;
        /* Drop the target's name now, but defer freeing its blocks and inode
         * until the rename is committed below.  A replaced file loses its link
         * count and is freed only if it reaches zero; a replaced directory is
         * orphaned by the removal and freed outright.  Nothing is freed yet, so
         * a rollback that re-adds the name and rewrites tip_saved stays clean. */
        rc = dir_remove(fs, &tdirip, tname);
        if (rc) return rc;
        if (tisdir) {
            free_target = 1;
        } else {
            tip.nlink--;
            rc = write_inode(fs, tino, &tip);
            if (rc) {
                dir_add(fs, &tdirip, tino, tname);   /* withdraw the removal */
                return rc;
            }
            free_target = tip.nlink == 0;
        }
    }

    /* The mutations that follow are not transactional (a non-journaled
     * filesystem has no atomic rename), so each is checked and the preceding
     * ones unwound best-effort on failure.  The replaced target is only freed at
     * the end (its blocks are still referenced until then), so a rollback that
     * re-adds its name and inode leaves no aliasing. */
    rc = do_link(fs, to, sino);
    if (rc) {
        if (had_target) {
            dir_add(fs, &tdirip, tino, tname);
            write_inode(fs, tino, &tip_saved);
        }
        return rc;
    }

    rc = do_unlink(fs, fdir, fname);
    if (rc) {
        do_unlink(fs, tdir, tname);   /* withdraw the link just added */
        if (had_target) {
            dir_add(fs, &tdirip, tino, tname);
            write_inode(fs, tino, &tip_saved);
        }
        return rc;
    }

    if (isdir) {
        /* Moving a directory: fix the two parents' link counts and rewrite the
         * moved directory's '..' entry to point at its new parent.  Unlike the
         * link/unlink steps above, these fixups are deliberately *not* unwound
         * on failure: a failure here leaves only a link-count / ".."
         * inconsistency that fsck repairs (a recoverable leak, not block
         * aliasing), and unwinding them would itself be a multi-step best-effort
         * that can fail the same way.  The crash-prefix test confirms every
         * prefix here stays alias-free (dup==0) and salvage-recoverable. */
        if (strcmp(fdir, tdir) != 0) {
            filsys_inode_t fddir;
            uint32_t fdino;
            if (lookup(fs, fdir, &fdino, &fddir) == 0) {
                if (fddir.nlink > 1) fddir.nlink--;
                rc = write_inode(fs, fdino, &fddir);
                if (rc) return rc;
            }
            /* tdirip was read before do_link, which rewrote the target
             * directory's inode (its size grew to hold the new entry).  Re-read
             * it fresh so we bump nlink without clobbering that update. */
            if (read_inode(fs, tdino, &tdirip) == 0) {
                tdirip.nlink++;
                rc = write_inode(fs, tdino, &tdirip);
                if (rc) return rc;
            }
        }
        filsys_inode_t cip;
        if (read_inode(fs, sino, &cip) == 0) {
            rc = dir_remove(fs, &cip, "..");
            if (rc) return rc;
            rc = dir_add(fs, &cip, tdino, "..");
            if (rc) return rc;
        }
    }

    /* Commit: the new name is in place and the old one is gone.  Only now free
     * the replaced target we orphaned above (tip_saved still holds its blocks).
     * On a failure here it stays an orphan -- a recoverable leak, not aliasing. */
    if (free_target) {
        if (open_refs(fs, tino) > 0) {
            /* hard_remove: the replaced target is still open.  Its nlink is
             * already persisted at 0 (for a file), so keep its mode non-zero --
             * mode != 0 is the on-disk invariant that stops ialloc from handing
             * the number back out -- and defer the block/inode free to the last
             * close.  Clearing mode here would let the next create() alias the
             * still-open handle. */
            mark_pending(fs, tino);
        } else {
            filsys_blklist_t *bl = NULL;
            if (itrunc(fs, &tip_saved, &bl) == 0) {
                tip_saved.mode = 0;
                if (write_inode(fs, tino, &tip_saved) == 0) {
                    filsys_blklist_drain(fs->fs, bl);
                    ifree(fs, tino);
                } else {
                    filsys_blklist_discard(bl);
                }
            } else {
                filsys_blklist_discard(bl);
            }
        }
    }
    return 0;
}

/* Shared truncate body: shrink or extend inode `ino` (whose decoded state is
 * `ip`) to `size` bytes.  Extension is a no-op (holes read back as zero). */
static int truncate_inode(filsys_t *fs, uint32_t ino, filsys_inode_t *ip, off_t size) {
    /* Reject sizes the format cannot address, rather than wrap a 5 GiB request
     * to 1 GiB via the uint32_t cast below (V7's ceiling is ~1.08 GB). */
    if (size < 0 || (uint64_t)size > maxfile(fs))
        return -EFBIG;
    uint32_t newsize = (uint32_t)size, oldsize = ip->size;
    if (newsize == oldsize) return 0;

    /* Shrink in place: zero the partial tail of the last surviving block, free
     * the blocks strictly past the new end, and leave everything else alone. */
    if (newsize < oldsize) {
        uint32_t bsize = fs->ops->blocksize(fs->fs);
        uint32_t last  = newsize ? (newsize - 1) / bsize : 0;
        uint32_t blk_end = newsize ? (last + 1) * bsize : 0;
        uint32_t tail = oldsize < blk_end ? oldsize : blk_end;
        /* Zero the partial tail of the last surviving block through the file
         * layer, so word-addressed backends (PDP-7, 128-byte logical blocks)
         * pack the zeros correctly.  Skip it if that block is a hole -- it
         * already reads as zero and file_write would only allocate it. */
        if (newsize < tail) {
            uint32_t bno;
            if (bmap(fs, ip, last, 0, &bno) == 0 && bno != 0) {
                uint8_t zeros[V7_MAXBSIZE] = {0};  /* up to the largest block size */
                ssize_t w = file_write(fs, ip, zeros, tail - newsize, newsize);
                if (w < 0) return (int)w;
            }
        }
        filsys_blklist_t *bl = NULL;
        int rc = itrunc_from(fs, ip, newsize ? last + 1 : 0, &bl);
        if (rc) {
            filsys_blklist_discard(bl);
            return rc;
        }
        ip->size = newsize;
        rc = write_inode(fs, ino, ip);
        if (rc) {
            filsys_blklist_discard(bl);
            return rc;
        }
        filsys_blklist_drain(fs->fs, bl);
        return 0;
    }

    ip->size = newsize;
    return write_inode(fs, ino, ip);
}

int filsys_truncate(filsys_t *fs, const char *path, off_t size) {
    filsys_inode_t ip;
    uint32_t ino;
    int rc = lookup(fs, path, &ino, &ip);
    if (rc) return rc;
    return truncate_inode(fs, ino, &ip, size);
}

int filsys_truncate_ino(filsys_t *fs, uint32_t ino, off_t size) {
    filsys_inode_t ip;
    int rc = read_inode(fs, ino, &ip);
    if (rc) return rc;
    return truncate_inode(fs, ino, &ip, size);
}

int filsys_stat_ino(filsys_t *fs, uint32_t ino, struct stat *st) {
    filsys_inode_t ip;
    int rc = read_inode(fs, ino, &ip);
    if (rc) return rc;
    filsys_fill_stat(fs, &ip, st);
    return 0;
}

ssize_t filsys_readlink(filsys_t *fs, const char *path, char *buf, size_t size) {
    if (!fs->fmt.iflnk)
        return -ENOSYS;   /* this edition predates symlinks */
    filsys_inode_t ip;
    uint32_t ino;
    int rc = lookup(fs, path, &ino, &ip);
    if (rc) return rc;
    if ((ip.mode & fs->fmt.ifmt) != fs->fmt.iflnk)
        return -EINVAL;
    size_t n = ip.size < size ? ip.size : size;
    if (n == 0)
        return 0;
    return file_read(fs, &ip, (uint8_t *)buf, n, 0);
}

int filsys_symlink(filsys_t *fs, const char *target, const char *linkpath) {
    if (!fs->fmt.iflnk)
        return -ENOSYS;   /* this edition predates symlinks */
    int rc = uidgid_fit(fs->uid, fs->gid);
    if (rc) return rc;
    char dir[PATH_MAX], name[64];
    rc = split_path(linkpath, dir, sizeof(dir), name, fs->fmt.max_namlen + 1);
    if (rc) return rc;
    filsys_inode_t ddir;
    uint32_t dino;
    rc = lookup(fs, dir, &dino, &ddir);
    if (rc) return rc;
    uint32_t nino;
    rc = ialloc(fs, &nino);
    if (rc) return rc;
    filsys_inode_t nip;
    memset(&nip, 0, sizeof(nip));
    nip.ino = nino;
    nip.mode = mode_to_disk(&fs->fmt, 0777, FILSYS_FT_LNK);
    nip.nlink = 1;
    nip.uid = (int16_t)fs->uid;
    nip.gid = (int16_t)fs->gid;
    nip.atime = nip.mtime = nip.ctime = (uint32_t)time(NULL);
    rc = write_inode(fs, nino, &nip);
    if (rc) { ifree(fs, nino); return rc; }
    /* The target is the file's data (a "slow" symlink): a short write. */
    ssize_t w = file_write(fs, &nip, (const uint8_t *)target, strlen(target), 0);
    if (w != (ssize_t)strlen(target)) {
        nip.mode = 0;
        if (write_inode(fs, nino, &nip) == 0)
            ifree(fs, nino);
        return w < 0 ? (int)w : -EIO;
    }
    rc = dir_add(fs, &ddir, nino, name);
    if (rc) {
        nip.mode = 0;
        if (write_inode(fs, nino, &nip) == 0)
            ifree(fs, nino);
    }
    return rc;
}

int filsys_chmod(filsys_t *fs, const char *path, mode_t mode) {
    filsys_inode_t ip;
    uint32_t ino;
    int rc = lookup(fs, path, &ino, &ip);
    if (rc) return rc;
    ip.mode = mode_chmod(&fs->fmt, ip.mode, mode);
    ip.ctime = (uint32_t)time(NULL);
    return write_inode(fs, ino, &ip);
}

int filsys_chown(filsys_t *fs, const char *path, uid_t uid, gid_t gid) {
    if ((uid != (uid_t)-1 && uid > INT16_MAX) ||
        (gid != (gid_t)-1 && gid > INT16_MAX))
        return -EINVAL;
    filsys_inode_t ip;
    uint32_t ino;
    int rc = lookup(fs, path, &ino, &ip);
    if (rc) return rc;
    if (uid != (uid_t)-1) ip.uid = (int16_t)uid;
    if (gid != (gid_t)-1) ip.gid = (int16_t)gid;
    ip.ctime = (uint32_t)time(NULL);
    return write_inode(fs, ino, &ip);
}

int filsys_utimens(filsys_t *fs, const char *path, const struct timespec tv[2]) {
    filsys_inode_t ip;
    uint32_t ino;
    int rc = lookup(fs, path, &ino, &ip);
    if (rc) return rc;
    uint32_t now = (uint32_t)time(NULL);
    if (!tv) {
        ip.atime = ip.mtime = now;
    } else {
        if (tv[0].tv_nsec != UTIME_OMIT)
            ip.atime = (tv[0].tv_nsec == UTIME_NOW) ? now : (uint32_t)tv[0].tv_sec;
        if (tv[1].tv_nsec != UTIME_OMIT)
            ip.mtime = (tv[1].tv_nsec == UTIME_NOW) ? now : (uint32_t)tv[1].tv_sec;
    }
    ip.ctime = now;
    return write_inode(fs, ino, &ip);
}

int filsys_statfs(filsys_t *fs, struct statvfs *st) {
    memset(st, 0, sizeof(*st));
    st->f_bsize = st->f_frsize = fs->ops->blocksize(fs->fs);
    fs->ops->statfs(fs->fs, st);
    st->f_namemax = fs->fmt.max_namlen;
    return 0;
}
