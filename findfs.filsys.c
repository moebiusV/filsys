/* filsys 1.8.0 - 2026-09-06 - Copyright (C) 2026 David Walther */
/* SPDX-License-Identifier: ISC */
/* findfs.filsys.c - locate filesystem superblocks on a raw disk image.
 *
 * A V7 (and V4/V5/V6/32V/Coherent) disk is a set of *partitions* in one file, and the
 * partition table is compiled into the kernel, not stored on the disk.  This
 * tool finds the filesystems by looking for their superblocks directly, and -
 * with -i - by scanning for inode-table runs and tracing backwards to the
 * superblock (block 2 of a filesystem is its first inode-table block, so the
 * block just before an inode-table run is the superblock).  The latter finds
 * filesystems even when a damaged superblock defeats the direct scan.
 *
 * Detection is the library's job now: each edition's ops table carries a
 * `probe` method (v7fs.c/v1fs.c/pdp7fs.c), and this tool loops the shared
 * edition table (filsys_format_nth) calling it at each candidate offset.  The
 * scan policy (stride, skip-ahead, backtrace) and the reporting live here.
 *
 * Usage:
 *     findfs.filsys [-s N] [-i] <image>
 *
 * -s N   scan every N-th block (use the disk's blocks-per-cylinder, e.g. 418
 *        for an RP06, to speed up a big image)
 * -i     also trace inode-table runs backwards to their superblocks
 *
 * Each hit is printed as a diagnostic line followed by a ready-to-paste
 * mount command:
 *     fs @ block START  (byte BYTES)  V7  isize=N fsize=M  chain ok (...)
 *       mount.filsys -v v7 -o offset=BYTES <image> /mnt
 * Mount the partition in place by cutting and pasting that second line.
 */
#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "byteorder.h"
#include "filsys.h"
#include "filsys_ops.h"
#include "v7fs.h"
#include "pdp7fs.h"

enum { BSIZE = 512 };

/* ---- class -> canonical -v token ------------------------------------------ */

const char *canonical_token(const char *class) {
    if (!strncmp(class, "v1,", 3))         return "v1";
    if (!strncmp(class, "v4,", 3))         return "v6";
    if (!strncmp(class, "v7,", 3))         return "v7";
    if (!strncmp(class, "v8 or v10", 9))   return "v8";
    if (!strncmp(class, "v10,", 4))        return "v10";
    if (!strncmp(class, "sysvr2", 6))      return "sysvr2";
    if (!strncmp(class, "32V", 3))         return "vax32";
    if (!strcmp(class, "Coherent"))        return "coherent";
    if (!strcmp(class, "Xenix"))           return "xenix";
    if (!strcmp(class, "2.9BSD"))          return "bsd29";
    if (!strcmp(class, "2.11BSD"))         return "bsd211";
    if (!strcmp(class, "sysvr4"))          return "sysvr4";
    if (!strcmp(class, "v9"))              return "v9";
    return class;
}

static int edition_selector(const char *class) {
    return filsys_edition_by_name(canonical_token(class));
}

static int matches(int filter, const char *class) {
    return filter < 0 || filter == FILSYS_UNIX || edition_selector(class) == filter;
}

/* ---- inode-table backtrace helpers ---------------------------------------- */

static int inode_ok(const uint8_t *d) {
    uint16_t mode = bo_get16le(d);
    if (mode == 0)
        return 1;
    uint16_t t = mode & 0170000;
    if (t != 0100000 && t != 0040000 && t != 0020000 && t != 0060000)
        return 0;
    return bo_get16le(d + 2) < 128 && bo_get16le(d + 4) < 256 && bo_get16le(d + 6) < 256;
}

static int inode_block(const uint8_t *b) {
    int ok = 0, used = 0;
    for (int i = 0; i < 8; i++) {
        const uint8_t *d = b + i * 64;
        if (inode_ok(d)) ok++;
        if (bo_get16le(d) != 0) used++;
    }
    return ok >= 6 && used > 0;
}

/* ---- candidate collection + reporting -------------------------------------- */

struct cand {
    uint32_t blk;
    uint64_t byte;
    char     class_desc[48];
    char     ed[16];
    char     note[48];
    uint16_t isz;
    uint32_t fsz;
    uint32_t segs;
    int      root;
    int      bitmap;
    int      valid;
    uint64_t end;
    const char *why;
    int      drop;
};

enum { MAXCAND = 256 };
static struct cand cands[MAXCAND];
static int ncand;

static void add_cand(uint32_t blk, uint64_t byte, const char *class, const char *note,
                     uint16_t isz, uint32_t fsz, uint32_t segs, int root,
                     int bitmap, int valid, uint64_t end, const char *why) {
    if (ncand >= MAXCAND)
        return;
    struct cand *c = &cands[ncand++];
    c->blk = blk; c->byte = byte; c->isz = isz; c->fsz = fsz;
    c->segs = segs; c->root = root; c->bitmap = bitmap;
    c->valid = valid; c->end = end; c->why = why;
    c->drop = 0;
    snprintf(c->class_desc, sizeof c->class_desc, "%s", class);
    snprintf(c->ed, sizeof c->ed, "%s", canonical_token(class));
    snprintf(c->note, sizeof c->note, "%s", note ? note : "");
}

static void print_cand(const char *image, const struct cand *c) {
    printf("found a %s filesystem", c->class_desc);
    if (c->note[0])
        printf(" (%s)", c->note);
    printf(" at block %u (byte %llu), isize=%u fsize=%u", c->blk,
           (unsigned long long)c->byte, c->isz, c->fsz);
    if (c->bitmap)
        printf(", bitmap free list");
    else
        printf(", chain ok (%u segs)", c->segs);
    if (c->root)
        printf(", root inode ok");
    printf("\n");
    printf("  mount.filsys -v %s -o offset=%llu %s /mnt\n",
           c->ed, (unsigned long long)c->byte, image);
}

static void print_miss(const struct cand *c) {
    printf("fs @ block %u  (byte %llu)  %s%s%s%s  isize=%u fsize=%u  REJECTED: %s\n",
           c->blk, (unsigned long long)c->byte, c->class_desc,
           c->note[0] ? " (" : "", c->note[0] ? c->note : "",
           c->note[0] ? ")" : "", c->isz, c->fsz, c->why);
}

static int cand_cmp(const void *a, const void *b) {
    const struct cand *ca = a, *cb = b;
    if (ca->valid != cb->valid)
        return ca->valid ? -1 : 1;
    if (ca->byte != cb->byte)
        return ca->byte < cb->byte ? -1 : 1;
    return 0;
}

static void emit(const char *image, int verbose) {
    for (int i = 0; i < ncand; i++) {
        if (!cands[i].valid || cands[i].drop)
            continue;
        for (int j = 0; j < ncand; j++) {
            if (j == i)
                continue;
            if (cands[j].byte >= cands[i].byte && cands[j].byte < cands[i].end)
                cands[j].drop = 1;
        }
    }
    qsort(cands, (size_t)ncand, sizeof *cands, cand_cmp);
    for (int i = 0; i < ncand; i++) {
        if (cands[i].drop)
            continue;
        if (cands[i].valid)
            print_cand(image, &cands[i]);
        else if (verbose)
            print_miss(&cands[i]);
    }
}

/* Run one edition's probe at `base` (the filesystem start byte). */
static int probe_one(const filsys_format_t *f, int fd, uint64_t base, int bsize,
                     uint64_t sz, filsys_probe_t *res) {
    filsys_edition_t desc = filsys_getformat(f->edition);
    if (!desc.ops)
        return 0;
    desc.fd = fd;
    desc.io = &filsys_io_file;
    return desc.ops->probe(&desc, &filsys_io_file, base, bsize, sz, res);
}

/* A "strong" edition identifies itself by a superblock magic word or the V8
 * family's rearranged superblock, so it takes precedence over the heuristic
 * (no-magic) V7/32V/Coherent/BSD probes -- exactly the precedence findfs had
 * before the per-edition fold (System V checked before V7, V8 before BSD). */
static int edition_is_strong(const filsys_format_t *f) {
    filsys_edition_t d = filsys_getformat(f->edition);
    return d.sb_decode != NULL || d.magic != 0;
}

/* The block sizes to try for one edition: the descriptor's own for the byte-
 * addressed formats, and the full V8-family range (1024/4096 for V8/V10, 8192
 * for V9) for the rearranged-superblock editions. */
static int edition_bsizes(const filsys_format_t *f, int sizes[3]) {
    filsys_edition_t d = filsys_getformat(f->edition);
    if (!d.sb_decode) {
        sizes[0] = (int)d.bsize;
        return 1;
    }
    if (f->edition == FILSYS_V9) {
        sizes[0] = 8192;
        return 1;
    }
    sizes[0] = 1024; sizes[1] = 4096;
    return 2;   /* V8 / V10 */
}

int main(int argc, char **argv) {
    int stride = 1, backtrace = 0, edition_filter = -1, verbose = 0;
    const char *image = NULL;
    int c;
    while ((c = getopt(argc, argv, "v:s:iV")) != -1) {
        switch (c) {
        case 'v':
            edition_filter = filsys_parse_edition("findfs.filsys", optarg);
            if (edition_filter < 0)
                return 2;
            break;
        case 's': stride = atoi(optarg); break;
        case 'i': backtrace = 1; break;
        case 'V': verbose = 1; break;
        default:
            fprintf(stderr, "usage: findfs.filsys [-v <edition>] [-s N] [-i] [-V] <image>\n"
                            "  editions: %s\n", filsys_editions_usage());
            return 2;
        }
    }
    if (optind >= argc || stride < 1) {
        fprintf(stderr, "usage: findfs.filsys [-v <edition>] [-s N] [-i] [-V] <image>\n"
                        "  editions: %s\n", filsys_editions_usage());
        return 2;
    }
    image = argv[optind];

    int fd = open(image, O_RDONLY);
    if (fd < 0) { perror(image); return 1; }
    uint64_t sz = 0;
    if (filsys_dev_size(fd, &sz) || sz < BSIZE) { fprintf(stderr, "cannot size image\n"); return 1; }
    uint32_t nblocks = (uint32_t)(sz / BSIZE);
    uint8_t *sb_buf = malloc(8192);
    if (!sb_buf) { fprintf(stderr, "out of memory\n"); return 1; }

    int found = 0;
    uint64_t skip_end = 0;

    /* One pass over filesystem-start offsets (512-byte units).  A bsize-byte-block
     * filesystem stores its superblock at block 1 = byte bsize, so its start is
     * bsize-aligned; each edition's probe is tried only where its block size
     * aligns. */
    for (uint32_t base512 = 0; base512 < nblocks; base512 += (uint32_t)stride) {
        uint64_t base = (uint64_t)base512 * BSIZE;
        if (base < skip_end) {
            base512 = (uint32_t)(skip_end / BSIZE);
            continue;
        }

        int strong_hit = 0;
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 1 && strong_hit)
                break;   /* a magic/V8 match outranks the heuristic probes */
            for (size_t i = 0; ; i++) {
                const filsys_format_t *f = filsys_format_nth(i);
                if (!f)
                    break;
                if (f->edition == FILSYS_PDP7)
                    continue;   /* word-addressed, probed below */
                if ((pass == 0) != edition_is_strong(f))
                    continue;
                int sizes[3];
                int ns = edition_bsizes(f, sizes);
                for (int b = 0; b < ns; b++) {
                    int bsize = sizes[b];
                    if (base % (uint64_t)bsize != 0 || base + (uint64_t)bsize > sz)
                        continue;
                    filsys_probe_t res;
                    memset(&res, 0, sizeof res);
                    int r = probe_one(f, fd, base, bsize, sz, &res);
                    if (r == 1 && matches(edition_filter, res.class)) {
                        uint64_t de = base + (uint64_t)res.fsize * res.blocksize;
                        add_cand(base512, base, res.class, res.note, res.isize, res.fsize, res.segs,
                                 strcmp(res.class, "v4, v5, v6 or usgpg3") != 0,
                                 res.bitmap, 1, de, NULL);
                        if (de > skip_end)
                            skip_end = de;
                        if (pass == 0)
                            strong_hit = 1;
                        found++;
                    } else if (r == 2 && matches(edition_filter, res.class)) {
                        add_cand(base512, base, res.class, res.note, res.isize, res.fsize, 0,
                                 0, res.bitmap, 0, 0, res.why);
                        found++;
                    }
                }
            }
        }

        /* inode-table backtrace: is block base+2 the first i-list block of a
         * filesystem whose (damaged) superblock sits at base+1? */
        if (backtrace && base + 2 * (uint64_t)BSIZE <= sz &&
            pread(fd, sb_buf, BSIZE, (off_t)(base + 2 * BSIZE)) == BSIZE &&
            inode_block(sb_buf)) {
            filsys_probe_t res;
            memset(&res, 0, sizeof res);
            int r = 0;
            for (size_t i = 0; ; i++) {
                const filsys_format_t *f = filsys_format_nth(i);
                if (!f)
                    break;
                if (f->edition != FILSYS_V7 && f->edition != FILSYS_V6)
                    continue;
                r = probe_one(f, fd, base, 512, sz, &res);
                if (r >= 1)
                    break;
                r = 0;
            }
            if (r >= 1 && matches(edition_filter, res.class)) {
                printf("fs @ block %u  (byte %llu)  %s  isize=%u fsize=%u   [via inode backtrace]\n",
                       base512, (unsigned long long)base, res.class, res.isize, res.fsize);
                found++;
            }
        }
    }
    free(sb_buf);

    emit(image, verbose);

    /* PDP-7 (V0): word-addressed, at a fixed surface offset, so it never lands in
     * the byte-aligned block scan above.  p7_probe tries all three codecs at both
     * surfaces itself. */
    {
        filsys_edition_t desc = filsys_getformat(FILSYS_PDP7);
        desc.fd = fd;
        desc.io = &filsys_io_file;
        filsys_probe_t res;
        memset(&res, 0, sizeof res);
        if (desc.ops->probe(&desc, &filsys_io_file, 0, 0, sz, &res) == 1 &&
            matches(edition_filter, res.class)) {
            printf("fs @ byte %llu  PDP-7 (%s)  chain ok (%u segs), root inode ok\n",
                   (unsigned long long)res.base, res.packing ? res.packing : "rb09", res.segs);
            printf("  mount.filsys -v pdp7 -o offset=%llu -o packing=%s %s /mnt\n",
                   (unsigned long long)res.base, res.packing ? res.packing : "rb09", image);
            found++;
        }
    }

    close(fd);
    return found ? 0 : 1;
}
