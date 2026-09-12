/* filsys_detect.c - autodetect an edition + geometry at a fixed offset.
 *
 * The read-only probes live in the backends (v7fs.c/v1fs.c/pdp7fs.c, exposed
 * through each edition's ops->probe); this file is the thin wrapper mount/fsck
 * call: probe every edition at `offset`, in the same precedence order findfs
 * uses (magic/V8 first, then the heuristic V7/32V/Coherent/BSD probes), and map
 * the winning equivalence class to a FILSYS_* edition plus geometry.
 *
 * SPDX-License-Identifier: ISC
 */
#include <config.h>
#include <string.h>

#include "filsys.h"
#include "filsys_ops.h"
#include "v7fs.h"

static int edition_is_strong(const filsys_format_t *f) {
    filsys_edition_t d = filsys_getformat(f->edition);
    return d.sb_decode != NULL || d.magic != 0;
}

/* Map a probe's equivalence class to a FILSYS_* edition + geometry, honouring
 * `filter` (FILSYS_UNIX/-1 = the canonical, earliest member; else must be one
 * of the class's candidate editions).  Returns 0 or -1 with *why. */
static int resolve_class(const char *class, const filsys_probe_t *res,
                         int filter, filsys_detect_t *out, const char **why)
{
    int candidates[3] = { -1, -1, -1 };
    uint32_t blocksize = res->blocksize;
    int freemap = res->freemap;
    const char *byteorder = res->byteorder;

    if (!strcmp(class, "v1, v2 or v3"))      { candidates[0] = FILSYS_V1; blocksize = 512; }
    else if (!strcmp(class, "v4, v5, v6 or usgpg3")) { candidates[0] = FILSYS_V6; blocksize = 512; }
    else if (!strcmp(class, "v7, sysiii or sysvr1")) { candidates[0] = FILSYS_V7; candidates[1] = FILSYS_SYSIII; blocksize = 512; }
    else if (!strcmp(class, "32V or sysiii")) { candidates[0] = FILSYS_32V; candidates[1] = FILSYS_SYSIII; blocksize = 512; }
    else if (!strcmp(class, "Coherent"))    { candidates[0] = FILSYS_COHERENT; blocksize = 512; }
    else if (!strcmp(class, "Xenix"))       { candidates[0] = FILSYS_XENIX; blocksize = 1024; }
    else if (!strcmp(class, "2.9BSD"))      { candidates[0] = FILSYS_BSD29; blocksize = 1024; }
    else if (!strcmp(class, "2.11BSD"))     { candidates[0] = FILSYS_BSD211; blocksize = 1024; }
    else if (!strcmp(class, "sysvr2 or sysvr3")) { candidates[0] = FILSYS_SVR2; blocksize = 0; byteorder = NULL; }
    else if (!strcmp(class, "sysvr4"))      { candidates[0] = FILSYS_SVR4; blocksize = 0; byteorder = NULL; }
    else if (!strcmp(class, "v9"))          { candidates[0] = FILSYS_V9; blocksize = 8192; }
    else if (!strcmp(class, "v8 or v10, bs=1024")) { candidates[0] = FILSYS_V8; candidates[1] = FILSYS_V10; blocksize = 1024; freemap = FILSYS_FREEMAP_LIST; }
    else if (!strcmp(class, "v8 or v10, bs=4096, bitmap")) { candidates[0] = FILSYS_V8; candidates[1] = FILSYS_V10; blocksize = 4096; freemap = FILSYS_FREEMAP_BITMAP; }
    else if (!strcmp(class, "v10, bs=4096, bigmap")) { candidates[0] = FILSYS_V10; blocksize = 4096; freemap = FILSYS_FREEMAP_BIGMAP; }
    else if (!strcmp(class, "PDP-7"))       { candidates[0] = FILSYS_PDP7; blocksize = 0; }
    else {
        *why = !strcmp(class, "AFS") ? "AFS bitmap free list is not supported"
                                     : "unrecognised filesystem";
        return -1;
    }

    int edition = candidates[0];
    if (filter >= 0 && filter != FILSYS_UNIX) {
        edition = -1;
        for (int k = 0; k < 3 && candidates[k] >= 0; k++)
            if (candidates[k] == filter) { edition = filter; break; }
        if (edition < 0) {
            *why = "not a match for the requested edition";
            return -1;
        }
    }

    out->edition   = edition;
    out->blocksize = blocksize;
    out->freemap   = freemap;
    out->byteorder = byteorder;
    out->packing   = (edition == FILSYS_PDP7) ? res->packing : NULL;
    return 0;
}

/* The block sizes to try for one edition: the descriptor's own for the byte-
 * addressed formats, and the full V8-family range (1024/4096 for V8/V10, 8192
 * for V9) for the rearranged-superblock editions, whose block size is chosen
 * per volume rather than fixed by the edition. */
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

int filsys_detect(filsys_detect_t *out, int fd, uint64_t offset, uint64_t size,
                  int filter, const char **why)
{
    *why = NULL;
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; ; i++) {
            const filsys_format_t *f = filsys_format_nth(i);
            if (!f)
                break;
            filsys_edition_t desc = filsys_getformat(f->edition);
            if (!desc.ops)
                continue;
            if ((pass == 0) != edition_is_strong(f))
                continue;
            if (f->edition == FILSYS_PDP7) {
                /* word-addressed: only a surface-0 image is reachable here */
                if (offset != 0)
                    continue;
                desc.fd = fd;
                desc.io = &filsys_io_file;
                filsys_probe_t res;
                memset(&res, 0, sizeof res);
                if (desc.ops->probe(&desc, &filsys_io_file, offset, 0, size, &res) == 1)
                    return resolve_class(res.class, &res, filter, out, why);
                continue;
            }
            int sizes[3];
            int ns = edition_bsizes(f, sizes);
            for (int b = 0; b < ns; b++) {
                int bsize = sizes[b];
                if (offset % (uint64_t)bsize != 0 || offset + (uint64_t)bsize > size)
                    continue;
                desc.fd = fd;
                desc.io = &filsys_io_file;
                filsys_probe_t res;
                memset(&res, 0, sizeof res);
                if (desc.ops->probe(&desc, &filsys_io_file, offset, bsize, size, &res) == 1)
                    return resolve_class(res.class, &res, filter, out, why);
            }
        }
    }
    *why = "no filesystem recognised at this offset";
    return -1;
}
