#include "glprobe.h"
#include "platform.h"

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace glp {
#define GLPROBE_DEFINE_FUNC(type, name) type name = nullptr;
GLPROBE_GL_FUNCS(GLPROBE_DEFINE_FUNC)
#undef GLPROBE_DEFINE_FUNC
}

static const Scene SCENES[] = {
#define GLPROBE_SCENE_ENTRY(name, feature) {#name, feature, scene_##name},
   GLPROBE_SCENES(GLPROBE_SCENE_ENTRY)
#undef GLPROBE_SCENE_ENTRY
};

const char *const VS_QUAD = R"(#version 400 core
layout(location = 0) in vec2 pos;
out vec2 uv;
void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }
)";

const char *const VS_POS = R"(#version 400 core
layout(location = 0) in vec4 pos;
void main() { gl_Position = pos; }
)";

const char *const FS_COLOR = R"(#version 400 core
uniform vec4 color;
layout(location = 0) out vec4 o;
void main() { o = color; }
)";

const char *const VS_POS_COLOR = R"(#version 400 core
layout(location = 0) in vec4 pos;
layout(location = 1) in vec4 col;
out vec4 c;
void main() { c = col; gl_Position = pos; }
)";

const char *const FS_VARYING = R"(#version 400 core
in vec4 c;
layout(location = 0) out vec4 o;
void main() { o = c; }
)";

// ---- PNG: stored deflate blocks, filter 0 on every row ----

static uint32_t crc32(const uint8_t *p, size_t n, uint32_t crc)
{
   static uint32_t table[256];
   if (!table[1]) {
      for (uint32_t i = 0; i < 256; i++) {
         uint32_t c = i;
         for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
         table[i] = c;
      }
   }
   crc = ~crc;
   for (size_t i = 0; i < n; i++)
      crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
   return ~crc;
}

static void put_be32(std::vector<uint8_t> &out, uint32_t v)
{
   for (int s = 24; s >= 0; s -= 8)
      out.push_back((uint8_t)(v >> s));
}

static void put_chunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &data)
{
   put_be32(out, (uint32_t)data.size());
   size_t start = out.size();
   out.insert(out.end(), type, type + 4);
   out.insert(out.end(), data.begin(), data.end());
   put_be32(out, crc32(out.data() + start, out.size() - start, 0));
}

static bool write_png(const std::string &path, int w, int h, const uint8_t *rgba_top_down)
{
   std::vector<uint8_t> raw;
   for (int y = 0; y < h; y++) {
      raw.push_back(0);
      raw.insert(raw.end(), rgba_top_down + (size_t)y * w * 4, rgba_top_down + (size_t)(y + 1) * w * 4);
   }
   uint32_t a = 1, b = 0;
   for (uint8_t v : raw) {
      a = (a + v) % 65521;
      b = (b + a) % 65521;
   }
   std::vector<uint8_t> z = {0x78, 0x01};
   size_t pos = 0;
   do {
      size_t len = std::min<size_t>(raw.size() - pos, 65535);
      z.push_back(pos + len == raw.size() ? 1 : 0);
      z.push_back((uint8_t)len);
      z.push_back((uint8_t)(len >> 8));
      z.push_back((uint8_t)~len);
      z.push_back((uint8_t)(~len >> 8));
      z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + len);
      pos += len;
   } while (pos < raw.size());
   put_be32(z, (b << 16) | a);

   std::vector<uint8_t> ihdr;
   put_be32(ihdr, (uint32_t)w);
   put_be32(ihdr, (uint32_t)h);
   ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});

   std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
   put_chunk(png, "IHDR", ihdr);
   put_chunk(png, "IDAT", z);
   put_chunk(png, "IEND", {});

   FILE *f = fopen(path.c_str(), "wb");
   if (!f)
      return false;
   bool ok = fwrite(png.data(), 1, png.size(), f) == png.size();
   return fclose(f) == 0 && ok;
}

// ---- helpers ----

uint8_t unorm8(double v)
{
   return (uint8_t)std::lround(std::clamp(v, 0.0, 1.0) * 255.0);
}

RGBA unorm(double r, double g, double b, double a)
{
   return {unorm8(r), unorm8(g), unorm8(b), unorm8(a)};
}

void probe(SceneCtx &ctx, int x, int y, RGBA want)
{
   ctx.probes.push_back({x, ctx.h - 1 - y, want});
}

bool has_extension(const char *name)
{
   GLint n = 0;
   glGetIntegerv(GL_NUM_EXTENSIONS, &n);
   for (GLint i = 0; i < n; i++) {
      const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
      if (e && strcmp(e, name) == 0)
         return true;
   }
   return false;
}

void require_extension(const char *name)
{
   if (!has_extension(name))
      throw Fail{2, std::string("required extension missing: ") + name};
}

static GLuint compile_shader(GLenum stage, const char *src, const char *stage_name)
{
   GLuint s = glCreateShader(stage);
   glShaderSource(s, 1, &src, nullptr);
   glCompileShader(s);
   GLint ok = 0, len = 0;
   glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
      std::string log(len > 0 ? len : 1, '\0');
      glGetShaderInfoLog(s, (GLsizei)log.size(), nullptr, &log[0]);
      throw Fail{3, std::string(stage_name) + " shader compile failed:\n" + log.c_str() +
                       "\n--- source ---\n" + src};
   }
   return s;
}

GLuint compile_program(const char *vs, const char *fs, const char *gs,
                       std::initializer_list<const char *> xfb_varyings)
{
   GLuint p = glCreateProgram();
   glAttachShader(p, compile_shader(GL_VERTEX_SHADER, vs, "vertex"));
   if (gs)
      glAttachShader(p, compile_shader(GL_GEOMETRY_SHADER, gs, "geometry"));
   if (fs)
      glAttachShader(p, compile_shader(GL_FRAGMENT_SHADER, fs, "fragment"));
   if (xfb_varyings.size()) {
      std::vector<const char *> names(xfb_varyings);
      glTransformFeedbackVaryings(p, (GLsizei)names.size(), names.data(), GL_INTERLEAVED_ATTRIBS);
   }
   glLinkProgram(p);
   GLint ok = 0, len = 0;
   glGetProgramiv(p, GL_LINK_STATUS, &ok);
   if (!ok) {
      glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
      std::string log(len > 0 ? len : 1, '\0');
      glGetProgramInfoLog(p, (GLsizei)log.size(), nullptr, &log[0]);
      throw Fail{3, std::string("program link failed:\n") + log.c_str()};
   }
   glUseProgram(p);
   return p;
}

GLint uloc(GLuint prog, const char *name)
{
   GLint l = glGetUniformLocation(prog, name);
   if (l < 0)
      throw Fail{3, std::string("uniform not active: ") + name};
   return l;
}

GLuint solid_program()
{
   return compile_program(VS_POS, FS_COLOR);
}

void solid_color(GLuint prog, float r, float g, float b, float a)
{
   glUseProgram(prog);
   glUniform4f(uloc(prog, "color"), r, g, b, a);
}

void draw_arrays(GLenum mode, const std::vector<float> &data, std::initializer_list<int> sizes,
                 GLsizei instances)
{
   GLuint vao, vbo;
   glGenVertexArrays(1, &vao);
   glBindVertexArray(vao);
   glGenBuffers(1, &vbo);
   glBindBuffer(GL_ARRAY_BUFFER, vbo);
   glBufferData(GL_ARRAY_BUFFER, data.size() * sizeof(float), data.data(), GL_STATIC_DRAW);
   int stride = 0;
   for (int s : sizes)
      stride += s;
   int offset = 0, loc = 0;
   for (int s : sizes) {
      glEnableVertexAttribArray(loc);
      glVertexAttribPointer(loc, s, GL_FLOAT, GL_FALSE, stride * sizeof(float),
                            (const void *)(uintptr_t)(offset * sizeof(float)));
      offset += s;
      loc++;
   }
   GLsizei count = (GLsizei)(data.size() / stride);
   if (instances > 1)
      glDrawArraysInstanced(mode, 0, count, instances);
   else
      glDrawArrays(mode, 0, count);
}

void fullscreen_quad()
{
   draw_arrays(GL_TRIANGLE_STRIP, {-1, -1, 1, -1, -1, 1, 1, 1}, {2});
}

void draw_rect(float x0, float y0, float x1, float y1, float z)
{
   draw_arrays(GL_TRIANGLE_STRIP, {x0, y0, z, x1, y0, z, x0, y1, z, x1, y1, z}, {3});
}

void check_fbo(GLenum target, const char *what)
{
   GLenum status = glCheckFramebufferStatus(target);
   if (status != GL_FRAMEBUFFER_COMPLETE) {
      char buf[96];
      snprintf(buf, sizeof buf, "%s: framebuffer incomplete, status 0x%04X", what, status);
      throw Fail{3, buf};
   }
}

GLuint make_fbo(int w, int h, GLuint *color_tex, GLenum color_format)
{
   GLuint fbo, tex, rb;
   glGenFramebuffers(1, &fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glGenTextures(1, &tex);
   glBindTexture(GL_TEXTURE_2D, tex);
   glTexImage2D(GL_TEXTURE_2D, 0, color_format, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
   glGenRenderbuffers(1, &rb);
   glBindRenderbuffer(GL_RENDERBUFFER, rb);
   glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
   glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb);
   check_fbo(GL_FRAMEBUFFER, "make_fbo");
   glViewport(0, 0, w, h);
   if (color_tex)
      *color_tex = tex;
   return fbo;
}

GLuint make_tex2d(int w, int h, GLenum internal_format, GLenum format, GLenum type, const void *data)
{
   GLuint t;
   glGenTextures(1, &t);
   glBindTexture(GL_TEXTURE_2D, t);
   glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
   glTexImage2D(GL_TEXTURE_2D, 0, internal_format, w, h, 0, format, type, data);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
   return t;
}

void write_result(SceneCtx &ctx, const std::vector<uint8_t> &rgba)
{
   glBindTexture(GL_TEXTURE_2D, ctx.color_tex);
   glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
   glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ctx.w, ctx.h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
}

void cell_viewport(int cell)
{
   glViewport((cell % 8) * 8, (cell / 8) * 8, 8, 8);
}

void probe_cell(SceneCtx &ctx, int cell, RGBA want)
{
   probe(ctx, (cell % 8) * 8 + 4, (cell / 8) * 8 + 4, want);
}

// ---- driver ----

static void on_abort(int)
{
   fputs("ABORT\n", stderr);
   fflush(stderr);
   _Exit(134);
}

static int usage()
{
   fprintf(stderr, "usage: glprobe --list\n       glprobe --scene NAME --out DIR\n");
   return 1;
}

static bool gl_errors(const char *when)
{
   bool any = false;
   for (GLenum e; (e = glGetError()) != GL_NO_ERROR;) {
      fprintf(stderr, "glGetError 0x%04X %s\n", e, when);
      any = true;
   }
   return any;
}

int main(int argc, char **argv)
{
   if (argc == 2 && strcmp(argv[1], "--list") == 0) {
      for (const Scene &s : SCENES)
         printf("%s\t%s\n", s.name, s.feature);
      return 0;
   }
   const char *scene_name = nullptr, *out_dir = nullptr;
   for (int i = 1; i + 1 < argc; i += 2) {
      if (strcmp(argv[i], "--scene") == 0)
         scene_name = argv[i + 1];
      else if (strcmp(argv[i], "--out") == 0)
         out_dir = argv[i + 1];
      else
         return usage();
   }
   if (argc != 5 || !scene_name || !out_dir)
      return usage();
   const Scene *scene = nullptr;
   for (const Scene &s : SCENES)
      if (strcmp(s.name, scene_name) == 0)
         scene = &s;
   if (!scene) {
      fprintf(stderr, "unknown scene: %s\n", scene_name);
      return 1;
   }
   std::error_code ec;
   std::filesystem::create_directories(out_dir, ec);
   std::string out_path = (std::filesystem::path(out_dir) / (std::string(scene->name) + ".png")).string();

   platform_install_crash_handlers();
   signal(SIGABRT, on_abort);

   std::string err;
   if (!platform_create_context(err)) {
      fprintf(stderr, "context creation failed: %s\n", err.c_str());
      return 2;
   }
#define GLPROBE_LOAD_FUNC(type, name) \
   name = (type)platform_get_proc(#name); \
   if (!name) { \
      fprintf(stderr, "GL entry point missing: %s\n", #name); \
      return 2; \
   }
   GLPROBE_GL_FUNCS(GLPROBE_LOAD_FUNC)
#undef GLPROBE_LOAD_FUNC

   printf("renderer=%s\n", (const char *)glGetString(GL_RENDERER));
   printf("version=%s\n", (const char *)glGetString(GL_VERSION));
   printf("glsl=%s\n", (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));
   fflush(stdout);

   GLint major = 0, minor = 0, profile = 0;
   glGetIntegerv(GL_MAJOR_VERSION, &major);
   glGetIntegerv(GL_MINOR_VERSION, &minor);
   if (major < 4) {
      fprintf(stderr, "GL %d.%d < 4.0\n", major, minor);
      return 2;
   }
   glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
   if (glGetError() == GL_NO_ERROR && !(profile & GL_CONTEXT_CORE_PROFILE_BIT)) {
      fprintf(stderr, "context is not core profile (mask 0x%X)\n", profile);
      return 2;
   }
   while (glGetError() != GL_NO_ERROR) {
   }

   SceneCtx ctx;
   int code = 0;
   try {
      ctx.fbo = make_fbo(ctx.w, ctx.h, &ctx.color_tex);
      glClearColor(0, 0, 0, 1);
      glClearDepth(1.0);
      glClearStencil(0);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
      scene->run(ctx);
   } catch (const Fail &f) {
      fprintf(stderr, "%s: %s\n", scene->name, f.what.c_str());
      return f.code;
   }
   if (gl_errors("after scene"))
      code = 3;

   std::vector<uint8_t> px((size_t)ctx.w * ctx.h * 4), flipped(px.size());
   glBindFramebuffer(GL_READ_FRAMEBUFFER, ctx.fbo);
   glReadBuffer(GL_COLOR_ATTACHMENT0);
   glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
   glPixelStorei(GL_PACK_ALIGNMENT, 1);
   glReadPixels(0, 0, ctx.w, ctx.h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
   if (gl_errors("in readback"))
      code = 3;
   size_t row = (size_t)ctx.w * 4;
   for (int y = 0; y < ctx.h; y++)
      memcpy(&flipped[y * row], &px[(ctx.h - 1 - y) * row], row);
   if (!write_png(out_path, ctx.w, ctx.h, flipped.data())) {
      fprintf(stderr, "cannot write %s\n", out_path.c_str());
      return 1;
   }
   if (code)
      return code;

   int bad = 0;
   for (const Probe &p : ctx.probes) {
      const uint8_t *got = &flipped[(p.y * ctx.w + p.x) * 4];
      const uint8_t want[4] = {p.want.r, p.want.g, p.want.b, p.want.a};
      bool ok = true;
      for (int c = 0; c < 4; c++)
         ok &= std::abs(int(got[c]) - int(want[c])) <= 2;
      if (!ok) {
         fprintf(stderr, "probe mismatch x=%d y=%d expected=(%d,%d,%d,%d) got=(%d,%d,%d,%d)\n", p.x,
                 p.y, want[0], want[1], want[2], want[3], got[0], got[1], got[2], got[3]);
         bad++;
      }
   }
   if (bad) {
      fprintf(stderr, "%s: %d of %zu probes mismatched\n", scene->name, bad, ctx.probes.size());
      return 4;
   }
   return 0;
}
