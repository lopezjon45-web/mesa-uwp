#include "glprobe.h"

#include <cmath>
#include <cstring>

// ALU scenes compile one program per case and draw it into its own 8x8 cell, so a single
// case never depends on branching or dynamic indexing. Inputs come from uniforms so the
// compiler cannot constant-fold the operation under test.

using Setter = void (*)(GLuint);

struct UintCase {
   const char *expr;
   uint32_t want;
   bool probed = true;
   const char *pre = "";
};

struct FloatCase {
   const char *expr;
   double want;
   const char *pre = "";
};

static void run_uint_cases(SceneCtx &ctx, const char *decls, std::initializer_list<UintCase> cases,
                           Setter set, int first_cell = 0)
{
   int cell = first_cell;
   for (const UintCase &c : cases) {
      std::string fs = std::string("#version 400 core\n") + decls +
                       "\nlayout(location = 0) out vec4 o;\nvoid main() {\n" + c.pre +
                       "\n   uint v = uint(" + c.expr + ");\n"
                       "   o = vec4(uvec4(v, v >> 8, v >> 16, v >> 24) & 255u) / 255.0;\n}\n";
      GLuint p = compile_program(VS_QUAD, fs.c_str());
      set(p);
      cell_viewport(cell);
      fullscreen_quad();
      if (c.probed)
         probe_cell(ctx, cell, {uint8_t(c.want), uint8_t(c.want >> 8), uint8_t(c.want >> 16),
                                uint8_t(c.want >> 24)});
      cell++;
   }
   glViewport(0, 0, ctx.w, ctx.h);
}

static void run_float_cases(SceneCtx &ctx, const char *decls, std::initializer_list<FloatCase> cases,
                            Setter set, int first_cell = 0)
{
   int cell = first_cell;
   for (const FloatCase &c : cases) {
      std::string fs = std::string("#version 400 core\n") + decls +
                       "\nuniform float want_;\nuniform float tint_;\n"
                       "layout(location = 0) out vec4 o;\nvoid main() {\n" + c.pre +
                       "\n   float v = float(" + c.expr + ");\n"
                       "   bool ok = abs(v - want_) <= 1e-3 * max(1.0, abs(want_));\n"
                       "   o = ok ? vec4(0.0, 1.0, tint_, 1.0) : vec4(1.0, 0.0, tint_, 1.0);\n}\n";
      GLuint p = compile_program(VS_QUAD, fs.c_str());
      set(p);
      glUniform1f(uloc(p, "want_"), (float)c.want);
      glUniform1f(uloc(p, "tint_"), cell * 4 / 255.0f);
      cell_viewport(cell);
      fullscreen_quad();
      probe_cell(ctx, cell, {0, 255, uint8_t(cell * 4), 255});
      cell++;
   }
   glViewport(0, 0, ctx.w, ctx.h);
}

static uint32_t u32(int32_t v)
{
   return (uint32_t)v;
}

static uint32_t bits(float f)
{
   uint32_t u;
   memcpy(&u, &f, 4);
   return u;
}

void scene_alu_float(SceneCtx &ctx)
{
   const double x = 1.5f, y = 0.25f, z = 2.0f, w = 0.7f;
   const double t = (w - y) / (z - y);
   run_float_cases(ctx, "uniform vec4 a;",
                   {
                      {"a.x * a.y", x * y},
                      {"a.x + a.y", x + y},
                      {"a.x - a.w", x - w},
                      {"a.x * a.y + a.z", x * y + z},
                      {"a.z / a.x", z / x},
                      {"sqrt(a.z)", std::sqrt(z)},
                      {"inversesqrt(a.z)", 1.0 / std::sqrt(z)},
                      {"exp2(a.x)", std::exp2(x)},
                      {"log2(a.z + a.w)", std::log2(z + w)},
                      {"exp(a.w)", std::exp(w)},
                      {"log(a.z)", std::log(z)},
                      {"sin(a.w)", std::sin(w)},
                      {"cos(a.w)", std::cos(w)},
                      {"atan(a.y, a.w)", std::atan2(y, w)},
                      {"pow(a.z, a.x)", std::pow(z, x)},
                      {"min(a.x, a.w)", w},
                      {"max(a.y, a.w)", w},
                      {"clamp(a.x, a.y, a.w)", w},
                      {"mix(a.y, a.z, a.w)", y + (z - y) * w},
                      {"step(a.w, a.x)", 1.0},
                      {"step(a.x, a.w)", 0.0},
                      {"smoothstep(a.y, a.z, a.w)", t * t * (3 - 2 * t)},
                      {"fract(a.x * 3.1)", x * 3.1 - std::floor(x * 3.1)},
                      {"floor(-a.x)", -2.0},
                      {"ceil(a.x)", 2.0},
                      {"trunc(-a.x)", -1.0},
                      {"roundEven(a.x + 1.0)", 2.0},
                      {"abs(a.y - a.w)", w - y},
                      {"sign(a.y - a.w)", -1.0},
                      {"mod(-a.x, a.w)", -x - w * std::floor(-x / w)},
                      {"dot(a.xy, a.zw)", x * z + y * w},
                      {"length(a.xy)", std::sqrt(x * x + y * y)},
                   },
                   [](GLuint p) { glUniform4f(uloc(p, "a"), 1.5f, 0.25f, 2.0f, 0.7f); });
}

void scene_alu_int(SceneCtx &ctx)
{
   run_uint_cases(ctx, "uniform ivec4 i;\nuniform uvec4 u;\nuniform vec2 f;",
                  {
                     {"i.x + i.y", 6},
                     {"i.x * i.y", u32(-91)},
                     {"i.x - i.y * i.z", 34},
                     {"i.y * 3 + i.z", u32(-18)},
                     {"-i.x", u32(-13)},
                     {"i.x << i.z", 104},
                     {"i.y >> 1", u32(-4)},
                     {"u.x >> 4u", 0x0F0F0A5Au},
                     {"u.x << 4u", 0x0F0A5A50u},
                     {"u.x & 0xFFFFu", 0xA5A5u},
                     {"u.y | (u.z << 8u)", 0x705u},
                     {"u.x ^ 0xFFFF0000u", 0x0F0FA5A5u},
                     {"~u.x", 0x0F0F5A5Au},
                     {"~i.x", u32(-14)},
                     {"min(i.x, i.y)", u32(-7)},
                     {"max(i.x, i.y)", 13},
                     {"min(u.x, u.w)", 0x80000000u},
                     {"max(u.y, u.z)", 7},
                     {"abs(i.y)", 7},
                     {"i.y < i.z", 1},
                     {"i.y >= i.z", 0},
                     {"i.x == 13", 1},
                     {"u.w > u.y", 1},
                     {"int(u.w) > i.y", 0},
                     {"uint(i.y)", 0xFFFFFFF9u},
                     {"int(f.x)", u32(-2)},
                     {"uint(f.y)", 3},
                     {"int(float(i.y) * 0.5)", u32(-3)},
                     {"floatBitsToUint(float(i.y))", 0xC0E00000u},
                     {"floatBitsToUint(float(u.w))", 0x4F000000u},
                  },
                  [](GLuint p) {
                     glUniform4i(glGetUniformLocation(p, "i"), 13, -7, 3, 0x0F0F);
                     glUniform4ui(glGetUniformLocation(p, "u"), 0xF0F0A5A5u, 5u, 7u, 0x80000000u);
                     glUniform2f(glGetUniformLocation(p, "f"), -2.7f, 3.9f);
                  });
}

void scene_int_div(SceneCtx &ctx)
{
   run_uint_cases(ctx, "uniform ivec4 i;\nuniform uvec4 u;",
                  {
                     {"i.x / i.z", 3},
                     {"i.y / i.z", u32(-3)},
                     {"i.x / i.w", u32(-3)},
                     {"i.y / i.w", 3},
                     {"i.x % i.z", 2},
                     {"i.y % i.z", u32(-2)},
                     {"i.x % i.w", 2},
                     {"i.y % i.w", u32(-2)},
                     {"i.y / i.y", 1},
                     {"int(u.z) / i.z", u32(-3)},
                     {"u.x / u.y", 3},
                     {"u.x % u.y", 2},
                     {"u.z / u.w", 613566754u},
                     {"u.z % u.w", 2},
                     {"u.z / u.y", 858993456u},
                     {"u.y / u.x", 0},
                  },
                  [](GLuint p) {
                     glUniform4i(glGetUniformLocation(p, "i"), 17, -17, 5, -5);
                     glUniform4ui(glGetUniformLocation(p, "u"), 17u, 5u, 0xFFFFFFF0u, 7u);
                  });
}

void scene_bitfield_ops(SceneCtx &ctx)
{
   run_uint_cases(ctx, "uniform ivec4 i;\nuniform uvec4 u;",
                  {
                     {"bitfieldExtract(u.x, 4, 8)", 0x5Au},
                     {"bitfieldExtract(i.y, 8, 12)", 0x456u},
                     {"bitfieldExtract(i.x, 1, 3)", u32(-4)},
                     {"bitfieldExtract(u.x, 0, 32)", 0xF0F0A5A5u},
                     {"bitfieldInsert(u.x, uint(i.y), 8, 8)", 0xF0F078A5u},
                     {"bitfieldInsert(i.z, i.x, 28, 4)", 0x80000000u},
                     {"bitCount(u.x)", 16},
                     {"bitCount(i.x)", 29},
                     {"findLSB(u.y)", 31},
                     {"findLSB(u.z)", 0xFFFFFFFFu},
                     {"findLSB(i.x)", 3},
                     {"findMSB(u.w)", 16},
                     {"findMSB(i.x)", 2},
                     {"findMSB(i.y)", 28},
                     {"findMSB(i.z)", 0xFFFFFFFFu},
                     {"bitfieldReverse(u.w)", 0x00008000u},
                     {"bitfieldReverse(u.x)", 0xA5A50F0Fu},
                  },
                  [](GLuint p) {
                     glUniform4i(glGetUniformLocation(p, "i"), -8, 0x12345678, 0, 1);
                     glUniform4ui(glGetUniformLocation(p, "u"), 0xF0F0A5A5u, 0x80000000u, 0u, 0x00010000u);
                  });
}

void scene_float_bits(SceneCtx &ctx)
{
   run_uint_cases(ctx, "uniform vec4 f;\nuniform uvec4 u;",
                  {
                     {"floatBitsToUint(f.x)", 0x3F800000u},
                     {"floatBitsToUint(f.y)", 0xC0200000u},
                     {"floatBitsToInt(f.z)", 0x3E200000u},
                     {"floatBitsToUint(uintBitsToFloat(u.x) * 2.0)", 0x40C90FDBu},
                     {"floatBitsToUint(intBitsToFloat(int(u.y)) + 1.0)", 0xBF800000u},
                     {"floatBitsToUint(uintBitsToFloat(u.z))", 0x7F7FFFFFu},
                     {"floatBitsToUint(clamp(f.w, uintBitsToFloat(0x1F800000u), uintBitsToFloat(0x5F800000u)))",
                      0x5F800000u},
                     {"floatBitsToUint(clamp(-f.w, uintBitsToFloat(0xDF800000u), uintBitsToFloat(0x9F800000u)))",
                      0xDF800000u},
                     {"floatBitsToUint(f.x) == 0x3F800000u", 1},
                  },
                  [](GLuint p) {
                     glUniform4f(glGetUniformLocation(p, "f"), 1.0f, -2.5f, 0.15625f, 1e30f);
                     glUniform4ui(glGetUniformLocation(p, "u"), 0x40490FDBu, 0xC0000000u, 0x7F7FFFFFu, 0u);
                  });
}

void scene_precise_fma(SceneCtx &ctx)
{
   // det = a.x*b.y - a.y*b.x with a.x = b.y = 1 + 2^-12: exact value 2^-11 + 2^-24 needs a fused
   // fma; an unfused mad gives 2^-11. Both are within the probe tolerance, and the unprobed
   // bit-pattern cells record which one the driver produced.
   const char *kahan =
      "   precise float w = a.y * b.x;\n"
      "   precise float e = fma(-a.y, b.x, w);\n"
      "   precise float f = fma(a.x, b.y, -w);\n"
      "   precise float r = f + e;";
   const char *decls = "uniform vec2 a;\nuniform vec2 b;\nuniform vec3 k;";
   Setter set = [](GLuint p) {
      const float e = 1.0f + 1.0f / 4096.0f;
      glUniform2f(glGetUniformLocation(p, "a"), e, 1.0f);
      glUniform2f(glGetUniformLocation(p, "b"), 1.0f, e);
      glUniform3f(glGetUniformLocation(p, "k"), 3.0f, 5.0f, 7.0f);
   };
   run_float_cases(ctx, decls, {{"r", 1.0 / 2048 + 1.0 / 16777216, kahan}}, set, 0);
   run_uint_cases(ctx, decls,
                  {
                     {"floatBitsToUint(r)", 0, false, kahan},
                     {"floatBitsToUint(fma(k.x, k.y, k.z))", bits(22.0f)},
                     {"floatBitsToUint(p)", 0, false, "   precise float p = a.x * b.y - a.y * b.x;"},
                  },
                  set, 1);
}

static int loops_reference(int id, int n, int *k_out, int *w_out)
{
   int acc = 0;
   for (int i = 0; i < n + id; i++) {
      if ((i & 1) == 1)
         continue;
      acc += i * 3;
      if (acc > 60 + id * 5)
         break;
   }
   int k = 0;
   switch (id % 5) {
   case 0:
      k = 11;
      break;
   case 1:
      k = 22;
      [[fallthrough]];
   case 2:
      k += 33;
      break;
   case 3:
   case 4:
      k = 44 + id;
      break;
   default:
      k = 99;
      break;
   }
   if (acc > 40) {
      if (id > 8)
         acc += 300;
      else
         acc -= 7;
   } else {
      acc = -acc;
   }
   int w = 0;
   do {
      w++;
   } while (w * w < id * 10);
   *k_out = k;
   *w_out = w;
   return acc;
}

void scene_loops_branches(SceneCtx &ctx)
{
   GLuint p = compile_program(VS_QUAD, R"(#version 400 core
uniform int n;
layout(location = 0) out vec4 o;
void main() {
   int id = (int(gl_FragCoord.y) / 16) * 4 + int(gl_FragCoord.x) / 16;
   int acc = 0;
   for (int i = 0; i < n + id; i++) {
      if ((i & 1) == 1) continue;
      acc += i * 3;
      if (acc > 60 + id * 5) break;
   }
   int k = 0;
   switch (id % 5) {
   case 0: k = 11; break;
   case 1: k = 22;
   case 2: k += 33; break;
   case 3:
   case 4: k = 44 + id; break;
   default: k = 99; break;
   }
   if (acc > 40) {
      if (id > 8) acc += 300; else acc -= 7;
   } else {
      acc = -acc;
   }
   int w = 0;
   do { w++; } while (w * w < id * 10);
   uint ua = uint(acc);
   o = vec4(float(ua & 255u), float((ua >> 8) & 255u), float(k), float(w * 16)) / 255.0;
}
)");
   glUniform1i(uloc(p, "n"), 9);
   fullscreen_quad();
   for (int id = 0; id < 16; id++) {
      int k, w;
      uint32_t acc = (uint32_t)loops_reference(id, 9, &k, &w);
      probe(ctx, (id % 4) * 16 + 8, (id / 4) * 16 + 8,
            {uint8_t(acc), uint8_t(acc >> 8), uint8_t(k), uint8_t(w * 16)});
   }
}

void scene_derivatives(SceneCtx &ctx)
{
   compile_program(R"(#version 400 core
layout(location = 0) in vec2 pos;
out float a;
out float b;
void main() { a = pos.x * 8.0; b = pos.y * 12.0; gl_Position = vec4(pos, 0.0, 1.0); }
)",
                   R"(#version 400 core
in float a;
in float b;
layout(location = 0) out vec4 o;
void main() {
   float off = abs(dFdx(b)) + abs(dFdy(a));
   o = vec4(dFdx(a), dFdy(b), fwidth(a + b), 1.0 - off);
}
)");
   fullscreen_quad();
   RGBA want = unorm(0.25, 0.375, 0.625, 1.0);
   probe(ctx, 16, 16, want);
   probe(ctx, 45, 50, want);
   probe(ctx, 30, 33, want);
}
