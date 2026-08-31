/* filsys 1.0.0 - 2026-08-26 - Copyright (C) 2026 David Walther */
/* SPDX-License-Identifier: ISC */
/* mkfs.filsys.c - create a V7 (PDP-11) filesystem in a disk image.
 *
 * Usage:
 *     mkfs.filsys [-o block] [-b boot] image [blocks]
 *
 * Builds a fresh V7 filesystem: a superblock, a zeroed i-list, an interleaved
 * free-block list, and an empty root directory.  The image is opened (created
 * if missing) and grown to the filesystem size.  -o places the filesystem at
 * a block offset within the image (for multi-partition images); without it the
 * filesystem starts at block 0.
 *
 * Block layout (block 0 is the start of the filesystem):
 *     block 0    boot block (written by -b, else left alone)
 *     block 1    superblock
 *     block 2..  i-list, then data
 * -b takes a PDP-11 a.out (V7 magic 0407) and writes its text+data as block 0.
 *
 * Sizes default to the image's own size, or `blocks` when given.
 */
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

#include "v7fs.h"

#define MAXFN 500      /* default interleave factor */
#define A_MAGIC1 0407  /* V7 normal a.out magic (boot block) */

static int    fd;
static uint64_t base;          /* byte offset of the filesystem in the image */
static uint32_t isize, fsize;
static uint8_t  sbuf[V7_BSIZE];  /* in-core superblock (block 1) */
static uint8_t  freebuf[V7_BSIZE];
static uint16_t nfree;
static uint32_t tinode, tfree;

static void pblock(uint32_t bno, const uint8_t *buf);
static void sb_put16(uint32_t off, uint16_t v);
static void sb_put32(uint32_t off, uint32_t v);
static uint32_t alloc(void);
static void bfree(uint32_t bno);
static void bflist(int m, int n);
static void mkroot(void);
static void iput(uint32_t ino, uint16_t mode, int16_t nlink, uint32_t size,
                 const uint32_t *addr);
static void write_boot(const char *path);
static void die(const char *fmt, ...);

int main(int argc, char **argv)
{
    const char *path;
    const char *bootfile = NULL;
    uint32_t blocks = 0;
    uint64_t offblock = 0;
    int c;

    while ((c = getopt(argc, argv, "o:b:")) != -1) {
        switch (c) {
        case 'o': offblock = strtoull(optarg, NULL, 0); break;
        case 'b': bootfile = optarg; break;
        default:
            fprintf(stderr, "usage: mkfs.filsys [-o block] [-b boot] image [blocks]\n");
            return 1;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "usage: mkfs.filsys [-o block] [-b boot] image [blocks]\n");
        return 1;
    }
    path = argv[optind];
    if (optind + 1 < argc)
        blocks = (uint32_t)strtoul(argv[optind + 1], NULL, 0);

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

    isize = isz;
    fsize = blocks;

    /* boot block (block 0), before the superblock at block 1 */
    if (bootfile)
        write_boot(bootfile);

    /* zero the superblock, set the static fields */
    memset(sbuf, 0, V7_BSIZE);
    sb_put16(0, (uint16_t)isize);
    sb_put32(2, fsize);
    /* s_nfree / s_free filled by bflist() */
    sb_put16(424, 3);          /* s_m  */
    sb_put16(426, MAXFN);      /* s_n  */

    printf("m/n = 3 %d, isize = %u, fsize = %u\n", MAXFN, isize * V7_INOPB, fsize);

    /* zero the whole i-list */
    uint8_t zb[V7_BSIZE] = {0};
    for (uint32_t b = 2; b < isize; b++) {
        pblock(b, zb);
        tinode += V7_INOPB;
    }

    bflist(3, MAXFN);
    mkroot();

    /* write the superblock last */
    sb_put16(6, (uint16_t)nfree);
    sb_put32(414, (uint32_t)time(NULL));   /* s_time */
    sb_put32(418, tfree);                  /* s_tfree */
    sb_put16(422, (uint16_t)tinode);       /* s_tinode */
    pblock(1, sbuf);

    if (ftruncate(fd, (off_t)(base + (uint64_t)fsize * V7_BSIZE)) < 0)
        die("%s: ftruncate: %s\n", path, strerror(errno));

    close(fd);
    printf("%s: %u blocks, %u inodes written\n", path, fsize, tinode);
    return 0;
}

static void pblock(uint32_t bno, const uint8_t *buf)
{
    if (pwrite(fd, buf, V7_BSIZE, (off_t)(base + (uint64_t)bno * V7_BSIZE)) != V7_BSIZE)
        die("write error at block %u\n", bno);
}

static void sb_put16(uint32_t off, uint16_t v) { v7_put16le(sbuf + off, v); }
static void sb_put32(uint32_t off, uint32_t v) { v7_put32me(sbuf + off, v); }

static uint32_t alloc(void)
{
    uint32_t bno;
    int i;

    tfree--;
    nfree--;
    bno = v7_get32me(sbuf + 8 + 4 * nfree);
    if (bno == 0)
        die("out of free space\n");
    if (nfree == 0) {
        /* free-list is exhausted: block bno holds the next batch */
        uint8_t fb[V7_BSIZE];
        if (pread(fd, fb, V7_BSIZE, (off_t)(base + (uint64_t)bno * V7_BSIZE)) != V7_BSIZE)
            die("read error at block %u\n", bno);
        nfree = v7_get16le(fb);
        for (i = 0; i < V7_NICFREE; i++)
            v7_put32me(sbuf + 8 + 4 * i, v7_get32me(fb + 2 + 4 * i));
    }
    return bno;
}

static void bfree(uint32_t bno)
{
    int i;

    tfree++;
    if (nfree >= V7_NICFREE) {
        memset(freebuf, 0, V7_BSIZE);
        v7_put16le(freebuf, (uint16_t)nfree);
        for (i = 0; i < V7_NICFREE; i++)
            v7_put32me(freebuf + 2 + 4 * i, v7_get32me(sbuf + 8 + 4 * i));
        pblock(bno, freebuf);
        nfree = 0;
    }
    v7_put32me(sbuf + 8 + 4 * nfree, bno);
    nfree++;
}

/* interleave free blocks with stride m modulo n, as V7's mkfs does */
static void bflist(int m, int n)
{
    uint8_t flg[MAXFN];
    uint32_t adr[MAXFN];
    uint32_t d, f;
    int i, j;

    if (n <= 0 || n > MAXFN)
        n = MAXFN;
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
        iput(1, V7_IFREG, 0, 0, zaddr);
    }
    bfree(0);
    d = fsize - 1;
    while (d % n)
        d++;
    for (; d > 0; d -= n) {
        for (i = 0; i < n; i++) {
            f = d - adr[i];
            if (f < fsize && f >= isize)
                bfree(f);
        }
    }
}

static void mkroot(void)
{
    uint32_t addr[V7_NIADDR] = {0};
    uint32_t bno = alloc();
    uint8_t db[V7_BSIZE];

    memset(db, 0, V7_BSIZE);
    /* "." and ".." both point at the root itself (inode 2) */
    v7_put16le(db, V7_ROOTINO);
    memcpy(db + 2, ".", 1);
    v7_put16le(db + V7_DIRSIZ, V7_ROOTINO);
    memcpy(db + V7_DIRSIZ + 2, "..", 2);
    pblock(bno, db);

    addr[0] = bno;
    iput(V7_ROOTINO, V7_IFDIR | 0777, 2, 2 * V7_DIRSIZ, addr);
}

static void iput(uint32_t ino, uint16_t mode, int16_t nlink, uint32_t size,
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
    /* di_uid, di_gid = 0 */
    v7_put32me(ip + 8, size);
    for (i = 0; i < V7_NIADDR; i++)
        v7_put24me(ip + 12 + 3 * i, addr[i]);
    v7_put32me(ip + 52, (uint32_t)time(NULL));  /* atime */
    v7_put32me(ip + 56, (uint32_t)time(NULL));  /* mtime */
    v7_put32me(ip + 60, (uint32_t)time(NULL));  /* ctime */

    pblock(d, ib);
    tinode--;
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
