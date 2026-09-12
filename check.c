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
static int is_regular(const filsys_edition_t *fs, const filsys_inode_t *ip);
int filsys_check_common(filsys_edition_t *fmt, filsys_edition_t *fs,
                        filsys_check_t *rep, int mode)
{
    const struct filsys_ops *o = fmt->ops;
    memset(rep, 0, sizeof(*rep));

    /* Only the auto-fix modes (-p preen, -y yes, -i interactive) honour the
     * clean flag: they are the boot-time "skip a cleanly-unmounted filesystem"
     * fast path, matching System V's `fsck -p`.  A plain report fsck always
     * runs the full check, because s_fmod==0 only means "cleanly unmounted",
     * not "internally consistent" -- corruption (orphaned inodes, duplicate
     * blocks) can coexist with a clean flag, and System III's fsck always
     * full-checks regardless.  No "(use -f to force)" here: -f means force only
     * on fsck.filsys; on mount.filsys -f is "stay in foreground", so the hint
     * would lie.  The fsck manpage/README document -f. */
    if (o->is_clean(fs) &&
        (mode & (FILSYS_CK_PREEN | FILSYS_CK_YES | FILSYS_CK_ASK)) &&
        !(mode & (FILSYS_CK_SALVAGE | FILSYS_CK_FORCE))) {
        if (!(mode & FILSYS_CK_QUIET))
            printf("filesystem clean; skipped\n");
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
        if (o->inode->read_inode(fs, ino, &ip)) {
            printf("inode %u unreadable\n", ino);
            rep->errors++;
            continue;
        }
        uint8_t st = o->inode->inode_state(fs, ino, ip.mode);
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
        filsys_mark_blocks(fs, &ip, ino, &cx);
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

    /* 4b. superblock free-space totals (System III/V fsck's "FREE BLK COUNT
     * WRONG IN SUPERBLK" / "FREE INODE COUNT WRONG IN SUPERBLK").  Only the
     * free-list editions whose superblock actually carries s_tfree/s_tinode:
     * the pack4/magic-free s5fs forms recompute them (their on-disk words are
     * unmaintained), V6 has 16-bit words, V1/PDP-7 and the V8-family use other
     * allocators.  Compare the walked counts against the on-disk totals. */
    if (fmt->alloc == &freelist_alloc_ops && !fmt->sb_decode &&
        !fmt->isize_count && (!fmt->pack4 || fmt->magic)) {
        if (rep->free_blocks != fs->fl.tfree) {
            printf("free block count wrong in superblock: %u free but s_tfree=%u\n",
                   rep->free_blocks, fs->fl.tfree);
            rep->errors++;
        }
        if (rep->inodes - rep->used_inodes != fs->fl.tinode) {
            printf("free inode count wrong in superblock: %u free but s_tinode=%u\n",
                   rep->inodes - rep->used_inodes, fs->fl.tinode);
            rep->errors++;
        }
    }

    /* 5. dcheck: directory link counts. */
    uint8_t *ecount = calloc(maxino + 1, 1);
    if (ecount) {
        for (uint32_t ino = 1; ino <= maxino; ino++) {
            filsys_inode_t ip;
            if (o->inode->read_inode(fs, ino, &ip))
                continue;
            if (state[ino] != FILSYS_IN_IDIR)
                continue;
            filsys_dirent_t *ents = NULL;
            size_t cnt = 0;
            if (o->dir->dir_read(fs, &ip, &ents, &cnt) == 0) {
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
            if (o->inode->read_inode(fs, ino, &ip))
                continue;
            int cnt = ecount[ino] & 0377;
            /* Any allocated inode that is not a directory must carry a directory
             * entry; one with zero entries is an orphan (System III fsck's "UNREF
             * FILE").  Tested by exclusion -- !fs_is_dir -- so it holds for every
             * file type a future edition might add (regular, symlink, FIFO,
             * socket, char/block/multiplexed device), not just the ones is_regular
             * names today.  It fires whatever nlink claims: hard_remove leaves
             * unlink-while-open files here (allocated, nlink == 0) until the last
             * close, and the nlink equality below would pass (0 == 0) and hide it.
             * Directories are excluded: their link count is a subdir count, not an
             * entry count, and their orphan state is the preen reconnect path.
             * The allocated guard matters: a mode-0 (free) inode is not an orphan. */
            if (filsys_in_allocated(state[ino]) && !fs_is_dir(fmt, &ip) &&
                cnt == 0 && ino != fmt->rootino && ino != fmt->badino) {
                printf("%u entries=0 link=%d (unreferenced)\n", ino, ip.nlink);
                rep->errors++;
                continue;
            }
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
            filsys_preen(fs, ecount, state, maxino, mode);
        free(ecount);
    }

    free(state);
    free(cx.owner);
    free(cx.bmap);

    if (!(mode & FILSYS_CK_QUIET))
        printf("used blocks=%u  free blocks=%u  missing=%u  dup=%u  inodes=%u/%u used  errors=%u\n",
               rep->used_blocks, rep->free_blocks, rep->missing_blocks,
               rep->dup_blocks, rep->used_inodes, rep->inodes, rep->errors);
    if (rep->missing_blocks > 0)
        printf("missing=%u: the free-block list is incomplete -- the dump(8)/restor(8) tape\n"
               "signature (dump writes the i-list and data blocks but NOT the free list, so a\n"
               "restored image comes up with s_tfree correct and its free-block chain short).\n"
               "Rebuild the free list from the block map with `-s` (icheck -s).\n",
               rep->missing_blocks);
    return rep->errors ? -1 : 0;
}

/* ---- shared maintenance: ncheck / clri / preen / resolve-dups ------------ */

/* True if the inode is a regular file (not a directory or device).  V1/PDP-7
 * detect directories and devices through their callbacks; the V6/V7 family
 * derives both from the type bits (V6's regular file is type 0, so ifreg==0). */
static int is_regular(const filsys_edition_t *fs, const filsys_inode_t *ip) {
    if (fs->is_dir)
        return !fs->is_dir(fs, ip) && !fs->is_device(fs, ip);
    uint32_t t = ip->mode & fs->ifmt;
    return t == fs->ifreg || (fs->iflnk && t == fs->iflnk);   /* symlinks carry data too */
}

/* True if the inode is a device (char/block/multiplexed), whose addr[0] is a
 * device number rather than a block. */
static int is_device(const filsys_edition_t *fs, const filsys_inode_t *ip) {
    if (fs->is_device)
        return fs->is_device(fs, ip);
    uint32_t t = ip->mode & fs->ifmt;
    return t == fs->ifchr || t == fs->ifblk ||
           (fs->ifmpc && t == fs->ifmpc) || (fs->ifmpb && t == fs->ifmpb);
}

/* The on-disk mode for a freshly created recovery directory. */
static uint32_t dir_mode(const filsys_edition_t *fs) {
    if (fs->to_disk_mode)
        return fs->to_disk_mode(fs, 0777, FILSYS_FT_DIR);
    return 0777 | fs->ifdir;
}

/* Recursively walk the directory tree from `dirino`, printing the pathname(s)
 * of `target` and descending into subdirectories. */
static void ncheck_dir(filsys_edition_t *fs, uint32_t dirino, const char *prefix,
                       uint32_t target, int *found, int depth)
{
    if (depth > 64)
        return;                       /* guard against a directory cycle */
    filsys_inode_t ip;
    if (fs->ops->inode->read_inode(fs, dirino, &ip))
        return;
    if (!fs_is_dir(fs, &ip))
        return;
    filsys_dirent_t *ents = NULL;
    size_t cnt = 0;
    if (fs->ops->dir->dir_read(fs, &ip, &ents, &cnt))
        return;
    for (size_t i = 0; i < cnt; i++) {
        uint32_t eino = ents[i].ino;
        if (eino == 0)
            continue;
        if (ents[i].name[0] == '.' &&
            (ents[i].name[1] == 0 ||
             (ents[i].name[1] == '.' && ents[i].name[2] == 0)))
            continue;                 /* skip "." and ".." */
        char path[1024];
        if (prefix[1] == 0)           /* prefix is "/" */
            snprintf(path, sizeof(path), "/%s", ents[i].name);
        else
            snprintf(path, sizeof(path), "%s/%s", prefix, ents[i].name);
        if (eino == target) {
            printf("%u\t%s\n", target, path);
            *found = 1;
        }
        filsys_inode_t cip;
        if (fs->ops->inode->read_inode(fs, eino, &cip) == 0 && fs_is_dir(fs, &cip))
            ncheck_dir(fs, eino, path, target, found, depth + 1);
    }
    free(ents);
}

int filsys_ncheck(filsys_edition_t *fs, uint32_t ino)
{
    int found = 0;
    ncheck_dir(fs, fs->rootino, "/", ino, &found, 0);
    if (!found)
        printf("%u: not found\n", ino);
    return 0;
}

int filsys_clri(filsys_edition_t *fs, uint32_t ino)
{
    uint32_t maxino = fs->ops->maxino(fs);
    if (ino == 0 || ino > maxino)
        return -EINVAL;
    filsys_inode_t ip;
    memset(&ip, 0, sizeof(ip));
    ip.ino = ino;
    int rc = fs->ops->inode->write_inode(fs, ino, &ip);
    if (rc == 0)
        printf("cleared inode %u\n", ino);
    return rc;
}

/* Count directory entries that name `target` -- a directory's link count is the
 * number of entries pointing at it: its own "." and ".." plus each child
 * directory's "..".  Preen's reconnects repoint orphan ".." entries, which the
 * stale ecount from the dcheck pass can no longer predict, so root's and
 * lost+found's counts are recomputed from the final structure this way. */
static uint32_t count_links_to(filsys_edition_t *fs, uint32_t target, uint32_t maxino)
{
    uint32_t cnt = 0;
    for (uint32_t ino = 1; ino <= maxino; ino++) {
        filsys_inode_t ip;
        if (fs->ops->inode->read_inode(fs, ino, &ip) != 0)
            continue;
        if (!fs_is_dir(fs, &ip))
            continue;
        filsys_dirent_t *ents = NULL;
        size_t n = 0;
        if (fs->ops->dir->dir_read(fs, &ip, &ents, &n) != 0)
            continue;
        for (size_t e = 0; e < n; e++) {
            if (fs->synth_dot && ents[e].name[0] == '.' &&
                (ents[e].name[1] == 0 ||
                 (ents[e].name[1] == '.' && ents[e].name[2] == 0)))
                continue;   /* synthesized "." / ".." (PDP-7) */
            if (ents[e].ino == target)
                cnt++;
        }
        free(ents);
    }
    return cnt;
}

/* Preen: fix the safe subset without prompting.  Ensures lost+found exists
 * (named "lostfils" where 8-char names can't hold "lost+found"), clears
 * free-but-referenced inodes, reconnects orphaned regular files, and corrects
 * link counts. */
void filsys_preen(filsys_edition_t *fs, const uint8_t *ecount, const uint8_t *state,
                  uint32_t maxino, int mode)
{
    const char *lfname = fs->max_namlen < 10 ? "lostfils" : "lost+found";
    filsys_inode_t root;
    uint32_t lf_ino = 0;
    if (fs->ops->inode->read_inode(fs, fs->rootino, &root) == 0 &&
        fs->ops->dir_lookup(fs, &root, lfname, &lf_ino) != 0) {
        filsys_inode_t lf;
        if (fs->ops->ialloc(fs, &lf_ino) == 0) {
            memset(&lf, 0, sizeof(lf));
            lf.ino = lf_ino;
            lf.mode = dir_mode(fs);
            lf.nlink = 2;
            fs->ops->inode->write_inode(fs, lf_ino, &lf);
            if (!fs->synth_dot) {     /* PDP-7 synthesizes "." / ".." */
                fs->ops->dir->dir_add(fs, &lf, lf_ino, ".");
                fs->ops->dir->dir_add(fs, &lf, fs->rootino, "..");
            }
            fs->ops->dir->dir_add(fs, &root, lf_ino, lfname);
            root.nlink++;
            fs->ops->inode->write_inode(fs, fs->rootino, &root);
            printf("created lost+found (inode %u)\n", lf_ino);
        }
    }

    for (uint32_t ino = 1; ino <= maxino; ino++) {
        int cnt = ecount[ino] & 0377;
        filsys_inode_t ip;
        if (fs->ops->inode->read_inode(fs, ino, &ip) != 0)
            continue;
        if (!filsys_in_allocated(state[ino])) {
            if (cnt != 0 && filsys_query(mode, "clear free-but-referenced inode %u", ino)) {
                ip.mode = 0;
                ip.nlink = 0;
                fs->ops->inode->write_inode(fs, ino, &ip);
                printf("cleared free-but-referenced inode %u\n", ino);
            }
            continue;
        }
        /* An allocated inode with no directory entry is an orphan (hard_remove's
         * unlink-while-open leaves nlink == 0 here); don't let the nlink-equality
         * shortcut (0 == 0) skip it before the reconnect below. */
        if (cnt == ip.nlink && cnt != 0)
            continue;
        if (ino == fs->rootino || ino == lf_ino || ino == fs->badino)
            continue;   /* root/lost+found: nlink just set, ecount stale; the
                         * bad-block inode is nameless by design (nlink 0, no entry) */
        if (cnt == 0) {
            if (lf_ino == 0)
                continue;
            if (!is_regular(fs, &ip)) {
                /* A non-data orphan (FIFO, socket, device, unknown type) has
                 * nothing to preserve: clear the inode, as System III fsck does
                 * for a zero-length UNREF inode.  A directory never lands here
                 * (its own "." entry is always counted). */
                if (filsys_query(mode, "clear unreferenced inode %u", ino)) {
                    ip.mode = 0;
                    ip.nlink = 0;
                    fs->ops->inode->write_inode(fs, ino, &ip);
                    printf("cleared unreferenced inode %u\n", ino);
                }
                continue;
            }
            filsys_inode_t lf;
            char name[16];
            snprintf(name, sizeof(name), "%u", ino);
            if (filsys_query(mode, "reconnect inode %u to lost+found", ino) &&
                fs->ops->inode->read_inode(fs, lf_ino, &lf) == 0 &&
                fs->ops->dir->dir_add(fs, &lf, ino, name) == 0) {
                ip.nlink = 1;
                fs->ops->inode->write_inode(fs, ino, &ip);
                fs->ops->inode->write_inode(fs, lf_ino, &lf);
                printf("reconnected inode %u to lost+found\n", ino);
            }
        } else if (cnt == 1 && state[ino] == FILSYS_IN_IDIR && !fs->synth_dot &&
                   lf_ino != 0) {
            /* An orphaned directory: its only entry is its own "." (cnt == 1),
             * and its ".." still points at a parent that no longer lists it.
             * Reconnect it to lost+found and repoint ".." so the old parent's
             * entry count drops back -- a dangling ".." would otherwise leave
             * the parent (often root) one link low, which nothing else repairs. */
            filsys_inode_t lf;
            char name[16];
            snprintf(name, sizeof(name), "%u", ino);
            if (filsys_query(mode, "reconnect dir inode %u to lost+found", ino) &&
                fs->ops->inode->read_inode(fs, lf_ino, &lf) == 0 &&
                fs->ops->dir->dir_remove(fs, &ip, "..") == 0 &&
                fs->ops->dir->dir_add(fs, &ip, lf_ino, "..") == 0 &&
                fs->ops->dir->dir_add(fs, &lf, ino, name) == 0) {
                ip.nlink = 2;
                fs->ops->inode->write_inode(fs, ino, &ip);
                lf.nlink++;
                fs->ops->inode->write_inode(fs, lf_ino, &lf);
                printf("reconnected dir inode %u to lost+found\n", ino);
            }
        } else {
            if (fs->synth_dot && !is_regular(fs, &ip))
                continue;   /* PDP-7: dir/device nlink is synthesized */
            int old = ip.nlink;
            if (filsys_query(mode, "fix link count of inode %u from %d to %d",
                             ino, old, cnt)) {
                ip.nlink = (int16_t)(cnt & 0377);
                fs->ops->inode->write_inode(fs, ino, &ip);
                printf("inode %u link count %d -> %d\n", ino, old, ip.nlink);
            }
        }
    }

    /* Root and lost+found were skipped above because their link counts depend on
     * the ".." entries reconnects just repointed; recompute them from the final
     * directory structure (the dcheck's ecount is stale by now). */
    if (!fs->synth_dot) {
        uint32_t targets[2] = { fs->rootino, lf_ino };
        for (size_t t = 0; t < 2; t++) {
            if (targets[t] == 0)
                continue;
            filsys_inode_t ip;
            if (fs->ops->inode->read_inode(fs, targets[t], &ip) != 0)
                continue;
            uint32_t want = count_links_to(fs, targets[t], maxino);
            if (want != (uint32_t)ip.nlink) {
                printf("inode %u link count %d -> %u\n", targets[t], ip.nlink, want);
                ip.nlink = (int16_t)want;
                fs->ops->inode->write_inode(fs, targets[t], &ip);
            }
        }
    }
}

/* Resolve duplicate blocks (salv -a): copy each block referenced twice to a
 * fresh block and re-point the second reference, then rebuild the free list. */
int filsys_resolve_dups(filsys_edition_t *fs)
{
    uint32_t maxino = fs->ops->maxino(fs);
    uint32_t dstart = fs->ops->data_start(fs);
    uint32_t dend   = fs->ops->data_end(fs);
    uint32_t nblk   = dend - dstart;

    filsys_chkctx_t cx;
    memset(&cx, 0, sizeof(cx));
    cx.nblk = nblk;
    cx.bmap = calloc((nblk + 7) / 8, 1);
    if (!cx.bmap)
        return -ENOMEM;

    struct dup { uint32_t blk, ino; int idx; };
    struct dup *dups = NULL;
    size_t ndup = 0, cap = 0;

    for (uint32_t ino = 1; ino <= maxino; ino++) {
        filsys_inode_t ip;
        if (fs->ops->inode->read_inode(fs, ino, &ip))
            continue;
        if (ip.mode == 0)
            continue;
        if (is_device(fs, &ip))
            continue;   /* device inode: addr[0] is a device number */
        cx.ino = ino;
        for (int i = 0; i < fs->niaddr; i++) {
            uint32_t a = ip.addr[i];
            if (a == 0)
                continue;
            int level = filsys_slot_level(fs, ip.mode, i);
            if (level >= 0) {
                filsys_mark_tree(fs, &cx, a, level);
                continue;
            }
            if (a < dstart || a >= dend) {
                printf("block %u bad; inode=%u\n", a, ino);
                continue;
            }
            uint32_t off = a - dstart;
            uint8_t  m = (uint8_t)(1u << (off & 7));
            if (cx.bmap[off >> 3] & m) {
                if (ndup == cap) {
                    size_t ncap = cap ? cap * 2 : 16;
                    struct dup *nb = realloc(dups, ncap * sizeof(*nb));
                    if (!nb) {
                        printf("out of memory tracking duplicates\n");
                        free(dups);
                        free(cx.bmap);
                        return -ENOMEM;
                    }
                    dups = nb;
                    cap = ncap;
                }
                dups[ndup].blk = a;
                dups[ndup].ino = ino;
                dups[ndup].idx = i;
                ndup++;
            } else {
                cx.bmap[off >> 3] |= m;
                cx.used_blocks++;
            }
        }
    }

    if (ndup == 0) {
        printf("no duplicate blocks\n");
        free(cx.bmap);
        return 0;
    }

    printf("%zu duplicate block(s); rebuilding free list\n", ndup);
    fs->ops->makefree(fs, &cx);

    int resolved = 0;
    for (size_t k = 0; k < ndup; k++) {
        uint32_t blk = dups[k].blk, ino = dups[k].ino;
        int idx = dups[k].idx;
        filsys_inode_t ip;
        if (fs->ops->inode->read_inode(fs, ino, &ip))
            continue;
        if (ip.addr[idx] != blk)
            continue;
        uint32_t nb;
        if (fs->alloc->balloc(fs, &nb)) {
            printf("block %u dup; inode=%u: out of space\n", blk, ino);
            continue;
        }
        uint8_t buf[V7_MAXBSIZE];
        if (fs->ops->read_block(fs, blk, buf) || fs->ops->write_block(fs, nb, buf)) {
            printf("block %u dup; inode=%u: copy failed\n", blk, ino);
            continue;
        }
        ip.addr[idx] = nb;
        fs->ops->inode->write_inode(fs, ino, &ip);
        uint32_t off = nb - dstart;
        cx.bmap[off >> 3] |= (uint8_t)(1u << (off & 7));
        printf("block %u dup; inode=%u: copied to %u\n", blk, ino, nb);
        resolved++;
    }
    free(dups);

    printf("resolved %d/%zu duplicates; finalizing free list\n", resolved, ndup);
    fs->ops->makefree(fs, &cx);
    free(cx.bmap);
    return resolved == (int)ndup ? 0 : -1;
}

