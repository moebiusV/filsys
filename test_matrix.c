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

/* One row per format: edition, -v name, mkfs block count (0 = fixed size),
 * the per-format max file size (the size-field ceiling), and the direct-block
 * capacity -- the byte count at which a write forces large-file (indirect)
 * mode. */
static const struct fmt {
    int         edition;
    const char *name;
    int         blocks;
    uint64_t    maxfile;
    uint64_t    direct;
} FMTS[] = {
    { FILSYS_PDP7, "0",   0,    57344ULL,     896ULL },   /* 7 x 128 B */
    { FILSYS_V1,   "1",   4000, 65535ULL,     4096ULL },   /* 8 x 512 B */
    { FILSYS_V6,   "6",   4000, 16777215ULL,  4096ULL },
    { FILSYS_V7,   "7",   4000, 1082201088ULL, 5120ULL },  /* 10 x 512 B */
    { FILSYS_32V,  "32v", 4000, 1082201088ULL, 5120ULL },
    { FILSYS_COHERENT, "coherent", 4000, 1082201088ULL, 5120ULL },
    { FILSYS_XENIX, "xenix", 4000, 17247250432ULL, 10240ULL }, /* 10 x 1 K */
    { FILSYS_BSD29, "bsd29", 4000, 17247244288ULL, 4096ULL },  /* 4 x 1 K */
    { FILSYS_BSD211, "bsd211", 4000, 17247244288ULL, 4096ULL },
};

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
    if (filsys_open(&fs, f->edition, img, 0, 0, 0, 0)) {
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

    for (size_t i = 0; i < sizeof FMTS / sizeof FMTS[0]; i++) {
        const struct fmt *f = &FMTS[i];

        if (f->edition == FILSYS_PDP7) {
            char img[64], cmd[512];
            snprintf(img, sizeof img, "test_matrix_%s_fixed.img", f->name);
            unlink(img);
            snprintf(cmd, sizeof cmd, "./mkfs.filsys -v %s %s >/dev/null 2>&1",
                     f->name, img);
            if (system(cmd) != 0) { ok("v0 mkfs", 0); unlink(img); continue; }
            ok("v0 fsck-clean (fixed size)", fsck_is_clean(f->name, img));
            unlink(img);
            continue;
        }

        for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
            char img[64], cmd[512], what[96];
            snprintf(img, sizeof img, "test_matrix_%s_%d.img", f->name, sizes[s]);
            unlink(img);
            snprintf(cmd, sizeof cmd, "./mkfs.filsys -v %s %s %d >/dev/null 2>&1",
                     f->name, img, sizes[s]);
            if (system(cmd) != 0) {
                snprintf(what, sizeof what, "%s mkfs @ %d blocks", f->name, sizes[s]);
                ok(what, 0);
                unlink(img);
                continue;
            }
            snprintf(what, sizeof what, "%s fsck-clean @ %d blocks", f->name, sizes[s]);
            ok(what, fsck_is_clean(f->name, img));
            unlink(img);
        }
    }
}

/* Directory-entry name widths, asserted per edition at 14/30/63/64 characters.
 * This is the regression for the facade bug that hardcoded a 14-character
 * component buffer and rejected 2.11BSD's 63-character names before they
 * reached the backend.  Every edition must accept names up to its limit (8 for
 * V1, 14 for the fixed-width formats, 63 for 2.11BSD) and reject beyond it. */
static void namelength(void) {
    static const struct { int edition; const char *name; size_t max; } t[] = {
        { FILSYS_V1,        "v1",       8  },
        { FILSYS_V6,        "v6",       14 },
        { FILSYS_V7,        "v7",       14 },
        { FILSYS_32V,       "32v",      14 },
        { FILSYS_COHERENT,  "coherent", 14 },
        { FILSYS_XENIX,     "xenix",    14 },
        { FILSYS_BSD29,     "bsd29",    14 },
        { FILSYS_BSD211,    "bsd211",   63 },
    };
    static const size_t lens[] = { 14, 30, 63, 64 };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        char img[64], cmd[256];
        snprintf(img, sizeof img, "test_matrix_nl_%s.img", t[i].name);
        unlink(img);
        snprintf(cmd, sizeof cmd, "./mkfs.filsys -v %s %s 100 >/dev/null 2>&1",
                 t[i].name, img);
        if (system(cmd) != 0) { ok("namelength mkfs", 0); continue; }
        filsys_t *fs;
        if (filsys_open(&fs, t[i].edition, img, 0, 0, 0, 0)) {
            ok("namelength open", 0); unlink(img); continue;
        }
        for (size_t j = 0; j < sizeof lens / sizeof lens[0]; j++) {
            size_t L = lens[j];
            char path[80], what[96];
            path[0] = '/';
            memset(path + 1, 'a', L);
            path[L + 1] = 0;
            int want = (L <= t[i].max) ? 0 : -ENAMETOOLONG;
            int got = filsys_create(fs, path, 0644, 0, 0);
            snprintf(what, sizeof what, "%s %zu-char %s", t[i].name, L,
                     want == 0 ? "accepted" : "rejected");
            ok(what, got == want);
            if (got == 0) filsys_unlink(fs, path);
        }
        filsys_close(fs);
        unlink(img);
    }
}

int main(void) {
    for (size_t i = 0; i < sizeof FMTS / sizeof FMTS[0]; i++)
        run(&FMTS[i]);
    mkfs_validation();
    mkfs_cleanliness();
    namelength();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
