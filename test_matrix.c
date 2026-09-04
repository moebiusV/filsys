/* test_matrix.c - exercise every backend through the library: create, write,
 * read, truncate and delete at sizes straddling each format's direct-block and
 * max-file boundaries, then run fsck and require errors=0 with no missing or
 * duplicate blocks.  mkfs_validation() checks mkfs argument rejection;
 * mkfs_cleanliness() makes a fresh filesystem at several volume sizes per
 * format and fsck-checks each.
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

/* One row per format: edition, -v name, mkfs block count (0 = fixed size),
 * and the per-format max file size (the size-field ceiling, not the
 * block-address ceiling). */
static const struct fmt {
    int         edition;
    const char *name;
    int         blocks;
    uint64_t    maxfile;
} FMTS[] = {
    { FILSYS_PDP7, "0",   0,    57344ULL },      /* fixed 8000-block RB09 */
    { FILSYS_V1,   "1",   4000, 65535ULL },      /* 16-bit size field */
    { FILSYS_V6,   "6",   4000, 16777215ULL },   /* 24-bit size field */
    { FILSYS_V7,   "7",   4000, 1082201088ULL }, /* triple indirect */
    { FILSYS_32V,  "32v", 4000, 1082201088ULL },
};

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
    int rc = filsys_open(&fs, f->edition, img, 0, 0, 0, 0);
    if (rc) {
        ok(f->name, 0);
        fprintf(stderr, "  open failed: %d\n", rc);
        return;
    }

    /* A write that forces large-file mode but stays within the ceiling. */
    uint64_t large = f->maxfile < 200000 ? f->maxfile : 200000;
    char *buf = malloc(large ? large : 1);
    memset(buf, 'L', large);
    filsys_create(fs, "/big", 0644, 0, 0);
    rc = filsys_write(fs, "/big", buf, large, 0);
    ok("large write", rc == (int)large);

    char *back = malloc(large ? large : 1);
    memset(back, 0, large);
    rc = filsys_read(fs, "/big", back, large, 0);
    ok("large read round-trip", rc == (int)large && memcmp(buf, back, large) == 0);

    ok("truncate down", filsys_truncate(fs, "/big", large / 2) == 0);
    ok("truncate up", filsys_truncate(fs, "/big", large) == 0);
    ok("truncate small", filsys_truncate(fs, "/big", 100) == 0);

    /* A write starting at the ceiling must fail EFBIG and leak nothing. */
    char one = 'x';
    rc = filsys_write(fs, "/big", &one, 1, (off_t)f->maxfile);
    ok("oversized write EFBIG", rc == -EFBIG);

    filsys_unlink(fs, "/big");
    filsys_close(fs);
    free(buf);
    free(back);

    /* fsck: clean, no missing or duplicate blocks. */
    snprintf(cmd, sizeof cmd, "./fsck.filsys -v %s %s 2>&1", f->name, img);
    FILE *p = popen(cmd, "r");
    char out[4096] = "";
    if (p) {
        (void)!fread(out, 1, sizeof out - 1, p);
        pclose(p);
    }
    ok("fsck clean", strstr(out, "errors=0") != NULL &&
                    strstr(out, "missing=0") != NULL &&
                    strstr(out, "dup=0") != NULL);
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
 * must report errors=0 with no missing or duplicate blocks.  The PDP-7's size
 * is fixed by the RB09 geometry, so it gets one sizeless mkfs. */
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
            snprintf(cmd, sizeof cmd, "./fsck.filsys -v %s %s 2>&1", f->name, img);
            char out[4096] = "";
            FILE *p = popen(cmd, "r");
            if (p) { (void)!fread(out, 1, sizeof out - 1, p); pclose(p); }
            ok("v0 fsck-clean (fixed size)",
               strstr(out, "errors=0") && strstr(out, "missing=0") && strstr(out, "dup=0"));
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
            snprintf(cmd, sizeof cmd, "./fsck.filsys -v %s %s 2>&1", f->name, img);
            char out[4096] = "";
            FILE *p = popen(cmd, "r");
            if (p) { (void)!fread(out, 1, sizeof out - 1, p); pclose(p); }
            snprintf(what, sizeof what, "%s fsck-clean @ %d blocks", f->name, sizes[s]);
            ok(what, strstr(out, "errors=0") && strstr(out, "missing=0") && strstr(out, "dup=0"));
            unlink(img);
        }
    }
}

int main(void) {
    for (size_t i = 0; i < sizeof FMTS / sizeof FMTS[0]; i++)
        run(&FMTS[i]);
    mkfs_validation();
    mkfs_cleanliness();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
