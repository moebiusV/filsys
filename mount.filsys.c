/* filsys 1.0.0 - 2026-08-26 - Copyright (C) 2026 David Walther */
/* SPDX-License-Identifier: ISC */
/* mount.filsys.c - mount a Research Unix filesystem image (PDP-11) as a FUSE
 * filesystem, selecting the on-disk edition at run time.
 *
 * This file is a thin FUSE shim over the filsys library (libfilsys); the
 * on-disk backends (V6, V7/32V) and the version dispatch live there.
 *
 * One binary, every edition we care about:
 *
 *   -v 4     V4 format (byte-identical to V5/V6)
 *   -v 5     V5 format (byte-identical to V4/V6)
 *   -v 6     V6 format
 *   -v 7     V7 format
 *   -v 32    32V format (V7 for the VAX; little-endian 32-bit fields + addresses)
 *
 * The on-disk layout is the 1969 Thompson/Canaday/Ritchie design - a flat
 * i-list at a fixed offset, directories as ordinary files of 16-byte entries,
 * and device files - carried essentially unchanged from the PDP-7 through V6,
 * then widened in V7 (64-byte inode, 24-bit block numbers).  See filsys.5.
 *
 * Usage:
 *     mount.filsys -v <4|5|6|7|32> [-o offset=N[,version=N][,uid=N,gid=N,...]]
 *                    [-r] [-f] [-d] <image> <mountpoint>
 *     mount.filsys -v <4|5|6|7|32> [-o offset=N] -c <image>   # integrity check
 *
 * `-o offset=N` mounts a filesystem that lives at byte offset N within the
 * file (a partition of a larger disk image), instead of one at block 0.
 * `-o version=N` selects the edition (so `mount -t filsys` can pass it in `-o`
 * rather than `-v`, which mount(8) has no generic way to supply).
 * `-o uid=N,gid=N` override the reported ownership (default: the mounting
 * user, so a nested mount point is writable).  Any other -o option is passed
 * through to FUSE (e.g. allow_other).  Installed as sbin/mount.filsys, so
 * `mount -t filsys device dir -o version=7,offset=N` works.
 */
#include <config.h>
#include "filsys.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fuse.h>

static filsys_t *K(void) {
    return (filsys_t *)fuse_get_context()->private_data;
}

/* ---- callbacks ----------------------------------------------------------- */

static int fuse_getattr(const char *path, struct stat *st, struct fuse_file_info *fi) {
    (void)fi;
    filsys_inode_t ip;
    uint32_t ino;
    int rc = filsys_lookup(K(), path, &ino, &ip);
    if (rc) return rc;
    filsys_fill_stat(K(), &ip, st);
    return 0;
}

static int fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t off, struct fuse_file_info *fi, enum fuse_readdir_flags fl) {
    (void)off; (void)fi; (void)fl;
    filsys_dirent_t *ents = NULL;
    size_t count = 0;
    int rc = filsys_readdir(K(), path, &ents, &count);
    if (rc) return rc;
    for (size_t i = 0; i < count; i++) {
        struct stat st;
        filsys_inode_t eip;
        if (filsys_read_inode(K(), ents[i].ino, &eip) == 0)
            filsys_fill_stat(K(), &eip, &st);
        else
            memset(&st, 0, sizeof(st));
        if (filler(buf, ents[i].name, &st, 0, 0))
            break;
    }
    free(ents);
    return 0;
}

static int fuse_open(const char *path, struct fuse_file_info *fi) {
    filsys_inode_t ip;
    uint32_t ino;
    int rc = filsys_lookup(K(), path, &ino, &ip);
    if (rc) return rc;
    struct stat st;
    filsys_fill_stat(K(), &ip, &st);
    if (S_ISDIR(st.st_mode)) return -EISDIR;
    if ((fi->flags & O_ACCMODE) != O_RDONLY && filsys_is_readonly(K()))
        return -EROFS;
    return 0;
}

static int fuse_read(const char *path, char *buf, size_t size, off_t off, struct fuse_file_info *fi) {
    (void)fi;
    return filsys_read(K(), path, buf, size, off);
}

static int fuse_write(const char *path, const char *buf, size_t size, off_t off, struct fuse_file_info *fi) {
    (void)fi;
    return filsys_write(K(), path, buf, size, off);
}

static int fuse_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)fi;
    const struct fuse_context *ctx = fuse_get_context();
    return filsys_create(K(), path, mode, ctx->uid, ctx->gid);
}

static int fuse_mkdir(const char *path, mode_t mode) {
    const struct fuse_context *ctx = fuse_get_context();
    return filsys_mkdir(K(), path, mode, ctx->uid, ctx->gid);
}

static int fuse_mknod(const char *path, mode_t mode, dev_t rdev) {
    const struct fuse_context *ctx = fuse_get_context();
    return filsys_mknod(K(), path, mode, rdev, ctx->uid, ctx->gid);
}

static int fuse_unlink(const char *path) { return filsys_unlink(K(), path); }
static int fuse_rmdir(const char *path) { return filsys_rmdir(K(), path); }
static int fuse_link(const char *from, const char *to) { return filsys_link(K(), from, to); }
static int fuse_rename(const char *from, const char *to, unsigned int flags) { return filsys_rename(K(), from, to, flags); }
static int fuse_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) { (void)fi; return filsys_chmod(K(), path, mode); }
static int fuse_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi) { (void)fi; return filsys_chown(K(), path, uid, gid); }
static int fuse_truncate(const char *path, off_t size, struct fuse_file_info *fi) { (void)fi; return filsys_truncate(K(), path, size); }
static int fuse_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi) { (void)fi; return filsys_utimens(K(), path, tv); }
static int fuse_statfs(const char *path, struct statvfs *st) { (void)path; return filsys_statfs(K(), st); }

static int fuse_access(const char *path, int mask) {
    filsys_inode_t ip;
    uint32_t ino;
    int rc = filsys_lookup(K(), path, &ino, &ip);
    if (rc) return rc;
    if ((mask & W_OK) && filsys_is_readonly(K()))
        return -EROFS;
    return 0;
}

static int fuse_flush(const char *path, struct fuse_file_info *fi) {
    (void)path; (void)fi;
    return 0;   /* no per-file state to flush; writes go straight to the image */
}

static int fuse_fsync(const char *path, int isdatasync, struct fuse_file_info *fi) {
    (void)path; (void)isdatasync; (void)fi;
    return filsys_sync(K());
}

static int fuse_release(const char *path, struct fuse_file_info *fi) {
    (void)path; (void)fi;
    return 0;   /* no per-open state to release */
}

static void filsys_destroy(void *private_data) {
    filsys_sync((filsys_t *)private_data);   /* flush on unmount; main() closes */
}

static struct fuse_operations filsys_ops = {
    .getattr  = fuse_getattr,
    .readdir  = fuse_readdir,
    .open     = fuse_open,
    .read     = fuse_read,
    .write    = fuse_write,
    .create   = fuse_create,
    .link     = fuse_link,
    .mkdir    = fuse_mkdir,
    .mknod    = fuse_mknod,
    .unlink   = fuse_unlink,
    .rmdir    = fuse_rmdir,
    .rename   = fuse_rename,
    .chmod    = fuse_chmod,
    .chown    = fuse_chown,
    .truncate = fuse_truncate,
    .utimens  = fuse_utimens,
    .statfs   = fuse_statfs,
    .access   = fuse_access,
    .flush    = fuse_flush,
    .fsync    = fuse_fsync,
    .release  = fuse_release,
    .destroy  = filsys_destroy,
};

/* ---- main ---------------------------------------------------------------- */

static void usage(const char *p) {
    fprintf(stderr,
            "usage: %s -v <4|5|6|7|32> [-o offset=N[,version=N][,uid=N][,gid=N]]\n"
            "                [-r] [-f] [-d] <image> <mountpoint>\n"
            "       %s -v <4|5|6|7|32> [-o offset=N] -c <image>   # integrity check\n",
            p, p);
}

/* Parse a -v / -o version= argument.  Returns FILSYS_V6/V7/32V, or -1. */
static int parse_version(const char *s) {
    if (s[0] == 'v' || s[0] == 'V') s++;   /* accept "v7" and "7" alike */
    if (!strcmp(s, "4") || !strcmp(s, "5") || !strcmp(s, "6"))
        return FILSYS_V6;
    if (!strcmp(s, "7"))
        return FILSYS_V7;
    if (!strcmp(s, "32") || !strcmp(s, "32v"))
        return FILSYS_32V;
    return -1;
}

int main(int argc, char *argv[]) {
    int ver = FILSYS_V7, readonly = 0, foreground = 0, debug = 0, check = 0;
    uint64_t offset = 0;
    int uid = -1, gid = -1;   /* -1 = report as the mounting user */
    char fuse_opts[512] = ""; /* -o options passed through to FUSE (allow_other, ...) */
    int c;

    /* getopt, not a hand-rolled loop, so the mount(8) argument order
     * `mount.filsys image dir -o opts` parses (GNU getopt permutes operands to
     * the front, so trailing options are fine). */
    while ((c = getopt(argc, argv, "v:o:rfdc")) != -1) {
        switch (c) {
        case 'v': {
            int v = parse_version(optarg);
            if (v < 0) {
                fprintf(stderr, "%s: unknown Unix version \"%s\" (4, 5, 6, 7, 32)\n",
                        argv[0], optarg);
                usage(argv[0]);
                return 2;
            }
            ver = v;
            break;
        }
        case 'o': {
            char *opts = strdup(optarg);
            int bad = 0;
            for (char *tok = strtok(opts, ","); tok; tok = strtok(NULL, ",")) {
                if (!strncmp(tok, "offset=", 7)) {
                    char *end = NULL;
                    offset = strtoull(tok + 7, &end, 0);
                    if (!end || *end) { bad = 1; break; }
                } else if (!strncmp(tok, "version=", 8)) {
                    int v = parse_version(tok + 8);
                    if (v < 0) bad = 1; else ver = v;
                } else if (!strncmp(tok, "uid=", 4)) {
                    uid = atoi(tok + 4);
                } else if (!strncmp(tok, "gid=", 4)) {
                    gid = atoi(tok + 4);
                } else {
                    /* pass anything else through to FUSE (allow_other, ...) */
                    if (fuse_opts[0]) strncat(fuse_opts, ",", sizeof(fuse_opts) - strlen(fuse_opts) - 1);
                    strncat(fuse_opts, tok, sizeof(fuse_opts) - strlen(fuse_opts) - 1);
                }
            }
            free(opts);
            if (bad) { usage(argv[0]); return 2; }
            break;
        }
        case 'r': readonly = 1; break;
        case 'f': foreground = 1; break;
        case 'd': debug = 1; break;
        case 'c': check = 1; break;
        default: usage(argv[0]); return 2;
        }
    }
    if (check && (argc - optind != 1)) { fprintf(stderr, "usage: %s -c <image>\n", argv[0]); return 2; }
    if (!check && (argc - optind != 2)) { usage(argv[0]); return 2; }
    const char *image = argv[optind];
    const char *mountpoint = check ? NULL : argv[optind + 1];

    filsys_t *k = NULL;
    int rc = filsys_open(&k, ver, image, readonly || check, offset,
                         uid >= 0 ? uid : (int)getuid(),
                         gid >= 0 ? gid : (int)getgid());
    if (rc) {
        fprintf(stderr, "filsys: cannot open %s: %s\n", image, strerror(-rc));
        return 1;
    }

    if (check) {
        int crc = filsys_check(k);
        filsys_close(k);
        return crc == 0 ? 0 : 1;
    }

    struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
    fuse_opt_add_arg(&args, argv[0]);
    fuse_opt_add_arg(&args, mountpoint);
    fuse_opt_add_arg(&args, "-s");
    if (foreground) fuse_opt_add_arg(&args, "-f");
    if (debug)      fuse_opt_add_arg(&args, "-d");
    if (readonly) {
        if (fuse_opts[0]) strncat(fuse_opts, ",", sizeof(fuse_opts) - strlen(fuse_opts) - 1);
        strncat(fuse_opts, "ro", sizeof(fuse_opts) - strlen(fuse_opts) - 1);
    }
    if (fuse_opts[0]) { fuse_opt_add_arg(&args, "-o"); fuse_opt_add_arg(&args, fuse_opts); }

    rc = fuse_main(args.argc, args.argv, &filsys_ops, k);
    fuse_opt_free_args(&args);

    filsys_close(k);
    return rc;
}
