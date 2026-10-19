#include "glprobe.h"

#include <cstring>

static const RGBA BLACK = {0, 0, 0, 255};

static GLuint make_buffer(GLenum target, const void *data, size_t size)
{
   GLuint b;
   glGenBuffers(1, &b);
   glBindBuffer(target, b);
   glBufferData(target, (GLsizeiptr)size, data, GL_STATIC_DRAW);
   return b;
}

static GLuint bind_new_vao()
{
   GLuint vao;
   glGenVertexArrays(1, &vao);
   glBindVertexArray(vao);
   return vao;
}

// Fills the 8x8 cell grid on the CPU: cell i gets colors[i], the rest stay gray.
static std::vector<uint8_t> cell_image(const std::vector<RGBA> &colors)
{
   std::vector<uint8_t> px(64 * 64 * 4);
   for (int y = 0; y < 64; y++)
      for (int x = 0; x < 64; x++) {
         int cell = (y / 8) * 8 + x / 8;
         RGBA c = cell < (int)colors.size() ? colors[cell] : RGBA{40, 40, 40, 255};
         memcpy(&px[(y * 64 + x) * 4], &c, 4);
      }
   return px;
}

static RGBA bytes_of(uint32_t v)
{
   return {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
}

void scene_point_size(SceneCtx &ctx)
{
   GLuint p = compile_program(R"(#version 400 core
layout(location = 0) in vec2 pos;
layout(location = 1) in float size;
void main() { gl_PointSize = size; gl_Position = vec4(pos, 0.0, 1.0); }
)",
                              FS_COLOR);
   glUniform4f(uloc(p, "color"), 1.0f, 0.8f, 0.2f, 1.0f);
   glEnable(GL_PROGRAM_POINT_SIZE);
   draw_arrays(GL_POINTS,
               {ndc(16), ndc(16), 8, ndc(48), ndc(16), 12, ndc(16.5f), ndc(48.5f), 1, ndc(48.5f), ndc(48.5f), 5},
               {2, 1});
   glDisable(GL_PROGRAM_POINT_SIZE);
   RGBA c = unorm(1.0, 0.8, 0.2);
   probe(ctx, 16, 16, c);
   probe(ctx, 13, 18, c);
   probe(ctx, 21, 16, BLACK);
   probe(ctx, 48, 16, c);
   probe(ctx, 43, 20, c);
   probe(ctx, 55, 16, BLACK);
   probe(ctx, 16, 48, c);
   probe(ctx, 17, 48, BLACK);
   probe(ctx, 48, 48, c);
   probe(ctx, 52, 48, BLACK);
}

void scene_mrt(SceneCtx &ctx)
{
   GLuint fbo, tex[3];
   glGenFramebuffers(1, &fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glGenTextures(3, tex);
   for (int i = 0; i < 3; i++) {
      glBindTexture(GL_TEXTURE_2D, tex[i]);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, tex[i], 0);
   }
   const GLenum bufs[3] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
   glDrawBuffers(3, bufs);
   check_fbo(GL_FRAMEBUFFER, "mrt");
   compile_program(VS_QUAD, R"(#version 400 core
layout(location = 0) out vec4 o0;
layout(location = 1) out vec4 o1;
layout(location = 2) out vec4 o2;
void main() {
   o0 = vec4(0.8, 0.2, 0.0, 1.0);
   o1 = vec4(0.0, 0.6, 0.2, 1.0);
   o2 = vec4(0.4, 0.0, 1.0, 0.6);
}
)");
   fullscreen_quad();

   std::vector<uint8_t> att[3], out(64 * 64 * 4);
   glPixelStorei(GL_PACK_ALIGNMENT, 1);
   for (int i = 0; i < 3; i++) {
      att[i].resize(64 * 64 * 4);
      glReadBuffer(GL_COLOR_ATTACHMENT0 + i);
      glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, att[i].data());
   }
   for (int y = 0; y < 64; y++)
      for (int x = 0; x < 64; x++) {
         int i = x < 21 ? 0 : x < 43 ? 1 : 2;
         memcpy(&out[(y * 64 + x) * 4], &att[i][(y * 64 + x) * 4], 4);
      }
   glBindFramebuffer(GL_FRAMEBUFFER, ctx.fbo);
   write_result(ctx, out);
   probe(ctx, 10, 32, {204, 51, 0, 255});
   probe(ctx, 32, 32, {0, 153, 51, 255});
   probe(ctx, 54, 32, {102, 0, 255, 153});
}

void scene_instancing(SceneCtx &ctx)
{
   compile_program(R"(#version 400 core
layout(location = 0) in vec2 pos;
layout(location = 1) in vec4 icol;
out vec4 c;
void main() {
   vec2 cell = vec2(float(gl_InstanceID % 4), float(gl_InstanceID / 4));
   c = icol;
   gl_Position = vec4(pos * 0.2 + (cell * 0.5 - 0.75), 0.0, 1.0);
}
)",
                   FS_VARYING);
   auto color = [](int i) { return RGBA{uint8_t(i * 16), uint8_t(255 - i * 16), uint8_t((i * 37) % 256), 255}; };
   std::vector<float> colors;
   for (int i = 0; i < 16; i++) {
      RGBA c = color(i);
      colors.insert(colors.end(), {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f});
   }
   const float quad[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
   bind_new_vao();
   make_buffer(GL_ARRAY_BUFFER, quad, sizeof quad);
   glEnableVertexAttribArray(0);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
   make_buffer(GL_ARRAY_BUFFER, colors.data(), colors.size() * sizeof(float));
   glEnableVertexAttribArray(1);
   glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
   glVertexAttribDivisor(1, 1);
   glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 16);
   for (int i = 0; i < 16; i++)
      probe(ctx, (i % 4) * 16 + 8, (i / 4) * 16 + 8, color(i));
   probe(ctx, 16, 8, BLACK);
   probe(ctx, 40, 32, BLACK);
}

void scene_vertex_formats(SceneCtx &ctx)
{
   compile_program(R"(#version 400 core
layout(location = 0) in vec4 pos;
layout(location = 1) in vec4 col;
layout(location = 2) in int bits;
out vec4 c;
void main() {
   vec3 extra = vec3(float(bits & 255), float((bits >> 8) & 255), float((bits >> 16) & 255)) / 255.0;
   c = vec4(col.rgb + extra, col.a);
   gl_Position = pos;
}
)",
                   FS_VARYING);
   bind_new_vao();
   auto float_quad = [](float x0, float y0) {
      const float v[8] = {x0, y0, x0 + 1, y0, x0, y0 + 1, x0 + 1, y0 + 1};
      make_buffer(GL_ARRAY_BUFFER, v, sizeof v);
      glEnableVertexAttribArray(0);
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
   };
   auto no_packed = [] {
      glDisableVertexAttribArray(2);
      glVertexAttribI4i(2, 0, 0, 0, 0);
   };

   // Bottom-left: D3DCOLOR-style size GL_BGRA unsigned bytes (memory order B, G, R, A).
   float_quad(-1, -1);
   const uint8_t bgra[16] = {50, 100, 200, 255, 50, 100, 200, 255, 50, 100, 200, 255, 50, 100, 200, 255};
   make_buffer(GL_ARRAY_BUFFER, bgra, sizeof bgra);
   glEnableVertexAttribArray(1);
   glVertexAttribPointer(1, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, 0, nullptr);
   no_packed();
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

   // Bottom-right: normalized GL_SHORT position and color.
   const int16_t spos[8] = {0, -32767, 32767, -32767, 0, 0, 32767, 0};
   make_buffer(GL_ARRAY_BUFFER, spos, sizeof spos);
   glVertexAttribPointer(0, 2, GL_SHORT, GL_TRUE, 0, nullptr);
   const int16_t scol[16] = {6553, 13107, 19660, 32767, 6553, 13107, 19660, 32767,
                             6553, 13107, 19660, 32767, 6553, 13107, 19660, 32767};
   make_buffer(GL_ARRAY_BUFFER, scol, sizeof scol);
   glVertexAttribPointer(1, 4, GL_SHORT, GL_TRUE, 0, nullptr);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

   // Top-left: color array disabled, constant generic attribute (xemu inline values).
   float_quad(-1, 0);
   glDisableVertexAttribArray(1);
   const float konst[4] = {0.8f, 0.2f, 0.4f, 1.0f};
   glVertexAttrib4fv(1, konst);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

   // Top-right: integer attribute through glVertexAttribIPointer (xemu packed CMP normals).
   float_quad(0, 0);
   glDisableVertexAttribArray(1);
   const float zero[4] = {0, 0, 0, 1};
   glVertexAttrib4fv(1, zero);
   const int32_t packed[4] = {0x00CC3399, 0x00CC3399, 0x00CC3399, 0x00CC3399};
   make_buffer(GL_ARRAY_BUFFER, packed, sizeof packed);
   glEnableVertexAttribArray(2);
   glVertexAttribIPointer(2, 1, GL_INT, 0, nullptr);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

   probe(ctx, 16, 16, {200, 100, 50, 255});
   probe(ctx, 48, 16, {51, 102, 153, 255});
   probe(ctx, 16, 48, {204, 51, 102, 255});
   probe(ctx, 48, 48, {153, 51, 204, 255});
}

void scene_draw_elements(SceneCtx &ctx)
{
   compile_program(VS_POS_COLOR, FS_VARYING);
   const float colors[3][4] = {{1, 0.6f, 0, 1}, {0, 1, 0.6f, 1}, {0.6f, 0, 1, 1}};
   std::vector<float> v;
   for (int k = 0; k < 3; k++) {
      float y0 = -1 + k * (2.0f / 3) + 0.1f, y1 = -1 + (k + 1) * (2.0f / 3) - 0.1f;
      const float corners[4][2] = {{-0.9f, y0}, {0.9f, y0}, {-0.9f, y1}, {0.9f, y1}};
      for (auto &c : corners) {
         v.insert(v.end(), {c[0], c[1], 0, 1});
         v.insert(v.end(), colors[k], colors[k] + 4);
      }
   }
   bind_new_vao();
   make_buffer(GL_ARRAY_BUFFER, v.data(), v.size() * sizeof(float));
   glEnableVertexAttribArray(0);
   glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 8 * sizeof(float), nullptr);
   glEnableVertexAttribArray(1);
   glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (const void *)(4 * sizeof(float)));

   const uint8_t i8[6] = {0, 1, 2, 2, 1, 3};
   make_buffer(GL_ELEMENT_ARRAY_BUFFER, i8, sizeof i8);
   glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_BYTE, nullptr);
   const uint16_t i16[6] = {4, 5, 6, 6, 5, 7};
   make_buffer(GL_ELEMENT_ARRAY_BUFFER, i16, sizeof i16);
   glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
   const uint32_t i32[8] = {0xFFFFFFFFu, 0xFFFFFFFFu, 8, 9, 10, 10, 9, 11};
   make_buffer(GL_ELEMENT_ARRAY_BUFFER, i32, sizeof i32);
   glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, (const void *)(2 * sizeof(uint32_t)));

   const int ys[3] = {10, 32, 53};
   for (int k = 0; k < 3; k++) {
      probe(ctx, 32, ys[k], unorm(colors[k][0], colors[k][1], colors[k][2]));
      probe(ctx, 8, ys[k], unorm(colors[k][0], colors[k][1], colors[k][2]));
   }
   probe(ctx, 1, 32, BLACK);
}

void scene_multidraw_fan(SceneCtx &ctx)
{
   compile_program(VS_POS_COLOR, FS_VARYING);
   std::vector<float> v;
   auto vert = [&](float x, float y, const float *c) {
      v.insert(v.end(), {x, y, 0, 1});
      v.insert(v.end(), c, c + 4);
   };
   const float c1[4] = {1.0f, 0.8f, 0.2f, 1}, c2[4] = {0.2f, 0.4f, 1.0f, 1};
   const float hex[7][2] = {{0.4f, 0}, {0.2f, 0.3464f}, {-0.2f, 0.3464f}, {-0.4f, 0},
                            {-0.2f, -0.3464f}, {0.2f, -0.3464f}, {0.4f, 0}};
   vert(-0.5f, 0, c1);
   for (auto &h : hex)
      vert(-0.5f + h[0], h[1], c1);
   const float diamond[5][2] = {{0.35f, 0}, {0, 0.35f}, {-0.35f, 0}, {0, -0.35f}, {0.35f, 0}};
   vert(0.5f, 0, c2);
   for (auto &d : diamond)
      vert(0.5f + d[0], d[1], c2);
   bind_new_vao();
   make_buffer(GL_ARRAY_BUFFER, v.data(), v.size() * sizeof(float));
   glEnableVertexAttribArray(0);
   glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 8 * sizeof(float), nullptr);
   glEnableVertexAttribArray(1);
   glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (const void *)(4 * sizeof(float)));
   const GLint first[2] = {0, 8};
   const GLsizei count[2] = {8, 6};
   glMultiDrawArrays(GL_TRIANGLE_FAN, first, count, 2);
   RGBA a = unorm(c1[0], c1[1], c1[2]), b = unorm(c2[0], c2[1], c2[2]);
   probe(ctx, 16, 32, a);
   probe(ctx, 21, 37, a);
   probe(ctx, 9, 28, a);
   probe(ctx, 48, 32, b);
   probe(ctx, 52, 28, b);
   probe(ctx, 32, 32, BLACK);
   probe(ctx, 58, 40, BLACK);
}

void scene_primitive_restart(SceneCtx &ctx)
{
   GLuint p = solid_program();
   solid_color(p, 1, 0, 0);
   const float v[16] = {-0.9f, -0.5f, -0.3f, -0.5f, -0.9f, 0.5f, -0.3f, 0.5f,
                        0.3f,  -0.5f, 0.9f,  -0.5f, 0.3f,  0.5f, 0.9f,  0.5f};
   bind_new_vao();
   make_buffer(GL_ARRAY_BUFFER, v, sizeof v);
   glEnableVertexAttribArray(0);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
   const uint16_t idx[9] = {0, 1, 2, 3, 0xFFFF, 4, 5, 6, 7};
   make_buffer(GL_ELEMENT_ARRAY_BUFFER, idx, sizeof idx);
   glEnable(GL_PRIMITIVE_RESTART);
   glPrimitiveRestartIndex(0xFFFF);
   glDrawElements(GL_TRIANGLE_STRIP, 9, GL_UNSIGNED_SHORT, nullptr);
   glDisable(GL_PRIMITIVE_RESTART);
   probe(ctx, 12, 32, {255, 0, 0, 255});
   probe(ctx, 51, 32, {255, 0, 0, 255});
   probe(ctx, 32, 32, BLACK);
   probe(ctx, 32, 22, BLACK);
   probe(ctx, 32, 42, BLACK);
}

void scene_transform_feedback(SceneCtx &ctx)
{
   compile_program(R"(#version 400 core
layout(location = 0) in vec4 a;
out vec4 tf0;
out float tf1;
void main() {
   tf0 = a * 2.0 + vec4(1.0, -1.0, 0.5, 0.25);
   tf1 = float(gl_VertexID) * 3.0 + a.x;
   gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
)",
                   R"(#version 400 core
layout(location = 0) out vec4 o;
void main() { o = vec4(1.0); }
)",
                   nullptr, {"tf0", "tf1"});
   std::vector<float> in;
   for (int i = 0; i < 8; i++)
      in.insert(in.end(), {float(i), i * 0.5f, -float(i), 1 + i * 0.25f});
   bind_new_vao();
   make_buffer(GL_ARRAY_BUFFER, in.data(), in.size() * sizeof(float));
   glEnableVertexAttribArray(0);
   glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
   GLuint out = make_buffer(GL_TRANSFORM_FEEDBACK_BUFFER, nullptr, 40 * sizeof(float));
   glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, out);
   GLuint q;
   glGenQueries(1, &q);
   glEnable(GL_RASTERIZER_DISCARD);
   glBeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, q);
   glBeginTransformFeedback(GL_POINTS);
   glDrawArrays(GL_POINTS, 0, 8);
   glEndTransformFeedback();
   glEndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
   glDisable(GL_RASTERIZER_DISCARD);
   float got[40];
   glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, out);
   glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof got, got);
   GLuint written = 0;
   glGetQueryObjectuiv(q, GL_QUERY_RESULT, &written);

   std::vector<RGBA> cells;
   for (float f : got) {
      uint32_t u;
      memcpy(&u, &f, 4);
      cells.push_back(bytes_of(u));
   }
   cells.push_back(bytes_of(written));
   write_result(ctx, cell_image(cells));

   for (int i = 0; i < 8; i++) {
      const float want[5] = {2.0f * i + 1, float(i) - 1, -2.0f * i + 0.5f, 2.25f + 0.5f * i, 4.0f * i};
      for (int k = 0; k < 5; k++) {
         uint32_t u;
         memcpy(&u, &want[k], 4);
         probe_cell(ctx, i * 5 + k, bytes_of(u));
      }
   }
   probe_cell(ctx, 40, bytes_of(8));
}

void scene_clip_distance(SceneCtx &ctx)
{
   GLuint p = compile_program(R"(#version 400 core
layout(location = 0) in vec2 pos;
out float gl_ClipDistance[2];
void main() {
   gl_ClipDistance[0] = pos.x - 0.25;
   gl_ClipDistance[1] = pos.y + 0.5;
   gl_Position = vec4(pos, 0.0, 1.0);
}
)",
                              FS_COLOR);
   glUniform4f(uloc(p, "color"), 0.0f, 1.0f, 0.4f, 1.0f);
   glEnable(GL_CLIP_DISTANCE0);
   glEnable(GL_CLIP_DISTANCE1);
   fullscreen_quad();
   glDisable(GL_CLIP_DISTANCE0);
   glDisable(GL_CLIP_DISTANCE1);
   RGBA in = unorm(0, 1, 0.4);
   probe(ctx, 56, 40, in);
   probe(ctx, 41, 17, in);
   probe(ctx, 38, 40, BLACK);
   probe(ctx, 56, 14, BLACK);
   probe(ctx, 8, 8, BLACK);
}

void scene_geometry_shader(SceneCtx &ctx)
{
   compile_program(VS_POS_COLOR, R"(#version 400 core
in vec4 gc;
layout(location = 0) out vec4 o;
void main() { o = gc; }
)",
                   R"(#version 400 core
layout(triangles) in;
layout(triangle_strip, max_vertices = 6) out;
in vec4 c[];
out vec4 gc;
void main() {
   for (int copy = 0; copy < 2; copy++) {
      for (int i = 0; i < 3; i++) {
         gc = copy == 0 ? c[i] : vec4(1.0 - c[i].rgb, 1.0);
         gl_Position = gl_in[i].gl_Position + vec4(float(copy), 0.0, 0.0, 0.0);
         EmitVertex();
      }
      EndPrimitive();
   }
}
)");
   draw_arrays(GL_TRIANGLES,
               {-0.9f, -0.6f, 0, 1, 0.8f, 0.2f, 0.2f, 1, -0.1f, -0.6f, 0, 1, 0.8f, 0.2f, 0.2f, 1,
                -0.5f, 0.6f, 0, 1, 0.8f, 0.2f, 0.2f, 1},
               {4, 4});
   probe(ctx, 16, 25, unorm(0.8, 0.2, 0.2));
   probe(ctx, 48, 25, unorm(0.2, 0.8, 0.8));
   probe(ctx, 32, 56, BLACK);
   probe(ctx, 32, 25, BLACK);
}

void scene_gs_lines_adjacency(SceneCtx &ctx)
{
   compile_program(VS_POS, R"(#version 400 core
in vec4 gc;
layout(location = 0) out vec4 o;
void main() { o = gc; }
)",
                   R"(#version 400 core
layout(lines_adjacency) in;
layout(triangle_strip, max_vertices = 4) out;
out vec4 gc;
void main() {
   vec4 col = gl_PrimitiveIDIn == 0 ? vec4(0.2, 0.6, 1.0, 1.0) : vec4(1.0, 0.6, 0.2, 1.0);
   gc = col; gl_Position = gl_in[0].gl_Position; EmitVertex();
   gc = col; gl_Position = gl_in[1].gl_Position; EmitVertex();
   gc = col; gl_Position = gl_in[3].gl_Position; EmitVertex();
   gc = col; gl_Position = gl_in[2].gl_Position; EmitVertex();
   EndPrimitive();
}
)");
   draw_arrays(GL_LINES_ADJACENCY,
               {-0.9f, -0.5f, -0.1f, -0.5f, -0.1f, 0.5f, -0.9f, 0.5f,
                0.1f,  -0.5f, 0.9f,  -0.5f, 0.9f,  0.5f, 0.1f,  0.5f},
               {2});
   probe(ctx, 16, 32, unorm(0.2, 0.6, 1.0));
   probe(ctx, 8, 22, unorm(0.2, 0.6, 1.0));
   probe(ctx, 48, 32, unorm(1.0, 0.6, 0.2));
   probe(ctx, 56, 42, unorm(1.0, 0.6, 0.2));
   probe(ctx, 32, 32, BLACK);
}

void scene_occlusion_query(SceneCtx &ctx)
{
   GLuint p = solid_program();
   auto rect = [](int x0, int y0, int x1, int y1, float z) { draw_rect(ndc(x0), ndc(y0), ndc(x1), ndc(y1), z); };
   glEnable(GL_DEPTH_TEST);
   glDepthFunc(GL_LESS);
   solid_color(p, 0.5f, 0.5f, 0.5f);
   rect(8, 8, 40, 24, -0.5f);
   glDepthMask(GL_FALSE);

   GLuint q[5];
   glGenQueries(5, q);
   const struct {
      GLenum target;
      int x0, y0, x1, y1;
      float z;
      uint32_t want;
   } cases[5] = {
      {GL_SAMPLES_PASSED, 8, 30, 28, 42, 0.0f, 240},
      {GL_SAMPLES_PASSED, 12, 10, 36, 22, 0.5f, 0},
      {GL_ANY_SAMPLES_PASSED, 12, 10, 36, 22, 0.5f, 0},
      {GL_ANY_SAMPLES_PASSED, 30, 10, 50, 22, 0.5f, 1},
      {GL_SAMPLES_PASSED, 30, 10, 50, 22, 0.5f, 120},
   };
   solid_color(p, 1, 0, 0);
   for (int i = 0; i < 5; i++) {
      glBeginQuery(cases[i].target, q[i]);
      rect(cases[i].x0, cases[i].y0, cases[i].x1, cases[i].y1, cases[i].z);
      glEndQuery(cases[i].target);
   }
   glDepthMask(GL_TRUE);
   glDisable(GL_DEPTH_TEST);

   // Results become scissored clear colors in the top strip: R = low byte, G = high byte;
   // a boolean ANY_SAMPLES result is shown as 255.
   glEnable(GL_SCISSOR_TEST);
   for (int i = 0; i < 5; i++) {
      GLuint r = 0;
      glGetQueryObjectuiv(q[i], GL_QUERY_RESULT, &r);
      if (cases[i].target == GL_ANY_SAMPLES_PASSED)
         r = r ? 255 : 0;
      glScissor(12 * i, 48, 12, 16);
      glClearColor((r & 255) / 255.0f, ((r >> 8) & 255) / 255.0f, 0.0f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      uint32_t want = cases[i].target == GL_ANY_SAMPLES_PASSED ? (cases[i].want ? 255 : 0) : cases[i].want;
      probe(ctx, 12 * i + 6, 56, {uint8_t(want), uint8_t(want >> 8), 0, 255});
   }
   glDisable(GL_SCISSOR_TEST);
   probe(ctx, 18, 36, {255, 0, 0, 255});
   probe(ctx, 20, 16, {128, 128, 128, 255});
   probe(ctx, 45, 16, {255, 0, 0, 255});
}

void scene_fence_sync(SceneCtx &ctx)
{
   GLuint p = solid_program();
   solid_color(p, 0, 1, 0);
   draw_rect(-1, -1, 0, 1);
   GLsync s = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
   GLenum r = glClientWaitSync(s, GL_SYNC_FLUSH_COMMANDS_BIT, 10000000000ull);
   glDeleteSync(s);
   bool signaled = r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED;
   glEnable(GL_SCISSOR_TEST);
   glScissor(32, 0, 32, 64);
   glClearColor(signaled ? 0.0f : 1.0f, 0.0f, signaled ? 1.0f : 0.0f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glDisable(GL_SCISSOR_TEST);
   probe(ctx, 16, 32, {0, 255, 0, 255});
   probe(ctx, 48, 32, {0, 0, 255, 255});
}
