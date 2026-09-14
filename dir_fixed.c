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

/* Compare a stored fixed-format name (max_namlen bytes, zero-padded by the
 * writer) against a NUL-terminated name.  A match requires the stored bytes to
 * equal `name` up to its terminator and be zero beyond it -- the same test the
 * old strcmp-over-a-copy performed. */
static int fixed_name_eq(const uint8_t *stored, const char *name, size_t max) {
    for (size_t i = 0; i < max; i++) {
        if (name[i] == '\0')
            return stored[i] == 0;
        if (stored[i] != (uint8_t)name[i])
            return 0;
    }
    return 1;
}

/* The directory is a flat array of dirent_size-byte entries, NOT block-aligned:
 * V1's 10-byte entries straddle its 512-byte blocks (512 % 10 = 2).  Scan it in
 * the largest dirent_size-aligned chunk that fits one block, so an entry never
 * crosses a chunk boundary while the chunk stays about one block of I/O. */
static size_t dir_chunk(const filsys_edition_t *fs) {
    size_t c = fs->desc.bsize / fs->desc.dirent_size * fs->desc.dirent_size;
    return c ? c : fs->desc.dirent_size;
}

/* Lookup, add and remove walk the directory one block at a time instead of
 * reading the whole directory into a buffer and rewriting it.  A fixed entry is
 * dirent_size bytes (ino at 0, name at 2), so a block-local scan is trivial and
 * a mutation is a single dirent_size-byte write rather than an ip->size-byte
 * rewrite: O(1) write amplification and no heap buffer sized by the directory.
 * Reading block-by-block is also kinder to a damaged directory -- a bad block
 * past the entry being looked for no longer aborts the whole operation. */

int v7fs_dir_lookup(filsys_edition_t *fs, v7_inode_t *ip, const char *name, uint32_t *ino) {
    size_t esize = fs->desc.dirent_size;
    size_t max = fs->desc.max_namlen;
    uint8_t blk[V7_MAXBSIZE];
    for (size_t off = 0; off < ip->size; off += dir_chunk(fs)) {
        size_t n = dir_chunk(fs);
        if (n > ip->size - off)
            n = ip->size - off;
        ssize_t got = filsys_file_read(fs, ip, blk, n, off);
        if (got < 0)
            return (int)got;
        for (size_t b = 0; b + esize <= (size_t)got; b += esize) {
            if (fs->desc.bo->get16(blk + b) == 0)
                continue;
            if (fixed_name_eq(blk + b + 2, name, max)) {
                *ino = fs->desc.bo->get16(blk + b);
                return 0;
            }
        }
    }
    return -ENOENT;
}

int v7fs_dir_add(filsys_edition_t *fs, v7_inode_t *ip, uint32_t ino, const char *name) {
    size_t namelen = strlen(name);
    if (namelen == 0 || namelen > fs->desc.max_namlen)
        return -ENAMETOOLONG;
    if (strchr(name, '/'))
        return -EINVAL;

    size_t esize = fs->desc.dirent_size;
    uint8_t ent[V7_DIRENTSZ];
    memset(ent, 0, sizeof ent);
    fs->desc.bo->put16(ent, (uint16_t)ino);
    memcpy(ent + 2, name, namelen);

    /* Reuse the first empty slot (ino == 0); else append at ip->size. */
    uint8_t blk[V7_MAXBSIZE];
    for (size_t off = 0; off < ip->size; off += dir_chunk(fs)) {
        size_t n = dir_chunk(fs);
        if (n > ip->size - off)
            n = ip->size - off;
        ssize_t got = filsys_file_read(fs, ip, blk, n, off);
        if (got < 0)
            return (int)got;
        for (size_t b = 0; b + esize <= (size_t)got; b += esize) {
            if (fs->desc.bo->get16(blk + b) == 0) {
                ssize_t w = filsys_file_write(fs, ip, ent, esize, (off_t)(off + b));
                return w < 0 ? (int)w : 0;
            }
        }
    }
    ssize_t w = filsys_file_write(fs, ip, ent, esize, (off_t)ip->size);
    return w < 0 ? (int)w : 0;
}

int v7fs_dir_remove(filsys_edition_t *fs, v7_inode_t *ip, const char *name) {
    size_t esize = fs->desc.dirent_size;
    size_t max = fs->desc.max_namlen;
    uint8_t blk[V7_MAXBSIZE];
    for (size_t off = 0; off < ip->size; off += dir_chunk(fs)) {
        size_t n = dir_chunk(fs);
        if (n > ip->size - off)
            n = ip->size - off;
        ssize_t got = filsys_file_read(fs, ip, blk, n, off);
        if (got < 0)
            return (int)got;
        for (size_t b = 0; b + esize <= (size_t)got; b += esize) {
            if (fs->desc.bo->get16(blk + b) == 0)
                continue;
            if (!fixed_name_eq(blk + b + 2, name, max))
                continue;
            /* The reader only tests ino, but clear the whole entry so a freed
             * slot does not retain its old name (as the old rewrite did). */
            memset(blk + b, 0, esize);
            ssize_t w = filsys_file_write(fs, ip, blk + b, esize, (off_t)(off + b));
            return w < 0 ? (int)w : 0;
        }
    }
    return -ENOENT;
}

