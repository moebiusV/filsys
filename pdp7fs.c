/* filsys 1.2.4 - 2026-08-26 - Copyright (C) 2026 David Walther */
/* SPDX-License-Identifier: ISC */
/* pdp7fs.c - PDP-7 Unix filesystem, on-disk access layer.
 *
 * The first Unix filesystem (Bell Labs, 1969) is word-addressed: 18-bit words,
 * 64-word blocks.  This backend unpacks the SimH RB09 image (one word per
 * 4-byte little-endian slot, filesystem on surface 1) into 32-bit words and
 * presents the result through the byte-oriented filsys ops table -- file sizes
 * and offsets are doubled (two 9-bit characters per word) so text files read
 * back as plain ASCII.  See pdp7fs.h and pdp7-unix's tools/mkfs7.
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

static int write_words(p7fs_t *fs, uint32_t bno, const uint32_t *words) {
    uint8_t raw[P7_BLOCKBYTES];
    for (int i = 0; i < P7_WSIZE; i++)
        p7_putword(raw + i * P7_WORDBYTES, words[i]);
    off_t pos = (off_t)fs->base + P7_SURFACE1 + (off_t)bno * P7_BLOCKBYTES;
    ssize_t n = pwrite(fs->fd, raw, P7_BLOCKBYTES, pos);
    if (n != P7_BLOCKBYTES)
        return -EIO;
    return 0;
}

/* Persist the free-list head into block 0 word 0. */
static int super_write(p7fs_t *fs) {
    uint32_t sb[P7_WSIZE];
    if (read_words(fs, 0, sb))
        return -EIO;
    sb[0] = fs->freelist & P7_MAXWORD;
    return write_words(fs, 0, sb);
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
        fs->tfree++;            /* the node block itself is free */
        for (int i = 1; i <= 9; i++)
            if (fl[i] != 0)
                fs->tfree++;
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
    for (int i = 0; i < P7_WSIZE; i++)
        p7_putword(buf + i * P7_WORDBYTES, words[i]);
    return 0;
}

int p7fs_write_block(p7fs_t *fs, uint32_t bno, const uint8_t *buf) {
    if (fs->readonly)
        return -EROFS;
    uint32_t words[P7_WSIZE];
    for (int i = 0; i < P7_WSIZE; i++)
        words[i] = p7_getword(buf + i * P7_WORDBYTES);
    return write_words(fs, bno, words);
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
    return write_words(fs, *bno, z);
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

/* Free the 64 block pointers in a single-indirect block, then the block. */
static void free_indirect(p7fs_t *fs, uint32_t blk) {
    if (blk == 0)
        return;
    uint32_t words[P7_WSIZE];
    if (read_words(fs, blk, words))
        return;
    for (int i = 0; i < P7_NINDIR; i++)
        if (words[i] != 0)
            p7fs_bfree(fs, words[i]);
    p7fs_bfree(fs, blk);
}

int p7fs_itrunc(p7fs_t *fs, p7_inode_t *ip) {
    if (fs->readonly)
        return -EROFS;
    if (ip->mode & P7_ILARG) {
        for (int i = 0; i < P7_NIADDR; i++) {
            uint32_t blk = ip->addr[i];
            if (blk) { ip->addr[i] = 0; free_indirect(fs, blk); }
        }
    } else {
        for (int i = 0; i < P7_NIADDR; i++) {
            uint32_t blk = ip->addr[i];
            if (blk) { ip->addr[i] = 0; p7fs_bfree(fs, blk); }
        }
    }
    ip->size = 0;
    return 0;
}

/* Free blocks [first_blk, ...) only; first_blk == 0 == p7fs_itrunc. */
int p7fs_itrunc_from(p7fs_t *fs, p7_inode_t *ip, uint32_t first_blk) {
    if (fs->readonly)
        return -EROFS;
    if (ip->mode & P7_ILARG) {
        uint32_t slot   = first_blk / P7_NINDIR;
        uint32_t within = first_blk % P7_NINDIR;
        for (int i = P7_NIADDR - 1; i > (int)slot; i--) {
            if (ip->addr[i]) { free_indirect(fs, ip->addr[i]); ip->addr[i] = 0; }
        }
        if (ip->addr[slot]) {
            if (within == 0) {
                free_indirect(fs, ip->addr[slot]);
                ip->addr[slot] = 0;
            } else {
                uint32_t words[P7_WSIZE];
                if (read_words(fs, ip->addr[slot], words) == 0) {
                    for (uint32_t j = within; j < P7_NINDIR; j++) {
                        if (words[j]) { p7fs_bfree(fs, words[j]); words[j] = 0; }
                    }
                    write_words(fs, ip->addr[slot], words);
                }
            }
        }
    } else {
        for (int i = P7_NIADDR - 1; i >= (int)first_blk; i--) {
            if (ip->addr[i]) { p7fs_bfree(fs, ip->addr[i]); ip->addr[i] = 0; }
        }
    }
    return 0;
}

/* ---- block mapping ------------------------------------------------------ */

int p7fs_bmap(p7fs_t *fs, p7_inode_t *ip, uint32_t lbn, int create, uint32_t *bno) {
    /* A write past the seven direct slots promotes a small file to a large one:
     * the seven direct block numbers move into the first indirect block. */
    if (!(ip->mode & P7_ILARG) && create && lbn >= P7_NIADDR) {
        uint32_t iblk;
        if (p7fs_balloc(fs, &iblk))
            return -ENOSPC;
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
            if (p7fs_balloc(fs, &iblk))
                return -ENOSPC;
            uint32_t z[P7_WSIZE] = {0};
            if (write_words(fs, iblk, z))
                return -EIO;
            ip->addr[slot] = iblk;
        }
        uint32_t words[P7_WSIZE];
        if (read_words(fs, iblk, words))
            return -EIO;
        if (words[idx] == 0 && create) {
            if (p7fs_balloc(fs, &words[idx]))
                return -ENOSPC;
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
        if (p7fs_balloc(fs, &ip->addr[lbn]))
            return -ENOSPC;
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
    if (off < 0)
        return -EINVAL;
    if (fs->readonly)
        return -EROFS;

    size_t done = 0;
    while (done < size) {
        uint64_t bpos = (uint64_t)off + done;   /* byte position */
        uint32_t woff = (uint32_t)(bpos / 2);   /* word position */
        uint32_t word;
        if (inode_read_word(fs, ip, woff, &word))
            return -EIO;
        uint32_t c = buf[done];
        if (bpos & 1)
            word = (word & ~(uint32_t)0x1ff) | (c & 0x7f);   /* low char */
        else
            word = (word & 0x1ff) | ((c & 0x7f) << 9);       /* high char */
        if (inode_write_word(fs, ip, woff, word))
            return -EIO;
        done++;
    }
    if ((uint64_t)off + size > ip->size)
        ip->size = (uint32_t)(off + size);
    p7fs_write_inode(fs, ip->ino, ip);
    return (ssize_t)done;
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
        char ent[P7_DIRSIZ + 1];
        unpack_name(namew, ent);
        if (!strcmp(ent, ".") || !strcmp(ent, ".."))
            continue;   /* synthesized above; a mkdir'd copy is redundant */
        out[cnt].ino = dino;
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
    for (int w = 0; w < 4; w++)
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
    for (int w = 0; w < 4; w++)
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
        for (int w = 0; w < 4; w++)
            if (inode_read_word(fs, ip, base + 1 + w, &namew[w]))
                return -EIO;
        char ent[P7_DIRSIZ + 1];
        unpack_name(namew, ent);
        if (!strcmp(ent, name))
            return inode_write_word(fs, ip, base, 0);   /* zero the i-number */
    }
    return -ENOENT;
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

/* ---- check / salvage / resolve-dups -------------------------------------- */

typedef struct {
    p7fs_t   *fs;
    uint8_t  *bmap;        /* bit i = data block (P7_DATASTART + i) */
    uint32_t  nblk;
    uint32_t  used_blocks;
    uint32_t  dup_blocks;
    uint32_t  errors;
    uint32_t  ino;
} p7_chkctx_t;

static int p7_mark_block(p7_chkctx_t *cx, uint32_t bno)
{
    if (bno == 0)
        return 0;
    if (bno < P7_DATASTART || bno >= P7_KDATA) {
        printf("block %u bad; inode=%u\n", bno, cx->ino);
        cx->errors++;
        return 1;
    }
    uint32_t d = bno - P7_DATASTART;
    uint8_t  m = (uint8_t)(1u << (d & 7));
    if (cx->bmap[d >> 3] & m) {
        printf("block %u dup; inode=%u\n", bno, cx->ino);
        cx->dup_blocks++;
        cx->errors++;
        return 0;
    }
    cx->bmap[d >> 3] |= m;
    cx->used_blocks++;
    return 0;
}

/* PDP-7 large file: all 7 slots are single-indirect (64 18-bit pointers). */
static void p7_mark_tree(p7_chkctx_t *cx, uint32_t blk)
{
    if (blk == 0)
        return;
    if (p7_mark_block(cx, blk))
        return;
    uint32_t words[P7_WSIZE];
    if (read_words(cx->fs, blk, words)) {
        printf("cannot read indirect block %u\n", blk);
        cx->errors++;
        return;
    }
    for (int i = 0; i < P7_NINDIR; i++) {
        uint32_t nb = words[i] & P7_MAXWORD;
        if (nb != 0)
            p7_mark_block(cx, nb);
    }
}

/* Rebuild the free list from the usage bitmap (icheck -s), chaining unused
 * data blocks back through the 9-per-node free-list allocator. */
static void p7fs_makefree(p7fs_t *fs, p7_chkctx_t *cx)
{
    uint32_t nfree = 0;
    fs->freelist = 0;
    fs->tfree = 0;
    for (uint32_t b = P7_KDATA - 1; b >= P7_DATASTART; b--) {
        uint32_t off = b - P7_DATASTART;
        if (!(cx->bmap[off >> 3] & (uint8_t)(1u << (off & 7)))) {
            p7fs_bfree(fs, b);
            nfree++;
        }
    }
    fs->tfree = nfree;
    super_write(fs);
}

int p7fs_check(p7fs_t *fs, p7_check_t *rep, int salvage) {
    memset(rep, 0, sizeof(*rep));
    rep->inodes = P7_MAXINO;

    uint32_t nblk = P7_KDATA - P7_DATASTART;

    p7_chkctx_t cx;
    memset(&cx, 0, sizeof(cx));
    cx.fs = fs;
    cx.nblk = nblk;
    cx.bmap = calloc((nblk + 7) / 8, 1);
    if (!cx.bmap)
        return -ENOMEM;

    for (uint32_t ino = 1; ino <= P7_MAXINO; ino++) {
        p7_inode_t ip;
        if (p7fs_read_inode(fs, ino, &ip)) {
            rep->errors++;
            continue;
        }
        if (!(ip.mode & P7_IUSED))
            continue;
        rep->used_inodes++;
        if (ip.mode & P7_ISPEC)         /* device: addr[0] is a device no. */
            continue;
        cx.ino = ino;
        for (int i = 0; i < P7_NIADDR; i++) {
            uint32_t a = ip.addr[i];
            if (a == 0)
                continue;
            if (ip.mode & P7_ILARG)
                p7_mark_tree(&cx, a);
            else
                p7_mark_block(&cx, a);
        }
    }
    rep->errors += cx.errors;
    rep->dup_blocks = cx.dup_blocks;

    if (salvage) {
        p7fs_makefree(fs, &cx);
        printf("salvaged: free list rebuilt (%u free blocks)\n", fs->tfree);
        free(cx.bmap);
        return rep->errors ? -1 : 0;
    }

    /* Walk the free list exactly as alloc() would, marking free blocks, then
     * flag blocks both used and free, and blocks missing. */
    uint8_t *freeb = calloc(nblk ? nblk : 1, 1);
    if (!freeb) {
        free(cx.bmap);
        return -ENOMEM;
    }
    uint32_t head = fs->freelist, guard = 0;
    while (head && guard++ < P7_NBLOCKS) {
        uint32_t fl[P7_WSIZE];
        if (read_words(fs, head, fl)) {
            printf("free-list block %u unreadable\n", head);
            rep->errors++;
            break;
        }
        /* the free-list node block itself is a free block (it is handed out
         * once its nine listed blocks are consumed) */
        if (head >= P7_DATASTART && head < P7_KDATA) {
            uint32_t off = head - P7_DATASTART;
            freeb[off >> 3] |= (uint8_t)(1u << (off & 7));
            rep->free_blocks++;
        }
        for (int i = 1; i <= 9; i++) {
            uint32_t f = fl[i];
            if (f == 0)
                continue;
            if (f < P7_DATASTART || f >= P7_KDATA) {
                printf("free block %u out of range\n", f);
                rep->errors++;
                continue;
            }
            uint32_t off = f - P7_DATASTART;
            freeb[off >> 3] |= (uint8_t)(1u << (off & 7));
            rep->free_blocks++;
        }
        head = fl[0] & P7_MAXWORD;
    }

    for (uint32_t b = P7_DATASTART; b < P7_KDATA; b++) {
        uint32_t off = b - P7_DATASTART;
        int used = cx.bmap[off >> 3] & (uint8_t)(1u << (off & 7));
        int fre  = freeb[off >> 3] & (uint8_t)(1u << (off & 7));
        if (used && fre) {
            printf("block %u used and free\n", b);
            rep->errors++;
        } else if (!used && !fre) {
            printf("block %u missing\n", b);
            rep->missing_blocks++;
            rep->errors++;
        }
    }

    free(freeb);
    free(cx.bmap);
    printf("used blocks=%u  free blocks=%u  missing=%u  dup=%u  inodes=%u/%u used  errors=%u\n",
           cx.used_blocks, rep->free_blocks, rep->missing_blocks, rep->dup_blocks,
           rep->used_inodes, rep->inodes, rep->errors);
    return rep->errors ? -1 : 0;
}

int p7fs_resolve_dups(p7fs_t *fs)
{
    uint32_t nblk = P7_KDATA - P7_DATASTART;

    p7_chkctx_t cx;
    memset(&cx, 0, sizeof(cx));
    cx.fs = fs;
    cx.nblk = nblk;
    cx.bmap = calloc((nblk + 7) / 8, 1);
    if (!cx.bmap)
        return -ENOMEM;

    struct dup { uint32_t blk, ino, idx; };
    struct dup *dups = NULL;
    size_t ndup = 0, cap = 0;

    for (uint32_t ino = 1; ino <= P7_MAXINO; ino++) {
        p7_inode_t ip;
        if (p7fs_read_inode(fs, ino, &ip))
            continue;
        if (!(ip.mode & P7_IUSED))
            continue;
        if (ip.mode & P7_ISPEC)
            continue;
        cx.ino = ino;
        for (int i = 0; i < P7_NIADDR; i++) {
            uint32_t a = ip.addr[i];
            if (a == 0)
                continue;
            if (ip.mode & P7_ILARG) {
                p7_mark_tree(&cx, a);
                continue;
            }
            if (a < P7_DATASTART || a >= P7_KDATA) {
                printf("block %u bad; inode=%u\n", a, ino);
                continue;
            }
            uint32_t off = a - P7_DATASTART;
            uint8_t  m = (uint8_t)(1u << (off & 7));
            if (cx.bmap[off >> 3] & m) {
                if (ndup == cap) {
                    cap = cap ? cap * 2 : 16;
                    dups = realloc(dups, cap * sizeof(*dups));
                }
                dups[ndup].blk = a;
                dups[ndup].ino = ino;
                dups[ndup].idx = i;
                ndup++;
            } else {
                cx.bmap[off >> 3] |= m;
                cx.used_blocks++;
            }
        }
    }

    if (ndup == 0) {
        printf("no duplicate blocks\n");
        free(cx.bmap);
        return 0;
    }

    printf("%zu duplicate block(s); rebuilding free list\n", ndup);
    p7fs_makefree(fs, &cx);

    int resolved = 0;
    for (size_t k = 0; k < ndup; k++) {
        uint32_t blk = dups[k].blk, ino = dups[k].ino, idx = dups[k].idx;
        p7_inode_t ip;
        if (p7fs_read_inode(fs, ino, &ip))
            continue;
        if (ip.addr[idx] != blk)
            continue;
        uint32_t nb;
        if (p7fs_balloc(fs, &nb)) {
            printf("block %u dup; inode=%u: out of space\n", blk, ino);
            continue;
        }
        uint32_t src[P7_WSIZE];
        if (read_words(fs, blk, src) || write_words(fs, nb, src)) {
            printf("block %u dup; inode=%u: copy failed\n", blk, ino);
            continue;
        }
        ip.addr[idx] = nb;
        p7fs_write_inode(fs, ino, &ip);
        uint32_t off = nb - P7_DATASTART;
        cx.bmap[off >> 3] |= (uint8_t)(1u << (off & 7));
        printf("block %u dup; inode=%u: copied to %u\n", blk, ino, nb);
        resolved++;
    }
    free(dups);

    printf("resolved %d/%zu duplicates; finalizing free list\n", resolved, ndup);
    p7fs_makefree(fs, &cx);
    free(cx.bmap);
    return resolved == (int)ndup ? 0 : -1;
}

/* ---- maintenance: ncheck / clri ---------------------------------------- */

static void p7_ncheck_dir(p7fs_t *fs, uint32_t dirino, const char *prefix,
                          uint32_t target, int *found, int depth)
{
    if (depth > 64)
        return;   /* the "dd" entry cycles back to root */
    p7_inode_t ip;
    if (p7fs_read_inode(fs, dirino, &ip))
        return;
    if (!(ip.mode & P7_IDIR))
        return;
    p7_dirent_t *ents = NULL;
    size_t cnt = 0;
    if (p7fs_dir_read(fs, &ip, &ents, &cnt))
        return;
    for (size_t i = 0; i < cnt; i++) {
        uint32_t eino = ents[i].ino;
        if (eino == 0)
            continue;
        if (ents[i].name[0] == '.' &&
            (ents[i].name[1] == 0 ||
             (ents[i].name[1] == '.' && ents[i].name[2] == 0)))
            continue;
        char path[1024];
        if (prefix[1] == 0)
            snprintf(path, sizeof(path), "/%s", ents[i].name);
        else
            snprintf(path, sizeof(path), "%s/%s", prefix, ents[i].name);
        if (eino == target) {
            printf("%u\t%s\n", target, path);
            *found = 1;
        }
        p7_inode_t cip;
        if (p7fs_read_inode(fs, eino, &cip) == 0 && (cip.mode & P7_IDIR))
            p7_ncheck_dir(fs, eino, path, target, found, depth + 1);
    }
    p7fs_dirents_free(ents);
}

int p7fs_ncheck(p7fs_t *fs, uint32_t ino)
{
    int found = 0;
    p7_ncheck_dir(fs, P7_ROOTINO, "/", ino, &found, 0);
    if (!found)
        printf("%u: not found\n", ino);
    return 0;
}

int p7fs_clri(p7fs_t *fs, uint32_t ino)
{
    if (ino == 0 || ino > P7_MAXINO)
        return -EINVAL;
    p7_inode_t ip;
    memset(&ip, 0, sizeof(ip));
    ip.ino = ino;
    int rc = p7fs_write_inode(fs, ino, &ip);
    if (rc == 0)
        printf("cleared inode %u\n", ino);
    return rc;
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
static int p7fs_check_op(void *fs) { p7_check_t rep; return p7fs_check(fs, &rep, 0); }
static uint64_t p7fs_max_file_op(void *fs) {
    (void)fs;
    return (uint64_t)P7_NIADDR * P7_NINDIR * P7_WSIZE * 2;   /* 7*64*64*2 bytes */
}

const struct filsys_ops p7fs_ops = {
    .name        = "pdp7",
    .blocksize   = P7_WSIZE * 2,   /* 64 words x 2 chars = 128 bytes */
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
