struct PsxNgPresentRoot {
    uint texture_index;
    uint sampler_index;
    int src_x;
    int src_y;
    int src_w;
    int src_h;
    int dst_w;
    int dst_h;
    int force_4_3;
    int pad0;
};

[[vk::push_constant]] ConstantBuffer<PsxNgPresentRoot> root;

struct VOut {
    float4 position : SV_Position;
};

[shader("vertex")]
VOut vertexMain(uint vertex_id : SV_VertexID)
{
    float2 p = float2(vertex_id == 1u ? 3.0f : -1.0f,
                      vertex_id == 2u ? 3.0f : -1.0f);
    VOut output;
    output.position = float4(p, 0.0f, 1.0f);
    return output;
}

[shader("fragment")]
float4 fragmentMain(float4 position : SV_Position) : SV_Target0
{
    Texture2D<float4> texture = ResourceDescriptorHeap[root.texture_index];
    SamplerState sampler = SamplerDescriptorHeap[root.sampler_index];
    float dst_aspect = float(root.dst_w) / float(max(root.dst_h, 1));
    float src_aspect = root.force_4_3 != 0 ? (4.0f / 3.0f) : (float(root.src_w) / float(max(root.src_h, 1)));
    float2 minp = float2(0.0f, 0.0f);
    float2 size = float2(float(root.dst_w), float(root.dst_h));
    if (dst_aspect > src_aspect) {
        size.x = size.y * src_aspect;
        minp.x = (float(root.dst_w) - size.x) * 0.5f;
    } else {
        size.y = size.x / src_aspect;
        minp.y = (float(root.dst_h) - size.y) * 0.5f;
    }
    float2 pixel = position.xy;
    if (pixel.x < minp.x || pixel.y < minp.y || pixel.x >= minp.x + size.x || pixel.y >= minp.y + size.y)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    float2 uv = (pixel - minp) / size;
    float2 source = float2(float(root.src_x), float(root.src_y)) + uv * float2(float(root.src_w), float(root.src_h));
    source = clamp(source, float2(root.src_x + 0.5f, root.src_y + 0.5f),
                   float2(root.src_x + root.src_w - 0.5f, root.src_y + root.src_h - 0.5f));
    uv = source / float2(1024.0f, 512.0f);
    float4 c = texture.Sample(sampler, uv);
    return float4(c.rgb, 1.0f);
}
