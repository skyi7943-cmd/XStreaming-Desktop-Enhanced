#pragma once
// XStreaming Desktop Enhanced - 全链路 D3D11 HLSL 计算与呈现着色器集合 (v2 含 30-in-60 硬件去重 + 前向投射无重影真 X2 补帧)

namespace XShaders {

// 0. 30-in-60 串流重复帧高速抗噪检测、全黑空帧识别与全局切镜检测着色器（单线程组 4,096 块 3x3 滤波归约，耗时 < 0.01ms）
static const char* HLSL_DETECT_DUPLICATE = R"hlsl(
Texture2D<float4> CandTex : register(t0);
Texture2D<float4> PrevTex : register(t1);

// [0].x = activeChangedTiles (0..4096), [0].y = sumDiffFixed, [0].z = sumCandLumaFixed, [0].w = maxCandLumaFixed
RWStructuredBuffer<uint4> OutStats : register(u0);

cbuffer DedupCB : register(b0) {
    uint2 Extent;
    float TileDiffThreshold; // 默认 0.013 (~3.3/255 灰阶，免疫 H.264 P 帧重编码量化蚊噪)
    uint  Pad0;
};

groupshared uint gsActive[256];
groupshared uint gsDiff[256];
groupshared uint gsLuma[256];
groupshared uint gsMaxLuma[256];

float SampleBox3x3Luma(Texture2D<float4> tex, int2 center) {
    float sum = 0.0;
    [unroll] for (int dy = -1; dy <= 1; ++dy) {
        [unroll] for (int dx = -1; dx <= 1; ++dx) {
            int2 p = clamp(center + int2(dx * 2, dy * 2), int2(0, 0), int2(Extent) - 1);
            float3 c = tex.Load(int3(p, 0)).rgb;
            sum += dot(c, float3(0.299, 0.587, 0.114));
        }
    }
    return sum * (1.0 / 9.0);
}

[numthreads(16, 16, 1)]
void CSMain(uint3 gtid : SV_GroupThreadID, uint flatIdx : SV_GroupIndex) {
    uint activeCount = 0;
    uint diffAccum = 0;
    uint lumaAccum = 0;
    uint maxLuma = 0;

    // 采样中央 84% 游戏核心画面（避开四角网络状态小图标闪烁干扰），共 64x64 = 4096 个采样区
    float2 minUv = float2(0.08, 0.08);
    float2 spanUv = float2(0.84, 0.84);

    [unroll] for (uint sy = 0; sy < 4; ++sy) {
        [unroll] for (uint sx = 0; sx < 4; ++sx) {
            uint gx = gtid.x * 4 + sx;
            uint gy = gtid.y * 4 + sy;
            float2 uv = minUv + (float2(gx, gy) + 0.5) * (spanUv / 64.0);
            int2 pt = int2(uv * float2(Extent));

            float lc = SampleBox3x3Luma(CandTex, pt);
            float lp = SampleBox3x3Luma(PrevTex, pt);
            float d = abs(lc - lp);
            if (d > TileDiffThreshold) {
                activeCount += 1;
            }
            diffAccum += uint(d * 65536.0);
            uint lf = uint(saturate(lc) * 65535.0);
            lumaAccum += lf;
            maxLuma = max(maxLuma, lf);
        }
    }

    gsActive[flatIdx] = activeCount;
    gsDiff[flatIdx] = diffAccum;
    gsLuma[flatIdx] = lumaAccum;
    gsMaxLuma[flatIdx] = maxLuma;
    GroupMemoryBarrierWithGroupSync();

    [unroll] for (uint stride = 128; stride > 0; stride >>= 1) {
        if (flatIdx < stride) {
            gsActive[flatIdx] += gsActive[flatIdx + stride];
            gsDiff[flatIdx] += gsDiff[flatIdx + stride];
            gsLuma[flatIdx] += gsLuma[flatIdx + stride];
            gsMaxLuma[flatIdx] = max(gsMaxLuma[flatIdx], gsMaxLuma[flatIdx + stride]);
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (flatIdx == 0) {
        OutStats[0] = uint4(gsActive[0], gsDiff[0], gsLuma[0], gsMaxLuma[0]);
    }
}
)hlsl";

// 1. NVOFA S10.5 硬件光流稠密化与双向一致性置信度计算着色器
static const char* HLSL_NVOFA_DENSIFY = R"hlsl(
Texture2D<int2>   FwdFlowTex : register(t0);
Texture2D<int2>   BwdFlowTex : register(t1);
Texture2D<uint>   FwdCostTex : register(t2);
Texture2D<uint>   BwdCostTex : register(t3);

RWTexture2D<float2> OutFwdMotion : register(u0);
RWTexture2D<float2> OutBwdMotion : register(u1);
RWTexture2D<float2> OutConf      : register(u2); // .x = Curr 空间前向可见置信度, .y = Prev 空间反向可见置信度

cbuffer DensifyCB : register(b0) {
    uint2 SourceExtent;
    uint2 FlowExtent;
    uint  GridSize;
    uint  HasCost;
    uint  HasBackward;
    uint  Pad0;
};

float2 LoadS105(Texture2D<int2> tex, int2 p) {
    p = clamp(p, int2(0, 0), int2(FlowExtent) - 1);
    return float2(tex.Load(int3(p, 0))) * (1.0 / 32.0);
}

float2 SampleFlowBilinear(Texture2D<int2> tex, float2 pixelPos) {
    float2 g = pixelPos / float(GridSize) - 0.5;
    int2 p0 = int2(floor(g));
    float2 f = frac(g);
    float2 v00 = LoadS105(tex, p0);
    float2 v10 = LoadS105(tex, p0 + int2(1, 0));
    float2 v01 = LoadS105(tex, p0 + int2(0, 1));
    float2 v11 = LoadS105(tex, p0 + int2(1, 1));
    return lerp(lerp(v00, v10, f.x), lerp(v01, v11, f.x), f.y);
}

float LoadCostNorm(Texture2D<uint> tex, int2 p) {
    p = clamp(p, int2(0, 0), int2(FlowExtent) - 1);
    return float(tex.Load(int3(p, 0))) * (1.0 / 255.0);
}

float SampleCostBilinear(Texture2D<uint> tex, float2 pixelPos) {
    float2 g = pixelPos / float(GridSize) - 0.5;
    int2 p0 = int2(floor(g));
    float2 f = frac(g);
    float c00 = LoadCostNorm(tex, p0);
    float c10 = LoadCostNorm(tex, p0 + int2(1, 0));
    float c01 = LoadCostNorm(tex, p0 + int2(0, 1));
    float c11 = LoadCostNorm(tex, p0 + int2(1, 1));
    return lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= SourceExtent)) return;
    float2 p = float2(tid.xy) + 0.5;
    // NVOFA 约定：Fwd 为 Curr -> Prev 位移矢量 (prevPixel = p + fwd)；Bwd 为 Prev -> Curr 位移矢量 (currPixel = p + bwd)
    float2 fwd = SampleFlowBilinear(FwdFlowTex, p);
    float fwdConf = (HasCost != 0) ? saturate(1.15 - SampleCostBilinear(FwdCostTex, p)) : 0.85;
    float bwdConf = fwdConf;

    float2 bwd = -fwd;
    if (HasBackward != 0) {
        bwd = SampleFlowBilinear(BwdFlowTex, p);
        bwdConf = (HasCost != 0) ? saturate(1.15 - SampleCostBilinear(BwdCostTex, p)) : 0.85;

        // 1. Curr 像素在 Prev 中的双向循环一致性校验
        float2 refPrev = p + fwd;
        bool insidePrev = all(refPrev >= 0.0) && all(refPrev < float2(SourceExtent));
        float2 bwdAtPrev = SampleFlowBilinear(BwdFlowTex, refPrev);
        float errFwd = length(fwd + bwdAtPrev);
        float tolFwd = 1.50 + 0.12 * length(fwd);
        fwdConf *= insidePrev ? saturate(1.0 - errFwd / tolFwd) : 0.0;

        // 2. Prev 像素在 Curr 中的双向循环一致性校验（用于精准识别模型前缘遮挡区！）
        float2 refCurr = p + bwd;
        bool insideCurr = all(refCurr >= 0.0) && all(refCurr < float2(SourceExtent));
        float2 fwdAtCurr = SampleFlowBilinear(FwdFlowTex, refCurr);
        float errBwd = length(bwd + fwdAtCurr);
        float tolBwd = 1.50 + 0.12 * length(bwd);
        bwdConf *= insideCurr ? saturate(1.0 - errBwd / tolBwd) : 0.0;
    }

    OutFwdMotion[tid.xy] = fwd;
    OutBwdMotion[tid.xy] = bwd;
    OutConf[tid.xy] = saturate(float2(fwdConf, bwdConf));
}
)hlsl";

// 2A. AMD Radeon / 通用 GPU 三级金字塔特征构建着色器 (Luma + 双色差 + 局部梯度结构)
static const char* HLSL_PYR_BUILD = R"hlsl(
Texture2D<float4> SrcTex : register(t0);
SamplerState LinearClamp : register(s0);

RWTexture2D<float4> DstPyr : register(u0); // .x=Luma, .y=(R-G)*0.5, .z=(B-G)*0.5, .w=GradStruct

cbuffer PyrBuildCB : register(b0) {
    uint2  DstExtent;
    float2 InvDstExtent;
    uint   FromBaseColor;
    uint3  Pad0;
};

float4 ExtractFeatureFromRgb(float3 c) {
    float luma = dot(c, float3(0.299, 0.587, 0.114));
    return float4(luma, (c.r - c.g) * 0.5, (c.b - c.g) * 0.5, 0.0);
}

float4 SampleFiltered(float2 uv) {
    uv = clamp(uv, float2(0.0, 0.0), float2(1.0, 1.0));
    float4 s = SrcTex.SampleLevel(LinearClamp, uv, 0);
    return (FromBaseColor != 0) ? ExtractFeatureFromRgb(s.rgb) : s;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= DstExtent)) return;
    float2 uv = (float2(tid.xy) + 0.5) * InvDstExtent;
    float2 q = InvDstExtent * 0.25;

    // 4 点抗混叠高斯盒式降采样，消除棋盘格与细线条摩尔纹
    float4 f00 = SampleFiltered(uv + float2(-q.x, -q.y));
    float4 f10 = SampleFiltered(uv + float2( q.x, -q.y));
    float4 f01 = SampleFiltered(uv + float2(-q.x,  q.y));
    float4 f11 = SampleFiltered(uv + float2( q.x,  q.y));
    float4 avg = 0.25 * (f00 + f10 + f01 + f11);

    float lL = SampleFiltered(uv - float2(InvDstExtent.x, 0.0)).x;
    float lR = SampleFiltered(uv + float2(InvDstExtent.x, 0.0)).x;
    float lU = SampleFiltered(uv - float2(0.0, InvDstExtent.y)).x;
    float lD = SampleFiltered(uv + float2(0.0, InvDstExtent.y)).x;
    float grad = saturate((abs(lR - lL) + abs(lD - lU)) * 1.75);

    DstPyr[tid.xy] = float4(avg.xyz, grad);
}
)hlsl";

// 2B. AMD Radeon 三级由粗到精金字塔双向光流搜索着色器 (L2 -> L1 -> L0，最高覆盖 ±96px 大位移转视角)
static const char* HLSL_PYR_FLOW_ESTIMATE = R"hlsl(
Texture2D<float4> SrcPyr     : register(t0);
Texture2D<float4> RefPyr     : register(t1);
Texture2D<float2> ParentFlow : register(t2);
SamplerState LinearClamp     : register(s0);

RWTexture2D<float2> OutLevelFlow : register(u0);

cbuffer PyrFlowCB : register(b0) {
    uint2  Extent;
    float2 InvExtent;
    float2 InvParentExtent;
    float2 ParentScale;
    uint   HasParent;
    uint3  Pad0;
};

float EvalPatchCost(float2 uvSrc, float2 vecPx) {
    float2 uvRef = uvSrc + vecPx * InvExtent;
    if (any(uvRef < 0.001) || any(uvRef > 0.999)) return 999.0;
    float cost = 0.0;
    [unroll] for (int dy = -1; dy <= 1; ++dy) {
        [unroll] for (int dx = -1; dx <= 1; ++dx) {
            float2 off = float2(dx, dy) * InvExtent;
            float4 a = SrcPyr.SampleLevel(LinearClamp, uvSrc + off, 0);
            float4 b = RefPyr.SampleLevel(LinearClamp, uvRef + off, 0);
            float4 d = abs(a - b);
            cost += d.x + 0.90 * (d.y + d.z) + 0.35 * d.w;
        }
    }
    // 极微弱零位移正则项：纯色暗场/蓝天无纹理区自动锚定 0 位移，绝不产生伪随机漂移
    return cost * (1.0 / 9.0) + length(vecPx) * 0.00035;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= Extent)) return;
    float2 uv = (float2(tid.xy) + 0.5) * InvExtent;

    float2 bestVec = float2(0.0, 0.0);
    float bestCost = EvalPatchCost(uv, bestVec);

    const float2 dirs[8] = {
        float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1),
        float2(1, 1), float2(-1, 1), float2(1, -1), float2(-1, -1)
    };

    if (HasParent != 0) {
        // 继承父级金字塔光流 + 4 邻域前景边界传播候选（精准锁住移动车体/人物边缘）
        float2 p0 = ParentFlow.SampleLevel(LinearClamp, uv, 0) * ParentScale;
        float c0 = EvalPatchCost(uv, p0);
        if (c0 < bestCost - 0.0008) { bestCost = c0; bestVec = p0; }

        const float2 pOffs[4] = {
            float2( 2.0, 0.0), float2(-2.0, 0.0),
            float2( 0.0, 2.0), float2( 0.0,-2.0)
        };
        [loop] for (int k = 0; k < 4; ++k) {
            float2 pk = ParentFlow.SampleLevel(LinearClamp, clamp(uv + pOffs[k] * InvParentExtent, 0.0, 1.0), 0) * ParentScale;
            float ck = EvalPatchCost(uv, pk);
            if (ck < bestCost - 0.0012) { bestCost = ck; bestVec = pk; }
        }

        const float steps[3] = { 2.0, 1.0, 0.5 };
        [loop] for (int s = 0; s < 3; ++s) {
            float r = steps[s];
            float2 center = bestVec;
            [loop] for (int d = 0; d < 8; ++d) {
                float2 cand = center + dirs[d] * r;
                float cc = EvalPatchCost(uv, cand);
                if (cc < bestCost - 0.0008) {
                    bestCost = cc;
                    bestVec = cand;
                }
            }
        }
    } else {
        // L2 顶层大范围种子搜索（覆盖 ±24px / ±48px / ±72px 主机 30fps 快速摇杆转视角）
        const float2 wideSeeds[8] = {
            float2(-3.0, 0.0), float2( 3.0, 0.0), float2(0.0, -3.0), float2(0.0,  3.0),
            float2(-6.0, 0.0), float2( 6.0, 0.0), float2(-9.0, 0.0), float2( 9.0, 0.0)
        };
        [loop] for (int w = 0; w < 8; ++w) {
            float cw = EvalPatchCost(uv, wideSeeds[w]);
            if (cw < bestCost - 0.0012) { bestCost = cw; bestVec = wideSeeds[w]; }
        }

        const float coarseSteps[4] = { 4.0, 2.0, 1.0, 0.5 };
        [loop] for (int s = 0; s < 4; ++s) {
            float r = coarseSteps[s];
            float2 center = bestVec;
            [loop] for (int d = 0; d < 8; ++d) {
                float2 cand = center + dirs[d] * r;
                float cc = EvalPatchCost(uv, cand);
                if (cc < bestCost - 0.0008) {
                    bestCost = cc;
                    bestVec = cand;
                }
            }
        }
    }

    OutLevelFlow[tid.xy] = bestVec;
}
)hlsl";

// 2C. AMD Radeon 全分辨率保边光流稠密化 + 真前向/反向循环一致性遮挡置信度着色器
static const char* HLSL_PYR_FLOW_DENSIFY = R"hlsl(
Texture2D<float2> FwdFlowL0 : register(t0);
Texture2D<float2> BwdFlowL0 : register(t1);
Texture2D<float4> CurrTex   : register(t2);
Texture2D<float4> PrevTex   : register(t3);
SamplerState LinearClamp    : register(s0);

RWTexture2D<float2> OutFwdMotion : register(u0);
RWTexture2D<float2> OutBwdMotion : register(u1);
RWTexture2D<float2> OutConf      : register(u2); // .x = Curr 前向可见置信度, .y = Prev 反向可见置信度

cbuffer PyrDensifyCB : register(b0) {
    uint2  Extent;
    float2 InvExtent;
    float2 InvL0Extent;
    float2 ScaleToFull;
};

float MaxDiff3(float3 a, float3 b) {
    float3 d = abs(a - b);
    return max(d.r, max(d.g, d.b));
}

float EvalFullMatch(Texture2D<float4> srcT, Texture2D<float4> refT, float2 uv, float2 vFull) {
    float2 uvR = uv + vFull * InvExtent;
    if (any(uvR < 0.0) || any(uvR > 1.0)) return 999.0;
    float3 s0 = srcT.SampleLevel(LinearClamp, uv, 0).rgb;
    float3 r0 = refT.SampleLevel(LinearClamp, uvR, 0).rgb;
    float3 sx = srcT.SampleLevel(LinearClamp, clamp(uv + float2(InvExtent.x * 2.0, 0.0), 0.0, 1.0), 0).rgb;
    float3 rx = refT.SampleLevel(LinearClamp, clamp(uvR + float2(InvExtent.x * 2.0, 0.0), 0.0, 1.0), 0).rgb;
    return 0.65 * MaxDiff3(s0, r0) + 0.35 * MaxDiff3(sx, rx) + length(vFull) * 0.0002;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= Extent)) return;
    float2 uv = (float2(tid.xy) + 0.5) * InvExtent;

    // 1. 前向光流 (Curr -> Prev) 全分辨率保边锐化上采样
    float2 fwd = FwdFlowL0.SampleLevel(LinearClamp, uv, 0) * ScaleToFull;
    float bestFwdErr = EvalFullMatch(CurrTex, PrevTex, uv, fwd);
    float zeroFwdErr = EvalFullMatch(CurrTex, PrevTex, uv, float2(0.0, 0.0));
    if (zeroFwdErr + 0.003 < bestFwdErr) { bestFwdErr = zeroFwdErr; fwd = float2(0.0, 0.0); }

    const float2 nOff[4] = { float2(1,0), float2(-1,0), float2(0,1), float2(0,-1) };
    [unroll] for (int i = 0; i < 4; ++i) {
        float2 cand = FwdFlowL0.SampleLevel(LinearClamp, clamp(uv + nOff[i] * InvL0Extent, 0.0, 1.0), 0) * ScaleToFull;
        float err = EvalFullMatch(CurrTex, PrevTex, uv, cand);
        if (err + 0.004 < bestFwdErr) { bestFwdErr = err; fwd = cand; }
    }

    // 2. 反向光流 (Prev -> Curr) 独立全分辨率保边锐化上采样
    float2 bwd = BwdFlowL0.SampleLevel(LinearClamp, uv, 0) * ScaleToFull;
    float bestBwdErr = EvalFullMatch(PrevTex, CurrTex, uv, bwd);
    float zeroBwdErr = EvalFullMatch(PrevTex, CurrTex, uv, float2(0.0, 0.0));
    if (zeroBwdErr + 0.003 < bestBwdErr) { bestBwdErr = zeroBwdErr; bwd = float2(0.0, 0.0); }

    [unroll] for (int j = 0; j < 4; ++j) {
        float2 cand = BwdFlowL0.SampleLevel(LinearClamp, clamp(uv + nOff[j] * InvL0Extent, 0.0, 1.0), 0) * ScaleToFull;
        float err = EvalFullMatch(PrevTex, CurrTex, uv, cand);
        if (err + 0.004 < bestBwdErr) { bestBwdErr = err; bwd = cand; }
    }

    // 3. 真双向循环一致性校验：精准区分前景遮挡区与新露出背景区，彻底消除 AMD 补帧重影！
    float2 refPrevUv = uv + fwd * InvExtent;
    bool insidePrev = all(refPrevUv >= 0.0) && all(refPrevUv <= 1.0);
    float2 bwdAtPrev = BwdFlowL0.SampleLevel(LinearClamp, clamp(refPrevUv, 0.0, 1.0), 0) * ScaleToFull;
    float errFwd = length(fwd + bwdAtPrev);
    float tolFwd = 1.65 + 0.14 * length(fwd);
    float photoFwd = saturate(1.12 - bestFwdErr * 2.2);
    float fwdConf = insidePrev ? (photoFwd * saturate(1.0 - errFwd / tolFwd)) : 0.0;

    float2 refCurrUv = uv + bwd * InvExtent;
    bool insideCurr = all(refCurrUv >= 0.0) && all(refCurrUv <= 1.0);
    float2 fwdAtCurr = FwdFlowL0.SampleLevel(LinearClamp, clamp(refCurrUv, 0.0, 1.0), 0) * ScaleToFull;
    float errBwd = length(bwd + fwdAtCurr);
    float tolBwd = 1.65 + 0.14 * length(bwd);
    float photoBwd = saturate(1.12 - bestBwdErr * 2.2);
    float bwdConf = insideCurr ? (photoBwd * saturate(1.0 - errBwd / tolBwd)) : 0.0;

    OutFwdMotion[tid.xy] = fwd;
    OutBwdMotion[tid.xy] = bwd;
    OutConf[tid.xy] = saturate(float2(fwdConf, bwdConf));
}
)hlsl";

// 2D. AMD Radeon 专属 Oklab 多尺度光影重塑 + H.264 天空去色带 + 暗场零斑点精雕着色器 (RadeonNR)
// 原生支持「自然均衡 (0) / 鲜明通透 (1) / 电影光影 (2)」三大风格及「再叠加一层 (Pass 1 -> Pass 2)」双层显存串联！
static const char* HLSL_AMD_RELIGHT_PASS = R"hlsl(
Texture2D<float4> WorkIn  : register(t0);
SamplerState LinearClamp  : register(s0);

RWTexture2D<float4> WorkOut : register(u0);

cbuffer AmdRelightCB : register(b0) {
    uint2  WorkExtent;
    float2 InvWorkExtent;
    uint   Style;           // 0 = 自然均衡, 1 = 鲜明通透, 2 = 电影光影
    float  Intensity;       // 0.0 ~ 2.0
    float  LocalTone;       // 0.0 ~ 2.0
    float  LocalStructure;  // 0.0 ~ 2.0
    float  SkinStructure;   // 0.0 ~ 2.0
    uint   PassIndex;       // 0 = 第 1 层, 1 = 第 2 层串联叠加
    float2 Pad0;
};

float DecodeSDR(float v) {
    float a = abs(v);
    return sign(v) * (a <= 0.04045 ? a / 12.92 : pow((a + 0.055) / 1.055, 2.4));
}
float EncodeSDR(float v) {
    float a = abs(v);
    return sign(v) * (a <= 0.0031308 ? a * 12.92 : 1.055 * pow(a, 1.0 / 2.4) - 0.055);
}
float3 DecodeRGB(float3 c) { return float3(DecodeSDR(c.r), DecodeSDR(c.g), DecodeSDR(c.b)); }
float3 EncodeRGB(float3 c) { return float3(EncodeSDR(c.r), EncodeSDR(c.g), EncodeSDR(c.b)); }

float SignedCbrt(float x) {
    float ax = abs(x);
    return (ax < 1e-12) ? 0.0 : (sign(x) * pow(ax, 1.0 / 3.0));
}

float3 RGBToLab(float3 c) {
    float3 lin = DecodeRGB(c);
    float l = SignedCbrt(dot(lin, float3(0.4122214708, 0.5363325363, 0.0514459929)));
    float m = SignedCbrt(dot(lin, float3(0.2119034982, 0.6806995451, 0.1073969566)));
    float s = SignedCbrt(dot(lin, float3(0.0883024619, 0.2817188376, 0.6299787005)));
    return float3(
        0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
        1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
        0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s
    );
}

float3 LabToLinear(float3 c) {
    float3 v = float3(
        c.x + 0.3963377774 * c.y + 0.2158037573 * c.z,
        c.x - 0.1055613458 * c.y - 0.0638541728 * c.z,
        c.x - 0.0894841775 * c.y - 1.2914855480 * c.z
    );
    v = v * v * v;
    return float3(
        dot(v, float3( 4.0767416621, -3.3077115913,  0.2309699292)),
        dot(v, float3(-1.2684380046,  2.6097574011, -0.3413193965)),
        dot(v, float3(-0.0041960863, -0.7034186147,  1.7076147010))
    );
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= WorkExtent)) return;
    float3 baseRgb = saturate(WorkIn.Load(int3(tid.xy, 0)).rgb);
    if (Intensity <= 0.001) {
        WorkOut[tid.xy] = float4(baseRgb, 1.0);
        return;
    }

    float2 uv = (float2(tid.xy) + 0.5) * InvWorkExtent;
    float3 lab0 = RGBToLab(baseRgb);

    // 1. 细尺度保边采样 (r = 1.4px)：分离真实材质微纹理与 8-bit H.264 宏块阶梯色带
    const float2 dirs4[4] = { float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1) };
    const float2 diag4[4] = { float2(0.707, 0.707), float2(-0.707, 0.707), float2(0.707, -0.707), float2(-0.707, -0.707) };

    float3 sumFine = lab0 * 0.40;
    float  wFine   = 0.40;
    [unroll] for (int i = 0; i < 4; ++i) {
        float3 s = RGBToLab(saturate(WorkIn.SampleLevel(LinearClamp, clamp(uv + dirs4[i] * InvWorkExtent * 1.35, 0.0, 1.0), 0).rgb));
        float dl = abs(s.x - lab0.x);
        float w = exp(-dl * 28.0);
        sumFine += s * w;
        wFine += w;
    }
    float3 labFine = sumFine / max(wFine, 1e-4);

    // 2. 中尺度腔体立体光影采样 (r = 4.2px)：提取建筑/车身/岩石凹凸体积感并柔化天空 8x8 压缩块色带
    float3 sumMeso = labFine * 0.35;
    float  wMeso   = 0.35;
    float  maxSmoothStep = 0.0;
    [unroll] for (int j = 0; j < 4; ++j) {
        float3 s1 = RGBToLab(saturate(WorkIn.SampleLevel(LinearClamp, clamp(uv + dirs4[j] * InvWorkExtent * 4.2, 0.0, 1.0), 0).rgb));
        float3 s2 = RGBToLab(saturate(WorkIn.SampleLevel(LinearClamp, clamp(uv + diag4[j] * InvWorkExtent * 5.2, 0.0, 1.0), 0).rgb));
        float d1 = abs(s1.x - lab0.x);
        float d2 = abs(s2.x - lab0.x);
        maxSmoothStep = max(maxSmoothStep, max(d1, d2));
        float w1 = exp(-d1 * 18.0);
        float w2 = exp(-d2 * 18.0) * 0.75;
        sumMeso += s1 * w1 + s2 * w2;
        wMeso += w1 + w2;
    }
    float3 labMeso = sumMeso / max(wMeso, 1e-4);

    // 3. 大尺度局部环境光照包络采样 (r = 10.5px)：用于局部色调映射重打光
    float sumMacroL = labMeso.x * 0.40;
    float wMacro    = 0.40;
    [unroll] for (int m = 0; m < 4; ++m) {
        float3 sm = RGBToLab(saturate(WorkIn.SampleLevel(LinearClamp, clamp(uv + diag4[m] * InvWorkExtent * 10.5, 0.0, 1.0), 0).rgb));
        float dm = abs(sm.x - lab0.x);
        float wm = exp(-dm * 11.0);
        sumMacroL += sm.x * wm;
        wMacro += wm;
    }
    float macroL = sumMacroL / max(wMacro, 1e-4);

    // 4. H.264 8-bit 缓变天空/迷雾自动去色带（仅在邻域整体梯度极平坦 < 0.016 时平滑阶梯，100% 保护真实纹理与边缘）
    float debandMask = 1.0 - smoothstep(0.006, 0.018, maxSmoothStep);
    float3 cleanLab = lerp(lab0, labMeso, debandMask * 0.55);

    // 5. 黑夜纯暗场防斑点门控：RGB < 16 (Oklab L < 0.19) 严禁拉亮压缩底噪，100% 杜绝黑夜大面积斑点！
    float nightGate = smoothstep(0.19, 0.33, lab0.x);
    float highRoll  = 1.0 - 0.55 * smoothstep(0.84, 0.99, lab0.x);

    // 微纹理抗蚊噪门限：屏蔽 ±2/255 编码噪点，精准雕刻真实游戏材质
    float microRaw = lab0.x - labFine.x;
    float microGate = smoothstep(0.0045, 0.015, abs(microRaw));
    float detailMicro = microRaw * microGate;
    float detailMeso  = (labFine.x - labMeso.x) * smoothstep(0.005, 0.020, abs(labFine.x - labMeso.x));
    float detailMacro = (labMeso.x - macroL);

    // 第 2 层叠加时采用 0.68 谐波阻尼系数，让双层串联既有浑厚立体感又绝不过曝振铃
    float passGain = (PassIndex == 0) ? Intensity : (Intensity * 0.68);

    float deltaL = 0.0;
    float2 outChroma = cleanLab.yz;

    if (Style == 0) {
        // 风格 0：自然均衡 — 原汁原味微结构清晰还原 + 柔和立体层次
        deltaL = (detailMicro * 0.30 * LocalStructure +
                  detailMeso  * 0.20 * LocalTone +
                  detailMacro * 0.08 * LocalTone) * nightGate * highRoll;
        outChroma *= lerp(1.0, 1.04, saturate(passGain * nightGate));
    } else if (Style == 1) {
        // 风格 1：鲜明通透 — 扫除串流灰雾感，提亮中暗部通透度并增强自然色彩张力（内置暖肤色保护）
        float clarityLift = 0.018 * sin(3.14159265 * saturate((cleanLab.x - 0.22) / 0.68));
        deltaL = (clarityLift * LocalTone +
                  detailMicro * 0.44 * LocalStructure +
                  detailMeso  * 0.32 * LocalTone +
                  detailMacro * 0.14 * LocalTone) * nightGate * highRoll;
        float warmSkin = smoothstep(0.01, 0.06, cleanLab.y) * smoothstep(0.01, 0.06, cleanLab.z);
        float chromaBoost = lerp(1.18, 1.06 + 0.04 * SkinStructure, warmSkin);
        outChroma *= lerp(1.0, chromaBoost, saturate(passGain * nightGate));
    } else {
        // 风格 2：电影光影 — 胶片级 S 型光影重塑（深邃立体暗部 + 温润高光质感）+ 强烈腔体 AO 浮雕感
        float tNorm = saturate((cleanLab.x - 0.22) / 0.68);
        float filmicS = -0.028 * sin(6.2831853 * tNorm) * smoothstep(0.22, 0.34, cleanLab.x);
        deltaL = (filmicS * LocalTone +
                  detailMicro * 0.26 * LocalStructure +
                  detailMeso  * 0.38 * LocalTone +
                  detailMacro * 0.20 * LocalTone) * nightGate * highRoll;
        // 电影冷暖色调分离：暗部微沉稳、亮部微暖金
        float2 tintShift = lerp(float2(-0.0020, -0.0035), float2(0.0022, 0.0045), tNorm) * nightGate;
        outChroma = outChroma * lerp(1.0, 1.10, saturate(passGain * nightGate)) + tintShift * saturate(passGain);
    }

    float finalL = clamp(cleanLab.x + deltaL * passGain, 0.0, 1.0);
    float3 outLin = max(LabToLinear(float3(finalL, outChroma)), 0.0);
    float3 outRgb = saturate(EncodeRGB(outLin));
    WorkOut[tid.xy] = float4(outRgb, 1.0);
}
)hlsl";

// 3. 神经工作集抗锯齿降采样与运动矢量同步缩放着色器
static const char* HLSL_DOWNSAMPLE_WORKSET = R"hlsl(
Texture2D<float4> SrcColor  : register(t0);
Texture2D<float2> SrcMotion : register(t1);
SamplerState LinearClamp    : register(s0);

RWTexture2D<float4> DstWorksetColor  : register(u0);
RWTexture2D<float2> DstWorksetMotion : register(u1);

cbuffer WorksetCB : register(b0) {
    uint2  SrcExtent;
    uint2  DstExtent;
    float2 InvSrcExtent;
    float2 InvDstExtent;
    float2 MotionScale;
    uint2  Pad0;
};

float Lanczos2Weight(float x) {
    x = abs(x);
    if (x < 1e-5) return 1.0;
    if (x >= 2.0) return 0.0;
    float px = 3.14159265 * x;
    return (sin(px) * sin(px * 0.5)) / (px * px * 0.5);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= DstExtent)) return;
    float2 uv = (float2(tid.xy) + 0.5) * InvDstExtent;

    if (all(SrcExtent == DstExtent)) {
        float4 c = SrcColor.Load(int3(tid.xy, 0));
        DstWorksetColor[tid.xy] = float4(c.rgb, 1.0);
        DstWorksetMotion[tid.xy] = SrcMotion.Load(int3(tid.xy, 0));
        return;
    }

    float2 srcPos = uv * float2(SrcExtent) - 0.5;
    int2 center = int2(floor(srcPos));
    float3 sum = 0.0;
    float wsum = 0.0;
    [unroll] for (int dy = -1; dy <= 2; ++dy) {
        [unroll] for (int dx = -1; dx <= 2; ++dx) {
            int2 sp = clamp(center + int2(dx, dy), int2(0, 0), int2(SrcExtent) - 1);
            float wx = Lanczos2Weight(srcPos.x - float(sp.x));
            float wy = Lanczos2Weight(srcPos.y - float(sp.y));
            float w = wx * wy;
            sum += SrcColor.Load(int3(sp, 0)).rgb * w;
            wsum += w;
        }
    }
    float3 col = saturate(sum / max(wsum, 1e-5));
    DstWorksetColor[tid.xy] = float4(col, 1.0);
    float2 mv = SrcMotion.SampleLevel(LinearClamp, uv, 0) * MotionScale;
    DstWorksetMotion[tid.xy] = mv;
}
)hlsl";

// 4. 1:1 移植 Magpie 原版 Oklab 感知空间残差精调 + 4x4 Catmull-Rom 双三次残差上采样着色器
// 彻底移除会导致车牌边缘局部色块与风格滤镜失效的 localContrast 空间遮罩，100% 完整释放自然/鲜明/电影三大神经风格！
static const char* HLSL_OKLAB_RESIDUAL_TEMPORAL = R"hlsl(
Texture2D<float4> WorksetIn       : register(t0);
Texture2D<float4> WorksetOut      : register(t1);
Texture2D<float4> FullSrcColor    : register(t2);
Texture2D<float2> FwdMotion       : register(t3);
Texture2D<float2> FlowConf        : register(t4);
Texture2D<float4> PrevResidualTex : register(t5);
SamplerState LinearClamp          : register(s0);

RWTexture2D<float4> OutResidual   : register(u0);

cbuffer ResidualCB : register(b0) {
    uint2  SrcExtent;
    uint2  WorkExtent;
    float2 InvSrcExtent;
    float2 InvWorkExtent;
    float  ResidualStrength;
    float  ShadowStructure;
    float  HighlightGlow;
    float  Saturation;
    float  DarkProtection;
    float  HighlightProtection;
    uint   AntiFlickerMode;
    uint   HasHistory;
};

float DecodeSDR(float v) {
    float a = abs(v);
    return sign(v) * (a <= 0.04045 ? a / 12.92 : pow((a + 0.055) / 1.055, 2.4));
}
float EncodeSDR(float v) {
    float a = abs(v);
    return sign(v) * (a <= 0.0031308 ? a * 12.92 : 1.055 * pow(a, 1.0 / 2.4) - 0.055);
}
float3 DecodeRGB(float3 c) { return float3(DecodeSDR(c.r), DecodeSDR(c.g), DecodeSDR(c.b)); }
float3 EncodeRGB(float3 c) { return float3(EncodeSDR(c.r), EncodeSDR(c.g), EncodeSDR(c.b)); }

float SignedCbrt(float x) {
    float ax = abs(x);
    return (ax < 1e-12) ? 0.0 : (sign(x) * pow(ax, 1.0 / 3.0));
}

float3 RGBToLab(float3 c) {
    float3 lin = DecodeRGB(c);
    float l = SignedCbrt(dot(lin, float3(0.4122214708, 0.5363325363, 0.0514459929)));
    float m = SignedCbrt(dot(lin, float3(0.2119034982, 0.6806995451, 0.1073969566)));
    float s = SignedCbrt(dot(lin, float3(0.0883024619, 0.2817188376, 0.6299787005)));
    return float3(
        0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
        1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
        0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s
    );
}

float3 LabToLinear(float3 c) {
    float3 v = float3(
        c.x + 0.3963377774 * c.y + 0.2158037573 * c.z,
        c.x - 0.1055613458 * c.y - 0.0638541728 * c.z,
        c.x - 0.0894841775 * c.y - 1.2914855480 * c.z
    );
    v = v * v * v;
    return float3(
        dot(v, float3( 4.0767416621, -3.3077115913,  0.2309699292)),
        dot(v, float3(-1.2684380046,  2.6097574011, -0.3413193965)),
        dot(v, float3(-0.0041960863, -0.7034186147,  1.7076147010))
    );
}

float CatmullRom(float x) {
    x = abs(x);
    if (x < 1.0) return ((1.5 * x - 2.5) * x) * x + 1.0;
    if (x < 2.0) return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
    return 0.0;
}

float3 ApplyResidualControls(float3 orig, float3 rawDiff) {
    if (ResidualStrength <= 0.001 || all(rawDiff == 0.0)) return 0.0;

    // 对齐 Magpie 中性直通路径：当残差微调处于默认中性值时，100% 无损保留 nvngx_dlssnr 原生神经光影与自然/鲜明/电影风格！
    bool isNeutral = abs(ResidualStrength - 1.0) < 0.002 &&
                     abs(ShadowStructure - 1.0) < 0.002 &&
                     abs(HighlightGlow - 1.0) < 0.002 &&
                     abs(Saturation - 1.0) < 0.002 &&
                     DarkProtection < 0.002 &&
                     HighlightProtection < 0.002;
    if (isNeutral) {
        return rawDiff;
    }

    float3 residual = rawDiff * ResidualStrength;
    float3 o = RGBToLab(orig);
    float3 d = RGBToLab(orig + residual) - o;

    d.x = min(d.x, 0.0) * ShadowStructure + max(d.x, 0.0) * HighlightGlow;
    d.yz *= Saturation;

    float dark = DarkProtection * (1.0 - smoothstep(0.06, 0.32, o.x));
    float bright = HighlightProtection * smoothstep(0.72, 0.98, o.x);
    float3 outward = max(EncodeRGB(LabToLinear(o + d)) - orig, 0.0);
    float headroom = max(outward.r / (1.0 - orig.r + 1e-4),
                     max(outward.g / (1.0 - orig.g + 1e-4), outward.b / (1.0 - orig.b + 1e-4)));
    bright *= lerp(0.5, 1.0, smoothstep(0.0, 0.02, d.x));
    bright = max(bright, HighlightProtection * smoothstep(0.72, 0.98, o.x) * smoothstep(0.5, 2.0, headroom));
    d *= (1.0 - dark) * (1.0 - bright);

    return EncodeRGB(LabToLinear(o + d)) - orig;
}

float3 UpsampleResidualCatmullRom(uint2 pixel) {
    if (ResidualStrength <= 0.001) return 0.0;
    if (all(SrcExtent == WorkExtent)) {
        int2 wp = clamp(int2(pixel), int2(0, 0), int2(WorkExtent) - 1);
        float3 o0 = WorksetIn.Load(int3(wp, 0)).rgb;
        float3 n0 = WorksetOut.Load(int3(wp, 0)).rgb;
        if (!all(isfinite(o0)) || !all(isfinite(n0))) return 0.0;
        return ApplyResidualControls(o0, n0 - o0);
    }
    float2 reducedPos = (float2(pixel) + 0.5) * float2(WorkExtent) * InvSrcExtent - 0.5;
    int2 center = int2(floor(reducedPos));
    float3 totalOrig = 0.0;
    float3 totalDiff = 0.0;
    float totalWeight = 0.0;
    [unroll] for (int dy = -1; dy <= 2; ++dy) {
        float wy = CatmullRom(reducedPos.y - float(center.y + dy));
        [unroll] for (int dx = -1; dx <= 2; ++dx) {
            int2 wp = clamp(center + int2(dx, dy), int2(0, 0), int2(WorkExtent) - 1);
            float3 oTap = WorksetIn.Load(int3(wp, 0)).rgb;
            float3 nTap = WorksetOut.Load(int3(wp, 0)).rgb;
            if (all(isfinite(oTap)) && all(isfinite(nTap))) {
                float w = CatmullRom(reducedPos.x - float(center.x + dx)) * wy;
                totalOrig += oTap * w;
                totalDiff += (nTap - oTap) * w;
                totalWeight += w;
            }
        }
    }
    float invW = abs(totalWeight) > 1e-6 ? (1.0 / totalWeight) : 0.0;
    return ApplyResidualControls(saturate(totalOrig * invW), totalDiff * invW);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= SrcExtent)) return;
    float2 uv = (float2(tid.xy) + 0.5) * InvSrcExtent;
    float3 curRes = UpsampleResidualCatmullRom(tid.xy);

    if (HasHistory != 0 && AntiFlickerMode > 0) {
        float2 mv = (AntiFlickerMode == 2) ? FwdMotion.Load(int3(tid.xy, 0)) : float2(0.0, 0.0);
        float2 prevUv = uv + mv * InvSrcExtent;
        if (all(prevUv >= 0.0) && all(prevUv <= 1.0)) {
            float3 prevRes = PrevResidualTex.SampleLevel(LinearClamp, prevUv, 0).rgb;
            float diffLen = length(curRes - prevRes);
            float matchWeight = exp(-diffLen * 18.0);
            float alpha = clamp(0.45 * matchWeight, 0.0, 0.50);
            curRes = lerp(curRes, prevRes, alpha);
        }
    }

    OutResidual[tid.xy] = float4(curRes, 1.0);
}
)hlsl";

// 5. 原生帧合成（像素级光度自检零残影异步残差叠回 + AMD CAS / FSR RCAS 硬件级低延迟去噪锐化）
static const char* HLSL_COMPOSE_AND_SHARPEN = R"hlsl(
Texture2D<float4> RawSrcTex        : register(t0);
Texture2D<float4> ResidualTex      : register(t1);
Texture2D<float2> FwdMotionTex     : register(t2);
Texture2D<float2> FlowConfTex      : register(t3);
Texture2D<float4> ResidualRefInTex : register(t4);
SamplerState LinearClamp           : register(s0);

RWTexture2D<float4> OutEnhanced : register(u0);

cbuffer ComposeCB : register(b0) {
    uint2  Extent;
    float2 InvExtent;
    uint   Dlss5Enabled;
    uint   AsyncWarpResidual;
    uint   SharpenMode;
    float  SharpenStrength;
    uint   SharpenDenoise;
    float3 Pad0;
};

float3 SampleEnhancedColor(int2 p) {
    p = clamp(p, int2(0, 0), int2(Extent) - 1);
    float3 baseCol = RawSrcTex.Load(int3(p, 0)).rgb;
    if (Dlss5Enabled == 0) return baseCol;

    float2 uv = (float2(p) + 0.5) * InvExtent;
    float3 res = 0.0;
    if (AsyncWarpResidual != 0) {
        float2 mv = FwdMotionTex.Load(int3(p, 0));
        float2 warpedUv = uv + mv * InvExtent;
        if (all(warpedUv >= 0.0) && all(warpedUv <= 1.0)) {
            // 像素级光度表面匹配校验（彻底取代会导致天空随机闪烁的 FlowConf 掩码）：
            // - 物体移动遮挡区：当前帧 baseCol 与产生残差的历史输入帧 ResidualRefInTex 颜色不一致 -> photoMatch 瞬间归 0，100% 消除物体移动残影！
            // - 天空/云雾/墙面：颜色一致 -> photoMatch 恒定为 1.0，且自动拒绝无纹理天空上的假光流偏移，100% 消除天空斑点闪烁！
            float3 refWarped = ResidualRefInTex.SampleLevel(LinearClamp, warpedUv, 0).rgb;
            float3 diffW = abs(baseCol - refWarped);
            float errWarped = max(diffW.r, max(diffW.g, diffW.b));

            float3 refStatic = ResidualRefInTex.SampleLevel(LinearClamp, uv, 0).rgb;
            float3 diffS = abs(baseCol - refStatic);
            float errStatic = max(diffS.r, max(diffS.g, diffS.b));

            float2 bestUv = (errStatic + 0.004 < errWarped) ? uv : warpedUv;
            float bestErr = min(errWarped, errStatic);
            float photoMatch = smoothstep(0.045, 0.010, bestErr);
            if (photoMatch > 0.002) {
                res = ResidualTex.SampleLevel(LinearClamp, bestUv, 0).rgb * photoMatch;
            }
        }
    } else {
        res = ResidualTex.Load(int3(p, 0)).rgb;
    }
    float3 combined = baseCol + res;
    if (!all(isfinite(combined))) return baseCol;
    return saturate(combined);
}

float LumaFast(float3 c) { return c.g + 0.5 * (c.r + c.b); }

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= Extent)) return;
    int2 p = int2(tid.xy);
    float3 e = SampleEnhancedColor(p);

    if (SharpenMode == 0 || SharpenStrength <= 0.001) {
        OutEnhanced[p] = float4(e, 1.0);
        return;
    }

    float3 b = SampleEnhancedColor(p + int2( 0, -1));
    float3 d = SampleEnhancedColor(p + int2(-1,  0));
    float3 f = SampleEnhancedColor(p + int2( 1,  0));
    float3 h = SampleEnhancedColor(p + int2( 0,  1));

    float bL = LumaFast(b), dL = LumaFast(d), eL = LumaFast(e), fL = LumaFast(f), hL = LumaFast(h);
    float rangeL = max(max(bL, dL), max(eL, max(fL, hL))) - min(min(bL, dL), min(eL, min(fL, hL)));
    float nz = (rangeL > 1e-4) ? saturate(abs(0.25 * (bL + dL + fL + hL) - eL) / rangeL) : 0.0;
    float denoiseAtten = (SharpenDenoise != 0) ? (1.0 - 0.55 * nz) : 1.0;

    if (SharpenMode == 1) {
        float3 a = SampleEnhancedColor(p + int2(-1, -1));
        float3 c = SampleEnhancedColor(p + int2( 1, -1));
        float3 g = SampleEnhancedColor(p + int2(-1,  1));
        float3 i = SampleEnhancedColor(p + int2( 1,  1));

        float3 mnRGB  = min(min(min(d, e), min(f, b)), h);
        float3 mnRGB2 = min(mnRGB, min(min(a, c), min(g, i)));
        mnRGB += mnRGB2;

        float3 mxRGB  = max(max(max(d, e), max(f, b)), h);
        float3 mxRGB2 = max(mxRGB, max(max(a, c), max(g, i)));
        mxRGB += mxRGB2;

        float3 rcpMRGB = rcp(max(mxRGB, 1e-4));
        float3 ampRGB = saturate(min(mnRGB, 2.0 - mxRGB) * rcpMRGB);
        ampRGB = sqrt(ampRGB);

        float peak = -rcp(lerp(8.0, 4.2, saturate(SharpenStrength * 0.65)));
        float3 wRGB = ampRGB * peak * denoiseAtten;
        float3 rcpWeight = rcp(1.0 + 4.0 * wRGB);
        float3 outCol = saturate(((b + d + f + h) * wRGB + e) * rcpWeight);
        OutEnhanced[p] = float4(outCol, 1.0);
    } else {
        float3 mn4 = min(min(b, d), min(f, h));
        float3 mx4 = max(max(b, d), max(f, h));
        float3 hitMin = mn4 / max(4.0 * mx4, 1e-4);
        float3 hitMax = (1.0 - mx4) / max(4.0 * mn4 - 4.0, -4.0);
        float3 lobeRGB = max(-hitMin, hitMax);
        float con = exp2(-clamp(2.0 - SharpenStrength * 1.15, 0.0, 2.5));
        float limit = 0.25 - (1.0 / 16.0);
        float lobe = max(-limit, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * con * denoiseAtten;
        float3 outCol = saturate((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0));
        OutEnhanced[p] = float4(outCol, 1.0);
    }
}
)hlsl";

// 6A. 零竞态光度自洽中点运动场汇聚着色器 (Race-Free Photometric Candidate Gather)：
// 彻底废除多线程随机覆盖写入的 CSSplat，通过双向候选矢量汇聚与光度匹配竞选，
// 既精准捕捉高速运动物体中点，又绝不允许快速旋转的背景运动矢量覆盖画面中心角色、武器或 UI！
static const char* HLSL_PROJECT_FLOW = R"hlsl(
Texture2D<float2> FwdMotion : register(t0);
Texture2D<float2> BwdMotion : register(t1);
Texture2D<float2> FlowConf  : register(t2);
Texture2D<float4> PrevFrame : register(t3);
Texture2D<float4> CurrFrame : register(t4);
SamplerState LinearClamp    : register(s0);

RWTexture2D<float2> OutProjFlow : register(u0);

cbuffer ProjCB : register(b0) {
    uint2  Extent;
    float2 InvExtent;
    uint   FgMode; // 0 = 外推 t = N + 0.5, 1 = 内插 t = N - 0.5
    float  Phase;
    uint   ExcludeCount;
    uint   Pad0;
    uint4  ExcludeRects[6];
};

bool IsInExcludeUi(int2 pt) {
    if (ExcludeCount == 0) return false;
    uint4 r0 = ExcludeRects[0];
    if (pt.x >= int(r0.x) && pt.y >= int(r0.y) && pt.x < int(r0.z) && pt.y < int(r0.w)) return true;
    if (ExcludeCount > 1) {
        uint4 r1 = ExcludeRects[1];
        if (pt.x >= int(r1.x) && pt.y >= int(r1.y) && pt.x < int(r1.z) && pt.y < int(r1.w)) return true;
    }
    if (ExcludeCount > 2) {
        uint4 r2 = ExcludeRects[2];
        if (pt.x >= int(r2.x) && pt.y >= int(r2.y) && pt.x < int(r2.z) && pt.y < int(r2.w)) return true;
    }
    return false;
}

float MaxDiff3(float3 a, float3 b) {
    float3 d = abs(a - b);
    return max(d.r, max(d.g, d.b));
}

float EvaluateFlowCandidate(float2 uv, float2 v, bool stationaryAnchored) {
    if (FgMode == 0) {
        float2 uvC = uv + v * Phase * InvExtent;
        if (any(uvC < 0.0) || any(uvC > 1.0)) return 999.0;
        float2 fAtC = FwdMotion.SampleLevel(LinearClamp, uvC, 0);
        float cAtC = FlowConf.SampleLevel(LinearClamp, uvC, 0).x;
        float vecErr = length(fAtC - v);
        return vecErr * 0.025 + (1.0 - cAtC) * 0.15;
    }

    float2 uvC = uv - v * Phase * InvExtent;
    float2 uvP = uv + v * Phase * InvExtent;
    if (any(uvC < 0.0) || any(uvC > 1.0) || any(uvP < 0.0) || any(uvP > 1.0)) return 999.0;

    float3 cC = CurrFrame.SampleLevel(LinearClamp, uvC, 0).rgb;
    float3 cP = PrevFrame.SampleLevel(LinearClamp, uvP, 0).rgb;
    float3 cCh = CurrFrame.SampleLevel(LinearClamp, clamp(uvC + float2(InvExtent.x * 2.0, 0.0), 0.0, 1.0), 0).rgb;
    float3 cPh = PrevFrame.SampleLevel(LinearClamp, clamp(uvP + float2(InvExtent.x * 2.0, 0.0), 0.0, 1.0), 0).rgb;

    float photoErr = 0.65 * MaxDiff3(cC, cP) + 0.35 * MaxDiff3(cCh, cPh);

    float2 fC = FwdMotion.SampleLevel(LinearClamp, uvC, 0);
    float2 vFromP = -BwdMotion.SampleLevel(LinearClamp, uvP, 0);
    float errC = length(fC - v);
    float errP = length(vFromP - v);
    float minVecErr = min(errC, errP);
    float maxVecErr = max(errC, errP);

    float cost = photoErr + 0.0035 * min(minVecErr, 28.0);

    // 若当前位置本身已经是前后两帧完全吻合的静止角色/武器/UI结构，严禁远处高速背景跨帧覆盖它！
    float mag = length(v);
    if (stationaryAnchored && mag > 3.5) {
        cost += 0.25;
    } else if (!stationaryAnchored && photoErr < 0.045 && maxVecErr < 4.5 && mag > 1.5) {
        // 对于真正跨过空背景的前景高速运动物体（两端矢量与颜色均高度吻合），赋予微弱前景优先权
        cost -= 0.012 * saturate(mag / 16.0);
    }
    return cost;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= Extent)) return;
    int2 p = int2(tid.xy);
    if (IsInExcludeUi(p)) {
        OutProjFlow[p] = float2(0.0, 0.0);
        return;
    }
    float2 uv = (float2(p) + 0.5) * InvExtent;

    float2 vLocalFwd = FwdMotion.Load(int3(p, 0));
    float2 vLocalBwd = -BwdMotion.Load(int3(p, 0));

    // 检测当前像素是否属于静止高结构实体（如第三人称中心人物、持枪手臂、HUD图标）
    float3 cc0 = CurrFrame.Load(int3(p, 0)).rgb;
    float3 cp0 = PrevFrame.Load(int3(p, 0)).rgb;
    float3 ccX = CurrFrame.Load(int3(clamp(p + int2(2, 0), int2(0,0), int2(Extent)-1), 0)).rgb;
    float3 cpX = PrevFrame.Load(int3(clamp(p + int2(2, 0), int2(0,0), int2(Extent)-1), 0)).rgb;
    float3 ccY = CurrFrame.Load(int3(clamp(p + int2(0, 2), int2(0,0), int2(Extent)-1), 0)).rgb;
    float3 cpY = PrevFrame.Load(int3(clamp(p + int2(0, 2), int2(0,0), int2(Extent)-1), 0)).rgb;

    float zeroDiff = max(MaxDiff3(cc0, cp0), max(MaxDiff3(ccX, cpX), MaxDiff3(ccY, cpY)));
    float localStruct = max(MaxDiff3(cc0, ccX), MaxDiff3(cc0, ccY));
    bool stationaryAnchored = (zeroDiff < 0.032) && (localStruct > 0.035) &&
                              (length(vLocalFwd) < 3.5) && (length(vLocalBwd) < 3.5);

    float2 bestV = float2(0.0, 0.0);
    float bestCost = EvaluateFlowCandidate(uv, bestV, stationaryAnchored);

    float c1 = EvaluateFlowCandidate(uv, vLocalFwd, stationaryAnchored);
    if (c1 < bestCost) { bestCost = c1; bestV = vLocalFwd; }

    float c2 = EvaluateFlowCandidate(uv, vLocalBwd, stationaryAnchored);
    if (c2 < bestCost) { bestCost = c2; bestV = vLocalBwd; }

    // 8 方向多尺度确定性汇聚搜索（覆盖最高 ±68px 剧烈视角移动，零线程竞态闪烁！）
    const int2 dirs[8] = {
        int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1),
        int2(1, 1), int2(-1, 1), int2(1, -1), int2(-1, -1)
    };
    const int radii[3] = { 8, 18, 32 };

    [unroll] for (int rIdx = 0; rIdx < 3; ++rIdx) {
        int rad = radii[rIdx];
        float acceptDist = float(rad) * 0.75 + 4.0;
        [unroll] for (int d = 0; d < 8; ++d) {
            int2 q = clamp(p + dirs[d] * rad, int2(0, 0), int2(Extent) - 1);

            // 检查 Curr[q] 投射回中点是否经过 p 附近（若采样源落在 F8 UI 面板内则直接跳过，防止把面板文字拉扯到车身上！）
            if (IsInExcludeUi(q)) continue;
            float2 vC = FwdMotion.Load(int3(q, 0));
            float2 midFromC = float2(q) + ((FgMode == 0) ? (-vC * Phase) : (vC * Phase));
            if (length(midFromC - float2(p)) <= acceptDist) {
                float costC = EvaluateFlowCandidate(uv, vC, stationaryAnchored);
                if (costC < bestCost - 0.004) {
                    bestCost = costC;
                    bestV = vC;
                }
            }

            // 检查 Prev[q] 投射到中点是否经过 p 附近
            if (FgMode != 0) {
                float2 vP = -BwdMotion.Load(int3(q, 0));
                float2 midFromP = float2(q) - vP * Phase;
                if (length(midFromP - float2(p)) <= acceptDist) {
                    float costP = EvaluateFlowCandidate(uv, vP, stationaryAnchored);
                    if (costP < bestCost - 0.004) {
                        bestCost = costP;
                        bestV = vP;
                    }
                }
            }
        }
    }

    OutProjFlow[p] = bestV;
}
)hlsl";

// 6B. 零闪烁模型边缘遮挡自洽修补 + 全彩抗压缩噪点 UI 锁定 X2 合成着色器
static const char* HLSL_FRAME_GEN_X2 = R"hlsl(
Texture2D<float4> PrevFrame   : register(t0);
Texture2D<float4> CurrFrame   : register(t1);
Texture2D<float2> ProjFlow    : register(t2);
Texture2D<float2> FwdMotion   : register(t3);
Texture2D<float2> BwdMotion   : register(t4);
Texture2D<float2> FlowConf    : register(t5);
SamplerState LinearClamp      : register(s0);

RWTexture2D<float4> OutGenFrame : register(u0);

cbuffer FrameGenCB : register(b0) {
    uint2  Extent;
    float2 InvExtent;
    uint   FgMode;            // 0 = 零等待外推预测 (N+0.5), 1 = 双向遮挡精插 (N-0.5)
    float  SceneCutThreshold;
    float  UiProtection;
    float  Phase;             // 0.5
};

float MaxDiff3(float3 a, float3 b) {
    float3 d = abs(a - b);
    return max(d.r, max(d.g, d.b));
}

float2 MedianOf5Flow(float2 c, float2 l, float2 r, float2 u, float2 d) {
    // 分量级 5 点中值滤波，彻底剔除模型轮廓线单像素毛刺，同时保持锐利运动边界
    float xs[5] = { c.x, l.x, r.x, u.x, d.x };
    float ys[5] = { c.y, l.y, r.y, u.y, d.y };
    [unroll] for (int i = 0; i < 4; ++i) {
        [unroll] for (int j = i + 1; j < 5; ++j) {
            if (xs[j] < xs[i]) { float t = xs[i]; xs[i] = xs[j]; xs[j] = t; }
            if (ys[j] < ys[i]) { float t = ys[i]; ys[i] = ys[j]; ys[j] = t; }
        }
    }
    return float2(xs[2], ys[2]);
}

// 全彩抗 H.264 量化蚊噪静态 UI / 血条 / 小地图 / 字幕 / 准星保护掩码 (0.0 ~ 1.0)
float ComputeStationaryUiLock(int2 p, float flowMag) {
    if (UiProtection <= 0.02) return 0.0;

    float3 c0 = CurrFrame.Load(int3(p, 0)).rgb;
    float3 p0 = PrevFrame.Load(int3(p, 0)).rgb;

    float meanZeroDiff = 0.0;
    float maxZeroDiff = 0.0;
    float localEdge = 0.0;
    float haloUiAnchor = 0.0;

    [unroll] for (int dy = -1; dy <= 1; ++dy) {
        [unroll] for (int dx = -1; dx <= 1; ++dx) {
            int2 q = clamp(p + int2(dx * 2, dy * 2), int2(0, 0), int2(Extent) - 1);
            float3 cq = CurrFrame.Load(int3(q, 0)).rgb;
            float3 pq = PrevFrame.Load(int3(q, 0)).rgb;
            float d = MaxDiff3(cq, pq);
            meanZeroDiff += d;
            maxZeroDiff = max(maxZeroDiff, d);
            localEdge = max(localEdge, MaxDiff3(cq, c0));
        }
    }
    meanZeroDiff *= (1.0 / 9.0);

    // 激烈转视角时 Xbox H.264 编码器 QP 升高，静止 UI 也会带 0.015~0.035 的块量化噪点，自适应放宽容差
    float noiseTol = (0.018 + 0.026 * UiProtection) + saturate(flowMag / 45.0) * 0.012;

    // 1. 当前 3x3 块本身在前后两帧完全静止（无论红血条、黑底框、白字幕、彩色图标全部覆盖！）
    float directLock = (1.0 - smoothstep(noiseTol * 0.65, noiseTol * 1.25, meanZeroDiff)) *
                       (1.0 - smoothstep(noiseTol * 1.35, noiseTol * 2.20, maxZeroDiff));

    // 若像素是均匀无纹理背景且存在跨背景运动，避免把纯色天空误锁死为 UI；但若本身就是低流静止区则直接锁定
    if (localEdge < 0.035 && flowMag > 2.5) {
        directLock *= 0.25;
    }

    // 2. 保护 UI 字体/图标外围 3px 抗锯齿描边光晕（防止背景高速划过时啃噬字体边缘造成闪烁）
    if (flowMag > 2.5 && UiProtection > 0.25) {
        const int2 haloOffs[4] = { int2(3, 0), int2(-3, 0), int2(0, 3), int2(0, -3) };
        [unroll] for (int k = 0; k < 4; ++k) {
            int2 hq = clamp(p + haloOffs[k], int2(0, 0), int2(Extent) - 1);
            float3 ch = CurrFrame.Load(int3(hq, 0)).rgb;
            float3 ph = PrevFrame.Load(int3(hq, 0)).rgb;
            int2 hq2 = clamp(hq + int2(2, 0), int2(0, 0), int2(Extent) - 1);
            float3 ch2 = CurrFrame.Load(int3(hq2, 0)).rgb;
            float hd = MaxDiff3(ch, ph);
            float hContrast = MaxDiff3(ch, ch2);
            if (hd < noiseTol * 0.75 && hContrast > 0.14 && MaxDiff3(c0, p0) < noiseTol * 1.6) {
                haloUiAnchor = max(haloUiAnchor, 0.88 * UiProtection);
            }
        }
    }

    return saturate(max(directLock, haloUiAnchor));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= Extent)) return;
    int2 p = int2(tid.xy);
    float2 uv = (float2(p) + 0.5) * InvExtent;

    float3 colCurr = CurrFrame.Load(int3(p, 0)).rgb;
    float3 colPrev = PrevFrame.Load(int3(p, 0)).rgb;

    // 5 点中值滤波去除模型轮廓边缘杂散矢量毛刺
    float2 fwdC = ProjFlow.Load(int3(p, 0));
    float2 fwdL = ProjFlow.Load(int3(clamp(p + int2(-2, 0), int2(0,0), int2(Extent)-1), 0));
    float2 fwdR = ProjFlow.Load(int3(clamp(p + int2( 2, 0), int2(0,0), int2(Extent)-1), 0));
    float2 fwdU = ProjFlow.Load(int3(clamp(p + int2(0, -2), int2(0,0), int2(Extent)-1), 0));
    float2 fwdD = ProjFlow.Load(int3(clamp(p + int2(0,  2), int2(0,0), int2(Extent)-1), 0));
    float2 fwd = MedianOf5Flow(fwdC, fwdL, fwdR, fwdU, fwdD);

    float flowMag = length(fwd);
    if (flowMag < 0.45 && MaxDiff3(colCurr, colPrev) < 0.015) {
        OutGenFrame[p] = float4(colCurr, 1.0);
        return;
    }

    // 计算全彩抗噪静态 UI 保护权重
    float uiLock = ComputeStationaryUiLock(p, flowMag);
    if (uiLock >= 0.98) {
        OutGenFrame[p] = float4(colCurr, 1.0);
        return;
    }

    float3 warped = colCurr;
    float fallbackToCurr = 0.0;

    if (FgMode == 0) {
        // ⚡ Mode 0: 零等待外推预测 X2 (t = N + 0.5, 真实帧 0ms 扣押)
        float2 sampleUv = clamp(uv + fwd * Phase * InvExtent, 0.0, 1.0);
        float2 fAtSrc = FwdMotion.SampleLevel(LinearClamp, sampleUv, 0);
        float confAtSrc = FlowConf.SampleLevel(LinearClamp, sampleUv, 0).x;
        float vecErr = length(fAtSrc - fwd);
        float validWeight = smoothstep(0.12, 0.45, confAtSrc) * (1.0 - smoothstep(6.0, 18.0, vecErr));
        float2 safeUv = clamp(uv + fwd * (Phase * validWeight) * InvExtent, 0.0, 1.0);
        warped = CurrFrame.SampleLevel(LinearClamp, safeUv, 0).rgb;
        fallbackToCurr = smoothstep(55.0, 100.0, flowMag) * (1.0 - validWeight);
    } else {
        // 🎯 Mode 1: 双向矢量自洽遮挡修补内插 X2 (t = N - 0.5)
        float2 uvCurr = clamp(uv - fwd * Phase * InvExtent, 0.0, 1.0);
        float2 uvPrev = clamp(uv + fwd * Phase * InvExtent, 0.0, 1.0);

        float3 sCurr = CurrFrame.SampleLevel(LinearClamp, uvCurr, 0).rgb;
        float3 sPrev = PrevFrame.SampleLevel(LinearClamp, uvPrev, 0).rgb;

        // 读取各自真实坐标系下的光流矢量与可见置信度，精准判定单侧遮挡！
        // 模型后缘新露出区：Curr[uvCurr] 光流自洽 (vecErrCurr≈0)，Prev[uvPrev] 仍是刚离开的模型 (vecErrPrev 很大) -> 平滑选 sCurr！
        // 模型前缘被遮挡区：Prev[uvPrev] 光流自洽 (vecErrPrev≈0)，Curr[uvCurr] 已变成闯入的模型 (vecErrCurr 很大) -> 平滑选 sPrev！
        float2 fAtCurr = FwdMotion.SampleLevel(LinearClamp, uvCurr, 0);
        float2 vAtPrev = -BwdMotion.SampleLevel(LinearClamp, uvPrev, 0);
        float vecErrCurr = length(fAtCurr - fwd);
        float vecErrPrev = length(vAtPrev - fwd);

        float confCurr = FlowConf.SampleLevel(LinearClamp, uvCurr, 0).x;
        float confPrev = FlowConf.SampleLevel(LinearClamp, uvPrev, 0).y;

        float scoreCurr = confCurr - 0.085 * vecErrCurr;
        float scorePrev = confPrev - 0.085 * vecErrPrev;

        // 连续平滑权重（绝不使用 >= 二值跳变，彻底根除角色轮廓碎裂与 30Hz 闪烁！）
        float colorDiff = MaxDiff3(sCurr, sPrev);
        float occSelect = smoothstep(-0.32, 0.32, scoreCurr - scorePrev);
        float blend = lerp(0.5, occSelect, smoothstep(0.035, 0.11, colorDiff));
        warped = lerp(sPrev, sCurr, blend);

        // 激烈甩视角超高速防崩安全阀：若两端矢量同时失配且色差巨大（爆炸粒子/超 70px 甩镜头），平滑回退至真实帧杜绝破面闪烁
        float minVecErr = min(vecErrCurr, vecErrPrev);
        float maxConf = max(confCurr, confPrev);
        float chaosUnreliable = smoothstep(7.0, 20.0, minVecErr) * smoothstep(0.10, 0.24, colorDiff);
        float extremeWhip = smoothstep(60.0, 110.0, flowMag) * (1.0 - maxConf * 0.75);
        fallbackToCurr = saturate(max(chaosUnreliable, extremeWhip));
    }

    float3 result = lerp(warped, colCurr, max(uiLock, fallbackToCurr));
    OutGenFrame[p] = float4(saturate(result), 1.0);
}
)hlsl";

// 7. 超低延迟全屏三角形顶点与分屏对比像素着色器
static const char* HLSL_PRESENT_VS = R"hlsl(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
)hlsl";

static const char* HLSL_PRESENT_PS = R"hlsl(
Texture2D<float4> FinalTex : register(t0);
Texture2D<float4> RawTex   : register(t1);
SamplerState LinearClamp   : register(s0);

cbuffer PresentCB : register(b0) {
    float SplitPos;
    float InvWidth;
    uint  ExcludeCount;
    uint  Pad0;
    uint4 ExcludeRects[6];
};

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float4 PSMain(VSOut i) : SV_Target {
    int2 px = int2(i.pos.xy);
    // GPU 像素级 UI 面板原生直通：解决 Windows 11 FLIP_DISCARD 交换链忽略 GDI SetWindowRgn 导致 F8 调参面板被光流拉扯模糊的问题！
    [unroll] for (uint k = 0; k < 6; ++k) {
        if (k < ExcludeCount) {
            uint4 r = ExcludeRects[k];
            if (px.x >= int(r.x) && px.y >= int(r.y) && px.x < int(r.z) && px.y < int(r.w)) {
                return float4(RawTex.Load(int3(px, 0)).rgb, 1.0);
            }
        }
    }
    if (SplitPos >= 0.999) {
        return float4(RawTex.SampleLevel(LinearClamp, i.uv, 0).rgb, 1.0);
    }
    if (SplitPos > 0.001) {
        if (abs(i.uv.x - SplitPos) < InvWidth * 1.5) {
            return float4(0.063, 0.486, 0.063, 1.0);
        }
        if (i.uv.x < SplitPos) {
            return float4(RawTex.SampleLevel(LinearClamp, i.uv, 0).rgb, 1.0);
        }
    }
    return float4(FinalTex.SampleLevel(LinearClamp, i.uv, 0).rgb, 1.0);
}
)hlsl";

} // namespace XShaders
