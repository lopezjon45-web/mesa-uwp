/*
 * Copyright © 2026 the mesa-uwp One S contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef DXBC_HASH_H
#define DXBC_HASH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "DXBC" + 16-byte digest: what the hash skips. */
#define DXBC_HEADER_SIZE 20

/* The DXBC MD5 variant over the whole container; 16 zero bytes if it is too short. */
void dxbc_hash(const void *container, size_t size, uint8_t digest[16]);

/* Writes the digest into the container's header in place. */
void dxbc_sign(void *container, size_t size);

#ifdef __cplusplus
}
#endif

#endif
