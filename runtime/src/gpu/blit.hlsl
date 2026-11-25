#ifdef __spirv__
struct PushConstants { uint64_t VertexShaderConstants; uint64_t PixelShaderConstants; uint64_t SharedConstants; };
[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;
#define TEXTURE_INDEX vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants)
#define FLIP vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 4)
#define OFFSET_X vk::RawBufferLoad<int>(g_PushConstants.SharedConstants + 8)
#define OFFSET_Y vk::RawBufferLoad<int>(g_PushConstants.SharedConstants + 12)
#define CIRCLE_COUNT vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 16)
#define CIRCLE(i) vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 32 + (i) * 16)
#define CIRCLE_COLOR(i) vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 32 + 32 * 16 + (i) * 16)
#else
cbuffer SharedConstants : register(b2, space4) { uint g_TextureIndex; uint g_Flip; int g_OffsetX; int g_OffsetY;
    uint g_CircleCount; uint3 g_Pad; float4 g_Circles[32]; float4 g_CircleColors[32]; };
#define CIRCLE_COUNT g_CircleCount
#define CIRCLE(i) g_Circles[i]
#define CIRCLE_COLOR(i) g_CircleColors[i]
#define TEXTURE_INDEX g_TextureIndex
#define FLIP g_Flip
#define OFFSET_X g_OffsetX
#define OFFSET_Y g_OffsetY
#endif
Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);

void mainVS(uint id : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD0)
{
    uv = float2((id << 1) & 2, id & 2);
    position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

#define BLIT_SAMPLER 63 // linear, clamped (renderer.cpp kBlitSampler)
#define FXAA_ENABLED FLIP // the final blit reuses the depth resolve's flag slot

float FxaaLuma(float3 c)
{
    return dot(c, float3(0.299, 0.587, 0.114));
}

// Final image to the window. FXAA ("console" variant of FXAA 3.11, Timothy Lottes) smooths the
// aliased edges: the console renders the scene with 4X MSAA, the host without.
float4 mainPS(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    Texture2D<float4> image = g_Texture2DDescriptorHeap[TEXTURE_INDEX];
    SamplerState linearClamp = g_SamplerDescriptorHeap[BLIT_SAMPLER];
    // A picture larger than the window (resolution scale 2x / 3x): average what lies under the
    // pixel (area filter); one bilinear tap per pixel skipped texels and looked pixelated.
    float2 footprint = float2(abs(ddx(uv.x)), abs(ddy(uv.y)));
    float2 imageSize;
    image.GetDimensions(imageSize.x, imageSize.y);
    float2 ratio = footprint * imageSize;
    if (max(ratio.x, ratio.y) > 1.01)
    {
        int2 taps = int2(clamp(ceil(ratio), 1.0, 4.0));
        float3 sum = 0.0;
        for (int ty = 0; ty < taps.y; ty++)
            for (int tx = 0; tx < taps.x; tx++)
                sum += image.SampleLevel(linearClamp, uv + ((float2(tx, ty) + 0.5) / float2(taps) - 0.5) * footprint, 0).rgb;
        return float4(sum / float(taps.x * taps.y), 1.0);
    }
    float3 rgbM = image.SampleLevel(linearClamp, uv, 0).rgb;
    if (FXAA_ENABLED == 0)
        return float4(rgbM, 1.0);

    float2 size;
    image.GetDimensions(size.x, size.y);
    float2 texel = 1.0 / size;
    float lumaNw = FxaaLuma(image.SampleLevel(linearClamp, uv + float2(-0.5, -0.5) * texel, 0).rgb);
    float lumaSw = FxaaLuma(image.SampleLevel(linearClamp, uv + float2(-0.5, 0.5) * texel, 0).rgb);
    float lumaNe = FxaaLuma(image.SampleLevel(linearClamp, uv + float2(0.5, -0.5) * texel, 0).rgb) + 1.0 / 384.0;
    float lumaSe = FxaaLuma(image.SampleLevel(linearClamp, uv + float2(0.5, 0.5) * texel, 0).rgb);
    float lumaM = FxaaLuma(rgbM);
    float lumaMax = max(max(lumaNw, lumaSw), max(lumaNe, lumaSe));
    float lumaMin = min(min(lumaNw, lumaSw), min(lumaNe, lumaSe));
    if (max(lumaMax, lumaM) - min(lumaMin, lumaM) < max(0.05, lumaMax * 0.125))
        return float4(rgbM, 1.0);

    float dirSwMinusNe = lumaSw - lumaNe;
    float dirSeMinusNw = lumaSe - lumaNw;
    float2 dir1 = normalize(float2(dirSwMinusNe + dirSeMinusNw, dirSwMinusNe - dirSeMinusNw));
    float3 rgbN1 = image.SampleLevel(linearClamp, uv - dir1 * 0.5 * texel, 0).rgb;
    float3 rgbP1 = image.SampleLevel(linearClamp, uv + dir1 * 0.5 * texel, 0).rgb;
    float dirAbsMinTimesC = max(min(abs(dir1.x), abs(dir1.y)) * 8.0, 1e-6);
    float2 dir2 = clamp(dir1 / dirAbsMinTimesC, -2.0, 2.0);
    float3 rgbN2 = image.SampleLevel(linearClamp, uv - dir2 * 2.0 * texel, 0).rgb;
    float3 rgbP2 = image.SampleLevel(linearClamp, uv + dir2 * 2.0 * texel, 0).rgb;
    float3 rgbA = rgbN1 + rgbP1;
    float3 rgbB = (rgbN2 + rgbP2) * 0.25 + rgbA * 0.25;
    float lumaB = FxaaLuma(rgbB);
    if (lumaB < lumaMin || lumaB > lumaMax)
        rgbB = rgbA * 0.5;
    return float4(rgbB, 1.0);
}

// Depth resolve: the copied host depth (R32F) into the destination texture, with the host's
// reversed-Z mirror undone (FLIP), the destination position subtracted (OFFSET).
float4 mainDepthPS(float4 position : SV_Position) : SV_Target
{
    float depth = g_Texture2DDescriptorHeap[TEXTURE_INDEX].Load(int3(int2(position.xy) - int2(OFFSET_X, OFFSET_Y), 0)).r;
    if (FLIP != 0)
        depth = 1.0 - depth;
    return float4(depth, depth, depth, 1.0);
}

// MSAA depth (the 4X scene pass): sample 0 of each pixel, as the console resolves depth, into an
// R32F target mainDepthPS then reads. Set 1 is bound to the same descriptor heap as set 0
// (renderer.cpp BindLayout).
Texture2DMS<float4> g_Texture2DMSDescriptorHeap[] : register(t0, space1);
float4 mainDepthMsaaPS(float4 position : SV_Position) : SV_Target
{
    float depth = g_Texture2DMSDescriptorHeap[TEXTURE_INDEX].Load(int2(position.xy) - int2(OFFSET_X, OFFSET_Y), 0).r;
    return float4(depth, depth, depth, 1.0);
}

// Resolve from the 10:10:10:2 fixed-point view of the HDR buffer: the console copies the raw 7e3
// bits, which the game decodes in its shaders. The host buffer holds values: encode them (unorm).
float encode7e3(float f)
{
    f = clamp(f, 0.0, 31.875);
    if (f < 0.25)
        return round(f * 512.0) / 1023.0;
    float e = floor(log2(f));
    if (f < exp2(e))
        e -= 1.0;
    else if (f >= exp2(e + 1.0))
        e += 1.0;
    float m = round((f / exp2(e) - 1.0) * 128.0);
    if (m >= 128.0)
    {
        m = 0.0;
        e += 1.0;
    }
    return min((e + 3.0) * 128.0 + m, 1023.0) / 1023.0;
}

float4 mainEncode7e3PS(float4 position : SV_Position) : SV_Target
{
    float4 v = g_Texture2DDescriptorHeap[TEXTURE_INDEX].Load(int3(int2(position.xy) - int2(OFFSET_X, OFFSET_Y), 0));
    return float4(encode7e3(v.r), encode7e3(v.g), encode7e3(v.b), round(saturate(v.a) * 3.0) / 3.0);
}

// Touch controls (hid/touch_controls.cpp): circles (centre, radius in pixels, filled > 0.5) with
// straight-alpha colours, blended over the final image.
float4 mainOverlayPS(float4 position : SV_Position) : SV_Target
{
    float4 result = 0.0;
    for (uint i = 0; i < CIRCLE_COUNT; i++)
    {
        float4 circle = CIRCLE(i);
        float4 color = CIRCLE_COLOR(i);
        float distance = length(position.xy - circle.xy);
        float coverage = circle.w > 0.5 ? saturate(circle.z - distance) : saturate(1.5 - abs(distance - circle.z));
        float alpha = coverage * color.a;
        result.rgb = color.rgb * alpha + result.rgb * (1.0 - alpha);
        result.a = alpha + result.a * (1.0 - alpha);
    }
    return float4(result.rgb / max(result.a, 1e-4), result.a);
}

// Options menu (ui/options_menu.cpp): a straight-alpha RGBA image over the final picture.
float4 mainMenuPS(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return g_Texture2DDescriptorHeap[TEXTURE_INDEX].SampleLevel(g_SamplerDescriptorHeap[BLIT_SAMPLER], uv, 0);
}
