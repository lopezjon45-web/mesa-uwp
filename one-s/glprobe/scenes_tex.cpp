#include "glprobe.h"

#include <cmath>

static const char *const FS_TEX2D = R"(#version 400 core
uniform sampler2D tex;
in vec2 uv;
layout(location = 0) out vec4 o;
void main() { o = texture(tex, uv); }
)";

static RGBA grid_texel(int i, int j)
{
   return {uint8_t(40 + 60 * i), uint8_t(40 + 60 * j), uint8_t(20 + 30 * (i + j)), 255};
}

// size x size RGBA8 texture of grid_texel colors.
static std::vector<uint8_t> grid_pixels(int size)
{
   std::vector<uint8_t> px;
   for (int j = 0; j < size; j++)
      for (int i = 0; i < size; i++) {
         RGBA c = grid_texel(i, j);
         px.insert(px.end(), {c.r, c.g, c.b, c.a});
      }
   return px;
}

static std::vector<uint8_t> solid_pixels(int count, RGBA c)
{
   std::vector<uint8_t> px;
   for (int i = 0; i < count; i++)
      px.insert(px.end(), {c.r, c.g, c.b, c.a});
   return px;
}

static void quadrant_viewport(int q)
{
   glViewport((q % 2) * 32, (q / 2) * 32, 32, 32);
}

static const RGBA LEVEL_COLORS[4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}};

// 8x8 texture whose levels 0..3 are solid LEVEL_COLORS.
static GLuint make_mip_texture()
{
   GLuint t = make_tex2d(8, 8, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, solid_pixels(64, LEVEL_COLORS[0]).data());
   for (int level = 1; level < 4; level++) {
      int s = 8 >> level;
      glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA8, s, s, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                   solid_pixels(s * s, LEVEL_COLORS[level]).data());
   }
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 3);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
   return t;
}

void scene_tex2d(SceneCtx &ctx)
{
   GLuint p = compile_program(VS_QUAD, FS_TEX2D);
   glActiveTexture(GL_TEXTURE3);
   make_tex2d(4, 4, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, grid_pixels(4).data());
   glUniform1i(uloc(p, "tex"), 3);
   fullscreen_quad();
   for (int j = 0; j < 4; j++)
      for (int i = 0; i < 4; i++)
         probe(ctx, 16 * i + 8, 16 * j + 8, grid_texel(i, j));
}

void scene_tex_mipmap_lod(SceneCtx &ctx)
{
   GLuint p = compile_program(VS_QUAD, R"(#version 400 core
uniform sampler2D tex;
uniform float lod;
in vec2 uv;
layout(location = 0) out vec4 o;
void main() { o = textureLod(tex, uv, lod); }
)");
   make_mip_texture();
   for (int q = 0; q < 4; q++) {
      quadrant_viewport(q);
      glUniform1f(uloc(p, "lod"), q + 0.2f);
      fullscreen_quad();
      probe(ctx, (q % 2) * 32 + 16, (q / 2) * 32 + 16, LEVEL_COLORS[q]);
   }
}

void scene_tex_grad(SceneCtx &ctx)
{
   GLuint p = compile_program(VS_QUAD, R"(#version 400 core
uniform sampler2D tex;
uniform float g;
in vec2 uv;
layout(location = 0) out vec4 o;
void main() { o = textureGrad(tex, uv, vec2(g, 0.0), vec2(0.0, g)); }
)");
   make_mip_texture();
   for (int q = 0; q < 4; q++) {
      quadrant_viewport(q);
      // rho = 8 * g, so lod = q + log2(1.15) = q + 0.2: never on a NEAREST_MIPMAP rounding edge.
      glUniform1f(uloc(p, "g"), std::ldexp(1.15f, q) / 8.0f);
      fullscreen_quad();
      probe(ctx, (q % 2) * 32 + 16, (q / 2) * 32 + 16, LEVEL_COLORS[q]);
   }
}

void scene_tex_base_max_level(SceneCtx &ctx)
{
   GLuint p = compile_program(VS_QUAD, R"(#version 400 core
uniform sampler2D tex;
uniform float lod;
in vec2 uv;
layout(location = 0) out vec4 o;
void main() { o = lod < 0.0 ? texture(tex, uv) : textureLod(tex, uv, lod); }
)");
   make_mip_texture();
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 1);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 2);
   const float lods[4] = {0.2f, 1.2f, 3.2f, -1.0f};
   const int levels[4] = {1, 2, 2, 1};
   for (int q = 0; q < 4; q++) {
      quadrant_viewport(q);
      glUniform1f(uloc(p, "lod"), lods[q]);
      fullscreen_quad();
      probe(ctx, (q % 2) * 32 + 16, (q / 2) * 32 + 16, LEVEL_COLORS[levels[q]]);
   }
}

void scene_tex3d(SceneCtx &ctx)
{
   GLuint p = compile_program(VS_QUAD, R"(#version 400 core
uniform sampler3D tex;
uniform float r;
in vec2 uv;
layout(location = 0) out vec4 o;
void main() { o = texture(tex, vec3(uv, r)); }
)");
   auto texel = [](int x, int y, int z) {
      return RGBA{uint8_t(60 * z + 30), uint8_t(100 * x + 50), uint8_t(100 * y + 50), 255};
   };
   std::vector<uint8_t> px;
   for (int z = 0; z < 4; z++)
      for (int y = 0; y < 2; y++)
         for (int x = 0; x < 2; x++) {
            RGBA c = texel(x, y, z);
            px.insert(px.end(), {c.r, c.g, c.b, c.a});
         }
   GLuint t;
   glGenTextures(1, &t);
   glBindTexture(GL_TEXTURE_3D, t);
   glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
   glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 2, 2, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
   glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAX_LEVEL, 0);
   for (int q = 0; q < 4; q++) {
      quadrant_viewport(q);
      glUniform1f(uloc(p, "r"), (q + 0.5f) / 4.0f);
      fullscreen_quad();
      for (int y = 0; y < 2; y++)
         for (int x = 0; x < 2; x++)
            probe(ctx, (q % 2) * 32 + 16 * x + 8, (q / 2) * 32 + 16 * y + 8, texel(x, y, q));
   }
}

void scene_tex_cube(SceneCtx &ctx)
{
   GLuint p = compile_program(VS_QUAD, R"(#version 400 core
uniform samplerCube tex;
uniform vec3 dir;
layout(location = 0) out vec4 o;
void main() { o = texture(tex, dir); }
)");
   const RGBA faces[6] = {{255, 0, 0, 255},   {0, 255, 255, 255}, {0, 255, 0, 255},
                          {255, 0, 255, 255}, {0, 0, 255, 255},   {255, 255, 0, 255}};
   const float dirs[6][3] = {{1, 0.2f, 0.1f},  {-1, -0.3f, 0.2f}, {0.1f, 1, -0.2f},
                             {0.3f, -1, 0.1f}, {-0.2f, 0.1f, 1},  {0.2f, 0.3f, -1}};
   GLuint t;
   glGenTextures(1, &t);
   glBindTexture(GL_TEXTURE_CUBE_MAP, t);
   glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
   for (int f = 0; f < 6; f++)
      glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &faces[f]);
   glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, 0);
   for (int f = 0; f < 6; f++) {
      int x = (f % 4) * 16, y = (f / 4) * 16;
      glViewport(x, y, 16, 16);
      glUniform3f(uloc(p, "dir"), dirs[f][0], dirs[f][1], dirs[f][2]);
      fullscreen_quad();
      probe(ctx, x + 8, y + 8, faces[f]);
   }
}

void scene_tex2d_array(SceneCtx &ctx)
{
   GLuint p = compile_program(VS_QUAD, R"(#version 400 core
uniform sampler2DArray tex;
uniform float layer;
in vec2 uv;
layout(location = 0) out vec4 o;
void main() { o = texture(tex, vec3(uv, layer)); }
)");
   auto texel = [](int x, int y, int l) {
      return RGBA{uint8_t(l == 0 ? 200 : 30 + 20 * x), uint8_t(l == 1 ? 200 : 30 + 20 * y),
                  uint8_t(l == 2 ? 200 : 30), 255};
   };
   std::vector<uint8_t> px;
   for (int l = 0; l < 3; l++)
      for (int y = 0; y < 2; y++)
         for (int x = 0; x < 2; x++) {
            RGBA c = texel(x, y, l);
            px.insert(px.end(), {c.r, c.g, c.b, c.a});
         }
   GLuint t;
   glGenTextures(1, &t);
   glBindTexture(GL_TEXTURE_2D_ARRAY, t);
   glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
   glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 2, 2, 3, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
   glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);
   const float layers[4] = {0.0f, 1.0f, 2.0f, 1.3f};
   const int picked[4] = {0, 1, 2, 1};
   for (int q = 0; q < 4; q++) {
      quadrant_viewport(q);
      glUniform1f(uloc(p, "layer"), layers[q]);
      fullscreen_quad();
      for (int y = 0; y < 2; y++)
         for (int x = 0; x < 2; x++)
            probe(ctx, (q % 2) * 32 + 16 * x + 8, (q / 2) * 32 + 16 * y + 8, texel(x, y, picked[q]));
   }
}

void scene_tex_shadow(SceneCtx &ctx)
{
   compile_program(VS_QUAD, R"(#version 400 core
uniform sampler2DShadow tex;
in vec2 uv;
layout(location = 0) out vec4 o;
void main() { float r = texture(tex, vec3(uv, 0.5)); o = vec4(r, 0.25 + 0.5 * r, 1.0 - r, 1.0); }
)");
   float depth[16];
   for (int k = 0; k < 16; k++)
      depth[k] = (k + 0.5f) / 16.0f;
   make_tex2d(4, 4, GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
   fullscreen_quad();
   for (int j = 0; j < 4; j++)
      for (int i = 0; i < 4; i++) {
         double r = 0.5 <= depth[j * 4 + i] ? 1.0 : 0.0;
         probe(ctx, 16 * i + 8, 16 * j + 8, unorm(r, 0.25 + 0.5 * r, 1.0 - r));
      }
}

void scene_tex_integer(SceneCtx &ctx)
{
   const char *vs = VS_QUAD;
   compile_program(vs, R"(#version 400 core
uniform usampler2D tex;
in vec2 uv;
layout(location = 0) out vec4 o;
void main() { o = vec4(texture(tex, uv)) / 255.0; }
)");
   const uint8_t u8[8] = {10, 20, 30, 40, 200, 150, 100, 250};
   make_tex2d(2, 1, GL_RGBA8UI, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, u8);
   glViewport(0, 0, 64, 21);
   fullscreen_quad();
   probe(ctx, 16, 10, {10, 20, 30, 40});
   probe(ctx, 48, 10, {200, 150, 100, 250});

   compile_program(vs, R"(#version 400 core
uniform usampler2D tex;
in vec2 uv;
layout(location = 0) out vec4 o;
void main() { uint v = texture(tex, uv).r; o = vec4(uvec4(v, v >> 8, v >> 16, v >> 24) & 255u) / 255.0; }
)");
   const uint32_t u32[2] = {0xDEADBEEFu, 0x01020304u};
   make_tex2d(2, 1, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, u32);
   glViewport(0, 21, 64, 21);
   fullscreen_quad();
   probe(ctx, 16, 31, {0xEF, 0xBE, 0xAD, 0xDE});
   probe(ctx, 48, 31, {0x04, 0x03, 0x02, 0x01});

   compile_program(vs, R"(#version 400 core
uniform isampler2D tex;
in vec2 uv;
layout(location = 0) out vec4 o;
void main() { o = vec4(texture(tex, uv) + 128) / 255.0; }
)");
   const int8_t i8[8] = {-100, -1, 0, 127, 5, -128, 64, -50};
   make_tex2d(2, 1, GL_RGBA8I, GL_RGBA_INTEGER, GL_BYTE, i8);
   glViewport(0, 42, 64, 22);
   fullscreen_quad();
   probe(ctx, 16, 53, {28, 127, 128, 255});
   probe(ctx, 48, 53, {133, 0, 192, 78});
}

void scene_texel_fetch(SceneCtx &ctx)
{
   GLuint p = compile_program(VS_QUAD, R"(#version 400 core
uniform sampler2D tex;
uniform ivec2 org;
uniform ivec2 cell;
uniform int level;
layout(location = 0) out vec4 o;
void main() { o = texelFetch(tex, (ivec2(gl_FragCoord.xy) - org) / cell, level); }
)");
   auto l1 = [](int i, int j) { return RGBA{255, uint8_t(30 + 100 * i), uint8_t(30 + 100 * j), 255}; };
   make_tex2d(4, 4, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, grid_pixels(4).data());
   std::vector<uint8_t> px1;
   for (int j = 0; j < 2; j++)
      for (int i = 0; i < 2; i++) {
         RGBA c = l1(i, j);
         px1.insert(px1.end(), {c.r, c.g, c.b, c.a});
      }
   glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, px1.data());
   const uint8_t white[4] = {255, 255, 255, 255};
   glTexImage2D(GL_TEXTURE_2D, 2, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 2);

   glViewport(0, 0, 32, 64);
   glUniform2i(uloc(p, "org"), 0, 0);
   glUniform2i(uloc(p, "cell"), 8, 16);
   glUniform1i(uloc(p, "level"), 0);
   fullscreen_quad();
   glViewport(32, 0, 32, 64);
   glUniform2i(uloc(p, "org"), 32, 0);
   glUniform2i(uloc(p, "cell"), 16, 32);
   glUniform1i(uloc(p, "level"), 1);
   fullscreen_quad();
   for (int j = 0; j < 4; j++)
      for (int i = 0; i < 4; i++)
         probe(ctx, 8 * i + 4, 16 * j + 8, grid_texel(i, j));
   for (int j = 0; j < 2; j++)
      for (int i = 0; i < 2; i++)
         probe(ctx, 32 + 16 * i + 8, 32 * j + 16, l1(i, j));
}

void scene_tex_proj(SceneCtx &ctx)
{
   GLuint p = compile_program(VS_QUAD, R"(#version 400 core
uniform sampler2D tex;
uniform float q;
uniform bool use_vec4;
in vec2 uv;
layout(location = 0) out vec4 o;
void main() {
   vec4 t = vec4(uv * q, 7.0, q);
   o = use_vec4 ? textureProj(tex, t) : textureProj(tex, t.xyw);
}
)");
   make_tex2d(4, 4, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, grid_pixels(4).data());
   glUniform1f(uloc(p, "q"), 2.5f);
   for (int half = 0; half < 2; half++) {
      glViewport(32 * half, 0, 32, 64);
      glUniform1i(uloc(p, "use_vec4"), half);
      fullscreen_quad();
      for (int j = 0; j < 4; j++)
         for (int i = 0; i < 4; i++)
            probe(ctx, 32 * half + 8 * i + 4, 16 * j + 8, grid_texel(i, j));
   }
}

void scene_tex_bgra_packed(SceneCtx &ctx)
{
   // RGB565 is a GL 4.1 / ARB_ES2_compatibility internal format; xemu requires the extension.
   GLint minor = 0;
   glGetIntegerv(GL_MINOR_VERSION, &minor);
   if (minor < 1)
      require_extension("GL_ARB_ES2_compatibility");
   compile_program(VS_QUAD, FS_TEX2D);
   const uint8_t bgra8[4] = {30, 80, 200, 255};
   const uint32_t rev8888 = (200u << 24) | (20u << 16) | (160u << 8) | 240u;
   const uint32_t rgba8888 = (60u << 24) | (120u << 16) | (180u << 8) | 240u;
   const uint16_t rgb565 = (31u << 11) | (21u << 5) | 10u;
   const uint16_t argb1555 = (1u << 15) | (8u << 10) | (31u << 5) | 0u;
   const uint16_t argb4444 = (12u << 12) | (3u << 8) | (9u << 4) | 15u;
   struct Upload {
      GLenum internal_format, format, type;
      const void *data;
      RGBA want;
   } uploads[6] = {
      {GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, bgra8, {200, 80, 30, 255}},
      {GL_RGBA8, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, &rev8888, {20, 160, 240, 200}},
      {GL_RGBA8, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8, &rgba8888, {60, 120, 180, 240}},
      {GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, &rgb565, {255, 85, unorm8(10 / 31.0), 255}},
      {GL_RGB5_A1, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, &argb1555, {unorm8(8 / 31.0), 255, 0, 255}},
      {GL_RGBA4, GL_BGRA, GL_UNSIGNED_SHORT_4_4_4_4_REV, &argb4444, {51, 153, 255, 204}},
   };
   for (int k = 0; k < 6; k++) {
      const Upload &u = uploads[k];
      make_tex2d(1, 1, u.internal_format, u.format, u.type, u.data);
      int x = (k % 2) * 32, y = (k / 2) * 21;
      glViewport(x, y, 32, k / 2 == 2 ? 22 : 21);
      fullscreen_quad();
      probe(ctx, x + 16, y + 10, u.want);
   }
}

// DXT color endpoints are RGB565; c0/c1 below are chosen so every palette entry is exact
// or a 1/3 or 2/3 mix of 0 and 255, which every decoder rounds to within 1.
static RGBA rgb565(uint16_t c)
{
   return {uint8_t(((c >> 11) & 31) * 255 / 31), uint8_t(((c >> 5) & 63) * 255 / 63), uint8_t((c & 31) * 255 / 31), 255};
}

static RGBA mix_rgb(RGBA a, RGBA b, int wa, int wb, int div)
{
   return {uint8_t((a.r * wa + b.r * wb + div / 2) / div), uint8_t((a.g * wa + b.g * wb + div / 2) / div),
           uint8_t((a.b * wa + b.b * wb + div / 2) / div), 255};
}

static void put16(std::vector<uint8_t> &v, uint16_t x)
{
   v.push_back(uint8_t(x));
   v.push_back(uint8_t(x >> 8));
}

static void put32(std::vector<uint8_t> &v, uint32_t x)
{
   put16(v, uint16_t(x));
   put16(v, uint16_t(x >> 16));
}

void scene_tex_dxt(SceneCtx &ctx)
{
   require_extension("GL_EXT_texture_compression_s3tc");
   compile_program(VS_QUAD, FS_TEX2D);
   glClearColor(0.5f, 0.5f, 0.5f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);

   auto upload = [](GLenum format, const std::vector<uint8_t> &block) {
      GLuint t;
      glGenTextures(1, &t);
      glBindTexture(GL_TEXTURE_2D, t);
      glCompressedTexImage2D(GL_TEXTURE_2D, 0, format, 4, 4, 0, (GLsizei)block.size(), block.data());
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
   };
   auto draw_quadrant = [&](int q, RGBA (*texel)(int x, int y)) {
      quadrant_viewport(q);
      fullscreen_quad();
      for (int y = 0; y < 4; y++)
         for (int x = 0; x < 4; x++)
            probe(ctx, (q % 2) * 32 + 8 * x + 4, (q / 2) * 32 + 8 * y + 4, texel(x, y));
   };

   // Every row of indices is 0,1,2,3 (byte 0xE4); texel (x, y) uses palette entry x.
   std::vector<uint8_t> dxt1;
   put16(dxt1, 0xF800);
   put16(dxt1, 0x001F);
   put32(dxt1, 0xE4E4E4E4u);
   upload(GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, dxt1);
   draw_quadrant(0, [](int x, int) {
      RGBA c0 = rgb565(0xF800), c1 = rgb565(0x001F);
      RGBA pal[4] = {c0, c1, mix_rgb(c0, c1, 2, 1, 3), mix_rgb(c0, c1, 1, 2, 3)};
      return pal[x];
   });

   // c0 <= c1 selects 3-color mode: entry 2 is the midpoint, entry 3 is transparent black.
   std::vector<uint8_t> dxt1a;
   put16(dxt1a, 0x001F);
   put16(dxt1a, 0x07E0);
   put32(dxt1a, 0xE4E4E4E4u);
   upload(GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, dxt1a);
   draw_quadrant(1, [](int x, int) {
      RGBA c0 = rgb565(0x001F), c1 = rgb565(0x07E0);
      RGBA pal[4] = {c0, c1, mix_rgb(c0, c1, 1, 1, 2), {0, 0, 0, 0}};
      return pal[x];
   });

   // DXT3: explicit 4-bit alpha 15 - k for texel k; color index = row.
   std::vector<uint8_t> dxt3;
   uint64_t alpha = 0;
   for (int k = 0; k < 16; k++)
      alpha |= uint64_t(15 - k) << (4 * k);
   put32(dxt3, uint32_t(alpha));
   put32(dxt3, uint32_t(alpha >> 32));
   put16(dxt3, 0x07E0);
   put16(dxt3, 0xFFFF);
   put32(dxt3, 0xFFAA5500u);
   upload(GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, dxt3);
   draw_quadrant(2, [](int x, int y) {
      RGBA c0 = rgb565(0x07E0), c1 = rgb565(0xFFFF);
      RGBA pal[4] = {c0, c1, mix_rgb(c0, c1, 2, 1, 3), mix_rgb(c0, c1, 1, 2, 3)};
      RGBA c = pal[y];
      c.a = uint8_t((15 - (y * 4 + x)) * 17);
      return c;
   });

   // DXT5: alpha endpoints 255 and 0 (8-entry mode), 3-bit alpha index k % 8 for texel k.
   std::vector<uint8_t> dxt5 = {255, 0};
   uint64_t aidx = 0;
   for (int k = 0; k < 16; k++)
      aidx |= uint64_t(k % 8) << (3 * k);
   for (int b = 0; b < 6; b++)
      dxt5.push_back(uint8_t(aidx >> (8 * b)));
   put16(dxt5, 0xFFE0);
   put16(dxt5, 0x0000);
   put32(dxt5, 0xE4E4E4E4u);
   upload(GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, dxt5);
   draw_quadrant(3, [](int x, int y) {
      RGBA c0 = rgb565(0xFFE0), c1 = rgb565(0x0000);
      RGBA pal[4] = {c0, c1, mix_rgb(c0, c1, 2, 1, 3), mix_rgb(c0, c1, 1, 2, 3)};
      const uint8_t alphas[8] = {255, 0, 219, 182, 146, 109, 73, 36};
      RGBA c = pal[x];
      c.a = alphas[(y * 4 + x) % 8];
      return c;
   });
}

void scene_tex_border_clamp(SceneCtx &ctx)
{
   compile_program(VS_QUAD, R"(#version 400 core
uniform sampler2D tex;
in vec2 uv;
layout(location = 0) out vec4 o;
void main() { o = texture(tex, uv * 2.0 - 0.5); }
)");
   const uint8_t px[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255};
   make_tex2d(2, 2, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, px);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
   const float border[4] = {0.2f, 0.4f, 0.8f, 1.0f};
   glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
   fullscreen_quad();
   RGBA b = unorm(0.2, 0.4, 0.8);
   probe(ctx, 8, 8, b);
   probe(ctx, 56, 56, b);
   probe(ctx, 32, 8, b);
   probe(ctx, 8, 32, b);
   probe(ctx, 24, 24, {255, 0, 0, 255});
   probe(ctx, 40, 24, {0, 255, 0, 255});
   probe(ctx, 24, 40, {0, 0, 255, 255});
   probe(ctx, 40, 40, {255, 255, 0, 255});
}

void scene_tex_swizzle(SceneCtx &ctx)
{
   compile_program(VS_QUAD, FS_TEX2D);
   const uint8_t r8 = 153, rg8[2] = {51, 204}, rgba8[4] = {10, 20, 30, 40};

   make_tex2d(1, 1, GL_R8, GL_RED, GL_UNSIGNED_BYTE, &r8);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ONE);
   quadrant_viewport(0);
   fullscreen_quad();
   probe(ctx, 16, 16, {153, 153, 153, 255});

   make_tex2d(1, 1, GL_RG8, GL_RG, GL_UNSIGNED_BYTE, rg8);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_GREEN);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_ZERO);
   quadrant_viewport(1);
   fullscreen_quad();
   probe(ctx, 48, 16, {204, 51, 0, 255});

   const GLint bgra[4] = {GL_BLUE, GL_GREEN, GL_RED, GL_ALPHA};
   make_tex2d(1, 1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, rgba8);
   glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, bgra);
   quadrant_viewport(2);
   fullscreen_quad();
   probe(ctx, 16, 48, {30, 20, 10, 40});

   const GLint consts[4] = {GL_ONE, GL_ALPHA, GL_ZERO, GL_RED};
   make_tex2d(1, 1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, rgba8);
   glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, consts);
   quadrant_viewport(3);
   fullscreen_quad();
   probe(ctx, 48, 48, {255, 40, 0, 10});
}

void scene_sampler_objects(SceneCtx &ctx)
{
   compile_program(VS_QUAD, R"(#version 400 core
uniform sampler2D tex;
in vec2 uv;
layout(location = 0) out vec4 o;
void main() { o = texture(tex, vec2(uv.x * 3.0 - 1.0, 0.5)); }
)");
   const uint8_t px[8] = {255, 0, 0, 255, 0, 255, 0, 255};
   make_tex2d(2, 1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, px);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
   GLuint s;
   glGenSamplers(1, &s);
   glSamplerParameteri(s, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glSamplerParameteri(s, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

   // Bottom: sampler bound (clamp). Top: sampler unbound again, texture state (repeat) applies.
   glBindSampler(0, s);
   glViewport(0, 0, 64, 32);
   fullscreen_quad();
   glBindSampler(0, 0);
   glViewport(0, 32, 64, 32);
   fullscreen_quad();
   const RGBA red = {255, 0, 0, 255}, green = {0, 255, 0, 255};
   probe(ctx, 8, 16, red);
   probe(ctx, 16, 16, red);
   probe(ctx, 48, 16, green);
   probe(ctx, 8, 48, red);
   probe(ctx, 16, 48, green);
   probe(ctx, 48, 48, red);
   probe(ctx, 40, 48, green);
}

void scene_pixel_store_unpack(SceneCtx &ctx)
{
   compile_program(VS_QUAD, FS_TEX2D);
   // 7x6 RGB source; the 4x4 texture comes from columns 2..5 and rows 1..4.
   auto src = [](int x, int y) { return RGBA{uint8_t(20 + 30 * x), uint8_t(20 + 40 * y), uint8_t(200 - 20 * x - 10 * y), 255}; };
   std::vector<uint8_t> px;
   for (int y = 0; y < 6; y++)
      for (int x = 0; x < 7; x++) {
         RGBA c = src(x, y);
         px.insert(px.end(), {c.r, c.g, c.b});
      }
   GLuint t;
   glGenTextures(1, &t);
   glBindTexture(GL_TEXTURE_2D, t);
   glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
   glPixelStorei(GL_UNPACK_ROW_LENGTH, 7);
   glPixelStorei(GL_UNPACK_SKIP_PIXELS, 2);
   glPixelStorei(GL_UNPACK_SKIP_ROWS, 1);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGB, GL_UNSIGNED_BYTE, px.data());
   glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
   glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
   glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
   fullscreen_quad();
   for (int j = 0; j < 4; j++)
      for (int i = 0; i < 4; i++)
         probe(ctx, 16 * i + 8, 16 * j + 8, src(i + 2, j + 1));
}

void scene_fbo_render_to_texture(SceneCtx &ctx)
{
   GLuint tex;
   make_fbo(32, 32, &tex);
   const float quad_colors[4][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}};
   glEnable(GL_SCISSOR_TEST);
   for (int q = 0; q < 4; q++) {
      glScissor((q % 2) * 16, (q / 2) * 16, 16, 16);
      glClearColor(quad_colors[q][0], quad_colors[q][1], quad_colors[q][2], 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
   }
   glDisable(GL_SCISSOR_TEST);
   GLuint solid = solid_program();
   solid_color(solid, 1, 1, 1);
   draw_rect(-0.25f, -0.25f, 0.25f, 0.25f);

   glBindFramebuffer(GL_FRAMEBUFFER, ctx.fbo);
   glViewport(0, 0, ctx.w, ctx.h);
   compile_program(VS_QUAD, FS_TEX2D);
   glBindTexture(GL_TEXTURE_2D, tex);
   fullscreen_quad();
   for (int q = 0; q < 4; q++)
      probe(ctx, (q % 2) * 40 + 12, (q / 2) * 40 + 12,
            unorm(quad_colors[q][0], quad_colors[q][1], quad_colors[q][2]));
   probe(ctx, 32, 32, {255, 255, 255, 255});
}

void scene_blit_framebuffer(SceneCtx &ctx)
{
   GLuint tex;
   GLuint src = make_fbo(32, 32, &tex);
   const float quad_colors[4][3] = {{0.8f, 0.2f, 0.2f}, {0.2f, 0.8f, 0.2f}, {0.2f, 0.2f, 0.8f}, {0.8f, 0.8f, 0.2f}};
   glEnable(GL_SCISSOR_TEST);
   for (int q = 0; q < 4; q++) {
      glScissor((q % 2) * 16, (q / 2) * 16, 16, 16);
      glClearColor(quad_colors[q][0], quad_colors[q][1], quad_colors[q][2], 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
   }
   glScissor(0, 0, 4, 32);
   glClearColor(1, 1, 1, 1);
   glClear(GL_COLOR_BUFFER_BIT);
   glDisable(GL_SCISSOR_TEST);

   glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
   glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ctx.fbo);
   glBlitFramebuffer(0, 0, 32, 32, 0, 0, 64, 64, GL_COLOR_BUFFER_BIT, GL_NEAREST);
   // Mirrored horizontally into the top-right quadrant: the white stripe lands on its right edge.
   glBlitFramebuffer(0, 0, 16, 16, 64, 32, 32, 64, GL_COLOR_BUFFER_BIT, GL_NEAREST);
   glBindFramebuffer(GL_FRAMEBUFFER, ctx.fbo);

   const RGBA white = {255, 255, 255, 255};
   auto q = [&](int i) { return unorm(quad_colors[i][0], quad_colors[i][1], quad_colors[i][2]); };
   probe(ctx, 4, 16, white);
   probe(ctx, 4, 48, white);
   probe(ctx, 20, 16, q(0));
   probe(ctx, 48, 16, q(1));
   probe(ctx, 20, 48, q(2));
   probe(ctx, 40, 48, q(0));
   probe(ctx, 60, 48, white);
}
