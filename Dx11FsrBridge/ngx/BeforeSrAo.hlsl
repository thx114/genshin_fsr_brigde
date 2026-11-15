// 2026-11-15：只做深度法线平滑与空间遮蔽。没有光流、随机抖动、历史或高分辨率滤镜链。
Texture2D<float4> color_in : register(t0);
Texture2D<float> device_depth : register(t1);
RWTexture2D<float4> color_out : register(u0);

cbuffer Projection : register(b0)
{
    float near_plane;
    float near_over_far;
    float ray_step;
    uint reversed_depth;
};

static const float k_radius = 0.85;
static const float k_strength = 0.47;
static const float k_normal_bias = 0.09;

float3 view_position(int2 p, int2 extent)
{
    if (any(p < 0) || any(p >= extent)) return 0.0;
    float raw = device_depth.Load(int3(p, 0));
    if (!(raw >= 0.0 && raw <= 1.0)) return 0.0;
    float d = reversed_depth != 0 ? raw : 1.0 - raw;
    // 清屏/天空不参与法线，也不能成为遮挡物。深度保持与 NGX 输入相同的方向和亚像素位置。
    if (d <= 0.0) return 0.0;
    float z = near_plane / lerp(near_over_far, 1.0, d);
    float2 xy = (float2(p) + 0.5 - float2(extent) * 0.5) * (ray_step * z) * float2(1.0, -1.0);
    return float3(xy, z);
}

void add_gradient(int2 p, int2 extent, int2 offset, float3 center, float band, float inverse_step,
    inout float3 sum, inout float weights)
{
    float3 sample_pos = view_position(p + offset, extent);
    if (sample_pos.z <= 0.0) return;
    float3 delta = sample_pos - center;
    // 对称近邻平滑；跨遮挡边界的差分不能把前景的法线拉到后景。
    float w = saturate(1.0 - abs(delta.z) / band);
    w *= w;
    sum += (inverse_step * w) * delta;
    weights += w;
}

float3 gradient(int2 p, int2 extent, int2 axis, float3 center, float band, float3 fallback)
{
    float3 sum = 0.0;
    float weights = 0.0;
    add_gradient(p, extent, axis, center, band, 1.0, sum, weights);
    add_gradient(p, extent, -axis, center, band, -1.0, sum, weights);
    add_gradient(p, extent, axis * 2, center, band * 2.0, 0.5, sum, weights);
    add_gradient(p, extent, -axis * 2, center, band * 2.0, -0.5, sum, weights);
    return weights > 0.001 ? sum / weights : fallback;
}

float occlusion(int2 pos, int2 extent, float3 center, float3 normal)
{
    float3 other = view_position(pos, extent);
    if (other.z <= 0.0) return 0.0;
    float3 v = other - center;
    float dist2 = dot(v, v);
    float angle = dot(v, normal) * rsqrt(max(dist2, 1e-8));
    float falloff = saturate(1.0 - dist2 / (k_radius * k_radius));
    return saturate(angle - k_normal_bias) * falloff;
}

[numthreads(8, 8, 1)]
void main(uint3 thread_id : SV_DispatchThreadID)
{
    uint width, height;
    device_depth.GetDimensions(width, height);
    if (thread_id.x >= width || thread_id.y >= height) return;
    int2 extent = int2(width, height), p = int2(thread_id.xy);
    float4 color = color_in.Load(int3(p, 0));
    float3 pos = view_position(p, extent);
    if (pos.z > 0.0)
    {
        float pixel_span = ray_step * pos.z;
        float band = max(0.08, 3.0 * pixel_span);
        float3 dx = gradient(p, extent, int2(1, 0), pos, band, float3(pixel_span, 0.0, 0.0));
        float3 dy = gradient(p, extent, int2(0, 1), pos, band, float3(0.0, -pixel_span, 0.0));
        float3 n = cross(dx, dy);
        float n2 = dot(n, n);
        n = n2 > 1e-18 ? n * rsqrt(n2) : float3(0.0, 0.0, -1.0);
        int r = int(clamp(round(k_radius * 0.84 / pixel_span), 3.0, 18.0));
        int h = (r + 1) >> 1;
        float ao = occlusion(p + int2( r, 0), extent, pos, n) + occlusion(p + int2(-r, 0), extent, pos, n) +
                   occlusion(p + int2( 0, r), extent, pos, n) + occlusion(p + int2( 0,-r), extent, pos, n) +
                   occlusion(p + int2( h, h), extent, pos, n) + occlusion(p + int2(-h, h), extent, pos, n) +
                   occlusion(p + int2( h,-h), extent, pos, n) + occlusion(p + int2(-h,-h), extent, pos, n);
        // 显式 opt-in 只衰减 RGBA8 的存储值。不能从 typeless 或 UNORM 推断 transfer。
        color.rgb *= 1.0 - k_strength * saturate(ao * 0.25);
    }
    color_out[p] = color;
}
