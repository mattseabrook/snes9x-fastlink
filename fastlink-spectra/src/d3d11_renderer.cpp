#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <cstring>

#include "d3d11_renderer.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace {

template <typename T>
void SafeRelease(T *&obj)
{
    if (obj) {
        obj->Release();
        obj = nullptr;
    }
}

static const char *kVsSrc = R"(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};
VSOut main(uint id : SV_VertexID) {
    VSOut o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.uv = p;
    o.pos = float4(p.x * 2.0f - 1.0f, 1.0f - p.y * 2.0f, 0.0f, 1.0f);
    return o;
}
)";

static const char *kPsSrc = R"(
Texture2D frameTex : register(t0);
SamplerState sam0 : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return frameTex.Sample(sam0, uv);
}
)";

} // namespace

D3D11Renderer::D3D11Renderer()
    : device_(nullptr), context_(nullptr), swapChain_(nullptr), rtv_(nullptr),
      frameTexture_(nullptr), frameSrv_(nullptr), sampler_(nullptr), vs_(nullptr), ps_(nullptr),
      backbufferWidth_(0), backbufferHeight_(0), frameWidth_(0), frameHeight_(0)
{
}

D3D11Renderer::~D3D11Renderer()
{
    Shutdown();
}

bool D3D11Renderer::Initialize(void *hwnd, int width, int height)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = width;
    sd.BufferDesc.Height = height;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = reinterpret_cast<HWND>(hwnd);
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr,
                                               D3D_DRIVER_TYPE_HARDWARE,
                                               nullptr,
                                               flags,
                                               &level,
                                               1,
                                               D3D11_SDK_VERSION,
                                               &sd,
                                               &swapChain_,
                                               &device_,
                                               nullptr,
                                               &context_);
    if (FAILED(hr))
        return false;

    if (!CreateBackbuffer())
        return false;
    if (!CreateFrameTexture(width, height))
        return false;
    if (!CreateShaders())
        return false;

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = device_->CreateSamplerState(&samplerDesc, &sampler_);
    if (FAILED(hr))
        return false;

    backbufferWidth_ = width;
    backbufferHeight_ = height;
    return true;
}

void D3D11Renderer::Shutdown()
{
    SafeRelease(sampler_);
    SafeRelease(vs_);
    SafeRelease(ps_);
    DestroyFrameTexture();
    DestroyBackbuffer();
    SafeRelease(swapChain_);
    SafeRelease(context_);
    SafeRelease(device_);
}

bool D3D11Renderer::CreateBackbuffer()
{
    ID3D11Texture2D *backBuffer = nullptr;
    HRESULT hr = swapChain_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&backBuffer));
    if (FAILED(hr))
        return false;

    hr = device_->CreateRenderTargetView(backBuffer, nullptr, &rtv_);
    backBuffer->Release();
    return SUCCEEDED(hr);
}

void D3D11Renderer::DestroyBackbuffer()
{
    SafeRelease(rtv_);
}

bool D3D11Renderer::CreateFrameTexture(int width, int height)
{
    frameWidth_ = width;
    frameHeight_ = height;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(width);
    td.Height = static_cast<UINT>(height);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = device_->CreateTexture2D(&td, nullptr, &frameTexture_);
    if (FAILED(hr))
        return false;

    hr = device_->CreateShaderResourceView(frameTexture_, nullptr, &frameSrv_);
    return SUCCEEDED(hr);
}

void D3D11Renderer::DestroyFrameTexture()
{
    SafeRelease(frameSrv_);
    SafeRelease(frameTexture_);
    frameWidth_ = 0;
    frameHeight_ = 0;
}

bool D3D11Renderer::CreateShaders()
{
    ID3DBlob *vsBlob = nullptr;
    ID3DBlob *psBlob = nullptr;
    ID3DBlob *errBlob = nullptr;

    HRESULT hr = D3DCompile(kVsSrc, std::strlen(kVsSrc), nullptr, nullptr, nullptr,
                            "main", "vs_5_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) {
        SafeRelease(errBlob);
        return false;
    }
    SafeRelease(errBlob);

    hr = D3DCompile(kPsSrc, std::strlen(kPsSrc), nullptr, nullptr, nullptr,
                    "main", "ps_5_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr)) {
        SafeRelease(vsBlob);
        SafeRelease(errBlob);
        return false;
    }
    SafeRelease(errBlob);

    hr = device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs_);
    if (FAILED(hr)) {
        SafeRelease(vsBlob);
        SafeRelease(psBlob);
        return false;
    }

    hr = device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps_);
    SafeRelease(vsBlob);
    SafeRelease(psBlob);
    return SUCCEEDED(hr);
}

void D3D11Renderer::Resize(int width, int height)
{
    if (!swapChain_ || width <= 0 || height <= 0)
        return;

    backbufferWidth_ = width;
    backbufferHeight_ = height;

    DestroyBackbuffer();

    swapChain_->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_UNKNOWN, 0);
    CreateBackbuffer();

    if (width != frameWidth_ || height != frameHeight_) {
        DestroyFrameTexture();
        CreateFrameTexture(width, height);
    }
}

bool D3D11Renderer::UploadFrame(const uint32_t *pixels, int width, int height)
{
    if (!frameTexture_ || !pixels || width <= 0 || height <= 0)
        return false;

    if (width != frameWidth_ || height != frameHeight_) {
        DestroyFrameTexture();
        if (!CreateFrameTexture(width, height))
            return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = context_->Map(frameTexture_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr))
        return false;

    const uint8_t *src = reinterpret_cast<const uint8_t *>(pixels);
    uint8_t *dst = reinterpret_cast<uint8_t *>(mapped.pData);
    const int rowBytes = width * static_cast<int>(sizeof(uint32_t));

    for (int y = 0; y < height; ++y) {
        std::memcpy(dst + y * mapped.RowPitch, src + y * rowBytes, rowBytes);
    }

    context_->Unmap(frameTexture_, 0);
    return true;
}

void D3D11Renderer::Render()
{
    if (!context_ || !rtv_)
        return;

    float clearColor[4] = { 0.04f, 0.04f, 0.06f, 1.0f };
    context_->OMSetRenderTargets(1, &rtv_, nullptr);
    context_->ClearRenderTargetView(rtv_, clearColor);

    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(backbufferWidth_);
    vp.Height = static_cast<float>(backbufferHeight_);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &vp);

    context_->VSSetShader(vs_, nullptr, 0);
    context_->PSSetShader(ps_, nullptr, 0);
    context_->PSSetShaderResources(0, 1, &frameSrv_);
    context_->PSSetSamplers(0, 1, &sampler_);

    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->Draw(3, 0);

    ID3D11ShaderResourceView *nullSrv = nullptr;
    context_->PSSetShaderResources(0, 1, &nullSrv);

    swapChain_->Present(1, 0);
}
