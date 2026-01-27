#include "VisualizationWindow.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <d3d9.h>

#include "../snes9x.h"
#include "../memmap.h"
#include "resource.h"

namespace {

static constexpr int kTexWidth = 512;
static constexpr int kTexHeight = 256;
static constexpr int kRamSize = 0x20000; // 128KB WRAM

struct Vertex {
    float x, y, z, rhw;
    float u, v;
};

static constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_TEX1;

class VisualizationWindow {
public:
    void Toggle(HWND parent) {
        if (IsVisible()) {
            Hide();
        } else {
            Show(parent);
        }
    }

    void Shutdown() {
        Destroy();
    }

    bool IsVisible() const {
        return hwnd_ && IsWindowVisible(hwnd_) != FALSE;
    }

private:
    HWND hwnd_ = nullptr;
    IDirect3D9* d3d_ = nullptr;
    IDirect3DDevice9* device_ = nullptr;
    IDirect3DTexture9* texture_ = nullptr;
    D3DPRESENT_PARAMETERS pp_{};

    std::vector<uint8_t> prev_;
    std::vector<float> age_;
    std::vector<uint32_t> pixels_;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        VisualizationWindow* self = reinterpret_cast<VisualizationWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

        switch (msg) {
        case WM_NCCREATE: {
            CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        case WM_DESTROY:
            if (self) {
                self->DestroyDevice();
            }
            return 0;
        case WM_SIZE:
            if (self) {
                const UINT w = LOWORD(lParam);
                const UINT h = HIWORD(lParam);
                if (w > 0 && h > 0) {
                    self->ResetDevice(w, h);
                }
            }
            return 0;
        case WM_TIMER:
            if (self) {
                self->Render();
            }
            return 0;
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
        }
    }

    void Show(HWND parent) {
        if (!hwnd_) {
            RegisterClass();
            Create(parent);
        }

        if (hwnd_) {
            ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
            UpdateWindow(hwnd_);
        }
    }

    void Hide() {
        if (hwnd_) {
            ShowWindow(hwnd_, SW_HIDE);
        }
    }

    void RegisterClass() {
        static bool registered = false;
        if (registered) return;

        WNDCLASSEX wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = VisualizationWindow::WndProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.hIcon = LoadIcon(wc.hInstance, MAKEINTRESOURCE(IDI_ICON1));
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = TEXT("S9xVisualizationWindow");
        RegisterClassEx(&wc);
        registered = true;
    }

    void Create(HWND parent) {
        prev_.assign(kRamSize, 0);
        age_.assign(kRamSize, 0.0f);
        pixels_.assign(kTexWidth * kTexHeight, 0xFF0A0A0F);

        DWORD exStyle = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
        DWORD style = WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX;

        RECT r{ 0, 0, 1024, 768 };
        AdjustWindowRectEx(&r, style, FALSE, exStyle);

        hwnd_ = CreateWindowEx(
            exStyle,
            TEXT("S9xVisualizationWindow"),
            TEXT("SNES9X-FastLink Visualization"),
            style,
            CW_USEDEFAULT, CW_USEDEFAULT,
            r.right - r.left, r.bottom - r.top,
            parent,
            nullptr,
            GetModuleHandle(nullptr),
            this);

        if (hwnd_) {
            InitializeDevice();
            SetTimer(hwnd_, 1, 33, nullptr);
        }
    }

    void Destroy() {
        if (hwnd_) {
            KillTimer(hwnd_, 1);
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

    void InitializeDevice() {
        d3d_ = Direct3DCreate9(D3D_SDK_VERSION);
        if (!d3d_) return;

        ZeroMemory(&pp_, sizeof(pp_));
        pp_.Windowed = TRUE;
        pp_.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp_.hDeviceWindow = hwnd_;
        pp_.BackBufferFormat = D3DFMT_X8R8G8B8;
        pp_.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

        DWORD flags = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED;
        if (FAILED(d3d_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd_, flags, &pp_, &device_))) {
            flags = D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED;
            d3d_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd_, flags, &pp_, &device_);
        }

        CreateTexture();
    }

    void DestroyDevice() {
        if (texture_) {
            texture_->Release();
            texture_ = nullptr;
        }
        if (device_) {
            device_->Release();
            device_ = nullptr;
        }
        if (d3d_) {
            d3d_->Release();
            d3d_ = nullptr;
        }
    }

    void CreateTexture() {
        if (!device_) return;
        if (texture_) {
            texture_->Release();
            texture_ = nullptr;
        }
        device_->CreateTexture(
            kTexWidth,
            kTexHeight,
            1,
            D3DUSAGE_DYNAMIC,
            D3DFMT_A8R8G8B8,
            D3DPOOL_DEFAULT,
            &texture_,
            nullptr);
    }

    void ResetDevice(UINT w, UINT h) {
        if (!device_) return;

        pp_.BackBufferWidth = w;
        pp_.BackBufferHeight = h;

        if (texture_) {
            texture_->Release();
            texture_ = nullptr;
        }

        if (SUCCEEDED(device_->Reset(&pp_))) {
            CreateTexture();
        }
    }

    static float HueForAddress(int address) {
        if (address < 0x0100) return 230.0f; // zero page
        if (address < 0x0200) return 190.0f; // stack
        if (address < 0x2000) return 280.0f; // game state
        if (address < 0x4000) return 320.0f; // sprite/object tables
        if (address < 0x8000) return 45.0f;  // buffers
        return 160.0f;                       // extended WRAM
    }

    static uint32_t HslToArgb(float h, float s, float l) {
        h = std::fmod(h, 360.0f);
        if (h < 0.0f) h += 360.0f;

        float c = (1.0f - std::fabs(2.0f * l - 1.0f)) * s;
        float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
        float m = l - c / 2.0f;

        float r = 0, g = 0, b = 0;
        if (h < 60)      { r = c; g = x; b = 0; }
        else if (h < 120){ r = x; g = c; b = 0; }
        else if (h < 180){ r = 0; g = c; b = x; }
        else if (h < 240){ r = 0; g = x; b = c; }
        else if (h < 300){ r = x; g = 0; b = c; }
        else             { r = c; g = 0; b = x; }

        uint8_t R = static_cast<uint8_t>(std::clamp((r + m) * 255.0f, 0.0f, 255.0f));
        uint8_t G = static_cast<uint8_t>(std::clamp((g + m) * 255.0f, 0.0f, 255.0f));
        uint8_t B = static_cast<uint8_t>(std::clamp((b + m) * 255.0f, 0.0f, 255.0f));
        return 0xFF000000u | (R << 16) | (G << 8) | B;
    }

    uint32_t ColorForByte(uint8_t value, int address, float age) {
        if (value == 0 && age <= 0.0f) {
            return 0xFF0A0A0Fu;
        }

        float h = HueForAddress(address);
        float s = 0.8f;
        float l = 0.15f + (value / 255.0f) * 0.45f;

        if (age > 0.0f) {
            float flash = age * age;
            l = std::min(0.9f, l + flash * 0.5f);
            s = std::max(0.3f, s - flash * 0.3f);
            h = h + (60.0f - h) * flash * 0.5f;
        }

        return HslToArgb(h, s, l);
    }

    void UpdateTexture() {
        if (!texture_) return;

        for (int i = 0; i < kRamSize; i++) {
            uint8_t v = Memory.RAM[i];
            if (v != prev_[i]) {
                age_[i] = 1.0f;
                prev_[i] = v;
            } else if (age_[i] > 0.0f) {
                age_[i] = std::max(0.0f, age_[i] - 0.15f);
            }
            pixels_[i] = ColorForByte(v, i, age_[i]);
        }

        D3DLOCKED_RECT lr{};
        if (FAILED(texture_->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD))) {
            return;
        }

        const uint8_t* src = reinterpret_cast<const uint8_t*>(pixels_.data());
        uint8_t* dst = reinterpret_cast<uint8_t*>(lr.pBits);
        const int rowBytes = kTexWidth * sizeof(uint32_t);

        for (int y = 0; y < kTexHeight; y++) {
            memcpy(dst + y * lr.Pitch, src + y * rowBytes, rowBytes);
        }

        texture_->UnlockRect(0);
    }

    void Render() {
        if (!device_) return;

        const HRESULT coop = device_->TestCooperativeLevel();
        if (coop == D3DERR_DEVICELOST) {
            return;
        }
        if (coop == D3DERR_DEVICENOTRESET) {
            ResetDevice(pp_.BackBufferWidth, pp_.BackBufferHeight);
            return;
        }

        UpdateTexture();

        device_->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(10, 10, 15), 1.0f, 0);

        if (SUCCEEDED(device_->BeginScene())) {
            device_->SetFVF(kFvf);
            device_->SetTexture(0, texture_);
            device_->SetRenderState(D3DRS_LIGHTING, FALSE);
            device_->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            device_->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            device_->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);

            RECT rc{};
            GetClientRect(hwnd_, &rc);
            float w = static_cast<float>(rc.right - rc.left);
            float h = static_cast<float>(rc.bottom - rc.top);

            Vertex verts[4] = {
                { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f },
                { w,    0.0f, 0.0f, 1.0f, 1.0f, 0.0f },
                { 0.0f, h,    0.0f, 1.0f, 0.0f, 1.0f },
                { w,    h,    0.0f, 1.0f, 1.0f, 1.0f },
            };

            device_->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(Vertex));
            device_->EndScene();
        }

        device_->Present(nullptr, nullptr, nullptr, nullptr);
    }
};

VisualizationWindow g_visualization;

} // namespace

void VisualizationToggle(HWND parent) {
    g_visualization.Toggle(parent);
}

void VisualizationShutdown() {
    g_visualization.Shutdown();
}

bool VisualizationIsVisible() {
    return g_visualization.IsVisible();
}
