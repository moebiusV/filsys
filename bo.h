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
 * and PDP-11 (a single word), so bo_get_me16 == bo_get_le16; only the 24-bit
 * packed block numbers and 32-bit daddr/off/time quantities differ.
 *
 * The explicit helpers (bo_get_le32, bo_put_be16, ...) are one copy of each
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
uint16_t bo_get_le16(const uint8_t *p);
void     bo_put_le16(uint8_t *p, uint16_t v);
uint32_t bo_get_le24(const uint8_t *p);
void     bo_put_le24(uint8_t *p, uint32_t v);
uint32_t bo_get_le32(const uint8_t *p);
void     bo_put_le32(uint8_t *p, uint32_t v);

/* big-endian */
uint16_t bo_get_be16(const uint8_t *p);
void     bo_put_be16(uint8_t *p, uint16_t v);
uint32_t bo_get_be24(const uint8_t *p);
void     bo_put_be24(uint8_t *p, uint32_t v);
uint32_t bo_get_be32(const uint8_t *p);
void     bo_put_be32(uint8_t *p, uint32_t v);

/* middle-endian (PDP-11) */
uint16_t bo_get_me16(const uint8_t *p);
void     bo_put_me16(uint8_t *p, uint16_t v);
uint32_t bo_get_me24(const uint8_t *p);
void     bo_put_me24(uint8_t *p, uint32_t v);
uint32_t bo_get_me32(const uint8_t *p);
void     bo_put_me32(uint8_t *p, uint32_t v);

/* one ops table per byte order */
extern const byte_order_ops_t bo_le;   /* little-endian */
extern const byte_order_ops_t bo_be;   /* big-endian */
extern const byte_order_ops_t bo_me;   /* middle-endian (PDP-11) */

#endif /* BO_H */
