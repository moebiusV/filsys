/* Copyright (C) 2026 David Walther */
/* SPDX-License-Identifier: ISC */
/* v7fs.h - Seventh Edition (V7) Unix filesystem, on-disk access layer.
 *
 * The V7 filesystem lives on a PDP-11 disk image.  Block size is 512 bytes:
 *
 *     block 0             boot block
 *     block 1             superblock (struct filsys)
 *     blocks 2..s_isize-1 i-list (s_isize-2 blocks, 8 inodes each)
 *     blocks s_isize..    data blocks
 *
 * Byte order is the PDP-11's "middle-endian" convention:
 *   - 16-bit quantities are little-endian (single word).
 *   - 32-bit quantities (daddr_t / off_t / time_t) are stored as two words,
 *     most-significant word first, each word little-endian.
 *   - the 3-byte block numbers packed into di_addr are the low three bytes of
 *     that middle-endian 32-bit layout: [ hi, lo, mid ].
 *   - indirect blocks store full 4-byte middle-endian daddr_t entries.
 */
#ifndef V7FS_H
#define V7FS_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <errno.h>

#include "filsys.h"
#include "byteorder.h"
#include "check.h"
#include "filsys_common.h"   /* engine types: filsys_io_t, filsys_probe_t, descriptor, state */

/* Map a CPU architecture name ("vax", "3b2", "68k", "pdp11", ...) to the byte
 * order it stored multi-byte fields in, or NULL for an unknown arch.  The arch
 * (not the edition) selects the byte order: System V ran on both endiannesses. */
const byte_order_ops_t *filsys_arch_bo(const char *arch);

/* The type a to_disk_mode caller is encoding (create / mkdir / mknod / symlink). */
enum {
    FILSYS_FT_REG = 0,
    FILSYS_FT_DIR,
    FILSYS_FT_CHR,
    FILSYS_FT_BLK,
    FILSYS_FT_LNK
};

enum {
    V7_BSIZE    = 512,          /* default / legacy block size */
    V7_INOPB    = 8,            /* inodes per block (at V7_BSIZE) */
    V7_INODESZ  = 64,           /* sizeof(struct dinode) */
    V7_NICFREE  = 50,          /* superblock free-block cache size (V7/32V) */
    V7_COH_NICFREE = 64,       /* Coherent free-block cache size */
    V7_XEN_NICFREE = 100,      /* Xenix free-block cache size */
    V7_XEN_MAGIC   = 0x2b5544, /* Xenix superblock magic (offset 1016) */
    V7_COH_MAXINTN  = 255,     /* Coherent interleave bound (fsck MAXINTN) */
    V7_ROOTINO  = 2,
    V7_BADFIN   = 1,           /* the bad-block inode (records bad i-list blocks) */
    V7_SUPERB   = 1,           /* block number of superblock */
    V7_NDADDR   = 10,          /* direct addresses per inode */
    V7_NIADDR   = 13,          /* total address slots per inode */
    V7_NINDIR   = V7_BSIZE / 4, /* 4-byte addresses per indirect block */
    V7_DIRSIZ   = 14,          /* chars per directory entry name */
    V7_DIRENTSZ = 2 + V7_DIRSIZ /* bytes per on-disk directory entry */
};

/* di_mode type/mode bits (sys/ino.h) */
enum {
    V7_IFMT   = 0170000,
    V7_IFCHR  = 0020000,
    V7_IFDIR  = 0040000,
    V7_IFBLK  = 0060000,
    V7_IFREG  = 0100000,
    V7_IFMPC  = 0030000,
    V7_IFMPB  = 0070000,
    V7_IFIFO  = 0010000,   /* Coherent: pipe inode (V7 has no such type) */
    V7_ISUID  = 0004000,
    V7_ISGID  = 0002000,
    V7_ISVTX  = 0001000,
    V7_IREAD  = 0000400,
    V7_IWRITE = 0000200,
    V7_IEXEC  = 0000100
};

/* Byte order lives in bo.h (bo_get16le/bo_get32me/...); v7fs reads and writes
 * through fs->bo (a byte_order_ops_t chosen by the format descriptor). */

/* System V s5fs: s_magic / s_type sit in the last 8 bytes of the 512-byte
 * superblock (offsets 504 and 508).  s_type names the block size.  The magic
 * lives in its own enum so a 32-bit value that overflows int does not widen the
 * type of the small V7 constants above (a C enum shares one underlying type). */
enum {
    V7_SYSV_MAGIC    = 0xfd187e20,
    V7_SYSV_MAGIC_OFF = 504,
    V7_SYSV_TYPE_OFF  = 508,
    V7_SYSV_STATE_OFF = 500,          /* s_state (R4; s_fill[12] in R2/R3) */
    V7_SYSV_STATE_CLEAN = 0x7c269d38, /* FsOKAY: cleanly unmounted */
    V7_SYSV_STATE_ACTIVE = 0x5e72d81a,/* FsACTIVE: mounted (dirty) */
    V7_SYSV_Fs1b      = 1,   /* 512-byte blocks */
    V7_SYSV_Fs2b      = 2,   /* 1024-byte blocks */
    V7_SYSV_Fs4b      = 3    /* 2048-byte blocks */
};

/* Superblock field offsets.  32V (VAX) aligns daddr_t/time_t to 4 bytes, so
 * the fields after such a type sit 2 bytes later than in V7; Coherent and
 * Xenix keep the 2-byte packing.  pack4 and nicfree come from the descriptor. */
static inline int sb_fsize_off(int pack4)  { return pack4 ? 4 : 2; }
static inline int sb_nfree_off(int pack4)  { return pack4 ? 8 : 6; }
static inline int sb_free_off(int pack4)   { return pack4 ? 12 : 8; }
static inline int sb_ninode_off(int pack4, int nicfree) { return sb_free_off(pack4) + 4 * nicfree; }
static inline int sb_inode_off(int pack4, int nicfree)  { return sb_ninode_off(pack4, nicfree) + 2; }
static inline int sb_time_off(int pack4, int nicfree)   { return sb_inode_off(pack4, nicfree) + 2*V7_NICINOD + 4 + (pack4 ? 2 : 0); }
/* s_tfree sits right after s_time in V7/32V/Xenix, but System III and System V
 * insert s_dinfo[4] (8 bytes of device info) first, so their totals are 8 bytes
 * later.  has_dinfo marks those editions (System III's PDP-11 form is packed,
 * so this shift must not be tied to dyn_bsize or pack4). */
static inline int sb_tfree_off(int pack4, int nicfree, int has_dinfo) {
    return sb_time_off(pack4, nicfree) + 4 + (has_dinfo ? 8 : 0);
}
static inline int fb_free_off(int pack4)   { return pack4 ? 4 : 2; }

/* ---- V6 on-disk constants (the V6 backend runs on this engine) ---------- */

/* V6 keeps V7's free-list superblock but with 16-bit block numbers, s_isize as
 * the *number* of i-list blocks, a 32-byte inode, and the ILARG large-file
 * layout.  These are the raw on-disk numbers plus the isize-semantics helpers,
 * shared by the V6 code in v7fs.c, the descriptor row, and mkfs. */
enum {
    V6_BSIZE   = 512,
    V6_INOPB   = 16,           /* inodes per block */
    V6_INODESZ = 32,           /* sizeof(struct inode on disk) */
    V6_NICFREE = 100,          /* superblock free-block cache size */
    V6_NICINOD = 100,          /* superblock free-inode cache size */
    V6_ROOTINO = 1,
    V6_SUPERB  = 1,            /* block number of superblock */
    V6_NIADDR  = 8,            /* total address slots per inode */
    V6_NDADDR  = 8,            /* direct blocks in a small file */
    V6_NINDIR  = V6_BSIZE / 2, /* 2-byte addresses per indirect block */
    V6_DIRSIZ  = 14            /* chars per directory entry name */
};

/* i_mode type/mode bits.  V6 has no IFREG (regular = type 0) and no
 * IFMPC/IFMPB; bit 010000 is the ILARG large-file flag. */
enum {
    V6_IALLOC = 0100000,   /* allocated bit (set in every live inode) */
    V6_IFMT   = 0060000,
    V6_IFCHR  = 0020000,
    V6_IFDIR  = 0040000,
    V6_IFBLK  = 0060000,
    V6_ILARG  = 0010000,
    V6_ISUID  = 0004000,
    V6_ISGID  = 0002000,
    V6_ISVTX  = 0001000,
    V6_IREAD  = 0000400,
    V6_IWRITE = 0000200,
    V6_IEXEC  = 0000100
};

/* V6 stores the NUMBER of i-list blocks in s_isize (unlike V7, which stores
 * the first data block).  The i-list occupies blocks 2..s_isize+1, so the
 * first data block is s_isize+2 and the inode count is s_isize*16. */
static inline uint32_t v6_data_start(uint32_t isize) { return isize + 2; }
static inline uint32_t v6_maxino(uint32_t isize) { return (uint32_t)isize * V6_INOPB; }

/* itod / itoo: inode number -> block and offset (16 inodes per block). */
static inline uint32_t v6_itod(uint32_t ino) { return (ino + 31) >> 4; }
static inline uint32_t v6_itoo(uint32_t ino) { return (ino + 31) & 15; }

/* ---- 2.11BSD on-disk constants (see docs/2bsd-format.md) ---------------- */

/* 2.11BSD keeps V7's free-list superblock but swaps in the "new" BSD inode
 * (32-bit block addresses, 4 direct + 3 indirect, di_flags, symlinks) and
 * variable-length directory entries.  Raw on-disk numbers, shared by the
 * descriptor row and mkfs's self-contained 2.11BSD path. */
enum {
    BSD211_BSIZE      = 1024,
    BSD211_INODESZ    = 64,     /* sizeof(struct dinode) */
    BSD211_INOPB      = 16,     /* inodes per block (1024/64) */
    BSD211_NICFREE    = 50,     /* superblock free-block cache size */
    BSD211_NICINOD    = 100,    /* superblock free-inode cache size */
    BSD211_ROOTINO    = 2,
    BSD211_LOSTFOUNDINO = 3,
    BSD211_SUPERB     = 1,      /* block number of superblock */
    BSD211_NDADDR     = 4,      /* direct addresses per inode */
    BSD211_NIADDR     = 7,      /* total address slots (4 direct + 3 indirect) */
    BSD211_NINDIR     = 256,    /* 4-byte addresses per indirect block (1024/4) */
    BSD211_MAXNAMLEN  = 63,
    BSD211_DIRBLKSIZ  = 512,    /* directory framing (still 512 inside 1024 blocks) */
    BSD211_DIRHDRSZ   = 6,      /* d_ino + d_reclen + d_namlen */
    BSD211_DIRMINSZ   = 8,      /* dirsiz(0): the smallest legal record */
};

/* Rounded record length: the 6-byte header plus the name plus its NUL, padded
 * to a 4-byte multiple (2.11BSD dirents are 4-byte aligned).  Shared by the
 * directory codec (dir_bsd211.c) and mkfs's root seeding, which must agree on
 * the encoding or one will mis-parse the other's records. */
static inline uint32_t bsd211_dirsiz(uint16_t namlen) {
    return (7u + namlen + 3u) & ~3u;
}

/* di_mode type/mode bits.  Note IFLNK (symlink) and IFSOCK. */
enum {
    BSD211_IFMT   = 0170000,
    BSD211_IFCHR  = 0020000,
    BSD211_IFDIR  = 0040000,
    BSD211_IFBLK  = 0060000,
    BSD211_IFREG  = 0100000,
    BSD211_IFLNK  = 0120000,   /* symbolic link */
    BSD211_IFSOCK = 0140000,   /* socket */
    BSD211_ISUID  = 0004000,
    BSD211_ISGID  = 0002000,
    BSD211_ISVTX  = 0001000,
    BSD211_IREAD  = 0000400,
    BSD211_IWRITE = 0000200,
    BSD211_IEXEC  = 0000100
};

/* Superblock field offsets (struct fs, packed, block 1). */
enum {
    BSD211_SB_ISIZE   = 0,     /* u16: first block after i-list */
    BSD211_SB_FSIZE   = 2,     /* u32: size of entire volume in blocks */
    BSD211_SB_NFREE   = 6,     /* u16 */
    BSD211_SB_FREE    = 8,     /* u32 x NICFREE */
    BSD211_SB_NINODE  = 208,   /* u16 */
    BSD211_SB_INODE   = 210,   /* u16 x NICINOD */
    BSD211_SB_FMOD    = 411,   /* u8: superblock modified flag */
    BSD211_SB_TIME    = 414,   /* u32 */
    BSD211_SB_TFREE   = 418,   /* u32: total free blocks */
    BSD211_SB_TINODE  = 422,   /* u16: total free inodes */
    BSD211_SB_STEP    = 424,   /* u16: interleave m */
    BSD211_SB_CYL     = 426,   /* u16: interleave n */
};

/* ---- V8-family (Eighth/Ninth/Tenth Edition) on-disk constants -------------- */
/* See docs/impl-v10fs.md §5.  The inode (64 bytes, 13 three-byte addresses) and
 * the 16-byte directory entry are byte-identical to V7's, so the V7 engine's
 * inode/dir/bmap code is reused unchanged; only the superblock is rearranged.
 * V7 puts the free-list cache (s_nfree/s_free) right after s_fsize and
 * s_fname/s_fpack at the tail; the V8 family moves the free-list/bitmap into a
 * union at the *end* and adds s_fsmnt/s_lasti/s_nbehind. */
enum {
    V8_SB_ISIZE    = 0,     /* u16: first data block number (not a count) */
    V8_SB_FSIZE    = 4,     /* u32: total blocks (4-byte aligned) */
    V8_SB_NINODE   = 8,     /* u16: valid entries in s_inode */
    V8_SB_INODE    = 10,    /* u16 x NICINOD */
    V8_SB_FMOD     = 212,   /* u8: superblock-modified flag */
    V8_SB_TIME     = 216,   /* u32 */
    V8_SB_TFREE    = 220,   /* u32: total free blocks */
    V8_SB_TINODE   = 224,   /* u16: total free inodes */
    V8_SB_DINFO    = 226,   /* s_m @226, s_n @228 (V7 interleave / V10 cylsize+aspace) */
    V8_SB_FSMNT    = 230,   /* 14-byte mount name */
    V8_SB_LASTI    = 244,   /* u16: inode allocation rotor */
    V8_SB_NBEHIND  = 246,   /* u16: est. free inodes below s_lasti */
    V8_SB_UNION    = 248,   /* free-list or bitmap (below) */
    /* free-list union arm (§5.9a): s_nfree is a 2-byte short here */
    V8_SB_NFREE    = 248,
    V8_SB_FREE     = 252,   /* u32 x NICFREE */
    /* in-superblock bitmap arm (§5.9b) */
    V8_SB_VALID    = 248,   /* u8: S_valid (1 on disk = valid) */
    V8_SB_FLAG     = 249,   /* u8: S_flag (V10 only; padding in V8/V9) */
    V8_SB_BFREE    = 252,   /* u32 x V8_BITMAP */
    /* out-of-superblock bitmap arm (§5.9c, v10 only) */
    V8_SB_BSIZE    = 252,   /* u32: bits per bitmap block */
    /* free-space forms (freemap descriptor field; = the public FILSYS_FREEMAP_*) */
    V8_FREEMAP_LIST   = FILSYS_FREEMAP_LIST,
    V8_FREEMAP_BITMAP = FILSYS_FREEMAP_BITMAP,
    V8_FREEMAP_BIGMAP = FILSYS_FREEMAP_BIGMAP,
    /* geometry constants */
    V8_NICFREE_SMALL = 178, /* free-list cache depth (1K/4K blocks) */
    V8_BITMAP        = 961, /* in-superblock bitmap longwords */
    V8_BITCELL       = 32,  /* bits per bitmap longword */
    V8_BITMAP_BITS   = V8_BITMAP * V8_BITCELL, /* 30752: data blocks the
                           in-superblock bitmap can describe (the bigmap
                           threshold) */
    V8_IFLNK         = 0120000, /* symbolic link (present in V8) */
    /* V9/V10 concurrency bits: ICONC overlaps the sticky bit, ICCTYP overlaps
     * setuid/setgid (§5.7).  V8 has ISVTX (V7_ISVTX) instead. */
    V8_ICONC         = 0001000,
    V8_ICCTYP        = 0007000,
    V8_ISYNC         = 0001000,
    V8_IEXCL         = 0003000,
};

/* Check that [off, off+n) lies within the opened image (fs->imgsize).  Returns 0,
 * or -EINVAL if the range runs past the end of the image (the subtraction form
 * cannot overflow).  Every block access funnels through this, so a corrupt block
 * number read out of a superblock or inode cannot read or write past the image.
 * imgsize == 0 means the size is not yet known (mkfs/detect), and the check passes. */
static inline int filsys_check_range(const filsys_edition_t *fs, uint64_t off, uint64_t n) {
    if (fs->imgsize == 0)
        return 0;
    if (off > fs->imgsize || n > fs->imgsize - off)
        return -EINVAL;
    return 0;
}

/* The descriptor for an edition: a copy of the V7 default with the edition's
 * overrides.  Returns a zeroed descriptor (ops == NULL) for an unknown
 * edition. */
filsys_desc_t filsys_getformat(int edition);

/* Resolve a magic-bearing edition's byte order from its on-disk magic word
 * (and, if given, check it against `arch`).  Sets fmt->bo; returns 0 or a
 * negative errno with *errmsg (if non-NULL) set to a static description.
 * `force` proceeds despite a magic that matches neither byte order. */
int filsys_resolve_byteorder(filsys_desc_t *fmt, const char *path,
                             uint64_t offset, const char *arch, int force,
                             const char **errmsg);

/* ---- lifecycle --------------------------------------------------------- */

/* Open a disk image.  Returns 0, or -errno.  proto is the format descriptor
 * returned by filsys_getformat() (byte order, block size, free-cache width,
 * superblock packing); v7fs_open copies it into *fs and fills the runtime
 * fields.  offset is the byte offset of the filesystem within the file (0 =
 * block 0 of the file). */
int v7fs_open(filsys_edition_t *fs, const char *path, int readonly,
              const filsys_desc_t *proto, uint64_t offset);
/* Flush the superblock and close. */
int v7fs_close(filsys_edition_t *fs);
/* Flush the superblock (and pending metadata) to the image without closing. */
int v7fs_sync(filsys_edition_t *fs);
/* Mark the filesystem dirty (s_fmod) and flush: a read-write mount is dirty
 * until a clean close clears it, so a crash leaves the image flagged for fsck. */
int v7fs_mark_dirty(filsys_edition_t *fs);
/* Write the in-core superblock (and free list) back to the image, preserving the
 * fields we don't maintain (s_tfree/s_tinode/s_fname/...).  Shared by the
 * allocator files (alloc_freelist.c, alloc_v8bitmap.c). */
int v7fs_super_write(filsys_edition_t *fs);

/* V8-family superblock codec (Eighth/Ninth/Tenth Edition, docs/impl-v10fs.md
 * §5.8).  The on-disk superblock is rearranged relative to V7's, so these
 * decode/encode it into/from the shared in-core fields.  Wired into the
 * descriptor's sb_decode/sb_encode slots; v8_sb_decode returns 0, or -EINVAL if
 * the free-list/inode cache counts exceed their depths (mis-decoded block 1). */
int v8_sb_decode(filsys_edition_t *fs, const uint8_t *sb);
int v8_sb_encode(filsys_edition_t *fs, uint8_t *sb);
/* Load the V8-family free-space bitmap into fs->v8_bits (bit i set = free). */
int v8_bitmap_load(filsys_edition_t *fs, const uint8_t *sb);

/* Apply the V8-family `-o` geometry overrides (blocksize/freemap/byteorder) to a
 * descriptor, enforcing the one-bit-selects-both rule the kernels honour.  ed is
 * the FILSYS_* selector.  Returns 0, or -EINVAL with *errmsg set. */
int filsys_apply_geom(filsys_desc_t *fmt, int ed, const filsys_geom_t *g,
                      const char **errmsg);

/* ---- block / inode io -------------------------------------------------- */

int v7fs_read_block(filsys_edition_t *fs, uint32_t bno, uint8_t *buf);
int v7fs_write_block(filsys_edition_t *fs, uint32_t bno, const uint8_t *buf);

/* Block-size-dependent quantities: the logical block size is fs->bsize, not the
 * compile-time V7_BSIZE.  The per-indirect-block entry count lives in
 * fs->nindir (it is not always bsize/daddr_wid: PDP-7's container block is 256
 * bytes of 4-byte words against a 128-byte logical block). */
static inline uint32_t v7_nindir(const filsys_edition_t *fs) { return fs->desc.nindir; }
static inline uint32_t v7_inopb(const filsys_edition_t *fs)  { return fs->desc.bsize / fs->desc.inode_size; }
/* itod / itoo: inode number -> block and offset. */
static inline uint32_t v7_itod(const filsys_edition_t *fs, uint32_t ino) { return 2 + (ino - 1) / v7_inopb(fs); }
static inline uint32_t v7_itoo(const filsys_edition_t *fs, uint32_t ino) { return (ino - 1) % v7_inopb(fs); }

/* First data block and last inode: V6's s_isize counts i-list blocks (data
 * starts at isize+2, max inode = isize * inodes/block); V7's s_isize is itself
 * the first data block (max inode = (isize-2) * inodes/block). */
static inline uint32_t v7_data_first(const filsys_edition_t *fs) {
    return fs->isize + (fs->desc.isize_count ? 2u : 0u);
}
static inline uint32_t v7_maxinode(const filsys_edition_t *fs) {
    return (uint32_t)(fs->isize - (fs->desc.isize_count ? 0u : 2u)) * v7_inopb(fs);
}

/* An on-disk block address in a free-list or indirect block: 2-byte LE for V6,
 * 4-byte in the descriptor's byte order otherwise. */
static inline uint32_t v7_get_daddr(const filsys_edition_t *fs, const uint8_t *p) {
    return fs->desc.daddr_wid == 2 ? (uint32_t)bo_get16le(p) : fs->desc.bo->get32(p);
}
static inline void v7_put_daddr(const filsys_edition_t *fs, uint8_t *p, uint32_t v) {
    if (fs->desc.daddr_wid == 2)
        bo_put16le(p, v & 0xFFFFu);
    else
        fs->desc.bo->put32(p, v);
}

/* The offset of df_free[] within a free-list *chain* block: after df_nfree
 * (4 bytes in the V8-family, 2 elsewhere).  V7/32V's 2-byte df_nfree is padded
 * to 4 on 32V, which fb_free_off(pack4) encodes. */
static inline int v7_chain_free_off(const filsys_edition_t *fs) {
    return fs->desc.df_nfree_wid == 4 ? 4 : fb_free_off(fs->desc.pack4);
}

/* The default indirect-entry codec: 2-byte LE (V6/V1) or 4-byte in the
 * descriptor's byte order (V7/2.11BSD), indexed by entry.  PDP-7 overrides the
 * descriptor's ind_get/ind_put with its 18-bit-word codec. */
static inline uint32_t v7_ind_get(const filsys_edition_t *fs, const uint8_t *buf, uint32_t i) {
    return fs->desc.daddr_wid == 2 ? (uint32_t)bo_get16le(buf + 2 * i) : fs->desc.bo->get32(buf + 4 * i);
}
static inline void v7_ind_put(const filsys_edition_t *fs, uint8_t *buf, uint32_t i, uint32_t v) {
    if (fs->desc.daddr_wid == 2)
        bo_put16le(buf + 2 * i, v & 0xFFFFu);
    else
        fs->desc.bo->put32(buf + 4 * i, v);
}

/* The level of address slot `slot` for an inode whose mode is `mode`:
 * -1 = direct, 0 = single indirect, 1 = double, 2 = triple.  The V7-family
 * layout is "ndaddr direct slots then levels 0,1,2 in order"; the V6/V1/PDP-7
 * layouts reinterpret the slots under the ILARG mode bit (large_single + one
 * optional large_double slot), all of which this describes as data. */
static inline int filsys_slot_level(const filsys_edition_t *fs, uint32_t mode, int slot) {
    if (fs->desc.ilarg_mask && (mode & fs->desc.ilarg_mask)) {
        if (slot < (int)fs->desc.large_single)
            return 0;
        if (slot < (int)(fs->desc.large_single + fs->desc.large_double))
            return 1;
        return -1;   /* no slot beyond the large layout */
    }
    if (slot < (int)fs->desc.ndaddr)
        return -1;
    return slot - (int)fs->desc.ndaddr;   /* 0, 1, 2, ... */
}

/* Directory test: V1/PDP-7 carry an is_dir callback (their type bits don't fit
 * the V6/V7 ifmt/ifdir pair); the V6/V7 family derives it from ifmt. */
static inline int fs_is_dir(const filsys_edition_t *fs, const filsys_inode_t *ip) {
    if (fs->desc.is_dir)
        return fs->desc.is_dir(fs, ip);
    return (ip->mode & fs->desc.ifmt) == fs->desc.ifdir;
}

int v7fs_read_inode(filsys_edition_t *fs, uint32_t ino, v7_inode_t *ip);
int v7fs_write_inode(filsys_edition_t *fs, uint32_t ino, const v7_inode_t *ip);

/* ---- allocation -------------------------------------------------------- */

/* Allocate a free data block into *bno (0 = absent/sparse). */
int v7fs_balloc(filsys_edition_t *fs, uint32_t *bno);
void v7fs_bfree(filsys_edition_t *fs, uint32_t bno);
/* Allocate a free inode into *ino. */
int v7fs_ialloc(filsys_edition_t *fs, uint32_t *ino);
void v7fs_ifree(filsys_edition_t *fs, uint32_t ino);

/* V8-family bitmap allocator (alloc_v8bitmap.c): the block side of V8/V9/V10
 * free space, behind v8_bitmap_alloc_ops. */
int  v8_bitmap_balloc(filsys_edition_t *fs, uint32_t *bno);
void v8_bitmap_bfree(filsys_edition_t *fs, uint32_t bno);
int  v8_bitmap_sync(filsys_edition_t *fs);

/* ---- file / directory data --------------------------------------------- */

ssize_t filsys_file_read(filsys_edition_t *fs, v7_inode_t *ip, uint8_t *buf, size_t size, off_t off);
ssize_t filsys_file_write(filsys_edition_t *fs, v7_inode_t *ip, const uint8_t *buf, size_t size, off_t off);

/* Offset-resumable iteration over a directory (see filsys_ops.h). */
int v7fs_dir_iter(filsys_iter_state_t *st, uint32_t *ino, const char **name,
                  uint16_t *namlen, uint64_t *next_off);

/* Look up name in a directory; returns 0 and *ino, or -ENOENT. */
int v7fs_dir_lookup(filsys_edition_t *fs, v7_inode_t *ip, const char *name, uint32_t *ino);
/* Add an entry (name must be <= V7_DIRSIZ, no '/'); 0 or -errno. */
int v7fs_dir_add(filsys_edition_t *fs, v7_inode_t *ip, uint32_t ino, const char *name);
/* Remove an entry; 0 or -errno. */
int v7fs_dir_remove(filsys_edition_t *fs, v7_inode_t *ip, const char *name);

/* 2.11BSD variable-length directory entries (dir_bsd211.c). */
int bsd211_dir_iter(filsys_iter_state_t *st, uint32_t *ino, const char **name,
                    uint16_t *namlen, uint64_t *next_off);
int bsd211_dir_add(filsys_edition_t *fs, v7_inode_t *ip, uint32_t ino, const char *name);
int bsd211_dir_remove(filsys_edition_t *fs, v7_inode_t *ip, const char *name);
int bsd211_dir_lookup(filsys_edition_t *fs, v7_inode_t *ip, const char *name, uint32_t *ino);

/* ---- integrity check ---------------------------------------------------- */

typedef filsys_check_t v7_check_t;

/* Run the V7 equivalent of icheck(8) + dcheck(8): walk the inode table marking
 * every referenced block, walk the free list, detect duplicates and missing
 * blocks, and check directory link counts.  If mode has FILSYS_CK_SALVAGE set, rebuild the
 * free list from the block-usage map (icheck -s) instead of checking it; the
 * filesystem must have been opened read-write.  Reports to stdout; returns 0
 * if no errors were found, -1 otherwise. */
int v7fs_check(filsys_edition_t *fs, v7_check_t *rep, int mode);
/* 2.11BSD's check: icheck+dcheck without the V7 repair modes (mode is ignored).
 * The on-disk layout is the V7 engine's, so it shares filsys_edition_t. */
int bsd211_check(filsys_edition_t *fs, v7_check_t *rep, int mode);
/* V6's check: its isize semantics (s_isize = i-list block count), ILARG bmap
 * and 32-byte inode make the codec V6-specific, but it operates on the shared
 * filsys_edition_t. */
int v6_check(filsys_edition_t *fs, v7_check_t *rep, int mode);

/* Free-list check helpers (alloc_freelist.c): rebuild / walk / count. */
uint32_t v7fs_makefree(filsys_edition_t *fs, filsys_chkctx_t *cx);
void v7_walk_free(filsys_edition_t *fs, filsys_chkctx_t *cx, filsys_check_t *rep);
void v7_count_free(filsys_edition_t *fs, uint32_t *nblk, uint32_t *nino);
void v6_count_free(filsys_edition_t *fs, uint32_t *nblk, uint32_t *nino);
/* V6 inode codec (32-byte inodes, ILARG layout); v6_write_inode stays private
 * to v7fs.c's ops table, but the count-free scan needs v6_read_inode. */
int v6_read_inode(filsys_edition_t *fs, uint32_t ino, v7_inode_t *ip);
/* V8-family bitmap check helpers (alloc_v8bitmap.c). */
uint32_t v8_makefree_bitmap(filsys_edition_t *fs, filsys_chkctx_t *cx);
void v8_walk_free_bitmap(filsys_edition_t *fs, filsys_chkctx_t *cx, filsys_check_t *rep);

/* ---- shared block-map walk + truncate (blocktree.c) ---------------------- */

/* Mark one data block as accounted-for (0 = first, 1 = bad, 2 = duplicate). */
int filsys_mark_block(filsys_edition_t *fs, filsys_chkctx_t *cx, uint32_t bno);
/* Mark an indirect block and everything beneath it (level 0 = single, 1 =
 * double, 2 = triple); a duplicate indirect block is not chased. */
void filsys_mark_tree(filsys_edition_t *fs, filsys_chkctx_t *cx, uint32_t blk, int level);
/* Mark every block referenced by an inode. */
void filsys_mark_blocks(filsys_edition_t *fs, const filsys_inode_t *ip, uint32_t ino,
                        filsys_chkctx_t *cx);

/* A deferred block-free list: filsys_itrunc/_from collect the blocks they
 * remove into one of these instead of freeing inline, so the caller can persist
 * the unlinked inode (write_inode) *before* any block returns to the free list. */
typedef struct filsys_blklist filsys_blklist_t;
filsys_blklist_t *filsys_blklist_new(void);
void filsys_blklist_drain(filsys_edition_t *fs, filsys_blklist_t *b);   /* bfree all + free list */
void filsys_blklist_release(filsys_blklist_t *b);                        /* mark blocks unowned (instrumentation) */
void filsys_blklist_discard(filsys_blklist_t *b);                        /* free list, keep blocks */

/* Collect every block of an inode (truncate to length 0) into *b. */
int filsys_itrunc(filsys_edition_t *fs, filsys_inode_t *ip, filsys_blklist_t *b);
/* Collect blocks [first_blk, ...) only; first_blk == 0 == filsys_itrunc. */
int filsys_itrunc_from(filsys_edition_t *fs, filsys_inode_t *ip, uint32_t first_blk,
                       filsys_blklist_t *b);

/* Map a logical block number to a physical block, allocating on create.  The
 * one bmap for every edition: the slot topology comes from filsys_slot_level,
 * the indirect-entry width from ind_get/ind_put, and allocation from
 * fs->alloc->balloc. */
int filsys_bmap(filsys_edition_t *fs, filsys_inode_t *ip, uint32_t lbn, int create,
                uint32_t *bno);

/* Count the allocated data + indirect blocks of an inode (the shared
 * allocated_blocks op, for st_blocks): direct slots count 1 each, indirect slots
 * their whole subtree via ind_get; a device inode counts 0.  Returns 0/-errno. */
int filsys_allocated_blocks(filsys_edition_t *fs, const filsys_inode_t *ip,
                            uint64_t *out);

/* ---- shared maintenance (check.c): ncheck / clri / preen / resolve-dups -- */

int filsys_ncheck(filsys_edition_t *fs, uint32_t ino);
int filsys_clri(filsys_edition_t *fs, uint32_t ino);
void filsys_preen(filsys_edition_t *fs, const uint8_t *ecount, const uint8_t *state,
                  uint32_t maxino, int mode);
int filsys_resolve_dups(filsys_edition_t *fs);

#endif /* V7FS_H */
