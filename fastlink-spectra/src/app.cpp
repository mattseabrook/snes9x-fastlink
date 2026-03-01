#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>

#include "app.h"

namespace {

static uint64_t NowMs()
{
    return static_cast<uint64_t>(GetTickCount64());
}

} // namespace

App::App()
    : sourceMode_(SourceMode::MemoryMapped), width_(1280), height_(720), frameIndex_(0), lastFetchTickMs_(0)
{
    rest_.SetEndpoint("127.0.0.1", 9000, L"/");
}

App::~App()
{
    Shutdown();
}

bool App::Initialize(void *hwnd, int width, int height)
{
    width_ = std::max(1, width);
    height_ = std::max(1, height);

    framePixels_.assign(static_cast<size_t>(width_) * static_cast<size_t>(height_), 0xFF0A0A0F);

    if (!renderer_.Initialize(hwnd, width_, height_))
        return false;

    mmf_.Open();
    lastFetchTickMs_ = NowMs();
    return true;
}

void App::Shutdown()
{
    mmf_.Close();
    renderer_.Shutdown();
}

void App::Resize(int width, int height)
{
    width_ = std::max(1, width);
    height_ = std::max(1, height);
    framePixels_.assign(static_cast<size_t>(width_) * static_cast<size_t>(height_), 0xFF0A0A0F);
    renderer_.Resize(width_, height_);
}

bool App::FetchSnapshot(std::vector<uint8_t> &out)
{
    if (sourceMode_ == SourceMode::MemoryMapped) {
        if (!mmf_.IsOpen()) {
            mmf_.Open();
        }
        MemorySnapshot snap;
        if (mmf_.ReadLatest(snap) && snap.valid) {
            out = std::move(snap.bytes);
            return true;
        }
        return false;
    }

    return rest_.Fetch(out);
}

void App::Tick()
{
    const uint64_t now = NowMs();
    if (now - lastFetchTickMs_ >= 100) {
        std::vector<uint8_t> bytes;
        if (FetchSnapshot(bytes)) {
            visualizer_.ProcessSnapshot(bytes);
        }
        lastFetchTickMs_ = now;
    }

    visualizer_.Draw(framePixels_.data(), width_, height_, frameIndex_++);
    renderer_.UploadFrame(framePixels_.data(), width_, height_);
    renderer_.Render();
}

void App::SetSourceMode(SourceMode mode)
{
    sourceMode_ = mode;
}

App::SourceMode App::GetSourceMode() const
{
    return sourceMode_;
}

Visualizer &App::GetVisualizer()
{
    return visualizer_;
}
