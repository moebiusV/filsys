/* dir_fixed.c - fixed 16-byte directory entries (V7/32V/Coherent/Xenix/System III/V, 2.9BSD) (split from v7fs.c along its allocator/dir seams).
 *
 * SPDX-License-Identifier: ISC */
#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "v7fs.h"
#include "filsys_ops.h"
#include "check.h"
#include "instrument.h"

int v7fs_dir_read(filsys_edition_t *fs, v7_inode_t *ip, v7_dirent_t **ents, size_t *count) {
    if (!fs_is_dir(fs, ip))
        return -ENOTDIR;
    /* A directory's data cannot exceed the filesystem's data area; reject a
     * corrupt size before the malloc below, else a bogus di_size (up to 4 GiB)
     * turns into a multi-gigabyte allocation. */
    if (ip->size > (uint64_t)(fs->fsize - fs->desc.ops->data_start(fs)) * fs->desc.bsize)
        return -EFBIG;
    size_t cap = ip->size / fs->desc.dirent_size + 1;
    v7_dirent_t *out = calloc(cap, sizeof(v7_dirent_t));
    if (!out)
        return -ENOMEM;

    uint8_t *buf = malloc(ip->size);
    if (!buf) {
        free(out);
        return -ENOMEM;
    }
    ssize_t n = filsys_file_read(fs, ip, buf, ip->size, 0);
    if (n < 0) {
        free(buf);
        free(out);
        return (int)n;
    }

    size_t cnt = 0;
    for (size_t off = 0; off + fs->desc.dirent_size <= (size_t)n; off += fs->desc.dirent_size) {
        uint16_t ino = fs->desc.bo->get16(buf + off);
        if (ino == 0)
            continue;
        out[cnt].ino = ino;
        memcpy(out[cnt].name, buf + off + 2, fs->desc.max_namlen);
        out[cnt].name[fs->desc.max_namlen] = 0;
        cnt++;
    }
    free(buf);
    *ents = out;
    *count = cnt;
    return 0;
}

void v7fs_dirents_free(v7_dirent_t *ents) {
    free(ents);
}

int filsys_dir_lookup(filsys_edition_t *fs, v7_inode_t *ip, const char *name, uint32_t *ino) {
    v7_dirent_t *ents = NULL;
    size_t count = 0;
    int rc = fs->desc.ops->dir->dir_read(fs, ip, &ents, &count);
    if (rc)
        return rc;
    rc = -ENOENT;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(ents[i].name, name) == 0) {
            *ino = ents[i].ino;
            rc = 0;
            break;
        }
    }
    v7fs_dirents_free(ents);
    return rc;
}

int v7fs_dir_add(filsys_edition_t *fs, v7_inode_t *ip, uint32_t ino, const char *name) {
    size_t namelen = strlen(name);
    if (namelen == 0 || namelen > fs->desc.max_namlen)
        return -ENAMETOOLONG;
    if (strchr(name, '/'))
        return -EINVAL;

    size_t newsize = ip->size + fs->desc.dirent_size;
    uint8_t *buf = malloc(newsize);
    if (!buf)
        return -ENOMEM;
    memset(buf, 0, newsize);
    ssize_t n = filsys_file_read(fs, ip, buf, ip->size, 0);
    if (n < 0) {
        free(buf);
        return (int)n;
    }

    /* find an empty slot, else append */
    size_t slot = SIZE_MAX;
    for (size_t off = 0; off + fs->desc.dirent_size <= (size_t)n; off += fs->desc.dirent_size) {
        if (fs->desc.bo->get16(buf + off) == 0) {
            slot = off;
            break;
        }
    }
    if (slot == SIZE_MAX) {
        slot = (size_t)n;
        n += fs->desc.dirent_size;
    }

    fs->desc.bo->put16(buf + slot, (uint16_t)ino);
    memset(buf + slot + 2, 0, fs->desc.max_namlen);
    memcpy(buf + slot + 2, name, namelen);

    ssize_t w = filsys_file_write(fs, ip, buf, (size_t)n, 0);
    free(buf);
    return w < 0 ? (int)w : 0;
}

int v7fs_dir_remove(filsys_edition_t *fs, v7_inode_t *ip, const char *name) {
    uint8_t *buf = malloc(ip->size);
    if (!buf)
        return -ENOMEM;
    ssize_t n = filsys_file_read(fs, ip, buf, ip->size, 0);
    if (n < 0) {
        free(buf);
        return (int)n;
    }
    int rc = -ENOENT;
    for (size_t off = 0; off + fs->desc.dirent_size <= (size_t)n; off += fs->desc.dirent_size) {
        if (fs->desc.bo->get16(buf + off) == 0)
            continue;
        char ent[64];
        memcpy(ent, buf + off + 2, fs->desc.max_namlen);
        ent[fs->desc.max_namlen] = 0;
        if (strcmp(ent, name) == 0) {
            fs->desc.bo->put16(buf + off, 0);
            memset(buf + off + 2, 0, fs->desc.max_namlen);
            ssize_t w = filsys_file_write(fs, ip, buf, (size_t)n, 0);
            rc = w < 0 ? (int)w : 0;
            break;
        }
    }
    free(buf);
    return rc;
}

