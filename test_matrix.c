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
    int         noprobe;
};

/* Fill one row from the shared table; returns 0 past the end, -1 to skip a
 * format that is not mkfs-able yet (read-only V8/V9/V10), 1 on success. */
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
        { "rename",   setup_file, op_rename },
        { "rename_over",     setup_file2, op_rename_over },
        { "rename_dir",      setup_dir,   op_rename_dir },
        { "rename_dir_over", setup_dir2,  op_rename_dir_over },
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
            for (size_t mo = 0; mo < sizeof modes / sizeof modes[0]; mo++)
                fault_mutator(&f, &mut[m], modes[mo].mode, modes[mo].label);
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
        /* 2.11BSD's check is "check only, no salvage/preen" (bsd211_check ignores
         * mode), so a destroyed free-list chain cannot be rebuilt; its chain-
         * reload path also still destroys the chain under this test.  Skip until
         * bsd211 grows a salvage mode. */
        if (f.edition == FILSYS_BSD211)
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
 * the specific edition: System III's is V7-shaped (no s_magic), and SVR4 adds
 * only s_state to the SVR2 superblock.  (2.9BSD and 2.11BSD share a superblock
 * too, but findfs tells them apart by reading the root directory's entry
 * format -- see bsd211_root_dir.) */
static const char *findfs_name(int edition) {
    switch (edition) {
    case FILSYS_PDP7:     return "PDP-7";
    case FILSYS_V1:       return "V1";
    case FILSYS_V6:       return "V6";
    case FILSYS_V7:       return "V7";
    case FILSYS_32V:      return "32V";
    case FILSYS_COHERENT: return "Coherent";
    case FILSYS_XENIX:    return "Xenix";
    case FILSYS_BSD29:    return "2.9BSD";
    case FILSYS_BSD211:   return "2.11BSD";
    case FILSYS_SYSIII:   return "V7";
    case FILSYS_SVR2:     return "sysvr2";
    case FILSYS_SVR4:     return "sysvr2";
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

int main(void) {
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
    namelength();
    v6_large_file();
    crash_consistency();
    durability_test();
    rename_semantics();
    dir_link_semantics();
    findfs_self_detect();
    fault_test();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
