/* v7fuse 0.1.0 — 2026-08-26 — Copyright (C) 2026 David Walther */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* v7mount.c — FUSE driver exposing a V7 (PDP-11) filesystem image.
 *
 * Usage:
 *     v7mount [options] <image> <mountpoint>
 *
 * Options:
 *     -r   open the image read-only
 *     -f   stay in the foreground
 *     -d   FUSE debug output
 *
 * Unmount with:
 *     fusermount3 -u <mountpoint>
 *
 * Coordination with the emulator: while mounted, this process holds an
 * exclusive flock on the image file.  The patched simulator takes that same
 * lock around each of its disk accesses, so it blocks (pauses) for the whole
 * time the image is mounted and resumes when we unmount.
 */
#include "v7fs.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <fuse.h>

/* ---- mode conversion ---------------------------------------------------- */

static mode_t v7_to_posix_mode(uint16_t mode) {
    mode_t m = mode & 07777;
    switch (mode & V7_IFMT) {
    case V7_IFDIR:            m |= S_IFDIR; break;
    case V7_IFREG:            m |= S_IFREG; break;
    case V7_IFCHR: case V7_IFMPC: m |= S_IFCHR; break;
    case V7_IFBLK: case V7_IFMPB: m |= S_IFBLK; break;
    default:                  m |= S_IFREG; break;
    }
    return m;
}

/* Return the V7 permission bits only; the caller ORs in the file type. */
static uint16_t posix_to_v7_mode(mode_t m) {
    return (uint16_t)(m & 07777);
}

/* ---- helpers ------------------------------------------------------------ */

static v7fs_t *fs_ctx(void) {
    return (v7fs_t *)fuse_get_context()->private_data;
}

/* Split "/a/b/c" into dir="/a/b" and name="c" (out buffers must be writable). */
static int split_path(const char *path, char *dir, size_t dirsz,
                      char *name, size_t namesz) {
    const char *slash = strrchr(path, '/');
    if (!slash)
        return -EINVAL;
    size_t dlen = (size_t)(slash - path);
    if (dlen == 0)
        dlen = 1;                      /* "/name" -> dir "/" */
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

static void fill_stat(const v7_inode_t *ip, struct stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_ino   = ip->ino;
    st->st_mode  = v7_to_posix_mode(ip->mode);
    st->st_nlink = ip->nlink;
    st->st_uid   = ip->uid;
    st->st_gid   = ip->gid;
    st->st_size  = ip->size;
    uint16_t t = ip->mode & V7_IFMT;
    if (t == V7_IFCHR || t == V7_IFBLK || t == V7_IFMPC || t == V7_IFMPB)
        st->st_rdev = ip->addr[0];      /* packed major<<8 | minor */
    st->st_atime   = ip->atime;
    st->st_mtime   = ip->mtime;
    st->st_ctime   = ip->ctime;
    st->st_blksize = V7_BSIZE;
    st->st_blocks  = (ip->size + V7_BSIZE - 1) / V7_BSIZE;
}

/* ---- callbacks ---------------------------------------------------------- */

static int v7_getattr(const char *path, struct stat *st,
                      struct fuse_file_info *fi) {
    (void)fi;
    v7fs_t *fs = fs_ctx();
    v7_inode_t ip;
    uint32_t ino;
    int rc = v7fs_lookup(fs, path, &ino, &ip);
    if (rc)
        return rc;
    fill_stat(&ip, st);
    return 0;
}

static int v7_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      off_t off, struct fuse_file_info *fi,
                      enum fuse_readdir_flags flags) {
    (void)off; (void)fi; (void)flags;
    v7fs_t *fs = fs_ctx();
    v7_inode_t ip;
    uint32_t ino;
    int rc = v7fs_lookup(fs, path, &ino, &ip);
    if (rc)
        return rc;
    v7_dirent_t *ents = NULL;
    size_t count = 0;
    rc = v7fs_dir_read(fs, &ip, &ents, &count);
    if (rc)
        return rc;
    for (size_t i = 0; i < count; i++) {
        struct stat st;
        v7_inode_t eip;
        if (v7fs_read_inode(fs, ents[i].ino, &eip) == 0)
            fill_stat(&eip, &st);
        else
            memset(&st, 0, sizeof(st));
        if (filler(buf, ents[i].name, &st, 0, 0))
            break;
    }
    v7fs_dirents_free(ents);
    return 0;
}

static int v7_open(const char *path, struct fuse_file_info *fi) {
    v7fs_t *fs = fs_ctx();
    v7_inode_t ip;
    uint32_t ino;
    int rc = v7fs_lookup(fs, path, &ino, &ip);
    if (rc)
        return rc;
    if ((ip.mode & V7_IFMT) == V7_IFDIR)
        return -EISDIR;
    if ((fi->flags & O_ACCMODE) != O_RDONLY && fs->readonly)
        return -EROFS;
    return 0;
}

static int v7_read(const char *path, char *buf, size_t size, off_t off,
                   struct fuse_file_info *fi) {
    (void)fi;
    v7fs_t *fs = fs_ctx();
    v7_inode_t ip;
    uint32_t ino;
    int rc = v7fs_lookup(fs, path, &ino, &ip);
    if (rc)
        return rc;
    return (int)v7fs_file_read(fs, &ip, (uint8_t *)buf, size, off);
}

static int v7_write(const char *path, const char *buf, size_t size, off_t off,
                    struct fuse_file_info *fi) {
    (void)fi;
    v7fs_t *fs = fs_ctx();
    v7_inode_t ip;
    uint32_t ino;
    int rc = v7fs_lookup(fs, path, &ino, &ip);
    if (rc)
        return rc;
    return (int)v7fs_file_write(fs, &ip, (const uint8_t *)buf, size, off);
}

static int v7_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)fi;
    v7fs_t *fs = fs_ctx();
    char dir[PATH_MAX], name[V7_DIRSIZ + 1];
    int rc = split_path(path, dir, sizeof(dir), name, sizeof(name));
    if (rc)
        return rc;

    v7_inode_t ddir;
    uint32_t dino;
    rc = v7fs_lookup(fs, dir, &dino, &ddir);
    if (rc)
        return rc;

    uint32_t nino;
    rc = v7fs_ialloc(fs, &nino);
    if (rc)
        return rc;

    const struct fuse_context *ctx = fuse_get_context();
    v7_inode_t nip;
    memset(&nip, 0, sizeof(nip));
    nip.ino   = nino;
    nip.mode  = posix_to_v7_mode(mode) | V7_IFREG;
    nip.nlink = 1;
    nip.uid   = (int16_t)ctx->uid;
    nip.gid   = (int16_t)ctx->gid;
    nip.atime = nip.mtime = nip.ctime = (uint32_t)time(NULL);
    v7fs_write_inode(fs, nino, &nip);

    rc = v7fs_dir_add(fs, &ddir, nino, name);
    return rc;
}

static int v7_mkdir(const char *path, mode_t mode) {
    v7fs_t *fs = fs_ctx();
    char dir[PATH_MAX], name[V7_DIRSIZ + 1];
    int rc = split_path(path, dir, sizeof(dir), name, sizeof(name));
    if (rc)
        return rc;

    v7_inode_t ddir;
    uint32_t dino;
    rc = v7fs_lookup(fs, dir, &dino, &ddir);
    if (rc)
        return rc;

    uint32_t nino;
    rc = v7fs_ialloc(fs, &nino);
    if (rc)
        return rc;

    const struct fuse_context *ctx = fuse_get_context();
    v7_inode_t nip;
    memset(&nip, 0, sizeof(nip));
    nip.ino   = nino;
    nip.mode  = posix_to_v7_mode(mode) | V7_IFDIR;
    nip.nlink = 2;                    /* "." and parent's ".." */
    nip.uid   = (int16_t)ctx->uid;
    nip.gid   = (int16_t)ctx->gid;
    nip.atime = nip.mtime = nip.ctime = (uint32_t)time(NULL);
    v7fs_write_inode(fs, nino, &nip);

    v7fs_dir_add(fs, &nip, nino, ".");
    v7fs_dir_add(fs, &nip, dino, "..");

    rc = v7fs_dir_add(fs, &ddir, nino, name);
    if (rc)
        return rc;
    ddir.nlink++;
    v7fs_write_inode(fs, dino, &ddir);
    return 0;
}

/* Unlink `name` from directory `dirpath` (shared by unlink and rename). */
static int v7_do_unlink(v7fs_t *fs, const char *dirpath, const char *name) {
    v7_inode_t ddir;
    uint32_t dino;
    int rc = v7fs_lookup(fs, dirpath, &dino, &ddir);
    if (rc)
        return rc;

    uint32_t ino;
    rc = v7fs_dir_lookup(fs, &ddir, name, &ino);
    if (rc)
        return rc;

    v7_inode_t ip;
    if (v7fs_read_inode(fs, ino, &ip))
        return -EIO;

    rc = v7fs_dir_remove(fs, &ddir, name);
    if (rc)
        return rc;

    ip.nlink--;
    if (ip.nlink <= 0) {
        v7fs_itrunc(fs, &ip);
        ip.mode = 0;
        v7fs_ifree(fs, ino);
    }
    v7fs_write_inode(fs, ino, &ip);
    return 0;
}

static int v7_unlink(const char *path) {
    v7fs_t *fs = fs_ctx();
    char dir[PATH_MAX], name[V7_DIRSIZ + 1];
    int rc = split_path(path, dir, sizeof(dir), name, sizeof(name));
    if (rc)
        return rc;
    /* refuse to unlink a directory through this path */
    v7_inode_t ip;
    uint32_t ino;
    rc = v7fs_lookup(fs, path, &ino, &ip);
    if (rc)
        return rc;
    if ((ip.mode & V7_IFMT) == V7_IFDIR)
        return -EISDIR;
    return v7_do_unlink(fs, dir, name);
}

static int v7_rmdir(const char *path) {
    v7fs_t *fs = fs_ctx();
    char dir[PATH_MAX], name[V7_DIRSIZ + 1];
    int rc = split_path(path, dir, sizeof(dir), name, sizeof(name));
    if (rc)
        return rc;

    v7_inode_t ddir;
    uint32_t dino;
    rc = v7fs_lookup(fs, dir, &dino, &ddir);
    if (rc)
        return rc;

    uint32_t ino;
    rc = v7fs_dir_lookup(fs, &ddir, name, &ino);
    if (rc)
        return rc;

    v7_inode_t tip;
    if (v7fs_read_inode(fs, ino, &tip))
        return -EIO;
    if ((tip.mode & V7_IFMT) != V7_IFDIR)
        return -ENOTDIR;

    /* must be empty (only "." and "..") */
    v7_dirent_t *ents = NULL;
    size_t count = 0;
    if (v7fs_dir_read(fs, &tip, &ents, &count)) {
        return -EIO;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(ents[i].name, ".") && strcmp(ents[i].name, "..")) {
            v7fs_dirents_free(ents);
            return -ENOTEMPTY;
        }
    }
    v7fs_dirents_free(ents);

    rc = v7fs_dir_remove(fs, &ddir, name);
    if (rc)
        return rc;
    ddir.nlink--;
    v7fs_write_inode(fs, dino, &ddir);

    v7fs_itrunc(fs, &tip);
    tip.mode = 0;
    v7fs_ifree(fs, ino);
    v7fs_write_inode(fs, ino, &tip);
    return 0;
}

static int v7_do_link(v7fs_t *fs, const char *dstpath, uint32_t src_ino) {
    char dir[PATH_MAX], name[V7_DIRSIZ + 1];
    int rc = split_path(dstpath, dir, sizeof(dir), name, sizeof(name));
    if (rc)
        return rc;
    v7_inode_t ddir;
    uint32_t dino;
    rc = v7fs_lookup(fs, dir, &dino, &ddir);
    if (rc)
        return rc;
    rc = v7fs_dir_add(fs, &ddir, src_ino, name);
    if (rc)
        return rc;

    v7_inode_t ip;
    if (v7fs_read_inode(fs, src_ino, &ip))
        return -EIO;
    ip.nlink++;
    v7fs_write_inode(fs, src_ino, &ip);
    return 0;
}

static int v7_rename(const char *from, const char *to, unsigned int flags) {
    if (flags)
        return -EINVAL;
    if (!strcmp(from, to))
        return 0;
    v7fs_t *fs = fs_ctx();

    v7_inode_t sip;
    uint32_t sino;
    int rc = v7fs_lookup(fs, from, &sino, &sip);
    if (rc)
        return rc;

    /* Replace an existing target first. */
    char tdir[PATH_MAX], tname[V7_DIRSIZ + 1];
    rc = split_path(to, tdir, sizeof(tdir), tname, sizeof(tname));
    if (rc)
        return rc;
    v7_inode_t tdirip;
    uint32_t tdino;
    rc = v7fs_lookup(fs, tdir, &tdino, &tdirip);
    if (rc)
        return rc;
    uint32_t tino;
    if (v7fs_dir_lookup(fs, &tdirip, tname, &tino) == 0)
        v7_do_unlink(fs, tdir, tname);

    v7_do_link(fs, to, sino);

    char fdir[PATH_MAX], fname[V7_DIRSIZ + 1];
    split_path(from, fdir, sizeof(fdir), fname, sizeof(fname));
    return v7_do_unlink(fs, fdir, fname);
}

static int v7_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)fi;
    v7fs_t *fs = fs_ctx();
    v7_inode_t ip;
    uint32_t ino;
    int rc = v7fs_lookup(fs, path, &ino, &ip);
    if (rc)
        return rc;
    ip.mode = (ip.mode & V7_IFMT) | (uint16_t)(mode & 07777);
    ip.ctime = (uint32_t)time(NULL);
    v7fs_write_inode(fs, ino, &ip);
    return 0;
}

static int v7_chown(const char *path, uid_t uid, gid_t gid,
                    struct fuse_file_info *fi) {
    (void)fi;
    v7fs_t *fs = fs_ctx();
    v7_inode_t ip;
    uint32_t ino;
    int rc = v7fs_lookup(fs, path, &ino, &ip);
    if (rc)
        return rc;
    if (uid != (uid_t)-1)
        ip.uid = (int16_t)uid;
    if (gid != (gid_t)-1)
        ip.gid = (int16_t)gid;
    ip.ctime = (uint32_t)time(NULL);
    v7fs_write_inode(fs, ino, &ip);
    return 0;
}

static int v7_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
    (void)fi;
    v7fs_t *fs = fs_ctx();
    v7_inode_t ip;
    uint32_t ino;
    int rc = v7fs_lookup(fs, path, &ino, &ip);
    if (rc)
        return rc;
    uint32_t newsize = (uint32_t)size;
    uint32_t oldsize = ip.size;
    if (newsize == oldsize)
        return 0;

    uint32_t keep = newsize < oldsize ? newsize : oldsize;
    uint8_t *data = malloc(keep ? keep : 1);
    if (!data)
        return -ENOMEM;
    if (keep)
        v7fs_file_read(fs, &ip, data, keep, 0);

    v7fs_itrunc(fs, &ip);
    if (keep)
        v7fs_file_write(fs, &ip, data, keep, 0);
    if (newsize != keep) {
        ip.size = newsize;
        v7fs_write_inode(fs, ino, &ip);
    }
    free(data);
    return 0;
}

static int v7_utimens(const char *path, const struct timespec tv[2],
                      struct fuse_file_info *fi) {
    (void)fi;
    v7fs_t *fs = fs_ctx();
    v7_inode_t ip;
    uint32_t ino;
    int rc = v7fs_lookup(fs, path, &ino, &ip);
    if (rc)
        return rc;
    uint32_t now = (uint32_t)time(NULL);
    ip.atime = tv ? (uint32_t)tv[0].tv_sec : now;
    ip.mtime = tv ? (uint32_t)tv[1].tv_sec : now;
    ip.ctime = now;
    v7fs_write_inode(fs, ino, &ip);
    return 0;
}

static int v7_statfs(const char *path, struct statvfs *st) {
    (void)path;
    v7fs_t *fs = fs_ctx();
    memset(st, 0, sizeof(*st));
    st->f_bsize   = V7_BSIZE;
    st->f_frsize  = V7_BSIZE;
    st->f_blocks  = fs->fsize;
    st->f_bfree   = fs->nfree;   /* cached count; not the full free list */
    st->f_bavail  = fs->nfree;
    st->f_files   = (fs->isize - 2) * V7_INOPB;
    st->f_ffree   = fs->ninode;
    st->f_namemax = V7_DIRSIZ;
    return 0;
}

/* ---- operations table --------------------------------------------------- */

static struct fuse_operations v7_ops = {
    .getattr  = v7_getattr,
    .readdir  = v7_readdir,
    .open     = v7_open,
    .read     = v7_read,
    .write    = v7_write,
    .create   = v7_create,
    .mkdir    = v7_mkdir,
    .unlink   = v7_unlink,
    .rmdir    = v7_rmdir,
    .rename   = v7_rename,
    .chmod    = v7_chmod,
    .chown    = v7_chown,
    .truncate = v7_truncate,
    .utimens  = v7_utimens,
    .statfs   = v7_statfs,
};

/* ---- main --------------------------------------------------------------- */

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [-r] [-f] [-d] <image> <mountpoint>\n"
            "       %s -c <image>            # integrity check (no mount)\n",
            prog, prog);
}

int main(int argc, char *argv[]) {
    int readonly = 0, foreground = 0, debug = 0, check = 0;
    int ai = 1;
    for (; ai < argc && argv[ai][0] == '-'; ai++) {
        for (const char *p = argv[ai] + 1; *p; p++) {
            switch (*p) {
            case 'r': readonly = 1; break;
            case 'f': foreground = 1; break;
            case 'd': debug = 1; break;
            case 'c': check = 1; break;
            default:
                usage(argv[0]);
                return 2;
            }
        }
    }
    if (check && (argc - ai != 1)) {
        fprintf(stderr, "usage: %s -c <image>\n", argv[0]);
        return 2;
    }
    if (!check && (argc - ai != 2)) {
        usage(argv[0]);
        return 2;
    }
    const char *image = argv[ai];
    const char *mountpoint = check ? NULL : argv[ai + 1];

    v7fs_t fs;
    int rc = v7fs_open(&fs, image, readonly || check);
    if (rc) {
        fprintf(stderr, "v7mount: cannot open %s: %s\n", image, strerror(-rc));
        return 1;
    }

    if (check) {
        v7_check_t rep;
        int crc = v7fs_check(&fs, &rep);
        v7fs_close(&fs);
        return crc == 0 ? 0 : 1;
    }

    /* Hold an exclusive lock on the image for the whole mount, so the
     * emulator (which takes this lock around its own I/O) blocks.  Blocking
     * here: if the emulator is mid-access, wait for it to finish, then grab
     * the lock; its *next* disk access will then block until we unmount. */
    if (flock(fs.fd, LOCK_EX) != 0) {
        fprintf(stderr, "v7mount: cannot lock %s: %s\n", image, strerror(errno));
        v7fs_close(&fs);
        return 1;
    }

    struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
    fuse_opt_add_arg(&args, argv[0]);
    fuse_opt_add_arg(&args, mountpoint);
    fuse_opt_add_arg(&args, "-s");        /* single-threaded: free list is not re-entrant */
    if (foreground) fuse_opt_add_arg(&args, "-f");
    if (debug)      fuse_opt_add_arg(&args, "-d");
    if (readonly)   { fuse_opt_add_arg(&args, "-o"); fuse_opt_add_arg(&args, "ro"); }

    rc = fuse_main(args.argc, args.argv, &v7_ops, &fs);
    fuse_opt_free_args(&args);

    flock(fs.fd, LOCK_UN);
    v7fs_close(&fs);
    return rc;
}
