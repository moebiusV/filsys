/* filsys 1.8.0 - 2026-09-06 - Copyright (C) 2026 David Walther */
/* SPDX-License-Identifier: ISC */
/* mkfs.filsys.c - create a Research Unix (PDP-7 through 32V) filesystem in a
 * disk image.
 *
 * Usage:
 *     mkfs.filsys [-v <pdp7|v1|v6|v7|vax32|coherent|xenix|bsd29|bsd211>] [-o block] [-b boot]
 *                 [-m m] [-n n] image [blocks]
 *
 * Builds a fresh filesystem: a superblock, a zeroed i-list, a free-block list,
 * and an empty root directory.  The image is opened (created if missing) and
 * grown to the filesystem size.  -o places the filesystem at a block offset
 * within the image (for multi-partition images); without it the filesystem
 * starts at block 0.
 *
 * -v selects the edition (the default is 7).  Several editions are one on-disk
 * format: 1, 2 and 3 are byte-identical (bitmap allocator, 10-byte dirents,
 * root inode 41); 4, 5 and 6 are byte-identical (the V6 format); vax32 (32v,
 * 32) is 32V, V7 recompiled for the VAX with little-endian 32-bit fields.  0 is the
 * word-addressed PDP-7.  coherent is the Mark Williams Coherent format:
 * middle-endian V7 with a 64-entry free cache and s_m/s_n/s_unique; -m/-n set
 * the interleave factors (s_m/s_n, default 1/1).
 *
 * Block layout (block 0 is the start of the filesystem):
 *     block 0    boot block (written by -b, else left alone)
 *     block 1    superblock
 *     block 2..  i-list, then data
 * -b takes a PDP-11 a.out (V7 magic 0407) and writes its text+data as block 0.
 *
 * Sizes default to the image's own size, or `blocks` when given.
 *
 * This file is argument parsing plus a call to filsys_mkfs() (filsys_mkfs.c);
 * the layout knowledge lives in the library.
 */
#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdarg.h>

#include "filsys.h"
#include "filsys_ops.h"
#include "v7fs.h"
#include "pdp7fs.h"

enum { A_MAGIC1 = 0407 };  /* V7 normal a.out magic (boot block) */

static int fd;

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}

/* A block device can't be resized (ftruncate returns EINVAL); its size is fixed
 * by the device itself.  Only grow a regular-file image. */
static int image_is_regular(void)
{
    struct stat st;
    return fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
}

/* Open the image read-write.  A block (or character) device -- and an existing
 * regular file -- opens directly; only a missing path is created as a regular
 * file.  The device is never truncated: its size is fixed by the device, and
 * O_TRUNC on one is at best ignored and at worst rejected. */
static int open_image(const char *path)
{
    int f = open(path, O_RDWR);
    if (f < 0 && errno == ENOENT)
        f = open(path, O_RDWR | O_CREAT, 0666);
    return f;
}

/* Read a PDP-11 a.out (V7 magic 0407) boot program into `buf` (at least
 * V7_BSIZE bytes, zero-filled).  Returns 0, or exits on a bad boot file. */
static int read_boot(const char *path, uint8_t *buf)
{
    uint8_t hdr[16];
    int f = open(path, O_RDONLY);
    if (f < 0)
        die("%s: cannot open boot: %s\n", path, strerror(errno));
    if (read(f, hdr, sizeof hdr) != (ssize_t)sizeof hdr)
        die("%s: short boot header\n", path);
    uint16_t magic = bo_get16le(hdr + 0);
    uint16_t text  = bo_get16le(hdr + 2);
    uint16_t data  = bo_get16le(hdr + 4);
    if (magic != A_MAGIC1)
        die("%s: bad boot magic 0%o (want 0%o)\n", path, magic, A_MAGIC1);
    uint32_t c = (uint32_t)text + data;
    if (c > V7_BSIZE)
        die("%s: boot too big (%u > %d bytes)\n", path, c, V7_BSIZE);
    memset(buf, 0, V7_MAXBSIZE);
    if (read(f, buf, c) != (ssize_t)c)
        die("%s: short boot\n", path);
    close(f);
    return 0;
}

/* Open (or create) the image, then work out the filesystem size in blocks: an
 * explicit `blocks` argument, else the image's own size, else a default. */
static uint32_t resolve_blocks(const char *path, uint64_t base, uint32_t bsize,
                               uint32_t blocks)
{
    fd = open_image(path);
    if (fd < 0)
        die("%s: cannot open/create: %s\n", path, strerror(errno));

    if (blocks == 0) {
        uint64_t sz = 0;
        if (filsys_dev_size(fd, &sz) == 0 && sz >= base + bsize)
            blocks = (uint32_t)((sz - base) / bsize);
        if (blocks < 16)
            blocks = 4000;      /* small usable volume */
    }
    return blocks;
}

/* Parse a -g geometry spec ("blocksize=4096,freemap=bitmap,byteorder=be") into
 * a filsys_geom_t.  Returns 0, or -1 with *errmsg set.  byteorder is strdup'd
 * (mkfs is one-shot; the process owns it until exit). */
static int parse_geom(const char *spec, filsys_geom_t *g, const char **errmsg)
{
    static char msgbuf[128];
    *errmsg = NULL;
    char *s = strdup(spec);
    if (!s)
        return -1;
    for (char *tok = strtok(s, ","); tok; tok = strtok(NULL, ",")) {
        if (!strncmp(tok, "blocksize=", 10)) {
            char *end = NULL;
            g->blocksize = (uint32_t)strtoul(tok + 10, &end, 0);
            if (!end || *end) {
                snprintf(msgbuf, sizeof msgbuf, "bad blocksize '%s'", tok + 10);
                *errmsg = msgbuf;
                free(s);
                return -1;
            }
        } else if (!strncmp(tok, "freemap=", 8)) {
            if (!strcmp(tok + 8, "list"))       g->freemap = FILSYS_FREEMAP_LIST;
            else if (!strcmp(tok + 8, "bitmap")) g->freemap = FILSYS_FREEMAP_BITMAP;
            else if (!strcmp(tok + 8, "bigmap")) g->freemap = FILSYS_FREEMAP_BIGMAP;
            else {
                snprintf(msgbuf, sizeof msgbuf, "bad freemap '%s'", tok + 8);
                *errmsg = msgbuf;
                free(s);
                return -1;
            }
        } else if (!strncmp(tok, "byteorder=", 10)) {
            g->byteorder = strdup(tok + 10);
        } else {
            snprintf(msgbuf, sizeof msgbuf, "unknown geometry '%s'", tok);
            *errmsg = msgbuf;
            free(s);
            return -1;
        }
    }
    free(s);
    return 0;
}

int main(int argc, char **argv)
{
    const char *path;
    const char *bootfile = NULL;
    uint32_t blocks = 0;
    uint64_t offblock = 0;
    int edition = -1;   /* no default: the version must be named explicitly */
    const char *packing = NULL;   /* PDP-7 word container codec */
    const char *arch = NULL;      /* CPU arch: overrides the edition's byte order */
    uint32_t user_bsize = 0;      /* -B: System V logical block size (512/1024/2048) */
    const char *geom_spec = NULL; /* -g: V8-family blocksize/freemap/byteorder */
    uint16_t m = 1, n = 1;        /* coherent interleave (s_m/s_n) */
    int c;

    while ((c = getopt(argc, argv, "v:o:b:m:n:P:a:B:g:")) != -1) {
        switch (c) {
        case 'v':
            edition = filsys_parse_edition("mkfs.filsys", optarg);
            if (edition < 0)
                return 1;
            break;
        case 'o': offblock = strtoull(optarg, NULL, 0); break;
        case 'b': bootfile = optarg; break;
        case 'm': m = (uint16_t)strtoul(optarg, NULL, 0); break;
        case 'n': n = (uint16_t)strtoul(optarg, NULL, 0); break;
        case 'P': packing = optarg; break;
        case 'a': arch = optarg; break;
        case 'B': user_bsize = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'g': geom_spec = optarg; break;
        default:
            fprintf(stderr, "usage: mkfs.filsys -v <edition> [-o block] [-P packing] [-a arch] [-B bsize] [-g geom] [-b boot] [-m m] [-n n] image [blocks]\n"
                            "  editions: %s\n", filsys_editions_usage());
            return 1;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "usage: mkfs.filsys -v <edition> [-o block] [-P packing] [-a arch] [-B bsize] [-g geom] [-b boot] [-m m] [-n n] image [blocks]\n"
                        "  editions: %s\n", filsys_editions_usage());
        return 1;
    }
    if (edition < 0) {
        fprintf(stderr, "mkfs.filsys: no filesystem version given; use -v <edition>\n");
        return 1;
    }
    path = argv[optind];
    if (optind + 1 < argc)
        blocks = (uint32_t)strtoul(argv[optind + 1], NULL, 0);

    filsys_edition_t fmt = filsys_getformat(edition);

    /* Resolve the geometry spec and the -B block size the way the library will,
     * so the byte offset and default block count are computed in the final
     * block size. */
    filsys_geom_t geom = {0, -1, NULL};
    if (geom_spec) {
        const char *gerr = NULL;
        if (parse_geom(geom_spec, &geom, &gerr)) {
            fprintf(stderr, "mkfs.filsys: %s\n", gerr ? gerr : "bad geometry");
            return 1;
        }
    }
    if (user_bsize) {
        if (!fmt.dyn_bsize) {
            fprintf(stderr, "mkfs.filsys: -B block size is System-V-only\n");
            return 1;
        }
        if (user_bsize != 512 && user_bsize != 1024 && user_bsize != 2048) {
            fprintf(stderr, "mkfs.filsys: bad block size %u (want 512, 1024 or 2048)\n",
                    user_bsize);
            return 1;
        }
    }
    uint32_t bsize = user_bsize ? user_bsize : fmt.bsize;
    uint64_t base = offblock * bsize;

    /* The word-addressed PDP-7 has a fixed RB09 geometry; blocks/bsize do not
     * apply.  Resolve the codec for the size computation. */
    if (edition == FILSYS_PDP7) {
        const word_codec_t *word = fmt.word;
        if (packing && !(word = filsys_word_codec_by_name(packing))) {
            fprintf(stderr, "mkfs.filsys: unknown packing '%s'\n", packing);
            return 1;
        }
        fd = open_image(path);
        if (fd < 0)
            die("%s: cannot open/create: %s\n", path, strerror(errno));
        uint64_t size = (uint64_t)P7_NBLOCKS * word->block_bytes;
        if (image_is_regular() && ftruncate(fd, (off_t)(base + size)) < 0)
            die("%s: ftruncate: %s\n", path, strerror(errno));
    } else {
        blocks = resolve_blocks(path, base, bsize, blocks);
        if (image_is_regular() &&
            ftruncate(fd, (off_t)(base + (uint64_t)blocks * bsize)) < 0)
            die("%s: ftruncate: %s\n", path, strerror(errno));
    }

    uint8_t boot[V7_MAXBSIZE];
    if (bootfile)
        read_boot(bootfile, boot);

    filsys_mkfs_opts_t opts = {0};
    opts.blocks = blocks;
    opts.bsize = user_bsize;
    opts.packing = packing;
    opts.arch = arch;
    opts.geom = (geom_spec ? &geom : NULL);
    opts.m = m;
    opts.n = n;
    opts.boot = (bootfile ? boot : NULL);

    uint64_t written = 0;
    const char *err = NULL;
    if (filsys_mkfs(edition, &filsys_io_file, fd, base, &opts, &written, &err)) {
        fprintf(stderr, "mkfs.filsys: %s: %s", path, err ? err : "failed");
        close(fd);
        return 1;
    }

    /* V1 rounds the volume up to a whole word; PDP-7's geometry is fixed, so the
     * actual size can differ from the pre-grown size.  Snap the image to it. */
    if (image_is_regular() && written && written != base + (uint64_t)blocks * bsize)
        if (ftruncate(fd, (off_t)(base + written)) < 0)
            die("%s: ftruncate: %s\n", path, strerror(errno));

    close(fd);
    printf("%s: filesystem written\n", path);
    return 0;
}
