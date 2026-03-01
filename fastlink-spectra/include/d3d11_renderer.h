#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;
struct ID3D11RenderTargetView;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;
struct ID3D11SamplerState;
struct ID3D11VertexShader;
struct ID3D11PixelShader;

class D3D11Renderer {
public:
    D3D11Renderer();
    ~D3D11Renderer();

    bool Initialize(void *hwnd, int width, int height);
    void Shutdown();
    void Resize(int width, int height);

    bool UploadFrame(const uint32_t *pixels, int width, int height);
    void Render();

private:
    bool CreateBackbuffer();
    void DestroyBackbuffer();
    bool CreateFrameTexture(int width, int height);
    void DestroyFrameTexture();
    bool CreateShaders();

    ID3D11Device *device_;
    ID3D11DeviceContext *context_;
    IDXGISwapChain *swapChain_;
    ID3D11RenderTargetView *rtv_;
    ID3D11Texture2D *frameTexture_;
    ID3D11ShaderResourceView *frameSrv_;
    ID3D11SamplerState *sampler_;
    ID3D11VertexShader *vs_;
    ID3D11PixelShader *ps_;

    int backbufferWidth_;
    int backbufferHeight_;
    int frameWidth_;
    int frameHeight_;
};
