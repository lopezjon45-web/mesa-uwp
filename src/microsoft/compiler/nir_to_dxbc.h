/*
 * Copyright © Microsoft Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/* NIR to DXBC (SM 5.0 / 5.1). Origin: Mesa MR !14253 (Jesse Natalie, 2021),
 * ported onto the 26.2 NIR API for the One S spike. */

#ifndef NIR_TO_DXBC_H
#define NIR_TO_DXBC_H

#include <stdbool.h>
#include <stddef.h>

#include "nir.h"
#include "nir_to_dxil.h"

#ifdef __cplusplus
extern "C" {
#endif

struct blob;

/* `opts->shader_model_max` picks 5.0 or 5.1. On failure `err` names the
 * first unsupported instruction. The container's digest is left zero. */
bool
nir_to_dxbc(struct nir_shader *s, const struct nir_to_dxil_options *opts,
            struct blob *blob, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif
