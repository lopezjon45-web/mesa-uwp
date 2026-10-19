#pragma once
#include <GL/glcorearb.h>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

// Every GL entry point glprobe calls. Nothing else is declared, so a scene that calls an
// entry point outside this list fails to compile instead of failing to resolve at run time.
#define GLPROBE_GL_FUNCS(X) \
   X(PFNGLGETSTRINGPROC, glGetString) \
   X(PFNGLGETSTRINGIPROC, glGetStringi) \
   X(PFNGLGETINTEGERVPROC, glGetIntegerv) \
   X(PFNGLGETERRORPROC, glGetError) \
   X(PFNGLENABLEPROC, glEnable) \
   X(PFNGLDISABLEPROC, glDisable) \
   X(PFNGLCLEARPROC, glClear) \
   X(PFNGLCLEARCOLORPROC, glClearColor) \
   X(PFNGLCLEARDEPTHPROC, glClearDepth) \
   X(PFNGLCLEARSTENCILPROC, glClearStencil) \
   X(PFNGLVIEWPORTPROC, glViewport) \
   X(PFNGLSCISSORPROC, glScissor) \
   X(PFNGLCOLORMASKPROC, glColorMask) \
   X(PFNGLDEPTHMASKPROC, glDepthMask) \
   X(PFNGLDEPTHFUNCPROC, glDepthFunc) \
   X(PFNGLSTENCILFUNCPROC, glStencilFunc) \
   X(PFNGLSTENCILOPPROC, glStencilOp) \
   X(PFNGLSTENCILMASKPROC, glStencilMask) \
   X(PFNGLBLENDFUNCPROC, glBlendFunc) \
   X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate) \
   X(PFNGLBLENDEQUATIONPROC, glBlendEquation) \
   X(PFNGLBLENDEQUATIONSEPARATEPROC, glBlendEquationSeparate) \
   X(PFNGLBLENDCOLORPROC, glBlendColor) \
   X(PFNGLCULLFACEPROC, glCullFace) \
   X(PFNGLFRONTFACEPROC, glFrontFace) \
   X(PFNGLPOLYGONMODEPROC, glPolygonMode) \
   X(PFNGLPOLYGONOFFSETPROC, glPolygonOffset) \
   X(PFNGLPROVOKINGVERTEXPROC, glProvokingVertex) \
   X(PFNGLPIXELSTOREIPROC, glPixelStorei) \
   X(PFNGLREADPIXELSPROC, glReadPixels) \
   X(PFNGLREADBUFFERPROC, glReadBuffer) \
   X(PFNGLDRAWBUFFERSPROC, glDrawBuffers) \
   X(PFNGLFINISHPROC, glFinish) \
   X(PFNGLGENTEXTURESPROC, glGenTextures) \
   X(PFNGLBINDTEXTUREPROC, glBindTexture) \
   X(PFNGLACTIVETEXTUREPROC, glActiveTexture) \
   X(PFNGLTEXIMAGE2DPROC, glTexImage2D) \
   X(PFNGLTEXIMAGE3DPROC, glTexImage3D) \
   X(PFNGLTEXSUBIMAGE2DPROC, glTexSubImage2D) \
   X(PFNGLCOMPRESSEDTEXIMAGE2DPROC, glCompressedTexImage2D) \
   X(PFNGLTEXPARAMETERIPROC, glTexParameteri) \
   X(PFNGLTEXPARAMETERIVPROC, glTexParameteriv) \
   X(PFNGLTEXPARAMETERFVPROC, glTexParameterfv) \
   X(PFNGLGENSAMPLERSPROC, glGenSamplers) \
   X(PFNGLBINDSAMPLERPROC, glBindSampler) \
   X(PFNGLSAMPLERPARAMETERIPROC, glSamplerParameteri) \
   X(PFNGLGENBUFFERSPROC, glGenBuffers) \
   X(PFNGLBINDBUFFERPROC, glBindBuffer) \
   X(PFNGLBUFFERDATAPROC, glBufferData) \
   X(PFNGLGETBUFFERSUBDATAPROC, glGetBufferSubData) \
   X(PFNGLBINDBUFFERBASEPROC, glBindBufferBase) \
   X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) \
   X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
   X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
   X(PFNGLDISABLEVERTEXATTRIBARRAYPROC, glDisableVertexAttribArray) \
   X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) \
   X(PFNGLVERTEXATTRIBIPOINTERPROC, glVertexAttribIPointer) \
   X(PFNGLVERTEXATTRIBDIVISORPROC, glVertexAttribDivisor) \
   X(PFNGLVERTEXATTRIB4FVPROC, glVertexAttrib4fv) \
   X(PFNGLVERTEXATTRIBI4IPROC, glVertexAttribI4i) \
   X(PFNGLDRAWARRAYSPROC, glDrawArrays) \
   X(PFNGLDRAWARRAYSINSTANCEDPROC, glDrawArraysInstanced) \
   X(PFNGLDRAWELEMENTSPROC, glDrawElements) \
   X(PFNGLMULTIDRAWARRAYSPROC, glMultiDrawArrays) \
   X(PFNGLPRIMITIVERESTARTINDEXPROC, glPrimitiveRestartIndex) \
   X(PFNGLCREATESHADERPROC, glCreateShader) \
   X(PFNGLSHADERSOURCEPROC, glShaderSource) \
   X(PFNGLCOMPILESHADERPROC, glCompileShader) \
   X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
   X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) \
   X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
   X(PFNGLATTACHSHADERPROC, glAttachShader) \
   X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
   X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) \
   X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
   X(PFNGLUSEPROGRAMPROC, glUseProgram) \
   X(PFNGLTRANSFORMFEEDBACKVARYINGSPROC, glTransformFeedbackVaryings) \
   X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
   X(PFNGLUNIFORM1IPROC, glUniform1i) \
   X(PFNGLUNIFORM2IPROC, glUniform2i) \
   X(PFNGLUNIFORM2IVPROC, glUniform2iv) \
   X(PFNGLUNIFORM4IPROC, glUniform4i) \
   X(PFNGLUNIFORM1UIPROC, glUniform1ui) \
   X(PFNGLUNIFORM4UIPROC, glUniform4ui) \
   X(PFNGLUNIFORM1FPROC, glUniform1f) \
   X(PFNGLUNIFORM2FPROC, glUniform2f) \
   X(PFNGLUNIFORM3FPROC, glUniform3f) \
   X(PFNGLUNIFORM4FPROC, glUniform4f) \
   X(PFNGLUNIFORM1FVPROC, glUniform1fv) \
   X(PFNGLUNIFORM3FVPROC, glUniform3fv) \
   X(PFNGLUNIFORM4FVPROC, glUniform4fv) \
   X(PFNGLUNIFORMMATRIX2FVPROC, glUniformMatrix2fv) \
   X(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv) \
   X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) \
   X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
   X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) \
   X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer) \
   X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) \
   X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers) \
   X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer) \
   X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage) \
   X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer) \
   X(PFNGLGENQUERIESPROC, glGenQueries) \
   X(PFNGLBEGINQUERYPROC, glBeginQuery) \
   X(PFNGLENDQUERYPROC, glEndQuery) \
   X(PFNGLGETQUERYOBJECTUIVPROC, glGetQueryObjectuiv) \
   X(PFNGLBEGINTRANSFORMFEEDBACKPROC, glBeginTransformFeedback) \
   X(PFNGLENDTRANSFORMFEEDBACKPROC, glEndTransformFeedback) \
   X(PFNGLFENCESYNCPROC, glFenceSync) \
   X(PFNGLCLIENTWAITSYNCPROC, glClientWaitSync) \
   X(PFNGLDELETESYNCPROC, glDeleteSync)

// Pointers live in a namespace so they never collide with the C symbols opengl32.dll or
// OpenGL.framework export under the same names.
namespace glp {
#define GLPROBE_DECLARE_FUNC(type, name) extern type name;
GLPROBE_GL_FUNCS(GLPROBE_DECLARE_FUNC)
#undef GLPROBE_DECLARE_FUNC
}
using namespace glp;

// The registry. Each X(name, feature) needs a `void scene_<name>(SceneCtx &)` in a scenes_*.cpp.
#define GLPROBE_SCENES(X) \
   X(clear, "glClear of the RGBA8 color attachment, no shaders") \
   X(triangle_basic, "one flat-colored triangle through a minimal VS/FS pair") \
   X(alu_float, "float ALU: arithmetic, sqrt/rsq, exp2/log2, trig, pow, min/max/clamp/mix/step/smoothstep, rounding") \
   X(alu_int, "int/uint ALU: add mul shifts and or xor not min max abs, signed vs unsigned compares, conversions") \
   X(int_div, "signed and unsigned integer / and %, negative operands") \
   X(bitfield_ops, "bitfieldExtract/Insert, bitCount, findLSB, findMSB, bitfieldReverse (xemu vsh CMP decode)") \
   X(float_bits, "floatBitsToUint/Int and uintBits/intBitsToFloat (xemu FLOAT_MAX and clamp constants)") \
   X(precise_fma, "precise qualifier with fma (xemu psh Kahan determinant)") \
   X(varying_smooth, "perspective-correct smooth varying interpolation with distinct vertex w") \
   X(varying_flat, "flat float and int varyings take the last vertex by default") \
   X(varying_noperspective, "noperspective varying interpolation with distinct vertex w") \
   X(provoking_vertex, "glProvokingVertex FIRST vs LAST with flat varyings (xemu uses FIRST)") \
   X(uniforms, "default-block float/vec/mat4/mat2/int/uint/array uniforms (xemu's only uniform path)") \
   X(ubo_binding, "two std140 UBOs with layout(binding=) via GL_ARB_shading_language_420pack") \
   X(tex2d, "sampler2D on texture unit 3 assigned with glUniform1i (xemu sampler binding)") \
   X(tex_mipmap_lod, "textureLod picks distinct per-level colors, NEAREST_MIPMAP_NEAREST") \
   X(tex_grad, "textureGrad selects mip levels from explicit gradients") \
   X(tex_base_max_level, "GL_TEXTURE_BASE_LEVEL and GL_TEXTURE_MAX_LEVEL clamp mip selection (xemu)") \
   X(tex3d, "sampler3D on a 2x2x4 glTexImage3D texture (xemu volume textures)") \
   X(tex_cube, "samplerCube face selection from 6 single-color faces (xemu cube maps)") \
   X(tex2d_array, "sampler2DArray layer selection and layer rounding") \
   X(tex_shadow, "sampler2DShadow LEQUAL depth compare on DEPTH_COMPONENT32F") \
   X(tex_integer, "usampler2D on RGBA8UI and R32UI, isampler2D on RGBA8I") \
   X(texel_fetch, "texelFetch on mip levels 0 and 1") \
   X(tex_proj, "textureProj with vec3 and vec4 coordinates (xemu pT.xyw pattern)") \
   X(tex_bgra_packed, "BGRA and packed uploads: 8888, 8888_REV, 565, 1555_REV, 4444_REV (xemu formats)") \
   X(tex_dxt, "hand-encoded DXT1 (opaque and punch-through), DXT3, DXT5 blocks (xemu s3tc)") \
   X(tex_border_clamp, "GL_CLAMP_TO_BORDER with a border color (xemu wrap modes)") \
   X(tex_swizzle, "GL_TEXTURE_SWIZZLE_R/G/B/A and SWIZZLE_RGBA on R8, RG8, RGBA8 (xemu texture swizzle)") \
   X(sampler_objects, "sampler object overrides texture wrap state; unbinding restores it") \
   X(pixel_store_unpack, "GL_UNPACK_ROW_LENGTH, SKIP_PIXELS, SKIP_ROWS, ALIGNMENT on upload (xemu uploads)") \
   X(discard, "discard on a fragment-coordinate checkerboard") \
   X(depth_test, "depth test LESS and GEQUAL against a D24S8 attachment") \
   X(depth_readback, "glReadPixels GL_DEPTH_COMPONENT float, encoded into the image") \
   X(depth_clamp, "GL_DEPTH_CLAMP keeps geometry beyond near and far (xemu enables it)") \
   X(stencil, "stencil REPLACE, INCR, DECR_WRAP and EQUAL tests") \
   X(blend, "blend funcs and equations: alpha, add, subtract, reverse subtract, min, max, constant, separate") \
   X(scissor, "scissored clear and scissored draw") \
   X(color_mask, "glColorMask on draws") \
   X(cull_face, "glCullFace and glFrontFace on CW and CCW triangles") \
   X(polygon_offset, "GL_POLYGON_OFFSET_FILL wins a coplanar depth test") \
   X(polygon_mode, "glPolygonMode GL_LINE draws edges only") \
   X(lines, "GL_LINES, GL_LINE_STRIP, GL_LINE_LOOP on pixel-center rows and columns") \
   X(point_size, "GL_PROGRAM_POINT_SIZE with gl_PointSize 1, 5, 8, 12") \
   X(mrt, "3 color attachments written through layout(location)") \
   X(instancing, "gl_InstanceID and glVertexAttribDivisor with glDrawArraysInstanced") \
   X(vertex_formats, "attribute formats: size GL_BGRA ubyte, normalized short, glVertexAttribIPointer int, constant attribute (xemu)") \
   X(draw_elements, "glDrawElements with ubyte, ushort, uint indices and a byte offset") \
   X(multidraw_fan, "glMultiDrawArrays of two GL_TRIANGLE_FAN ranges (xemu draw path)") \
   X(primitive_restart, "GL_PRIMITIVE_RESTART with index 0xFFFF in a ushort strip") \
   X(transform_feedback, "interleaved transform feedback capture with rasterizer discard, read back and encoded") \
   X(clip_distance, "gl_ClipDistance[0] and [1] with GL_CLIP_DISTANCE0/1") \
   X(derivatives, "dFdx, dFdy, fwidth of linear varyings") \
   X(loops_branches, "dynamic for/do-while loops, break, continue, nested if, switch with fallthrough") \
   X(geometry_shader, "geometry shader emits an extra translated triangle") \
   X(gs_lines_adjacency, "lines_adjacency GS turns 4 vertices into a quad with gl_PrimitiveIDIn (xemu quad emulation)") \
   X(occlusion_query, "GL_SAMPLES_PASSED and GL_ANY_SAMPLES_PASSED results encoded as colors") \
   X(fence_sync, "glFenceSync and glClientWaitSync signal after a draw (xemu)") \
   X(fbo_render_to_texture, "render into a texture FBO, sample it in a second pass (RetroArch glcore)") \
   X(blit_framebuffer, "glBlitFramebuffer 2x NEAREST upscale and a mirrored blit (RetroArch glcore)")

struct RGBA {
   uint8_t r, g, b, a;
};

// x, y are PNG coordinates: row 0 is the top row of the written image.
struct Probe {
   int x, y;
   RGBA want;
};

struct SceneCtx {
   int w = 64, h = 64;
   GLuint fbo = 0;       // the scene's result is read from COLOR_ATTACHMENT0 of this FBO
   GLuint color_tex = 0;
   std::vector<Probe> probes;
};

struct Scene {
   const char *name;
   const char *feature;
   void (*run)(SceneCtx &);
};

// Thrown by helpers and scenes; `code` is the process exit code.
struct Fail {
   int code;
   std::string what;
};

#define GLPROBE_DECLARE_SCENE(name, feature) void scene_##name(SceneCtx &);
GLPROBE_SCENES(GLPROBE_DECLARE_SCENE)
#undef GLPROBE_DECLARE_SCENE

// Vertex shader for fullscreen_quad(): location 0 vec2 pos, out vec2 uv in [0,1].
extern const char *const VS_QUAD;
// Vertex shader passing location 0 vec4 pos through; pairs with FS_COLOR's `uniform vec4 color`.
extern const char *const VS_POS;
extern const char *const FS_COLOR;
// Locations 0 vec4 pos and 1 vec4 col; FS_VARYING writes the interpolated col.
extern const char *const VS_POS_COLOR;
extern const char *const FS_VARYING;

uint8_t unorm8(double v);
RGBA unorm(double r, double g, double b, double a = 1.0);
inline float ndc(float window) { return window / 32.0f - 1.0f; }

// y is a GL window coordinate (origin bottom-left), the space scenes draw in.
void probe(SceneCtx &ctx, int x, int y, RGBA want);

bool has_extension(const char *name);
void require_extension(const char *name);  // throws Fail{2}
GLuint compile_program(const char *vs, const char *fs, const char *gs = nullptr,
                       std::initializer_list<const char *> xfb_varyings = {});
GLint uloc(GLuint prog, const char *name);  // throws Fail{3} if the uniform is not active
GLuint solid_program();                     // VS_POS + FS_COLOR, left bound
void solid_color(GLuint prog, float r, float g, float b, float a = 1.0f);

// Interleaved float attributes at locations 0..n-1, one fresh VAO and VBO per call.
void draw_arrays(GLenum mode, const std::vector<float> &data, std::initializer_list<int> sizes,
                 GLsizei instances = 1);
void fullscreen_quad();
void draw_rect(float x0, float y0, float x1, float y1, float z = 0.0f);  // NDC, vec3 at location 0

// Texture-backed color attachment plus a D24S8 renderbuffer; throws Fail{3} if incomplete.
GLuint make_fbo(int w, int h, GLuint *color_tex, GLenum color_format = GL_RGBA8);
void check_fbo(GLenum target, const char *what);
// NEAREST, CLAMP_TO_EDGE, one level, UNPACK_ALIGNMENT 1. Leaves the texture bound on the active unit.
GLuint make_tex2d(int w, int h, GLenum internal_format, GLenum format, GLenum type, const void *data);
// Replace the scene result with CPU-built pixels in GL row order (bottom row first).
void write_result(SceneCtx &ctx, const std::vector<uint8_t> &rgba);

// 8x8 grid of 8x8-pixel cells; cell i sits at column i % 8, row i / 8 from the bottom.
void cell_viewport(int cell);
void probe_cell(SceneCtx &ctx, int cell, RGBA want);
