/* filsys_mkfs.c - image creation (mkfs) folded into the library.
 *
 * The mkfs driver used to live in mkfs.filsys.c, half on the library's ops
 * vtable and half on raw pread/pwrite against a file descriptor.  It is folded
 * here so image creation is one library call that writes through the same
 * fault-injectable byte-slice transport (filsys_io_t) as every other metadata
 * write, and so the crash-prefix enumerator can cover it.
 *
 * `filsys_mkfs` is internal (filsys_ops.h): the io type is not part of the
 * public filsys.h surface.  mkfs.filsys.c reduces to argument parsing plus a
 * call; the free-list-cache editions (V7/32V/Coherent/Xenix/BSD/SysV/V6/V8..V10)
 * share one driver, while the genuinely different layouts -- V1's dual-bitmap
 * superblock and the word-addressed PDP-7 -- keep their own implementations
 * behind the same entry point.
 *
 * SPDX-License-Identifier: ISC */
#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <stdarg.h>

#include "filsys.h"
#include "filsys_ops.h"
#include "v1fs.h"
#include "v7fs.h"
#include "pdp7fs.h"

static char mkfs_errbuf[256];

static int fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(mkfs_errbuf, sizeof mkfs_errbuf, fmt, ap);
    va_end(ap);
    return -1;
}

/* Write one logical block through the transport.  Return 0, or -1 on a short
 * write. */
static int pblock(filsys_edition_t *fs, uint32_t bno, const uint8_t *buf)
{
    if (fs->io->write(fs, buf, fs->bsize, (off_t)bno * fs->bsize + (off_t)fs->base))
        return fail("write error at block %u\n", bno);
    return 0;
}

/* ---- shared mkfs driver (free-list-cache editions: V7/32V/Coherent/Xenix/
 *      2.9BSD/V6/2.11BSD/V8..V10) ------------------------------------------- */

/* One hook per format: the parts that genuinely differ are the i-list sizing
 * and the seed (reserved + root) inodes/directories.  Everything else -- the
 * allocator, the inode codec, the free-list build with its per-edition
 * interleave, and the superblock layout -- comes from the library's ops/alloc
 * vtable. */
struct mkfs_fmt {
    uint32_t (*isize)(uint32_t blocks, uint32_t ipb);  /* -> fs->isize */
    uint32_t (*seed)(filsys_edition_t *fs);            /* returns used inodes */
};

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
    fs->ops->inode->write_inode(fs, 1, &ip);

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
    fs->ops->inode->write_inode(fs, fs->rootino, &ip);
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
    fs->ops->inode->write_inode(fs, fs->rootino, &ip);
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
    fs->ops->inode->write_inode(fs, V6_ROOTINO, &ip);
    return 1;
}

static uint32_t bsd211_dirsiz(uint16_t namlen) { return (7u + namlen + 3u) & ~3u; }

static uint32_t seed_bsd211(filsys_edition_t *fs)
{
    filsys_inode_t ip;
    uint8_t lf[BSD211_BSIZE];

    /* inode 1: reserved (empty regular file), as BSD's mkfs writes */
    memset(&ip, 0, sizeof ip);
    ip.ino = 1;
    ip.mode = BSD211_IFREG;
    fs->ops->inode->write_inode(fs, 1, &ip);

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
    fs->ops->inode->write_inode(fs, BSD211_LOSTFOUNDINO, &ip);

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
    fs->ops->inode->write_inode(fs, BSD211_ROOTINO, &ip);
    return 3;
}

static int mkfs_common(filsys_edition_t *fs, const struct mkfs_fmt *fmt,
                       uint32_t blocks, const void *boot)
{
    uint32_t ipb = fs->bsize / fs->inode_size;
    fs->readonly = 0;
    fs->isize = fmt->isize(blocks, ipb) & 0xFFFFu;   /* s_isize is a 16-bit field */
    fs->fsize = blocks;

    /* The out-of-superblock bitmap's blocks sit at the tail as metadata; work
     * them out up front so data_end (and the checker's data band) exclude them. */
    if (fs->freemap == V8_FREEMAP_BIGMAP) {
        uint32_t bits_per_blk = fs->bsize * 8;
        fs->v8_nblks = (fs->fsize + bits_per_blk - 1) / bits_per_blk;
        fs->v8_blk_start = fs->fsize - fs->v8_nblks;
    }

    if (v7_data_first(fs) >= fs->fsize)
        return fail("%u blocks too small for an i-list of %u blocks\n",
                    blocks, fs->isize);

    /* The in-superblock bitmap holds V8_BITMAP_BITS (30752) data blocks; a
     * larger data area must use the out-of-superblock bigmap (v10 only).  Reject
     * it here rather than write a bitmap the decode side cannot read back. */
    if (fs->freemap == V8_FREEMAP_BITMAP &&
        fs->fsize - fs->isize > V8_BITMAP_BITS)
        return fail("%u-block data area exceeds the %u-block in-superblock bitmap; "
                    "use -g freemap=bigmap (v10) or a smaller volume\n",
                    fs->fsize - fs->isize, (uint32_t)V8_BITMAP_BITS);

    if (boot && pblock(fs, 0, boot))
        return -1;

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
        return fail("out of memory\n");
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
    if (fs->ops->sync(fs))
        return fail("final flush failed\n");

    return 0;
}

/* ---- V1 ------------------------------------------------------------------ */

static uint32_t v1_fsize, v1_maxino, v1_dstart;
static uint8_t  v1_sb[V1_BSIZE * 2];   /* superblock spans blocks 0 and 1 */
static uint32_t v1_imap_off;           /* byte offset of the inode map */

static int v1_balloc(uint32_t *out)
{
    for (uint32_t b = v1_dstart; b < v1_fsize; b++) {
        if (v1_sb[2 + (b >> 3)] & (1u << (b & 7))) {   /* bit=1 free */
            v1_sb[2 + (b >> 3)] &= (uint8_t)~(1u << (b & 7));
            *out = b;
            return 0;
        }
    }
    return fail("out of free space\n");
}

static int v1_iput(filsys_edition_t *fs, uint32_t ino, uint16_t mode, int16_t nlink,
                   uint32_t size, const uint32_t *addr)
{
    uint32_t bno = v1_itod(ino);
    uint32_t off = v1_itoo(ino);
    uint8_t ib[V1_BSIZE];
    if (fs->io->read(fs, ib, V1_BSIZE, (off_t)bno * V1_BSIZE + (off_t)fs->base))
        return fail("read error at inode block %u\n", bno);

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

    return pblock(fs, bno, ib);
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

static int mkfs_v1(filsys_edition_t *fs, uint32_t blocks)
{
    /* V1's free map and inode-map size fields are 16-bit words, so the maps
     * (and the filesystem they describe) are a whole number of words: round the
     * volume up to a multiple of 16 blocks, so fsize = freemap_bytes * 8. */
    v1_fsize = (blocks + 15) & ~15u;
    if (v1_fsize > v1_max_fsize())
        return fail("v1: maximum filesystem size is %u blocks (superblock free map)\n",
                    v1_max_fsize());

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
        return fail("%u blocks too small\n", blocks);

    v1_imap_off = 2 + freemap_bytes + 2;
    if (v1_imap_off + inodemap_bytes > sizeof(v1_sb))
        return fail("superblock overflow\n");

    memset(v1_sb, 0, sizeof(v1_sb));
    bo_me.put16(v1_sb + 0, (uint16_t)freemap_bytes);
    bo_me.put16(v1_sb + 2 + freemap_bytes, (uint16_t)inodemap_bytes);

    /* free map: data blocks are free (bit=1) */
    for (uint32_t b = v1_dstart; b < v1_fsize; b++)
        v1_sb[2 + (b >> 3)] |= (uint8_t)(1u << (b & 7));

    /* inode map: all free (0); root inode 41 is used (bit 0 = 1) */
    v1_sb[v1_imap_off] |= 1u;

    if (pblock(fs, 0, v1_sb) || pblock(fs, 1, v1_sb + V1_BSIZE))
        return -1;

    /* zero the i-list: blocks 2 .. v1_dstart-1 */
    uint8_t zb[V1_BSIZE] = {0};
    for (uint32_t b = 2; b < v1_dstart; b++)
        if (pblock(fs, b, zb))
            return -1;

    /* root directory: inode 41, "." and ".." (10-byte entries) */
    uint32_t rb;
    if (v1_balloc(&rb))
        return -1;
    uint8_t db[V1_BSIZE] = {0};
    bo_me.put16(db + 0, V1_ROOTINO);
    memcpy(db + 2, ".", 1);
    bo_me.put16(db + V1_DIRENTSZ, V1_ROOTINO);
    memcpy(db + V1_DIRENTSZ + 2, "..", 2);
    if (pblock(fs, rb, db))
        return -1;

    uint32_t addr[V1_NIADDR] = {0};
    addr[0] = rb;
    uint16_t mode = V1_IALLOC | V1_IFDIR | V1_IREAD | V1_IWRITE | V1_IEXEC | V1_OREAD;
    if (v1_iput(fs, V1_ROOTINO, mode, 2, 2 * V1_DIRENTSZ, addr))
        return -1;

    /* persist the superblock (the free map changed when the root block was
     * allocated) */
    if (pblock(fs, 0, v1_sb) || pblock(fs, 1, v1_sb + V1_BSIZE))
        return -1;

    fs->fsize = v1_fsize;
    return 0;
}

/* ---- PDP-7 --------------------------------------------------------------- */

static uint32_t p7_pool[P7_NBLOCKS];   /* free data-block pool */
static uint32_t p7_nfree;
static uint32_t p7_free_count;         /* free blocks listed in the free list */
static const word_codec_t *p7_wc;      /* the container codec being written */

static int p7_pblock(filsys_edition_t *fs, uint32_t bno, const uint32_t *words)
{
    uint8_t raw[P7_MAXBLOCKBYTES];
    for (uint32_t i = 0; i < P7_WSIZE; i++)
        p7_wc->put(raw, i, words[i]);
    /* The image is the bare filesystem surface (surface 1), so block 0 of the
     * filesystem is byte 0 of the file -- the same "-o offset=" convention as
     * every other edition. */
    off_t pos = (off_t)bno * (off_t)p7_wc->block_bytes;
    if (fs->io->write(fs, raw, p7_wc->block_bytes, pos + (off_t)fs->base))
        return fail("write error at block %u\n", bno);
    return 0;
}

static int p7_take_free(uint32_t *out)
{
    if (p7_nfree == 0)
        return fail("out of free space\n");
    *out = p7_pool[--p7_nfree];
    return 0;
}

/* Build the free-list chain: each free-list block holds nine free block
 * numbers (words 1..9) and a next pointer (word 0).  The free-list blocks
 * themselves are drawn from the pool, so they are not returned as free. */
static int p7_build_freelist(filsys_edition_t *fs, uint32_t *head)
{
    uint32_t h = 0;
    p7_free_count = 0;
    while (p7_nfree > 0) {
        uint32_t fb;
        if (p7_take_free(&fb))
            return -1;
        p7_free_count++;           /* the node block itself is free */
        uint32_t words[P7_WSIZE] = {0};
        words[0] = h;
        for (int i = 1; i <= 9 && p7_nfree > 0; i++) {
            uint32_t f;
            if (p7_take_free(&f))
                return -1;
            words[i] = f;
            p7_free_count++;
        }
        if (p7_pblock(fs, fb, words))
            return -1;
        h = fb;
    }
    *head = h;
    return 0;
}

static int mkfs_pdp7(filsys_edition_t *fs, const word_codec_t *word)
{
    /* The RB09 fixed-head disk has one valid geometry (8000 blocks/surface);
     * there is no size to choose.  We write the bare filesystem surface (one
     * surface, not the full two-surface disk), so block 0 is byte 0. */
    p7_wc = word;

    /* free data blocks: 712 .. 6399 (the kernel area 6400..7999 is reserved) */
    p7_nfree = 0;
    for (uint32_t b = 712; b <= 6399; b++)
        p7_pool[p7_nfree++] = b;

    /* root directory data block, drawn before the free list is built */
    uint32_t rb;
    if (p7_take_free(&rb))
        return -1;
    uint32_t empty[P7_WSIZE] = {0};
    if (p7_pblock(fs, rb, empty))
        return -1;

    uint32_t head;
    if (p7_build_freelist(fs, &head))
        return -1;

    /* superblock: block 0 word 0 = free-list head */
    uint32_t sb[P7_WSIZE] = {0};
    sb[0] = head;
    if (p7_pblock(fs, 0, sb))
        return -1;

    /* zero the i-list: blocks 2 .. 711 */
    uint32_t z[P7_WSIZE] = {0};
    for (uint32_t b = P7_FIRSTINOBLK; b < P7_FIRSTINOBLK + P7_NINOBLKS; b++)
        if (p7_pblock(fs, b, z))
            return -1;

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
    if (p7_pblock(fs, ino_block, ib))
        return -1;

    fs->fsize = P7_NBLOCKS;
    return 0;
}

/* ---- dispatcher ----------------------------------------------------------- */

int filsys_mkfs(int edition, const filsys_io_t *io, int fd, uint64_t base,
                const filsys_mkfs_opts_t *opts, uint64_t *written,
                const char **errmsg)
{
    mkfs_errbuf[0] = '\0';
    *written = 0;
    if (errmsg)
        *errmsg = NULL;

    filsys_edition_t fs = filsys_getformat(edition);
    fs.io = io;
    fs.fd = fd;
    fs.base = base;
    fs.m = opts->m;
    fs.n = opts->n;

    if (opts->arch) {
        const byte_order_ops_t *bo = filsys_arch_bo(opts->arch);
        if (!bo)
            return fail("bad arch '%s'\n", opts->arch);
        fs.bo = bo;
        if (bo == &bo_me)
            fs.pack4 = 0;   /* PDP-11 packs the s5fs; no 4-byte alignment */
    }
    if (opts->geom) {
        const char *gerr = NULL;
        if (filsys_apply_geom(&fs, edition, opts->geom, &gerr))
            return fail("%s\n", gerr ? gerr : "bad geometry");
    }
    if (opts->bsize) {
        if (!fs.dyn_bsize)
            return fail("block size is System-V-only\n");
        if (opts->bsize != 512 && opts->bsize != 1024 && opts->bsize != 2048)
            return fail("bad block size %u (want 512, 1024 or 2048)\n", opts->bsize);
        fs.bsize = opts->bsize;
        fs.nindir = opts->bsize / fs.daddr_wid;
    }

    int rc;
    if (edition == FILSYS_PDP7) {
        if (opts->blocks != 0)
            return fail("pdp7: size is fixed by the RB09 geometry (8000 blocks/surface)\n");
        if (opts->packing && !(fs.word = filsys_word_codec_by_name(opts->packing)))
            return fail("unknown packing '%s'\n", opts->packing);
        rc = mkfs_pdp7(&fs, fs.word);
        *written = (uint64_t)P7_NBLOCKS * fs.word->block_bytes;
    } else if (edition == FILSYS_V1) {
        rc = mkfs_v1(&fs, opts->blocks);
        *written = (uint64_t)fs.fsize * V1_BSIZE;
    } else {
        const struct mkfs_fmt v7_hook = { isize_v7, seed_v7 };
        const struct mkfs_fmt v6_hook = { isize_v6, seed_v6 };
        const struct mkfs_fmt v8_hook = { isize_v8, seed_v8 };
        const struct mkfs_fmt bsd211_hook = { isize_v7, seed_bsd211 };
        const struct mkfs_fmt *hook =
            (edition == FILSYS_V6) ? &v6_hook :
            (edition == FILSYS_BSD211) ? &bsd211_hook :
            (edition == FILSYS_V8 || edition == FILSYS_V9 || edition == FILSYS_V10) ? &v8_hook :
            &v7_hook;
        rc = mkfs_common(&fs, hook, opts->blocks, opts->boot);
        *written = (uint64_t)fs.fsize * fs.bsize;
    }

    /* mkfs_common builds the free space by reusing the check driver's salvage
     * op, which for the V8-family bitmap editions heap-allocates fs.v8_bits.
     * A freshly made fs has no close path to free it, so release it here. */
    free(fs.v8_bits);

    if (rc)
        *errmsg = mkfs_errbuf;
    return rc;
}
