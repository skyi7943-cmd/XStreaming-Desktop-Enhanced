#pragma once
// XStreaming Desktop Enhanced - NVIDIA 第 4 代硬件光流加速器 (NVOFA) 与通用计算着色器双栈光流服务
// 优先调用 RTX 独立硬件光流单元 (nvofapi64.dll，< 0.35ms 零抢占 Tensor Core)，不可用时自动无感回退 CS 光流

#include "d3d_helpers.h"
#include "shaders_hlsl.h"
#include "../third_party/include/nvOpticalFlowCommon.h"
#include "../third_party/include/nvOpticalFlowD3D11.h"

class OpticalFlowEngine {
public:
    ~OpticalFlowEngine() { Shutdown(); }

    bool Initialize(ID3D11Device5* dev, ID3D11DeviceContext4* ctx, uint32_t w, uint32_t h, bool isNvidia) {
        Shutdown();
        _dev = dev;
        _ctx = ctx;
        _width = w;
        _height = h;

        // 创建稠密光流与置信度纹理
        UINT bind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (!CreateTex2D(dev, w, h, DXGI_FORMAT_R16G16_FLOAT, bind, _fwdMotion.Put(), _fwdMotionSrv.Put(), _fwdMotionUav.Put()) ||
            !CreateTex2D(dev, w, h, DXGI_FORMAT_R16G16_FLOAT, bind, _bwdMotion.Put(), _bwdMotionSrv.Put(), _bwdMotionUav.Put()) ||
            !CreateTex2D(dev, w, h, DXGI_FORMAT_R8G8_UNORM, bind, _conf.Put(), _confSrv.Put(), _confUav.Put())) {
            return false;
        }

        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        dev->CreateSamplerState(&sd, _linearSampler.Put());

        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = 32;
        cbd.Usage = D3D11_USAGE_DEFAULT;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        dev->CreateBuffer(&cbd, nullptr, _cb.Put());

        if (isNvidia && _InitNvofa(w, h)) {
            _usingNvofa = true;
        } else {
            _usingNvofa = false;
            if (!BuildComputeShader(dev, XShaders::HLSL_FALLBACK_FLOW, "CSMain", _fallbackCs.Put())) {
                return false;
            }
            if (!CreateTex2D(dev, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, _prevFallbackTex.Put(), _prevFallbackSrv.Put())) {
                return false;
            }
        }

        ClearOutputs();
        _hasPrev = false;
        return true;
    }

    void Shutdown() {
        if (_nvofSession && _api.nvOFUnregisterResourceD3D11) {
            for (auto& h : _regHandles) {
                if (h) { _api.nvOFUnregisterResourceD3D11(h); h = nullptr; }
            }
        }
        if (_nvofSession && _api.nvOFDestroy) {
            _api.nvOFDestroy(_nvofSession);
            _nvofSession = nullptr;
        }
        _api = {};
        if (_nvofModule) {
            FreeLibrary(_nvofModule);
            _nvofModule = nullptr;
        }
        for (auto& t : _nvofInput) t.Reset();
        for (auto& t : _nvofFlow) t.Reset();
        for (auto& s : _nvofFlowSrv) s.Reset();
        for (auto& t : _nvofCost) t.Reset();
        for (auto& s : _nvofCostSrv) s.Reset();
        _densifyCs.Reset();
        _fallbackCs.Reset();
        _prevFallbackTex.Reset();
        _prevFallbackSrv.Reset();
        _fwdMotion.Reset(); _fwdMotionSrv.Reset(); _fwdMotionUav.Reset();
        _bwdMotion.Reset(); _bwdMotionSrv.Reset(); _bwdMotionUav.Reset();
        _conf.Reset(); _confSrv.Reset(); _confUav.Reset();
        _linearSampler.Reset();
        _cb.Reset();
        _usingNvofa = false;
        _hasPrev = false;
    }

    void ClearOutputs() {
        if (!_ctx) return;
        const float zero[4]{ 0, 0, 0, 0 };
        if (_fwdMotionUav) _ctx->ClearUnorderedAccessViewFloat(_fwdMotionUav.Get(), zero);
        if (_bwdMotionUav) _ctx->ClearUnorderedAccessViewFloat(_bwdMotionUav.Get(), zero);
        if (_confUav) _ctx->ClearUnorderedAccessViewFloat(_confUav.Get(), zero);
    }

    void ResetHistory() {
        _hasPrev = false;
        ClearOutputs();
    }

    // 输入当前帧 BGRA8 纹理，计算 Curr->Prev 与 Prev->Curr 光流及置信度
    bool Compute(ID3D11Texture2D* currBgra, ID3D11ShaderResourceView* currSrv) {
        if (_usingNvofa) {
            uint32_t curSlot = _slot ^ 1;
            _ctx->CopyResource(_nvofInput[curSlot].Get(), currBgra);
            if (!_hasPrev) {
                _slot = curSlot;
                _hasPrev = true;
                ClearOutputs();
                return true;
            }

            // 前向光流：input = Curr, reference = Prev (得到 Curr -> Prev 位移)
            NV_OF_EXECUTE_INPUT_PARAMS inParams{};
            inParams.inputFrame = _regHandles[curSlot];
            inParams.referenceFrame = _regHandles[_slot];
            inParams.disableTemporalHints = NV_OF_FALSE;

            NV_OF_EXECUTE_OUTPUT_PARAMS outParams{};
            outParams.outputBuffer = _regHandles[2];
            outParams.outputCostBuffer = _hasCost ? _regHandles[4] : nullptr;
            outParams.bwdOutputBuffer = _hasBidirectional ? _regHandles[3] : nullptr;
            outParams.bwdOutputCostBuffer = (_hasBidirectional && _hasCost) ? _regHandles[5] : nullptr;

            NV_OF_STATUS st = _api.nvOFExecute(_nvofSession, &inParams, &outParams);
            _slot = curSlot;
            if (st != NV_OF_SUCCESS) {
                ClearOutputs();
                return false;
            }

            struct alignas(16) DensifyParams {
                uint32_t srcW, srcH, flowW, flowH, grid, hasCost, hasBwd, pad;
            } dp{
                _width, _height, _flowWidth, _flowHeight, _gridSize,
                _hasCost ? 1u : 0u, _hasBidirectional ? 1u : 0u, 0u
            };
            _ctx->UpdateSubresource(_cb.Get(), 0, nullptr, &dp, 0, 0);

            ID3D11ShaderResourceView* srvs[4]{
                _nvofFlowSrv[0].Get(),
                _hasBidirectional ? _nvofFlowSrv[1].Get() : nullptr,
                _hasCost ? _nvofCostSrv[0].Get() : nullptr,
                (_hasBidirectional && _hasCost) ? _nvofCostSrv[1].Get() : nullptr
            };
            ID3D11UnorderedAccessView* uavs[3]{
                _fwdMotionUav.Get(), _bwdMotionUav.Get(), _confUav.Get()
            };
            ID3D11Buffer* cbs[1]{ _cb.Get() };
            _ctx->CSSetShader(_densifyCs.Get(), nullptr, 0);
            _ctx->CSSetConstantBuffers(0, 1, cbs);
            _ctx->CSSetShaderResources(0, 4, srvs);
            _ctx->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
            _ctx->Dispatch((_width + 7) / 8, (_height + 7) / 8, 1);

            ID3D11ShaderResourceView* nullSrvs[4]{};
            ID3D11UnorderedAccessView* nullUavs[3]{};
            _ctx->CSSetShaderResources(0, 4, nullSrvs);
            _ctx->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
            _ctx->CSSetShader(nullptr, nullptr, 0);
            return true;
        }

        // Fallback CS 光流路径
        if (!_hasPrev) {
            _ctx->CopyResource(_prevFallbackTex.Get(), currBgra);
            _hasPrev = true;
            ClearOutputs();
            return true;
        }
        struct alignas(16) FbParams {
            uint32_t w, h; float invW, invH; uint32_t pad[4];
        } fp{ _width, _height, 1.0f / float(_width), 1.0f / float(_height), {0,0,0,0} };
        _ctx->UpdateSubresource(_cb.Get(), 0, nullptr, &fp, 0, 0);

        ID3D11ShaderResourceView* srvs[2]{ currSrv, _prevFallbackSrv.Get() };
        ID3D11UnorderedAccessView* uavs[3]{ _fwdMotionUav.Get(), _bwdMotionUav.Get(), _confUav.Get() };
        ID3D11SamplerState* sams[1]{ _linearSampler.Get() };
        ID3D11Buffer* cbs[1]{ _cb.Get() };
        _ctx->CSSetShader(_fallbackCs.Get(), nullptr, 0);
        _ctx->CSSetConstantBuffers(0, 1, cbs);
        _ctx->CSSetSamplers(0, 1, sams);
        _ctx->CSSetShaderResources(0, 2, srvs);
        _ctx->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
        _ctx->Dispatch((_width + 7) / 8, (_height + 7) / 8, 1);

        ID3D11ShaderResourceView* nullSrvs[2]{};
        ID3D11UnorderedAccessView* nullUavs[3]{};
        _ctx->CSSetShaderResources(0, 2, nullSrvs);
        _ctx->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
        _ctx->CSSetShader(nullptr, nullptr, 0);

        _ctx->CopyResource(_prevFallbackTex.Get(), currBgra);
        return true;
    }

    bool IsHardwareNvofa() const { return _usingNvofa; }
    ID3D11ShaderResourceView* FwdMotionSrv() const { return _fwdMotionSrv.Get(); }
    ID3D11ShaderResourceView* BwdMotionSrv() const { return _bwdMotionSrv.Get(); }
    ID3D11ShaderResourceView* ConfidenceSrv() const { return _confSrv.Get(); }

private:
    bool _InitNvofa(uint32_t w, uint32_t h) {
        _nvofModule = LoadLibraryExW(L"nvofapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!_nvofModule) return false;
        using CreateFn = NV_OF_STATUS (NVOFAPI*)(uint32_t, NV_OF_D3D11_API_FUNCTION_LIST*);
        auto createFn = reinterpret_cast<CreateFn>(GetProcAddress(_nvofModule, "NvOFAPICreateInstanceD3D11"));
        if (!createFn || createFn(NV_OF_API_VERSION, &_api) != NV_OF_SUCCESS) return false;
        if (_api.nvCreateOpticalFlowD3D11(_dev, _ctx, &_nvofSession) != NV_OF_SUCCESS || !_nvofSession) return false;

        _gridSize = 4;
        _flowWidth = (w + _gridSize - 1) / _gridSize;
        _flowHeight = (h + _gridSize - 1) / _gridSize;

        NV_OF_INIT_PARAMS ip{};
        ip.width = w;
        ip.height = h;
        ip.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
        ip.mode = NV_OF_MODE_OPTICALFLOW;
        // 针对 30fps 主机游戏转视角单帧大位移 (20~64px)，采用 MEDIUM 硬件档位，在保持 ~0.4ms 极低延迟的同时显著提升大位移光流精度
        ip.perfLevel = NV_OF_PERF_LEVEL_MEDIUM;
        ip.enableOutputCost = NV_OF_TRUE;
        ip.predDirection = NV_OF_PRED_DIRECTION_BOTH;
        ip.inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;

        NV_OF_STATUS st = _api.nvOFInit(_nvofSession, &ip);
        if (st == NV_OF_SUCCESS) {
            _hasBidirectional = true;
            _hasCost = true;
        } else {
            ip.predDirection = NV_OF_PRED_DIRECTION_FORWARD;
            st = _api.nvOFInit(_nvofSession, &ip);
            if (st != NV_OF_SUCCESS) {
                ip.enableOutputCost = NV_OF_FALSE;
                st = _api.nvOFInit(_nvofSession, &ip);
                if (st != NV_OF_SUCCESS) return false;
                _hasCost = false;
            } else {
                _hasCost = true;
            }
            _hasBidirectional = false;
        }

        for (int i = 0; i < 2; ++i) {
            if (!CreateTex2D(_dev, w, h, DXGI_FORMAT_B8G8R8A8_UNORM,
                             D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                             _nvofInput[i].Put())) return false;
            if (_api.nvOFRegisterResourceD3D11(_nvofSession, _nvofInput[i].Get(), &_regHandles[i]) != NV_OF_SUCCESS) return false;
        }

        int flowCount = _hasBidirectional ? 2 : 1;
        for (int i = 0; i < flowCount; ++i) {
            if (!CreateTex2D(_dev, _flowWidth, _flowHeight, DXGI_FORMAT_R16G16_SINT,
                             D3D11_BIND_SHADER_RESOURCE, _nvofFlow[i].Put(), _nvofFlowSrv[i].Put())) return false;
            if (_api.nvOFRegisterResourceD3D11(_nvofSession, _nvofFlow[i].Get(), &_regHandles[2 + i]) != NV_OF_SUCCESS) return false;
            if (_hasCost) {
                if (!CreateTex2D(_dev, _flowWidth, _flowHeight, DXGI_FORMAT_R8_UINT,
                                 D3D11_BIND_SHADER_RESOURCE, _nvofCost[i].Put(), _nvofCostSrv[i].Put())) return false;
                if (_api.nvOFRegisterResourceD3D11(_nvofSession, _nvofCost[i].Get(), &_regHandles[4 + i]) != NV_OF_SUCCESS) return false;
            }
        }

        return BuildComputeShader(_dev, XShaders::HLSL_NVOFA_DENSIFY, "CSMain", _densifyCs.Put());
    }

    ID3D11Device5* _dev = nullptr;
    ID3D11DeviceContext4* _ctx = nullptr;
    uint32_t _width = 0, _height = 0;
    uint32_t _gridSize = 4, _flowWidth = 0, _flowHeight = 0;
    bool _usingNvofa = false;
    bool _hasBidirectional = false;
    bool _hasCost = false;
    bool _hasPrev = false;
    uint32_t _slot = 0;

    HMODULE _nvofModule = nullptr;
    NV_OF_D3D11_API_FUNCTION_LIST _api{};
    NvOFHandle _nvofSession = nullptr;
    NvOFGPUBufferHandle _regHandles[6]{};

    ComRef<ID3D11Texture2D> _nvofInput[2];
    ComRef<ID3D11Texture2D> _nvofFlow[2];
    ComRef<ID3D11ShaderResourceView> _nvofFlowSrv[2];
    ComRef<ID3D11Texture2D> _nvofCost[2];
    ComRef<ID3D11ShaderResourceView> _nvofCostSrv[2];
    ComRef<ID3D11ComputeShader> _densifyCs;

    ComRef<ID3D11ComputeShader> _fallbackCs;
    ComRef<ID3D11Texture2D> _prevFallbackTex;
    ComRef<ID3D11ShaderResourceView> _prevFallbackSrv;

    ComRef<ID3D11Texture2D> _fwdMotion, _bwdMotion, _conf;
    ComRef<ID3D11ShaderResourceView> _fwdMotionSrv, _bwdMotionSrv, _confSrv;
    ComRef<ID3D11UnorderedAccessView> _fwdMotionUav, _bwdMotionUav, _confUav;
    ComRef<ID3D11SamplerState> _linearSampler;
    ComRef<ID3D11Buffer> _cb;
};
