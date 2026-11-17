#pragma once
// XStreaming Desktop Enhanced - D3D11/D3D12 共享纹理与运行时着色器编译辅助工具

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

template <typename T>
struct ComRef {
    T* ptr = nullptr;
    ComRef() = default;
    ~ComRef() { Reset(); }
    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;
    ComRef(ComRef&& o) noexcept : ptr(o.ptr) { o.ptr = nullptr; }
    ComRef& operator=(ComRef&& o) noexcept {
        if (this != &o) { Reset(); ptr = o.ptr; o.ptr = nullptr; }
        return *this;
    }
    void Reset() { if (ptr) { ptr->Release(); ptr = nullptr; } }
    T** Put() { Reset(); return &ptr; }
    T* Get() const { return ptr; }
    T* operator->() const { return ptr; }
    explicit operator bool() const { return ptr != nullptr; }
};

inline bool CompileHlslShader(const char* src, const char* entry, const char* target, ID3DBlob** outBlob) {
    static HMODULE s_compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!s_compiler) return false;
    using CompileFn = HRESULT (WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    static auto fnCompile = reinterpret_cast<CompileFn>(GetProcAddress(s_compiler, "D3DCompile"));
    if (!fnCompile) return false;

    ComRef<ID3DBlob> errBlob;
    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    HRESULT hr = fnCompile(src, std::strlen(src), "XStreamingShader.hlsl", nullptr, nullptr,
                           entry, target, flags, 0, outBlob, errBlob.Put());
    if (FAILED(hr)) {
        if (errBlob) {
            std::fprintf(stderr, "[ShaderCompileError %s/%s]: %.*s\n",
                         entry, target, (int)errBlob->GetBufferSize(), (const char*)errBlob->GetBufferPointer());
        }
        return false;
    }
    return true;
}

inline bool BuildComputeShader(ID3D11Device* dev, const char* src, const char* entry, ID3D11ComputeShader** outCs) {
    ComRef<ID3DBlob> blob;
    if (!CompileHlslShader(src, entry, "cs_5_0", blob.Put())) return false;
    return SUCCEEDED(dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, outCs));
}

inline bool CreateTex2D(
    ID3D11Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT fmt, UINT bindFlags,
    ID3D11Texture2D** outTex,
    ID3D11ShaderResourceView** outSrv = nullptr,
    ID3D11UnorderedAccessView** outUav = nullptr,
    UINT miscFlags = 0
) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bindFlags;
    td.MiscFlags = miscFlags;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, outTex))) return false;
    if (outSrv && (bindFlags & D3D11_BIND_SHADER_RESOURCE)) {
        if (FAILED(dev->CreateShaderResourceView(*outTex, nullptr, outSrv))) return false;
    }
    if (outUav && (bindFlags & D3D11_BIND_UNORDERED_ACCESS)) {
        if (FAILED(dev->CreateUnorderedAccessView(*outTex, nullptr, outUav))) return false;
    }
    return true;
}

inline bool CreateSharedD3D11ToD3D12Tex(
    ID3D11Device* dev11, ID3D12Device* dev12,
    uint32_t w, uint32_t h, DXGI_FORMAT fmt,
    ID3D11Texture2D** outTex11,
    ID3D11ShaderResourceView** outSrv11,
    ID3D11UnorderedAccessView** outUav11,
    ID3D12Resource** outTex12
) {
    UINT bind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    UINT misc = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    if (!CreateTex2D(dev11, w, h, fmt, bind, outTex11, outSrv11, outUav11, misc)) return false;

    ComRef<IDXGIResource1> dxgiRes;
    if (FAILED((*outTex11)->QueryInterface(__uuidof(IDXGIResource1), (void**)dxgiRes.Put()))) return false;
    HANDLE hShared = nullptr;
    if (FAILED(dxgiRes->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &hShared))) return false;
    HRESULT hr = dev12->OpenSharedHandle(hShared, __uuidof(ID3D12Resource), (void**)outTex12);
    CloseHandle(hShared);
    return SUCCEEDED(hr);
}
