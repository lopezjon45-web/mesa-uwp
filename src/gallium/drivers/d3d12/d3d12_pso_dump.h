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

#ifndef D3D12_PSO_DUMP_H
#define D3D12_PSO_DUMP_H

#include "d3d12_pipeline_state.h"

/* A record of every graphics pipeline, written before its create call, for a driver that refuses one
 * without saying how. With MESA_D3D12_PSO_DUMP_DIR set, or a file named pso-dump inside
 * MESA_SHADER_CACHE_DIR (a UWP app has no environment of its own), the directory receives pso-NNNN.txt
 * ("format 1", then the V0 descriptor as "key value" lines, array elements positional as its # lines
 * say), pso-NNNN-rs.bin (the serialized root signature) and pso-NNNN-<stage>.dxbc; the HRESULT is
 * appended after the call. Off, nothing here does anything. */

/* Keeps the serialized blob on the root signature so the record can carry the bytes it was created from. */
void
d3d12_pso_dump_keep_root_signature(ID3D12RootSignature *sig, const void *blob, size_t size);

/* Writes the record; returns its index, or -1 when dumping is off. */
int
d3d12_pso_dump_begin(const struct d3d12_gfx_pipeline_state *state, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                     const d3d12_depth_stencil_desc_type *ds2, bool stream_form);

/* Appends the create call's HRESULT to record n. */
void
d3d12_pso_dump_end(int n, HRESULT hr);

#endif
