#include "gpu_ng_abi.h"
#include "psx_ng_shared.h"

#include <NoGraphicsAPI/NoGraphicsAPI.hpp>

#include "psx_ng_present_frag_spv.h"
#include "psx_ng_present_vert_spv.h"
#include "psx_ng_raster_spv.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr int vram_w = 1024;
constexpr int vram_h = 512;
constexpr uint64_t raw_bytes = uint64_t{vram_w} * vram_h * sizeof(uint16_t);
constexpr uint64_t rgba_bytes = uint64_t{vram_w} * vram_h * sizeof(uint32_t);
constexpr uint32_t sampler_nearest = 0;
constexpr uint32_t sampler_linear = 1;

struct NgState {
    gpu::Device *device = nullptr;
    gpu::CommandBuffer *pending = nullptr;
    uint32_t pending_dispatches = 0;
    gpu::TimelineSemaphore *timeline = nullptr;
    uint64_t timeline_value = 0;
    gpu::GpuHeap upload{};
    gpu::GpuHeap readback{};
    gpu::GpuHeap texture_desc{};
    gpu::GpuHeap sampler_desc{};
    gpu::TextureHeap texture_heap{};
    gpu::Texture *raw = nullptr;
    gpu::Texture *color = nullptr;
    gpu::Texture *scratch = nullptr;
    gpu::Texture *cpu_present = nullptr;
    gpu::PSO *raster_pso = nullptr;
    gpu::PSO *present_pso = nullptr;
    uint16_t *vram = nullptr;
    int width = 0;
    int height = 0;
    int draw_x1 = 0;
    int draw_y1 = 0;
    int draw_x2 = vram_w - 1;
    int draw_y2 = vram_h - 1;
    int off_x = 0;
    int off_y = 0;
    int semi_enabled = 0;
    int semi_mode = 0;
    int mask_set = 0;
    int mask_check = 0;
    int tw_mask_x = 0;
    int tw_mask_y = 0;
    int tw_off_x = 0;
    int tw_off_y = 0;
    int mod_r = 16;
    int mod_g = 16;
    int mod_b = 16;
    int raw_texture = 0;
    int texture_filter = 0;
    int requested_scale = 1;
    int cpu_dirty = 0;
    int ready = 0;
    char last_error[256] = {};
};

NgState s;

uint64_t align_up(uint64_t value, uint64_t align)
{
    return align ? ((value + align - 1) / align) * align : value;
}

gpu::Span<const uint32_t> spv_span(const uint32_t *words, uint32_t count)
{
    return gpu::Span<const uint32_t>{words, count};
}

void set_error(const char *text)
{
    std::snprintf(s.last_error, sizeof(s.last_error), "%s", text ? text : "");
    if (text && *text) std::fprintf(stderr, "psxrecomp: %s\n", text);
}

const char *last_error()
{
    return s.last_error;
}

void flush_commands();

void destroy_all()
{
    flush_commands();
    if (s.device)
        gpu::wait_idle(s.device);
    gpu::destroy_pso(s.present_pso);
    gpu::destroy_pso(s.raster_pso);
    gpu::destroy_texture(s.cpu_present);
    gpu::destroy_texture(s.scratch);
    gpu::destroy_texture(s.color);
    gpu::destroy_texture(s.raw);
    if (s.texture_heap.owner) gpu::destroy_texture_heap(s.texture_heap);
    if (s.sampler_desc.owner) gpu::destroy_gpu_heap(s.sampler_desc);
    if (s.texture_desc.owner) gpu::destroy_gpu_heap(s.texture_desc);
    if (s.readback.owner) gpu::destroy_gpu_heap(s.readback);
    if (s.upload.owner) gpu::destroy_gpu_heap(s.upload);
    gpu::destroy_timeline_semaphore(s.timeline);
    gpu::destroy_device(s.device);
    uint16_t *vram = s.vram;
    int scale = s.requested_scale;
    std::memset(&s, 0, sizeof(s));
    s.vram = vram;
    s.requested_scale = scale ? scale : 1;
    s.draw_x2 = vram_w - 1;
    s.draw_y2 = vram_h - 1;
    s.mod_r = s.mod_g = s.mod_b = 16;
}

gpu::Texture *create_texture_at(const gpu::TextureDesc &desc, uint64_t *offset)
{
    const gpu::SizeAlign sa = gpu::get_texture_size_align(s.device, desc);
    if (!sa.size || !sa.align || !s.texture_heap.owner) return nullptr;
    *offset = align_up(*offset, sa.align);
    gpu::Texture *texture = gpu::create_texture(s.device, desc, s.texture_heap, *offset);
    *offset += sa.size;
    return texture;
}

int submit_wait(gpu::CommandBuffer *commands)
{
    if (!s.ready || !commands)
        return 0;
    const gpu::TimelinePoint done{.semaphore = s.timeline, .value = ++s.timeline_value};
    gpu::CommandBuffer *list[] = {commands};
    gpu::submit(gpu::Span<gpu::CommandBuffer *const>{list, 1}, done);
    gpu::wait_timeline(done);
    return 1;
}

void flush_commands()
{
    if (!s.pending) return;
    gpu::CommandBuffer *commands = s.pending;
    s.pending = nullptr;
    s.pending_dispatches = 0;
    submit_wait(commands);
}

void dispatch_root(const PsxNgRasterRoot &root)
{
    if (!s.ready)
        return;
    if (!s.pending) s.pending = gpu::begin_commands(s.device);
    gpu::CommandBuffer *commands = s.pending;
    gpu::set_texture_descriptor_heap(commands, gpu::gpu_range(s.texture_desc));
    gpu::barrier(commands, gpu::Stage::transfer | gpu::Stage::compute, gpu::Access::transfer_write | gpu::Access::shader_write,
                 gpu::Stage::compute, gpu::Access::shader_read | gpu::Access::shader_write | gpu::Access::descriptor_read);
    gpu::bind_pso(commands, s.raster_pso);
    gpu::dispatch(commands, gpu::ByteSpan(root), gpu::uint32x3{.x = uint32_t((root.w + 7) / 8), .y = uint32_t((root.h + 7) / 8), .z = 1});
    gpu::barrier(commands, gpu::Stage::compute, gpu::Access::shader_write,
                 gpu::Stage::transfer | gpu::Stage::fragment, gpu::Access::transfer_read | gpu::Access::shader_read);
    /* Keep primitive ordering/barriers in one command buffer. CPU uploads,
     * readback and presentation flush before reusing mapped memory. Bound the
     * batch so a game that never reads VRAM cannot grow it indefinitely. */
    if (++s.pending_dispatches >= 512) flush_commands();
    if (root.op != PSX_NG_PRIM_SCRATCH_COPY_IN)
        s.cpu_dirty = 1;
}

PsxNgRasterRoot base_root(uint32_t op, int x, int y, int w, int h)
{
    PsxNgRasterRoot r{};
    r.op = op;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    r.clip_x1 = s.draw_x1;
    r.clip_y1 = s.draw_y1;
    r.clip_x2 = s.draw_x2;
    r.clip_y2 = s.draw_y2;
    r.mask_set = s.mask_set;
    r.mask_check = s.mask_check;
    r.semi_enabled = s.semi_enabled;
    r.semi_mode = s.semi_mode & 3;
    r.tw_mask_x = s.tw_mask_x;
    r.tw_mask_y = s.tw_mask_y;
    r.tw_off_x = s.tw_off_x;
    r.tw_off_y = s.tw_off_y;
    r.mod_r = s.mod_r;
    r.mod_g = s.mod_g;
    r.mod_b = s.mod_b;
    return r;
}

void snapshot_raw()
{
    int old_dirty = s.cpu_dirty;
    PsxNgRasterRoot r = base_root(PSX_NG_PRIM_SCRATCH_COPY_IN, 0, 0, vram_w, vram_h);
    r.sx = 0;
    r.sy = 0;
    r.dx = 0;
    r.dy = 0;
    dispatch_root(r);
    s.cpu_dirty = old_dirty;
}

void colorize_region(int x, int y, int w, int h)
{
    dispatch_root(base_root(PSX_NG_PRIM_COLORIZE, x, y, w, h));
}

void upload_vram_region(int x, int y, int w, int h, const uint16_t *data)
{
    if (!s.ready || !data || w <= 0 || h <= 0)
        return;
    if (w > vram_w) w = vram_w;
    if (h > vram_h) h = vram_h;
    if ((x & 1023) + w > vram_w || (y & 511) + h > vram_h) {
        /* At most four contiguous rectangles, not one submission per pixel. */
        for (int row = 0; row < h;) {
            const int dst_y = (y + row) & 511;
            const int rows = std::min(h - row, vram_h - dst_y);
            for (int col = 0; col < w;) {
                const int dst_x = (x + col) & 1023;
                const int cols = std::min(w - col, vram_w - dst_x);
                std::vector<uint16_t> part(size_t(rows) * cols);
                for (int r = 0; r < rows; ++r)
                    std::memcpy(part.data() + size_t(r) * cols,
                                data + size_t(row + r) * w + col, size_t(cols) * sizeof(uint16_t));
                upload_vram_region(dst_x, dst_y, cols, rows, part.data());
                col += cols;
            }
            row += rows;
        }
        return;
    }
    flush_commands();
    uint16_t *dst = reinterpret_cast<uint16_t *>(s.upload.range.cpu);
    for (int row = 0; row < h; ++row)
        std::memcpy(dst + size_t(row) * w, data + size_t(row) * w, size_t(w) * sizeof(uint16_t));
    gpu::CommandBuffer *commands = gpu::begin_commands(s.device);
    const gpu::TextureCopyDesc copy{.offset = {.x = uint32_t(x & 1023), .y = uint32_t(y & 511), .z = 0},
                                    .extent = {.x = uint32_t(w), .y = uint32_t(h), .z = 1},
                                    .row_pitch_bytes = uint64_t(w) * sizeof(uint16_t),
                                    .slice_pitch_bytes = uint64_t(w) * h * sizeof(uint16_t)};
    gpu::copy_memory_to_texture(commands, gpu::GpuRange{.gpu = s.upload.range.gpu, .size = uint64_t(w) * h * sizeof(uint16_t)}, s.raw, copy);
    gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::compute, gpu::Access::shader_read | gpu::Access::shader_write);
    submit_wait(commands);
    colorize_region(x & 1023, y & 511, w, h);
}

void sync_cpu()
{
    if (!s.ready || !s.vram || !s.cpu_dirty)
        return;
    flush_commands();
    gpu::CommandBuffer *commands = gpu::begin_commands(s.device);
    const gpu::TextureCopyDesc copy{.extent = {.x = vram_w, .y = vram_h, .z = 1},
                                    .row_pitch_bytes = vram_w * sizeof(uint16_t),
                                    .slice_pitch_bytes = raw_bytes};
    gpu::copy_texture_to_memory(commands, s.raw, gpu::GpuRange{.gpu = s.readback.range.gpu, .size = raw_bytes}, copy);
    gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::host, gpu::Access::host_read);
    submit_wait(commands);
    std::memcpy(s.vram, s.readback.range.cpu, raw_bytes);
    s.cpu_dirty = 0;
}

void init_backend(uint16_t *vram)
{
    s.vram = vram;
    if (s.ready && vram)
        upload_vram_region(0, 0, vram_w, vram_h, vram);
}

int init_context(void *hwnd, int width, int height)
{
    destroy_all();
    s.width = width;
    s.height = height;
    const gpu::DeviceInit init = gpu::create_device({.window = hwnd, .swapchain_format = gpu::Format::bgra8_unorm});
    s.device = init.device;
    if (init.error != gpu::Error::none || !s.device) {
        set_error(init.error == gpu::Error::unsupported ? "NoGraphicsAPI device unsupported" : "NoGraphicsAPI device init failed");
        destroy_all();
        return 0;
    }
    const gpu::DeviceCaps &caps = gpu::get_device_caps(s.device);
    s.upload = gpu::create_gpu_heap(s.device, rgba_bytes, gpu::MemoryType::cpu_visible);
    s.readback = gpu::create_gpu_heap(s.device, raw_bytes, gpu::MemoryType::readback);
    s.texture_desc = gpu::create_gpu_heap(s.device, caps.texture_descriptor_size * PSX_NG_DESC_COUNT, gpu::MemoryType::texture_descriptor_heap);
    s.sampler_desc = gpu::create_gpu_heap(s.device, caps.sampler_descriptor_size * 2, gpu::MemoryType::sampler_descriptor_heap);
    s.timeline = gpu::create_timeline_semaphore(s.device);

    const gpu::TextureDesc raw_desc{.extent = {.x = vram_w, .y = vram_h, .z = 1}, .format = gpu::Format::r16_uint,
                                    .usage = gpu::TextureUsage::sampled | gpu::TextureUsage::storage | gpu::TextureUsage::transfer_source | gpu::TextureUsage::transfer_destination};
    const gpu::TextureDesc color_desc{.extent = {.x = vram_w, .y = vram_h, .z = 1}, .format = gpu::Format::rgba8_unorm,
                                      .usage = gpu::TextureUsage::sampled | gpu::TextureUsage::storage | gpu::TextureUsage::transfer_destination};
    const gpu::TextureDesc cpu_desc{.extent = {.x = vram_w, .y = vram_h, .z = 1}, .format = gpu::Format::rgba8_unorm,
                                    .usage = gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_destination};
    if (!gpu::supports_texture_format(s.device, raw_desc.format, raw_desc.usage) ||
        !gpu::supports_texture_format(s.device, color_desc.format, color_desc.usage) ||
        !gpu::supports_texture_format(s.device, cpu_desc.format, cpu_desc.usage) ||
        !s.upload.owner || !s.upload.range.cpu || !s.upload.range.gpu ||
        !s.readback.owner || !s.readback.range.cpu || !s.readback.range.gpu ||
        !s.texture_desc.owner || !s.texture_desc.range.cpu ||
        !s.sampler_desc.owner || !s.sampler_desc.range.cpu || !s.timeline ||
        caps.max_push_data_size < sizeof(PsxNgRasterRoot)) {
        set_error("NoGraphicsAPI renderer required format or heap allocation unavailable");
        destroy_all();
        return 0;
    }
    uint64_t texture_bytes = 0;
    texture_bytes = align_up(texture_bytes, gpu::get_texture_size_align(s.device, raw_desc).align) + gpu::get_texture_size_align(s.device, raw_desc).size;
    texture_bytes = align_up(texture_bytes, gpu::get_texture_size_align(s.device, color_desc).align) + gpu::get_texture_size_align(s.device, color_desc).size;
    texture_bytes = align_up(texture_bytes, gpu::get_texture_size_align(s.device, raw_desc).align) + gpu::get_texture_size_align(s.device, raw_desc).size;
    texture_bytes = align_up(texture_bytes, gpu::get_texture_size_align(s.device, cpu_desc).align) + gpu::get_texture_size_align(s.device, cpu_desc).size;
    s.texture_heap = gpu::create_texture_heap(s.device, texture_bytes);
    uint64_t off = 0;
    s.raw = create_texture_at(raw_desc, &off);
    s.color = create_texture_at(color_desc, &off);
    s.scratch = create_texture_at(raw_desc, &off);
    s.cpu_present = create_texture_at(cpu_desc, &off);
    if (!s.texture_heap.owner || !s.raw || !s.color || !s.scratch || !s.cpu_present) {
        set_error("NoGraphicsAPI renderer texture allocation failed");
        destroy_all();
        return 0;
    }

    char *td = reinterpret_cast<char *>(s.texture_desc.range.cpu);
    gpu::write_texture_descriptor(s.device, td + caps.texture_descriptor_size * PSX_NG_DESC_RAW_STORAGE, s.raw, gpu::TextureDescriptorType::storage);
    gpu::write_texture_descriptor(s.device, td + caps.texture_descriptor_size * PSX_NG_DESC_COLOR_STORAGE, s.color, gpu::TextureDescriptorType::storage);
    gpu::write_texture_descriptor(s.device, td + caps.texture_descriptor_size * PSX_NG_DESC_RAW_SAMPLED, s.raw, gpu::TextureDescriptorType::sampled);
    gpu::write_texture_descriptor(s.device, td + caps.texture_descriptor_size * PSX_NG_DESC_COLOR_SAMPLED, s.color, gpu::TextureDescriptorType::sampled);
    gpu::write_texture_descriptor(s.device, td + caps.texture_descriptor_size * PSX_NG_DESC_SCRATCH_STORAGE, s.scratch, gpu::TextureDescriptorType::storage);
    gpu::write_texture_descriptor(s.device, td + caps.texture_descriptor_size * PSX_NG_DESC_CPU_SAMPLED, s.cpu_present, gpu::TextureDescriptorType::sampled);
    char *sd = reinterpret_cast<char *>(s.sampler_desc.range.cpu);
    gpu::write_sampler_descriptor(s.device, sd + caps.sampler_descriptor_size * sampler_nearest, {.min_filter = gpu::Filter::nearest, .mag_filter = gpu::Filter::nearest, .address_u = gpu::AddressMode::clamp_to_edge, .address_v = gpu::AddressMode::clamp_to_edge});
    gpu::write_sampler_descriptor(s.device, sd + caps.sampler_descriptor_size * sampler_linear, {.min_filter = gpu::Filter::linear, .mag_filter = gpu::Filter::linear, .address_u = gpu::AddressMode::clamp_to_edge, .address_v = gpu::AddressMode::clamp_to_edge});

    s.raster_pso = gpu::create_compute_pso(s.device, spv_span(psx_ng_raster_spv, psx_ng_raster_spv_word_count));
    const gpu::ColorTargetDesc target{.format = gpu::Format::bgra8_unorm};
    s.present_pso = gpu::create_graphics_pso(s.device, {.vertex_spirv = spv_span(psx_ng_present_vert_spv, psx_ng_present_vert_spv_word_count),
                                                        .fragment_spirv = spv_span(psx_ng_present_frag_spv, psx_ng_present_frag_spv_word_count),
                                                        .color_targets = gpu::Span<const gpu::ColorTargetDesc>{&target, 1}});
    s.ready = s.raw && s.color && s.scratch && s.cpu_present && s.raster_pso && s.present_pso && s.timeline;
    if (!s.ready) {
        set_error("NoGraphicsAPI renderer resource creation failed");
        destroy_all();
        return 0;
    }
    if (s.vram) {
        upload_vram_region(0, 0, vram_w, vram_h, s.vram);
        s.cpu_dirty = 0;
    }
    set_error("");
    return 1;
}

void resize_context(int width, int height)
{
    s.width = width;
    s.height = height;
    if (s.device)
        (void)gpu::get_drawable_extent(s.device);
}

void restage()
{
    if (s.vram) {
        upload_vram_region(0, 0, vram_w, vram_h, s.vram);
        s.cpu_dirty = 0;
    }
}

void set_scale(int scale)
{
    s.requested_scale = scale > 0 ? scale : 1;
}

int scale()
{
    return 1;
}

void set_texture_filter(int bilinear)
{
    (void)bilinear;
    s.texture_filter = 0;
}

int texture_filter()
{
    return s.texture_filter;
}

void set_semi(int enabled, int mode)
{
    s.semi_enabled = enabled ? 1 : 0;
    s.semi_mode = mode & 3;
}

void set_mask(int set_bit, int check_bit)
{
    s.mask_set = set_bit ? 1 : 0;
    s.mask_check = check_bit ? 1 : 0;
}

void set_texture_window(uint32_t raw)
{
    s.tw_mask_x = int(raw & 0x1f);
    s.tw_mask_y = int((raw >> 5) & 0x1f);
    s.tw_off_x = int((raw >> 10) & 0x1f);
    s.tw_off_y = int((raw >> 15) & 0x1f);
}

void set_color_mod(int r, int g, int b, int raw)
{
    s.mod_r = r >> 3;
    s.mod_g = g >> 3;
    s.mod_b = b >> 3;
    s.raw_texture = raw ? 1 : 0;
}

void set_precise(int, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) {}
void set_perspective(int, float, float, float) {}

void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    PsxNgRasterRoot r = base_root(PSX_NG_PRIM_FILL, x & 1023, y & 511, w, h);
    int old_dirty = s.cpu_dirty;
    r.clip_x1 = 0; r.clip_y1 = 0; r.clip_x2 = vram_w - 1; r.clip_y2 = vram_h - 1;
    r.c0 = color;
    dispatch_root(r);
    if (s.vram)
        for (int row = 0; row < h; ++row)
            for (int col = 0; col < w; ++col)
                s.vram[((y + row) & 511) * vram_w + ((x + col) & 1023)] = color;
    s.cpu_dirty = old_dirty;
}

void copy_rect(int sx, int sy, int dx, int dy, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    w = std::min(w, vram_w);
    h = std::min(h, vram_h);
    PsxNgRasterRoot in = base_root(PSX_NG_PRIM_SCRATCH_COPY_IN, 0, 0, w, h);
    in.sx = sx; in.sy = sy; in.dx = 0; in.dy = 0;
    dispatch_root(in);
    PsxNgRasterRoot out = base_root(PSX_NG_PRIM_SCRATCH_COPY_OUT, 0, 0, w, h);
    out.sx = 0; out.sy = 0; out.dx = dx; out.dy = dy;
    dispatch_root(out);
    if (s.vram) {
        sync_cpu();
        s.cpu_dirty = 0;
    }
}

void tri_common(int textured, int gouraud, float x0, float y0, float x1, float y1, float x2, float y2,
                float u0, float v0, float u1, float v1, float u2, float v2, uint32_t c0, uint32_t c1, uint32_t c2,
                uint16_t clut_x, uint16_t clut_y, uint16_t texpage, int raw_texture)
{
    if (textured)
        snapshot_raw();
    int minx = int(std::floor(std::min({x0, x1, x2})));
    int miny = int(std::floor(std::min({y0, y1, y2})));
    int maxx = int(std::ceil(std::max({x0, x1, x2})));
    int maxy = int(std::ceil(std::max({y0, y1, y2})));
    minx = std::max(minx, s.draw_x1); miny = std::max(miny, s.draw_y1);
    maxx = std::min(maxx, s.draw_x2); maxy = std::min(maxy, s.draw_y2);
    if (maxx < minx || maxy < miny) return;
    PsxNgRasterRoot r = base_root(PSX_NG_PRIM_TRI, minx, miny, maxx - minx + 1, maxy - miny + 1);
    r.textured = uint32_t(textured);
    r.gouraud = uint32_t(gouraud);
    r.raw_texture = uint32_t(raw_texture);
    r.x0 = x0; r.y0 = y0;
    r.x1 = x1; r.y1 = y1;
    r.x2 = x2; r.y2 = y2;
    r.u0 = u0; r.v0 = v0; r.u1 = u1; r.v1 = v1; r.u2 = u2; r.v2 = v2;
    r.c0 = c0; r.c1 = c1; r.c2 = c2;
    r.clut_x = clut_x; r.clut_y = clut_y;
    r.tpage_x = (texpage & 0xf) * 64;
    r.tpage_y = ((texpage >> 4) & 1) * 256;
    r.tex_depth = (texpage >> 7) & 3;
    if (r.tex_depth > 2) r.tex_depth = 2;
    dispatch_root(r);
}

void draw_flat_triangle(int x0,int y0,int x1,int y1,int x2,int y2,uint16_t color)
{
    tri_common(0, 0, float(x0), float(y0), float(x1), float(y1), float(x2), float(y2), 0, 0, 0, 0, 0, 0, color, color, color, 0, 0, 0, 0);
}

void draw_gouraud_triangle(int x0,int y0,uint16_t c0,int x1,int y1,uint16_t c1,int x2,int y2,uint16_t c2)
{
    tri_common(0, 1, float(x0), float(y0), float(x1), float(y1), float(x2), float(y2), 0, 0, 0, 0, 0, 0, c0, c1, c2, 0, 0, 0, 0);
}

void draw_textured_triangle(int x0,int y0,int u0,int v0,int x1,int y1,int u1,int v1,int x2,int y2,int u2,int v2,uint16_t cx,uint16_t cy,uint16_t tp)
{
    tri_common(1, 0, float(x0), float(y0), float(x1), float(y1), float(x2), float(y2), float(u0), float(v0), float(u1), float(v1), float(u2), float(v2), 0, 0, 0, cx, cy, tp, s.raw_texture);
}

void draw_shaded_textured_triangle(int x0,int y0,int u0,int v0,uint32_t c0,int x1,int y1,int u1,int v1,uint32_t c1,int x2,int y2,int u2,int v2,uint32_t c2,uint16_t cx,uint16_t cy,uint16_t tp,int raw)
{
    uint32_t cc0 = ((c0 & 0xff) >> 3) | (((c0 >> 8) & 0xff) >> 3) << 5 | ((((c0 >> 16) & 0xff) >> 3) << 10);
    uint32_t cc1 = ((c1 & 0xff) >> 3) | (((c1 >> 8) & 0xff) >> 3) << 5 | ((((c1 >> 16) & 0xff) >> 3) << 10);
    uint32_t cc2 = ((c2 & 0xff) >> 3) | (((c2 >> 8) & 0xff) >> 3) << 5 | ((((c2 >> 16) & 0xff) >> 3) << 10);
    tri_common(1, 1, float(x0), float(y0), float(x1), float(y1), float(x2), float(y2), float(u0), float(v0), float(u1), float(v1), float(u2), float(v2), cc0, cc1, cc2, cx, cy, tp, raw);
}

void draw_flat_rect(int x, int y, int w, int h, uint16_t c)
{
    if (w <= 0 || h <= 0) return;
    int x0 = std::max(x, s.draw_x1);
    int y0 = std::max(y, s.draw_y1);
    int x1 = std::min(x + w - 1, s.draw_x2);
    int y1 = std::min(y + h - 1, s.draw_y2);
    if (x1 < x0 || y1 < y0) return;
    PsxNgRasterRoot r = base_root(PSX_NG_PRIM_RECT, x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    r.c0 = c;
    dispatch_root(r);
}

void draw_textured_rect_scaled(int x,int y,int w,int h,int u0,int v0,int u1,int v1,uint16_t cx,uint16_t cy,uint16_t tp)
{
    if (w <= 0 || h <= 0) return;
    int x0 = std::max(x, s.draw_x1);
    int y0 = std::max(y, s.draw_y1);
    int x1 = std::min(x + w - 1, s.draw_x2);
    int y1 = std::min(y + h - 1, s.draw_y2);
    if (x1 < x0 || y1 < y0) return;
    snapshot_raw();
    PsxNgRasterRoot r = base_root(PSX_NG_PRIM_TEX_RECT, x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    r.u0 = float(u0) + float(x0 - x) * float(u1 - u0) / float(w);
    r.v0 = float(v0) + float(y0 - y) * float(v1 - v0) / float(h);
    r.u1 = float(u0) + float(x1 + 1 - x) * float(u1 - u0) / float(w);
    r.v1 = float(v0) + float(y1 + 1 - y) * float(v1 - v0) / float(h);
    r.clut_x = cx; r.clut_y = cy;
    r.tpage_x = (tp & 0xf) * 64;
    r.tpage_y = ((tp >> 4) & 1) * 256;
    r.tex_depth = (tp >> 7) & 3;
    if (r.tex_depth > 2) r.tex_depth = 2;
    r.raw_texture = uint32_t(s.raw_texture);
    dispatch_root(r);
}

void draw_textured_rect(int x,int y,int w,int h,int u,int v,uint16_t cx,uint16_t cy,uint16_t tp)
{
    draw_textured_rect_scaled(x, y, w, h, u, v, u + w, v + h, cx, cy, tp);
}

void draw_line_common(int x0, int y0, int x1, int y1, uint16_t c0, uint16_t c1)
{
    int minx = std::max(std::min(x0, x1) - 1, s.draw_x1);
    int miny = std::max(std::min(y0, y1) - 1, s.draw_y1);
    int maxx = std::min(std::max(x0, x1) + 1, s.draw_x2);
    int maxy = std::min(std::max(y0, y1) + 1, s.draw_y2);
    if (maxx < minx || maxy < miny) return;
    PsxNgRasterRoot r = base_root(PSX_NG_PRIM_LINE, minx, miny, maxx - minx + 1, maxy - miny + 1);
    r.x0 = float(x0); r.y0 = float(y0); r.x1 = float(x1); r.y1 = float(y1);
    r.c0 = c0; r.c1 = c1;
    dispatch_root(r);
}

void draw_line(int x0,int y0,int x1,int y1,uint16_t c) { draw_line_common(x0, y0, x1, y1, c, c); }
void draw_shaded_line(int x0,int y0,uint16_t c0,int x1,int y1,uint16_t c1) { draw_line_common(x0, y0, x1, y1, c0, c1); }

int render_display(uint32_t *out, int pitch, int dx, int dy, int dw, int dh)
{
    if (!out || dw <= 0 || dh <= 0) return 0;
    sync_cpu();
    for (int row = 0; row < dh; ++row) {
        uint32_t *dst = reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(out) + size_t(row) * pitch);
        for (int col = 0; col < dw; ++col) {
            uint16_t v = s.vram[((dy + row) & 511) * vram_w + ((dx + col) & 1023)];
            dst[col] = 0xff000000u | uint32_t((v & 31u) * 255u / 31u) << 16 | uint32_t(((v >> 5) & 31u) * 255u / 31u) << 8 | uint32_t(((v >> 10) & 31u) * 255u / 31u);
        }
    }
    return dw * dh;
}

void vram_write(int x, int y, uint16_t pixel)
{
    int old_dirty = s.cpu_dirty;
    int wx = x & 1023;
    int wy = y & 511;
    if (s.mask_check && old_dirty)
        sync_cpu();
    old_dirty = s.cpu_dirty;
    if (s.vram && s.mask_check && (s.vram[wy * vram_w + wx] & 0x8000))
        return;
    if (s.mask_set)
        pixel |= 0x8000;
    if (s.vram) s.vram[wy * vram_w + wx] = pixel;
    upload_vram_region(wx, wy, 1, 1, &pixel);
    s.cpu_dirty = old_dirty;
}

uint16_t vram_read(int x, int y)
{
    sync_cpu();
    return s.vram ? s.vram[(y & 511) * vram_w + (x & 1023)] : 0;
}

void transfer_in(int x, int y, int w, int h, const uint16_t *data)
{
    if (!data || w <= 0 || h <= 0) return;
    const int input_pitch = w;
    w = std::min(w, vram_w);
    h = std::min(h, vram_h);
    std::vector<uint16_t> upload(size_t(w) * h);
    for (int row = 0; row < h; ++row)
        std::memcpy(upload.data() + size_t(row) * w,
                    data + size_t(row) * input_pitch, size_t(w) * sizeof(uint16_t));
    if (s.mask_check) sync_cpu();
    const int old_dirty = s.cpu_dirty;
    for (int row = 0; row < h; ++row)
        for (int col = 0; col < w; ++col) {
            const size_t index = size_t((y + row) & 511) * vram_w + ((x + col) & 1023);
            uint16_t &value = upload[size_t(row) * w + col];
            if (s.vram && s.mask_check && (s.vram[index] & 0x8000)) value = s.vram[index];
            else if (s.mask_set) value |= 0x8000;
            if (s.vram) s.vram[index] = value;
        }
    upload_vram_region(x, y, w, h, upload.data());
    s.cpu_dirty = old_dirty;
}

void transfer_out(int x, int y, int w, int h, uint16_t *data)
{
    sync_cpu();
    if (!s.vram || !data) return;
    for (int row = 0; row < h; ++row)
        for (int col = 0; col < w; ++col)
            data[size_t(row) * w + col] = s.vram[((y + row) & 511) * vram_w + ((x + col) & 1023)];
}

void set_draw_area(int x1, int y1, int x2, int y2)
{
    s.draw_x1 = std::clamp(x1, 0, vram_w - 1);
    s.draw_y1 = std::clamp(y1, 0, vram_h - 1);
    s.draw_x2 = std::clamp(x2, 0, vram_w - 1);
    s.draw_y2 = std::clamp(y2, 0, vram_h - 1);
}

void get_draw_area(int *x1, int *y1, int *x2, int *y2)
{
    if (x1) *x1 = s.draw_x1; if (y1) *y1 = s.draw_y1; if (x2) *x2 = s.draw_x2; if (y2) *y2 = s.draw_y2;
}

void set_draw_offset(int x, int y) { s.off_x = x; s.off_y = y; }

int present_texture(uint32_t texture_index, uint32_t sampler_index, int dx, int dy, int w, int h, int force_4_3)
{
    if (!s.ready || w <= 0 || h <= 0) return 0;
    flush_commands();
    const gpu::SwapchainFrame frame = gpu::acquire(s.device);
    if (!frame.render_view) return 0;
    gpu::CommandBuffer *commands = gpu::begin_commands(s.device);
    gpu::set_texture_descriptor_heap(commands, gpu::gpu_range(s.texture_desc));
    gpu::set_sampler_descriptor_heap(commands, gpu::gpu_range(s.sampler_desc));
    gpu::begin_render_pass(commands, {.colors = {{.render_view = frame.render_view, .load = gpu::LoadOp::clear}}});
    gpu::bind_pso(commands, s.present_pso);
    const PsxNgPresentRoot root{.texture_index = texture_index, .sampler_index = sampler_index, .src_x = dx, .src_y = dy, .src_w = w, .src_h = h,
                                .dst_w = int(frame.extent.x), .dst_h = int(frame.extent.y), .force_4_3 = force_4_3};
    gpu::draw(commands, gpu::ByteSpan(root), 3);
    gpu::end_render_pass(commands);
    const gpu::TimelinePoint done{.semaphore = s.timeline, .value = ++s.timeline_value};
    gpu::CommandBuffer *list[] = {commands};
    gpu::submit_and_present(s.device, gpu::Span<gpu::CommandBuffer *const>{list, 1}, done);
    gpu::wait_timeline(done);
    return 1;
}

int present_vram(int dx,int dy,int w,int h,int linear,int force_4_3) { return present_texture(PSX_NG_DESC_COLOR_SAMPLED, linear ? sampler_linear : sampler_nearest, dx, dy, w, h, force_4_3); }
int present_wide(int, int, int, int) { return 0; }

void present_cpu(const uint32_t *pixels, int w, int h, int linear, int force_4_3)
{
    if (!s.ready || !pixels || w <= 0 || h <= 0) { present_texture(PSX_NG_DESC_COLOR_SAMPLED, sampler_nearest, 0, 0, 1, 1, 0); return; }
    if (w > vram_w) w = vram_w;
    if (h > vram_h) h = vram_h;
    flush_commands();
    uint8_t *dst = reinterpret_cast<uint8_t *>(s.upload.range.cpu);
    for (int i = 0; i < w * h; ++i) {
        uint32_t p = pixels[i];
        dst[i * 4 + 0] = uint8_t((p >> 16) & 0xff);
        dst[i * 4 + 1] = uint8_t((p >> 8) & 0xff);
        dst[i * 4 + 2] = uint8_t(p & 0xff);
        dst[i * 4 + 3] = 0xff;
    }
    gpu::CommandBuffer *commands = gpu::begin_commands(s.device);
    const gpu::TextureCopyDesc copy{.extent = {.x = uint32_t(w), .y = uint32_t(h), .z = 1}, .row_pitch_bytes = uint64_t(w) * 4u, .slice_pitch_bytes = uint64_t(w) * h * 4u};
    gpu::copy_memory_to_texture(commands, gpu::GpuRange{.gpu = s.upload.range.gpu, .size = uint64_t(w) * h * 4u}, s.cpu_present, copy);
    gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::fragment, gpu::Access::shader_read);
    submit_wait(commands);
    present_texture(PSX_NG_DESC_CPU_SAMPLED, linear ? sampler_linear : sampler_nearest, 0, 0, w, h, force_4_3);
}

void present_blank()
{
    const uint32_t black = 0xff000000u;
    present_cpu(&black, 1, 1, 0, 0);
}

void set_present_mode(int) {}

const GpuRenderBackend backend = {
    "nographics",
    init_backend,
    set_scale,
    scale,
    set_texture_filter,
    texture_filter,
    set_semi,
    set_mask,
    set_texture_window,
    set_color_mod,
    nullptr,
    nullptr,
    fill_rect,
    copy_rect,
    draw_flat_triangle,
    draw_gouraud_triangle,
    draw_textured_triangle,
    draw_shaded_textured_triangle,
    draw_flat_rect,
    draw_textured_rect,
    draw_textured_rect_scaled,
    draw_line,
    draw_shaded_line,
    render_display,
    render_display,
    vram_write,
    vram_read,
    transfer_in,
    transfer_out,
    set_draw_area,
    get_draw_area,
    set_draw_offset,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

const PsxNgApi api = {
    PSX_NG_ABI_VERSION,
    sizeof(PsxNgApi),
    &backend,
    init_context,
    destroy_all,
    resize_context,
    present_vram,
    present_wide,
    present_cpu,
    present_blank,
    sync_cpu,
    restage,
    set_present_mode,
};

}

extern "C" __declspec(dllexport) const PsxNgApi *psx_ng_get_api(uint32_t version)
{
    if (version != PSX_NG_ABI_VERSION)
        return nullptr;
    return &api;
}
