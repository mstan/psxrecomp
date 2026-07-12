#version 450
/* CRT present shader (CRT-Royale-inspired, single pass). Replaces the plain
 * present blit when screen kind != raw. Sampled in linear filtering.
 *
 * Kinds (must match ScreenKind in color_lut.h):
 *   1 crt        shadow-mask triads + gaussian scanline beam
 *   2 composite  strong horizontal blur + chroma bleed + soft scanlines
 *   3 trinitron  aperture-grille stripes + gaussian scanline beam
 *
 * Royale signature bits kept: gamma-correct pipeline, energy-conserving
 * gaussian beam whose width grows with brightness (bright lines bloom),
 * phosphor mask applied in linear light with gain compensation, and the
 * scanline effect fading out when the output has too few pixels per line
 * to resolve it (instead of aliasing into moire). */
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
layout(set = 0, binding = 0) uniform sampler2D u_src;
layout(push_constant) uniform PC {
    vec4  u_src_rect;   /* displayed region in normalized src tex coords: x0,y0,x1,y1 */
    vec2  u_out_size;   /* viewport (letterbox rect) size in px */
    vec2  u_native;     /* native source resolution (w = px per line, h = scanlines) */
    int   u_kind;
} pc;

vec3 fetch(vec2 uv) {
    uv = clamp(uv, vec2(0.0), vec2(1.0));
    return texture(u_src, mix(pc.u_src_rect.xy, pc.u_src_rect.zw, uv)).rgb;
}
vec3 to_lin(vec3 c)  { return pow(max(c, 0.0), vec3(2.4)); }
vec3 to_gam(vec3 c)  { return pow(max(c, 0.0), vec3(1.0 / 2.2)); }

/* Horizontally pre-filtered, gamma-decoded source sample at scanline y. */
vec3 line_sample(float x, float y) {
    float hx = (pc.u_kind == 2) ? 1.1 : 0.4;      /* blur radius, native px */
    vec2 d = vec2(hx / pc.u_native.x, 0.0);
    vec2 uv = vec2(x, y);
    vec3 c = fetch(uv) * 0.5 + (fetch(uv - d) + fetch(uv + d)) * 0.25;
    if (pc.u_kind == 2) {                          /* composite chroma bleed */
        c.r = mix(c.r, fetch(uv - 2.0 * d).r, 0.4);
        c.b = mix(c.b, fetch(uv + 2.0 * d).b, 0.4);
    }
    return to_lin(c);
}

void main() {
    float lines = max(pc.u_native.y, 1.0);
    float ly = v_uv.y * lines;                     /* position in scanline space */
    float lc = floor(ly - 0.5) + 0.5;              /* nearest line centre */

    /* Energy-conserving gaussian beam over the 3 nearest scanlines. */
    vec3 beam = vec3(0.0);
    const float INV_SQRT_2PI = 0.3989423;
    for (int i = -1; i <= 1; i++) {
        float yc = lc + float(i);
        vec3 c = line_sample(v_uv.x, yc / lines);
        float lum = clamp(dot(c, vec3(0.299, 0.587, 0.114)), 0.0, 1.0);
        float sigma = mix(0.30, 0.55, lum);        /* bright lines bloom wider */
        float d = ly - yc;
        beam += c * exp(-0.5 * d * d / (sigma * sigma)) * (INV_SQRT_2PI / sigma);
    }

    /* Too few output px per scanline -> fade the beam into a flat sample. */
    float px_per_line = pc.u_out_size.y / lines;
    float fade = clamp(px_per_line * 0.5 - 0.5, 0.0, 1.0);
    vec3 col = mix(line_sample(v_uv.x, v_uv.y), beam, fade);

    /* Phosphor mask (output-pixel space), gain-compensated in linear light. */
    float mstr = (pc.u_kind == 2) ? 0.0 : 0.5;
    mstr *= fade;                                  /* tiny windows: skip mask too */
    if (mstr > 0.0) {
        float x = gl_FragCoord.x;
        if (pc.u_kind == 1)                        /* shadow mask: triads, half-
                                                    * period shift every 2 rows */
            x += (mod(floor(gl_FragCoord.y * 0.5), 2.0) < 1.0) ? 0.0 : 1.5;
        int m = int(mod(x, 3.0));
        vec3 triad = vec3(m == 0 ? 1.0 : 0.0, m == 1 ? 1.0 : 0.0, m == 2 ? 1.0 : 0.0);
        col *= mix(vec3(1.0), triad, mstr);
        col /= 1.0 - mstr * (2.0 / 3.0);
    }

    o_col = vec4(to_gam(min(col, 1.0)), 1.0);
}
