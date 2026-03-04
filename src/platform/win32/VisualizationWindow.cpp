#include "VisualizationWindow.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/util/snes9x.h"
#include "core/memory/memmap.h"
#include "resource.h"

namespace {

static constexpr int kTexWidth = 512;
static constexpr int kTexHeight = 256;
static constexpr int kRamSize = 0x20000;

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
    std::vector<uint8_t> prev_;
    std::vector<float> age_;
    std::vector<uint32_t> pixels_;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        VisualizationWindow *self = reinterpret_cast<VisualizationWindow *>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

        switch (msg) {
        case WM_NCCREATE: {
            CREATESTRUCT *cs = reinterpret_cast<CREATESTRUCT *>(lParam);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
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
        if (registered)
            return;

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

        RECT r{0, 0, 1024, 768};
        AdjustWindowRectEx(&r, style, FALSE, exStyle);

        hwnd_ = CreateWindowEx(
            exStyle,
            TEXT("S9xVisualizationWindow"),
            TEXT("SNES9X-FastLink Visualization"),
            style,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            r.right - r.left,
            r.bottom - r.top,
            parent,
            nullptr,
            GetModuleHandle(nullptr),
            this);

        if (hwnd_) {
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

    static float HueForAddress(int address) {
        if (address < 0x0100)
            return 230.0f;
        if (address < 0x0200)
            return 190.0f;
        if (address < 0x2000)
            return 280.0f;
        if (address < 0x4000)
            return 320.0f;
        if (address < 0x8000)
            return 45.0f;
        return 160.0f;
    }

    static uint32_t HslToArgb(float h, float s, float l) {
        h = std::fmod(h, 360.0f);
        if (h < 0.0f)
            h += 360.0f;

        float c = (1.0f - std::fabs(2.0f * l - 1.0f)) * s;
        float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
        float m = l - c / 2.0f;

        float r = 0, g = 0, b = 0;
        if (h < 60) {
            r = c;
            g = x;
            b = 0;
        } else if (h < 120) {
            r = x;
            g = c;
            b = 0;
        } else if (h < 180) {
            r = 0;
            g = c;
            b = x;
        } else if (h < 240) {
            r = 0;
            g = x;
            b = c;
        } else if (h < 300) {
            r = x;
            g = 0;
            b = c;
        } else {
            r = c;
            g = 0;
            b = x;
        }

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
    }

    void Render() {
        if (!hwnd_)
            return;

        UpdateTexture();

        RECT rc{};
        GetClientRect(hwnd_, &rc);
        const int width = (rc.right - rc.left) > 0 ? (rc.right - rc.left) : 1;
        const int height = (rc.bottom - rc.top) > 0 ? (rc.bottom - rc.top) : 1;

        HDC dc = GetDC(hwnd_);
        if (!dc)
            return;

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = kTexWidth;
        bmi.bmiHeader.biHeight = -kTexHeight;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        StretchDIBits(
            dc,
            0,
            0,
            width,
            height,
            0,
            0,
            kTexWidth,
            kTexHeight,
            pixels_.data(),
            &bmi,
            DIB_RGB_COLORS,
            SRCCOPY);

        ReleaseDC(hwnd_, dc);
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
