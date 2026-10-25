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

#include "d3d12_pso_dump.h"

#include "util/log.h"
#include "util/u_atomic.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static const char *
dump_dir(void)
{
   static const char *dir = []() -> const char * {
      const char *env = getenv("MESA_D3D12_PSO_DUMP_DIR");
      const char *cache = getenv("MESA_SHADER_CACHE_DIR");
      if (!env && cache) {
         char path[1024];
         snprintf(path, sizeof(path), "%s/pso-dump", cache);
         FILE *flag = fopen(path, "r");
         if (flag) {
            fclose(flag);
            env = cache;
         }
      }
      return env ? strdup(env) : NULL;
   }();
   return dir;
}

static FILE *
open_record(const char *dir, unsigned n, const char *suffix, const char *mode)
{
   char path[1024];
   snprintf(path, sizeof(path), "%s/pso-%04u%s", dir, n, suffix);
   FILE *f = fopen(path, mode);
   static bool warned = false;
   if (!f && !warned) {
      warned = true;
      mesa_logw("D3D12: cannot write the pipeline dump %s", path);
   }
   return f;
}

static void
write_bytes(const char *dir, unsigned n, const char *suffix, const void *p, size_t len)
{
   FILE *f = open_record(dir, n, suffix, "wb");
   if (!f)
      return;
   fwrite(p, 1, len, f);
   fclose(f);
}

static const GUID root_signature_blob_guid =
   { 0x4d455341, 0x5253, 0x4231, { 0x9b, 0x3c, 0x2a, 0x71, 0x1d, 0x4e, 0x4f, 0x55 } };

void
d3d12_pso_dump_keep_root_signature(ID3D12RootSignature *sig, const void *blob, size_t size)
{
   if (!dump_dir())
      return;
   if (FAILED(sig->SetPrivateData(root_signature_blob_guid, (UINT)size, blob)))
      mesa_logw("D3D12: cannot keep a root signature blob for the pipeline dump");
}

static std::vector<uint8_t>
root_signature_blob(ID3D12RootSignature *sig)
{
   const HRESULT more_data = (HRESULT)0x887A0003L; /* DXGI_ERROR_MORE_DATA: the size query's answer */
   std::vector<uint8_t> blob;
   UINT n = 0;
   if (!sig)
      return blob;
   HRESULT hr = sig->GetPrivateData(root_signature_blob_guid, &n, NULL);
   if ((FAILED(hr) && hr != more_data) || n == 0)
      return blob;
   blob.resize(n);
   if (sig->GetPrivateData(root_signature_blob_guid, &n, blob.data()) != S_OK)
      blob.clear();
   return blob;
}

int
d3d12_pso_dump_begin(const struct d3d12_gfx_pipeline_state *state, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d,
                     const d3d12_depth_stencil_desc_type *ds2, bool stream_form)
{
   const char *dir = dump_dir();
   if (!dir)
      return -1;
   /* Every context on the screen shares the counter; xemu creates pipelines from two threads. */
   static unsigned count = 0;
   const unsigned n = (unsigned)(p_atomic_inc_return(&count) - 1);
   FILE *f = open_record(dir, n, ".txt", "w");
   if (!f)
      return (int)n;
   fprintf(f, "format 1\n");
   fprintf(f, "form %s\n", stream_form ? "stream" : "v0");
   fprintf(f, "prim %d\nnum_so_targets %u\nsamples %u\nnum_cbufs %u\n", (int)state->prim_type,
           state->num_so_targets, state->samples, state->num_cbufs);
   const struct { const char *name; const D3D12_SHADER_BYTECODE *code; } stages[] = {
      { "vs", &d->VS }, { "hs", &d->HS }, { "ds", &d->DS }, { "gs", &d->GS }, { "ps", &d->PS },
   };
   for (const auto &s : stages) {
      fprintf(f, "%s.bytes %u\n", s.name, (unsigned)s.code->BytecodeLength);
      if (s.code->BytecodeLength) {
         char suffix[16];
         snprintf(suffix, sizeof(suffix), "-%s.dxbc", s.name);
         write_bytes(dir, n, suffix, s.code->pShaderBytecode, s.code->BytecodeLength);
      }
   }
   std::vector<uint8_t> rs = root_signature_blob(d->pRootSignature);
   fprintf(f, "rs.bytes %u\n", (unsigned)rs.size());
   if (!rs.empty())
      write_bytes(dir, n, "-rs.bin", rs.data(), rs.size());

   const D3D12_STREAM_OUTPUT_DESC &so = d->StreamOutput;
   fprintf(f, "so.NumEntries %u\nso.pSODeclaration %s\nso.NumStrides %u\nso.pBufferStrides %s\nso.RasterizedStream %u\n",
           so.NumEntries, so.pSODeclaration ? "nonnull" : "null", so.NumStrides,
           so.pBufferStrides ? "nonnull" : "null", so.RasterizedStream);
   fprintf(f, "# so.entry<i> Stream SemanticName SemanticIndex StartComponent ComponentCount OutputSlot\n");
   for (unsigned i = 0; i < so.NumEntries; i++) {
      const D3D12_SO_DECLARATION_ENTRY &e = so.pSODeclaration[i];
      fprintf(f, "so.entry%u %u %s %u %u %u %u\n", i, e.Stream, e.SemanticName ? e.SemanticName : "-",
              e.SemanticIndex, e.StartComponent, e.ComponentCount, e.OutputSlot);
   }
   for (unsigned i = 0; i < so.NumStrides; i++)
      fprintf(f, "so.stride%u %u\n", i, so.pBufferStrides[i]);

   const D3D12_BLEND_DESC &b = d->BlendState;
   fprintf(f, "blend.AlphaToCoverageEnable %d\nblend.IndependentBlendEnable %d\n",
           b.AlphaToCoverageEnable, b.IndependentBlendEnable);
   fprintf(f, "# blend.rt<i> BlendEnable LogicOpEnable SrcBlend DestBlend BlendOp SrcBlendAlpha DestBlendAlpha BlendOpAlpha LogicOp RenderTargetWriteMask\n");
   for (unsigned i = 0; i < 8; i++) {
      const D3D12_RENDER_TARGET_BLEND_DESC &rt = b.RenderTarget[i];
      fprintf(f, "blend.rt%u %d %d %d %d %d %d %d %d %d %u\n", i, rt.BlendEnable, rt.LogicOpEnable, rt.SrcBlend,
              rt.DestBlend, rt.BlendOp, rt.SrcBlendAlpha, rt.DestBlendAlpha, rt.BlendOpAlpha, rt.LogicOp,
              rt.RenderTargetWriteMask);
   }
   fprintf(f, "SampleMask 0x%08x\n", d->SampleMask);

   const D3D12_RASTERIZER_DESC &r = d->RasterizerState;
   fprintf(f, "rast.FillMode %d\nrast.CullMode %d\nrast.FrontCounterClockwise %d\nrast.DepthBias %d\n"
              "rast.DepthBiasClamp %.9g\nrast.SlopeScaledDepthBias %.9g\nrast.DepthClipEnable %d\n"
              "rast.MultisampleEnable %d\nrast.AntialiasedLineEnable %d\nrast.ForcedSampleCount %u\n"
              "rast.ConservativeRaster %d\n",
           r.FillMode, r.CullMode, r.FrontCounterClockwise, r.DepthBias, r.DepthBiasClamp, r.SlopeScaledDepthBias,
           r.DepthClipEnable, r.MultisampleEnable, r.AntialiasedLineEnable, r.ForcedSampleCount, r.ConservativeRaster);

   const D3D12_DEPTH_STENCIL_DESC &z = d->DepthStencilState;
   fprintf(f, "ds.DepthEnable %d\nds.DepthWriteMask %d\nds.DepthFunc %d\nds.StencilEnable %d\n"
              "ds.StencilReadMask %u\nds.StencilWriteMask %u\n",
           z.DepthEnable, z.DepthWriteMask, z.DepthFunc, z.StencilEnable, z.StencilReadMask, z.StencilWriteMask);
   fprintf(f, "# ds.FrontFace/BackFace StencilFailOp StencilDepthFailOp StencilPassOp StencilFunc\n");
   fprintf(f, "ds.FrontFace %d %d %d %d\nds.BackFace %d %d %d %d\n",
           z.FrontFace.StencilFailOp, z.FrontFace.StencilDepthFailOp, z.FrontFace.StencilPassOp, z.FrontFace.StencilFunc,
           z.BackFace.StencilFailOp, z.BackFace.StencilDepthFailOp, z.BackFace.StencilPassOp, z.BackFace.StencilFunc);
   fprintf(f, "# ds2.*: Mesa's depth-stencil input beyond the V0 fields; only the stream form passes them on\n");
   fprintf(f, "ds2.DepthBoundsTestEnable %d\n", ds2->DepthBoundsTestEnable);
#ifndef _GAMING_XBOX
   fprintf(f, "ds2.FrontFaceMasks %u %u\nds2.BackFaceMasks %u %u\n",
           ds2->FrontFace.StencilReadMask, ds2->FrontFace.StencilWriteMask,
           ds2->BackFace.StencilReadMask, ds2->BackFace.StencilWriteMask);
#endif

   const D3D12_INPUT_LAYOUT_DESC &ia = d->InputLayout;
   fprintf(f, "ia.NumElements %u\nia.pInputElementDescs %s\n", ia.NumElements,
           ia.pInputElementDescs ? "nonnull" : "null");
   fprintf(f, "# ia.el<i> SemanticName SemanticIndex Format InputSlot AlignedByteOffset InputSlotClass InstanceDataStepRate\n");
   for (unsigned i = 0; i < ia.NumElements; i++) {
      const D3D12_INPUT_ELEMENT_DESC &e = ia.pInputElementDescs[i];
      fprintf(f, "ia.el%u %s %u %d %u %u %d %u\n", i, e.SemanticName ? e.SemanticName : "-", e.SemanticIndex,
              e.Format, e.InputSlot, e.AlignedByteOffset, e.InputSlotClass, e.InstanceDataStepRate);
   }

   fprintf(f, "IBStripCutValue %d\nPrimitiveTopologyType %d\nNumRenderTargets %u\n",
           d->IBStripCutValue, d->PrimitiveTopologyType, d->NumRenderTargets);
   for (unsigned i = 0; i < 8; i++)
      fprintf(f, "rtv%u %d\n", i, d->RTVFormats[i]);
   fprintf(f, "DSVFormat %d\nSampleDesc.Count %u\nSampleDesc.Quality %u\nNodeMask %u\nCachedPSO.bytes %u\nFlags %d\n",
           d->DSVFormat, d->SampleDesc.Count, d->SampleDesc.Quality, d->NodeMask,
           (unsigned)d->CachedPSO.CachedBlobSizeInBytes, d->Flags);
   fclose(f);
   return (int)n;
}

void
d3d12_pso_dump_end(int n, HRESULT hr)
{
   if (n < 0)
      return;
   FILE *f = open_record(dump_dir(), (unsigned)n, ".txt", "a");
   if (!f)
      return;
   fprintf(f, "hr 0x%08lx\n", (unsigned long)hr);
   fclose(f);
}
