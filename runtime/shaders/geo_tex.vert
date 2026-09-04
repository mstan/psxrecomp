#version 450
/* Textured primitives. Per-prim texture state (texpage, clut, depth, raw, uv
 * limits) rides in FLAT vertex attributes so consecutive prims with the same
 * blend/mask/texture-window/filter coalesce into one draw (see flush_tex_batch).
 * The push-constant block carries the vertex transform (u_shift/u_xoff/u_xhalf)
 * and the fragment batch keys (semipass/maskset/filter/twin). Ported from the
 * GL backend's TEX_VS. */
layout(location = 0) in vec2  a_pos;
layout(location = 1) in vec2  a_uv;
layout(location = 2) in vec4  a_col;
layout(location = 3) in vec2  a_tpage;
layout(location = 4) in vec2  a_clut;
layout(location = 5) in float a_depth;
layout(location = 6) in float a_raw;
layout(location = 7) in vec4  a_limits;
/* Perspective weight ([video] perspective_texturing). 0 = affine, the
 * PS1-faithful default: w below is then exactly 1.0 and the fragment shader
 * reads the noperspective varying, i.e. the pre-feature pipeline unchanged. */
layout(location = 8) in float a_q;

layout(push_constant) uniform PC {
    float u_shift;
    float u_xoff;
    float u_xhalf;
    int   u_semipass;
    int   u_maskset;
    int   u_filter;
    ivec4 u_twin;
} pc;

layout(location = 0) noperspective out vec2  v_uv;
layout(location = 1) noperspective out vec4  v_col;
layout(location = 2) flat out ivec2 v_tpage;
layout(location = 3) flat out ivec2 v_clut;
layout(location = 4) flat out int   v_depth;
layout(location = 5) flat out int   v_raw;
layout(location = 6) flat out ivec4 v_limits;
layout(location = 7) smooth out vec2 v_uv_p;   /* perspective-correct UV */
layout(location = 8) flat   out int  v_persp;

void main() {
    v_uv = a_uv; v_uv_p = a_uv; v_col = a_col;
    v_persp = (a_q > 0.0) ? 1 : 0;
    v_tpage = ivec2(a_tpage + 0.5);
    v_clut  = ivec2(a_clut + 0.5);
    v_depth = int(a_depth + 0.5);
    v_raw   = int(a_raw + 0.5);
    v_limits = ivec4(floor(a_limits + 0.5));
    /* Perspective-correct UV rides the standard w divide: emit clip coords
     * pre-multiplied by w = 1/q so post-divide NDC is unchanged while the
     * rasterizer interpolates v_uv_p hyperbolically. a_q == 0 => w == 1.0. */
    float w = (a_q > 0.0) ? (1.0 / a_q) : 1.0;
    vec2 ndc = vec2((a_pos.x + pc.u_shift + pc.u_xoff) / pc.u_xhalf - 1.0,
                    (a_pos.y + pc.u_shift) / 256.0 - 1.0);
    gl_Position = vec4(ndc * w, 0.0, w);
}
