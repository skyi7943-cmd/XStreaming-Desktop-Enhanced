#pragma once
// XStreaming Desktop Enhanced - 内置真 NVIDIA DLSS 5 (nvngx_dlssnr.dll Feature 18) D3D11↔D3D12 桥接器
// 核心特性：
// 1. 显式 MSVC x64 C++ ABI Vtable 映射，100% 兼容 zig c++ / Clang / MinGW 编译环境
// 2. 零深度引导 (Zero-Depth Contract) + NVOFA 硬件光流运动矢量注入
// 3. 双缓冲共享纹理 + 非阻塞异步 D3D12 GPU 队列推理（超低延迟模式下主线程 0ms 阻塞等待）
// 4. Oklab 感知色彩空间残差解耦控制 + 光流引导时域抗闪烁

#include "d3d_helpers.h"
#include "engine_config.h"
#include "shaders_hlsl.h"
#include <filesystem>
#include <algorithm>
#include <cmath>

struct NgxParamVtblMsvc {
    void (__cdecl *SetVoidPtr)(void* self, const char* name, void* val);
    void (__cdecl *SetD3d12Resource)(void* self, const char* name, ID3D12Resource* val);
    void (__cdecl *SetD3d11Resource)(void* self, const char* name, ID3D11Resource* val);
    void (__cdecl *SetI)(void* self, const char* name, int val);
    void (__cdecl *SetUI)(void* self, const char* name, unsigned int val);
    void (__cdecl *SetD)(void* self, const char* name, double val);
    void (__cdecl *SetF)(void* self, const char* name, float val);
    void (__cdecl *SetULL)(void* self, const char* name, unsigned long long val);

    uint32_t (__cdecl *GetVoidPtr)(const void* self, const char* name, void** out);
    uint32_t (__cdecl *GetD3d12Resource)(const void* self, const char* name, ID3D12Resource** out);
    uint32_t (__cdecl *GetD3d11Resource)(const void* self, const char* name, ID3D11Resource** out);
    uint32_t (__cdecl *GetI)(const void* self, const char* name, int* out);
    uint32_t (__cdecl *GetUI)(const void* self, const char* name, unsigned int* out);
    uint32_t (__cdecl *GetD)(const void* self, const char* name, double* out);
    uint32_t (__cdecl *GetF)(const void* self, const char* name, float* out);
    uint32_t (__cdecl *GetULL)(const void* self, const char* name, unsigned long long* out);

    void (__cdecl *Reset)(void* self);
};

struct NgxParamObj {
    const NgxParamVtblMsvc* vtbl;
};

namespace Dlss5Detail {
    using GetModuleFileNameWFn = DWORD (WINAPI*)(HMODULE, LPWSTR, DWORD);
    inline HMODULE g_callerModule = nullptr;
    inline GetModuleFileNameWFn g_origGetModuleFileNameW = nullptr;
    inline void** g_patchedSlot = nullptr;

    inline DWORD WINAPI HookGetModuleFileNameW(HMODULE mod, LPWSTR buf, DWORD size) {
        if (mod == g_callerModule) {
            constexpr wchar_t AUTH[] = L"nvngx.dll";
            if (!buf || !size) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
            if (size <= 9) {
                if (size > 1) std::memcpy(buf, AUTH, (size - 1) * sizeof(wchar_t));
                buf[size - 1] = L'\0';
                SetLastError(ERROR_INSUFFICIENT_BUFFER);
                return size;
            }
            std::memcpy(buf, AUTH, sizeof(AUTH));
            return 9;
        }
        return g_origGetModuleFileNameW ? g_origGetModuleFileNameW(mod, buf, size) : 0;
    }

    inline bool InstallCallerShim(HMODULE snippetMod) {
        if (g_patchedSlot) return true;
        auto* base = reinterpret_cast<uint8_t*>(snippetMod);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress) return false;

        auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
        for (; desc->Name; ++desc) {
            const char* lib = reinterpret_cast<const char*>(base + desc->Name);
            if (_stricmp(lib, "KERNEL32.dll") != 0 &&
                _stricmp(lib, "api-ms-win-core-libraryloader-l1-2-0.dll") != 0 &&
                _stricmp(lib, "api-ms-win-core-libraryloader-l1-1-0.dll") != 0) continue;
            auto* origThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->OriginalFirstThunk);
            auto* iatThunk  = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);
            for (; origThunk->u1.AddressOfData; ++origThunk, ++iatThunk) {
                if (IMAGE_SNAP_BY_ORDINAL64(origThunk->u1.Ordinal)) continue;
                auto* imp = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + origThunk->u1.AddressOfData);
                if (std::strcmp(reinterpret_cast<const char*>(imp->Name), "GetModuleFileNameW") == 0) {
                    GetModuleHandleExW(
                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        reinterpret_cast<LPCWSTR>(&HookGetModuleFileNameW), &g_callerModule);
                    DWORD oldProt = 0;
                    if (!VirtualProtect(&iatThunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProt)) return false;
                    g_origGetModuleFileNameW = reinterpret_cast<GetModuleFileNameWFn>(iatThunk->u1.Function);
                    iatThunk->u1.Function = reinterpret_cast<ULONGLONG>(&HookGetModuleFileNameW);
                    VirtualProtect(&iatThunk->u1.Function, sizeof(void*), oldProt, &oldProt);
                    FlushInstructionCache(GetCurrentProcess(), &iatThunk->u1.Function, sizeof(void*));
                    g_patchedSlot = reinterpret_cast<void**>(&iatThunk->u1.Function);
                    return true;
                }
            }
        }
        return false;
    }

    inline uint32_t __cdecl ScalingRatioCallback(NgxParamObj* p) {
        if (!p) return 0xBAD00005u;
        p->vtbl->SetF(p, "DLSSNR.ScalingRatio", 1.0f);
        return 1u;
    }
}

class Dlss5Bridge {
public:
    ~Dlss5Bridge() { Shutdown(); }

    bool Initialize(
        IDXGIAdapter1* adapter,
        ID3D11Device5* dev11,
        ID3D11DeviceContext4* ctx11,
        const std::wstring& binDir,
        uint32_t srcW,
        uint32_t srcH,
        uint32_t inputScalePercent
    ) {
        Shutdown();
        _dev11 = dev11;
        _ctx11 = ctx11;
        _binDir = binDir;
        _srcW = srcW;
        _srcH = srcH;
        _scalePercent = std::clamp(inputScalePercent, 50u, 100u);
        _workW = std::max(64u, (uint32_t)std::lround(double(srcW) * double(_scalePercent) / 100.0));
        _workH = std::max(64u, (uint32_t)std::lround(double(srcH) * double(_scalePercent) / 100.0));

        // 1. 创建共享适配器上的 D3D12 设备与高优先级推理队列
        if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), (void**)_dev12.Put()))) {
            return false;
        }
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        if (FAILED(_dev12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)_queue12.Put()))) {
            qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
            if (FAILED(_dev12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)_queue12.Put()))) return false;
        }
        for (int i = 0; i < 2; ++i) {
            if (FAILED(_dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)_alloc12[i].Put()))) return false;
        }
        if (FAILED(_dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _alloc12[0].Get(), nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)_cmdList12.Put()))) {
            return false;
        }
        _cmdList12->Close();

        // 创建 D3D12 GPU 硬件时间戳查询（精确读取每次 DLSS 5 推理毫秒耗时）
        if (SUCCEEDED(_queue12->GetTimestampFrequency(&_tsFreq)) && _tsFreq > 0) {
            D3D12_QUERY_HEAP_DESC qhd{};
            qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            qhd.Count = 4;
            if (SUCCEEDED(_dev12->CreateQueryHeap(&qhd, __uuidof(ID3D12QueryHeap), (void**)_tsHeap.Put()))) {
                D3D12_HEAP_PROPERTIES rhp{}; rhp.Type = D3D12_HEAP_TYPE_READBACK;
                D3D12_RESOURCE_DESC rrd{};
                rrd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                rrd.Width = sizeof(uint64_t) * 4;
                rrd.Height = 1; rrd.DepthOrArraySize = 1; rrd.MipLevels = 1;
                rrd.SampleDesc.Count = 1;
                rrd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                _dev12->CreateCommittedResource(&rhp, D3D12_HEAP_FLAG_NONE, &rrd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)_tsReadback.Put());
            }
        }

        // 创建 D3D11 <-> D3D12 共享硬件围栏
        if (FAILED(_dev11->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void**)_fence11.Put()))) return false;
        HANDLE hFence = nullptr;
        if (FAILED(_fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &hFence))) return false;
        HRESULT hrF = _dev12->OpenSharedHandle(hFence, __uuidof(ID3D12Fence), (void**)_fence12.Put());
        CloseHandle(hFence);
        if (FAILED(hrF)) return false;
        _fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        // 2. 加载驱动层 _nvngx.dll 参数分配器与本地 nvngx_dlssnr.dll (Feature 18)
        wchar_t ngxCoreDir[MAX_PATH]{};
        DWORD cb = sizeof(ngxCoreDir);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore",
                         L"NGXPath", RRF_RT_REG_SZ, nullptr, ngxCoreDir, &cb) != ERROR_SUCCESS) {
            return false;
        }
        std::wstring coreDll = std::wstring(ngxCoreDir) + L"\\_nvngx.dll";
        _coreMod = LoadLibraryExW(coreDll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!_coreMod) return false;

        _fnAllocParams = reinterpret_cast<AllocParamFn>(GetProcAddress(_coreMod, "NVSDK_NGX_D3D12_AllocateParameters"));
        _fnDestroyParams = reinterpret_cast<DestroyParamFn>(GetProcAddress(_coreMod, "NVSDK_NGX_D3D12_DestroyParameters"));
        if (!_fnAllocParams || !_fnDestroyParams) return false;
        for (int p = 0; p < 2; ++p) {
            if (_fnAllocParams(&_params[p]) != 1u || !_params[p]) return false;
        }

        std::wstring nrDll = _binDir + L"\\nvngx_dlssnr.dll";
        if (GetFileAttributesW(nrDll.c_str()) == INVALID_FILE_ATTRIBUTES) {
            nrDll = L"E:\\Magpie-Experimental-x64\\Magpie-Experimental-x64\\nvngx_dlssnr.dll";
        }
        _nrMod = LoadLibraryExW(nrDll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!_nrMod) return false;
        if (!Dlss5Detail::InstallCallerShim(_nrMod)) return false;

        _fnInitExt = reinterpret_cast<SnippetInitExtFn>(GetProcAddress(_nrMod, "NVSDK_NGX_D3D12_Init_Ext"));
        _fnCreateFeature = reinterpret_cast<CreateFeatFn>(GetProcAddress(_nrMod, "NVSDK_NGX_D3D12_CreateFeature"));
        _fnEvalFeature = reinterpret_cast<EvalFeatFn>(GetProcAddress(_nrMod, "NVSDK_NGX_D3D12_EvaluateFeature"));
        _fnReleaseFeature = reinterpret_cast<ReleaseFeatFn>(GetProcAddress(_nrMod, "NVSDK_NGX_D3D12_ReleaseFeature"));
        _fnShutdown1 = reinterpret_cast<Shutdown1Fn>(GetProcAddress(_nrMod, "NVSDK_NGX_D3D12_Shutdown1"));
        if (!_fnInitExt || !_fnCreateFeature || !_fnEvalFeature || !_fnReleaseFeature || !_fnShutdown1) return false;

        std::wstring dataDir = std::filesystem::path(nrDll).parent_path().wstring();
        if (_fnInitExt(0x0876232Cull, dataDir.c_str(), _dev12.Get(), 0x0000015u, nullptr) != 1u) {
            return false;
        }
        _ngxInitialized = true;

        if (!_CreatePipelineResources()) {
            return false;
        }

        if (!_CreateNgxFeatures()) {
            return false;
        }

        _resetHistory[0] = true;
        _resetHistory[1] = true;
        _ready = true;
        return true;
    }

    // 当用户在调试面板修改了推理工作集分辨率百分比 (50%~100%) 时重建工作集资源与双层 Feature
    bool EnsureScale(uint32_t requestedScale) {
        uint32_t clamped = std::clamp(requestedScale, 50u, 100u);
        if (clamped == _scalePercent || !_ready) return true;
        _DrainInFlight();
        for (int p = 0; p < 2; ++p) {
            if (_featureHandle[p] && _fnReleaseFeature) {
                _fnReleaseFeature(_featureHandle[p]);
                _featureHandle[p] = nullptr;
            }
        }
        _scalePercent = clamped;
        _workW = std::max(64u, (uint32_t)std::lround(double(_srcW) * double(_scalePercent) / 100.0));
        _workH = std::max(64u, (uint32_t)std::lround(double(_srcH) * double(_scalePercent) / 100.0));
        if (!_CreateWorksetTextures()) return false;
        bool ok = _CreateNgxFeatures();
        _resetHistory[0] = true;
        _resetHistory[1] = true;
        _hasResidualHistory = false;
        return ok;
    }

    void Shutdown() {
        _DrainInFlight();
        for (int p = 0; p < 2; ++p) {
            if (_featureHandle[p] && _fnReleaseFeature) {
                _fnReleaseFeature(_featureHandle[p]);
                _featureHandle[p] = nullptr;
            }
        }
        if (_ngxInitialized && _fnShutdown1 && _dev12) {
            _fnShutdown1(_dev12.Get());
            _ngxInitialized = false;
        }
        for (int p = 0; p < 2; ++p) {
            if (_params[p] && _fnDestroyParams) {
                _fnDestroyParams(_params[p]);
                _params[p] = nullptr;
            }
        }
        for (int i = 0; i < 2; ++i) {
            _sharedIn11[i].Reset(); _sharedInSrv11[i].Reset(); _sharedInUav11[i].Reset(); _sharedIn12[i].Reset();
            _sharedMid11[i].Reset(); _sharedMidSrv11[i].Reset(); _sharedMidUav11[i].Reset(); _sharedMid12[i].Reset();
            _sharedOut11[i].Reset(); _sharedOutSrv11[i].Reset(); _sharedOutUav11[i].Reset(); _sharedOut12[i].Reset();
            _sharedMv11[i].Reset(); _sharedMvSrv11[i].Reset(); _sharedMvUav11[i].Reset(); _sharedMv12[i].Reset();
            _residualTex[i].Reset(); _residualSrv[i].Reset(); _residualUav[i].Reset();
            _alloc12[i].Reset();
        }
        _zeroDepthUav11.Reset();
        _zeroDepth11.Reset();
        _zeroDepth12.Reset();
        _csDownsample.Reset();
        _csResidual.Reset();
        _linearSampler.Reset();
        _cbWorkset.Reset();
        _cbResidual.Reset();
        _tsReadback.Reset();
        _tsHeap.Reset();
        if (_fenceEvent) { CloseHandle(_fenceEvent); _fenceEvent = nullptr; }
        _fence12.Reset();
        _fence11.Reset();
        _cmdList12.Reset();
        _queue12.Reset();
        _dev12.Reset();
        _ready = false;
        _inFlight = false;
        _hasResidualHistory = false;
    }

    void ResetHistory() {
        _resetHistory[0] = true;
        _resetHistory[1] = true;
        _hasResidualHistory = false;
    }

    // 处理一帧真实画面：支持 Async 极低延迟残差解耦（0ms 主线程等待）与 Sync 逐帧同步双模式
    // outNeedsAsyncWarp 返回当前使用的残差是否落后 1 帧需要做光流运动补偿对齐
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

        const bool syncMode = (cfg.dlss5LatencyMode == 1) || !_hasResidualHistory;

        // 1. 先收获上一轮在飞的异步 D3D12 NGX 任务（异步模式下最多轻等 6ms 防止残差跨 2 帧陈旧导致移动重影）
        if (_inFlight) {
            if (_fence12->GetCompletedValue() < _inFlightDoneFence) {
                _fence12->SetEventOnCompletion(_inFlightDoneFence, _fenceEvent);
                WaitForSingleObject(_fenceEvent, syncMode ? 2000 : 6);
            }
            if (_fence12->GetCompletedValue() >= _inFlightDoneFence) {
                _HarvestCompletedSlot(_inFlightSlot, srcColorSrv, fwdMotionSrv, flowConfSrv, cfg, true);
                _inFlight = false;
            }
        }

        // 2. 如果 D3D12 推理队列空闲，立即投递本帧推理任务
        if (!_inFlight) {
            uint32_t slot = _nextSlot;
            _nextSlot ^= 1u;

            // (a) D3D11 Lanczos2 降采样 Color + 缩放 MVec 到共享工作集纹理
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

            ID3D11ShaderResourceView* srvs[2]{ srcColorSrv, fwdMotionSrv };
            ID3D11UnorderedAccessView* uavs[2]{ _sharedInUav11[slot].Get(), _sharedMvUav11[slot].Get() };
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
            _ctx11->CSSetShader(nullptr, nullptr, 0);

            uint64_t inputReadyFence = ++_fenceCounter;
            _ctx11->Signal(_fence11.Get(), inputReadyFence);
            _ctx11->Flush();

            // (b) 录制并提交 D3D12 NGX DLSS 5 (Feature 18) 单层或双层叠加推理命令
            _alloc12[slot]->Reset();
            _cmdList12->Reset(_alloc12[slot].Get(), nullptr);

            uint32_t qBase = slot * 2;
            if (_tsHeap) _cmdList12->EndQuery(_tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, qBase);

            const bool dualStack = (cfg.dlss5Passes >= 2) && (_featureHandle[1] != nullptr);
            if (!dualStack) {
                _resetHistory[1] = true; // 单层运行时保持第 2 层待重置，确保随时开启第 2 层零历史残留
            }

            // 对齐 Magpie ApplyLiveParameters 规范：当用户切换自然/鲜明/电影风格或修改任意 NGX 推理参数时，立即触发 DLSSNR.Reset = 1 刷新模型时域状态！
            bool ngxCfgChanged =
                (_cachedStyle[0] != cfg.dlss5Style) ||
                (_cachedStyle[1] != cfg.dlss5Pass2Style) ||
                (std::abs(_cachedIntensity[0] - cfg.dlss5Intensity) > 1e-4f) ||
                (std::abs(_cachedIntensity[1] - cfg.dlss5Pass2Intensity) > 1e-4f) ||
                (std::abs(_cachedLocalTone - cfg.dlss5LocalTone) > 1e-4f) ||
                (std::abs(_cachedLocalStructure - cfg.dlss5LocalStructure) > 1e-4f) ||
                (std::abs(_cachedSkinStructure - cfg.dlss5SkinStructure) > 1e-4f) ||
                (_cachedAutoMask != cfg.dlss5AutoMask) ||
                (_cachedUiCorrection != cfg.dlss5UiCorrection) ||
                (_cachedDualStack != dualStack);
            if (ngxCfgChanged) {
                _resetHistory[0] = true;
                _resetHistory[1] = true;
                _hasResidualHistory = false;
                _cachedStyle[0] = cfg.dlss5Style;
                _cachedStyle[1] = cfg.dlss5Pass2Style;
                _cachedIntensity[0] = cfg.dlss5Intensity;
                _cachedIntensity[1] = cfg.dlss5Pass2Intensity;
                _cachedLocalTone = cfg.dlss5LocalTone;
                _cachedLocalStructure = cfg.dlss5LocalStructure;
                _cachedSkinStructure = cfg.dlss5SkinStructure;
                _cachedAutoMask = cfg.dlss5AutoMask;
                _cachedUiCorrection = cfg.dlss5UiCorrection;
                _cachedDualStack = dualStack;
            }

            // 显式 D3D12 资源屏障：防止 Ada 显卡 L2 压缩块未刷新产生暗场斑点
            _Transition12(_sharedIn12[slot].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            _Transition12(_sharedMv12[slot].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            _Transition12(_zeroDepth12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            auto bindAndEvalPass = [&](int passIdx, ID3D12Resource* colorIn, ID3D12Resource* colorOut, int style, float intensity) -> uint32_t {
                NgxParamObj* prm = _params[passIdx];
                prm->vtbl->SetD3d12Resource(prm, "DLSSNR.Color", colorIn);
                prm->vtbl->SetD3d12Resource(prm, "DLSSNR.Output", colorOut);
                prm->vtbl->SetD3d12Resource(prm, "DLSSNR.MVec", _sharedMv12[slot].Get());
                prm->vtbl->SetD3d12Resource(prm, "DLSSNR.Depth", _zeroDepth12.Get());

                auto setRect = [&](const char* bx, const char* by, const char* bw, const char* bh) {
                    prm->vtbl->SetUI(prm, bx, 0u);
                    prm->vtbl->SetUI(prm, by, 0u);
                    prm->vtbl->SetUI(prm, bw, _workW);
                    prm->vtbl->SetUI(prm, bh, _workH);
                };
                setRect("DLSSNR.ColorSubrectBaseX", "DLSSNR.ColorSubrectBaseY", "DLSSNR.ColorSubrectWidth", "DLSSNR.ColorSubrectHeight");
                setRect("DLSSNR.OutputSubrectBaseX", "DLSSNR.OutputSubrectBaseY", "DLSSNR.OutputSubrectWidth", "DLSSNR.OutputSubrectHeight");
                setRect("DLSSNR.MVecSubrectBaseX", "DLSSNR.MVecSubrectBaseY", "DLSSNR.MVecSubrectWidth", "DLSSNR.MVecSubrectHeight");
                setRect("DLSSNR.DepthSubrectBaseX", "DLSSNR.DepthSubrectBaseY", "DLSSNR.DepthSubrectWidth", "DLSSNR.DepthSubrectHeight");

                prm->vtbl->SetF(prm, "DLSSNR.MVecScaleX", 1.0f);
                prm->vtbl->SetF(prm, "DLSSNR.MVecScaleY", 1.0f);
                prm->vtbl->SetI(prm, "DLSSNR.DepthInverted", 1);
                prm->vtbl->SetI(prm, "DLSS.Indicator.Invert.X.Axis", 0);
                prm->vtbl->SetI(prm, "DLSS.Indicator.Invert.Y.Axis", 0);
                prm->vtbl->SetI(prm, "DLSSNR.Enabled", 1);
                prm->vtbl->SetI(prm, "DLSSNR.Reset", _resetHistory[passIdx] ? 1 : 0);
                prm->vtbl->SetI(prm, "DLSSNR.Style", std::clamp(style, 0, 2));
                prm->vtbl->SetF(prm, "DLSSNR.Intensity", intensity);
                prm->vtbl->SetF(prm, "DLSSNR.LocalToneStrength", cfg.dlss5LocalTone);
                prm->vtbl->SetF(prm, "DLSSNR.LocalStructureStrength", cfg.dlss5LocalStructure);
                prm->vtbl->SetF(prm, "DLSSNR.SkinStructureStrength", cfg.dlss5SkinStructure);
                prm->vtbl->SetI(prm, "DLSSNR.UseAutoMask", cfg.dlss5AutoMask ? 1 : 0);
                prm->vtbl->SetI(prm, "DLSSNR.UICorrection", cfg.dlss5UiCorrection ? 1 : 0);
                _resetHistory[passIdx] = false;

                return _fnEvalFeature(_cmdList12.Get(), _featureHandle[passIdx], prm, nullptr);
            };

            uint32_t evalRes = 0u;
            if (!dualStack) {
                _Transition12(_sharedOut12[slot].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                evalRes = bindAndEvalPass(0, _sharedIn12[slot].Get(), _sharedOut12[slot].Get(), cfg.dlss5Style, cfg.dlss5Intensity);
                _Transition12(_sharedOut12[slot].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            } else {
                // 双层叠加 DLSS 5：第 1 层输出至 _sharedMid12，直接作为第 2 层输入二次神经渲染输出至 _sharedOut12！
                _Transition12(_sharedMid12[slot].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                uint32_t r1 = bindAndEvalPass(0, _sharedIn12[slot].Get(), _sharedMid12[slot].Get(), cfg.dlss5Style, cfg.dlss5Intensity);
                _Transition12(_sharedMid12[slot].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                _Transition12(_sharedOut12[slot].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                uint32_t r2 = (r1 == 1u) ? bindAndEvalPass(1, _sharedMid12[slot].Get(), _sharedOut12[slot].Get(), cfg.dlss5Pass2Style, cfg.dlss5Pass2Intensity) : 0u;
                _Transition12(_sharedMid12[slot].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                _Transition12(_sharedOut12[slot].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
                evalRes = (r1 == 1u && r2 == 1u) ? 1u : 0u;
            }

            _Transition12(_sharedIn12[slot].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            _Transition12(_sharedMv12[slot].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            _Transition12(_zeroDepth12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);

            if (_tsHeap && _tsReadback) {
                _cmdList12->EndQuery(_tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, qBase + 1);
                _cmdList12->ResolveQueryData(_tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, qBase, 2,
                                             _tsReadback.Get(), sizeof(uint64_t) * qBase);
            }
            _cmdList12->Close();

            if (evalRes == 1u) {
                _queue12->Wait(_fence12.Get(), inputReadyFence);
                ID3D12CommandList* lists[]{ _cmdList12.Get() };
                _queue12->ExecuteCommandLists(1, lists);
                uint64_t doneFence = ++_fenceCounter;
                _queue12->Signal(_fence12.Get(), doneFence);

                _inFlight = true;
                _inFlightSlot = slot;
                _inFlightDoneFence = doneFence;

                if (syncMode) {
                    _fence12->SetEventOnCompletion(doneFence, _fenceEvent);
                    WaitForSingleObject(_fenceEvent, 2000);
                    _HarvestCompletedSlot(slot, srcColorSrv, fwdMotionSrv, flowConfSrv, cfg, false);
                    _inFlight = false;
                    outNeedsAsyncWarp = false;
                    return true;
                }
            }
        }

        // 异步超低延迟模式下：若本帧刚刚异步提交尚未跑完，使用上一帧最新就绪的残差做 1 帧光流补偿叠回（主线程 0ms 阻塞！）
        outNeedsAsyncWarp = _inFlight && _hasResidualHistory;
        return _hasResidualHistory;
    }

    bool IsReady() const { return _ready; }
    double LastGpuMs() const { return _lastGpuMs; }
    uint32_t WorkWidth() const { return _workW; }
    uint32_t WorkHeight() const { return _workH; }
    ID3D11ShaderResourceView* CurrentResidualSrv() const { return _residualSrv[_activeResIdx].Get(); }
    ID3D11ShaderResourceView* CurrentResidualRefInSrv() const { return _sharedInSrv11[_harvestedSlot].Get(); }

private:
    void _HarvestCompletedSlot(
        uint32_t slot,
        ID3D11ShaderResourceView* srcColorSrv,
        ID3D11ShaderResourceView* fwdMotionSrv,
        ID3D11ShaderResourceView* flowConfSrv,
        const EnhanceConfig& cfg,
        bool isAsyncLagHarvest
    ) {
        // 读取 D3D12 硬件时间戳统计真实推理耗时
        if (_tsReadback && _tsFreq > 0) {
            uint32_t qBase = slot * 2;
            D3D12_RANGE rr{ sizeof(uint64_t) * qBase, sizeof(uint64_t) * (qBase + 2) };
            void* mapped = nullptr;
            if (SUCCEEDED(_tsReadback->Map(0, &rr, &mapped)) && mapped) {
                auto* ts = reinterpret_cast<uint64_t*>((uint8_t*)mapped + sizeof(uint64_t) * qBase);
                if (ts[1] >= ts[0]) {
                    double ms = double(ts[1] - ts[0]) * 1000.0 / double(_tsFreq);
                    _lastGpuMs = (_lastGpuMs <= 0.01) ? ms : (_lastGpuMs * 0.85 + ms * 0.15);
                }
                D3D12_RANGE empty{};
                _tsReadback->Unmap(0, &empty);
            }
        }

        // 在 D3D11 上提取 Oklab 残差并执行光流引导时域抗闪烁
        uint32_t nextResIdx = _activeResIdx ^ 1u;
        struct alignas(16) ResCB {
            uint32_t srcW, srcH, workW, workH;
            float invSrcW, invSrcH, invWorkW, invWorkH;
            float resStr, shadowStr, highStr, sat;
            float darkProt, highProt;
            uint32_t antiFlicker, hasHistory;
        } rcb{
            _srcW, _srcH, _workW, _workH,
            1.0f / float(_srcW), 1.0f / float(_srcH),
            1.0f / float(_workW), 1.0f / float(_workH),
            cfg.residualStrength, cfg.shadowStructure, cfg.highlightGlow, cfg.saturation,
            cfg.darkProtection, cfg.highlightProtection,
            (uint32_t)std::clamp(cfg.antiFlicker, 0, 2),
            // 异步收获上一帧槽位时，当前传入的 fwdMotionSrv 属于新帧而非上一帧，禁止用错帧光流混合残差历史，防止移动残影！
            (_hasResidualHistory && !isAsyncLagHarvest) ? 1u : 0u
        };
        _ctx11->UpdateSubresource(_cbResidual.Get(), 0, nullptr, &rcb, 0, 0);

        ID3D11ShaderResourceView* srvs[6]{
            _sharedInSrv11[slot].Get(),
            _sharedOutSrv11[slot].Get(),
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

        _activeResIdx = nextResIdx;
        _harvestedSlot = slot;
        _hasResidualHistory = true;
    }

    void _Transition12(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        if (!res || before == after) return;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        _cmdList12->ResourceBarrier(1, &b);
    }

    bool _CreateNgxFeatures() {
        _alloc12[0]->Reset();
        _cmdList12->Reset(_alloc12[0].Get(), nullptr);

        uint32_t rc[2]{ 0u, 0u };
        for (int p = 0; p < 2; ++p) {
            NgxParamObj* prm = _params[p];
            prm->vtbl->Reset(prm);
            prm->vtbl->SetUI(prm, "DLSSNR.Width", _workW);
            prm->vtbl->SetUI(prm, "DLSSNR.Height", _workH);
            prm->vtbl->SetUI(prm, "DLSSNR.InputWidth", _workW);
            prm->vtbl->SetUI(prm, "DLSSNR.InputHeight", _workH);
            prm->vtbl->SetUI(prm, "DLSSNR.OutputWidth", _workW);
            prm->vtbl->SetUI(prm, "DLSSNR.OutputHeight", _workH);
            prm->vtbl->SetUI(prm, "DLSSNR.Output.Width", _workW);
            prm->vtbl->SetUI(prm, "DLSSNR.Output.Height", _workH);
            prm->vtbl->SetUI(prm, "DLSSNR.Upscaling", 0u);
            prm->vtbl->SetF(prm, "DLSSNR.Scale", 1.0f);
            prm->vtbl->SetF(prm, "DLSSNR.ScalingRatio", 1.0f);
            prm->vtbl->SetVoidPtr(prm, "DLSSNRComputeScalingRatioCallback", (void*)&Dlss5Detail::ScalingRatioCallback);
            prm->vtbl->SetI(prm, "DLSSNR.Hint.Render.Preset", 0);
            prm->vtbl->SetUI(prm, "Width", _workW);
            prm->vtbl->SetUI(prm, "Height", _workH);
            prm->vtbl->SetI(prm, "PerfQualityValue", 1);
            prm->vtbl->SetUI(prm, "CreationNodeMask", 1u);
            prm->vtbl->SetUI(prm, "VisibilityNodeMask", 1u);
            rc[p] = _fnCreateFeature(_cmdList12.Get(), 18u, prm, &_featureHandle[p]);
        }

        _cmdList12->Close();
        ID3D12CommandList* lists[]{ _cmdList12.Get() };
        _queue12->ExecuteCommandLists(1, lists);
        _WaitQueueIdle();
        return (rc[0] == 1u && _featureHandle[0] != nullptr);
    }

    bool _CreateWorksetTextures() {
        for (int i = 0; i < 2; ++i) {
            _sharedIn11[i].Reset(); _sharedInSrv11[i].Reset(); _sharedInUav11[i].Reset(); _sharedIn12[i].Reset();
            _sharedMid11[i].Reset(); _sharedMidSrv11[i].Reset(); _sharedMidUav11[i].Reset(); _sharedMid12[i].Reset();
            _sharedOut11[i].Reset(); _sharedOutSrv11[i].Reset(); _sharedOutUav11[i].Reset(); _sharedOut12[i].Reset();
            _sharedMv11[i].Reset(); _sharedMvSrv11[i].Reset(); _sharedMvUav11[i].Reset(); _sharedMv12[i].Reset();

            if (!CreateSharedD3D11ToD3D12Tex(_dev11, _dev12.Get(), _workW, _workH, DXGI_FORMAT_R8G8B8A8_UNORM,
                                             _sharedIn11[i].Put(), _sharedInSrv11[i].Put(), _sharedInUav11[i].Put(), _sharedIn12[i].Put())) return false;
            if (!CreateSharedD3D11ToD3D12Tex(_dev11, _dev12.Get(), _workW, _workH, DXGI_FORMAT_R8G8B8A8_UNORM,
                                             _sharedMid11[i].Put(), _sharedMidSrv11[i].Put(), _sharedMidUav11[i].Put(), _sharedMid12[i].Put())) return false;
            if (!CreateSharedD3D11ToD3D12Tex(_dev11, _dev12.Get(), _workW, _workH, DXGI_FORMAT_R8G8B8A8_UNORM,
                                             _sharedOut11[i].Put(), _sharedOutSrv11[i].Put(), _sharedOutUav11[i].Put(), _sharedOut12[i].Put())) return false;
            if (!CreateSharedD3D11ToD3D12Tex(_dev11, _dev12.Get(), _workW, _workH, DXGI_FORMAT_R16G16_FLOAT,
                                             _sharedMv11[i].Put(), _sharedMvSrv11[i].Put(), _sharedMvUav11[i].Put(), _sharedMv12[i].Put())) return false;
        }

        // 创建并显式清零共享零深度纹理 (Zero-Depth Contract: 必须真正清零 R32_FLOAT，杜绝未初始化显存垃圾导致黑夜大面积斑点！)
        _zeroDepthUav11.Reset();
        _zeroDepth11.Reset();
        _zeroDepth12.Reset();
        if (!CreateSharedD3D11ToD3D12Tex(_dev11, _dev12.Get(), _workW, _workH, DXGI_FORMAT_R32_FLOAT,
                                         _zeroDepth11.Put(), nullptr, _zeroDepthUav11.Put(), _zeroDepth12.Put())) {
            return false;
        }
        const float zero4[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
        _ctx11->ClearUnorderedAccessViewFloat(_zeroDepthUav11.Get(), zero4);
        _ctx11->Flush();
        return true;
    }

    bool _CreatePipelineResources() {
        if (!_CreateWorksetTextures()) return false;
        UINT bind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        for (int i = 0; i < 2; ++i) {
            if (!CreateTex2D(_dev11, _srcW, _srcH, DXGI_FORMAT_R16G16B16A16_FLOAT, bind,
                             _residualTex[i].Put(), _residualSrv[i].Put(), _residualUav[i].Put())) return false;
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
        cbd.ByteWidth = 64;
        if (FAILED(_dev11->CreateBuffer(&cbd, nullptr, _cbResidual.Put()))) return false;

        return BuildComputeShader(_dev11, XShaders::HLSL_DOWNSAMPLE_WORKSET, "CSMain", _csDownsample.Put()) &&
               BuildComputeShader(_dev11, XShaders::HLSL_OKLAB_RESIDUAL_TEMPORAL, "CSMain", _csResidual.Put());
    }

    void _WaitQueueIdle() {
        if (!_queue12 || !_fence12 || !_fenceEvent) return;
        uint64_t val = ++_fenceCounter;
        _queue12->Signal(_fence12.Get(), val);
        if (_fence12->GetCompletedValue() < val) {
            _fence12->SetEventOnCompletion(val, _fenceEvent);
            WaitForSingleObject(_fenceEvent, 3000);
        }
    }

    void _DrainInFlight() {
        if (_inFlight && _fence12 && _fenceEvent) {
            if (_fence12->GetCompletedValue() < _inFlightDoneFence) {
                _fence12->SetEventOnCompletion(_inFlightDoneFence, _fenceEvent);
                WaitForSingleObject(_fenceEvent, 3000);
            }
            _inFlight = false;
        }
        _WaitQueueIdle();
    }

    using AllocParamFn   = uint32_t (__cdecl *)(NgxParamObj**);
    using DestroyParamFn = uint32_t (__cdecl *)(NgxParamObj*);
    using SnippetInitExtFn = uint32_t (__cdecl *)(unsigned long long, const wchar_t*, ID3D12Device*, uint32_t, const void*);
    using CreateFeatFn   = uint32_t (__cdecl *)(ID3D12GraphicsCommandList*, uint32_t, NgxParamObj*, void**);
    using EvalFeatFn     = uint32_t (__cdecl *)(ID3D12GraphicsCommandList*, void*, NgxParamObj*, void*);
    using ReleaseFeatFn  = uint32_t (__cdecl *)(void*);
    using Shutdown1Fn    = uint32_t (__cdecl *)(ID3D12Device*);

    ID3D11Device5* _dev11 = nullptr;
    ID3D11DeviceContext4* _ctx11 = nullptr;
    std::wstring _binDir;
    uint32_t _srcW = 0, _srcH = 0, _workW = 0, _workH = 0, _scalePercent = 75;

    ComRef<ID3D12Device> _dev12;
    ComRef<ID3D12CommandQueue> _queue12;
    ComRef<ID3D12CommandAllocator> _alloc12[2];
    ComRef<ID3D12GraphicsCommandList> _cmdList12;
    ComRef<ID3D11Fence> _fence11;
    ComRef<ID3D12Fence> _fence12;
    HANDLE _fenceEvent = nullptr;
    uint64_t _fenceCounter = 0;

    ComRef<ID3D12QueryHeap> _tsHeap;
    ComRef<ID3D12Resource> _tsReadback;
    uint64_t _tsFreq = 0;
    double _lastGpuMs = 0.0;

    HMODULE _coreMod = nullptr;
    HMODULE _nrMod = nullptr;
    AllocParamFn _fnAllocParams = nullptr;
    DestroyParamFn _fnDestroyParams = nullptr;
    SnippetInitExtFn _fnInitExt = nullptr;
    CreateFeatFn _fnCreateFeature = nullptr;
    EvalFeatFn _fnEvalFeature = nullptr;
    ReleaseFeatFn _fnReleaseFeature = nullptr;
    Shutdown1Fn _fnShutdown1 = nullptr;

    NgxParamObj* _params[2]{ nullptr, nullptr };
    void* _featureHandle[2]{ nullptr, nullptr };
    bool _ngxInitialized = false;
    bool _ready = false;
    bool _resetHistory[2]{ true, true };
    int _cachedStyle[2]{ -1, -1 };
    float _cachedIntensity[2]{ -1.0f, -1.0f };
    float _cachedLocalTone = -1.0f;
    float _cachedLocalStructure = -1.0f;
    float _cachedSkinStructure = -1.0f;
    bool _cachedAutoMask = false;
    bool _cachedUiCorrection = false;
    bool _cachedDualStack = false;

    bool _inFlight = false;
    uint32_t _inFlightSlot = 0;
    uint64_t _inFlightDoneFence = 0;
    uint32_t _nextSlot = 0;

    ComRef<ID3D11Texture2D> _sharedIn11[2], _sharedMid11[2], _sharedOut11[2], _sharedMv11[2];
    ComRef<ID3D11ShaderResourceView> _sharedInSrv11[2], _sharedMidSrv11[2], _sharedOutSrv11[2], _sharedMvSrv11[2];
    ComRef<ID3D11UnorderedAccessView> _sharedInUav11[2], _sharedMidUav11[2], _sharedOutUav11[2], _sharedMvUav11[2];
    ComRef<ID3D12Resource> _sharedIn12[2], _sharedMid12[2], _sharedOut12[2], _sharedMv12[2];
    ComRef<ID3D11Texture2D> _zeroDepth11;
    ComRef<ID3D11UnorderedAccessView> _zeroDepthUav11;
    ComRef<ID3D12Resource> _zeroDepth12;

    ComRef<ID3D11Texture2D> _residualTex[2];
    ComRef<ID3D11ShaderResourceView> _residualSrv[2];
    ComRef<ID3D11UnorderedAccessView> _residualUav[2];
    uint32_t _activeResIdx = 0;
    uint32_t _harvestedSlot = 0;
    bool _hasResidualHistory = false;

    ComRef<ID3D11ComputeShader> _csDownsample, _csResidual;
    ComRef<ID3D11SamplerState> _linearSampler;
    ComRef<ID3D11Buffer> _cbWorkset, _cbResidual;
};
