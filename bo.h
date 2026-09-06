/* bo.h - byte-order primitives for on-disk multi-byte quantities.
 *
 * Research Unix filesystems store multi-byte fields in three byte orders:
 *
 *   LE     little-endian:   low byte first    0 1 2 3   (32V, x86 Xenix)
 *   BE     big-endian:      high byte first   3 2 1 0   (SysV/68k, SysV/SPARC)
 *   PDP11  middle-endian:   high 16-bit word first      2 3 0 1   (V6/V7/Coherent)
 *
 * "Middle-endian" stores a 32-bit quantity as two little-endian 16-bit words,
 * most-significant word first.  A 16-bit quantity is little-endian in both LE
 * and PDP-11 (a single word), so bo_get16me == bo_get16le; only the 24-bit
 * packed block numbers and 32-bit daddr/off/time quantities differ.
 *
 * The explicit helpers (bo_get32le, bo_put16be, ...) are one copy of each
 * width/order; a byte_order_ops table bundles the six for a given order, and
 * each filesystem's format descriptor points at the table it uses, so a
 * consolidated algorithm calls through bo->get32() and gets the right order
 * for that filesystem.
 *
 * SPDX-License-Identifier: ISC
 */
#ifndef BO_H
#define BO_H

#include <stdint.h>

typedef struct byte_order_ops {
    uint16_t (*get16)(const uint8_t *p);
    void     (*put16)(uint8_t *p, uint16_t v);
    uint32_t (*get24)(const uint8_t *p);
    void     (*put24)(uint8_t *p, uint32_t v);
    uint32_t (*get32)(const uint8_t *p);
    void     (*put32)(uint8_t *p, uint32_t v);
} byte_order_ops_t;

/* little-endian */
uint16_t bo_get16le(const uint8_t *p);
void     bo_put16le(uint8_t *p, uint16_t v);
uint32_t bo_get24le(const uint8_t *p);
void     bo_put24le(uint8_t *p, uint32_t v);
uint32_t bo_get32le(const uint8_t *p);
void     bo_put32le(uint8_t *p, uint32_t v);

/* big-endian */
uint16_t bo_get16be(const uint8_t *p);
void     bo_put16be(uint8_t *p, uint16_t v);
uint32_t bo_get24be(const uint8_t *p);
void     bo_put24be(uint8_t *p, uint32_t v);
uint32_t bo_get32be(const uint8_t *p);
void     bo_put32be(uint8_t *p, uint32_t v);

/* middle-endian (PDP-11) */
uint16_t bo_get16me(const uint8_t *p);
void     bo_put16me(uint8_t *p, uint16_t v);
uint32_t bo_get24me(const uint8_t *p);
void     bo_put24me(uint8_t *p, uint32_t v);
uint32_t bo_get32me(const uint8_t *p);
void     bo_put32me(uint8_t *p, uint32_t v);

/* one ops table per byte order */
extern const byte_order_ops_t bo_le;   /* little-endian */
extern const byte_order_ops_t bo_be;   /* big-endian */
extern const byte_order_ops_t bo_me;   /* middle-endian (PDP-11) */

#endif /* BO_H */
