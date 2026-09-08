/* filsys_names.c - the edition name table and its lookup.
 *
 * One row per on-disk format: the canonical "-v" spelling, its alternates, and
 * the FILSYS_* selector.  The tools resolve and list editions through this
 * table rather than their own strcmp chains, so the spellings live in one place
 * and cannot drift.
 *
 * This lives apart from filsys_format.c (the descriptor table) to keep the
 * edition *names* separate from the per-format on-disk descriptors.
 *
 * SPDX-License-Identifier: ISC
 */
#include <stdio.h>
#include <string.h>

#include "filsys.h"
#include "byteorder.h"

/* `name` is the canonical "-v" spelling; `aliases` are the alternates the tools
 * have always accepted: the numeric edition numbers, the pre-rename spellings,
 * and the family members that share a format (V2/V3 with V1, V4/V5 with V6), and
 * the format-equivalent releases verified against real media (USG PG3 shares
 * V6's layout; System V Release 1 shares V7's).  A leading "v"/"V" is stripped
 * by filsys_edition_by_name, so "v7" and "7" resolve alike. */
static const char *const pdp7_alias[]     = { "0", "p7", NULL };
static const char *const v1_alias[]       = { "1", "2", "3", NULL };
static const char *const v6_alias[]       = { "4", "5", "6", "usgpg3", NULL };
static const char *const v7_alias[]       = { "7", "sysvr1", NULL };
static const char *const v8_alias[]       = { "8", NULL };
static const char *const v9_alias[]       = { "9", NULL };
static const char *const v10_alias[]      = { "10", NULL };
static const char *const vax32_alias[]    = { "32v", "32", NULL };
static const char *const coherent_alias[] = { "coh", "33", NULL };
static const char *const xenix_alias[]    = { "34", NULL };
static const char *const bsd29_alias[]    = { "35", NULL };
static const char *const bsd211_alias[]   = { "36", NULL };
static const char *const sysiii_alias[]   = { "sys3", "sysiii-pdp11", NULL };
static const char *const sysvr2_alias[]   = { "sys5", "s5", "sysv2", "sysv", "sysvax", "sysv386", NULL };
static const char *const sysvr4_alias[]   = { "svr4", "sysv4", "s5r4", NULL };

static const filsys_format_t formats[] = {
    { FILSYS_PDP7,     "pdp7",     pdp7_alias },
    { FILSYS_V1,       "v1",       v1_alias },
    { FILSYS_V6,       "v6",       v6_alias },
    { FILSYS_V7,       "v7",       v7_alias },
    { FILSYS_V8,       "v8",       v8_alias },
    { FILSYS_V9,       "v9",       v9_alias },
    { FILSYS_V10,      "v10",      v10_alias },
    { FILSYS_32V,      "vax32",    vax32_alias },
    { FILSYS_COHERENT, "coherent", coherent_alias },
    { FILSYS_XENIX,    "xenix",    xenix_alias },
    { FILSYS_BSD29,    "bsd29",    bsd29_alias },
    { FILSYS_BSD211,   "bsd211",   bsd211_alias },
    { FILSYS_SYSIII,   "sysiii",   sysiii_alias },
    { FILSYS_SVR2,     "sysvr2",   sysvr2_alias },
    { FILSYS_SVR4,     "sysvr4",   sysvr4_alias },
};
static const size_t NFMT = sizeof formats / sizeof formats[0];

const filsys_format_t *filsys_format_nth(size_t i) {
    return i < NFMT ? &formats[i] : NULL;
}

int filsys_parse_edition(const char *tool, const char *name) {
    int e = filsys_edition_by_name(name);
    if (e < 0)
        fprintf(stderr, "%s: bad edition '%s'\n", tool, name);
    return e;
}

int filsys_edition_by_name(const char *name) {
    if (name == NULL)
        return -1;
    /* "vax32" predates the leading-v convention; match it (either case) before
     * the strip, which would otherwise turn it into "ax32". */
    if (!strcmp(name, "vax32") || !strcmp(name, "VAX32"))
        return FILSYS_32V;
    if (name[0] == 'v' || name[0] == 'V')
        name++;
    for (size_t i = 0; i < NFMT; i++) {
        if (!strcmp(name, formats[i].name))
            return formats[i].edition;
        for (const char *const *a = formats[i].aliases; a && *a; a++)
            if (!strcmp(name, *a))
                return formats[i].edition;
    }
    return -1;
}

const char *filsys_editions_usage(void) {
    static char buf[128];
    size_t n = 0;
    for (size_t i = 0; i < NFMT; i++)
        n += (size_t)snprintf(buf + n, sizeof buf - n, "%s%s",
                              i ? "|" : "", formats[i].name);
    return buf;
}

/* Map a CPU architecture name to the byte order it stored multi-byte fields in.
 * The format and the arch are orthogonal: System V ran on both endiannesses, so
 * the arch (not the edition) selects the byte order.  16-bit fields are the same
 * in PDP-11 middle-endian and little-endian (bo_me.get16 == bo_le.get16). */
const byte_order_ops_t *filsys_arch_bo(const char *arch) {
    if (arch == NULL)
        return NULL;
    /* The on-disk format varies only on the endianness axis, which is fixed by
     * the CPU.  PDP-11 stores 16-bit fields little-endian but 32-bit fields
     * middle-endian (bo_me); every other System V port is plain LE or BE. */
    static const struct { const char *name; const byte_order_ops_t *bo; } A[] = {
        { "pdp11",    &bo_me },
        { "vax",      &bo_le }, { "x86", &bo_le }, { "386", &bo_le }, { "i386", &bo_le },
        { "ns32k",    &bo_le }, { "ns32000", &bo_le }, { "i860", &bo_le }, { "clipper", &bo_le },
        { "3b2",      &bo_be }, { "3b20", &bo_be }, { "we32000", &bo_be },
        { "68k",      &bo_be }, { "m68k", &bo_be }, { "68000", &bo_be },
        { "sparc",    &bo_be }, { "mips", &bo_be },
        { "parisc",   &bo_be }, { "hppa", &bo_be }, { "powerpc", &bo_be }, { "ppc", &bo_be },
    };
    for (size_t i = 0; i < sizeof A / sizeof A[0]; i++)
        if (!strcmp(arch, A[i].name))
            return A[i].bo;
    return NULL;
}
