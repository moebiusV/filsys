/* check.c - the shared integrity-check driver.
 *
 * icheck(8)+dcheck(8) in one pass, run through the per-edition ops in the
 * vtable (filsys_ops.h).  See the comment on filsys_check_common below.
 *
 * SPDX-License-Identifier: ISC
 */
#include <config.h>
#include "filsys.h"
#include "filsys_ops.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- shared integrity-check driver --------------------------------------
 *
 * icheck(8)+dcheck(8) in one pass, run through the per-edition ops in the
 * vtable.  The block-accounting / dup-rescan / missing-block / link-count
 * logic is format-independent; the four things that differ (how an inode's
 * blocks are walked, how the allocator is walked, how a mode maps to a
 * checker state, and the salvage/repair actions) are vtable seams, so every
 * edition's *_check is a one-line call into this. */
int filsys_check_common(filsys_edition_t *fmt, filsys_edition_t *fs,
                        filsys_check_t *rep, int mode)
{
    const struct filsys_ops *o = fmt->ops;
    memset(rep, 0, sizeof(*rep));

    if (o->is_clean(fs) && !(mode & (FILSYS_CK_SALVAGE | FILSYS_CK_FORCE))) {
        printf("filesystem clean; skipped (use -f to force)\n");
        return 0;
    }

    uint32_t maxino = o->maxino(fs);
    uint32_t dstart = o->data_start(fs);
    uint32_t dend   = o->data_end(fs);
    rep->inodes = maxino;

    /* 1. superblock sanity.  A bad superblock makes everything downstream
     * meaningless, so bail before the walks (nblk would underflow). */
    if (dstart >= dend || dend == 0) {
        printf("bad superblock: data start=%u end=%u\n", dstart, dend);
        rep->errors++;
        printf("superblock unusable; skipping check\n");
        return -1;
    }
    uint32_t nblk = dend - dstart;

    filsys_chkctx_t cx;
    memset(&cx, 0, sizeof(cx));
    cx.nblk = nblk;
    cx.bmap = calloc((nblk + 7) / 8, 1);
    if (!cx.bmap)
        return -ENOMEM;
    /* owner table: name the inode that first claimed each block (BSD fsck's
     * phase-1b rescan), so a duplicate report says who already owns it. */
    cx.owner = calloc(nblk + 1, sizeof(uint32_t));
    if (!cx.owner) {
        free(cx.bmap);
        return -ENOMEM;
    }

    uint8_t *state = calloc(maxino + 1, 1);
    if (!state) {
        free(cx.owner);
        free(cx.bmap);
        return -ENOMEM;
    }

    /* 2. icheck pass 1: mark every block referenced by an inode. */
    for (uint32_t ino = 1; ino <= maxino; ino++) {
        filsys_inode_t ip;
        if (o->read_inode(fs, ino, &ip)) {
            printf("inode %u unreadable\n", ino);
            rep->errors++;
            continue;
        }
        uint8_t st = o->inode_state(fs, ino, ip.mode);
        state[ino] = st;
        if (st == FILSYS_IN_UNALLOC)
            continue;   /* mode says free */
        rep->used_inodes++;
        if (st == FILSYS_IN_UNKNOWN) {
            printf("inode %u unknown type 0%o\n", ino, ip.mode & 0177777);
            rep->errors++;
            continue;   /* unknown type: block addresses are untrusted */
        }
        if (st == FILSYS_IN_ICHR || st == FILSYS_IN_IBLK)
            continue;   /* device inode: addr[0] is a device number, not a block */
        cx.ino = ino;
        o->mark_blocks(fs, &ip, ino, &cx);
    }
    rep->errors += cx.errors;

    /* Too many bad blocks leave the accounting untrustworthy (BSD fsck's
     * errflag), so skip the free-list and directory phases rather than report
     * phantom "missing" blocks. */
    if (cx.bad_blocks > FILSYS_MAXBADOK) {
        printf("too many bad blocks (%u); skipping free-list and directory checks\n",
               cx.bad_blocks);
        free(state);
        free(cx.owner);
        free(cx.bmap);
        return -1;
    }

    if (mode & FILSYS_CK_SALVAGE) {
        uint32_t nf = o->makefree(fs, &cx);
        printf("salvaged: free space rebuilt (%u free blocks)\n", nf);
        free(state);
        free(cx.owner);
        free(cx.bmap);
        return rep->errors ? -1 : 0;
    }

    /* 3. walk the allocator exactly as it would hand blocks out, marking free
     * blocks into cx.bmap (a free block already used is a duplicate). */
    o->walk_free(fs, &cx, rep);

    /* 4. missing blocks: in the data area but neither used nor free. */
    for (uint32_t off = 0; off < nblk; off++)
        if (!(cx.bmap[off >> 3] & (uint8_t)(1u << (off & 7)))) {
            rep->missing_blocks++;
            rep->errors++;
        }

    rep->used_blocks = cx.used_blocks;
    rep->dup_blocks  = cx.dup_blocks;

    /* 5. dcheck: directory link counts. */
    uint8_t *ecount = calloc(maxino + 1, 1);
    if (ecount) {
        for (uint32_t ino = 1; ino <= maxino; ino++) {
            filsys_inode_t ip;
            if (o->read_inode(fs, ino, &ip))
                continue;
            if (state[ino] != FILSYS_IN_IDIR)
                continue;
            filsys_dirent_t *ents = NULL;
            size_t cnt = 0;
            if (o->dir_read(fs, &ip, &ents, &cnt) == 0) {
                for (size_t e = 0; e < cnt; e++) {
                    if (fmt->synth_dot &&
                        ents[e].name[0] == '.' &&
                        (ents[e].name[1] == 0 ||
                         (ents[e].name[1] == '.' && ents[e].name[2] == 0)))
                        continue;   /* synthesized "." / ".." */
                    uint32_t dno = ents[e].ino;
                    if (dno == 0)
                        continue;
                    if (dno > maxino) {
                        printf("%u bad; %u/%s\n", dno, ino, ents[e].name);
                        rep->errors++;
                        continue;
                    }
                    if (!filsys_in_allocated(state[dno])) {
                        printf("dir %u references free inode %u\n", ino, dno);
                        rep->errors++;
                    }
                    ecount[dno]++;
                    if (ecount[dno] == 0)
                        ecount[dno] = 0377;
                }
                free(ents);
            }
        }
        for (uint32_t ino = 1; ino <= maxino; ino++) {
            filsys_inode_t ip;
            if (o->read_inode(fs, ino, &ip))
                continue;
            int cnt = ecount[ino] & 0377;
            if (cnt == ip.nlink)
                continue;
            if (state[ino] == FILSYS_IN_UNALLOC && cnt == 0)
                continue;
            if (fmt->synth_dot &&
                (state[ino] == FILSYS_IN_IDIR || state[ino] == FILSYS_IN_ICHR ||
                 state[ino] == FILSYS_IN_IBLK))
                continue;   /* directory/device link counts are synthesized */
            if (fmt->synth_dot && ino == fmt->rootino)
                continue;   /* root's own link is a synthesized mount point */
            printf("%u entries=%d link=%d\n", ino, cnt, ip.nlink);
            rep->errors++;
        }
        if (mode & (FILSYS_CK_PREEN | FILSYS_CK_YES | FILSYS_CK_ASK))
            o->preen(fs, ecount, state, maxino, mode);
        free(ecount);
    }

    free(state);
    free(cx.owner);
    free(cx.bmap);

    printf("used blocks=%u  free blocks=%u  missing=%u  dup=%u  inodes=%u/%u used  errors=%u\n",
           rep->used_blocks, rep->free_blocks, rep->missing_blocks,
           rep->dup_blocks, rep->used_inodes, rep->inodes, rep->errors);
    return rep->errors ? -1 : 0;
}

