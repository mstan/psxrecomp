#include "gpu_render.h"
#include "gpu_interlace.h"
#include "gpu_sw_renderer.h"
#include <stdio.h>
#include <string.h>

static uint16_t vram[1024 * 512];
static uint32_t hires[32*4*32*4], reference_hires[32*4*32*4];
static uint16_t reference_vram[32*32];
static int skipped_row;
static int failures;
static int mock_enabled, mock_pc, mock_pq, mock_submissions, mock_bad;
static void mock_precise(int e,int32_t x0,int32_t y0,int32_t x1,int32_t y1,int32_t x2,int32_t y2) {
    (void)x0;(void)y0;(void)x1;(void)y1;(void)x2;(void)y2;mock_pc=e;
}
static void mock_perspective(int e,float q0,float q1,float q2) {
    (void)q0;(void)q1;(void)q2;mock_pq=e;
}
static void mock_triangle(int x0,int y0,int x1,int y1,int x2,int y2,uint16_t c) {
    (void)x0;(void)y0;(void)x1;(void)y1;(void)x2;(void)y2;(void)c;
    ++mock_submissions;if(!mock_pc || !mock_pq)++mock_bad;
    mock_pc=mock_pq=0; /* GPU backends consume these per submission. */
}
/* Wide overlay paths can deliberately ignore the draw-area scissor. */
static unsigned overlay_rows[32];
static void mock_flat_overlay(int x,int y,int w,int h,uint16_t c) {
    (void)x;(void)w;(void)c;
    for (int row=y; row<y+h && row<32; ++row)
        if (row>=0) ++overlay_rows[row];
}
static const GpuRenderBackend mock_backend={
    .name="metadata fixture",.get_draw_area=sw_get_draw_area,.set_draw_area=sw_set_draw_area,
    .set_precise_triangle=mock_precise,.set_perspective_triangle=mock_perspective,
    .draw_flat_triangle=mock_triangle,.draw_flat_rect=mock_flat_overlay
};
/* This fixture keeps optional widescreen presentation disabled. */
int g_ws_bd_stretch_pct, g_ws_bd_stretch_on;
int psx_ws_prim_in_backdrop(void) { return 0; }
int gpu_raster_skipped_row(void) { return skipped_row; }
const GpuRenderBackend *gl_backend_get(void) { return mock_enabled ? &mock_backend : NULL; }
const GpuRenderBackend *vk_backend_get(void) { return NULL; }
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); ++failures; \
} } while (0)

static void reset_canvas(void) {
    skipped_row = -1;
    gr_fill_rect(0, 0, 32, 32, 0);
    gr_set_draw_area(0, 0, 31, 31);
    gr_set_semi_transparency(0, 0);
    gr_set_mask_bits(0, 0);
    gr_set_color_modulation(128, 128, 128, 1);
}

static void draw_kind(int kind) {
    switch (kind) {
    case 0: gr_draw_flat_rect(2, 2, 8, 8, 0x7FFF); break;
    case 1: gr_draw_flat_triangle(2, 2, 10, 2, 2, 10, 0x7FFF); break;
    case 2: gr_draw_gouraud_triangle(2, 2, 0x7FFF, 10, 2, 0x7FFF, 2, 10, 0x7FFF); break;
    case 3: gr_draw_line(2, 2, 2, 9, 0x7FFF); break;
    case 4: gr_draw_shaded_line(2, 2, 0x7FFF, 2, 9, 0x7FFF); break;
    case 5: gr_draw_textured_rect(2, 2, 8, 8, 0, 0, 0, 0, 0x104); break;
    case 6: gr_draw_textured_rect_scaled(2, 2, 8, 8, 0, 0, 8, 8, 0, 0, 0x104); break;
    case 7: gr_draw_textured_triangle(2, 2, 0, 0, 10, 2, 8, 0, 2, 10, 0, 8, 0, 0, 0x104); break;
    case 8: gr_draw_shaded_textured_triangle(2, 2, 0, 0, 0x808080, 10, 2, 8, 0, 0x808080,
                2, 10, 0, 8, 0x808080, 0, 0, 0x104, 1); break;
    }
}

int main(void) {
    for (unsigned bits = 0; bits < 32; ++bits) {
        unsigned i=bits&1, h=(bits>>1)&1, allow=(bits>>2)&1;
        unsigned y=(bits>>3)&1, field=(bits>>4)&1;
        CHECK(psx_gpu_raster_skipped_row(i,h,allow,y,field) ==
              ((i && h && !allow) ? (int)(y ^ field) : -1));
    }
    for (int scale = 1; scale <= 4; scale *= 4) {
        gr_init(vram); gr_set_scale(scale);
        gr_fill_rect(256, 0, 16, 16, 0x7FFF);
        for (int kind = 0; kind < 9; ++kind) {
            for (int skip = -1; skip <= 1; ++skip) {
                reset_canvas(); skipped_row=skip; draw_kind(kind);
                for (int y=2; y<9; ++y) {
                    CHECK((vram[y*1024+2] != 0) == (skip < 0 || (y&1) != skip));
                }
                int x1,y1,x2,y2; gr_get_draw_area(&x1,&y1,&x2,&y2);
                CHECK(x1==0 && y1==0 && x2==31 && y2==31);
                if (kind == 0) {
                    gr_render_display_hires(hires,32*scale*4,0,0,32,32);
                    for (int y=2*scale;y<9*scale;++y)
                        CHECK(((hires[y*32*scale+3*scale]&0xFFFFFF)!=0) ==
                              (skip<0 || ((y/scale)&1)!=skip));
                }
            }
        }
        /* Enhanced vertices can lie above/below integer fallback vertices.
         * Compare each masked surface with its own unmasked pixel oracle. */
        for (int shift = -10; shift <= 10; shift += 20) {
            reset_canvas();
            gr_set_precise_triangle(1,2*65536,(8+shift)*65536+16384,
                10*65536,(8+shift)*65536+16384,2*65536,(16+shift)*65536+16384);
            gr_draw_flat_triangle(2,8,10,8,2,16,0x7FFF);
            for (int y=0; y<32; ++y)
                memcpy(reference_vram+y*32,vram+y*1024,32*sizeof(uint16_t));
            gr_render_display_hires(reference_hires,32*scale*4,0,0,32,32);
            for (int skip=0; skip<=1; ++skip) {
                reset_canvas(); skipped_row=skip;
                gr_set_precise_triangle(1,2*65536,(8+shift)*65536+16384,
                    10*65536,(8+shift)*65536+16384,2*65536,(16+shift)*65536+16384);
                gr_draw_flat_triangle(2,8,10,8,2,16,0x7FFF);
                for (int y=0; y<32; ++y) for (int x=0; x<32; ++x)
                    CHECK(vram[y*1024+x] == ((y&1)==skip ? 0 : reference_vram[y*32+x]));
                gr_render_display_hires(hires,32*scale*4,0,0,32,32);
                for (int y=0; y<32*scale; ++y) for (int x=0; x<32*scale; ++x)
                    CHECK((hires[y*32*scale+x]&0xFFFFFF) == (((y/scale)&1)==skip ? 0 : (reference_hires[y*32*scale+x]&0xFFFFFF)));
            }
        }
        reset_canvas(); skipped_row=1;
        gr_set_draw_area(3,3,8,8); gr_draw_flat_rect(0,0,10,10,0x7FFF);
        CHECK(vram[4*1024+3]==0x7FFF && vram[3*1024+3]==0 && vram[4*1024+2]==0);
        /* Fill/copy/upload/readback must retain BOTH fields. */
        gr_fill_rect(0,0,16,16,0x1234);
        gr_copy_rect(0,0,16,0,16,16);
        uint16_t in[4]={1,2,3,4},out[4]={0};
        gr_vram_transfer_in(0,0,2,2,in); gr_vram_transfer_out(0,0,2,2,out);
        CHECK(memcmp(in,out,sizeof in)==0 && vram[17]==0x1234 && vram[1024+17]==0x1234);
        /* A mode switch restores drawing to both fields immediately. */
        reset_canvas(); skipped_row=0; gr_draw_flat_rect(2,2,1,2,0x7FFF);
        skipped_row=-1; gr_draw_flat_rect(2,2,1,2,0x4321);
        CHECK(vram[2*1024+2]==0x4321 && vram[3*1024+2]==0x4321);
    }
    /* One logical perspective triangle stays one hit despite row re-arms. */
    for (int skip = -1; skip <= 1; ++skip) {
        for (int clipped = 0; clipped <= 1; ++clipped) {
            reset_canvas(); skipped_row = skip;
            if (clipped) gr_set_draw_area(20,20,21,21);
            uint32_t before = gr_perspective_triangle_count();
            gr_set_perspective_triangle(1,1.0f,0.5f,0.25f);
            gr_draw_textured_triangle(2,2,0,0,10,2,8,0,2,10,0,8,0,0,0x104);
            CHECK(gr_perspective_triangle_count() == before + 1);
            gr_set_perspective_triangle(1,0.5f,0.25f,0.125f);
            gr_draw_textured_triangle(2,2,0,0,10,2,8,0,2,10,0,8,0,0,0x104);
            CHECK(gr_perspective_triangle_count() == before + 2);
            gr_set_perspective_triangle(0,1.0f,1.0f,1.0f);
            gr_set_perspective_triangle(1,0.0f,1.0f,1.0f);
            gr_draw_textured_triangle(2,2,0,0,10,2,8,0,2,10,0,8,0,0,0x104);
            CHECK(gr_perspective_triangle_count() == before + 2);
        }
    }
    /* A single enhanced triangle keeps its metadata through all row clips. */
    mock_enabled=1;gr_set_backend(GR_BACKEND_OPENGL);skipped_row=0;
    gr_set_draw_area(0,0,31,31);
    gr_set_precise_triangle(1,0,0,10*65536,0,0,10*65536);
    gr_set_perspective_triangle(1,1.0f,0.5f,0.25f);
    gr_draw_flat_triangle(0,0,10,0,0,10,0x7FFF);
    CHECK(mock_submissions==6 && mock_bad==0 && !mock_pc && !mock_pq);
    mock_submissions=0;
    gr_set_draw_area(0,0,1023,511);
    gr_set_precise_triangle(1,0,8*65536+16384,65536,8*65536+16384,0,9*65536+16384);
    gr_set_perspective_triangle(1,1.0f,1.0f,1.0f);
    gr_draw_flat_triangle(0,8,1,8,0,9,0x7FFF);
    CHECK(mock_submissions==3 && mock_bad==0 && !mock_pc && !mock_pq);
    for (int skip=-1; skip<=1; ++skip) {
        memset(overlay_rows,0,sizeof overlay_rows); skipped_row=skip;
        gr_set_draw_area(0,4,31,15);
        gr_draw_flat_rect(0,0,32,20,0x7FFF);
        for (int y=0; y<32; ++y) {
            unsigned expected = skip<0 ? y<20 : (y>=4 && y<=15 && (y&1)!=skip);
            CHECK(overlay_rows[y] == expected);
        }
        int x1,y1,x2,y2; gr_get_draw_area(&x1,&y1,&x2,&y2);
        CHECK(x1==0 && y1==4 && x2==31 && y2==15);
    }
    gr_set_backend(GR_BACKEND_SOFTWARE);
    if (failures) return 1;
    puts("PASS: field predicate, all raster primitives, clipping, transfers, mode switch, scales 1/4");
    return 0;
}
