#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "app.h"

namespace {

static constexpr UINT ID_VIEW_WRAM = 1001;
static constexpr UINT ID_VIEW_VRAM = 1002;
static constexpr UINT ID_VIEW_SRAM = 1003;

static constexpr UINT ID_EFFECT_NONE = 1101;
static constexpr UINT ID_EFFECT_GLOW = 1102;
static constexpr UINT ID_EFFECT_PARTICLES = 1103;
static constexpr UINT ID_EFFECT_WAVE = 1104;

static constexpr UINT ID_COLOR_REGION = 1201;
static constexpr UINT ID_COLOR_PURPLE = 1202;
static constexpr UINT ID_COLOR_HEAT = 1203;
static constexpr UINT ID_COLOR_RAINBOW = 1204;

static constexpr UINT ID_SOURCE_MMF = 1301;
static constexpr UINT ID_SOURCE_REST = 1302;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    App *app = reinterpret_cast<App *>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCT *cs = reinterpret_cast<CREATESTRUCT *>(lParam);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
    }
    case WM_SIZE:
        if (app) {
            int w = LOWORD(lParam);
            int h = HIWORD(lParam);
            if (w > 0 && h > 0)
                app->Resize(w, h);
        }
        return 0;
    case WM_RBUTTONUP:
        if (app) {
            HMENU root = CreatePopupMenu();
            HMENU viewMenu = CreatePopupMenu();
            HMENU effectMenu = CreatePopupMenu();
            HMENU colorMenu = CreatePopupMenu();
            HMENU sourceMenu = CreatePopupMenu();

            const auto view = app->GetVisualizer().GetView();
            const auto eff = app->GetVisualizer().GetEffect();
            const auto color = app->GetVisualizer().GetColorMode();
            const auto src = app->GetSourceMode();

            AppendMenu(viewMenu, MF_STRING | (view == Visualizer::View::WRAM ? MF_CHECKED : 0), ID_VIEW_WRAM, TEXT("WRAM (128KB)"));
            AppendMenu(viewMenu, MF_STRING | (view == Visualizer::View::VRAM ? MF_CHECKED : 0), ID_VIEW_VRAM, TEXT("VRAM (64KB)"));
            AppendMenu(viewMenu, MF_STRING | (view == Visualizer::View::SRAM ? MF_CHECKED : 0), ID_VIEW_SRAM, TEXT("SRAM (32KB)"));

            AppendMenu(effectMenu, MF_STRING | (eff == Visualizer::Effect::None ? MF_CHECKED : 0), ID_EFFECT_NONE, TEXT("None"));
            AppendMenu(effectMenu, MF_STRING | (eff == Visualizer::Effect::Glow ? MF_CHECKED : 0), ID_EFFECT_GLOW, TEXT("Glow"));
            AppendMenu(effectMenu, MF_STRING | (eff == Visualizer::Effect::Particles ? MF_CHECKED : 0), ID_EFFECT_PARTICLES, TEXT("Particles"));
            AppendMenu(effectMenu, MF_STRING | (eff == Visualizer::Effect::Wave ? MF_CHECKED : 0), ID_EFFECT_WAVE, TEXT("Wave"));

            AppendMenu(colorMenu, MF_STRING | (color == Visualizer::ColorMode::Region ? MF_CHECKED : 0), ID_COLOR_REGION, TEXT("Region-Based"));
            AppendMenu(colorMenu, MF_STRING | (color == Visualizer::ColorMode::Purple ? MF_CHECKED : 0), ID_COLOR_PURPLE, TEXT("Classic Purple"));
            AppendMenu(colorMenu, MF_STRING | (color == Visualizer::ColorMode::Heat ? MF_CHECKED : 0), ID_COLOR_HEAT, TEXT("Activity Heatmap"));
            AppendMenu(colorMenu, MF_STRING | (color == Visualizer::ColorMode::Rainbow ? MF_CHECKED : 0), ID_COLOR_RAINBOW, TEXT("Rainbow"));

            AppendMenu(sourceMenu, MF_STRING | (src == App::SourceMode::MemoryMapped ? MF_CHECKED : 0), ID_SOURCE_MMF, TEXT("Memory-Mapped"));
            AppendMenu(sourceMenu, MF_STRING | (src == App::SourceMode::RestApi ? MF_CHECKED : 0), ID_SOURCE_REST, TEXT("REST API"));

            AppendMenu(root, MF_POPUP, reinterpret_cast<UINT_PTR>(viewMenu), TEXT("View"));
            AppendMenu(root, MF_POPUP, reinterpret_cast<UINT_PTR>(effectMenu), TEXT("Effects"));
            AppendMenu(root, MF_POPUP, reinterpret_cast<UINT_PTR>(colorMenu), TEXT("Color Mode"));
            AppendMenu(root, MF_POPUP, reinterpret_cast<UINT_PTR>(sourceMenu), TEXT("Source"));

            POINT p{};
            GetCursorPos(&p);
            TrackPopupMenu(root, TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, nullptr);
            DestroyMenu(root);
        }
        return 0;
    case WM_COMMAND:
        if (!app) return 0;
        switch (LOWORD(wParam)) {
        case ID_VIEW_WRAM: app->GetVisualizer().SetView(Visualizer::View::WRAM); break;
        case ID_VIEW_VRAM: app->GetVisualizer().SetView(Visualizer::View::VRAM); break;
        case ID_VIEW_SRAM: app->GetVisualizer().SetView(Visualizer::View::SRAM); break;

        case ID_EFFECT_NONE: app->GetVisualizer().SetEffect(Visualizer::Effect::None); break;
        case ID_EFFECT_GLOW: app->GetVisualizer().SetEffect(Visualizer::Effect::Glow); break;
        case ID_EFFECT_PARTICLES: app->GetVisualizer().SetEffect(Visualizer::Effect::Particles); break;
        case ID_EFFECT_WAVE: app->GetVisualizer().SetEffect(Visualizer::Effect::Wave); break;

        case ID_COLOR_REGION: app->GetVisualizer().SetColorMode(Visualizer::ColorMode::Region); break;
        case ID_COLOR_PURPLE: app->GetVisualizer().SetColorMode(Visualizer::ColorMode::Purple); break;
        case ID_COLOR_HEAT: app->GetVisualizer().SetColorMode(Visualizer::ColorMode::Heat); break;
        case ID_COLOR_RAINBOW: app->GetVisualizer().SetColorMode(Visualizer::ColorMode::Rainbow); break;

        case ID_SOURCE_MMF: app->SetSourceMode(App::SourceMode::MemoryMapped); break;
        case ID_SOURCE_REST: app->SetSourceMode(App::SourceMode::RestApi); break;
        default: break;
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

} // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    WNDCLASSEX wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = TEXT("FastLinkSpectraWindow");
    RegisterClassEx(&wc);

    App app;

    RECT r{0, 0, 1280, 720};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowEx(0,
                               wc.lpszClassName,
                               TEXT("SNES9X FastLink Spectra"),
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               CW_USEDEFAULT,
                               CW_USEDEFAULT,
                               r.right - r.left,
                               r.bottom - r.top,
                               nullptr,
                               nullptr,
                               hInstance,
                               &app);
    if (!hwnd)
        return 1;

    if (!app.Initialize(hwnd, 1280, 720)) {
        MessageBox(hwnd, TEXT("Failed to initialize D3D11 renderer."), TEXT("FastLink Spectra"), MB_ICONERROR | MB_OK);
        return 1;
    }

    MSG msg{};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            app.Tick();
            Sleep(1);
        }
    }

    app.Shutdown();
    return static_cast<int>(msg.wParam);
}
