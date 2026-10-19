#include "glprobe.h"

#include <cmath>
#include <cstring>

static const RGBA BLACK = {0, 0, 0, 255};

static void probe_ndc(SceneCtx &ctx, float x, float y, RGBA want)
{
   probe(ctx, (int)((x + 1.0f) * 32.0f), (int)((y + 1.0f) * 32.0f), want);
}

void scene_clear(SceneCtx &ctx)
{
   glClearColor(0.2f, 0.4f, 0.6f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   probe(ctx, 32, 32, {51, 102, 153, 255});
   probe(ctx, 3, 60, {51, 102, 153, 255});
}

void scene_triangle_basic(SceneCtx &ctx)
{
   GLuint p = solid_program();
   solid_color(p, 1.0f, 0.6f, 0.2f);
   draw_arrays(GL_TRIANGLES, {-0.9f, -0.9f, 0.9f, -0.83f, 0.03f, 0.91f}, {2});
   probe(ctx, 32, 24, {255, 153, 51, 255});
   probe(ctx, 3, 60, BLACK);
   probe(ctx, 60, 60, BLACK);
}

// One triangle with distinct w per vertex, so perspective-correct and screen-linear
// interpolation give different colors at the same pixel.
static const float INTERP_S[3][2] = {{-0.9f, -0.85f}, {0.95f, -0.9f}, {-0.05f, 0.93f}};
static const float INTERP_W[3] = {1.0f, 2.0f, 4.0f};

static RGBA expected_interp(int px, int py, bool perspective)
{
   double x = ndc(px + 0.5f), y = ndc(py + 0.5f);
   double x0 = INTERP_S[0][0], y0 = INTERP_S[0][1], x1 = INTERP_S[1][0], y1 = INTERP_S[1][1];
   double x2 = INTERP_S[2][0], y2 = INTERP_S[2][1];
   double area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
   double l[3] = {((x1 - x) * (y2 - y) - (x2 - x) * (y1 - y)) / area,
                  ((x2 - x) * (y0 - y) - (x0 - x) * (y2 - y)) / area, 0};
   l[2] = 1 - l[0] - l[1];
   if (perspective) {
      double s = 0;
      for (int i = 0; i < 3; i++)
         s += l[i] /= INTERP_W[i];
      for (double &v : l)
         v /= s;
   }
   return unorm(l[0], l[1], l[2]);
}

static void interp_scene(SceneCtx &ctx, const char *qualifier)
{
   std::string vs = std::string("#version 400 core\nlayout(location = 0) in vec4 pos;\n"
                                "layout(location = 1) in vec4 col;\n") +
                    qualifier + " out vec4 c;\nvoid main() { c = col; gl_Position = pos; }\n";
   std::string fs = std::string("#version 400 core\n") + qualifier +
                    " in vec4 c;\nlayout(location = 0) out vec4 o;\nvoid main() { o = c; }\n";
   compile_program(vs.c_str(), fs.c_str());
   std::vector<float> v;
   for (int i = 0; i < 3; i++) {
      float w = INTERP_W[i];
      v.insert(v.end(), {INTERP_S[i][0] * w, INTERP_S[i][1] * w, 0.0f, w});
      v.insert(v.end(), {i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f, 1.0f});
   }
   draw_arrays(GL_TRIANGLES, v, {4, 4});
   bool perspective = strcmp(qualifier, "smooth") == 0;
   const int pts[4][2] = {{20, 20}, {40, 16}, {30, 40}, {32, 28}};
   for (auto &pt : pts)
      probe(ctx, pt[0], pt[1], expected_interp(pt[0], pt[1], perspective));
}

void scene_varying_smooth(SceneCtx &ctx)
{
   interp_scene(ctx, "smooth");
}

void scene_varying_noperspective(SceneCtx &ctx)
{
   interp_scene(ctx, "noperspective");
}

static const char *const VS_FLAT = R"(#version 400 core
layout(location = 0) in vec4 pos;
layout(location = 1) in vec4 col;
flat out vec4 c;
flat out int vid;
void main() { c = col; vid = gl_VertexID; gl_Position = pos; }
)";

static const char *const FS_FLAT = R"(#version 400 core
flat in vec4 c;
flat in int vid;
layout(location = 0) out vec4 o;
void main() { o = vec4(c.rgb, float(vid * 40 + 15) / 255.0); }
)";

static const float FLAT_COLORS[3][4] = {{0.8f, 0.2f, 0.2f, 1}, {0.2f, 0.8f, 0.2f, 1}, {0.2f, 0.2f, 0.8f, 1}};

static std::vector<float> flat_triangle(float x0)
{
   const float pos[3][2] = {{x0, -0.8f}, {x0 + 0.8f, -0.8f}, {x0 + 0.4f, 0.8f}};
   std::vector<float> v;
   for (int i = 0; i < 3; i++) {
      v.insert(v.end(), {pos[i][0], pos[i][1]});
      v.insert(v.end(), FLAT_COLORS[i], FLAT_COLORS[i] + 4);
   }
   return v;
}

static RGBA flat_expect(int vertex, int vid)
{
   return unorm(FLAT_COLORS[vertex][0], FLAT_COLORS[vertex][1], FLAT_COLORS[vertex][2], (vid * 40 + 15) / 255.0);
}

void scene_varying_flat(SceneCtx &ctx)
{
   compile_program(VS_FLAT, FS_FLAT);
   std::vector<float> v = flat_triangle(-0.9f), right = flat_triangle(0.1f);
   v.insert(v.end(), right.begin(), right.end());
   draw_arrays(GL_TRIANGLES, v, {2, 4});
   probe(ctx, 16, 24, flat_expect(2, 2));
   probe(ctx, 48, 24, flat_expect(2, 5));
}

void scene_provoking_vertex(SceneCtx &ctx)
{
   compile_program(VS_FLAT, FS_FLAT);
   glProvokingVertex(GL_FIRST_VERTEX_CONVENTION);
   draw_arrays(GL_TRIANGLES, flat_triangle(-0.9f), {2, 4});
   glProvokingVertex(GL_LAST_VERTEX_CONVENTION);
   draw_arrays(GL_TRIANGLES, flat_triangle(0.1f), {2, 4});
   probe(ctx, 16, 24, flat_expect(0, 0));
   probe(ctx, 48, 24, flat_expect(2, 2));
}

void scene_uniforms(SceneCtx &ctx)
{
   auto quadrant = [&](int q, const char *decls, const char *expr) {
      std::string fs = std::string("#version 400 core\n") + decls +
                       "\nlayout(location = 0) out vec4 o;\nvoid main() { o = " + expr + "; }\n";
      GLuint p = compile_program(VS_QUAD, fs.c_str());
      glViewport((q % 2) * 32, (q / 2) * 32, 32, 32);
      return p;
   };

   GLuint p = quadrant(0, "uniform float f;\nuniform vec2 v2;\nuniform vec3 v3;\nuniform vec4 v4;",
                       "vec4(f, v2.y, v3.z, v4.w)");
   glUniform1f(uloc(p, "f"), 0.2f);
   glUniform2f(uloc(p, "v2"), 0.9f, 0.4f);
   const float v3[3] = {0.0f, 0.0f, 0.6f};
   glUniform3fv(uloc(p, "v3"), 1, v3);
   glUniform4f(uloc(p, "v4"), 0.0f, 0.0f, 0.0f, 0.8f);
   fullscreen_quad();
   probe(ctx, 16, 16, {51, 102, 153, 204});

   p = quadrant(1, "uniform mat4 m;", "m * vec4(0.2, 0.4, 0.6, 1.0)");
   // Column-major: result = (v.z, v.x, v.y + 0.2 v.w, v.w). A transposed upload gives other values.
   const float m[16] = {0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0.2f, 1};
   glUniformMatrix4fv(uloc(p, "m"), 1, GL_FALSE, m);
   fullscreen_quad();
   probe(ctx, 48, 16, {153, 51, 153, 255});

   p = quadrant(2, "uniform float fa[4];\nuniform vec4 va[3];", "vec4(fa[1], fa[3], va[2].x, va[1].w)");
   const float fa[4] = {0.1f, 0.2f, 0.3f, 0.4f};
   const float va[12] = {0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.6f, 0.8f, 0.9f, 0.9f, 0.9f};
   glUniform1fv(uloc(p, "fa"), 4, fa);
   glUniform4fv(uloc(p, "va"), 3, va);
   fullscreen_quad();
   probe(ctx, 16, 48, {51, 102, 204, 153});

   p = quadrant(3, "uniform int i;\nuniform ivec2 iv;\nuniform uint u;\nuniform mat2 m2;",
                "vec4(float(i) / 255.0, float(iv.y) / 255.0, float(u) / 255.0, (m2 * vec2(1.0, 0.0)).y)");
   glUniform1i(uloc(p, "i"), 17);
   const GLint iv[2] = {3, 200};
   glUniform2iv(uloc(p, "iv"), 1, iv);
   glUniform1ui(uloc(p, "u"), 99u);
   const float m2[4] = {0.25f, 0.6f, 0.9f, 0.9f};
   glUniformMatrix2fv(uloc(p, "m2"), 1, GL_FALSE, m2);
   fullscreen_quad();
   probe(ctx, 48, 48, {17, 200, 99, 153});
}

void scene_ubo_binding(SceneCtx &ctx)
{
   require_extension("GL_ARB_shading_language_420pack");
   compile_program(VS_QUAD, R"(#version 400 core
#extension GL_ARB_shading_language_420pack : require
layout(std140, binding = 2) uniform Material {
   vec4 base;
   float gain;
   vec3 dir;
   mat4 m;
   float weights[3];
};
layout(std140, binding = 5) uniform Tint {
   vec4 tint;
   ivec2 iv;
};
layout(location = 0) out vec4 o;
void main() {
   vec4 left = vec4(base.x * gain, dir.z, (m * vec4(0.0, 1.0, 0.0, 0.0)).w, weights[2]);
   vec4 right = tint * vec4(1.0, 1.0, float(iv.y) / 255.0, 1.0);
   o = gl_FragCoord.x < 32.0 ? left : right;
}
)");
   // std140 offsets: base 0, gain 16, dir 32, m 48 (column c at 48 + 16c), weights[i] at 112 + 16i.
   // Every other float is a decoy 0.9 so a wrong offset shows up as a wrong color.
   float a[40];
   for (float &f : a)
      f = 0.9f;
   a[0] = 0.5f;           // base.x
   a[4] = 0.8f;           // gain
   a[8 + 2] = 0.6f;       // dir.z
   a[12 + 4 * 1 + 3] = 0.2f;  // m[1].w
   a[28 + 4 * 2] = 0.8f;  // weights[2]
   struct {
      float tint[4];
      int32_t iv[2];
      int32_t pad[2];
   } b = {{0.2f, 0.4f, 1.0f, 1.0f}, {7, 153}, {0, 0}};
   GLuint bufs[2];
   glGenBuffers(2, bufs);
   glBindBuffer(GL_UNIFORM_BUFFER, bufs[0]);
   glBufferData(GL_UNIFORM_BUFFER, sizeof a, a, GL_STATIC_DRAW);
   glBindBuffer(GL_UNIFORM_BUFFER, bufs[1]);
   glBufferData(GL_UNIFORM_BUFFER, sizeof b, &b, GL_STATIC_DRAW);
   glBindBufferBase(GL_UNIFORM_BUFFER, 2, bufs[0]);
   glBindBufferBase(GL_UNIFORM_BUFFER, 5, bufs[1]);
   fullscreen_quad();
   probe(ctx, 16, 32, {102, 153, 51, 204});
   probe(ctx, 48, 32, {51, 102, 153, 255});
}

void scene_discard(SceneCtx &ctx)
{
   glClearColor(0.6f, 0.2f, 0.2f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   compile_program(VS_QUAD, R"(#version 400 core
layout(location = 0) out vec4 o;
void main() {
   ivec2 cell = ivec2(gl_FragCoord.xy) >> 3;
   if (((cell.x + cell.y) & 1) == 1)
      discard;
   o = vec4(0.2, 0.8, 0.4, 1.0);
}
)");
   fullscreen_quad();
   const RGBA kept = unorm(0.2, 0.8, 0.4), cleared = unorm(0.6, 0.2, 0.2);
   probe(ctx, 4, 4, kept);
   probe(ctx, 12, 4, cleared);
   probe(ctx, 36, 44, cleared);
   probe(ctx, 44, 44, kept);
}

void scene_depth_test(SceneCtx &ctx)
{
   GLuint p = solid_program();
   glEnable(GL_DEPTH_TEST);
   glDepthFunc(GL_LESS);
   solid_color(p, 1, 0, 0);
   draw_rect(-1.0f, -1.0f, 0.5f, 1.0f, 0.2f);
   solid_color(p, 0, 1, 0);
   draw_rect(-0.5f, -1.0f, 1.0f, 1.0f, -0.2f);
   glDepthFunc(GL_GEQUAL);
   solid_color(p, 0, 0, 1);
   draw_rect(-1.0f, 0.5f, 1.0f, 1.0f, 0.0f);
   glDisable(GL_DEPTH_TEST);
   const RGBA red = {255, 0, 0, 255}, green = {0, 255, 0, 255}, blue = {0, 0, 255, 255};
   probe(ctx, 8, 32, red);
   probe(ctx, 24, 32, green);
   probe(ctx, 56, 32, green);
   probe(ctx, 8, 56, red);
   probe(ctx, 40, 56, blue);
}

void scene_depth_readback(SceneCtx &ctx)
{
   compile_program(VS_POS, FS_COLOR);
   glEnable(GL_DEPTH_TEST);
   glDepthFunc(GL_ALWAYS);
   draw_arrays(GL_TRIANGLE_STRIP, {-1, -1, -0.8f, 1, -1, 0.8f, -1, 0.5f, -0.8f, 1, 0.5f, 0.8f}, {3});
   glDisable(GL_DEPTH_TEST);
   std::vector<float> depth((size_t)ctx.w * ctx.h);
   glPixelStorei(GL_PACK_ALIGNMENT, 4);
   glReadPixels(0, 0, ctx.w, ctx.h, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
   // Top 16 bits of depth in R (high byte) and G (low byte); B marks the cleared value 1.0.
   auto encode = [](double d) {
      uint32_t v = (uint32_t)std::lround(d * 65535.0);
      return RGBA{uint8_t(v >> 8), uint8_t(v), uint8_t(d >= 1.0 ? 255 : 0), 255};
   };
   std::vector<uint8_t> px;
   for (float d : depth) {
      RGBA c = encode(d);
      px.insert(px.end(), {c.r, c.g, c.b, c.a});
   }
   write_result(ctx, px);
   for (int x : {6, 20, 33, 50, 61})
      probe(ctx, x, 20, encode(0.5 + 0.4 * ndc(x + 0.5f)));
   probe(ctx, 32, 56, encode(1.0));
}

void scene_depth_clamp(SceneCtx &ctx)
{
   GLuint p = solid_program();
   glEnable(GL_DEPTH_TEST);
   glDepthFunc(GL_LEQUAL);
   glEnable(GL_DEPTH_CLAMP);
   solid_color(p, 0, 1, 0);
   draw_rect(-1.0f, -0.5f, 0.0f, 1.0f, 1.5f);
   solid_color(p, 0, 0, 1);
   draw_rect(-1.0f, -1.0f, 1.0f, -0.5f, -1.5f);
   glDisable(GL_DEPTH_CLAMP);
   solid_color(p, 1, 0, 0);
   draw_rect(0.0f, -0.5f, 1.0f, 1.0f, 1.5f);
   draw_rect(-1.0f, -1.0f, 1.0f, -0.5f, -0.9f);
   glDisable(GL_DEPTH_TEST);
   probe(ctx, 16, 40, {0, 255, 0, 255});
   probe(ctx, 48, 40, BLACK);
   probe(ctx, 32, 8, {0, 0, 255, 255});
}

void scene_stencil(SceneCtx &ctx)
{
   GLuint p = solid_program();
   glEnable(GL_STENCIL_TEST);
   glStencilMask(0xFF);
   glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
   glStencilFunc(GL_ALWAYS, 1, 0xFF);
   glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
   draw_rect(-0.5f, -0.5f, 0.5f, 0.5f);
   glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
   draw_rect(0.0f, -1.0f, 1.0f, 1.0f);
   glStencilOp(GL_KEEP, GL_KEEP, GL_DECR_WRAP);
   draw_rect(-1.0f, -1.0f, -0.5f, -0.75f);
   glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
   glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
   const struct {
      int ref;
      float r, g, b;
   } passes[4] = {{0, 1, 0, 0}, {1, 0, 0, 1}, {2, 0, 1, 0}, {255, 1, 1, 1}};
   for (auto &pass : passes) {
      glStencilFunc(GL_EQUAL, pass.ref, 0xFF);
      solid_color(p, pass.r, pass.g, pass.b);
      draw_rect(-1, -1, 1, 1);
   }
   glDisable(GL_STENCIL_TEST);
   probe(ctx, 40, 32, {0, 255, 0, 255});
   probe(ctx, 56, 32, {0, 0, 255, 255});
   probe(ctx, 24, 32, {0, 0, 255, 255});
   probe(ctx, 8, 48, {255, 0, 0, 255});
   probe(ctx, 4, 2, {255, 255, 255, 255});
}

void scene_blend(SceneCtx &ctx)
{
   GLuint p = solid_program();
   const float dst[4] = {0.4f, 0.6f, 0.2f, 0.8f}, src[4] = {0.8f, 0.2f, 0.6f, 0.4f};
   struct Case {
      GLenum src_rgb, dst_rgb, src_a, dst_a, eq_rgb, eq_a;
      RGBA want;
   } cases[8] = {
      {GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_FUNC_ADD, GL_FUNC_ADD, {143, 112, 92, 163}},
      {GL_ONE, GL_ONE, GL_ONE, GL_ONE, GL_FUNC_ADD, GL_FUNC_ADD, {255, 204, 204, 255}},
      {GL_ONE, GL_ONE, GL_ONE, GL_ONE, GL_FUNC_SUBTRACT, GL_FUNC_SUBTRACT, {102, 0, 102, 0}},
      {GL_ONE, GL_ONE, GL_ONE, GL_ONE, GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, {0, 102, 0, 102}},
      {GL_ONE, GL_ONE, GL_ONE, GL_ONE, GL_MIN, GL_MIN, {102, 51, 51, 102}},
      {GL_ONE, GL_ONE, GL_ONE, GL_ONE, GL_MAX, GL_MAX, {204, 153, 153, 204}},
      {GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR, GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA, GL_FUNC_ADD, GL_FUNC_ADD, {122, 92, 133, 163}},
      {GL_DST_COLOR, GL_ZERO, GL_ONE, GL_ONE, GL_FUNC_ADD, GL_MAX, {82, 31, 31, 204}},
   };
   glBlendColor(0.2f, 0.6f, 0.8f, 0.4f);
   glEnable(GL_SCISSOR_TEST);
   for (int i = 0; i < 8; i++) {
      int x = (i % 4) * 16, y = (i / 4) * 32;
      glScissor(x, y, 16, 32);
      glClearColor(dst[0], dst[1], dst[2], dst[3]);
      glClear(GL_COLOR_BUFFER_BIT);
      const Case &c = cases[i];
      glEnable(GL_BLEND);
      glBlendFuncSeparate(c.src_rgb, c.dst_rgb, c.src_a, c.dst_a);
      glBlendEquationSeparate(c.eq_rgb, c.eq_a);
      solid_color(p, src[0], src[1], src[2], src[3]);
      draw_rect(-1, -1, 1, 1);
      glDisable(GL_BLEND);
      probe(ctx, x + 8, y + 16, c.want);
   }
   glDisable(GL_SCISSOR_TEST);
   glBlendEquation(GL_FUNC_ADD);
   glBlendFunc(GL_ONE, GL_ZERO);
}

void scene_scissor(SceneCtx &ctx)
{
   GLuint p = solid_program();
   glEnable(GL_SCISSOR_TEST);
   glScissor(8, 8, 24, 16);
   glClearColor(1, 0, 0, 1);
   glClear(GL_COLOR_BUFFER_BIT);
   glScissor(40, 30, 16, 30);
   solid_color(p, 0, 1, 0);
   draw_rect(-1, -1, 1, 1);
   glDisable(GL_SCISSOR_TEST);
   probe(ctx, 20, 16, {255, 0, 0, 255});
   probe(ctx, 4, 16, BLACK);
   probe(ctx, 20, 28, BLACK);
   probe(ctx, 48, 45, {0, 255, 0, 255});
   probe(ctx, 48, 26, BLACK);
   probe(ctx, 60, 45, BLACK);
}

void scene_color_mask(SceneCtx &ctx)
{
   GLuint p = solid_program();
   glClearColor(0.2f, 0.4f, 0.6f, 0.8f);
   glClear(GL_COLOR_BUFFER_BIT);
   glColorMask(GL_TRUE, GL_FALSE, GL_TRUE, GL_FALSE);
   solid_color(p, 1, 1, 1, 1);
   draw_rect(-1, -1, 0, 1);
   glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE);
   solid_color(p, 0, 0, 0, 0);
   draw_rect(0, -1, 1, 1);
   glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
   probe(ctx, 16, 32, {255, 102, 255, 204});
   probe(ctx, 48, 32, {51, 0, 153, 0});
}

void scene_cull_face(SceneCtx &ctx)
{
   GLuint p = solid_program();
   auto tri = [](float cx, float cy, bool ccw) {
      std::vector<float> v = {cx - 0.35f, cy - 0.3f, cx + 0.35f, cy - 0.3f, cx, cy + 0.35f};
      if (!ccw)
         std::swap(v[2], v[4]), std::swap(v[3], v[5]);
      draw_arrays(GL_TRIANGLES, v, {2});
   };
   glEnable(GL_CULL_FACE);
   glCullFace(GL_BACK);
   glFrontFace(GL_CCW);
   solid_color(p, 1, 0, 0);
   tri(-0.5f, -0.5f, true);
   tri(0.5f, -0.5f, false);
   glFrontFace(GL_CW);
   solid_color(p, 0, 1, 0);
   tri(-0.5f, 0.5f, false);
   tri(0.5f, 0.5f, true);
   glFrontFace(GL_CCW);
   glCullFace(GL_FRONT);
   solid_color(p, 0, 0, 1);
   draw_rect(-0.1f, -0.1f, 0.1f, 0.1f);
   glDisable(GL_CULL_FACE);
   glCullFace(GL_BACK);
   probe_ndc(ctx, -0.5f, -0.55f, {255, 0, 0, 255});
   probe_ndc(ctx, 0.5f, -0.55f, BLACK);
   probe_ndc(ctx, -0.5f, 0.45f, {0, 255, 0, 255});
   probe_ndc(ctx, 0.5f, 0.45f, BLACK);
   probe(ctx, 32, 32, BLACK);
}

void scene_polygon_offset(SceneCtx &ctx)
{
   GLuint p = solid_program();
   glEnable(GL_DEPTH_TEST);
   glDepthFunc(GL_LESS);
   solid_color(p, 1, 0, 0);
   draw_rect(-1, -1, 1, 1, 0.3f);
   solid_color(p, 0, 1, 0);
   glEnable(GL_POLYGON_OFFSET_FILL);
   glPolygonOffset(0.0f, -8.0f);
   draw_rect(-1, -1, 0, 1, 0.3f);
   glDisable(GL_POLYGON_OFFSET_FILL);
   draw_rect(0, -1, 1, 1, 0.3f);
   glDisable(GL_DEPTH_TEST);
   probe(ctx, 16, 32, {0, 255, 0, 255});
   probe(ctx, 48, 32, {255, 0, 0, 255});
}

void scene_polygon_mode(SceneCtx &ctx)
{
   GLuint p = solid_program();
   solid_color(p, 1, 1, 1);
   glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
   draw_arrays(GL_TRIANGLES, {ndc(8.5f), ndc(10.5f), ndc(55.5f), ndc(10.5f), ndc(32.5f), ndc(56.5f)}, {2});
   glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
   probe(ctx, 32, 10, {255, 255, 255, 255});
   probe(ctx, 20, 10, {255, 255, 255, 255});
   probe(ctx, 32, 25, BLACK);
   probe(ctx, 32, 5, BLACK);
}

void scene_lines(SceneCtx &ctx)
{
   GLuint p = solid_program();
   auto line = [](GLenum mode, std::initializer_list<float> window_xy) {
      std::vector<float> v;
      for (float c : window_xy)
         v.push_back(ndc(c));
      draw_arrays(mode, v, {2});
   };
   solid_color(p, 1, 0, 0);
   line(GL_LINES, {4.5f, 5.5f, 59.5f, 5.5f});
   solid_color(p, 0, 1, 0);
   line(GL_LINE_STRIP, {4.5f, 20.5f, 59.5f, 20.5f, 59.5f, 40.5f});
   solid_color(p, 0, 0, 1);
   line(GL_LINE_LOOP, {8.5f, 30.5f, 30.5f, 30.5f, 30.5f, 58.5f, 8.5f, 58.5f});
   probe(ctx, 32, 5, {255, 0, 0, 255});
   probe(ctx, 32, 20, {0, 255, 0, 255});
   probe(ctx, 59, 30, {0, 255, 0, 255});
   probe(ctx, 8, 45, {0, 0, 255, 255});
   probe(ctx, 20, 58, {0, 0, 255, 255});
   probe(ctx, 20, 45, BLACK);
   probe(ctx, 32, 12, BLACK);
}

