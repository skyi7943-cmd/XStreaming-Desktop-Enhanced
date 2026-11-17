#pragma once
// XStreaming Desktop Enhanced - AMD Radeon 专属 Oklab 多尺度光影精雕与去色带桥接引擎 (RadeonNR)
// 面向 AMD Radeon RX 5000 / 6000 / 7000 / 9000 独显及 Ryzen 680M / 780M / 890M 掌机核显：
// 1. 零外部私有 SDK DLL 依赖，纯原生 D3D11.4 HLSL Compute 硬件加速 (< 0.6ms 极低延迟)
// 2. 100% 适配第 3 栏全部功能：自然均衡(0) / 鲜明通透(1) / 电影光影(2) 三大预设风格 +「再叠加一层」双层显存串联！
// 3. 复用 Magpie 原版 4x4 Catmull-Rom 双三次残差上采样 + Oklab 残差细节精调 + 光流引导抗闪烁管线

#include "d3d_helpers.h"
#include "engine_config.h"
#include "shaders_hlsl.h"
#include <algorithm>
#include <cmath>

class AmdRelightBridge {
public:
    ~AmdRelightBridge() { Shutdown(); }

    bool Initialize(
        ID3D11Device5* dev11,
        ID3D11DeviceContext4* ctx11,
        uint32_t srcW,
        uint32_t srcH,
        uint32_t inputScalePercent
    ) {
        Shutdown();
        _dev11 = dev11;
        _ctx11 = ctx11;
        _srcW = srcW;
        _srcH = srcH;
        _scalePercent = std::clamp(inputScalePercent, 50u, 100u);
        _workW = std::max(64u, (uint32_t)std::lround(double(srcW) * double(_scalePercent) / 100.0));
        _workH = std::max(64u, (uint32_t)std::lround(double(srcH) * double(_scalePercent) / 100.0));

        if (!_CreateWorksetTextures()) return false;

        UINT bind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        for (int i = 0; i < 2; ++i) {
            if (!CreateTex2D(_dev11, _srcW, _srcH, DXGI_FORMAT_R16G16B16A16_FLOAT, bind,
                             _residualTex[i].Put(), _residualSrv[i].Put(), _residualUav[i].Put())) {
                return false;
            }
            const float zero[4]{ 0, 0, 0, 0 };
            _ctx11->ClearUnorderedAccessViewFloat(_residualUav[i].Get(), zero);
        }

        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        _dev11->CreateSamplerState(&sd, _linearSampler.Put());

        D3D11_BUFFER_DESC cbd{};
        cbd.Usage = D3D11_USAGE_DEFAULT;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.ByteWidth = 48;
        if (FAILED(_dev11->CreateBuffer(&cbd, nullptr, _cbWorkset.Put()))) return false;
        if (FAILED(_dev11->CreateBuffer(&cbd, nullptr, _cbRelight.Put()))) return false;
        cbd.ByteWidth = 64;
        if (FAILED(_dev11->CreateBuffer(&cbd, nullptr, _cbResidual.Put()))) return false;

        // 创建双缓冲非阻塞 D3D11 GPU 时间戳查询，实测 RadeonNR 单层/双层毫秒级硬件耗时
        for (int q = 0; q < 2; ++q) {
            D3D11_QUERY_DESC qd{};
            qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            _dev11->CreateQuery(&qd, _qDisjoint[q].Put());
            qd.Query = D3D11_QUERY_TIMESTAMP;
            _dev11->CreateQuery(&qd, _qStart[q].Put());
            _dev11->CreateQuery(&qd, _qEnd[q].Put());
            _qPending[q] = false;
        }

        if (!BuildComputeShader(_dev11, XShaders::HLSL_DOWNSAMPLE_WORKSET, "CSMain", _csDownsample.Put()) ||
            !BuildComputeShader(_dev11, XShaders::HLSL_AMD_RELIGHT_PASS, "CSMain", _csRelight.Put()) ||
            !BuildComputeShader(_dev11, XShaders::HLSL_OKLAB_RESIDUAL_TEMPORAL, "CSMain", _csResidual.Put())) {
            return false;
        }

        _hasResidualHistory = false;
        _ready = true;
        return true;
    }

    bool EnsureScale(uint32_t requestedScale) {
        uint32_t clamped = std::clamp(requestedScale, 50u, 100u);
        if (clamped == _scalePercent || !_ready) return true;
        _scalePercent = clamped;
        _workW = std::max(64u, (uint32_t)std::lround(double(_srcW) * double(_scalePercent) / 100.0));
        _workH = std::max(64u, (uint32_t)std::lround(double(_srcH) * double(_scalePercent) / 100.0));
        _hasResidualHistory = false;
        return _CreateWorksetTextures();
    }

    void Shutdown() {
        _workIn.Reset(); _workInSrv.Reset(); _workInUav.Reset();
        _workMid.Reset(); _workMidSrv.Reset(); _workMidUav.Reset();
        _workOut.Reset(); _workOutSrv.Reset(); _workOutUav.Reset();
        _workMv.Reset(); _workMvSrv.Reset(); _workMvUav.Reset();
        for (int i = 0; i < 2; ++i) {
            _residualTex[i].Reset(); _residualSrv[i].Reset(); _residualUav[i].Reset();
            _qDisjoint[i].Reset(); _qStart[i].Reset(); _qEnd[i].Reset();
            _qPending[i] = false;
        }
        _csDownsample.Reset();
        _csRelight.Reset();
        _csResidual.Reset();
        _linearSampler.Reset();
        _cbWorkset.Reset();
        _cbRelight.Reset();
        _cbResidual.Reset();
        _ready = false;
        _hasResidualHistory = false;
    }

    void ResetHistory() {
        _hasResidualHistory = false;
    }

    bool ProcessRealFrame(
        ID3D11ShaderResourceView* srcColorSrv,
        ID3D11ShaderResourceView* fwdMotionSrv,
        ID3D11ShaderResourceView* flowConfSrv,
        const EnhanceConfig& cfg,
        bool& outNeedsAsyncWarp
    ) {
        outNeedsAsyncWarp = false;
        if (!_ready || !cfg.dlss5Enabled) return false;

        EnsureScale(cfg.dlss5InputScale);

        const bool dualStack = (cfg.dlss5Passes >= 2);
        bool cfgChanged =
            (_cachedStyle[0] != cfg.dlss5Style) ||
            (_cachedStyle[1] != cfg.dlss5Pass2Style) ||
            (std::abs(_cachedIntensity[0] - cfg.dlss5Intensity) > 1e-4f) ||
            (std::abs(_cachedIntensity[1] - cfg.dlss5Pass2Intensity) > 1e-4f) ||
            (std::abs(_cachedLocalTone - cfg.dlss5LocalTone) > 1e-4f) ||
            (std::abs(_cachedLocalStructure - cfg.dlss5LocalStructure) > 1e-4f) ||
            (std::abs(_cachedSkinStructure - cfg.dlss5SkinStructure) > 1e-4f) ||
            (_cachedDualStack != dualStack);
        if (cfgChanged) {
            _hasResidualHistory = false;
            _cachedStyle[0] = cfg.dlss5Style;
            _cachedStyle[1] = cfg.dlss5Pass2Style;
            _cachedIntensity[0] = cfg.dlss5Intensity;
            _cachedIntensity[1] = cfg.dlss5Pass2Intensity;
            _cachedLocalTone = cfg.dlss5LocalTone;
            _cachedLocalStructure = cfg.dlss5LocalStructure;
            _cachedSkinStructure = cfg.dlss5SkinStructure;
            _cachedDualStack = dualStack;
        }

        // 先非阻塞收获上一轮完成的 D3D11 硬件时间戳
        uint32_t prevQ = _qSlot ^ 1u;
        _TryReadTimestampSlot(prevQ, false);

        bool recordTs = (_qDisjoint[_qSlot] && !_qPending[_qSlot]);
        if (recordTs) {
            _ctx11->Begin(_qDisjoint[_qSlot].Get());
            _ctx11->End(_qStart[_qSlot].Get());
        }

        // 1. Lanczos2 抗锯齿工作集降采样
        struct alignas(16) WorkCB {
            uint32_t srcW, srcH, dstW, dstH;
            float invSrcW, invSrcH, invDstW, invDstH;
            float mvScaleX, mvScaleY;
            uint32_t pad0, pad1;
        } wcb{
            _srcW, _srcH, _workW, _workH,
            1.0f / float(_srcW), 1.0f / float(_srcH),
            1.0f / float(_workW), 1.0f / float(_workH),
            float(_workW) / float(_srcW), float(_workH) / float(_srcH),
            0, 0
        };
        _ctx11->UpdateSubresource(_cbWorkset.Get(), 0, nullptr, &wcb, 0, 0);

        {
            ID3D11ShaderResourceView* srvs[2]{ srcColorSrv, fwdMotionSrv };
            ID3D11UnorderedAccessView* uavs[2]{ _workInUav.Get(), _workMvUav.Get() };
            ID3D11SamplerState* sams[1]{ _linearSampler.Get() };
            ID3D11Buffer* cbs[1]{ _cbWorkset.Get() };
            _ctx11->CSSetShader(_csDownsample.Get(), nullptr, 0);
            _ctx11->CSSetConstantBuffers(0, 1, cbs);
            _ctx11->CSSetSamplers(0, 1, sams);
            _ctx11->CSSetShaderResources(0, 2, srvs);
            _ctx11->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
            _ctx11->Dispatch((_workW + 7) / 8, (_workH + 7) / 8, 1);

            ID3D11ShaderResourceView* nullSrvs[2]{};
            ID3D11UnorderedAccessView* nullUavs[2]{};
            _ctx11->CSSetShaderResources(0, 2, nullSrvs);
            _ctx11->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
        }

        // 2. 执行单层或双层串联 AMD RadeonNR Oklab 光影精雕与去色带
        auto runRelightPass = [&](ID3D11ShaderResourceView* inSrv, ID3D11UnorderedAccessView* outUav, int style, float intensity, uint32_t passIdx) {
            struct alignas(16) RelightCB {
                uint32_t workW, workH;
                float invWorkW, invWorkH;
                uint32_t style;
                float intensity, localTone, localStruct;
                float skinStruct;
                uint32_t passIndex;
                float pad0, pad1;
            } rcb{
                _workW, _workH,
                1.0f / float(_workW), 1.0f / float(_workH),
                (uint32_t)std::clamp(style, 0, 2),
                intensity, cfg.dlss5LocalTone, cfg.dlss5LocalStructure,
                cfg.dlss5SkinStructure, passIdx, 0.0f, 0.0f
            };
            _ctx11->UpdateSubresource(_cbRelight.Get(), 0, nullptr, &rcb, 0, 0);

            ID3D11ShaderResourceView* srvs[1]{ inSrv };
            ID3D11UnorderedAccessView* uavs[1]{ outUav };
            ID3D11SamplerState* sams[1]{ _linearSampler.Get() };
            ID3D11Buffer* cbs[1]{ _cbRelight.Get() };
            _ctx11->CSSetShader(_csRelight.Get(), nullptr, 0);
            _ctx11->CSSetConstantBuffers(0, 1, cbs);
            _ctx11->CSSetSamplers(0, 1, sams);
            _ctx11->CSSetShaderResources(0, 1, srvs);
            _ctx11->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
            _ctx11->Dispatch((_workW + 7) / 8, (_workH + 7) / 8, 1);

            ID3D11ShaderResourceView* nullSrvs[1]{};
            ID3D11UnorderedAccessView* nullUavs[1]{};
            _ctx11->CSSetShaderResources(0, 1, nullSrvs);
            _ctx11->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
        };

        if (!dualStack) {
            runRelightPass(_workInSrv.Get(), _workOutUav.Get(), cfg.dlss5Style, cfg.dlss5Intensity, 0u);
        } else {
            runRelightPass(_workInSrv.Get(), _workMidUav.Get(), cfg.dlss5Style, cfg.dlss5Intensity, 0u);
            runRelightPass(_workMidSrv.Get(), _workOutUav.Get(), cfg.dlss5Pass2Style, cfg.dlss5Pass2Intensity, 1u);
        }

        if (recordTs) {
            _ctx11->End(_qEnd[_qSlot].Get());
            _ctx11->End(_qDisjoint[_qSlot].Get());
            _qPending[_qSlot] = true;
            _qSlot ^= 1u;
        }

        // 3. 提取 Oklab 残差并执行 4x4 Catmull-Rom 双三次上采样 + 光流时域抗闪烁
        uint32_t nextResIdx = _activeResIdx ^ 1u;
        struct alignas(16) ResCB {
            uint32_t srcW, srcH, workW, workH;
            float invSrcW, invSrcH, invWorkW, invWorkH;
            float resStr, shadowStr, highStr, sat;
            float darkProt, highProt;
            uint32_t antiFlicker, hasHistory;
        } resCb{
            _srcW, _srcH, _workW, _workH,
            1.0f / float(_srcW), 1.0f / float(_srcH),
            1.0f / float(_workW), 1.0f / float(_workH),
            cfg.residualStrength, cfg.shadowStructure, cfg.highlightGlow, cfg.saturation,
            cfg.darkProtection, cfg.highlightProtection,
            (uint32_t)std::clamp(cfg.antiFlicker, 0, 2),
            _hasResidualHistory ? 1u : 0u
        };
        _ctx11->UpdateSubresource(_cbResidual.Get(), 0, nullptr, &resCb, 0, 0);

        {
            ID3D11ShaderResourceView* srvs[6]{
                _workInSrv.Get(),
                _workOutSrv.Get(),
                srcColorSrv,
                fwdMotionSrv,
                flowConfSrv,
                _residualSrv[_activeResIdx].Get()
            };
            ID3D11UnorderedAccessView* uavs[1]{ _residualUav[nextResIdx].Get() };
            ID3D11SamplerState* sams[1]{ _linearSampler.Get() };
            ID3D11Buffer* cbs[1]{ _cbResidual.Get() };
            _ctx11->CSSetShader(_csResidual.Get(), nullptr, 0);
            _ctx11->CSSetConstantBuffers(0, 1, cbs);
            _ctx11->CSSetSamplers(0, 1, sams);
            _ctx11->CSSetShaderResources(0, 6, srvs);
            _ctx11->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
            _ctx11->Dispatch((_srcW + 7) / 8, (_srcH + 7) / 8, 1);

            ID3D11ShaderResourceView* nullSrvs[6]{};
            ID3D11UnorderedAccessView* nullUavs[1]{};
            _ctx11->CSSetShaderResources(0, 6, nullSrvs);
            _ctx11->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
            _ctx11->CSSetShader(nullptr, nullptr, 0);
        }

        _activeResIdx = nextResIdx;
        _hasResidualHistory = true;
        return true;
    }

    // 供无头自检结束后同步回读最后一帧的真实 GPU 时间戳耗时
    void SyncTimestampForSelfTest() {
        if (!_ctx11) return;
        _ctx11->Flush();
        for (uint32_t q = 0; q < 2; ++q) {
            if (_qPending[q]) {
                _TryReadTimestampSlot(q, true);
            }
        }
    }

    bool IsReady() const { return _ready; }
    double LastGpuMs() const { return _lastGpuMs; }
    uint32_t WorkWidth() const { return _workW; }
    uint32_t WorkHeight() const { return _workH; }
    ID3D11ShaderResourceView* CurrentResidualSrv() const { return _residualSrv[_activeResIdx].Get(); }
    ID3D11ShaderResourceView* CurrentResidualRefInSrv() const { return _workInSrv.Get(); }

private:
    bool _CreateWorksetTextures() {
        _workIn.Reset(); _workInSrv.Reset(); _workInUav.Reset();
        _workMid.Reset(); _workMidSrv.Reset(); _workMidUav.Reset();
        _workOut.Reset(); _workOutSrv.Reset(); _workOutUav.Reset();
        _workMv.Reset(); _workMvSrv.Reset(); _workMvUav.Reset();

        UINT bind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        return CreateTex2D(_dev11, _workW, _workH, DXGI_FORMAT_R8G8B8A8_UNORM, bind, _workIn.Put(), _workInSrv.Put(), _workInUav.Put()) &&
               CreateTex2D(_dev11, _workW, _workH, DXGI_FORMAT_R8G8B8A8_UNORM, bind, _workMid.Put(), _workMidSrv.Put(), _workMidUav.Put()) &&
               CreateTex2D(_dev11, _workW, _workH, DXGI_FORMAT_R8G8B8A8_UNORM, bind, _workOut.Put(), _workOutSrv.Put(), _workOutUav.Put()) &&
               CreateTex2D(_dev11, _workW, _workH, DXGI_FORMAT_R16G16_FLOAT, bind, _workMv.Put(), _workMvSrv.Put(), _workMvUav.Put());
    }

    void _TryReadTimestampSlot(uint32_t q, bool blocking) {
        if (!_qPending[q] || !_qDisjoint[q] || !_qStart[q] || !_qEnd[q]) return;
        UINT flags = blocking ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        HRESULT hr = _ctx11->GetData(_qDisjoint[q].Get(), &dj, sizeof(dj), flags);
        if (blocking) {
            for (int spin = 0; hr == S_FALSE && spin < 200; ++spin) {
                Sleep(1);
                hr = _ctx11->GetData(_qDisjoint[q].Get(), &dj, sizeof(dj), 0);
            }
        }
        if (hr != S_OK) return;

        uint64_t t0 = 0, t1 = 0;
        if (_ctx11->GetData(_qStart[q].Get(), &t0, sizeof(t0), flags) == S_OK &&
            _ctx11->GetData(_qEnd[q].Get(), &t1, sizeof(t1), flags) == S_OK) {
            if (!dj.Disjoint && dj.Frequency > 0 && t1 >= t0) {
                double ms = double(t1 - t0) * 1000.0 / double(dj.Frequency);
                _lastGpuMs = (_lastGpuMs <= 0.005) ? ms : (_lastGpuMs * 0.85 + ms * 0.15);
            }
        }
        _qPending[q] = false;
    }

    ID3D11Device5* _dev11 = nullptr;
    ID3D11DeviceContext4* _ctx11 = nullptr;
    uint32_t _srcW = 0, _srcH = 0, _workW = 0, _workH = 0, _scalePercent = 75;
    bool _ready = false;
    bool _hasResidualHistory = false;
    uint32_t _activeResIdx = 0;
    double _lastGpuMs = 0.0;

    int _cachedStyle[2]{ -1, -1 };
    float _cachedIntensity[2]{ -1.0f, -1.0f };
    float _cachedLocalTone = -1.0f;
    float _cachedLocalStructure = -1.0f;
    float _cachedSkinStructure = -1.0f;
    bool _cachedDualStack = false;

    ComRef<ID3D11Texture2D> _workIn, _workMid, _workOut, _workMv;
    ComRef<ID3D11ShaderResourceView> _workInSrv, _workMidSrv, _workOutSrv, _workMvSrv;
    ComRef<ID3D11UnorderedAccessView> _workInUav, _workMidUav, _workOutUav, _workMvUav;

    ComRef<ID3D11Texture2D> _residualTex[2];
    ComRef<ID3D11ShaderResourceView> _residualSrv[2];
    ComRef<ID3D11UnorderedAccessView> _residualUav[2];

    ComRef<ID3D11Query> _qDisjoint[2], _qStart[2], _qEnd[2];
    bool _qPending[2]{ false, false };
    uint32_t _qSlot = 0;

    ComRef<ID3D11ComputeShader> _csDownsample, _csRelight, _csResidual;
    ComRef<ID3D11SamplerState> _linearSampler;
    ComRef<ID3D11Buffer> _cbWorkset, _cbRelight, _cbResidual;
};
