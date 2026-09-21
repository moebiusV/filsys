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

/* The fourth walker: an offset-resumable scan for filsys_dir_next.  One
 * dir_chunk at a time sits in the iterator's buffer; *name points into it and
 * is NOT NUL-terminated (the field is max_namlen bytes, zero-padded), so the
 * effective length is returned in *namlen. */
int v7fs_dir_iter(filsys_iter_state_t *st, uint32_t *ino, const char **name,
                  uint16_t *namlen, uint64_t *next_off) {
    filsys_edition_t *fs = st->fs;
    filsys_inode_t *ip = &st->ip;
    size_t esize = fs->desc.dirent_size;
    size_t max = fs->desc.max_namlen;
    size_t chunk = dir_chunk(fs);

    for (;;) {
        uint64_t off = st->next_off;
        if (off >= ip->size)
            return 0;
        size_t chunk_off = (size_t)(off / chunk * chunk);
        if (st->buf_off != chunk_off || st->buf_n == 0) {
            size_t n = chunk;
            if (n > ip->size - chunk_off)
                n = (size_t)(ip->size - chunk_off);
            ssize_t got = filsys_file_read(fs, ip, st->buf, n, (off_t)chunk_off);
            if (got < 0)
                return (int)got;
            st->buf_off = chunk_off;
            st->buf_n = (size_t)got;
        }
        size_t rel = (size_t)(off - chunk_off);
        while (rel + esize <= st->buf_n) {
            uint16_t eino = fs->desc.bo->get16(st->buf + rel);
            if (eino != 0) {
                size_t nlen = 0;
                while (nlen < max && st->buf[rel + 2 + nlen] != 0)
                    nlen++;
                *ino = eino;
                *name = (const char *)(st->buf + rel + 2);
                *namlen = (uint16_t)nlen;
                *next_off = (uint64_t)(chunk_off + rel + esize);
                st->next_off = *next_off;
                return 1;
            }
            rel += esize;
        }
        st->next_off = (uint64_t)(chunk_off + chunk);
    }
}

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

