/* instrument.h - test-build block-ownership table and mutation trace.
 *
 * Compile the library with FILSYS_INSTRUMENT (./configure --enable-instrumentation)
 * to track, per data block, who owns it -- {ino, logical, kind} -- and to log a
 * sequence-numbered trace of allocator and directory mutations.  The ownership
 * table turns an aliasing bug (a block claimed twice, class D) into a violation
 * reported at the allocator transition that commits it, instead of a class-D
 * report at the next fsck; the trace is dumped alongside the report so the
 * failure reads as a transcript, not a bare "FAIL at prefix N".
 *
 * When the flag is absent every hook is a no-op inline, so production builds
 * pay nothing.  The table is a single global: the instrumentation is for the
 * single-threaded test harness, not the FUSE path.
 *
 * SPDX-License-Identifier: ISC
 */
#ifndef FILSYS_INSTRUMENT_H
#define FILSYS_INSTRUMENT_H

#include <stdint.h>

#ifdef FILSYS_INSTRUMENT

/* Reset the ownership table (nblk data blocks) and the trace, then return the
 * number of blocks the table can track.  Call once per test scenario, after
 * the image is opened, so the table reflects only the mutations that follow. */
void filsys_instr_reset(uint32_t nblk);

/* Allocator transitions.  balloc asserts the block is currently free and marks
 * it allocated; bfree asserts it is currently owned and marks it free; assign
 * attributes an allocated block to an inode (ino, logical block, kind: 1 data,
 * 2 indirect).  A violation is printed with the trace and counted, never
 * fatal, so a test can inspect filsys_instr_violations() afterward. */
void filsys_instr_balloc(uint32_t bno);
void filsys_instr_bfree(uint32_t bno);
void filsys_instr_assign(uint32_t ino, uint32_t lbn, uint32_t bno, int kind);

/* Mutation trace (sequence-numbered, dumped on a violation). */
void filsys_instr_ialloc(uint32_t ino);
void filsys_instr_ifree(uint32_t ino);
void filsys_instr_dir_add(uint32_t parent, const char *name, uint32_t ino);
void filsys_instr_dir_remove(uint32_t parent, const char *name);
void filsys_instr_link(uint32_t ino, uint32_t from, uint32_t to);
void filsys_instr_truncate(uint32_t ino);

/* Dump the trace to stderr (called automatically on a violation). */
void filsys_instr_dump(void);

/* Number of ownership violations seen since the last reset. */
int filsys_instr_violations(void);

#else /* !FILSYS_INSTRUMENT */

static inline void filsys_instr_reset(uint32_t nblk) { (void)nblk; }
static inline void filsys_instr_balloc(uint32_t bno) { (void)bno; }
static inline void filsys_instr_bfree(uint32_t bno) { (void)bno; }
static inline void filsys_instr_assign(uint32_t ino, uint32_t lbn, uint32_t bno, int kind) {
    (void)ino; (void)lbn; (void)bno; (void)kind;
}
static inline void filsys_instr_ialloc(uint32_t ino) { (void)ino; }
static inline void filsys_instr_ifree(uint32_t ino) { (void)ino; }
static inline void filsys_instr_dir_add(uint32_t parent, const char *name, uint32_t ino) {
    (void)parent; (void)name; (void)ino;
}
static inline void filsys_instr_dir_remove(uint32_t parent, const char *name) {
    (void)parent; (void)name;
}
static inline void filsys_instr_link(uint32_t ino, uint32_t from, uint32_t to) {
    (void)ino; (void)from; (void)to;
}
static inline void filsys_instr_truncate(uint32_t ino) { (void)ino; }
static inline void filsys_instr_dump(void) {}
static inline int filsys_instr_violations(void) { return 0; }

#endif /* FILSYS_INSTRUMENT */

#endif /* FILSYS_INSTRUMENT_H */
