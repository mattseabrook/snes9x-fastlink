#pragma once

#include <cstdint>
#include <vector>

class Visualizer {
public:
    enum class View : int { WRAM = 0, VRAM = 1, SRAM = 2 };
    enum class Effect : int { None = 0, Glow = 1, Particles = 2, Wave = 3 };
    enum class ColorMode : int { Region = 0, Purple = 1, Heat = 2, Rainbow = 3 };

    struct Stats {
        uint32_t changeCount = 0;
        uint32_t hotspotCount = 0;
    };

    Visualizer();

    void SetView(View v);
    void SetEffect(Effect e);
    void SetColorMode(ColorMode c);

    View GetView() const;
    Effect GetEffect() const;
    ColorMode GetColorMode() const;

    void ProcessSnapshot(const std::vector<uint8_t> &bytes);
    void Draw(uint32_t *pixels, int width, int height, uint64_t frameIndex);
    const Stats &GetStats() const;

private:
    struct Particle {
        float x;
        float y;
        float vx;
        float vy;
        float life;
        float decay;
        float size;
        uint32_t color;
    };

    uint32_t HslToArgb(float h, float s, float l, float boost) const;
    uint32_t RegionHue(uint32_t address) const;
    uint32_t GetColor(uint8_t value, uint32_t address, float age, float heat) const;
    void SpawnParticles(float x, float y, uint32_t color, int count);
    void EnsureCapacity(size_t size);
    size_t ActiveSize() const;

    View view_;
    Effect effect_;
    ColorMode colorMode_;

    std::vector<uint8_t> ram_;
    std::vector<uint8_t> prev_;
    std::vector<float> age_;
    std::vector<float> heat_;
    std::vector<Particle> particles_;

    Stats stats_;
};
