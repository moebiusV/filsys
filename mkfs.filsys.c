/* filsys 1.2.5 - 2026-09-04 - Copyright (C) 2026 David Walther */
/* SPDX-License-Identifier: ISC */
/* mkfs.filsys.c - create a Research Unix (PDP-7 through 32V) filesystem in a
 * disk image.
 *
 * Usage:
 *     mkfs.filsys [-v <v0|v1|v2|v3|v4|v5|v6|v7|32v>] [-o block] [-b boot] image [blocks]
 *
 * Builds a fresh filesystem: a superblock, a zeroed i-list, an interleaved
 * free-block list, and an empty root directory.  The image is opened (created
 * if missing) and grown to the filesystem size.  -o places the filesystem at
 * a block offset within the image (for multi-partition images); without it the
 * filesystem starts at block 0.
 *
 * -v selects the edition (the default is 7).  Several editions are one on-disk
 * format: 1, 2 and 3 are byte-identical (bitmap allocator, 10-byte dirents,
 * root inode 41); 4, 5 and 6 are byte-identical (the V6 format); 32 is 32V,
 * V7 recompiled for the VAX with little-endian 32-bit fields.  0 is the
 * word-addressed PDP-7.  The V6 and V7 layouts differ on disk (see README): V6
 * has 16 32-byte inodes per block, 16-bit block numbers, an s_isize that
 * counts i-list *blocks* (first data block = s_isize+2), and no bad-block file;
 * V7 has 8 64-byte inodes per block, 24-bit block numbers, an s_isize that is
 * the first data block, and reserves inode 1 as the bad-block file.
 *
 * Block layout (block 0 is the start of the filesystem):
 *     block 0    boot block (written by -b, else left alone)
 *     block 1    superblock
 *     block 2..  i-list, then data
 * -b takes a PDP-11 a.out (V7 magic 0407) and writes its text+data as block 0.
 *
 * Sizes default to the image's own size, or `blocks` when given.
 */
#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include <stdarg.h>

#include "filsys.h"
#include "v1fs.h"
#include "v6fs.h"
#include "v7fs.h"
#include "pdp7fs.h"

enum { A_MAGIC1 = 0407 };  /* V7 normal a.out magic (boot block) */

static int      fd;
static uint64_t base;   /* byte offset of the filesystem in the image */

static void pblock(uint32_t bno, const uint8_t *buf);
static void write_boot(const char *path);
static void die(const char *fmt, ...);

/* ---- V7 ----------------------------------------------------------------- */

static uint32_t v7_isize, v7_fsize;
static uint8_t  v7_sbuf[V7_BSIZE];    /* in-core superblock (block 1) */
static uint8_t  v7_freebuf[V7_BSIZE];
static uint16_t v7_nfree;
static uint32_t v7_tinode, v7_tfree;
static int      v7_le;                /* 0 = V7 middle-endian, 1 = 32V little-endian */

static void     v7_sb_put16(uint32_t off, uint16_t v);
static void     v7_sb_put32(uint32_t off, uint32_t v);
static uint32_t v7_alloc(void);
static void     v7_bfree(uint32_t bno);
static void     v7_bflist(int m, int n);
static void     v7_mkroot(void);
static void     v7_iput(uint32_t ino, uint16_t mode, int16_t nlink,
                        uint32_t size, const uint32_t *addr);

/* ---- V6 ----------------------------------------------------------------- */

static uint32_t v6_isize, v6_fsize;
static uint8_t  v6_sbuf[V6_BSIZE];    /* in-core superblock (block 1) */
static uint8_t  v6_freebuf[V6_BSIZE];
static uint16_t v6_nfree;

static void     v6_sb_put16(uint32_t off, uint16_t v);
static void     v6_sb_put32(uint32_t off, uint32_t v);
static uint32_t v6_alloc(void);
static void     v6_bfree(uint32_t bno);
static void     v6_bflist(int m, int n);
static void     v6_mkroot(void);
static void     v6_iput(uint32_t ino, uint16_t mode, int16_t nlink,
                        uint32_t size, const uint32_t *addr);

static void mkfs_v7(const char *path, uint32_t blocks, const char *bootfile);
static void mkfs_v6(const char *path, uint32_t blocks, const char *bootfile);

/* ---- shared helpers ----------------------------------------------------- */

static int parse_edition(const char *s)
{
    if (s[0] == 'v' || s[0] == 'V')
        s++;
    if (strcmp(s, "v0") == 0 || strcmp(s, "0") == 0 ||
        strcmp(s, "pdp7") == 0 || strcmp(s, "p7") == 0)
        return FILSYS_PDP7;
    if (strcmp(s, "1") == 0 || strcmp(s, "2") == 0 || strcmp(s, "3") == 0)
        return FILSYS_V1;   /* V1, V2, V3: one format (10-byte dirents, bitmap, root 41) */
    if (strcmp(s, "4") == 0 || strcmp(s, "5") == 0 || strcmp(s, "6") == 0)
        return FILSYS_V6;
    if (strcmp(s, "7") == 0)
        return FILSYS_V7;
    if (strcmp(s, "32") == 0 || strcmp(s, "32v") == 0)
        return FILSYS_32V;
    return -1;
}

/* Open (or create) the image, then work out the filesystem size in blocks:
 * an explicit `blocks` argument, else the image's own size, else a default. */
static uint32_t resolve_blocks(const char *path, uint64_t offblock, uint32_t blocks)
{
    base = offblock * V7_BSIZE;

    fd = open(path, O_RDWR | O_CREAT, 0666);
    if (fd < 0)
        die("%s: cannot open/create: %s\n", path, strerror(errno));

    if (blocks == 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && (uint64_t)st.st_size >= base + V7_BSIZE)
            blocks = (uint32_t)((st.st_size - base) / V7_BSIZE);
        if (blocks < 16)
            blocks = 4000;      /* small usable volume */
    }
    return blocks;
}

static void pblock(uint32_t bno, const uint8_t *buf)
{
    if (pwrite(fd, buf, V7_BSIZE, (off_t)(base + (uint64_t)bno * V7_BSIZE)) != V7_BSIZE)
        die("write error at block %u\n", bno);
}

/* Write a PDP-11 a.out (V7 magic 0407) boot program to block 0. */
static void write_boot(const char *path)
{
    uint8_t hdr[16];
    int f = open(path, O_RDONLY);
    if (f < 0)
        die("%s: cannot open boot: %s\n", path, strerror(errno));
    if (read(f, hdr, sizeof hdr) != (ssize_t)sizeof hdr)
        die("%s: short boot header\n", path);
    uint16_t magic = v7_get16le(hdr + 0);
    uint16_t text  = v7_get16le(hdr + 2);
    uint16_t data  = v7_get16le(hdr + 4);
    if (magic != A_MAGIC1)
        die("%s: bad boot magic 0%o (want 0%o)\n", path, magic, A_MAGIC1);
    uint32_t c = (uint32_t)text + data;
    if (c > V7_BSIZE)
        die("%s: boot too big (%u > %d bytes)\n", path, c, V7_BSIZE);
    uint8_t buf[V7_BSIZE] = {0};
    if (read(f, buf, c) != (ssize_t)c)
        die("%s: short boot\n", path);
    close(f);
    pblock(0, buf);
}

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}

/* ---- V7 implementation -------------------------------------------------- */

static void v7_sb_put16(uint32_t off, uint16_t v) { v7_put16le(v7_sbuf + off, v); }
static void v7_sb_put32(uint32_t off, uint32_t v) { v7_put32(v7_sbuf + off, v7_le, v); }

static uint32_t v7_alloc(void)
{
    uint32_t bno;
    int i;

    v7_tfree--;
    v7_nfree--;
    bno = v7_get32(v7_sbuf + sb_free_off(v7_le) + 4 * v7_nfree, v7_le);
    if (bno == 0)
        die("out of free space\n");
    if (v7_nfree == 0) {
        /* free-list exhausted: block bno holds the next batch */
        uint8_t fb[V7_BSIZE];
        if (pread(fd, fb, V7_BSIZE, (off_t)(base + (uint64_t)bno * V7_BSIZE)) != V7_BSIZE)
            die("read error at block %u\n", bno);
        v7_nfree = v7_get16le(fb);
        for (i = 0; i < V7_NICFREE; i++)
            v7_put32(v7_sbuf + sb_free_off(v7_le) + 4 * i, v7_le,
                     v7_get32(fb + fb_free_off(v7_le) + 4 * i, v7_le));
    }
    return bno;
}

static void v7_bfree(uint32_t bno)
{
    int i;

    v7_tfree++;
    if (v7_nfree >= V7_NICFREE) {
        memset(v7_freebuf, 0, V7_BSIZE);
        v7_put16le(v7_freebuf, (uint16_t)v7_nfree);
        for (i = 0; i < V7_NICFREE; i++)
            v7_put32(v7_freebuf + fb_free_off(v7_le) + 4 * i, v7_le,
                     v7_get32(v7_sbuf + sb_free_off(v7_le) + 4 * i, v7_le));
        pblock(bno, v7_freebuf);
        v7_nfree = 0;
    }
    v7_put32(v7_sbuf + sb_free_off(v7_le) + 4 * v7_nfree, v7_le, bno);
    v7_nfree++;
}

/* Interleave free blocks with stride m modulo n, as V7's mkfs does. */
static void v7_bflist(int m, int n)
{
    uint8_t  flg[500];
    uint32_t adr[500];
    uint32_t d, f;
    int i, j;

    if (n <= 0 || n > 500)
        n = 500;
    if (m <= 0 || m > n)
        m = 3;

    memset(flg, 0, sizeof flg);
    i = 0;
    for (j = 0; j < n; j++) {
        while (flg[i])
            i = (i + 1) % n;
        adr[j] = i + 1;
        flg[i]++;
        i = (i + m) % n;
    }

    /* inode 1: the (empty) bad-block file, as V7's mkfs writes */
    {
        uint32_t zaddr[V7_NIADDR] = {0};
        v7_iput(1, V7_IFREG, 0, 0, zaddr);
    }
    v7_bfree(0);
    d = v7_fsize - 1;
    while (d % n)
        d++;
    for (; d > 0; d -= n) {
        for (i = 0; i < n; i++) {
            f = d - adr[i];
            if (f < v7_fsize && f >= v7_isize)
                v7_bfree(f);
        }
    }
}

static void v7_mkroot(void)
{
    uint32_t addr[V7_NIADDR] = {0};
    uint32_t bno = v7_alloc();
    uint8_t db[V7_BSIZE];

    memset(db, 0, V7_BSIZE);
    v7_put16le(db, V7_ROOTINO);
    memcpy(db + 2, ".", 1);
    v7_put16le(db + V7_DIRENTSZ, V7_ROOTINO);
    memcpy(db + V7_DIRENTSZ + 2, "..", 2);
    pblock(bno, db);

    addr[0] = bno;
    v7_iput(V7_ROOTINO, V7_IFDIR | 0777, 2, 2 * V7_DIRENTSZ, addr);
}

static void v7_iput(uint32_t ino, uint16_t mode, int16_t nlink, uint32_t size,
                    const uint32_t *addr)
{
    uint32_t d = (ino + 15) >> 3;
    uint32_t o = (ino + 15) & 7;
    uint8_t ib[V7_BSIZE];
    int i;

    if (pread(fd, ib, V7_BSIZE, (off_t)(base + (uint64_t)d * V7_BSIZE)) != V7_BSIZE)
        die("read error at inode block %u\n", d);

    uint8_t *ip = ib + o * V7_INODESZ;
    memset(ip, 0, V7_INODESZ);
    v7_put16le(ip + 0, mode);
    v7_put16le(ip + 2, (uint16_t)nlink);
    v7_put32(ip + 8, v7_le, size);
    for (i = 0; i < V7_NIADDR; i++)
        v7_put24(ip + 12 + 3 * i, v7_le, addr[i]);
    v7_put32(ip + 52, v7_le, (uint32_t)time(NULL));
    v7_put32(ip + 56, v7_le, (uint32_t)time(NULL));
    v7_put32(ip + 60, v7_le, (uint32_t)time(NULL));

    pblock(d, ib);
    v7_tinode--;
}

static void mkfs_v7(const char *path, uint32_t blocks, const char *bootfile)
{
    /* i-list: ~1 inode per 25 blocks, minimum 1 block, +2 (boot/super) */
    uint32_t isz = blocks / 25;
    if (isz == 0)
        isz = 1;
    if (isz > 65500 / V7_INOPB)
        isz = 65500 / V7_INOPB;
    isz += 2;
    if (isz >= blocks)
        die("%s: %u blocks too small for an i-list of %u blocks\n",
            path, blocks, isz);

    v7_isize = isz;
    v7_fsize = blocks;

    if (bootfile)
        write_boot(bootfile);

    memset(v7_sbuf, 0, V7_BSIZE);
    v7_sb_put16(0, (uint16_t)v7_isize);
    v7_sb_put32(sb_fsize_off(v7_le), v7_fsize);
    if (!v7_le) {
        v7_sb_put16(424, 3);      /* s_m (V7 interleave hint) */
        v7_sb_put16(426, 500);    /* s_n */
    }

    /* s_isize is the first data block (not the i-list size), so the i-list is
     * blocks 2..s_isize-1 and the inode count is (s_isize-2) * 8.  V7's own
     * mkfs prints (n)*NIPB for n = s_isize-2; print the same, not isize*8. */
    printf("m/n = 3 500, isize = %u, fsize = %u\n",
           (v7_isize - 2) * V7_INOPB, v7_fsize);

    uint8_t zb[V7_BSIZE] = {0};
    for (uint32_t b = 2; b < v7_isize; b++) {
        pblock(b, zb);
        v7_tinode += V7_INOPB;
    }

    v7_bflist(3, 500);
    v7_mkroot();

    v7_sb_put16(sb_nfree_off(v7_le), (uint16_t)v7_nfree);
    v7_sb_put32(sb_time_off(v7_le), (uint32_t)time(NULL));   /* s_time */
    if (!v7_le) {
        v7_sb_put32(418, v7_tfree);               /* s_tfree (V7 only) */
        v7_sb_put16(422, (uint16_t)v7_tinode);    /* s_tinode (V7 only) */
    }
    pblock(1, v7_sbuf);

    if (ftruncate(fd, (off_t)(base + (uint64_t)v7_fsize * V7_BSIZE)) < 0)
        die("%s: ftruncate: %s\n", path, strerror(errno));

    printf("%s: %u blocks, %u inodes written\n", path, v7_fsize, v7_tinode);
}

/* ---- V6 implementation -------------------------------------------------- */

static void v6_sb_put16(uint32_t off, uint16_t v) { v6_put16le(v6_sbuf + off, v); }
static void v6_sb_put32(uint32_t off, uint32_t v) { v6_put32me(v6_sbuf + off, v); }

static uint32_t v6_alloc(void)
{
    uint32_t bno;
    int i;

    v6_nfree--;
    bno = v6_get16le(v6_sbuf + 6 + 2 * v6_nfree);
    if (bno == 0)
        die("out of free space\n");
    if (v6_nfree == 0) {
        uint8_t fb[V6_BSIZE];
        if (pread(fd, fb, V6_BSIZE, (off_t)(base + (uint64_t)bno * V6_BSIZE)) != V6_BSIZE)
            die("read error at block %u\n", bno);
        v6_nfree = v6_get16le(fb);
        for (i = 0; i < V6_NICFREE; i++)
            v6_put16le(v6_sbuf + 6 + 2 * i, v6_get16le(fb + 2 + 2 * i));
    }
    return bno;
}

static void v6_bfree(uint32_t bno)
{
    int i;

    if (v6_nfree >= V6_NICFREE) {
        memset(v6_freebuf, 0, V6_BSIZE);
        v6_put16le(v6_freebuf, (uint16_t)v6_nfree);
        for (i = 0; i < V6_NICFREE; i++)
            v6_put16le(v6_freebuf + 2 + 2 * i, v6_get16le(v6_sbuf + 6 + 2 * i));
        pblock(bno, v6_freebuf);
        v6_nfree = 0;
    }
    v6_put16le(v6_sbuf + 6 + 2 * v6_nfree, (uint16_t)bno);
    v6_nfree++;
}

/* Interleave free blocks with stride m modulo n, as V6's mkfs does. */
static void v6_bflist(int m, int n)
{
    uint8_t  flg[100];
    uint32_t adr[100];
    uint32_t d, f;
    int i, j;

    if (n <= 0 || n > 100)
        n = 100;
    if (m <= 0 || m > n)
        m = 3;

    memset(flg, 0, sizeof flg);
    i = 0;
    for (j = 0; j < n; j++) {
        while (flg[i])
            i = (i + 1) % n;
        adr[j] = i + 1;
        flg[i]++;
        i = (i + m) % n;
    }

    v6_bfree(0);   /* sentinel */
    d = v6_fsize - 1;
    while (d % n)
        d++;
    for (; d > 0; d -= n) {
        for (i = 0; i < n; i++) {
            f = d - adr[i];
            if (f < v6_fsize && f >= v6_isize + 2)   /* data starts at isize+2 */
                v6_bfree(f);
        }
    }
}

static void v6_mkroot(void)
{
    uint32_t addr[V6_NIADDR] = {0};
    uint32_t bno = v6_alloc();
    uint8_t db[V6_BSIZE];

    memset(db, 0, V6_BSIZE);
    /* "." and ".." both point at the root itself (inode 1); 16-byte entries */
    v6_put16le(db, V6_ROOTINO);
    memcpy(db + 2, ".", 1);
    v6_put16le(db + 16, V6_ROOTINO);
    memcpy(db + 18, "..", 2);
    pblock(bno, db);

    addr[0] = bno;
    v6_iput(V6_ROOTINO, V6_IALLOC | V6_IFDIR | 0777, 2, 2 * 16, addr);
}

static void v6_iput(uint32_t ino, uint16_t mode, int16_t nlink, uint32_t size,
                    const uint32_t *addr)
{
    uint32_t d = v6_itod(ino);
    uint32_t o = v6_itoo(ino);
    uint8_t ib[V6_BSIZE];
    int i;

    if (pread(fd, ib, V6_BSIZE, (off_t)(base + (uint64_t)d * V6_BSIZE)) != V6_BSIZE)
        die("read error at inode block %u\n", d);

    uint8_t *ip = ib + o * V6_INODESZ;
    memset(ip, 0, V6_INODESZ);
    v6_put16le(ip + 0, mode);
    ip[2] = (uint8_t)nlink;                 /* i_nlink */
    /* i_uid, i_gid = 0 */
    ip[5] = (uint8_t)((size >> 16) & 0xff); /* i_size0: high byte */
    v6_put16le(ip + 6, (uint16_t)(size & 0xffff));   /* i_size1: low word */
    for (i = 0; i < V6_NIADDR; i++)
        v6_put16le(ip + 8 + 2 * i, (uint16_t)addr[i]);
    v6_put32me(ip + 24, (uint32_t)time(NULL));   /* i_atime */
    v6_put32me(ip + 28, (uint32_t)time(NULL));   /* i_mtime */

    pblock(d, ib);
}

static void mkfs_v6(const char *path, uint32_t blocks, const char *bootfile)
{
    /* i-list block count: fsize / (43 + fsize/1000), as V6's mkfs sized it */
    uint32_t isz = blocks / (43 + blocks / 1000);
    if (isz == 0)
        isz = 1;
    if (isz + 2 >= blocks)
        die("%s: %u blocks too small for an i-list of %u blocks\n",
            path, blocks, isz);

    v6_isize = isz;         /* number of i-list blocks */
    v6_fsize = blocks;

    if (bootfile)
        write_boot(bootfile);

    memset(v6_sbuf, 0, V6_BSIZE);
    v6_sb_put16(0, (uint16_t)v6_isize);
    v6_sb_put16(2, (uint16_t)v6_fsize);

    printf("isize = %u, fsize = %u\n", v6_isize * V6_INOPB, v6_fsize);

    /* zero the i-list: blocks 2..isize+1 */
    uint8_t zb[V6_BSIZE] = {0};
    for (uint32_t b = 2; b < v6_isize + 2; b++)
        pblock(b, zb);

    v6_bflist(3, 100);
    v6_mkroot();

    v6_sb_put16(4, (uint16_t)v6_nfree);
    v6_sb_put32(412, (uint32_t)time(NULL));   /* s_time[2] */
    pblock(1, v6_sbuf);

    if (ftruncate(fd, (off_t)(base + (uint64_t)v6_fsize * V6_BSIZE)) < 0)
        die("%s: ftruncate: %s\n", path, strerror(errno));

    printf("%s: %u blocks, %u inodes written\n", path, v6_fsize,
           v6_isize * V6_INOPB);
}

/* ---- V1 ------------------------------------------------------------------ */

static uint32_t v1_fsize, v1_maxino, v1_dstart;
static uint8_t  v1_sb[V1_BSIZE * 2];   /* superblock spans blocks 0 and 1 */
static uint32_t v1_imap_off;           /* byte offset of the inode map */

static uint32_t v1_balloc(void)
{
    for (uint32_t b = v1_dstart; b < v1_fsize; b++) {
        if (v1_sb[2 + (b >> 3)] & (1u << (b & 7))) {   /* bit=1 free */
            v1_sb[2 + (b >> 3)] &= (uint8_t)~(1u << (b & 7));
            return b;
        }
    }
    die("out of free space\n");
    return 0;
}

static void v1_iput(uint32_t ino, uint16_t mode, int16_t nlink, uint32_t size,
                    const uint32_t *addr)
{
    uint32_t bno = v1_itod(ino);
    uint32_t off = v1_itoo(ino);
    uint8_t ib[V1_BSIZE];
    if (pread(fd, ib, V1_BSIZE, (off_t)(base + (uint64_t)bno * V1_BSIZE)) != V1_BSIZE)
        die("read error at inode block %u\n", bno);

    uint8_t *ip = ib + off * V1_INODESZ;
    memset(ip, 0, V1_INODESZ);
    v1_put16le(ip + 0, mode);
    ip[2] = (uint8_t)nlink;
    /* i_uid = 0 */
    v1_put16le(ip + 4, (uint16_t)size);
    for (int i = 0; i < V1_NIADDR; i++)
        v1_put16le(ip + 6 + 2 * i, (uint16_t)addr[i]);
    v1_put32me(ip + 22, (uint32_t)time(NULL) * 60u);   /* ctime (60ths) */
    v1_put32me(ip + 26, (uint32_t)time(NULL) * 60u);   /* mtime (60ths) */

    pblock(bno, ib);
}

/* Largest volume a V1 superblock can describe: the free-block and inode
 * bitmaps must both fit in the two-block (1024-byte) superblock. */
static uint32_t v1_max_fsize(void)
{
    for (uint32_t n = 0; ; n++) {
        uint32_t freemap_bytes = (n + 7) / 8;
        if (freemap_bytes & 1)
            freemap_bytes++;
        uint32_t maxino = n / 4;
        if (maxino < 48)
            maxino = 48;
        maxino = (maxino + 15) & ~15u;
        if (2 + freemap_bytes + 2 + maxino / 8 > V1_BSIZE * 2)
            return n - 1;
    }
}

static void mkfs_v1(const char *path, uint32_t blocks, const char *bootfile)
{
    (void)bootfile;   /* no boot-block install for V1 */

    /* V1's free map and inode-map size fields are 16-bit words, so the maps
     * (and the filesystem they describe) are a whole number of words: round the
     * volume up to a multiple of 16 blocks, so fsize = freemap_bytes * 8. */
    v1_fsize = (blocks + 15) & ~15u;
    if (v1_fsize > v1_max_fsize())
        die("%s: v1: maximum filesystem size is %u blocks (superblock free map)\n",
            path, v1_max_fsize());

    uint32_t freemap_bytes = v1_fsize / 8;

    /* ~1 inode per 4 blocks, minimum 48 (so root inode 41 exists), a multiple
     * of 16 so the inode-map byte count is even. */
    v1_maxino = v1_fsize / 4;
    if (v1_maxino < 48)
        v1_maxino = 48;
    v1_maxino = (v1_maxino + 15) & ~15u;
    uint32_t inodemap_bytes = v1_maxino / 8;

    v1_dstart = (v1_maxino + 31) / 16 + 1;
    if (v1_dstart >= v1_fsize)
        die("%s: %u blocks too small\n", path, blocks);

    v1_imap_off = 2 + freemap_bytes + 2;
    if (v1_imap_off + inodemap_bytes > sizeof(v1_sb))
        die("%s: superblock overflow\n", path);

    memset(v1_sb, 0, sizeof(v1_sb));
    v1_put16le(v1_sb + 0, (uint16_t)freemap_bytes);
    v1_put16le(v1_sb + 2 + freemap_bytes, (uint16_t)inodemap_bytes);

    /* free map: data blocks are free (bit=1) */
    for (uint32_t b = v1_dstart; b < v1_fsize; b++)
        v1_sb[2 + (b >> 3)] |= (uint8_t)(1u << (b & 7));

    /* inode map: all free (0); root inode 41 is used (bit 0 = 1) */
    v1_sb[v1_imap_off] |= 1u;

    pblock(0, v1_sb);
    pblock(1, v1_sb + V1_BSIZE);

    /* zero the i-list: blocks 2 .. v1_dstart-1 */
    uint8_t zb[V1_BSIZE] = {0};
    for (uint32_t b = 2; b < v1_dstart; b++)
        pblock(b, zb);

    /* root directory: inode 41, "." and ".." (10-byte entries) */
    uint32_t rb = v1_balloc();
    uint8_t db[V1_BSIZE] = {0};
    v1_put16le(db + 0, V1_ROOTINO);
    memcpy(db + 2, ".", 1);
    v1_put16le(db + V1_DIRENTSZ, V1_ROOTINO);
    memcpy(db + V1_DIRENTSZ + 2, "..", 2);
    pblock(rb, db);

    uint32_t addr[V1_NIADDR] = {0};
    addr[0] = rb;
    uint16_t mode = V1_IALLOC | V1_IFDIR | V1_IREAD | V1_IWRITE | V1_IEXEC | V1_OREAD;
    v1_iput(V1_ROOTINO, mode, 2, 2 * V1_DIRENTSZ, addr);

    /* persist the superblock (the free map changed when the root block was
     * allocated) */
    pblock(0, v1_sb);
    pblock(1, v1_sb + V1_BSIZE);

    if (ftruncate(fd, (off_t)(base + (uint64_t)v1_fsize * V1_BSIZE)) < 0)
        die("%s: ftruncate: %s\n", path, strerror(errno));

    printf("%s: %u blocks, %u inodes written\n", path, v1_fsize, v1_maxino);
}

/* ---- PDP-7 --------------------------------------------------------------- */

static uint32_t p7_pool[P7_NBLOCKS];   /* free data-block pool */
static uint32_t p7_nfree;
static uint32_t p7_free_count;         /* free blocks listed in the free list */

static void p7_pblock(uint32_t bno, const uint32_t *words)
{
    uint8_t raw[P7_BLOCKBYTES];
    for (int i = 0; i < P7_WSIZE; i++)
        p7_putword(raw + i * P7_WORDBYTES, words[i]);
    off_t pos = (off_t)P7_SURFACE1 + (off_t)bno * P7_BLOCKBYTES;
    if (pwrite(fd, raw, P7_BLOCKBYTES, pos) != P7_BLOCKBYTES)
        die("write error at block %u\n", bno);
}

static uint32_t p7_take_free(void)
{
    if (p7_nfree == 0)
        die("out of free space\n");
    return p7_pool[--p7_nfree];
}

/* Build the free-list chain: each free-list block holds nine free block
 * numbers (words 1..9) and a next pointer (word 0).  The free-list blocks
 * themselves are drawn from the pool, so they are not returned as free. */
static uint32_t p7_build_freelist(void)
{
    uint32_t head = 0;
    p7_free_count = 0;
    while (p7_nfree > 0) {
        uint32_t fb = p7_take_free();
        p7_free_count++;           /* the node block itself is free */
        uint32_t words[P7_WSIZE] = {0};
        words[0] = head;
        for (int i = 1; i <= 9 && p7_nfree > 0; i++) {
            words[i] = p7_take_free();
            p7_free_count++;
        }
        p7_pblock(fb, words);
        head = fb;
    }
    return head;
}

static void mkfs_pdp7(const char *path, uint32_t blocks, const char *bootfile)
{
    /* The RB09 fixed-head disk has one valid geometry (8000 blocks/surface);
     * there is no size to choose. */
    if (blocks != 0)
        die("%s: v0: size is fixed by the RB09 geometry (8000 blocks/surface)\n",
            path);
    (void)bootfile;

    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        die("%s: cannot create: %s\n", path, strerror(errno));
    if (ftruncate(fd, (off_t)(P7_SURFACE1 * 2)) < 0)
        die("%s: ftruncate: %s\n", path, strerror(errno));

    /* free data blocks: 712 .. 6399 (the kernel area 6400..7999 is reserved) */
    p7_nfree = 0;
    for (uint32_t b = 712; b <= 6399; b++)
        p7_pool[p7_nfree++] = b;

    /* root directory data block, drawn before the free list is built */
    uint32_t rb = p7_take_free();
    uint32_t empty[P7_WSIZE] = {0};
    p7_pblock(rb, empty);

    uint32_t head = p7_build_freelist();

    /* superblock: block 0 word 0 = free-list head */
    uint32_t sb[P7_WSIZE] = {0};
    sb[0] = head;
    p7_pblock(0, sb);

    /* zero the i-list: blocks 2 .. 711 */
    uint32_t z[P7_WSIZE] = {0};
    for (uint32_t b = P7_FIRSTINOBLK; b < P7_FIRSTINOBLK + P7_NINOBLKS; b++)
        p7_pblock(b, z);

    /* root "dd" directory: inode 4 (I_DIRECTORY, owner rw / world r) */
    uint32_t ino_block = p7_itod(P7_ROOTINO);
    uint32_t ino_off   = p7_itoo(P7_ROOTINO);
    uint32_t ib[P7_WSIZE] = {0};
    ib[ino_off + 0]  = P7_IUSED | P7_IDIR | P7_IOREAD | P7_IOWRITE | P7_IWREAD;
    ib[ino_off + 1]  = rb;          /* first (only) disk pointer */
    ib[ino_off + 8]  = 0;           /* uid */
    ib[ino_off + 9]  = P7_MAXWORD;  /* nlink = -1 */
    ib[ino_off + 10] = 0;           /* size (empty) */
    ib[ino_off + 11] = 1;           /* uniq */
    p7_pblock(ino_block, ib);

    printf("%s: PDP-7 filesystem written (%u free blocks, root inode %u)\n",
           path, p7_free_count, P7_ROOTINO);
}

/* ---- main --------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *path;
    const char *bootfile = NULL;
    uint32_t blocks = 0;
    uint64_t offblock = 0;
    int edition = FILSYS_V7;
    int c;

    while ((c = getopt(argc, argv, "v:o:b:")) != -1) {
        switch (c) {
        case 'v':
            edition = parse_edition(optarg);
            if (edition < 0) {
                fprintf(stderr, "mkfs.filsys: bad edition '%s' (want v0|v1|v2|v3|v4|v5|v6|v7|32v)\n", optarg);
                return 1;
            }
            break;
        case 'o': offblock = strtoull(optarg, NULL, 0); break;
        case 'b': bootfile = optarg; break;
        default:
            fprintf(stderr, "usage: mkfs.filsys [-v <v0|v1|v2|v3|v4|v5|v6|v7|32v>] [-o block] [-b boot] image [blocks]\n");
            return 1;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "usage: mkfs.filsys [-v <v0|v1|v2|v3|v4|v5|v6|v7|32v>] [-o block] [-b boot] image [blocks]\n");
        return 1;
    }
    path = argv[optind];
    if (optind + 1 < argc)
        blocks = (uint32_t)strtoul(argv[optind + 1], NULL, 0);

    if (edition == FILSYS_PDP7) {
        /* PDP-7 opens its own 2-surface image (fixed RB09 geometry). */
        mkfs_pdp7(path, blocks, bootfile);
        close(fd);
        return 0;
    }

    blocks = resolve_blocks(path, offblock, blocks);

    if (edition == FILSYS_V6)
        mkfs_v6(path, blocks, bootfile);
    else if (edition == FILSYS_V1)
        mkfs_v1(path, blocks, bootfile);
    else {
        v7_le = (edition == FILSYS_32V);
        mkfs_v7(path, blocks, bootfile);
    }

    close(fd);
    return 0;
}
