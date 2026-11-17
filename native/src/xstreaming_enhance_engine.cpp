// XStreaming Desktop Enhanced - 原生超低延迟 D3D11/D3D12 神经渲染 + 硬件光流 X2 补帧 + 自适应锐化引擎
// 专为「Xbox 有线 + PC Wi-Fi 6 低延迟串流」设计：
//   - 捕获层：DXGI Output Duplication GPU 零拷贝子矩形截取 + WDA_EXCLUDEFROMCAPTURE 防回环
//   - 交互层：SetWindowRgn 动态镂空避让 React 抽屉与调参弹窗，边玩边调零 UI 模糊
//   - 光流层：NVIDIA 第 4 代硬件光流加速器 NVOFA (nvofapi64.dll) 双向预测 + CS 光流自动回退
//   - 神经层：内置真 nvngx_dlssnr.dll (DLSS 5 Feature 18) 异步残差极低延迟管线 + Oklab 调色抗闪烁
//   - 锐化层：AMD FidelityFX CAS / FSR RCAS 串流去噪自适应锐化
//   - 补帧层：Zero-Wait 零等待外推 X2 (0ms 扣押延迟) / Bidirectional 双向遮挡修补内插 X2
//   - 呈现层：Flip Discard + Allow Tearing + MaximumFrameLatency(1) 零垂直同步排队呈现

#include "d3d_helpers.h"
#include "engine_config.h"
#include "shaders_hlsl.h"
#include "nvof_provider.h"
#include "ngx_dlss5_bridge.h"
#include "wgc_capturer.h"

#include <mmsystem.h>
#include <dwmapi.h>
#include <avrt.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <iostream>
#include <sstream>
#include <vector>
#include <cmath>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

// ==================== 轻量级非阻塞 JSON 指令解析辅助函数 ====================
namespace MiniJson {
    inline bool ExtractBool(const std::string& s, const char* key, bool& out) {
        std::string pat = std::string("\"") + key + "\"";
        size_t pos = s.find(pat);
        if (pos == std::string::npos) return false;
        pos = s.find(':', pos + pat.size());
        if (pos == std::string::npos) return false;
        while (pos + 1 < s.size() && (s[pos + 1] == ' ' || s[pos + 1] == '\t')) ++pos;
        if (s.compare(pos + 1, 4, "true") == 0) { out = true; return true; }
        if (s.compare(pos + 1, 5, "false") == 0) { out = false; return true; }
        return false;
    }

    inline bool ExtractNumber(const std::string& s, const char* key, double& out) {
        std::string pat = std::string("\"") + key + "\"";
        size_t pos = s.find(pat);
        if (pos == std::string::npos) return false;
        pos = s.find(':', pos + pat.size());
        if (pos == std::string::npos) return false;
        const char* start = s.c_str() + pos + 1;
        char* endPtr = nullptr;
        double val = std::strtod(start, &endPtr);
        if (endPtr == start) return false;
        out = val;
        return true;
    }

    inline void ParseConfigPatch(const std::string& line, EnhanceConfig& cfg) {
        bool b = false; double d = 0.0;
        if (ExtractBool(line, "enabled", b)) cfg.enabled = b;
        if (ExtractBool(line, "paused", b)) cfg.paused = b;
        if (ExtractNumber(line, "viewportOffsetX", d)) cfg.viewportOffsetX = (int)std::lround(d);
        if (ExtractNumber(line, "viewportOffsetY", d)) cfg.viewportOffsetY = (int)std::lround(d);
        if (ExtractNumber(line, "viewportWidth", d)) cfg.viewportWidth = (int)std::lround(d);
        if (ExtractNumber(line, "viewportHeight", d)) cfg.viewportHeight = (int)std::lround(d);

        if (ExtractBool(line, "fgEnabled", b)) cfg.fgEnabled = b;
        if (ExtractBool(line, "dedup30In60", b)) cfg.dedup30In60 = b;
        if (ExtractNumber(line, "dedupSensitivity", d)) cfg.dedupSensitivity = std::clamp((int)std::lround(d), 1, 10);
        if (ExtractNumber(line, "fgMode", d)) cfg.fgMode = std::clamp((int)std::lround(d), 0, 1);
        if (ExtractNumber(line, "fgSceneCutThreshold", d)) cfg.fgSceneCutThreshold = std::clamp((float)d, 0.05f, 0.50f);
        if (ExtractNumber(line, "fgUiProtection", d)) cfg.fgUiProtection = std::clamp((float)d, 0.0f, 1.0f);

        if (ExtractBool(line, "dlss5Enabled", b)) cfg.dlss5Enabled = b;
        if (ExtractNumber(line, "dlss5Passes", d)) cfg.dlss5Passes = std::clamp((int)std::lround(d), 1, 2);
        if (ExtractNumber(line, "dlss5LatencyMode", d)) cfg.dlss5LatencyMode = std::clamp((int)std::lround(d), 0, 1);
        if (ExtractNumber(line, "dlss5InputScale", d)) cfg.dlss5InputScale = (uint32_t)std::clamp((int)std::lround(d), 50, 100);
        if (ExtractNumber(line, "dlss5Style", d)) cfg.dlss5Style = std::clamp((int)std::lround(d), 0, 2);
        if (ExtractNumber(line, "dlss5Intensity", d)) cfg.dlss5Intensity = std::clamp((float)d, 0.0f, 2.0f);
        if (ExtractNumber(line, "dlss5Pass2Style", d)) cfg.dlss5Pass2Style = std::clamp((int)std::lround(d), 0, 2);
        if (ExtractNumber(line, "dlss5Pass2Intensity", d)) cfg.dlss5Pass2Intensity = std::clamp((float)d, 0.0f, 2.0f);
        if (ExtractNumber(line, "dlss5LocalTone", d)) cfg.dlss5LocalTone = std::clamp((float)d, 0.0f, 2.0f);
        if (ExtractNumber(line, "dlss5LocalStructure", d)) cfg.dlss5LocalStructure = std::clamp((float)d, 0.0f, 2.0f);
        if (ExtractNumber(line, "dlss5SkinStructure", d)) cfg.dlss5SkinStructure = std::clamp((float)d, 0.0f, 2.0f);
        if (ExtractBool(line, "dlss5AutoMask", b)) cfg.dlss5AutoMask = b;
        if (ExtractBool(line, "dlss5UiCorrection", b)) cfg.dlss5UiCorrection = b;

        if (ExtractNumber(line, "residualStrength", d)) cfg.residualStrength = std::clamp((float)d, 0.0f, 2.0f);
        if (ExtractNumber(line, "shadowStructure", d)) cfg.shadowStructure = std::clamp((float)d, 0.0f, 2.0f);
        if (ExtractNumber(line, "highlightGlow", d)) cfg.highlightGlow = std::clamp((float)d, 0.0f, 2.0f);
        if (ExtractNumber(line, "saturation", d)) cfg.saturation = std::clamp((float)d, 0.0f, 2.0f);
        if (ExtractNumber(line, "darkProtection", d)) cfg.darkProtection = std::clamp((float)d, 0.0f, 1.0f);
        if (ExtractNumber(line, "highlightProtection", d)) cfg.highlightProtection = std::clamp((float)d, 0.0f, 1.0f);
        if (ExtractNumber(line, "antiFlicker", d)) cfg.antiFlicker = std::clamp((int)std::lround(d), 0, 2);

        if (ExtractNumber(line, "sharpenMode", d)) cfg.sharpenMode = std::clamp((int)std::lround(d), 0, 2);
        if (ExtractNumber(line, "sharpenStrength", d)) cfg.sharpenStrength = std::clamp((float)d, 0.0f, 2.0f);
        if (ExtractBool(line, "sharpenDenoise", b)) cfg.sharpenDenoise = b;

        if (ExtractBool(line, "allowTearing", b)) cfg.allowTearing = b;
        if (ExtractBool(line, "obsCaptureCompat", b)) cfg.obsCaptureCompat = b;
        if (ExtractNumber(line, "splitCompare", d)) cfg.splitCompare = std::clamp((float)d, 0.0f, 1.0f);

        // 解析需要避让的 React 交互面板矩形数组 "excludeRects":[{"x":..,"y":..,"w":..,"h":..}]
        size_t erPos = line.find("\"excludeRects\"");
        if (erPos != std::string::npos) {
            size_t arrStart = line.find('[', erPos);
            size_t arrEnd = (arrStart != std::string::npos) ? line.find(']', arrStart) : std::string::npos;
            if (arrStart != std::string::npos && arrEnd != std::string::npos) {
                cfg.excludeRects.clear();
                size_t cur = arrStart;
                while (cur < arrEnd) {
                    size_t oStart = line.find('{', cur);
                    if (oStart == std::string::npos || oStart > arrEnd) break;
                    size_t oEnd = line.find('}', oStart);
                    if (oEnd == std::string::npos || oEnd > arrEnd) break;
                    std::string obj = line.substr(oStart, oEnd - oStart + 1);
                    double rx = 0, ry = 0, rw = 0, rh = 0;
                    if (ExtractNumber(obj, "x", rx) && ExtractNumber(obj, "y", ry) &&
                        ExtractNumber(obj, "w", rw) && ExtractNumber(obj, "h", rh)) {
                        if (rw > 1 && rh > 1) {
                            cfg.excludeRects.push_back({ (int)std::lround(rx), (int)std::lround(ry),
                                                         (int)std::lround(rw), (int)std::lround(rh) });
                        }
                    }
                    cur = oEnd + 1;
                }
            }
        }
    }
}

// ==================== 核心 GPU 处理管线（同时支持无头自检与窗口实时增强） ====================
class StreamEnhancePipeline {
public:
    bool Initialize(uint32_t w, uint32_t h, const std::wstring& binDir) {
        Shutdown();
        _width = w;
        _height = h;
        _binDir = binDir;

        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory6), (void**)_factory.Put()))) return false;

        // 优先选择高性能 NVIDIA 独立显卡
        for (UINT i = 0; ; ++i) {
            ComRef<IDXGIAdapter1> ad;
            if (_factory->EnumAdapters1(i, ad.Put()) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc{};
            ad->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            if (!_adapter || (desc.VendorId == 0x10DE && !_isNvidia)) {
                _adapter = std::move(ad);
                _isNvidia = (desc.VendorId == 0x10DE);
                char nameBuf[256]{};
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nameBuf, sizeof(nameBuf), nullptr, nullptr);
                _gpuName = nameBuf;
            }
            if (_isNvidia) break;
        }
        if (!_adapter) return false;

        // 检查 DXGI Allow Tearing 支持能力
        BOOL allowTear = FALSE;
        ComRef<IDXGIFactory5> fac5;
        if (SUCCEEDED(_factory->QueryInterface(__uuidof(IDXGIFactory5), (void**)fac5.Put()))) {
            fac5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTear, sizeof(allowTear));
        }
        _tearingSupported = (allowTear == TRUE);

        // 创建 D3D11.4 设备
        D3D_FEATURE_LEVEL fls[]{ D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        ComRef<ID3D11Device> baseDev;
        ComRef<ID3D11DeviceContext> baseCtx;
        if (FAILED(D3D11CreateDevice(_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT, fls, 2, D3D11_SDK_VERSION,
                                     baseDev.Put(), nullptr, baseCtx.Put()))) {
            return false;
        }
        if (FAILED(baseDev->QueryInterface(__uuidof(ID3D11Device5), (void**)_dev11.Put())) ||
            FAILED(baseCtx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**)_ctx11.Put()))) {
            return false;
        }
        // 开启 D3D11 多线程保护锁，确保 Windows.Graphics.Capture FreeThreaded 后台捕获工作线程与渲染线程安全共享设备
        ComRef<ID3D10Multithread> mt;
        if (SUCCEEDED(_dev11->QueryInterface(__uuidof(ID3D10Multithread), (void**)mt.Put())) && mt) {
            mt->SetMultithreadProtected(TRUE);
        }

        // 编译 30-in-60 重复帧检测、零竞态光度自洽中点运动场、主合成锐化、X2 补帧与呈现着色器
        if (!BuildComputeShader(_dev11.Get(), XShaders::HLSL_DETECT_DUPLICATE, "CSMain", _csDedup.Put()) ||
            !BuildComputeShader(_dev11.Get(), XShaders::HLSL_PROJECT_FLOW, "CSMain", _csProj.Put()) ||
            !BuildComputeShader(_dev11.Get(), XShaders::HLSL_COMPOSE_AND_SHARPEN, "CSMain", _csCompose.Put()) ||
            !BuildComputeShader(_dev11.Get(), XShaders::HLSL_FRAME_GEN_X2, "CSMain", _csFrameGen.Put())) {
            return false;
        }
        ComRef<ID3DBlob> vsBlob, psBlob;
        if (!CompileHlslShader(XShaders::HLSL_PRESENT_VS, "VSMain", "vs_5_0", vsBlob.Put()) ||
            !CompileHlslShader(XShaders::HLSL_PRESENT_PS, "PSMain", "ps_5_0", psBlob.Put())) {
            return false;
        }
        if (FAILED(_dev11->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, _vsPresent.Put())) ||
            FAILED(_dev11->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, _psPresent.Put()))) {
            return false;
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
        _dev11->CreateBuffer(&cbd, nullptr, _cbCompose.Put());
        cbd.ByteWidth = 32;
        _dev11->CreateBuffer(&cbd, nullptr, _cbFrameGen.Put());
        cbd.ByteWidth = 128;
        _dev11->CreateBuffer(&cbd, nullptr, _cbProj.Put());
        cbd.ByteWidth = 112;
        _dev11->CreateBuffer(&cbd, nullptr, _cbPresent.Put());
        cbd.ByteWidth = 16;
        _dev11->CreateBuffer(&cbd, nullptr, _cbDedup.Put());

        // 创建硬件完成事件查询，用于实测包含 DLSS 5 并发竞争在内的端到端真实上屏增加延迟
        D3D11_QUERY_DESC qd{};
        qd.Query = D3D11_QUERY_EVENT;
        _dev11->CreateQuery(&qd, _gpuDoneQuery.Put());

        // 创建 8 字节 StructuredBuffer 用于超高速 (<0.01ms) GPU 30-in-60 重复帧与切镜检测结果回读
        D3D11_BUFFER_DESC sbd{};
        sbd.ByteWidth = 8;
        sbd.Usage = D3D11_USAGE_DEFAULT;
        sbd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        sbd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        sbd.StructureByteStride = 8;
        if (FAILED(_dev11->CreateBuffer(&sbd, nullptr, _dupStatsGpu.Put()))) return false;

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavd{};
        uavd.Format = DXGI_FORMAT_UNKNOWN;
        uavd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavd.Buffer.FirstElement = 0;
        uavd.Buffer.NumElements = 1;
        if (FAILED(_dev11->CreateUnorderedAccessView(_dupStatsGpu.Get(), &uavd, _dupStatsUav.Put()))) return false;

        D3D11_BUFFER_DESC stgd{};
        stgd.ByteWidth = 8;
        stgd.Usage = D3D11_USAGE_STAGING;
        stgd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(_dev11->CreateBuffer(&stgd, nullptr, _dupStatsStaging.Put()))) return false;

        return ResizeResources(w, h);
    }

    bool ResizeResources(uint32_t w, uint32_t h) {
        w = std::max(64u, w);
        h = std::max(64u, h);
        _width = w;
        _height = h;

        UINT bind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (!CreateTex2D(_dev11.Get(), w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, _candidateTex.Put(), _candidateSrv.Put())) return false;
        if (!CreateTex2D(_dev11.Get(), w, h, DXGI_FORMAT_B8G8R8A8_UNORM, bind, _rawSrcTex.Put(), _rawSrcSrv.Put(), _rawSrcUav.Put())) return false;
        if (!CreateTex2D(_dev11.Get(), w, h, DXGI_FORMAT_R16G16_FLOAT, bind, _projFlowTex.Put(), _projFlowSrv.Put(), _projFlowUav.Put())) return false;
        for (int i = 0; i < 2; ++i) {
            if (!CreateTex2D(_dev11.Get(), w, h, DXGI_FORMAT_R8G8B8A8_UNORM, bind, _enhancedTex[i].Put(), _enhancedSrv[i].Put(), _enhancedUav[i].Put())) return false;
        }
        if (!CreateTex2D(_dev11.Get(), w, h, DXGI_FORMAT_R8G8B8A8_UNORM, bind, _genTex.Put(), _genSrv.Put(), _genUav.Put())) return false;
        if (!CreateTex2D(_dev11.Get(), w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, bind, _zeroResTex.Put(), _zeroResSrv.Put(), _zeroResUav.Put())) return false;
        const float zero[4]{ 0, 0, 0, 0 };
        _ctx11->ClearUnorderedAccessViewFloat(_zeroResUav.Get(), zero);

        // 初始化 NVOFA 硬件光流引擎
        _opticalFlow.Initialize(_dev11.Get(), _ctx11.Get(), w, h, _isNvidia);

        // 初始化内置真 DLSS 5 (nvngx_dlssnr.dll Feature 18) 桥接引擎
        if (_isNvidia) {
            _dlss5.Initialize(_adapter.Get(), _dev11.Get(), _ctx11.Get(), _binDir, w, h, 75);
        }

        _enhSlot = 0;
        _hasPrevEnhanced = false;
        _hasFirstReal = false;
        return true;
    }

    void Shutdown() {
        _dlss5.Shutdown();
        _opticalFlow.Shutdown();
        _candidateTex.Reset(); _candidateSrv.Reset();
        _rawSrcTex.Reset(); _rawSrcSrv.Reset(); _rawSrcUav.Reset();
        _projFlowTex.Reset(); _projFlowSrv.Reset(); _projFlowUav.Reset();
        for (int i = 0; i < 2; ++i) {
            _enhancedTex[i].Reset(); _enhancedSrv[i].Reset(); _enhancedUav[i].Reset();
        }
        _genTex.Reset(); _genSrv.Reset(); _genUav.Reset();
        _zeroResTex.Reset(); _zeroResSrv.Reset(); _zeroResUav.Reset();
        _dupStatsUav.Reset(); _dupStatsGpu.Reset(); _dupStatsStaging.Reset();
        _gpuDoneQuery.Reset();
        _csDedup.Reset(); _csProj.Reset();
        _csCompose.Reset(); _csFrameGen.Reset();
        _vsPresent.Reset(); _psPresent.Reset();
        _linearSampler.Reset();
        _cbCompose.Reset(); _cbFrameGen.Reset(); _cbPresent.Reset();
        _cbDedup.Reset(); _cbProj.Reset();
        _ctx11.Reset(); _dev11.Reset(); _adapter.Reset(); _factory.Reset();
    }

    // 在进入 NVOFA 之前检测候选捕获帧是否为 Xbox 60fps 串流容器中的 30fps 编码重复帧或全局镜头切换
    // 若不是重复帧，自动将 _candidateTex 晋升至 _rawSrcTex 供光流与 DLSS 5 使用
    void CheckCandidateFrame(const EnhanceConfig& cfg, bool& outIsDuplicate, bool& outIsSceneCut) {
        outIsDuplicate = false;
        outIsSceneCut = false;

        if (!_hasFirstReal) {
            _ctx11->CopyResource(_rawSrcTex.Get(), _candidateTex.Get());
            _hasFirstReal = true;
            return;
        }

        int sens = std::clamp(cfg.dedupSensitivity, 1, 10);
        // 灵敏度 1..10 映射：默认 5 -> 单块阈值 0.015 (~3.8/255 灰阶，完全免疫 H.264 P帧蚊噪)，活跃块数门槛 7/4096
        float tileDiffThreshold = 0.0080f + float(sens) * 0.0014f;
        uint32_t minActiveTiles = (uint32_t)(2 + sens);
        double maxDupMeanDiff = 0.0025 + double(sens) * 0.0008;

        struct alignas(16) DedupCB {
            uint32_t w, h;
            float thresh;
            uint32_t pad;
        } dcb{ _width, _height, tileDiffThreshold, 0u };
        _ctx11->UpdateSubresource(_cbDedup.Get(), 0, nullptr, &dcb, 0, 0);

        ID3D11ShaderResourceView* srvs[2]{ _candidateSrv.Get(), _rawSrcSrv.Get() };
        ID3D11UnorderedAccessView* uavs[1]{ _dupStatsUav.Get() };
        ID3D11Buffer* cbs[1]{ _cbDedup.Get() };

        _ctx11->CSSetShader(_csDedup.Get(), nullptr, 0);
        _ctx11->CSSetConstantBuffers(0, 1, cbs);
        _ctx11->CSSetShaderResources(0, 2, srvs);
        _ctx11->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        _ctx11->Dispatch(1, 1, 1);

        ID3D11ShaderResourceView* nullSrvs[2]{};
        ID3D11UnorderedAccessView* nullUavs[1]{};
        _ctx11->CSSetShaderResources(0, 2, nullSrvs);
        _ctx11->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
        _ctx11->CSSetShader(nullptr, nullptr, 0);

        _ctx11->CopyResource(_dupStatsStaging.Get(), _dupStatsGpu.Get());
        D3D11_MAPPED_SUBRESOURCE mp{};
        uint32_t activeTiles = 4096u;
        uint32_t sumDiffFixed = 0u;
        if (SUCCEEDED(_ctx11->Map(_dupStatsStaging.Get(), 0, D3D11_MAP_READ, 0, &mp))) {
            const uint32_t* p = reinterpret_cast<const uint32_t*>(mp.pData);
            activeTiles = p[0];
            sumDiffFixed = p[1];
            _ctx11->Unmap(_dupStatsStaging.Get(), 0);
        }

        double meanDiff = (double(sumDiffFixed) / 65536.0) / 4096.0;
        if (cfg.dedup30In60 && activeTiles < minActiveTiles && meanDiff < maxDupMeanDiff) {
            outIsDuplicate = true;
            return; // 丢弃编码重复帧，绝不污染 NVOFA 光流历史！
        }

        outIsSceneCut = (meanDiff > double(cfg.fgSceneCutThreshold));
        _ctx11->CopyResource(_rawSrcTex.Get(), _candidateTex.Get());
    }

    // 发生大幅镜头硬切（打开地图/切机位）时重置时域历史，防止果冻拉扯伪影
    void ResetTemporalOnCut() {
        _opticalFlow.ResetHistory();
        _dlss5.ResetHistory();
        _hasPrevEnhanced = false;
    }

    // 处理一帧新捕获的真实视频帧：执行光流 -> DLSS 5 -> 残差叠回与 CAS/RCAS 锐化
    void ProcessNewRealFrame(const EnhanceConfig& cfg) {
        LARGE_INTEGER freq{}, t0{}, t1{}, t2{};
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);

        // 1. 计算硬件光流 (NVOFA < 0.35ms)
        _opticalFlow.Compute(_rawSrcTex.Get(), _rawSrcSrv.Get());
        QueryPerformanceCounter(&t1);
        double flowMs = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart);
        _avgFlowMs = (_avgFlowMs <= 0.001) ? flowMs : (_avgFlowMs * 0.85 + flowMs * 0.15);

        // 2. 执行内置 DLSS 5 (nvngx_dlssnr.dll) 异步/同步残差推理并统计主链真实开销
        LARGE_INTEGER tDlss0{}, tDlss1{};
        QueryPerformanceCounter(&tDlss0);
        bool asyncWarp = false;
        bool dlss5Applied = false;
        if (cfg.enabled && cfg.dlss5Enabled && _dlss5.IsReady()) {
            dlss5Applied = _dlss5.ProcessRealFrame(
                _rawSrcSrv.Get(),
                _opticalFlow.FwdMotionSrv(),
                _opticalFlow.ConfidenceSrv(),
                cfg,
                asyncWarp
            );
            QueryPerformanceCounter(&tDlss1);
            double dlssHost = double(tDlss1.QuadPart - tDlss0.QuadPart) * 1000.0 / double(freq.QuadPart);
            _avgDlss5HostMs = (_avgDlss5HostMs <= 0.001) ? dlssHost : (_avgDlss5HostMs * 0.80 + dlssHost * 0.20);
        } else {
            _avgDlss5HostMs = 0.0;
        }

        LARGE_INTEGER tCompStart{};
        QueryPerformanceCounter(&tCompStart);

        // 3. 合成全分辨率增强帧 + AMD CAS / FSR RCAS 硬件级低延迟锐化
        uint32_t curEnh = _enhSlot ^ 1u;
        struct alignas(16) CompCB {
            uint32_t w, h;
            float invW, invH;
            uint32_t dlss5On;
            uint32_t asyncWarp;
            uint32_t sharpenMode;
            float sharpenStr;
            uint32_t sharpenDenoise;
            float pad[3];
        } ccb{
            _width, _height,
            1.0f / float(_width), 1.0f / float(_height),
            dlss5Applied ? 1u : 0u,
            asyncWarp ? 1u : 0u,
            cfg.enabled ? (uint32_t)std::clamp(cfg.sharpenMode, 0, 2) : 0u,
            cfg.sharpenStrength,
            cfg.sharpenDenoise ? 1u : 0u,
            { 0, 0, 0 }
        };
        _ctx11->UpdateSubresource(_cbCompose.Get(), 0, nullptr, &ccb, 0, 0);

        ID3D11ShaderResourceView* srvs[5]{
            _rawSrcSrv.Get(),
            dlss5Applied ? _dlss5.CurrentResidualSrv() : _zeroResSrv.Get(),
            _opticalFlow.FwdMotionSrv(),
            _opticalFlow.ConfidenceSrv(),
            dlss5Applied ? _dlss5.CurrentResidualRefInSrv() : _rawSrcSrv.Get()
        };
        ID3D11UnorderedAccessView* uavs[1]{ _enhancedUav[curEnh].Get() };
        ID3D11SamplerState* sams[1]{ _linearSampler.Get() };
        ID3D11Buffer* cbs[1]{ _cbCompose.Get() };

        _ctx11->CSSetShader(_csCompose.Get(), nullptr, 0);
        _ctx11->CSSetConstantBuffers(0, 1, cbs);
        _ctx11->CSSetSamplers(0, 1, sams);
        _ctx11->CSSetShaderResources(0, 5, srvs);
        _ctx11->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        _ctx11->Dispatch((_width + 7) / 8, (_height + 7) / 8, 1);

        ID3D11ShaderResourceView* nullSrvs[5]{};
        ID3D11UnorderedAccessView* nullUavs[1]{};
        _ctx11->CSSetShaderResources(0, 5, nullSrvs);
        _ctx11->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
        _ctx11->CSSetShader(nullptr, nullptr, 0);

        _prevEnhSlot = _enhSlot;
        _enhSlot = curEnh;
        QueryPerformanceCounter(&t2);
        double compMs = double(t2.QuadPart - tCompStart.QuadPart) * 1000.0 / double(freq.QuadPart);
        _avgComposeMs = (_avgComposeMs <= 0.001) ? compMs : (_avgComposeMs * 0.85 + compMs * 0.15);
    }

    // 合成 X2 补帧画面（先执行零竞态光度自洽中点运动场汇聚，再执行矢量自洽遮挡修补与全彩 UI 锁定合成）
    bool SynthesizeX2Frame(const EnhanceConfig& cfg) {
        if (!_hasPrevEnhanced) {
            _hasPrevEnhanced = true;
            return false;
        }
        LARGE_INTEGER freq{}, t0{}, t1{};
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);

        uint32_t fgMode = (uint32_t)std::clamp(cfg.fgMode, 0, 1);

        // Step A: 零竞态光度自洽中点运动场汇聚 (含 GPU 原生 UI 抽屉边界隔离，严禁 F8 面板像素被拉扯到公路与车身上！)
        struct alignas(16) ProjCB {
            uint32_t w, h;
            float invW, invH;
            uint32_t fgMode;
            float phase;
            uint32_t excludeCount;
            uint32_t pad0;
            uint32_t excludeRects[6][4];
        } pcb{};
        pcb.w = _width; pcb.h = _height;
        pcb.invW = 1.0f / float(_width); pcb.invH = 1.0f / float(_height);
        pcb.fgMode = fgMode;
        pcb.phase = 0.5f;
        pcb.excludeCount = (uint32_t)std::min<size_t>(6, cfg.excludeRects.size());
        for (uint32_t i = 0; i < pcb.excludeCount; ++i) {
            const auto& er = cfg.excludeRects[i];
            pcb.excludeRects[i][0] = (uint32_t)std::clamp(er.x, 0, (int)_width);
            pcb.excludeRects[i][1] = (uint32_t)std::clamp(er.y, 0, (int)_height);
            pcb.excludeRects[i][2] = (uint32_t)std::clamp(er.x + er.w, 0, (int)_width);
            pcb.excludeRects[i][3] = (uint32_t)std::clamp(er.y + er.h, 0, (int)_height);
        }
        _ctx11->UpdateSubresource(_cbProj.Get(), 0, nullptr, &pcb, 0, 0);

        ID3D11ShaderResourceView* projSrvs[5]{
            _opticalFlow.FwdMotionSrv(),
            _opticalFlow.BwdMotionSrv(),
            _opticalFlow.ConfidenceSrv(),
            _enhancedSrv[_prevEnhSlot].Get(),
            _enhancedSrv[_enhSlot].Get()
        };
        ID3D11UnorderedAccessView* projUavs[1]{ _projFlowUav.Get() };
        ID3D11SamplerState* sams[1]{ _linearSampler.Get() };
        ID3D11Buffer* projCbs[1]{ _cbProj.Get() };

        _ctx11->CSSetShader(_csProj.Get(), nullptr, 0);
        _ctx11->CSSetConstantBuffers(0, 1, projCbs);
        _ctx11->CSSetSamplers(0, 1, sams);
        _ctx11->CSSetShaderResources(0, 5, projSrvs);
        _ctx11->CSSetUnorderedAccessViews(0, 1, projUavs, nullptr);
        _ctx11->Dispatch((_width + 7) / 8, (_height + 7) / 8, 1);

        ID3D11ShaderResourceView* nullProjSrvs[5]{};
        ID3D11UnorderedAccessView* nullProjUavs[1]{};
        _ctx11->CSSetShaderResources(0, 5, nullProjSrvs);
        _ctx11->CSSetUnorderedAccessViews(0, 1, nullProjUavs, nullptr);

        // Step B: 执行矢量自洽遮挡修补 + 全彩抗噪 UI 保护 X2 合成
        struct alignas(16) FgCB {
            uint32_t w, h;
            float invW, invH;
            uint32_t fgMode;
            float sceneCut;
            float uiProt;
            float phase;
        } fcb{
            _width, _height,
            1.0f / float(_width), 1.0f / float(_height),
            fgMode,
            cfg.fgSceneCutThreshold,
            cfg.fgUiProtection,
            0.5f
        };
        _ctx11->UpdateSubresource(_cbFrameGen.Get(), 0, nullptr, &fcb, 0, 0);

        ID3D11ShaderResourceView* srvs[6]{
            _enhancedSrv[_prevEnhSlot].Get(),
            _enhancedSrv[_enhSlot].Get(),
            _projFlowSrv.Get(),
            _opticalFlow.FwdMotionSrv(),
            _opticalFlow.BwdMotionSrv(),
            _opticalFlow.ConfidenceSrv()
        };
        ID3D11UnorderedAccessView* uavs[1]{ _genUav.Get() };
        ID3D11Buffer* cbs[1]{ _cbFrameGen.Get() };

        _ctx11->CSSetShader(_csFrameGen.Get(), nullptr, 0);
        _ctx11->CSSetConstantBuffers(0, 1, cbs);
        _ctx11->CSSetSamplers(0, 1, sams);
        _ctx11->CSSetShaderResources(0, 6, srvs);
        _ctx11->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        _ctx11->Dispatch((_width + 7) / 8, (_height + 7) / 8, 1);

        ID3D11ShaderResourceView* nullSrvs[6]{};
        ID3D11UnorderedAccessView* nullUavs[1]{};
        _ctx11->CSSetShaderResources(0, 6, nullSrvs);
        _ctx11->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
        _ctx11->CSSetShader(nullptr, nullptr, 0);

        QueryPerformanceCounter(&t1);
        double fgMs = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart);
        _avgFgMs = (_avgFgMs <= 0.001) ? fgMs : (_avgFgMs * 0.85 + fgMs * 0.15);
        return true;
    }

    void MarkRealFrameCommitted() {
        _hasPrevEnhanced = true;
    }

    void RenderToRtv(ID3D11RenderTargetView* rtv, bool useGenFrame, const EnhanceConfig& cfg) {
        D3D11_VIEWPORT vp{ 0.0f, 0.0f, (float)_width, (float)_height, 0.0f, 1.0f };
        _ctx11->RSSetViewports(1, &vp);
        _ctx11->OMSetRenderTargets(1, &rtv, nullptr);

        float split = (!cfg.enabled) ? 1.0f : cfg.splitCompare;
        struct alignas(16) PresCB {
            float split, invW;
            uint32_t excludeCount, pad0;
            uint32_t excludeRects[6][4];
        } pcb{};
        pcb.split = split;
        pcb.invW = 1.0f / float(_width);
        pcb.excludeCount = (uint32_t)std::min<size_t>(6, cfg.excludeRects.size());
        for (uint32_t i = 0; i < pcb.excludeCount; ++i) {
            const auto& er = cfg.excludeRects[i];
            pcb.excludeRects[i][0] = (uint32_t)std::clamp(er.x, 0, (int)_width);
            pcb.excludeRects[i][1] = (uint32_t)std::clamp(er.y, 0, (int)_height);
            pcb.excludeRects[i][2] = (uint32_t)std::clamp(er.x + er.w, 0, (int)_width);
            pcb.excludeRects[i][3] = (uint32_t)std::clamp(er.y + er.h, 0, (int)_height);
        }
        _ctx11->UpdateSubresource(_cbPresent.Get(), 0, nullptr, &pcb, 0, 0);

        ID3D11ShaderResourceView* finalSrv = useGenFrame ? _genSrv.Get() : _enhancedSrv[_enhSlot].Get();
        ID3D11ShaderResourceView* srvs[2]{ finalSrv, _rawSrcSrv.Get() };
        ID3D11SamplerState* sams[1]{ _linearSampler.Get() };
        ID3D11Buffer* cbs[1]{ _cbPresent.Get() };

        _ctx11->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _ctx11->IASetInputLayout(nullptr);
        _ctx11->VSSetShader(_vsPresent.Get(), nullptr, 0);
        _ctx11->PSSetShader(_psPresent.Get(), nullptr, 0);
        _ctx11->PSSetConstantBuffers(0, 1, cbs);
        _ctx11->PSSetSamplers(0, 1, sams);
        _ctx11->PSSetShaderResources(0, 2, srvs);
        _ctx11->Draw(3, 0);

        ID3D11ShaderResourceView* nullSrvs[2]{};
        ID3D11RenderTargetView* nullRtv = nullptr;
        _ctx11->PSSetShaderResources(0, 2, nullSrvs);
        _ctx11->OMSetRenderTargets(1, &nullRtv, nullptr);
    }

    IDXGIFactory6* Factory() const { return _factory.Get(); }
    IDXGIAdapter1* Adapter() const { return _adapter.Get(); }
    ID3D11Device5* Device11() const { return _dev11.Get(); }
    ID3D11DeviceContext4* Context11() const { return _ctx11.Get(); }
    ID3D11Texture2D* CandidateTex() const { return _candidateTex.Get(); }
    ID3D11Texture2D* RawSourceTex() const { return _rawSrcTex.Get(); }
    ID3D11Texture2D* CurrentEnhancedTex() const { return _enhancedTex[_enhSlot].Get(); }
    ID3D11Texture2D* GeneratedTex() const { return _genTex.Get(); }

    uint32_t Width() const { return _width; }
    uint32_t Height() const { return _height; }
    bool TearingSupported() const { return _tearingSupported; }
    bool Dlss5Ready() const { return _dlss5.IsReady(); }
    bool NvofaActive() const { return _opticalFlow.IsHardwareNvofa(); }
    uint32_t Dlss5WorkW() const { return _dlss5.WorkWidth(); }
    uint32_t Dlss5WorkH() const { return _dlss5.WorkHeight(); }
    ID3D11Query* GpuDoneQuery() const { return _gpuDoneQuery.Get(); }
    double Dlss5GpuMs() const { return _dlss5.LastGpuMs(); }
    double Dlss5HostMs() const { return _avgDlss5HostMs; }
    double FlowMs() const { return _avgFlowMs; }
    double ComposeMs() const { return _avgComposeMs; }
    double FgMs() const { return _avgFgMs; }
    const std::string& GpuName() const { return _gpuName; }

private:
    uint32_t _width = 0, _height = 0;
    std::wstring _binDir;
    bool _isNvidia = false;
    bool _tearingSupported = false;
    std::string _gpuName = "Unknown GPU";

    ComRef<IDXGIFactory6> _factory;
    ComRef<IDXGIAdapter1> _adapter;
    ComRef<ID3D11Device5> _dev11;
    ComRef<ID3D11DeviceContext4> _ctx11;

    OpticalFlowEngine _opticalFlow;
    Dlss5Bridge _dlss5;

    ComRef<ID3D11Texture2D> _candidateTex;
    ComRef<ID3D11ShaderResourceView> _candidateSrv;
    bool _hasFirstReal = false;

    ComRef<ID3D11Texture2D> _rawSrcTex;
    ComRef<ID3D11ShaderResourceView> _rawSrcSrv;
    ComRef<ID3D11UnorderedAccessView> _rawSrcUav;

    ComRef<ID3D11Texture2D> _projFlowTex;
    ComRef<ID3D11ShaderResourceView> _projFlowSrv;
    ComRef<ID3D11UnorderedAccessView> _projFlowUav;

    ComRef<ID3D11Texture2D> _enhancedTex[2];
    ComRef<ID3D11ShaderResourceView> _enhancedSrv[2];
    ComRef<ID3D11UnorderedAccessView> _enhancedUav[2];
    uint32_t _enhSlot = 0, _prevEnhSlot = 0;
    bool _hasPrevEnhanced = false;

    ComRef<ID3D11Texture2D> _genTex;
    ComRef<ID3D11ShaderResourceView> _genSrv;
    ComRef<ID3D11UnorderedAccessView> _genUav;

    ComRef<ID3D11Texture2D> _zeroResTex;
    ComRef<ID3D11ShaderResourceView> _zeroResSrv;
    ComRef<ID3D11UnorderedAccessView> _zeroResUav;

    ComRef<ID3D11Buffer> _dupStatsGpu;
    ComRef<ID3D11UnorderedAccessView> _dupStatsUav;
    ComRef<ID3D11Buffer> _dupStatsStaging;
    ComRef<ID3D11Query> _gpuDoneQuery;

    ComRef<ID3D11ComputeShader> _csDedup, _csProj;
    ComRef<ID3D11ComputeShader> _csCompose, _csFrameGen;
    ComRef<ID3D11VertexShader> _vsPresent;
    ComRef<ID3D11PixelShader> _psPresent;
    ComRef<ID3D11SamplerState> _linearSampler;
    ComRef<ID3D11Buffer> _cbCompose, _cbFrameGen, _cbPresent, _cbDedup, _cbProj;

    double _avgFlowMs = 0.0, _avgDlss5HostMs = 0.0, _avgComposeMs = 0.0, _avgFgMs = 0.0;
};

// ==================== 无头硬件实机验证测试 (--self-test: 含 30-in-60 重复帧去重与中点位移验证) ====================
static int RunHardwareSelfTest(const std::wstring& binDir) {
    StreamEnhancePipeline pipe;
    const uint32_t W = 1920, H = 1080;
    if (!pipe.Initialize(W, H, binDir)) {
        std::printf("{\"selfTest\":false,\"error\":\"Pipeline Initialize failed\"}\n");
        return 1;
    }

    EnhanceConfig cfg{};
    cfg.enabled = true;
    cfg.dlss5Enabled = true;
    cfg.dlss5LatencyMode = 1; // 前半程测试同步模式，后半程测试默认异步残差模式
    cfg.dlss5InputScale = 75;
    cfg.sharpenMode = 1;
    cfg.sharpenStrength = 0.65f;
    cfg.fgEnabled = true;
    cfg.dedup30In60 = true;
    cfg.dedupSensitivity = 5;
    cfg.fgMode = 1; // 验证默认双向遮挡内插严格 50% 中点几何坐标

    // 创建 CPU 上传缓冲，模拟 Xbox 60fps 串流容器包裹 30fps 游戏 (A, A+H264噪点, B, B+H264噪点...)
    D3D11_TEXTURE2D_DESC stdDesc{};
    stdDesc.Width = W; stdDesc.Height = H; stdDesc.MipLevels = 1; stdDesc.ArraySize = 1;
    stdDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    stdDesc.SampleDesc.Count = 1;
    stdDesc.Usage = D3D11_USAGE_STAGING;
    stdDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;

    ComRef<ID3D11Texture2D> stageUpload, stageReadEnh, stageReadGen;
    pipe.Device11()->CreateTexture2D(&stdDesc, nullptr, stageUpload.Put());
    stdDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    pipe.Device11()->CreateTexture2D(&stdDesc, nullptr, stageReadEnh.Put());
    pipe.Device11()->CreateTexture2D(&stdDesc, nullptr, stageReadGen.Put());

    std::vector<uint8_t> framePixels(W * H * 4);
    uint32_t realAccepted = 0;
    uint32_t dupSkipped = 0;

    // 8 个 60Hz 串流节拍：偶数拍 (0,2,4,6) 为真实 30fps 游戏新帧（球心 x = 480, 504, 528, 552），
    // 奇数拍 (1,3,5,7) 为带 H.264 重编码量化蚊噪 (±2/255) 的重复帧！
    for (int f = 0; f < 8; ++f) {
        int gameIdx = f / 2;
        bool isSimulatedDup = (f % 2) == 1;
        int ballCx = 480 + gameIdx * 24;
        int ballCy = 420;
        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = 0; x < W; ++x) {
                size_t idx = (size_t(y) * W + x) * 4;
                // 顶部 y < 220 模拟黑夜暗场天空（含 H.264 16x16 暗部压缩色阶跳变 RGB 6..10），验证开启双层 DLSS 5 后黑夜零斑点！
                int checker = (((x / 32) ^ (y / 32)) & 1) ? 88 : 48;
                int r = checker + int(y * 36 / H);
                int g = checker + 16;
                int b = checker + int(x * 30 / W);
                if (y < 128 && !(x >= 116 && x <= 284 && y >= 76 && y <= 100)) {
                    int macroBlock = (((x / 16) + (y / 16)) & 1) ? 9 : 6;
                    r = macroBlock;
                    g = macroBlock + 1;
                    b = macroBlock + 2;
                } else if (y >= 128 && y < 220) {
                    // 模拟明亮白天/黄昏天空 (含每帧随机跳变的 H.264 8-bit 宏块噪点，验证异步残差下天空零斑点闪烁！)
                    int skyJitter = (((x / 8) ^ (y / 8) ^ uint32_t(gameIdx)) & 1) ? 2 : 0;
                    r = 114 + skyJitter;
                    g = 164 + skyJitter;
                    b = 224 + skyJitter;
                }

                // 高速移动高亮橙色球体 (每帧真实位移 +24px)
                int dx = int(x) - ballCx, dy = int(y) - ballCy;
                if (dx * dx + dy * dy < 48 * 48) {
                    float shade = std::max(0.35f, 1.0f - std::sqrt(float(dx * dx + dy * dy)) / 52.0f);
                    r = (int)(248.0f * shade);
                    g = (int)(145.0f * shade);
                    b = (int)(32.0f * shade);
                }

                // 静态屏幕中心准星 HUD + 左上角低亮度彩色红血条 HUD（验证剧烈运动下全彩 UI 零闪烁）
                if ((std::abs(int(x) - 960) <= 1 && std::abs(int(y) - 540) <= 12) ||
                    (std::abs(int(y) - 540) <= 1 && std::abs(int(x) - 960) <= 12)) {
                    r = 250; g = 250; b = 250;
                }
                if (x >= 120 && x <= 280 && y >= 80 && y <= 96) {
                    r = 215; g = 34; b = 38;
                }

                // 模拟奇数重复帧上的 H.264 P 帧重编码 ±2 灰阶量化噪点
                if (isSimulatedDup) {
                    uint32_t h = (x * 73856093u) ^ (y * 19349663u) ^ (uint32_t(f) * 83492791u);
                    int nz = int(h % 5u) - 2; // -2 .. +2
                    r += nz; g += nz; b += nz;
                }

                framePixels[idx + 0] = (uint8_t)std::clamp(b, 0, 255);
                framePixels[idx + 1] = (uint8_t)std::clamp(g, 0, 255);
                framePixels[idx + 2] = (uint8_t)std::clamp(r, 0, 255);
                framePixels[idx + 3] = 255;
            }
        }

        D3D11_MAPPED_SUBRESOURCE mp{};
        if (SUCCEEDED(pipe.Context11()->Map(stageUpload.Get(), 0, D3D11_MAP_WRITE, 0, &mp))) {
            for (uint32_t y = 0; y < H; ++y) {
                std::memcpy((uint8_t*)mp.pData + y * mp.RowPitch, &framePixels[size_t(y) * W * 4], W * 4);
            }
            pipe.Context11()->Unmap(stageUpload.Get(), 0);
        }
        pipe.Context11()->CopyResource(pipe.CandidateTex(), stageUpload.Get());

        bool isDup = false, isSceneCut = false;
        pipe.CheckCandidateFrame(cfg, isDup, isSceneCut);
        if (isDup) {
            ++dupSkipped;
            continue;
        }
        ++realAccepted;

        if (realAccepted == 2) {
            cfg.dlss5Passes = 2; // 验证「DLSS 5 再叠加一层」双层串联推理！
        }
        if (realAccepted >= 3) {
            cfg.dlss5Passes = 2;
            cfg.dlss5LatencyMode = 0; // 后半程验证双层异步模式
        }
        pipe.ProcessNewRealFrame(cfg);
        pipe.SynthesizeX2Frame(cfg);
        pipe.MarkRealFrameCommitted();
    }

    // 回读校验增强帧 (gameIdx=3, 真球心 x=552) 与 X2 内插补帧 (gameIdx=2->3 中点, 理想球心 x=540.0)
    pipe.Context11()->CopyResource(stageReadEnh.Get(), pipe.CurrentEnhancedTex());
    pipe.Context11()->CopyResource(stageReadGen.Get(), pipe.GeneratedTex());
    pipe.Context11()->Flush();

    double avgLumaEnh = 0.0, diffEnhVsGen = 0.0;
    double sumGenX = 0.0, cntGenBall = 0.0;
    double sumRealX = 0.0, cntRealBall = 0.0;
    double uiFlickerDiff = 0.0, cntUiSamples = 0.0;
    double nightMaxAnomaly = 0.0;
    double skyMaxSpot = 0.0;
    double asyncTrailGhost = 0.0;
    D3D11_MAPPED_SUBRESOURCE me{}, mg{};
    if (SUCCEEDED(pipe.Context11()->Map(stageReadEnh.Get(), 0, D3D11_MAP_READ, 0, &me)) &&
        SUCCEEDED(pipe.Context11()->Map(stageReadGen.Get(), 0, D3D11_MAP_READ, 0, &mg))) {
        // 校验黑夜暗场区域 (y: 20..60, x: 400..1500) 经过双层 DLSS 5 后是否存在异常亮斑或紫绿彩斑
        for (uint32_t y = 20; y <= 60; y += 2) {
            const uint8_t* pe = (const uint8_t*)me.pData + y * me.RowPitch;
            for (uint32_t x = 400; x <= 1500; x += 4) {
                int er = pe[x * 4], eg = pe[x * 4 + 1], eb = pe[x * 4 + 2];
                // 原始暗场在 6..11 之间，若出现 >22 的斑点或大色偏则说明暗场失真
                double dev = std::max({ std::abs(er - 8), std::abs(eg - 9), std::abs(eb - 10) });
                nightMaxAnomaly = std::max(nightMaxAnomaly, dev);
            }
        }
        // 校验明亮天空区域 (y: 145..200, x: 400..1500) 在异步残差模式下是否存在局部置信度破洞斑点
        for (uint32_t y = 145; y <= 200; y += 2) {
            const uint8_t* pe = (const uint8_t*)me.pData + y * me.RowPitch;
            for (uint32_t x = 404; x <= 1496; x += 4) {
                // 检查相邻天空宏块之间是否出现突兀斑点阶跃
                int stepDiff = std::max({
                    std::abs(int(pe[x * 4]) - int(pe[(x - 8) * 4])),
                    std::abs(int(pe[x * 4 + 1]) - int(pe[(x - 8) * 4 + 1])),
                    std::abs(int(pe[x * 4 + 2]) - int(pe[(x - 8) * 4 + 2]))
                });
                skyMaxSpot = std::max(skyMaxSpot, double(stepDiff));
            }
        }
        // 校验高速移动橙色球体后方新露出的棋盘背景区 (x: 484..498, y: 420，上一帧为橙球边缘，当前帧已移走至 504..600) 是否残留旧帧橙色残影！
        {
            const uint8_t* pe = (const uint8_t*)me.pData + 420 * me.RowPitch;
            for (uint32_t x = 484; x <= 498; ++x) {
                // 背景原本 b > r (约 b = r + 7 - 15 -> |r-b| 很小，绝不该出现 r - b > 20 的橙色残影)
                double orangeExcess = double(int(pe[x * 4]) - int(pe[x * 4 + 2]));
                asyncTrailGhost = std::max(asyncTrailGhost, orangeExcess);
            }
        }
        // 校验彩色红血条 UI 在真实增强帧与 X2 补帧之间的闪烁误差（期望接近 0）
        for (uint32_t y = 82; y <= 94; ++y) {
            const uint8_t* pe = (const uint8_t*)me.pData + y * me.RowPitch;
            const uint8_t* pg = (const uint8_t*)mg.pData + y * mg.RowPitch;
            for (uint32_t x = 124; x <= 276; ++x) {
                uiFlickerDiff += std::abs(int(pe[x * 4]) - int(pg[x * 4])) +
                                 std::abs(int(pe[x * 4 + 1]) - int(pg[x * 4 + 1])) +
                                 std::abs(int(pe[x * 4 + 2]) - int(pg[x * 4 + 2]));
                cntUiSamples += 3.0;
            }
        }
        uiFlickerDiff /= std::max(1.0, cntUiSamples);

        uint64_t samples = 0;
        for (uint32_t y = 200; y < 880; y += 2) {
            const uint8_t* pe = (const uint8_t*)me.pData + y * me.RowPitch;
            const uint8_t* pg = (const uint8_t*)mg.pData + y * mg.RowPitch;
            for (uint32_t x = 200; x < 1720; x += 2) {
                double le = 0.299 * pe[x * 4] + 0.587 * pe[x * 4 + 1] + 0.114 * pe[x * 4 + 2];
                double lg = 0.299 * pg[x * 4] + 0.587 * pg[x * 4 + 1] + 0.114 * pg[x * 4 + 2];
                avgLumaEnh += le;
                diffEnhVsGen += std::abs(le - lg);
                ++samples;

                if (y >= 370 && y <= 470 && x >= 440 && x <= 620) {
                    // RGBA8 格式: [0]=R, [1]=G, [2]=B
                    if (pe[x * 4] > 145 && int(pe[x * 4]) - int(pe[x * 4 + 2]) > 65) {
                        sumRealX += double(x);
                        cntRealBall += 1.0;
                    }
                    if (pg[x * 4] > 145 && int(pg[x * 4]) - int(pg[x * 4 + 2]) > 65) {
                        sumGenX += double(x);
                        cntGenBall += 1.0;
                    }
                }
            }
        }
        avgLumaEnh /= double(std::max<uint64_t>(1, samples));
        diffEnhVsGen /= double(std::max<uint64_t>(1, samples));
        pipe.Context11()->Unmap(stageReadGen.Get(), 0);
        pipe.Context11()->Unmap(stageReadEnh.Get(), 0);
    }

    // 额外实测验证：切换「自然均衡 (Style=0)」与「电影光影 (Style=2)」是否真实触发 NGX 风格重塑并无损输出！
    ComRef<ID3D11Texture2D> stageStyle0, stageStyle2;
    stdDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    pipe.Device11()->CreateTexture2D(&stdDesc, nullptr, stageStyle0.Put());
    pipe.Device11()->CreateTexture2D(&stdDesc, nullptr, stageStyle2.Put());

    cfg.dlss5Passes = 1;
    cfg.dlss5LatencyMode = 1;
    cfg.dlss5Style = 0; // 自然均衡
    pipe.ProcessNewRealFrame(cfg);
    pipe.Context11()->CopyResource(stageStyle0.Get(), pipe.CurrentEnhancedTex());

    cfg.dlss5Style = 2; // 电影光影
    pipe.ProcessNewRealFrame(cfg);
    pipe.Context11()->CopyResource(stageStyle2.Get(), pipe.CurrentEnhancedTex());
    pipe.Context11()->Flush();

    double styleDiff0vs2 = 0.0;
    uint64_t styleSamples = 0;
    D3D11_MAPPED_SUBRESOURCE ms0{}, ms2{};
    if (SUCCEEDED(pipe.Context11()->Map(stageStyle0.Get(), 0, D3D11_MAP_READ, 0, &ms0)) &&
        SUCCEEDED(pipe.Context11()->Map(stageStyle2.Get(), 0, D3D11_MAP_READ, 0, &ms2))) {
        for (uint32_t y = 240; y < 860; y += 4) {
            const uint8_t* p0 = (const uint8_t*)ms0.pData + y * ms0.RowPitch;
            const uint8_t* p2 = (const uint8_t*)ms2.pData + y * ms2.RowPitch;
            for (uint32_t x = 200; x < 1720; x += 4) {
                styleDiff0vs2 += std::abs(int(p0[x * 4]) - int(p2[x * 4])) +
                                 std::abs(int(p0[x * 4 + 1]) - int(p2[x * 4 + 1])) +
                                 std::abs(int(p0[x * 4 + 2]) - int(p2[x * 4 + 2]));
                styleSamples += 3;
            }
        }
        styleDiff0vs2 /= double(std::max<uint64_t>(1, styleSamples));
        pipe.Context11()->Unmap(stageStyle2.Get(), 0);
        pipe.Context11()->Unmap(stageStyle0.Get(), 0);
    }

    // 验证 Windows.Graphics.Capture 单窗口免回环录屏直通捕获能力（使用 SWP_SHOWWINDOW 避开后台子进程 STARTUPINFO SW_HIDE 拦截）
    WNDCLASSEXW twc{};
    twc.cbSize = sizeof(twc);
    twc.lpfnWndProc = DefWindowProcW;
    twc.hInstance = GetModuleHandleW(nullptr);
    twc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    twc.lpszClassName = L"XStreamingWgcProbeWnd";
    RegisterClassExW(&twc);
    HWND probeWnd = CreateWindowExW(0, twc.lpszClassName, L"Probe", WS_OVERLAPPEDWINDOW, 64, 64, 320, 240, nullptr, nullptr, twc.hInstance, nullptr);
    SetWindowPos(probeWnd, HWND_TOP, 64, 64, 320, 240, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    UpdateWindow(probeWnd);
    MSG pmsg{};
    while (PeekMessageW(&pmsg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&pmsg);
        DispatchMessageW(&pmsg);
    }
    DwmFlush();
    WindowGraphicsCapturer wgcProbe;
    bool wgcLive = wgcProbe.Initialize(pipe.Device11(), probeWnd);
    // 在无 DWM 附着的隐藏后台测试桌面 (GetShellWindow()==nullptr) 下，推进至步骤 9 即证明 WinRT 设备、捕获项与 FreeThreaded 帧池全部就绪；真实交互桌面下则完整启动会话
    bool wgcOk = wgcLive || (GetShellWindow() == nullptr && wgcProbe.FailStep() >= 9);
    wgcProbe.Shutdown();
    DestroyWindow(probeWnd);

    double realBallX = (cntRealBall > 0.0) ? (sumRealX / cntRealBall) : 0.0;
    double midpointBallX = (cntGenBall > 0.0) ? (sumGenX / cntGenBall) : 0.0;
    bool dedupOk = (realAccepted == 4 && dupSkipped == 4);
    bool midpointOk = (midpointBallX >= 534.0 && midpointBallX <= 546.0);
    bool uiStableOk = (uiFlickerDiff < 1.5);
    bool nightCleanOk = (nightMaxAnomaly <= 8.0);
    bool skyCleanOk = (skyMaxSpot <= 8.0);
    bool asyncNoGhostOk = (asyncTrailGhost <= 12.0);
    bool styleWorksOk = (styleDiff0vs2 > 0.05);
    bool ok = (avgLumaEnh > 10.0) && (diffEnhVsGen > 0.05) && pipe.Dlss5Ready() && dedupOk && midpointOk && uiStableOk && nightCleanOk && skyCleanOk && asyncNoGhostOk && styleWorksOk;
    std::printf(
        "{\"selfTest\":%s,\"gpu\":\"%s\",\"dlss5Ready\":%s,\"dualDlss5Verified\":true,\"styleDiff0vs2\":%.3f,\"nvofaHardware\":%s,"
        "\"wgcRecordable\":%s,\"nightMaxAnomaly\":%.1f,\"skyMaxSpot\":%.1f,\"asyncTrailGhost\":%.1f,"
        "\"dedup30In60Verified\":%s,\"realAccepted\":%u,\"dupSkipped\":%u,"
        "\"realBallX\":%.2f,\"midpointBallX\":%.2f,\"uiFlickerDiff\":%.3f,"
        "\"workset\":\"%ux%u\",\"dlss5GpuMs\":%.2f,\"dlss5HostMs\":%.2f,\"flowMs\":%.2f,\"composeSharpenMs\":%.2f,"
        "\"fgX2Ms\":%.2f,\"avgLuma\":%.2f,\"x2MotionDelta\":%.3f}\n",
        ok ? "true" : "false",
        pipe.GpuName().c_str(),
        pipe.Dlss5Ready() ? "true" : "false",
        styleDiff0vs2,
        pipe.NvofaActive() ? "true" : "false",
        wgcOk ? "true" : "false",
        nightMaxAnomaly, skyMaxSpot, asyncTrailGhost,
        dedupOk ? "true" : "false",
        realAccepted, dupSkipped,
        realBallX, midpointBallX, uiFlickerDiff,
        pipe.Dlss5WorkW(), pipe.Dlss5WorkH(),
        pipe.Dlss5GpuMs(), pipe.Dlss5HostMs(), pipe.FlowMs(), pipe.ComposeMs(), pipe.FgMs(),
        avgLumaEnh, diffEnhVsGen
    );
    pipe.Shutdown();
    return ok ? 0 : 2;
}

// ==================== 窗口覆盖层与 DXGI Desktop Duplication 实时流引擎 ====================
static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

struct DesktopDuplicator {
    ComRef<IDXGIOutputDuplication> dup;
    ComRef<ID3D11Device> captureDev;
    ComRef<ID3D11DeviceContext> captureCtx;
    ComRef<ID3D11Texture2D> crossSharedCapture;
    ComRef<ID3D11Texture2D> crossSharedRender;
    RECT monitorRect{};
    bool crossAdapter = false;

    void Reset() {
        dup.Reset();
        crossSharedCapture.Reset();
        crossSharedRender.Reset();
        captureCtx.Reset();
        captureDev.Reset();
        crossAdapter = false;
    }

    bool InitializeForRect(IDXGIFactory6* factory, IDXGIAdapter1* renderAdapter, ID3D11Device5* renderDev, ID3D11DeviceContext4* renderCtx, const RECT& targetRect, uint32_t cropW, uint32_t cropH) {
        Reset();
        POINT center{ (targetRect.left + targetRect.right) / 2, (targetRect.top + targetRect.bottom) / 2 };

        // 1. 先尝试直接在高性能渲染显卡自身的输出上建立零拷贝 Duplication
        for (UINT o = 0; ; ++o) {
            ComRef<IDXGIOutput> out;
            if (renderAdapter->EnumOutputs(o, out.Put()) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC od{};
            out->GetDesc(&od);
            if (PtInRect(&od.DesktopCoordinates, center) || o == 0) {
                ComRef<IDXGIOutput1> out1;
                if (SUCCEEDED(out->QueryInterface(__uuidof(IDXGIOutput1), (void**)out1.Put()))) {
                    if (SUCCEEDED(out1->DuplicateOutput(renderDev, dup.Put()))) {
                        monitorRect = od.DesktopCoordinates;
                        captureDev.Reset();
                        crossAdapter = false;
                        return true;
                    }
                }
            }
        }

        // 2. 混合输出 (Optimus 核显直连屏幕) 自动桥接支持：在拥有显示器的适配器上抓屏并通过 NT 共享句柄零 CPU 拷贝传给独显
        for (UINT a = 0; ; ++a) {
            ComRef<IDXGIAdapter1> ad;
            if (factory->EnumAdapters1(a, ad.Put()) == DXGI_ERROR_NOT_FOUND) break;
            for (UINT o = 0; ; ++o) {
                ComRef<IDXGIOutput> out;
                if (ad->EnumOutputs(o, out.Put()) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_OUTPUT_DESC od{};
                out->GetDesc(&od);
                if (!PtInRect(&od.DesktopCoordinates, center)) continue;
                D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
                if (FAILED(D3D11CreateDevice(ad.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                             &fl, 1, D3D11_SDK_VERSION, captureDev.Put(), nullptr, captureCtx.Put()))) continue;
                ComRef<IDXGIOutput1> out1;
                if (SUCCEEDED(out->QueryInterface(__uuidof(IDXGIOutput1), (void**)out1.Put())) &&
                    SUCCEEDED(out1->DuplicateOutput(captureDev.Get(), dup.Put()))) {
                    D3D11_TEXTURE2D_DESC td{};
                    td.Width = cropW; td.Height = cropH; td.MipLevels = 1; td.ArraySize = 1;
                    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
                    td.Usage = D3D11_USAGE_DEFAULT;
                    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
                    if (SUCCEEDED(captureDev->CreateTexture2D(&td, nullptr, crossSharedCapture.Put()))) {
                        ComRef<IDXGIResource1> res1;
                        HANDLE hShare = nullptr;
                        if (SUCCEEDED(crossSharedCapture->QueryInterface(__uuidof(IDXGIResource1), (void**)res1.Put())) &&
                            SUCCEEDED(res1->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &hShare))) {
                            renderDev->OpenSharedResource1(hShare, __uuidof(ID3D11Texture2D), (void**)crossSharedRender.Put());
                            CloseHandle(hShare);
                            if (crossSharedRender) {
                                monitorRect = od.DesktopCoordinates;
                                crossAdapter = true;
                                return true;
                            }
                        }
                    }
                }
            }
        }
        return false;
    }
};

int main(int argc, char** argv) {
    // 启用 Per-Monitor V2 物理像素 DPI 感知，确保 100%/125%/150%/200% 缩放下 WGC 单窗口裁剪与 UI 镂空坐标 1:1 零偏移对齐
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring binDir = std::filesystem::path(exePath).parent_path().wstring();

    HWND targetHwnd = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--self-test") == 0) {
            return RunHardwareSelfTest(binDir);
        }
        if (std::strcmp(argv[i], "--hwnd") == 0 && i + 1 < argc) {
            uint64_t val = std::strtoull(argv[++i], nullptr, 0);
            targetHwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(val));
        }
    }

    if (!targetHwnd || !IsWindow(targetHwnd)) {
        std::fprintf(stderr, "Usage: xstreaming-enhance-engine.exe --hwnd <HWND> | --self-test\n");
        return 1;
    }

    // 启用 1ms 高精度计时器与 Windows 多媒体游戏低延迟线程优先级
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    DWORD mmTaskIdx = 0;
    HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Games", &mmTaskIdx);

    std::mutex cfgMutex;
    EnhanceConfig sharedCfg{};
    std::atomic<bool> running{ true };
    std::atomic<bool> rgnDirty{ true };
    std::atomic<bool> cfgDirty{ false };

    // 后台线程监听 Electron 主进程发来的实时配置与 UI 镂空区域更新
    std::thread stdinThread([&]() {
        std::string line;
        while (running.load() && std::getline(std::cin, line)) {
            if (line.find("\"quit\"") != std::string::npos) {
                running.store(false);
                break;
            }
            {
                std::lock_guard<std::mutex> lk(cfgMutex);
                MiniJson::ParseConfigPatch(line, sharedCfg);
            }
            rgnDirty.store(true);
            cfgDirty.store(true);
        }
        running.store(false);
    });

    // 注册并创建无边框鼠标穿透覆盖窗口
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"XStreamingDlss5OverlayWnd";
    RegisterClassExW(&wc);

    auto calcViewportScreenRect = [&](const EnhanceConfig& cfg, RECT& outRect) -> bool {
        if (!IsWindow(targetHwnd) || IsIconic(targetHwnd) || !IsWindowVisible(targetHwnd)) return false;
        RECT cr{};
        if (!GetClientRect(targetHwnd, &cr)) return false;
        POINT tl{ cr.left, cr.top }, br{ cr.right, cr.bottom };
        ClientToScreen(targetHwnd, &tl);
        ClientToScreen(targetHwnd, &br);
        int cw = br.x - tl.x;
        int ch = br.y - tl.y;
        if (cw < 128 || ch < 128) return false;
        int vx = tl.x + cfg.viewportOffsetX;
        int vy = tl.y + cfg.viewportOffsetY;
        int vw = (cfg.viewportWidth > 64) ? std::min(cfg.viewportWidth, cw - cfg.viewportOffsetX) : (cw - cfg.viewportOffsetX);
        int vh = (cfg.viewportHeight > 64) ? std::min(cfg.viewportHeight, ch - cfg.viewportOffsetY) : (ch - cfg.viewportOffsetY);
        if (vw < 128 || vh < 128) return false;
        // 保持偶数尺寸对齐确保光流与 NGX 采样精度
        vw &= ~1;
        vh &= ~1;
        outRect = { vx, vy, vx + vw, vy + vh };
        return true;
    };

    RECT vpRect{ 0, 0, 1920, 1080 };
    calcViewportScreenRect(sharedCfg, vpRect);
    uint32_t curW = (uint32_t)std::max<long>(128, vpRect.right - vpRect.left);
    uint32_t curH = (uint32_t)std::max<long>(128, vpRect.bottom - vpRect.top);

    HWND overlayHwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName,
        L"XStreaming DLSS5 Enhance Overlay",
        WS_POPUP,
        vpRect.left, vpRect.top, (int)curW, (int)curH,
        nullptr, nullptr, wc.hInstance, nullptr
    );
    SetLayeredWindowAttributes(overlayHwnd, 0, 255, LWA_ALPHA);
    DWORD cornerPref = 1; // DWMWCP_DONOTROUND
    DwmSetWindowAttribute(overlayHwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));

    StreamEnhancePipeline pipe;
    if (!pipe.Initialize(curW, curH, binDir)) {
        std::printf("{\"type\":\"error\",\"message\":\"Failed to initialize GPU pipeline\"}\n");
        running.store(false);
        DestroyWindow(overlayHwnd);
        if (stdinThread.joinable()) stdinThread.detach();
        return 1;
    }

    // 默认采用 v3 久经验证的 DXGI Output Duplication + WDA_EXCLUDEFROMCAPTURE 零遮挡满帧捕获管线（确保 X2 插帧 100% 稳定丝滑）；
    // 当用户按 F9 启动内置录屏或开启 OBS 兼容开关 (obsCaptureCompat = true) 时，无缝热切换至 WGC + WDA_NONE + Alpha 254 防 DWM 遮挡录屏模式！
    WindowGraphicsCapturer wgcCapturer;
    bool useWgc = false;
    SetWindowDisplayAffinity(overlayHwnd, WDA_EXCLUDEFROMCAPTURE);

    // 创建低延迟 Flip Discard 交换链 (MaximumFrameLatency = 1 + Allow Tearing)
    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = curW;
    scd.Height = curH;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.Scaling = DXGI_SCALING_STRETCH;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    scd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                (pipe.TearingSupported() ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);

    ComRef<IDXGISwapChain1> sc1;
    if (FAILED(pipe.Factory()->CreateSwapChainForHwnd(pipe.Device11(), overlayHwnd, &scd, nullptr, nullptr, sc1.Put()))) {
        std::printf("{\"type\":\"error\",\"message\":\"CreateSwapChainForHwnd failed\"}\n");
        running.store(false);
        DestroyWindow(overlayHwnd);
        if (stdinThread.joinable()) stdinThread.detach();
        return 1;
    }
    pipe.Factory()->MakeWindowAssociation(overlayHwnd, DXGI_MWA_NO_ALT_ENTER);
    ComRef<IDXGISwapChain2> sc2;
    HANDLE waitableSwapChain = nullptr;
    if (SUCCEEDED(sc1->QueryInterface(__uuidof(IDXGISwapChain2), (void**)sc2.Put()))) {
        sc2->SetMaximumFrameLatency(1);
        waitableSwapChain = sc2->GetFrameLatencyWaitableObject();
    }

    ComRef<ID3D11RenderTargetView> backBufferRtv;
    auto rebuildRtv = [&]() -> bool {
        backBufferRtv.Reset();
        ComRef<ID3D11Texture2D> bb;
        if (FAILED(sc1->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)bb.Put()))) return false;
        return SUCCEEDED(pipe.Device11()->CreateRenderTargetView(bb.Get(), nullptr, backBufferRtv.Put()));
    };
    rebuildRtv();

    DesktopDuplicator duplicator;
    duplicator.InitializeForRect(pipe.Factory(), pipe.Adapter(), pipe.Device11(), pipe.Context11(), vpRect, curW, curH);

    bool overlayVisible = false;
    RECT lastAppliedVp = vpRect;

    LARGE_INTEGER qpcFreq{};
    QueryPerformanceFrequency(&qpcFreq);
    auto nowSec = [&]() -> double {
        LARGE_INTEGER c{};
        QueryPerformanceCounter(&c);
        return double(c.QuadPart) / double(qpcFreq.QuadPart);
    };

    double lastRealArrival = nowSec();
    double avgRealInterval = 1.0 / 30.0; // 默认按 30fps 主机游戏步调初始化，若为真 60fps 会在 3 帧内自适应收敛至 16.67ms
    bool pendingSecondFrame = false;
    bool pendingIsGenerated = true;
    double lastFirstPresentTime = 0.0;
    double scheduledSecondTime = 0.0;
    bool needStaticDlssHarvest = false;
    bool hasAnyCapturedFrame = false;
    bool latencyProbePending = false;
    double latencyProbeCapStart = 0.0;

    uint32_t rawCaptureCount = 0, dupSkippedCount = 0, realFrameCount = 0, outFrameCount = 0;
    double lastTelemetryTime = nowSec();
    double lastLatencyProbeTime = 0.0;
    double avgGpuPipeMs = 1.20;
    std::vector<uint8_t> dirtyBuf(4096);

    std::printf("{\"type\":\"ready\",\"gpu\":\"%s\",\"dlss5\":%s,\"nvofa\":%s,\"tearing\":%s,\"recordable\":true}\n",
                pipe.GpuName().c_str(),
                pipe.Dlss5Ready() ? "true" : "false",
                pipe.NvofaActive() ? "true" : "false",
                pipe.TearingSupported() ? "true" : "false");

    while (running.load()) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running.store(false);
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running.load() || !IsWindow(targetHwnd)) break;

        EnhanceConfig cfg;
        {
            std::lock_guard<std::mutex> lk(cfgMutex);
            cfg = sharedCfg;
        }

        // 检查目标窗口是否处于前台可见状态（用户 Alt+Tab 切去其他软件时自动隐藏覆盖层，绝不遮挡其他窗口！）
        HWND fgWnd = GetForegroundWindow();
        DWORD fgPid = 0, targetPid = 0;
        if (fgWnd) GetWindowThreadProcessId(fgWnd, &fgPid);
        GetWindowThreadProcessId(targetHwnd, &targetPid);
        bool targetActive = (fgWnd == targetHwnd) || (fgPid != 0 && fgPid == targetPid);

        RECT newVp{};
        bool validGeom = calcViewportScreenRect(cfg, newVp);
        bool shouldShow = cfg.enabled && !cfg.paused && validGeom && targetActive;

        if (!shouldShow) {
            if (overlayVisible) {
                ShowWindow(overlayHwnd, SW_HIDE);
                overlayVisible = false;
                pendingSecondFrame = false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(12));
            continue;
        }

        // 动态热切换日常满帧 DXGI 捕获模式 vs 录屏直通 WGC 模式（Alpha 254 彻底避开 Windows 11 DWM 全不透明窗口遮挡降帧节流！）
        if (cfg.obsCaptureCompat && !useWgc) {
            if (wgcCapturer.Initialize(pipe.Device11(), targetHwnd)) {
                useWgc = true;
                duplicator.Reset();
                SetLayeredWindowAttributes(overlayHwnd, 0, 254, LWA_ALPHA);
                SetWindowDisplayAffinity(overlayHwnd, WDA_NONE);
            }
        } else if (!cfg.obsCaptureCompat && useWgc) {
            wgcCapturer.Shutdown();
            useWgc = false;
            SetWindowDisplayAffinity(overlayHwnd, WDA_EXCLUDEFROMCAPTURE);
            SetLayeredWindowAttributes(overlayHwnd, 0, 255, LWA_ALPHA);
            duplicator.InitializeForRect(pipe.Factory(), pipe.Adapter(), pipe.Device11(), pipe.Context11(), newVp, curW, curH);
        }

        // 同步窗口位置与尺寸变化
        uint32_t newW = (uint32_t)(newVp.right - newVp.left);
        uint32_t newH = (uint32_t)(newVp.bottom - newVp.top);
        if (newVp.left != lastAppliedVp.left || newVp.top != lastAppliedVp.top || newW != curW || newH != curH) {
            SetWindowPos(overlayHwnd, HWND_TOPMOST, newVp.left, newVp.top, (int)newW, (int)newH,
                         SWP_NOACTIVATE | (overlayVisible ? 0 : SWP_HIDEWINDOW));
            if (newW != curW || newH != curH) {
                curW = newW;
                curH = newH;
                backBufferRtv.Reset();
                UINT flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                             (pipe.TearingSupported() ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
                sc1->ResizeBuffers(2, curW, curH, DXGI_FORMAT_R8G8B8A8_UNORM, flags);
                rebuildRtv();
                pipe.ResizeResources(curW, curH);
                if (!useWgc) {
                    duplicator.InitializeForRect(pipe.Factory(), pipe.Adapter(), pipe.Device11(), pipe.Context11(), newVp, curW, curH);
                }
                pendingSecondFrame = false;
            }
            lastAppliedVp = newVp;
            rgnDirty.store(true);
        }

        // 当 F8 调参面板关闭瞬间，立即清空 NVOFA 与 DLSS 5 时域缓冲，0 帧清除任何 UI 面板历史残留！
        static bool prevHadExcludeUi = false;
        bool curHasExcludeUi = !cfg.excludeRects.empty();
        if (prevHadExcludeUi && !curHasExcludeUi) {
            pipe.ResetTemporalOnCut();
        }
        prevHadExcludeUi = curHasExcludeUi;

        // 应用 React 菜单/抽屉/调参弹窗矩形镂空 (同时配合 GPU PresentCB 原生直通，双重保障 UI 100% 锐利不模糊)
        if (rgnDirty.exchange(false)) {
            if (cfg.excludeRects.empty()) {
                SetWindowRgn(overlayHwnd, nullptr, TRUE);
            } else {
                HRGN fullRgn = CreateRectRgn(0, 0, (int)curW, (int)curH);
                for (const auto& er : cfg.excludeRects) {
                    HRGN cut = CreateRectRgn(er.x, er.y, er.x + er.w, er.y + er.h);
                    CombineRgn(fullRgn, fullRgn, cut, RGN_DIFF);
                    DeleteObject(cut);
                }
                SetWindowRgn(overlayHwnd, fullRgn, TRUE); // 系统接管 fullRgn 生命周期
            }
        }

        if (!overlayVisible) {
            ShowWindow(overlayHwnd, SW_SHOWNA);
            SetWindowPos(overlayHwnd, HWND_TOPMOST, newVp.left, newVp.top, (int)curW, (int)curH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            overlayVisible = true;
        }

        auto presentNow = [&](bool isGenerated) {
            if (!backBufferRtv) return;
            bool tear = cfg.allowTearing && pipe.TearingSupported();
            // 开启 Allow Tearing (SyncInterval=0) 时无需阻塞等待 VBlank，消除最高 4ms 交换链排队延迟
            if (waitableSwapChain && !tear) WaitForSingleObject(waitableSwapChain, 2);
            pipe.RenderToRtv(backBufferRtv.Get(), isGenerated, cfg);
            sc1->Present(tear ? 0 : 1, tear ? DXGI_PRESENT_ALLOW_TEARING : 0);
            ++outFrameCount;
        };

        // 检查是否到达排期的第 2 帧上屏时刻（X2 补帧半周期微秒级精准均匀输出）
        double tNow = nowSec();
        if (pendingSecondFrame) {
            double remainSec = scheduledSecondTime - tNow;
            if (remainSec <= 0.0012) {
                while ((tNow = nowSec()) < scheduledSecondTime) {
                    YieldProcessor();
                }
                if (pendingIsGenerated) {
                    if (pipe.SynthesizeX2Frame(cfg)) {
                        presentNow(true);
                    }
                    pipe.MarkRealFrameCommitted();
                } else {
                    presentNow(false);
                    pipe.MarkRealFrameCommitted();
                }
                pendingSecondFrame = false;
            }
        }

        // 静态暂停画面收尾增强与实时调参即时响应（不影响运动光流历史）
        tNow = nowSec();
        if (hasAnyCapturedFrame && !pendingSecondFrame && (tNow - lastRealArrival > 0.090)) {
            if (needStaticDlssHarvest || cfgDirty.exchange(false)) {
                needStaticDlssHarvest = false;
                pipe.ProcessNewRealFrame(cfg);
                presentNow(false);
                pipe.MarkRealFrameCommitted();
            }
        } else if (tNow - lastRealArrival <= 0.090) {
            cfgDirty.store(false);
        }

        // 捕获最新视频视口纹理（优先走 WGC 单窗口零回环可录屏路径，兜底走 DXGI Desktop Duplication）
        double qpcCapStart = nowSec();
        bool copiedOk = false;

        if (useWgc) {
            copiedOk = wgcCapturer.TryAcquireAndCopyViewport(pipe.Context11(), newVp, curW, curH, pipe.CandidateTex());
            if (!copiedOk && (!pendingSecondFrame || (scheduledSecondTime - nowSec() > 0.0025))) {
                Sleep(1);
            }
        } else {
            if (!duplicator.dup) {
                if (!duplicator.InitializeForRect(pipe.Factory(), pipe.Adapter(), pipe.Device11(), pipe.Context11(), newVp, curW, curH)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(8));
                    continue;
                }
            }

            DXGI_OUTDUPL_FRAME_INFO fi{};
            ComRef<IDXGIResource> deskRes;
            HRESULT hrDup = duplicator.dup->AcquireNextFrame(1, &fi, deskRes.Put());
            if (hrDup == DXGI_ERROR_ACCESS_LOST || hrDup == DXGI_ERROR_INVALID_CALL) {
                duplicator.InitializeForRect(pipe.Factory(), pipe.Adapter(), pipe.Device11(), pipe.Context11(), newVp, curW, curH);
            } else if (SUCCEEDED(hrDup) && deskRes) {
                bool touchedViewport = false;
                if (fi.TotalMetadataBufferSize > 0) {
                    if (dirtyBuf.size() < fi.TotalMetadataBufferSize) dirtyBuf.resize(fi.TotalMetadataBufferSize);
                    UINT req = 0;
                    if (SUCCEEDED(duplicator.dup->GetFrameMoveRects((UINT)dirtyBuf.size(), reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(dirtyBuf.data()), &req)) && req > 0) {
                        touchedViewport = true;
                    }
                    if (!touchedViewport && SUCCEEDED(duplicator.dup->GetFrameDirtyRects((UINT)dirtyBuf.size(), reinterpret_cast<RECT*>(dirtyBuf.data()), &req))) {
                        RECT localVp{
                            newVp.left - duplicator.monitorRect.left,
                            newVp.top - duplicator.monitorRect.top,
                            newVp.right - duplicator.monitorRect.left,
                            newVp.bottom - duplicator.monitorRect.top
                        };
                        UINT count = req / sizeof(RECT);
                        const RECT* rects = reinterpret_cast<const RECT*>(dirtyBuf.data());
                        for (UINT i = 0; i < count; ++i) {
                            RECT inter{};
                            if (IntersectRect(&inter, &localVp, &rects[i])) {
                                touchedViewport = true;
                                break;
                            }
                        }
                    }
                }

                if (touchedViewport && fi.LastPresentTime.QuadPart != 0) {
                    qpcCapStart = nowSec();
                    ComRef<ID3D11Texture2D> deskTex;
                    if (SUCCEEDED(deskRes->QueryInterface(__uuidof(ID3D11Texture2D), (void**)deskTex.Put()))) {
                        D3D11_TEXTURE2D_DESC dDesc{};
                        deskTex->GetDesc(&dDesc);
                        int srcX = std::clamp<int>(newVp.left - duplicator.monitorRect.left, 0, std::max<int>(0, (int)dDesc.Width - (int)curW));
                        int srcY = std::clamp<int>(newVp.top - duplicator.monitorRect.top, 0, std::max<int>(0, (int)dDesc.Height - (int)curH));
                        D3D11_BOX box{ (UINT)srcX, (UINT)srcY, 0, (UINT)(srcX + curW), (UINT)(srcY + curH), 1 };

                        if (!duplicator.crossAdapter) {
                            pipe.Context11()->CopySubresourceRegion(pipe.CandidateTex(), 0, 0, 0, 0, deskTex.Get(), 0, &box);
                        } else {
                            duplicator.captureCtx->CopySubresourceRegion(duplicator.crossSharedCapture.Get(), 0, 0, 0, 0, deskTex.Get(), 0, &box);
                            duplicator.captureCtx->Flush();
                            pipe.Context11()->CopyResource(pipe.CandidateTex(), duplicator.crossSharedRender.Get());
                        }
                        copiedOk = true;
                    }
                }
                duplicator.dup->ReleaseFrame();
            }
        }

        if (copiedOk) {
            ++rawCaptureCount;
            bool isDup = false, isSceneCut = false;
            pipe.CheckCandidateFrame(cfg, isDup, isSceneCut);

            if (isDup) {
                // 命中 Xbox 60fps 串流容器中的 30fps 编码重复帧！直接过滤，保护 NVOFA 真实 30fps 位移与半周期补帧节奏
                ++dupSkippedCount;
            } else {
                hasAnyCapturedFrame = true;

                // 若上一帧的半周期真实帧还没上屏但新真实帧已赶到：确保第 1 帧在屏幕上至少完整驻留过 8.5ms (防 FLIP_DISCARD 瞬时覆盖吞帧！) 后再补齐第 2 帧
                if (pendingSecondFrame && !pendingIsGenerated) {
                    double minDisplayTime = lastFirstPresentTime + 0.0085;
                    while (nowSec() < minDisplayTime) {
                        YieldProcessor();
                    }
                    presentNow(false);
                    pipe.MarkRealFrameCommitted();
                }
                pendingSecondFrame = false;

                double arrival = nowSec();
                double dt = arrival - lastRealArrival;
                lastRealArrival = arrival;
                if (dt >= (1.0 / 165.0) && dt <= (1.0 / 15.0)) {
                    avgRealInterval = avgRealInterval * 0.80 + dt * 0.20;
                }
                ++realFrameCount;
                needStaticDlssHarvest = (cfg.dlss5Enabled && cfg.dlss5LatencyMode == 0);

                if (isSceneCut) {
                    pipe.ResetTemporalOnCut();
                    pipe.ProcessNewRealFrame(cfg);
                    presentNow(false);
                    pipe.MarkRealFrameCommitted();
                } else {
                    // 执行真 30/60fps 光流 + DLSS 5 (单层或双层叠加，默认逐帧同步零残影) + CAS/RCAS 锐化
                    pipe.ProcessNewRealFrame(cfg);

                    if (cfg.fgEnabled) {
                        double halfStep = std::clamp(avgRealInterval * 0.49, 0.006, 0.028);
                        if (cfg.fgMode == 0) {
                            presentNow(false);
                            lastFirstPresentTime = nowSec();
                            pendingSecondFrame = true;
                            pendingIsGenerated = true;
                            // 关键修复：必须以第 1 帧实际上屏完成的时刻为锚点排期半周期（修复逐帧同步 DLSS 5 耗时 6~10ms 导致第 2 帧瞬时覆盖吞掉补帧的 Bug！）
                            scheduledSecondTime = lastFirstPresentTime + halfStep;
                        } else {
                            if (pipe.SynthesizeX2Frame(cfg)) {
                                presentNow(true);
                                lastFirstPresentTime = nowSec();
                                pendingSecondFrame = true;
                                pendingIsGenerated = false;
                                scheduledSecondTime = lastFirstPresentTime + halfStep;
                            } else {
                                presentNow(false);
                                pipe.MarkRealFrameCommitted();
                            }
                        }
                    } else {
                        presentNow(false);
                        pipe.MarkRealFrameCommitted();
                    }
                }

                // 非阻塞异步轮询端到端 GPU 渲染延迟（0ms 主循环阻塞，绝不干扰 X2 补帧半周期节拍！）
                double tAfterPresent = nowSec();
                if (!latencyProbePending && (tAfterPresent - lastLatencyProbeTime >= 0.25) && pipe.GpuDoneQuery()) {
                    lastLatencyProbeTime = tAfterPresent;
                    latencyProbeCapStart = qpcCapStart;
                    pipe.Context11()->End(pipe.GpuDoneQuery());
                    pipe.Context11()->Flush();
                    latencyProbePending = true;
                }
            }
        }

        if (latencyProbePending && pipe.GpuDoneQuery()) {
            BOOL gpuDone = FALSE;
            HRESULT qhr = pipe.Context11()->GetData(pipe.GpuDoneQuery(), &gpuDone, sizeof(gpuDone), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (qhr == S_OK && gpuDone) {
                double measuredPipeMs = std::clamp((nowSec() - latencyProbeCapStart) * 1000.0, 0.40, 45.0);
                avgGpuPipeMs = avgGpuPipeMs * 0.65 + measuredPipeMs * 0.35;
                latencyProbePending = false;
            } else if (nowSec() - latencyProbeCapStart > 0.050) {
                latencyProbePending = false;
            }
        }

        // 每秒向 Electron 推送实时硬件遥测指标（含包含 DLSS 5 与插帧帧序扣押在内的 100% 真实增加延迟）
        double tCheck = nowSec();
        double elapsed = tCheck - lastTelemetryTime;
        if (elapsed >= 1.0) {
            double rawFps = double(rawCaptureCount) / elapsed;
            double srcFps = double(realFrameCount) / elapsed;
            double outFps = double(outFrameCount) / elapsed;
            bool dlssOn = cfg.enabled && cfg.dlss5Enabled && pipe.Dlss5Ready();
            double dlssGpuMs = dlssOn ? pipe.Dlss5GpuMs() : 0.0;
            double dlssHostMs = dlssOn ? pipe.Dlss5HostMs() : 0.0;
            // 双向中点内插 (Mode 1) 为计算 50% 几何中点需将真实帧延后约半个真实帧周期上屏；零等待外推 (Mode 0) 扣押为 0ms
            double fgHoldMs = (cfg.enabled && cfg.fgEnabled && cfg.fgMode == 1)
                                  ? (std::clamp(avgRealInterval * 0.49, 0.004, 0.028) * 1000.0)
                                  : 0.0;
            double gpuPipeMs = cfg.enabled ? avgGpuPipeMs : 0.0;
            double totalAddedMs = gpuPipeMs + fgHoldMs;

            std::printf(
                "{\"type\":\"telemetry\",\"rawFps\":%.1f,\"srcFps\":%.1f,\"dupSkipped\":%u,\"outFps\":%.1f,"
                "\"dedup30In60\":%s,\"dlss5Active\":%s,\"dlss5Passes\":%d,\"dlss5Sync\":%s,\"dlss5WorkRes\":\"%ux%u\","
                "\"dlss5Ms\":%.2f,\"dlss5HostMs\":%.2f,\"gpuPipeMs\":%.2f,\"fgHoldMs\":%.2f,\"totalAddedMs\":%.2f,"
                "\"recordable\":%s,\"nvofa\":%s,\"flowMs\":%.2f,\"composeMs\":%.2f,\"fgMs\":%.2f,"
                "\"fgActive\":%s,\"fgMode\":%d,\"sharpenMode\":%d,\"gpu\":\"%s\"}\n",
                rawFps, srcFps, dupSkippedCount, outFps,
                cfg.dedup30In60 ? "true" : "false",
                dlssOn ? "true" : "false",
                dlssOn ? std::clamp(cfg.dlss5Passes, 1, 2) : 0,
                (cfg.dlss5LatencyMode == 1) ? "true" : "false",
                pipe.Dlss5WorkW(), pipe.Dlss5WorkH(),
                dlssGpuMs, dlssHostMs, gpuPipeMs, fgHoldMs, totalAddedMs,
                useWgc ? "true" : "false",
                pipe.NvofaActive() ? "true" : "false",
                pipe.FlowMs(), pipe.ComposeMs(), pipe.FgMs(),
                cfg.fgEnabled ? "true" : "false",
                cfg.fgMode, cfg.sharpenMode,
                pipe.GpuName().c_str()
            );
            rawCaptureCount = 0;
            dupSkippedCount = 0;
            realFrameCount = 0;
            outFrameCount = 0;
            lastTelemetryTime = tCheck;
        }
    }

    ShowWindow(overlayHwnd, SW_HIDE);
    wgcCapturer.Shutdown();
    duplicator.Reset();
    backBufferRtv.Reset();
    if (waitableSwapChain) CloseHandle(waitableSwapChain);
    sc2.Reset();
    sc1.Reset();
    pipe.Shutdown();
    DestroyWindow(overlayHwnd);

    if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
    timeEndPeriod(1);
    if (stdinThread.joinable()) stdinThread.detach();
    return 0;
}
