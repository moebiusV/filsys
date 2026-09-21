/* filsys_path.c - the userspace path-taking API.
 *
 * The engine core (filsys.c) is node-anchored: it exposes filsys_walk and the
 * _in / _ino primitives and never sees a path string.  This file is the
 * userspace shell over them: it splits a path, walks it one component at a
 * time via filsys_walk, and delegates each operation to the matching
 * node-anchored primitive.  It is not compiled into a kernel; only the FUSE
 * frontend and the tools link it.
 *
 * SPDX-License-Identifier: ISC
 */
#include <config.h>
#include "filsys.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>

/* Split `path` into a parent directory and a final name.  On success `dir` /
 * `name` hold the two halves (the name need only hold up to 63 bytes + NUL). */
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

/* Resolve a full path to an inode, walking one component at a time from the
 * root via filsys_walk. */
static int path_lookup(filsys_t *fs, const char *path, uint32_t *ino,
                       filsys_inode_t *ip) {
    if (path[0] != '/')
        return -EINVAL;
    uint32_t cur = filsys_rootino(fs);
    const char *p = path + 1;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (len == 0) {
            p++;
            continue;
        }
        char name[64];
        if (len >= sizeof(name))
            return -ENAMETOOLONG;
        memcpy(name, p, len);
        name[len] = 0;
        int rc = filsys_walk(fs, cur, name, &cur);
        if (rc)
            return rc;
        p = slash ? slash + 1 : p + len;
    }
    *ino = cur;
    if (ip)
        return filsys_read_inode(fs, cur, ip);
    return 0;
}

/* Split `path` into a parent directory and a final name, then resolve the
 * parent.  On success `dino`/`ddir` hold the parent and `name` the basename. */
static int resolve_parent(filsys_t *fs, const char *path,
                          uint32_t *dino, filsys_inode_t *ddir, char *name) {
    char dir[PATH_MAX];
    int rc = split_path(path, dir, sizeof dir, name, filsys_namemax(fs) + 1);
    if (rc)
        return rc;
    return path_lookup(fs, dir, dino, ddir);
}

int filsys_lookup(filsys_t *fs, const char *path, uint32_t *ino, filsys_inode_t *ip) {
    return path_lookup(fs, path, ino, ip);
}

int filsys_readdir(filsys_t *fs, const char *path, filsys_dirent_t **ents, size_t *count) {
    uint32_t ino;
    int rc = path_lookup(fs, path, &ino, NULL);
    if (rc)
        return rc;
    return filsys_readdir_ino(fs, ino, ents, count);
}

ssize_t filsys_read(filsys_t *fs, const char *path, void *buf, size_t size, off_t off) {
    uint32_t ino;
    int rc = path_lookup(fs, path, &ino, NULL);
    if (rc)
        return rc;
    return filsys_read_ino(fs, ino, buf, size, off);
}

ssize_t filsys_write(filsys_t *fs, const char *path, const void *buf, size_t size, off_t off) {
    uint32_t ino;
    int rc = path_lookup(fs, path, &ino, NULL);
    if (rc)
        return rc;
    return filsys_write_ino(fs, ino, buf, size, off);
}

int filsys_create(filsys_t *fs, const char *path, mode_t mode, uid_t uid, gid_t gid,
                  uint32_t *ino) {
    char name[64];
    uint32_t dino;
    filsys_inode_t ddir;
    int rc = resolve_parent(fs, path, &dino, &ddir, name);
    if (rc)
        return rc;
    return filsys_create_in(fs, dino, name, mode, uid, gid, ino);
}

int filsys_mkdir(filsys_t *fs, const char *path, mode_t mode, uid_t uid, gid_t gid) {
    char name[64];
    uint32_t dino;
    filsys_inode_t ddir;
    int rc = resolve_parent(fs, path, &dino, &ddir, name);
    if (rc)
        return rc;
    return filsys_mkdir_in(fs, dino, name, mode, uid, gid);
}

int filsys_mknod(filsys_t *fs, const char *path, mode_t mode, dev_t rdev,
                 uid_t uid, gid_t gid) {
    if ((mode & S_IFMT) == S_IFREG)
        return filsys_create(fs, path, mode & 07777, uid, gid, NULL);
    char name[64];
    uint32_t dino;
    filsys_inode_t ddir;
    int rc = resolve_parent(fs, path, &dino, &ddir, name);
    if (rc)
        return rc;
    return filsys_mknod_in(fs, dino, name, mode, rdev, uid, gid);
}

int filsys_unlink(filsys_t *fs, const char *path) {
    char dir[PATH_MAX], name[64];
    int rc = split_path(path, dir, sizeof(dir), name, filsys_namemax(fs) + 1);
    if (rc)
        return rc;
    filsys_inode_t ip;
    uint32_t ino;
    rc = path_lookup(fs, path, &ino, &ip);
    if (rc)
        return rc;
    if (filsys_is_dir(fs, &ip))
        return -EISDIR;
    uint32_t dino;
    filsys_inode_t ddir;
    rc = path_lookup(fs, dir, &dino, &ddir);
    if (rc)
        return rc;
    return filsys_unlink_in(fs, dino, name);
}

int filsys_rmdir(filsys_t *fs, const char *path) {
    char name[64];
    uint32_t dino;
    filsys_inode_t ddir;
    int rc = resolve_parent(fs, path, &dino, &ddir, name);
    if (rc)
        return rc;
    return filsys_rmdir_in(fs, dino, name);
}

int filsys_link(filsys_t *fs, const char *from, const char *to) {
    uint32_t ino;
    int rc = path_lookup(fs, from, &ino, NULL);
    if (rc)
        return rc;
    char name[64];
    uint32_t dino;
    filsys_inode_t ddir;
    rc = resolve_parent(fs, to, &dino, &ddir, name);
    if (rc)
        return rc;
    return filsys_link_in(fs, ino, dino, name);
}

int filsys_rename(filsys_t *fs, const char *from, const char *to, unsigned int flags) {
    if (!strcmp(from, to))
        return 0;
    char fdir[PATH_MAX], fname[64];
    split_path(from, fdir, sizeof(fdir), fname, filsys_namemax(fs) + 1);
    char tdir[PATH_MAX], tname[64];
    int rc = split_path(to, tdir, sizeof(tdir), tname, filsys_namemax(fs) + 1);
    if (rc)
        return rc;
    filsys_inode_t fddir;
    uint32_t sdino;
    rc = path_lookup(fs, fdir, &sdino, &fddir);
    if (rc)
        return rc;
    filsys_inode_t tddir;
    uint32_t tdino;
    rc = path_lookup(fs, tdir, &tdino, &tddir);
    if (rc)
        return rc;
    return filsys_rename_in(fs, sdino, fname, tdino, tname, flags);
}

int filsys_symlink(filsys_t *fs, const char *target, const char *linkpath) {
    char name[64];
    uint32_t dino;
    filsys_inode_t ddir;
    int rc = resolve_parent(fs, linkpath, &dino, &ddir, name);
    if (rc)
        return rc;
    return filsys_symlink_in(fs, target, dino, name);
}

int filsys_truncate(filsys_t *fs, const char *path, off_t size) {
    uint32_t ino;
    int rc = path_lookup(fs, path, &ino, NULL);
    if (rc)
        return rc;
    return filsys_truncate_ino(fs, ino, size);
}

int filsys_chmod(filsys_t *fs, const char *path, mode_t mode) {
    uint32_t ino;
    int rc = path_lookup(fs, path, &ino, NULL);
    if (rc)
        return rc;
    return filsys_chmod_ino(fs, ino, mode);
}

int filsys_chown(filsys_t *fs, const char *path, uid_t uid, gid_t gid) {
    uint32_t ino;
    int rc = path_lookup(fs, path, &ino, NULL);
    if (rc)
        return rc;
    return filsys_chown_ino(fs, ino, uid, gid);
}

int filsys_utimens(filsys_t *fs, const char *path, const int64_t tv[2]) {
    uint32_t ino;
    int rc = path_lookup(fs, path, &ino, NULL);
    if (rc)
        return rc;
    return filsys_utimens_ino(fs, ino, tv);
}

ssize_t filsys_readlink(filsys_t *fs, const char *path, char *buf, size_t size) {
    uint32_t ino;
    int rc = path_lookup(fs, path, &ino, NULL);
    if (rc)
        return rc;
    return filsys_readlink_ino(fs, ino, buf, size);
}
