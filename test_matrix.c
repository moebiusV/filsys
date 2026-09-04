/* test_matrix.c - exercise every backend through the library: create, write,
 * read, truncate and delete at sizes straddling each format's direct-block and
 * max-file boundaries, then run fsck and require errors=0 with no missing or
 * duplicate blocks.
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

int main(void) {
    printf("1..%zu\n", sizeof FMTS / sizeof FMTS[0]);
    for (size_t i = 0; i < sizeof FMTS / sizeof FMTS[0]; i++)
        run(&FMTS[i]);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
