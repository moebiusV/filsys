/* fuzz/filsys-fuzz.c - libFuzzer harness for the filsys read path.
 *
 * Feed an arbitrary byte buffer to the same code that parses an untrusted disk
 * image -- the entry points a hostile filesystem reaches first:
 *
 *   filsys_detect()      autodetect across every edition (findfs's probe), then
 *   filsys_open_arch()   parse the superblock for the detected edition, then
 *   filsys_invariants()  the full integrity walk (icheck+dcheck, read-only,
 *                        no output -- the same walk filsys_check drives).
 *
 * Most corpus bytes are rejected by filsys_detect; that is the point.  The
 * harness proves none of the parsers crash, hang, or read/write out of bounds
 * on arbitrary input -- the class the README's ASan/UBSan note describes.  It
 * is a development/corpus tool, not a shipped program: build with clang's
 * libFuzzer via `./configure --enable-fuzzing && make filsys-fuzz`, then
 * `./filsys-fuzz -max_total_time=60`.
 *
 * SPDX-License-Identifier: ISC
 */
#include <config.h>
#include "filsys.h"
#include "filsys_ops.h"        /* filsys_invariants (internal, quiet walk) */

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 512)
        return 0;               /* below the smallest superblock */

    /* The library reads by path/fd, so stage the input in a scratch file. */
    char tmpl[] = "/tmp/filsys-fuzz-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0)
        return 0;
    size_t off = 0;
    while (off < size) {
        ssize_t n = write(fd, data + off, size - off);
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    close(fd);

    int rfd = open(tmpl, O_RDONLY);
    if (rfd >= 0) {
        filsys_detect_t det;
        const char *why = NULL;
        if (filsys_detect(&det, rfd, 0, (uint64_t)size, FILSYS_UNIX, &why) == 0) {
            filsys_geom_t geom = { det.blocksize, det.freemap, det.byteorder };
            filsys_t *fs = NULL;
            const char *errmsg = NULL;
            /* readonly + no_lock: the fuzzer is the only process here. */
            if (filsys_open_arch(&fs, det.edition, tmpl, 1, 0, 0, 0,
                                 det.packing, NULL, 0, &geom, 1, &errmsg) == 0) {
                filsys_check_t rep;
                (void)filsys_invariants(fs, &rep);
                filsys_close(fs);
            }
        }
        close(rfd);
    }
    unlink(tmpl);
    return 0;
}
