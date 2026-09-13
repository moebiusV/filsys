/* dir_bsd211.c - 2.11BSD variable-length directory entries (split from v7fs.c along its allocator/dir seams).
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

/* Rounded entry length: 7 bytes of fixed header + name, padded to a 4-byte
 * multiple (2.11BSD dirents are 4-byte aligned). */
static inline uint32_t bsd211_dirsiz(uint16_t namlen) {
    return (7u + namlen + 3u) & ~3u;
}

int bsd211_dir_read(filsys_edition_t *fs, v7_inode_t *ip, v7_dirent_t **ents, size_t *count) {
    if ((ip->mode & fs->ifmt) != fs->ifdir)
        return -ENOTDIR;
    if (ip->size > (uint64_t)(fs->fsize - fs->isize) * fs->bsize)
        return -EFBIG;
    size_t cap = ip->size / 12 + 1;
    v7_dirent_t *out = calloc(cap, sizeof(*out));
    if (!out)
        return -ENOMEM;
    uint8_t *buf = malloc(ip->size ? ip->size : 1);
    if (!buf) { free(out); return -ENOMEM; }
    ssize_t n = v7fs_file_read(fs, ip, buf, ip->size, 0);
    if (n < 0) { free(buf); free(out); return (int)n; }

    size_t cnt = 0;
    for (size_t off = 0; off + 6 <= (size_t)n; ) {
        uint16_t ino    = fs->bo->get16(buf + off);
        uint16_t reclen = fs->bo->get16(buf + off + 2);
        uint16_t namlen = fs->bo->get16(buf + off + 4);
        if (reclen < 6 || off + reclen > (size_t)n)
            break;
        if (ino != 0 && namlen <= fs->max_namlen) {
            out[cnt].ino = ino;
            memcpy(out[cnt].name, buf + off + 6, namlen);
            out[cnt].name[namlen] = 0;
            cnt++;
        }
        off += reclen;
    }
    free(buf);
    *ents = out;
    *count = cnt;
    return 0;
}

int bsd211_dir_add(filsys_edition_t *fs, v7_inode_t *ip, uint32_t ino, const char *name) {
    size_t namlen = strlen(name);
    if (namlen > fs->max_namlen)
        return -ENAMETOOLONG;
    uint32_t need = bsd211_dirsiz((uint16_t)namlen);

    uint8_t *buf = malloc(ip->size ? ip->size : 1);
    if (!buf)
        return -ENOMEM;
    ssize_t n = v7fs_file_read(fs, ip, buf, ip->size, 0);
    if (n < 0) { free(buf); return (int)n; }

    /* Reuse a free entry (d_ino == 0) that is big enough. */
    for (size_t off = 0; off + 6 <= (size_t)n; ) {
        uint16_t d_ino    = fs->bo->get16(buf + off);
        uint16_t reclen   = fs->bo->get16(buf + off + 2);
        if (reclen < 6 || off + reclen > (size_t)n)
            break;
        if (d_ino == 0 && reclen >= need) {
            memset(buf + off, 0, reclen);
            fs->bo->put16(buf + off, (uint16_t)ino);
            fs->bo->put16(buf + off + 2, reclen);
            fs->bo->put16(buf + off + 4, (uint16_t)namlen);
            memcpy(buf + off + 6, name, namlen);
            ssize_t w = v7fs_file_write(fs, ip, buf, ip->size, 0);
            free(buf);
            return w < 0 ? (int)w : 0;
        }
        off += reclen;
    }

    /* Append a new entry at the end (the directory grows). */
    uint8_t ent[80];
    memset(ent, 0, sizeof(ent));
    fs->bo->put16(ent, (uint16_t)ino);
    fs->bo->put16(ent + 2, (uint16_t)need);
    fs->bo->put16(ent + 4, (uint16_t)namlen);
    memcpy(ent + 6, name, namlen);
    ssize_t w = v7fs_file_write(fs, ip, ent, need, n);
    free(buf);
    return w < 0 ? (int)w : 0;
}

int bsd211_dir_remove(filsys_edition_t *fs, v7_inode_t *ip, const char *name) {
    uint8_t *buf = malloc(ip->size ? ip->size : 1);
    if (!buf)
        return -ENOMEM;
    ssize_t n = v7fs_file_read(fs, ip, buf, ip->size, 0);
    if (n < 0) { free(buf); return (int)n; }

    for (size_t off = 0; off + 6 <= (size_t)n; ) {
        uint16_t d_ino    = fs->bo->get16(buf + off);
        uint16_t reclen   = fs->bo->get16(buf + off + 2);
        uint16_t namlen   = fs->bo->get16(buf + off + 4);
        if (reclen < 6 || off + reclen > (size_t)n)
            break;
        if (d_ino != 0 && namlen == strlen(name) &&
            memcmp(buf + off + 6, name, namlen) == 0) {
            fs->bo->put16(buf + off, 0);   /* mark free */
            ssize_t w = v7fs_file_write(fs, ip, buf, ip->size, 0);
            free(buf);
            return w < 0 ? (int)w : 0;
        }
        off += reclen;
    }
    free(buf);
    return -ENOENT;
}

