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
    uint16_t ino    = it->desc->bo->get16(it->buf + off);
    uint16_t reclen = it->desc->bo->get16(it->buf + off + 2);
    uint16_t namlen = it->desc->bo->get16(it->buf + off + 4);
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
    ssize_t got = v7fs_file_read(fs, ip, buf, ip->size, 0);
    if (got < 0) { free(buf); return (int)got; }
    *out = buf;
    *n = (size_t)got;
    return 0;
}

int bsd211_dir_read(filsys_edition_t *fs, v7_inode_t *ip, v7_dirent_t **ents, size_t *count) {
    if ((ip->mode & fs->desc.ifmt) != fs->desc.ifdir)
        return -ENOTDIR;
    if (ip->size > (uint64_t)(fs->fsize - fs->isize) * fs->desc.bsize)
        return -EFBIG;

    uint8_t *buf = NULL;
    size_t n = 0;
    int rc = bsd211_slurp(fs, ip, &buf, &n);
    if (rc)
        return rc;

    /* Count first, then allocate exactly.  The old code sized the array from
     * size/12, but the smallest record this format can hold is eight bytes
     * (dirsiz(1)), so a directory of short names overflowed it. */
    bsd211_iter_t it;
    size_t live = 0;
    bsd211_iter_init(&it, &fs->desc, buf, n);
    while (bsd211_iter_next(&it))
        if (it.ino != 0)
            live++;

    v7_dirent_t *out = calloc(live + 1, sizeof *out);
    if (!out) { free(buf); return -ENOMEM; }

    size_t cnt = 0;
    bsd211_iter_init(&it, &fs->desc, buf, n);
    while (bsd211_iter_next(&it) && cnt < live) {
        if (it.ino == 0)
            continue;
        out[cnt].ino = it.ino;
        memcpy(out[cnt].name, it.name, it.namlen);
        out[cnt].name[it.namlen] = 0;
        cnt++;
    }
    free(buf);
    *ents = out;
    *count = cnt;
    return 0;
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
        ssize_t w = v7fs_file_write(fs, ip, buf, n, 0);
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
        if (v7fs_file_write(fs, ip, reclen, 2, (off_t)(prev_off + 2)) < 0)
            return -EIO;
    }

    uint8_t ent[BSD211_DIRHDRSZ + BSD211_MAXNAMLEN + 4];
    memset(ent, 0, sizeof ent);
    fs->desc.bo->put16(ent, (uint16_t)ino);
    fs->desc.bo->put16(ent + 2, (uint16_t)need);
    fs->desc.bo->put16(ent + 4, (uint16_t)namlen);
    memcpy(ent + BSD211_DIRHDRSZ, name, namlen);
    ssize_t w = v7fs_file_write(fs, ip, ent, need, (off_t)slot);
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
        ssize_t w = v7fs_file_write(fs, ip, buf, n, 0);
        free(buf);
        return w < 0 ? (int)w : 0;
    }
    free(buf);
    return -ENOENT;
}
