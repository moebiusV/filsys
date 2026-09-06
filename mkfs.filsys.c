/* filsys 1.5.1 - 2026-09-06 - Copyright (C) 2026 David Walther */
/* SPDX-License-Identifier: ISC */
/* mkfs.filsys.c - create a Research Unix (PDP-7 through 32V) filesystem in a
 * disk image.
 *
 * Usage:
 *     mkfs.filsys [-v <pdp7|v1|v6|v7|vax32|coherent|xenix|bsd29|bsd211>] [-o block] [-b boot]
 *                 [-m m] [-n n] image [blocks]
 *
 * Builds a fresh filesystem: a superblock, a zeroed i-list, a free-block list,
 * and an empty root directory.  The image is opened (created if missing) and
 * grown to the filesystem size.  -o places the filesystem at a block offset
 * within the image (for multi-partition images); without it the filesystem
 * starts at block 0.
 *
 * -v selects the edition (the default is 7).  Several editions are one on-disk
 * format: 1, 2 and 3 are byte-identical (bitmap allocator, 10-byte dirents,
 * root inode 41); 4, 5 and 6 are byte-identical (the V6 format); vax32 (32v,
 * 32) is 32V, V7 recompiled for the VAX with little-endian 32-bit fields.  0 is the
 * word-addressed PDP-7.  coherent is the Mark Williams Coherent format:
 * middle-endian V7 with a 64-entry free cache and s_m/s_n/s_unique; -m/-n set
 * the interleave factors (s_m/s_n, default 1/1).
 * The V6 and V7 layouts differ on disk (see README): V6 has 16 32-byte inodes
 * per block, 16-bit block numbers, an s_isize that counts i-list *blocks*
 * (first data block = s_isize+2), and no bad-block file; V7 has 8 64-byte
 * inodes per block, 24-bit block numbers, an s_isize that is the first data
 * block, and reserves inode 1 as the bad-block file.
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
#include "v7fs.h"
#include "pdp7fs.h"

enum { A_MAGIC1 = 0407 };  /* V7 normal a.out magic (boot block) */

static int      fd;
static uint64_t base;   /* byte offset of the filesystem in the image */
static uint32_t bsize = V7_BSIZE;   /* logical block size (512 or 1024) */

static void pblock(uint32_t bno, const uint8_t *buf);
static void write_boot(const char *path);
static void die(const char *fmt, ...);

/* ---- V7 ----------------------------------------------------------------- */

static uint32_t v7_isize, v7_fsize;
static uint8_t  v7_sbuf[V7_MAXBSIZE]; /* in-core superblock (block 1) */
static uint8_t  v7_freebuf[V7_MAXBSIZE];
static uint16_t v7_nfree;
static uint32_t v7_tinode, v7_tfree;
static filsys_edition_t v7_fmt; /* format descriptor (bo, bsize, pack4, ...) */
static uint32_t v7_ipb = V7_INOPB;  /* inodes per block (bsize / inode_size) */
static uint16_t v7_m = 1, v7_n = 1;   /* interleave factors (s_m/s_n, coherent) */

static uint32_t v7_alloc(void);
static void     v7_bfree(uint32_t bno);
static void     v7_bflist(void);
static void     v7_mkroot(void);
static void     v7_iput(uint32_t ino, uint16_t mode, int16_t nlink,
                        uint32_t size, const uint32_t *addr);

/* ---- V6 ----------------------------------------------------------------- */

static uint32_t v6_isize, v6_fsize;
static uint8_t  v6_sbuf[V6_BSIZE];    /* in-core superblock (block 1) */
static uint8_t  v6_freebuf[V6_BSIZE];
static uint16_t v6_nfree;

static uint32_t v6_alloc(void);
static void     v6_bfree(uint32_t bno);
static void     v6_bflist(uint32_t m, uint32_t n);
static void     v6_mkroot(void);
static void     v6_iput(uint32_t ino, uint16_t mode, int16_t nlink,
                        uint32_t size, const uint32_t *addr);

static void mkfs_v7(const char *path, uint32_t blocks, const char *bootfile);
static void mkfs_v6(const char *path, uint32_t blocks, const char *bootfile);

/* Open (or create) the image, then work out the filesystem size in blocks:
 * an explicit `blocks` argument, else the image's own size, else a default. */
static uint32_t resolve_blocks(const char *path, uint64_t offblock, uint32_t blocks)
{
    base = offblock * bsize;

    fd = open(path, O_RDWR | O_CREAT, 0666);
    if (fd < 0)
        die("%s: cannot open/create: %s\n", path, strerror(errno));

    if (blocks == 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && (uint64_t)st.st_size >= base + bsize)
            blocks = (uint32_t)(((uint64_t)st.st_size - base) / bsize);
        if (blocks < 16)
            blocks = 4000;      /* small usable volume */
    }
    return blocks;
}

static void pblock(uint32_t bno, const uint8_t *buf)
{
    if (pwrite(fd, buf, bsize, (off_t)(base + (uint64_t)bno * bsize)) != (ssize_t)bsize)
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
    uint16_t magic = bo_get16le(hdr + 0);
    uint16_t text  = bo_get16le(hdr + 2);
    uint16_t data  = bo_get16le(hdr + 4);
    if (magic != A_MAGIC1)
        die("%s: bad boot magic 0%o (want 0%o)\n", path, magic, A_MAGIC1);
    uint32_t c = (uint32_t)text + data;
    if (c > V7_BSIZE)
        die("%s: boot too big (%u > %d bytes)\n", path, c, V7_BSIZE);
    uint8_t buf[V7_MAXBSIZE] = {0};
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


static uint32_t v7_alloc(void)
{
    uint32_t bno;
    int i;

    v7_tfree--;
    v7_nfree--;
    bno = v7_fmt.bo->get32(v7_sbuf + sb_free_off(v7_fmt.pack4) + 4 * v7_nfree);
    if (bno == 0)
        die("out of free space\n");
    if (v7_nfree == 0) {
        /* free-list exhausted: block bno holds the next batch */
        uint8_t fb[V7_MAXBSIZE];
        if (pread(fd, fb, bsize, (off_t)(base + (uint64_t)bno * bsize)) != (ssize_t)bsize)
            die("read error at block %u\n", bno);
        v7_nfree = v7_fmt.bo->get16(fb);
        for (i = 0; i < v7_fmt.nicfree; i++)
            v7_fmt.bo->put32(v7_sbuf + sb_free_off(v7_fmt.pack4) + 4 * i, 
                     v7_fmt.bo->get32(fb + fb_free_off(v7_fmt.pack4) + 4 * i));
    }
    return bno;
}

static void v7_bfree(uint32_t bno)
{
    int i;

    if (v7_nfree == 0) {
        /* Seed the 0 sentinel at the bottom of the stack, mirroring v7fs_bfree;
         * it terminates the free-list chain and is not itself a free block. */
        v7_fmt.bo->put32(v7_sbuf + sb_free_off(v7_fmt.pack4), 0);
        v7_nfree = 1;
    }
    if (v7_nfree >= v7_fmt.nicfree) {
        memset(v7_freebuf, 0, bsize);
        v7_fmt.bo->put16(v7_freebuf, (uint16_t)v7_nfree);
        for (i = 0; i < v7_fmt.nicfree; i++)
            v7_fmt.bo->put32(v7_freebuf + fb_free_off(v7_fmt.pack4) + 4 * i, 
                     v7_fmt.bo->get32(v7_sbuf + sb_free_off(v7_fmt.pack4) + 4 * i));
        pblock(bno, v7_freebuf);
        v7_nfree = 0;
    }
    v7_fmt.bo->put32(v7_sbuf + sb_free_off(v7_fmt.pack4) + 4 * v7_nfree, bno);
    v7_nfree++;
    v7_tfree++;
}

/* Build the free list.  Coherent interleaves it so that sequential allocation
 * walks blocks around the cylinder: a logical block bn maps to the physical
 * block (bn/n)*n + maptab[bn%n], where maptab[i] = (i/ratio) + (i%ratio)*m and
 * ratio = n/m (s_m, s_n).  V7/32V have no interleave, which is m = n = 1 (the
 * identity map).  The s_m/s_n factors are Coherent's own — no other edition
 * carries them. */
static void v7_bflist(void)
{
    uint32_t m = v7_fmt.interleave ? v7_m : 1u;
    uint32_t n = v7_fmt.interleave ? v7_n : 1u;
    if (n < 1 || n > V7_COH_MAXINTN || m < 1 || m > n || n % m != 0) {
        m = 1;
        n = 1;
    }

    uint32_t maptab[V7_COH_MAXINTN];
    uint32_t ratio = n / m;
    for (uint32_t i = 0; i < n; i++)
        maptab[i] = (i / ratio) + (i % ratio) * m;

    /* Interleave only within the data band, aligned to n blocks. */
    uint32_t mapbot = ((v7_isize + n - 1) / n) * n;
    uint32_t maptop = (v7_fsize / n) * n;

    /* inode 1: the (empty) bad-block file, as V7's mkfs writes */
    {
        uint32_t zaddr[V7_NIADDR] = {0};
        v7_iput(1, V7_IFREG, 0, 0, zaddr);
    }
    for (uint32_t bn = v7_isize; bn < v7_fsize; bn++) {
        uint32_t blk = bn;
        if (bn >= mapbot && bn < maptop)
            blk = (bn / n) * n + maptab[bn % n];
        v7_bfree(blk);
    }
}

static void v7_mkroot(void)
{
    uint32_t addr[V7_NIADDR] = {0};
    uint32_t bno = v7_alloc();
    uint8_t db[V7_MAXBSIZE];

    memset(db, 0, bsize);
    v7_fmt.bo->put16(db, V7_ROOTINO);
    memcpy(db + 2, ".", 1);
    v7_fmt.bo->put16(db + V7_DIRENTSZ, V7_ROOTINO);
    memcpy(db + V7_DIRENTSZ + 2, "..", 2);
    pblock(bno, db);

    addr[0] = bno;
    v7_iput(V7_ROOTINO, V7_IFDIR | 0777, 2, 2 * V7_DIRENTSZ, addr);
}

static void v7_iput(uint32_t ino, uint16_t mode, int16_t nlink, uint32_t size,
                    const uint32_t *addr)
{
    uint32_t d = 2 + (ino - 1) / v7_ipb;
    uint32_t o = (ino - 1) % v7_ipb;
    uint8_t ib[V7_MAXBSIZE];
    int i;

    if (pread(fd, ib, bsize, (off_t)(base + (uint64_t)d * bsize)) != (ssize_t)bsize)
        die("read error at inode block %u\n", d);

    uint8_t *ip = ib + o * V7_INODESZ;
    memset(ip, 0, V7_INODESZ);
    v7_fmt.bo->put16(ip + 0, mode);
    v7_fmt.bo->put16(ip + 2, (uint16_t)nlink);
    v7_fmt.bo->put32(ip + 8, size);
    for (i = 0; i < v7_fmt.niaddr; i++)
        v7_fmt.bo->put24(ip + 12 + 3 * i, addr[i]);
    v7_fmt.bo->put32(ip + 52, (uint32_t)time(NULL));
    v7_fmt.bo->put32(ip + 56, (uint32_t)time(NULL));
    v7_fmt.bo->put32(ip + 60, (uint32_t)time(NULL));

    pblock(d, ib);
    v7_tinode--;
}

static void mkfs_v7(const char *path, uint32_t blocks, const char *bootfile)
{
    /* i-list: ~1 inode per 25 blocks, minimum 1 block, +2 (boot/super) */
    uint32_t isz = blocks / 25;
    if (isz == 0)
        isz = 1;
    if (isz > 65500 / v7_ipb)
        isz = 65500 / v7_ipb;
    isz += 2;
    if (isz >= blocks)
        die("%s: %u blocks too small for an i-list of %u blocks\n",
            path, blocks, isz);

    v7_isize = isz;
    v7_fsize = blocks;

    if (bootfile)
        write_boot(bootfile);

    memset(v7_sbuf, 0, bsize);
    v7_fmt.bo->put16(v7_sbuf, (uint16_t)v7_isize);
    v7_fmt.bo->put32(v7_sbuf + sb_fsize_off(v7_fmt.pack4), v7_fsize);
    if (v7_fmt.magic)
        bo_put32le(v7_sbuf + 0x3F8, V7_XEN_MAGIC);   /* Xenix magic (offset 1016) */
    if (v7_fmt.interleave) {
        int toff = sb_time_off(v7_fmt.pack4, v7_fmt.nicfree);
        v7_fmt.bo->put16(v7_sbuf + toff + 10, v7_m);             /* s_m */
        v7_fmt.bo->put16(v7_sbuf + toff + 12, v7_n);             /* s_n */
        v7_fmt.bo->put32(v7_sbuf + toff + 26, 0);                /* s_unique (4-byte `long`) */
    }

    /* s_isize is the first data block (not the i-list size), so the i-list is
     * blocks 2..s_isize-1 and the inode count is (s_isize-2) * inodes/block. */
    if (v7_fmt.interleave)
        printf("m/n = %u %u, ", v7_m, v7_n);
    printf("isize = %u, fsize = %u\n",
           (v7_isize - 2) * v7_ipb, v7_fsize);

    uint8_t zb[V7_MAXBSIZE] = {0};
    for (uint32_t b = 2; b < v7_isize; b++) {
        pblock(b, zb);
        v7_tinode += v7_ipb;
    }

    v7_bflist();
    v7_mkroot();

    v7_fmt.bo->put16(v7_sbuf + sb_nfree_off(v7_fmt.pack4), (uint16_t)v7_nfree);
    v7_fmt.bo->put32(v7_sbuf + sb_time_off(v7_fmt.pack4, v7_fmt.nicfree), (uint32_t)time(NULL));   /* s_time */
    if (!v7_fmt.pack4) {
        int toff = sb_time_off(v7_fmt.pack4, v7_fmt.nicfree);
        v7_fmt.bo->put32(v7_sbuf + toff + 4, v7_tfree);          /* s_tfree */
        v7_fmt.bo->put16(v7_sbuf + toff + 8, (uint16_t)v7_tinode); /* s_tinode */
    }
    pblock(1, v7_sbuf);

    if (ftruncate(fd, (off_t)(base + (uint64_t)v7_fsize * bsize)) < 0)
        die("%s: ftruncate: %s\n", path, strerror(errno));

    printf("%s: %u blocks, %u inodes written\n", path, v7_fsize, v7_tinode);
}

/* ---- V6 implementation -------------------------------------------------- */


static uint32_t v6_alloc(void)
{
    uint32_t bno;
    int i;

    v6_nfree--;
    bno = bo_me.get16(v6_sbuf + 6 + 2 * v6_nfree);
    if (bno == 0)
        die("out of free space\n");
    if (v6_nfree == 0) {
        uint8_t fb[V6_BSIZE];
        if (pread(fd, fb, V6_BSIZE, (off_t)(base + (uint64_t)bno * V6_BSIZE)) != V6_BSIZE)
            die("read error at block %u\n", bno);
        v6_nfree = bo_me.get16(fb);
        for (i = 0; i < V6_NICFREE; i++)
            bo_me.put16(v6_sbuf + 6 + 2 * i, bo_me.get16(fb + 2 + 2 * i));
    }
    return bno;
}

static void v6_bfree(uint32_t bno)
{
    int i;

    if (v6_nfree >= V6_NICFREE) {
        memset(v6_freebuf, 0, V6_BSIZE);
        bo_me.put16(v6_freebuf, (uint16_t)v6_nfree);
        for (i = 0; i < V6_NICFREE; i++)
            bo_me.put16(v6_freebuf + 2 + 2 * i, bo_me.get16(v6_sbuf + 6 + 2 * i));
        pblock(bno, v6_freebuf);
        v6_nfree = 0;
    }
    bo_me.put16(v6_sbuf + 6 + 2 * v6_nfree, (uint16_t)bno);
    v6_nfree++;
}

/* Interleave free blocks with stride m modulo n, as V6's mkfs does. */
static void v6_bflist(uint32_t m, uint32_t n)
{
    uint8_t  flg[100];
    uint32_t adr[100];
    uint32_t d, f;
    uint32_t i, j;

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
    bo_me.put16(db, V6_ROOTINO);
    memcpy(db + 2, ".", 1);
    bo_me.put16(db + 16, V6_ROOTINO);
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
    bo_me.put16(ip + 0, mode);
    ip[2] = (uint8_t)nlink;                 /* i_nlink */
    /* i_uid, i_gid = 0 */
    ip[5] = (uint8_t)((size >> 16) & 0xff); /* i_size0: high byte */
    bo_me.put16(ip + 6, (uint16_t)(size & 0xffff));   /* i_size1: low word */
    for (i = 0; i < V6_NIADDR; i++)
        bo_me.put16(ip + 8 + 2 * i, (uint16_t)addr[i]);
    bo_me.put32(ip + 24, (uint32_t)time(NULL));   /* i_atime */
    bo_me.put32(ip + 28, (uint32_t)time(NULL));   /* i_mtime */

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
    bo_me.put16(v6_sbuf, (uint16_t)v6_isize);
    bo_me.put16(v6_sbuf + 2, (uint16_t)v6_fsize);

    printf("isize = %u, fsize = %u\n", v6_isize * V6_INOPB, v6_fsize);

    /* zero the i-list: blocks 2..isize+1 */
    uint8_t zb[V6_BSIZE] = {0};
    for (uint32_t b = 2; b < v6_isize + 2; b++)
        pblock(b, zb);

    v6_bflist(3, 100);
    v6_mkroot();

    bo_me.put16(v6_sbuf + 4, (uint16_t)v6_nfree);
    bo_me.put32(v6_sbuf + 412, (uint32_t)time(NULL));   /* s_time[2] */
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
    bo_me.put16(ip + 0, mode);
    ip[2] = (uint8_t)nlink;
    /* i_uid = 0 */
    bo_me.put16(ip + 4, (uint16_t)size);
    for (int i = 0; i < V1_NIADDR; i++)
        bo_me.put16(ip + 6 + 2 * i, (uint16_t)addr[i]);
    bo_me.put32(ip + 22, (uint32_t)time(NULL) * 60u);   /* ctime (60ths) */
    bo_me.put32(ip + 26, (uint32_t)time(NULL) * 60u);   /* mtime (60ths) */

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
    bo_me.put16(v1_sb + 0, (uint16_t)freemap_bytes);
    bo_me.put16(v1_sb + 2 + freemap_bytes, (uint16_t)inodemap_bytes);

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
    bo_me.put16(db + 0, V1_ROOTINO);
    memcpy(db + 2, ".", 1);
    bo_me.put16(db + V1_DIRENTSZ, V1_ROOTINO);
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
        bo_put32le(raw + i * P7_WORDBYTES, words[i]);
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
        die("%s: pdp7: size is fixed by the RB09 geometry (8000 blocks/surface)\n",
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

/* ---- 2.11BSD -------------------------------------------------------------- */

static uint32_t bsd211_isize, bsd211_fsize;
static uint8_t  bsd211_sbuf[BSD211_BSIZE];
static uint8_t  bsd211_freebuf[BSD211_BSIZE];
static uint16_t bsd211_nfree;
static uint32_t bsd211_tinode, bsd211_tfree;


static uint32_t bsd211_alloc(void)
{
    uint32_t bno;
    int i;

    bsd211_tfree--;
    bsd211_nfree--;
    bno = bo_me.get32(bsd211_sbuf + BSD211_SB_FREE + 4 * bsd211_nfree);
    if (bno == 0)
        die("out of free space\n");
    if (bsd211_nfree == 0) {
        uint8_t fb[BSD211_BSIZE];
        if (pread(fd, fb, BSD211_BSIZE, (off_t)(base + (uint64_t)bno * BSD211_BSIZE)) != BSD211_BSIZE)
            die("read error at block %u\n", bno);
        bsd211_nfree = bo_me.get16(fb);
        for (i = 0; i < BSD211_NICFREE; i++)
            bo_me.put32(bsd211_sbuf + BSD211_SB_FREE + 4 * i, bo_me.get32(fb + 2 + 4 * i));
    }
    return bno;
}

static void bsd211_bfree(uint32_t bno)
{
    int i;

    if (bsd211_nfree == 0) {
        /* Seed the 0 sentinel, mirroring the kernel's free(): it terminates the
         * free-list chain and is not itself a free block. */
        bo_me.put32(bsd211_sbuf + BSD211_SB_FREE, 0);
        bsd211_nfree = 1;
    }
    if (bsd211_nfree >= BSD211_NICFREE) {
        memset(bsd211_freebuf, 0, BSD211_BSIZE);
        bo_me.put16(bsd211_freebuf, (uint16_t)bsd211_nfree);
        for (i = 0; i < BSD211_NICFREE; i++)
            bo_me.put32(bsd211_freebuf + 2 + 4 * i,
                          bo_me.get32(bsd211_sbuf + BSD211_SB_FREE + 4 * i));
        pblock(bno, bsd211_freebuf);
        bsd211_nfree = 0;
    }
    bo_me.put32(bsd211_sbuf + BSD211_SB_FREE + 4 * bsd211_nfree, bno);
    bsd211_nfree++;
    bsd211_tfree++;
}

static void bsd211_iput(uint32_t ino, uint16_t mode, int16_t nlink, uint32_t size,
                       const uint32_t *addr)
{
    uint32_t d = 2 + (ino - 1) / BSD211_INOPB;
    uint32_t o = (ino - 1) % BSD211_INOPB;
    uint8_t ib[BSD211_BSIZE];
    int i;

    if (pread(fd, ib, BSD211_BSIZE, (off_t)(base + (uint64_t)d * BSD211_BSIZE)) != BSD211_BSIZE)
        die("read error at inode block %u\n", d);

    uint8_t *ip = ib + o * BSD211_INODESZ;
    memset(ip, 0, BSD211_INODESZ);
    bo_me.put16(ip + 0, mode);
    bo_me.put16(ip + 2, (uint16_t)nlink);
    bo_me.put32(ip + 8, size);
    for (i = 0; i < BSD211_NIADDR; i++)
        bo_me.put32(ip + 12 + 4 * i, addr[i]);
    /* di_flags (offset 50) stays zero */
    bo_me.put32(ip + 52, (uint32_t)time(NULL));
    bo_me.put32(ip + 56, (uint32_t)time(NULL));
    bo_me.put32(ip + 60, (uint32_t)time(NULL));

    pblock(d, ib);
    bsd211_tinode--;
}

/* DIRSIZ: round_up(6 + namlen, 4), the same (7+namlen+3)&~3 as bsd211fs.c. */
static uint32_t bsd211_dirsiz(uint16_t namlen) { return (7u + namlen + 3u) & ~3u; }

static void bsd211_mkroot(void)
{
    /* lost+found (inode 3): two 512-byte directory blocks.  The first holds
     * "." and ".."; the second is empty (one free entry spanning it). */
    uint32_t lfb = bsd211_alloc();
    uint8_t lf[BSD211_BSIZE] = {0};
    bo_me.put16(lf + 0, BSD211_LOSTFOUNDINO);          /* . */
    bo_me.put16(lf + 2, (uint16_t)bsd211_dirsiz(1));
    bo_me.put16(lf + 4, 1);
    lf[6] = '.';
    bo_me.put16(lf + 8, BSD211_ROOTINO);               /* .. (fills the block) */
    bo_me.put16(lf + 10, (uint16_t)(BSD211_DIRBLKSIZ - 8));
    bo_me.put16(lf + 12, 2);
    lf[14] = '.'; lf[15] = '.';
    bo_me.put16(lf + BSD211_DIRBLKSIZ + 2, (uint16_t)BSD211_DIRBLKSIZ);  /* 2nd block empty */
    pblock(lfb, lf);

    uint32_t lfa[BSD211_NIADDR] = {0};
    lfa[0] = lfb;
    bsd211_iput(BSD211_LOSTFOUNDINO, BSD211_IFDIR | 0777, 2, BSD211_DIRBLKSIZ * 2, lfa);

    /* root (inode 2): one 512-byte directory block with ".", "..", "lost+found". */
    uint32_t rb = bsd211_alloc();
    uint8_t rd[BSD211_BSIZE] = {0};
    uint32_t off = 0;
    bo_me.put16(rd + off, BSD211_ROOTINO);             /* . */
    bo_me.put16(rd + off + 2, (uint16_t)bsd211_dirsiz(1));
    bo_me.put16(rd + off + 4, 1);
    rd[off + 6] = '.';
    off += bsd211_dirsiz(1);
    bo_me.put16(rd + off, BSD211_ROOTINO);             /* .. */
    bo_me.put16(rd + off + 2, (uint16_t)bsd211_dirsiz(2));
    bo_me.put16(rd + off + 4, 2);
    rd[off + 6] = '.'; rd[off + 7] = '.';
    off += bsd211_dirsiz(2);
    bo_me.put16(rd + off, BSD211_LOSTFOUNDINO);        /* lost+found (fills the block) */
    bo_me.put16(rd + off + 2, (uint16_t)(BSD211_DIRBLKSIZ - off));
    bo_me.put16(rd + off + 4, 10);
    memcpy(rd + off + 6, "lost+found", 10);
    pblock(rb, rd);

    uint32_t ra[BSD211_NIADDR] = {0};
    ra[0] = rb;
    bsd211_iput(BSD211_ROOTINO, BSD211_IFDIR | 0777, 3, BSD211_DIRBLKSIZ, ra);
}

static void mkfs_bsd211(const char *path, uint32_t blocks, const char *bootfile)
{
    (void)bootfile;   /* no boot-block install for 2.11BSD (yet) */

    uint32_t isz = blocks / 25;
    if (isz == 0)
        isz = 1;
    if (isz > 65500 / BSD211_INOPB)
        isz = 65500 / BSD211_INOPB;
    isz += 2;
    if (isz >= blocks)
        die("%s: %u blocks too small for an i-list of %u blocks\n", path, blocks, isz);

    bsd211_isize = isz;
    bsd211_fsize = blocks;

    memset(bsd211_sbuf, 0, BSD211_BSIZE);
    bo_me.put16(bsd211_sbuf + BSD211_SB_ISIZE, (uint16_t)bsd211_isize);
    bo_me.put32(bsd211_sbuf + BSD211_SB_FSIZE, bsd211_fsize);

    printf("isize = %u, fsize = %u\n", (bsd211_isize - 2) * BSD211_INOPB, bsd211_fsize);

    uint8_t zb[BSD211_BSIZE] = {0};
    for (uint32_t b = 2; b < bsd211_isize; b++) {
        pblock(b, zb);
        bsd211_tinode += BSD211_INOPB;
    }

    /* inode 1: reserved (empty regular file), as BSD's mkfs writes */
    {
        uint32_t zaddr[BSD211_NIADDR] = {0};
        bsd211_iput(1, BSD211_IFREG, 0, 0, zaddr);
    }

    for (uint32_t bn = bsd211_isize; bn < bsd211_fsize; bn++)
        bsd211_bfree(bn);

    bsd211_mkroot();

    bo_me.put16(bsd211_sbuf + BSD211_SB_NFREE, (uint16_t)bsd211_nfree);
    bo_me.put32(bsd211_sbuf + BSD211_SB_TIME, (uint32_t)time(NULL));
    bo_me.put32(bsd211_sbuf + BSD211_SB_TFREE, bsd211_tfree);
    bo_me.put16(bsd211_sbuf + BSD211_SB_TINODE, (uint16_t)bsd211_tinode);
    bsd211_sbuf[BSD211_SB_FMOD] = 0;   /* clean */
    pblock(1, bsd211_sbuf);

    if (ftruncate(fd, (off_t)(base + (uint64_t)bsd211_fsize * BSD211_BSIZE)) < 0)
        die("%s: ftruncate: %s\n", path, strerror(errno));

    printf("%s: %u blocks, %u inodes written\n", path, bsd211_fsize, bsd211_tinode);
}

/* ---- main --------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *path;
    const char *bootfile = NULL;
    uint32_t blocks = 0;
    uint64_t offblock = 0;
    int edition = -1;   /* no default: the version must be named explicitly */
    int c;

    while ((c = getopt(argc, argv, "v:o:b:m:n:")) != -1) {
        switch (c) {
        case 'v':
            edition = filsys_edition_by_name(optarg);
            if (edition < 0) {
                fprintf(stderr, "mkfs.filsys: bad edition '%s'\n", optarg);
                return 1;
            }
            break;
        case 'o': offblock = strtoull(optarg, NULL, 0); break;
        case 'b': bootfile = optarg; break;
        case 'm': v7_m = (uint16_t)strtoul(optarg, NULL, 0); break;
        case 'n': v7_n = (uint16_t)strtoul(optarg, NULL, 0); break;
        default:
            fprintf(stderr, "usage: mkfs.filsys -v <edition> [-o block] [-b boot] [-m m] [-n n] image [blocks]\n"
                            "  editions: %s\n", filsys_editions_usage());
            return 1;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "usage: mkfs.filsys -v <edition> [-o block] [-b boot] [-m m] [-n n] image [blocks]\n"
                        "  editions: %s\n", filsys_editions_usage());
        return 1;
    }
    if (edition < 0) {
        fprintf(stderr, "mkfs.filsys: no filesystem version given; use -v <edition>\n");
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

    /* Set the edition's block size and layout flags before resolve_blocks
     * computes the byte offset and block count in that block size. */
    v7_fmt = filsys_getformat(edition);
    bsize = v7_fmt.bsize;
    v7_ipb = v7_fmt.bsize / v7_fmt.inode_size;

    blocks = resolve_blocks(path, offblock, blocks);

    if (edition == FILSYS_V6)
        mkfs_v6(path, blocks, bootfile);
    else if (edition == FILSYS_V1)
        mkfs_v1(path, blocks, bootfile);
    else if (edition == FILSYS_BSD211)
        mkfs_bsd211(path, blocks, bootfile);
    else
        mkfs_v7(path, blocks, bootfile);   /* v7, vax32/32v, coherent, xenix, bsd29 */

    close(fd);
    return 0;
}
