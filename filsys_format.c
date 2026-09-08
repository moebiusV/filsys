/* filsys_format.c - the per-format descriptor table.
 *
 * Each edition's on-disk constants and structural predicates (mode conversion,
 * device/directory detection) live here as a filsys_edition_t row; filsys.c and
 * the standalone tools look a format up by edition and read its descriptor
 * rather than switching on the edition.  Adding a format is a new row.
 *
 * SPDX-License-Identifier: ISC
 */
#include <config.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "filsys.h"
#include "v7fs.h"
#include "filsys_ops.h"
#include "v1fs.h"
#include "pdp7fs.h"

/* POSIX mode -> V1's on-disk flag word.  V1 has a two-class permission model
 * (owner r/w/x, non-owner r/w) with no group or sticky bit, a single universal
 * exec bit, and regular files carry no type bit (only IALLOC).  Group/other
 * read-write collapse onto the non-owner bits; owner-exec drives the universal
 * exec bit. */
static uint16_t to_v1_mode(mode_t m, int isdir) {
    uint16_t f = V1_IALLOC;
    if (isdir) f |= V1_IFDIR;
    if (m & S_IRUSR) f |= V1_IREAD;
    if (m & S_IWUSR) f |= V1_IWRITE;
    if (m & S_IXUSR) f |= V1_IEXEC;
    if ((m & S_IRGRP) || (m & S_IROTH)) f |= V1_OREAD;
    if ((m & S_IWGRP) || (m & S_IWOTH)) f |= V1_OWRITE;
    if (m & S_ISUID) f |= V1_ISUID;
    return f;
}

/* POSIX mode -> PDP-7's on-disk flag word: I_USED + four permission bits
 * (owner r/w, world r/w; no execute, no group). */
static uint32_t to_p7_mode(mode_t m, int isdir) {
    uint32_t f = P7_IUSED;
    if (isdir) f |= P7_IDIR;
    if (m & S_IRUSR) f |= P7_IOREAD;
    if (m & S_IWUSR) f |= P7_IOWRITE;
    if ((m & S_IRGRP) || (m & S_IROTH)) f |= P7_IWREAD;
    if ((m & S_IWGRP) || (m & S_IWOTH)) f |= P7_IWWRITE;
    return f;
}

/* V1's mode conversion: a two-class permission model, a single universal exec
 * bit, devices identified by inode number (< 41), and regular files carrying
 * no type bit. */
static mode_t v1_to_posix_mode(const filsys_edition_t *f, const filsys_inode_t *ip) {
    (void)f;
    mode_t m = (ip->mode & V1_IFDIR) ? S_IFDIR : S_IFREG;
    if (ip->mode & V1_IREAD)  m |= S_IRUSR;
    if (ip->mode & V1_IWRITE) m |= S_IWUSR;
    if (ip->mode & V1_IEXEC)  m |= S_IXUSR | S_IXGRP | S_IXOTH;
    if (ip->mode & V1_OREAD)  m |= S_IRGRP | S_IROTH;
    if (ip->mode & V1_OWRITE) m |= S_IWGRP | S_IWOTH;
    if (ip->mode & V1_ISUID)  m |= S_ISUID;
    return m;
}
static int v1_is_dir(const filsys_edition_t *f, const filsys_inode_t *ip) {
    (void)f;
    return (ip->mode & V1_IFDIR) != 0;
}
static int v1_is_device(const filsys_edition_t *f, const filsys_inode_t *ip) {
    (void)f;
    return ip->ino < V1_ROOTINO;   /* V1: devices by inode number */
}
static uint32_t v1_to_disk_mode(const filsys_edition_t *f, mode_t m, int type) {
    (void)f;
    return to_v1_mode(m, type == FILSYS_FT_DIR);
}
static uint32_t v1_chmod_mode(const filsys_edition_t *f, uint32_t old, mode_t m) {
    (void)f;
    /* V1 chmod replaces the low six permission bits (preserving IALLOC/IFDIR/
     * ILARG) and, like V1's sys/chmod, clears setuid+exec on directories. */
    uint16_t bits = to_v1_mode(m, 0) & (uint16_t)0077;
    if (old & V1_IFDIR)
        bits &= (uint16_t)~(V1_ISUID | V1_IEXEC);
    return (old & (uint16_t)~0077) | bits;
}

/* PDP-7's mode conversion: four permission bits (owner r/w, world r/w), no
 * execute, devices as I_SPECIAL inodes, directories as I_DIRECTORY. */
static mode_t p7_to_posix_mode(const filsys_edition_t *f, const filsys_inode_t *ip) {
    (void)f;
    mode_t m = (ip->mode & P7_IDIR)  ? S_IFDIR :
               (ip->mode & P7_ISPEC) ? S_IFCHR : S_IFREG;
    if (ip->mode & P7_IOREAD)  m |= S_IRUSR;
    if (ip->mode & P7_IOWRITE) m |= S_IWUSR;
    if (ip->mode & P7_IWREAD)  m |= S_IRGRP | S_IROTH;
    if (ip->mode & P7_IWWRITE) m |= S_IWGRP | S_IWOTH;
    return m;
}
static int p7_is_dir(const filsys_edition_t *f, const filsys_inode_t *ip) {
    (void)f;
    return (ip->mode & P7_IDIR) != 0;
}
static int p7_is_device(const filsys_edition_t *f, const filsys_inode_t *ip) {
    (void)f;
    return (ip->mode & P7_ISPEC) != 0;
}
static uint32_t p7_to_disk_mode(const filsys_edition_t *f, mode_t m, int type) {
    (void)f;
    uint32_t mode = to_p7_mode(m, type == FILSYS_FT_DIR);
    if (type == FILSYS_FT_CHR || type == FILSYS_FT_BLK)
        mode |= P7_ISPEC;
    return mode;
}
static uint32_t p7_chmod_mode(const filsys_edition_t *f, uint32_t old, mode_t m) {
    (void)f;
    uint32_t bits = to_p7_mode(m, 0) & (uint32_t)017;
    return (old & ~(uint32_t)017) | bits;
}

/* V9/V10 mode: bit 01000 is ICONC (concurrency), not ISVTX (sticky).  When
 * clear, 04000/02000 are setuid/setgid as usual; when set, ICCTYP (07000) is
 * the concurrency type and the setuid/setgid bits are NOT reported (docs/
 * impl-v10fs.md §5.7).  V8 keeps V7's ISVTX and uses the shared derivation. */
static mode_t v910_to_posix_mode(const filsys_edition_t *f, const filsys_inode_t *ip) {
    mode_t m = ip->mode & 0777;                    /* permissions */
    if (!(ip->mode & V8_ICONC)) {
        if (ip->mode & 04000) m |= S_ISUID;
        if (ip->mode & 02000) m |= S_ISGID;
    }
    uint32_t t = ip->mode & f->ifmt;
    if (t == f->ifdir)
        m |= S_IFDIR;
    else if (t == f->ifchr || (f->ifmpc && t == f->ifmpc))
        m |= S_IFCHR;
    else if (t == f->ifblk || (f->ifmpb && t == f->ifmpb))
        m |= S_IFBLK;
    else if (f->iflnk && t == f->iflnk)
        m |= S_IFLNK;
    else
        m |= S_IFREG;
    return m;
}

/* ---- format table -------------------------------------------------------- */

/* The V7 format is the prototype for the V7-family variants (32V, Coherent,
 * Xenix, 2.9BSD): each copies it and overrides only what differs, so a new
 * variant is a few field assignments rather than a full descriptor row. */
static const filsys_edition_t v7 = {
    .ops = &v7fs_ops, .alloc = &freelist_alloc_ops,
    .state_size = sizeof(filsys_edition_t), .name = "v7",
    .bsize = V7_BSIZE, .bo = &bo_me,
    .nicfree = V7_NICFREE, .nicinod = V7_NICINOD,
    .inode_size = V7_INODESZ, .ndaddr = V7_NDADDR, .niaddr = V7_NIADDR,
    .addr_width = 3, .daddr_wid = 4, .df_nfree_wid = 2, .nindir = V7_NINDIR, .fmod_back = 2,
    .rootino = V7_ROOTINO, .badino = V7_BADFIN,
    .max_namlen = V7_DIRSIZ, .dirent_size = V7_DIRENTSZ,
    .ifmt = V7_IFMT, .ifdir = V7_IFDIR, .ifreg = V7_IFREG,
    .ifchr = V7_IFCHR, .ifblk = V7_IFBLK, .ifmpc = V7_IFMPC, .ifmpb = V7_IFMPB,
};
static const filsys_edition_t v6 = {
    .ops = &v6fs_ops, .alloc = &freelist_alloc_ops,
    .state_size = sizeof(filsys_edition_t), .name = "v6",
    .bsize = V6_BSIZE, .bo = &bo_me,
    .nicfree = V6_NICFREE, .nicinod = V6_NICINOD,
    .inode_size = V6_INODESZ, .ndaddr = V6_NDADDR, .niaddr = V6_NIADDR,
    .daddr_wid = 2, .df_nfree_wid = 2, .nindir = V6_NINDIR, .isize_count = 1, .rootino = V6_ROOTINO,
    .ilarg_mask = V6_ILARG, .large_single = 7, .large_double = 1,
    .max_namlen = V6_DIRSIZ, .dirent_size = 2 + V6_DIRSIZ,
    .ifmt = V6_IFMT, .ifdir = V6_IFDIR, .ifchr = V6_IFCHR, .ifblk = V6_IFBLK,
};
static const filsys_edition_t v1 = {
    .ops = &v1fs_ops, .alloc = &bitmap_alloc_ops,
    .state_size = sizeof(filsys_edition_t), .name = "v1",
    .bsize = V1_BSIZE, .bo = &bo_me,
    .inode_size = V1_INODESZ, .ndaddr = V1_NDADDR, .niaddr = V1_NIADDR,
    .daddr_wid = 2, .nindir = V1_NINDIR,
    .ilarg_mask = V1_ILARG, .large_single = 8, .large_double = 0,
    .rootino = V1_ROOTINO, .max_namlen = V1_DIRSIZ, .dirent_size = V1_DIRENTSZ,
    .to_posix_mode = v1_to_posix_mode, .is_dir = v1_is_dir,
    .is_device = v1_is_device, .to_disk_mode = v1_to_disk_mode,
    .chmod_mode = v1_chmod_mode,
};
static const filsys_edition_t pdp7 = {
    .ops = &p7fs_ops, .alloc = &pdp7_alloc_ops,
    .state_size = sizeof(filsys_edition_t), .name = "pdp7",
    .bsize = P7_WSIZE * 2, .bo = &bo_me,
    .inode_size = P7_INODESZ, .ndaddr = P7_NIADDR, .niaddr = P7_NIADDR,
    .nindir = P7_NINDIR, .ilarg_mask = P7_ILARG, .large_single = 7, .large_double = 0,
    .ind_get = p7_ind_get, .ind_put = p7_ind_put, .word = &word_rb09,
    .max_namlen = P7_DIRSIZ, .dirent_size = P7_DIRENTSZ,
    .rootino = P7_ROOTINO, .synth_dot = 1,
    .to_posix_mode = p7_to_posix_mode, .is_dir = p7_is_dir,
    .is_device = p7_is_device, .to_disk_mode = p7_to_disk_mode,
    .chmod_mode = p7_chmod_mode,
};
static const filsys_edition_t bsd211 = {
    .ops = &bsd211fs_ops, .alloc = &freelist_alloc_ops,
    .state_size = sizeof(filsys_edition_t), .name = "bsd211",
    .bsize = BSD211_BSIZE, .bo = &bo_me,
    .nicfree = BSD211_NICFREE, .nicinod = BSD211_NICINOD,
    .inode_size = BSD211_INODESZ, .ndaddr = BSD211_NDADDR, .niaddr = BSD211_NIADDR,
    .addr_width = 4, .daddr_wid = 4, .df_nfree_wid = 2, .nindir = BSD211_NINDIR, .fmod_back = 3,
    .rootino = BSD211_ROOTINO,
    .max_namlen = BSD211_MAXNAMLEN, .dirent_size = 0,
    .ifmt = BSD211_IFMT, .ifdir = BSD211_IFDIR, .ifreg = BSD211_IFREG,
    .ifchr = BSD211_IFCHR, .ifblk = BSD211_IFBLK,
    .iflnk = BSD211_IFLNK, .ifsock = BSD211_IFSOCK,
};

/* The V8-family (Eighth/Ninth/Tenth Edition) is the V7 inode/dir/bmap engine
 * with a rearranged superblock (v8_sb_decode/v8_sb_encode), a larger block size
 * (1K/8K), and IFLNK symlinks.  The V8 prototype is the 1K free-list form; v9
 * and v10 override byte order / block size / free-cache depth from it. */
static const filsys_edition_t v8 = {
    .ops = &v7fs_ops, .alloc = &freelist_alloc_ops,
    .state_size = sizeof(filsys_edition_t), .name = "v8",
    .bsize = 1024, .bo = &bo_le,
    .nicfree = V8_NICFREE_SMALL, .nicinod = V7_NICINOD,
    .inode_size = V7_INODESZ, .ndaddr = V7_NDADDR, .niaddr = V7_NIADDR,
    .addr_width = 3, .daddr_wid = 4, .df_nfree_wid = 4, .nindir = 1024 / 4,
    .rootino = V7_ROOTINO, .max_namlen = V7_DIRSIZ, .dirent_size = V7_DIRENTSZ,
    .freemap = V8_FREEMAP_LIST,
    .noprobe = 1,   /* findfs's unified probe (Task 98) does not cover it yet */
    .sb_decode = v8_sb_decode, .sb_encode = v8_sb_encode,
    .ifmt = V7_IFMT, .ifdir = V7_IFDIR, .ifreg = V7_IFREG,
    .ifchr = V7_IFCHR, .ifblk = V7_IFBLK, .iflnk = V8_IFLNK,
};

filsys_edition_t filsys_getformat(int edition) {
    switch (edition) {
    case FILSYS_PDP7:   return pdp7;
    case FILSYS_V1:     return v1;
    case FILSYS_V6:     return v6;
    case FILSYS_BSD211: return bsd211;
    case FILSYS_V7:     return v7;
    case FILSYS_V8:     return v8;
    case FILSYS_V9: {
        /* Ninth Edition: the Sun-3 port, 8K blocks, big-endian.  NICFREE 946
         * (the V8-family free-list cache depth for an 8K superblock). */
        filsys_edition_t f; memcpy(&f, &v8, sizeof f);
        f.name = "v9"; f.bsize = 8192; f.bo = &bo_be;
        f.nicfree = V8_NICFREE_LARGE; f.nindir = 8192 / 4;
        f.to_posix_mode = v910_to_posix_mode;   /* ICONC/ICCTYP, not ISVTX */
        return f;
    }
    case FILSYS_V10: {
        /* Tenth Edition: the 1K free-list default is byte-identical to V8's;
         * the 4K in-/out-of-superblock bitmap forms are selected by -o
         * overrides (Task 6). */
        filsys_edition_t f; memcpy(&f, &v8, sizeof f);
        f.name = "v10";
        f.to_posix_mode = v910_to_posix_mode;   /* ICONC/ICCTYP, not ISVTX */
        return f;
    }
    case FILSYS_32V: {
        filsys_edition_t f; memcpy(&f, &v7, sizeof f);
        f.name = "vax32"; f.bo = &bo_le; f.pack4 = 1;
        return f;
    }
    case FILSYS_COHERENT: {
        filsys_edition_t f; memcpy(&f, &v7, sizeof f);
        f.name = "coherent"; f.nicfree = V7_COH_NICFREE; f.interleave = 1;
        return f;
    }
    case FILSYS_XENIX: {
        filsys_edition_t f; memcpy(&f, &v7, sizeof f);
        f.name = "xenix"; f.bsize = 1024; f.bo = &bo_le; f.nicfree = V7_XEN_NICFREE;
        f.nindir = 1024 / 4; f.magic = V7_XEN_MAGIC; f.magic_off = 0x3F8;
        return f;
    }
    case FILSYS_BSD29: {
        filsys_edition_t f; memcpy(&f, &v7, sizeof f);
        f.name = "bsd29"; f.bsize = 1024; f.ndaddr = 4; f.niaddr = 7; f.nindir = 1024 / 4;
        return f;
    }
    case FILSYS_SYSIII: {
        /* System III kept V7's superblock (no magic, 512-byte blocks); its
         * on-disk format is V7.  A distinct name so the edition table lists it
         * as its own entry rather than an alias. */
        filsys_edition_t f; memcpy(&f, &v7, sizeof f);
        f.name = "sysiii";
        return f;
    }
    case FILSYS_SVR2: {
        /* System V Release 2/3: the 4-byte-aligned s5fs layout (daddr_t/time_t
         * are 4-byte aligned, so s_ninode is at 212 and s_inode at 214), plus
         * s_dinfo[4] between s_time and s_tfree, and s_magic/s_type at the tail.
         * The byte order is the arch's, not the edition's: default little-endian
         * (VAX/x86), overridden by -o arch=3b2 / -o arch=68k for the big-endian
         * ports. */
        filsys_edition_t f; memcpy(&f, &v7, sizeof f);
        f.name = "sysvr2"; f.bo = &bo_le; f.pack4 = 1;
        f.magic = V7_SYSV_MAGIC; f.magic_off = V7_SYSV_MAGIC_OFF; f.dyn_bsize = 1;
        return f;
    }
    case FILSYS_SVR4: {
        /* System V Release 4: the same on-disk layout as sysvr2, plus an
         * s_state field at offset 500 (R2/R3 leave that as s_fill[12]).  The
         * clean/dirty values are FsOKAY/FsACTIVE, written by super_write. */
        filsys_edition_t f; memcpy(&f, &v7, sizeof f);
        f.name = "sysvr4"; f.bo = &bo_le;
        f.pack4 = 1;
        f.magic = V7_SYSV_MAGIC; f.magic_off = V7_SYSV_MAGIC_OFF; f.dyn_bsize = 1;
        return f;
    }
    default: {
        filsys_edition_t f = {0};
        return f;   /* ops == NULL marks an unknown edition */
    }
    }
}

/* ---- V8-family geometry overrides ----------------------------------------- */

int filsys_apply_geom(filsys_edition_t *fmt, int ed, const filsys_geom_t *g,
                      const char **errmsg) {
    static char msgbuf[128];
    if (errmsg)
        *errmsg = NULL;

    uint32_t bs = g->blocksize ? g->blocksize : fmt->bsize;
    int fm = g->freemap;
    if (fm < 0)
        fm = (bs == 4096) ? V8_FREEMAP_BITMAP : V8_FREEMAP_LIST;   /* 4096 ⇒ bitmap */

    if (fm == V8_FREEMAP_BIGMAP && ed != FILSYS_V10) {
        snprintf(msgbuf, sizeof msgbuf, "out-of-superblock bitmap is V10-only");
        if (errmsg) *errmsg = msgbuf;
        return -EINVAL;
    }

    if (ed == FILSYS_V9) {
        if (bs != 8192) {
            snprintf(msgbuf, sizeof msgbuf, "V9 block size is fixed at 8192 (got %u)", bs);
            if (errmsg) *errmsg = msgbuf;
            return -EINVAL;
        }
        /* V9 accepts either free-space form at 8192 */
    } else {
        /* V8/V10: one device-minor bit selects block size AND free-space form. */
        if (bs == 1024 && fm != V8_FREEMAP_LIST) {
            snprintf(msgbuf, sizeof msgbuf, "1024-byte blocks imply a free list (BITFS clear)");
            if (errmsg) *errmsg = msgbuf;
            return -EINVAL;
        }
        if (bs == 4096 && fm == V8_FREEMAP_LIST) {
            snprintf(msgbuf, sizeof msgbuf, "4096-byte blocks imply a bitmap (BITFS set)");
            if (errmsg) *errmsg = msgbuf;
            return -EINVAL;
        }
        if (bs != 1024 && bs != 4096) {
            snprintf(msgbuf, sizeof msgbuf, "unsupported block size %u (1024 or 4096)", bs);
            if (errmsg) *errmsg = msgbuf;
            return -EINVAL;
        }
    }

    if (g->byteorder) {
        if (!strcmp(g->byteorder, "le")) fmt->bo = &bo_le;
        else if (!strcmp(g->byteorder, "be")) fmt->bo = &bo_be;
        else {
            snprintf(msgbuf, sizeof msgbuf, "unknown byte order '%s' (le|be)", g->byteorder);
            if (errmsg) *errmsg = msgbuf;
            return -EINVAL;
        }
    }

    fmt->bsize = bs;
    fmt->nindir = bs / 4;
    fmt->freemap = (uint8_t)fm;
    fmt->alloc = (fm == V8_FREEMAP_LIST) ? &freelist_alloc_ops : &v8_bitmap_alloc_ops;
    return 0;
}

/* ---- byte-order resolution ------------------------------------------------ */

static const char *bo_endian_name(const byte_order_ops_t *bo) {
    if (bo == &bo_le) return "little-endian";
    if (bo == &bo_be) return "big-endian";
    if (bo == &bo_me) return "middle-endian (PDP-11)";
    return "unknown";
}

/* Byte offset of a magic-bearing edition's 4-byte magic word within the image,
 * measured from the start of the filesystem.  System V keeps its 512-byte
 * superblock at byte 512 whatever the logical block size; Xenix keeps its magic
 * at byte 1016 of its block-1 superblock (1024 bytes). */
static off_t magic_byte_offset(const filsys_edition_t *fmt) {
    if (fmt->dyn_bsize)
        return 512 + (off_t)fmt->magic_off;
    return (off_t)fmt->bsize + fmt->magic_off;
}

/* Resolve the byte order of a magic-bearing edition from its on-disk magic
 * word, and apply the user's `arch` (if any) against it.  The magic determines
 * the byte order, and the byte order determines whether the other superblock
 * fields parse -- the kernel reads s_magic in each byte order first, sets
 * s_bytesex, then reads everything else.  This is the same chicken-and-egg
 * resolved up front, so a big-endian volume opens whether or not the user
 * names the arch.
 *
 * On success fmt->bo is the resolved order and 0 is returned.  On failure a
 * negative errno is returned and *errmsg (if non-NULL) receives a static
 * description the caller should print.
 *
 *   arch == NULL   -- detect the order from the magic word (little-, big- or
 *                     middle-endian).  If it reads in none (a wrong magic) the
 *                     open is refused.
 *   arch != NULL   -- the user insists on `arch`.  A magic word that reads in
 *                     a different order is a conflict (EINVAL); a magic word
 *                     that reads in none is refused (EILSEQ) unless `force`.
 */
int filsys_resolve_byteorder(filsys_edition_t *fmt, const char *path,
                             uint64_t offset, const char *arch, int force,
                             const char **errmsg) {
    static char msgbuf[160];
    if (errmsg)
        *errmsg = NULL;

    const byte_order_ops_t *arch_bo = NULL;
    if (arch) {
        arch_bo = filsys_arch_bo(arch);
        if (!arch_bo) {
            snprintf(msgbuf, sizeof msgbuf, "unknown architecture '%s'", arch);
            if (errmsg) *errmsg = msgbuf;
            return -EINVAL;
        }
    }

    /* No magic word: nothing to detect; the arch (if any) simply wins. */
    if (!fmt->magic) {
        if (arch_bo)
            fmt->bo = arch_bo;
        return 0;
    }

    uint8_t m[4];
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        int e = errno;
        snprintf(msgbuf, sizeof msgbuf, "cannot open image: %s", strerror(e));
        if (errmsg) *errmsg = msgbuf;
        return -e;
    }
    off_t moff = magic_byte_offset(fmt) + (off_t)offset;
    ssize_t n = pread(fd, m, sizeof m, moff);
    close(fd);
    if (n != (ssize_t)sizeof m) {
        snprintf(msgbuf, sizeof msgbuf, "cannot read superblock magic at byte %lld",
                 (long long)moff);
        if (errmsg) *errmsg = msgbuf;
        return -EIO;
    }

    int le = (bo_get32le(m) == fmt->magic);
    int be = (bo_get32be(m) == fmt->magic);
    int me = (bo_get32me(m) == fmt->magic);

    if (arch_bo) {
        if (arch_bo->get32(m) == fmt->magic) {
            fmt->bo = arch_bo;             /* the arch matches the magic */
            return 0;
        }
        if (le || be || me) {              /* valid magic, but the other order */
            const byte_order_ops_t *det = le ? &bo_le : be ? &bo_be : &bo_me;
            snprintf(msgbuf, sizeof msgbuf,
                     "superblock magic is %s, but arch '%s' is %s",
                     bo_endian_name(det), arch, bo_endian_name(arch_bo));
            if (errmsg) *errmsg = msgbuf;
            return -EINVAL;
        }
        if (force) {                       /* wrong magic, but the user insists */
            fmt->bo = arch_bo;
            fmt->ignore_magic = 1;
            return 0;
        }
        snprintf(msgbuf, sizeof msgbuf,
                 "bad superblock magic 0x%08x (expected 0x%08x); use -F to force arch '%s'",
                 bo_get32le(m), fmt->magic, arch);
        if (errmsg) *errmsg = msgbuf;
        return -EILSEQ;
    }

    if (le) { fmt->bo = &bo_le; return 0; }
    if (be) { fmt->bo = &bo_be; return 0; }
    if (me) { fmt->bo = &bo_me; return 0; }
    snprintf(msgbuf, sizeof msgbuf,
             "bad superblock magic 0x%08x (expected 0x%08x); cannot determine byte order",
             bo_get32le(m), fmt->magic);
    if (errmsg) *errmsg = msgbuf;
    return -EILSEQ;
}

