/* byteorder.c - byte-order primitives (see byteorder.h).  The one copy of each order. */
/* SPDX-License-Identifier: ISC */
#include "byteorder.h"

/* ---- little-endian ------------------------------------------------------ */

uint16_t bo_get16le(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
void bo_put16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}
uint32_t bo_get24le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}
void bo_put24le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
}
uint32_t bo_get32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
void bo_put32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* ---- big-endian --------------------------------------------------------- */

uint16_t bo_get16be(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
void bo_put16be(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xff);
}
uint32_t bo_get24be(const uint8_t *p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}
void bo_put24be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 16) & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)(v & 0xff);
}
uint32_t bo_get32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
void bo_put32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* ---- middle-endian (PDP-11) --------------------------------------------- */

uint16_t bo_get16me(const uint8_t *p) {
    return bo_get16le(p);   /* a 16-bit word is little-endian */
}
void bo_put16me(uint8_t *p, uint16_t v) {
    bo_put16le(p, v);
}
uint32_t bo_get24me(const uint8_t *p) {
    /* 24-bit block number packed as [ hi, lo, mid ]. */
    return (uint32_t)p[1] | ((uint32_t)p[2] << 8) | ((uint32_t)p[0] << 16);
}
void bo_put24me(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 16) & 0xff);  /* hi  */
    p[1] = (uint8_t)(v & 0xff);          /* lo  */
    p[2] = (uint8_t)((v >> 8) & 0xff);   /* mid */
}
uint32_t bo_get32me(const uint8_t *p) {
    /* high 16-bit word first, each word little-endian */
    return ((uint32_t)bo_get16le(p) << 16) | bo_get16le(p + 2);
}
void bo_put32me(uint8_t *p, uint32_t v) {
    bo_put16le(p,     (uint16_t)(v >> 16));
    bo_put16le(p + 2, (uint16_t)(v & 0xffff));
}

/* ---- packed 18-bit ------------------------------------------------------- */

uint32_t bo_get18packed(const uint8_t *p, uint32_t i) {
    uint32_t bit = i * 18;
    uint32_t v = 0;
    for (uint32_t b = 0; b < 18; b++)
        v = (v << 1) | ((p[(bit + b) >> 3] >> (7 - ((bit + b) & 7))) & 1u);
    return v;
}

void bo_put18packed(uint8_t *p, uint32_t i, uint32_t v) {
    uint32_t bit = i * 18;
    for (uint32_t b = 0; b < 18; b++) {
        uint32_t pos = bit + b;
        uint8_t mask = (uint8_t)(1u << (7 - (pos & 7)));
        if ((v >> (17 - b)) & 1u)
            p[pos >> 3] |= mask;
        else
            p[pos >> 3] &= (uint8_t)~mask;
    }
}

/* ---- one ops table per byte order --------------------------------------- */

const byte_order_ops_t bo_le = {
    bo_get16le, bo_put16le, bo_get24le, bo_put24le, bo_get32le, bo_put32le
};
const byte_order_ops_t bo_be = {
    bo_get16be, bo_put16be, bo_get24be, bo_put24be, bo_get32be, bo_put32be
};
const byte_order_ops_t bo_me = {
    bo_get16me, bo_put16me, bo_get24me, bo_put24me, bo_get32me, bo_put32me
};
