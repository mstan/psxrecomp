/* gpu_gl_renderer.c — hardware OpenGL renderer backend.
 *
 * ARCHITECTURE (v2 — GPU-authoritative VRAM)
 * -------------------------------------------
 * The FBO color texture (`s_hr_tex`, RGBA8, 1024*S x 512*S where S is the
 * internal-resolution scale from [video] supersampling) is the single
 * authoritative copy of VRAM. EVERY mutation goes through the GPU:
 *
 *   - polys / rects / lines  -> rasterized into the hr FBO
 *   - GP0(02h) fills         -> scissored glClear (color + stencil)
 *   - VRAM->VRAM copies      -> hr FBO -> scratch texture blit, then a
 *                               masked quad draw back into the hr FBO
 *   - CPU->VRAM transfers    -> written to the CPU mirror immediately and
 *                               accumulated into a pending-upload rect; the
 *                               rect is flushed (staging texture + quad into
 *                               the hr FBO, plus a direct R16UI subimage)
 *                               before the next GPU op, preserving op order
 *
 * PS1 MASK BIT (bit15). The FBO alpha channel carries bit15 exactly
 * (1.0 = set), and a stencil buffer mirrors it (stencil bit0 == bit15):
 *   - "set mask"  -> fragment alpha + stencil write value
 *   - "check mask"-> stencil test (pass iff stored == 0). The stencil write
 *     value is coupled to the test reference in GL, so when checking we use
 *     GL_INVERT on pass to write a 1 (stored is known 0 when the test
 *     passes) and GL_KEEP to write a 0.
 *   - textured prims (and copies/uploads, whose pixels carry per-texel STP
 *     bits) are drawn in TWO passes split by the STP bit via discard, so
 *     each pass writes a single known stencil value. The same split already
 *     existed for semi-transparent texture blending.
 *   - blending uses glBlendFuncSeparate so the alpha (mask) channel is
 *     always REPLACED by the source fragment's mask bit, never blended.
 *
 * TEXTURE SAMPLING / RENDER-TO-TEXTURE. Textured prims sample a native-res
 * R16UI mirror (`s_raw_tex`) holding raw 1555 VRAM values (CLUT decode +
 * texture window + optional bilinear in the fragment shader). GPU draws
 * mark a native-coords dirty union; before any textured draw whose texture
 * page or CLUT intersects the union, a PACK pass re-encodes the dirty
 * region of the hr FBO into the raw mirror (point-sampled at native
 * coords). CPU->VRAM uploads update the raw mirror directly. So content
 * rendered by the GPU is immediately valid as a texture source.
 *
 * CPU READBACKS (VRAM->CPU transfers, GPUREAD, screenshots, 24-bit FMV
 * display) flush uploads + pack, then glReadPixels the raw mirror straight
 * into the CPU VRAM array (raw 1555, no conversion loop).
 *
 * PRESENT is deterministic: 15-bit frames always blit the display region
 * from the hr FBO into a 4:3 letterboxed rect (single path — no more
 * frame-to-frame alternation between FBO and CPU presents). 24-bit (FMV)
 * frames sync to CPU and use the quad-present path, also letterboxed.
 * PSX_GL_FORCE_CPU_PRESENT=1 (read by main.cpp) forces the CPU path as a
 * diagnostic.
 *
 * Known divergences from the software rasterizer (accepted, documented):
 *   - GL triangle/line coverage rules differ from the PS1 DDA by ±1px on
 *     edges; lines use GL_LINES (width S) instead of Bresenham.
 *   - No dithering (the software path doesn't dither either).
 *   - Gouraud interpolation happens at 8-bit precision instead of 5-bit
 *     (smoother gradients; readback re-quantizes to 5-bit).
 *   - VRAM-wrapping draws are clamped, except GP0(02h) fills which split
 *     into wrapped segments. Wrapping copies/draws are unused by real SDKs.
 *
 * Init is all-or-nothing: if any shader/FBO fails, gl_renderer_init_context
 * returns 0 and the runtime falls back to the pure software renderer — no
 * half-GL hybrid. */

#include "gpu.h"
#include "gpu_render.h"
#include "gpu_sw_renderer.h"
#include "gpu_gl_renderer.h"
#include "latency_ring.h"

#include <SDL.h>
#include <SDL_opengl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
/* GL 1.4+ enums absent from MinGW's GL 1.1 headers. */
#define PSXGL_FRAGMENT_SHADER       0x8B30
#define PSXGL_VERTEX_SHADER         0x8B31
#define PSXGL_COMPILE_STATUS        0x8B81
#define PSXGL_LINK_STATUS           0x8B82
#define PSXGL_TEXTURE0              0x84C0
#define PSXGL_ARRAY_BUFFER          0x8892
#define PSXGL_STREAM_DRAW           0x88E0
#define PSXGL_FRAMEBUFFER           0x8D40
#define PSXGL_READ_FRAMEBUFFER      0x8CA8
#define PSXGL_DRAW_FRAMEBUFFER      0x8CA9
#define PSXGL_COLOR_ATTACHMENT0     0x8CE0
#define PSXGL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define PSXGL_FRAMEBUFFER_COMPLETE  0x8CD5
#define PSXGL_RENDERBUFFER          0x8D41
#define PSXGL_DEPTH24_STENCIL8      0x88F0
#define PSXGL_R16UI                 0x8234
#define PSXGL_RED_INTEGER           0x8D94
#define PSXGL_FUNC_ADD              0x8006
#define PSXGL_FUNC_REVERSE_SUBTRACT 0x800B
#define PSXGL_CONSTANT_ALPHA        0x8003
#define PSXGL_UNPACK_ROW_LENGTH     0x0CF2
#define PSXGL_SRC1_ALPHA            0x8589
#define PSXGL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define PSXGL_TIMEOUT_IGNORED       0xFFFFFFFFFFFFFFFFull

#ifndef APIENTRY
#define APIENTRY
#endif

#define VRAM_W 1024
#define VRAM_H 512
#define GL_MAX_INTERNAL_SCALE 4

/* ---- Loaded modern-GL entry points ------------------------------------- */
typedef GLuint (APIENTRY *PFN_glCreateShader)(GLenum);
typedef void   (APIENTRY *PFN_glShaderSource)(GLuint, GLsizei, const char *const *, const GLint *);
typedef void   (APIENTRY *PFN_glCompileShader)(GLuint);
typedef void   (APIENTRY *PFN_glGetShaderiv)(GLuint, GLenum, GLint *);
typedef void   (APIENTRY *PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, char *);
typedef void   (APIENTRY *PFN_glDeleteShader)(GLuint);
typedef GLuint (APIENTRY *PFN_glCreateProgram)(void);
typedef void   (APIENTRY *PFN_glAttachShader)(GLuint, GLuint);
typedef void   (APIENTRY *PFN_glLinkProgram)(GLuint);
typedef void   (APIENTRY *PFN_glGetProgramiv)(GLuint, GLenum, GLint *);
typedef void   (APIENTRY *PFN_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei *, char *);
typedef void   (APIENTRY *PFN_glUseProgram)(GLuint);
typedef GLint  (APIENTRY *PFN_glGetUniformLocation)(GLuint, const char *);
typedef void   (APIENTRY *PFN_glUniform1i)(GLint, GLint);
typedef void   (APIENTRY *PFN_glUniform1f)(GLint, GLfloat);
typedef void   (APIENTRY *PFN_glUniform2i)(GLint, GLint, GLint);
typedef void   (APIENTRY *PFN_glUniform4i)(GLint, GLint, GLint, GLint, GLint);
typedef void   (APIENTRY *PFN_glUniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void   (APIENTRY *PFN_glBlendColor)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void   (APIENTRY *PFN_glBlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
typedef void   (APIENTRY *PFN_glBlendEquationSeparate)(GLenum, GLenum);
typedef void   (APIENTRY *PFN_glGenVertexArrays)(GLsizei, GLuint *);
typedef void   (APIENTRY *PFN_glBindVertexArray)(GLuint);
typedef void   (APIENTRY *PFN_glActiveTexture)(GLenum);
typedef void   (APIENTRY *PFN_glGenBuffers)(GLsizei, GLuint *);
typedef void   (APIENTRY *PFN_glBindBuffer)(GLenum, GLuint);
typedef void   (APIENTRY *PFN_glBufferData)(GLenum, ptrdiff_t, const void *, GLenum);
typedef void   (APIENTRY *PFN_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
typedef void   (APIENTRY *PFN_glEnableVertexAttribArray)(GLuint);
typedef void   (APIENTRY *PFN_glBindFragDataLocationIndexed)(GLuint, GLuint, GLuint, const char *);
typedef GLsync (APIENTRY *PFN_glFenceSync)(GLenum, GLbitfield);
typedef void   (APIENTRY *PFN_glWaitSync)(GLsync, GLbitfield, GLuint64);
typedef void   (APIENTRY *PFN_glDeleteSync)(GLsync);
typedef void   (APIENTRY *PFN_glGenFramebuffers)(GLsizei, GLuint *);
typedef void   (APIENTRY *PFN_glDeleteFramebuffers)(GLsizei, const GLuint *);
typedef void   (APIENTRY *PFN_glBindFramebuffer)(GLenum, GLuint);
typedef void   (APIENTRY *PFN_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (APIENTRY *PFN_glCheckFramebufferStatus)(GLenum);
typedef void   (APIENTRY *PFN_glBlitFramebuffer)(GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLbitfield,GLenum);
typedef void   (APIENTRY *PFN_glGenRenderbuffers)(GLsizei, GLuint *);
typedef void   (APIENTRY *PFN_glDeleteRenderbuffers)(GLsizei, const GLuint *);
typedef void   (APIENTRY *PFN_glBindRenderbuffer)(GLenum, GLuint);
typedef void   (APIENTRY *PFN_glRenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
typedef void   (APIENTRY *PFN_glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
/* GPU timer queries (ARB_timer_query / core GL 3.3) — frame_perf instrumentation. */
typedef void   (APIENTRY *PFN_glGenQueries)(GLsizei, GLuint *);
typedef void   (APIENTRY *PFN_glDeleteQueries)(GLsizei, const GLuint *);
typedef void   (APIENTRY *PFN_glBeginQuery)(GLenum, GLuint);
typedef void   (APIENTRY *PFN_glEndQuery)(GLenum);
typedef void   (APIENTRY *PFN_glGetQueryObjectui64v)(GLuint, GLenum, GLuint64 *);
typedef void   (APIENTRY *PFN_glGetQueryObjectiv)(GLuint, GLenum, GLint *);
typedef void   (APIENTRY *PFN_glQueryCounter)(GLuint, GLenum);
#ifndef GL_TIME_ELAPSED
#define GL_TIME_ELAPSED            0x88BF
#endif
#ifndef GL_TIMESTAMP
#define GL_TIMESTAMP               0x8E28
#endif
#ifndef GL_QUERY_RESULT
#define GL_QUERY_RESULT            0x8866
#endif
#ifndef GL_QUERY_RESULT_AVAILABLE
#define GL_QUERY_RESULT_AVAILABLE  0x8867
#endif

static PFN_glCreateShader      p_glCreateShader;
static PFN_glShaderSource      p_glShaderSource;
static PFN_glCompileShader     p_glCompileShader;
static PFN_glGetShaderiv       p_glGetShaderiv;
static PFN_glGetShaderInfoLog  p_glGetShaderInfoLog;
static PFN_glDeleteShader      p_glDeleteShader;
static PFN_glCreateProgram     p_glCreateProgram;
static PFN_glAttachShader      p_glAttachShader;
static PFN_glLinkProgram       p_glLinkProgram;
static PFN_glGetProgramiv      p_glGetProgramiv;
static PFN_glGetProgramInfoLog p_glGetProgramInfoLog;
static PFN_glUseProgram        p_glUseProgram;
static PFN_glGetUniformLocation p_glGetUniformLocation;
static PFN_glUniform1i         p_glUniform1i;
static PFN_glUniform1f         p_glUniform1f;
static PFN_glUniform2i         p_glUniform2i;
static PFN_glUniform4i         p_glUniform4i;
static PFN_glUniform4f         p_glUniform4f;
static PFN_glBlendColor        p_glBlendColor;
static PFN_glBlendFuncSeparate p_glBlendFuncSeparate;
static PFN_glBlendEquationSeparate p_glBlendEquationSeparate;
static PFN_glGenVertexArrays   p_glGenVertexArrays;
static PFN_glBindVertexArray   p_glBindVertexArray;
static PFN_glActiveTexture     p_glActiveTexture;
static PFN_glGenBuffers        p_glGenBuffers;
static PFN_glBindBuffer        p_glBindBuffer;
static PFN_glBufferData        p_glBufferData;
static PFN_glVertexAttribPointer p_glVertexAttribPointer;
static PFN_glEnableVertexAttribArray p_glEnableVertexAttribArray;
static PFN_glBindFragDataLocationIndexed p_glBindFragDataLocationIndexed;
static PFN_glFenceSync p_glFenceSync;
static PFN_glWaitSync p_glWaitSync;
static PFN_glDeleteSync p_glDeleteSync;
static PFN_glGenFramebuffers   p_glGenFramebuffers;
static PFN_glDeleteFramebuffers p_glDeleteFramebuffers;
static PFN_glBindFramebuffer   p_glBindFramebuffer;
static PFN_glFramebufferTexture2D p_glFramebufferTexture2D;
static PFN_glCheckFramebufferStatus p_glCheckFramebufferStatus;
static PFN_glBlitFramebuffer   p_glBlitFramebuffer;
static PFN_glGenQueries          p_glGenQueries;
static PFN_glDeleteQueries       p_glDeleteQueries;
static PFN_glBeginQuery          p_glBeginQuery;
static PFN_glEndQuery            p_glEndQuery;
static PFN_glGetQueryObjectui64v p_glGetQueryObjectui64v;
static PFN_glGetQueryObjectiv    p_glGetQueryObjectiv;
static PFN_glQueryCounter        p_glQueryCounter;
static void gl_perf_init(void);   /* frame_perf — defined below, called from init_gpu_raster */
static void gl_perf_mirror_begin(void); /* frame_perf: GPU-time bracket around ONE native-wide */
static void gl_perf_mirror_end(void);   /* mirror pass (timestamp pair; splits scene canonical/mirror) */
/* Native-wide mirror ABLATION (perf attribution, debug cmd gl_ws_ablate):
 * 0 = normal, 1 = skip the whole mirror pass, 2 = full mirror state churn but no
 * draw calls, 3 = mirror draws land in the hr FBO (no per-pass FBO rebind; wide
 * margins go stale + hr gets garbage — perf probe only). */
static int s_ws_ablate = 0;
static void flush_tex_batch(void); /* textured-prim batch — defined below, flushed from coherency points */
static void flush_flat_batch(void); /* flat/gouraud GEO batch (MotK starfield 0x68 dots) */
static PFN_glGenRenderbuffers  p_glGenRenderbuffers;
static PFN_glDeleteRenderbuffers p_glDeleteRenderbuffers;
static PFN_glBindRenderbuffer  p_glBindRenderbuffer;
static PFN_glRenderbufferStorage p_glRenderbufferStorage;
static PFN_glFramebufferRenderbuffer p_glFramebufferRenderbuffer;

static int load_modern_gl(void) {
    int ok = 1;
#define LOAD(p, n) do { p = (void *)SDL_GL_GetProcAddress(n); if (!p) ok = 0; } while (0)
    LOAD(p_glCreateShader, "glCreateShader");   LOAD(p_glShaderSource, "glShaderSource");
    LOAD(p_glCompileShader, "glCompileShader"); LOAD(p_glGetShaderiv, "glGetShaderiv");
    LOAD(p_glGetShaderInfoLog, "glGetShaderInfoLog"); LOAD(p_glDeleteShader, "glDeleteShader");
    LOAD(p_glCreateProgram, "glCreateProgram"); LOAD(p_glAttachShader, "glAttachShader");
    LOAD(p_glLinkProgram, "glLinkProgram");     LOAD(p_glGetProgramiv, "glGetProgramiv");
    LOAD(p_glGetProgramInfoLog, "glGetProgramInfoLog"); LOAD(p_glUseProgram, "glUseProgram");
    LOAD(p_glGetUniformLocation, "glGetUniformLocation"); LOAD(p_glUniform1i, "glUniform1i");
    LOAD(p_glUniform1f, "glUniform1f");
    LOAD(p_glUniform2i, "glUniform2i"); LOAD(p_glUniform4i, "glUniform4i");
    LOAD(p_glUniform4f, "glUniform4f");
    LOAD(p_glBlendColor, "glBlendColor");
    LOAD(p_glBlendFuncSeparate, "glBlendFuncSeparate");
    LOAD(p_glBlendEquationSeparate, "glBlendEquationSeparate");
    LOAD(p_glGenVertexArrays, "glGenVertexArrays"); LOAD(p_glBindVertexArray, "glBindVertexArray");
    LOAD(p_glActiveTexture, "glActiveTexture");  LOAD(p_glGenBuffers, "glGenBuffers");
    LOAD(p_glBindBuffer, "glBindBuffer");        LOAD(p_glBufferData, "glBufferData");
    LOAD(p_glVertexAttribPointer, "glVertexAttribPointer");
    LOAD(p_glEnableVertexAttribArray, "glEnableVertexAttribArray");
    LOAD(p_glBindFragDataLocationIndexed, "glBindFragDataLocationIndexed");
    LOAD(p_glFenceSync, "glFenceSync");
    LOAD(p_glWaitSync, "glWaitSync");
    LOAD(p_glDeleteSync, "glDeleteSync");
    LOAD(p_glGenFramebuffers, "glGenFramebuffers"); LOAD(p_glBindFramebuffer, "glBindFramebuffer");
    LOAD(p_glDeleteFramebuffers, "glDeleteFramebuffers");
    LOAD(p_glFramebufferTexture2D, "glFramebufferTexture2D");
    LOAD(p_glCheckFramebufferStatus, "glCheckFramebufferStatus");
    LOAD(p_glBlitFramebuffer, "glBlitFramebuffer");
    LOAD(p_glGenRenderbuffers, "glGenRenderbuffers");
    LOAD(p_glDeleteRenderbuffers, "glDeleteRenderbuffers");
    LOAD(p_glBindRenderbuffer, "glBindRenderbuffer");
    LOAD(p_glRenderbufferStorage, "glRenderbufferStorage");
    LOAD(p_glFramebufferRenderbuffer, "glFramebufferRenderbuffer");
    /* GPU timer queries — optional (frame_perf). Don't fail the renderer if
     * absent; gl_perf just stays disabled. */
    p_glGenQueries          = (void *)SDL_GL_GetProcAddress("glGenQueries");
    p_glDeleteQueries       = (void *)SDL_GL_GetProcAddress("glDeleteQueries");
    p_glBeginQuery          = (void *)SDL_GL_GetProcAddress("glBeginQuery");
    p_glEndQuery            = (void *)SDL_GL_GetProcAddress("glEndQuery");
    p_glGetQueryObjectui64v = (void *)SDL_GL_GetProcAddress("glGetQueryObjectui64v");
    p_glGetQueryObjectiv    = (void *)SDL_GL_GetProcAddress("glGetQueryObjectiv");
    p_glQueryCounter        = (void *)SDL_GL_GetProcAddress("glQueryCounter");
#undef LOAD
    return ok;
}

/* ---- state ------------------------------------------------------------- */
static SDL_Window   *s_win = NULL;
static SDL_GLContext s_ctx = NULL;
static uint16_t     *s_vram = NULL;       /* CPU VRAM array (gpu.c's storage) */
static int           s_swap_interval = 1; /* SDL_GL swap interval (vsync mode) */

static int           s_scale = 1;          /* internal-res scale (hr FBO) */
static int           s_req_scale = 1;      /* requested before context init */

static GLuint        s_present_tex = 0;    /* CPU-readout present path (24bpp) */
static int           s_present_w = 0, s_present_h = 0;
static GLuint        s_present_prog = 0, s_present_vao = 0;
static GLint         s_present_uTex = -1, s_present_uUvRect = -1;
static GLuint        s_interp_prog = 0, s_interp_tex[3];
static GLsync        s_interp_fence[3];
static GLsync        s_interp_draw_fence = NULL;
static GLint         s_interp_uPrev = -1, s_interp_uCurr = -1;
static GLint         s_interp_uAlpha = -1, s_interp_uUvRect = -1;
static int           s_interp_enabled = 0, s_interp_valid = 0;
static int           s_interp_suspended = 0;
static int           s_interp_prev = 0, s_interp_cur = 0;
static int           s_interp_w = 0, s_interp_h = 0, s_interp_linear = 0;
static int           s_interp_force_4_3 = 0, s_interp_source_path = -1;
static uint64_t      s_interp_start = 0, s_interp_duration = 1;
static uint64_t      s_interp_last_capture = 0, s_interp_swaps = 0;
static uint64_t      s_interp_captures = 0;
static int           s_interp_diag = 0;
static double        s_interp_host_hz = 0.0;
static double        s_interp_target_hz = 0.0;
static SDL_GLContext s_interp_ctx = NULL;
static SDL_Thread   *s_interp_thread = NULL;
static SDL_mutex    *s_interp_mutex = NULL;
static SDL_atomic_t  s_interp_thread_run;
static GLuint        s_interp_thread_vao = 0;
static void interp_reset_history(void);
static int interp_thread_main(void *opaque);
static int interp_present(void);
static void interp_draw_quad(float alpha, int lx, int ly, int lw, int lh);

static int           s_raster_ok = 0;      /* full GPU pipeline available */

/* Authoritative VRAM: hr color texture + stencil (mask bit) FBO. */
static GLuint        s_hr_tex = 0, s_hr_fbo = 0, s_hr_rb = 0;
/* Native raw-1555 sampling mirror + readback source. */
static GLuint        s_raw_tex = 0, s_raw_fbo = 0;
/* CPU->VRAM upload staging (native RGBA8). */
static GLuint        s_up_tex = 0;
/* copy_rect staging (hr-sized RGBA8). */
static GLuint        s_scratch_tex = 0, s_scratch_fbo = 0;

/* Programs. */
static GLuint s_geo_prog = 0, s_geo_vao = 0, s_geo_vbo = 0;
static GLuint s_tex_prog = 0, s_tex_vao = 0, s_tex_vbo = 0;
/* Textured vertex: pos(2) uv(2) col(4) tpage(2) clut(2) depth(1) raw(1) limits(4)
 * — per-prim texture state in flat attributes so prims batch (see flush_tex_batch). */
#define TEXV 19
static GLuint s_blit_prog = 0, s_blit_vao = 0, s_blit_vbo = 0;
static GLuint s_pack_prog = 0, s_stencil_prog = 0, s_empty_vao = 0;

/* TEX program uniforms. */
static GLint s_uVram = -1, s_uTpage = -1, s_uClut = -1, s_uDepth = -1;
static GLint s_uRaw = -1, s_uSemipass = -1, s_uSemimode = -1;
static GLint s_uTwin = -1, s_uMaskset = -1, s_uFilter = -1;
static GLint s_uLimits = -1;
/* Native-wide x-projection uniforms (per program). u_xoff = x translation in
 * native px (0 canonical), u_xhalf = x clip half-extent in native px (512
 * canonical). When wide is off these stay 0 / 512 so the canonical pass is
 * bit-identical to the pre-native-wide projection. */
static GLint s_geo_uXoff = -1, s_geo_uXhalf = -1;
static GLint s_tex_uXoff = -1, s_tex_uXhalf = -1;
/* Native-wide 2D-backdrop x-stretch uniforms. The far 2D backdrop layer is a
 * ~4:3-width set of static prims scrolled by the draw offset; native-wide widens
 * 3D via the GTE but never these, so they leave a void in the 16:9 margins. The
 * wide mirror scales them about screen centre (u_xscale, u_xcenter) to fill it.
 * Applied only to "backdrop-phase" prims (drawn before the first clearly-wide
 * prim each frame). 1.0 / 0.0 => no-op, so the canonical pass stays identical. */
static GLint s_geo_uXscale = -1, s_geo_uXcenter = -1;
static GLint s_tex_uXscale = -1, s_tex_uXcenter = -1;
/* Runtime controls (ws_backdrop_stretch debug command). */
int g_ws_bd_stretch_on   = 1;   /* feature on (gated by native-wide + per-prim gate) */
int g_ws_bd_stretch_pct  = 0;   /* 0 = auto (g_wide_w/native_w); else pct/100 */
int g_ws_bd_phase_thresh = 24;  /* "narrow" margin (px): a prim overhanging the 4:3
                                 * region by more than this is treated as already-
                                 * widened GTE geometry and NOT stretched */
int g_ws_bd_phase_mode   = 1;   /* (retained for the debug command; unused since the
                                 * gate is now per-prim !tagged && narrow) */
/* Per-prim 2D-backdrop gate (replaces the old draw-order phase): a prim is
 * stretched iff native-wide + feature on + NOT sprite-tagged (foreground chars/
 * HUD are tagged) + NARROW (the GTE far-parallax/3D extend into the margins).
 * s_bd_gate is what wide_set_bd_scale reads; s_tb_gate is the open textured
 * batch's gate (batch flushes when a prim's gate differs, so a batch is uniform). */
static int s_bd_gate = 0;
static int s_tb_gate = 0;
/* ws_backdrop_stretch diagnostics: per-frame snapshot reported by the command. */
int g_bdg_applied = 0, g_bdg_prims = 0, g_bdg_clearx = -999999;
int g_bdg_cur = 0, g_bdg_base = 0, g_bdg_w = 0, g_bdg_off = 0;
static int s_bdg_applied = 0, s_bdg_prims = 0, s_bdg_clearx = -999999;
/* per-prim draw-order trace (ws_backdrop_trace): x-extent + textured flag, in
 * draw order, for the last frame -- so we can SEE where the background sits. */
typedef struct { short x0, x1, y0, y1; unsigned char tex; } PrimRec;
#define PTRACE_CAP 200
static PrimRec s_ptrace[PTRACE_CAP]; static int s_ptrace_n = 0;
PrimRec g_ptrace[PTRACE_CAP]; int g_ptrace_n = 0;   /* snapshot (extern) */
/* BLIT program uniforms. */
static GLint s_uBlitSrc = -1, s_uBlitPass = -1, s_uBlitMaskset = -1;
static GLint s_uBlitSrcDiv = -1, s_uBlitSrcOff = -1;
/* PACK program uniforms. */
static GLint s_uPackHr = -1, s_uPackScale = -1;
static GLint s_uStencilSrc = -1;

static uint32_t     *s_conv = NULL;        /* RGBA8 staging for uploads      */
static int           s_gpu_dirty = 0;      /* CPU VRAM array may be stale    */

/* Dirty-rect unions, native VRAM coords, inclusive bounds. */
typedef struct { int x0, y0, x1, y1, set; } DirtyRect;
static DirtyRect s_pack_dirty;             /* hr FBO content not in raw mirror */

/* CPU writes not yet in the FBO — an EXACT rect list, NOT a single union.
 *
 * THE FLICKER CLASS BUG (MMX6 GL black-frame flicker, ISSUES.md #7): the old
 * single-union s_up_pending merged DISJOINT uploads (e.g. a sprite column at
 * x>=320 and a tile row at y>=480) into one bounding box that covered the
 * framebuffers in between; flush_cpu_upload then painted that whole box from
 * the CPU VRAM array — which is STALE under GL (the FBO is authoritative,
 * ensure_cpu only syncs on demand) — stomping freshly-rendered framebuffer
 * content with stale (typically black) pixels for 1-2 presents until the game
 * redrew each buffer. Fix: track the exact uploaded rects and flush each one;
 * only pixels the CPU actually wrote are ever painted. Merging is allowed only
 * when it adds NO uncovered pixels (containment / same-band extension). On
 * overflow the pending set is flushed and the new rect starts a fresh list —
 * order-preserving and still batched for the common poke patterns.
 * (Pre-context, s_raster_ok == 0, the CPU array is fully authoritative — the
 * software rasterizer mirrors every draw — so union-merging is harmless and
 * used as the overflow strategy there.) */
#define UP_RECTS_MAX 16
static DirtyRect s_up_rects[UP_RECTS_MAX];
static int       s_up_nrects = 0;
static uint64_t  s_rt_up_diag[6];

static int runtime_upload_diag_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("PSX_RUNTIME_PERF_DIAG");
        enabled = e && e[0] && e[0] != '0';
    }
    return enabled;
}

void gl_renderer_runtime_diag(uint64_t out[6]) {
    for (int i = 0; i < 6; i++) out[i] = s_rt_up_diag[i];
}

/* Draw state mirrored from the vtable set_* calls. */
static int s_off_x = 0, s_off_y = 0;
static int s_area_x1 = 0, s_area_y1 = 0, s_area_x2 = VRAM_W - 1, s_area_y2 = VRAM_H - 1;
static int s_semi_en = 0, s_semi_mode = 0;
static int s_mod_r = 128, s_mod_g = 128, s_mod_b = 128, s_mod_raw = 0;
static int s_mask_set = 0, s_mask_check = 0;
static int s_tw_mask_x = 0, s_tw_mask_y = 0, s_tw_off_x = 0, s_tw_off_y = 0;
static int s_tex_filter = 0;
/* Opaque textured draws carry the exact mask bit in FBO alpha. Keeping the
 * duplicate stencil copy current is deferred until mask checking is requested. */
static int s_stencil_valid = 1;

/* ---- native-wide compositor (mirrors gpu_sw_renderer.c g_wide_*) ----------
 * Canonical VRAM (the hr FBO) stays 100% faithful. Native-wide lives in
 * SEPARATE wide FBOs keyed by framebuffer base_x; each framebuffer-targeting
 * primitive is mirrored into the active wide surface at local x = vram_x -
 * base_x + OFFSET. Present reads the displayed buffer's wide surface. Textures
 * always sample canonical VRAM (s_raw_tex). 4:3 / non-opted games never call
 * wide_configure, so g_wide_cur stays 0 and the wide pass never runs. */
#define WIDE_MAX_SURF 4
static GLuint s_wide_tex[WIDE_MAX_SURF];     /* color tex per surface (0 = free) */
static GLuint s_wide_fbo[WIDE_MAX_SURF];     /* FBO per surface */
static GLuint s_wide_rb[WIDE_MAX_SURF];      /* depth-stencil RB per surface (mask
                                              * mirror, same as hr). PERF-CRITICAL:
                                              * a stencil-less wide FBO made every
                                              * stencil-enabled mirror draw cost
                                              * ~0.6ms of driver work (Tomba2 16:9
                                              * collapsed to 12fps); with the RB
                                              * attached the pass costs ~2us. */
static int    s_wide_base[WIDE_MAX_SURF];    /* base_x per surface (-1 = free) */
static int    g_wide_w        = 0;           /* wide width (native px); 0 = disabled */
static int    g_wide_off      = 0;           /* centering OFFSET (native px) */
static GLuint g_wide_cur      = 0;           /* active mirror FBO (0 = no mirror) */
static int    g_wide_cur_base = 0;           /* base_x of g_wide_cur */
/* Set by gpu_flat_rect for the full-screen-overlay case so the generic
 * gpu_geometry wide mirror is skipped and the flat path emits its own
 * full-wide-width pass instead (mirrors sw_draw_flat_rect). */
static int    s_wide_suppress = 0;

/* X-translation (native px) from canonical VRAM space into the active wide
 * surface: local_x = vram_x - base_x + OFFSET. Same as SW wide_dx(). */
static inline int wide_dx(void) { return g_wide_off - g_wide_cur_base; }

/* ---- dirty-rect helpers ------------------------------------------------- */
static void rect_clear(DirtyRect *r) { r->set = 0; }
static void rect_add(DirtyRect *r, int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > VRAM_W - 1) x1 = VRAM_W - 1;
    if (y1 > VRAM_H - 1) y1 = VRAM_H - 1;
    if (x0 > x1 || y0 > y1) return;
    if (!r->set) { r->x0 = x0; r->y0 = y0; r->x1 = x1; r->y1 = y1; r->set = 1; return; }
    if (x0 < r->x0) r->x0 = x0;
    if (y0 < r->y0) r->y0 = y0;
    if (x1 > r->x1) r->x1 = x1;
    if (y1 > r->y1) r->y1 = y1;
}
static int rect_intersects(const DirtyRect *r, int x0, int y0, int x1, int y1) {
    if (!r->set) return 0;
    return !(x1 < r->x0 || x0 > r->x1 || y1 < r->y0 || y0 > r->y1);
}

/* ---- exact pending-upload rect list (see s_up_rects comment) ------------- */
static void flush_cpu_upload(void);   /* fwd: overflow flushes then re-adds */

/* Add an uploaded rect. Merges ONLY when the merge adds no uncovered pixels:
 * containment either way, or an extension within the same row-band / column-
 * band (equal y-range with touching/overlapping x-ranges, or equal x-range
 * with touching/overlapping y-ranges — the row-scan / column-scan poke
 * patterns). Never unions disjoint rects post-init (that was the flicker bug). */
static void up_add(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > VRAM_W - 1) x1 = VRAM_W - 1;
    if (y1 > VRAM_H - 1) y1 = VRAM_H - 1;
    if (x0 > x1 || y0 > y1) return;
    for (int i = 0; i < s_up_nrects; i++) {
        DirtyRect *r = &s_up_rects[i];
        if (x0 >= r->x0 && x1 <= r->x1 && y0 >= r->y0 && y1 <= r->y1)
            return;                                   /* contained */
        if (x0 <= r->x0 && x1 >= r->x1 && y0 <= r->y0 && y1 >= r->y1) {
            r->x0 = x0; r->y0 = y0; r->x1 = x1; r->y1 = y1;  /* contains */
            return;
        }
        if (y0 == r->y0 && y1 == r->y1 &&
            x0 <= r->x1 + 1 && x1 >= r->x0 - 1) {     /* same row-band extension */
            if (x0 < r->x0) r->x0 = x0;
            if (x1 > r->x1) r->x1 = x1;
            return;
        }
        if (x0 == r->x0 && x1 == r->x1 &&
            y0 <= r->y1 + 1 && y1 >= r->y0 - 1) {     /* same column-band extension */
            if (y0 < r->y0) r->y0 = y0;
            if (y1 > r->y1) r->y1 = y1;
            return;
        }
    }
    if (s_up_nrects >= UP_RECTS_MAX) {
        if (s_raster_ok) {
            flush_cpu_upload();       /* order-preserving: land the old ones */
        } else {
            /* Pre-context: the CPU array is fully authoritative (software
             * rasterizer mirrors every op), so a union is harmless. */
            DirtyRect *r = &s_up_rects[0];
            for (int i = 1; i < s_up_nrects; i++) {
                if (s_up_rects[i].x0 < r->x0) r->x0 = s_up_rects[i].x0;
                if (s_up_rects[i].y0 < r->y0) r->y0 = s_up_rects[i].y0;
                if (s_up_rects[i].x1 > r->x1) r->x1 = s_up_rects[i].x1;
                if (s_up_rects[i].y1 > r->y1) r->y1 = s_up_rects[i].y1;
            }
            if (x0 < r->x0) r->x0 = x0;
            if (y0 < r->y0) r->y0 = y0;
            if (x1 > r->x1) r->x1 = x1;
            if (y1 > r->y1) r->y1 = y1;
            s_up_nrects = 1;
            return;
        }
    }
    DirtyRect *r = &s_up_rects[s_up_nrects++];
    r->x0 = x0; r->y0 = y0; r->x1 = x1; r->y1 = y1; r->set = 1;
}

/* Add a GP0(A0) transfer's exact touched region. The software reference wraps
 * per pixel (px = (x+col) & 1023, py = (y+row) & 511), so a wrapping transfer
 * touches up to four exact rects — NOT all of VRAM (the old "wrapped: take
 * all" full-VRAM union painted stale CPU content over the framebuffers). */
static void up_add_transfer(int x, int y, int w, int h) {
    x &= VRAM_W - 1; y &= VRAM_H - 1;
    if (w > VRAM_W) w = VRAM_W;
    if (h > VRAM_H) h = VRAM_H;
    if (w <= 0 || h <= 0) return;
    int w1 = w, w2 = 0, h1 = h, h2 = 0;
    if (x + w > VRAM_W) { w1 = VRAM_W - x; w2 = w - w1; }
    if (y + h > VRAM_H) { h1 = VRAM_H - y; h2 = h - h1; }
    up_add(x, y, x + w1 - 1, y + h1 - 1);
    if (w2)       up_add(0, y, w2 - 1, y + h1 - 1);
    if (h2)       up_add(x, 0, x + w1 - 1, h2 - 1);
    if (w2 && h2) up_add(0, 0, w2 - 1, h2 - 1);
}

/* ---- coherency event ring (always-on, debug server "gl_coh_ring") -------- */
/* Every coherency-relevant operation — upload flushes, fills, copies, draw
 * bboxes, packs, full readbacks, presents, and probe perturbations — is
 * recorded with its rect and frame number. Per CLAUDE.md ring-buffer rule:
 * capture is continuous, observers query a window after the fact. Trigger
 * attribution convention: an op that flushes internally (fill/copy/draw/
 * present/peek) records its own event AFTER the FLUSH event it caused, so
 * the event following a FLUSH names the trigger. 16 B * 64 Ki = 1 MB. */
extern uint64_t s_frame_count;  /* defined in debug_server.c */

#define GL_COH_RING_CAP (1u << 16)
static GlCohEvent s_coh_ring[GL_COH_RING_CAP];
static uint64_t   s_coh_seq = 0;

/* 16x16 native-pixel tiles changed since their last on-screen present. This
 * lets a double-buffered 30 Hz game avoid swapping the unchanged front buffer
 * on the intervening 60 Hz vblank without guessing from game identity. */
#define PRES_TILE 16
#define PRES_ROWS (VRAM_H / PRES_TILE)
static uint64_t s_present_dirty[PRES_ROWS];
static int s_last_present_path = -1;
static int s_last_dx, s_last_dy, s_last_dw, s_last_dh;
/* After savestate restore: keep swapping for a few presents even when the
 * display rect is byte-identical to the last swap. Double/triple-buffered
 * windows otherwise can leave a stale back buffer on screen while vblanks
 * (and FPS) keep advancing — especially on a 2nd+ load of the same slot. */
static int s_force_present_remaining = 0;

static void present_dirty_rect(int x0, int y0, int x1, int y1, int set) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 >= VRAM_W) x1 = VRAM_W - 1; if (y1 >= VRAM_H) y1 = VRAM_H - 1;
    if (x0 > x1 || y0 > y1) return;
    int tx0 = x0 / PRES_TILE, tx1 = x1 / PRES_TILE;
    uint64_t mask = (~0ull << tx0) & (~0ull >> (63 - tx1));
    for (int ty = y0 / PRES_TILE; ty <= y1 / PRES_TILE; ty++) {
        if (set) s_present_dirty[ty] |= mask; else s_present_dirty[ty] &= ~mask;
    }
}

static int present_dirty_test(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 >= VRAM_W) x1 = VRAM_W - 1; if (y1 >= VRAM_H) y1 = VRAM_H - 1;
    if (x0 > x1 || y0 > y1) return 0;
    int tx0 = x0 / PRES_TILE, tx1 = x1 / PRES_TILE;
    uint64_t mask = (~0ull << tx0) & (~0ull >> (63 - tx1));
    for (int ty = y0 / PRES_TILE; ty <= y1 / PRES_TILE; ty++)
        if (s_present_dirty[ty] & mask) return 1;
    return 0;
}

static void present_force_consumed(void) {
    if (s_force_present_remaining > 0)
        s_force_present_remaining--;
}

static void coh_record(int kind, int x0, int y0, int x1, int y1) {
    GlCohEvent *e = &s_coh_ring[s_coh_seq % GL_COH_RING_CAP];
    e->frame = (uint32_t)s_frame_count;
    e->kind  = (uint8_t)kind;
    e->x0 = (int16_t)x0; e->y0 = (int16_t)y0;
    e->x1 = (int16_t)x1; e->y1 = (int16_t)y1;
    s_coh_seq++;
    if (kind == GL_COH_FLUSH || kind == GL_COH_FILL ||
        kind == GL_COH_COPY || kind == GL_COH_DRAW)
        present_dirty_rect(x0, y0, x1, y1, 1);
}

uint64_t gl_renderer_coh_total(void) { return s_coh_seq; }
int gl_renderer_coh_get(uint64_t seq, GlCohEvent *out) {
    if (seq >= s_coh_seq) return 0;
    if (s_coh_seq - seq > GL_COH_RING_CAP) return 0;  /* evicted */
    *out = s_coh_ring[seq % GL_COH_RING_CAP];
    return 1;
}

/* ---- present ring (always-on, debug server "present_ring") --------------- */
/* Records every SwapWindow (vram/wide/cpu/blank/interpolated) and its
 * source + letterbox rects. PSX_GL_PRESENT_PROBE=1 additionally drains
 * glGetError and samples one backbuffer pixel before the swap; that synchronous
 * diagnostic is intentionally opt-in. Observers query a window after the fact. */
#define GL_PRES_RING_CAP 4096
static GlPresEvent s_pres_ring[GL_PRES_RING_CAP];
static uint64_t    s_pres_seq = 0;

static void pres_record(int path, int dx, int dy, int w, int h,
                        int lx, int ly, int lw, int lh) {
    /* The ring metadata stays always-on, but pixel probing must not: each
     * glReadPixels synchronously drains queued GPU work. Two probes per frame
     * were enough to make Tomba 2 miss its frame budget. */
    static int probe_pixels = -1;
    if (probe_pixels < 0) {
        const char *cfg = getenv("PSX_GL_PRESENT_PROBE");
        probe_pixels = (cfg && cfg[0] == '1') ? 1 : 0;
    }
    GlPresEvent *e = &s_pres_ring[s_pres_seq % GL_PRES_RING_CAP];
    e->frame = (uint32_t)s_frame_count;
    e->t_ms  = (uint32_t)SDL_GetTicks();
    e->path  = (uint8_t)path;
    e->glerr = probe_pixels ? (uint16_t)glGetError() : 0;
    e->dx = (int16_t)dx; e->dy = (int16_t)dy;
    e->w  = (int16_t)w;  e->h  = (int16_t)h;
    e->lx = (int16_t)lx; e->ly = (int16_t)ly;
    e->lw = (int16_t)lw; e->lh = (int16_t)lh;
    /* Backbuffer sample at the letterbox centre (GL bottom-origin; the rects
     * we pass in are already bottom-origin GL window coords). */
    uint8_t px[3] = { 0, 0, 0 };
    if (probe_pixels && lw > 0 && lh > 0) {
        glReadBuffer(GL_BACK);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(lx + lw / 2, ly + lh / 2, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, px);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
    }
    e->px_r = px[0]; e->px_g = px[1]; e->px_b = px[2];
    /* Blit-source sample: the hr FBO pixel at the display-rect centre. Splits
     * "FBO content was black" from "the blit malfunctioned". */
    uint8_t sp[3] = { 0, 0, 0 };
    e->src_valid = 0;
    if (probe_pixels && (path == GL_PRES_VRAM) && w > 0 && h > 0 && s_hr_fbo) {
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_hr_fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels((dx + w / 2) * s_scale, (dy + h / 2) * s_scale,
                     1, 1, GL_RGB, GL_UNSIGNED_BYTE, sp);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
        e->src_valid = 1;
    }
    e->src_r = sp[0]; e->src_g = sp[1]; e->src_b = sp[2];
    s_pres_seq++;
}

uint64_t gl_renderer_pres_total(void) { return s_pres_seq; }
int gl_renderer_pres_get(uint64_t seq, GlPresEvent *out) {
    if (seq >= s_pres_seq) return 0;
    if (s_pres_seq - seq > GL_PRES_RING_CAP) return 0;  /* evicted */
    *out = s_pres_ring[seq % GL_PRES_RING_CAP];
    return 1;
}

/* ---- shaders ------------------------------------------------------------ */
static const char *PRESENT_VS =
    "#version 330\n"
    "out vec2 v_uv;\n"
    "uniform vec4 u_uv_rect;\n"
    "void main(){ vec2 p = vec2((gl_VertexID<<1)&2, gl_VertexID&2);\n"
    "  v_uv = vec2(mix(u_uv_rect.x,u_uv_rect.z,p.x),\n"
    "              mix(u_uv_rect.y,u_uv_rect.w,1.0-p.y));\n"
    "  gl_Position = vec4(p*2.0-1.0,0.0,1.0); }\n";
static const char *PRESENT_FS =
    "#version 330\n"
    "in vec2 v_uv; uniform sampler2D u_tex; out vec4 frag;\n"
    "void main(){ frag = texture(u_tex, v_uv); }\n";
static const char *INTERP_FS =
    "#version 330\n"
    "in vec2 v_uv; uniform sampler2D u_prev; uniform sampler2D u_curr;\n"
    "uniform float u_alpha; out vec4 frag;\n"
    "void main(){ frag=mix(texture(u_prev,v_uv),texture(u_curr,v_uv),u_alpha); }\n";

/* Geometry: position in VRAM pixels (draw offset already applied by gpu.c),
 * color rgb in 0..1, color a = mask bit (0/1). The clip transform is in
 * native VRAM space; the viewport at S* the size scales rasterization.
 *
 * ALL drawn prims shift positions by u_shift = half an HR pixel (0.5/S in
 * native units): GL samples coverage/attributes at pixel CENTERS, the PS1
 * DDA at INTEGER coords. The shift aligns GL's sample grid with the PS1
 * grid — without it, any texture mapping with slope != 1 (scaled sprites,
 * squished menu fonts) samples one texel off per row/column (striped
 * glyphs, seam lines). Half an HR pixel (not half a native pixel!) keeps
 * rect coverage exactly [x*S, (x+w)*S) at every scale AND makes the
 * top-left subpixel of each S*S block sample the exact PS1 value (which is
 * what the PACK pass reads back). */
static const char *GEO_VS =
    "#version 330\n"
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec4 a_col;\n"
    "uniform float u_shift;\n"
    "uniform float u_xoff;   /* native-wide x translation (px); 0 canonical */\n"
    "uniform float u_xhalf;  /* x clip half-extent (px); 512 canonical */\n"
    "uniform float u_xscale; /* native-wide 2D-backdrop x-stretch; 1 canonical */\n"
    "uniform float u_xcenter;/* stretch centre in VRAM px; 0 canonical */\n"
    "noperspective out vec4 v_col;\n"
    "void main(){ v_col = a_col;\n"
    "  float xb = a_pos.x;\n"
    "  if (u_xscale < 0.0) {\n"
    "    float s = -u_xscale; float h = u_xhalf / s;\n"
    "    float l = u_xcenter - h, r = u_xcenter + h;\n"
    "    if (xb < l) xb = l + (xb-l)*s; else if (xb > r) xb = r + (xb-r)*s;\n"
    "  } else xb = (xb - u_xcenter)*u_xscale + u_xcenter;\n"
    "  gl_Position = vec4((xb+u_shift+u_xoff)/u_xhalf - 1.0, (a_pos.y+u_shift)/256.0 - 1.0, 0.0, 1.0); }\n";
static const char *GEO_FS =
    "#version 330\n"
    "noperspective in vec4 v_col; out vec4 frag;\n"
    "void main(){ frag = v_col; }\n";

/* Textured prims: sample raw 1555 VRAM (integer), CLUT decode per depth,
 * texture window, optional bilinear, texel-0 discard, STP-split discard,
 * PS1 *2-around-0x80 modulation. Output alpha = bit15 of the written pixel.
 * Texel coords use floor() to match the software rasterizer's truncation
 * (rounding shifted sampling +1 texel half the time: smeared text). */
/* Textured program. Per-prim texture state (texpage, clut, depth, raw, uv
 * limits) is carried in FLAT vertex attributes — constant across a prim's
 * vertices — instead of uniforms, so consecutive textured prims with the same
 * blend/mask/texture-window state batch into one draw (see flush_tex_batch).
 * The remaining uniforms (u_twin/u_maskset/u_filter/u_semipass) are the batch
 * keys + per-pass state. */
static const char *TEX_VS =
    "#version 330\n"
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec2 a_uv;\n"
    "layout(location=2) in vec4 a_col;\n"
    "layout(location=3) in vec2 a_tpage;\n"
    "layout(location=4) in vec2 a_clut;\n"
    "layout(location=5) in float a_depth;\n"
    "layout(location=6) in float a_raw;\n"
    "layout(location=7) in vec4 a_limits;\n"
    "layout(location=8) in float a_semi;\n"
    "uniform float u_shift;\n"
    "uniform float u_xoff;   /* native-wide x translation (px); 0 canonical */\n"
    "uniform float u_xhalf;  /* x clip half-extent (px); 512 canonical */\n"
    "uniform float u_xscale; /* native-wide 2D-backdrop x-stretch; 1 canonical */\n"
    "uniform float u_xcenter;/* stretch centre in VRAM px; 0 canonical */\n"
    "noperspective out vec2 v_uv; noperspective out vec4 v_col;\n"
    "flat out ivec2 v_tpage; flat out ivec2 v_clut; flat out int v_depth;\n"
    "flat out int v_raw; flat out ivec4 v_limits; flat out int v_semi;\n"
    "void main(){ v_uv = a_uv; v_col = a_col;\n"
    "  v_tpage = ivec2(a_tpage + 0.5); v_clut = ivec2(a_clut + 0.5);\n"
    "  v_depth = int(a_depth + 0.5); v_raw = int(a_raw + 0.5);\n"
    "  v_semi = int(a_semi + 0.5);\n"
    "  v_limits = ivec4(floor(a_limits + 0.5));\n"
    "  /* u_shift: align GL's center-sample grid with the PS1 integer grid (see\n"
    "   * GEO_VS) so interpolated uv at a fragment equals the PS1 DDA value. */\n"
    "  float xb = a_pos.x;\n"
    "  if (u_xscale < 0.0) {\n"
    "    float s = -u_xscale; float h = u_xhalf / s;\n"
    "    float l = u_xcenter - h, r = u_xcenter + h;\n"
    "    if (xb < l) xb = l + (xb-l)*s; else if (xb > r) xb = r + (xb-r)*s;\n"
    "  } else xb = (xb - u_xcenter)*u_xscale + u_xcenter;\n"
    "  gl_Position = vec4((xb+u_shift+u_xoff)/u_xhalf - 1.0, (a_pos.y+u_shift)/256.0 - 1.0, 0.0, 1.0); }\n";
static const char *TEX_FS =
    "#version 330\n"
    "noperspective in vec2 v_uv; noperspective in vec4 v_col;\n"
    "out vec4 frag; out vec4 blend_factor;\n"
    "flat in ivec2 v_tpage;   /* texture page base, VRAM px */\n"
    "flat in ivec2 v_clut;    /* CLUT base, VRAM px */\n"
    "flat in int v_depth;     /* 0=4bit 1=8bit 2=15bit */\n"
    "flat in int v_raw;       /* 1 = no color modulation */\n"
    "flat in ivec4 v_limits;  /* prim uv sampling bounds (inclusive, post-wrap) */\n"
    "flat in int v_semi;      /* GP0 command has semi-transparency enabled */\n"
    "uniform usampler2D u_vram;\n"
    "uniform int u_semipass;  /* 0=all texels, 1=STP=0 only, 2=STP=1 only */\n"
    "uniform int u_semimode;  /* PS1 blend mode; drives dual-source factors */\n"
    "uniform ivec4 u_twin;    /* texture window: mask_x, mask_y, off_x, off_y */\n"
    "uniform int u_maskset;   /* GP0(E6h) set-mask: OR bit15 into output */\n"
    "uniform int u_filter;    /* 1 = bilinear */\n"
    "int vram_at(int x, int y){\n"
    "  return int(texelFetch(u_vram, ivec2(x & 1023, y & 511), 0).r);\n"
    "}\n"
    "int fetch_texel(int u, int v){\n"
    "  u &= 255; v &= 255;\n"
    "  if ((u_twin.x | u_twin.y) != 0) {\n"
    "    u = (u & ~(u_twin.x * 8)) | ((u_twin.z & u_twin.x) * 8);\n"
    "    v = (v & ~(u_twin.y * 8)) | ((u_twin.w & u_twin.y) * 8);\n"
    "  } else {\n"
    "    u = clamp(u, v_limits.x, v_limits.z);\n"
    "    v = clamp(v, v_limits.y, v_limits.w);\n"
    "  }\n"
    "  if (v_depth == 0) {\n"
    "    int px = vram_at(v_tpage.x + (u >> 2), v_tpage.y + v);\n"
    "    return vram_at(v_clut.x + ((px >> ((u & 3) * 4)) & 0xF), v_clut.y);\n"
    "  } else if (v_depth == 1) {\n"
    "    int px = vram_at(v_tpage.x + (u >> 1), v_tpage.y + v);\n"
    "    return vram_at(v_clut.x + ((px >> ((u & 1) * 8)) & 0xFF), v_clut.y);\n"
    "  }\n"
    "  return vram_at(v_tpage.x + u, v_tpage.y + v);\n"
    "}\n"
    "vec3 col5(int raw){\n"
    "  return vec3(float(raw & 31), float((raw >> 5) & 31), float((raw >> 10) & 31)) / 31.0;\n"
    "}\n"
    "void main(){\n"
    "  int stp; vec3 rgb;\n"
    "  if (u_filter == 0) {\n"
    "    int raw = fetch_texel(int(floor(v_uv.x)), int(floor(v_uv.y)));\n"
    "    if (raw == 0) discard;\n"
    "    rgb = col5(raw);\n"
    "    stp = (raw >> 15) & 1;\n"
    "  } else {\n"
    "    /* Bilinear, Beetle-PSX formulation: the NEAREST texel is the base\n"
    "     * (cutout + STP authority), the neighbours lie toward the sub-texel\n"
    "     * offset and clamp to u_limits, and each texel's weight is gated by\n"
    "     * its opacity with the colour renormalised — so prim edges and\n"
    "     * cutout borders keep their colour instead of dissolving into the\n"
    "     * transparent (black) neighbour and discarding whole edge columns. */\n"
    "    int iu = int(floor(v_uv.x)), iv = int(floor(v_uv.y));\n"
    "    float fx = v_uv.x - float(iu) - 0.5, fy = v_uv.y - float(iv) - 0.5;\n"
    "    int sx = fx < 0.0 ? -1 : 1, sy = fy < 0.0 ? -1 : 1;\n"
    "    fx = abs(fx); fy = abs(fy);\n"
    "    int c00 = fetch_texel(iu, iv);\n"
    "    int c10 = fetch_texel(iu + sx, iv);\n"
    "    int c01 = fetch_texel(iu, iv + sy);\n"
    "    int c11 = fetch_texel(iu + sx, iv + sy);\n"
    "    float w00 = (c00 == 0 ? 0.0 : 1.0) * (1.0 - fx) * (1.0 - fy);\n"
    "    float w10 = (c10 == 0 ? 0.0 : 1.0) * fx * (1.0 - fy);\n"
    "    float w01 = (c01 == 0 ? 0.0 : 1.0) * (1.0 - fx) * fy;\n"
    "    float w11 = (c11 == 0 ? 0.0 : 1.0) * fx * fy;\n"
    "    float opac = w00 + w10 + w01 + w11;\n"
    "    if (opac < 0.5) discard;\n"
    "    rgb = (col5(c00)*w00 + col5(c10)*w10 + col5(c01)*w01 + col5(c11)*w11) / opac;\n"
    "    float stpf = (float((c00 >> 15) & 1) * w00 + float((c10 >> 15) & 1) * w10\n"
    "                + float((c01 >> 15) & 1) * w01 + float((c11 >> 15) & 1) * w11) / opac;\n"
    "    stp = stpf >= 0.5 ? 1 : 0;\n"
    "  }\n"
    "  if (u_semipass == 1 && stp == 1) discard;\n"
    "  if (u_semipass == 2 && stp == 0) discard;\n"
    "  if (v_raw == 0) rgb = clamp(rgb * v_col.rgb * 2.0, 0.0, 1.0);\n"
    "  float dst_factor = 0.0;\n"
    "  if (u_semimode == 4 && v_semi != 0 && stp != 0) {\n"
    "    dst_factor = v_semi == 1 ? 0.5 : 1.0;\n"
    "    if (v_semi == 1) rgb *= 0.5; else if (v_semi == 4) rgb *= 0.25;\n"
    "  }\n"
    "  frag = vec4(rgb, (stp == 1 || u_maskset == 1) ? 1.0 : 0.0);\n"
    "  blend_factor = vec4(0.0, 0.0, 0.0, dst_factor);\n"
    "}\n";

/* Quad blit: used for CPU->VRAM upload flushes and VRAM->VRAM copies.
 * Samples an RGBA8 source (alpha = bit15), splits by STP for the stencil
 * write, optionally ORs set-mask into the output alpha. */
static const char *BLIT_VS =
    "#version 330\n"
    "layout(location=0) in vec2 a_pos;   /* native VRAM px */\n"
    "uniform float u_shift;\n"
    "void main(){\n"
    "  gl_Position = vec4((a_pos.x+u_shift)/512.0 - 1.0, (a_pos.y+u_shift)/256.0 - 1.0, 0.0, 1.0); }\n";
static const char *BLIT_FS =
    "#version 330\n"
    "out vec4 frag;\n"
    "uniform sampler2D u_src;\n"
    "uniform int u_stp_pass;  /* 0=all, 1=bit15=0 only, 2=bit15=1 only */\n"
    "uniform int u_maskset;\n"
    "uniform int u_src_div;   /* fragcoord -> src texel divisor (S for native-res\n"
    "                            sources, 1 for hr-res sources) */\n"
    "uniform ivec2 u_src_off; /* added after the divide, in src texel units */\n"
    "void main(){\n"
    "  /* Exact integer source fetch — no normalized-uv edge precision. */\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  vec4 c = texelFetch(u_src, p / u_src_div + u_src_off, 0);\n"
    "  bool stp = c.a >= 0.5;\n"
    "  if (u_stp_pass == 1 && stp) discard;\n"
    "  if (u_stp_pass == 2 && !stp) discard;\n"
    "  frag = vec4(c.rgb, (stp || u_maskset != 0) ? 1.0 : 0.0);\n"
    "}\n";

/* Pack: re-encode the hr FBO into the native R16UI raw mirror. Runs over a
 * native-res viewport with scissor = dirty rect; each native pixel takes the
 * top-left sample of its S*S block (the sample at the exact native coord). */
static const char *PACK_VS =
    "#version 330\n"
    "void main(){ vec2 p = vec2((gl_VertexID<<1)&2, gl_VertexID&2);\n"
    "  gl_Position = vec4(p*2.0-1.0, 0.0, 1.0); }\n";
static const char *PACK_FS =
    "#version 330\n"
    "uniform sampler2D u_hr;\n"
    "uniform int u_scale;\n"
    "out uint o_pix;\n"
    "void main(){\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  vec4 c = texelFetch(u_hr, p * u_scale, 0);\n"
    "  uint r = uint(c.r * 255.0 + 0.5) >> 3;\n"
    "  uint g = uint(c.g * 255.0 + 0.5) >> 3;\n"
    "  uint b = uint(c.b * 255.0 + 0.5) >> 3;\n"
    "  o_pix = r | (g << 5) | (b << 10) | (c.a >= 0.5 ? 0x8000u : 0u);\n"
    "}\n";

/* Rebuild stencil bit 0 from a copied RGBA target's alpha. Color writes are
 * disabled while this shader runs; only alpha>=0.5 fragments replace stencil. */
static const char *STENCIL_FS =
    "#version 330\n"
    "uniform sampler2D u_src; out vec4 frag;\n"
    "void main(){ vec4 c=texelFetch(u_src,ivec2(gl_FragCoord.xy),0);\n"
    "  if(c.a<0.5) discard; frag=vec4(0.0); }\n";

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = p_glCreateShader(type);
    p_glShaderSource(s, 1, &src, NULL);
    p_glCompileShader(s);
    GLint ok = 0; p_glGetShaderiv(s, PSXGL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; log[0]=0; p_glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stdout, "psxrecomp: GL shader compile failed: %s\n", log);
        p_glDeleteShader(s); return 0; }
    return s;
}
static GLuint build_program_ex(const char *vs, const char *fs, int dual_source) {
    GLuint v = compile_shader(PSXGL_VERTEX_SHADER, vs), f = compile_shader(PSXGL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint p = p_glCreateProgram();
    p_glAttachShader(p, v); p_glAttachShader(p, f);
    if (dual_source) {
        p_glBindFragDataLocationIndexed(p, 0, 0, "frag");
        p_glBindFragDataLocationIndexed(p, 0, 1, "blend_factor");
    }
    p_glLinkProgram(p);
    p_glDeleteShader(v); p_glDeleteShader(f);
    GLint ok = 0; p_glGetProgramiv(p, PSXGL_LINK_STATUS, &ok);
    if (!ok) { char log[1024]; log[0]=0; p_glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stdout, "psxrecomp: GL program link failed: %s\n", log); return 0; }
    return p;
}
static GLuint build_program(const char *vs, const char *fs) {
    return build_program_ex(vs, fs, 0);
}

/* ---- pixel conversion (PS1 1555: bit15=mask, B[14:10] G[9:5] R[4:0]) ---- */
static inline uint32_t conv_1555_to_rgba8(uint16_t p) {
    uint32_t r = (p & 0x1F) << 3, g = ((p >> 5) & 0x1F) << 3, b = ((p >> 10) & 0x1F) << 3;
    uint32_t a = (p >> 15) & 1 ? 0xFF : 0;
    return r | (g << 8) | (b << 16) | (a << 24);   /* RGBA8 little-endian */
}

/* ---- mask-bit stencil --------------------------------------------------- *
 * Stencil bit0 mirrors bit15 of every pixel.
 *   check off: test ALWAYS, op REPLACE with ref = write value.
 *   check on:  test EQUAL 0 (pass iff dest unmasked). GL couples the REPLACE
 *              value to the test reference, so write a 1 via INVERT (the
 *              stored value is known to be 0 when the test passed) and a 0
 *              via KEEP. */
static void mask_stencil(int write_val) {
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0x01);
    if (s_mask_check) {
        glStencilFunc(GL_EQUAL, 0, 0x01);
        glStencilOp(GL_KEEP, GL_KEEP, write_val ? GL_INVERT : GL_KEEP);
    } else {
        glStencilFunc(GL_ALWAYS, write_val ? 1 : 0, 0x01);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    }
}
/* Like mask_stencil but never checks (uploads: gpu.c already applied mask). */
static void plain_stencil(int write_val) {
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0x01);
    glStencilFunc(GL_ALWAYS, write_val ? 1 : 0, 0x01);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
}

/* PS1 semi-transparency as fixed-function blending, RGB only — the alpha
 * channel (mask bit) is always replaced by the source fragment's alpha.
 *   0: B/2 + F/2   1: B + F   2: B - F   3: B + F/4 */
static void apply_psx_blend(int mode) {
    glEnable(GL_BLEND);
    p_glBlendEquationSeparate((mode & 3) == 2 ? PSXGL_FUNC_REVERSE_SUBTRACT
                                              : PSXGL_FUNC_ADD,
                              PSXGL_FUNC_ADD);
    switch (mode & 3) {
    case 0:
        p_glBlendColor(0.5f, 0.5f, 0.5f, 0.5f);
        p_glBlendFuncSeparate(PSXGL_CONSTANT_ALPHA, PSXGL_CONSTANT_ALPHA, GL_ONE, GL_ZERO);
        break;
    case 1:
    case 2:
        p_glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
        break;
    case 3:
        p_glBlendColor(0.25f, 0.25f, 0.25f, 0.25f);
        p_glBlendFuncSeparate(PSXGL_CONSTANT_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
        break;
    }
}

/* ---- hr FBO render-state bracket ---------------------------------------- */
static void hr_begin(int clip_to_draw_area) {
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_hr_fbo);
    glViewport(0, 0, VRAM_W * s_scale, VRAM_H * s_scale);
    glEnable(GL_SCISSOR_TEST);
    if (clip_to_draw_area) {
        int sw = s_area_x2 - s_area_x1 + 1, sh = s_area_y2 - s_area_y1 + 1;
        if (sw < 0) sw = 0; if (sh < 0) sh = 0;
        glScissor(s_area_x1 * s_scale, s_area_y1 * s_scale,
                  sw * s_scale, sh * s_scale);
    }
}
static void hr_end(void) {
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    p_glBindVertexArray(0);
    p_glUseProgram(0);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
}

/* ---- coherency: CPU -> GPU upload flush --------------------------------- */
/* CPU-side VRAM writes (GP0 A0 transfers, DMA, single pixel pokes) land in
 * the CPU array immediately and accumulate s_up_rects. Flushing before the
 * next GPU op (or readback/present) preserves PS1 command order. */
static void flush_cpu_upload(void) {
    if (!s_raster_ok || s_up_nrects == 0) return;
    const int diag = runtime_upload_diag_enabled();
    if (diag) { s_rt_up_diag[0]++; s_rt_up_diag[1] += (uint64_t)s_up_nrects; }
    flush_flat_batch();  /* queued flat GEO before upload mutates VRAM */
    flush_tex_batch();   /* queued textured draws before this upload writes VRAM */
    /* Snapshot + clear first (re-entrancy safe; up_add overflow calls back in). */
    DirtyRect rects[UP_RECTS_MAX];
    int nrects = s_up_nrects;
    memcpy(rects, s_up_rects, (size_t)nrects * sizeof(DirtyRect));
    s_up_nrects = 0;

    /* Stage every rect's CPU data first (texture uploads outside the FBO
     * bracket), then draw all the quads in one bracket. Only the exact
     * uploaded rects are painted — never the union bounding box (stale-CPU
     * flicker class bug, see s_up_rects). */
    for (int i = 0; i < nrects; i++) {
        int x = rects[i].x0, y = rects[i].y0;
        int w = rects[i].x1 - rects[i].x0 + 1;
        int h = rects[i].y1 - rects[i].y0 + 1;
        if (diag) s_rt_up_diag[2] += (uint64_t)w * (uint64_t)h;
        coh_record(GL_COH_FLUSH, x, y, x + w - 1, y + h - 1);

        /* RGBA8 staging for the hr quad draw. */
        uint64_t t0 = diag ? SDL_GetPerformanceCounter() : 0;
        for (int row = 0; row < h; row++) {
            const uint16_t *src = s_vram + (size_t)(y + row) * VRAM_W + x;
            uint32_t *dst = s_conv + (size_t)row * w;
            for (int col = 0; col < w; col++) dst[col] = conv_1555_to_rgba8(src[col]);
        }
        if (diag) s_rt_up_diag[3] += SDL_GetPerformanceCounter() - t0;
        t0 = diag ? SDL_GetPerformanceCounter() : 0;
        glBindTexture(GL_TEXTURE_2D, s_up_tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, s_conv);

        /* Raw mirror takes the CPU data directly — current for this rect, so no
         * pack is needed for uploaded content. */
        glBindTexture(GL_TEXTURE_2D, s_raw_tex);
        glPixelStorei(PSXGL_UNPACK_ROW_LENGTH, VRAM_W);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h,
                        PSXGL_RED_INTEGER, GL_UNSIGNED_SHORT,
                        s_vram + (size_t)y * VRAM_W + x);
        glPixelStorei(PSXGL_UNPACK_ROW_LENGTH, 0);
        if (diag) s_rt_up_diag[4] += SDL_GetPerformanceCounter() - t0;
    }

    /* Quads into the hr FBO; two passes split by bit15 so the stencil mirror
     * stays exact. gpu.c applied mask set/check per pixel already — no check
     * here, the data is final. up_tex is VRAM-aligned: src texel = frag/S. */
    uint64_t draw_t0 = diag ? SDL_GetPerformanceCounter() : 0;
    hr_begin(0);
    glDisable(GL_BLEND);
    p_glUseProgram(s_blit_prog);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_up_tex);
    p_glUniform1i(s_uBlitSrc, 0);
    p_glUniform1i(s_uBlitMaskset, 0);
    p_glUniform1i(s_uBlitSrcDiv, s_scale);
    p_glUniform2i(s_uBlitSrcOff, 0, 0);
    p_glBindVertexArray(s_blit_vao);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_blit_vbo);
    for (int i = 0; i < nrects; i++) {
        int x = rects[i].x0, y = rects[i].y0;
        int w = rects[i].x1 - rects[i].x0 + 1;
        int h = rects[i].y1 - rects[i].y0 + 1;
        glScissor(x * s_scale, y * s_scale, w * s_scale, h * s_scale);
        float fx0 = (float)x, fy0 = (float)y, fx1 = (float)(x + w), fy1 = (float)(y + h);
        float verts[6 * 2] = {
            fx0, fy0,  fx1, fy0,  fx0, fy1,
            fx1, fy0,  fx0, fy1,  fx1, fy1,
        };
        p_glBufferData(PSXGL_ARRAY_BUFFER, sizeof verts, verts, PSXGL_STREAM_DRAW);
        plain_stencil(0); p_glUniform1i(s_uBlitPass, 1); glDrawArrays(GL_TRIANGLES, 0, 6);
        plain_stencil(1); p_glUniform1i(s_uBlitPass, 2); glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    hr_end();
    if (diag) s_rt_up_diag[5] += SDL_GetPerformanceCounter() - draw_t0;
}

/* Recreate one target's stencil mask from its authoritative alpha channel.
 * Sampling an attached render target is undefined, so copy color to the shared
 * scratch texture first. Wide targets never exceed the 1024-pixel VRAM width. */
static void rebuild_target_stencil(GLuint target_fbo, int target_w, int target_h) {
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, target_fbo);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, s_scratch_fbo);
    p_glBlitFramebuffer(0, 0, target_w, target_h, 0, 0, target_w, target_h,
                        GL_COLOR_BUFFER_BIT, GL_NEAREST);

    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, target_fbo);
    glViewport(0, 0, target_w, target_h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0x01);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glStencilFunc(GL_ALWAYS, 1, 0x01);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    p_glUseProgram(s_stencil_prog);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_scratch_tex);
    p_glUniform1i(s_uStencilSrc, 0);
    p_glBindVertexArray(s_empty_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_STENCIL_TEST);
    p_glBindVertexArray(0);
    p_glUseProgram(0);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
}

static void rebuild_mask_stencils(void) {
    if (s_stencil_valid || !s_raster_ok) return;
    int hw = VRAM_W * s_scale, hh = VRAM_H * s_scale;
    rebuild_target_stencil(s_hr_fbo, hw, hh);
    for (int i = 0; i < WIDE_MAX_SURF; i++) {
        if (s_wide_fbo[i])
            rebuild_target_stencil(s_wide_fbo[i], g_wide_w * s_scale, hh);
    }
    s_stencil_valid = 1;
}

/* ---- coherency: hr FBO -> raw mirror (pack) ------------------------------ */
static void pack_flush(void) {
    if (!s_raster_ok || !s_pack_dirty.set) return;
    int x = s_pack_dirty.x0, y = s_pack_dirty.y0;
    int w = s_pack_dirty.x1 - s_pack_dirty.x0 + 1;
    int h = s_pack_dirty.y1 - s_pack_dirty.y0 + 1;
    rect_clear(&s_pack_dirty);
    coh_record(GL_COH_PACK, x, y, x + w - 1, y + h - 1);

    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_raw_fbo);
    glViewport(0, 0, VRAM_W, VRAM_H);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, w, h);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    p_glUseProgram(s_pack_prog);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_hr_tex);
    p_glUniform1i(s_uPackHr, 0);
    p_glUniform1i(s_uPackScale, s_scale);
    p_glBindVertexArray(s_empty_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    p_glBindVertexArray(0);
    p_glUseProgram(0);
    glDisable(GL_SCISSOR_TEST);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
}

/* Make sure the raw mirror is current for a textured draw that samples the
 * given texture page / CLUT. */
static void flush_pack_if_sampling(int tpage_x, int tpage_y, int depth,
                                   int clut_x, int clut_y) {
    if (!s_pack_dirty.set) return;
    int page_w = depth == 0 ? 64 : depth == 1 ? 128 : 256;  /* VRAM columns */
    if (rect_intersects(&s_pack_dirty, tpage_x, tpage_y,
                        tpage_x + page_w - 1, tpage_y + 255)) {
        flush_flat_batch();
        flush_tex_batch();   /* queued draws are part of s_pack_dirty — realise them before packing */
        pack_flush(); return;
    }
    if (depth <= 1) {
        int n = depth == 0 ? 16 : 256;
        if (rect_intersects(&s_pack_dirty, clut_x, clut_y, clut_x + n - 1, clut_y)) {
            flush_flat_batch();
            flush_tex_batch();
            pack_flush();
        }
    }
}

/* ---- coherency: GPU -> CPU readback -------------------------------------- */
static void ensure_cpu(void) {
    if (!s_raster_ok || !s_gpu_dirty) return;
    flush_flat_batch();
    flush_tex_batch();   /* realise queued textured draws before reading the FBO back */
    flush_cpu_upload();
    pack_flush();
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_raw_fbo);
    glReadPixels(0, 0, VRAM_W, VRAM_H, PSXGL_RED_INTEGER, GL_UNSIGNED_SHORT, s_vram);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    s_gpu_dirty = 0;
    coh_record(GL_COH_ENSURE, 0, 0, VRAM_W - 1, VRAM_H - 1);
}

/* ---- GPU primitives ------------------------------------------------------ */

static uint64_t s_scene_prims = 0;     /* frame_perf: scene primitives submitted (pre double-draw) */
static uint64_t s_scene_prims_tex = 0; /* frame_perf: of which textured (vs flat geometry)         */
static void flush_tex_batch(void);     /* fwd: drained at the backdrop-phase boundary below */
static void mark_prim_dirty(const int *xs, const int *ys, int n, int textured) {
    s_scene_prims++;
    int x0 = xs[0], x1 = xs[0], y0 = ys[0], y1 = ys[0];
    for (int i = 1; i < n; i++) {
        if (xs[i] < x0) x0 = xs[i]; if (xs[i] > x1) x1 = xs[i];
        if (ys[i] < y0) y0 = ys[i]; if (ys[i] > y1) y1 = ys[i];
    }
    s_bdg_prims++;   /* dbg: prims seen this frame (gate is now per-prim, see bd_prim_gate) */
    if (s_ptrace_n < PTRACE_CAP) {
        PrimRec *p = &s_ptrace[s_ptrace_n++];
        p->x0 = (short)x0; p->x1 = (short)x1; p->y0 = (short)y0; p->y1 = (short)y1;
        p->tex = (unsigned char)textured;
    }
    if (x0 < s_area_x1) x0 = s_area_x1;
    if (y0 < s_area_y1) y0 = s_area_y1;
    if (x1 > s_area_x2) x1 = s_area_x2;
    if (y1 > s_area_y2) y1 = s_area_y2;
    rect_add(&s_pack_dirty, x0, y0, x1, y1);
    s_gpu_dirty = 1;
    coh_record(GL_COH_DRAW, x0, y0, x1, y1);
}

/* ---- native-wide mirror pass plumbing ----------------------------------- *
 * Re-issue an already-set-up draw (program/VAO/VBO/blend/stencil bound) into
 * the active wide surface. The geometry positions are identical on the host
 * side; the x translation (wide_dx) and the wider clip are applied entirely in
 * the vertex shader via u_xoff / u_xhalf, so the SAME glDrawArrays produces the
 * shifted copy. wide_target_begin binds the wide FBO + viewport + scissor and
 * sets the projection uniforms; wide_target_end restores u_xoff=0/u_xhalf=512
 * on the active program so the next canonical pass is bit-identical. The
 * caller stays inside hr_begin/hr_end; hr_end unbinds and the next hr_begin
 * resets the viewport, so no viewport restore is needed mid-function.
 *
 * The scissor X is the FULL wide surface (NOT the draw area): the SW reference
 * (rt_wide()) deliberately lets the shifted geometry fill the revealed 16:9
 * margins that lie OUTSIDE the game's 4:3 draw-area x-range; scissoring X to
 * the (translated) draw area would crop exactly the margin content native-wide
 * exists to reveal. The scissor Y stays clamped to the DRAW AREA, exactly like
 * rt_wide() (t.cy1/cy2 = g_clip_y1/y2): native-wide only widens X. A full-height
 * Y scissor let draws that canonically clip at a vertical double-buffer band
 * boundary (MMX6: draw area alternates y=0/y=240, both bands in ONE wide
 * surface) bleed into the OTHER band's rows — presented one frame later as
 * top/bottom edge flicker (16:9 GL only). */
static void wide_target_begin(int dx, GLint uXoff, GLint uXhalf) {
    if (s_ws_ablate != 3)   /* ablate 3: no FBO rebind (draws land in hr — perf probe) */
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER, g_wide_cur);
    glViewport(0, 0, g_wide_w * s_scale, VRAM_H * s_scale);
    glEnable(GL_SCISSOR_TEST);
    {
        int sy = s_area_y1, sh = s_area_y2 - s_area_y1 + 1;
        if (sy < 0) { sh += sy; sy = 0; }
        if (sy + sh > VRAM_H) sh = VRAM_H - sy;
        if (sh < 0) sh = 0;
        glScissor(0, sy * s_scale, g_wide_w * s_scale, sh * s_scale);
    }
    p_glUniform1f(uXoff, (float)dx);
    p_glUniform1f(uXhalf, (float)g_wide_w / 2.0f);
}
static void wide_target_end(GLint uXoff, GLint uXhalf) {
    p_glUniform1f(uXoff, 0.0f);
    p_glUniform1f(uXhalf, 512.0f);
    if (s_ws_ablate != 3)
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_hr_fbo);
}

extern int psx_ws_prim_is_tagged(void);   /* gpu.c: is the current GP0 prim sprite-tagged? */
extern int psx_ws_prim_in_backdrop(void); /* gpu.c: is its source addr in the flower-field struct? */
extern int gpu_ws_nw_flat_backdrop_enabled(void); /* gpu.c: per-title flat backdrop opt-in */

/* Per-prim gate: stretch this prim iff native-wide + feature on AND the prim's
 * source address is inside the flower-field backdrop data structure (precise —
 * excludes the 3D rock/foreground, which is untagged AND has narrow prims so the
 * earlier tag/narrow heuristic tore it). mode!=0 falls back to the old
 * tag+narrow heuristic for A/B. */
static int bd_prim_gate(const int *xs, int n, int textured) {
    if (g_wide_w <= 0 || !g_ws_bd_stretch_on) return 0;
    /* Some games draw their authored 4:3 sky/water as flat-colour polygons.
     * Stretch those only in the native-wide mirror: the canonical framebuffer
     * remains byte-for-byte 4:3, while the flat backdrop reaches the reveal
     * margins. Opt-in because flat foreground geometry is title-dependent. */
    if (!textured && gpu_ws_nw_flat_backdrop_enabled()) return 1;
    if (g_ws_bd_phase_mode != 0) return psx_ws_prim_in_backdrop();  /* default: precise address gate */
    /* mode 0: legacy tag+narrow heuristic (kept for comparison) */
    int native_w = g_wide_w - 2 * g_wide_off;
    if (native_w <= 0) return 0;
    if (psx_ws_prim_is_tagged()) return 0;
    int base = g_wide_cur_base, lo = xs[0], hi = xs[0];
    for (int i = 1; i < n; i++) { if (xs[i] < lo) lo = xs[i]; if (xs[i] > hi) hi = xs[i]; }
    if (lo < base - g_ws_bd_phase_thresh) return 0;             /* into left margin -> GTE-wide */
    if (hi > base + native_w + g_ws_bd_phase_thresh) return 0;  /* into right margin -> GTE-wide */
    return 1;
}

/* ---- native-wide FAST path (skip redundant center mirror) ----------------- *
 * The wide surface's CENTRE columns [g_wide_off, g_wide_off+native_w) are, by
 * construction, identical to the canonical 4:3 framebuffer. So instead of
 * re-rasterizing every primitive into the wide surface (the "mirror" pass — the
 * dominant native-wide GPU cost, ~2x scene fill), we copy the canonical centre
 * into the wide surface once at present (blit_wide_center_from_canonical), and
 * the per-prim mirror only needs to produce the reveal MARGINS. Any prim/batch
 * whose x-range is fully inside the 4:3 frame contributes nothing to the margins,
 * so its mirror is skipped entirely. Correctness does not depend on the skip
 * being precise: the centre is authoritatively overwritten by the blit, so the
 * ONLY requirement is that a margin-reaching prim is NOT skipped — hence the
 * conservative strict-inside test. 4:3 never runs any of this (g_wide_cur == 0).
 * Toggle via gl_wide_fast for A/B; default ON. */
static int s_wide_fast = 1;
void gl_renderer_set_wide_fast(int on) { s_wide_fast = on ? 1 : 0; }
int  gl_renderer_get_wide_fast(void) { return s_wide_fast; }
static void wide_blit_center(GLuint wide_fbo, int base_x, int disp_y, int disp_h); /* def below */
/* True if [lo,hi] (canonical draw-x) lies strictly inside the 4:3 frame, so the
 * prim adds nothing to either reveal margin and its mirror can be skipped. */
static int mirror_x_center_only(int lo, int hi) {
    if (!s_wide_fast) return 0;
    int base = g_wide_cur_base, native_w = g_wide_w - 2 * g_wide_off;
    if (native_w <= 0) return 0;
    return (lo >= base) && (hi < base + native_w);
}
static int mirror_geo_center_only(const int *xs, int n) {
    if (!s_wide_fast) return 0;
    int lo = xs[0], hi = xs[0];
    for (int i = 1; i < n; i++) { if (xs[i] < lo) lo = xs[i]; if (xs[i] > hi) hi = xs[i]; }
    return mirror_x_center_only(lo, hi);
}
/* mirror_batch_center_only (textured-batch variant) is defined after s_tb below. */

/* Set / clear the 2D-backdrop x-stretch for a wide-mirror draw, per the current
 * gate (s_bd_gate, set by the caller from bd_prim_gate / the batch gate). */
static void wide_set_bd_scale(GLint uScale, GLint uCenter) {
    extern int g_ws_tex_edge_pct;
    float scale = 1.0f, center = 0.0f;
    if (s_bd_gate && g_ws_bd_stretch_on && g_wide_w > 0) {
        int native_w = g_wide_w - 2 * g_wide_off;
        if (native_w > 0) {
            scale  = g_ws_bd_stretch_pct > 0 ? (float)g_ws_bd_stretch_pct / 100.0f
                                             : (float)g_wide_w / (float)native_w;
            if (s_bd_gate == 2) {
                scale = g_ws_tex_edge_pct > 0
                      ? (float)g_ws_tex_edge_pct / 100.0f : scale;
                scale = -scale; /* shader: expand only beyond canonical edges */
            }
            center = (float)g_wide_cur_base + (float)native_w / 2.0f;
        }
    }
    if (scale != 1.0f) s_bdg_applied++;
    p_glUniform1f(uScale, scale);
    p_glUniform1f(uCenter, center);
}
static void wide_clear_bd_scale(GLint uScale, GLint uCenter) {
    p_glUniform1f(uScale, 1.0f);
    p_glUniform1f(uCenter, 0.0f);
}

/* ---- textured-prim batching -------------------------------------------- *
 * Consecutive textured prims sharing blend/mask/texwindow/filter coalesce into
 * one draw. Per-prim texture state (texpage/clut/depth/raw/uv-limits) rides in
 * the vertex (TEXV flat attributes), so only `semi` (blend) and the global
 * mask/twin/filter are batch keys. flush_tex_batch() draws the queued verts; it
 * is called before any op that reads VRAM, writes it outside the batch, or
 * changes batch state (see callers: flush_cpu_upload, flush_pack_if_sampling,
 * every non-textured glb_ wrapper, and the present path). Drawing reads only the
 * already-coherent texture (per-prim coherency was ensured at append time), so
 * flush_tex_batch never re-enters those helpers. */
#define TEXBATCH_MAXV 8190                 /* multiple of 3; ~2730 tris */
static float s_tb[TEXBATCH_MAXV * TEXV];
static int   s_tb_n = 0;                    /* verts queued */
static int   s_tb_semi = -2;
static int   s_tb_mask = 0, s_tb_filter = 0;
static int   s_tb_twin[4] = {0, 0, 0, 0};
static uint64_t s_batch_total = 0, s_batch_reason[7];

void gl_renderer_batch_diag(uint64_t out[8]) {
    out[0] = s_batch_total;
    for (int i = 0; i < 7; i++) out[i + 1] = s_batch_reason[i];
}

/* Draw the queued textured batch with correct PSX mask-bit handling AND correct
 * painter's order. The mask bit lives in both the colour-attachment alpha
 * (frag.a) and the stencil; STP=1 texels must always set it.
 *
 * OPAQUE batch (semi < 0): the STP bit does NOT gate COLOUR (every texel is
 * opaque), so colour is drawn in ONE ordered pass over all texels — frag.a still
 * carries the per-texel mask bit, so the alpha mask is correct — followed by a
 * COLOUR-MASKED pass that only fixes the STENCIL for STP=1 texels. The old code
 * split colour into STP=0 (pass 1) then STP=1 (pass 2) across the WHOLE batch,
 * which let a behind prim's STP=1 texels overwrite a front prim's STP=0 colour
 * (the Tomba character drew behind an AP-block's letters / a save post on GL
 * only). Same draw count, order preserved, mask preserved.
 *
 * SEMI batch (semi >= 0): genuine two-pass — STP=0 texels opaque, STP=1 texels
 * blended per the PSX mode. Cross-prim order is kept by isolating semi prims to
 * one per batch (see gpu_textured_triangle), so this batch holds a single prim
 * whose two passes do not self-overlap. */
static void tex_batch_draw_passes(int nverts, int semi) {
    p_glUniform1i(s_uSemimode, semi < 0 ? 0 : semi);
    if (semi < 0) {
        glDisable(GL_BLEND);
        mask_stencil(s_tb_mask);
        p_glUniform1i(s_uSemipass, 0);                 /* all texels, one ordered colour pass */
        glDrawArrays(GL_TRIANGLES, 0, nverts);
        if (s_mask_check) {
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);  /* stencil-only fixup */
            mask_stencil(1);
            p_glUniform1i(s_uSemipass, 2);             /* STP=1 texels set the mask bit */
            glDrawArrays(GL_TRIANGLES, 0, nverts);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        } else if (!s_tb_mask) {
            /* Alpha is already exact; defer its duplicate stencil encoding
             * until a later GP0(E6h) actually enables destination masking. */
            s_stencil_valid = 0;
        }
    } else if (!s_mask_check && semi == 4) {
        /* Modes 0/1/3 can select opaque-vs-semi behavior per fragment with
         * dual-source factors, so the whole painter-ordered batch is one draw.
         * Mode 2 needs a different blend equation and stays on the conservative
         * isolated path below. */
        glEnable(GL_BLEND);
        p_glBlendEquationSeparate(PSXGL_FUNC_ADD, PSXGL_FUNC_ADD);
        p_glBlendFuncSeparate(GL_ONE, PSXGL_SRC1_ALPHA, GL_ONE, GL_ZERO);
        if (s_tb_mask) mask_stencil(1); else glDisable(GL_STENCIL_TEST);
        p_glUniform1i(s_uSemipass, 0);
        glDrawArrays(GL_TRIANGLES, 0, nverts);
        if (!s_tb_mask) s_stencil_valid = 0;
    } else {
        glDisable(GL_BLEND);                           /* Pass 1: STP=0 texels (opaque) */
        mask_stencil(s_tb_mask);
        p_glUniform1i(s_uSemipass, 1);
        glDrawArrays(GL_TRIANGLES, 0, nverts);
        apply_psx_blend(semi);                         /* Pass 2: STP=1 texels (blended) */
        mask_stencil(1);
        p_glUniform1i(s_uSemipass, 2);
        glDrawArrays(GL_TRIANGLES, 0, nverts);
    }
}

/* ---- frame_perf CPU attribution (native-wide wedge hunt) ----------------- *
 * Per-frame CPU wall time spent inside the GL submission paths (driver CPU
 * cost surfaces INSIDE our gl* calls) + counters for the wide plumbing, so a
 * CPU-bound wide frame (emu_cpu >> scene_gpu) can be attributed without a
 * sampling profiler. Reset at present enter; reported by frame_perf. */
static double cw_ms(void) {
    static double freq = 0.0;
    if (freq == 0.0) {
        uint64_t f = SDL_GetPerformanceFrequency();
        freq = f ? (double)f : 1.0;
    }
    return (double)SDL_GetPerformanceCounter() * 1000.0 / freq;
}
static double s_cw_flush_ms = 0.0;   /* CPU wall inside flush_tex_batch        */
static double s_cw_wide_ms  = 0.0;   /* CPU wall inside glb_wide_* entry points */
static int    s_cw_batches = 0, s_cw_wide_sets = 0, s_cw_wide_cfgs = 0,
              s_cw_wide_clears = 0, s_cw_fbo_creates = 0, s_cw_flush_depth = 0;

/* Textured-batch variant of mirror_x_center_only: scan the queued verts' x
 * (attr 0, stride TEXV). Defined here so s_tb / TEXV are in scope. */
static int mirror_batch_center_only(int nverts) {
    if (!s_wide_fast || nverts <= 0) return 0;
    int lo = (int)s_tb[0], hi = (int)s_tb[0];
    for (int i = 1; i < nverts; i++) {
        int x = (int)s_tb[i * TEXV];
        if (x < lo) lo = x; if (x > hi) hi = x;
    }
    return mirror_x_center_only(lo, hi);
}

static void flush_tex_batch(void) {
    if (s_tb_n == 0) return;
    int nverts = s_tb_n, semi = s_tb_semi;
    s_tb_n = 0;                             /* clear first: re-entrancy safe */
    double cw_t0 = cw_ms();
    s_cw_batches++; s_batch_total++; s_cw_flush_depth++;

    hr_begin(1);
    p_glUseProgram(s_tex_prog);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_raw_tex);
    p_glUniform1i(s_uVram, 0);
    p_glUniform4i(s_uTwin, s_tb_twin[0], s_tb_twin[1], s_tb_twin[2], s_tb_twin[3]);
    p_glUniform1i(s_uMaskset, s_tb_mask);
    p_glUniform1i(s_uFilter, s_tb_filter);
    p_glBindVertexArray(s_tex_vao);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_tex_vbo);
    p_glBufferData(PSXGL_ARRAY_BUFFER, (ptrdiff_t)(nverts * TEXV * sizeof(float)), s_tb, PSXGL_STREAM_DRAW);

    tex_batch_draw_passes(nverts, semi);

    /* Native-wide mirror — skipped for a batch fully inside the 4:3 frame (its
     * centre content comes from the present-time canonical blit; nothing to add
     * to the margins). A backdrop-stretched batch (s_tb_gate) widens past the
     * frame, so it is never treated as centre-only. */
    if (g_wide_cur && s_ws_ablate != 1 &&
        !(s_tb_gate == 0 && mirror_batch_center_only(nverts))) {   /* native-wide mirror */
        int dx = wide_dx();
        s_bd_gate = s_tb_gate;              /* this batch is uniform-gate (flushed on change) */
        gl_perf_mirror_begin();
        wide_target_begin(dx, s_tex_uXoff, s_tex_uXhalf);
        wide_set_bd_scale(s_tex_uXscale, s_tex_uXcenter);
        if (s_ws_ablate != 2) tex_batch_draw_passes(nverts, semi);
        wide_clear_bd_scale(s_tex_uXscale, s_tex_uXcenter);
        wide_target_end(s_tex_uXoff, s_tex_uXhalf);
        gl_perf_mirror_end();
    }
    hr_end();
    if (--s_cw_flush_depth == 0) s_cw_flush_ms += cw_ms() - cw_t0;
}

/* Flat / gouraud GEO batch — MotK title/char-select starfields issue ~30k/s
 * GP0(68h) 1x1 dots; each was two immediate gpu_triangle draws (BufferData +
 * DrawArrays each). Coalesce opaque/semi-uniform tris into one draw. */
#define FLATBATCH_MAXV 8190                 /* multiple of 3 */
static float s_fb[FLATBATCH_MAXV * 6];
static int   s_fb_n = 0;
static int   s_fb_semi = -2;
static int   s_fb_mask = -1;

static int mirror_flat_batch_center_only(int nverts) {
    if (!s_wide_fast || nverts <= 0) return 0;
    int lo = (int)s_fb[0], hi = (int)s_fb[0];
    for (int i = 1; i < nverts; i++) {
        int x = (int)s_fb[i * 6];
        if (x < lo) lo = x; if (x > hi) hi = x;
    }
    return mirror_x_center_only(lo, hi);
}

static void flush_flat_batch(void) {
    if (s_fb_n == 0) return;
    int nverts = s_fb_n, semi = s_fb_semi, mask = s_fb_mask;
    s_fb_n = 0;

    hr_begin(1);
    if (semi >= 0) apply_psx_blend(semi); else glDisable(GL_BLEND);
    mask_stencil(mask);
    p_glUseProgram(s_geo_prog);
    p_glBindVertexArray(s_geo_vao);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_geo_vbo);
    p_glBufferData(PSXGL_ARRAY_BUFFER, (ptrdiff_t)(nverts * 6 * sizeof(float)),
                   s_fb, PSXGL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, nverts);

    if (g_wide_cur && !s_wide_suppress && s_ws_ablate != 1 &&
        !(!g_ws_bd_stretch_on && mirror_flat_batch_center_only(nverts))) {
        int dx = wide_dx();
        /* Batch may span many prims; use stretch gate off (flat dots/UI). */
        s_bd_gate = 0;
        gl_perf_mirror_begin();
        wide_target_begin(dx, s_geo_uXoff, s_geo_uXhalf);
        wide_set_bd_scale(s_geo_uXscale, s_geo_uXcenter);
        if (s_ws_ablate != 2) glDrawArrays(GL_TRIANGLES, 0, nverts);
        wide_clear_bd_scale(s_geo_uXscale, s_geo_uXcenter);
        wide_target_end(s_geo_uXoff, s_geo_uXhalf);
        gl_perf_mirror_end();
    }
    hr_end();
}

/* Flat / gouraud triangles and lines share the GEO program. mode: GL_TRIANGLES
 * or GL_LINES; verts are (x, y, r, g, b, a) tuples with colors as 1555. */
static void gpu_geometry(GLenum mode, const int *xs, const int *ys,
                         const uint16_t *cs, int n, int semi) {
    flush_tex_batch();   /* flat prim: drain textured draws first (order + program) */
    flush_cpu_upload();  /* also drains flat batch if an upload was pending */
    mark_prim_dirty(xs, ys, n, 0 /* flat */);

    /* Lines stay immediate (rare); tris batch for MotK 0x68 starfields. */
    if (mode != GL_TRIANGLES || n < 3) {
        flush_flat_batch();
        float verts[3 * 6];
        float mask_a = s_mask_set ? 1.0f : 0.0f;
        for (int i = 0; i < n; i++) {
            verts[i*6+0] = (float)xs[i];
            verts[i*6+1] = (float)ys[i];
            verts[i*6+2] = ((cs[i] & 0x1F) << 3) / 255.0f;
            verts[i*6+3] = (((cs[i] >> 5) & 0x1F) << 3) / 255.0f;
            verts[i*6+4] = (((cs[i] >> 10) & 0x1F) << 3) / 255.0f;
            verts[i*6+5] = mask_a;
        }
        hr_begin(1);
        if (semi >= 0) apply_psx_blend(semi); else glDisable(GL_BLEND);
        mask_stencil(s_mask_set);
        if (mode == GL_LINES) glLineWidth((float)s_scale);
        p_glUseProgram(s_geo_prog);
        p_glBindVertexArray(s_geo_vao);
        p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_geo_vbo);
        p_glBufferData(PSXGL_ARRAY_BUFFER, (ptrdiff_t)(n * 6 * sizeof(float)),
                       verts, PSXGL_STREAM_DRAW);
        glDrawArrays(mode, 0, n);
        if (g_wide_cur && !s_wide_suppress && s_ws_ablate != 1 &&
            !(!g_ws_bd_stretch_on && mirror_geo_center_only(xs, n))) {
            int dx = wide_dx();
            s_bd_gate = bd_prim_gate(xs, n, 0);
            gl_perf_mirror_begin();
            wide_target_begin(dx, s_geo_uXoff, s_geo_uXhalf);
            wide_set_bd_scale(s_geo_uXscale, s_geo_uXcenter);
            if (s_ws_ablate != 2) glDrawArrays(mode, 0, n);
            wide_clear_bd_scale(s_geo_uXscale, s_geo_uXcenter);
            wide_target_end(s_geo_uXoff, s_geo_uXhalf);
            gl_perf_mirror_end();
        }
        hr_end();
        return;
    }

    if (s_fb_n > 0 && (s_fb_semi != semi || s_fb_mask != (int)s_mask_set))
        flush_flat_batch();
    if (s_fb_n + n > FLATBATCH_MAXV)
        flush_flat_batch();
    s_fb_semi = semi;
    s_fb_mask = (int)s_mask_set;

    float mask_a = s_mask_set ? 1.0f : 0.0f;
    for (int i = 0; i < n; i++) {
        float *v = &s_fb[s_fb_n * 6];
        v[0] = (float)xs[i];
        v[1] = (float)ys[i];
        v[2] = ((cs[i] & 0x1F) << 3) / 255.0f;
        v[3] = (((cs[i] >> 5) & 0x1F) << 3) / 255.0f;
        v[4] = (((cs[i] >> 10) & 0x1F) << 3) / 255.0f;
        v[5] = mask_a;
        s_fb_n++;
    }
}

static void gpu_triangle(int x0,int y0,uint16_t c0, int x1,int y1,uint16_t c1,
                         int x2,int y2,uint16_t c2, int semi) {
    int xs[3] = {x0, x1, x2}, ys[3] = {y0, y1, y2};
    uint16_t cs[3] = {c0, c1, c2};
    gpu_geometry(GL_TRIANGLES, xs, ys, cs, 3, semi);
}

static void gpu_line(int x0,int y0,uint16_t c0,int x1,int y1,uint16_t c1,int semi) {
    int xs[2] = {x0, x1}, ys[2] = {y0, y1};
    uint16_t cs[2] = {c0, c1};
    gpu_geometry(GL_LINES, xs, ys, cs, 2, semi);
}

/* Shared PS1 uv-sampling model (limits + mirrored-2D compensation) — one
 * implementation for GL/VK/SW, see gpu_uv.h. */
#include "gpu_uv.h"

/* Textured triangle. Always two passes split by the per-texel STP bit so the
 * stencil (mask) write value is constant within each pass; the semi pass is
 * also where PS1 blending applies. lim = uv sampling bounds (see
 * tri_uv_limits); NULL computes them from the vertices. */
static void gpu_textured_triangle(const int *xs, const int *ys,
                                  const int *us, const int *vs,
                                  const float *col, uint16_t texpage,
                                  uint16_t clut_x, uint16_t clut_y, int rawtex,
                                  int semi, const int *lim) {
    int lim_buf[4];
    int uv_buf[6];
    if (!lim) {
        /* Poly path: exact sampled bounds from the ORIGINAL uvs, then the
         * center-sampling mirror compensation (rect prims arrive with their
         * own precomputed lim and pre-bumped uvs). */
        int *mu = uv_buf, *mv = uv_buf + 3;
        for (int i = 0; i < 3; i++) { mu[i] = us[i]; mv[i] = vs[i]; }
        psx_uv_tri_limits(xs, ys, mu, mv, lim_buf);
        psx_uv_tri_mirror_offset(xs, ys, mu, mv);
        us = mu; vs = mv;
        lim = lim_buf;
    }
    s_scene_prims_tex++;
    int base_x = (texpage & 0xF) * 64;
    int base_y = ((texpage >> 4) & 1) * 256;
    int depth  = (texpage >> 7) & 3; if (depth > 2) depth = 2;

    flush_cpu_upload();   /* if a CPU->VRAM upload is pending it flushes the batch first */
    flush_pack_if_sampling(base_x, base_y, depth, clut_x, clut_y);  /* flushes batch iff it must pack */
    mark_prim_dirty(xs, ys, 3, 1 /* textured */);

    /* Append to the textured batch. Flush first if this prim's blend/mask/twin/
     * filter differ from the open batch, or the buffer is full. Per-prim texture
     * state goes in the vertex; only these keys force a new draw. */
    {
        flush_flat_batch();   /* painter order: flat GEO before textured */
        int twx = s_tw_mask_x, twy = s_tw_mask_y, tox = s_tw_off_x, toy = s_tw_off_y;
        int gate = bd_prim_gate(xs, 3, 1); /* backdrop-stretch gate is also a batch key */
        /* With mask checking off, opaque and mode-0 semi primitives use the
         * same dual-source blend state. The per-vertex a_semi flag selects
         * replace vs half-blend without breaking painter order. */
        int batch_semi = (!s_mask_check && semi != 2) ? 4 : semi;
        /* STP draw-ORDER correctness. flush_tex_batch draws a batch in two passes
         * over the WHOLE batch (pass 1 = every prim's STP=0/opaque texels, pass 2
         * = every prim's STP=1/semi texels with the PSX blend). For overlapping
         * prims that share a batch, a BEHIND prim's semi texels (pass 2) then
         * paint OVER a FRONT prim's opaque texels (pass 1) — a painter's-order
         * violation that only exists on GL (Tomba: the character drew behind the
         * AP-block letters / a save post on GL, correct on software). So a
         * semi-transparent prim must NOT coalesce with its neighbours: drain the
         * open batch, draw this prim alone (its own STP=0+STP=1 passes, which do
         * not self-overlap → composited fully before the next prim, exactly like
         * the software renderer), and let opaque prims keep batching. Opaque
         * content (terrain/foliage — the batching perf win) is untouched; the cost
         * is one draw per semi prim, which are sparse. (A future single-pass
         * optimization for modes 0/1/3 via GL_ONE,GL_SRC_ALPHA with a per-fragment
         * destination factor in alpha could re-batch semi prims — see memory
         * tomba_sprite_zorder_bug; mode 2 subtractive still needs isolation.) */
        int isolate = (semi >= 0 && (semi == 2 || s_mask_check));
        int reason = -1;
        if (s_tb_n > 0) {
            if (isolate) reason = 0;
            else if (batch_semi != s_tb_semi) reason = 1;
            else if (s_mask_set != s_tb_mask) reason = 2;
            else if (s_tex_filter != s_tb_filter) reason = 3;
            else if (gate != s_tb_gate) reason = 4;
            else if (twx != s_tb_twin[0] || twy != s_tb_twin[1] ||
                     tox != s_tb_twin[2] || toy != s_tb_twin[3]) reason = 5;
        }
        if (reason >= 0) {
            s_batch_reason[reason]++;
            flush_tex_batch();
        }
        if (s_tb_n + 3 > TEXBATCH_MAXV) { s_batch_reason[6]++; flush_tex_batch(); }
        if (s_tb_n == 0) {            /* opening a batch: capture its keyed state */
            s_tb_semi = batch_semi; s_tb_mask = s_mask_set; s_tb_filter = s_tex_filter; s_tb_gate = gate;
            s_tb_twin[0] = twx; s_tb_twin[1] = twy; s_tb_twin[2] = tox; s_tb_twin[3] = toy;
        }
        float *vp = &s_tb[s_tb_n * TEXV];
        for (int i = 0; i < 3; i++, vp += TEXV) {
            vp[0] = (float)xs[i];   vp[1] = (float)ys[i];
            vp[2] = (float)us[i];   vp[3] = (float)vs[i];
            vp[4] = col[i*3+0];     vp[5] = col[i*3+1];     vp[6] = col[i*3+2];   vp[7] = 1.0f;
            vp[8]  = (float)base_x;  vp[9]  = (float)base_y;        /* a_tpage  */
            vp[10] = (float)clut_x;  vp[11] = (float)clut_y;        /* a_clut   */
            vp[12] = (float)depth;   vp[13] = (float)rawtex;        /* a_depth, a_raw */
            vp[14] = (float)lim[0];  vp[15] = (float)lim[1];        /* a_limits */
            vp[16] = (float)lim[2];  vp[17] = (float)lim[3];
            vp[18] = semi >= 0 ? (float)(semi + 1) : 0.0f;          /* a_semi code */
        }
        s_tb_n += 3;
        if (isolate) flush_tex_batch();   /* draw this semi prim alone, in submission order */
    }
}

/* Draw a flat-colored rect (GEO program) DIRECTLY into the active wide surface
 * at wide-space coords [wx, wx+ww) × [y, y+h). Used only by the full-screen-
 * overlay path; positions are already in wide space so u_xoff stays 0. Mirrors
 * raster_flat_rect(&wt, ...) in sw_draw_flat_rect. Caller stays inside
 * hr_begin/hr_end. */
static void wide_flat_rect_direct(int wx, int y, int ww, int h, uint16_t c, int semi) {
    if (ww <= 0 || h <= 0) return;
    float r = ((c & 0x1F) << 3) / 255.0f;
    float g = (((c >> 5) & 0x1F) << 3) / 255.0f;
    float b = (((c >> 10) & 0x1F) << 3) / 255.0f;
    float a = s_mask_set ? 1.0f : 0.0f;
    float fx0 = (float)wx, fy0 = (float)y, fx1 = (float)(wx + ww), fy1 = (float)(y + h);
    float verts[6 * 6] = {
        fx0,fy0,r,g,b,a,  fx1,fy0,r,g,b,a,  fx0,fy1,r,g,b,a,
        fx1,fy0,r,g,b,a,  fx0,fy1,r,g,b,a,  fx1,fy1,r,g,b,a,
    };
    /* Wide target: positions already wide-space so u_xoff = 0; full-width
     * scissor; u_xhalf = g_wide_w/2. */
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, g_wide_cur);
    glViewport(0, 0, g_wide_w * s_scale, VRAM_H * s_scale);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, g_wide_w * s_scale, VRAM_H * s_scale);  /* full surface (rt_wide) */
    if (semi >= 0) apply_psx_blend(semi); else glDisable(GL_BLEND);
    mask_stencil(s_mask_set);
    p_glUseProgram(s_geo_prog);
    p_glUniform1f(s_geo_uXoff, 0.0f);
    p_glUniform1f(s_geo_uXhalf, (float)g_wide_w / 2.0f);
    p_glBindVertexArray(s_geo_vao);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_geo_vbo);
    p_glBufferData(PSXGL_ARRAY_BUFFER, sizeof verts, verts, PSXGL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    p_glUniform1f(s_geo_uXoff, 0.0f);
    p_glUniform1f(s_geo_uXhalf, 512.0f);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_hr_fbo);
}

static void gpu_flat_rect(int x,int y,int w,int h,uint16_t c,int semi) {
    if (w <= 0 || h <= 0) return;
    /* Full-screen 2D overlay (pause gray-filter / load fade): a flat rect
     * spanning the whole 4:3 framebuffer must cover the whole wide surface too,
     * else the revealed 16:9 margins are left undimmed/unfaded. Same detection
     * as sw_draw_flat_rect: native_w = the 4:3 framebuffer width (g_wide_w less
     * the per-side reveal on both sides); the rect's native screen-X span
     * (x - base) must cover [0, native_w). When it does, the two canonical
     * triangles are drawn WITHOUT the per-triangle wide mirror (suppressed) and
     * a single full-width rect is drawn into the wide surface instead; every
     * other rect mirrors 1:1 via the generic gpu_geometry path. Only runs in
     * native-wide (g_wide_cur != 0), so 4:3 is unaffected. */
    int overlay = 0;
    if (g_wide_cur) {
        int native_w = g_wide_w - 2 * g_wide_off;
        int lx = x - g_wide_cur_base, rx = x + w - g_wide_cur_base;
        overlay = (native_w > 0 && lx <= 0 && rx >= native_w);
    }
    if (overlay) s_wide_suppress = 1;
    gpu_triangle(x,   y,   c, x+w, y,   c, x,   y+h, c, semi);
    gpu_triangle(x+w, y,   c, x,   y+h, c, x+w, y+h, c, semi);
    if (overlay) {
        s_wide_suppress = 0;
        /* The two canonical triangles drew into the hr FBO already (each
         * gpu_triangle ran its own hr_begin/hr_end). Re-open the bracket just
         * for the full-width wide pass so blend/scissor/program state is
         * clean. */
        if (s_ws_ablate != 1) {
            hr_begin(0);
            gl_perf_mirror_begin();
            wide_flat_rect_direct(0, y, g_wide_w, h, c, semi);
            gl_perf_mirror_end();
            hr_end();
        }
    }
}

static void gpu_textured_rect(int x,int y,int w,int h,
                              int u0,int v0,int u1,int v1,
                              uint16_t clut_x,uint16_t clut_y,uint16_t tp,int semi) {
    if (w <= 0 || h <= 0) return;
    float mr=s_mod_r/255.0f, mg=s_mod_g/255.0f, mb=s_mod_b/255.0f;
    float col[9]={mr,mg,mb, mr,mg,mb, mr,mg,mb};
    /* gpu.c routes axis-aligned MIRRORED quads (X/Y-flipped 2D sprites,
     * e.g. right-facing MMX entities) through THIS path as scaled rects
     * with u0>u1 / v0>v1 — they never reach the poly path. Exact bounds
     * from the original corners, then the mirror bump (see gpu_uv.h). */
    int lim[4];
    psx_uv_rect_limits(u0, v0, u1, v1, lim);
    psx_uv_rect_mirror_offset(&u0, &v0, &u1, &v1);
    int xs1[3]={x, x+w, x},    ys1[3]={y, y, y+h};
    int us1[3]={u0,u1,u0},     vs1[3]={v0,v0,v1};
    gpu_textured_triangle(xs1,ys1,us1,vs1,col,tp,clut_x,clut_y,s_mod_raw,semi,lim);
    int xs2[3]={x+w, x, x+w},  ys2[3]={y, y+h, y+h};
    int us2[3]={u1,u0,u1},     vs2[3]={v0,v1,v1};
    gpu_textured_triangle(xs2,ys2,us2,vs2,col,tp,clut_x,clut_y,s_mod_raw,semi,lim);
}

/* GP0(02h) fill: writes color with bit15=0, ignoring draw area, mask and
 * offset; coordinates wrap. A scissored clear (color + stencil) per wrapped
 * segment is exactly this. */
static void fill_segment(int x, int y, int w, int h, float r, float g, float b) {
    if (w <= 0 || h <= 0) return;
    glScissor(x * s_scale, y * s_scale, w * s_scale, h * s_scale);
    glClearColor(r, g, b, 0.0f);
    glClearStencil(0);
    glStencilMask(0xFF);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    rect_add(&s_pack_dirty, x, y, x + w - 1, y + h - 1);
}

static void gpu_fill(int x,int y,int w,int h,uint16_t c) {
    if (w <= 0 || h <= 0) return;
    flush_flat_batch();
    flush_tex_batch();
    flush_cpu_upload();
    float r=(c&0x1F)/31.0f, g=((c>>5)&0x1F)/31.0f, b=((c>>10)&0x1F)/31.0f;
    x &= VRAM_W - 1; y &= VRAM_H - 1;
    if (w > VRAM_W) w = VRAM_W;
    if (h > VRAM_H) h = VRAM_H;
    int w1 = w, w2 = 0, h1 = h, h2 = 0;
    if (x + w > VRAM_W) { w1 = VRAM_W - x; w2 = w - w1; }
    if (y + h > VRAM_H) { h1 = VRAM_H - y; h2 = h - h1; }

    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_hr_fbo);
    glViewport(0, 0, VRAM_W * s_scale, VRAM_H * s_scale);
    glEnable(GL_SCISSOR_TEST);
    fill_segment(x, y, w1, h1, r, g, b);
    if (w2)       fill_segment(0, y, w2, h1, r, g, b);
    if (h2)       fill_segment(x, 0, w1, h2, r, g, b);
    if (w2 && h2) fill_segment(0, 0, w2, h2, r, g, b);
    glDisable(GL_SCISSOR_TEST);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    s_gpu_dirty = 1;
    coh_record(GL_COH_FILL, x, y, x + w - 1, y + h - 1);
}

/* VRAM->VRAM copy: blit the source region to the scratch texture (resolves
 * overlap), then draw it back at the destination through the BLIT program so
 * mask set/check and the stencil mirror apply, exactly like sw_copy_rect. */
static void gpu_copy_rect(int sx,int sy,int dx,int dy,int w,int h) {
    if (w <= 0 || h <= 0) return;
    flush_flat_batch();
    flush_tex_batch();
    flush_cpu_upload();
    /* Clamp to bounds (the software path wraps; wrapping copies are unused
     * in practice — see the file header). */
    if (sx < 0) sx = 0; if (sy < 0) sy = 0;
    if (dx < 0) dx = 0; if (dy < 0) dy = 0;
    if (sx + w > VRAM_W) w = VRAM_W - sx;
    if (dx + w > VRAM_W) w = VRAM_W - dx;
    if (sy + h > VRAM_H) h = VRAM_H - sy;
    if (dy + h > VRAM_H) h = VRAM_H - dy;
    if (w <= 0 || h <= 0) return;

    int S = s_scale;
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_hr_fbo);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, s_scratch_fbo);
    glDisable(GL_SCISSOR_TEST);
    p_glBlitFramebuffer(sx*S, sy*S, (sx+w)*S, (sy+h)*S,
                        sx*S, sy*S, (sx+w)*S, (sy+h)*S,
                        GL_COLOR_BUFFER_BIT, GL_NEAREST);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);

    hr_begin(0);   /* copies ignore the draw area */
    glScissor(dx*S, dy*S, w*S, h*S);
    glDisable(GL_BLEND);
    p_glUseProgram(s_blit_prog);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_scratch_tex);
    p_glUniform1i(s_uBlitSrc, 0);
    p_glUniform1i(s_uBlitMaskset, s_mask_set);
    /* Scratch holds the source at its own hr coords: texel = frag + (src-dst)*S. */
    p_glUniform1i(s_uBlitSrcDiv, 1);
    p_glUniform2i(s_uBlitSrcOff, (sx - dx) * S, (sy - dy) * S);
    float fx0 = (float)dx, fy0 = (float)dy, fx1 = (float)(dx + w), fy1 = (float)(dy + h);
    float verts[6 * 2] = {
        fx0, fy0,  fx1, fy0,  fx0, fy1,
        fx1, fy0,  fx0, fy1,  fx1, fy1,
    };
    p_glBindVertexArray(s_blit_vao);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_blit_vbo);
    p_glBufferData(PSXGL_ARRAY_BUFFER, sizeof verts, verts, PSXGL_STREAM_DRAW);
    /* Two passes split by source bit15 so stencil tracks the copied mask. */
    mask_stencil(s_mask_set); p_glUniform1i(s_uBlitPass, 1); glDrawArrays(GL_TRIANGLES, 0, 6);
    mask_stencil(1);          p_glUniform1i(s_uBlitPass, 2); glDrawArrays(GL_TRIANGLES, 0, 6);
    hr_end();

    rect_add(&s_pack_dirty, dx, dy, dx + w - 1, dy + h - 1);
    s_gpu_dirty = 1;
    coh_record(GL_COH_COPY_SRC, sx, sy, sx + w - 1, sy + h - 1);
    coh_record(GL_COH_COPY,     dx, dy, dx + w - 1, dy + h - 1);
}

/* ---- backend vtable wrappers ------------------------------------------- */
static void glb_init(uint16_t *vram) { s_vram = vram; sw_renderer_init(vram); }

/* Under GL the internal-resolution scale lives in the hr FBO; the CPU-side
 * (software mirror, readbacks, screenshots) stays native, so the reported
 * scale is 1 and the software hi-res mirror stays off. */
static void glb_set_scale(int s) {
    if (s < 1) s = 1;
    if (s > GL_MAX_INTERNAL_SCALE) s = GL_MAX_INTERNAL_SCALE;
    s_req_scale = s;
    sw_renderer_set_scale(1);
}
static int  glb_scale(void) { return s_scale; }   /* real internal SSAA scale (was a stub 1; the
                                                      native-wide CPU present path + gr_scale() callers
                                                      need the true scale — the FBO-direct present is
                                                      unaffected since it never reads gr_scale()) */
static void glb_set_texture_filter(int b) { s_tex_filter = b ? 1 : 0; sw_set_texture_filter(b); }
static int  glb_texture_filter(void) { return s_tex_filter; }

static void glb_set_semi_transparency(int e, int m) { s_semi_en = e; s_semi_mode = m & 3; sw_set_semi_transparency(e, m); }
static void glb_set_mask_bits(int s, int c) {
    int next_check = c ? 1 : 0;
    if (next_check && !s_mask_check) {
        /* Land all alpha-authoritative work before deriving stencil from it. */
        flush_tex_batch();
        flush_cpu_upload();
        rebuild_mask_stencils();
    }
    s_mask_set = s ? 1 : 0;
    s_mask_check = next_check;
    sw_set_mask_bits(s, c);
}
static void glb_set_texture_window(uint32_t r) {
    s_tw_mask_x = (int)(r & 0x1F);
    s_tw_mask_y = (int)((r >> 5) & 0x1F);
    s_tw_off_x  = (int)((r >> 10) & 0x1F);
    s_tw_off_y  = (int)((r >> 15) & 0x1F);
    sw_set_texture_window(r);
}
static void glb_set_color_modulation(int r,int g,int b,int raw) { s_mod_r=r; s_mod_g=g; s_mod_b=b; s_mod_raw=raw; sw_set_color_modulation(r,g,b,raw); }
static void glb_set_draw_area(int x1,int y1,int x2,int y2) { flush_flat_batch(); flush_tex_batch(); s_area_x1=x1; s_area_y1=y1; s_area_x2=x2; s_area_y2=y2; sw_set_draw_area(x1,y1,x2,y2); }
static void glb_get_draw_area(int *x1,int *y1,int *x2,int *y2) { sw_get_draw_area(x1,y1,x2,y2); }
static void glb_set_draw_offset(int x,int y) { flush_flat_batch(); flush_tex_batch(); s_off_x=x; s_off_y=y; sw_set_draw_offset(x,y); }

/* Pre-context draws (s_raster_ok == 0) fall back to the software rasterizer
 * over CPU VRAM; the initial full-VRAM upload at context init folds them in.
 * Post-init, the GPU pipeline is all-or-nothing — no per-prim fallback. */
static void glb_draw_flat_triangle(int x0,int y0,int x1,int y1,int x2,int y2,uint16_t col) {
    if (!s_raster_ok) { sw_draw_flat_triangle(x0,y0,x1,y1,x2,y2,col); return; }
    gpu_triangle(x0,y0,col, x1,y1,col, x2,y2,col, s_semi_en?s_semi_mode:-1);
}
static void glb_draw_gouraud_triangle(int x0,int y0,uint16_t c0,int x1,int y1,uint16_t c1,int x2,int y2,uint16_t c2) {
    if (!s_raster_ok) { sw_draw_gouraud_triangle(x0,y0,c0,x1,y1,c1,x2,y2,c2); return; }
    gpu_triangle(x0,y0,c0, x1,y1,c1, x2,y2,c2, s_semi_en?s_semi_mode:-1);
}
static void glb_fill_rect(int x,int y,int w,int h,uint16_t c){
    if (!s_raster_ok) { sw_fill_rect(x,y,w,h,c); return; }
    gpu_fill(x,y,w,h,c);
}
static void glb_copy_rect(int sx,int sy,int dx,int dy,int w,int h){
    if (!s_raster_ok) { sw_copy_rect(sx,sy,dx,dy,w,h); return; }
    gpu_copy_rect(sx,sy,dx,dy,w,h);
}
static void glb_draw_textured_triangle(int x0,int y0,int u0,int v0,int x1,int y1,int u1,int v1,int x2,int y2,int u2,int v2,uint16_t cx,uint16_t cy,uint16_t tp){
    if (!s_raster_ok) { sw_draw_textured_triangle(x0,y0,u0,v0,x1,y1,u1,v1,x2,y2,u2,v2,cx,cy,tp); return; }
    int xs[3]={x0,x1,x2}, ys[3]={y0,y1,y2}, us[3]={u0,u1,u2}, vs[3]={v0,v1,v2};
    float mr=s_mod_r/255.0f, mg=s_mod_g/255.0f, mb=s_mod_b/255.0f;
    float col[9]={mr,mg,mb, mr,mg,mb, mr,mg,mb};
    gpu_textured_triangle(xs,ys,us,vs,col,tp,cx,cy,s_mod_raw, s_semi_en?s_semi_mode:-1, NULL);
}
static void glb_draw_shaded_textured_triangle(int x0,int y0,int u0,int v0,uint32_t c0,int x1,int y1,int u1,int v1,uint32_t c1,int x2,int y2,int u2,int v2,uint32_t c2,uint16_t cx,uint16_t cy,uint16_t tp,int raw){
    if (!s_raster_ok) { sw_draw_shaded_textured_triangle(x0,y0,u0,v0,c0,x1,y1,u1,v1,c1,x2,y2,u2,v2,c2,cx,cy,tp,raw); return; }
    int xs[3]={x0,x1,x2}, ys[3]={y0,y1,y2}, us[3]={u0,u1,u2}, vs[3]={v0,v1,v2};
    uint32_t cc[3]={c0,c1,c2}; float col[9];
    for (int i=0;i<3;i++){ col[i*3+0]=(cc[i]&0xFF)/255.0f; col[i*3+1]=((cc[i]>>8)&0xFF)/255.0f; col[i*3+2]=((cc[i]>>16)&0xFF)/255.0f; }
    gpu_textured_triangle(xs,ys,us,vs,col,tp,cx,cy,raw, s_semi_en?s_semi_mode:-1, NULL);
}
static void glb_draw_flat_rect(int x,int y,int w,int h,uint16_t c){
    if (!s_raster_ok) { sw_draw_flat_rect(x,y,w,h,c); return; }
    gpu_flat_rect(x,y,w,h,c, s_semi_en?s_semi_mode:-1);
}
static void glb_draw_textured_rect(int x,int y,int w,int h,int u,int v,uint16_t cx,uint16_t cy,uint16_t tp){
    if (!s_raster_ok) { sw_draw_textured_rect(x,y,w,h,u,v,cx,cy,tp); return; }
    gpu_textured_rect(x,y,w,h, u,v, u+w,v+h, cx,cy,tp, s_semi_en?s_semi_mode:-1);
}
static void glb_draw_textured_rect_scaled(int x,int y,int w,int h,int u0,int v0,int u1,int v1,uint16_t cx,uint16_t cy,uint16_t tp){
    if (!s_raster_ok) { sw_draw_textured_rect_scaled(x,y,w,h,u0,v0,u1,v1,cx,cy,tp); return; }
    gpu_textured_rect(x,y,w,h, u0,v0, u1,v1, cx,cy,tp, s_semi_en?s_semi_mode:-1);
}
static void glb_draw_line(int x0,int y0,int x1,int y1,uint16_t c){
    if (!s_raster_ok) { sw_draw_line(x0,y0,x1,y1,c); return; }
    gpu_line(x0,y0,c, x1,y1,c, s_semi_en?s_semi_mode:-1);
}
static void glb_draw_shaded_line(int x0,int y0,uint16_t c0,int x1,int y1,uint16_t c1){
    if (!s_raster_ok) { sw_draw_shaded_line(x0,y0,c0,x1,y1,c1); return; }
    gpu_line(x0,y0,c0, x1,y1,c1, s_semi_en?s_semi_mode:-1);
}
static int  glb_render_display(uint32_t *o,int p,int dx,int dy,int dw,int dh){ ensure_cpu(); return sw_render_display(o,p,dx,dy,dw,dh); }
static int  glb_render_display_hires(uint32_t *o,int p,int dx,int dy,int dw,int dh){ ensure_cpu(); return sw_render_display_hires(o,p,dx,dy,dw,dh); }
/* While GP1 depth24 is on, packed RGB888 lives in the CPU mirror and is
 * presented via gl_renderer_present — never as 1555 FBO texels. Queuing those
 * MDEC A0 rects hits UP_RECTS_MAX (16) and force-flushes mid-movie (MotK intro
 * ~50→~30 FPS). Skip ONLY framebuffer-sized transfers (RGB888); still upload
 * smaller texture A0s so post-FMV menus keep VRAM pages coherent.
 * On leave: clear the skipped FB union in the FBO — do NOT restage CPU RGB888
 * as 1555 (that painted MotK title rainbow/static). */
static int s_depth24_skip_up = 0;
static DirtyRect s_d24_skip_fb; /* union of skipped MDEC FB rects (VRAM halfwords) */

static int depth24_is_fb_transfer(int w, int h) {
    if (!gpu_display_is_depth24() || w <= 0 || h <= 0) return 0;
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    int fb_w = (int)((di.width * 3u + 1u) / 2u); /* RGB W → halfwords */
    int fb_h = (int)di.height;
    if (fb_w < 8) fb_w = 8;
    if (fb_h < 1) fb_h = 1;
    /* MotK: 768×128 class blits; allow slack. Reject small texture pages. */
    if (h >= fb_h - 8 && h <= fb_h + 16 && w >= (fb_w * 3) / 4) return 1;
    if ((int64_t)w * (int64_t)h >= ((int64_t)fb_w * fb_h) / 2) return 1;
    return 0;
}

static void depth24_clear_skipped_fb(void) {
    if (!s_raster_ok || !s_d24_skip_fb.set) return;
    flush_flat_batch();
    flush_tex_batch();
    int x0 = s_d24_skip_fb.x0, y0 = s_d24_skip_fb.y0;
    int x1 = s_d24_skip_fb.x1, y1 = s_d24_skip_fb.y1;
    int rw = x1 - x0 + 1, rh = y1 - y0 + 1;
    int S = s_scale;
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_hr_fbo);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_SCISSOR_TEST);
    glViewport(0, 0, VRAM_W * S, VRAM_H * S);
    glScissor(x0 * S, y0 * S, rw * S, rh * S);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClearStencil(0);
    glStencilMask(0xFF);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    if (s_raw_fbo) {
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_raw_fbo);
        glViewport(0, 0, VRAM_W, VRAM_H);
        glScissor(x0, y0, rw, rh);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glDisable(GL_SCISSOR_TEST);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    rect_add(&s_pack_dirty, x0, y0, x1, y1);
    present_dirty_rect(x0, y0, x1, y1, 1);
    rect_clear(&s_d24_skip_fb);
}

static void depth24_upload_policy(void) {
    int d24 = gpu_display_is_depth24();
    if (d24 && !s_depth24_skip_up) {
        s_up_nrects = 0;
        rect_clear(&s_d24_skip_fb);
    } else if (!d24 && s_depth24_skip_up) {
        s_up_nrects = 0;
        depth24_clear_skipped_fb();
        gpu_depth24_upload_span_reset();
    }
    s_depth24_skip_up = d24;
}

static void glb_vram_write(int x,int y,uint16_t px){
    sw_vram_write(x,y,px);
    depth24_upload_policy();
    /* Point pokes are never MDEC frames — always stage to FBO. */
    up_add(x & (VRAM_W-1), y & (VRAM_H-1), x & (VRAM_W-1), y & (VRAM_H-1));
}
static uint16_t glb_vram_read(int x,int y){ ensure_cpu(); return sw_vram_read(x,y); }
static void glb_vram_transfer_in(int x,int y,int w,int h,const uint16_t *d){
    sw_vram_transfer_in(x,y,w,h,d);
    depth24_upload_policy();
    if (s_depth24_skip_up && depth24_is_fb_transfer(w, h)) {
        int x0 = x & (VRAM_W - 1), y0 = y & (VRAM_H - 1);
        rect_add(&s_d24_skip_fb, x0, y0, x0 + w - 1, y0 + h - 1);
        coh_record(GL_COH_UPLOAD, x, y, x+w-1, y+h-1);
        return;
    }
    up_add_transfer(x, y, w, h);   /* exact touched rects, incl. per-pixel wrap */
    coh_record(GL_COH_UPLOAD, x, y, x+w-1, y+h-1);
}
static void glb_vram_transfer_out(int x,int y,int w,int h,uint16_t *d){ ensure_cpu(); sw_vram_transfer_out(x,y,w,h,d); }

/* ---- context init / present -------------------------------------------- */
static void upload_present_tex(const uint32_t *pixels, int w, int h, int linear) {
    glBindTexture(GL_TEXTURE_2D, s_present_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    /* Re-assert clamp every upload: a stale REPEAT wrap samples past the
     * right edge into garbage (MotK FMV right-strip flicker). */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (w != s_present_w || h != s_present_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
        s_present_w = w; s_present_h = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
    }
}

/* Display aspect for the present letterbox. Default 4:3 (native). When a wide
 * aspect is configured the 4:3 frame is stretched into it — paired with the
 * GTE X-squash (gte_set_display_aspect) this nets a wider field of view. */
static int s_aspect_num = 4, s_aspect_den = 3;

void gl_renderer_set_display_aspect(int num, int den) {
    if (num <= 0 || den <= 0) { num = 4; den = 3; }
    s_aspect_num = num; s_aspect_den = den;
}

/* Letterbox: largest num:den rect centered in the drawable. */
static void letterbox_rect_aspect(int ww, int wh, int num, int den,
                                  int *x, int *y, int *w, int *h) {
    int dw = ww, dh = (ww * den) / num;
    if (dh > wh) { dh = wh; dw = (wh * num) / den; }
    *x = (ww - dw) / 2;
    *y = (wh - dh) / 2;
    *w = dw; *h = dh;
}
static void letterbox_rect(int ww, int wh, int *x, int *y, int *w, int *h) {
    letterbox_rect_aspect(ww, wh, s_aspect_num, s_aspect_den, x, y, w, h);
}

static GLuint make_tex(GLenum internal, int w, int h, GLenum fmt, GLenum type) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal, w, h, 0, fmt, type, NULL);
    return t;
}

static int make_fbo(GLuint *out_fbo, GLuint color_tex, GLuint stencil_rb) {
    p_glGenFramebuffers(1, out_fbo);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, *out_fbo);
    p_glFramebufferTexture2D(PSXGL_FRAMEBUFFER, PSXGL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_tex, 0);
    if (stencil_rb)
        p_glFramebufferRenderbuffer(PSXGL_FRAMEBUFFER, PSXGL_DEPTH_STENCIL_ATTACHMENT,
                                    PSXGL_RENDERBUFFER, stencil_rb);
    GLenum st = p_glCheckFramebufferStatus(PSXGL_FRAMEBUFFER);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    if (st != PSXGL_FRAMEBUFFER_COMPLETE) {
        fprintf(stdout, "psxrecomp: GL FBO incomplete (0x%X)\n", st);
        return 0;
    }
    return 1;
}

static int init_gpu_raster(void) {
    s_scale = s_req_scale;

    s_geo_prog  = build_program(GEO_VS, GEO_FS);
    s_tex_prog  = build_program_ex(TEX_VS, TEX_FS, 1);
    s_blit_prog = build_program(BLIT_VS, BLIT_FS);
    s_pack_prog = build_program(PACK_VS, PACK_FS);
    s_stencil_prog = build_program(PACK_VS, STENCIL_FS);
    if (!s_geo_prog || !s_tex_prog || !s_blit_prog || !s_pack_prog || !s_stencil_prog) return 0;

    s_conv = (uint32_t *)malloc((size_t)VRAM_W * VRAM_H * sizeof(uint32_t));
    if (!s_conv) return 0;

    int hw = VRAM_W * s_scale, hh = VRAM_H * s_scale;
    s_hr_tex      = make_tex(GL_RGBA8, hw, hh, GL_RGBA, GL_UNSIGNED_BYTE);
    s_scratch_tex = make_tex(GL_RGBA8, hw, hh, GL_RGBA, GL_UNSIGNED_BYTE);
    s_up_tex      = make_tex(GL_RGBA8, VRAM_W, VRAM_H, GL_RGBA, GL_UNSIGNED_BYTE);
    s_raw_tex     = make_tex(PSXGL_R16UI, VRAM_W, VRAM_H, PSXGL_RED_INTEGER, GL_UNSIGNED_SHORT);
    /* Force the driver's first texture-upload allocation while the renderer is
     * initializing. NVIDIA otherwise defers it until the first MDEC frame,
     * producing a measured ~33 ms glTexSubImage hitch and an audible underrun. */
    {
        const uint32_t zero_rgba = 0;
        const uint16_t zero_raw = 0;
        glBindTexture(GL_TEXTURE_2D, s_up_tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1,
                        GL_RGBA, GL_UNSIGNED_BYTE, &zero_rgba);
        glBindTexture(GL_TEXTURE_2D, s_raw_tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1,
                        PSXGL_RED_INTEGER, GL_UNSIGNED_SHORT, &zero_raw);
        glFinish();
    }

    p_glGenRenderbuffers(1, &s_hr_rb);
    p_glBindRenderbuffer(PSXGL_RENDERBUFFER, s_hr_rb);
    p_glRenderbufferStorage(PSXGL_RENDERBUFFER, PSXGL_DEPTH24_STENCIL8, hw, hh);
    p_glBindRenderbuffer(PSXGL_RENDERBUFFER, 0);

    if (!make_fbo(&s_hr_fbo, s_hr_tex, s_hr_rb)) return 0;
    if (!make_fbo(&s_raw_fbo, s_raw_tex, 0)) return 0;
    if (!make_fbo(&s_scratch_fbo, s_scratch_tex, 0)) return 0;

    s_uVram  = p_glGetUniformLocation(s_tex_prog, "u_vram");
    s_uTpage = p_glGetUniformLocation(s_tex_prog, "u_tpage");
    s_uClut  = p_glGetUniformLocation(s_tex_prog, "u_clut");
    s_uDepth = p_glGetUniformLocation(s_tex_prog, "u_depth");
    s_uRaw   = p_glGetUniformLocation(s_tex_prog, "u_raw");
    s_uSemipass = p_glGetUniformLocation(s_tex_prog, "u_semipass");
    s_uSemimode = p_glGetUniformLocation(s_tex_prog, "u_semimode");
    s_uTwin     = p_glGetUniformLocation(s_tex_prog, "u_twin");
    s_uMaskset  = p_glGetUniformLocation(s_tex_prog, "u_maskset");
    s_uFilter   = p_glGetUniformLocation(s_tex_prog, "u_filter");
    s_uLimits   = p_glGetUniformLocation(s_tex_prog, "u_limits");
    s_uBlitSrc     = p_glGetUniformLocation(s_blit_prog, "u_src");
    s_uBlitPass    = p_glGetUniformLocation(s_blit_prog, "u_stp_pass");
    s_uBlitMaskset = p_glGetUniformLocation(s_blit_prog, "u_maskset");
    s_uBlitSrcDiv  = p_glGetUniformLocation(s_blit_prog, "u_src_div");
    s_uBlitSrcOff  = p_glGetUniformLocation(s_blit_prog, "u_src_off");
    s_uPackHr    = p_glGetUniformLocation(s_pack_prog, "u_hr");
    s_uPackScale = p_glGetUniformLocation(s_pack_prog, "u_scale");
    s_uStencilSrc = p_glGetUniformLocation(s_stencil_prog, "u_src");
    s_geo_uXoff  = p_glGetUniformLocation(s_geo_prog, "u_xoff");
    s_geo_uXhalf = p_glGetUniformLocation(s_geo_prog, "u_xhalf");
    s_tex_uXoff  = p_glGetUniformLocation(s_tex_prog, "u_xoff");
    s_tex_uXhalf = p_glGetUniformLocation(s_tex_prog, "u_xhalf");
    s_geo_uXscale  = p_glGetUniformLocation(s_geo_prog, "u_xscale");
    s_geo_uXcenter = p_glGetUniformLocation(s_geo_prog, "u_xcenter");
    s_tex_uXscale  = p_glGetUniformLocation(s_tex_prog, "u_xscale");
    s_tex_uXcenter = p_glGetUniformLocation(s_tex_prog, "u_xcenter");
    /* Default the new uniforms to the no-op (1.0 scale, 0 centre) -- GLSL would
     * otherwise zero them, collapsing all x to 0. */
    p_glUseProgram(s_geo_prog);
    p_glUniform1f(s_geo_uXscale, 1.0f); p_glUniform1f(s_geo_uXcenter, 0.0f);
    p_glUseProgram(s_tex_prog);
    p_glUniform1f(s_tex_uXscale, 1.0f); p_glUniform1f(s_tex_uXcenter, 0.0f);

    /* Sample-grid alignment shift: half an HR pixel, set once (S is fixed
     * for the lifetime of the pipeline). Backed off by 1/64 native px so
     * primitive edges never land EXACTLY on sample centers — that float tie
     * dropped 1px columns at quad seams (e.g. the 256px texture-page seam in
     * Tomba's title background). The 1/64 bias keeps floor(uv) on the exact
     * PS1 texel for |uv slope| < 64; mirrored (negative-slope) mappings can
     * be off by one texel at exact-integer uv — accepted. */
    {
        float shift = 0.5f / (float)s_scale - 1.0f / 64.0f;
        p_glUseProgram(s_geo_prog);
        p_glUniform1f(p_glGetUniformLocation(s_geo_prog, "u_shift"), shift);
        /* Native-wide projection defaults: x translation 0, clip half-extent
         * 512 — so the GEO_VS x term reduces to (x+u_shift)/512-1, bit-identical
         * to the pre-native-wide projection. The wide passes set these, then
         * restore these defaults. */
        p_glUniform1f(s_geo_uXoff, 0.0f);
        p_glUniform1f(s_geo_uXhalf, 512.0f);
        p_glUseProgram(s_tex_prog);
        p_glUniform1f(p_glGetUniformLocation(s_tex_prog, "u_shift"), shift);
        p_glUniform1f(s_tex_uXoff, 0.0f);
        p_glUniform1f(s_tex_uXhalf, 512.0f);
        p_glUseProgram(s_blit_prog);
        p_glUniform1f(p_glGetUniformLocation(s_blit_prog, "u_shift"), shift);
        p_glUseProgram(0);
    }

    p_glGenVertexArrays(1, &s_geo_vao);
    p_glBindVertexArray(s_geo_vao);
    p_glGenBuffers(1, &s_geo_vbo);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_geo_vbo);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)0);
    p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)(2 * sizeof(float)));
    p_glEnableVertexAttribArray(1);

    p_glGenVertexArrays(1, &s_tex_vao);
    p_glBindVertexArray(s_tex_vao);
    p_glGenBuffers(1, &s_tex_vbo);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_tex_vbo);
    {
        GLsizei st = TEXV * sizeof(float);
        p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, st, (void*)0);                  p_glEnableVertexAttribArray(0); /* pos    */
        p_glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, st, (void*)(2*sizeof(float)));  p_glEnableVertexAttribArray(1); /* uv     */
        p_glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, st, (void*)(4*sizeof(float)));  p_glEnableVertexAttribArray(2); /* col    */
        p_glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, st, (void*)(8*sizeof(float)));  p_glEnableVertexAttribArray(3); /* tpage  */
        p_glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, st, (void*)(10*sizeof(float))); p_glEnableVertexAttribArray(4); /* clut   */
        p_glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, st, (void*)(12*sizeof(float))); p_glEnableVertexAttribArray(5); /* depth  */
        p_glVertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, st, (void*)(13*sizeof(float))); p_glEnableVertexAttribArray(6); /* raw    */
        p_glVertexAttribPointer(7, 4, GL_FLOAT, GL_FALSE, st, (void*)(14*sizeof(float))); p_glEnableVertexAttribArray(7); /* limits */
        p_glVertexAttribPointer(8, 1, GL_FLOAT, GL_FALSE, st, (void*)(18*sizeof(float))); p_glEnableVertexAttribArray(8); /* semi   */
    }

    p_glGenVertexArrays(1, &s_blit_vao);
    p_glBindVertexArray(s_blit_vao);
    p_glGenBuffers(1, &s_blit_vbo);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_blit_vbo);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2*sizeof(float), (void*)0);
    p_glEnableVertexAttribArray(0);

    p_glGenVertexArrays(1, &s_empty_vao);
    p_glBindVertexArray(0);

    /* Clear the authoritative surface (color + stencil) and queue a full
     * upload of whatever the CPU VRAM already holds (pre-context software
     * draws, BIOS logo state, ...). */
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_hr_fbo);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 0);
    glClearStencil(0);
    glStencilMask(0xFF);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    rect_clear(&s_pack_dirty);
    s_up_nrects = 0;
    up_add(0, 0, VRAM_W - 1, VRAM_H - 1);
    s_gpu_dirty = 0;
    s_stencil_valid = 1;
    for (int i = 0; i < PRES_ROWS; i++) s_present_dirty[i] = ~0ull;
    s_last_present_path = -1;

    /* Native-wide compositor surfaces start unallocated (lazily created when a
     * widescreen game calls wide_configure + wide_set_target). */
    for (int i = 0; i < WIDE_MAX_SURF; i++) {
        s_wide_tex[i] = 0; s_wide_fbo[i] = 0; s_wide_base[i] = -1;
    }
    g_wide_w = 0; g_wide_off = 0; g_wide_cur = 0; g_wide_cur_base = 0;

    s_raster_ok = 1;
    gl_perf_init();   /* frame_perf GPU/CPU phase timing (no-op if queries absent) */
    fprintf(stdout, "psxrecomp: GL GPU pipeline ready (internal scale %dx, "
            "mask-bit stencil, texture window, GPU copy/upload)\n", s_scale);
    return 1;
}

int gl_renderer_init_context(SDL_Window *win) {
    s_win = win;
    s_present_w = 0;
    s_present_h = 0;
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    s_ctx = SDL_GL_CreateContext(win);
    if (!s_ctx) { fprintf(stdout, "psxrecomp: GL context creation failed (%s)\n", SDL_GetError()); return 0; }
    if (SDL_GL_MakeCurrent(win, s_ctx) != 0) { fprintf(stdout, "psxrecomp: MakeCurrent failed (%s)\n", SDL_GetError()); SDL_GL_DeleteContext(s_ctx); s_ctx=NULL; return 0; }
    /* Swap interval: 1=vsync (tear-free, default), 0=immediate (lowest display
     * latency, may tear; our wall-clock pacer still holds 59.94Hz), -1=adaptive.
     * Adaptive falls back to vsync if the driver rejects it. */
    if (SDL_GL_SetSwapInterval(s_swap_interval) != 0 && s_swap_interval < 0) {
        SDL_GL_SetSwapInterval(1);
        s_swap_interval = 1;
    }
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    const char *ver = (const char *)glGetString(GL_VERSION);
    fprintf(stdout, "psxrecomp: OpenGL context created (%s)\n", ver ? ver : "?");

    /* All-or-nothing: any missing entry point / failed shader / bad FBO means
     * the whole GL renderer is unavailable and the runtime stays on the pure
     * software path — no half-GL hybrid (that mixed mode is what produced
     * the alternating-present menu jitter). */
    int ok = load_modern_gl();
    if (ok) {
        glGenTextures(1, &s_present_tex);
        glBindTexture(GL_TEXTURE_2D, s_present_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        s_present_prog = build_program(PRESENT_VS, PRESENT_FS);
        s_interp_prog = build_program(PRESENT_VS, INTERP_FS);
        if (s_present_prog && s_interp_prog) {
            p_glGenVertexArrays(1, &s_present_vao);
            s_present_uTex = p_glGetUniformLocation(s_present_prog, "u_tex");
            s_present_uUvRect = p_glGetUniformLocation(s_present_prog, "u_uv_rect");
            s_interp_uPrev = p_glGetUniformLocation(s_interp_prog, "u_prev");
            s_interp_uCurr = p_glGetUniformLocation(s_interp_prog, "u_curr");
            s_interp_uAlpha = p_glGetUniformLocation(s_interp_prog, "u_alpha");
            s_interp_uUvRect = p_glGetUniformLocation(s_interp_prog, "u_uv_rect");
            glGenTextures(3, s_interp_tex);
            for (int i = 0; i < 3; i++) {
                glBindTexture(GL_TEXTURE_2D, s_interp_tex[i]);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            }
        } else ok = 0;
    }
    if (ok) ok = init_gpu_raster();
    if (!ok) {
        fprintf(stdout, "psxrecomp: GL pipeline init failed — falling back to software renderer\n");
        SDL_GL_DeleteContext(s_ctx); s_ctx = NULL;
        s_raster_ok = 0;
        return 0;
    }
    return 1;
}

/* Set the GL swap interval (vsync mode): 1=vsync, 0=immediate, -1=adaptive.
 * Safe to call before or after context creation; applies live when a context
 * exists. Adaptive falls back to vsync if unsupported. */
void gl_renderer_set_swap_interval(int interval) {
    s_swap_interval = interval;
    if (s_ctx) {
        if (SDL_GL_SetSwapInterval(interval) != 0 && interval < 0) {
            SDL_GL_SetSwapInterval(1);
            s_swap_interval = 1;
        }
    }
}

void gl_renderer_shutdown(void) {
    if (s_interp_thread) {
        SDL_AtomicSet(&s_interp_thread_run, 0);
        SDL_WaitThread(s_interp_thread, NULL);
        s_interp_thread = NULL;
    }
    if (s_interp_ctx) {
        SDL_GL_DeleteContext(s_interp_ctx);
        s_interp_ctx = NULL;
    }
    if (s_ctx) {
        SDL_GL_MakeCurrent(s_win, s_ctx);
        if (s_interp_draw_fence) {
            p_glDeleteSync(s_interp_draw_fence);
            s_interp_draw_fence = NULL;
        }
        for (int i = 0; i < 3; i++) {
            if (s_interp_fence[i]) {
                p_glDeleteSync(s_interp_fence[i]);
                s_interp_fence[i] = NULL;
            }
        }
        ensure_cpu();
        SDL_GL_DeleteContext(s_ctx); s_ctx = NULL;
    }
    if (s_interp_mutex) {
        SDL_DestroyMutex(s_interp_mutex);
        s_interp_mutex = NULL;
    }
    free(s_conv); s_conv = NULL;
    s_raster_ok = 0;
    /* New context regenerates s_present_tex empty; a stale size makes
     * upload_present_tex take glTexSubImage2D into an unallocated texture
     * (rematch 24-bit FMV → black picture, audio still runs). */
    s_present_w = 0;
    s_present_h = 0;
    s_depth24_skip_up = 0;
    rect_clear(&s_d24_skip_fb);
}

/* CPU-readout present (24-bit FMV frames and the PSX_GL_FORCE_CPU_PRESENT
 * diagnostic): full-window clear, then a quad into the letterbox rect.
 * force_4_3 pins the rect to native 4:3 regardless of the display aspect —
 * FMVs are authored 4:3 and have no GTE squash to compensate a stretch, so
 * widescreen presents them pillarboxed instead of distorted. */
void gl_renderer_present(const uint32_t *pixels, int src_w, int src_h, int linear,
                         int force_4_3, int content_w) {
    if (!s_ctx) return;
    interp_reset_history();
    int ww = 0, wh = 0; SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, ww, wh);
    glClearColor(0.f,0.f,0.f,1.f); glClear(GL_COLOR_BUFFER_BIT);
    int lx, ly, lw, lh;
    if (force_4_3)
        letterbox_rect_aspect(ww, wh, 4, 3, &lx, &ly, &lw, &lh);
    else
        letterbox_rect(ww, wh, &lx, &ly, &lw, &lh);
    /* Short GP1(07h) bands (MotK FMV is 128 lines) only fill a fraction of
     * NTSC active height on hardware. Stretching them to the full letterbox
     * doubles vertical scale vs horizontal and makes the frame look too wide
     * with the right edge clipped. Letterbox within the present rect instead.
     * Apply whenever the source is short — not only when force_4_3 — so a
     * misclassified FMV frame still keeps correct pixel aspect. */
    if (src_h > 0 && src_h < 240) {
        int content_h = (lh * src_h) / 240;
        if (content_h < 1) content_h = 1;
        ly += (lh - content_h) / 2;
        lh = content_h;
    }
    /* Optional trailing-column crop (depth24 margin): shrink the draw width
     * left-aligned so cleared black remains on the right — never stretch. */
    float uv_x1 = 1.f;
    int crop = (content_w > 0 && content_w < src_w && src_w > 0);
    if (crop) {
        uv_x1 = (float)content_w / (float)src_w;
        lw = (lw * content_w) / src_w;
        if (lw < 1) lw = 1;
    }
    glViewport(lx, ly, lw, lh);
    p_glActiveTexture(PSXGL_TEXTURE0);
    upload_present_tex(pixels, src_w, src_h, linear);
    p_glUseProgram(s_present_prog); p_glUniform1i(s_present_uTex, 0);
    if (crop) {
        p_glUniform4f(s_present_uUvRect, 0.f, 0.f, uv_x1, 1.f);
    } else if (!linear && src_w > 0 && src_h > 0) {
        /* Nearest: half-texel UV inset so UV=1.0 never grazes past the last
         * column into undefined border samples on some drivers. */
        float u0 = 0.5f / (float)src_w, v0 = 0.5f / (float)src_h;
        p_glUniform4f(s_present_uUvRect, u0, v0, 1.f - u0, 1.f - v0);
    } else {
        p_glUniform4f(s_present_uUvRect, 0.f, 0.f, 1.f, 1.f);
    }
    p_glBindVertexArray(s_present_vao); glDrawArrays(GL_TRIANGLES, 0, 3);
    p_glBindVertexArray(0); p_glUseProgram(0);
    pres_record(GL_PRES_CPU, 0, 0, src_w, src_h, lx, ly, lw, lh);
    latency_ring_mark(LAT_SWAP_BEGIN);
    SDL_GL_SwapWindow(s_win);
    latency_ring_mark(LAT_SWAP_END);
    present_force_consumed();
    s_last_present_path = GL_PRES_CPU;
}

void gl_renderer_present_blank(void) {
    if (!s_ctx) return;
    interp_reset_history();
    int ww = 0, wh = 0; SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, ww, wh); glClearColor(0.f,0.f,0.f,1.f); glClear(GL_COLOR_BUFFER_BIT);
    pres_record(GL_PRES_BLANK, 0, 0, 0, 0, 0, 0, ww, wh);
    latency_ring_mark(LAT_SWAP_BEGIN);
    SDL_GL_SwapWindow(s_win);
    latency_ring_mark(LAT_SWAP_END);
    present_force_consumed();
    s_last_present_path = GL_PRES_BLANK;
}

/* Sync the authoritative FBO down into CPU VRAM (no-op when current).
 * Screenshots / debug server. Not for 24-bit FMV scanout (see flush). */
void gl_renderer_sync_cpu(void) {
    ensure_cpu();
}

void gl_renderer_invalidate_present(void) {
    for (int i = 0; i < PRES_ROWS; i++) s_present_dirty[i] = ~0ull;
    s_last_present_path = -1;
    s_force_present_remaining = 8;
    interp_reset_history();
}

void gl_renderer_flush_cpu_uploads(void) {
    if (!s_raster_ok) return;
    flush_flat_batch();
    flush_tex_batch();
    flush_cpu_upload();
}

/* Diagnostic (debug server "gl_fbo_peek"): read a rect of the GPU-side
 * authoritative VRAM (via the pack pass + raw mirror) WITHOUT writing CPU
 * VRAM — lets a probe diff FBO truth against CPU truth. Returns 0 when the
 * GL pipeline is inactive (software backend). */
int gl_renderer_fbo_peek(int x, int y, int w, int h, uint16_t *out) {
    if (!s_raster_ok || !s_ctx) return 0;
    if (x < 0 || y < 0 || w < 1 || h < 1 ||
        x + w > VRAM_W || y + h > VRAM_H) return 0;
    flush_cpu_upload();
    rect_add(&s_pack_dirty, x, y, x + w - 1, y + h - 1);
    pack_flush();
    coh_record(GL_COH_PEEK, x, y, x + w - 1, y + h - 1);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_raw_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 2);
    glReadPixels(x, y, w, h, PSXGL_RED_INTEGER, GL_UNSIGNED_SHORT, out);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    return 1;
}

/* Diagnostic (debug server "gl_vram_diff"): full-VRAM comparison of the
 * GPU-side truth (FBO via pack) against the CPU array, WITHOUT writing
 * either. Reports mismatch count + bounding box + a few sample coords.
 * Divergence is expected where the GPU is legitimately ahead (gpu_dirty);
 * at upload-only scenes the two must match exactly. */
int gl_renderer_vram_diff(uint32_t *count, int bbox[4],
                          int samples[8][2], uint16_t samples_px[8][2]) {
    if (!s_raster_ok || !s_ctx) return 0;
    uint16_t *tmp = (uint16_t *)malloc((size_t)VRAM_W * VRAM_H * 2);
    if (!tmp) return 0;
    flush_cpu_upload();
    /* Force a full pack: the diff must read FBO truth even where the
     * raw-mirror invariant (raw == FBO outside s_pack_dirty) is broken —
     * a broken invariant is exactly what this tool hunts. */
    rect_add(&s_pack_dirty, 0, 0, VRAM_W - 1, VRAM_H - 1);
    pack_flush();
    coh_record(GL_COH_DIFF, 0, 0, VRAM_W - 1, VRAM_H - 1);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_raw_fbo);
    glReadPixels(0, 0, VRAM_W, VRAM_H, PSXGL_RED_INTEGER, GL_UNSIGNED_SHORT, tmp);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    uint32_t n = 0;
    int x0 = VRAM_W, y0 = VRAM_H, x1 = -1, y1 = -1, ns = 0;
    for (int y = 0; y < VRAM_H; y++) {
        for (int x = 0; x < VRAM_W; x++) {
            uint16_t f = tmp[y * VRAM_W + x], c = s_vram[y * VRAM_W + x];
            if (f == c) continue;
            n++;
            if (x < x0) x0 = x; if (x > x1) x1 = x;
            if (y < y0) y0 = y; if (y > y1) y1 = y;
            if (ns < 8 && (n % 977) == 1) {  /* spread samples */
                samples[ns][0] = x; samples[ns][1] = y;
                samples_px[ns][0] = f; samples_px[ns][1] = c;
                ns++;
            }
        }
    }
    free(tmp);
    *count = n;
    bbox[0] = x0; bbox[1] = y0; bbox[2] = x1; bbox[3] = y1;
    return 1 + ns;  /* >=1 means valid; ns = samples filled */
}

/* Diagnostic state for the debug server: coherency flags + dirty rects. */
void gl_renderer_diag(int *gpu_dirty, int pending[5], int pack[5]) {
    if (gpu_dirty) *gpu_dirty = s_gpu_dirty;
    if (pending) {
        /* [0] = pending rect count; [1..4] = union bbox (diagnostic only —
         * the flush itself paints the exact rects, never this union). */
        pending[0] = s_up_nrects;
        pending[1] = pending[2] = pending[3] = pending[4] = 0;
        for (int i = 0; i < s_up_nrects; i++) {
            if (i == 0) {
                pending[1] = s_up_rects[i].x0; pending[2] = s_up_rects[i].y0;
                pending[3] = s_up_rects[i].x1; pending[4] = s_up_rects[i].y1;
            } else {
                if (s_up_rects[i].x0 < pending[1]) pending[1] = s_up_rects[i].x0;
                if (s_up_rects[i].y0 < pending[2]) pending[2] = s_up_rects[i].y0;
                if (s_up_rects[i].x1 > pending[3]) pending[3] = s_up_rects[i].x1;
                if (s_up_rects[i].y1 > pending[4]) pending[4] = s_up_rects[i].y1;
            }
        }
    }
    if (pack) {
        pack[0] = s_pack_dirty.set;
        pack[1] = s_pack_dirty.x0; pack[2] = s_pack_dirty.y0;
        pack[3] = s_pack_dirty.x1; pack[4] = s_pack_dirty.y1;
    }
}

/* ------------------------------------------------------------------------- *
 * Native-wide compositor (GL). Mirrors gpu_sw_renderer.c's wide functions:
 * canonical VRAM (the hr FBO) is untouched; framebuffer draws are also mirrored
 * into per-base_x wide FBOs (see the draw funcs' wide passes). Present reads the
 * displayed buffer's wide FBO via glReadPixels into the CPU present buffer, then
 * the existing CPU present path uploads/letterboxes it (Option B: reuse the CPU
 * present path). Self-gates on the GL pipeline being live; never runs for
 * 4:3 / non-opted games (gpu.c never calls wide_configure for those).
 * ------------------------------------------------------------------------- */

static void wide_free_all(void) {
    for (int i = 0; i < WIDE_MAX_SURF; i++) {
        if (s_wide_fbo[i]) { p_glDeleteFramebuffers(1, &s_wide_fbo[i]); s_wide_fbo[i] = 0; }
        if (s_wide_tex[i]) { glDeleteTextures(1, &s_wide_tex[i]); s_wide_tex[i] = 0; }
        if (s_wide_rb[i])  { p_glDeleteRenderbuffers(1, &s_wide_rb[i]); s_wide_rb[i] = 0; }
        s_wide_base[i] = -1;
    }
    g_wide_cur = 0;
}

/* Lazily allocate (or find) the wide FBO+tex for base_x. Returns the FBO id, or
 * 0 on failure / more distinct buffers than WIDE_MAX_SURF. */
static GLuint wide_fbo_for(int base_x) {
    if (g_wide_w <= 0) return 0;
    for (int i = 0; i < WIDE_MAX_SURF; i++)
        if (s_wide_fbo[i] && s_wide_base[i] == base_x) return s_wide_fbo[i];
    for (int i = 0; i < WIDE_MAX_SURF; i++) {
        if (!s_wide_fbo[i]) {
            int w = g_wide_w * s_scale, h = VRAM_H * s_scale;
            s_cw_fbo_creates++;
            s_wide_tex[i] = make_tex(GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE);
            /* Depth-stencil RB, same as the hr FBO: the stencil carries the
             * PSX mask-bit mirror for the wide surface, and (the hard lesson)
             * a stencil-less FBO turns every stencil-enabled mirror draw into
             * ~0.6ms of driver-side work — the 16:9 GL perf collapse. */
            p_glGenRenderbuffers(1, &s_wide_rb[i]);
            p_glBindRenderbuffer(PSXGL_RENDERBUFFER, s_wide_rb[i]);
            p_glRenderbufferStorage(PSXGL_RENDERBUFFER, PSXGL_DEPTH24_STENCIL8, w, h);
            if (!make_fbo(&s_wide_fbo[i], s_wide_tex[i], s_wide_rb[i])) {
                glDeleteTextures(1, &s_wide_tex[i]); s_wide_tex[i] = 0;
                p_glDeleteRenderbuffers(1, &s_wide_rb[i]); s_wide_rb[i] = 0;
                return 0;
            }
            /* Clear to black so unwritten margins are clean (not stale). */
            p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_wide_fbo[i]);
            glDisable(GL_SCISSOR_TEST);
            glClearColor(0, 0, 0, 0);
            glClearStencil(0);
            glStencilMask(0xFF);
            glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
            s_wide_base[i] = base_x;
            return s_wide_fbo[i];
        }
    }
    return 0;  /* more distinct buffers than WIDE_MAX_SURF — shouldn't happen */
}

/* Enable native-wide with a wide width + centering offset (native px), or
 * disable (wide_w <= 0). Re-allocates if the width changed. Mirrors
 * sw_wide_configure. */
static void glb_wide_configure(int wide_w, int offset) {
    if (!s_raster_ok) return;
    double t0 = cw_ms(); s_cw_wide_cfgs++;
    flush_tex_batch();   /* a queued batch's wide mirror targets the CURRENT surfaces */
    if (wide_w <= 0) { wide_free_all(); g_wide_w = 0; g_wide_off = 0; s_cw_wide_ms += cw_ms() - t0; return; }
    if (wide_w != g_wide_w) wide_free_all();
    g_wide_w = wide_w;
    g_wide_off = offset;
    s_cw_wide_ms += cw_ms() - t0;
}

/* Select the wide surface to mirror into for the back buffer at base_x. */
static void glb_wide_set_target(int base_x) {
    if (!s_raster_ok) { g_wide_cur = 0; return; }
    double t0 = cw_ms(); s_cw_wide_sets++;
    flush_tex_batch();   /* drain into the OLD target before switching */
    g_wide_cur = wide_fbo_for(base_x);
    g_wide_cur_base = base_x;
    s_cw_wide_ms += cw_ms() - t0;
}

/* Stop mirroring (offscreen draws that don't target a framebuffer). */
static void glb_wide_disable_target(void) { flush_tex_batch(); g_wide_cur = 0; }

/* Mirror a framebuffer clear: fill the full wide width over [y, y+h) of the
 * surface for base_x, so the revealed margins are clean. Mirrors sw_wide_clear:
 * a scissored glClear with the 1555 color converted to RGBA8 (alpha = bit15). */
static void glb_wide_clear(int base_x, int y, int h, uint16_t color) {
    if (!s_raster_ok || s_ws_ablate == 1) return;
    double t0 = cw_ms(); s_cw_wide_clears++;
    flush_tex_batch();
    GLuint fbo = wide_fbo_for(base_x);
    if (!fbo) { s_cw_wide_ms += cw_ms() - t0; return; }
    gl_perf_mirror_begin();
    int H = VRAM_H * s_scale;
    int y0 = y * s_scale, y1 = (y + h) * s_scale;
    if (y0 < 0) y0 = 0;
    if (y1 > H) y1 = H;
    if (y1 <= y0) return;
    float r = (color & 0x1F) / 31.0f;
    float g = ((color >> 5) & 0x1F) / 31.0f;
    float b = ((color >> 10) & 0x1F) / 31.0f;
    float a = (color >> 15) & 1 ? 1.0f : 0.0f;
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, fbo);
    glViewport(0, 0, g_wide_w * s_scale, H);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, y0, g_wide_w * s_scale, y1 - y0);
    glClearColor(r, g, b, a);
    glClearStencil((color >> 15) & 1);   /* stencil mirrors bit15, like the color alpha */
    glStencilMask(0xFF);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    gl_perf_mirror_end();
    s_cw_wide_ms += cw_ms() - t0;
}

/* Clear only the two synthetic reveal strips, preserving the centred canonical
 * framebuffer. This is an opt-in transition cleanup driven by gpu.c. */
static void glb_wide_clear_margins(int base_x, int y, int h, uint16_t color, int sides) {
    if (!s_raster_ok || s_ws_ablate == 1 || g_wide_off <= 0) return;
    double t0 = cw_ms(); s_cw_wide_clears++;
    flush_tex_batch();
    GLuint fbo = wide_fbo_for(base_x);
    if (!fbo) { s_cw_wide_ms += cw_ms() - t0; return; }
    gl_perf_mirror_begin();
    int H = VRAM_H * s_scale;
    int W = g_wide_w * s_scale;
    int margin = g_wide_off * s_scale;
    int y0 = y * s_scale, y1 = (y + h) * s_scale;
    if (y0 < 0) y0 = 0;
    if (y1 > H) y1 = H;
    if (y1 <= y0 || margin * 2 >= W) {
        gl_perf_mirror_end();
        s_cw_wide_ms += cw_ms() - t0;
        return;
    }
    float r = (color & 0x1F) / 31.0f;
    float g = ((color >> 5) & 0x1F) / 31.0f;
    float b = ((color >> 10) & 0x1F) / 31.0f;
    float a = (color >> 15) & 1 ? 1.0f : 0.0f;
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, fbo);
    glViewport(0, 0, W, H);
    glEnable(GL_SCISSOR_TEST);
    glClearColor(r, g, b, a);
    glClearStencil((color >> 15) & 1);
    glStencilMask(0xFF);
    if (sides & 1) {
        glScissor(0, y0, margin, y1 - y0);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
    if (sides & 2) {
        glScissor(W - margin, y0, margin, y1 - y0);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
    glDisable(GL_SCISSOR_TEST);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    gl_perf_mirror_end();
    s_cw_wide_ms += cw_ms() - t0;
}

/* Present source: read the wide FBO for the displayed buffer (base_x) into the
 * CPU present buffer as ARGB8888, byte-identical to sw_render_wide_display's
 * output so the shared CPU present path consumes it the same way. Output is
 * (g_wide_w*scale) wide × (disp_h*scale) tall. Returns pixel count (>0), or 0
 * if no surface exists for base_x (caller falls back to the canonical present).
 *
 * PIXEL FORMAT: glReadPixels(GL_BGRA, GL_UNSIGNED_BYTE) yields, per pixel, the
 * little-endian uint32 0xAARRGGBB == ARGB8888 — exactly what rgb555_to_argb
 * produces and what upload_present_tex feeds to glTexImage2D(GL_BGRA,...). The
 * SW path forces alpha to 0xFF; we OR it in to match (present ignores alpha, but
 * we keep the two paths bit-identical). GL's read origin is bottom-left, so the
 * block is read bottom-to-top and copied into `out` reversed (out row 0 = the
 * display's top scanline, as the SW path and the PRESENT_VS V-flip expect). */
static int glb_render_wide_display(uint32_t *out, int pitch, int base_x,
                                   int disp_y, int disp_h) {
    if (!s_raster_ok || !s_ctx || g_wide_w <= 0) return 0;
    GLuint fbo = 0;
    for (int i = 0; i < WIDE_MAX_SURF; i++)
        if (s_wide_fbo[i] && s_wide_base[i] == base_x) { fbo = s_wide_fbo[i]; break; }
    if (!fbo) return 0;

    /* Fold any pending CPU->VRAM uploads into the canonical FBO first (uploads
     * are never mirrored to wide, but draws after them are; keep op order) and
     * make sure all wide-FBO draws have completed before the readback. */
    flush_tex_batch();
    flush_cpu_upload();
    wide_blit_center(fbo, base_x, disp_y, disp_h);   /* fast-path: authoritative centre before readback */
    glFinish();

    int W = g_wide_w * s_scale;
    int H = VRAM_H * s_scale;
    int out_h = disp_h * s_scale;
    int ry0 = disp_y * s_scale;
    if (ry0 < 0) ry0 = 0;
    if (ry0 + out_h > H) out_h = H - ry0;
    if (out_h <= 0) return 0;

    uint32_t *tmp = (uint32_t *)malloc((size_t)W * out_h * sizeof(uint32_t));
    if (!tmp) return 0;
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, ry0, W, out_h, GL_BGRA, GL_UNSIGNED_BYTE, tmp);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);

    /* Orientation: the wide FBO stores PS1 y inverted (geo shader maps vram_y=0
     * to NDC y=-1 = FBO BOTTOM), and glReadPixels reads bottom-up — the two
     * inversions CANCEL, so glReadPixels row 0 already = PS1 top scanline. Copy
     * straight (NO flip) so `out` is top-down, matching sw_render_wide_display
     * (which the shared CPU present path + its PRESENT_VS V-flip expect). Force
     * alpha = 0xFF (match SW). [An earlier reversal here made the frame
     * upside-down.] */
    int count = 0;
    for (int row = 0; row < out_h; row++) {
        const uint32_t *src = tmp + (size_t)row * W;
        uint32_t *dst = (uint32_t *)((uint8_t *)out + (size_t)row * pitch);
        for (int col = 0; col < W; col++) { dst[col] = src[col] | 0xFF000000u; count++; }
    }
    free(tmp);
    return count;
}

/* Dump the ENTIRE wide compositor surface for base_x (all double-buffer bands +
 * both reveal margins), g_wide_w x VRAM_H at scale. Debug/inspection tool (TCP
 * wide_full) — the GL analog of sw_wide_dump_full, so native-wide can be
 * inspected without touching the game window. Runs the same authoritative
 * centre blit first so the dump matches what present shows. Top-down, alpha=FF
 * (matches sw_wide_dump_full / render_wide_display orientation). */
static int glb_wide_dump_full(uint32_t *out, int cap_pixels, int *ow, int *oh,
                              int base_x) {
    if (!s_raster_ok || !s_ctx || g_wide_w <= 0) return 0;
    GLuint fbo = 0;
    for (int i = 0; i < WIDE_MAX_SURF; i++)
        if (s_wide_fbo[i] && s_wide_base[i] == base_x) { fbo = s_wide_fbo[i]; break; }
    if (!fbo) return 0;
    flush_tex_batch();
    flush_cpu_upload();
    wide_blit_center(fbo, base_x, 0, VRAM_H);   /* authoritative centre (full height) */
    glFinish();
    int W = g_wide_w * s_scale;
    int H = VRAM_H * s_scale;
    if (cap_pixels > 0 && (long)W * H > cap_pixels) { H = cap_pixels / W; if (H <= 0) return 0; }
    uint32_t *tmp = (uint32_t *)malloc((size_t)W * H * sizeof(uint32_t));
    if (!tmp) return 0;
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, W, H, GL_BGRA, GL_UNSIGNED_BYTE, tmp);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    /* Same orientation reasoning as glb_render_wide_display: glReadPixels row 0 =
     * PS1 top scanline, so copy straight (top-down). */
    int count = 0;
    for (int i = 0; i < W * H; i++) { out[i] = tmp[i] | 0xFF000000u; count++; }
    free(tmp);
    if (ow) *ow = W;
    if (oh) *oh = H;
    return count;
}

/* THE present path for 15-bit frames: blit the display region from the
 * authoritative hr FBO into a letterboxed rect. Deterministic — runs
 * every 15-bit frame regardless of what mix of ops produced it.
 * force_4_3 pins to native 4:3 (15-bit MDEC FMV frames on a wide aspect). */
/* ===================== frame_perf: per-frame GPU/CPU phase timing ============
 * Developer builds use two GL_TIME_ELAPSED queries per frame to bracket (a) the
 * scene draws (all GP0 raster issued between two presents) and (b) the present
 * clear+blit, giving TRUE GPU time per phase independent of CPU/GPU overlap
 * (glFinish would only catch the non-overlapped tail and mislead). CPU wall time
 * (present-to-present total, and the present call) comes from SDL perf counters.
 * Results are read back GLPERF_NBUF frames late (no pipeline stall) into a ring
 * the debug server's frame_perf command aggregates. Release builds compile out
 * the debug server, so they also leave this instrumentation disabled: a native-
 * wide frame can otherwise issue hundreds of unused mirror timestamp queries.
 * One question this answers in a diagnostics build:
 * where does a 16:9 frame go vs 4:3 — scene fill, wide composite, or CPU. */
#define GLPERF_NBUF 4
#define GLPERF_RING 256
typedef struct {
    double   total_ms;        /* present-entry to present-entry (full frame)  */
    double   present_wall_ms; /* CPU wall time inside the present call         */
    double   scene_gpu_ms;    /* GPU: all scene draws this frame               */
    double   present_gpu_ms;  /* GPU: the present clear+blit                   */
    double   mirror_gpu_ms;   /* GPU: of scene_gpu, the native-wide mirror passes */
    double   prims;           /* scene primitives submitted this frame         */
    double   mirror_passes;   /* mirror passes this frame (measured + overflow) */
    double   cw_flush_ms;     /* CPU wall inside flush_tex_batch this frame    */
    double   cw_wide_ms;      /* CPU wall inside glb_wide_* this frame         */
    double   batches;         /* flush_tex_batch draws this frame              */
    double   wide_sets;       /* glb_wide_set_target calls this frame          */
    double   fbo_creates;     /* wide FBO+tex creations this frame             */
    int      wide;            /* 1 = native-wide (16:9) present, 0 = 4:3       */
    uint64_t frame;
} GlPerfSample;

/* Native-wide mirror-pass GPU attribution: the scene TIME_ELAPSED query spans
 * the whole frame (queries of one target cannot nest), so each mirror pass is
 * bracketed with a GL_TIMESTAMP pair instead (glQueryCounter does not conflict
 * with an active TIME_ELAPSED query). Pairs are pooled per buffered frame and
 * summed at readback, splitting scene_gpu into canonical vs mirror cost. */
#define GLPERF_MIRQ 1024               /* measured mirror passes per frame */
static GLuint s_mq_q[GLPERF_NBUF][GLPERF_MIRQ * 2];
static int    s_mq_n[GLPERF_NBUF];     /* pairs recorded this frame        */
static int    s_mq_over[GLPERF_NBUF];  /* passes beyond the pool (counted, untimed) */
static int    s_mq_open = 0;           /* begin issued, end pending        */
static int    s_mq_ok = 0;             /* glQueryCounter available         */

static int      s_pf_on = 0;
static GLuint   s_pf_scene_q[GLPERF_NBUF];
static GLuint   s_pf_present_q[GLPERF_NBUF];
static int      s_pf_b = 0;            /* buffer for the CURRENT frame         */
static int      s_pf_scene_active = 0;
static uint64_t s_pf_count = 0;
static uint64_t s_pf_freq = 1;
static uint64_t s_pf_last_enter = 0;
static uint64_t s_pf_enter = 0;
static double   s_pf_total_pending = 0.0;
static double   s_pf_buf_total[GLPERF_NBUF];
static double   s_pf_buf_pwall[GLPERF_NBUF];
static double   s_pf_buf_prims[GLPERF_NBUF];
static int      s_pf_buf_wide[GLPERF_NBUF];
static uint64_t s_pf_buf_frame[GLPERF_NBUF];
static uint64_t s_pf_prims_last = 0;
static double   s_pf_prims_pending = 0.0;
static double   s_pf_cw_pending[5];            /* flush_ms, wide_ms, batches, wide_sets, fbo_creates */
static double   s_pf_buf_cw[GLPERF_NBUF][5];
static GlPerfSample s_pf_ring[GLPERF_RING];
static uint64_t     s_pf_ring_seq = 0;

static void gl_perf_init(void) {
#ifdef PSX_NO_DEBUG_TOOLS
    return;
#else
    /* Timer queries are intentionally available in diagnostics builds, but a
     * driver may serialize command submission while collecting them.  Keep an
     * escape hatch so frame cadence can be A/B tested without rebuilding or
     * losing the rest of the debug server telemetry. */
    {
        const char *enabled = getenv("PSX_GL_PERF");
        if (enabled && enabled[0] == '0') return;
    }
    if (!p_glGenQueries || !p_glBeginQuery || !p_glEndQuery || !p_glGetQueryObjectui64v) return;
    p_glGenQueries(GLPERF_NBUF, s_pf_scene_q);
    p_glGenQueries(GLPERF_NBUF, s_pf_present_q);
    s_mq_ok = (p_glQueryCounter != NULL);
    if (s_mq_ok)
        for (int i = 0; i < GLPERF_NBUF; i++) {
            p_glGenQueries(GLPERF_MIRQ * 2, s_mq_q[i]);
            s_mq_n[i] = 0; s_mq_over[i] = 0;
        }
    s_mq_open = 0;
    s_pf_freq = SDL_GetPerformanceFrequency();
    if (!s_pf_freq) s_pf_freq = 1;
    s_pf_b = 0; s_pf_scene_active = 0; s_pf_count = 0; s_pf_ring_seq = 0;
    s_pf_last_enter = 0;
    s_pf_on = 1;
#endif
}

/* Bracket ONE native-wide mirror pass (called from the wide-mirror draw sites).
 * Timestamp pairs, not TIME_ELAPSED — see the pool comment above. */
static void gl_perf_mirror_begin(void) {
    if (!s_pf_on || !s_mq_ok) return;
    int b = s_pf_b;
    if (s_mq_n[b] >= GLPERF_MIRQ) { s_mq_over[b]++; return; }
    p_glQueryCounter(s_mq_q[b][s_mq_n[b] * 2], GL_TIMESTAMP);
    s_mq_open = 1;
}
static void gl_perf_mirror_end(void) {
    if (!s_pf_on || !s_mq_ok || !s_mq_open) return;
    int b = s_pf_b;
    p_glQueryCounter(s_mq_q[b][s_mq_n[b] * 2 + 1], GL_TIMESTAMP);
    s_mq_n[b]++;
    s_mq_open = 0;
}

/* Top of present (after flush_cpu_upload, before clear/blit). */
static void gl_perf_present_enter(void) {
    /* Per-frame boundary for the 2D-backdrop stretch — runs from BOTH present
     * paths (4:3 present_vram AND native-wide present_wide_fbo), before the
     * perf-on gate so native-wide frames reset too. Snapshot this frame's
     * backdrop-stretch diagnostics, then reset the per-frame counters. (The gate
     * is per-prim now — no draw-order phase to reset.) Final batch already flushed
     * by the caller. */
    g_bdg_applied = s_bdg_applied; g_bdg_prims = s_bdg_prims; g_bdg_clearx = s_bdg_clearx;
    g_bdg_cur = (g_wide_cur != 0); g_bdg_base = g_wide_cur_base; g_bdg_w = g_wide_w; g_bdg_off = g_wide_off;
    s_bdg_applied = 0; s_bdg_prims = 0; s_bdg_clearx = -999999;
    { extern void psx_ws_dbg_gate_frame_snapshot(void); psx_ws_dbg_gate_frame_snapshot(); }
    if (!s_pf_on) return;
    uint64_t now = SDL_GetPerformanceCounter();
    s_pf_enter = now;
    s_pf_total_pending = s_pf_last_enter
        ? (double)(now - s_pf_last_enter) * 1000.0 / (double)s_pf_freq : 0.0;
    s_pf_last_enter = now;
    s_pf_prims_pending = (double)(s_scene_prims - s_pf_prims_last);   /* prims drawn this frame */
    s_pf_prims_last = s_scene_prims;
    s_pf_cw_pending[0] = s_cw_flush_ms;  s_pf_cw_pending[1] = s_cw_wide_ms;
    s_pf_cw_pending[2] = (double)s_cw_batches;
    s_pf_cw_pending[3] = (double)s_cw_wide_sets;
    s_pf_cw_pending[4] = (double)s_cw_fbo_creates;
    s_cw_flush_ms = 0.0; s_cw_wide_ms = 0.0;
    s_cw_batches = 0; s_cw_wide_sets = 0; s_cw_wide_cfgs = 0;
    s_cw_wide_clears = 0; s_cw_fbo_creates = 0;
    if (s_pf_scene_active) { p_glEndQuery(GL_TIME_ELAPSED); s_pf_scene_active = 0; } /* end frame b's scene draws */
    p_glBeginQuery(GL_TIME_ELAPSED, s_pf_present_q[s_pf_b]);                         /* time frame b's present */
}

/* End of present (after SwapWindow). wide = native-wide path. */
static void gl_perf_present_exit(int wide) {
    if (!s_pf_on) return;
    uint64_t now = SDL_GetPerformanceCounter();
    p_glEndQuery(GL_TIME_ELAPSED);   /* end present_q[b] */
    s_pf_buf_total[s_pf_b] = s_pf_total_pending;
    s_pf_buf_pwall[s_pf_b] = (double)(now - s_pf_enter) * 1000.0 / (double)s_pf_freq;
    s_pf_buf_prims[s_pf_b] = s_pf_prims_pending;
    for (int ci = 0; ci < 5; ci++) s_pf_buf_cw[s_pf_b][ci] = s_pf_cw_pending[ci];
    s_pf_buf_wide[s_pf_b]  = wide;
    s_pf_buf_frame[s_pf_b] = s_pf_count;
    int rd = (s_pf_b + 1) % GLPERF_NBUF;   /* oldest buffer (frame count+1-NBUF), now done */
    if (s_pf_count >= (uint64_t)GLPERF_NBUF) {
        GLuint64 sc = 0, pr = 0;
        p_glGetQueryObjectui64v(s_pf_scene_q[rd],   GL_QUERY_RESULT, &sc);
        p_glGetQueryObjectui64v(s_pf_present_q[rd], GL_QUERY_RESULT, &pr);
        double mir = 0.0;
        for (int i = 0; i < s_mq_n[rd]; i++) {   /* sum that frame's mirror pairs */
            GLuint64 t0 = 0, t1 = 0;
            p_glGetQueryObjectui64v(s_mq_q[rd][i * 2],     GL_QUERY_RESULT, &t0);
            p_glGetQueryObjectui64v(s_mq_q[rd][i * 2 + 1], GL_QUERY_RESULT, &t1);
            if (t1 > t0) mir += (double)(t1 - t0);
        }
        GlPerfSample *s = &s_pf_ring[s_pf_ring_seq % GLPERF_RING];
        s->total_ms        = s_pf_buf_total[rd];
        s->present_wall_ms = s_pf_buf_pwall[rd];
        s->scene_gpu_ms    = (double)sc / 1.0e6;
        s->present_gpu_ms  = (double)pr / 1.0e6;
        s->mirror_gpu_ms   = mir / 1.0e6;
        s->prims           = s_pf_buf_prims[rd];
        s->mirror_passes   = (double)(s_mq_n[rd] + s_mq_over[rd]);
        s->cw_flush_ms     = s_pf_buf_cw[rd][0];
        s->cw_wide_ms      = s_pf_buf_cw[rd][1];
        s->batches         = s_pf_buf_cw[rd][2];
        s->wide_sets       = s_pf_buf_cw[rd][3];
        s->fbo_creates     = s_pf_buf_cw[rd][4];
        s->wide            = s_pf_buf_wide[rd];
        s->frame           = s_pf_buf_frame[rd];
        s_pf_ring_seq++;
    }
    s_mq_n[rd] = 0; s_mq_over[rd] = 0;   /* rd becomes the next frame's buffer */
    s_pf_count++;
    s_pf_b = rd;                                          /* reuse oldest for next frame */
    p_glBeginQuery(GL_TIME_ELAPSED, s_pf_scene_q[s_pf_b]); /* open next frame's scene draws */
    s_pf_scene_active = 1;
}

/* Aggregate the ring for the debug server. wide_filter: -1 all, 0 = 4:3, 1 = wide.
 * out[0]=count, [1]=total_avg, [2]=total_max, [3]=emu_cpu_avg (total-present_wall),
 * [4]=present_wall_avg, [5]=scene_gpu_avg, [6]=scene_gpu_max, [7]=present_gpu_avg,
 * [8]=present_gpu_max. Returns the sample count. */
/* Cumulative textured fraction of scene prims (decides flat vs textured batching
 * priority). out_tex_frac = textured/total since boot; returns total prim count. */
uint64_t gl_renderer_perf_prim_split(double *out_tex_frac) {
    if (out_tex_frac) *out_tex_frac = s_scene_prims ? (double)s_scene_prims_tex / (double)s_scene_prims : 0.0;
    return s_scene_prims;
}

int gl_renderer_perf_aggregate(int wide_filter, double out[18]) {
    for (int i = 0; i < 18; i++) out[i] = 0.0;
    if (!s_pf_on) return 0;
    int navail = (int)(s_pf_ring_seq < (uint64_t)GLPERF_RING ? s_pf_ring_seq : GLPERF_RING);
    uint64_t start = s_pf_ring_seq - (uint64_t)navail;
    int n = 0;
    for (int i = 0; i < navail; i++) {
        const GlPerfSample *s = &s_pf_ring[(start + i) % GLPERF_RING];
        if (wide_filter >= 0 && s->wide != wide_filter) continue;
        double emu = s->total_ms - s->present_wall_ms; if (emu < 0) emu = 0;
        out[1] += s->total_ms;       if (s->total_ms     > out[2]) out[2] = s->total_ms;
        out[3] += emu;
        out[4] += s->present_wall_ms;
        out[5] += s->scene_gpu_ms;   if (s->scene_gpu_ms > out[6]) out[6] = s->scene_gpu_ms;
        out[7] += s->present_gpu_ms; if (s->present_gpu_ms > out[8]) out[8] = s->present_gpu_ms;
        out[9] += s->prims;
        out[10] += s->mirror_gpu_ms; if (s->mirror_gpu_ms > out[11]) out[11] = s->mirror_gpu_ms;
        out[12] += s->mirror_passes;
        out[13] += s->cw_flush_ms;
        out[14] += s->cw_wide_ms;
        out[15] += s->batches;
        out[16] += s->wide_sets;
        out[17] += s->fbo_creates;
        n++;
    }
    if (n) {
        out[1]/=n; out[3]/=n; out[4]/=n; out[5]/=n; out[7]/=n; out[9]/=n; out[10]/=n; out[12]/=n;
        out[13]/=n; out[14]/=n; out[15]/=n; out[16]/=n; out[17]/=n;
    }
    out[0] = (double)n;
    return n;
}

/* Native-wide mirror ablation (perf attribution): see s_ws_ablate. */
void gl_renderer_set_ws_ablate(int mode) { s_ws_ablate = (mode >= 0 && mode <= 3) ? mode : 0; }
int  gl_renderer_get_ws_ablate(void)     { return s_ws_ablate; }

static void interp_reset_history_unlocked(void) {
    s_interp_valid = 0;
    s_interp_w = s_interp_h = 0;
    s_interp_start = s_interp_last_capture = 0;
    s_interp_duration = 1;
    s_interp_source_path = -1;
}

static void interp_reset_history(void) {
    if (s_interp_mutex) SDL_LockMutex(s_interp_mutex);
    interp_reset_history_unlocked();
    if (s_interp_mutex) SDL_UnlockMutex(s_interp_mutex);
}

void gl_renderer_set_interpolation(int enabled, double host_hz, double target_hz) {
    int active = (enabled && host_hz >= 90.0) ? 1 : 0;
    double effective_hz = target_hz >= 90.0 ? target_hz : host_hz;
    const char *diag = getenv("PSX_GL_INTERP_DIAG");
    s_interp_diag = diag && diag[0] && diag[0] != '0';
    if (active && !s_interp_ctx && s_ctx) {
        if (!s_interp_mutex) s_interp_mutex = SDL_CreateMutex();
        SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
        s_interp_ctx = SDL_GL_CreateContext(s_win);
        SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
        SDL_GL_MakeCurrent(s_win, s_ctx);
        /* The wall-clock pacer and interpolation scheduler own cadence. Keeping
         * driver vsync on the main context adds a 6-11 ms block whenever an
         * FMV temporarily suspends interpolation and the main context presents. */
        SDL_GL_SetSwapInterval(0);
        if (s_interp_ctx && s_interp_mutex) {
            SDL_AtomicSet(&s_interp_thread_run, 1);
            s_interp_thread = SDL_CreateThread(interp_thread_main,
                                               "psx-gl-interp", NULL);
        }
        if (!s_interp_thread) {
            SDL_AtomicSet(&s_interp_thread_run, 0);
            if (s_interp_ctx) SDL_GL_DeleteContext(s_interp_ctx);
            s_interp_ctx = NULL;
            active = 0;
        }
    }
    if (s_interp_mutex) SDL_LockMutex(s_interp_mutex);
    if (active != s_interp_enabled) interp_reset_history_unlocked();
    s_interp_enabled = active;
    s_interp_host_hz = host_hz;
    s_interp_target_hz = active ? effective_hz : 0.0;
    if (s_interp_mutex) SDL_UnlockMutex(s_interp_mutex);
    if (active)
        fprintf(stdout, "psxrecomp: GL frame interpolation enabled: %.1f FPS "
                "target on %.1f Hz display\n", effective_hz, host_hz);
    else
        fprintf(stdout, "psxrecomp: GL frame interpolation disabled (host %.1f Hz)\n", host_hz);
}

void gl_renderer_set_interpolation_suspended(int suspended) {
    suspended = suspended ? 1 : 0;
    if (s_interp_mutex) SDL_LockMutex(s_interp_mutex);
    if (suspended != s_interp_suspended) interp_reset_history_unlocked();
    s_interp_suspended = suspended;
    if (s_interp_mutex) SDL_UnlockMutex(s_interp_mutex);
}

void gl_renderer_interpolation_diag(int *enabled, int *suspended,
                                    int *history_frames,
                                    double *host_hz, double *target_hz,
                                    uint64_t *swaps) {
    if (s_interp_mutex) SDL_LockMutex(s_interp_mutex);
    if (enabled) *enabled = s_interp_enabled;
    if (suspended) *suspended = s_interp_suspended;
    if (history_frames) *history_frames = s_interp_valid;
    if (host_hz) *host_hz = s_interp_host_hz;
    if (target_hz) *target_hz = s_interp_target_hz;
    if (swaps) *swaps = s_interp_swaps;
    if (s_interp_mutex) SDL_UnlockMutex(s_interp_mutex);
}

/* Copy a stable display image out of the mutable VRAM/wide render target.
 * Returns true once both previous and current images are available. */
static int interp_capture(GLuint fbo, int x, int y, int w, int h,
                          int linear, int force_4_3, int source_path) {
    if (!s_interp_enabled || s_interp_suspended || !fbo || w <= 0 || h <= 0) return 0;
    SDL_LockMutex(s_interp_mutex);
    /* The presentation context may still have a draw queued which samples one
     * of the shared history textures.  Order this context's next allocation or
     * copy after that draw before recycling a texture.  glWaitSync keeps the
     * dependency on the GPU; unlike glFinish it does not stall the guest CPU. */
    if (s_interp_draw_fence) {
        p_glWaitSync(s_interp_draw_fence, 0, PSXGL_TIMEOUT_IGNORED);
        p_glDeleteSync(s_interp_draw_fence);
        s_interp_draw_fence = NULL;
    }
    int pw = w * s_scale, ph = h * s_scale;
    if (pw != s_interp_w || ph != s_interp_h ||
        source_path != s_interp_source_path || force_4_3 != s_interp_force_4_3) {
        s_interp_valid = 0;
        s_interp_w = pw; s_interp_h = ph;
        s_interp_prev = s_interp_cur = 0;
        for (int i = 0; i < 3; i++) {
            glBindTexture(GL_TEXTURE_2D, s_interp_tex[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pw, ph, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        }
    }

    int dst = 0;
    if (s_interp_valid == 1) dst = s_interp_cur == 0 ? 1 : 0;
    else if (s_interp_valid >= 2) dst = s_interp_prev;
    if (s_interp_fence[dst]) {
        p_glDeleteSync(s_interp_fence[dst]);
        s_interp_fence[dst] = NULL;
    }

    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, fbo);
    glBindTexture(GL_TEXTURE_2D, s_interp_tex[dst]);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                        x * s_scale, y * s_scale, pw, ph);
    s_interp_fence[dst] = p_glFenceSync(PSXGL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);

    uint64_t now = SDL_GetPerformanceCounter();
    uint64_t freq = SDL_GetPerformanceFrequency();
    if (s_interp_valid > 0 && s_interp_last_capture && now > s_interp_last_capture) {
        uint64_t d = now - s_interp_last_capture;
        uint64_t lo = freq / 240u, hi = freq / 10u;
        if (d < lo) d = lo;
        if (d > hi) d = hi;
        s_interp_duration = d;
    }
    s_interp_last_capture = now;
    s_interp_start = now;
    if (s_interp_valid == 0) {
        s_interp_cur = dst;
        s_interp_valid = 1;
    } else {
        s_interp_prev = s_interp_cur;
        s_interp_cur = dst;
        s_interp_valid = 2;
    }
    s_interp_linear = linear;
    s_interp_force_4_3 = force_4_3;
    s_interp_source_path = source_path;
    s_interp_captures++;
    int ready = s_interp_valid >= 2;
    SDL_UnlockMutex(s_interp_mutex);
    return ready;
}

static void interp_draw_quad(float alpha, int lx, int ly, int lw, int lh) {
    int prev = s_interp_prev, curr = s_interp_cur;
    if (s_interp_fence[prev])
        p_glWaitSync(s_interp_fence[prev], 0, PSXGL_TIMEOUT_IGNORED);
    if (s_interp_fence[curr])
        p_glWaitSync(s_interp_fence[curr], 0, PSXGL_TIMEOUT_IGNORED);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    glViewport(lx, ly, lw, lh);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_interp_tex[prev]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, s_interp_linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, s_interp_linear ? GL_LINEAR : GL_NEAREST);
    p_glActiveTexture(PSXGL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, s_interp_tex[curr]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, s_interp_linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, s_interp_linear ? GL_LINEAR : GL_NEAREST);
    p_glUseProgram(s_interp_prog);
    p_glUniform1i(s_interp_uPrev, 0);
    p_glUniform1i(s_interp_uCurr, 1);
    p_glUniform1f(s_interp_uAlpha, alpha);
    p_glUniform4f(s_interp_uUvRect, 0.f, 0.f, 1.f, 1.f);
    p_glBindVertexArray(s_interp_thread_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    p_glBindVertexArray(0);
    p_glUseProgram(0);
    p_glActiveTexture(PSXGL_TEXTURE0);
}

static int interp_present(void) {
    if (!s_ctx || !s_interp_enabled || s_interp_suspended || s_interp_valid < 2) return 0;
    uint64_t now = SDL_GetPerformanceCounter();
    if (now <= s_interp_start || !s_interp_duration) return 0;
    double a = (double)(now - s_interp_start) / (double)s_interp_duration;
    /* Keep swapping at the host cadence after the blend completes.  Holding
     * alpha at one is visually identical to leaving the current image on the
     * front buffer, but avoids an irregular 2/3-swap pattern on 120/144/165 Hz
     * displays while the next 59.94 Hz guest frame is being produced. */
    if (a > 1.0) a = 1.0;
    if (a < 0.0) a = 0.0;

    int ww = 0, wh = 0; SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    int lx, ly, lw, lh;
    if (s_interp_force_4_3)
        letterbox_rect_aspect(ww, wh, 4, 3, &lx, &ly, &lw, &lh);
    else
        letterbox_rect(ww, wh, &lx, &ly, &lw, &lh);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, ww, wh);
    if (lx != 0 || ly != 0 || lw != ww || lh != wh) {
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    interp_draw_quad((float)a, lx, ly, lw, lh);
    if (s_interp_draw_fence) p_glDeleteSync(s_interp_draw_fence);
    s_interp_draw_fence = p_glFenceSync(PSXGL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    pres_record(GL_PRES_INTERP, 0, 0, s_interp_w, s_interp_h,
                lx, ly, lw, lh);
    SDL_GL_SwapWindow(s_win);
    s_interp_swaps++;
    return 1;
}

static int interp_thread_main(void *opaque) {
    (void)opaque;
    if (SDL_GL_MakeCurrent(s_win, s_interp_ctx) != 0) return -1;
    SDL_GL_SetSwapInterval(0); /* host-period scheduler owns cadence */
    p_glGenVertexArrays(1, &s_interp_thread_vao);
    uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t deadline = SDL_GetPerformanceCounter();
    uint64_t diag_start = deadline, diag_swaps = 0, diag_captures = 0;

    while (SDL_AtomicGet(&s_interp_thread_run)) {
        SDL_LockMutex(s_interp_mutex);
        double hz = s_interp_target_hz >= 90.0 ? s_interp_target_hz : 120.0;
        SDL_UnlockMutex(s_interp_mutex);
        uint64_t period = (uint64_t)((double)freq / hz);
        if (!period) period = 1;
        deadline += period;
        uint64_t now = SDL_GetPerformanceCounter();
        if (now > deadline + period * 4u) deadline = now + period;
        for (;;) {
            now = SDL_GetPerformanceCounter();
            if (now >= deadline) break;
            uint64_t remain = deadline - now;
            uint32_t ms = (uint32_t)((remain * 1000u) / (freq ? freq : 1u));
            if (ms > 1) SDL_Delay(ms - 1);
        }
        while (SDL_GetPerformanceCounter() < deadline) {}

        SDL_LockMutex(s_interp_mutex);
        if (SDL_AtomicGet(&s_interp_thread_run) && s_interp_enabled)
            interp_present();
        if (s_interp_diag && now - diag_start >= freq * 5u) {
            double seconds = (double)(now - diag_start) / (double)freq;
            fprintf(stdout, "psxrecomp: GL interpolation cadence: "
                    "%.2f captures/s, %.2f presents/s\n",
                    (double)(s_interp_captures - diag_captures) / seconds,
                    (double)(s_interp_swaps - diag_swaps) / seconds);
            fflush(stdout);
            diag_start = now;
            diag_captures = s_interp_captures;
            diag_swaps = s_interp_swaps;
        }
        SDL_UnlockMutex(s_interp_mutex);
    }
    p_glBindVertexArray(0);
    SDL_GL_MakeCurrent(s_win, NULL);
    return 0;
}

static void present_target_quad(GLuint tex, int tex_w, int tex_h,
                                int x, int y, int w, int h, int linear,
                                int lx, int ly, int lw, int lh) {
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    glViewport(lx, ly, lw, lh);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    p_glUseProgram(s_present_prog);
    p_glUniform1i(s_present_uTex, 0);
    p_glUniform4f(s_present_uUvRect,
                  (float)x / (float)tex_w, (float)y / (float)tex_h,
                  (float)(x + w) / (float)tex_w, (float)(y + h) / (float)tex_h);
    p_glBindVertexArray(s_present_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    p_glBindVertexArray(0);
    p_glUseProgram(0);
}

void gl_renderer_present_vram(int disp_x, int disp_y, int w, int h, int linear,
                              int force_4_3) {
    if (!s_ctx || !s_raster_ok) return;
    flush_flat_batch();
    flush_tex_batch();
    flush_cpu_upload();
    if (s_force_present_remaining <= 0 &&
        s_last_present_path == GL_PRES_VRAM &&
        s_last_dx == disp_x && s_last_dy == disp_y &&
        s_last_dw == w && s_last_dh == h &&
        !present_dirty_test(disp_x, disp_y, disp_x + w - 1, disp_y + h - 1)) {
        gl_perf_present_enter();
        gl_perf_present_exit(0);
        return;
    }
    gl_perf_present_enter();   /* per-frame backdrop-phase reset + dbg snapshot live in here */
    int ww = 0, wh = 0; SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    int lx, ly, lw, lh;
    if (force_4_3)
        letterbox_rect_aspect(ww, wh, 4, 3, &lx, &ly, &lw, &lh);
    else
        letterbox_rect(ww, wh, &lx, &ly, &lw, &lh);
    /* Match CPU present: short GP1(07h) bands letterbox inside the rect. */
    if (h > 0 && h < 240) {
        int content_h = (lh * h) / 240;
        if (content_h < 1) content_h = 1;
        ly += (lh - content_h) / 2;
        lh = content_h;
    }

    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, ww, wh);
    if (lx != 0 || ly != 0 || lw != ww || lh != wh) {
        glClearColor(0.f,0.f,0.f,1.f); glClear(GL_COLOR_BUFFER_BIT);
    }
    int interp_pair = interp_capture(s_hr_fbo, disp_x, disp_y, w, h,
                                     linear, force_4_3, GL_PRES_VRAM);
    if (interp_pair) {
        gl_perf_present_exit(0);
        present_dirty_rect(disp_x, disp_y, disp_x + w - 1, disp_y + h - 1, 0);
        present_force_consumed();
        s_last_present_path = GL_PRES_VRAM;
        s_last_dx = disp_x; s_last_dy = disp_y; s_last_dw = w; s_last_dh = h;
        return;
    }
    present_target_quad(s_hr_tex, VRAM_W, VRAM_H,
                        disp_x, disp_y, w, h, linear, lx, ly, lw, lh);
    pres_record(GL_PRES_VRAM, disp_x, disp_y, w, h, lx, ly, lw, lh);
    latency_ring_mark(LAT_SWAP_BEGIN);
    SDL_GL_SwapWindow(s_win);
    latency_ring_mark(LAT_SWAP_END);
    gl_perf_present_exit(0);
    present_dirty_rect(disp_x, disp_y, disp_x + w - 1, disp_y + h - 1, 0);
    present_force_consumed();
    s_last_present_path = GL_PRES_VRAM;
    s_last_dx = disp_x; s_last_dy = disp_y; s_last_dw = w; s_last_dh = h;
    coh_record(GL_COH_PRESENT, disp_x, disp_y, disp_x + w - 1, disp_y + h - 1);
}

/* Native-wide fast path: authoritatively copy the canonical 4:3 framebuffer
 * into the wide surface's CENTRE columns [g_wide_off, g_wide_off+native_w) for
 * the displayed Y band, so the per-prim mirror could skip every centre-only
 * prim (the dominant native-wide GPU saving). The reveal margins were already
 * produced by the mirror; this leaves them untouched. One FBO->FBO blit,
 * x-translated by the reveal offset. No-op when s_wide_fast is off (then the
 * mirror drew the full surface, as before). Shared by both present paths. */
static void wide_blit_center(GLuint wide_fbo, int base_x, int disp_y, int disp_h) {
    if (!s_wide_fast || g_wide_w <= 0) return;
    int native_w = g_wide_w - 2 * g_wide_off;
    if (native_w <= 0) return;
    int S = s_scale;
    (void)disp_y; (void)disp_h;
    /* Copy the canonical framebuffer column into the wide surface CENTRE over the
     * FULL VRAM height, not just the current display band [disp_y, disp_y+disp_h].
     * The wide surface holds BOTH vertical double-buffer bands (Ape flips
     * display_y 0<->256), and the game's draw area / display band can differ per
     * scene (the cityscape intro exposed rows outside disp_h). Copying the whole
     * column makes the wide CENTRE bit-identical to what the full mirror would
     * have drawn there for every band, so nothing the present reads is ever left
     * black. The margins (x outside the centre) are untouched. */
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_hr_fbo);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, wide_fbo);
    glDisable(GL_SCISSOR_TEST);
    p_glBlitFramebuffer(base_x * S, 0,
                        (base_x + native_w) * S, VRAM_H * S,
                        g_wide_off * S, 0,
                        (g_wide_off + native_w) * S, VRAM_H * S,
                        GL_COLOR_BUFFER_BIT, GL_NEAREST);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
}

/* GPU-direct native-wide present: blit the displayed buffer's wide FBO straight
 * to the window (no glReadPixels / glFinish CPU round-trip). The wide surface is
 * g_wide_w wide × VRAM_H tall (at scale S); present its [0,g_wide_w] × [disp_y,
 * disp_y+disp_h] region into the letterbox, V-flipped like present_vram (FBO y
 * bottom-origin → window top). Returns 0 if there's no wide surface for base_x
 * (caller falls back). disp_x is the displayed buffer base (the wide-surface key). */
int gl_renderer_present_wide_fbo(int disp_x, int disp_y, int disp_h, int linear) {
    if (!s_ctx || !s_raster_ok || g_wide_w <= 0) return 0;
    GLuint fbo = 0, tex = 0;
    for (int i = 0; i < WIDE_MAX_SURF; i++)
        if (s_wide_fbo[i] && s_wide_base[i] == disp_x) {
            fbo = s_wide_fbo[i]; tex = s_wide_tex[i]; break;
        }
    if (!fbo) return 0;
    flush_flat_batch();
    flush_tex_batch();
    flush_cpu_upload();
    if (s_force_present_remaining <= 0 &&
        s_last_present_path == GL_PRES_WIDE &&
        s_last_dx == disp_x && s_last_dy == disp_y &&
        s_last_dw == g_wide_w && s_last_dh == disp_h &&
        !present_dirty_test(0, disp_y, VRAM_W - 1, disp_y + disp_h - 1)) {
        gl_perf_present_enter();
        gl_perf_present_exit(1);
        return 1;
    }
    gl_perf_present_enter();
    int ww = 0, wh = 0; SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    int lx, ly, lw, lh;
    letterbox_rect(ww, wh, &lx, &ly, &lw, &lh);
    wide_blit_center(fbo, disp_x, disp_y, disp_h);   /* fast-path: authoritative centre */

    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, ww, wh);
    if (lx != 0 || ly != 0 || lw != ww || lh != wh) {
        glClearColor(0.f, 0.f, 0.f, 1.f); glClear(GL_COLOR_BUFFER_BIT);
    }
    int interp_pair = interp_capture(fbo, 0, disp_y, g_wide_w, disp_h,
                                     linear, 0, GL_PRES_WIDE);
    if (interp_pair) {
        gl_perf_present_exit(1);
        present_dirty_rect(0, disp_y, VRAM_W - 1, disp_y + disp_h - 1, 0);
        present_force_consumed();
        s_last_present_path = GL_PRES_WIDE;
        s_last_dx = disp_x; s_last_dy = disp_y;
        s_last_dw = g_wide_w; s_last_dh = disp_h;
        return 1;
    }
    present_target_quad(tex, g_wide_w, VRAM_H,
                        0, disp_y, g_wide_w, disp_h, linear, lx, ly, lw, lh);
    pres_record(GL_PRES_WIDE, disp_x, disp_y, g_wide_w, disp_h, lx, ly, lw, lh);
    latency_ring_mark(LAT_SWAP_BEGIN);
    SDL_GL_SwapWindow(s_win);
    latency_ring_mark(LAT_SWAP_END);
    gl_perf_present_exit(1);
    present_dirty_rect(0, disp_y, VRAM_W - 1, disp_y + disp_h - 1, 0);
    present_force_consumed();
    s_last_present_path = GL_PRES_WIDE;
    s_last_dx = disp_x; s_last_dy = disp_y; s_last_dw = g_wide_w; s_last_dh = disp_h;
    coh_record(GL_COH_PRESENT, 0, disp_y, g_wide_w - 1, disp_y + disp_h - 1);
    return 1;
}

static const GpuRenderBackend GL_BACKEND = {
    .name = "opengl",
    .init = glb_init, .set_scale = glb_set_scale, .scale = glb_scale,
    .set_texture_filter = glb_set_texture_filter, .texture_filter = glb_texture_filter,
    .set_semi_transparency = glb_set_semi_transparency, .set_mask_bits = glb_set_mask_bits,
    .set_texture_window = glb_set_texture_window, .set_color_modulation = glb_set_color_modulation,
    .fill_rect = glb_fill_rect, .copy_rect = glb_copy_rect,
    .draw_flat_triangle = glb_draw_flat_triangle, .draw_gouraud_triangle = glb_draw_gouraud_triangle,
    .draw_textured_triangle = glb_draw_textured_triangle,
    .draw_shaded_textured_triangle = glb_draw_shaded_textured_triangle,
    .draw_flat_rect = glb_draw_flat_rect, .draw_textured_rect = glb_draw_textured_rect,
    .draw_textured_rect_scaled = glb_draw_textured_rect_scaled,
    .draw_line = glb_draw_line, .draw_shaded_line = glb_draw_shaded_line,
    .render_display = glb_render_display, .render_display_hires = glb_render_display_hires,
    .vram_write = glb_vram_write, .vram_read = glb_vram_read,
    .vram_transfer_in = glb_vram_transfer_in, .vram_transfer_out = glb_vram_transfer_out,
    .set_draw_area = glb_set_draw_area, .get_draw_area = glb_get_draw_area,
    .set_draw_offset = glb_set_draw_offset,
    .wide_configure = glb_wide_configure,
    .wide_set_target = glb_wide_set_target,
    .wide_disable_target = glb_wide_disable_target,
    .wide_clear = glb_wide_clear,
    .wide_clear_margins = glb_wide_clear_margins,
    .render_wide_display = glb_render_wide_display,
    .wide_dump_full = glb_wide_dump_full,
};

const GpuRenderBackend *gl_backend_get(void) { return &GL_BACKEND; }
