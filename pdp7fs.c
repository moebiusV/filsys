/* filsys 1.0.0 - 2026-08-26 - Copyright (C) 2026 David Walther */
/* SPDX-License-Identifier: ISC */
/* pdp7fs.c - PDP-7 Unix filesystem, on-disk access layer (read-only).
 *
 * The first Unix filesystem (Bell Labs, 1969) is word-addressed: 18-bit words,
 * 64-word blocks.  This backend unpacks the SimH RB09 image (one word per
 * 4-byte little-endian slot, filesystem on surface 1) into 32-bit words and
 * presents the result through the byte-oriented filsys ops table -- file sizes
 * and offsets are doubled (two 9-bit characters per word) so text files read
 * back as plain ASCII.  See pdp7fs.h and pdp7-unix's tools/mkfs7.
 *
 * Read-only for now: every mutating op returns -EROFS.
 */
#include <config.h>
#include "pdp7fs.h"
#include "filsys_ops.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* ---- word-level block io ------------------------------------------------ */

/* Read block `bno` (64 words) out of the image's filesystem surface. */
static int read_words(p7fs_t *fs, uint32_t bno, uint32_t *words) {
    uint8_t raw[P7_BLOCKBYTES];
    off_t pos = (off_t)fs->base + P7_SURFACE1 + (off_t)bno * P7_BLOCKBYTES;
    ssize_t n = pread(fs->fd, raw, P7_BLOCKBYTES, pos);
    if (n != P7_BLOCKBYTES)
        return -EIO;
    for (int i = 0; i < P7_WSIZE; i++)
        words[i] = p7_getword(raw + i * P7_WORDBYTES);
    return 0;
}

/* Sign-extend an 18-bit word into a signed 32-bit value. */
static int32_t sign18(uint32_t v) {
    v &= P7_MAXWORD;
    return (v & 0400000) ? (int32_t)(v - 01000000u) : (int32_t)v;
}

/* Read logical word `woff` of an inode's data (map through bmap). */
static int inode_read_word(p7fs_t *fs, p7_inode_t *ip, uint32_t woff, uint32_t *out) {
    uint32_t lbn = woff / P7_WSIZE;
    uint32_t idx = woff % P7_WSIZE;
    uint32_t pbn;
    if (p7fs_bmap(fs, ip, lbn, 0, &pbn))
        return -EIO;
    uint32_t words[P7_WSIZE];
    if (pbn == 0) {
        *out = 0;
        return 0;
    }
    if (read_words(fs, pbn, words))
        return -EIO;
    *out = words[idx];
    return 0;
}

/* Unpack four name words (2 chars each) into a NUL-terminated, space-trimmed
 * string of at most P7_DIRSIZ chars. */
static void unpack_name(const uint32_t *w, char *name) {
    for (int i = 0; i < 4; i++) {
        name[2 * i]     = (char)((w[i] >> 9) & 0x7f);
        name[2 * i + 1] = (char)(w[i] & 0x7f);
    }
    name[P7_DIRSIZ] = 0;
    for (int i = P7_DIRSIZ - 1; i >= 0 && name[i] == ' '; i--)
        name[i] = 0;   /* mkfs7 pads names to 8 chars with spaces */
}

/* ---- lifecycle --------------------------------------------------------- */

int p7fs_open(p7fs_t *fs, const char *path, int readonly, uint64_t offset) {
    memset(fs, 0, sizeof(*fs));
    fs->readonly = readonly;
    fs->base = offset;
    fs->fd = open(path, readonly ? O_RDONLY : O_RDWR);
    if (fs->fd < 0)
        return -errno;

    struct stat st;
    if (fstat(fs->fd, &st) != 0 ||
        fs->base + P7_SURFACE1 + P7_BLOCKBYTES > (uint64_t)st.st_size) {
        close(fs->fd);
        fs->fd = -1;
        return -EINVAL;   /* image too small to hold surface 1 */
    }

    uint32_t sb[P7_WSIZE];
    if (read_words(fs, 0, sb)) {
        close(fs->fd);
        fs->fd = -1;
        return -EIO;
    }
    fs->freelist = sb[0] & P7_MAXWORD;

    /* Walk the free list once for the statfs free-block count. */
    uint32_t head = fs->freelist, guard = 0;
    while (head && guard++ < P7_NBLOCKS) {
        uint32_t fl[P7_WSIZE];
        if (read_words(fs, head, fl))
            break;
        for (int i = 1; i <= 9; i++)
            if (fl[i] != 0)
                fs->tfree++;
        head = fl[0] & P7_MAXWORD;
    }
    return 0;
}

void p7fs_close(p7fs_t *fs) {
    if (fs->fd >= 0) {
        close(fs->fd);
        fs->fd = -1;
    }
}

int p7fs_sync(p7fs_t *fs) {
    (void)fs;
    return 0;   /* read-only: nothing to flush */
}

/* ---- block / inode io -------------------------------------------------- */

int p7fs_read_block(p7fs_t *fs, uint32_t bno, uint8_t *buf) {
    uint32_t words[P7_WSIZE];
    if (read_words(fs, bno, words))
        return -EIO;
    for (int i = 0; i < P7_WSIZE; i++)
        p7_putword(buf + i * P7_WORDBYTES, words[i]);
    return 0;
}

int p7fs_write_block(p7fs_t *fs, uint32_t bno, const uint8_t *buf) {
    (void)fs; (void)bno; (void)buf;
    return -EROFS;
}

int p7fs_read_inode(p7fs_t *fs, uint32_t ino, p7_inode_t *ip) {
    if (ino == 0 || ino > P7_MAXINO)
        return -EINVAL;
    uint32_t words[P7_WSIZE];
    if (read_words(fs, p7_itod(ino), words))
        return -EIO;
    const uint32_t *d = words + p7_itoo(ino);
    memset(ip, 0, sizeof(*ip));
    ip->ino   = ino;
    ip->mode  = d[0];               /* 18-bit flags */
    ip->nlink = (int16_t)(-sign18(d[9]));   /* stored negative */
    ip->uid   = (int16_t)sign18(d[8]);
    ip->gid   = 0;                  /* no gid */
    ip->size  = d[10] * 2;          /* words -> bytes (two chars per word) */
    for (int i = 0; i < P7_NIADDR; i++)
        ip->addr[i] = d[1 + i];
    ip->atime = ip->mtime = ip->ctime = 0;   /* PDP-7 has no timestamps */
    return 0;
}

int p7fs_write_inode(p7fs_t *fs, uint32_t ino, const p7_inode_t *ip) {
    (void)fs; (void)ino; (void)ip;
    return -EROFS;
}

/* ---- allocation (stubs: read-only) -------------------------------------- */

int p7fs_balloc(p7fs_t *fs, uint32_t *bno) {
    (void)fs; (void)bno;
    return -EROFS;
}

void p7fs_bfree(p7fs_t *fs, uint32_t bno) {
    (void)fs; (void)bno;
}

int p7fs_ialloc(p7fs_t *fs, uint32_t *ino) {
    (void)fs; (void)ino;
    return -EROFS;
}

void p7fs_ifree(p7fs_t *fs, uint32_t ino) {
    (void)fs; (void)ino;
}

int p7fs_itrunc(p7fs_t *fs, p7_inode_t *ip) {
    (void)fs; (void)ip;
    return -EROFS;
}

int p7fs_itrunc_from(p7fs_t *fs, p7_inode_t *ip, uint32_t first_blk) {
    (void)fs; (void)ip; (void)first_blk;
    return -EROFS;
}

/* ---- block mapping ------------------------------------------------------ */

int p7fs_bmap(p7fs_t *fs, p7_inode_t *ip, uint32_t lbn, int create, uint32_t *bno) {
    if (create)
        return -EROFS;

    if (ip->mode & P7_ILARG) {
        /* large file: 7 single-indirect slots, 64 block numbers each */
        if (lbn >= P7_NIADDR * P7_NINDIR) {
            *bno = 0;
            return 0;
        }
        uint32_t iblk = ip->addr[lbn / P7_NINDIR];
        if (iblk == 0) {
            *bno = 0;
            return 0;
        }
        uint32_t words[P7_WSIZE];
        if (read_words(fs, iblk, words))
            return -EIO;
        *bno = words[lbn % P7_NINDIR];
        return 0;
    }

    /* small file: 7 direct block pointers */
    if (lbn >= P7_NIADDR) {
        *bno = 0;
        return 0;
    }
    *bno = ip->addr[lbn];
    return 0;
}

/* ---- file data ---------------------------------------------------------- */

ssize_t p7fs_file_read(p7fs_t *fs, p7_inode_t *ip, uint8_t *buf, size_t size, off_t off) {
    if (off < 0)
        return -EINVAL;
    if ((uint64_t)off >= ip->size)
        return 0;
    uint64_t remaining = ip->size - (uint64_t)off;
    if (size > remaining)
        size = (size_t)remaining;

    size_t done = 0;
    while (done < size) {
        uint64_t bpos = (uint64_t)off + done;   /* byte position */
        uint32_t woff = (uint32_t)(bpos / 2);   /* word position */
        uint32_t word;
        if (inode_read_word(fs, ip, woff, &word))
            return -EIO;
        uint32_t c = (bpos & 1) ? (word & 0x7f) : ((word >> 9) & 0x7f);
        buf[done++] = (uint8_t)c;
    }
    return (ssize_t)done;
}

ssize_t p7fs_file_write(p7fs_t *fs, p7_inode_t *ip, const uint8_t *buf, size_t size, off_t off) {
    (void)fs; (void)ip; (void)buf; (void)size; (void)off;
    return -EROFS;
}

/* ---- directories -------------------------------------------------------- */

int p7fs_dir_read(p7fs_t *fs, p7_inode_t *ip, p7_dirent_t **ents, size_t *count) {
    if (!(ip->mode & P7_IDIR))
        return -ENOTDIR;
    uint32_t nwords = ip->size / 2;
    size_t ndirents = nwords / P7_DIRENTSZ;
    /* +2 for the synthesized "." and ".." (absent on a real PDP-7 disk) */
    p7_dirent_t *out = calloc(ndirents + 2, sizeof(*out));
    if (!out)
        return -ENOMEM;

    size_t cnt = 0;
    out[cnt].ino = ip->ino;       strcpy(out[cnt].name, ".");   cnt++;
    out[cnt].ino = P7_ROOTINO;    strcpy(out[cnt].name, "..");  cnt++;

    for (size_t d = 0; d < ndirents; d++) {
        uint32_t base = (uint32_t)(d * P7_DIRENTSZ);
        uint32_t dino;
        if (inode_read_word(fs, ip, base, &dino))
            goto fail;
        if (dino == 0)
            continue;
        uint32_t namew[4];
        for (int w = 0; w < 4; w++)
            if (inode_read_word(fs, ip, base + 1 + w, &namew[w]))
                goto fail;
        out[cnt].ino = dino;
        unpack_name(namew, out[cnt].name);
        cnt++;
    }
    *ents = out;
    *count = cnt;
    return 0;

fail:
    free(out);
    return -EIO;
}

void p7fs_dirents_free(p7_dirent_t *ents) {
    free(ents);
}

int p7fs_dir_lookup(p7fs_t *fs, p7_inode_t *ip, const char *name, uint32_t *ino) {
    if (!strcmp(name, "."))  { *ino = ip->ino; return 0; }
    if (!strcmp(name, "..")) { *ino = P7_ROOTINO; return 0; }

    uint32_t nwords = ip->size / 2;
    size_t ndirents = nwords / P7_DIRENTSZ;
    for (size_t d = 0; d < ndirents; d++) {
        uint32_t base = (uint32_t)(d * P7_DIRENTSZ);
        uint32_t dino;
        if (inode_read_word(fs, ip, base, &dino))
            return -EIO;
        if (dino == 0)
            continue;
        uint32_t namew[4];
        for (int w = 0; w < 4; w++)
            if (inode_read_word(fs, ip, base + 1 + w, &namew[w]))
                return -EIO;
        char ent[P7_DIRSIZ + 1];
        unpack_name(namew, ent);
        if (!strcmp(ent, name)) {
            *ino = dino;
            return 0;
        }
    }
    return -ENOENT;
}

int p7fs_dir_add(p7fs_t *fs, p7_inode_t *ip, uint32_t ino, const char *name) {
    (void)fs; (void)ip; (void)ino; (void)name;
    return -EROFS;
}

int p7fs_dir_remove(p7fs_t *fs, p7_inode_t *ip, const char *name) {
    (void)fs; (void)ip; (void)name;
    return -EROFS;
}

/* ---- path lookup -------------------------------------------------------- */

int p7fs_lookup(p7fs_t *fs, const char *path, uint32_t *ino, p7_inode_t *ip) {
    if (path[0] != '/')
        return -EINVAL;
    uint32_t cur = P7_ROOTINO;
    p7_inode_t dip;
    if (p7fs_read_inode(fs, cur, &dip))
        return -EIO;

    const char *p = path + 1;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (len == 0) { p++; continue; }
        if (len > P7_DIRSIZ)
            return -ENAMETOOLONG;
        char name[P7_DIRSIZ + 1];
        memcpy(name, p, len);
        name[len] = 0;

        if (!(dip.mode & P7_IDIR))
            return -ENOTDIR;
        uint32_t next;
        int rc = p7fs_dir_lookup(fs, &dip, name, &next);
        if (rc)
            return rc;
        if (p7fs_read_inode(fs, next, &dip))
            return -EIO;
        p = slash ? slash + 1 : p + len;
    }
    *ino = dip.ino;
    if (ip)
        *ip = dip;
    return 0;
}

/* ---- integrity check ---------------------------------------------------- */

int p7fs_check(p7fs_t *fs, p7_check_t *rep) {
    memset(rep, 0, sizeof(*rep));
    rep->inodes = P7_MAXINO;

    uint32_t head = fs->freelist, guard = 0;
    while (head && guard++ < P7_NBLOCKS) {
        uint32_t fl[P7_WSIZE];
        if (read_words(fs, head, fl)) {
            printf("free-list block %u unreadable\n", head);
            rep->errors++;
            break;
        }
        for (int i = 1; i <= 9; i++)
            if (fl[i] != 0)
                rep->free_blocks++;
        head = fl[0] & P7_MAXWORD;
    }

    for (uint32_t ino = 1; ino <= P7_MAXINO; ino++) {
        p7_inode_t ip;
        if (p7fs_read_inode(fs, ino, &ip)) {
            rep->errors++;
            continue;
        }
        if (!(ip.mode & P7_IUSED))
            continue;
        rep->used_inodes++;
    }

    printf("free blocks=%u  inodes=%u/%u used  errors=%u\n",
           rep->free_blocks, rep->used_inodes, rep->inodes, rep->errors);
    return rep->errors ? -1 : 0;
}

/* ---- ops table ----------------------------------------------------------
 * Each op takes `void *` (the backend state).  The adapters forward to the
 * typed backend function; the `void *` argument converts implicitly to
 * p7fs_t*, so there is no cast anywhere. */

static int p7fs_open_op(void *fs, const char *path, int readonly, int le, uint64_t offset) {
    (void)le;   /* no byte-order variant */
    return p7fs_open(fs, path, readonly, offset);
}
static void p7fs_close_op(void *fs) { p7fs_close(fs); }
static int p7fs_sync_op(void *fs) { return p7fs_sync(fs); }
static int p7fs_read_block_op(void *fs, uint32_t bno, uint8_t *buf)
{ return p7fs_read_block(fs, bno, buf); }
static int p7fs_write_block_op(void *fs, uint32_t bno, const uint8_t *buf)
{ return p7fs_write_block(fs, bno, buf); }
static int p7fs_read_inode_op(void *fs, uint32_t ino, filsys_inode_t *ip)
{ return p7fs_read_inode(fs, ino, ip); }
static int p7fs_write_inode_op(void *fs, uint32_t ino, const filsys_inode_t *ip)
{ return p7fs_write_inode(fs, ino, ip); }
static int p7fs_ialloc_op(void *fs, uint32_t *ino)
{ return p7fs_ialloc(fs, ino); }
static void p7fs_ifree_op(void *fs, uint32_t ino) { p7fs_ifree(fs, ino); }
static int p7fs_bmap_op(void *fs, filsys_inode_t *ip, uint32_t lbn, int create, uint32_t *bno)
{ return p7fs_bmap(fs, ip, lbn, create, bno); }
static int p7fs_itrunc_op(void *fs, filsys_inode_t *ip)
{ return p7fs_itrunc(fs, ip); }
static int p7fs_itrunc_from_op(void *fs, filsys_inode_t *ip, uint32_t first_blk)
{ return p7fs_itrunc_from(fs, ip, first_blk); }
static ssize_t p7fs_file_read_op(void *fs, filsys_inode_t *ip, uint8_t *buf, size_t size, off_t off)
{ return p7fs_file_read(fs, ip, buf, size, off); }
static ssize_t p7fs_file_write_op(void *fs, filsys_inode_t *ip, const uint8_t *buf, size_t size, off_t off)
{ return p7fs_file_write(fs, ip, buf, size, off); }
static int p7fs_dir_read_op(void *fs, filsys_inode_t *ip, filsys_dirent_t **ents, size_t *count)
{ return p7fs_dir_read(fs, ip, ents, count); }
static int p7fs_dir_lookup_op(void *fs, filsys_inode_t *ip, const char *name, uint32_t *ino)
{ return p7fs_dir_lookup(fs, ip, name, ino); }
static int p7fs_dir_add_op(void *fs, filsys_inode_t *ip, uint32_t ino, const char *name)
{ return p7fs_dir_add(fs, ip, ino, name); }
static int p7fs_dir_remove_op(void *fs, filsys_inode_t *ip, const char *name)
{ return p7fs_dir_remove(fs, ip, name); }
static int p7fs_lookup_op(void *fs, const char *path, uint32_t *ino, filsys_inode_t *ip)
{ return p7fs_lookup(fs, path, ino, ip); }
static int p7fs_check_op(void *fs) { p7_check_t rep; return p7fs_check(fs, &rep); }
static uint64_t p7fs_max_file_op(void *fs) {
    (void)fs;
    return (uint64_t)P7_NIADDR * P7_NINDIR * P7_WSIZE * 2;   /* 7*64*64*2 bytes */
}

const struct filsys_ops p7fs_ops = {
    .name        = "pdp7",
    .open        = p7fs_open_op,
    .close       = p7fs_close_op,
    .sync        = p7fs_sync_op,
    .read_block  = p7fs_read_block_op,
    .write_block = p7fs_write_block_op,
    .read_inode  = p7fs_read_inode_op,
    .write_inode = p7fs_write_inode_op,
    .ialloc      = p7fs_ialloc_op,
    .ifree       = p7fs_ifree_op,
    .bmap        = p7fs_bmap_op,
    .itrunc      = p7fs_itrunc_op,
    .itrunc_from = p7fs_itrunc_from_op,
    .file_read   = p7fs_file_read_op,
    .file_write  = p7fs_file_write_op,
    .dir_read    = p7fs_dir_read_op,
    .dir_lookup  = p7fs_dir_lookup_op,
    .dir_add     = p7fs_dir_add_op,
    .dir_remove  = p7fs_dir_remove_op,
    .lookup      = p7fs_lookup_op,
    .check       = p7fs_check_op,
    .max_file    = p7fs_max_file_op,
};
