/* filsys_ops.h - internal vtable for the filsys library backends.
 *
 * Each on-disk format (v1fs, v7fs, pdp7fs, ...) exposes one of these; filsys.c routes
 * every operation through it rather than a per-edition switch, so a new format
 * is one more ops table instead of a third arm of every ternary.
 *
 * The first parameter of every op is a `void *` pointing at the backend's own (filsys_edition_t)
 * state struct; the decoded types (filsys_inode_t / filsys_dirent_t) are the
 * public structs from filsys.h.  Backends may expose their functions with
 * layout-identical private typedefs and cast at the ops-table definition.
 *
 * SPDX-License-Identifier: ISC
 */
#ifndef FILSYS_OPS_H
#define FILSYS_OPS_H

#include "filsys.h"
#include "v7fs.h"
#include "check.h"

#include <stdio.h>
#include <stdarg.h>

/* fsck check modes passed to the per-edition *_check functions.  A bitmask so
 * salvage (rebuild the free list) and preen (auto-fix the safe subset) are
 * independent knobs rather than another boolean in the signature.  YES/ASK
 * select how a repair is answered; PREEN implies YES for the safe subset. */
enum {
    FILSYS_CK_SALVAGE = 1,  /* rebuild the free list / free map */
    FILSYS_CK_PREEN   = 2,  /* auto-fix the safe subset (needs rw open) */
    FILSYS_CK_FORCE   = 4,  /* check even if the superblock is marked clean */
    FILSYS_CK_YES     = 8,  /* assume yes: auto-apply each repair (-y) */
    FILSYS_CK_ASK     = 16, /* interactive: prompt on stdin before a repair (-i) */
    FILSYS_MAXBADOK   = 10  /* bad blocks tolerated before a check aborts */
};

/* Prompt for a repair decision (BSD fsck's reply()/query()).  mode carries the
 * answer: FILSYS_CK_YES (or preen's FILSYS_CK_PREEN) auto-applies, FILSYS_CK_ASK
 * reads y/n from stdin, and anything else skips the repair.  Returns 1 to apply
 * the repair, 0 to skip it. */
static inline int filsys_query(int mode, const char *fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    if (mode & (FILSYS_CK_YES | FILSYS_CK_PREEN)) {
        printf("%s [yes]\n", msg);
        return 1;
    }
    if (!(mode & FILSYS_CK_ASK)) {
        printf("%s [no]\n", msg);
        return 0;
    }
    for (;;) {
        printf("%s [yes/no]? ", msg);
        fflush(stdout);
        int c = getchar();
        if (c == EOF || c == '\n')
            continue;
        while (getchar() != '\n' && !feof(stdin))
            ;                       /* drain the rest of the line */
        if (c == 'y' || c == 'Y') return 1;
        if (c == 'n' || c == 'N') return 0;
    }
}

/* Per-inode state tracked by the checker -- BSD fsck's flag byte.  The
 * low three bits (IN_MODEMASK) hold the inode's type; the rest are phase flags
 * set as the walk discovers them.  A state of 0 (UNALLOC) is a free inode;
 * UNKNOWN is an inode whose mode is set but whose type is not one of the five
 * recognised kinds. */
enum {
    FILSYS_IN_UNALLOC  = 0x00,  /* mode == 0: free inode */
    FILSYS_IN_UNKNOWN  = 0x01,  /* allocated, type not recognised */
    FILSYS_IN_IDIR     = 0x02,  /* directory */
    FILSYS_IN_IREG     = 0x03,  /* regular file */
    FILSYS_IN_ICHR     = 0x04,  /* character device (or multiplexed) */
    FILSYS_IN_IBLK     = 0x05,  /* block device (or multiplexed) */
    FILSYS_IN_IPIPE    = 0x06,  /* FIFO / pipe (Coherent only) */
    FILSYS_IN_IBAD     = 0x08,  /* inode has a bad or duplicate block */
    FILSYS_IN_VISITED  = 0x10,  /* reached during directory traversal */
    FILSYS_IN_UNREFDIR = 0x20,  /* unreferenced directory (orphan) */
    FILSYS_IN_CHILDDIR = 0x40,  /* directory with a parent link */
    FILSYS_IN_IFREE    = 0x80,  /* on the free-inode list */
    FILSYS_IN_MODEMASK  = 0x07,
    FILSYS_IN_ALLOCMASK = 0x06  /* any classified type (IDIR..IPIPE) */
};

/* True if the low type bits say the inode is allocated (mode != 0). */
static inline int filsys_in_allocated(uint8_t st) {
    return (st & FILSYS_IN_MODEMASK) != FILSYS_IN_UNALLOC;
}

/* Test hook: swap the byte-slice transport on an open filesystem (fault
 * injection).  Internal, not part of the public filsys.h API. */
void filsys_set_io(filsys_t *fs, const filsys_io_t *io);

/* Directory-format sub-vtable: the ops that differ with the on-disk directory
 * entry layout (fixed 16-byte V7 entries vs variable-length 2.11BSD entries vs
 * the 10-byte V1 entries).  The rest of the directory machinery (dir_lookup) is
 * shared and stays in filsys_ops. */
struct filsys_dir_ops {
    int (*dir_read)(filsys_edition_t *fs, filsys_inode_t *ip, filsys_dirent_t **ents, size_t *count);
    int (*dir_add)(filsys_edition_t *fs, filsys_inode_t *ip, uint32_t ino, const char *name);
    int (*dir_remove)(filsys_edition_t *fs, filsys_inode_t *ip, const char *name);
};

/* Inode-format sub-vtable: the ops that differ with the on-disk inode layout
 * and address width (32-byte V6, 64-byte V7, 64-byte+32-bit-addr 2.11BSD). */
struct filsys_inode_ops {
    int  (*read_inode)(filsys_edition_t *fs, uint32_t ino, filsys_inode_t *ip);
    int  (*write_inode)(filsys_edition_t *fs, uint32_t ino, const filsys_inode_t *ip);
    int  (*bmap)(filsys_edition_t *fs, filsys_inode_t *ip, uint32_t lbn, int create, uint32_t *bno);
    uint8_t (*inode_state)(filsys_edition_t *fs, uint32_t ino, uint32_t mode); /* -> FILSYS_IN_* */
    /* Count of allocated filesystem blocks (data + indirect), for st_blocks.
     * A zero block address is a hole -- not allocated, not counted. */
    uint64_t (*allocated_blocks)(filsys_edition_t *fs, const filsys_inode_t *ip);
};

struct filsys_ops {
    const char *name;
    const struct filsys_dir_ops   *dir;    /* fixed-16 | variable | v1-10byte */
    const struct filsys_inode_ops *inode;  /* 32-byte | 64-byte | 64+32bit-addr */
    uint32_t (*blocksize)(const filsys_edition_t *fs);  /* logical block size in bytes */

    /* lifecycle */
    int  (*open)(filsys_edition_t *fs, const char *path, int readonly,
                 const filsys_edition_t *proto, uint64_t offset);
    int  (*close)(filsys_edition_t *fs);   /* final flush; returns the sync result */
    int  (*sync)(filsys_edition_t *fs);
    /* Mark the superblock dirty (s_fmod) and flush.  Optional: only the V6/V7
     * formats carry an s_fmod byte; other backends leave it NULL. */
    int  (*mark_dirty)(filsys_edition_t *fs);

    /* block io */
    int  (*read_block)(filsys_edition_t *fs, uint32_t bno, uint8_t *buf);
    int  (*write_block)(filsys_edition_t *fs, uint32_t bno, const uint8_t *buf);

    /* data-block codec: read/write a block as `blocksize` *logical* bytes.
     * For byte-addressed editions logical == physical, so this is read_block;
     * for the word-addressed PDP-7 it is read_words + the 2-chars-per-word
     * pack/unpack (a 128-logical-byte block from a 256-byte container). */
    int  (*blk_get)(filsys_edition_t *fs, uint32_t bno, uint8_t *buf);
    int  (*blk_put)(filsys_edition_t *fs, uint32_t bno, const uint8_t *buf);

    /* inode allocation (block allocation is internal to bmap/itrunc) */
    int  (*ialloc)(filsys_edition_t *fs, uint32_t *ino);
    void (*ifree)(filsys_edition_t *fs, uint32_t ino);

    /* file data */
    ssize_t (*file_read)(filsys_edition_t *fs, filsys_inode_t *ip, uint8_t *buf, size_t size, off_t off);
    ssize_t (*file_write)(filsys_edition_t *fs, filsys_inode_t *ip, const uint8_t *buf, size_t size, off_t off);

    /* directories (dir_read/add/remove live in the dir sub-vtable) */
    int  (*dir_lookup)(filsys_edition_t *fs, filsys_inode_t *ip, const char *name, uint32_t *ino);

    /* path lookup */
    int  (*lookup)(filsys_edition_t *fs, const char *path, uint32_t *ino, filsys_inode_t *ip);

    /* integrity check; returns -1 if problems were found */
    int  (*check)(filsys_edition_t *fs);

    /* Check-driver seams.  The shared filsys_check_common() driver (filsys.c)
     * calls these per-edition ops so the block-accounting / dup-rescan /
     * directory-walk / link-count logic lives once instead of per backend. */
    uint32_t (*maxino)(filsys_edition_t *fs);              /* last inode number */
    uint32_t (*data_start)(filsys_edition_t *fs);          /* first data block */
    uint32_t (*data_end)(filsys_edition_t *fs);            /* one past the last data block */
    void     (*walk_free)(filsys_edition_t *fs, filsys_chkctx_t *cx, filsys_check_t *rep);
                              /* walk the allocator, marking free blocks into cx->bmap
                               * (detecting used+free as dup), counting free_blocks */
    uint32_t (*makefree)(filsys_edition_t *fs, filsys_chkctx_t *cx); /* salvage: rebuild free space */
    int      (*is_clean)(filsys_edition_t *fs);            /* 1 = superblock marked clean (fmod==0) */

    /* fill the edition-specific statvfs totals (blocks / free / files / free) */
    void (*statfs)(filsys_edition_t *fs, struct statvfs *st);

    /* largest addressable file, in bytes */
    uint64_t (*max_file)(filsys_edition_t *fs);
};

extern const struct filsys_ops v6fs_ops;
extern const struct filsys_ops v7fs_ops;
extern const struct filsys_ops v1fs_ops;
extern const struct filsys_ops p7fs_ops;
extern const struct filsys_ops bsd211fs_ops;

/* The shared fixed-length directory ops (v7fs_dir_*), used by the V6/V7 family
 * and V1 (which differs only in dirent_size/max_namlen). */
extern const struct filsys_dir_ops dir_fixed;

/* The check-side ops shared across every edition (filsys_format.c). */
int filsys_check_op(filsys_edition_t *fs);
int filsys_is_clean(filsys_edition_t *fs);
uint64_t filsys_max_file_op(filsys_edition_t *fs);

/* The shared integrity-check driver (check.c).  fmt is the format descriptor
 * (for rootino / cache depths / generic fields); fs is the backend state. */
int filsys_check_common(filsys_edition_t *fmt, filsys_edition_t *fs,
                        filsys_check_t *rep, int mode);

#endif /* FILSYS_OPS_H */
