/* filsys 1.8.0 - 2026-09-06 - Copyright (C) 2026 David Walther */
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
#include "filsys_ops.h"
#include "v1fs.h"
#include "v7fs.h"
#include "pdp7fs.h"

enum { A_MAGIC1 = 0407 };  /* V7 normal a.out magic (boot block) */

static int      fd;
static uint64_t base;   /* byte offset of the filesystem in the image */
static uint32_t bsize = V7_BSIZE;   /* logical block size (512 or 1024) */

/* A block device can't be resized (ftruncate returns EINVAL); its size is fixed
 * by the device itself.  Only grow a regular-file image. */
static int image_is_regular(void)
{
    struct stat st;
    return fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
}

/* Open the image read-write.  A block (or character) device -- and an existing
 * regular file -- opens directly; only a missing path is created as a regular
 * file.  The device is never truncated: its size is fixed by the device, and
 * O_TRUNC on one is at best ignored and at worst rejected. */
static int open_image(const char *path)
{
    int f = open(path, O_RDWR);
    if (f < 0 && errno == ENOENT)
        f = open(path, O_RDWR | O_CREAT, 0666);
    return f;
}

static void pblock(uint32_t bno, const uint8_t *buf);
static void write_boot(const char *path);
static void die(const char *fmt, ...);

/* ---- shared mkfs driver (free-list-cache editions: V7/32V/Coherent/Xenix/
 *      2.9BSD/V6/2.11BSD) -------------------------------------------------- */

static uint16_t v7_m = 1, v7_n = 1;   /* interleave factors (s_m/s_n, coherent) */

/* One hook per format: the parts that genuinely differ are the i-list sizing
 * and the seed (reserved + root) inodes/directories.  Everything else -- the
 * allocator, the inode codec, the free-list build with its per-edition
 * interleave, and the superblock layout -- comes from the library's ops/alloc
 * vtable. */
struct mkfs_fmt {
    uint32_t (*isize)(uint32_t blocks, uint32_t ipb);  /* -> fs->isize */
    uint32_t (*seed)(filsys_edition_t *fs);            /* returns used inodes */
};

static uint32_t isize_v7(uint32_t blocks, uint32_t ipb);
static uint32_t isize_v6(uint32_t blocks, uint32_t ipb);
static uint32_t isize_v8(uint32_t blocks, uint32_t ipb);
static uint32_t seed_v7(filsys_edition_t *fs);
static uint32_t seed_v6(filsys_edition_t *fs);
static uint32_t seed_v8(filsys_edition_t *fs);
static uint32_t seed_bsd211(filsys_edition_t *fs);
static void mkfs_common(filsys_edition_t *fs, const struct mkfs_fmt *fmt,
                        const char *path, uint32_t blocks, const char *bootfile);

/* Open (or create) the image, then work out the filesystem size in blocks:
 * an explicit `blocks` argument, else the image's own size, else a default. */
static uint32_t resolve_blocks(const char *path, uint64_t offblock, uint32_t blocks)
{
    base = offblock * bsize;

    fd = open_image(path);
    if (fd < 0)
        die("%s: cannot open/create: %s\n", path, strerror(errno));

    if (blocks == 0) {
        uint64_t sz = 0;
        if (filsys_dev_size(fd, &sz) == 0 && sz >= base + bsize)
            blocks = (uint32_t)((sz - base) / bsize);
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

/* ---- shared driver + free-list-cache hooks -------------------------------- */

static uint32_t isize_v7(uint32_t blocks, uint32_t ipb)
{
    uint32_t isz = blocks / 25;
    if (isz == 0)
        isz = 1;
    if (isz > 65500 / ipb)
        isz = 65500 / ipb;
    return isz + 2;   /* s_isize is the first data block */
}

static uint32_t isize_v6(uint32_t blocks, uint32_t ipb)
{
    (void)ipb;
    uint32_t isz = blocks / (43 + blocks / 1000);
    if (isz == 0)
        isz = 1;
    return isz;       /* s_isize counts i-list blocks */
}

/* V8-family: s_isize is the first data block; mkbitfs picks
 * (size - 2) / (1 + INOPB) and caps the inode count at 65536 (16-bit ino_t). */
static uint32_t isize_v8(uint32_t blocks, uint32_t ipb)
{
    uint32_t isz = (blocks - 2) / (1 + ipb);
    if (isz < 3)
        isz = 3;                 /* need block 2 for inode 2 (the root) */
    if (isz > 65536 / ipb)
        isz = 65536 / ipb;
    return isz;                  /* s_isize is the first data block */
}

static uint32_t seed_v7(filsys_edition_t *fs)
{
    filsys_inode_t ip;
    uint8_t db[V7_MAXBSIZE];

    /* inode 1: the (empty) bad-block file, as V7's mkfs writes */
    memset(&ip, 0, sizeof ip);
    ip.ino = 1;
    ip.mode = fs->ifreg;
    fs->ops->write_inode(fs, 1, &ip);

    /* root: a one-block directory with "." and ".." */
    uint32_t bno;
    fs->alloc->balloc(fs, &bno);
    memset(db, 0, fs->bsize);
    fs->bo->put16(db, fs->rootino);
    memcpy(db + 2, ".", 1);
    fs->bo->put16(db + fs->dirent_size, fs->rootino);
    memcpy(db + fs->dirent_size + 2, "..", 2);
    fs->ops->write_block(fs, bno, db);

    memset(&ip, 0, sizeof ip);
    ip.ino = fs->rootino;
    ip.mode = fs->ifdir | 0777;
    ip.nlink = 2;
    ip.size = 2 * fs->dirent_size;
    ip.addr[0] = bno;
    fs->ops->write_inode(fs, fs->rootino, &ip);
    return 2;
}

/* V8-family: no bad-block inode; only the root directory is seeded. */
static uint32_t seed_v8(filsys_edition_t *fs)
{
    filsys_inode_t ip;
    uint8_t db[V7_MAXBSIZE];

    uint32_t bno;
    fs->alloc->balloc(fs, &bno);
    memset(db, 0, fs->bsize);
    fs->bo->put16(db, fs->rootino);
    memcpy(db + 2, ".", 1);
    fs->bo->put16(db + fs->dirent_size, fs->rootino);
    memcpy(db + fs->dirent_size + 2, "..", 2);
    fs->ops->write_block(fs, bno, db);

    memset(&ip, 0, sizeof ip);
    ip.ino = fs->rootino;
    ip.mode = fs->ifdir | 0777;
    ip.nlink = 2;
    ip.size = 2 * fs->dirent_size;
    ip.addr[0] = bno;
    fs->ops->write_inode(fs, fs->rootino, &ip);
    return 1;
}

static uint32_t seed_v6(filsys_edition_t *fs)
{
    filsys_inode_t ip;
    uint8_t db[V6_BSIZE];

    /* root (inode 1): "." and ".." point at the root itself */
    uint32_t bno;
    fs->alloc->balloc(fs, &bno);
    memset(db, 0, V6_BSIZE);
    bo_put16le(db, V6_ROOTINO);
    memcpy(db + 2, ".", 1);
    bo_put16le(db + 16, V6_ROOTINO);
    memcpy(db + 18, "..", 2);
    fs->ops->write_block(fs, bno, db);

    memset(&ip, 0, sizeof ip);
    ip.ino = V6_ROOTINO;
    ip.mode = V6_IALLOC | V6_IFDIR | 0777;
    ip.nlink = 2;
    ip.size = 32;
    ip.addr[0] = bno;
    fs->ops->write_inode(fs, V6_ROOTINO, &ip);
    return 1;
}

static void mkfs_common(filsys_edition_t *fs, const struct mkfs_fmt *fmt,
                        const char *path, uint32_t blocks, const char *bootfile)
{
    uint32_t ipb = fs->bsize / fs->inode_size;
    fs->readonly = 0;
    fs->base = base;
    fs->fd = fd;
    fs->io = &filsys_io_file;
    fs->isize = fmt->isize(blocks, ipb) & 0xFFFFu;   /* s_isize is a 16-bit field */
    fs->fsize = blocks;
    fs->m = v7_m;         /* coherent -m/-n (ignored unless fs->interleave) */
    fs->n = v7_n;
    fs->unique = 0;

    /* The out-of-superblock bitmap's blocks sit at the tail as metadata; work
     * them out up front so data_end (and the checker's data band) exclude them. */
    if (fs->freemap == V8_FREEMAP_BIGMAP) {
        uint32_t bits_per_blk = fs->bsize * 8;
        fs->v8_nblks = (fs->fsize + bits_per_blk - 1) / bits_per_blk;
        fs->v8_blk_start = fs->fsize - fs->v8_nblks;
    }

    if (v7_data_first(fs) >= fs->fsize)
        die("%s: %u blocks too small for an i-list of %u blocks\n",
            path, blocks, fs->isize);

    if (bootfile)
        write_boot(bootfile);

    /* Grow the image first: the library reads blocks read-modify-write, so the
     * superblock and i-list blocks must already exist (as zeros). */
    if (image_is_regular() &&
        ftruncate(fd, (off_t)(base + (uint64_t)blocks * fs->bsize)) < 0)
        die("%s: ftruncate: %s\n", path, strerror(errno));

    /* Xenix and System V carry a magic word at a fixed superblock offset (in the
     * edition's byte order); System V also carries s_type naming the block size.
     * Write both once, since super_write preserves the bytes it does not
     * maintain.  System V's superblock is a 512-byte struct at byte 512, not
     * block 1, whatever the logical block size. */
    if (fs->magic) {
        uint8_t sb[V7_MAXBSIZE] = {0};
        fs->bo->put32(sb + fs->magic_off, fs->magic);
        if (fs->dyn_bsize) {
            uint32_t st = fs->bsize == 512 ? V7_SYSV_Fs1b
                        : fs->bsize == 1024 ? V7_SYSV_Fs2b
                        : V7_SYSV_Fs4b;
            fs->bo->put32(sb + V7_SYSV_TYPE_OFF, st);
            fs->io->write(fs, sb, 512, 512 + (off_t)fs->base);
        } else {
            fs->ops->write_block(fs, V7_SUPERB, sb);
        }
    }

    uint8_t zb[V7_MAXBSIZE] = {0};
    for (uint32_t b = 2; b < v7_data_first(fs); b++)
        fs->ops->write_block(fs, b, zb);

    /* Build the free list by reusing the check driver's salvage op, which
     * applies the per-edition interleave (Coherent maptab / V6 stride / none)
     * against an empty used-block map -- nothing is allocated yet. */
    uint32_t nblk = fs->ops->data_end(fs) - v7_data_first(fs);
    uint8_t *bmap = calloc((size_t)(nblk + 7) / 8, 1);
    if (!bmap)
        die("out of memory\n");
    filsys_chkctx_t cx;
    memset(&cx, 0, sizeof cx);
    cx.bmap = bmap;
    cx.nblk = nblk;
    fs->ops->makefree(fs, &cx);
    free(bmap);

    /* reserved + root inodes and directories */
    uint32_t used = fmt->seed(fs);

    /* makefree resets the free-inode total; restore it here. */
    fs->fl.tinode = v7_maxinode(fs) - used;
    fs->ops->sync(fs);

    printf("%s: %u blocks, %u inodes written\n",
           path, fs->fsize, v7_maxinode(fs));
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

    if (image_is_regular() &&
        ftruncate(fd, (off_t)(base + (uint64_t)v1_fsize * V1_BSIZE)) < 0)
        die("%s: ftruncate: %s\n", path, strerror(errno));

    printf("%s: %u blocks, %u inodes written\n", path, v1_fsize, v1_maxino);
}

/* ---- PDP-7 --------------------------------------------------------------- */

static uint32_t p7_pool[P7_NBLOCKS];   /* free data-block pool */
static uint32_t p7_nfree;
static uint32_t p7_free_count;         /* free blocks listed in the free list */
static const word_codec_t *p7_wc;      /* the container codec being written */

static void p7_pblock(uint32_t bno, const uint32_t *words)
{
    uint8_t raw[P7_MAXBLOCKBYTES];
    for (uint32_t i = 0; i < P7_WSIZE; i++)
        p7_wc->put(raw, i, words[i]);
    /* The image is the bare filesystem surface (surface 1), so block 0 of the
     * filesystem is byte 0 of the file -- the same "-o offset=" convention as
     * every other edition. */
    off_t pos = (off_t)bno * (off_t)p7_wc->block_bytes;
    if (pwrite(fd, raw, p7_wc->block_bytes, pos) != (ssize_t)p7_wc->block_bytes)
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

static void mkfs_pdp7(const char *path, uint32_t blocks, const char *bootfile,
                      const word_codec_t *word)
{
    /* The RB09 fixed-head disk has one valid geometry (8000 blocks/surface);
     * there is no size to choose.  We write the bare filesystem surface (one
     * surface, not the full two-surface disk), so block 0 is byte 0. */
    if (blocks != 0)
        die("%s: pdp7: size is fixed by the RB09 geometry (8000 blocks/surface)\n",
            path);
    (void)bootfile;
    p7_wc = word;

    fd = open_image(path);
    if (fd < 0)
        die("%s: cannot open/create: %s\n", path, strerror(errno));
    if (image_is_regular() &&
        ftruncate(fd, (off_t)((uint64_t)P7_NBLOCKS * word->block_bytes)) < 0)
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

static uint32_t bsd211_dirsiz(uint16_t namlen) { return (7u + namlen + 3u) & ~3u; }

static uint32_t seed_bsd211(filsys_edition_t *fs)
{
    filsys_inode_t ip;
    uint8_t lf[BSD211_BSIZE];

    /* inode 1: reserved (empty regular file), as BSD's mkfs writes */
    memset(&ip, 0, sizeof ip);
    ip.ino = 1;
    ip.mode = BSD211_IFREG;
    fs->ops->write_inode(fs, 1, &ip);

    /* lost+found (inode 3): two 512-byte directory blocks */
    uint32_t lfb;
    fs->alloc->balloc(fs, &lfb);
    memset(lf, 0, sizeof lf);
    bo_put16le(lf + 0, BSD211_LOSTFOUNDINO);
    bo_put16le(lf + 2, (uint16_t)bsd211_dirsiz(1));
    bo_put16le(lf + 4, 1);
    lf[6] = '.';
    bo_put16le(lf + 8, BSD211_ROOTINO);
    bo_put16le(lf + 10, (uint16_t)(BSD211_DIRBLKSIZ - 8));
    bo_put16le(lf + 12, 2);
    lf[14] = '.'; lf[15] = '.';
    bo_put16le(lf + BSD211_DIRBLKSIZ + 2, (uint16_t)BSD211_DIRBLKSIZ);
    fs->ops->write_block(fs, lfb, lf);

    memset(&ip, 0, sizeof ip);
    ip.ino = BSD211_LOSTFOUNDINO;
    ip.mode = BSD211_IFDIR | 0777;
    ip.nlink = 2;
    ip.size = BSD211_DIRBLKSIZ * 2;
    ip.addr[0] = lfb;
    fs->ops->write_inode(fs, BSD211_LOSTFOUNDINO, &ip);

    /* root (inode 2): ".", "..", "lost+found" */
    uint32_t rb;
    fs->alloc->balloc(fs, &rb);
    uint8_t rd[BSD211_BSIZE];
    memset(rd, 0, sizeof rd);
    uint32_t off = 0;
    bo_put16le(rd + off, BSD211_ROOTINO);
    bo_put16le(rd + off + 2, (uint16_t)bsd211_dirsiz(1));
    bo_put16le(rd + off + 4, 1);
    rd[off + 6] = '.';
    off += bsd211_dirsiz(1);
    bo_put16le(rd + off, BSD211_ROOTINO);
    bo_put16le(rd + off + 2, (uint16_t)bsd211_dirsiz(2));
    bo_put16le(rd + off + 4, 2);
    rd[off + 6] = '.'; rd[off + 7] = '.';
    off += bsd211_dirsiz(2);
    bo_put16le(rd + off, BSD211_LOSTFOUNDINO);
    bo_put16le(rd + off + 2, (uint16_t)(BSD211_DIRBLKSIZ - off));
    bo_put16le(rd + off + 4, 10);
    memcpy(rd + off + 6, "lost+found", 10);
    fs->ops->write_block(fs, rb, rd);

    memset(&ip, 0, sizeof ip);
    ip.ino = BSD211_ROOTINO;
    ip.mode = BSD211_IFDIR | 0777;
    ip.nlink = 3;
    ip.size = BSD211_DIRBLKSIZ;
    ip.addr[0] = rb;
    fs->ops->write_inode(fs, BSD211_ROOTINO, &ip);
    return 3;
}



/* ---- main --------------------------------------------------------------- */

/* Parse a -g geometry spec ("blocksize=4096,freemap=bitmap,byteorder=be") into
 * a filsys_geom_t.  Returns 0, or -1 with *errmsg set.  byteorder is strdup'd
 * (mkfs is one-shot; the process owns it until exit). */
static int parse_geom(const char *spec, filsys_geom_t *g, const char **errmsg)
{
    static char msgbuf[128];
    *errmsg = NULL;
    char *s = strdup(spec);
    if (!s)
        return -1;
    for (char *tok = strtok(s, ","); tok; tok = strtok(NULL, ",")) {
        if (!strncmp(tok, "blocksize=", 10)) {
            char *end = NULL;
            g->blocksize = (uint32_t)strtoul(tok + 10, &end, 0);
            if (!end || *end) {
                snprintf(msgbuf, sizeof msgbuf, "bad blocksize '%s'", tok + 10);
                *errmsg = msgbuf;
                free(s);
                return -1;
            }
        } else if (!strncmp(tok, "freemap=", 8)) {
            if (!strcmp(tok + 8, "list"))       g->freemap = FILSYS_FREEMAP_LIST;
            else if (!strcmp(tok + 8, "bitmap")) g->freemap = FILSYS_FREEMAP_BITMAP;
            else if (!strcmp(tok + 8, "bigmap")) g->freemap = FILSYS_FREEMAP_BIGMAP;
            else {
                snprintf(msgbuf, sizeof msgbuf, "bad freemap '%s'", tok + 8);
                *errmsg = msgbuf;
                free(s);
                return -1;
            }
        } else if (!strncmp(tok, "byteorder=", 10)) {
            g->byteorder = strdup(tok + 10);
        } else {
            snprintf(msgbuf, sizeof msgbuf, "unknown geometry '%s'", tok);
            *errmsg = msgbuf;
            free(s);
            return -1;
        }
    }
    free(s);
    return 0;
}

int main(int argc, char **argv)
{
    const char *path;
    const char *bootfile = NULL;
    uint32_t blocks = 0;
    uint64_t offblock = 0;
    int edition = -1;   /* no default: the version must be named explicitly */
    const char *packing = NULL;   /* PDP-7 word container codec */
    const char *arch = NULL;      /* CPU arch: overrides the edition's byte order */
    uint32_t user_bsize = 0;      /* -B: System V logical block size (512/1024/2048) */
    const char *geom_spec = NULL; /* -g: V8-family blocksize/freemap/byteorder */
    int c;

    while ((c = getopt(argc, argv, "v:o:b:m:n:P:a:B:g:")) != -1) {
        switch (c) {
        case 'v':
            edition = filsys_parse_edition("mkfs.filsys", optarg);
            if (edition < 0)
                return 1;
            break;
        case 'o': offblock = strtoull(optarg, NULL, 0); break;
        case 'b': bootfile = optarg; break;
        case 'm': v7_m = (uint16_t)strtoul(optarg, NULL, 0); break;
        case 'n': v7_n = (uint16_t)strtoul(optarg, NULL, 0); break;
        case 'P': packing = optarg; break;
        case 'a': arch = optarg; break;
        case 'B': user_bsize = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'g': geom_spec = optarg; break;
        default:
            fprintf(stderr, "usage: mkfs.filsys -v <edition> [-o block] [-P packing] [-a arch] [-B bsize] [-g geom] [-b boot] [-m m] [-n n] image [blocks]\n"
                            "  editions: %s\n", filsys_editions_usage());
            return 1;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "usage: mkfs.filsys -v <edition> [-o block] [-P packing] [-a arch] [-B bsize] [-g geom] [-b boot] [-m m] [-n n] image [blocks]\n"
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
        filsys_edition_t desc = filsys_getformat(FILSYS_PDP7);
        if (packing && !(desc.word = filsys_word_codec_by_name(packing))) {
            fprintf(stderr, "mkfs.filsys: unknown packing '%s'\n", packing);
            return 1;
        }
        mkfs_pdp7(path, blocks, bootfile, desc.word);
        close(fd);
        return 0;
    }

    /* Set the edition's block size before resolve_blocks computes the byte
     * offset and block count in that block size. */
    filsys_edition_t fs = filsys_getformat(edition);
    if (arch) {
        const byte_order_ops_t *bo = filsys_arch_bo(arch);
        if (!bo) {
            fprintf(stderr, "mkfs.filsys: bad arch '%s'\n", arch);
            return 1;
        }
        fs.bo = bo;
    }
    if (geom_spec) {
        filsys_geom_t geom = {0, -1, NULL};
        const char *errmsg = NULL;
        if (parse_geom(geom_spec, &geom, &errmsg)) {
            fprintf(stderr, "mkfs.filsys: %s\n", errmsg ? errmsg : "bad geometry");
            return 1;
        }
        if (filsys_apply_geom(&fs, edition, &geom, &errmsg)) {
            fprintf(stderr, "mkfs.filsys: %s\n", errmsg ? errmsg : "bad geometry");
            return 1;
        }
    }
    if (user_bsize) {
        if (!fs.dyn_bsize) {
            fprintf(stderr, "mkfs.filsys: -B block size is System-V-only\n");
            return 1;
        }
        if (user_bsize != 512 && user_bsize != 1024 && user_bsize != 2048) {
            fprintf(stderr, "mkfs.filsys: bad block size %u (want 512, 1024 or 2048)\n",
                    user_bsize);
            return 1;
        }
        fs.bsize = user_bsize;
        fs.nindir = user_bsize / fs.daddr_wid;
    }
    bsize = fs.bsize;
    blocks = resolve_blocks(path, offblock, blocks);

    if (edition == FILSYS_V1) {
        mkfs_v1(path, blocks, bootfile);
        close(fd);
        return 0;
    }

    const struct mkfs_fmt v7_hook = { isize_v7, seed_v7 };
    const struct mkfs_fmt v6_hook = { isize_v6, seed_v6 };
    const struct mkfs_fmt v8_hook = { isize_v8, seed_v8 };
    const struct mkfs_fmt bsd211_hook = { isize_v7, seed_bsd211 };
    const struct mkfs_fmt *hook =
        (edition == FILSYS_V6) ? &v6_hook :
        (edition == FILSYS_BSD211) ? &bsd211_hook :
        (edition == FILSYS_V8 || edition == FILSYS_V9 || edition == FILSYS_V10) ? &v8_hook :
        &v7_hook;
    mkfs_common(&fs, hook, path, blocks, bootfile);

    close(fd);
    return 0;
}
