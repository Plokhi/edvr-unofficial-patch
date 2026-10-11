#pragma once

namespace edvr {

// SMAA 1x for the map temporal-off frames BELOW NATIVE, before EASU (2026-10-10, the maintainer's decision: EASU upscales the stair-steps
// of the half-res image, so the edges are cleaned first). The SMAA text itself is src/d3d11/smaa/SMAA.hlsl (MIT, Jorge Jimenez et al.,
// upstream commit 71c806a8), embedded whole by tools/gen_smaa_hlsl.py (smaa_hlsl_gen.h). tools/temporal_shader_build builds one source
// per entry: kMapSmaaPrologueHlsl, then the SMAA chunks, then one of the three entries below. The runtime has no HLSL compiler.
//
// The prologue selects SMAA_CUSTOM_SL, so the samplers and texture macros are the ones here, with the bodies the reference's own
// SMAA_HLSL_4 block uses; only the sampler registers are fixed (LinearSampler s0, PointSampler s1). SMAA_PRESET_HIGH is the preset. The
// reference's vertex functions compute the neighbour offsets and the entries call them directly, so nothing is interpolated.
//
// Pass 1 (smaaEdges): colour edges over the bounded colour (c / (1 + max c), the map's compression, mapCompress). Pass 2 (smaaWeights):
// the blending weights from the edges, with AreaTex and SearchTex (map_smaa.cpp). Pass 3 (smaaBlend): the neighbourhood blend, the output
// EASU reads in place of the bounded colour. All three run at the render size.
inline constexpr char kMapSmaaPrologueHlsl[] = R"HLSL(
#define SMAA_CUSTOM_SL 1
#define SMAA_PRESET_HIGH 1
#define SMAA_RT_METRICS smaaMetrics
#define SMAA_AREATEX_SELECT(sample) sample.rg
#define SMAA_SEARCHTEX_SELECT(sample) sample.r
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
// Compute has no implicit derivatives, so the implicit-LOD Sample calls become SampleLevel at mip 0. Every SMAA texture here has one mip,
// so the result is the same sample the reference's pixel shader takes.
#define SMAASample(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASamplePoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
// A compute entry has no pixel discard. SMAA's colour edge test discards a pixel with no edge: its pixel shader writes nothing, so the edge
// target keeps zero. The compute version returns zero edges for that pixel explicitly, which is what the target would hold (the function
// returns float2 in both edge tests, and the discard is only in those).
#define discard return float2(0.0, 0.0);
SamplerState LinearSampler : register(s0) { Filter = MIN_MAG_LINEAR_MIP_POINT; AddressU = Clamp; AddressV = Clamp; };
SamplerState PointSampler : register(s1) { Filter = MIN_MAG_MIP_POINT; AddressU = Clamp; AddressV = Clamp; };
// (1/width, 1/height, width, height) at the render size.
cbuffer SmaaCon : register(b0) { float4 smaaMetrics; };
)HLSL";

inline constexpr char kMapSmaaEdgesHlsl[] = R"HLSL(
Texture2D MapSmaaColor : register(t0);
RWTexture2D<float2> MapSmaaEdges : register(u0);

[numthreads(8, 8, 1)]
void smaaEdges(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uint(smaaMetrics.z) || id.y >= uint(smaaMetrics.w)) return;
    const float2 texcoord = (float2(id.xy) + 0.5) * smaaMetrics.xy;
    float4 offset[3];
    SMAAEdgeDetectionVS(texcoord, offset);
    MapSmaaEdges[id.xy] = SMAAColorEdgeDetectionPS(texcoord, offset, MapSmaaColor);
}
)HLSL";

inline constexpr char kMapSmaaWeightsHlsl[] = R"HLSL(
Texture2D MapSmaaEdgesIn : register(t0);
Texture2D MapSmaaArea : register(t1);
Texture2D MapSmaaSearch : register(t2);
RWTexture2D<float4> MapSmaaWeights : register(u0);

[numthreads(8, 8, 1)]
void smaaWeights(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uint(smaaMetrics.z) || id.y >= uint(smaaMetrics.w)) return;
    const float2 texcoord = (float2(id.xy) + 0.5) * smaaMetrics.xy;
    float2 pixcoord;
    float4 offset[3];
    SMAABlendingWeightCalculationVS(texcoord, pixcoord, offset);
    // SMAA 1x: no subsample indices, so the zero vector.
    MapSmaaWeights[id.xy] = SMAABlendingWeightCalculationPS(texcoord, pixcoord, offset, MapSmaaEdgesIn, MapSmaaArea, MapSmaaSearch,
                                                            float4(0.0, 0.0, 0.0, 0.0));
}
)HLSL";

inline constexpr char kMapSmaaBlendHlsl[] = R"HLSL(
Texture2D MapSmaaColorIn : register(t0);
Texture2D MapSmaaWeightsIn : register(t1);
RWTexture2D<float4> MapSmaaBlended : register(u0);

[numthreads(8, 8, 1)]
void smaaBlend(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uint(smaaMetrics.z) || id.y >= uint(smaaMetrics.w)) return;
    const float2 texcoord = (float2(id.xy) + 0.5) * smaaMetrics.xy;
    float4 offset;
    SMAANeighborhoodBlendingVS(texcoord, offset);
    MapSmaaBlended[id.xy] = SMAANeighborhoodBlendingPS(texcoord, offset, MapSmaaColorIn, MapSmaaWeightsIn);
}
)HLSL";

}  // namespace edvr
