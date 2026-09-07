/* test_oracle.c - the external oracle: mutate a V7 image with a hard-linked
 * directory, boot it on simh, and require the *historical* icheck/dcheck to
 * report a clean filesystem.
 *
 * Historical Unix let the superuser hard-link a directory, so a directory can
 * carry several names and a subdirectory's ".." is one more link.  The library's
 * own fsck can only check against filsys's own model; the point of this test is
 * to check against the real thing: the V7 tools running under simh on the very
 * image filsys just wrote.
 *
 * The image is the pcollinson V7 RP06 disk (rp06-0.disk, ./fetch.sh), whose
 * free list is broken as distributed (restor never rebuilds it).  fsck.filsys
 * -s repairs that first, so the mutation allocates from a clean free list and
 * icheck's block accounting is meaningful, not inherited noise.
 *
 * This test is opt-in (--enable-oracle-tests): it needs rp06-0.disk, the
 * `prebsd` driver (a sibling repo; point PREBSD at its binary), and the `pdp11`
 * simh executable.  When enabled it *fails* -- rather than skips -- if any of
 * those is missing, so an explicitly-enabled run cannot pass silently.
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

/* Run a shell command; capture stdout+stderr (up to outsz-1 bytes) into `out`.
 * Returns the exit status, or -1 if the command couldn't be run. */
static int sh(const char *cmd, char *out, size_t outsz) {
    int rc = -1;
    out[0] = '\0';
    FILE *p = popen(cmd, "r");
    if (p) {
        size_t n = fread(out, 1, outsz - 1, p);
        out[n] = '\0';
        int st = pclose(p);
        if (st != -1)
            rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    }
    return rc;
}

/* icheck's summary "missing <n>"; clean means n == 0. */
static int missing_is_zero(const char *out) {
    const char *p = strstr(out, "missing");
    if (!p)
        return 0;
    p += 7;
    while (*p == ' ' || *p == '\t')
        p++;
    return *p == '0' && (p[1] < '0' || p[1] > '9');
}

/* The mutation.  /oracle ends up with nlink 4 -- "." (self), "/oracle" (the
 * root entry), "/oracle/renamed"'s "..", and "/oracle-link" (the hard link) --
 * which real V7's dcheck must agree with.  The cycle (linking /oracle into its
 * own subtree) must be refused.  Returns 0 on success. */
static int mutate(const char *img) {
    filsys_t *fs;
    if (filsys_open(&fs, FILSYS_V7, img, 0, 0, 0, 0, NULL))
        return -1;
    int rc = 0;
    if (filsys_mkdir(fs, "/oracle", 0755, 0, 0) ||
        filsys_mkdir(fs, "/oracle/sub", 0755, 0, 0) ||
        filsys_create(fs, "/oracle/file", 0644, 0, 0)) {
        rc = -1; goto out;
    }
    static const char msg[] = "hello from filsys\n";
    if (filsys_write(fs, "/oracle/file", msg, sizeof msg - 1, 0) !=
        (int)(sizeof msg - 1)) {
        rc = -1; goto out;
    }
    if (filsys_link(fs, "/oracle", "/oracle-link") ||          /* dir hard-link */
        filsys_rename(fs, "/oracle/sub", "/oracle/renamed", 0) ||
        filsys_link(fs, "/oracle", "/oracle/renamed/loop") != -EINVAL) {
        rc = -1; goto out;                                     /* cycle refused */
    }
out:
    filsys_close(fs);
    return rc;
}

int main(void)
{
    const char *prebsd = getenv("PREBSD");
    if (!prebsd || !*prebsd)
        prebsd = "../prebsd/prebsd";
    if (access(prebsd, X_OK) != 0) {
        printf("FAIL prebsd driver not found at %s (set PREBSD=... or check out "
               "../prebsd)\n", prebsd);
        return 1;
    }
    if (access("rp06-0.disk", F_OK) != 0) {
        printf("FAIL rp06-0.disk absent (run ./fetch.sh)\n");
        return 1;
    }
    if (system("command -v pdp11 >/dev/null 2>&1") != 0 &&
        !getenv("SIMH")) {
        printf("FAIL pdp11 (simh) not found on PATH and $SIMH unset\n");
        return 1;
    }

    char tmp[] = "/tmp/filsys-oracle-XXXXXX";
    if (!mkdtemp(tmp)) {
        perror("mkdtemp");
        return 1;
    }
    char img[512], ini[512], cmd[8192], out[1 << 20];
    snprintf(img, sizeof img, "%s/rp06-0.disk", tmp);
    snprintf(ini, sizeof ini, "%s/oracle.ini", tmp);

    snprintf(cmd, sizeof cmd, "cp rp06-0.disk %s", img);
    ok("copy pristine image", sh(cmd, out, sizeof out) == 0);

    snprintf(cmd, sizeof cmd, "./fsck.filsys -f -v v7 -s %s", img);
    ok("repair free list (fsck -s)", sh(cmd, out, sizeof out) == 0);

    ok("mutate (hard-linked dir, rename, refused cycle)", mutate(img) == 0);

    /* simh ini pointing at the mutated image, on a non-default console port */
    FILE *f = fopen(ini, "w");
    if (!f) {
        perror("fopen ini");
        return 1;
    }
    fprintf(f, "set cpu 11/70\nset cpu 2M\nset cpu idle\n"
               "set rp0 rp06\natt rp0 rp06-0.disk\n"
               "set console telnet=10047\nboot rp0\n");
    fclose(f);

    /* boot the mutated image and run the historical icheck + dcheck */
    char prebsd_abs[4096];
    if (!realpath(prebsd, prebsd_abs))
        strcpy(prebsd_abs, prebsd);
    snprintf(cmd, sizeof cmd,
             "timeout 300 %s %s 'icheck /dev/rp0; dcheck /dev/rp0' 2>&1",
             prebsd_abs, ini);
    int ran = (sh(cmd, out, sizeof out) >= 0);
    ok("boot under simh and run icheck+dcheck", ran);

    ok("icheck: no missing blocks", missing_is_zero(out));
    ok("icheck: no duplicate blocks", strstr(out, "dup") == NULL);
    ok("dcheck: no link-count errors", strstr(out, "link cnt") == NULL);

    snprintf(cmd, sizeof cmd, "rm -rf %s", tmp);
    (void)!system(cmd);

    if (failures) {
        /* dump the relevant tail so a failure is diagnosable */
        const char *c = strstr(out, "/dev/rp0:");
        printf("\n--- oracle console (icheck/dcheck) ---\n%s\n",
               c ? c : out);
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
