/* conformance-client.c - drives the torture sequence against the mounted
 * fuse-conformance filesystem and correlates each step with the callbacks the
 * filesystem logged.  It reads the log file the filesystem writes (its stderr)
 * and, after each step, prints the callbacks that step produced, so the output
 * is a self-describing transcript.
 *
 * Usage: conformance-client <mountpoint> <logfile>
 *
 * SPDX-License-Identifier: ISC */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static FILE *g_log;
static long g_log_off;

/* Print the callbacks the filesystem logged since the last call. */
static void dump_log(void)
{
    if (fseek(g_log, g_log_off, SEEK_SET) != 0)
        return;
    char line[256];
    int n = 0;
    while (fgets(line, sizeof line, g_log)) {
        printf("    fs: %s", line);
        n++;
    }
    g_log_off = ftell(g_log);
    (void)n;
}

static void step(const char *what)
{
    printf("== %s\n", what);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <mountpoint> <logfile>\n", argv[0]);
        return 2;
    }
    const char *mnt = argv[1];
    g_log = fopen(argv[2], "r");
    if (!g_log) {
        perror("log open");
        return 2;
    }
    char path[512];
    snprintf(path, sizeof path, "%s/f", mnt);

    /* Q1/Q2: does getattr/truncate receive fi (and the right fh)? */
    step("open(O_RDWR|CREAT), fstat(fd)");
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) { perror("open"); return 1; }
    struct stat st;
    if (fstat(fd, &st) != 0) perror("fstat");
    dump_log();

    step("ftruncate(fd, 100) on the open fd");
    if (ftruncate(fd, 100) != 0) perror("ftruncate");
    dump_log();
    close(fd);

    /* Q3: does release fire at close(2), or later at reclaim? */
    step("open(O_RDONLY), close(fd)");
    fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    close(fd);
    dump_log();   /* RELEASE present here => release at close */

    /* Q4: is open deduplicated, and do open/release balance? */
    step("two concurrent opens (fd1, fd2), close both");
    int fd1 = open(path, O_RDONLY);
    int fd2 = open(path, O_RDONLY);
    close(fd1);
    close(fd2);
    dump_log();

    /* Q5: two processes holding the same file */
    step("parent + child each open the file");
    int fa = open(path, O_RDONLY);
    if (fa < 0) { perror("open"); return 1; }
    pid_t pid = fork();
    if (pid == 0) {
        int fb = open(path, O_RDONLY);
        close(fb);
        _exit(0);
    }
    close(fa);
    waitpid(pid, NULL, 0);
    dump_log();

    /* Q6: does unlink of an open file reach the FS (hard_remove)? */
    step("open(O_RDWR), unlink(path) while open, close(fd)");
    fd = open(path, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    if (unlink(path) != 0) perror("unlink");
    close(fd);
    dump_log();

    /* Q7: does truncate carry fi->fh to an *unlinked* open fd?  This is the
     * column the deferred-free lifecycle actually needs: ftruncate(2) after
     * unlink must reach the inode by handle, or it falls through to the (gone)
     * path and returns ENOENT. */
    step("open(O_RDWR|CREAT), unlink(path) while open, ftruncate(fd, 100), close(fd)");
    fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) { perror("open"); return 1; }
    if (unlink(path) != 0) perror("unlink");
    if (ftruncate(fd, 100) != 0) perror("ftruncate (unlinked)");
    close(fd);
    dump_log();

    fclose(g_log);
    return 0;
}
