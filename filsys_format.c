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
#include <stdio.h>
#include <string.h>
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
    return to_p7_mode(m, type == FILSYS_FT_DIR);
}
static uint32_t p7_chmod_mode(const filsys_edition_t *f, uint32_t old, mode_t m) {
    (void)f;
    uint32_t bits = to_p7_mode(m, 0) & (uint32_t)017;
    return (old & ~(uint32_t)017) | bits;
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
    .addr_width = 3, .daddr_wid = 4, .fmod_back = 2, .rootino = V7_ROOTINO,
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
    .daddr_wid = 2, .isize_count = 1, .rootino = V6_ROOTINO,
    .max_namlen = V6_DIRSIZ, .dirent_size = 2 + V6_DIRSIZ,
    .ifmt = V6_IFMT, .ifdir = V6_IFDIR, .ifchr = V6_IFCHR, .ifblk = V6_IFBLK,
};
static const filsys_edition_t v1 = {
    .ops = &v1fs_ops, .alloc = &bitmap_alloc_ops,
    .state_size = sizeof(filsys_edition_t), .name = "v1",
    .bsize = V1_BSIZE, .bo = &bo_me,
    .inode_size = V1_INODESZ, .ndaddr = V1_NDADDR, .niaddr = V1_NIADDR,
    .rootino = V1_ROOTINO, .max_namlen = V1_DIRSIZ, .dirent_size = V1_DIRENTSZ,
    .to_posix_mode = v1_to_posix_mode, .is_dir = v1_is_dir,
    .is_device = v1_is_device, .to_disk_mode = v1_to_disk_mode,
    .chmod_mode = v1_chmod_mode,
};
static const filsys_edition_t pdp7 = {
    .ops = &p7fs_ops, .state_size = sizeof(filsys_edition_t), .name = "pdp7",
    .bsize = P7_WSIZE * 2, .bo = &bo_me,
    .inode_size = P7_INODESZ, .ndaddr = P7_NIADDR, .niaddr = P7_NIADDR,
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
    .addr_width = 4, .daddr_wid = 4, .fmod_back = 3, .rootino = BSD211_ROOTINO,
    .max_namlen = BSD211_MAXNAMLEN, .dirent_size = 0,
    .ifmt = BSD211_IFMT, .ifdir = BSD211_IFDIR, .ifreg = BSD211_IFREG,
    .ifchr = BSD211_IFCHR, .ifblk = BSD211_IFBLK,
    .iflnk = BSD211_IFLNK, .ifsock = BSD211_IFSOCK,
};

filsys_edition_t filsys_getformat(int edition) {
    switch (edition) {
    case FILSYS_PDP7:   return pdp7;
    case FILSYS_V1:     return v1;
    case FILSYS_V6:     return v6;
    case FILSYS_BSD211: return bsd211;
    case FILSYS_V7:     return v7;
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
        f.magic = V7_XEN_MAGIC; f.magic_off = 0x3F8;
        return f;
    }
    case FILSYS_BSD29: {
        filsys_edition_t f; memcpy(&f, &v7, sizeof f);
        f.name = "bsd29"; f.bsize = 1024; f.ndaddr = 4; f.niaddr = 7;
        return f;
    }
    default: {
        filsys_edition_t f = {0};
        return f;   /* ops == NULL marks an unknown edition */
    }
    }
}

/* ---- edition name table -------------------------------------------------- */

/* One row per on-disk format.  `name` is the canonical "-v" spelling; `aliases`
 * are the alternates the tools have always accepted: the numeric edition
 * numbers, the pre-rename spellings, and the family members that share a format
 * (V2/V3 with V1, V4/V5 with V6).  A leading "v"/"V" is stripped by
 * filsys_edition_by_name, so "v7" and "7" resolve alike. */
static const char *const pdp7_alias[]     = { "0", "p7", NULL };
static const char *const v1_alias[]       = { "1", "2", "3", NULL };
static const char *const v6_alias[]       = { "4", "5", "6", NULL };
static const char *const v7_alias[]       = { "7", NULL };
static const char *const vax32_alias[]    = { "32v", "32", NULL };
static const char *const coherent_alias[] = { "coh", "33", NULL };
static const char *const xenix_alias[]    = { "34", NULL };
static const char *const bsd29_alias[]    = { "35", NULL };
static const char *const bsd211_alias[]   = { "36", NULL };

static const filsys_format_t formats[] = {
    { FILSYS_PDP7,     "pdp7",     pdp7_alias },
    { FILSYS_V1,       "v1",       v1_alias },
    { FILSYS_V6,       "v6",       v6_alias },
    { FILSYS_V7,       "v7",       v7_alias },
    { FILSYS_32V,      "vax32",    vax32_alias },
    { FILSYS_COHERENT, "coherent", coherent_alias },
    { FILSYS_XENIX,    "xenix",    xenix_alias },
    { FILSYS_BSD29,    "bsd29",    bsd29_alias },
    { FILSYS_BSD211,   "bsd211",   bsd211_alias },
};
#define NFMT (sizeof formats / sizeof formats[0])

const filsys_format_t *filsys_format_nth(size_t i) {
    return i < NFMT ? &formats[i] : NULL;
}

int filsys_edition_by_name(const char *name) {
    if (name == NULL)
        return -1;
    /* "vax32" predates the leading-v convention; match it (either case) before
     * the strip, which would otherwise turn it into "ax32". */
    if (!strcmp(name, "vax32") || !strcmp(name, "VAX32"))
        return FILSYS_32V;
    if (name[0] == 'v' || name[0] == 'V')
        name++;
    for (size_t i = 0; i < NFMT; i++) {
        if (!strcmp(name, formats[i].name))
            return formats[i].edition;
        for (const char *const *a = formats[i].aliases; a && *a; a++)
            if (!strcmp(name, *a))
                return formats[i].edition;
    }
    return -1;
}

const char *filsys_editions_usage(void) {
    static char buf[128];
    size_t n = 0;
    for (size_t i = 0; i < NFMT; i++)
        n += (size_t)snprintf(buf + n, sizeof buf - n, "%s%s",
                              i ? "|" : "", formats[i].name);
    return buf;
}
