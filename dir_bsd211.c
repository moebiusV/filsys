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

/* ---- validated record iteration ------------------------------------------
 *
 * Every read/add/remove walk goes through this one iterator, so the bounds
 * rules are stated once.  A record is accepted only if it is 4-byte aligned,
 * its header is inside the buffer, its length is a legal multiple of four that
 * does not run past the end, and its name (plus the NUL 2.11BSD stores) fits
 * inside the record.  Anything else ends the walk: a truncated or foreign
 * directory yields the entries up to the damage rather than a parse that reads
 * or writes past the buffer.
 *
 * The name bound matters as much as the record bound: d_namlen is attacker-
 * controlled on a foreign image and is what the copies below are sized by. */
typedef struct {
    const uint8_t       *buf;
    size_t               n;
    const filsys_desc_t *desc;
    size_t               off;     /* offset of the record just returned */
    uint16_t             ino;
    uint16_t             reclen;
    uint16_t             namlen;
    const uint8_t       *name;
} bsd211_iter_t;

static void bsd211_iter_init(bsd211_iter_t *it, const filsys_desc_t *desc,
                             const uint8_t *buf, size_t n) {
    memset(it, 0, sizeof *it);
    it->buf = buf;
    it->n = n;
    it->desc = desc;
    it->off = 0;
    it->reclen = 0;
}

/* Advance to the next valid record.  Returns 1 on a record, 0 at the end of the
 * directory or at the first malformed record. */
static int bsd211_iter_next(bsd211_iter_t *it) {
    size_t off = it->off + it->reclen;      /* reclen is 0 before the first call */
    if (off & 3u)
        return 0;                            /* misaligned: stop */
    if (off + BSD211_DIRHDRSZ > it->n)
        return 0;
    uint16_t (*get16)(const uint8_t *) = it->desc->bo->get16;
    uint16_t ino    = get16(it->buf + off);
    uint16_t reclen = get16(it->buf + off + 2);
    uint16_t namlen = get16(it->buf + off + 4);
    if (reclen < BSD211_DIRMINSZ || (reclen & 3u) || off + reclen > it->n)
        return 0;
    if ((uint32_t)namlen + 7u > reclen || namlen > it->desc->max_namlen)
        return 0;
    if (off % BSD211_DIRBLKSIZ + reclen > BSD211_DIRBLKSIZ)
        return 0;   /* a record may not span a 512-byte chunk boundary */
    it->off    = off;
    it->ino    = ino;
    it->reclen = reclen;
    it->namlen = namlen;
    it->name   = it->buf + off + BSD211_DIRHDRSZ;
    return 1;
}

/* Read the whole directory into a malloc'd buffer.  *n receives the byte count.
 * The caller frees. */
static int bsd211_slurp(filsys_edition_t *fs, v7_inode_t *ip, uint8_t **out, size_t *n) {
    uint8_t *buf = malloc(ip->size ? ip->size : 1);
    if (!buf)
        return -ENOMEM;
    ssize_t got = filsys_file_read(fs, ip, buf, ip->size, 0);
    if (got < 0) { free(buf); return (int)got; }
    *out = buf;
    *n = (size_t)got;
    return 0;
}

/* The fourth walker: an offset-resumable scan for filsys_dir_next.  The
 * directory is read one 512-byte framing chunk at a time (a record never spans
 * a chunk boundary); the name is a pointer into the chunk buffer with its
 * explicit length in *namlen.  The first malformed record ends the walk. */
int bsd211_dir_iter(filsys_iter_state_t *st, uint32_t *ino, const char **name,
                    uint16_t *namlen, uint64_t *next_off) {
    filsys_edition_t *fs = st->fs;
    filsys_inode_t *ip = &st->ip;

    for (;;) {
        uint64_t off = st->next_off;
        if (off >= ip->size)
            return 0;
        size_t chunk_off = (size_t)(off / BSD211_DIRBLKSIZ * BSD211_DIRBLKSIZ);
        if (st->buf_off != chunk_off || st->buf_n == 0) {
            size_t n = BSD211_DIRBLKSIZ;
            if (n > ip->size - chunk_off)
                n = (size_t)(ip->size - chunk_off);
            ssize_t got = filsys_file_read(fs, ip, st->buf, n, (off_t)chunk_off);
            if (got < 0)
                return (int)got;
            st->buf_off = chunk_off;
            st->buf_n = (size_t)got;
        }
        size_t rel = (size_t)(off - chunk_off);
        while (rel + BSD211_DIRHDRSZ <= st->buf_n) {
            uint16_t eino   = fs->desc.bo->get16(st->buf + rel);
            uint16_t reclen = fs->desc.bo->get16(st->buf + rel + 2);
            uint16_t nlen   = fs->desc.bo->get16(st->buf + rel + 4);
            if (reclen < BSD211_DIRMINSZ || (reclen & 3u) || rel + reclen > st->buf_n)
                return 0;   /* truncated / malformed: end the walk */
            if ((uint32_t)nlen + 7u > reclen || nlen > fs->desc.max_namlen)
                return 0;
            if (eino != 0) {
                *ino = eino;
                *name = (const char *)(st->buf + rel + BSD211_DIRHDRSZ);
                *namlen = nlen;
                *next_off = (uint64_t)(chunk_off + rel + reclen);
                st->next_off = *next_off;
                return 1;
            }
            rel += reclen;
        }
        st->next_off = (uint64_t)(chunk_off + BSD211_DIRBLKSIZ);
    }
}

int bsd211_dir_lookup(filsys_edition_t *fs, v7_inode_t *ip, const char *name, uint32_t *ino) {
    uint8_t *buf = NULL;
    size_t n = 0;
    int rc = bsd211_slurp(fs, ip, &buf, &n);
    if (rc)
        return rc;
    bsd211_iter_t it;
    bsd211_iter_init(&it, &fs->desc, buf, n);
    rc = -ENOENT;
    while (bsd211_iter_next(&it)) {
        if (it.ino == 0)
            continue;
        if ((size_t)it.namlen == strlen(name) && memcmp(it.name, name, it.namlen) == 0) {
            *ino = it.ino;
            rc = 0;
            break;
        }
    }
    free(buf);
    return rc;
}

int bsd211_dir_add(filsys_edition_t *fs, v7_inode_t *ip, uint32_t ino, const char *name) {
    size_t namlen = strlen(name);
    if (namlen == 0 || namlen > fs->desc.max_namlen)
        return -ENAMETOOLONG;
    if (strchr(name, '/'))
        return -EINVAL;
    uint32_t need = bsd211_dirsiz((uint16_t)namlen);

    uint8_t *buf = NULL;
    size_t n = 0;
    int rc = bsd211_slurp(fs, ip, &buf, &n);
    if (rc)
        return rc;

    /* Reuse a free entry (d_ino == 0) that is big enough. */
    bsd211_iter_t it;
    bsd211_iter_init(&it, &fs->desc, buf, n);
    while (bsd211_iter_next(&it)) {
        if (it.ino != 0 || it.reclen < need)
            continue;
        uint16_t reclen = it.reclen;
        memset(buf + it.off, 0, reclen);
        fs->desc.bo->put16(buf + it.off, (uint16_t)ino);
        fs->desc.bo->put16(buf + it.off + 2, reclen);
        fs->desc.bo->put16(buf + it.off + 4, (uint16_t)namlen);
        memcpy(buf + it.off + BSD211_DIRHDRSZ, name, namlen);
        ssize_t w = filsys_file_write(fs, ip, buf, n, 0);
        free(buf);
        return w < 0 ? (int)w : 0;
    }

    /* Append a new entry at the end (the directory grows).  The append offset
     * is the end of the last *valid* record, not ip->size: a directory whose
     * tail is damaged must not have new records stacked behind the damage.
     *
     * 2.11BSD forbids a record from spanning a DIRBLKSIZ (512) boundary, so a
     * new entry that would cross one first pads the previous record out to the
     * chunk edge, and the new entry starts at the next 512-byte boundary. */
    size_t end = 0, prev_off = 0;
    bsd211_iter_init(&it, &fs->desc, buf, n);
    while (bsd211_iter_next(&it)) {
        prev_off = it.off;
        end = it.off + it.reclen;
    }
    free(buf);
    if (end & 3u)
        return -EIO;

    size_t slot = end;
    size_t in_chunk = end % BSD211_DIRBLKSIZ;
    if (in_chunk != 0 && in_chunk + need > BSD211_DIRBLKSIZ) {
        /* Extend the previous record's d_reclen so it fills its chunk, leaving
         * the new record to start at the next boundary.  The padding bytes are
         * skipped by every reader (they sit inside the padded record's reclen),
         * so they need not be written out. */
        slot = end + (BSD211_DIRBLKSIZ - in_chunk);
        uint8_t reclen[2];
        fs->desc.bo->put16(reclen, (uint16_t)(slot - prev_off));
        if (filsys_file_write(fs, ip, reclen, 2, (off_t)(prev_off + 2)) < 0)
            return -EIO;
    }

    uint8_t ent[BSD211_DIRHDRSZ + BSD211_MAXNAMLEN + 4];
    memset(ent, 0, sizeof ent);
    fs->desc.bo->put16(ent, (uint16_t)ino);
    fs->desc.bo->put16(ent + 2, (uint16_t)need);
    fs->desc.bo->put16(ent + 4, (uint16_t)namlen);
    memcpy(ent + BSD211_DIRHDRSZ, name, namlen);
    ssize_t w = filsys_file_write(fs, ip, ent, need, (off_t)slot);
    return w < 0 ? (int)w : 0;
}

int bsd211_dir_remove(filsys_edition_t *fs, v7_inode_t *ip, const char *name) {
    size_t namlen = strlen(name);
    uint8_t *buf = NULL;
    size_t n = 0;
    int rc = bsd211_slurp(fs, ip, &buf, &n);
    if (rc)
        return rc;

    bsd211_iter_t it;
    bsd211_iter_init(&it, &fs->desc, buf, n);
    while (bsd211_iter_next(&it)) {
        if (it.ino == 0 || it.namlen != namlen)
            continue;
        if (memcmp(it.name, name, namlen) != 0)
            continue;
        fs->desc.bo->put16(buf + it.off, 0);   /* mark free */
        ssize_t w = filsys_file_write(fs, ip, buf, n, 0);
        free(buf);
        return w < 0 ? (int)w : 0;
    }
    free(buf);
    return -ENOENT;
}
