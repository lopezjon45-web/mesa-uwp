/*
 * Copyright © 2026 the mesa-uwp One S contributors
 * SPDX-License-Identifier: MIT
 *
 * DXBC container digest. D3D11 and D3D12 refuse a container whose 16-byte
 * digest does not match (measured on WARP: E_INVALIDARG for a zeroed or
 * corrupted digest, see one-s/dxbc/dxbccheck.cpp --fxc-experiment).
 *
 * The digest is MD5 (RFC 1321) over everything after the 20-byte header,
 * with a non-standard final block: the bit length goes in front of the last
 * partial block and a second word, (bits >> 2) | 1, closes it, and there is
 * no separate length block. The padding rules follow hashDxbcBinary in
 * dxbc-spirv (MIT, Copyright (c) 2025 Philip Rebohle).
 */

#include "dxbc_hash.h"

#include <string.h>

static const uint32_t md5_k[64] = {
   0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
   0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
   0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
   0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
   0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
   0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
   0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
   0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static const uint8_t md5_r[64] = {
   7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
   5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
   4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
   6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

struct md5 {
   uint32_t h[4];
   uint8_t buf[64];
   unsigned buf_len;
};

static uint32_t
rol(uint32_t x, unsigned n)
{
   return (x << n) | (x >> (32 - n));
}

/* Straight-line stores: a loop over shift amounts auto-vectorizes to AVX2
 * variable shifts on MSVC, which the One S ISA gate refuses. */
static void
put_le32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)v;
   p[1] = (uint8_t)(v >> 8);
   p[2] = (uint8_t)(v >> 16);
   p[3] = (uint8_t)(v >> 24);
}

static void
md5_block(uint32_t h[4], const uint8_t *p)
{
   uint32_t w[16];
   for (unsigned i = 0; i < 16; i++)
      w[i] = (uint32_t)p[4 * i] | ((uint32_t)p[4 * i + 1] << 8) |
             ((uint32_t)p[4 * i + 2] << 16) | ((uint32_t)p[4 * i + 3] << 24);

   uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
   for (unsigned i = 0; i < 64; i++) {
      uint32_t f, g;
      if (i < 16) {
         f = (b & c) | (~b & d);
         g = i;
      } else if (i < 32) {
         f = (d & b) | (~d & c);
         g = (5 * i + 1) & 15;
      } else if (i < 48) {
         f = b ^ c ^ d;
         g = (3 * i + 5) & 15;
      } else {
         f = c ^ (b | ~d);
         g = (7 * i) & 15;
      }
      uint32_t t = d;
      d = c;
      c = b;
      b = b + rol(a + f + md5_k[i] + w[g], md5_r[i]);
      a = t;
   }
   h[0] += a;
   h[1] += b;
   h[2] += c;
   h[3] += d;
}

static void
md5_update(struct md5 *m, const uint8_t *p, size_t n)
{
   while (n) {
      unsigned take = 64 - m->buf_len;
      if (take > n)
         take = (unsigned)n;
      memcpy(m->buf + m->buf_len, p, take);
      m->buf_len += take;
      p += take;
      n -= take;
      if (m->buf_len == 64) {
         md5_block(m->h, m->buf);
         m->buf_len = 0;
      }
   }
}

void
dxbc_hash(const void *container, size_t size, uint8_t digest[16])
{
   memset(digest, 0, 16);
   if (size < DXBC_HEADER_SIZE)
      return;

   const uint8_t *bytes = (const uint8_t *)container + DXBC_HEADER_SIZE;
   size -= DXBC_HEADER_SIZE;

   uint32_t a_num = (uint32_t)size * 8u;
   uint32_t b_num = (a_num >> 2) | 1u;
   uint8_t a[4], b[4];
   put_le32(a, a_num);
   put_le32(b, b_num);

   size_t remainder = size % 64;
   size_t padding = 64 - remainder;
   static const uint8_t pad[64] = { 0x80 };

   struct md5 m = { { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 }, { 0 }, 0 };
   md5_update(&m, bytes, size - remainder);
   if (remainder >= 56) {
      md5_update(&m, bytes + size - remainder, remainder);
      md5_update(&m, pad, padding);
      md5_update(&m, a, 4);
      md5_update(&m, pad + 4, 64 - 8);
      md5_update(&m, b, 4);
   } else {
      md5_update(&m, a, 4);
      if (remainder)
         md5_update(&m, bytes + size - remainder, remainder);
      md5_update(&m, pad, padding - 8);
      md5_update(&m, b, 4);
   }

   put_le32(digest, m.h[0]);
   put_le32(digest + 4, m.h[1]);
   put_le32(digest + 8, m.h[2]);
   put_le32(digest + 12, m.h[3]);
}

void
dxbc_sign(void *container, size_t size)
{
   if (size < DXBC_HEADER_SIZE)
      return;
   uint8_t digest[16];
   dxbc_hash(container, size, digest);
   memcpy((uint8_t *)container + 4, digest, 16);
}
