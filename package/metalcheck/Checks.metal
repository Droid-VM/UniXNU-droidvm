#include <metal_stdlib>
using namespace metal;

kernel void mc_affine(device const float *input [[buffer(0)]],
                      device float *output [[buffer(1)]],
                      constant uint &count [[buffer(2)]],
                      uint index [[thread_position_in_grid]]) {
    if (index >= count) return;
    output[index] = 3.0f * input[index] + 7.0f;
}

kernel void mc_atomic_add(device atomic_uint *counter [[buffer(0)]],
                          constant uint &count [[buffer(1)]],
                          uint index [[thread_position_in_grid]]) {
    if (index >= count) return;
    atomic_fetch_add_explicit(counter, 1u, memory_order_relaxed);
}

kernel void mc_threadgroup_sum(device const uint *input [[buffer(0)]],
                               device uint *output [[buffer(1)]],
                               constant uint &count [[buffer(2)]],
                               threadgroup uint *scratch [[threadgroup(0)]],
                               uint tid [[thread_index_in_threadgroup]],
                               uint index [[thread_position_in_grid]]) {
    scratch[tid] = index < count ? input[index] : 0u;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint stride = 8; stride > 0; stride >>= 1) {
        if (tid < stride) scratch[tid] += scratch[tid + stride];
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (tid == 0) output[0] = scratch[0];
}

constant float mc_scale [[function_constant(0)]];

kernel void mc_specialized_scale(device const float *input [[buffer(0)]],
                                 device float *output [[buffer(1)]],
                                 constant uint &count [[buffer(2)]],
                                 uint index [[thread_position_in_grid]]) {
    if (index >= count) return;
    output[index] = input[index] * mc_scale;
}

kernel void mc_fill_sequence(device uint *output [[buffer(0)]],
                             constant uint &count [[buffer(1)]],
                             uint index [[thread_position_in_grid]]) {
    if (index >= count) return;
    output[index] = index * 5u + 9u;
}

kernel void mc_consume_sequence(device const uint *input [[buffer(0)]],
                                device uint *output [[buffer(1)]],
                                constant uint &count [[buffer(2)]],
                                uint index [[thread_position_in_grid]]) {
    if (index >= count) return;
    output[index] = input[index] * 3u + 1u;
}

kernel void mc_benchmark_compute(device const float *input [[buffer(0)]],
                                device float *output [[buffer(1)]],
                                constant uint &count [[buffer(2)]],
                                constant uint &serial [[buffer(3)]],
                                uint index [[thread_position_in_grid]]) {
    if (index >= count) return;
    output[index] = input[index] * 3.0f + float(serial);
}

struct MCVertexOut {
    float4 position [[position]];
    float4 color;
};

struct MCUVOut {
    float4 position [[position]];
    float2 uv [[user(locn0)]];
};

struct MCQuadIn {
    float2 position [[attribute(0)]];
};

struct MCMRTOut {
    float4 first [[color(0)]];
    float4 second [[color(1)]];
};

vertex MCVertexOut mc_fullscreen_vertex(uint vertexID [[vertex_id]]) {
    const float2 positions[3] = {
        float2(-1.0f, -1.0f), float2(3.0f, -1.0f), float2(-1.0f, 3.0f)
    };
    MCVertexOut out;
    out.position = float4(positions[vertexID], 0.0f, 1.0f);
    out.color = float4(1.0f);
    return out;
}

vertex MCUVOut mc_fullscreen_uv_vertex(uint vertexID [[vertex_id]]) {
    const float2 positions[3] = {
        float2(-1.0f, -1.0f), float2(3.0f, -1.0f), float2(-1.0f, 3.0f)
    };
    const float2 coordinates[3] = {
        float2(0.0f, 1.0f), float2(2.0f, 1.0f), float2(0.0f, -1.0f)
    };
    MCUVOut out;
    out.position = float4(positions[vertexID], 0.0f, 1.0f);
    out.uv = coordinates[vertexID];
    return out;
}

fragment float4 mc_red_fragment() {
    return float4(1.0f, 0.0f, 0.0f, 1.0f);
}

fragment float4 mc_blue_fragment() {
    return float4(0.0f, 0.0f, 1.0f, 1.0f);
}

fragment float4 mc_alpha_fragment() {
    return float4(1.0f, 0.0f, 0.0f, 0.5f);
}

fragment float4 mc_bgra_fragment() {
    return float4(1.0f, 0.25f, 0.0f, 1.0f);
}

fragment float4 mc_benchmark_color_fragment(constant float4 &color [[buffer(0)]]) {
    return color;
}

vertex MCVertexOut mc_quad_vertex(MCQuadIn input [[stage_in]]) {
    MCVertexOut out;
    out.position = float4(input.position, 0.0f, 1.0f);
    out.color = float4(1.0f);
    return out;
}

vertex MCVertexOut mc_instanced_quad_vertex(MCQuadIn input [[stage_in]],
                                           uint instanceID [[instance_id]]) {
    MCVertexOut out;
    float xOffset = instanceID == 0 ? -0.5f : 0.5f;
    out.position = float4(input.position + float2(xOffset, 0.0f), 0.0f, 1.0f);
    out.color = instanceID == 0 ? float4(1.0f, 0.0f, 0.0f, 1.0f)
                                : float4(0.0f, 1.0f, 0.0f, 1.0f);
    return out;
}

fragment float4 mc_vertex_color_fragment(MCVertexOut input [[stage_in]]) {
    return input.color;
}

vertex MCVertexOut mc_depth_vertex(uint vertexID [[vertex_id]],
                                   constant float &depth [[buffer(0)]],
                                   constant float4 &color [[buffer(1)]]) {
    const float2 positions[3] = {
        float2(-1.0f, -1.0f), float2(3.0f, -1.0f), float2(-1.0f, 3.0f)
    };
    MCVertexOut out;
    out.position = float4(positions[vertexID], depth, 1.0f);
    out.color = color;
    return out;
}

fragment MCMRTOut mc_mrt_fragment() {
    MCMRTOut out;
    out.first = float4(1.0f, 0.0f, 0.0f, 1.0f);
    out.second = float4(0.0f, 1.0f, 0.0f, 1.0f);
    return out;
}

fragment float4 mc_texture_sample_fragment(MCUVOut input [[stage_in]],
                                          texture2d<float> image [[texture(0)]],
                                          sampler imageSampler [[sampler(0)]]) {
    return image.sample(imageSampler, input.uv);
}
