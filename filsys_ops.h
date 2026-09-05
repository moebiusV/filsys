/* filsys_ops.h - internal vtable for the filsys library backends.
 *
 * Each on-disk format (v6fs, v7fs, ...) exposes one of these; filsys.c routes
 * every operation through it rather than a per-edition switch, so a new format
 * is one more ops table instead of a third arm of every ternary.
 *
 * The first parameter of every op is a `void *` pointing at the backend's own
 * state struct; the decoded types (filsys_inode_t / filsys_dirent_t) are the
 * public structs from filsys.h.  Backends may expose their functions with
 * layout-identical private typedefs and cast at the ops-table definition.
 *
 * SPDX-License-Identifier: ISC
 */
#ifndef FILSYS_OPS_H
#define FILSYS_OPS_H

#include "filsys.h"

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

/* Prompt for a repair decision (Coherent fsck's query()).  mode carries the
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

/* Per-inode state tracked by the checker -- Coherent fsck's flag byte.  The
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

struct filsys_ops {
    const char *name;
    uint32_t blocksize;   /* logical block size in bytes (bmap/truncate unit) */

    /* lifecycle */
    int  (*open)(void *fs, const char *path, int readonly, int little_endian,
                 uint64_t offset);
    void (*close)(void *fs);
    int  (*sync)(void *fs);
    /* Mark the superblock dirty (s_fmod) and flush.  Optional: only the V6/V7
     * formats carry an s_fmod byte; other backends leave it NULL. */
    int  (*mark_dirty)(void *fs);

    /* block io */
    int  (*read_block)(void *fs, uint32_t bno, uint8_t *buf);
    int  (*write_block)(void *fs, uint32_t bno, const uint8_t *buf);

    /* inode io: on-disk bytes <-> decoded filsys_inode_t */
    int  (*read_inode)(void *fs, uint32_t ino, filsys_inode_t *ip);
    int  (*write_inode)(void *fs, uint32_t ino, const filsys_inode_t *ip);

    /* inode allocation (block allocation is internal to bmap/itrunc) */
    int  (*ialloc)(void *fs, uint32_t *ino);
    void (*ifree)(void *fs, uint32_t ino);

    /* block mapping + truncate */
    int  (*bmap)(void *fs, filsys_inode_t *ip, uint32_t lbn, int create, uint32_t *bno);
    int  (*itrunc)(void *fs, filsys_inode_t *ip);
    int  (*itrunc_from)(void *fs, filsys_inode_t *ip, uint32_t first_blk);

    /* file data */
    ssize_t (*file_read)(void *fs, filsys_inode_t *ip, uint8_t *buf, size_t size, off_t off);
    ssize_t (*file_write)(void *fs, filsys_inode_t *ip, const uint8_t *buf, size_t size, off_t off);

    /* directories */
    int  (*dir_read)(void *fs, filsys_inode_t *ip, filsys_dirent_t **ents, size_t *count);
    int  (*dir_lookup)(void *fs, filsys_inode_t *ip, const char *name, uint32_t *ino);
    int  (*dir_add)(void *fs, filsys_inode_t *ip, uint32_t ino, const char *name);
    int  (*dir_remove)(void *fs, filsys_inode_t *ip, const char *name);

    /* path lookup */
    int  (*lookup)(void *fs, const char *path, uint32_t *ino, filsys_inode_t *ip);

    /* integrity check; returns -1 if problems were found */
    int  (*check)(void *fs);

    /* largest addressable file, in bytes */
    uint64_t (*max_file)(void *fs);
};

extern const struct filsys_ops v6fs_ops;
extern const struct filsys_ops v7fs_ops;
extern const struct filsys_ops v1fs_ops;
extern const struct filsys_ops p7fs_ops;

#endif /* FILSYS_OPS_H */
