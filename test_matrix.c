/* test_matrix.c - the regression suite: exercise every backend through the
 * library at the sizes and boundaries that have actually shipped bugs, then
 * run fsck and require a clean filesystem.
 *
 * Per format the suite writes and reads back a set of sizes that straddle the
 * direct-block / indirect-block promotion boundary (`direct`) and the odd-byte
 * packing cases (a half-full trailing word), checks the reported size matches,
 * rejects the one-byte-past-the-ceiling write, cycles truncate down/up/zero and
 * requires fsck to report errors=0 missing=0 dup=0 afterward.
 *
 * This is the matrix the earlier releases lacked; it would have caught the
 * PDP-7 large-file break, the odd-byte size bug, the truncate/delete block
 * leak, and the allocator zeroing leak before they shipped.
 *
 * SPDX-License-Identifier: ISC
 */
#include <config.h>
#include "filsys.h"
#include "v7fs.h"
#include "filsys_ops.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <setjmp.h>
#include <sys/wait.h>

static int failures;

static void ok(const char *what, int cond) {
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond)
        failures++;
}

/* Run fsck -f and return whether it reports a clean filesystem (no errors, no
 * missing, no duplicate blocks). */
static int fsck_is_clean(const char *name, const char *img) {
    char cmd[512], out[4096] = "";
    snprintf(cmd, sizeof cmd, "./fsck.filsys -f -v %s %s 2>&1", name, img);
    FILE *p = popen(cmd, "r");
    if (p) {
        (void)!fread(out, 1, sizeof out - 1, p);
        pclose(p);
    }
    return strstr(out, "errors=0") && strstr(out, "missing=0") && strstr(out, "dup=0");
}

/* Run fsck -f and return whether it reports no duplicate blocks (the aliasing
 * invariant -- leaks are acceptable, but a block in two places is not). */
static int fsck_dup_zero(const char *name, const char *img) {
    char cmd[512], out[4096] = "";
    snprintf(cmd, sizeof cmd, "./fsck.filsys -f -v %s %s 2>&1", name, img);
    FILE *p = popen(cmd, "r");
    if (p) {
        (void)!fread(out, 1, sizeof out - 1, p);
        pclose(p);
    }
    return strstr(out, "dup=0") != NULL;
}

/* Run preen (-p -f: reconnect orphans, fix link counts) then salvage (-s:
 * rebuild the free list), then fsck -f; return whether the repaired filesystem
 * is clean.  The two families need different tools: an orphan is a namespace
 * defect (preen), a leaked block is a free-space defect (salvage); running both
 * in this order is safe because salvage roots in the i-list.  Neither exit code
 * is the signal -- preen/salvage exit non-zero when they *find* (and fix). */
static int fsck_recover_clean(const char *name, const char *img) {
    char cmd[512];
    int rc;
    snprintf(cmd, sizeof cmd, "./fsck.filsys -p -f -v %s %s >/dev/null 2>&1", name, img);
    rc = system(cmd);
    snprintf(cmd, sizeof cmd, "./fsck.filsys -s -v %s %s >/dev/null 2>&1", name, img);
    rc = system(cmd);
    (void)rc;
    return fsck_is_clean(name, img);
}

/* Run fsck -f and extract its free-block and free-inode counts (fsck computes
 * both by walking the free list and i-list, independent of any superblock
 * totals).  Returns 0 and sets *fblocks and *finodes, or -1 if it cannot parse the
 * summary line. */
static int fsck_free_counts(const char *name, const char *img,
                            uint64_t *fblocks, uint64_t *finodes) {
    char cmd[512], out[8192] = "";
    snprintf(cmd, sizeof cmd, "./fsck.filsys -f -v %s %s 2>&1", name, img);
    FILE *p = popen(cmd, "r");
    if (!p)
        return -1;
    (void)!fread(out, 1, sizeof out - 1, p);
    pclose(p);

    char *fb = strstr(out, "free blocks=");
    unsigned long fbv = 0, used = 0, total = 0;
    if (!fb || sscanf(fb, "free blocks=%lu", &fbv) != 1)
        return -1;
    char *ino = strstr(out, "inodes=");
    if (!ino || sscanf(ino, "inodes=%lu/%lu used", &used, &total) != 2)
        return -1;
    *fblocks = fbv;
    *finodes = total - used;
    return 0;
}

/* One row per format.  Everything here is derived from the shared edition table
 * (filsys_format_nth) and the descriptor: the mkfs block count (0 = the fixed
 * PDP-7 size), the direct-block capacity (ndaddr * bsize -- the byte count at
 * which a write forces large-file/indirect mode), and the size-field ceiling
 * (ops->max_file). */
struct fmt {
    int         edition;
    const char *name;
    int         blocks;
    uint64_t    maxfile;
    uint64_t    direct;
    uint32_t    bsize;
    int         iflnk;
    int         noprobe;
};

/* Fill one row from the shared table; returns 0 past the end, -1 to skip a
 * format that is not mkfs-able yet (nomkfs), 1 on success. */
static int fmt_at(size_t i, struct fmt *out) {
    const filsys_format_t *f = filsys_format_nth(i);
    if (!f)
        return 0;
    filsys_edition_t desc = filsys_getformat(f->edition);
    if (!desc.ops)
        return 0;
    if (desc.nomkfs)
        return -1;
    out->edition = f->edition;
    out->name    = f->name;
    out->blocks  = (f->edition == FILSYS_PDP7) ? 0 : 4000;
    out->direct  = (uint64_t)desc.ndaddr * desc.bsize;
    out->bsize   = desc.bsize;
    out->iflnk   = desc.iflnk != 0;
    out->maxfile = desc.ops->max_file(&desc);
    out->noprobe = desc.noprobe;
    return 1;
}

/* Create `path`, write `size` bytes of a deterministic 7-bit pattern (the
 * PDP-7 packs two 7-bit chars per word), read them back, and require the
 * round-trip and the reported size to match.  Returns 1 on success. */
static int write_verify(filsys_t *fs, const char *path, uint64_t size) {
    uint8_t *buf = malloc(size ? size : 1);
    uint8_t *back = malloc(size ? size : 1);
    if (!buf || !back) {
        free(buf); free(back);
        return 0;
    }
    for (uint64_t i = 0; i < size; i++)
        buf[i] = (uint8_t)(i & 0x7f);   /* 7-bit: survives PDP-7 packing */

    filsys_create(fs, path, 0644, 0, 0);
    if (filsys_write(fs, path, buf, (size_t)size, 0) != (int)size) {
        free(buf); free(back);
        return 0;
    }
    memset(back, 0, size);
    if (filsys_read(fs, path, back, (size_t)size, 0) != (int)size ||
        memcmp(buf, back, size) != 0) {
        free(buf); free(back);
        return 0;
    }

    uint32_t ino;
    filsys_inode_t ip;
    struct stat st;
    if (filsys_lookup(fs, path, &ino, &ip) != 0) {
        free(buf); free(back);
        return 0;
    }
    filsys_fill_stat(fs, &ip, &st);
    free(buf); free(back);
    return (uint64_t)st.st_size == size;
}

/* st_blocks (512-byte units) for `path`, or -1 on lookup failure. */
static long stat_blocks(filsys_t *fs, const char *path) {
    uint32_t ino;
    filsys_inode_t ip;
    struct stat st;
    if (filsys_lookup(fs, path, &ino, &ip))
        return -1;
    filsys_fill_stat(fs, &ip, &st);
    return (long)st.st_blocks;
}

static void run(const struct fmt *f) {
    char img[64], cmd[512];
    snprintf(img, sizeof img, "test_matrix_%s.img", f->name);
    unlink(img);

    if (f->blocks)
        snprintf(cmd, sizeof cmd, "./mkfs.filsys -v %s %s %d >/dev/null 2>&1",
                 f->name, img, f->blocks);
    else
        snprintf(cmd, sizeof cmd, "./mkfs.filsys -v %s %s >/dev/null 2>&1",
                 f->name, img);
    if (system(cmd) != 0) {
        ok(f->name, 0);
        fprintf(stderr, "  mkfs failed\n");
        return;
    }

    filsys_t *fs;
    if (filsys_open(&fs, f->edition, img, 0, 0, 0, 0, NULL)) {
        ok(f->name, 0);
        fprintf(stderr, "  open failed\n");
        unlink(img);
        return;
    }

    /* A second read-write open of the same image must be refused by the
     * advisory lock (two allocators would hand out the same block).  The lock
     * guards against a second *process* -- OFD locks (Linux) are
     * per-open-description, but POSIX record locks (the BSDs/macOS) are
     * per-process, so a same-process double-open cannot be detected there.
     * Fork a child to exercise the cross-process guarantee, which every host's
     * lock model enforces. */
    {
        pid_t pid = fork();
        if (pid == 0) {
            filsys_t *fs2 = NULL;
            _exit(filsys_open(&fs2, f->edition, img, 0, 0, 0, 0, NULL) == -EBUSY
                  ? 0 : 1);
        }
        int st = 0;
        if (pid > 0)
            waitpid(pid, &st, 0);
        ok("second RW open rejected (lock)",
           pid > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }

    /* The sizes that bit us: odd bytes (a half-full trailing word), the exact
     * direct->indirect promotion boundary, and a large write to force every
     * indirect level.  `large` is the exact size-field ceiling when that is
     * small enough to materialise (PDP-7, V1). */
    uint64_t large = f->maxfile < 200000 ? f->maxfile : 200000;
    uint64_t sizes[] = { 1, 127, f->direct - 1, f->direct, f->direct + 1, large };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        char what[96];
        snprintf(what, sizeof what, "%s write/read %lluB", f->name,
                 (unsigned long long)sizes[i]);
        ok(what, write_verify(fs, "/big", sizes[i]));
        filsys_unlink(fs, "/big");
    }

    /* One byte past the size-field ceiling must fail EFBIG and leak nothing. */
    {
        char one = 'x';
        filsys_create(fs, "/big", 0644, 0, 0);
        ok("oversized write EFBIG",
           filsys_write(fs, "/big", &one, 1, (off_t)f->maxfile) == -EFBIG);
        filsys_unlink(fs, "/big");
    }

    /* Truncate cycles: write large, then shrink/grow/zero; the freed indirect
     * blocks must all return to the free list (fsck below reports missing=0). */
    {
        uint8_t *buf = malloc(large ? large : 1);
        memset(buf, 'T', large);
        filsys_create(fs, "/big", 0644, 0, 0);
        ok("truncate pre-write", filsys_write(fs, "/big", buf, (size_t)large, 0) == (int)large);
        ok("truncate down", filsys_truncate(fs, "/big", (off_t)(large / 2)) == 0);
        ok("truncate up", filsys_truncate(fs, "/big", (off_t)large) == 0);
        ok("truncate small", filsys_truncate(fs, "/big", 100) == 0);
        ok("truncate zero", filsys_truncate(fs, "/big", 0) == 0);
        filsys_unlink(fs, "/big");
        free(buf);
    }

    /* st_blocks: POSIX 512-byte units of blocks actually allocated, not the
     * size-derived fs-block count.  A hole (zero block address) allocates
     * nothing; an indirect block itself is allocated and must be counted. */
    {
        uint32_t bsize = f->bsize;
        uint8_t *b = malloc(bsize + 1);
        memset(b, 'a', bsize + 1);

        filsys_create(fs, "/s1", 0644, 0, 0);
        filsys_write(fs, "/s1", b, bsize, 0);
        ok("st_blocks one block (512-unit)",
           stat_blocks(fs, "/s1") == (long)((bsize + 511) / 512));

        filsys_create(fs, "/s2", 0644, 0, 0);
        filsys_write(fs, "/s2", b, bsize + 1, 0);
        ok("st_blocks two blocks",
           stat_blocks(fs, "/s2") == (long)((2 * bsize + 511) / 512));

        filsys_create(fs, "/s3", 0644, 0, 0);
        filsys_truncate(fs, "/s3", 1048576);
        ok("st_blocks sparse == 0", stat_blocks(fs, "/s3") == 0);

        /* direct+1 bytes forces one indirect block past the direct slots; that
         * block is allocated, so the count must exceed the direct data alone. */
        uint8_t *big = malloc(f->direct + 1);
        memset(big, 'b', f->direct + 1);
        filsys_create(fs, "/s4", 0644, 0, 0);
        filsys_write(fs, "/s4", big, f->direct + 1, 0);
        ok("st_blocks counts indirect block",
           stat_blocks(fs, "/s4") * 512 >= (long)(f->direct + bsize));
        free(big);
        free(b);
    }

    /* Symlinks: only the editions that carry IFLNK (V8 family, 2.11BSD). */
    {
        static const char tgt[] = "/some/target";
        char buf[64] = "";
        if (f->iflnk) {
            ok("symlink create", filsys_symlink(fs, tgt, "/lnk") == 0);
            ssize_t n = filsys_readlink(fs, "/lnk", buf, sizeof buf);
            ok("readlink target", n == (ssize_t)strlen(tgt) &&
               memcmp(buf, tgt, strlen(tgt)) == 0);

            /* readlink truncation is silent (POSIX: no trailing NUL, returns
             * min(target, bufsiz)).  Pin the equal / one-over / far-over
             * boundaries so a wrong-by-one in the clamp can't hide. */
            char tgt2[256], b[64];
            size_t lens[] = { sizeof b, sizeof b + 1, sizeof tgt2 - 1 };
            for (size_t i = 0; i < sizeof lens / sizeof lens[0]; i++) {
                size_t L = lens[i];
                for (size_t j = 0; j < L; j++)
                    tgt2[j] = (char)('a' + (j % 26));
                tgt2[L] = 0;
                char what[96];
                snprintf(what, sizeof what, "readlink truncate @%zu", L);
                if (filsys_symlink(fs, tgt2, "/lnk2") != 0) {
                    ok(what, 0);
                    continue;
                }
                ssize_t m = filsys_readlink(fs, "/lnk2", b, sizeof b);
                size_t want = L < sizeof b ? L : sizeof b;
                ok(what, m == (ssize_t)want && memcmp(b, tgt2, want) == 0);
                filsys_unlink(fs, "/lnk2");
            }
        } else {
            ok("symlink rejected (predates IFLNK)",
               filsys_symlink(fs, tgt, "/lnk") == -ENOSYS);
        }
    }

    /* Ownership that doesn't fit the 16-bit field must be rejected, not
     * silently truncated (a uid of 70000 would wrap to 4464). */
    ok("create rejects oversized uid",
       filsys_create(fs, "/biguid", 0644, 70000, 0) == -EINVAL);

    /* Ino-keyed read/write: an open descriptor reads by inode, not path. */
    {
        uint8_t wbuf[32], rbuf[32] = {0};
        for (int i = 0; i < 32; i++) wbuf[i] = (uint8_t)i;
        filsys_create(fs, "/ino", 0644, 0, 0);
        filsys_write(fs, "/ino", wbuf, sizeof wbuf, 0);
        uint32_t ino; filsys_inode_t ip;
        filsys_lookup(fs, "/ino", &ino, &ip);
        ok("read_ino round-trip",
           filsys_read_ino(fs, ino, rbuf, sizeof rbuf, 0) == (ssize_t)sizeof rbuf &&
           memcmp(wbuf, rbuf, sizeof wbuf) == 0);
        /* rename NOREPLACE: refuse to clobber, allow a fresh target */
        filsys_create(fs, "/ino2", 0644, 0, 0);
        ok("rename NOREPLACE over existing",
           filsys_rename(fs, "/ino", "/ino2", 1) == -EEXIST);
        ok("rename NOREPLACE fresh",
           filsys_rename(fs, "/ino", "/ino3", 1) == 0);
    }

    /* open -> unlink -> read (hard_remove): an inode survives its name until
     * the last open handle closes -- the data must stay readable after the
     * unlink and be freed on the final release, not before. */
    {
        struct statvfs st0;
        filsys_statfs(fs, &st0);   /* free blocks before (for the leak check) */
        uint8_t wbuf[32], rbuf[32] = {0};
        for (int i = 0; i < 32; i++) wbuf[i] = (uint8_t)(i + 1);
        filsys_create(fs, "/open", 0644, 0, 0);
        filsys_write(fs, "/open", wbuf, sizeof wbuf, 0);
        uint32_t ino; filsys_inode_t ip;
        filsys_lookup(fs, "/open", &ino, &ip);
        ok("open_ino tracks", filsys_open_ino(fs, ino) == 0);
        ok("unlink while open", filsys_unlink(fs, "/open") == 0);
        ok("read after unlink",
           filsys_read_ino(fs, ino, rbuf, sizeof rbuf, 0) == (ssize_t)sizeof rbuf &&
           memcmp(wbuf, rbuf, sizeof wbuf) == 0);
        ok("close_ino frees", filsys_close_ino(fs, ino) == 0);
        struct statvfs st1;
        filsys_statfs(fs, &st1);
        ok("unlink-open blocks freed", st1.f_bfree == st0.f_bfree);
    }

    /* open -> rename-over -> read (hard_remove): renaming /b over the open /a
     * must not free /a's inode -- that would strand the handle and let the next
     * create() reuse the number (the data-destroying bug).  The replaced inode
     * survives until close_ino. */
    {
        uint8_t ab[32], bb[32], rb[32] = {0};
        for (int i = 0; i < 32; i++) { ab[i] = 0x55; bb[i] = 0x2a; }   /* 7-bit: survives PDP-7 packing */
        filsys_create(fs, "/a", 0644, 0, 0);
        filsys_write(fs, "/a", ab, sizeof ab, 0);
        filsys_create(fs, "/b", 0644, 0, 0);
        filsys_write(fs, "/b", bb, sizeof bb, 0);
        uint32_t ino; filsys_inode_t ip;
        filsys_lookup(fs, "/a", &ino, &ip);
        ok("rename-over: open_ino", filsys_open_ino(fs, ino) == 0);
        ok("rename-over: rename", filsys_rename(fs, "/b", "/a", 0) == 0);
        ok("rename-over: read survives",
           filsys_read_ino(fs, ino, rb, sizeof rb, 0) == (ssize_t)sizeof rb &&
           memcmp(ab, rb, sizeof ab) == 0);
        uint32_t cino; filsys_inode_t cip;
        filsys_create(fs, "/c", 0644, 0, 0);
        filsys_lookup(fs, "/c", &cino, &cip);
        ok("rename-over: inode not reused", cino != ino);
        ok("rename-over: close frees", filsys_close_ino(fs, ino) == 0);
    }

    filsys_close(fs);
    ok("fsck clean", fsck_is_clean(f->name, img));
    unlink(img);
}

/* mkfs argument validation: reject a size the format cannot honor.  These are
 * deterministic and image-free. */
static void mkfs_validation(void) {
    /* V1: 7000 blocks overflows the superblock bitmaps (max is 6528). */
    unlink("test_matrix_v1big.img");
    int rc = system("./mkfs.filsys -v 1 test_matrix_v1big.img 7000 >/dev/null 2>&1");
    ok("v1 mkfs rejects oversized volume", rc != 0);
    unlink("test_matrix_v1big.img");

    /* PDP-7: any size argument is rejected (the RB09 geometry is fixed). */
    unlink("test_matrix_p7sized.img");
    rc = system("./mkfs.filsys -v 0 test_matrix_p7sized.img 500 >/dev/null 2>&1");
    ok("v0 mkfs rejects size argument", rc != 0);
    unlink("test_matrix_p7sized.img");
}

/* Fresh-mkfs cleanliness: mkfs at several volume sizes per format, then fsck
 * must report a clean filesystem.  The PDP-7's size is fixed by the RB09
 * geometry, so it gets one sizeless mkfs. */
static void mkfs_cleanliness(void) {
    /* sizes in blocks; all under V1's 6528-block superblock-bitmap ceiling */
    static const int sizes[] = { 100, 1000, 4000, 6500 };

    for (size_t i = 0; ; i++) {
        struct fmt f;
        int frc = fmt_at(i, &f);
        if (!frc)
            break;
        if (frc < 0)
            continue;

        if (f.edition == FILSYS_PDP7) {
            char img[64], cmd[512];
            snprintf(img, sizeof img, "test_matrix_%s_fixed.img", f.name);
            unlink(img);
            snprintf(cmd, sizeof cmd, "./mkfs.filsys -v %s %s >/dev/null 2>&1",
                     f.name, img);
            if (system(cmd) != 0) { ok("v0 mkfs", 0); unlink(img); continue; }
            ok("v0 fsck-clean (fixed size)", fsck_is_clean(f.name, img));
            unlink(img);
            continue;
        }

        for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
            char img[64], cmd[512], what[96];
            snprintf(img, sizeof img, "test_matrix_%s_%d.img", f.name, sizes[s]);
            unlink(img);
            snprintf(cmd, sizeof cmd, "./mkfs.filsys -v %s %s %d >/dev/null 2>&1",
                     f.name, img, sizes[s]);
            if (system(cmd) != 0) {
                snprintf(what, sizeof what, "%s mkfs @ %d blocks", f.name, sizes[s]);
                ok(what, 0);
                unlink(img);
                continue;
            }
            snprintf(what, sizeof what, "%s fsck-clean @ %d blocks", f.name, sizes[s]);
            ok(what, fsck_is_clean(f.name, img));
            unlink(img);
        }
    }
}

/* The badino invariant.  A descriptor whose mkfs reserves a nameless inode as
 * the bad-block file (v7 and its 32V/Coherent/Xenix/2.9BSD/SysIII/SysV
 * derivatives, plus 2.11BSD: inode 1, seeded with a regular-file mode and no
 * directory entry) is allocated yet unreferenced by construction.  check.c's
 * orphan report must exclude fmt->badino, or fsck flags every freshly-mkfs'd
 * image of those editions as having an unreferenced inode.  Pin both halves:
 * the reserved inode really is allocated (and only where the descriptor says
 * so), and fsck still reports the image clean (so it is not misread as an
 * orphan).  Editions whose descriptor leaves badino == 0 assert only the latter,
 * which fails if any nameless inode is seeded without the matching exclusion. */
static void badino_consistency(void) {
    for (size_t i = 0; ; i++) {
        struct fmt f;
        int frc = fmt_at(i, &f);
        if (!frc)
            break;
        if (frc < 0)
            continue;

        char img[64], cmd[512], what[96];
        snprintf(img, sizeof img, "test_matrix_%s_badino.img", f.name);
        unlink(img);
        if (f.blocks)
            snprintf(cmd, sizeof cmd, "./mkfs.filsys -v %s %s %d >/dev/null 2>&1",
                     f.name, img, f.blocks);
        else
            snprintf(cmd, sizeof cmd, "./mkfs.filsys -v %s %s >/dev/null 2>&1",
                     f.name, img);
        if (system(cmd) != 0) { ok("badino mkfs", 0); unlink(img); continue; }

        filsys_edition_t desc = filsys_getformat(f.edition);
        if (desc.badino) {
            filsys_t *fs;
            if (filsys_open(&fs, f.edition, img, 0, 0, 0, 0, NULL)) {
                ok("badino open", 0); unlink(img); continue;
            }
            filsys_inode_t ip;
            int rd = filsys_read_inode(fs, desc.badino, &ip);
            snprintf(what, sizeof what, "%s reserved badino %u allocated",
                     f.name, desc.badino);
            ok(what, rd == 0 && (ip.mode & desc.ifmt) == desc.ifreg);
            filsys_close(fs);
        }
        snprintf(what, sizeof what, "%s fsck-clean w/ nameless badino", f.name);
        ok(what, fsck_is_clean(f.name, img));
        unlink(img);
    }
}

/* V6 large files: a write past the seven single-indirect slots (7 * 256 = 1792
 * blocks) forces slot 7, the double-indirect slot.  Before the shared
 * block-tree walk, V6's checker walked slot 7 single-level, so the second-level
 * data blocks went unmarked and fsck reported them missing.  This pins the fix:
 * the walk must descend slot 7 as a double-indirect block. */
static void v6_large_file(void) {
    char img[64], cmd[256];
    snprintf(img, sizeof img, "test_matrix_v6big.img");
    unlink(img);
    snprintf(cmd, sizeof cmd, "./mkfs.filsys -v v6 %s 4000 >/dev/null 2>&1", img);
    if (system(cmd) != 0) { ok("v6 large mkfs", 0); unlink(img); return; }

    filsys_t *fs;
    if (filsys_open(&fs, FILSYS_V6, img, 0, 0, 0, 0, NULL)) {
        ok("v6 large open", 0); unlink(img); return;
    }

    size_t bytes = 2000 * 512;   /* 2000 blocks: past the 1792-block boundary */
    uint8_t *buf = malloc(bytes);
    uint8_t *back = malloc(bytes);
    int write_ok = 0, read_ok = 0;
    if (buf && back) {
        memset(buf, 'V', bytes);
        filsys_create(fs, "/big", 0644, 0, 0);
        write_ok = filsys_write(fs, "/big", buf, bytes, 0) == (int)bytes;
        if (write_ok) {
            memset(back, 0, bytes);
            read_ok = filsys_read(fs, "/big", back, bytes, 0) == (int)bytes &&
                      memcmp(buf, back, bytes) == 0;
        }
    }
    ok("v6 large write", write_ok);
    ok("v6 large read", read_ok);
    free(buf);
    free(back);
    filsys_close(fs);
    ok("v6 large fsck clean", fsck_is_clean("v6", img));
    unlink(img);
}

/* V7 triple indirect: the triple slot di_addr[12] is only reached past
 * 10 direct + 128 single + 128² double = 16522 blocks (~8.07 MiB).  run()'s
 * largest write (200000 bytes) stops in double-indirect, so this pins the third
 * level: write 9 MiB, read it back, and require fsck to still be clean. */
static void v7_triple_indirect(void) {
    char img[64], cmd[256];
    snprintf(img, sizeof img, "test_matrix_v7triple.img");
    unlink(img);
    snprintf(cmd, sizeof cmd, "./mkfs.filsys -v v7 %s 22000 >/dev/null 2>&1", img);
    if (system(cmd) != 0) { ok("v7 triple mkfs", 0); unlink(img); return; }

    filsys_t *fs;
    if (filsys_open(&fs, FILSYS_V7, img, 0, 0, 0, 0, NULL)) {
        ok("v7 triple open", 0); unlink(img); return;
    }

    size_t bytes = 9 * 1024 * 1024;   /* past 8.07 MiB: forces the triple slot */
    uint8_t *buf = malloc(bytes);
    uint8_t *back = malloc(bytes);
    int write_ok = 0, read_ok = 0;
    if (buf && back) {
        for (size_t i = 0; i < bytes; i++)
            buf[i] = (uint8_t)(i & 0xff);
        filsys_create(fs, "/big", 0644, 0, 0);
        write_ok = filsys_write(fs, "/big", buf, bytes, 0) == (int)bytes;
        if (write_ok) {
            memset(back, 0, bytes);
            read_ok = filsys_read(fs, "/big", back, bytes, 0) == (int)bytes &&
                      memcmp(buf, back, bytes) == 0;
        }
    }
    ok("v7 triple write", write_ok);
    ok("v7 triple read", read_ok);
    free(buf);
    free(back);
    filsys_close(fs);
    ok("v7 triple fsck clean", fsck_is_clean("v7", img));
    unlink(img);
}

/* Directory-entry name widths, asserted per edition at 14/30/63/64 characters.
 * This is the regression for the facade bug that hardcoded a 14-character
 * component buffer and rejected 2.11BSD's 63-character names before they
 * reached the backend.  It iterates the shared edition table (filsys_format_nth)
 * so every edition -- including PDP-7, which the earlier hand-written list
 * skipped -- is exercised, and each edition must accept names up to its limit
 * (8 for V1/PDP-7, 14 for the fixed-width formats, 63 for 2.11BSD) and reject
 * beyond it. */
static void namelength(void) {
    static const size_t lens[] = { 14, 30, 63, 64 };
    for (size_t i = 0; ; i++) {
        const filsys_format_t *f = filsys_format_nth(i);
        if (!f)
            break;
        filsys_edition_t desc = filsys_getformat(f->edition);
        if (!desc.ops)
            continue;
        size_t max = desc.max_namlen;

        char img[64], cmd[256];
        snprintf(img, sizeof img, "test_matrix_nl_%s.img", f->name);
        unlink(img);
        if (f->edition == FILSYS_PDP7)   /* fixed RB09 geometry: no size arg */
            snprintf(cmd, sizeof cmd, "./mkfs.filsys -v %s %s >/dev/null 2>&1",
                     f->name, img);
        else
            snprintf(cmd, sizeof cmd, "./mkfs.filsys -v %s %s 100 >/dev/null 2>&1",
                     f->name, img);
        if (system(cmd) != 0) { ok("namelength mkfs", 0); continue; }
        filsys_t *fs;
        if (filsys_open(&fs, f->edition, img, 0, 0, 0, 0, NULL)) {
            ok("namelength open", 0); unlink(img); continue;
        }
        for (size_t j = 0; j < sizeof lens / sizeof lens[0]; j++) {
            size_t L = lens[j];
            char path[80], what[96];
            path[0] = '/';
            memset(path + 1, 'a', L);
            path[L + 1] = 0;
            int want = (L <= max) ? 0 : -ENAMETOOLONG;
            int got = filsys_create(fs, path, 0644, 0, 0);
            snprintf(what, sizeof what, "%s %zu-char %s", f->name, L,
                     want == 0 ? "accepted" : "rejected");
            ok(what, got == want);
            if (got == 0) filsys_unlink(fs, path);
        }
        filsys_close(fs);
        unlink(img);
    }
}

/* Crash-consistency: allocator state must reach disk before any reference to an
 * allocated block.  Write a file (allocating blocks and inodes) and, *before*
 * the close that would flush the superblock/bitmap, run fsck: write_inode must
 * have already flushed the allocator, so no block is both free and referenced
 * (dup==0).  Without the fl_dirty flush this fails on every format: the batched
 * superblock leaves the allocated blocks listed free while the inode references
 * them. */
static void crash_consistency(void) {
    for (size_t i = 0; ; i++) {
        struct fmt f;
        int frc = fmt_at(i, &f);
        if (!frc)
            break;
        if (frc < 0)
            continue;
        char img[64], cmd[512], what[96];
        snprintf(img, sizeof img, "test_matrix_%s_crash.img", f.name);
        unlink(img);
        if (f.edition == FILSYS_PDP7)
            snprintf(cmd, sizeof cmd, "./mkfs.filsys -v %s %s >/dev/null 2>&1", f.name, img);
        else
            snprintf(cmd, sizeof cmd, "./mkfs.filsys -v %s %s %d >/dev/null 2>&1",
                     f.name, img, f.blocks);
        if (system(cmd) != 0) { ok("crash mkfs", 0); unlink(img); continue; }

        filsys_t *fs;
        if (filsys_open(&fs, f.edition, img, 0, 0, 0, 0, NULL)) {
            ok("crash open", 0); unlink(img); continue;
        }
        /* Enough blocks to overflow the free-list cache (50/100) and force
         * indirect blocks, so the allocator is genuinely exercised. */
        uint64_t size = f.direct * 12;
        uint8_t *buf = malloc(size ? size : 1);
        memset(buf, 'c', size);
        filsys_create(fs, "/big", 0644, 0, 0);
        int wr = filsys_write(fs, "/big", buf, (size_t)size, 0);
        free(buf);
        snprintf(what, sizeof what, "%s crash-consistency", f.name);
        ok(what, wr == (int)size && fsck_is_clean(f.name, img));
        filsys_close(fs);
        unlink(img);
    }
}

/* ---- fault injection ----------------------------------------------------- */

/* A byte-slice transport that fails exactly one read or write (the g_fail_at-th
 * one) with -EIO, then delegates to the real file io.  The mutation's error
 * handling must unwind the partial update to a state with dup==0; salvage must
 * then recover the leaks it is allowed to leave. */
typedef enum { FAIL_NONE, FAIL_WRITE, FAIL_READ } fail_mode_e;
static fail_mode_e g_fmode = FAIL_NONE;
static int g_fail_at;   /* 1-based index of the read/write to fail (0 = never) */
static int g_fcount;    /* reads/writes seen since (re)arm */

static int fault_read(filsys_edition_t *fs, void *buf, size_t n, off_t off) {
    if (g_fmode == FAIL_READ && g_fail_at > 0) {
        g_fcount++;
        if (g_fcount == g_fail_at)
            return -EIO;
    }
    return filsys_io_file.read(fs, buf, n, off);
}
static int fault_write(filsys_edition_t *fs, const void *buf, size_t n, off_t off) {
    if (g_fmode == FAIL_WRITE && g_fail_at > 0) {
        g_fcount++;
        if (g_fcount == g_fail_at)
            return -EIO;
    }
    return filsys_io_file.write(fs, buf, n, off);
}
static const filsys_io_t fault_io = { fault_read, fault_write };

/* One mutator: setup builds the preconditions with the fault disabled, op is
 * the single mutation driven at each failing boundary. */
typedef struct {
    const char *name;
    void (*setup)(filsys_t *fs);
    int  (*op)(filsys_t *fs);
} mutator_t;

static void setup_none(filsys_t *fs) { (void)fs; }
static void setup_file(filsys_t *fs) {   /* a 4096-byte (8-block) file exists */
    uint8_t buf[4096];
    memset(buf, 'a', sizeof buf);
    filsys_create(fs, "/f", 0644, 0, 0);
    filsys_write(fs, "/f", buf, sizeof buf, 0);
}
static void setup_dir(filsys_t *fs) { filsys_mkdir(fs, "/d", 0755, 0, 0); }

static int op_create(filsys_t *fs)   { return filsys_create(fs, "/f", 0644, 0, 0); }
static int op_mkdir(filsys_t *fs)    { return filsys_mkdir(fs, "/d", 0755, 0, 0); }
static int op_mknod(filsys_t *fs)    { return filsys_mknod(fs, "/c", S_IFCHR | 0644, (dev_t)0x0103, 0, 0); }
static int op_write(filsys_t *fs) {
    uint8_t buf[4096];
    memset(buf, 'b', sizeof buf);
    return filsys_write(fs, "/f", buf, sizeof buf, 0);
}
static int op_truncate(filsys_t *fs) { return filsys_truncate(fs, "/f", 0); }
static int op_unlink(filsys_t *fs)   { return filsys_unlink(fs, "/f"); }
static int op_rmdir(filsys_t *fs)    { return filsys_rmdir(fs, "/d"); }
static int op_link(filsys_t *fs)     { return filsys_link(fs, "/f", "/g"); }
static int op_symlink(filsys_t *fs)  { return filsys_symlink(fs, "/t", "/lnk"); }
static int op_rename(filsys_t *fs)   { return filsys_rename(fs, "/f", "/g", 0); }

/* rename with a replaced target, and a directory rename -- these exercise the
 * deferred-target-free and the parent-link/".." fixups that the plain
 * file->new-name rename does not reach. */
static void setup_file2(filsys_t *fs) {   /* /f and /g both exist */
    uint8_t buf[4096];
    memset(buf, 'a', sizeof buf);
    filsys_create(fs, "/f", 0644, 0, 0);
    filsys_write(fs, "/f", buf, sizeof buf, 0);
    filsys_create(fs, "/g", 0644, 0, 0);
    filsys_write(fs, "/g", buf, sizeof buf, 0);
}
static void setup_dir2(filsys_t *fs) {    /* /d1 and /d2 both exist (empty) */
    filsys_mkdir(fs, "/d1", 0755, 0, 0);
    filsys_mkdir(fs, "/d2", 0755, 0, 0);
}
static int op_rename_over(filsys_t *fs)     { return filsys_rename(fs, "/f", "/g", 0); }
static int op_rename_dir(filsys_t *fs)      { return filsys_rename(fs, "/d", "/e", 0); }
static int op_rename_dir_over(filsys_t *fs) { return filsys_rename(fs, "/d1", "/d2", 0); }

/* Deferred free (hard_remove close): /f is created, opened, then unlinked, so it
 * is pending; the op closes the last handle, driving free_deferred_ino's itrunc
 * / write_inode / ifree sequence through the fault and crash injectors. */
static uint32_t g_open_ino;
static void setup_open_unlink(filsys_t *fs) {
    uint8_t buf[512];
    memset(buf, 'a', sizeof buf);
    filsys_create(fs, "/f", 0644, 0, 0);
    filsys_write(fs, "/f", buf, sizeof buf, 0);
    filsys_inode_t ip;
    if (filsys_lookup(fs, "/f", &g_open_ino, &ip) == 0) {
        filsys_open_ino(fs, g_open_ino);
        filsys_unlink(fs, "/f");
    }
}
static int op_close_ino(filsys_t *fs) { return filsys_close_ino(fs, g_open_ino); }

/* Fail the 1st, 2nd, ... read/write of a single mutator and require, after every
 * injected failure: no aliasing (dup==0) and a salvage-recoverable filesystem
 * (fsck -s then errors==0).  The loop stops once an injection point does fewer
 * I/O ops than the fail index (i.e. the failure was never reached). */
static void fault_mutator(const struct fmt *f, const mutator_t *mut,
                          fail_mode_e mode, const char *label) {
    for (int n = 1; ; n++) {
        char img[64], cmd[512], what[128];
        snprintf(img, sizeof img, "test_matrix_%s_fault.img", f->name);
        unlink(img);
        if (f->edition == FILSYS_PDP7)
            snprintf(cmd, sizeof cmd, "./mkfs.filsys -v %s %s >/dev/null 2>&1",
                     f->name, img);
        else
            snprintf(cmd, sizeof cmd, "./mkfs.filsys -v %s %s %d >/dev/null 2>&1",
                     f->name, img, f->blocks);
        if (system(cmd) != 0) { ok("fault mkfs", 0); unlink(img); return; }

        filsys_t *fs;
        if (filsys_open(&fs, f->edition, img, 0, 0, 0, 0, NULL)) {
            ok("fault open", 0); unlink(img); return;
        }
        filsys_set_io(fs, &fault_io);
        mut->setup(fs);                 /* fault disabled */

        g_fmode = mode;
        g_fail_at = n;
        g_fcount = 0;
        (void)mut->op(fs);              /* fault enabled */
        int reached = g_fcount;         /* reads/writes the op performed */
        g_fmode = FAIL_NONE;
        filsys_close(fs);

        int dup_ok = fsck_dup_zero(f->name, img);
        int recov_ok = fsck_recover_clean(f->name, img);
        snprintf(what, sizeof what, "%s %s fail-%s-%d%s%s", f->name, mut->name, label, n,
                 dup_ok ? "" : " [dup]", recov_ok ? "" : " [recover]");
        ok(what, dup_ok && recov_ok);
        unlink(img);

        if (reached < n)                /* failure never reached: done */
            return;
    }
}

/* ---- test selection (developer iteration) --------------------------------
 *
 * The full soak (fault injection + crash-prefix enumeration + property
 * sequences) is minutes across every edition; iterating on one mutator is
 * painful.  Two env vars narrow it without a rebuild:
 *
 *   FILSYS_ONLY=crash|fault|property   run just that slow phase (skip the fast
 *                                      gate and the other phases)
 *   FILSYS_MUT=<substring>             within fault/crash, keep only mutators
 *                                      whose name contains the substring
 *
 * e.g. FILSYS_ONLY=crash FILSYS_MUT=rename runs only the crash-prefix
 * enumeration of the rename family. */
static const char *g_only;      /* FILSYS_ONLY */
static const char *g_mut;       /* FILSYS_MUT */
static int mut_selected(const char *name) {
    return !g_mut || strstr(name, g_mut) != NULL;
}

static void fault_test(void) {
    static const mutator_t mut[] = {
        { "create",   setup_none, op_create },
        { "mkdir",    setup_none, op_mkdir },
        { "mknod",    setup_none, op_mknod },
        { "write",    setup_file, op_write },
        { "truncate", setup_file, op_truncate },
        { "unlink",   setup_file, op_unlink },
        { "rmdir",    setup_dir,  op_rmdir },
        { "link",     setup_file, op_link },
        { "symlink",  setup_none, op_symlink },
        { "rename",   setup_file, op_rename },
        { "rename_over",     setup_file2, op_rename_over },
        { "rename_dir",      setup_dir,   op_rename_dir },
        { "rename_dir_over", setup_dir2,  op_rename_dir_over },
        { "close_ino",       setup_open_unlink, op_close_ino },
    };
    static const struct { fail_mode_e mode; const char *label; } modes[] = {
        { FAIL_WRITE, "write" },
        { FAIL_READ,  "read"  },
    };

    for (size_t i = 0; ; i++) {
        struct fmt f;
        int frc = fmt_at(i, &f);
        if (!frc)
            break;
        if (frc < 0)
            continue;
        for (size_t m = 0; m < sizeof mut / sizeof mut[0]; m++)
            if (mut_selected(mut[m].name))
                for (size_t mo = 0; mo < sizeof modes / sizeof modes[0]; mo++)
                    fault_mutator(&f, &mut[m], modes[mo].mode, modes[mo].label);
    }
}

/* ---- crash-prefix enumeration (soft updates) ----------------------------
 *
 * The fault-injection suite above samples the crash space (one injected EIO at
 * a time, with the mutation's error handling free to unwind): that proves the
 * *process-error* boundary.  The host-crash boundary is different -- the
 * process simply dies, so no error handling runs, and the on-disk state is
 * exactly the prefix of the write sequence that landed before the crash.
 *
 * A bounded mutation is only ~8-12 writes, so every prefix is enumerable: run
 * the op once cleanly to learn its write count W, then re-run it for each
 * k = 0..W, killing the process after k writes (longjmp out of the write
 * transport, before write k+1 lands).  Each image must be alias-free (dup==0,
 * soft-updates rule 2) and fsck-salvageable back to clean (rule 3 leaves only
 * recoverable leaks).  Complete over the operation, not a sample of it. */

static jmp_buf g_crash_jmp;
static int g_crash_at;      /* crash before this 1-based write (0 = never) */
static int g_crash_count;   /* writes seen since (re)arm */

static int crash_read(filsys_edition_t *fs, void *buf, size_t n, off_t off) {
    return filsys_io_file.read(fs, buf, n, off);
}
static int crash_write(filsys_edition_t *fs, const void *buf, size_t n, off_t off) {
    g_crash_count++;
    if (g_crash_at > 0 && g_crash_count == g_crash_at)
        longjmp(g_crash_jmp, 1);   /* crash: this write never lands */
    return filsys_io_file.write(fs, buf, n, off);
}
static const filsys_io_t crash_io = { crash_read, crash_write };

/* Closing after a crash must not flush: close clears s_fmod and re-writes the
 * superblock, which would stamp the in-memory (post-op) state over the crashed
 * image and mask the crash.  Swap to a discard transport first. */
static int discard_write(filsys_edition_t *fs, const void *buf, size_t n, off_t off) {
    (void)fs; (void)buf; (void)n; (void)off;
    return 0;
}
static const filsys_io_t discard_io = { crash_read, discard_write };

static void crash_mutator(const struct fmt *f, const mutator_t *mut) {
    char img[64], cmd[512], what[128];
    filsys_t *fs;

    /* learn the op's write count W on a clean run */
    snprintf(img, sizeof img, "test_matrix_%s_crash.img", f->name);
    unlink(img);
    if (f->edition == FILSYS_PDP7)
        snprintf(cmd, sizeof cmd, "./mkfs.filsys -v %s %s >/dev/null 2>&1", f->name, img);
    else
        snprintf(cmd, sizeof cmd, "./mkfs.filsys -v %s %s %d >/dev/null 2>&1",
                 f->name, img, f->blocks);
    if (system(cmd) != 0) { ok("crash mkfs", 0); unlink(img); return; }
    if (filsys_open(&fs, f->edition, img, 0, 0, 0, 0, NULL)) {
        ok("crash open", 0); unlink(img); return;
    }
    filsys_set_io(fs, &crash_io);
    g_crash_at = 0;
    mut->setup(fs);
    g_crash_count = 0;
    (void)mut->op(fs);
    int W = g_crash_count;
    filsys_close(fs);
    unlink(img);
    if (W == 0)
        return;

    /* enumerate every prefix k = 0..W */
    for (int k = 0; k <= W; k++) {
        unlink(img);
        if (f->edition == FILSYS_PDP7)
            snprintf(cmd, sizeof cmd, "./mkfs.filsys -v %s %s >/dev/null 2>&1", f->name, img);
        else
            snprintf(cmd, sizeof cmd, "./mkfs.filsys -v %s %s %d >/dev/null 2>&1",
                     f->name, img, f->blocks);
        if (system(cmd) != 0) { ok("crash mkfs", 0); unlink(img); return; }
        if (filsys_open(&fs, f->edition, img, 0, 0, 0, 0, NULL)) {
            ok("crash open", 0); unlink(img); return;
        }
        filsys_set_io(fs, &crash_io);
        g_crash_at = 0;
        mut->setup(fs);
        g_crash_count = 0;
        g_crash_at = k + 1;
        if (setjmp(g_crash_jmp) == 0)
            (void)mut->op(fs);   /* longjmps out on write k+1 (k < W) */
        filsys_set_io(fs, &discard_io);   /* close without flushing */
        filsys_close(fs);

        int dup_ok   = fsck_dup_zero(f->name, img);
        int recov_ok = fsck_recover_clean(f->name, img);
        snprintf(what, sizeof what, "%s %s crash-prefix-%d%s%s", f->name, mut->name, k,
                 dup_ok ? "" : " [dup]", recov_ok ? "" : " [recover]");
        ok(what, dup_ok && recov_ok);
        unlink(img);
    }
}

static void crash_prefix_test(void) {
    /* mkdir is deliberately absent: its crash prefixes orphan the new directory
     * (the inode and . / .. are written before the parent entry, per rule 1),
     * and fsck cannot auto-repair an orphan *directory* without a lost+found
     * phase.  That is a limit of the format -- V7 mkdir is non-atomic, and the
     * original relied on `sync` before halt -- not an aliasing defect: dup==0
     * still holds at every prefix.  mkdir's *error* path (where the rollback
     * runs) is covered by fault_test below. */
    static const mutator_t mut[] = {
        { "create",   setup_none, op_create },
        { "write",    setup_file, op_write },
        { "truncate", setup_file, op_truncate },
        { "unlink",   setup_file, op_unlink },
        { "rmdir",    setup_dir,  op_rmdir },
        { "link",     setup_file, op_link },
        { "symlink",  setup_none, op_symlink },
        { "rename",   setup_file, op_rename },
        { "rename_over",     setup_file2, op_rename_over },
        { "rename_dir",      setup_dir,   op_rename_dir },
        { "rename_dir_over", setup_dir2,  op_rename_dir_over },
        { "close_ino", setup_open_unlink, op_close_ino },
    };
    for (size_t i = 0; ; i++) {
        struct fmt f;
        int frc = fmt_at(i, &f);
        if (!frc)
            break;
        if (frc < 0)
            continue;
        for (size_t m = 0; m < sizeof mut / sizeof mut[0]; m++)
            if (mut_selected(mut[m].name))
                crash_mutator(&f, &mut[m]);
    }
}

/* ---- property-based operation sequences ----------------------------------
 *
 * The matrix above is hand-written, which by construction only finds bugs
 * someone thought to write a case for.  This drives *random* operation
 * histories against the library and a tiny in-memory model, then checks two
 * invariants after the dust settles: the image is fsck-clean, and every file
 * the model thinks exists has the size it should.  A deterministic PRNG keeps
 * the whole thing reproducible. */

static uint64_t g_prng;
static uint32_t prng_next(void) {
    g_prng ^= g_prng >> 12;
    g_prng ^= g_prng << 25;
    g_prng ^= g_prng >> 27;
    return (uint32_t)((g_prng * 0x2545F4914F6CDD1DULL) >> 32);
}

enum { NMODEL = 8, PROP_STEPS = 128 };
static uint64_t g_model[NMODEL];   /* file size; ~0ULL = absent */

static void property_sequences(void) {
    static const char *names[] = { "a", "b", "c", "d", "e", "f", "g", "h" };
    for (size_t i = 0; ; i++) {
        struct fmt f;
        int frc = fmt_at(i, &f);
        if (!frc)
            break;
        if (frc < 0)
            continue;
        char img[64], cmd[512], what[128];
        snprintf(img, sizeof img, "test_matrix_%s_prop.img", f.name);
        unlink(img);
        if (f.edition == FILSYS_PDP7)
            snprintf(cmd, sizeof cmd, "./mkfs.filsys -v %s %s >/dev/null 2>&1", f.name, img);
        else
            snprintf(cmd, sizeof cmd, "./mkfs.filsys -v %s %s %d >/dev/null 2>&1",
                     f.name, img, f.blocks);
        if (system(cmd) != 0) { ok("prop mkfs", 0); unlink(img); continue; }
        filsys_t *fs;
        if (filsys_open(&fs, f.edition, img, 0, 0, 0, 0, NULL)) {
            ok("prop open", 0); unlink(img); continue;
        }
        for (int j = 0; j < NMODEL; j++)
            g_model[j] = ~0ULL;
        g_prng = 0x9e3779b97f4a7c15ULL;

        for (int step = 0; step < PROP_STEPS; step++) {
            int slot = (int)(prng_next() % NMODEL);
            const char *nm = names[slot];
            int op = (int)(prng_next() % 5);
            uint64_t sz = (uint64_t)(prng_next() % 6) * 512;   /* 0..2560 */
            int rc = 0;
            uint8_t buf[512];
            memset(buf, 'a' + slot, sizeof buf);

            switch (op) {
            case 0:  /* create if absent, else overwrite block 0 */
                if (g_model[slot] == ~0ULL) {
                    rc = filsys_create(fs, nm, 0644, 0, 0);
                    if (rc == 0) g_model[slot] = 0;
                } else {
                    rc = filsys_write(fs, nm, buf, sizeof buf, 0);
                    if (g_model[slot] == 0 && rc == (int)sizeof buf)
                        g_model[slot] = sizeof buf;
                }
                break;
            case 1:  /* truncate to a new size */
                if (g_model[slot] != ~0ULL) {
                    rc = filsys_truncate(fs, nm, (off_t)sz);
                    if (rc == 0) g_model[slot] = sz;
                }
                break;
            case 2:  /* unlink */
                if (g_model[slot] != ~0ULL) {
                    rc = filsys_unlink(fs, nm);
                    if (rc == 0) g_model[slot] = ~0ULL;
                }
                break;
            case 3:  /* append one block past the current size */
                if (g_model[slot] != ~0ULL) {
                    rc = filsys_write(fs, nm, buf, sizeof buf, (off_t)g_model[slot]);
                    if (rc == (int)sizeof buf) g_model[slot] += sizeof buf;
                }
                break;
            case 4:  /* overwrite the whole current size */
                if (g_model[slot] != ~0ULL && g_model[slot] > 0) {
                    size_t n = (size_t)(g_model[slot] < 4096 ? g_model[slot] : 4096);
                    uint8_t *big = malloc(n ? n : 1);
                    memset(big, 'a' + slot, n);
                    rc = filsys_write(fs, nm, big, n, 0);
                    free(big);
                }
                break;
            }
            (void)rc;
        }
        filsys_close(fs);   /* flush the superblock, then check */

        int clean = fsck_is_clean(f.name, img);
        /* every surviving file must read back at the modelled size */
        int sizes_ok = 1;
        filsys_t *r;
        if (filsys_open(&r, f.edition, img, 1, 0, 0, 0, NULL) == 0) {
            for (int j = 0; j < NMODEL && sizes_ok; j++) {
                filsys_inode_t ip;
                uint32_t ino;
                if (g_model[j] == ~0ULL)
                    continue;
                if (filsys_lookup(r, names[j], &ino, &ip) != 0 ||
                    (uint64_t)ip.size != g_model[j])
                    sizes_ok = 0;
            }
            filsys_close(r);
        } else
            sizes_ok = 0;

        snprintf(what, sizeof what, "%s property-sequences", f.name);
        ok(what, clean && sizes_ok);
        unlink(img);
    }
}

/* Rename semantics: the POSIX type rules and the no-cycle rule.  Deterministic
 * (no fault injection): each rejected operation must return the right errno and
 * leave the filesystem clean.  The cycle rule walks ".." entries, which the
 * PDP-7 backend synthesizes (always pointing at the root, since a real PDP-7
 * directory stores no parent link), so that one check is skipped there. */
static void rename_semantics(void) {
    for (size_t i = 0; ; i++) {
        struct fmt f;
        int frc = fmt_at(i, &f);
        if (!frc)
            break;
        if (frc < 0)
            continue;
        char img[64], cmd[512], what[128];
        snprintf(img, sizeof img, "test_matrix_%s_ren.img", f.name);
        unlink(img);
        if (f.edition == FILSYS_PDP7)
            snprintf(cmd, sizeof cmd, "./mkfs.filsys -v %s %s >/dev/null 2>&1", f.name, img);
        else
            snprintf(cmd, sizeof cmd, "./mkfs.filsys -v %s %s %d >/dev/null 2>&1",
                     f.name, img, f.blocks);
        if (system(cmd) != 0) { ok("rename mkfs", 0); unlink(img); continue; }

        filsys_t *fs;
        if (filsys_open(&fs, f.edition, img, 0, 0, 0, 0, NULL)) {
            ok("rename open", 0); unlink(img); continue;
        }

        filsys_create(fs, "/f", 0644, 0, 0);
        filsys_mkdir(fs, "/d", 0755, 0, 0);

        snprintf(what, sizeof what, "%s rename file->dir EISDIR", f.name);
        ok(what, filsys_rename(fs, "/f", "/d", 0) == -EISDIR);
        snprintf(what, sizeof what, "%s rename dir->file ENOTDIR", f.name);
        ok(what, filsys_rename(fs, "/d", "/f", 0) == -ENOTDIR);

        if (f.edition != FILSYS_PDP7) {
            filsys_mkdir(fs, "/a", 0755, 0, 0);
            filsys_mkdir(fs, "/a/b", 0755, 0, 0);
            snprintf(what, sizeof what, "%s rename dir->descendant EINVAL", f.name);
            ok(what, filsys_rename(fs, "/a", "/a/b/c", 0) == -EINVAL);
            /* The cycle check must run before any target removal: an existing
             * target inside the source subtree is left untouched. */
            filsys_create(fs, "/a/b/t", 0644, 0, 0);
            uint32_t tino; filsys_inode_t tip;
            snprintf(what, sizeof what, "%s rename dir->descendant keeps target", f.name);
            ok(what, filsys_rename(fs, "/a", "/a/b/t", 0) == -EINVAL &&
                      filsys_lookup(fs, "/a/b/t", &tino, &tip) == 0);
        }

        filsys_close(fs);
        snprintf(what, sizeof what, "%s rename rejected clean", f.name);
        ok(what, fsck_is_clean(f.name, img));
        unlink(img);
    }
}

/* Historical Unix lets the superuser hard-link a directory, so a directory can
 * have several names.  The library must honor that: rmdir of one name only drops
 * a link, it frees the directory when the last name goes away; and a directory
 * must never be linked into its own subtree (a cycle).  PDP-7 is skipped -- its
 * directories store no parent link and its nlink is synthesized. */
static void dir_link_semantics(void) {
    for (size_t i = 0; ; i++) {
        struct fmt f;
        int frc = fmt_at(i, &f);
        if (!frc)
            break;
        if (frc < 0)
            continue;
        if (f.edition == FILSYS_PDP7)
            continue;
        char img[64], cmd[512], what[128];
        snprintf(img, sizeof img, "test_matrix_%s_dirlink.img", f.name);
        unlink(img);
        if (f.edition == FILSYS_PDP7)
            snprintf(cmd, sizeof cmd, "./mkfs.filsys -v %s %s >/dev/null 2>&1", f.name, img);
        else
            snprintf(cmd, sizeof cmd, "./mkfs.filsys -v %s %s %d >/dev/null 2>&1",
                     f.name, img, f.blocks);
        if (system(cmd) != 0) { ok("dirlink mkfs", 0); unlink(img); continue; }

        filsys_t *fs;
        if (filsys_open(&fs, f.edition, img, 0, 0, 0, 0, NULL)) {
            ok("dirlink open", 0); unlink(img); continue;
        }

        /* hard-link a directory, then remove one name at a time */
        filsys_mkdir(fs, "/d", 0755, 0, 0);
        snprintf(what, sizeof what, "%s link dir", f.name);
        ok(what, filsys_link(fs, "/d", "/e") == 0);
        uint32_t dino = 0, eino = 0;
        filsys_inode_t dip, eip;
        snprintf(what, sizeof what, "%s dir link same inode", f.name);
        ok(what, filsys_lookup(fs, "/d", &dino, &dip) == 0 &&
                  filsys_lookup(fs, "/e", &eino, &eip) == 0 && dino == eino);

        snprintf(what, sizeof what, "%s rmdir one link keeps the other", f.name);
        ok(what, filsys_rmdir(fs, "/d") == 0 && filsys_lookup(fs, "/e", &eino, &eip) == 0);

        snprintf(what, sizeof what, "%s rmdir last link frees it", f.name);
        ok(what, filsys_rmdir(fs, "/e") == 0 && filsys_lookup(fs, "/e", &eino, &eip) != 0);

        /* a directory may not be linked into its own subtree */
        filsys_mkdir(fs, "/a", 0755, 0, 0);
        filsys_mkdir(fs, "/a/b", 0755, 0, 0);
        snprintf(what, sizeof what, "%s link dir into descendant EINVAL", f.name);
        ok(what, filsys_link(fs, "/a", "/a/b/c") == -EINVAL);

        filsys_close(fs);
        snprintf(what, sizeof what, "%s dir-link clean", f.name);
        ok(what, fsck_is_clean(f.name, img));
        unlink(img);
    }
}

/* Durability loss: a successful shrink whose free-list flush never reaches disk
 * must not alias.  Write a large (multi-chain-reload) file, snapshot the
 * superblock, shrink it, splice the old superblock back (the shrink's bfree
 * flush "never happened"), and require: no aliasing (dup==0 -- the freed blocks
 * are "used but unreferenced", a leak), salvage recovers (errors==0), and the
 * retained half is byte-exact (data integrity, which metadata-only checks miss).
 * Free-list formats keep their cache in block 1; V1's bitmap and PDP-7's
 * free-list head are skipped here. */
static void durability_test(void) {
    for (size_t i = 0; ; i++) {
        struct fmt f;
        int frc = fmt_at(i, &f);
        if (!frc)
            break;
        if (frc < 0)
            continue;
        if (f.edition == FILSYS_V1 || f.edition == FILSYS_PDP7)
            continue;

        filsys_edition_t desc = filsys_getformat(f.edition);
        uint32_t bsize = desc.bsize;

        char img[64], sb[64], cmd[512], what[128];
        snprintf(img, sizeof img, "test_matrix_%s_dur.img", f.name);
        snprintf(sb,  sizeof sb,  "test_matrix_%s_dur.sb",  f.name);
        unlink(img); unlink(sb);
        snprintf(cmd, sizeof cmd, "./mkfs.filsys -v %s %s %d >/dev/null 2>&1",
                 f.name, img, f.blocks);
        if (system(cmd) != 0) { ok("dur mkfs", 0); unlink(img); continue; }

        filsys_t *fs;
        if (filsys_open(&fs, f.edition, img, 0, 0, 0, 0, NULL)) {
            ok("dur open", 0); unlink(img); unlink(sb); continue;
        }

        size_t full = bsize * 1000, half = full / 2;   /* multi-reload, then shrink */
        uint8_t *buf = malloc(full);
        for (size_t k = 0; k < full; k++)
            buf[k] = (uint8_t)(k % 251);
        filsys_create(fs, "/big", 0644, 0, 0);
        if (filsys_write(fs, "/big", buf, full, 0) != (int)full) {
            ok("dur write", 0); free(buf); filsys_close(fs); unlink(img); unlink(sb); continue;
        }

        /* snapshot the superblock AFTER the write, BEFORE the shrink */
        snprintf(cmd, sizeof cmd, "dd if=%s of=%s bs=1 skip=%u count=%u 2>/dev/null",
                 img, sb, bsize, bsize);
        if (system(cmd) != 0) { ok("dur snapshot", 0); free(buf); filsys_close(fs); unlink(img); unlink(sb); continue; }

        filsys_truncate(fs, "/big", (off_t)half);   /* itrunc: inode write before bfree */
        filsys_close(fs);

        /* the shrink's free-list flush never landed: splice the snapshot back */
        snprintf(cmd, sizeof cmd, "dd if=%s of=%s bs=1 seek=%u conv=notrunc 2>/dev/null",
                 sb, img, bsize);
        if (system(cmd) != 0) { ok("dur splice", 0); free(buf); unlink(img); unlink(sb); continue; }

        int dup_ok  = fsck_dup_zero(f.name, img);     /* no aliasing */
        int salv_ok = fsck_recover_clean(f.name, img); /* salvage -> clean */
        int data_ok = 0;                              /* retained half intact */
        filsys_t *r;
        if (filsys_open(&r, f.edition, img, 0, 0, 0, 0, NULL) == 0) {
            uint8_t *back = malloc(half);
            data_ok = filsys_read(r, "/big", back, half, 0) == (int)half &&
                      memcmp(buf, back, half) == 0;
            free(back);
            filsys_close(r);
        }
        snprintf(what, sizeof what, "%s durability", f.name);
        ok(what, dup_ok && salv_ok && data_ok);
        free(buf);
        unlink(img); unlink(sb);
    }
}

/* What findfs names each edition in its first-line report.  A few editions share
 * their on-disk superblock with another, so findfs reports the family rather than
 * the specific edition: System III's is 32V-shaped (no s_magic, 4-byte aligned),
 * and SVR4 adds only s_state to the SVR2 superblock.  (2.9BSD and 2.11BSD share
 * a superblock too, but findfs tells them apart by reading the root directory's
 * entry format -- see bsd211_root_dir.) */
static const char *findfs_name(int edition) {
    switch (edition) {
    case FILSYS_PDP7:     return "PDP-7";
    case FILSYS_V1:       return "v1";
    case FILSYS_V6:       return "v6";
    case FILSYS_V7:       return "v7";
    case FILSYS_32V:      return "32V";
    case FILSYS_COHERENT: return "Coherent";
    case FILSYS_XENIX:    return "Xenix";
    case FILSYS_BSD29:    return "2.9BSD";
    case FILSYS_BSD211:   return "2.11BSD";
    case FILSYS_SYSIII:   return "32V";
    case FILSYS_SVR2:     return "sysvr2";
    case FILSYS_SVR4:     return "sysvr4";
    case FILSYS_V8:       return "v8";
    case FILSYS_V9:       return "v9";
    case FILSYS_V10:      return "v10";
    default:              return NULL;
    }
}

/* findfs self-detection: for each edition, mkfs an image and require findfs's
 * *first* line to be that edition's validated hit at offset 0 -- not a rejected
 * near-miss (a 1K-block fs's own bytes also parse as a 512-byte near-miss at
 * byte 512, which must not print first). */
static void findfs_self_detect(void) {
    for (size_t i = 0; ; i++) {
        struct fmt f;
        int frc = fmt_at(i, &f);
        if (!frc)
            break;
        if (frc < 0)
            continue;
        if (f.noprobe)
            continue;   /* findfs's unified probe does not cover it yet */
        const char *want = findfs_name(f.edition);
        char img[64], cmd[512], line[256], what[128];
        snprintf(img, sizeof img, "test_matrix_%s_find.img", f.name);
        unlink(img);
        if (f.edition == FILSYS_PDP7)
            snprintf(cmd, sizeof cmd, "./mkfs.filsys -v %s %s >/dev/null 2>&1", f.name, img);
        else
            snprintf(cmd, sizeof cmd, "./mkfs.filsys -v %s %s %d >/dev/null 2>&1",
                     f.name, img, f.blocks);
        if (system(cmd) != 0) { ok("findfs mkfs", 0); unlink(img); continue; }

        snprintf(cmd, sizeof cmd, "./findfs.filsys %s 2>&1", img);
        FILE *p = popen(cmd, "r");
        line[0] = '\0';
        if (p) {
            (void)!fgets(line, sizeof line, p);
            pclose(p);
        }

        int at_zero = strstr(line, "block 0") || strstr(line, "byte 0");
        int validated = (strstr(line, "chain ok") || strstr(line, "bitmap free list")) &&
                        !strstr(line, "REJECTED");
        int named = want && strstr(line, want);
        snprintf(what, sizeof what, "%s findfs self-detect", f.name);
        ok(what, at_zero && validated && named);
        unlink(img);
    }
}

/* V8-family bitmap free-space forms: drive the in-superblock and
 * out-of-superblock bitmaps through write/read/truncate/fsck, the same cycle
 * run() drives for the free-list forms.  This pins the bitmap allocator's
 * balloc/bfree and the bigmap's tail-metadata exclusion (data_end). */
static void bitmap_roundtrip(void) {
    static const struct {
        const char *name;
        int edition;
        uint32_t blocksize;
        int freemap;
    } forms[] = {
        { "v8",  FILSYS_V8,  4096, FILSYS_FREEMAP_BITMAP },
        { "v9",  FILSYS_V9,  8192, FILSYS_FREEMAP_BITMAP },
        { "v10", FILSYS_V10, 4096, FILSYS_FREEMAP_BITMAP },
        { "v10", FILSYS_V10, 4096, FILSYS_FREEMAP_BIGMAP },
    };
    for (size_t i = 0; i < sizeof forms / sizeof forms[0]; i++) {
        char img[64], gspec[64], cmd[512], what[96];
        const char *fm = forms[i].freemap == FILSYS_FREEMAP_BITMAP ? "bitmap" : "bigmap";
        snprintf(gspec, sizeof gspec, "blocksize=%u,freemap=%s", forms[i].blocksize, fm);
        snprintf(img, sizeof img, "test_matrix_%s_%s.img", forms[i].name, fm);
        unlink(img);
        snprintf(cmd, sizeof cmd, "./mkfs.filsys -v %s -g %s %s 1000 >/dev/null 2>&1",
                 forms[i].name, gspec, img);
        if (system(cmd) != 0) {
            snprintf(what, sizeof what, "%s %s mkfs", forms[i].name, fm);
            ok(what, 0);
            unlink(img);
            continue;
        }

        filsys_geom_t geom = { forms[i].blocksize, forms[i].freemap, NULL };
        filsys_t *fs;
        if (filsys_open_arch(&fs, forms[i].edition, img, 0, 0, 0, 0, NULL,
                             NULL, 0, &geom, 0, NULL)) {
            snprintf(what, sizeof what, "%s %s open", forms[i].name, fm);
            ok(what, 0);
            unlink(img);
            continue;
        }

        uint64_t sz = 8192;   /* several blocks, past the direct-block boundary */
        uint8_t *buf = malloc(sz), *back = malloc(sz);
        int wr = 0, rd = 0, trunc_ok = 0;
        if (buf && back) {
            for (uint64_t j = 0; j < sz; j++)
                buf[j] = (uint8_t)(j & 0x7f);
            filsys_create(fs, "/big", 0644, 0, 0);
            wr = filsys_write(fs, "/big", buf, (size_t)sz, 0) == (int)sz;
            memset(back, 0, sz);
            rd = wr && filsys_read(fs, "/big", back, (size_t)sz, 0) == (int)sz &&
                 memcmp(buf, back, sz) == 0;
            trunc_ok = filsys_truncate(fs, "/big", 0) == 0;
            filsys_unlink(fs, "/big");
        }
        free(buf);
        free(back);
        snprintf(what, sizeof what, "%s %s write", forms[i].name, fm); ok(what, wr);
        snprintf(what, sizeof what, "%s %s read", forms[i].name, fm);  ok(what, rd);
        snprintf(what, sizeof what, "%s %s truncate", forms[i].name, fm); ok(what, trunc_ok);
        filsys_close(fs);

        char fcmd[512], out[4096] = "";
        snprintf(fcmd, sizeof fcmd, "./fsck.filsys -f -v %s -g %s %s 2>&1",
                 forms[i].name, gspec, img);
        FILE *p = popen(fcmd, "r");
        if (p) {
            (void)!fread(out, 1, sizeof out - 1, p);
            pclose(p);
        }
        snprintf(what, sizeof what, "%s %s fsck clean", forms[i].name, fm);
        ok(what, strstr(out, "errors=0") && strstr(out, "missing=0") &&
                 strstr(out, "dup=0"));
        unlink(img);
    }
}

/* The statfs-vs-fsck invariant: for every mkfs-able edition, the free-block and
 * free-inode counts statfs reports must equal what fsck -f computes by walking
 * the free list and i-list.  This catches a format whose on-disk totals are
 * unreadable or unmaintained (e.g. 32V/System III's pack4 superblock), where
 * statfs would silently report zero or a stale count. */
static void statfs_fsck_agreement(void) {
    for (size_t i = 0; ; i++) {
        struct fmt f;
        int frc = fmt_at(i, &f);
        if (!frc)
            break;
        if (frc < 0)
            continue;

        char img[64], cmd[512], what[128];
        snprintf(img, sizeof img, "test_matrix_%s_statfs.img", f.name);
        unlink(img);
        if (f.edition == FILSYS_PDP7)
            snprintf(cmd, sizeof cmd, "./mkfs.filsys -v %s %s >/dev/null 2>&1", f.name, img);
        else
            snprintf(cmd, sizeof cmd, "./mkfs.filsys -v %s %s 1000 >/dev/null 2>&1", f.name, img);
        if (system(cmd) != 0) {
            snprintf(what, sizeof what, "%s statfs mkfs", f.name);
            ok(what, 0);
            unlink(img);
            continue;
        }

        filsys_t *fs = NULL;
        int rc = filsys_open(&fs, f.edition, img, 1, 0, getuid(), getgid(), NULL);
        if (rc) {
            snprintf(what, sizeof what, "%s statfs open", f.name);
            ok(what, 0);
            unlink(img);
            continue;
        }
        struct statvfs st;
        rc = filsys_statfs(fs, &st);
        filsys_close(fs);
        if (rc) {
            snprintf(what, sizeof what, "%s statfs call", f.name);
            ok(what, 0);
            unlink(img);
            continue;
        }

        uint64_t fb = 0, fi = 0;
        if (fsck_free_counts(f.name, img, &fb, &fi) != 0) {
            snprintf(what, sizeof what, "%s fsck summary parse", f.name);
            ok(what, 0);
            unlink(img);
            continue;
        }
        snprintf(what, sizeof what, "%s statfs free blocks == fsck", f.name);
        ok(what, (uint64_t)st.f_bfree == fb);
        /* V1 and PDP-7 reserve inode numbers outside their allocatable range (V1's
         * inode map starts at inode 41; PDP-7 reserves the low inodes), so fsck's
         * all-inodes walk counts those reserved slots as free while statfs reports
         * only the allocatable ones.  The free-block invariant is the universal
         * one; the free-inode invariant applies to the editions whose inode space
         * is a single contiguous allocatable range. */
        if (f.edition != FILSYS_V1 && f.edition != FILSYS_PDP7) {
            snprintf(what, sizeof what, "%s statfs free inodes == fsck", f.name);
            ok(what, (uint64_t)st.f_ffree == fi);
        }
        unlink(img);
    }
}

int main(void) {
    /* The slow soak (fault injection x editions, exhaustive crash-prefix
     * enumeration, property-based op sequences) runs only when
     * FILSYS_SLOW_TESTS is set: `make check` is a fast gate by default, and
     * `./configure --enable-slow-tests` opts the soak back in.  FILSYS_ONLY
     * narrows that to a single phase (see the comment above fault_test). */
    const char *slow_env = getenv("FILSYS_SLOW_TESTS");
    int slow = slow_env && slow_env[0] && strcmp(slow_env, "0") != 0;
    g_only = getenv("FILSYS_ONLY");
    g_mut  = getenv("FILSYS_MUT");

    int phase = g_only && g_only[0];
    int want_fast  = !phase;
    int want_fault = phase ? !strcmp(g_only, "fault")     : slow;
    int want_crash = phase ? !strcmp(g_only, "crash")     : slow;
    int want_prop  = phase ? !strcmp(g_only, "property")  : slow;

    if (want_fast) {
        for (size_t i = 0; ; i++) {
            struct fmt f;
            int frc = fmt_at(i, &f);
            if (!frc)
                break;
            if (frc < 0)
                continue;
            run(&f);
        }
        mkfs_validation();
        mkfs_cleanliness();
        statfs_fsck_agreement();
        badino_consistency();
        namelength();
        v6_large_file();
        v7_triple_indirect();
        bitmap_roundtrip();
        crash_consistency();
        durability_test();
        rename_semantics();
        dir_link_semantics();
        findfs_self_detect();
    }
    if (want_fault) fault_test();
    if (want_crash) crash_prefix_test();
    if (want_prop)  property_sequences();
    if (!phase && !slow) {
        printf("slow tests skipped (set FILSYS_SLOW_TESTS, or configure with "
               "--enable-slow-tests, to run the fault-injection / crash-prefix / "
               "property-sequences soak)\n");
    }
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
