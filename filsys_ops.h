/* filsys_ops.h - internal vtable for the filsys library backends.
 *
 * Each on-disk format (v6fs, v7fs, ...) exposes one of these; filsys.c routes
 * every operation through it rather than a per-edition switch, so a new format
 * is one more ops table instead of a third arm of every ternary.
 *
 * The first parameter of every op is a `void *` pointing at the backend's own
 * state struct; the decoded types (filsys_inode_t / filsys_dirent_t) are the
 * public structs from filsys.h.  Backends may expose their functions with
 * layout-identical private typedefs and cast at the ops-table definition.
 *
 * SPDX-License-Identifier: ISC
 */
#ifndef FILSYS_OPS_H
#define FILSYS_OPS_H

#include "filsys.h"

struct filsys_ops {
    const char *name;
    uint32_t blocksize;   /* logical block size in bytes (bmap/truncate unit) */

    /* lifecycle */
    int  (*open)(void *fs, const char *path, int readonly, int little_endian,
                 uint64_t offset);
    void (*close)(void *fs);
    int  (*sync)(void *fs);

    /* block io */
    int  (*read_block)(void *fs, uint32_t bno, uint8_t *buf);
    int  (*write_block)(void *fs, uint32_t bno, const uint8_t *buf);

    /* inode io: on-disk bytes <-> decoded filsys_inode_t */
    int  (*read_inode)(void *fs, uint32_t ino, filsys_inode_t *ip);
    int  (*write_inode)(void *fs, uint32_t ino, const filsys_inode_t *ip);

    /* inode allocation (block allocation is internal to bmap/itrunc) */
    int  (*ialloc)(void *fs, uint32_t *ino);
    void (*ifree)(void *fs, uint32_t ino);

    /* block mapping + truncate */
    int  (*bmap)(void *fs, filsys_inode_t *ip, uint32_t lbn, int create, uint32_t *bno);
    int  (*itrunc)(void *fs, filsys_inode_t *ip);
    int  (*itrunc_from)(void *fs, filsys_inode_t *ip, uint32_t first_blk);

    /* file data */
    ssize_t (*file_read)(void *fs, filsys_inode_t *ip, uint8_t *buf, size_t size, off_t off);
    ssize_t (*file_write)(void *fs, filsys_inode_t *ip, const uint8_t *buf, size_t size, off_t off);

    /* directories */
    int  (*dir_read)(void *fs, filsys_inode_t *ip, filsys_dirent_t **ents, size_t *count);
    int  (*dir_lookup)(void *fs, filsys_inode_t *ip, const char *name, uint32_t *ino);
    int  (*dir_add)(void *fs, filsys_inode_t *ip, uint32_t ino, const char *name);
    int  (*dir_remove)(void *fs, filsys_inode_t *ip, const char *name);

    /* path lookup */
    int  (*lookup)(void *fs, const char *path, uint32_t *ino, filsys_inode_t *ip);

    /* integrity check; returns -1 if problems were found */
    int  (*check)(void *fs);

    /* largest addressable file, in bytes */
    uint64_t (*max_file)(void *fs);
};

extern const struct filsys_ops v6fs_ops;
extern const struct filsys_ops v7fs_ops;
extern const struct filsys_ops v1fs_ops;
extern const struct filsys_ops p7fs_ops;

#endif /* FILSYS_OPS_H */
