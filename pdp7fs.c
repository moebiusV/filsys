/* filsys 1.8.0 - 2026-09-06 - Copyright (C) 2026 David Walther */
/* SPDX-License-Identifier: ISC */
/* pdp7fs.c - PDP-7 Unix filesystem, on-disk access layer.
 *
 * The first Unix filesystem (Bell Labs, 1969) is word-addressed: 18-bit words,
 * 64-word blocks.  This backend unpacks the SimH RB09 image (one word per
 * 4-byte little-endian slot) into 32-bit words and presents the result through
 * the byte-oriented filsys ops table -- file sizes and offsets are doubled (two
 * 7-bit ASCII characters per 18-bit word, packed in the low bits of each 9-bit
 * half) so text files read back as plain ASCII.  The mount offset points at the
 * filesystem surface: byte 0 of a bare surface image, or P7_NBLOCKS*block_bytes
 * into a full two-surface RB09 disk image.  See pdp7fs.h and pdp7-unix's
 * tools/mkfs7.
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

/* Read block `bno` (64 words) out of the image's filesystem surface.  `fs->base`
 * is the byte offset of the surface itself, so this matches every other
 * backend's "-o offset=" convention. */
static int read_words(p7fs_t *fs, uint32_t bno, uint32_t *words) {
    uint8_t raw[P7_MAXBLOCKBYTES];
    uint32_t bb = fs->word->block_bytes;
    off_t pos = (off_t)fs->base + (off_t)bno * (off_t)bb;
    if (fs->io->read(fs, raw, bb, pos))
        return -EIO;
    for (uint32_t i = 0; i < P7_WSIZE; i++)
        words[i] = fs->word->get(raw, i);
    return 0;
}

static int write_words(p7fs_t *fs, uint32_t bno, const uint32_t *words) {
    uint8_t raw[P7_MAXBLOCKBYTES];
    for (uint32_t i = 0; i < P7_WSIZE; i++)
        fs->word->put(raw, i, words[i]);
    uint32_t bb = fs->word->block_bytes;
    off_t pos = (off_t)fs->base + (off_t)bno * (off_t)bb;
    if (fs->io->write(fs, raw, bb, pos))
        return -EIO;
    return 0;
}

/* Persist the free-list head into block 0 word 0. */
static int super_write(p7fs_t *fs) {
    uint32_t sb[P7_WSIZE];
    if (read_words(fs, 0, sb))
        return -EIO;
    sb[0] = fs->freelist & P7_MAXWORD;
    if (write_words(fs, 0, sb))
        return -EIO;
    fs->fl_dirty = 0;
    return 0;
}

/* Sign-extend an 18-bit word into a signed 32-bit value. */
static int32_t sign18(uint32_t v) {
    v &= P7_MAXWORD;
    return (v & 0400000) ? (int32_t)(v - 01000000u) : (int32_t)v;
}

/* ---- word container codecs ---------------------------------------------- */

/* SimH RB09: one 18-bit word per 4-byte little-endian slot. */
static uint32_t rb09_get(const uint8_t *buf, uint32_t i) {
    return bo_get32le(buf + 4 * i) & P7_MAXWORD;
}
static void rb09_put(uint8_t *buf, uint32_t i, uint32_t v) {
    bo_put32le(buf + 4 * i, v & P7_MAXWORD);
}

/* Paper-tape RIM (mkfs7's word2three): 18 bits as three 6-bit frames, high six
 * bits first, each frame in the low six bits of a byte. */
static uint32_t rim_get(const uint8_t *buf, uint32_t i) {
    return ((uint32_t)(buf[3 * i]     & 0x3fu) << 12) |
           ((uint32_t)(buf[3 * i + 1] & 0x3fu) << 6)  |
            (uint32_t)(buf[3 * i + 2] & 0x3fu);
}
static void rim_put(uint8_t *buf, uint32_t i, uint32_t v) {
    buf[3 * i]     = (uint8_t)((v >> 12) & 0x3fu);
    buf[3 * i + 1] = (uint8_t)((v >> 6)  & 0x3fu);
    buf[3 * i + 2] = (uint8_t)(v         & 0x3fu);
}

const word_codec_t word_rb09     = { rb09_get,       rb09_put,       P7_WSIZE * 4 };
const word_codec_t word_packed18 = { bo_get18packed, bo_put18packed, P7_WSIZE * 18 / 8 };
const word_codec_t word_rim      = { rim_get,        rim_put,        P7_WSIZE * 3 };

const word_codec_t *filsys_word_codec_by_name(const char *name) {
    if (!name)
        return NULL;
    if (!strcmp(name, "rb09") || !strcmp(name, "simh"))
        return &word_rb09;
    if (!strcmp(name, "packed18") || !strcmp(name, "packed"))
        return &word_packed18;
    if (!strcmp(name, "rim"))
        return &word_rim;
    return NULL;
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

/* Write logical word `woff` of an inode's data (allocating the block if the
 * word lies in a hole). */
static int inode_write_word(p7fs_t *fs, p7_inode_t *ip, uint32_t woff, uint32_t val) {
    uint32_t lbn = woff / P7_WSIZE;
    uint32_t idx = woff % P7_WSIZE;
    uint32_t pbn;
    if (p7fs_bmap(fs, ip, lbn, 1, &pbn))
        return -EIO;
    if (pbn == 0)
        return -EIO;
    uint32_t words[P7_WSIZE];
    if (read_words(fs, pbn, words))
        return -EIO;
    words[idx] = val;
    return write_words(fs, pbn, words);
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

int p7fs_open(p7fs_t *fs, const char *path, int readonly,
              const filsys_edition_t *proto, uint64_t offset) {
    if (fs != proto)
        memcpy(fs, proto, sizeof *fs); /* copy the static descriptor fields */
    fs->readonly = readonly;
    fs->base = offset;
    fs->fd = open(path, readonly ? O_RDONLY : O_RDWR);
    if (fs->fd < 0)
        return -errno;
    fs->io = &filsys_io_file;

    uint32_t bb = fs->word->block_bytes;
    uint64_t imgsize;
    if (filsys_dev_size(fs->fd, &imgsize) != 0 ||
        fs->base + bb > imgsize) {
        close(fs->fd);
        fs->fd = -1;
        return -EINVAL;   /* image too small to hold the filesystem surface */
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
        fs->fl.tfree++;         /* the node block itself is free */
        for (int i = 1; i <= 9; i++)
            if (fl[i] != 0)
                fs->fl.tfree++;
        head = fl[0] & P7_MAXWORD;
    }
    return 0;
}

void p7fs_close(p7fs_t *fs) {
    if (fs->fd >= 0) {
        if (!fs->readonly)
            super_write(fs);
        close(fs->fd);
        fs->fd = -1;
    }
}

int p7fs_sync(p7fs_t *fs) {
    if (fs->readonly)
        return 0;
    return super_write(fs);
}

/* ---- block / inode io -------------------------------------------------- */

int p7fs_read_block(p7fs_t *fs, uint32_t bno, uint8_t *buf) {
    uint32_t words[P7_WSIZE];
    if (read_words(fs, bno, words))
        return -EIO;
    for (uint32_t i = 0; i < P7_WSIZE; i++)
        fs->word->put(buf, i, words[i]);
    return 0;
}

int p7fs_write_block(p7fs_t *fs, uint32_t bno, const uint8_t *buf) {
    if (fs->readonly)
        return -EROFS;
    uint32_t words[P7_WSIZE];
    for (uint32_t i = 0; i < P7_WSIZE; i++)
        words[i] = fs->word->get(buf, i);
    return write_words(fs, bno, words);
}

/* PDP-7's indirect-entry codec (the descriptor's ind_get/ind_put override):
 * the word container codec, applied to the block buffer. */
uint32_t p7_ind_get(const filsys_edition_t *fs, const uint8_t *buf, uint32_t i) {
    return fs->word->get(buf, i);
}
void p7_ind_put(const filsys_edition_t *fs, uint8_t *buf, uint32_t i, uint32_t v) {
    fs->word->put(buf, i, v);
}

/* Data-block codec: 64 words <-> bsize (128) logical bytes, two 7-bit ASCII
 * characters per word in the low bits of each 9-bit half. */
static int p7fs_blk_get(filsys_edition_t *fs, uint32_t bno, uint8_t *buf) {
    p7fs_t *f = fs;
    uint32_t words[P7_WSIZE];
    if (read_words(f, bno, words))
        return -EIO;
    for (uint32_t i = 0; i < P7_WSIZE; i++) {
        buf[2 * i]     = (uint8_t)((words[i] >> 9) & 0x7f);
        buf[2 * i + 1] = (uint8_t)(words[i] & 0x7f);
    }
    return 0;
}
static int p7fs_blk_put(filsys_edition_t *fs, uint32_t bno, const uint8_t *buf) {
    p7fs_t *f = fs;
    if (f->readonly)
        return -EROFS;
    uint32_t words[P7_WSIZE];
    for (uint32_t i = 0; i < P7_WSIZE; i++)
        words[i] = ((uint32_t)(buf[2 * i] & 0x7f) << 9) | (uint32_t)(buf[2 * i + 1] & 0x7f);
    return write_words(f, bno, words);
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
    /* mkfs7 stores the size in words and NUL-pads the low half of the last word
     * for an odd byte count; recover the byte-exact size for regular files
     * (directories are always a whole number of 8-word dirents). */
    if (!(ip->mode & (P7_IDIR | P7_ISPEC)) && ip->size > 0) {
        uint32_t lastw;
        if (inode_read_word(fs, ip, (uint32_t)(ip->size / 2 - 1), &lastw) == 0 &&
            (lastw & 0x1ff) == 0)
            ip->size--;
    }
    ip->atime = ip->mtime = ip->ctime = 0;   /* PDP-7 has no timestamps */
    return 0;
}

int p7fs_write_inode(p7fs_t *fs, uint32_t ino, const p7_inode_t *ip) {
    if (ino == 0 || ino > P7_MAXINO)
        return -EINVAL;
    if (fs->readonly)
        return -EROFS;
    /* Allocator state before reference: flush block 0 (the free-list head and
     * the inode bits) before this inode's reference to a newly-allocated
     * block/inode, so a crash can't leave it both free and referenced. */
    if (fs->fl_dirty) {
        int rc = super_write(fs);
        if (rc)
            return rc;
    }
    uint32_t words[P7_WSIZE];
    if (read_words(fs, p7_itod(ino), words))
        return -EIO;
    uint32_t *d = words + p7_itoo(ino);
    d[0] = ip->mode & P7_MAXWORD;
    for (int i = 0; i < P7_NIADDR; i++)
        d[1 + i] = ip->addr[i] & P7_MAXWORD;
    d[8]  = (uint32_t)(uint16_t)ip->uid & P7_MAXWORD;
    d[9]  = (uint32_t)(-(int32_t)ip->nlink) & P7_MAXWORD;   /* stored negative */
    d[10] = ((ip->size + 1) / 2) & P7_MAXWORD;              /* bytes -> words (round up) */
    /* d[11] (uniq) is left untouched */
    return write_words(fs, p7_itod(ino), words);
}

/* ---- allocation (free list) --------------------------------------------- */

int p7fs_balloc(p7fs_t *fs, uint32_t *bno) {
    if (fs->readonly)
        return -EROFS;
    uint32_t h = fs->freelist;
    if (h == 0)
        return -ENOSPC;
    uint32_t words[P7_WSIZE];
    if (read_words(fs, h, words))
        return -EIO;
    int last = 0;
    for (int i = 1; i <= 9; i++)
        if (words[i] != 0)
            last = i;
    if (last == 0) {
        /* node exhausted: its own block is the free block now */
        fs->freelist = words[0] & P7_MAXWORD;
        *bno = h;
    } else {
        *bno = words[last];
        words[last] = 0;
        if (write_words(fs, h, words))
            return -EIO;
    }
    /* Zero the freshly-allocated block so a deleted file's data doesn't leak
     * into a new one (V7's alloc() clrbuf()s). */
    uint32_t z[P7_WSIZE] = {0};
    if (write_words(fs, *bno, z))
        return -EIO;
    fs->fl_dirty = 1;   /* free-list head changed: flush before reference */
    return 0;
}

void p7fs_bfree(p7fs_t *fs, uint32_t bno) {
    if (bno == 0 || fs->readonly)
        return;
    uint32_t h = fs->freelist;
    if (h == 0) {
        fs->freelist = bno;
        uint32_t words[P7_WSIZE] = {0};
        write_words(fs, bno, words);   /* lone node holding itself */
        return;
    }
    uint32_t words[P7_WSIZE];
    if (read_words(fs, h, words))
        return;
    for (int i = 1; i <= 9; i++) {
        if (words[i] == 0) {
            words[i] = bno;
            write_words(fs, h, words);
            return;
        }
    }
    /* head node full: the freed block becomes a new empty head node */
    uint32_t nw[P7_WSIZE] = {0};
    nw[0] = h;
    write_words(fs, bno, nw);
    fs->freelist = bno;
}

int p7fs_ialloc(p7fs_t *fs, uint32_t *ino) {
    if (fs->readonly)
        return -EROFS;
    for (uint32_t i = 1; i <= P7_MAXINO; i++) {
        uint32_t words[P7_WSIZE];
        if (read_words(fs, p7_itod(i), words))
            return -EIO;
        uint32_t *d = words + p7_itoo(i);
        if (!(d[0] & P7_IUSED)) {
            memset(d, 0, P7_INODESZ * sizeof(uint32_t));
            d[0]  = P7_IUSED;
            d[9]  = P7_MAXWORD;           /* nlink = -1 (one link) */
            d[11] = i & P7_MAXWORD;       /* uniq = inode number */
            if (write_words(fs, p7_itod(i), words))
                return -EIO;
            fs->fl_dirty = 1;   /* inode list changed: flush before reference */
            *ino = i;
            return 0;
        }
    }
    return -ENOSPC;
}

void p7fs_ifree(p7fs_t *fs, uint32_t ino) {
    if (ino == 0 || ino > P7_MAXINO || fs->readonly)
        return;
    uint32_t words[P7_WSIZE];
    if (read_words(fs, p7_itod(ino), words))
        return;
    memset(words + p7_itoo(ino), 0, P7_INODESZ * sizeof(uint32_t));
    write_words(fs, p7_itod(ino), words);
}

/* ---- block mapping ------------------------------------------------------ */

int p7fs_bmap(p7fs_t *fs, p7_inode_t *ip, uint32_t lbn, int create, uint32_t *bno) {
    /* A write past the seven direct slots promotes a small file to a large one:
     * the seven direct block numbers move into the first indirect block. */
    if (!(ip->mode & P7_ILARG) && create && lbn >= P7_NIADDR) {
        uint32_t iblk;
        int rc = p7fs_balloc(fs, &iblk);
        if (rc)
            return rc;   /* -EIO or -EROFS or -ENOSPC */
        uint32_t words[P7_WSIZE] = {0};
        for (int i = 0; i < P7_NIADDR; i++)
            words[i] = ip->addr[i];
        if (write_words(fs, iblk, words))
            return -EIO;
        for (int i = 0; i < P7_NIADDR; i++)
            ip->addr[i] = 0;
        ip->addr[0] = iblk;
        ip->mode |= P7_ILARG;
    }

    if (ip->mode & P7_ILARG) {
        if (lbn >= P7_NIADDR * P7_NINDIR) {
            *bno = 0;
            return create ? -EFBIG : 0;
        }
        uint32_t slot = lbn / P7_NINDIR;
        uint32_t idx  = lbn % P7_NINDIR;
        uint32_t iblk = ip->addr[slot];
        if (iblk == 0) {
            if (!create) { *bno = 0; return 0; }
            int rc = p7fs_balloc(fs, &iblk);
            if (rc)
                return rc;
            uint32_t z[P7_WSIZE] = {0};
            if (write_words(fs, iblk, z))
                return -EIO;
            ip->addr[slot] = iblk;
        }
        uint32_t words[P7_WSIZE];
        if (read_words(fs, iblk, words))
            return -EIO;
        if (words[idx] == 0 && create) {
            int rc = p7fs_balloc(fs, &words[idx]);
            if (rc)
                return rc;
            if (write_words(fs, iblk, words))
                return -EIO;
        }
        *bno = words[idx];
        return 0;
    }

    /* small file: 7 direct block pointers */
    if (lbn >= P7_NIADDR) {
        *bno = 0;
        return create ? -EFBIG : 0;
    }
    if (ip->addr[lbn] == 0 && create) {
        int rc = p7fs_balloc(fs, &ip->addr[lbn]);
        if (rc)
            return rc;
    }
    *bno = ip->addr[lbn];
    return 0;
}

/* ---- file data ----------------------------------------------------------
 * Shared with the V7 engine: blk_get/blk_put unpack each block to bsize
 * logical bytes, so v7fs_file_read/write apply unchanged.
 */

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
    out[cnt].ino = (uint16_t)ip->ino;       strcpy(out[cnt].name, ".");   cnt++;
    out[cnt].ino = P7_ROOTINO;    strcpy(out[cnt].name, "..");  cnt++;

    for (size_t d = 0; d < ndirents; d++) {
        uint32_t base = (uint32_t)(d * P7_DIRENTSZ);
        uint32_t dino;
        if (inode_read_word(fs, ip, base, &dino))
            goto fail;
        if (dino == 0)
            continue;
        uint32_t namew[4];
        for (uint32_t w = 0; w < 4; w++)
            if (inode_read_word(fs, ip, base + 1 + w, &namew[w]))
                goto fail;
        char ent[P7_DIRSIZ + 1];
        unpack_name(namew, ent);
        if (!strcmp(ent, ".") || !strcmp(ent, ".."))
            continue;   /* synthesized above; a mkdir'd copy is redundant */
        out[cnt].ino = (uint16_t)dino;
        memcpy(out[cnt].name, ent, P7_DIRSIZ + 1);
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

int p7fs_dir_add(p7fs_t *fs, p7_inode_t *ip, uint32_t ino, const char *name) {
    size_t namelen = strlen(name);
    if (namelen == 0 || namelen > P7_DIRSIZ)
        return -ENAMETOOLONG;
    if (strchr(name, '/'))
        return -EINVAL;
    if (fs->readonly)
        return -EROFS;

    uint32_t nwords = ip->size / 2;
    size_t ndirents = nwords / P7_DIRENTSZ;

    /* pack the name (space-padded to 8) into four 2-char words */
    char padded[P7_DIRSIZ];
    memset(padded, ' ', P7_DIRSIZ);
    memcpy(padded, name, namelen);
    uint32_t namew[4];
    for (uint32_t w = 0; w < 4; w++)
        namew[w] = ((uint32_t)(padded[2 * w] & 0x7f) << 9) | (padded[2 * w + 1] & 0x7f);

    /* find a free slot (inode number word == 0) */
    size_t slot = SIZE_MAX;
    for (size_t d = 0; d < ndirents; d++) {
        uint32_t dino;
        if (inode_read_word(fs, ip, (uint32_t)(d * P7_DIRENTSZ), &dino))
            return -EIO;
        if (dino == 0) {
            slot = d * P7_DIRENTSZ;
            break;
        }
    }
    if (slot == SIZE_MAX) {
        slot = ndirents * P7_DIRENTSZ;
        nwords += P7_DIRENTSZ;
    }

    if (inode_write_word(fs, ip, (uint32_t)slot, ino))
        return -EIO;
    for (uint32_t w = 0; w < 4; w++)
        if (inode_write_word(fs, ip, (uint32_t)slot + 1 + w, namew[w]))
            return -EIO;
    if (inode_write_word(fs, ip, (uint32_t)slot + 5, ino & P7_MAXWORD))   /* uniq */
        return -EIO;

    ip->size = nwords * 2;
    return p7fs_write_inode(fs, ip->ino, ip);
}

int p7fs_dir_remove(p7fs_t *fs, p7_inode_t *ip, const char *name) {
    if (fs->readonly)
        return -EROFS;
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
        for (uint32_t w = 0; w < 4; w++)
            if (inode_read_word(fs, ip, base + 1 + w, &namew[w]))
                return -EIO;
        char ent[P7_DIRSIZ + 1];
        unpack_name(namew, ent);
        if (!strcmp(ent, name))
            return inode_write_word(fs, ip, base, 0);   /* zero the i-number */
    }
    return -ENOENT;
}

/* Rebuild the free list from the usage bitmap (icheck -s), chaining unused
 * data blocks back through the 9-per-node free-list allocator. */
static uint32_t p7fs_makefree(filsys_edition_t *fs, filsys_chkctx_t *cx)
{
    p7fs_t *f = fs;
    uint32_t nfree = 0;
    f->freelist = 0;
    f->fl.tfree = 0;
    for (uint32_t b = P7_KDATA - 1; b >= P7_DATASTART; b--) {
        uint32_t off = b - P7_DATASTART;
        if (!(cx->bmap[off >> 3] & (uint8_t)(1u << (off & 7)))) {
            p7fs_bfree(f, b);
            nfree++;
        }
    }
    f->fl.tfree = nfree;
    super_write(f);
    return nfree;
}

/* Preen for the PDP-7: fix link counts and reconnect orphaned regular files to
 * lost+found.  The PDP-7 directory has no on-disk "." and ".." (dir_read
 * synthesizes them), so lost+found is created empty -- its "." and ".." are
 * synthesized on read. */
/* Classify an inode's mode into a checker state.  The PDP-7 has two type bits,
 * P7_IDIR and P7_ISPEC (device); every other allocated inode is a regular
 * file, so there is no unknown type. */
static uint8_t p7_inode_state(filsys_edition_t *fs, uint32_t ino, uint32_t mode) {
    (void)fs; (void)ino;
    if (!(mode & P7_IUSED))
        return FILSYS_IN_UNALLOC;
    if (mode & P7_IDIR)  return FILSYS_IN_IDIR;
    if (mode & P7_ISPEC) return FILSYS_IN_ICHR;   /* special (device) file */
    return FILSYS_IN_IREG;
}

static uint32_t p7_chk_maxino(filsys_edition_t *fs)     { (void)fs; return P7_MAXINO; }
static uint32_t p7_chk_data_start(filsys_edition_t *fs) { (void)fs; return P7_DATASTART; }
static uint32_t p7_chk_data_end(filsys_edition_t *fs)   { (void)fs; return P7_KDATA; }
static int p7_chk_is_clean(filsys_edition_t *fs)        { (void)fs; return 0; }

/* Walk the PDP-7 on-disk free list (9 free blocks per 64-word node), marking
 * free blocks into cx->bmap and counting them. */
static void p7_chk_walk_free(filsys_edition_t *fs, filsys_chkctx_t *cx, filsys_check_t *rep)
{
    p7fs_t *f = fs;
    uint8_t *freeb = calloc(cx->nblk ? cx->nblk : 1, 1);
    if (!freeb)
        return;
    uint32_t head = f->freelist, guard = 0;
    while (head && guard++ < P7_NBLOCKS) {
        uint32_t fl[P7_WSIZE];
        if (read_words(f, head, fl)) {
            printf("free-list block %u unreadable\n", head);
            rep->errors++;
            break;
        }
        if (head >= P7_DATASTART && head < P7_KDATA) {
            uint32_t off = head - P7_DATASTART;
            freeb[off >> 3] |= (uint8_t)(1u << (off & 7));
        }
        for (int i = 1; i <= 9; i++) {
            uint32_t b = fl[i];
            if (b == 0)
                continue;
            if (b < P7_DATASTART || b >= P7_KDATA) {
                printf("free block %u out of range\n", b);
                rep->errors++;
                continue;
            }
            uint32_t off = b - P7_DATASTART;
            freeb[off >> 3] |= (uint8_t)(1u << (off & 7));
        }
        head = fl[0] & P7_MAXWORD;
    }
    for (uint32_t b = P7_DATASTART; b < P7_KDATA; b++) {
        uint32_t off = b - P7_DATASTART;
        int fre = freeb[off >> 3] & (uint8_t)(1u << (off & 7));
        if (!fre)
            continue;
        rep->free_blocks++;
        uint8_t m = (uint8_t)(1u << (off & 7));
        if (cx->bmap[off >> 3] & m) {
            printf("block %u used and free\n", b);
            cx->dup_blocks++;
            rep->errors++;
        } else {
            cx->bmap[off >> 3] |= m;
        }
    }
    free(freeb);
}

int p7fs_check(p7fs_t *fs, p7_check_t *rep, int mode) {
    filsys_edition_t fmt = filsys_getformat(FILSYS_PDP7);
    return filsys_check_common(&fmt, fs, rep, mode);
}

/* ---- ops table ----------------------------------------------------------
 * Each op takes `void *` (the backend state).  The adapters forward to the
 * typed backend function; the `void *` argument converts implicitly to
 * p7fs_t*, so there is no cast anywhere. */


static uint32_t p7fs_blocksize_op(const filsys_edition_t *fs) { (void)fs; return P7_WSIZE * 2; }


















static int p7fs_check_op(filsys_edition_t *fs) { p7_check_t rep; return p7fs_check(fs, &rep, 0); }
static uint64_t p7fs_max_file_op(filsys_edition_t *fs) {
    (void)fs;
    return (uint64_t)P7_NIADDR * P7_NINDIR * P7_WSIZE * 2;   /* 7*64*64*2 bytes */
}

static void p7fs_statfs_op(filsys_edition_t *fs, struct statvfs *st) {
    p7fs_t *p7 = fs;
    st->f_blocks = P7_NBLOCKS;
    st->f_bfree = st->f_bavail = p7->fl.tfree;
    st->f_files = P7_MAXINO;
    st->f_ffree = 0;   /* not tracked (read-only) */
}
const struct filsys_ops p7fs_ops = {
    .name        = "pdp7",
    .blocksize   = p7fs_blocksize_op,   /* 64 words x 2 chars = 128 bytes */
    .open        = p7fs_open,
    .close       = p7fs_close,
    .sync        = p7fs_sync,
    .read_block  = p7fs_read_block,
    .write_block = p7fs_write_block,
    .blk_get     = p7fs_blk_get,
    .blk_put     = p7fs_blk_put,
    .read_inode  = p7fs_read_inode,
    .write_inode = p7fs_write_inode,
    .ialloc      = p7fs_ialloc,
    .ifree       = p7fs_ifree,
    .bmap        = p7fs_bmap,
    .file_read   = v7fs_file_read,
    .file_write  = v7fs_file_write,
    .dir_read    = p7fs_dir_read,
    .dir_lookup  = v7fs_dir_lookup,
    .dir_add     = p7fs_dir_add,
    .dir_remove  = p7fs_dir_remove,
    .lookup      = v7fs_lookup,
    .check       = p7fs_check_op,
    .maxino      = p7_chk_maxino,
    .data_start  = p7_chk_data_start,
    .data_end    = p7_chk_data_end,
    .inode_state = p7_inode_state,
    .walk_free   = p7_chk_walk_free,
    .makefree    = p7fs_makefree,
    .is_clean    = p7_chk_is_clean,
    .statfs      = p7fs_statfs_op,
    .max_file    = p7fs_max_file_op,
};

/* ---- allocator vtable: PDP-7's on-disk free list ------------------------ */

const alloc_ops_t pdp7_alloc_ops = {
    .balloc = (int  (*)(void *, uint32_t *))p7fs_balloc,
    .bfree  = (void (*)(void *, uint32_t))p7fs_bfree,
    .ialloc = (int  (*)(void *, uint32_t *))p7fs_ialloc,
    .ifree  = (void (*)(void *, uint32_t))p7fs_ifree,
    .sync   = (int  (*)(void *))p7fs_sync,
};
