/* filsys_alloc_user.c - the userspace allocation arm (malloc/calloc/free).
 *
 * The engine calls filsys_alloc/filsys_free directly; this file is the
 * production userspace supplier.  A kernel build supplies its own arm, and a
 * fault-injection build supplies an arm that can fail allocations on demand.
 * kind is ignored here: every kind is a malloc.
 *
 * SPDX-License-Identifier: ISC
 */
#include "filsys_engine.h"

#include <stdlib.h>

void *filsys_alloc(filsys_alloc_kind_t kind, size_t n, int zero)
{
    (void)kind;
    return zero ? calloc(1, n) : malloc(n);
}

void filsys_free(filsys_alloc_kind_t kind, void *p, size_t n)
{
    (void)kind;
    (void)n;
    free(p);
}
