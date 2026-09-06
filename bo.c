/* bo.c - byte-order primitives (see bo.h).  The one copy of each order. */
/* SPDX-License-Identifier: ISC */
#include "bo.h"

/* ---- little-endian ------------------------------------------------------ */

uint16_t bo_get_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
void bo_put_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}
uint32_t bo_get_le24(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}
void bo_put_le24(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
}
uint32_t bo_get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
void bo_put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* ---- big-endian --------------------------------------------------------- */

uint16_t bo_get_be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
void bo_put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xff);
}
uint32_t bo_get_be24(const uint8_t *p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}
void bo_put_be24(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 16) & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)(v & 0xff);
}
uint32_t bo_get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
void bo_put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* ---- middle-endian (PDP-11) --------------------------------------------- */

uint16_t bo_get_me16(const uint8_t *p) {
    return bo_get_le16(p);   /* a 16-bit word is little-endian */
}
void bo_put_me16(uint8_t *p, uint16_t v) {
    bo_put_le16(p, v);
}
uint32_t bo_get_me24(const uint8_t *p) {
    /* 24-bit block number packed as [ hi, lo, mid ]. */
    return (uint32_t)p[1] | ((uint32_t)p[2] << 8) | ((uint32_t)p[0] << 16);
}
void bo_put_me24(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 16) & 0xff);  /* hi  */
    p[1] = (uint8_t)(v & 0xff);          /* lo  */
    p[2] = (uint8_t)((v >> 8) & 0xff);   /* mid */
}
uint32_t bo_get_me32(const uint8_t *p) {
    /* high 16-bit word first, each word little-endian */
    return ((uint32_t)bo_get_le16(p) << 16) | bo_get_le16(p + 2);
}
void bo_put_me32(uint8_t *p, uint32_t v) {
    bo_put_le16(p,     (uint16_t)(v >> 16));
    bo_put_le16(p + 2, (uint16_t)(v & 0xffff));
}

/* ---- one ops table per byte order --------------------------------------- */

const byte_order_ops_t bo_le = {
    bo_get_le16, bo_put_le16, bo_get_le24, bo_put_le24, bo_get_le32, bo_put_le32
};
const byte_order_ops_t bo_be = {
    bo_get_be16, bo_put_be16, bo_get_be24, bo_put_be24, bo_get_be32, bo_put_be32
};
const byte_order_ops_t bo_me = {
    bo_get_me16, bo_put_me16, bo_get_me24, bo_put_me24, bo_get_me32, bo_put_me32
};
