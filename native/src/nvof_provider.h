#pragma once
// XStreaming Desktop Enhanced - NVIDIA NVOFA 第 4 代硬件光流与 AMD Radeon 三级金字塔双向光流双引擎服务
// - NVIDIA RTX 显卡：优先调用独立硬件光流单元 (nvofapi64.dll，< 0.35ms 零抢占渲染核心)
// - AMD Radeon RX 6000/7000/9000 及 Ryzen 780M/890M 掌机核显：自动启用三级由粗到精金字塔双向光流 (AMD_PyrFlow)，
//   覆盖最高 ±96px 大位移摇杆转视角，并输出独立前/反向光流与循环一致性遮挡置信度，彻底杜绝补帧重影！

#include "d3d_helpers.h"
#include "shaders_hlsl.h"
#include "../third_party/include/nvOpticalFlowCommon.h"
#include "../third_party/include/nvOpticalFlowD3D11.h"
#include <algorithm>

class OpticalFlowEngine {
public:
    ~OpticalFlowEngine() { Shutdown(); }

    bool Initialize(ID3D11Device5* dev, ID3D11DeviceContext4* ctx, uint32_t w, uint32_t h, bool allowNvofa) {
        Shutdown();
        _dev = dev;
        _ctx = ctx;
        _width = w;
        _height = h;

        // 创建全分辨率稠密前/反向光流与双向可见置信度纹理
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
        cbd.Usage = D3D11_USAGE_DEFAULT;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.ByteWidth = 32;
        if (FAILED(dev->CreateBuffer(&cbd, nullptr, _cb32.Put()))) return false;
        cbd.ByteWidth = 48;
        if (FAILED(dev->CreateBuffer(&cbd, nullptr, _cb48.Put()))) return false;

        if (allowNvofa && _InitNvofa(w, h)) {
            _usingNvofa = true;
        } else {
            _usingNvofa = false;
            if (!_InitPyramidalFlow(w, h)) {
                return false;
            }
        }

        ClearOutputs();
        _hasPrev = false;
        _slot = 0;
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

        _pyrBuildCs.Reset();
        _pyrEstimateCs.Reset();
        _pyrDensifyCs.Reset();
        for (int s = 0; s < 2; ++s) {
            _pyrFullTex[s].Reset();
            _pyrFullSrv[s].Reset();
            for (int l = 0; l < 3; ++l) {
                _pyrTex[s][l].Reset();
                _pyrSrv[s][l].Reset();
                _pyrUav[s][l].Reset();
            }
        }
        for (int l = 0; l < 3; ++l) {
            _pyrFlowFwd[l].Reset(); _pyrFlowFwdSrv[l].Reset(); _pyrFlowFwdUav[l].Reset();
            _pyrFlowBwd[l].Reset(); _pyrFlowBwdSrv[l].Reset(); _pyrFlowBwdUav[l].Reset();
        }

        _fwdMotion.Reset(); _fwdMotionSrv.Reset(); _fwdMotionUav.Reset();
        _bwdMotion.Reset(); _bwdMotionSrv.Reset(); _bwdMotionUav.Reset();
        _conf.Reset(); _confSrv.Reset(); _confUav.Reset();
        _linearSampler.Reset();
        _cb32.Reset();
        _cb48.Reset();
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

    // 输入当前帧 BGRA8 纹理，计算 Curr->Prev 与 Prev->Curr 光流及双向可见置信度
    bool Compute(ID3D11Texture2D* currBgra, ID3D11ShaderResourceView* /*currSrv*/) {
        if (_usingNvofa) {
            uint32_t curSlot = _slot ^ 1u;
            _ctx->CopyResource(_nvofInput[curSlot].Get(), currBgra);
            if (!_hasPrev) {
                _slot = curSlot;
                _hasPrev = true;
                ClearOutputs();
                return true;
            }

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
            _ctx->UpdateSubresource(_cb32.Get(), 0, nullptr, &dp, 0, 0);

            ID3D11ShaderResourceView* srvs[4]{
                _nvofFlowSrv[0].Get(),
                _hasBidirectional ? _nvofFlowSrv[1].Get() : nullptr,
                _hasCost ? _nvofCostSrv[0].Get() : nullptr,
                (_hasBidirectional && _hasCost) ? _nvofCostSrv[1].Get() : nullptr
            };
            ID3D11UnorderedAccessView* uavs[3]{
                _fwdMotionUav.Get(), _bwdMotionUav.Get(), _confUav.Get()
            };
            ID3D11Buffer* cbs[1]{ _cb32.Get() };
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

        // ==================== AMD Radeon 三级由粗到精金字塔双向光流路径 ====================
        uint32_t curSlot = _slot ^ 1u;
        uint32_t prevSlot = _slot;
        _ctx->CopyResource(_pyrFullTex[curSlot].Get(), currBgra);
        _BuildFeaturePyramid(curSlot);

        if (!_hasPrev) {
            _slot = curSlot;
            _hasPrev = true;
            ClearOutputs();
            return true;
        }

        // 1. 由粗到精 (L2 -> L1 -> L0) 独立估计前向光流 (Curr -> Prev) 与反向光流 (Prev -> Curr)
        _EstimateDirection(curSlot, prevSlot, _pyrFlowFwdSrv, _pyrFlowFwdUav);
        _EstimateDirection(prevSlot, curSlot, _pyrFlowBwdSrv, _pyrFlowBwdUav);

        // 2. 全分辨率保边锐化稠密化 + 前/反向循环一致性遮挡置信度合成
        struct alignas(16) PyrDensifyCB {
            uint32_t w, h;
            float invW, invH;
            float invL0W, invL0H;
            float scaleX, scaleY;
        } dcb{
            _width, _height,
            1.0f / float(_width), 1.0f / float(_height),
            1.0f / float(_pyrW[0]), 1.0f / float(_pyrH[0]),
            float(_width) / float(_pyrW[0]), float(_height) / float(_pyrH[0])
        };
        _ctx->UpdateSubresource(_cb32.Get(), 0, nullptr, &dcb, 0, 0);

        ID3D11ShaderResourceView* srvs[4]{
            _pyrFlowFwdSrv[0].Get(),
            _pyrFlowBwdSrv[0].Get(),
            _pyrFullSrv[curSlot].Get(),
            _pyrFullSrv[prevSlot].Get()
        };
        ID3D11UnorderedAccessView* uavs[3]{
            _fwdMotionUav.Get(), _bwdMotionUav.Get(), _confUav.Get()
        };
        ID3D11SamplerState* sams[1]{ _linearSampler.Get() };
        ID3D11Buffer* cbs[1]{ _cb32.Get() };

        _ctx->CSSetShader(_pyrDensifyCs.Get(), nullptr, 0);
        _ctx->CSSetConstantBuffers(0, 1, cbs);
        _ctx->CSSetSamplers(0, 1, sams);
        _ctx->CSSetShaderResources(0, 4, srvs);
        _ctx->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
        _ctx->Dispatch((_width + 7) / 8, (_height + 7) / 8, 1);

        ID3D11ShaderResourceView* nullSrvs[4]{};
        ID3D11UnorderedAccessView* nullUavs[3]{};
        _ctx->CSSetShaderResources(0, 4, nullSrvs);
        _ctx->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
        _ctx->CSSetShader(nullptr, nullptr, 0);

        _slot = curSlot;
        return true;
    }

    bool IsHardwareNvofa() const { return _usingNvofa; }
    const char* BackendName() const { return _usingNvofa ? "NVOFA" : "AMD_PyrFlow"; }
    ID3D11ShaderResourceView* FwdMotionSrv() const { return _fwdMotionSrv.Get(); }
    ID3D11ShaderResourceView* BwdMotionSrv() const { return _bwdMotionSrv.Get(); }
    ID3D11ShaderResourceView* ConfidenceSrv() const { return _confSrv.Get(); }

private:
    bool _InitPyramidalFlow(uint32_t w, uint32_t h) {
        if (!BuildComputeShader(_dev, XShaders::HLSL_PYR_BUILD, "CSMain", _pyrBuildCs.Put()) ||
            !BuildComputeShader(_dev, XShaders::HLSL_PYR_FLOW_ESTIMATE, "CSMain", _pyrEstimateCs.Put()) ||
            !BuildComputeShader(_dev, XShaders::HLSL_PYR_FLOW_DENSIFY, "CSMain", _pyrDensifyCs.Put())) {
            return false;
        }

        _pyrW[0] = std::max(16u, (w + 1u) / 2u);
        _pyrH[0] = std::max(16u, (h + 1u) / 2u);
        _pyrW[1] = std::max(16u, (_pyrW[0] + 1u) / 2u);
        _pyrH[1] = std::max(16u, (_pyrH[0] + 1u) / 2u);
        _pyrW[2] = std::max(16u, (_pyrW[1] + 1u) / 2u);
        _pyrH[2] = std::max(16u, (_pyrH[1] + 1u) / 2u);

        UINT bindRw = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        for (int s = 0; s < 2; ++s) {
            if (!CreateTex2D(_dev, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE,
                             _pyrFullTex[s].Put(), _pyrFullSrv[s].Put())) {
                return false;
            }
            for (int l = 0; l < 3; ++l) {
                if (!CreateTex2D(_dev, _pyrW[l], _pyrH[l], DXGI_FORMAT_R16G16B16A16_FLOAT, bindRw,
                                 _pyrTex[s][l].Put(), _pyrSrv[s][l].Put(), _pyrUav[s][l].Put())) {
                    return false;
                }
            }
        }
        for (int l = 0; l < 3; ++l) {
            if (!CreateTex2D(_dev, _pyrW[l], _pyrH[l], DXGI_FORMAT_R16G16_FLOAT, bindRw,
                             _pyrFlowFwd[l].Put(), _pyrFlowFwdSrv[l].Put(), _pyrFlowFwdUav[l].Put()) ||
                !CreateTex2D(_dev, _pyrW[l], _pyrH[l], DXGI_FORMAT_R16G16_FLOAT, bindRw,
                             _pyrFlowBwd[l].Put(), _pyrFlowBwdSrv[l].Put(), _pyrFlowBwdUav[l].Put())) {
                return false;
            }
        }
        return true;
    }

    void _BuildFeaturePyramid(uint32_t slot) {
        ID3D11SamplerState* sams[1]{ _linearSampler.Get() };
        ID3D11Buffer* cbs[1]{ _cb32.Get() };
        _ctx->CSSetShader(_pyrBuildCs.Get(), nullptr, 0);
        _ctx->CSSetConstantBuffers(0, 1, cbs);
        _ctx->CSSetSamplers(0, 1, sams);

        for (int l = 0; l < 3; ++l) {
            struct alignas(16) PyrBuildCB {
                uint32_t dstW, dstH;
                float invDstW, invDstH;
                uint32_t fromBase, pad0, pad1, pad2;
            } bcb{
                _pyrW[l], _pyrH[l],
                1.0f / float(_pyrW[l]), 1.0f / float(_pyrH[l]),
                (l == 0) ? 1u : 0u, 0u, 0u, 0u
            };
            _ctx->UpdateSubresource(_cb32.Get(), 0, nullptr, &bcb, 0, 0);

            ID3D11ShaderResourceView* srvs[1]{ (l == 0) ? _pyrFullSrv[slot].Get() : _pyrSrv[slot][l - 1].Get() };
            ID3D11UnorderedAccessView* uavs[1]{ _pyrUav[slot][l].Get() };
            _ctx->CSSetShaderResources(0, 1, srvs);
            _ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
            _ctx->Dispatch((_pyrW[l] + 7) / 8, (_pyrH[l] + 7) / 8, 1);

            ID3D11ShaderResourceView* nullSrvs[1]{};
            ID3D11UnorderedAccessView* nullUavs[1]{};
            _ctx->CSSetShaderResources(0, 1, nullSrvs);
            _ctx->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
        }
        _ctx->CSSetShader(nullptr, nullptr, 0);
    }

    void _EstimateDirection(
        uint32_t srcSlot,
        uint32_t refSlot,
        ComRef<ID3D11ShaderResourceView> (&flowSrv)[3],
        ComRef<ID3D11UnorderedAccessView> (&flowUav)[3]
    ) {
        ID3D11SamplerState* sams[1]{ _linearSampler.Get() };
        ID3D11Buffer* cbs[1]{ _cb48.Get() };
        _ctx->CSSetShader(_pyrEstimateCs.Get(), nullptr, 0);
        _ctx->CSSetConstantBuffers(0, 1, cbs);
        _ctx->CSSetSamplers(0, 1, sams);

        for (int l = 2; l >= 0; --l) {
            bool hasParent = (l < 2);
            uint32_t pw = hasParent ? _pyrW[l + 1] : _pyrW[l];
            uint32_t ph = hasParent ? _pyrH[l + 1] : _pyrH[l];

            struct alignas(16) PyrFlowCB {
                uint32_t w, h;
                float invW, invH;
                float invParentW, invParentH;
                float parentScaleX, parentScaleY;
                uint32_t hasParent, pad0, pad1, pad2;
            } fcb{
                _pyrW[l], _pyrH[l],
                1.0f / float(_pyrW[l]), 1.0f / float(_pyrH[l]),
                1.0f / float(pw), 1.0f / float(ph),
                float(_pyrW[l]) / float(pw), float(_pyrH[l]) / float(ph),
                hasParent ? 1u : 0u, 0u, 0u, 0u
            };
            _ctx->UpdateSubresource(_cb48.Get(), 0, nullptr, &fcb, 0, 0);

            ID3D11ShaderResourceView* srvs[3]{
                _pyrSrv[srcSlot][l].Get(),
                _pyrSrv[refSlot][l].Get(),
                hasParent ? flowSrv[l + 1].Get() : nullptr
            };
            ID3D11UnorderedAccessView* uavs[1]{ flowUav[l].Get() };
            _ctx->CSSetShaderResources(0, 3, srvs);
            _ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
            _ctx->Dispatch((_pyrW[l] + 7) / 8, (_pyrH[l] + 7) / 8, 1);

            ID3D11ShaderResourceView* nullSrvs[3]{};
            ID3D11UnorderedAccessView* nullUavs[1]{};
            _ctx->CSSetShaderResources(0, 3, nullSrvs);
            _ctx->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
        }
        _ctx->CSSetShader(nullptr, nullptr, 0);
    }

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

    // AMD Radeon 三级金字塔双向光流纹理与着色器
    uint32_t _pyrW[3]{}, _pyrH[3]{};
    ComRef<ID3D11ComputeShader> _pyrBuildCs, _pyrEstimateCs, _pyrDensifyCs;
    ComRef<ID3D11Texture2D> _pyrFullTex[2];
    ComRef<ID3D11ShaderResourceView> _pyrFullSrv[2];
    ComRef<ID3D11Texture2D> _pyrTex[2][3];
    ComRef<ID3D11ShaderResourceView> _pyrSrv[2][3];
    ComRef<ID3D11UnorderedAccessView> _pyrUav[2][3];
    ComRef<ID3D11Texture2D> _pyrFlowFwd[3], _pyrFlowBwd[3];
    ComRef<ID3D11ShaderResourceView> _pyrFlowFwdSrv[3], _pyrFlowBwdSrv[3];
    ComRef<ID3D11UnorderedAccessView> _pyrFlowFwdUav[3], _pyrFlowBwdUav[3];

    ComRef<ID3D11Texture2D> _fwdMotion, _bwdMotion, _conf;
    ComRef<ID3D11ShaderResourceView> _fwdMotionSrv, _bwdMotionSrv, _confSrv;
    ComRef<ID3D11UnorderedAccessView> _fwdMotionUav, _bwdMotionUav, _confUav;
    ComRef<ID3D11SamplerState> _linearSampler;
    ComRef<ID3D11Buffer> _cb32, _cb48;
};
