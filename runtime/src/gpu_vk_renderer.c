/* gpu_vk_renderer.c — hardware Vulkan renderer backend.  See gpu_vk_renderer.h.
 *
 * GPU-authoritative VRAM (mirrors the GL backend's model): the canonical copy of
 * VRAM is a Vulkan image (s_vram_img, RGBA8, 1024*S x 512*S).  A CPU mirror is
 * kept only for readback (screenshots / debug server / 24-bit FMV present) and
 * is synced lazily.  Present blits the displayed region straight from the VRAM
 * image to the swapchain — no per-frame readback.
 *
 * Vulkan is loaded entirely at runtime (SDL_Vulkan_LoadLibrary +
 * vkGetInstanceProcAddr), so the exe never link-depends on vulkan-1 and falls
 * back to software when no ICD is present.
 *
 * BUILD STATUS: instance/device/swapchain + VRAM image + fills + CPU<->VRAM
 * transfers + present are implemented; geometry pipelines (triangles/rects/
 * lines), texturing/CLUT, mask-bit, semi-transparency, SSAA and the native-wide
 * compositor are added in later phases — those vtable entries currently abort
 * via psx_fatal_halt rather than silently no-op (no stubs).
 */

#include "gpu.h"
#include "gpu_render.h"
#include "gpu_vk_renderer.h"
#include "gpu_sw_renderer.h"
#include "host_osd.h"
#include "psx_savestate_menu.h"
#include "psx_rewind.h"
#include "crash_trace.h"
#include "gpu_vk_upload.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#if !defined(PSX_HAVE_VULKAN)
/* Vulkan SDK headers/glslc were not found at configure time: compile an inert
 * backend so the runtime still builds and simply falls back to software. */
int  vk_renderer_init_context(struct SDL_Window *win) { (void)win; return 0; }
void vk_renderer_shutdown(void) {}
int  vk_renderer_present_vram(int a,int b,int c,int d,int e,int f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return 0;}
int  vk_renderer_present_wide(int a,int b,int c,int d){(void)a;(void)b;(void)c;(void)d;return 0;}
void vk_renderer_present_cpu(const uint32_t*p,int w,int h,int l,int f){(void)p;(void)w;(void)h;(void)l;(void)f;}
void vk_renderer_present_blank(void){}
void vk_renderer_sync_cpu(void){}
void vk_renderer_restage_vram_after_savestate(void){}
void vk_renderer_set_present_mode(int m){(void)m;}
int  vk_perf_json(char *out,int cap,int count){(void)count; return cap>2?snprintf(out,cap,"[]"):0;}
const GpuRenderBackend *vk_backend_get(void) { return 0; }

#else  /* PSX_HAVE_VULKAN */

#include "psx_sdl.h"
#if defined(PSX_SDL3)
#include <SDL3/SDL_vulkan.h>
#else
#include <SDL_vulkan.h>
#endif
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "vk_shaders_spv.h"   /* generated: spv_geo_vert/frag, spv_geo_tex_vert/frag,
                               *            spv_pack_comp, spv_blit_vert/frag */

#define VRAM_W 1024
#define VRAM_H 512

/* ---- dynamic loader ---------------------------------------------------- */
/* vkGetInstanceProcAddr comes from SDL; everything else is loaded through it
 * (global/instance scope) or vkGetDeviceProcAddr (device scope). */
static PFN_vkGetInstanceProcAddr p_vkGetInstanceProcAddr;
static PFN_vkGetDeviceProcAddr   p_vkGetDeviceProcAddr;

#define VK_GLOBAL_FUNCS(X) \
    X(vkCreateInstance) \
    X(vkEnumerateInstanceExtensionProperties) \
    X(vkEnumerateInstanceLayerProperties)

#define VK_INSTANCE_FUNCS(X) \
    X(vkDestroyInstance) \
    X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceFormatProperties) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    X(vkCreateDevice) \
    X(vkDestroySurfaceKHR)

#define VK_DEVICE_FUNCS(X) \
    X(vkDestroyDevice) \
    X(vkGetDeviceQueue) \
    X(vkDeviceWaitIdle) \
    X(vkQueueWaitIdle) \
    X(vkCreateSwapchainKHR) \
    X(vkDestroySwapchainKHR) \
    X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) \
    X(vkQueuePresentKHR) \
    X(vkCreateImageView) \
    X(vkDestroyImageView) \
    X(vkCreateImage) \
    X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements) \
    X(vkBindImageMemory) \
    X(vkCreateBuffer) \
    X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) \
    X(vkAllocateMemory) \
    X(vkFreeMemory) \
    X(vkMapMemory) \
    X(vkUnmapMemory) \
    X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) \
    X(vkResetCommandPool) \
    X(vkAllocateCommandBuffers) \
    X(vkFreeCommandBuffers) \
    X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) \
    X(vkResetCommandBuffer) \
    X(vkQueueSubmit) \
    X(vkCreateSemaphore) \
    X(vkDestroySemaphore) \
    X(vkCreateFence) \
    X(vkDestroyFence) \
    X(vkWaitForFences) \
    X(vkResetFences) \
    X(vkCmdPipelineBarrier) \
    X(vkCmdClearColorImage) \
    X(vkCmdClearDepthStencilImage) \
    X(vkCmdCopyBufferToImage) \
    X(vkCmdCopyImageToBuffer) \
    X(vkCmdBlitImage) \
    X(vkCmdCopyImage) \
    X(vkCreateRenderPass) \
    X(vkDestroyRenderPass) \
    X(vkCreateFramebuffer) \
    X(vkDestroyFramebuffer) \
    X(vkCreateShaderModule) \
    X(vkDestroyShaderModule) \
    X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) \
    X(vkCreateComputePipelines) \
    X(vkDestroyPipeline) \
    X(vkCreateDescriptorSetLayout) \
    X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool) \
    X(vkDestroyDescriptorPool) \
    X(vkAllocateDescriptorSets) \
    X(vkUpdateDescriptorSets) \
    X(vkCreateSampler) \
    X(vkDestroySampler) \
    X(vkCmdBeginRenderPass) \
    X(vkCmdEndRenderPass) \
    X(vkCmdBindPipeline) \
    X(vkCmdBindVertexBuffers) \
    X(vkCmdBindDescriptorSets) \
    X(vkCmdPushConstants) \
    X(vkCmdSetViewport) \
    X(vkCmdSetScissor) \
    X(vkCmdSetStencilReference) \
    X(vkCmdDispatch) \
    X(vkCmdDraw) \
    X(vkCmdClearAttachments)

#define DECL_FP(n) static PFN_##n p_##n;
VK_GLOBAL_FUNCS(DECL_FP)
VK_INSTANCE_FUNCS(DECL_FP)
VK_DEVICE_FUNCS(DECL_FP)
#undef DECL_FP

/* ---- state ------------------------------------------------------------- */
static SDL_Window      *s_win;
static VkInstance       s_instance;
static VkSurfaceKHR     s_surface;
static VkPhysicalDevice s_phys;
static VkDevice         s_dev;
static uint32_t         s_qfam = 0;       /* graphics+present queue family */
static VkQueue          s_queue;
static VkCommandPool    s_cmd_pool;       /* present command buffers */
static VkCommandPool    s_work_pool;      /* one-shot work CBs (reset at gpu_sync) */
static int              s_work_pending;   /* submitted work not yet waited */
static VkPhysicalDeviceMemoryProperties s_memprops;

/* Staging buffers whose GPU consumption is still in flight; freed at gpu_sync
 * after the queue goes idle (work is submitted without per-op waits now). */
#define PENDING_STAGING_MAX 1024
static VkBuffer       s_pending_buf[PENDING_STAGING_MAX];
static VkDeviceMemory s_pending_mem[PENDING_STAGING_MAX];
static int            s_pending_n;

#define STAGING_CACHE_MAX 16
typedef struct {
    VkBuffer buf;
    VkDeviceMemory mem;
    VkDeviceSize size;
    void *map;
    int busy;
} StagingCacheEntry;
static StagingCacheEntry s_staging_cache[STAGING_CACHE_MAX];

static void staging_release(VkBuffer buf, VkDeviceMemory mem) {
    for (int i = 0; i < STAGING_CACHE_MAX; ++i) {
        if (s_staging_cache[i].buf == buf && s_staging_cache[i].mem == mem) {
            s_staging_cache[i].busy = 0;
            return;
        }
    }
    p_vkUnmapMemory(s_dev, mem);
    p_vkDestroyBuffer(s_dev, buf, NULL);
    p_vkFreeMemory(s_dev, mem, NULL);
}

static void staging_destroy(VkBuffer buf, VkDeviceMemory mem) {
    for (int i = 0; i < STAGING_CACHE_MAX; ++i) {
        StagingCacheEntry *entry = &s_staging_cache[i];
        if (entry->buf == buf && entry->mem == mem) {
            if (entry->map) p_vkUnmapMemory(s_dev, mem);
            p_vkDestroyBuffer(s_dev, buf, NULL);
            p_vkFreeMemory(s_dev, mem, NULL);
            memset(entry, 0, sizeof *entry);
            return;
        }
    }
    p_vkUnmapMemory(s_dev, mem);
    p_vkDestroyBuffer(s_dev, buf, NULL);
    p_vkFreeMemory(s_dev, mem, NULL);
}

static VkSwapchainKHR   s_swapchain;
static VkFormat         s_sc_format;
static VkExtent2D       s_sc_extent;
static uint32_t         s_sc_count;
static VkImage          s_sc_images[8];
static int              s_present_mode_req = 1;   /* 1 tear-free, 0 IMMEDIATE, -1 MAILBOX */

/* Per-frame sync (double-buffered command recording). */
#define VK_FRAMES 2
static VkCommandBuffer  s_cmd[VK_FRAMES];
static VkSemaphore      s_sem_acquire[VK_FRAMES];
static VkSemaphore      s_sem_render[VK_FRAMES];
static VkFence          s_fence[VK_FRAMES];
static uint32_t         s_frame_idx;

/* GPU-authoritative VRAM image + CPU mirror. */
static int        s_scale = 1;
static VkImage    s_vram_img;
static VkDeviceMemory s_vram_mem;
static VkImageLayout s_vram_layout = VK_IMAGE_LAYOUT_UNDEFINED;  /* tracked */
static uint16_t  *s_vram;                 /* gpu.c's CPU VRAM array (1024x512) */
static int        s_cpu_dirty;            /* CPU mirror stale vs GPU image */

static int        s_ready;                /* full pipeline ready (Phase 2+) */
static int        s_ctx_ok;              /* instance/device/swapchain up */

static int        s_texfilter;

/* ---- geometry / render targets ----------------------------------------- */
static VkImageView     s_vram_view;       /* RGBA8 hr render target view */
static VkRenderPass    s_rpass;           /* color (hr) + depth/stencil */
static VkFramebuffer   s_fbo;

/* Native R16_UINT raw VRAM mirror: the canonical 1555 bits, sampled in the
 * textured fragment shader (CLUT decode needs the exact index bits, which the
 * lossy RGBA8 image cannot provide) and read back to the CPU mirror. Kept
 * coherent with the hr render target by the PACK compute pass. */
static VkImage         s_raw_img;
static VkDeviceMemory  s_raw_mem;
static VkImageView     s_raw_view;
static VkImageLayout   s_raw_layout = VK_IMAGE_LAYOUT_UNDEFINED;

/* Depth/stencil attachment: stencil bit0 mirrors VRAM bit15 (mask). */
static VkImage         s_ds_img;
static VkDeviceMemory  s_ds_mem;
static VkImageView     s_ds_view;
static VkFormat        s_ds_format;

/* Scratch hr image for VRAM->VRAM copies (resolves overlap). */
static VkImage         s_scratch_img;
static VkDeviceMemory  s_scratch_mem;
static VkImageView     s_scratch_view;
static VkImageLayout   s_scratch_layout = VK_IMAGE_LAYOUT_UNDEFINED;

/* Native-res RGBA8 upload staging image (CPU->VRAM blit source). */
static VkImage         s_up_img;
static VkDeviceMemory  s_up_mem;
static VkImageView     s_up_view;
static VkImageLayout   s_up_layout = VK_IMAGE_LAYOUT_UNDEFINED;

/* Samplers + descriptor sets. One nearest sampler serves every fetch. */
/* ---- native-wide compositor surfaces (mirrors the GL backend) -----------
 * Canonical VRAM (the hr image) stays 100% faithful; framebuffer-targeting
 * batches are ALSO drawn into a per-base_x wide surface, x-translated in the
 * vertex shader via the existing u_xoff/u_xhalf push constants. Each surface
 * carries its OWN depth-stencil image (PSX mask-bit mirror; Part-A lesson:
 * never render stencil-enabled passes into a stencil-less target) and its own
 * VkFramebuffer on the SHARED render pass, so every existing pipeline works
 * unchanged. The mirror is appended to the SAME one-shot command buffer as the
 * canonical pass — one extra render pass per flushed batch, no extra submits. */
#define VK_WIDE_MAX_SURF 4
static VkImage        s_wide_img[VK_WIDE_MAX_SURF];
static VkDeviceMemory s_wide_mem[VK_WIDE_MAX_SURF];
static VkImageView    s_wide_view[VK_WIDE_MAX_SURF];
static VkImage        s_wide_ds_img[VK_WIDE_MAX_SURF];
static VkDeviceMemory s_wide_ds_mem[VK_WIDE_MAX_SURF];
static VkImageView    s_wide_ds_view[VK_WIDE_MAX_SURF];
static VkFramebuffer  s_wide_fb[VK_WIDE_MAX_SURF];
static VkImageLayout  s_wide_layout[VK_WIDE_MAX_SURF];
static int            s_wide_base[VK_WIDE_MAX_SURF] = { -1, -1, -1, -1 };
static int            s_wide_w = 0;        /* wide width (native px); 0 = disabled */
static int            s_wide_offset = 0;   /* centering OFFSET (native px) */
static int            s_wide_cur = -1;     /* active mirror surface index, -1 = none */
static int            s_wide_cur_base = 0;
static void wide_free_all(void);           /* defined with the wide helpers below */

static VkSampler       s_samp;
static VkDescriptorPool s_dpool;
static VkDescriptorSetLayout s_dsl_tex;   /* binding0: raw usampler2D (frag) */
static VkDescriptorSetLayout s_dsl_pack;  /* binding0: hr sampler, binding1: raw storage (compute) */
static VkDescriptorSetLayout s_dsl_blit;  /* binding0: src sampler (frag) */
static VkDescriptorSet s_ds_tex;
static VkDescriptorSet s_ds_pack;
/* Blit src image is rebound per blit; a ring of sets avoids updating a set still
 * in flight (work is no longer waited per-op). Cycled within a frame; the index
 * resets at gpu_sync (which waits the queue idle). */
#define BLIT_DESC_RING 64
static VkDescriptorSet s_ds_blit_ring[BLIT_DESC_RING];
static int s_ds_blit_idx;

/* ---- always-on per-frame perf ring (transition-stall diagnosis) ----------
 * Per-op counters accumulate into s_perf_cur; each present() snapshots them into
 * the ring keyed by present index, then resets. Query via the debug-server
 * "vk_perf" command (vk_perf_json). Op COUNTS alone reveal the dominant
 * per-frame cost (e.g. make_staging vkAllocateMemory churn during transitions:
 * a frame that jumps from ~10 to ~2000 allocs is the smoking gun). Always on. */
typedef struct {
    uint32_t present_idx;
    uint64_t present_qpc;
    uint32_t wait_us, acquire_us, present_us;
    uint32_t allocs, alloc_kb, oneshots, submits, syncs, pack_flushes,
             blits, upload_blocks, copy_rects, geo_flushes, tex_flushes,
             wide_passes, wide_clears;
} VkPerf;
#define VK_PERF_RING 256
static VkPerf s_perf_ring[VK_PERF_RING];
static uint32_t s_perf_head;          /* number of frames recorded (monotonic) */
static VkPerf s_perf_cur;             /* current (in-progress) frame */
static uint32_t s_present_idx;

static uint32_t perf_elapsed_us(uint64_t start) {
    uint64_t frequency = SDL_GetPerformanceFrequency();
    uint64_t ticks = SDL_GetPerformanceCounter() - start;
    return frequency ? (uint32_t)(ticks * 1000000u / frequency) : 0;
}

static void perf_snapshot_present(void) {
    s_perf_cur.present_idx = s_present_idx++;
    s_perf_cur.present_qpc = SDL_GetPerformanceCounter();
    s_perf_ring[s_perf_head % VK_PERF_RING] = s_perf_cur;
    s_perf_head++;
    memset(&s_perf_cur, 0, sizeof s_perf_cur);
}

/* Pipeline layouts (one per program family). */
static VkPipelineLayout s_pl_geo;
static VkPipelineLayout s_pl_tex;
static VkPipelineLayout s_pl_blit;
static VkPipelineLayout s_pl_pack;
static VkPipeline       s_pipe_pack;      /* compute */

/* Lazy graphics-pipeline cache keyed by (program, topology, blend, stencil,
 * cmask). program: 0 GEO, 1 TEX, 2 BLIT. blend: 0 off, 1..4 = PS1 modes 0..3
 * (+1). stencil: 0 SET (always+replace, dyn ref), 1 CHECK keep, 2 CHECK
 * invert. cmask: 0 writes colour, 1 disables ALL colour writes (stencil-only
 * fixup pass of an opaque textured batch — mirrors GL's glColorMask off). */
#define PIPE_PROGS 3
#define PIPE_TOPOS 2   /* 0 TRIANGLE_LIST, 1 LINE_LIST */
#define PIPE_BLENDS 5
#define PIPE_STENCILS 3
#define PIPE_CMASKS 2
#define PIPE_CACHE_N (PIPE_PROGS * PIPE_TOPOS * PIPE_BLENDS * PIPE_STENCILS * PIPE_CMASKS)
static VkPipeline s_pipe_cache[PIPE_CACHE_N];
static VkShaderModule s_mod_geo_v, s_mod_geo_f, s_mod_tex_v, s_mod_tex_f, s_mod_blit_v, s_mod_blit_f;

/* Untextured batch (flat/gouraud triangles, lines-as-quads, flat rects). */
typedef struct { float x, y, r, g, b, a; } Vert;   /* GEO vertex, stride 24 */
#define VK_VBUF_VERTS 65536
static VkBuffer       s_vbuf;
static VkDeviceMemory s_vbuf_mem;
static Vert          *s_vmap;             /* persistently mapped host-visible */
static uint32_t       s_vcount;           /* verts accumulated this batch */
/* Draws are SUBMITTED without waiting (deferred-submit model), so a flushed
 * batch's vertices may still be read by the GPU when the next batch begins.
 * Rewriting the buffer from offset 0 per batch raced that read and shredded
 * whole batches on screen (terrain/mesh chunks missing or drawn with another
 * batch's vertices). Batches therefore SUB-ALLOCATE: each flush draws
 * [base, base+count) and bumps base; gpu_sync (queue idle) recycles to 0. */
static uint32_t       s_vbase;            /* first vertex of the open batch */
static int            s_geo_semi = -2;    /* open untextured batch keys */
static int            s_geo_mask = 0, s_geo_check = 0;
static int            s_geo_mirror_suppress = 0;   /* open batch: skip the wide mirror
                                                    * (full-screen overlay rect draws its
                                                    * own full-width wide pass instead) */

/* Sub-pixel vertex override + perspective UV weights for the next triangle
 * ([video] geometry_correction / perspective_texturing; see gpu_render.h).
 * gpu.c sets these immediately before the matching gr_draw_*_triangle and they
 * are consumed by it. All-zero == the faithful integer/affine path. */
static int   s_pc_valid = 0;           /* sub-pixel positions present */
static float s_pc_x[3], s_pc_y[3];     /* native VRAM px, fractional  */
static int   s_pq_valid = 0;           /* perspective weights present */
static float s_pq[3];

/* Textured batch: 19 floats/vert (pos,uv,col,tpage,clut,depth,raw,limits,q).
 * q is the perspective weight ([video] perspective_texturing); 0 = affine,
 * which the vertex shader turns into w == 1.0 — the faithful default. */
#define TEXV 19
typedef struct { float v[TEXV]; } TexVert;
#define VK_TBUF_VERTS 24576               /* multiple of 3 */
static VkBuffer       s_tbuf;
static VkDeviceMemory s_tbuf_mem;
static TexVert       *s_tmap;
static uint32_t       s_tcount;
static uint32_t       s_tbase;            /* first vertex of the open batch */
static int            s_tb_semi = -2, s_tb_mask = 0, s_tb_check = 0, s_tb_filter = 0;
static int            s_tb_twin[4] = {0,0,0,0};

static void flush_geometry(void);         /* commit pending untextured batch */
static void flush_tex_batch(void);        /* commit pending textured batch */
static void flush_cpu_upload(void);       /* pending CPU writes -> GPU images */
static void pack_flush(void);             /* hr -> raw mirror (dirty rect) */
static void flush_pack_if_sampling(int tpx, int tpy, int depth, int clx, int cly);
static void vram_upload_block(int x, int y, int w, int h, const uint16_t *data);
static void gpu_copy_rect(int sx, int sy, int dx, int dy, int w, int h);
static void ensure_cpu(void);             /* GPU -> CPU mirror readback */
static int  make_staging(VkDeviceSize bytes, VkBuffer *buf, VkDeviceMemory *mem, void **map);
static void free_staging(VkBuffer buf, VkDeviceMemory mem);

/* draw area / offset / per-prim render state (set via the vtable setters) */
static int s_da_x1, s_da_y1, s_da_x2 = VRAM_W - 1, s_da_y2 = VRAM_H - 1;
static int s_off_x, s_off_y;
static int s_semi_en = 0, s_semi_mode = 0;
static int s_mask_set = 0, s_mask_check = 0;
static int s_tw_mask_x = 0, s_tw_mask_y = 0, s_tw_off_x = 0, s_tw_off_y = 0;
static int s_mod_r = 128, s_mod_g = 128, s_mod_b = 128, s_mod_raw = 0;

/* Coherency: regions rendered to the hr image but not yet packed to the raw
 * mirror, and whether the hr image diverges from the CPU mirror. */
typedef struct { int set, x0, y0, x1, y1; } DirtyRect;
static DirtyRect s_pack_dirty;
static int       s_gpu_dirty;
/* CPU-side VRAM writes (GP0 A0 transfers, DMA, fills, single pokes) land in
 * the CPU array immediately and accumulate here; ONE batched upload flushes
 * them before the next GPU op that could observe them (mirrors the GL
 * backend). Eager per-op uploads are the transition-stall:
 * each PIXEL cost 2 vkAllocateMemory + 2 vkQueueSubmit + a 2-draw blit pass,
 * so a single GP0 A0 texture upload wedged the main thread for minutes inside
 * the driver (freeze dump: 8/8 samples in D3DKMTCreateAllocation churn).
 *
 * EXACT rect list, NOT a single union (ported from the GL fix for the MMX6
 * black-frame flicker, ISSUES #7): a union of DISJOINT uploads covers the
 * framebuffers in between, and flushing paints that whole box from the CPU
 * VRAM array — STALE under VK once the hr image is authoritative (ensure_cpu
 * syncs on demand only) — stomping freshly rendered frames. Only pixels the
 * CPU actually wrote may ever be uploaded. See up_add()/up_add_transfer(). */
/* 64 (vs GL's 16): VK pays TWO queue submits per flush (copies + blit pass),
 * so overflow-triggered flushes must stay rare. MDEC/FMV macroblock streams
 * coalesce into one rect per block row via the row-band merge, so a whole
 * streamed frame fits comfortably. */
#define UP_RECTS_MAX 64
static DirtyRect s_up_rects[UP_RECTS_MAX];
static int       s_up_nrects = 0;

static void rect_clear(DirtyRect *r) { r->set = 0; }
static void rect_add(DirtyRect *r, int x0, int y0, int x1, int y1) {
    if (x0 > x1 || y0 > y1) return;
    if (!r->set) { r->x0=x0; r->y0=y0; r->x1=x1; r->y1=y1; r->set=1; return; }
    if (x0 < r->x0) r->x0 = x0; if (y0 < r->y0) r->y0 = y0;
    if (x1 > r->x1) r->x1 = x1; if (y1 > r->y1) r->y1 = y1;
}
static int rect_intersects(const DirtyRect *r, int x0, int y0, int x1, int y1) {
    if (!r->set) return 0;
    return !(x1 < r->x0 || x0 > r->x1 || y1 < r->y0 || y0 > r->y1);
}

/* Add an uploaded rect (see s_up_rects). Merges ONLY when the merge adds no
 * uncovered pixels: containment either way, or same row-band / column-band
 * extension. On overflow: flush the pending set (order-preserving) when the
 * device is up; pre-ready the CPU array is fully authoritative, so a union is
 * harmless there. Mirrors the GL backend's up_add (20k-randomized-rect
 * host-test-proven merge rule). */
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
        if (s_ready) {
            flush_cpu_upload();       /* order-preserving: land the old ones */
        } else {
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

/* GP0(A0) transfer: per-pixel wrap means a wrapping transfer touches up to
 * FOUR exact rects — never all of VRAM (the "wrapped: take all" union was the
 * same stale-stomp class). Mirrors the GL backend's up_add_transfer. */
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

/* ---- helpers ----------------------------------------------------------- */
static void vk_log(const char *msg) { fprintf(stdout, "psxrecomp: vulkan: %s\n", msg); }

static int vk_die(const char *why) { fprintf(stdout, "psxrecomp: vulkan: %s\n", why); return 0; }

static uint32_t find_mem_type(uint32_t type_bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < s_memprops.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (s_memprops.memoryTypes[i].propertyFlags & want) == want)
            return i;
    }
    return UINT32_MAX;
}

static void vk_gpu_sync_internal(void);   /* drain queue + reclaim work pool/staging */

/* Coalesce work operations into one command buffer per sync interval. This
 * preserves recording order while removing a queue submission per small GPU
 * operation. */
static VkCommandBuffer s_work_cb;

static VkCommandBuffer begin_oneshot(void) {
    s_perf_cur.oneshots++;
    if (s_work_cb) return s_work_cb;
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = s_work_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    if (p_vkAllocateCommandBuffers(s_dev, &ai, &cb) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    p_vkBeginCommandBuffer(cb, &bi);
    s_work_cb = cb;
    return cb;
}

/* Finish a logical operation but keep the shared command buffer open. */
static void end_oneshot(VkCommandBuffer cb) {
    (void)cb;
    s_work_pending = 1;
}

static void flush_work(void) {
    if (!s_work_cb) return;
    p_vkEndCommandBuffer(s_work_cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s_work_cb;
    p_vkQueueSubmit(s_queue, 1, &si, VK_NULL_HANDLE);
    s_perf_cur.submits++;
    s_work_cb = VK_NULL_HANDLE;
}

/* Defer a staging buffer's destruction until the queue is next idle (its
 * copy may still be in flight). */
static void defer_staging(VkBuffer buf, VkDeviceMemory mem) {
    if (s_pending_n >= PENDING_STAGING_MAX)
        vk_gpu_sync_internal();   /* ring full mid-frame: drain so we never leak */
    s_pending_buf[s_pending_n] = buf; s_pending_mem[s_pending_n] = mem; s_pending_n++;
}

/* Drain all submitted work, then reclaim the work-CB pool and deferred staging.
 * The single sync point for the deferred-submit model. */
static void vk_gpu_sync_internal(void) {
    s_perf_cur.syncs++;
    flush_work();
    if (s_work_pending) {
        uint64_t wait_start = SDL_GetPerformanceCounter();
        p_vkQueueWaitIdle(s_queue);
        s_perf_cur.wait_us += perf_elapsed_us(wait_start);
        /* RELEASE_RESOURCES so the per-op command buffers allocated since the
         * last sync are actually freed (a plain reset only recycles them, and
         * begin_oneshot always allocates fresh -> unbounded growth). */
        p_vkResetCommandPool(s_dev, s_work_pool, VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT);
        s_work_pending = 0;
    }
    for (int i = 0; i < s_pending_n; i++) {
        staging_release(s_pending_buf[i], s_pending_mem[i]);
    }
    s_pending_n = 0;
    s_ds_blit_idx = 0;
    /* Queue idle: every submitted draw has consumed its vertex-buffer region,
     * so recycle the sub-allocation cursors. An OPEN (unflushed) batch's
     * vertices move to offset 0 so its eventual draw stays contiguous. */
    if (s_vmap) {
        if (s_vcount && s_vbase)
            memmove(s_vmap, s_vmap + s_vbase, (size_t)s_vcount * sizeof(Vert));
        s_vbase = 0;
    }
    if (s_tmap) {
        if (s_tcount && s_tbase)
            memmove(s_tmap, s_tmap + s_tbase, (size_t)s_tcount * sizeof(TexVert));
        s_tbase = 0;
    }
}
#define gpu_sync() vk_gpu_sync_internal()

/* Image layout transition (full subresource, color aspect). */
static void img_barrier(VkCommandBuffer cb, VkImage img,
                        VkImageLayout from, VkImageLayout to,
                        VkAccessFlags src_access, VkAccessFlags dst_access,
                        VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = src_access; b.dstAccessMask = dst_access;
    p_vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* Map a layout to its (access mask, pipeline stage) for barriers. */
static void layout_access(VkImageLayout l, VkAccessFlags *acc, VkPipelineStageFlags *stage) {
    switch (l) {
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            *acc = VK_ACCESS_TRANSFER_READ_BIT;  *stage = VK_PIPELINE_STAGE_TRANSFER_BIT; break;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            *acc = VK_ACCESS_TRANSFER_WRITE_BIT; *stage = VK_PIPELINE_STAGE_TRANSFER_BIT; break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            *acc = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            *stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            /* sampled in both the fragment (textured draw) and compute (pack)
             * stages, so cover all commands rather than guess a single stage. */
            *acc = VK_ACCESS_SHADER_READ_BIT; *stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; break;
        case VK_IMAGE_LAYOUT_GENERAL:
            *acc = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            *stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; break;
        default:
            *acc = 0; *stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT; break;
    }
}

/* Transition an arbitrary color-aspect image to `want` on cb, tracking layout
 * through *cur. */
static void img_to(VkCommandBuffer cb, VkImage img, VkImageLayout *cur, VkImageLayout want) {
    if (*cur == want) return;
    VkAccessFlags sa, da; VkPipelineStageFlags ss, ds;
    layout_access(*cur, &sa, &ss);
    layout_access(want, &da, &ds);
    img_barrier(cb, img, *cur, want, sa, da, ss, ds);
    *cur = want;
}

/* Transition the VRAM image to `want` on cb, tracking its current layout. */
static void vram_to(VkCommandBuffer cb, VkImageLayout want) {
    img_to(cb, s_vram_img, &s_vram_layout, want);
}

/* ---- loader bring-up --------------------------------------------------- */
static int load_global_funcs(void) {
#define LOAD(n) p_##n = (PFN_##n)p_vkGetInstanceProcAddr(NULL, #n); if (!p_##n) return vk_die("missing " #n);
    VK_GLOBAL_FUNCS(LOAD)
#undef LOAD
    return 1;
}
static int load_instance_funcs(void) {
#define LOAD(n) p_##n = (PFN_##n)p_vkGetInstanceProcAddr(s_instance, #n); if (!p_##n) return vk_die("missing " #n);
    VK_INSTANCE_FUNCS(LOAD)
#undef LOAD
    p_vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)
        p_vkGetInstanceProcAddr(s_instance, "vkGetDeviceProcAddr");
    return p_vkGetDeviceProcAddr ? 1 : vk_die("missing vkGetDeviceProcAddr");
}
static int load_device_funcs(void) {
#define LOAD(n) p_##n = (PFN_##n)p_vkGetDeviceProcAddr(s_dev, #n); if (!p_##n) return vk_die("missing " #n);
    VK_DEVICE_FUNCS(LOAD)
#undef LOAD
    return 1;
}

/* ---- instance / device / swapchain ------------------------------------- */
static int create_instance(void) {
    unsigned ext_count = 0;
#if defined(PSX_SDL3)
    const char *const *sdl_exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
    if (!sdl_exts)
        return vk_die("SDL_Vulkan_GetInstanceExtensions failed");
    const char *exts[16];
    if (ext_count > 16) ext_count = 16;
    for (unsigned i = 0; i < ext_count; ++i) exts[i] = sdl_exts[i];
#else
    if (!SDL_Vulkan_GetInstanceExtensions(s_win, &ext_count, NULL))
        return vk_die("SDL_Vulkan_GetInstanceExtensions(count) failed");
    const char *exts[16];
    if (ext_count > 16) ext_count = 16;
    if (!SDL_Vulkan_GetInstanceExtensions(s_win, &ext_count, exts))
        return vk_die("SDL_Vulkan_GetInstanceExtensions(list) failed");
#endif

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "psxrecomp";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = ext_count;
    ci.ppEnabledExtensionNames = exts;
    if (p_vkCreateInstance(&ci, NULL, &s_instance) != VK_SUCCESS)
        return vk_die("vkCreateInstance failed");
    return 1;
}

static int pick_device(void) {
    uint32_t n = 0;
    p_vkEnumeratePhysicalDevices(s_instance, &n, NULL);
    if (!n) return vk_die("no Vulkan physical devices");
    if (n > 8) n = 8;
    VkPhysicalDevice devs[8];
    p_vkEnumeratePhysicalDevices(s_instance, &n, devs);

    /* Prefer a discrete GPU with a graphics+present queue family. */
    VkPhysicalDevice best = VK_NULL_HANDLE; uint32_t best_fam = 0; int best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties pr; p_vkGetPhysicalDeviceProperties(devs[i], &pr);
        uint32_t qn = 0; p_vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, NULL);
        if (qn > 16) qn = 16;
        VkQueueFamilyProperties qf[16];
        p_vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, qf);
        for (uint32_t f = 0; f < qn; f++) {
            VkBool32 present = VK_FALSE;
            p_vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], f, s_surface, &present);
            if ((qf[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                int score = (pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) ? 2 : 1;
                if (score > best_score) { best_score = score; best = devs[i]; best_fam = f; }
                break;
            }
        }
    }
    if (best == VK_NULL_HANDLE) return vk_die("no graphics+present queue family");
    s_phys = best; s_qfam = best_fam;
    p_vkGetPhysicalDeviceMemoryProperties(s_phys, &s_memprops);
    VkPhysicalDeviceProperties pr; p_vkGetPhysicalDeviceProperties(s_phys, &pr);
    fprintf(stdout, "psxrecomp: vulkan: device = %s\n", pr.deviceName);
    return 1;
}

static int create_device(void) {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo q = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    q.queueFamilyIndex = s_qfam; q.queueCount = 1; q.pQueuePriorities = &prio;
    const char *dev_exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo ci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    ci.queueCreateInfoCount = 1; ci.pQueueCreateInfos = &q;
    ci.enabledExtensionCount = 1; ci.ppEnabledExtensionNames = dev_exts;
    if (p_vkCreateDevice(s_phys, &ci, NULL, &s_dev) != VK_SUCCESS)
        return vk_die("vkCreateDevice failed");
    if (!load_device_funcs()) return 0;
    p_vkGetDeviceQueue(s_dev, s_qfam, 0, &s_queue);
    return 1;
}

static VkPresentModeKHR choose_present_mode(void) {
    uint32_t n = 0;
    p_vkGetPhysicalDeviceSurfacePresentModesKHR(s_phys, s_surface, &n, NULL);
    if (n > 8) n = 8;
    VkPresentModeKHR modes[8];
    p_vkGetPhysicalDeviceSurfacePresentModesKHR(s_phys, s_surface, &n, modes);
    /* The frontend already owns frame cadence. Prefer MAILBOX for tear-free
     * delivery so FIFO does not introduce a second blocking pacer. */
    VkPresentModeKHR want = (s_present_mode_req == 0)
                          ? VK_PRESENT_MODE_IMMEDIATE_KHR
                          : VK_PRESENT_MODE_MAILBOX_KHR;
    for (uint32_t i = 0; i < n; i++) if (modes[i] == want) return want;
    return VK_PRESENT_MODE_FIFO_KHR;  /* always supported */
}

static int create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_phys, s_surface, &caps);

    uint32_t fn = 0;
    p_vkGetPhysicalDeviceSurfaceFormatsKHR(s_phys, s_surface, &fn, NULL);
    if (fn > 16) fn = 16;
    VkSurfaceFormatKHR fmts[16];
    p_vkGetPhysicalDeviceSurfaceFormatsKHR(s_phys, s_surface, &fn, fmts);
    VkSurfaceFormatKHR chosen = fmts[0];
    for (uint32_t i = 0; i < fn; i++) {
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM &&
            fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = fmts[i]; break; }
    }
    s_sc_format = chosen.format;

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        int w = 0, h = 0;
#if defined(PSX_SDL3)
        SDL_GetWindowSizeInPixels(s_win, &w, &h);
#else
        SDL_Vulkan_GetDrawableSize(s_win, &w, &h);
#endif
        ext.width = (uint32_t)w; ext.height = (uint32_t)h;
    }
    s_sc_extent = ext;

    uint32_t img_count = caps.minImageCount + 1;
    if (caps.maxImageCount && img_count > caps.maxImageCount) img_count = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = s_surface;
    ci.minImageCount = img_count;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = choose_present_mode();
    ci.clipped = VK_TRUE;
    if (p_vkCreateSwapchainKHR(s_dev, &ci, NULL, &s_swapchain) != VK_SUCCESS)
        return vk_die("vkCreateSwapchainKHR failed");

    s_sc_count = 0;
    p_vkGetSwapchainImagesKHR(s_dev, s_swapchain, &s_sc_count, NULL);
    if (s_sc_count > 8) s_sc_count = 8;
    p_vkGetSwapchainImagesKHR(s_dev, s_swapchain, &s_sc_count, s_sc_images);
    return 1;
}

static void destroy_swapchain(void) {
    if (s_swapchain) { p_vkDestroySwapchainKHR(s_dev, s_swapchain, NULL); s_swapchain = VK_NULL_HANDLE; }
}

static int create_sync(void) {
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = s_cmd_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = VK_FRAMES;
    if (p_vkAllocateCommandBuffers(s_dev, &ai, s_cmd) != VK_SUCCESS)
        return vk_die("vkAllocateCommandBuffers failed");
    VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i = 0; i < VK_FRAMES; i++) {
        if (p_vkCreateSemaphore(s_dev, &si, NULL, &s_sem_acquire[i]) != VK_SUCCESS ||
            p_vkCreateSemaphore(s_dev, &si, NULL, &s_sem_render[i]) != VK_SUCCESS ||
            p_vkCreateFence(s_dev, &fi, NULL, &s_fence[i]) != VK_SUCCESS)
            return vk_die("sync object creation failed");
    }
    return 1;
}

/* The VRAM image: RGBA8, transfer src+dst (fills via clear, copies, present
 * blit) + color attachment (geometry, Phase 2). Created in UNDEFINED, cleared
 * to black, left in TRANSFER_SRC_OPTIMAL between frames. */
static int create_vram_image(void) {
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent.width = VRAM_W * s_scale;
    ci.extent.height = VRAM_H * s_scale;
    ci.extent.depth = 1;
    ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (p_vkCreateImage(s_dev, &ci, NULL, &s_vram_img) != VK_SUCCESS)
        return vk_die("vram image create failed");

    VkMemoryRequirements req; p_vkGetImageMemoryRequirements(s_dev, s_vram_img, &req);
    VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mi.allocationSize = req.size;
    mi.memoryTypeIndex = find_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mi.memoryTypeIndex == UINT32_MAX) return vk_die("no device-local memory");
    if (p_vkAllocateMemory(s_dev, &mi, NULL, &s_vram_mem) != VK_SUCCESS)
        return vk_die("vram memory alloc failed");
    p_vkBindImageMemory(s_dev, s_vram_img, s_vram_mem, 0);

    /* Clear to black and park in TRANSFER_SRC_OPTIMAL. */
    VkCommandBuffer cb = begin_oneshot();
    vram_to(cb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black = {{0,0,0,0}};
    VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    p_vkCmdClearColorImage(cb, s_vram_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rng);
    vram_to(cb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    end_oneshot(cb);
    return 1;
}

/* ---- geometry pipeline (Phase 2) --------------------------------------- */
static VkShaderModule make_module(const uint32_t *code, uint32_t size) {
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = size; ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    p_vkCreateShaderModule(s_dev, &ci, NULL, &m);
    return m;
}

/* ---- push-constant blocks (must match the shaders' layouts) ------------- */
typedef struct { float shift, xoff, xhalf; } GeoPush;          /* 12 bytes */
typedef struct {
    float shift, xoff, xhalf;        /* 0,4,8 */
    int   semipass, maskset, filter; /* 12,16,20 */
    int   _pad[2];                   /* 24,28 -> ivec4 16-byte align */
    int   twin[4];                   /* 32..47 */
} TexPush;                           /* 48 bytes */
typedef struct {
    float shift;        /* 0 */
    int   stp_pass;     /* 4 */
    int   maskset;      /* 8 */
    int   src_div;      /* 12 */
    int   src_off[2];   /* 16,20 */
    int   _pad[2];      /* 24,28 -> ivec4 16-byte align */
    int   rect[4];      /* 32..47: x0,y0,x1,y1 native px */
} BlitPush;             /* 48 bytes */
typedef struct { int scale, off_x, off_y; } PackPush;          /* 12 bytes */

/* Create a device-local image + view (color aspect by default). */
static int make_image(VkFormat fmt, int w, int h, VkImageUsageFlags usage,
                      VkImageAspectFlags aspect,
                      VkImage *img, VkDeviceMemory *mem, VkImageView *view) {
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt;
    ci.extent.width = w; ci.extent.height = h; ci.extent.depth = 1;
    ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (p_vkCreateImage(s_dev, &ci, NULL, img) != VK_SUCCESS) return vk_die("image create failed");
    VkMemoryRequirements req; p_vkGetImageMemoryRequirements(s_dev, *img, &req);
    VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mi.allocationSize = req.size;
    mi.memoryTypeIndex = find_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mi.memoryTypeIndex == UINT32_MAX || p_vkAllocateMemory(s_dev, &mi, NULL, mem) != VK_SUCCESS)
        return vk_die("image memory alloc failed");
    p_vkBindImageMemory(s_dev, *img, *mem, 0);
    if (view) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = *img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
        vi.subresourceRange.aspectMask = aspect;
        vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
        if (p_vkCreateImageView(s_dev, &vi, NULL, view) != VK_SUCCESS)
            return vk_die("image view failed");
    }
    return 1;
}

static VkFormat choose_ds_format(void) {
    VkFormat cand[2] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT };
    for (int i = 0; i < 2; i++) {
        VkFormatProperties fp; p_vkGetPhysicalDeviceFormatProperties(s_phys, cand[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            return cand[i];
    }
    return VK_FORMAT_D24_UNORM_S8_UINT;
}

static int create_buffer_mapped(VkDeviceSize size, VkBuffer *buf, VkDeviceMemory *mem, void **map) {
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = size; bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (p_vkCreateBuffer(s_dev, &bci, NULL, buf) != VK_SUCCESS) return vk_die("vertex buffer failed");
    VkMemoryRequirements req; p_vkGetBufferMemoryRequirements(s_dev, *buf, &req);
    VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mi.allocationSize = req.size;
    mi.memoryTypeIndex = find_mem_type(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mi.memoryTypeIndex == UINT32_MAX || p_vkAllocateMemory(s_dev, &mi, NULL, mem) != VK_SUCCESS)
        return vk_die("vertex buffer memory failed");
    p_vkBindBufferMemory(s_dev, *buf, *mem, 0);
    p_vkMapMemory(s_dev, *mem, 0, size, 0, map);
    return 1;
}

/* Build all render targets, samplers, descriptors, pipeline layouts and the
 * pack compute pipeline. Graphics pipelines are built lazily by get_pipeline. */
static int create_render_targets(void) {
    int S = s_scale;

    /* RGBA8 hr render-target view */
    VkImageViewCreateInfo vv = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vv.image = s_vram_img; vv.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vv.format = VK_FORMAT_R8G8B8A8_UNORM;
    vv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vv.subresourceRange.levelCount = 1; vv.subresourceRange.layerCount = 1;
    if (p_vkCreateImageView(s_dev, &vv, NULL, &s_vram_view) != VK_SUCCESS)
        return vk_die("vram image view failed");

    /* Native raw mirror (sampled + storage), depth/stencil, scratch, upload. */
    if (!make_image(VK_FORMAT_R16_UINT, VRAM_W, VRAM_H,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, &s_raw_img, &s_raw_mem, &s_raw_view)) return 0;
    s_ds_format = choose_ds_format();
    if (!make_image(s_ds_format, VRAM_W * S, VRAM_H * S,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_STENCIL_BIT,
                    &s_ds_img, &s_ds_mem, &s_ds_view)) return 0;
    if (!make_image(VK_FORMAT_R8G8B8A8_UNORM, VRAM_W * S, VRAM_H * S,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, &s_scratch_img, &s_scratch_mem, &s_scratch_view)) return 0;
    if (!make_image(VK_FORMAT_R8G8B8A8_UNORM, VRAM_W, VRAM_H,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, &s_up_img, &s_up_mem, &s_up_view)) return 0;

    /* Clear raw mirror to 0 (park sampled) and stencil to 0 (park attachment). */
    {
        VkCommandBuffer cb = begin_oneshot();
        img_to(cb, s_raw_img, &s_raw_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue zero = {{0,0,0,0}};
        VkImageSubresourceRange crng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        p_vkCmdClearColorImage(cb, s_raw_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &crng);
        img_to(cb, s_raw_img, &s_raw_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        VkImageMemoryBarrier db = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        db.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; db.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        db.srcQueueFamilyIndex = db.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        db.image = s_ds_img; db.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        db.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        db.subresourceRange.levelCount = 1; db.subresourceRange.layerCount = 1;
        p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, 0, NULL, 0, NULL, 1, &db);
        VkClearDepthStencilValue dsv = { 0.0f, 0 };
        VkImageSubresourceRange srng = { VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 };
        p_vkCmdClearDepthStencilImage(cb, s_ds_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dsv, 1, &srng);
        db.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        db.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        db.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        db.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, NULL, 0, NULL, 1, &db);
        end_oneshot(cb);
    }

    /* Render pass: color (hr, load/store) + depth-stencil (stencil load/store). */
    VkAttachmentDescription att[2] = {0};
    att[0].format = VK_FORMAT_R8G8B8A8_UNORM; att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att[1].format = s_ds_format; att[1].samples = VK_SAMPLE_COUNT_1_BIT;
    att[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference dref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {0};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &cref;
    sub.pDepthStencilAttachment = &dref;
    VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpi.attachmentCount = 2; rpi.pAttachments = att;
    rpi.subpassCount = 1; rpi.pSubpasses = &sub;
    if (p_vkCreateRenderPass(s_dev, &rpi, NULL, &s_rpass) != VK_SUCCESS)
        return vk_die("render pass failed");

    VkImageView fbviews[2] = { s_vram_view, s_ds_view };
    VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fi.renderPass = s_rpass; fi.attachmentCount = 2; fi.pAttachments = fbviews;
    fi.width = VRAM_W * S; fi.height = VRAM_H * S; fi.layers = 1;
    if (p_vkCreateFramebuffer(s_dev, &fi, NULL, &s_fbo) != VK_SUCCESS)
        return vk_die("framebuffer failed");

    /* Nearest sampler (texelFetch ignores filtering/addressing, but a combined
     * image sampler still needs one). */
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (p_vkCreateSampler(s_dev, &sci, NULL, &s_samp) != VK_SUCCESS) return vk_die("sampler failed");

    /* Descriptor set layouts. */
    {
        VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                           VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
        VkDescriptorSetLayoutCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        ci.bindingCount = 1; ci.pBindings = &b;
        if (p_vkCreateDescriptorSetLayout(s_dev, &ci, NULL, &s_dsl_tex) != VK_SUCCESS) return vk_die("dsl tex");
        if (p_vkCreateDescriptorSetLayout(s_dev, &ci, NULL, &s_dsl_blit) != VK_SUCCESS) return vk_die("dsl blit");
        VkDescriptorSetLayoutBinding pb[2] = {
            { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
            { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1, VK_SHADER_STAGE_COMPUTE_BIT, NULL } };
        ci.bindingCount = 2; ci.pBindings = pb;
        if (p_vkCreateDescriptorSetLayout(s_dev, &ci, NULL, &s_dsl_pack) != VK_SUCCESS) return vk_die("dsl pack");
    }

    /* Descriptor pool + sets: ds_tex (1) + ds_pack (1) + blit ring (BLIT_DESC_RING). */
    {
        VkDescriptorPoolSize sizes[2] = {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, BLIT_DESC_RING + 2 },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1 } };
        VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pci.maxSets = BLIT_DESC_RING + 2; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
        if (p_vkCreateDescriptorPool(s_dev, &pci, NULL, &s_dpool) != VK_SUCCESS) return vk_die("desc pool");
        VkDescriptorSetLayout layouts[2] = { s_dsl_tex, s_dsl_pack };
        VkDescriptorSet *sets[2] = { &s_ds_tex, &s_ds_pack };
        for (int i = 0; i < 2; i++) {
            VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = s_dpool; ai.descriptorSetCount = 1; ai.pSetLayouts = &layouts[i];
            if (p_vkAllocateDescriptorSets(s_dev, &ai, sets[i]) != VK_SUCCESS) return vk_die("desc set alloc");
        }
        for (int i = 0; i < BLIT_DESC_RING; i++) {
            VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = s_dpool; ai.descriptorSetCount = 1; ai.pSetLayouts = &s_dsl_blit;
            if (p_vkAllocateDescriptorSets(s_dev, &ai, &s_ds_blit_ring[i]) != VK_SUCCESS) return vk_die("blit set alloc");
        }
        /* ds_tex: raw mirror sampled. ds_pack: hr sampled + raw storage. */
        VkDescriptorImageInfo raw_smp = { s_samp, s_raw_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo hr_smp  = { s_samp, s_vram_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo raw_st  = { VK_NULL_HANDLE, s_raw_view, VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet w[3] = {0};
        w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = s_ds_tex; w[0].dstBinding = 0;
        w[0].descriptorCount = 1; w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &raw_smp;
        w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = s_ds_pack; w[1].dstBinding = 0;
        w[1].descriptorCount = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo = &hr_smp;
        w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[2].dstSet = s_ds_pack; w[2].dstBinding = 1;
        w[2].descriptorCount = 1; w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[2].pImageInfo = &raw_st;
        p_vkUpdateDescriptorSets(s_dev, 3, w, 0, NULL);
    }

    /* Pipeline layouts. */
    {
        VkPushConstantRange pr;
        VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT; pr.offset = 0; pr.size = sizeof(GeoPush);
        li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pr;
        if (p_vkCreatePipelineLayout(s_dev, &li, NULL, &s_pl_geo) != VK_SUCCESS) return vk_die("pl geo");
        pr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT; pr.size = sizeof(TexPush);
        li.setLayoutCount = 1; li.pSetLayouts = &s_dsl_tex;
        if (p_vkCreatePipelineLayout(s_dev, &li, NULL, &s_pl_tex) != VK_SUCCESS) return vk_die("pl tex");
        pr.size = sizeof(BlitPush); li.pSetLayouts = &s_dsl_blit;
        if (p_vkCreatePipelineLayout(s_dev, &li, NULL, &s_pl_blit) != VK_SUCCESS) return vk_die("pl blit");
        pr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; pr.size = sizeof(PackPush); li.pSetLayouts = &s_dsl_pack;
        if (p_vkCreatePipelineLayout(s_dev, &li, NULL, &s_pl_pack) != VK_SUCCESS) return vk_die("pl pack");
    }

    /* Shader modules (kept for lazy graphics-pipeline creation). */
    s_mod_geo_v  = make_module(spv_geo_vert, spv_geo_vert_size);
    s_mod_geo_f  = make_module(spv_geo_frag, spv_geo_frag_size);
    s_mod_tex_v  = make_module(spv_geo_tex_vert, spv_geo_tex_vert_size);
    s_mod_tex_f  = make_module(spv_geo_tex_frag, spv_geo_tex_frag_size);
    s_mod_blit_v = make_module(spv_blit_vert, spv_blit_vert_size);
    s_mod_blit_f = make_module(spv_blit_frag, spv_blit_frag_size);
    if (!s_mod_geo_v || !s_mod_geo_f || !s_mod_tex_v || !s_mod_tex_f || !s_mod_blit_v || !s_mod_blit_f)
        return vk_die("shader module failed");

    /* Pack compute pipeline. */
    {
        VkShaderModule cm = make_module(spv_pack_comp, spv_pack_comp_size);
        if (!cm) return vk_die("pack module failed");
        VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = cm; ci.stage.pName = "main";
        ci.layout = s_pl_pack;
        VkResult r = p_vkCreateComputePipelines(s_dev, VK_NULL_HANDLE, 1, &ci, NULL, &s_pipe_pack);
        p_vkDestroyShaderModule(s_dev, cm, NULL);
        if (r != VK_SUCCESS) return vk_die("pack pipeline failed");
    }

    /* Vertex buffers (untextured + textured). */
    if (!create_buffer_mapped((VkDeviceSize)VK_VBUF_VERTS * sizeof(Vert),
                              &s_vbuf, &s_vbuf_mem, (void**)&s_vmap)) return 0;
    if (!create_buffer_mapped((VkDeviceSize)VK_TBUF_VERTS * sizeof(TexVert),
                              &s_tbuf, &s_tbuf_mem, (void**)&s_tmap)) return 0;
    gpu_sync();   /* complete the init clears (VRAM/raw/stencil) before any frame */
    s_ready = 1;
    /* CPU VRAM writes made before the context existed (facade routed to us
     * since gr_init, s_ready was 0) accumulated only in the CPU array — seed
     * a full-frame upload so the GPU images start coherent with it. */
    up_add(0, 0, VRAM_W - 1, VRAM_H - 1);
    flush_cpu_upload();
    return 1;
}

/* Lazily build + cache a graphics pipeline for (program, topology, blend,
 * stencil). prog 0 GEO / 1 TEX / 2 BLIT; topo 0 tri / 1 line; blend 0 off else
 * PS1 mode+1; stencil 0 SET / 1 CHECK-keep / 2 CHECK-invert. */
static VkPipeline get_pipeline(int prog, int topo, int blend, int stencil, int cmask) {
    int key = (((prog * PIPE_TOPOS + topo) * PIPE_BLENDS + blend) * PIPE_STENCILS + stencil)
              * PIPE_CMASKS + cmask;
    if (s_pipe_cache[key]) return s_pipe_cache[key];

    VkShaderModule vs, fs; VkPipelineLayout layout;
    VkVertexInputBindingDescription bind = {0};
    VkVertexInputAttributeDescription attrs[9]; uint32_t nattr;
    if (prog == 0) {            /* GEO */
        vs = s_mod_geo_v; fs = s_mod_geo_f; layout = s_pl_geo;
        bind = (VkVertexInputBindingDescription){ 0, sizeof(Vert), VK_VERTEX_INPUT_RATE_VERTEX };
        attrs[0] = (VkVertexInputAttributeDescription){ 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };
        attrs[1] = (VkVertexInputAttributeDescription){ 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 8 };
        nattr = 2;
    } else if (prog == 1) {     /* TEX */
        vs = s_mod_tex_v; fs = s_mod_tex_f; layout = s_pl_tex;
        bind = (VkVertexInputBindingDescription){ 0, sizeof(TexVert), VK_VERTEX_INPUT_RATE_VERTEX };
        attrs[0] = (VkVertexInputAttributeDescription){ 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };
        attrs[1] = (VkVertexInputAttributeDescription){ 1, 0, VK_FORMAT_R32G32_SFLOAT, 8 };
        attrs[2] = (VkVertexInputAttributeDescription){ 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16 };
        attrs[3] = (VkVertexInputAttributeDescription){ 3, 0, VK_FORMAT_R32G32_SFLOAT, 32 };
        attrs[4] = (VkVertexInputAttributeDescription){ 4, 0, VK_FORMAT_R32G32_SFLOAT, 40 };
        attrs[5] = (VkVertexInputAttributeDescription){ 5, 0, VK_FORMAT_R32_SFLOAT, 48 };
        attrs[6] = (VkVertexInputAttributeDescription){ 6, 0, VK_FORMAT_R32_SFLOAT, 52 };
        attrs[7] = (VkVertexInputAttributeDescription){ 7, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 56 };
        attrs[8] = (VkVertexInputAttributeDescription){ 8, 0, VK_FORMAT_R32_SFLOAT, 72 };  /* a_q */
        nattr = 9;
    } else {                    /* BLIT (vertex-less: rect from push constant) */
        vs = s_mod_blit_v; fs = s_mod_blit_f; layout = s_pl_blit;
        nattr = 0;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vin.vertexBindingDescriptionCount = nattr ? 1 : 0; vin.pVertexBindingDescriptions = nattr ? &bind : NULL;
    vin.vertexAttributeDescriptionCount = nattr; vin.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = topo == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    /* Blend (PS1 modes). Alpha always replaced so the colour-alpha mask mirror
     * is exact (src ONE / dst ZERO). */
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = cmask ? 0 : 0xF;
    float bc[4] = {1,1,1,1};
    if (blend == 0) {
        cba.blendEnable = VK_FALSE;
    } else {
        cba.blendEnable = VK_TRUE;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
        switch (blend - 1) {
        case 0: /* B/2 + F/2 */
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
            cba.colorBlendOp = VK_BLEND_OP_ADD; bc[0]=bc[1]=bc[2]=bc[3]=0.5f; break;
        case 1: /* B + F */
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE; cba.colorBlendOp = VK_BLEND_OP_ADD; break;
        case 2: /* B - F */
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.colorBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT; break;
        case 3: /* B + F/4 */
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.colorBlendOp = VK_BLEND_OP_ADD; bc[0]=bc[1]=bc[2]=bc[3]=0.25f; break;
        }
    }
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    cb.blendConstants[0]=bc[0]; cb.blendConstants[1]=bc[1]; cb.blendConstants[2]=bc[2]; cb.blendConstants[3]=bc[3];

    /* Stencil = bit15 mirror (bit0). SET: always+replace, dyn ref. CHECK: test
     * stored==0; write 1 via INVERT (stencil 2) or leave (stencil 1). */
    VkStencilOpState st = {0};
    st.compareMask = 0x01; st.writeMask = 0x01; st.reference = 0;
    if (stencil == 0)      { st.compareOp = VK_COMPARE_OP_ALWAYS; st.failOp = VK_STENCIL_OP_KEEP; st.passOp = VK_STENCIL_OP_REPLACE; st.depthFailOp = VK_STENCIL_OP_KEEP; }
    else if (stencil == 1) { st.compareOp = VK_COMPARE_OP_EQUAL;  st.failOp = VK_STENCIL_OP_KEEP; st.passOp = VK_STENCIL_OP_KEEP;    st.depthFailOp = VK_STENCIL_OP_KEEP; }
    else                   { st.compareOp = VK_COMPARE_OP_EQUAL;  st.failOp = VK_STENCIL_OP_KEEP; st.passOp = VK_STENCIL_OP_INVERT;  st.depthFailOp = VK_STENCIL_OP_KEEP; }
    VkPipelineDepthStencilStateCreateInfo dss = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    dss.depthTestEnable = VK_FALSE; dss.depthWriteEnable = VK_FALSE;
    dss.stencilTestEnable = VK_TRUE; dss.front = st; dss.back = st;

    VkDynamicState dyn[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 3; dy.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo pi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pi.stageCount = 2; pi.pStages = stages;
    pi.pVertexInputState = &vin; pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp; pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms; pi.pDepthStencilState = &dss; pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy; pi.layout = layout; pi.renderPass = s_rpass;
    VkPipeline p = VK_NULL_HANDLE;
    if (p_vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &pi, NULL, &p) != VK_SUCCESS) {
        vk_die("graphics pipeline create failed"); return VK_NULL_HANDLE;
    }
    s_pipe_cache[key] = p;
    return p;
}

/* PSX 1555 -> RGBA8 (bit15 mask -> alpha). */
static inline void rgb555_to_rgba8(uint16_t p, uint8_t out[4]) {
    out[0] = (uint8_t)(((p)        & 0x1F) << 3);
    out[1] = (uint8_t)(((p >> 5)   & 0x1F) << 3);
    out[2] = (uint8_t)(((p >> 10)  & 0x1F) << 3);
    out[3] = (uint8_t)((p & 0x8000) ? 0xFF : 0x00);
}
static inline uint16_t rgba8_to_rgb555(const uint8_t in[4]) {
    return (uint16_t)(((in[0] >> 3) & 0x1F) |
                      (((in[1] >> 3) & 0x1F) << 5) |
                      (((in[2] >> 3) & 0x1F) << 10) |
                      (in[3] >= 0x80 ? 0x8000 : 0));
}

int vk_renderer_init_context(SDL_Window *win) {
    s_win = win;
#if defined(PSX_SDL3)
    if (!SDL_Vulkan_LoadLibrary(NULL))
#else
    if (SDL_Vulkan_LoadLibrary(NULL) != 0)
#endif
        return vk_die("SDL_Vulkan_LoadLibrary failed (no Vulkan ICD?)");
    p_vkGetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!p_vkGetInstanceProcAddr) return vk_die("no vkGetInstanceProcAddr from SDL");

    if (!load_global_funcs()) return 0;
    if (!create_instance()) return 0;
    if (!load_instance_funcs()) return 0;
#if defined(PSX_SDL3)
    if (!SDL_Vulkan_CreateSurface(s_win, s_instance, NULL, &s_surface))
#else
    if (!SDL_Vulkan_CreateSurface(s_win, s_instance, &s_surface))
#endif
        return vk_die("SDL_Vulkan_CreateSurface failed");
    if (!pick_device()) return 0;
    if (!create_device()) return 0;

    VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = s_qfam;
    if (p_vkCreateCommandPool(s_dev, &pi, NULL, &s_cmd_pool) != VK_SUCCESS)
        return vk_die("vkCreateCommandPool failed");
    /* Work pool for one-shot ops, bulk-reset at gpu_sync (TRANSIENT: short-lived
     * CBs). */
    pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (p_vkCreateCommandPool(s_dev, &pi, NULL, &s_work_pool) != VK_SUCCESS)
        return vk_die("vkCreateCommandPool(work) failed");

    if (!create_swapchain()) return 0;
    if (!create_sync()) return 0;
    if (!create_vram_image()) return 0;
    if (!create_render_targets()) return 0;

    s_ctx_ok = 1;
    vk_log("instance/device/swapchain/VRAM + raw mirror/pipelines ready");
    return 1;
}

static void cpres_cache_free(void);   /* fwd: CPU-present resource cache */
static void osd_staging_free(void);
void vk_renderer_shutdown(void) {
    if (!s_dev) return;
    p_vkDeviceWaitIdle(s_dev);
    vk_gpu_sync_internal();   /* reclaim deferred staging before tearing down */
    cpres_cache_free();       /* FMV CPU-present cached image + staging */
    osd_staging_free();       /* host toast OSD staging */
    for (int i = 0; i < STAGING_CACHE_MAX; ++i) {
        if (s_staging_cache[i].buf)
            staging_destroy(s_staging_cache[i].buf, s_staging_cache[i].mem);
    }
    wide_free_all();          /* native-wide surfaces (color + DS + framebuffers) */
    for (int i = 0; i < PIPE_CACHE_N; i++)
        if (s_pipe_cache[i]) p_vkDestroyPipeline(s_dev, s_pipe_cache[i], NULL);
    if (s_pipe_pack)   p_vkDestroyPipeline(s_dev, s_pipe_pack, NULL);
    if (s_mod_geo_v)   p_vkDestroyShaderModule(s_dev, s_mod_geo_v, NULL);
    if (s_mod_geo_f)   p_vkDestroyShaderModule(s_dev, s_mod_geo_f, NULL);
    if (s_mod_tex_v)   p_vkDestroyShaderModule(s_dev, s_mod_tex_v, NULL);
    if (s_mod_tex_f)   p_vkDestroyShaderModule(s_dev, s_mod_tex_f, NULL);
    if (s_mod_blit_v)  p_vkDestroyShaderModule(s_dev, s_mod_blit_v, NULL);
    if (s_mod_blit_f)  p_vkDestroyShaderModule(s_dev, s_mod_blit_f, NULL);
    if (s_pl_geo)  p_vkDestroyPipelineLayout(s_dev, s_pl_geo, NULL);
    if (s_pl_tex)  p_vkDestroyPipelineLayout(s_dev, s_pl_tex, NULL);
    if (s_pl_blit) p_vkDestroyPipelineLayout(s_dev, s_pl_blit, NULL);
    if (s_pl_pack) p_vkDestroyPipelineLayout(s_dev, s_pl_pack, NULL);
    if (s_dpool)    p_vkDestroyDescriptorPool(s_dev, s_dpool, NULL);
    if (s_dsl_tex)  p_vkDestroyDescriptorSetLayout(s_dev, s_dsl_tex, NULL);
    if (s_dsl_pack) p_vkDestroyDescriptorSetLayout(s_dev, s_dsl_pack, NULL);
    if (s_dsl_blit) p_vkDestroyDescriptorSetLayout(s_dev, s_dsl_blit, NULL);
    if (s_samp)     p_vkDestroySampler(s_dev, s_samp, NULL);
    if (s_fbo)         p_vkDestroyFramebuffer(s_dev, s_fbo, NULL);
    if (s_rpass)       p_vkDestroyRenderPass(s_dev, s_rpass, NULL);
    if (s_vram_view)   p_vkDestroyImageView(s_dev, s_vram_view, NULL);
    if (s_raw_view)    p_vkDestroyImageView(s_dev, s_raw_view, NULL);
    if (s_ds_view)     p_vkDestroyImageView(s_dev, s_ds_view, NULL);
    if (s_scratch_view)p_vkDestroyImageView(s_dev, s_scratch_view, NULL);
    if (s_up_view)     p_vkDestroyImageView(s_dev, s_up_view, NULL);
    if (s_vbuf)        p_vkDestroyBuffer(s_dev, s_vbuf, NULL);
    if (s_vbuf_mem)    p_vkFreeMemory(s_dev, s_vbuf_mem, NULL);
    if (s_tbuf)        p_vkDestroyBuffer(s_dev, s_tbuf, NULL);
    if (s_tbuf_mem)    p_vkFreeMemory(s_dev, s_tbuf_mem, NULL);
    if (s_vram_img) { p_vkDestroyImage(s_dev, s_vram_img, NULL); s_vram_img = VK_NULL_HANDLE; }
    if (s_vram_mem) { p_vkFreeMemory(s_dev, s_vram_mem, NULL); s_vram_mem = VK_NULL_HANDLE; }
    if (s_raw_img)     p_vkDestroyImage(s_dev, s_raw_img, NULL);
    if (s_raw_mem)     p_vkFreeMemory(s_dev, s_raw_mem, NULL);
    if (s_ds_img)      p_vkDestroyImage(s_dev, s_ds_img, NULL);
    if (s_ds_mem)      p_vkFreeMemory(s_dev, s_ds_mem, NULL);
    if (s_scratch_img) p_vkDestroyImage(s_dev, s_scratch_img, NULL);
    if (s_scratch_mem) p_vkFreeMemory(s_dev, s_scratch_mem, NULL);
    if (s_up_img)      p_vkDestroyImage(s_dev, s_up_img, NULL);
    if (s_up_mem)      p_vkFreeMemory(s_dev, s_up_mem, NULL);
    for (int i = 0; i < VK_FRAMES; i++) {
        if (s_sem_acquire[i]) p_vkDestroySemaphore(s_dev, s_sem_acquire[i], NULL);
        if (s_sem_render[i])  p_vkDestroySemaphore(s_dev, s_sem_render[i], NULL);
        if (s_fence[i])       p_vkDestroyFence(s_dev, s_fence[i], NULL);
    }
    destroy_swapchain();
    if (s_work_pool) p_vkDestroyCommandPool(s_dev, s_work_pool, NULL);
    if (s_cmd_pool) p_vkDestroyCommandPool(s_dev, s_cmd_pool, NULL);
    p_vkDestroyDevice(s_dev, NULL); s_dev = VK_NULL_HANDLE;
    if (s_surface) p_vkDestroySurfaceKHR(s_instance, s_surface, NULL);
    if (s_instance) p_vkDestroyInstance(s_instance, NULL);
    s_ctx_ok = 0;
}

void vk_renderer_set_present_mode(int mode) { s_present_mode_req = mode; }

/* ---- present ----------------------------------------------------------- */
/* Acquire a swapchain image, run `record` (which leaves it in
 * PRESENT_SRC_KHR-ready state), submit, present. record may be NULL for a
 * blank (the image is cleared to black). Returns 1 on success. */
static int acquire_present(VkImage *out_sc, VkCommandBuffer *out_cb,
                           uint32_t *out_idx, uint32_t *out_frame) {
    uint32_t fr = s_frame_idx % VK_FRAMES;
    p_vkWaitForFences(s_dev, 1, &s_fence[fr], VK_TRUE, UINT64_MAX);
    uint32_t img_idx = 0;
    uint64_t acquire_start = SDL_GetPerformanceCounter();
    VkResult r = p_vkAcquireNextImageKHR(s_dev, s_swapchain, UINT64_MAX,
                                         s_sem_acquire[fr], VK_NULL_HANDLE, &img_idx);
    s_perf_cur.acquire_us += perf_elapsed_us(acquire_start);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        p_vkDeviceWaitIdle(s_dev);
        destroy_swapchain();
        if (!create_swapchain()) return 0;
        return 0;  /* skip this frame; next one uses the new swapchain */
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return 0;
    p_vkResetFences(s_dev, 1, &s_fence[fr]);
    p_vkResetCommandBuffer(s_cmd[fr], 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    p_vkBeginCommandBuffer(s_cmd[fr], &bi);
    *out_sc = s_sc_images[img_idx];
    *out_cb = s_cmd[fr];
    *out_idx = img_idx;
    *out_frame = fr;
    return 1;
}

/* Copy host toast into the swapchain (must still be TRANSFER_DST). Opaque
 * overwrite — host_osd bakes an opaque panel so no blend pass is needed. */
static VkBuffer       s_osd_buf  = VK_NULL_HANDLE;
static VkDeviceMemory s_osd_mem  = VK_NULL_HANDLE;
static void          *s_osd_map  = NULL;
static VkDeviceSize   s_osd_cap  = 0;

static void osd_staging_free(void) {
    if (s_osd_buf) {
        staging_destroy(s_osd_buf, s_osd_mem);
        s_osd_buf = VK_NULL_HANDLE;
        s_osd_mem = VK_NULL_HANDLE;
    }
    s_osd_map = NULL;
    s_osd_cap = 0;
}

static void vk_osd_copy_rect(VkCommandBuffer cb, VkImage sc,
                             const uint32_t *px, int w, int h,
                             int x, int y, VkDeviceSize buf_off) {
    if (!px || w <= 0 || h <= 0) return;
    int dw = w, dh = h;
    if (x + dw > (int)s_sc_extent.width) dw = (int)s_sc_extent.width - x;
    if (y + dh > (int)s_sc_extent.height) dh = (int)s_sc_extent.height - y;
    if (dw < 1 || dh < 1) return;
    memcpy((uint8_t *)s_osd_map + (size_t)buf_off, px,
           (size_t)w * (size_t)h * 4u);
    VkBufferImageCopy bc = {0};
    bc.bufferOffset = buf_off;
    bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bc.imageSubresource.layerCount = 1;
    bc.imageOffset.x = x;
    bc.imageOffset.y = y;
    bc.imageExtent.width = (uint32_t)dw;
    bc.imageExtent.height = (uint32_t)dh;
    bc.imageExtent.depth = 1;
    bc.bufferRowLength = (uint32_t)w;
    bc.bufferImageHeight = (uint32_t)h;
    p_vkCmdCopyBufferToImage(cb, s_osd_buf, sc,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
}

static void vk_osd_blit(VkCommandBuffer cb, VkImage sc) {
    const uint32_t *text_px = NULL, *vol_px = NULL, *rw_px = NULL, *ssm_px = NULL;
    int tw = 0, th = 0, vw = 0, vh = 0, rw = 0, rh = 0, sw = 0, sh = 0;
    const int have_text = host_osd_image(&text_px, &tw, &th) && text_px;
    const int have_vol = host_osd_volume_image(&vol_px, &vw, &vh) && vol_px;
    const int have_rw = psx_rewind_overlay_image(&rw_px, &rw, &rh) && rw_px;
    const int have_ssm =
        psx_savestate_menu_overlay_image(&ssm_px, &sw, &sh) && ssm_px;
    if (!have_text && !have_vol && !have_rw && !have_ssm) {
        host_osd_present_done();
        return;
    }
    /* Only BGRA swapchains match ARGB8888 LE packing used by host_osd. */
    if (s_sc_format != VK_FORMAT_B8G8R8A8_UNORM &&
        s_sc_format != VK_FORMAT_B8G8R8A8_SRGB) {
        host_osd_present_done();
        return;
    }
    VkDeviceSize text_bytes =
        have_text ? (VkDeviceSize)tw * (VkDeviceSize)th * 4u : 0;
    VkDeviceSize vol_bytes =
        have_vol ? (VkDeviceSize)vw * (VkDeviceSize)vh * 4u : 0;
    VkDeviceSize rw_bytes =
        have_rw ? (VkDeviceSize)rw * (VkDeviceSize)rh * 4u : 0;
    VkDeviceSize ssm_bytes =
        have_ssm ? (VkDeviceSize)sw * (VkDeviceSize)sh * 4u : 0;
    VkDeviceSize bytes = text_bytes + vol_bytes + rw_bytes + ssm_bytes;
    if (bytes > s_osd_cap) {
        p_vkQueueWaitIdle(s_queue);
        osd_staging_free();
        if (!make_staging(bytes, &s_osd_buf, &s_osd_mem, &s_osd_map)) {
            host_osd_present_done();
            return;
        }
        s_osd_cap = bytes;
    }
    const int margin = 8;
    VkDeviceSize off = 0;
    if (have_text) {
        vk_osd_copy_rect(cb, sc, text_px, tw, th, margin, margin, off);
        off += text_bytes;
    }
    if (have_vol) {
        int x = ((int)s_sc_extent.width > vw + margin)
                    ? ((int)s_sc_extent.width - vw - margin)
                    : margin;
        int y = ((int)s_sc_extent.height > vh)
                    ? (((int)s_sc_extent.height - vh) / 2)
                    : margin;
        vk_osd_copy_rect(cb, sc, vol_px, vw, vh, x, y, off);
        off += vol_bytes;
    }
    if (have_rw) {
        float slide = psx_rewind_slide();
        int x = ((int)s_sc_extent.width > rw)
                    ? (((int)s_sc_extent.width - rw) / 2)
                    : 0;
        int y = (int)s_sc_extent.height - (int)((float)rh * slide + 0.5f);
        vk_osd_copy_rect(cb, sc, rw_px, rw, rh, x, y, off);
        off += rw_bytes;
    }
    if (have_ssm) {
        int x = ((int)s_sc_extent.width > sw)
                    ? (((int)s_sc_extent.width - sw) / 2)
                    : 0;
        int y = ((int)s_sc_extent.height > sh)
                    ? (((int)s_sc_extent.height - sh) / 2)
                    : 0;
        vk_osd_copy_rect(cb, sc, ssm_px, sw, sh, x, y, off);
    }
    host_osd_present_done();
}

static void submit_present(VkCommandBuffer cb, uint32_t img_idx, uint32_t fr);

static void finish_present(VkCommandBuffer cb, VkImage sc,
                           uint32_t img_idx, uint32_t fr) {
    vk_osd_blit(cb, sc);
    img_barrier(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    submit_present(cb, img_idx, fr);
}

static void submit_present(VkCommandBuffer cb, uint32_t img_idx, uint32_t fr) {
    p_vkEndCommandBuffer(cb);
    /* Present command buffers write the acquired swapchain image with
     * transfer barriers, clears, and blits. Waiting only at color attachment
     * output does not order those operations after presentation-engine
     * release on stricter drivers. */
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT |
                                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &s_sem_acquire[fr];
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &s_sem_render[fr];
    p_vkQueueSubmit(s_queue, 1, &si, s_fence[fr]);

    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &s_sem_render[fr];
    pi.swapchainCount = 1; pi.pSwapchains = &s_swapchain;
    pi.pImageIndices = &img_idx;
    uint64_t present_start = SDL_GetPerformanceCounter();
    p_vkQueuePresentKHR(s_queue, &pi);
    s_perf_cur.present_us += perf_elapsed_us(present_start);
    s_frame_idx++;
}

/* Compute a letterbox rect for the swapchain given a source aspect. */
static void letterbox(int sw, int sh, int aw, int ah, VkOffset3D off[2]) {
    /* center-fit aw:ah inside sw x sh */
    int tw = sw, th = sw * ah / aw;
    if (th > sh) { th = sh; tw = sh * aw / ah; }
    int x = (sw - tw) / 2, y = (sh - th) / 2;
    off[0].x = x;       off[0].y = y;       off[0].z = 0;
    off[1].x = x + tw;  off[1].y = y + th;  off[1].z = 1;
}

int vk_renderer_present_vram(int disp_x, int disp_y, int w, int h,
                             int linear, int force_4_3) {
    if (!s_ctx_ok) return 0;
    flush_cpu_upload();   /* displayed VRAM may include pending CPU writes */
    flush_tex_batch(); flush_geometry(); gpu_sync();   /* drain all draws; VRAM in TRANSFER_SRC */
    VkImage sc; VkCommandBuffer cb; uint32_t idx, fr;
    if (!acquire_present(&sc, &cb, &idx, &fr)) return 1; /* frame skipped/recreated */

    int S = s_scale;
    img_barrier(cb, sc, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    /* clear the swapchain (letterbox bars) to black */
    VkClearColorValue black = {{0,0,0,1}};
    VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    p_vkCmdClearColorImage(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rng);

    VkOffset3D dst[2];
    letterbox((int)s_sc_extent.width, (int)s_sc_extent.height,
              force_4_3 ? 4 : 4, force_4_3 ? 3 : 3, dst);

    VkImageBlit blit = {0};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[0].x = disp_x * S;        blit.srcOffsets[0].y = disp_y * S;
    blit.srcOffsets[1].x = (disp_x + w) * S;  blit.srcOffsets[1].y = (disp_y + h) * S;
    blit.srcOffsets[1].z = 1;
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[0] = dst[0]; blit.dstOffsets[1] = dst[1];

    /* VRAM image is parked in TRANSFER_SRC_OPTIMAL. */
    p_vkCmdBlitImage(cb, s_vram_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     1, &blit, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);

    finish_present(cb, sc, idx, fr);
    perf_snapshot_present();
    return 1;
}

void vk_renderer_present_blank(void) {
    if (!s_ctx_ok) return;
    flush_tex_batch(); flush_geometry(); gpu_sync();
    VkImage sc; VkCommandBuffer cb; uint32_t idx, fr;
    if (!acquire_present(&sc, &cb, &idx, &fr)) return;
    img_barrier(cb, sc, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue black = {{0,0,0,1}};
    VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    p_vkCmdClearColorImage(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rng);
    finish_present(cb, sc, idx, fr);
    perf_snapshot_present();
}

/* Present an ARGB8888 image (24-bit FMV / display-disabled clear) letterboxed.
 * The pixels are 0xAARRGGBB; little-endian byte order [B,G,R,A] matches a
 * B8G8R8A8_UNORM staging image, blitted straight to the swapchain. */
/* CPU-present resource cache (FMV path): the depth24 compose presents a new
 * frame 15-24x/s for minutes; creating + destroying a VkImage and a staging
 * buffer per frame is the same driver-allocation churn class the batched
 * upload fix killed. Cache both keyed by (w,h) — zero allocations at steady
 * state; the staging stays persistently mapped. Freed on resize + shutdown.
 * The per-frame vkQueueWaitIdle below also makes reuse race-free (the copy
 * from the staging has fully completed before the next frame rewrites it). */
static VkImage        s_cpres_img  = VK_NULL_HANDLE;
static VkDeviceMemory s_cpres_imem = VK_NULL_HANDLE;
static VkImageLayout  s_cpres_layout = VK_IMAGE_LAYOUT_UNDEFINED;
static VkBuffer       s_cpres_buf  = VK_NULL_HANDLE;
static VkDeviceMemory s_cpres_mem  = VK_NULL_HANDLE;
static void          *s_cpres_map  = NULL;
static int            s_cpres_w = 0, s_cpres_h = 0;

static void cpres_cache_free(void) {
    if (s_cpres_img)  { p_vkDestroyImage(s_dev, s_cpres_img, NULL);  s_cpres_img = VK_NULL_HANDLE; }
    if (s_cpres_imem) { p_vkFreeMemory(s_dev, s_cpres_imem, NULL);   s_cpres_imem = VK_NULL_HANDLE; }
    if (s_cpres_buf)  {
        staging_destroy(s_cpres_buf, s_cpres_mem);
        s_cpres_buf = VK_NULL_HANDLE;
        s_cpres_mem = VK_NULL_HANDLE;
    }
    s_cpres_map = NULL; s_cpres_w = s_cpres_h = 0;
    s_cpres_layout = VK_IMAGE_LAYOUT_UNDEFINED;
}

void vk_renderer_present_cpu(const uint32_t *pixels, int src_w, int src_h,
                             int linear, int force_4_3) {
    (void)force_4_3;
    if (!s_ctx_ok || !pixels || src_w <= 0 || src_h <= 0) { vk_renderer_present_blank(); return; }
    flush_tex_batch(); flush_geometry(); gpu_sync();

    if (s_cpres_w != src_w || s_cpres_h != src_h) {
        p_vkQueueWaitIdle(s_queue);      /* old resources may be in flight */
        cpres_cache_free();
        if (!make_staging((VkDeviceSize)src_w * src_h * 4,
                          &s_cpres_buf, &s_cpres_mem, &s_cpres_map)) {
            vk_renderer_present_blank(); return;
        }
        if (!make_image(VK_FORMAT_B8G8R8A8_UNORM, src_w, src_h,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, &s_cpres_img, &s_cpres_imem, NULL)) {
            cpres_cache_free(); vk_renderer_present_blank(); return;
        }
        s_cpres_w = src_w; s_cpres_h = src_h;
        s_cpres_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    memcpy(s_cpres_map, pixels, (size_t)src_w * src_h * 4);
    VkBuffer buf = s_cpres_buf;
    VkImage img = s_cpres_img;

    VkImage sc; VkCommandBuffer cb; uint32_t idx, fr;
    if (!acquire_present(&sc, &cb, &idx, &fr)) return;
    img_to(cb, img, &s_cpres_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy bc = {0};
    bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; bc.imageSubresource.layerCount = 1;
    bc.imageExtent.width = src_w; bc.imageExtent.height = src_h; bc.imageExtent.depth = 1;
    p_vkCmdCopyBufferToImage(cb, buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    img_to(cb, img, &s_cpres_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    img_barrier(cb, sc, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue black = {{0,0,0,1}};
    VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    p_vkCmdClearColorImage(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rng);

    VkOffset3D dst[2];
    letterbox((int)s_sc_extent.width, (int)s_sc_extent.height, 4, 3, dst);
    /* Short GP1(07h) bands: letterbox inside the 4:3 rect (see GL present). */
    if (src_h > 0 && src_h < 240) {
        int box_h = dst[1].y - dst[0].y;
        int content_h = (box_h * src_h) / 240;
        if (content_h < 1) content_h = 1;
        int y0 = dst[0].y + (box_h - content_h) / 2;
        dst[0].y = y0;
        dst[1].y = y0 + content_h;
    }
    VkImageBlit blit = {0};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[1].x = src_w; blit.srcOffsets[1].y = src_h; blit.srcOffsets[1].z = 1;
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[0] = dst[0]; blit.dstOffsets[1] = dst[1];
    p_vkCmdBlitImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     1, &blit, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    finish_present(cb, sc, idx, fr);

    /* Cached resources are rewritten next frame: wait for this frame's copy +
     * blit to complete first (FMV cadence is 15-24 fps; one idle-wait per
     * movie frame is cheap, and it is what makes the persistent staging safe). */
    p_vkQueueWaitIdle(s_queue);
    perf_snapshot_present();
}

/* GPU-direct native-wide present: blit the displayed buffer's wide surface
 * band straight to the swapchain (no readback), mirroring
 * gl_renderer_present_wide_fbo. Letterbox aspect generalises past 16:9:
 * the 4:3 native frame widened by EXTRA presents at (4*wide_w : 3*native_w).
 * Returns 0 when no wide surface exists for disp_x (caller falls back). */
int vk_renderer_present_wide(int disp_x, int disp_y, int disp_h, int linear) {
    if (!s_ctx_ok || !s_ready || s_wide_w <= 0) return 0;
    int i = -1;
    for (int k = 0; k < VK_WIDE_MAX_SURF; k++)
        if (s_wide_img[k] && s_wide_base[k] == disp_x) { i = k; break; }
    if (i < 0) return 0;
    flush_cpu_upload();
    flush_tex_batch(); flush_geometry(); gpu_sync();
    VkImage sc; VkCommandBuffer cb; uint32_t idx, fr;
    if (!acquire_present(&sc, &cb, &idx, &fr)) return 1;  /* frame skipped/recreated */

    int S = s_scale;
    img_barrier(cb, sc, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue black = {{0, 0, 0, 1}};
    VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    p_vkCmdClearColorImage(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rng);

    img_to(cb, s_wide_img[i], &s_wide_layout[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    int native_w = s_wide_w - 2 * s_wide_offset;
    if (native_w <= 0) native_w = s_wide_w;
    VkOffset3D dst[2];
    letterbox((int)s_sc_extent.width, (int)s_sc_extent.height,
              4 * s_wide_w, 3 * native_w, dst);

    int sy0 = disp_y * S, sy1 = (disp_y + disp_h) * S;
    if (sy0 < 0) sy0 = 0;
    if (sy1 > VRAM_H * S) sy1 = VRAM_H * S;
    VkImageBlit blit = {0};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[0].x = 0;              blit.srcOffsets[0].y = sy0;
    blit.srcOffsets[1].x = s_wide_w * S;   blit.srcOffsets[1].y = sy1;
    blit.srcOffsets[1].z = 1;
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[0] = dst[0]; blit.dstOffsets[1] = dst[1];
    p_vkCmdBlitImage(cb, s_wide_img[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     1, &blit, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    img_to(cb, s_wide_img[i], &s_wide_layout[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    finish_present(cb, sc, idx, fr);
    perf_snapshot_present();
    return 1;
}

void vk_renderer_sync_cpu(void) {
    if (!s_ctx_ok) return;
    ensure_cpu();   /* drain batches, pack hr->raw, copy raw->CPU mirror */
}

/* Dump the last `count` per-frame perf records as a JSON array (oldest..newest).
 * Always-on; the debug-server "vk_perf" command reads this to find which op
 * explodes during a transition stall (e.g. allocs jumping 10 -> thousands). */
int vk_perf_json(char *out, int cap, int count) {
    uint32_t total = s_perf_head;
    uint32_t n = ((uint32_t)count < total) ? (uint32_t)count : total;
    if (n > VK_PERF_RING) n = VK_PERF_RING;
    int o = snprintf(out, cap, "[");
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = total - n + i;          /* frame index, oldest..newest */
        VkPerf *p = &s_perf_ring[k % VK_PERF_RING];
        o += snprintf(out + o, cap - o,
            "%s{\"f\":%u,\"qpc\":%llu,\"wait_us\":%u,\"acquire_us\":%u,\"present_us\":%u,"
            "\"alloc\":%u,\"alloc_kb\":%u,\"oneshot\":%u,\"submit\":%u,"
            "\"sync\":%u,\"pack\":%u,\"blit\":%u,\"upload\":%u,\"copy\":%u,"
            "\"geo\":%u,\"tex\":%u,\"wide\":%u,\"wclr\":%u}",
            i ? "," : "",
            p->present_idx, (unsigned long long)p->present_qpc,
            p->wait_us, p->acquire_us, p->present_us,
            p->allocs, p->alloc_kb, p->oneshots, p->submits,
            p->syncs, p->pack_flushes, p->blits, p->upload_blocks, p->copy_rects,
            p->geo_flushes, p->tex_flushes, p->wide_passes, p->wide_clears);
        if (o >= cap - 256) break;
    }
    o += snprintf(out + o, cap - o, "]");
    return o;
}

/* ---- render-pass + coherency helpers ----------------------------------- */
/* Half an HR pixel in native units, minus 1/64 to keep exact-edge samples off
 * texel centres (see geo.vert / the GL backend's u_shift). */
static float px_shift(void) { return 0.5f / (float)s_scale - 1.0f / 64.0f; }

static void set_vp_full(VkCommandBuffer cb) {
    VkViewport vp = { 0.f, 0.f, (float)(VRAM_W * s_scale), (float)(VRAM_H * s_scale), 0.f, 1.f };
    p_vkCmdSetViewport(cb, 0, 1, &vp);
}
static void set_scissor_px(VkCommandBuffer cb, int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; } if (y < 0) { h += y; y = 0; }
    if (x + w > VRAM_W) w = VRAM_W - x; if (y + h > VRAM_H) h = VRAM_H - y;
    if (w < 0) w = 0; if (h < 0) h = 0;
    VkRect2D sc = { { x * s_scale, y * s_scale }, { (uint32_t)(w * s_scale), (uint32_t)(h * s_scale) } };
    p_vkCmdSetScissor(cb, 0, 1, &sc);
}

/* A same-layout transition is elided by img_to(), while the render pass has
 * no external dependency. Explicitly order a preceding pass's attachment
 * writes before a following LOAD/blend pass on the same color image. */
static void color_self_barrier(VkCommandBuffer cb, VkImage img) {
    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = img;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    p_vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, NULL, 0, NULL, 1, &barrier);
}

static void begin_geo_pass(VkCommandBuffer cb) {
    /* Explicit stencil ordering across one-shot submits: this pass both TESTS
     * and WRITES the stencil (PSX mask bits) that a PRIOR submit's pass wrote.
     * Cross-submit ordering previously rode on the COLOR image barriers'
     * execution dependencies alone — no memory dependency covered the DS
     * attachment, which is spec-fragile (worked on this NVIDIA driver, could
     * produce mask-bit flicker elsewhere). A self-barrier on the stencil
     * aspect (late-tests write -> early-tests read|write) makes prior stencil
     * writes visible before this pass's tests, per submission order. */
    VkImageMemoryBarrier db = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    db.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    db.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    db.srcQueueFamilyIndex = db.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    db.image = s_ds_img;
    db.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    db.subresourceRange.levelCount = 1;
    db.subresourceRange.layerCount = 1;
    db.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    db.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    p_vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        0, 0, NULL, 0, NULL, 1, &db);
    color_self_barrier(cb, s_vram_img);
    VkRenderPassBeginInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rp.renderPass = s_rpass; rp.framebuffer = s_fbo;
    rp.renderArea.extent.width = VRAM_W * s_scale;
    rp.renderArea.extent.height = VRAM_H * s_scale;
    p_vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
}
/* Bind a graphics pipeline implementing GL's mask_stencil: check==0 -> SET
 * (always+replace, ref=write_val); check!=0 -> test stored==0 and write 1 via
 * INVERT (write_val!=0) or leave (write_val==0), ref=0. */
static void bind_masked(VkCommandBuffer cb, int prog, int topo, int blend, int check, int write_val) {
    int stencil = !check ? 0 : (write_val ? 2 : 1);
    VkPipeline p = get_pipeline(prog, topo, blend, stencil, 0);
    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    p_vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, check ? 0u : (uint32_t)write_val);
}
/* Like bind_masked but with ALL colour writes disabled (stencil-only pass). */
static void bind_masked_stencil_only(VkCommandBuffer cb, int prog, int topo, int check, int write_val) {
    int stencil = !check ? 0 : (write_val ? 2 : 1);
    VkPipeline p = get_pipeline(prog, topo, 0, stencil, 1);
    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    p_vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, check ? 0u : (uint32_t)write_val);
}

/* Host-visible staging buffer (TRANSFER_SRC). */
static int make_staging(VkDeviceSize bytes, VkBuffer *buf, VkDeviceMemory *mem, void **map) {
    int best = -1;
    for (int i = 0; i < STAGING_CACHE_MAX; ++i) {
        StagingCacheEntry *entry = &s_staging_cache[i];
        if (!entry->busy && entry->buf && entry->size >= bytes &&
            (best < 0 || entry->size < s_staging_cache[best].size))
            best = i;
    }
    if (best >= 0) {
        StagingCacheEntry *entry = &s_staging_cache[best];
        entry->busy = 1;
        *buf = entry->buf;
        *mem = entry->mem;
        *map = entry->map;
        return 1;
    }

    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = bytes; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (p_vkCreateBuffer(s_dev, &bci, NULL, buf) != VK_SUCCESS) return 0;
    VkMemoryRequirements req; p_vkGetBufferMemoryRequirements(s_dev, *buf, &req);
    VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mi.allocationSize = req.size;
    mi.memoryTypeIndex = find_mem_type(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mi.memoryTypeIndex == UINT32_MAX || p_vkAllocateMemory(s_dev, &mi, NULL, mem) != VK_SUCCESS) {
        p_vkDestroyBuffer(s_dev, *buf, NULL); return 0;
    }
    p_vkBindBufferMemory(s_dev, *buf, *mem, 0);
    if (p_vkMapMemory(s_dev, *mem, 0, bytes, 0, map) != VK_SUCCESS) {
        p_vkDestroyBuffer(s_dev, *buf, NULL);
        p_vkFreeMemory(s_dev, *mem, NULL);
        return 0;
    }
    for (int i = 0; i < STAGING_CACHE_MAX; ++i) {
        if (!s_staging_cache[i].buf) {
            s_staging_cache[i].buf = *buf;
            s_staging_cache[i].mem = *mem;
            s_staging_cache[i].size = bytes;
            s_staging_cache[i].map = *map;
            s_staging_cache[i].busy = 1;
            break;
        }
    }
    s_perf_cur.allocs++;
    s_perf_cur.alloc_kb += (uint32_t)(bytes / 1024);
    return 1;
}
static void free_staging(VkBuffer buf, VkDeviceMemory mem) {
    staging_release(buf, mem);
}

/* hr -> raw mirror: pack the dirty rect via the compute pass (top-left sample of
 * each SxS block). Caller has already drained the batches that dirtied it. */
static void pack_flush(void) {
    if (!s_ready || !s_pack_dirty.set) return;
    s_perf_cur.pack_flushes++;
    int x = s_pack_dirty.x0, y = s_pack_dirty.y0;
    int w = s_pack_dirty.x1 - s_pack_dirty.x0 + 1;
    int h = s_pack_dirty.y1 - s_pack_dirty.y0 + 1;
    rect_clear(&s_pack_dirty);
    VkCommandBuffer cb = begin_oneshot();
    vram_to(cb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);     /* hr sampled */
    img_to(cb, s_raw_img, &s_raw_layout, VK_IMAGE_LAYOUT_GENERAL); /* raw storage */
    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipe_pack);
    p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_pl_pack, 0, 1, &s_ds_pack, 0, NULL);
    PackPush pp = { s_scale, x, y };
    p_vkCmdPushConstants(cb, s_pl_pack, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pp, &pp);
    p_vkCmdDispatch(cb, (uint32_t)((w + 7) / 8), (uint32_t)((h + 7) / 8), 1);
    img_to(cb, s_raw_img, &s_raw_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vram_to(cb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    end_oneshot(cb);
}

/* Ensure the raw mirror is current for a textured draw sampling the given page
 * / CLUT (mirrors the GL backend). */
static void flush_pack_if_sampling(int tpx, int tpy, int depth, int clx, int cly) {
    if (!s_pack_dirty.set) return;
    int page_w = depth == 0 ? 64 : depth == 1 ? 128 : 256;
    if (rect_intersects(&s_pack_dirty, tpx, tpy, tpx + page_w - 1, tpy + 255)) {
        flush_tex_batch(); pack_flush(); return;
    }
    if (depth <= 1) {
        int n = depth == 0 ? 16 : 256;
        if (rect_intersects(&s_pack_dirty, clx, cly, clx + n - 1, cly)) {
            flush_tex_batch(); pack_flush();
        }
    }
}

/* Blit a native [x,y,w,h] rect from src_img into the hr image (two STP passes,
 * bit15 -> stencil mirror). plain=1 (CPU upload: SET stencil, src already final)
 * or plain=0 (VRAM copy: honour mask set/check). src_div: 1 hr-res src, S native.
 * Marks the dest pack-dirty when mark_pack (copies re-derive raw from hr). */
static void blit_region(int x, int y, int w, int h, VkImageView src_view,
                        VkImage src_img, VkImageLayout *src_layout,
                        int src_div, int sox, int soy, int plain, int mark_pack) {
    if (w <= 0 || h <= 0) return;
    s_perf_cur.blits++;
    if (s_ds_blit_idx >= BLIT_DESC_RING) gpu_sync();   /* ring exhausted: drain, reset idx */
    VkDescriptorSet ds = s_ds_blit_ring[s_ds_blit_idx++];
    VkDescriptorImageInfo ii = { s_samp, src_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wr.dstSet = ds; wr.dstBinding = 0; wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &ii;
    p_vkUpdateDescriptorSets(s_dev, 1, &wr, 0, NULL);

    VkCommandBuffer cb = begin_oneshot();
    img_to(cb, src_img, src_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vram_to(cb, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    begin_geo_pass(cb);
    set_vp_full(cb); set_scissor_px(cb, x, y, w, h);
    p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pl_blit, 0, 1, &ds, 0, NULL);
    BlitPush bp = {0};
    bp.shift = px_shift(); bp.maskset = plain ? 0 : s_mask_set; bp.src_div = src_div;
    bp.src_off[0] = sox; bp.src_off[1] = soy;
    bp.rect[0] = x; bp.rect[1] = y; bp.rect[2] = x + w; bp.rect[3] = y + h;
    int chk = plain ? 0 : s_mask_check;
    /* pass 1: bit15=0 texels, write stencil 0 (or honour mask set) */
    bind_masked(cb, 2, 0, 0, chk, plain ? 0 : s_mask_set);
    bp.stp_pass = 1;
    p_vkCmdPushConstants(cb, s_pl_blit, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof bp, &bp);
    p_vkCmdDraw(cb, 6, 1, 0, 0);
    /* pass 2: bit15=1 texels, write stencil 1 */
    bind_masked(cb, 2, 0, 0, chk, 1);
    bp.stp_pass = 2;
    p_vkCmdPushConstants(cb, s_pl_blit, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof bp, &bp);
    p_vkCmdDraw(cb, 6, 1, 0, 0);
    p_vkCmdEndRenderPass(cb);
    vram_to(cb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    end_oneshot(cb);
    if (mark_pack) rect_add(&s_pack_dirty, x, y, x + w - 1, y + h - 1);
    s_gpu_dirty = 1;
}

/* ---- VRAM transfers (CPU <-> GPU image) -------------------------------- */
/* Upload a w x h block of 1555 pixels at (x,y). The raw mirror takes the exact
 * 1555 bits directly (CLUT indices must survive), and the hr render target is
 * updated via a blit so the stencil bit15 mirror stays exact. */
static void vram_upload_block(int x, int y, int w, int h, const uint16_t *data) {
    if (w <= 0 || h <= 0 || !s_ready) return;
    s_perf_cur.upload_blocks++;
    flush_tex_batch(); flush_geometry();

    /* 1) raw mirror: native 1555, exact. */
    VkBuffer rbuf; VkDeviceMemory rmem; void *rmap;
    if (make_staging((VkDeviceSize)w * h * 2, &rbuf, &rmem, &rmap)) {
        memcpy(rmap, data, (size_t)w * h * 2);
        VkCommandBuffer cb = begin_oneshot();
        img_to(cb, s_raw_img, &s_raw_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy rc = {0};
        rc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; rc.imageSubresource.layerCount = 1;
        rc.imageOffset.x = x; rc.imageOffset.y = y;
        rc.imageExtent.width = w; rc.imageExtent.height = h; rc.imageExtent.depth = 1;
        p_vkCmdCopyBufferToImage(cb, rbuf, s_raw_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rc);
        img_to(cb, s_raw_img, &s_raw_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        end_oneshot(cb);
        defer_staging(rbuf, rmem);   /* copy in flight; freed at next gpu_sync */
    }

    /* 2) native RGBA8 staging -> upload image. */
    VkBuffer ubuf; VkDeviceMemory umem; void *umap;
    if (make_staging((VkDeviceSize)w * h * 4, &ubuf, &umem, &umap)) {
        uint8_t *m = (uint8_t*)umap;
        for (int i = 0; i < w * h; i++) rgb555_to_rgba8(data[i], m + (size_t)i * 4);
        VkCommandBuffer cb = begin_oneshot();
        img_to(cb, s_up_img, &s_up_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy uc = {0};
        uc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; uc.imageSubresource.layerCount = 1;
        uc.imageOffset.x = x; uc.imageOffset.y = y;
        uc.imageExtent.width = w; uc.imageExtent.height = h; uc.imageExtent.depth = 1;
        p_vkCmdCopyBufferToImage(cb, ubuf, s_up_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &uc);
        end_oneshot(cb);
        defer_staging(ubuf, umem);   /* copy in flight; freed at next gpu_sync */
        /* 3) blit upload image -> hr (native src: src_div=S, off 0). */
        blit_region(x, y, w, h, s_up_view, s_up_img, &s_up_layout, s_scale, 0, 0, 1, 0);
    }
}

/* CPU -> GPU: flush accumulated CPU-side VRAM writes as ONE upload (see
 * s_up_rects). vram_upload_block drains the draw batches first, so PS1
 * submission order is preserved: every draw appended before these writes has
 * landed by the time the upload writes VRAM, and every GPU-op entry point
 * calls this before touching VRAM so the writes land before later ops. */
static void flush_cpu_upload(void) {
    if (s_up_nrects == 0 || !s_ready || !s_vram) return;
    /* Snapshot + clear first (re-entrancy safe: up_add overflow calls back). */
    DirtyRect rects[UP_RECTS_MAX];
    int nrects = s_up_nrects;
    memcpy(rects, s_up_rects, (size_t)nrects * sizeof(DirtyRect));
    s_up_nrects = 0;

    flush_tex_batch(); flush_geometry();   /* PS1 order: queued draws land first */
    s_perf_cur.upload_blocks++;

    /* COALESCED upload: exactly TWO queue submits per flush regardless of rect
     * count (one for the buffer->image copies, one render pass for the hr
     * blits). The naive per-rect vram_upload_block/blit_region loop paid
     * 2 staging allocs + 3 submits PER RECT — with the exact-rect list feeding
     * MDEC macroblock streams that re-created the boot-wedge submit churn the
     * batched-upload fix existed to kill (measured 0.7 fps at the Tomba2
     * Whoopee logo). Rect data is packed back-to-back in ONE 1555 staging +
     * ONE RGBA8 staging, addressed via VkBufferImageCopy bufferOffset. */
    size_t total = 0;
    for (int i = 0; i < nrects; i++)
        total += vk_upload_aligned_texels(
            (size_t)(rects[i].x1 - rects[i].x0 + 1) *
            (size_t)(rects[i].y1 - rects[i].y0 + 1));
    if (total == 0) return;

    VkBuffer rbuf; VkDeviceMemory rmem; void *rmap;
    if (!make_staging((VkDeviceSize)total * 2, &rbuf, &rmem, &rmap)) return;
    VkBuffer ubuf; VkDeviceMemory umem; void *umap;
    if (!make_staging((VkDeviceSize)total * 4, &ubuf, &umem, &umap)) {
        defer_staging(rbuf, rmem);
        return;
    }

    VkBufferImageCopy rcopies[UP_RECTS_MAX], ucopies[UP_RECTS_MAX];
    memset(rcopies, 0, sizeof rcopies); memset(ucopies, 0, sizeof ucopies);
    size_t texoff = 0;
    uint16_t *rdst = (uint16_t *)rmap;
    uint8_t  *udst = (uint8_t *)umap;
    for (int i = 0; i < nrects; i++) {
        int x = rects[i].x0, y = rects[i].y0;
        int w = rects[i].x1 - rects[i].x0 + 1;
        int h = rects[i].y1 - rects[i].y0 + 1;
        for (int row = 0; row < h; row++) {
            const uint16_t *src = s_vram + (size_t)(y + row) * VRAM_W + x;
            memcpy(rdst + texoff + (size_t)row * w, src, (size_t)w * 2);
            uint8_t *m = udst + (texoff + (size_t)row * w) * 4;
            for (int col = 0; col < w; col++)
                rgb555_to_rgba8(src[col], m + (size_t)col * 4);
        }
        rcopies[i].bufferOffset = (VkDeviceSize)texoff * 2;
        rcopies[i].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        rcopies[i].imageSubresource.layerCount = 1;
        rcopies[i].imageOffset.x = x; rcopies[i].imageOffset.y = y;
        rcopies[i].imageExtent.width = w; rcopies[i].imageExtent.height = h;
        rcopies[i].imageExtent.depth = 1;
        ucopies[i] = rcopies[i];
        ucopies[i].bufferOffset = (VkDeviceSize)texoff * 4;
        /* Padding prevents an odd-sized R16 rect from leaving all following
         * VkBufferImageCopy offsets at two modulo four bytes. */
        texoff += vk_upload_aligned_texels((size_t)w * h);
    }

    /* Submit 1: both image copy sets. */
    VkCommandBuffer cb = begin_oneshot();
    img_to(cb, s_raw_img, &s_raw_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    p_vkCmdCopyBufferToImage(cb, rbuf, s_raw_img,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             (uint32_t)nrects, rcopies);
    img_to(cb, s_raw_img, &s_raw_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    img_to(cb, s_up_img, &s_up_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    p_vkCmdCopyBufferToImage(cb, ubuf, s_up_img,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             (uint32_t)nrects, ucopies);
    end_oneshot(cb);
    defer_staging(rbuf, rmem);
    defer_staging(ubuf, umem);

    /* Submit 2: one render pass, N scissored 2-draw blits upload-image -> hr
     * (plain blit semantics, matching vram_upload_block's blit_region call:
     * mask writes off, no pack mark — uploaded content IS the CPU data). */
    if (s_ds_blit_idx >= BLIT_DESC_RING) gpu_sync();
    VkDescriptorSet ds = s_ds_blit_ring[s_ds_blit_idx++];
    VkDescriptorImageInfo ii = { s_samp, s_up_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wr.dstSet = ds; wr.dstBinding = 0; wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &ii;
    p_vkUpdateDescriptorSets(s_dev, 1, &wr, 0, NULL);
    cb = begin_oneshot();
    img_to(cb, s_up_img, &s_up_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vram_to(cb, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    begin_geo_pass(cb);
    set_vp_full(cb);
    p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pl_blit, 0, 1, &ds, 0, NULL);
    for (int i = 0; i < nrects; i++) {
        int x = rects[i].x0, y = rects[i].y0;
        int w = rects[i].x1 - rects[i].x0 + 1;
        int h = rects[i].y1 - rects[i].y0 + 1;
        set_scissor_px(cb, x, y, w, h);
        s_perf_cur.blits++;
        BlitPush bp = {0};
        bp.shift = px_shift(); bp.maskset = 0; bp.src_div = s_scale;
        bp.src_off[0] = 0; bp.src_off[1] = 0;
        bp.rect[0] = x; bp.rect[1] = y; bp.rect[2] = x + w; bp.rect[3] = y + h;
        bind_masked(cb, 2, 0, 0, 0, 0);
        bp.stp_pass = 1;
        p_vkCmdPushConstants(cb, s_pl_blit, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof bp, &bp);
        p_vkCmdDraw(cb, 6, 1, 0, 0);
        bind_masked(cb, 2, 0, 0, 0, 1);
        bp.stp_pass = 2;
        p_vkCmdPushConstants(cb, s_pl_blit, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof bp, &bp);
        p_vkCmdDraw(cb, 6, 1, 0, 0);
    }
    p_vkCmdEndRenderPass(cb);
    vram_to(cb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    end_oneshot(cb);
    s_gpu_dirty = 1;
}

/* GPU -> CPU mirror: drain batches, pack, copy the whole raw mirror down. */
static void ensure_cpu(void) {
    extern int psx_netplay_active(void);
    if (!s_ready || !s_gpu_dirty || !s_vram) return;
    if (psx_netplay_active()) {
        s_gpu_dirty = 0;
        return;
    }
    flush_cpu_upload();   /* readback overwrites s_vram: pending writes land first */
    flush_tex_batch(); flush_geometry();
    /* Readback must reflect ALL current hr content, not just the incremental
     * s_pack_dirty rect. pack_flush() packs only that rect and early-returns if
     * it is unset — so draws that updated hr (and present) but never marked the
     * pack rect (e.g. the BIOS shell after the logo) left the raw mirror stale,
     * and screenshots/guest VRAM reads returned an old frame (the logo) while
     * the live window showed the shell. Force a full-frame pack here; this path
     * is readback-only (NOT the per-frame present path), so the cost is paid
     * only on screenshots / guest VRAM reads, never every frame. */
    rect_add(&s_pack_dirty, 0, 0, VRAM_W - 1, VRAM_H - 1);
    pack_flush();
    VkBuffer buf; VkDeviceMemory mem; void *map;
    if (!make_staging((VkDeviceSize)VRAM_W * VRAM_H * 2, &buf, &mem, &map)) return;
    VkCommandBuffer cb = begin_oneshot();
    img_to(cb, s_raw_img, &s_raw_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy rc = {0};
    rc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; rc.imageSubresource.layerCount = 1;
    rc.imageExtent.width = VRAM_W; rc.imageExtent.height = VRAM_H; rc.imageExtent.depth = 1;
    p_vkCmdCopyImageToBuffer(cb, s_raw_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &rc);
    img_to(cb, s_raw_img, &s_raw_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    end_oneshot(cb);
    gpu_sync();   /* the readback result must be complete before we read it */
    memcpy(s_vram, map, (size_t)VRAM_W * VRAM_H * 2);
    free_staging(buf, mem);
    s_gpu_dirty = 0;
}

/* ---- backend vtable ---------------------------------------------------- */
/* Also initialize the software renderer on the SAME VRAM array so a failed
 * Vulkan context creation can fall back to software cleanly (mirrors how the GL
 * backend's init calls sw_renderer_init). When Vulkan is active the SW mirror
 * is just the CPU readback target. */
static void vkb_init(uint16_t *vram) { s_vram = vram; s_cpu_dirty = 0; sw_renderer_init(vram); }
static void vkb_set_scale(int scale) { if (scale >= 1 && scale <= 4) s_scale = scale; }
static int  vkb_scale(void) { return s_scale; }
static void vkb_set_texture_filter(int b) { s_texfilter = b ? 1 : 0; }
static int  vkb_texture_filter(void) { return s_texfilter; }

static void vkb_set_draw_area(int x1, int y1, int x2, int y2) {
    /* Scissor is per-batch (one render pass uses one scissor), so drain both
     * independent geometry batches before the clip rect changes. Otherwise
     * earlier textured GP0 primitives render with the next draw area's scissor. */
    if (x1 != s_da_x1 || y1 != s_da_y1 || x2 != s_da_x2 || y2 != s_da_y2) {
        flush_tex_batch();
        flush_geometry();
    }
    s_da_x1 = x1; s_da_y1 = y1; s_da_x2 = x2; s_da_y2 = y2;
}
static void vkb_get_draw_area(int *x1, int *y1, int *x2, int *y2) {
    if (x1) *x1 = s_da_x1; if (y1) *y1 = s_da_y1; if (x2) *x2 = s_da_x2; if (y2) *y2 = s_da_y2;
}
static void vkb_set_draw_offset(int x, int y) { s_off_x = x; s_off_y = y; }

/* GP0 02h fill: set a VRAM rect to a solid 1555 colour. The write lands in
 * the CPU array immediately (like every CPU-side VRAM write) and rides the
 * batched s_up_rects upload — the old per-fill staging upload was part of
 * the per-op alloc/submit churn. */
static void vkb_fill_rect(int x, int y, int w, int h, uint16_t color) {
    sw_fill_rect(x, y, w, h, color);
    if (!s_ctx_ok) return;
    if (x < 0) { w += x; x = 0; } if (y < 0) { h += y; y = 0; }
    if (x + w > VRAM_W) w = VRAM_W - x; if (y + h > VRAM_H) h = VRAM_H - y;
    if (w <= 0 || h <= 0) return;
    up_add(x, y, x + w - 1, y + h - 1);
}

/* Same policy as GL: skip framebuffer-sized depth24 (RGB888) transfers only;
 * still stage smaller texture A0s. On leave clear skipped FB union — never
 * restage packed RGB888 as 1555. */
static int s_depth24_skip_up = 0;
static DirtyRect s_d24_skip_fb;

static int depth24_is_fb_transfer(int x, int y, int w, int h) {
    if (!gpu_display_is_depth24() || w <= 0 || h <= 0) return 0;
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    int fb_w = (int)((di.width * 3u + 1u) / 2u);
    int fb_h = (int)di.height;
    if (fb_w < 8) fb_w = 8;
    if (fb_h < 1) fb_h = 1;
    /* See GL: 256×256 pages must not match the half-FB area heuristic. */
    if (w <= 256 && h <= 256) return 0;
    {
        int dx = (int)(di.display_x & 1023u);
        int dy = (int)(di.display_y & 511u);
        int x0 = x & (VRAM_W - 1), y0 = y & (VRAM_H - 1);
        if (x0 + w <= dx || x0 >= dx + fb_w || y0 + h <= dy || y0 >= dy + fb_h)
            return 0;
    }
    if (h >= fb_h - 8 && h <= fb_h + 16 && w >= (fb_w * 3) / 4) return 1;
    if ((int64_t)w * (int64_t)h >= ((int64_t)fb_w * fb_h) / 2) return 1;
    return 0;
}

static void depth24_mark_scanout_band(void) {
    GpuDisplayInfo di;
    int fb_w, fb_h, x0, y0, x1, y1;
    if (!gpu_display_is_depth24()) return;
    gpu_get_display_info(&di);
    fb_w = (int)((di.width * 3u + 1u) / 2u);
    fb_h = (int)di.height;
    if (fb_w < 8) fb_w = 8;
    if (fb_h < 1) fb_h = 1;
    x0 = (int)(di.display_x & 1023u);
    y0 = (int)(di.display_y & 511u);
    x1 = x0 + fb_w - 1;
    y1 = y0 + fb_h - 1;
    if (x1 > VRAM_W - 1) x1 = VRAM_W - 1;
    if (y1 > VRAM_H - 1) y1 = VRAM_H - 1;
    rect_add(&s_d24_skip_fb, x0, y0, x1, y1);
}

static void depth24_clear_skipped_fb(void) {
    /* GL scissor-clears the skipped FB union. VK keeps texture uploads that
     * landed during depth24; the FB bands are overwritten by the next 15-bit
     * draws. (A full-image ClearColorImage would wipe those textures.) */
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

static void vkb_vram_transfer_in(int x, int y, int w, int h, const uint16_t *data) {
    sw_vram_transfer_in(x, y, w, h, data);
    if (!s_ctx_ok) return;
    depth24_upload_policy();
    if (s_depth24_skip_up && depth24_is_fb_transfer(x, y, w, h)) {
        /* Full-VRAM savestate restore: stage FBO; mark scanout band only. */
        if (w >= VRAM_W && h >= VRAM_H) {
            up_add_transfer(x, y, w, h);
            rect_clear(&s_d24_skip_fb);
            depth24_mark_scanout_band();
            return;
        }
        depth24_mark_scanout_band();
        return;
    }
    up_add_transfer(x, y, w, h);   /* exact touched rects, incl. per-pixel wrap */
}

void vk_renderer_restage_vram_after_savestate(void) {
    if (!s_ctx_ok || !s_vram) return;
    s_up_nrects = 0;
    rect_clear(&s_d24_skip_fb);
    s_depth24_skip_up = 0;
    up_add_transfer(0, 0, VRAM_W, VRAM_H);
    flush_cpu_upload();
    if (gpu_display_is_depth24()) {
        s_depth24_skip_up = 1;
        depth24_mark_scanout_band();
    }
}
static void vkb_vram_transfer_out(int x, int y, int w, int h, uint16_t *data) {
    ensure_cpu();   /* sync GPU-rendered content down to the CPU mirror first */
    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++)
            data[row * w + col] = s_vram ? s_vram[(y + row) * VRAM_W + (x + col)] : 0;
}
static void vkb_vram_write(int x, int y, uint16_t pixel) {
    sw_vram_write(x, y, pixel);
    if (!s_ctx_ok) return;
    depth24_upload_policy();
    up_add(x & (VRAM_W - 1), y & (VRAM_H - 1),
           x & (VRAM_W - 1), y & (VRAM_H - 1));
}
static uint16_t vkb_vram_read(int x, int y) {
    ensure_cpu();   /* gated by s_gpu_dirty; cheap when nothing was drawn */
    return s_vram ? s_vram[y * VRAM_W + x] : 0;
}

/* render_display: CPU readout path (screenshots / debug server / the 24-bit
 * FMV present). Sync the GPU-authoritative VRAM down, then read out via the
 * software renderer on the shared array — mirrors the GL backend exactly. */
static int vkb_render_display(uint32_t *o, int p, int dx, int dy, int dw, int dh) {
    ensure_cpu();
    return sw_render_display(o, p, dx, dy, dw, dh);
}
static int vkb_render_display_hires(uint32_t *o, int p, int dx, int dy, int dw, int dh) {
    ensure_cpu();
    return sw_render_display_hires(o, p, dx, dy, dw, dh);
}

/* ---- geometry batch (Phase 2) ------------------------------------------ */
/* Set the full-image viewport and the draw-area scissor (in framebuffer px). */
static void set_vp_scissor(VkCommandBuffer cb) {
    int S = s_scale;
    VkViewport vp = { 0.f, 0.f, (float)(VRAM_W * S), (float)(VRAM_H * S), 0.f, 1.f };
    p_vkCmdSetViewport(cb, 0, 1, &vp);
    int x1 = s_da_x1 < 0 ? 0 : s_da_x1, y1 = s_da_y1 < 0 ? 0 : s_da_y1;
    int x2 = s_da_x2 >= VRAM_W ? VRAM_W - 1 : s_da_x2;
    int y2 = s_da_y2 >= VRAM_H ? VRAM_H - 1 : s_da_y2;
    int w = x2 - x1 + 1, h = y2 - y1 + 1;
    if (w < 0) w = 0; if (h < 0) h = 0;
    VkRect2D sc = { { x1 * S, y1 * S }, { (uint32_t)(w * S), (uint32_t)(h * S) } };
    p_vkCmdSetScissor(cb, 0, 1, &sc);
}

/* ---- native-wide compositor helpers ------------------------------------- */
/* X-translation (native px) from canonical VRAM space into the active wide
 * surface: local_x = vram_x - base_x + OFFSET. Same as GL/SW wide_dx(). */
static int wide_dx(void) { return s_wide_offset - s_wide_cur_base; }

static void wide_free_all(void) {
    int any = 0;
    for (int i = 0; i < VK_WIDE_MAX_SURF; i++) any |= (s_wide_img[i] != VK_NULL_HANDLE);
    if (any) p_vkQueueWaitIdle(s_queue);   /* surfaces may be referenced in flight */
    for (int i = 0; i < VK_WIDE_MAX_SURF; i++) {
        if (s_wide_fb[i])      { p_vkDestroyFramebuffer(s_dev, s_wide_fb[i], NULL); s_wide_fb[i] = VK_NULL_HANDLE; }
        if (s_wide_view[i])    { p_vkDestroyImageView(s_dev, s_wide_view[i], NULL); s_wide_view[i] = VK_NULL_HANDLE; }
        if (s_wide_img[i])     { p_vkDestroyImage(s_dev, s_wide_img[i], NULL); s_wide_img[i] = VK_NULL_HANDLE; }
        if (s_wide_mem[i])     { p_vkFreeMemory(s_dev, s_wide_mem[i], NULL); s_wide_mem[i] = VK_NULL_HANDLE; }
        if (s_wide_ds_view[i]) { p_vkDestroyImageView(s_dev, s_wide_ds_view[i], NULL); s_wide_ds_view[i] = VK_NULL_HANDLE; }
        if (s_wide_ds_img[i])  { p_vkDestroyImage(s_dev, s_wide_ds_img[i], NULL); s_wide_ds_img[i] = VK_NULL_HANDLE; }
        if (s_wide_ds_mem[i])  { p_vkFreeMemory(s_dev, s_wide_ds_mem[i], NULL); s_wide_ds_mem[i] = VK_NULL_HANDLE; }
        s_wide_layout[i] = VK_IMAGE_LAYOUT_UNDEFINED;
        s_wide_base[i] = -1;
    }
    s_wide_cur = -1;
}

/* Find (or lazily create) the wide surface for base_x. Returns the surface
 * index or -1. A new surface = RGBA8 color image (attachment + blit src/dst)
 * + its own stencil image + a framebuffer on the SHARED render pass; color is
 * cleared black, stencil to 0, then both are parked in the layouts the render
 * pass expects (color/DS ATTACHMENT_OPTIMAL). */
static int wide_surf_for(int base_x) {
    if (s_wide_w <= 0 || !s_ready) return -1;
    for (int i = 0; i < VK_WIDE_MAX_SURF; i++)
        if (s_wide_img[i] && s_wide_base[i] == base_x) return i;
    for (int i = 0; i < VK_WIDE_MAX_SURF; i++) {
        if (s_wide_img[i]) continue;
        int S = s_scale, w = s_wide_w * S, h = VRAM_H * S;
        if (!make_image(VK_FORMAT_R8G8B8A8_UNORM, w, h,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, &s_wide_img[i], &s_wide_mem[i], &s_wide_view[i]))
            return -1;
        if (!make_image(s_ds_format, w, h,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_STENCIL_BIT, &s_wide_ds_img[i], &s_wide_ds_mem[i], &s_wide_ds_view[i]))
            return -1;
        VkImageView views[2] = { s_wide_view[i], s_wide_ds_view[i] };
        VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fi.renderPass = s_rpass; fi.attachmentCount = 2; fi.pAttachments = views;
        fi.width = (uint32_t)w; fi.height = (uint32_t)h; fi.layers = 1;
        if (p_vkCreateFramebuffer(s_dev, &fi, NULL, &s_wide_fb[i]) != VK_SUCCESS)
            return -1;
        /* Clear + park in attachment layouts (mirrors the s_ds init chain). */
        VkCommandBuffer cb = begin_oneshot();
        s_wide_layout[i] = VK_IMAGE_LAYOUT_UNDEFINED;
        img_to(cb, s_wide_img[i], &s_wide_layout[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue black = {{0, 0, 0, 0}};
        VkImageSubresourceRange crng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        p_vkCmdClearColorImage(cb, s_wide_img[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &crng);
        img_to(cb, s_wide_img[i], &s_wide_layout[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkImageMemoryBarrier db = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        db.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; db.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        db.srcQueueFamilyIndex = db.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        db.image = s_wide_ds_img[i]; db.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        db.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        db.subresourceRange.levelCount = 1; db.subresourceRange.layerCount = 1;
        p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, 0, NULL, 0, NULL, 1, &db);
        VkClearDepthStencilValue dsv = { 0.0f, 0 };
        VkImageSubresourceRange srng = { VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 };
        p_vkCmdClearDepthStencilImage(cb, s_wide_ds_img[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dsv, 1, &srng);
        db.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        db.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        db.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        db.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, NULL, 0, NULL, 1, &db);
        end_oneshot(cb);
        s_wide_base[i] = base_x;
        return i;
    }
    return -1;  /* more distinct buffers than VK_WIDE_MAX_SURF — shouldn't happen */
}

/* Begin the mirror render pass on the active wide surface inside an already-
 * recording one-shot CB (canonical pass already ended). Sets the wide viewport
 * and the mirror scissor: FULL width X (the margins are the point) but the
 * DRAW-AREA Y band — native-wide only widens X; a full-height Y let draws bleed
 * across a vertical double-buffer band boundary (the GL band-flicker lesson). */
static void wide_pass_begin(VkCommandBuffer cb) {
    int i = s_wide_cur, S = s_scale;
    s_perf_cur.wide_passes++;
    img_to(cb, s_wide_img[i], &s_wide_layout[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    /* Stencil self-barrier for the WIDE DS image (same hazard class as
     * begin_geo_pass covers for the canonical DS across one-shot submits). */
    VkImageMemoryBarrier db = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    db.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    db.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    db.srcQueueFamilyIndex = db.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    db.image = s_wide_ds_img[i];
    db.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    db.subresourceRange.levelCount = 1; db.subresourceRange.layerCount = 1;
    db.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    db.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    p_vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        0, 0, NULL, 0, NULL, 1, &db);
    color_self_barrier(cb, s_wide_img[i]);
    VkRenderPassBeginInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rp.renderPass = s_rpass; rp.framebuffer = s_wide_fb[i];
    rp.renderArea.extent.width = (uint32_t)(s_wide_w * S);
    rp.renderArea.extent.height = (uint32_t)(VRAM_H * S);
    p_vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp = { 0.f, 0.f, (float)(s_wide_w * S), (float)(VRAM_H * S), 0.f, 1.f };
    p_vkCmdSetViewport(cb, 0, 1, &vp);
    int y1 = s_da_y1 < 0 ? 0 : s_da_y1;
    int y2 = s_da_y2 >= VRAM_H ? VRAM_H - 1 : s_da_y2;
    int hh = y2 - y1 + 1; if (hh < 0) hh = 0;
    VkRect2D sc = { { 0, y1 * S }, { (uint32_t)(s_wide_w * S), (uint32_t)(hh * S) } };
    p_vkCmdSetScissor(cb, 0, 1, &sc);
}

/* Tight bbox of the open untextured batch (added to s_pack_dirty at flush). */
static DirtyRect s_geo_bbox;

/* Flush accumulated untextured geometry: one render pass draws the whole batch
 * (blend/mask per the batch keys), then the image returns to TRANSFER_SRC. */
static void flush_geometry(void) {
    if (s_vcount == 0 || !s_ready) return;
    s_perf_cur.geo_flushes++;
    uint32_t base = s_vbase, n = s_vcount;
    s_vbase += n; s_vcount = 0;   /* consumed region stays untouched until gpu_sync */
    int semi = s_geo_semi, blend = (semi < 0) ? 0 : (semi + 1);
    VkCommandBuffer cb = begin_oneshot();
    vram_to(cb, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    begin_geo_pass(cb);
    set_vp_scissor(cb);
    bind_masked(cb, 0, 0, blend, s_geo_check, s_geo_mask);
    GeoPush gp = { px_shift(), 0.0f, 512.0f };
    p_vkCmdPushConstants(cb, s_pl_geo, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof gp, &gp);
    VkDeviceSize off = 0;
    p_vkCmdBindVertexBuffers(cb, 0, 1, &s_vbuf, &off);
    p_vkCmdDraw(cb, n, 1, base, 0);
    p_vkCmdEndRenderPass(cb);
    if (s_wide_cur >= 0 && !s_geo_mirror_suppress) {   /* native-wide mirror pass (same CB) */
        wide_pass_begin(cb);
        bind_masked(cb, 0, 0, blend, s_geo_check, s_geo_mask);
        GeoPush wgp = { px_shift(), (float)wide_dx(), (float)s_wide_w / 2.0f };
        p_vkCmdPushConstants(cb, s_pl_geo, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof wgp, &wgp);
        p_vkCmdDraw(cb, n, 1, base, 0);
        p_vkCmdEndRenderPass(cb);
    }
    s_geo_mirror_suppress = 0;
    vram_to(cb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    end_oneshot(cb);
    if (s_geo_bbox.set) {
        int x0 = s_geo_bbox.x0 < s_da_x1 ? s_da_x1 : s_geo_bbox.x0;
        int y0 = s_geo_bbox.y0 < s_da_y1 ? s_da_y1 : s_geo_bbox.y0;
        int x1 = s_geo_bbox.x1 > s_da_x2 ? s_da_x2 : s_geo_bbox.x1;
        int y1 = s_geo_bbox.y1 > s_da_y2 ? s_da_y2 : s_geo_bbox.y1;
        rect_add(&s_pack_dirty, x0, y0, x1, y1);
        rect_clear(&s_geo_bbox);
    }
    s_gpu_dirty = 1;
}

static inline void col555(uint16_t c, float out[3]) {
    out[0] = ((c)       & 0x1F) / 31.0f;
    out[1] = ((c >> 5)  & 0x1F) / 31.0f;
    out[2] = ((c >> 10) & 0x1F) / 31.0f;
}
static inline void col888(uint32_t c, float out[3]) {
    out[0] = ( c        & 0xFF) / 255.0f;
    out[1] = ((c >> 8)  & 0xFF) / 255.0f;
    out[2] = ((c >> 16) & 0xFF) / 255.0f;
}
static inline void push_vert(float x, float y, const float rgb[3]) {
    Vert *v = &s_vmap[s_vbase + s_vcount++];
    v->x = x; v->y = y; v->r = rgb[0]; v->g = rgb[1]; v->b = rgb[2];
    v->a = (float)s_geo_mask;
    rect_add(&s_geo_bbox, (int)floorf(x), (int)floorf(y), (int)ceilf(x), (int)ceilf(y));
}
/* Ensure room for `need` verts (flush mid-batch only between primitives). */
static void ensure_room(uint32_t need) {
    if (s_vbase + s_vcount + need > VK_VBUF_VERTS) {
        flush_geometry();
        if (s_vbase + need > VK_VBUF_VERTS)
            gpu_sync();   /* buffer exhausted: wait idle, cursors recycle to 0 */
    }
}
/* Open / continue an untextured batch with the current blend/mask state; drains
 * the textured batch and any differently-keyed untextured batch first (PS1
 * submission order). */
static void geo_prim_begin(void) {
    flush_cpu_upload();   /* pending CPU writes precede this prim (PS1 order) */
    flush_tex_batch();
    int semi = s_semi_en ? s_semi_mode : -1;
    if (s_vcount > 0 && (semi != s_geo_semi || s_mask_set != s_geo_mask || s_mask_check != s_geo_check))
        flush_geometry();
    if (s_vcount == 0) { s_geo_semi = semi; s_geo_mask = s_mask_set; s_geo_check = s_mask_check; }
}

static void tri3(float ax,float ay,const float ca[3],
                 float bx,float by,const float cb_[3],
                 float cx,float cy,const float cc[3]) {
    if (!s_ready) return;
    ensure_room(3);
    push_vert(ax, ay, ca); push_vert(bx, by, cb_); push_vert(cx, cy, cc);
}
static void quad(float x0,float y0,float x1,float y1,float x2,float y2,float x3,float y3,
                 const float c[3]) {
    /* (x0,y0)-(x1,y1)-(x2,y2)-(x3,y3) as two tris */
    tri3(x0,y0,c, x1,y1,c, x2,y2,c);
    tri3(x0,y0,c, x2,y2,c, x3,y3,c);
}

/* Per-prim render state. Batch keys are compared at draw-append time (the draw
 * funcs flush when they change), so the setters only record state. */
static void vkb_set_semi_transparency(int e, int m) { s_semi_en = e ? 1 : 0; s_semi_mode = m & 3; }
static void vkb_set_mask_bits(int s, int c) { s_mask_set = s ? 1 : 0; s_mask_check = c ? 1 : 0; }
static void vkb_set_texture_window(uint32_t raw) {
    s_tw_mask_x = (int)(raw & 0x1F);        s_tw_mask_y = (int)((raw >> 5) & 0x1F);
    s_tw_off_x  = (int)((raw >> 10) & 0x1F); s_tw_off_y  = (int)((raw >> 15) & 0x1F);
}
static void vkb_set_color_modulation(int r, int g, int b, int raw) {
    s_mod_r = r; s_mod_g = g; s_mod_b = b; s_mod_raw = raw ? 1 : 0;
}

/* Sub-pixel / perspective overrides for the next triangle. */
static void vkb_set_precise_triangle(int enabled,
                                     int32_t x0,int32_t y0, int32_t x1,int32_t y1,
                                     int32_t x2,int32_t y2) {
    s_pc_valid = enabled ? 1 : 0;
    if (s_pc_valid) {
        /* 16.16 native VRAM px -> float; the VK pipeline is float end-to-end. */
        s_pc_x[0] = (float)x0 / 65536.0f;  s_pc_y[0] = (float)y0 / 65536.0f;
        s_pc_x[1] = (float)x1 / 65536.0f;  s_pc_y[1] = (float)y1 / 65536.0f;
        s_pc_x[2] = (float)x2 / 65536.0f;  s_pc_y[2] = (float)y2 / 65536.0f;
    }
}
static void vkb_set_perspective_triangle(int enabled, float q0, float q1, float q2) {
    s_pq_valid = (enabled && q0 > 0.0f && q1 > 0.0f && q2 > 0.0f) ? 1 : 0;
    s_pq[0] = q0; s_pq[1] = q1; s_pq[2] = q2;
    /* The corrected-triangle tally lives in the software rasterizer and backs
     * gpu_texture_correction_hits() — the "is this doing anything on this
     * title" counter. Forward so it reads the same on every backend. */
    sw_set_perspective_triangle(enabled, q0, q1, q2);
}
/* The override describes exactly one triangle; drop it once submitted so a
 * later prim can never inherit it. */
static inline void precise_consumed(void) { s_pc_valid = 0; s_pq_valid = 0; }

/* Position a vertex: the sub-pixel value when geometry_correction supplied one
 * for this triangle, else the integer GP0 position. */
#define PCX(i, v) (s_pc_valid ? s_pc_x[i] : (float)(v))
#define PCY(i, v) (s_pc_valid ? s_pc_y[i] : (float)(v))

static void vkb_draw_flat_triangle(int x0,int y0,int x1,int y1,int x2,int y2,uint16_t c){
    geo_prim_begin();
    float col[3]; col555(c, col);
    tri3(PCX(0,x0),PCY(0,y0),col, PCX(1,x1),PCY(1,y1),col, PCX(2,x2),PCY(2,y2),col);
    precise_consumed();
}
static void vkb_draw_gouraud_triangle(int x0,int y0,uint16_t c0,int x1,int y1,uint16_t c1,int x2,int y2,uint16_t c2){
    geo_prim_begin();
    float a[3],b[3],cc[3]; col555(c0,a); col555(c1,b); col555(c2,cc);
    tri3(PCX(0,x0),PCY(0,y0),a, PCX(1,x1),PCY(1,y1),b, PCX(2,x2),PCY(2,y2),cc);
    precise_consumed();
}
/* Full-screen-overlay wide pass (pause gray-filter / load fade): draw a flat
 * rect covering the FULL wide width [0, wide_w) x [y, y+h) directly into the
 * active wide surface (positions already wide-space, u_xoff = 0). Mirrors GL's
 * wide_flat_rect_direct: full-surface scissor (the overlay must dim the
 * revealed margins too). The 6 verts are consumed privately (s_vbase bumped)
 * so they never join a canonical batch. */
static void wide_overlay_rect(int y, int h, uint16_t c, int semi) {
    if (s_wide_cur < 0 || !s_ready) return;
    ensure_room(6);
    float col[3]; col555(c, col);
    uint32_t base = s_vbase + s_vcount;   /* == s_vbase (batches drained) */
    float x0 = 0.0f, x1 = (float)s_wide_w, fy0 = (float)y, fy1 = (float)(y + h);
    push_vert(x0, fy0, col); push_vert(x1, fy0, col); push_vert(x1, fy1, col);
    push_vert(x0, fy0, col); push_vert(x1, fy1, col); push_vert(x0, fy1, col);
    s_vbase += s_vcount; s_vcount = 0;    /* consume privately */
    int blend = (semi < 0) ? 0 : (semi + 1);
    int S = s_scale;
    VkCommandBuffer cb = begin_oneshot();
    wide_pass_begin(cb);
    /* Overlay covers the whole surface: override the band scissor. */
    VkRect2D sc = { { 0, 0 }, { (uint32_t)(s_wide_w * S), (uint32_t)(VRAM_H * S) } };
    p_vkCmdSetScissor(cb, 0, 1, &sc);
    bind_masked(cb, 0, 0, blend, s_mask_check, s_mask_set);
    GeoPush gp = { px_shift(), 0.0f, (float)s_wide_w / 2.0f };
    p_vkCmdPushConstants(cb, s_pl_geo, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof gp, &gp);
    VkDeviceSize off = 0;
    p_vkCmdBindVertexBuffers(cb, 0, 1, &s_vbuf, &off);
    p_vkCmdDraw(cb, 6, 1, base, 0);
    p_vkCmdEndRenderPass(cb);
    end_oneshot(cb);
}

static void vkb_draw_flat_rect(int x,int y,int w,int h,uint16_t c){
    /* Full-screen 2D overlay under native-wide: the canonical rect must NOT
     * mirror 1:1 (it would cover only the translated 4:3 span) — suppress the
     * batch mirror and emit one full-wide-width rect instead, so the revealed
     * margins are dimmed/faded too. Same detection as GL/SW. */
    int overlay = 0;
    if (s_wide_cur >= 0) {
        int native_w = s_wide_w - 2 * s_wide_offset;
        int lx = x - s_wide_cur_base, rx = x + w - s_wide_cur_base;
        overlay = (native_w > 0 && lx <= 0 && rx >= native_w);
    }
    if (overlay) { flush_tex_batch(); flush_geometry(); }
    geo_prim_begin();
    float col[3]; col555(c, col);
    if (overlay) s_geo_mirror_suppress = 1;
    quad((float)x,(float)y,(float)(x+w),(float)y,(float)(x+w),(float)(y+h),(float)x,(float)(y+h),col);
    if (overlay) {
        int semi = s_semi_en ? s_semi_mode : -1;
        flush_geometry();                  /* canonical only (mirror suppressed) */
        wide_overlay_rect(y, h, c, semi);
    }
}
/* 1px line as a thin quad along the segment. */
static void line_quad(int x0,int y0,int x1,int y1,const float ca[3],const float cb_[3]){
    float dx = (float)(x1 - x0), dy = (float)(y1 - y0);
    float len = dx*dx + dy*dy;
    float nx = 0.5f, ny = 0.5f;
    if (len > 0.0001f) { float il = 0.5f / (float)sqrt(len); nx = -dy * il; ny = dx * il; }
    ensure_room(6);
    float ax0 = x0 + nx, ay0 = y0 + ny, bx0 = x0 - nx, by0 = y0 - ny;
    float ax1 = x1 + nx, ay1 = y1 + ny, bx1 = x1 - nx, by1 = y1 - ny;
    tri3(ax0,ay0,ca, ax1,ay1,cb_, bx1,by1,cb_);
    tri3(ax0,ay0,ca, bx1,by1,cb_, bx0,by0,ca);
}
static void vkb_draw_line(int x0,int y0,int x1,int y1,uint16_t c){
    geo_prim_begin();
    float col[3]; col555(c, col); line_quad(x0,y0,x1,y1,col,col);
}
static void vkb_draw_shaded_line(int x0,int y0,uint16_t c0,int x1,int y1,uint16_t c1){
    geo_prim_begin();
    float a[3],b[3]; col555(c0,a); col555(c1,b); line_quad(x0,y0,x1,y1,a,b);
}

/* ---- textured primitives + VRAM->VRAM copy (Phase 3) ------------------- */
/* Flush the textured batch: two STP-split passes (pass 1 opaque texels, pass 2
 * semi-transparent texels with PS1 blending). Per-prim texture state rode in the
 * vertex; the batch keys (semi/mask/twin/filter) drive the push constants and
 * pipeline. The raw mirror was made coherent at append time (flush_pack_if_sampling). */
static void flush_tex_batch(void) {
    if (s_tcount == 0 || !s_ready) return;
    s_perf_cur.tex_flushes++;
    uint32_t base = s_tbase, n = s_tcount;
    s_tbase += n; s_tcount = 0;   /* consumed region stays untouched until gpu_sync */
    int semi = s_tb_semi, blend = (semi < 0) ? 0 : (semi + 1);
    VkCommandBuffer cb = begin_oneshot();
    img_to(cb, s_raw_img, &s_raw_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vram_to(cb, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    begin_geo_pass(cb);
    set_vp_scissor(cb);
    p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pl_tex, 0, 1, &s_ds_tex, 0, NULL);
    VkDeviceSize off = 0;
    p_vkCmdBindVertexBuffers(cb, 0, 1, &s_tbuf, &off);
    TexPush tp = {0};
    tp.shift = px_shift(); tp.xoff = 0.0f; tp.xhalf = 512.0f;
    tp.maskset = s_tb_mask; tp.filter = s_tb_filter;
    tp.twin[0] = s_tb_twin[0]; tp.twin[1] = s_tb_twin[1]; tp.twin[2] = s_tb_twin[2]; tp.twin[3] = s_tb_twin[3];
    if (semi < 0) {
        /* OPAQUE batch: the STP bit does not gate COLOUR (every texel is
         * opaque), so colour draws in ONE ordered pass over all texels —
         * painter's order across the batch is exact — followed by a
         * COLOUR-MASKED pass that only fixes the STENCIL for STP=1 texels.
         * (The old whole-batch STP-split let a behind prim's STP=1 texels
         * paint OVER a front prim's STP=0 colour — the water-sparkle field
         * speckled every hill in the Tomba2 attract demo. Mirrors GL's
         * tex_batch_draw_passes.) */
        bind_masked(cb, 1, 0, 0, s_tb_check, s_tb_mask);
        tp.semipass = 0;
        p_vkCmdPushConstants(cb, s_pl_tex, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof tp, &tp);
        p_vkCmdDraw(cb, n, 1, base, 0);
        bind_masked_stencil_only(cb, 1, 0, s_tb_check, 1);
        tp.semipass = 2;
        p_vkCmdPushConstants(cb, s_pl_tex, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof tp, &tp);
        p_vkCmdDraw(cb, n, 1, base, 0);
    } else {
        /* SEMI batch: genuine two-pass — STP=0 texels opaque, STP=1 texels
         * blended per the PSX mode. Cross-prim order is kept by isolating
         * semi prims to one per batch (gpu_textured_triangle), so the two
         * passes never straddle overlapping prims. */
        bind_masked(cb, 1, 0, 0, s_tb_check, s_tb_mask);
        tp.semipass = 1;
        p_vkCmdPushConstants(cb, s_pl_tex, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof tp, &tp);
        p_vkCmdDraw(cb, n, 1, base, 0);
        bind_masked(cb, 1, 0, blend, s_tb_check, 1);
        tp.semipass = 2;
        p_vkCmdPushConstants(cb, s_pl_tex, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof tp, &tp);
        p_vkCmdDraw(cb, n, 1, base, 0);
    }
    p_vkCmdEndRenderPass(cb);
    if (s_wide_cur >= 0) {                 /* native-wide mirror pass (same CB) */
        wide_pass_begin(cb);
        tp.xoff = (float)wide_dx(); tp.xhalf = (float)s_wide_w / 2.0f;
        if (semi < 0) {
            bind_masked(cb, 1, 0, 0, s_tb_check, s_tb_mask);
            tp.semipass = 0;
            p_vkCmdPushConstants(cb, s_pl_tex, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof tp, &tp);
            p_vkCmdDraw(cb, n, 1, base, 0);
            bind_masked_stencil_only(cb, 1, 0, s_tb_check, 1);
            tp.semipass = 2;
            p_vkCmdPushConstants(cb, s_pl_tex, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof tp, &tp);
            p_vkCmdDraw(cb, n, 1, base, 0);
        } else {
            bind_masked(cb, 1, 0, 0, s_tb_check, s_tb_mask);
            tp.semipass = 1;
            p_vkCmdPushConstants(cb, s_pl_tex, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof tp, &tp);
            p_vkCmdDraw(cb, n, 1, base, 0);
            bind_masked(cb, 1, 0, blend, s_tb_check, 1);
            tp.semipass = 2;
            p_vkCmdPushConstants(cb, s_pl_tex, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof tp, &tp);
            p_vkCmdDraw(cb, n, 1, base, 0);
        }
        p_vkCmdEndRenderPass(cb);
    }
    vram_to(cb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    end_oneshot(cb);
    s_gpu_dirty = 1;
}

/* Shared PS1 uv-sampling model (limits + mirrored-2D compensation) — one
 * implementation for GL/VK/SW, see gpu_uv.h. */
#include "gpu_uv.h"

/* Append a textured triangle to the batch (per-prim state in the vertex; flush
 * first if the batch keys differ or the raw mirror needs packing). col = 3x RGB
 * modulation per vertex; lim = uv bounds (NULL -> derive). */
static void gpu_textured_triangle(const int *xs, const int *ys, const int *us, const int *vs,
                                  const float *col, uint16_t texpage,
                                  uint16_t clut_x, uint16_t clut_y, int rawtex,
                                  int semi, const int *lim) {
    if (!s_ready) return;
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
    int base_x = (texpage & 0xF) * 64;
    int base_y = ((texpage >> 4) & 1) * 256;
    int depth  = (texpage >> 7) & 3; if (depth > 2) depth = 2;

    flush_geometry();   /* drain untextured (PS1 order) */
    flush_cpu_upload(); /* uploaded texels/CLUTs must be sampleable + order */
    flush_pack_if_sampling(base_x, base_y, depth, clut_x, clut_y);

    int twx = s_tw_mask_x, twy = s_tw_mask_y, tox = s_tw_off_x, toy = s_tw_off_y;
    /* A semi-transparent prim must NOT coalesce with its neighbours (its two
     * STP passes would straddle overlapping prims — painter's-order violation;
     * see flush_tex_batch). It drains the open batch and draws alone; the next
     * append flushes it out again. Opaque prims keep batching. */
    int isolate = (semi >= 0);
    if (s_tcount > 0 &&
        (isolate || semi != s_tb_semi || s_mask_set != s_tb_mask || s_mask_check != s_tb_check ||
         s_texfilter != s_tb_filter ||
         twx != s_tb_twin[0] || twy != s_tb_twin[1] || tox != s_tb_twin[2] || toy != s_tb_twin[3]))
        flush_tex_batch();
    if (s_tbase + s_tcount + 3 > VK_TBUF_VERTS) {
        flush_tex_batch();
        if (s_tbase + 3 > VK_TBUF_VERTS)
            gpu_sync();   /* buffer exhausted: wait idle, cursors recycle to 0 */
    }
    if (s_tcount == 0) {
        s_tb_semi = semi; s_tb_mask = s_mask_set; s_tb_check = s_mask_check; s_tb_filter = s_texfilter;
        s_tb_twin[0] = twx; s_tb_twin[1] = twy; s_tb_twin[2] = tox; s_tb_twin[3] = toy;
    }
    int bx0 = xs[0], bx1 = xs[0], by0 = ys[0], by1 = ys[0];
    for (int i = 0; i < 3; i++) {
        float *vp = s_tmap[s_tbase + s_tcount + i].v;
        vp[0] = s_pc_valid ? s_pc_x[i] : (float)xs[i];
        vp[1] = s_pc_valid ? s_pc_y[i] : (float)ys[i];
        vp[2] = (float)us[i];   vp[3] = (float)vs[i];
        vp[4] = col[i*3+0];     vp[5] = col[i*3+1];     vp[6] = col[i*3+2]; vp[7] = 1.0f;
        vp[8]  = (float)base_x;  vp[9]  = (float)base_y;
        vp[10] = (float)clut_x;  vp[11] = (float)clut_y;
        vp[12] = (float)depth;   vp[13] = (float)rawtex;
        vp[14] = (float)lim[0];  vp[15] = (float)lim[1];
        vp[16] = (float)lim[2];  vp[17] = (float)lim[3];
        vp[18] = s_pq_valid ? s_pq[i] : 0.0f;   /* a_q; 0 = affine */
        if (xs[i] < bx0) bx0 = xs[i]; if (xs[i] > bx1) bx1 = xs[i];
        if (ys[i] < by0) by0 = ys[i]; if (ys[i] > by1) by1 = ys[i];
    }
    s_tcount += 3;
    /* A sub-pixel-corrected vertex lies in [int, int+1) of the integer position
     * it was rounded from, so widen the readback rect by one pixel. */
    if (s_pc_valid) { bx1 += 1; by1 += 1; }
    /* mark dirty for later readback/pack (clamped to draw area) */
    if (bx0 < s_da_x1) bx0 = s_da_x1; if (by0 < s_da_y1) by0 = s_da_y1;
    if (bx1 > s_da_x2) bx1 = s_da_x2; if (by1 > s_da_y2) by1 = s_da_y2;
    rect_add(&s_pack_dirty, bx0, by0, bx1, by1);
    s_gpu_dirty = 1;
}

static void gpu_textured_rect(int x,int y,int w,int h, int u0,int v0,int u1,int v1,
                              uint16_t clut_x,uint16_t clut_y,uint16_t tp,int semi) {
    if (w <= 0 || h <= 0) return;
    float mr = s_mod_r/255.0f, mg = s_mod_g/255.0f, mb = s_mod_b/255.0f;
    float col[9] = { mr,mg,mb, mr,mg,mb, mr,mg,mb };
    /* Mirrored quads arrive here as scaled rects with u0>u1 / v0>v1 (see
     * the GL backend): exact bounds from the original corners, then the
     * mirror bump (gpu_uv.h). */
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

static void vkb_draw_textured_triangle(int x0,int y0,int u0,int v0,int x1,int y1,int u1,int v1,
                                       int x2,int y2,int u2,int v2,uint16_t cx,uint16_t cy,uint16_t tp){
    int xs[3]={x0,x1,x2}, ys[3]={y0,y1,y2}, us[3]={u0,u1,u2}, vs[3]={v0,v1,v2};
    float mr=s_mod_r/255.0f, mg=s_mod_g/255.0f, mb=s_mod_b/255.0f;
    float col[9]={mr,mg,mb, mr,mg,mb, mr,mg,mb};
    gpu_textured_triangle(xs,ys,us,vs,col,tp,cx,cy,s_mod_raw, s_semi_en?s_semi_mode:-1, NULL);
    precise_consumed();
}
static void vkb_draw_shaded_textured_triangle(int x0,int y0,int u0,int v0,uint32_t c0,
                                              int x1,int y1,int u1,int v1,uint32_t c1,
                                              int x2,int y2,int u2,int v2,uint32_t c2,
                                              uint16_t cx,uint16_t cy,uint16_t tp,int raw){
    int xs[3]={x0,x1,x2}, ys[3]={y0,y1,y2}, us[3]={u0,u1,u2}, vs[3]={v0,v1,v2};
    /* shaded-textured: per-vertex 24-bit colour modulates the texel (PS1 *2). */
    float col[9];
    col888(c0, &col[0]); col888(c1, &col[3]); col888(c2, &col[6]);
    gpu_textured_triangle(xs,ys,us,vs,col,tp,cx,cy,raw, s_semi_en?s_semi_mode:-1, NULL);
    precise_consumed();
}
static void vkb_draw_textured_rect(int x,int y,int w,int h,int u,int v,uint16_t cx,uint16_t cy,uint16_t tp){
    gpu_textured_rect(x,y,w,h, u,v, u+w,v+h, cx,cy,tp, s_semi_en?s_semi_mode:-1);
}
static void vkb_draw_textured_rect_scaled(int x,int y,int w,int h,int u0,int v0,int u1,int v1,uint16_t cx,uint16_t cy,uint16_t tp){
    gpu_textured_rect(x,y,w,h, u0,v0, u1,v1, cx,cy,tp, s_semi_en?s_semi_mode:-1);
}

/* VRAM->VRAM copy: blit the source hr region to scratch (resolves overlap),
 * then blit scratch -> dest with STP/mask split. Mirrors the GL backend. */
static void vkb_copy_rect(int sx,int sy,int dx,int dy,int w,int h){
    if (!s_ctx_ok || w <= 0 || h <= 0) return;
    flush_cpu_upload();   /* the copy source may include pending CPU writes */
    flush_tex_batch(); flush_geometry();
    if (sx + w > VRAM_W) w = VRAM_W - sx;
    if (dx + w > VRAM_W) w = VRAM_W - dx;
    if (sy + h > VRAM_H) h = VRAM_H - sy;
    if (dy + h > VRAM_H) h = VRAM_H - dy;
    if (w <= 0 || h <= 0) return;
    s_perf_cur.copy_rects++;
    int S = s_scale;

    /* stage 1: copy source hr region into scratch at the same coords */
    VkCommandBuffer cb = begin_oneshot();
    vram_to(cb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    img_to(cb, s_scratch_img, &s_scratch_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy ic = {0};
    ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ic.srcSubresource.layerCount = 1;
    ic.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ic.dstSubresource.layerCount = 1;
    ic.srcOffset.x = sx * S; ic.srcOffset.y = sy * S;
    ic.dstOffset.x = sx * S; ic.dstOffset.y = sy * S;
    ic.extent.width = w * S; ic.extent.height = h * S; ic.extent.depth = 1;
    p_vkCmdCopyImage(cb, s_vram_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     s_scratch_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    end_oneshot(cb);

    /* stage 2: blit scratch -> dest (hr-res source: src_div=1, offset (sx-dx)*S). */
    blit_region(dx, dy, w, h, s_scratch_view, s_scratch_img, &s_scratch_layout,
                1, (sx - dx) * S, (sy - dy) * S, 0, 1);

    if (s_vram) {   /* keep CPU mirror coherent for non-GPU readers */
        for (int row = 0; row < h; row++)
            for (int col = 0; col < w; col++)
                s_vram[(dy + row) * VRAM_W + (dx + col)] = s_vram[(sy + row) * VRAM_W + (sx + col)];
    }
}

/* ---- native-wide vtable entry points ------------------------------------ */
/* Enable native-wide with a wide width + centering offset (native px), or
 * disable (wide_w <= 0). No-ops when nothing changed (gpu.c calls this on
 * every draw-area set via ws_nw_sync_target — Part-A lesson: the wide entry
 * points sit inside guest emulation, keep the common path free). */
static void vkb_wide_configure(int wide_w, int offset) {
    if (!s_ready) return;
    if (wide_w == s_wide_w && offset == s_wide_offset) return;
    flush_tex_batch(); flush_geometry();
    if (wide_w <= 0) { wide_free_all(); s_wide_w = 0; s_wide_offset = 0; return; }
    if (wide_w != s_wide_w) wide_free_all();
    s_wide_w = wide_w;
    s_wide_offset = offset;
}

/* Select the wide surface to mirror into for the back buffer at base_x. */
static void vkb_wide_set_target(int base_x) {
    if (!s_ready) { s_wide_cur = -1; return; }
    if (s_wide_cur >= 0 && s_wide_base[s_wide_cur] == base_x) { s_wide_cur_base = base_x; return; }
    flush_tex_batch(); flush_geometry();   /* drain into the OLD target first */
    s_wide_cur = wide_surf_for(base_x);
    s_wide_cur_base = base_x;
}

/* Stop mirroring (offscreen draws that don't target a framebuffer). */
static void vkb_wide_disable_target(void) {
    if (s_wide_cur < 0) return;
    flush_tex_batch(); flush_geometry();
    s_wide_cur = -1;
}

/* Mirror a framebuffer clear: fill the full wide width over [y, y+h) of the
 * surface for base_x (revealed margins stay clean). Stencil clears to bit15,
 * like the canonical fill path. */
static void vkb_wide_clear(int base_x, int y, int h, uint16_t color) {
    if (!s_ready || s_wide_w <= 0) return;
    flush_tex_batch(); flush_geometry();
    int i = wide_surf_for(base_x);
    if (i < 0) return;
    s_perf_cur.wide_clears++;
    int S = s_scale, H = VRAM_H * S;
    int y0 = y * S, y1 = (y + h) * S;
    if (y0 < 0) y0 = 0;
    if (y1 > H) y1 = H;
    if (y1 <= y0) return;
    VkCommandBuffer cb = begin_oneshot();
    img_to(cb, s_wide_img[i], &s_wide_layout[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    color_self_barrier(cb, s_wide_img[i]);
    VkRenderPassBeginInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rp.renderPass = s_rpass; rp.framebuffer = s_wide_fb[i];
    rp.renderArea.extent.width = (uint32_t)(s_wide_w * S);
    rp.renderArea.extent.height = (uint32_t)H;
    p_vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkClearAttachment ca[2] = {0};
    ca[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ca[0].colorAttachment = 0;
    ca[0].clearValue.color.float32[0] = (float)((color)       & 0x1F) / 31.0f;
    ca[0].clearValue.color.float32[1] = (float)((color >> 5)  & 0x1F) / 31.0f;
    ca[0].clearValue.color.float32[2] = (float)((color >> 10) & 0x1F) / 31.0f;
    ca[0].clearValue.color.float32[3] = (color >> 15) & 1 ? 1.0f : 0.0f;
    ca[1].aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    ca[1].clearValue.depthStencil.depth = 0.0f;
    ca[1].clearValue.depthStencil.stencil = (color >> 15) & 1;
    VkClearRect cr = { { { 0, y0 }, { (uint32_t)(s_wide_w * S), (uint32_t)(y1 - y0) } }, 0, 1 };
    p_vkCmdClearAttachments(cb, 2, ca, 1, &cr);
    p_vkCmdEndRenderPass(cb);
    end_oneshot(cb);
}

/* Clear only the two synthetic reveal strips, leaving the guest-owned centre
 * intact. Mirrors the software/OpenGL opt-in transition cleanup. */
static void vkb_wide_clear_margins(int base_x, int y, int h, uint16_t color, int sides) {
    if (!s_ready || s_wide_w <= 0 || s_wide_offset <= 0) return;
    flush_tex_batch(); flush_geometry();
    int i = wide_surf_for(base_x);
    if (i < 0) return;
    s_perf_cur.wide_clears++;
    int S = s_scale, H = VRAM_H * S;
    int W = s_wide_w * S, margin = s_wide_offset * S;
    int y0 = y * S, y1 = (y + h) * S;
    if (y0 < 0) y0 = 0;
    if (y1 > H) y1 = H;
    if (y1 <= y0 || margin * 2 >= W) return;
    VkCommandBuffer cb = begin_oneshot();
    img_to(cb, s_wide_img[i], &s_wide_layout[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    color_self_barrier(cb, s_wide_img[i]);
    VkRenderPassBeginInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rp.renderPass = s_rpass; rp.framebuffer = s_wide_fb[i];
    rp.renderArea.extent.width = (uint32_t)W;
    rp.renderArea.extent.height = (uint32_t)H;
    p_vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkClearAttachment ca[2] = {0};
    ca[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ca[0].colorAttachment = 0;
    ca[0].clearValue.color.float32[0] = (float)((color)       & 0x1F) / 31.0f;
    ca[0].clearValue.color.float32[1] = (float)((color >> 5)  & 0x1F) / 31.0f;
    ca[0].clearValue.color.float32[2] = (float)((color >> 10) & 0x1F) / 31.0f;
    ca[0].clearValue.color.float32[3] = (color >> 15) & 1 ? 1.0f : 0.0f;
    ca[1].aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    ca[1].clearValue.depthStencil.depth = 0.0f;
    ca[1].clearValue.depthStencil.stencil = (color >> 15) & 1;
    VkClearRect cr[2];
    uint32_t ncr = 0;
    if (sides & 1)
        cr[ncr++] = (VkClearRect){ { { 0, y0 }, { (uint32_t)margin, (uint32_t)(y1 - y0) } }, 0, 1 };
    if (sides & 2)
        cr[ncr++] = (VkClearRect){ { { W - margin, y0 }, { (uint32_t)margin, (uint32_t)(y1 - y0) } }, 0, 1 };
    if (ncr) p_vkCmdClearAttachments(cb, 2, ca, ncr, cr);
    p_vkCmdEndRenderPass(cb);
    end_oneshot(cb);
}

/* Present source (CPU): read the wide surface band for the displayed buffer
 * into an ARGB8888 buffer, byte-compatible with sw_render_wide_display — used
 * by the shared CPU present fallback and the ws debug dump. The GPU-direct
 * vk_renderer_present_wide is the per-frame path; this readback is cold. */
static int vkb_render_wide_display(uint32_t *out, int pitch, int base_x,
                                   int disp_y, int disp_h) {
    if (!s_ready || s_wide_w <= 0) return 0;
    int i = -1;
    for (int k = 0; k < VK_WIDE_MAX_SURF; k++)
        if (s_wide_img[k] && s_wide_base[k] == base_x) { i = k; break; }
    if (i < 0) return 0;
    flush_cpu_upload(); flush_tex_batch(); flush_geometry();
    int S = s_scale, W = s_wide_w * S, H = VRAM_H * S;
    int out_h = disp_h * S, ry0 = disp_y * S;
    if (ry0 < 0) ry0 = 0;
    if (ry0 + out_h > H) out_h = H - ry0;
    if (out_h <= 0) return 0;
    VkBuffer buf; VkDeviceMemory mem; void *map;
    if (!make_staging((VkDeviceSize)W * out_h * 4, &buf, &mem, &map)) return 0;
    VkCommandBuffer cb = begin_oneshot();
    img_to(cb, s_wide_img[i], &s_wide_layout[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy rc = {0};
    rc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rc.imageSubresource.layerCount = 1;
    rc.imageOffset.y = ry0;
    rc.imageExtent.width = (uint32_t)W; rc.imageExtent.height = (uint32_t)out_h;
    rc.imageExtent.depth = 1;
    p_vkCmdCopyImageToBuffer(cb, s_wide_img[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &rc);
    img_to(cb, s_wide_img[i], &s_wide_layout[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    end_oneshot(cb);
    gpu_sync();   /* readback must be complete before the CPU reads it */
    const uint8_t *src = (const uint8_t *)map;
    int count = 0;
    for (int row = 0; row < out_h; row++) {
        uint32_t *dst = (uint32_t *)((uint8_t *)out + (size_t)row * pitch);
        const uint8_t *s8 = src + (size_t)row * W * 4;
        for (int col = 0; col < W; col++) {   /* RGBA8 -> 0xAARRGGBB, alpha forced */
            dst[col] = 0xFF000000u | ((uint32_t)s8[col * 4 + 0] << 16)
                     | ((uint32_t)s8[col * 4 + 1] << 8) | s8[col * 4 + 2];
            count++;
        }
    }
    free_staging(buf, mem);
    return count;
}

static const GpuRenderBackend VK_BACKEND = {
    .name                          = "vulkan",
    .init                          = vkb_init,
    .set_scale                     = vkb_set_scale,
    .scale                         = vkb_scale,
    .set_texture_filter            = vkb_set_texture_filter,
    .texture_filter                = vkb_texture_filter,
    .set_semi_transparency         = vkb_set_semi_transparency,
    .set_mask_bits                 = vkb_set_mask_bits,
    .set_texture_window            = vkb_set_texture_window,
    .set_color_modulation          = vkb_set_color_modulation,
    .set_precise_triangle          = vkb_set_precise_triangle,
    .set_perspective_triangle      = vkb_set_perspective_triangle,
    .fill_rect                     = vkb_fill_rect,
    .copy_rect                     = vkb_copy_rect,
    .draw_flat_triangle            = vkb_draw_flat_triangle,
    .draw_gouraud_triangle         = vkb_draw_gouraud_triangle,
    .draw_textured_triangle        = vkb_draw_textured_triangle,
    .draw_shaded_textured_triangle = vkb_draw_shaded_textured_triangle,
    .draw_flat_rect                = vkb_draw_flat_rect,
    .draw_textured_rect            = vkb_draw_textured_rect,
    .draw_textured_rect_scaled     = vkb_draw_textured_rect_scaled,
    .draw_line                     = vkb_draw_line,
    .draw_shaded_line              = vkb_draw_shaded_line,
    .render_display                = vkb_render_display,
    .render_display_hires          = vkb_render_display_hires,
    .vram_write                    = vkb_vram_write,
    .vram_read                     = vkb_vram_read,
    .vram_transfer_in              = vkb_vram_transfer_in,
    .vram_transfer_out             = vkb_vram_transfer_out,
    .set_draw_area                 = vkb_set_draw_area,
    .get_draw_area                 = vkb_get_draw_area,
    .set_draw_offset               = vkb_set_draw_offset,
    .wide_configure                = vkb_wide_configure,
    .wide_set_target               = vkb_wide_set_target,
    .wide_disable_target           = vkb_wide_disable_target,
    .wide_clear                    = vkb_wide_clear,
    .wide_clear_margins            = vkb_wide_clear_margins,
    .render_wide_display           = vkb_render_wide_display,
};

/* Returned unconditionally (like gl_backend_get): the table is selected before
 * the context exists; vk_renderer_init_context creates the device, and on
 * failure main.cpp switches the facade back to software. */
const GpuRenderBackend *vk_backend_get(void) { return &VK_BACKEND; }

#endif /* PSX_HAVE_VULKAN */
