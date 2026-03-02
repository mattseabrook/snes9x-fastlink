#pragma once

#include <cstdint>
#include <vector>

#include "d3d11_renderer.h"
#include "ipc.h"
#include "rest_client.h"
#include "visualizer.h"

class App {
public:
    enum class SourceMode : int { MemoryMapped = 0, RestApi = 1 };

    App();
    ~App();

    bool Initialize(void *hwnd, int width, int height);
    void Shutdown();
    void Resize(int width, int height);
    void Tick();

    void SetSourceMode(SourceMode mode);
    SourceMode GetSourceMode() const;

    Visualizer &GetVisualizer();

private:
    bool FetchSnapshot(std::vector<uint8_t> &out);

    D3D11Renderer renderer_;
    SharedMemoryClient mmf_;
    RestClient rest_;
    Visualizer visualizer_;

    SourceMode sourceMode_;
    std::vector<uint32_t> framePixels_;
    int width_;
    int height_;
    uint64_t frameIndex_;
    uint64_t lastFetchTickMs_;
};
