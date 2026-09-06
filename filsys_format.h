/* filsys_format.h - the descriptor that parameterizes a filesystem format.
 *
 * filsys_ops (the vtable) carries each backend's *operations*; this carries the
 * *constants* and *structural predicates* that tell one on-disk format from
 * another.  A new edition is a new row here, not another arm of a dozen
 * `== FILSYS_` switches in filsys.c.
 *
 * The mode-conversion function pointers are optional: the V6/V7/BSD211 family
 * derives all of them from the ifmt/ifdir/... constants below, while V1 and
 * PDP-7 (whose type models are not a V7-style type field) supply their own.
 *
 * SPDX-License-Identifier: ISC
 */
#ifndef FILSYS_FORMAT_H
#define FILSYS_FORMAT_H

#include "filsys.h"
#include "bo.h"

/* The type a to_disk_mode caller is encoding (create / mkdir / mknod). */
enum {
    FILSYS_FT_REG = 0,
    FILSYS_FT_DIR,
    FILSYS_FT_CHR,
    FILSYS_FT_BLK
};

struct filsys_ops;                /* forward: the per-backend vtable */

typedef struct filsys_format {
    const struct filsys_ops *ops; /* backend vtable */
    size_t      state_size;       /* sizeof the backend state struct */
    const char *name;             /* "-v" spelling */
    uint32_t    bsize;            /* logical block size in bytes */
    const byte_order_ops_t *bo;  /* byte-order ops (bo_le / bo_be / bo_me) */

    /* superblock */
    uint16_t    nicfree;     /* free-block cache depth (s_nfree) */
    uint16_t    nicinod;     /* free-inode cache depth (s_ninode) */
    uint8_t     pack4;       /* 4-byte-aligned superblock fields (32V) */
    uint8_t     interleave;  /* Coherent s_m/s_n free-list interleave + s_unique */
    uint32_t    magic;       /* superblock magic word (0 = none) */
    int         magic_off;   /* byte offset of magic within the superblock */

    /* inode geometry */
    uint8_t     inode_size;  /* bytes per on-disk inode */
    uint8_t     ndaddr;      /* direct block addresses */
    uint8_t     niaddr;      /* total block addresses */

    /* directory-entry geometry */
    uint8_t     max_namlen;  /* longest entry name (8 / 14 / 63) */
    uint8_t     dirent_size; /* bytes per fixed entry (0 = variable-length) */

    /* on-disk type field (the V6/V7/BSD211 family); 0 for V1/PDP-7 */
    uint16_t    ifmt, ifdir, ifreg, ifchr, ifblk, iflnk, ifsock, ifmpc, ifmpb;

    /* mode conversion.  NULL means "derive from the constants above"; V1 and
     * PDP-7 supply their own because their type model is not a type field. */
    mode_t   (*to_posix_mode)(const struct filsys_format *, const filsys_inode_t *);
    int      (*is_dir)(const struct filsys_format *, const filsys_inode_t *);
    int      (*is_device)(const struct filsys_format *, const filsys_inode_t *);
    uint32_t (*to_disk_mode)(const struct filsys_format *, mode_t m, int type);
    uint32_t (*chmod_mode)(const struct filsys_format *, uint32_t old_mode, mode_t m);
} filsys_format_t;

/* The descriptor for an edition: a copy of the V7 default with the edition's
 * overrides.  Returns a zeroed descriptor (ops == NULL) for an unknown
 * edition. */
filsys_format_t filsys_getformat(int edition);

#endif /* FILSYS_FORMAT_H */
