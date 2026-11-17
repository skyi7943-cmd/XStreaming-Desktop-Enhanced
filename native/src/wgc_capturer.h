#pragma once
// XStreaming Desktop Enhanced - Windows.Graphics.Capture 零拷贝单窗口捕获引擎
// 核心价值：仅捕获 XStreaming 原始底窗 DWM 表面，彻底无视上层增强覆盖窗 (overlayHwnd)！
// 从而允许 overlayHwnd 设为 WDA_NONE (完全开放录屏权限)，让 OBS / NVIDIA ShadowPlay / 内置 F9 录屏 100% 正常录制增强后的 60FPS + DLSS 5 画面且零自捕获回环！

#include "d3d_helpers.h"
#include <dwmapi.h>

#ifndef WDA_NONE
#define WDA_NONE 0x00000000
#endif

namespace WgcAbi {
    struct HSTRING__ { int unused; };
    using HSTRING = HSTRING__*;

    struct SizeInt32 {
        int32_t Width;
        int32_t Height;
    };

    // WinRT 标准 COM GUID 定义
    static constexpr GUID IID_IClosable =
        { 0x30D5A829, 0x7FA4, 0x4026, { 0x83, 0xBB, 0xD7, 0x5B, 0xAE, 0x4E, 0xA9, 0x9E } };
    static constexpr GUID IID_IDirect3DDevice =
        { 0xA37624AB, 0x8D5F, 0x4650, { 0x9D, 0x3E, 0x9E, 0xAE, 0x3D, 0x9B, 0xC6, 0x70 } };
    static constexpr GUID IID_IGraphicsCaptureItemInterop =
        { 0x3628E81B, 0x3CAC, 0x4C60, { 0xB7, 0xF4, 0x23, 0xCE, 0x0E, 0x0C, 0x33, 0x56 } };
    static constexpr GUID IID_IGraphicsCaptureItem =
        { 0x79C3F95B, 0x31F7, 0x4EC2, { 0xA4, 0x64, 0x63, 0x2E, 0xF5, 0xD3, 0x07, 0x60 } };
    static constexpr GUID IID_IDirect3D11CaptureFramePoolStatics2 =
        { 0x589B103F, 0x6BBC, 0x5DF5, { 0xA9, 0x91, 0x02, 0xE2, 0x8B, 0x3B, 0x66, 0xD5 } };
    static constexpr GUID IID_IGraphicsCaptureSession2 =
        { 0x2C39AE40, 0x7D2E, 0x5044, { 0x80, 0x4E, 0x8B, 0x67, 0x99, 0xD4, 0xCF, 0x9E } };
    static constexpr GUID IID_IGraphicsCaptureSession3 =
        { 0xF2CDD966, 0x22AE, 0x5EA1, { 0x95, 0x96, 0x3A, 0x28, 0x93, 0x44, 0xC3, 0xBE } };
    static constexpr GUID IID_IGraphicsCaptureSession5 =
        { 0x67C0EA62, 0x1F85, 0x5061, { 0x92, 0x5A, 0x23, 0x9B, 0xE0, 0xAC, 0x09, 0xCB } };
    static constexpr GUID IID_IGraphicsCaptureSession6 =
        { 0xD7419236, 0xBE20, 0x5E9F, { 0xBC, 0xD6, 0xC4, 0xE9, 0x8F, 0xD6, 0xAF, 0xDC } };
    static constexpr GUID IID_IDirect3DDxgiInterfaceAccess =
        { 0xA9B3D012, 0x3DF2, 0x4EE3, { 0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1 } };

    // IUnknown 派生接口 (3 个基础槽位)
    struct GraphicsCaptureItemInteropVtbl {
        HRESULT (STDMETHODCALLTYPE *QueryInterface)(void* self, REFIID riid, void** ppv);
        ULONG   (STDMETHODCALLTYPE *AddRef)(void* self);
        ULONG   (STDMETHODCALLTYPE *Release)(void* self);
        HRESULT (STDMETHODCALLTYPE *CreateForWindow)(void* self, HWND window, REFIID riid, void** result);
        HRESULT (STDMETHODCALLTYPE *CreateForMonitor)(void* self, HMONITOR monitor, REFIID riid, void** result);
    };
    struct GraphicsCaptureItemInteropRaw { const GraphicsCaptureItemInteropVtbl* vtbl; };

    struct DxgiInterfaceAccessVtbl {
        HRESULT (STDMETHODCALLTYPE *QueryInterface)(void* self, REFIID riid, void** ppv);
        ULONG   (STDMETHODCALLTYPE *AddRef)(void* self);
        ULONG   (STDMETHODCALLTYPE *Release)(void* self);
        HRESULT (STDMETHODCALLTYPE *GetInterface)(void* self, REFIID iid, void** p);
    };
    struct DxgiInterfaceAccessRaw { const DxgiInterfaceAccessVtbl* vtbl; };

    // IInspectable 派生接口 (6 个基础槽位: QI, AddRef, Release, GetIids, GetRuntimeClassName, GetTrustLevel)
    struct ClosableVtbl {
        HRESULT (STDMETHODCALLTYPE *QueryInterface)(void* self, REFIID riid, void** ppv);
        ULONG   (STDMETHODCALLTYPE *AddRef)(void* self);
        ULONG   (STDMETHODCALLTYPE *Release)(void* self);
        void* p3; void* p4; void* p5;
        HRESULT (STDMETHODCALLTYPE *Close)(void* self);
    };
    struct ClosableRaw { const ClosableVtbl* vtbl; };

    struct CaptureItemVtbl {
        HRESULT (STDMETHODCALLTYPE *QueryInterface)(void* self, REFIID riid, void** ppv);
        ULONG   (STDMETHODCALLTYPE *AddRef)(void* self);
        ULONG   (STDMETHODCALLTYPE *Release)(void* self);
        void* p3; void* p4; void* p5;
        HRESULT (STDMETHODCALLTYPE *get_DisplayName)(void* self, HSTRING* value);
        HRESULT (STDMETHODCALLTYPE *get_Size)(void* self, SizeInt32* value);
    };
    struct CaptureItemRaw { const CaptureItemVtbl* vtbl; };

    struct FramePoolStatics2Vtbl {
        HRESULT (STDMETHODCALLTYPE *QueryInterface)(void* self, REFIID riid, void** ppv);
        ULONG   (STDMETHODCALLTYPE *AddRef)(void* self);
        ULONG   (STDMETHODCALLTYPE *Release)(void* self);
        void* p3; void* p4; void* p5;
        HRESULT (STDMETHODCALLTYPE *CreateFreeThreaded)(void* self, void* device, int32_t pixelFormat, int32_t numberOfBuffers, SizeInt32 size, void** result);
    };
    struct FramePoolStatics2Raw { const FramePoolStatics2Vtbl* vtbl; };

    struct FramePoolVtbl {
        HRESULT (STDMETHODCALLTYPE *QueryInterface)(void* self, REFIID riid, void** ppv);
        ULONG   (STDMETHODCALLTYPE *AddRef)(void* self);
        ULONG   (STDMETHODCALLTYPE *Release)(void* self);
        void* p3; void* p4; void* p5;
        HRESULT (STDMETHODCALLTYPE *Recreate)(void* self, void* device, int32_t pixelFormat, int32_t numberOfBuffers, SizeInt32 size);
        HRESULT (STDMETHODCALLTYPE *TryGetNextFrame)(void* self, void** value);
        HRESULT (STDMETHODCALLTYPE *add_FrameArrived)(void* self, void* handler, int64_t* token);
        HRESULT (STDMETHODCALLTYPE *remove_FrameArrived)(void* self, int64_t token);
        HRESULT (STDMETHODCALLTYPE *CreateCaptureSession)(void* self, void* item, void** value);
    };
    struct FramePoolRaw { const FramePoolVtbl* vtbl; };

    struct CaptureSessionVtbl {
        HRESULT (STDMETHODCALLTYPE *QueryInterface)(void* self, REFIID riid, void** ppv);
        ULONG   (STDMETHODCALLTYPE *AddRef)(void* self);
        ULONG   (STDMETHODCALLTYPE *Release)(void* self);
        void* p3; void* p4; void* p5;
        HRESULT (STDMETHODCALLTYPE *StartCapture)(void* self);
    };
    struct CaptureSessionRaw { const CaptureSessionVtbl* vtbl; };

    struct CaptureSessionToggleVtbl {
        HRESULT (STDMETHODCALLTYPE *QueryInterface)(void* self, REFIID riid, void** ppv);
        ULONG   (STDMETHODCALLTYPE *AddRef)(void* self);
        ULONG   (STDMETHODCALLTYPE *Release)(void* self);
        void* p3; void* p4; void* p5;
        HRESULT (STDMETHODCALLTYPE *get_Flag)(void* self, uint8_t* value);
        HRESULT (STDMETHODCALLTYPE *put_Flag)(void* self, uint8_t value);
    };
    struct CaptureSessionToggleRaw { const CaptureSessionToggleVtbl* vtbl; };

    struct CaptureSessionIntervalVtbl {
        HRESULT (STDMETHODCALLTYPE *QueryInterface)(void* self, REFIID riid, void** ppv);
        ULONG   (STDMETHODCALLTYPE *AddRef)(void* self);
        ULONG   (STDMETHODCALLTYPE *Release)(void* self);
        void* p3; void* p4; void* p5;
        HRESULT (STDMETHODCALLTYPE *get_MinUpdateInterval)(void* self, int64_t* value);
        HRESULT (STDMETHODCALLTYPE *put_MinUpdateInterval)(void* self, int64_t value);
    };
    struct CaptureSessionIntervalRaw { const CaptureSessionIntervalVtbl* vtbl; };

    struct CaptureFrameVtbl {
        HRESULT (STDMETHODCALLTYPE *QueryInterface)(void* self, REFIID riid, void** ppv);
        ULONG   (STDMETHODCALLTYPE *AddRef)(void* self);
        ULONG   (STDMETHODCALLTYPE *Release)(void* self);
        void* p3; void* p4; void* p5;
        HRESULT (STDMETHODCALLTYPE *get_Surface)(void* self, void** value);
        HRESULT (STDMETHODCALLTYPE *get_SystemRelativeTime)(void* self, int64_t* value);
        HRESULT (STDMETHODCALLTYPE *get_ContentSize)(void* self, SizeInt32* value);
    };
    struct CaptureFrameRaw { const CaptureFrameVtbl* vtbl; };

    inline void CloseAndRelease(void*& obj) {
        if (!obj) return;
        auto* unk = reinterpret_cast<ClosableRaw*>(obj);
        ClosableRaw* closable = nullptr;
        if (SUCCEEDED(unk->vtbl->QueryInterface(obj, IID_IClosable, (void**)&closable)) && closable) {
            closable->vtbl->Close(closable);
            closable->vtbl->Release(closable);
        }
        unk->vtbl->Release(obj);
        obj = nullptr;
    }

    inline void SafeRelease(void*& obj) {
        if (!obj) return;
        auto* unk = reinterpret_cast<ClosableRaw*>(obj);
        unk->vtbl->Release(obj);
        obj = nullptr;
    }
}

class WindowGraphicsCapturer {
public:
    ~WindowGraphicsCapturer() { Shutdown(); }

    bool Initialize(ID3D11Device5* dev11, HWND targetHwnd) {
        Shutdown();
        if (!dev11 || !targetHwnd || !IsWindow(targetHwnd) || targetHwnd == GetDesktopWindow()) return false;

        _combaseMod = LoadLibraryExW(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        _d3d11Mod = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!_combaseMod || !_d3d11Mod) return false;

        using RoInitFn = HRESULT (WINAPI*)(int);
        using WinCreateStrFn = HRESULT (WINAPI*)(const wchar_t*, uint32_t, WgcAbi::HSTRING*);
        using WinDeleteStrFn = HRESULT (WINAPI*)(WgcAbi::HSTRING);
        using RoGetFacFn = HRESULT (WINAPI*)(WgcAbi::HSTRING, REFIID, void**);
        using CreateWinRtDevFn = HRESULT (WINAPI*)(IDXGIDevice*, IUnknown**);

        auto fnRoInit = reinterpret_cast<RoInitFn>(GetProcAddress(_combaseMod, "RoInitialize"));
        auto fnCreateStr = reinterpret_cast<WinCreateStrFn>(GetProcAddress(_combaseMod, "WindowsCreateString"));
        auto fnDeleteStr = reinterpret_cast<WinDeleteStrFn>(GetProcAddress(_combaseMod, "WindowsDeleteString"));
        auto fnGetFac = reinterpret_cast<RoGetFacFn>(GetProcAddress(_combaseMod, "RoGetActivationFactory"));
        auto fnCreateDev = reinterpret_cast<CreateWinRtDevFn>(GetProcAddress(_d3d11Mod, "CreateDirect3D11DeviceFromDXGIDevice"));
        if (!fnRoInit || !fnCreateStr || !fnDeleteStr || !fnGetFac || !fnCreateDev) { _failStep = 1; return false; }

        fnRoInit(1); // RO_INIT_MULTITHREADED

        ComRef<IDXGIDevice> dxgiDev;
        if (FAILED(_lastHr = dev11->QueryInterface(__uuidof(IDXGIDevice), (void**)dxgiDev.Put()))) { _failStep = 2; return false; }

        ComRef<IUnknown> inspectableDev;
        if (FAILED(_lastHr = fnCreateDev(dxgiDev.Get(), inspectableDev.Put())) || !inspectableDev) { _failStep = 3; return false; }
        if (FAILED(_lastHr = inspectableDev->QueryInterface(WgcAbi::IID_IDirect3DDevice, &_winrtDevice)) || !_winrtDevice) {
            _failStep = 4;
            Shutdown();
            return false;
        }

        // 1. 为 targetHwnd 创建 GraphicsCaptureItem（仅捕获目标窗口自身表面，完全无视覆盖在上面的 overlayHwnd！）
        const wchar_t* itemClass = L"Windows.Graphics.Capture.GraphicsCaptureItem";
        WgcAbi::HSTRING hsItem = nullptr;
        fnCreateStr(itemClass, (uint32_t)std::wcslen(itemClass), &hsItem);
        WgcAbi::GraphicsCaptureItemInteropRaw* interop = nullptr;
        HRESULT hr = fnGetFac(hsItem, WgcAbi::IID_IGraphicsCaptureItemInterop, (void**)&interop);
        fnDeleteStr(hsItem);
        if (FAILED(hr) || !interop) {
            _lastHr = hr; _failStep = 5;
            Shutdown();
            return false;
        }

        hr = interop->vtbl->CreateForWindow(interop, targetHwnd, WgcAbi::IID_IGraphicsCaptureItem, &_captureItem);
        interop->vtbl->Release(interop);
        if (FAILED(hr) || !_captureItem) {
            _lastHr = hr; _failStep = 6;
            Shutdown();
            return false;
        }

        auto* itemObj = reinterpret_cast<WgcAbi::CaptureItemRaw*>(_captureItem);
        WgcAbi::SizeInt32 itemSize{ 1280, 800 };
        if (FAILED(itemObj->vtbl->get_Size(_captureItem, &itemSize)) || itemSize.Width < 64 || itemSize.Height < 64) {
            RECT r{};
            GetWindowRect(targetHwnd, &r);
            itemSize.Width = std::max<int32_t>(128, r.right - r.left);
            itemSize.Height = std::max<int32_t>(128, r.bottom - r.top);
        }
        _poolWidth = itemSize.Width;
        _poolHeight = itemSize.Height;

        // 2. 创建 FreeThreaded 零拷贝帧池 (87 = B8G8R8A8UIntNormalized, 2 缓冲防丢帧)
        const wchar_t* poolClass = L"Windows.Graphics.Capture.Direct3D11CaptureFramePool";
        WgcAbi::HSTRING hsPool = nullptr;
        fnCreateStr(poolClass, (uint32_t)std::wcslen(poolClass), &hsPool);
        WgcAbi::FramePoolStatics2Raw* poolStatics = nullptr;
        hr = fnGetFac(hsPool, WgcAbi::IID_IDirect3D11CaptureFramePoolStatics2, (void**)&poolStatics);
        fnDeleteStr(hsPool);
        if (FAILED(hr) || !poolStatics) {
            _lastHr = hr; _failStep = 7;
            Shutdown();
            return false;
        }

        hr = poolStatics->vtbl->CreateFreeThreaded(poolStatics, _winrtDevice, 87, 2, itemSize, &_framePool);
        poolStatics->vtbl->Release(poolStatics);
        if (FAILED(hr) || !_framePool) {
            _lastHr = hr; _failStep = 8;
            Shutdown();
            return false;
        }

        auto* poolObj = reinterpret_cast<WgcAbi::FramePoolRaw*>(_framePool);
        hr = poolObj->vtbl->CreateCaptureSession(_framePool, _captureItem, &_session);
        if (FAILED(hr) || !_session) {
            _lastHr = hr; _failStep = 9;
            Shutdown();
            return false;
        }

        // 关闭捕获鼠标指针与黄色高亮边框 (Windows 11 原生免权限支持)
        auto* sessObj = reinterpret_cast<WgcAbi::CaptureSessionRaw*>(_session);
        WgcAbi::CaptureSessionToggleRaw* s2 = nullptr;
        if (SUCCEEDED(sessObj->vtbl->QueryInterface(_session, WgcAbi::IID_IGraphicsCaptureSession2, (void**)&s2)) && s2) {
            s2->vtbl->put_Flag(s2, 0); // IsCursorCaptureEnabled = false
            s2->vtbl->Release(s2);
        }
        WgcAbi::CaptureSessionToggleRaw* s3 = nullptr;
        if (SUCCEEDED(sessObj->vtbl->QueryInterface(_session, WgcAbi::IID_IGraphicsCaptureSession3, (void**)&s3)) && s3) {
            s3->vtbl->put_Flag(s3, 0); // IsBorderRequired = false
            s3->vtbl->Release(s3);
        }
        // Windows 11 24H2 解除最小帧间隔节流，并禁止把附属二级弹窗混入底窗纹理
        WgcAbi::CaptureSessionIntervalRaw* s5 = nullptr;
        if (SUCCEEDED(sessObj->vtbl->QueryInterface(_session, WgcAbi::IID_IGraphicsCaptureSession5, (void**)&s5)) && s5) {
            s5->vtbl->put_MinUpdateInterval(s5, 0);
            s5->vtbl->Release(s5);
        }
        WgcAbi::CaptureSessionToggleRaw* s6 = nullptr;
        if (SUCCEEDED(sessObj->vtbl->QueryInterface(_session, WgcAbi::IID_IGraphicsCaptureSession6, (void**)&s6)) && s6) {
            s6->vtbl->put_Flag(s6, 0); // IncludeSecondaryWindows = false
            s6->vtbl->Release(s6);
        }

        if (FAILED(hr = sessObj->vtbl->StartCapture(_session))) {
            _lastHr = hr; _failStep = 10;
            Shutdown();
            return false;
        }

        _targetHwnd = targetHwnd;
        _active = true;
        return true;
    }

    void Shutdown() {
        _active = false;
        WgcAbi::CloseAndRelease(_session);
        WgcAbi::CloseAndRelease(_framePool);
        WgcAbi::SafeRelease(_captureItem);
        WgcAbi::CloseAndRelease(_winrtDevice);
        if (_d3d11Mod) { FreeLibrary(_d3d11Mod); _d3d11Mod = nullptr; }
        if (_combaseMod) { FreeLibrary(_combaseMod); _combaseMod = nullptr; }
        _targetHwnd = nullptr;
    }

    bool IsActive() const { return _active; }
    int FailStep() const { return _failStep; }
    HRESULT LastHr() const { return _lastHr; }

    // 非阻塞尝试获取最新一帧目标窗口 GPU 纹理并裁剪出客户区视口拷贝至 dstCandidateTex
    bool TryAcquireAndCopyViewport(ID3D11DeviceContext4* ctx11, const RECT& vpScreenRect, uint32_t cropW, uint32_t cropH, ID3D11Texture2D* dstCandidateTex) {
        if (!_active || !_framePool) return false;
        auto* poolObj = reinterpret_cast<WgcAbi::FramePoolRaw*>(_framePool);

        // 若窗口里有多个排队帧，直接排空旧帧只取最新一帧，确保 0 帧积压最低延迟！
        void* latestFrame = nullptr;
        for (int iter = 0; iter < 4; ++iter) {
            void* nextF = nullptr;
            if (FAILED(poolObj->vtbl->TryGetNextFrame(_framePool, &nextF)) || !nextF) break;
            if (latestFrame) WgcAbi::CloseAndRelease(latestFrame);
            latestFrame = nextF;
        }
        if (!latestFrame) return false;

        auto* frameObj = reinterpret_cast<WgcAbi::CaptureFrameRaw*>(latestFrame);
        int64_t sysRelTime = 0;
        if (SUCCEEDED(frameObj->vtbl->get_SystemRelativeTime(latestFrame, &sysRelTime)) && sysRelTime != 0) {
            if (sysRelTime == _lastSysRelTime) {
                WgcAbi::CloseAndRelease(latestFrame);
                return false;
            }
            _lastSysRelTime = sysRelTime;
        }
        WgcAbi::SizeInt32 contentSize{ _poolWidth, _poolHeight };
        frameObj->vtbl->get_ContentSize(latestFrame, &contentSize);

        bool copied = false;
        void* surface = nullptr;
        if (SUCCEEDED(frameObj->vtbl->get_Surface(latestFrame, &surface)) && surface) {
            auto* surfUnk = reinterpret_cast<WgcAbi::ClosableRaw*>(surface);
            WgcAbi::DxgiInterfaceAccessRaw* access = nullptr;
            if (SUCCEEDED(surfUnk->vtbl->QueryInterface(surface, WgcAbi::IID_IDirect3DDxgiInterfaceAccess, (void**)&access)) && access) {
                ComRef<ID3D11Texture2D> srcTex;
                if (SUCCEEDED(access->vtbl->GetInterface(access, __uuidof(ID3D11Texture2D), (void**)srcTex.Put())) && srcTex) {
                    D3D11_TEXTURE2D_DESC td{};
                    srcTex->GetDesc(&td);

                    // 计算视口相对于窗口 DWM Extended Frame Bounds 的精确像素偏移（自动扣除窗口标题栏阴影边框）
                    RECT frameBounds{};
                    if (FAILED(DwmGetWindowAttribute(_targetHwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frameBounds, sizeof(frameBounds)))) {
                        GetWindowRect(_targetHwnd, &frameBounds);
                    }
                    int offX = std::clamp<int>(vpScreenRect.left - frameBounds.left, 0, std::max<int>(0, int(td.Width) - 1));
                    int offY = std::clamp<int>(vpScreenRect.top - frameBounds.top, 0, std::max<int>(0, int(td.Height) - 1));
                    uint32_t copyW = std::min<uint32_t>(cropW, td.Width - (uint32_t)offX);
                    uint32_t copyH = std::min<uint32_t>(cropH, td.Height - (uint32_t)offY);

                    if (copyW >= 64 && copyH >= 64) {
                        D3D11_BOX box{ (UINT)offX, (UINT)offY, 0, (UINT)(offX + copyW), (UINT)(offY + copyH), 1 };
                        ctx11->CopySubresourceRegion(dstCandidateTex, 0, 0, 0, 0, srcTex.Get(), 0, &box);
                        copied = true;
                    }
                }
                access->vtbl->Release(access);
            }
            surfUnk->vtbl->Release(surface);
        }

        WgcAbi::CloseAndRelease(latestFrame);

        // 若窗口尺寸发生改变，动态 Recreate 帧池尺寸避免裁剪越界
        if (contentSize.Width >= 64 && contentSize.Height >= 64 &&
            (std::abs(contentSize.Width - _poolWidth) > 2 || std::abs(contentSize.Height - _poolHeight) > 2)) {
            _poolWidth = contentSize.Width;
            _poolHeight = contentSize.Height;
            poolObj->vtbl->Recreate(_framePool, _winrtDevice, 87, 2, contentSize);
        }
        return copied;
    }

private:
    HMODULE _combaseMod = nullptr;
    HMODULE _d3d11Mod = nullptr;
    HWND _targetHwnd = nullptr;
    bool _active = false;
    int _failStep = 0;
    HRESULT _lastHr = S_OK;
    int32_t _poolWidth = 0;
    int32_t _poolHeight = 0;
    int64_t _lastSysRelTime = 0;

    void* _winrtDevice = nullptr;
    void* _captureItem = nullptr;
    void* _framePool = nullptr;
    void* _session = nullptr;
};
