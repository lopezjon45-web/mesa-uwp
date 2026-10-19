// dxbccheck: checks DXBC containers the Mesa d3d12 driver dumped (D3D12_DEBUG=dxil) outside Mesa.
//
//   dxbccheck [--asm-dir DIR] blob...     one TSV line per blob: program type, stored digest state,
//                                         D3D11 FL 11_0 (WARP) shader creation as-is and after signing,
//                                         D3DDisassemble result (text saved under DIR)
//   dxbccheck --fxc-experiment            compiles two trivial shaders with d3dcompiler_47 and reports
//                                         whether D3D11 FL 11_0 and D3D12 (both WARP) accept the blob
//                                         with its digest zeroed, corrupted, and recomputed by this tool
//
// The digest is the DXBC variant of MD5: the stream after the 20-byte header is hashed with a
// non-standard final block (padding rules as in dxbc-spirv's hashDxbcBinary, MIT) and no final
// length block. The MD5 core follows RFC 1321.
//
// SPDX-License-Identifier: MIT

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

// ---- DXBC digest ----

namespace {

const uint32_t K[64] = {
   0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
   0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
   0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
   0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
   0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
   0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
   0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
   0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
const uint8_t R[64] = {
   7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
   5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
   4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
   6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

struct Md5 {
   uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
   uint8_t buf[64];
   unsigned len = 0;

   static uint32_t rol(uint32_t x, unsigned n) { return (x << n) | (x >> (32 - n)); }

   void block(const uint8_t *p)
   {
      uint32_t w[16];
      for (unsigned i = 0; i < 16; i++)
         w[i] = (uint32_t)p[4 * i] | ((uint32_t)p[4 * i + 1] << 8) | ((uint32_t)p[4 * i + 2] << 16) |
                ((uint32_t)p[4 * i + 3] << 24);
      uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
      for (unsigned i = 0; i < 64; i++) {
         uint32_t f, g;
         if (i < 16) { f = (b & c) | (~b & d); g = i; }
         else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
         else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
         else { f = c ^ (b | ~d); g = (7 * i) & 15; }
         uint32_t t = d;
         d = c;
         c = b;
         b = b + rol(a + f + K[i] + w[g], R[i]);
         a = t;
      }
      h[0] += a; h[1] += b; h[2] += c; h[3] += d;
   }

   void update(const uint8_t *p, size_t n)
   {
      while (n) {
         unsigned take = 64 - len;
         if (take > n) take = (unsigned)n;
         memcpy(buf + len, p, take);
         len += take; p += take; n -= take;
         if (len == 64) { block(buf); len = 0; }
      }
   }
};

void dxbc_hash(const uint8_t *container, size_t size, uint8_t digest[16])
{
   memset(digest, 0, 16);
   if (size < 20) return;
   const uint8_t *bytes = container + 20;
   size -= 20;

   uint32_t a_num = (uint32_t)size * 8u;
   uint32_t b_num = (a_num >> 2) | 1u;
   uint8_t a[4], b[4];
   for (unsigned i = 0; i < 4; i++) { a[i] = (a_num >> (8 * i)) & 0xff; b[i] = (b_num >> (8 * i)) & 0xff; }

   size_t remainder = size % 64;
   size_t padding = 64 - remainder;
   static const uint8_t pad[64] = {0x80};

   Md5 m;
   m.update(bytes, size - remainder);
   if (remainder >= 56) {
      m.update(bytes + size - remainder, remainder);
      m.update(pad, padding);
      m.update(a, 4);
      m.update(pad + 4, 64 - 8);
      m.update(b, 4);
   } else {
      m.update(a, 4);
      if (remainder) m.update(bytes + size - remainder, remainder);
      m.update(pad, padding - 8);
      m.update(b, 4);
   }
   for (unsigned i = 0; i < 4; i++)
      for (unsigned j = 0; j < 4; j++)
         digest[4 * i + j] = (m.h[i] >> (8 * j)) & 0xff;
}

// ---- container ----

enum class Program { PS = 0, VS = 1, GS = 2, HS = 3, DS = 4, CS = 5, Unknown = 99 };

const char *program_name(Program p)
{
   switch (p) {
   case Program::PS: return "ps";
   case Program::VS: return "vs";
   case Program::GS: return "gs";
   case Program::HS: return "hs";
   case Program::DS: return "ds";
   case Program::CS: return "cs";
   default: return "??";
   }
}

struct Container {
   std::vector<uint8_t> bytes;
   Program program = Program::Unknown;
   unsigned major = 0, minor = 0;
   bool valid = false;
};

uint32_t rd32(const std::vector<uint8_t> &b, size_t at)
{
   return (uint32_t)b[at] | ((uint32_t)b[at + 1] << 8) | ((uint32_t)b[at + 2] << 16) | ((uint32_t)b[at + 3] << 24);
}

Container parse(std::vector<uint8_t> bytes)
{
   Container c;
   c.bytes = std::move(bytes);
   const auto &b = c.bytes;
   if (b.size() < 32 || memcmp(b.data(), "DXBC", 4) != 0)
      return c;
   uint32_t total = rd32(b, 24), parts = rd32(b, 28);
   if (total != b.size() || 32 + 4 * (size_t)parts > b.size())
      return c;
   for (uint32_t i = 0; i < parts; i++) {
      uint32_t off = rd32(b, 32 + 4 * i);
      if (off + 8 > b.size()) return c;
      uint32_t size = rd32(b, off + 4);
      if (off + 8 + size > b.size()) return c;
      if (memcmp(&b[off], "SHEX", 4) == 0 || memcmp(&b[off], "SHDR", 4) == 0) {
         if (size < 8) return c;
         uint32_t version = rd32(b, off + 8);
         c.minor = version & 0xf;
         c.major = (version >> 4) & 0xf;
         uint32_t type = version >> 16;
         c.program = type <= 5 ? (Program)type : Program::Unknown;
         c.valid = true;
      }
   }
   return c;
}

enum class Digest { Zero, Ok, Mismatch };

Digest digest_state(const Container &c)
{
   uint8_t want[16];
   dxbc_hash(c.bytes.data(), c.bytes.size(), want);
   bool zero = true;
   for (unsigned i = 4; i < 20; i++)
      if (c.bytes[i]) zero = false;
   if (zero) return Digest::Zero;
   return memcmp(want, &c.bytes[4], 16) == 0 ? Digest::Ok : Digest::Mismatch;
}

void sign(std::vector<uint8_t> &bytes)
{
   uint8_t d[16];
   dxbc_hash(bytes.data(), bytes.size(), d);
   memcpy(&bytes[4], d, 16);
}

// ---- runtimes ----

ComPtr<ID3D11Device> d3d11_warp()
{
   ComPtr<ID3D11Device> dev;
   D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
   HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev,
                                  nullptr, nullptr);
   if (FAILED(hr))
      fprintf(stderr, "D3D11CreateDevice(WARP, 11_0) failed: 0x%08lx\n", hr);
   return dev;
}

HRESULT d3d11_create(ID3D11Device *dev, Program p, const std::vector<uint8_t> &b)
{
   switch (p) {
   case Program::VS: { ComPtr<ID3D11VertexShader> s; return dev->CreateVertexShader(b.data(), b.size(), nullptr, &s); }
   case Program::PS: { ComPtr<ID3D11PixelShader> s; return dev->CreatePixelShader(b.data(), b.size(), nullptr, &s); }
   case Program::GS: { ComPtr<ID3D11GeometryShader> s; return dev->CreateGeometryShader(b.data(), b.size(), nullptr, &s); }
   case Program::HS: { ComPtr<ID3D11HullShader> s; return dev->CreateHullShader(b.data(), b.size(), nullptr, &s); }
   case Program::DS: { ComPtr<ID3D11DomainShader> s; return dev->CreateDomainShader(b.data(), b.size(), nullptr, &s); }
   case Program::CS: { ComPtr<ID3D11ComputeShader> s; return dev->CreateComputeShader(b.data(), b.size(), nullptr, &s); }
   default: return E_INVALIDARG;
   }
}

ComPtr<ID3D12Device> d3d12_warp()
{
   ComPtr<IDXGIFactory4> factory;
   ComPtr<IDXGIAdapter> warp;
   ComPtr<ID3D12Device> dev;
   HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
   if (SUCCEEDED(hr)) hr = factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
   if (SUCCEEDED(hr)) hr = D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
   if (FAILED(hr))
      fprintf(stderr, "D3D12CreateDevice(WARP) failed: 0x%08lx\n", hr);
   return dev;
}

// A graphics PSO with no inputs, one RGBA8 target and an empty root signature: enough for the
// runtime to validate the two blobs.
HRESULT d3d12_pso(ID3D12Device *dev, const std::vector<uint8_t> &vs, const std::vector<uint8_t> &ps)
{
   D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs = {};
   rs.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
   ComPtr<ID3DBlob> rs_blob, err;
   HRESULT hr = D3D12SerializeVersionedRootSignature(&rs, &rs_blob, &err);
   if (FAILED(hr)) return hr;
   ComPtr<ID3D12RootSignature> root;
   hr = dev->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&root));
   if (FAILED(hr)) return hr;

   D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
   d.pRootSignature = root.Get();
   d.VS = {vs.data(), vs.size()};
   d.PS = {ps.data(), ps.size()};
   d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
   d.SampleMask = UINT_MAX;
   d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
   d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
   d.RasterizerState.DepthClipEnable = TRUE;
   d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
   d.NumRenderTargets = 1;
   d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
   d.SampleDesc.Count = 1;
   ComPtr<ID3D12PipelineState> pso;
   return dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
}

std::vector<uint8_t> compile(const char *src, const char *entry, const char *target)
{
   ComPtr<ID3DBlob> code, err;
   HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, target, 0, 0, &code, &err);
   if (FAILED(hr)) {
      fprintf(stderr, "D3DCompile(%s) failed 0x%08lx: %s\n", target, hr,
              err ? (const char *)err->GetBufferPointer() : "");
      return {};
   }
   const uint8_t *p = (const uint8_t *)code->GetBufferPointer();
   return std::vector<uint8_t>(p, p + code->GetBufferSize());
}

bool disassemble(const std::vector<uint8_t> &b, std::string *text, HRESULT *hr)
{
   ComPtr<ID3DBlob> out;
   *hr = D3DDisassemble(b.data(), b.size(), 0, nullptr, &out);
   if (FAILED(*hr)) return false;
   text->assign((const char *)out->GetBufferPointer(), out->GetBufferSize());
   while (!text->empty() && text->back() == 0) text->pop_back();
   return true;
}

std::vector<uint8_t> read_file(const char *path)
{
   std::vector<uint8_t> b;
   FILE *f = fopen(path, "rb");
   if (!f) return b;
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   b.resize(n > 0 ? (size_t)n : 0);
   if (n > 0 && fread(b.data(), 1, b.size(), f) != b.size()) b.clear();
   fclose(f);
   return b;
}

const char *basename_of(const char *path)
{
   const char *s = strrchr(path, '\\');
   const char *t = strrchr(path, '/');
   const char *p = s > t ? s : t;
   return p ? p + 1 : path;
}

int fxc_experiment()
{
   const char *vs_src = "float4 main(uint id : SV_VertexID) : SV_Position { float2 p = float2((id << 1) & 2, id & 2); "
                        "return float4(p * 2 - 1, 0, 1); }";
   const char *ps_src = "float4 main() : SV_Target { return float4(1, 0, 0, 1); }";
   ComPtr<ID3D11Device> d11 = d3d11_warp();
   ComPtr<ID3D12Device> d12 = d3d12_warp();
   if (!d11 || !d12) return 2;

   int failures = 0;
   const char *models[] = {"5_0", "5_1"};
   for (const char *sm : models) {
      std::string vs_t = std::string("vs_") + sm, ps_t = std::string("ps_") + sm;
      std::vector<uint8_t> vs = compile(vs_src, "main", vs_t.c_str()), ps = compile(ps_src, "main", ps_t.c_str());
      if (vs.empty() || ps.empty()) return 2;

      Container cv = parse(vs);
      Digest fxc_digest = digest_state(cv);
      printf("fxc %s\tour_hash_matches_fxc=%s\n", sm, fxc_digest == Digest::Ok ? "yes" : "NO");
      if (fxc_digest != Digest::Ok) failures++;

      struct Variant { const char *name; std::vector<uint8_t> vs, ps; } variants[3] = {
         {"as_compiled", vs, ps}, {"digest_zero", vs, ps}, {"digest_corrupt", vs, ps}};
      memset(&variants[1].vs[4], 0, 16);
      memset(&variants[1].ps[4], 0, 16);
      variants[2].vs[4] ^= 0x5a;
      variants[2].ps[4] ^= 0x5a;
      for (auto &v : variants) {
         HRESULT h11 = strcmp(sm, "5_0") == 0 ? d3d11_create(d11.Get(), Program::VS, v.vs) : S_FALSE;
         HRESULT h12 = d3d12_pso(d12.Get(), v.vs, v.ps);
         printf("fxc %s\t%s\td3d11_fl11_0=0x%08lx\td3d12=0x%08lx\n", sm, v.name, h11, h12);
      }
   }
   return failures ? 1 : 0;
}

} // namespace

int main(int argc, char **argv)
{
   std::string asm_dir;
   std::vector<const char *> files;
   for (int i = 1; i < argc; i++) {
      if (strcmp(argv[i], "--fxc-experiment") == 0)
         return fxc_experiment();
      if (strcmp(argv[i], "--asm-dir") == 0 && i + 1 < argc) {
         asm_dir = argv[++i];
         CreateDirectoryA(asm_dir.c_str(), nullptr);
         continue;
      }
      files.push_back(argv[i]);
   }
   if (files.empty()) {
      fprintf(stderr, "usage: dxbccheck [--asm-dir DIR] blob...\n       dxbccheck --fxc-experiment\n");
      return 2;
   }

   ComPtr<ID3D11Device> d11 = d3d11_warp();
   printf("file\tprogram\tsm\tbytes\tdigest\td3d11_as_is\td3d11_signed\tdisasm\n");
   int bad = 0;
   for (const char *path : files) {
      Container c = parse(read_file(path));
      if (!c.valid) {
         printf("%s\tinvalid-container\n", basename_of(path));
         bad++;
         continue;
      }
      Digest ds = digest_state(c);
      std::vector<uint8_t> signed_bytes = c.bytes;
      sign(signed_bytes);
      HRESULT as_is = d11 ? d3d11_create(d11.Get(), c.program, c.bytes) : E_FAIL;
      HRESULT after = d11 ? d3d11_create(d11.Get(), c.program, signed_bytes) : E_FAIL;
      std::string text;
      HRESULT dh;
      bool dis = disassemble(signed_bytes, &text, &dh);
      if (dis && !asm_dir.empty()) {
         std::string out = asm_dir + "\\" + basename_of(path) + ".asm";
         FILE *f = fopen(out.c_str(), "wb");
         if (f) { fwrite(text.data(), 1, text.size(), f); fclose(f); }
      }
      printf("%s\t%s\t%u_%u\t%zu\t%s\t0x%08lx\t0x%08lx\t%s\n", basename_of(path), program_name(c.program), c.major,
             c.minor, c.bytes.size(), ds == Digest::Zero ? "zero" : ds == Digest::Ok ? "ok" : "mismatch", as_is,
             after, dis ? "ok" : "FAIL");
      if (!dis) {
         fprintf(stderr, "%s: D3DDisassemble failed 0x%08lx\n", path, dh);
         bad++;
      }
   }
   return bad ? 1 : 0;
}
