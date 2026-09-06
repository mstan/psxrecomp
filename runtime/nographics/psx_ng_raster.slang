struct PsxNgRasterRoot {
    uint op;
    uint textured;
    uint gouraud;
    uint raw_texture;
    int x;
    int y;
    int w;
    int h;
    int sx;
    int sy;
    int dx;
    int dy;
    int clip_x1;
    int clip_y1;
    int clip_x2;
    int clip_y2;
    int mask_set;
    int mask_check;
    int semi_enabled;
    int semi_mode;
    int tw_mask_x;
    int tw_mask_y;
    int tw_off_x;
    int tw_off_y;
    int tpage_x;
    int tpage_y;
    int tex_depth;
    int clut_x;
    int clut_y;
    int mod_r;
    int mod_g;
    int mod_b;
    float x0;
    float y0;
    float x1;
    float y1;
    float x2;
    float y2;
    float u0;
    float v0;
    float u1;
    float v1;
    float u2;
    float v2;
    uint c0;
    uint c1;
    uint c2;
    uint pad0;
};

[[vk::push_constant]] ConstantBuffer<PsxNgRasterRoot> root;

uint wrap_x(int x) { return uint(x & 1023); }
uint wrap_y(int y) { return uint(y & 511); }

uint read_raw(int x, int y)
{
    RWTexture2D<uint> raw = ResourceDescriptorHeap[0];
    return raw[uint2(wrap_x(x), wrap_y(y))].x & 0xffffu;
}

uint read_scratch(int x, int y)
{
    RWTexture2D<uint> scratch = ResourceDescriptorHeap[4];
    return scratch[uint2(wrap_x(x), wrap_y(y))].x & 0xffffu;
}

void write_raw_color(int x, int y, uint color)
{
    RWTexture2D<uint> raw = ResourceDescriptorHeap[0];
    RWTexture2D<float4> rgba = ResourceDescriptorHeap[1];
    uint v = color & 0xffffu;
    raw[uint2(wrap_x(x), wrap_y(y))] = v;
    rgba[uint2(wrap_x(x), wrap_y(y))] = float4(float(v & 31u) / 31.0f,
                                               float((v >> 5u) & 31u) / 31.0f,
                                               float((v >> 10u) & 31u) / 31.0f,
                                               ((v >> 15u) & 1u) != 0u ? 1.0f : 0.0f);
}

uint blend_1555(uint back, uint front, int mode)
{
    int br = int(back & 31u);
    int bg = int((back >> 5u) & 31u);
    int bb = int((back >> 10u) & 31u);
    int fr = int(front & 31u);
    int fg = int((front >> 5u) & 31u);
    int fb = int((front >> 10u) & 31u);
    int r = fr;
    int g = fg;
    int b = fb;
    if (mode == 0) { r = (br + fr) >> 1; g = (bg + fg) >> 1; b = (bb + fb) >> 1; }
    else if (mode == 1) { r = min(31, br + fr); g = min(31, bg + fg); b = min(31, bb + fb); }
    else if (mode == 2) { r = max(0, br - fr); g = max(0, bg - fg); b = max(0, bb - fb); }
    else if (mode == 3) { r = min(31, br + (fr >> 2)); g = min(31, bg + (fg >> 2)); b = min(31, bb + (fb >> 2)); }
    return uint(r | (g << 5) | (b << 10));
}

void put_pixel(int x, int y, uint color, bool textured, bool texel_stp)
{
    if (x < root.clip_x1 || x > root.clip_x2 || y < root.clip_y1 || y > root.clip_y2)
        return;
    uint dst = read_raw(x, y);
    if (root.mask_check != 0 && ((dst >> 15u) & 1u) != 0u)
        return;
    uint out_color = textured ? (color & 0xffffu) : (color & 0x7fffu);
    if (root.semi_enabled != 0 && (!textured || texel_stp))
        out_color = blend_1555(dst, out_color, root.semi_mode);
    if (root.mask_set != 0 || (textured && texel_stp))
        out_color |= 0x8000u;
    write_raw_color(x, y, out_color);
}

uint fetch_texel(int u, int v)
{
    u &= 255;
    v &= 255;
    if ((root.tw_mask_x | root.tw_mask_y) != 0) {
        u = (u & ~(root.tw_mask_x * 8)) | ((root.tw_off_x & root.tw_mask_x) * 8);
        v = (v & ~(root.tw_mask_y * 8)) | ((root.tw_off_y & root.tw_mask_y) * 8);
    }
    if (root.tex_depth == 0) {
        uint px = read_scratch(root.tpage_x + (u >> 2), root.tpage_y + v);
        return read_scratch(root.clut_x + int((px >> uint((u & 3) * 4)) & 15u), root.clut_y);
    }
    if (root.tex_depth == 1) {
        uint px = read_scratch(root.tpage_x + (u >> 1), root.tpage_y + v);
        return read_scratch(root.clut_x + int((px >> uint((u & 1) * 8)) & 255u), root.clut_y);
    }
    return read_scratch(root.tpage_x + u, root.tpage_y + v);
}

uint modulate(uint texel, float3 color)
{
    if (root.raw_texture != 0)
        return texel & 0xffffu;
    int tr = int(texel & 31u);
    int tg = int((texel >> 5u) & 31u);
    int tb = int((texel >> 10u) & 31u);
    int r = min(31, (tr * int(color.x)) >> 4);
    int g = min(31, (tg * int(color.y)) >> 4);
    int b = min(31, (tb * int(color.z)) >> 4);
    return uint(r | (g << 5) | (b << 10)) | (texel & 0x8000u);
}

float edge(float ax, float ay, float bx, float by, float px, float py)
{
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

float3 color5(uint c)
{
    return float3(float(c & 31u), float((c >> 5u) & 31u), float((c >> 10u) & 31u));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    int px = root.x + int(tid.x);
    int py = root.y + int(tid.y);
    if (int(tid.x) >= root.w || int(tid.y) >= root.h)
        return;

    if (root.op == 1u) {
        write_raw_color(px, py, root.c0);
        return;
    }

    if (root.op == 2u) {
        RWTexture2D<uint> scratch = ResourceDescriptorHeap[4];
        scratch[uint2(uint(root.dx + int(tid.x)), uint(root.dy + int(tid.y)))] = read_raw(root.sx + int(tid.x), root.sy + int(tid.y));
        return;
    }

    if (root.op == 3u) {
        RWTexture2D<uint> scratch = ResourceDescriptorHeap[4];
        uint v = scratch[uint2(wrap_x(root.sx + int(tid.x)), wrap_y(root.sy + int(tid.y)))].x & 0xffffu;
        int tx = root.dx + int(tid.x);
        int ty = root.dy + int(tid.y);
        uint dst = read_raw(tx, ty);
        if (root.mask_check != 0 && ((dst >> 15u) & 1u) != 0u)
            return;
        if (root.mask_set != 0)
            v |= 0x8000u;
        write_raw_color(tx, ty, v);
        return;
    }

    if (root.op == 6u) {
        write_raw_color(px, py, read_raw(px, py));
        return;
    }

    if (root.op == 7u) {
        put_pixel(px, py, root.c0, false, false);
        return;
    }

    if (root.op == 8u) {
        float ru = root.w > 0 ? float(px - root.x) / float(root.w) : 0.0f;
        float rv = root.h > 0 ? float(py - root.y) / float(root.h) : 0.0f;
        uint texel = fetch_texel(int(floor(lerp(root.u0, root.u1, ru))), int(floor(lerp(root.v0, root.v1, rv))));
        if (texel == 0u)
            return;
        put_pixel(px, py, modulate(texel, float3(float(root.mod_r), float(root.mod_g), float(root.mod_b))), true, ((texel >> 15u) & 1u) != 0u);
        return;
    }

    if (root.op == 5u) {
        float x0 = root.x0;
        float y0 = root.y0;
        float x1 = root.x1;
        float y1 = root.y1;
        float2 p = float2(float(px) + 0.5f, float(py) + 0.5f);
        float2 a = float2(x0, y0);
        float2 b = float2(x1, y1);
        float2 ab = b - a;
        float len2 = max(dot(ab, ab), 0.0001f);
        float t = clamp(dot(p - a, ab) / len2, 0.0f, 1.0f);
        float2 q = a + ab * t;
        if (dot(p - q, p - q) > 0.75f)
            return;
        float3 c = lerp(color5(root.c0), color5(root.c1), t);
        put_pixel(px, py, uint(int(c.x + 0.5f) | (int(c.y + 0.5f) << 5) | (int(c.z + 0.5f) << 10)), false, false);
        return;
    }

    float area = edge(root.x0, root.y0, root.x1, root.y1, root.x2, root.y2);
    if (abs(area) < 0.0001f)
        return;
    float fx = float(px) + 0.5f;
    float fy = float(py) + 0.5f;
    float e0 = edge(root.x1, root.y1, root.x2, root.y2, fx, fy);
    float e1 = edge(root.x2, root.y2, root.x0, root.y0, fx, fy);
    float e2 = edge(root.x0, root.y0, root.x1, root.y1, fx, fy);
    bool top0 = (root.y2 < root.y1) || (root.y2 == root.y1 && root.x2 > root.x1);
    bool top1 = (root.y0 < root.y2) || (root.y0 == root.y2 && root.x0 > root.x2);
    bool top2 = (root.y1 < root.y0) || (root.y1 == root.y0 && root.x1 > root.x0);
    if (area < 0.0f) {
        e0 = -e0; e1 = -e1; e2 = -e2; area = -area;
    }
    if (e0 < 0.0f || e1 < 0.0f || e2 < 0.0f)
        return;
    if ((e0 == 0.0f && !top0) || (e1 == 0.0f && !top1) || (e2 == 0.0f && !top2))
        return;
    float w0 = e0 / area;
    float w1 = e1 / area;
    float w2 = e2 / area;

    if (root.textured != 0u) {
        float u = root.u0 * w0 + root.u1 * w1 + root.u2 * w2;
        float v = root.v0 * w0 + root.v1 * w1 + root.v2 * w2;
        uint texel = fetch_texel(int(floor(u)), int(floor(v)));
        if (texel == 0u)
            return;
        float3 c = root.gouraud != 0u
            ? color5(root.c0) * w0 + color5(root.c1) * w1 + color5(root.c2) * w2
            : float3(float(root.mod_r), float(root.mod_g), float(root.mod_b));
        put_pixel(px, py, modulate(texel, c), true, ((texel >> 15u) & 1u) != 0u);
        return;
    }

    float3 c = root.gouraud != 0u
        ? color5(root.c0) * w0 + color5(root.c1) * w1 + color5(root.c2) * w2
        : color5(root.c0);
    put_pixel(px, py, uint(int(c.x + 0.5f) | (int(c.y + 0.5f) << 5) | (int(c.z + 0.5f) << 10)), false, false);
}
