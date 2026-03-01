#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "visualizer.h"

namespace {

static constexpr size_t kWramSize = 131072;
static constexpr size_t kVramSize = 65536;
static constexpr size_t kSramSize = 32768;
static constexpr uint32_t kBackground = 0xFF0A0A0F;
static constexpr size_t kMaxParticles = 500;

inline float Frand()
{
    return static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
}

} // namespace

Visualizer::Visualizer()
    : view_(View::WRAM), effect_(Effect::Glow), colorMode_(ColorMode::Region)
{
    EnsureCapacity(kWramSize);
}

void Visualizer::SetView(View v)
{
    view_ = v;
    EnsureCapacity(ActiveSize());
}

void Visualizer::SetEffect(Effect e) { effect_ = e; }
void Visualizer::SetColorMode(ColorMode c) { colorMode_ = c; }
Visualizer::View Visualizer::GetView() const { return view_; }
Visualizer::Effect Visualizer::GetEffect() const { return effect_; }
Visualizer::ColorMode Visualizer::GetColorMode() const { return colorMode_; }

size_t Visualizer::ActiveSize() const
{
    switch (view_) {
    case View::WRAM: return kWramSize;
    case View::VRAM: return kVramSize;
    case View::SRAM: return kSramSize;
    }
    return kWramSize;
}

void Visualizer::EnsureCapacity(size_t size)
{
    ram_.assign(size, 0);
    prev_.assign(size, 0);
    age_.assign(size, 0.0f);
    heat_.assign(size, 0.0f);
}

uint32_t Visualizer::RegionHue(uint32_t address) const
{
    if (address < 0x100) return 230;
    if (address < 0x200) return 190;
    if (address < 0x2000) return 280;
    if (address < 0x4000) return 320;
    if (address < 0x8000) return 45;
    return 160;
}

uint32_t Visualizer::HslToArgb(float h, float s, float l, float boost) const
{
    while (h < 0.0f) h += 360.0f;
    while (h >= 360.0f) h -= 360.0f;

    const float c = (1.0f - std::fabs(2.0f * l - 1.0f)) * s;
    const float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    const float m = l - c / 2.0f;

    float r = 0, g = 0, b = 0;
    if (h < 60)      { r = c; g = x; b = 0; }
    else if (h < 120){ r = x; g = c; b = 0; }
    else if (h < 180){ r = 0; g = c; b = x; }
    else if (h < 240){ r = 0; g = x; b = c; }
    else if (h < 300){ r = x; g = 0; b = c; }
    else             { r = c; g = 0; b = x; }

    const uint8_t R = static_cast<uint8_t>(std::min(255.0f, (r + m) * 255.0f + boost));
    const uint8_t G = static_cast<uint8_t>(std::min(255.0f, (g + m) * 255.0f + boost));
    const uint8_t B = static_cast<uint8_t>(std::min(255.0f, (b + m) * 255.0f + boost));
    return 0xFF000000u | (R << 16) | (G << 8) | B;
}

uint32_t Visualizer::GetColor(uint8_t value, uint32_t address, float age, float heat) const
{
    if (value == 0 && age <= 0.0f)
        return kBackground;

    float h = static_cast<float>(RegionHue(address));
    float s = 0.8f;
    float l = 0.15f + (static_cast<float>(value) / 255.0f) * 0.45f;

    switch (colorMode_) {
    case ColorMode::Region:
        h = static_cast<float>(RegionHue(address));
        break;
    case ColorMode::Purple:
        h = 280.0f;
        break;
    case ColorMode::Heat:
        h = 240.0f - heat * 180.0f;
        s = 0.9f;
        break;
    case ColorMode::Rainbow:
        h = (static_cast<float>(value) / 255.0f) * 300.0f;
        break;
    }

    float boost = 0.0f;
    if (age > 0.0f) {
        const float flash = age * age;
        l = std::min(0.9f, l + flash * 0.5f);
        s = std::max(0.3f, s - flash * 0.3f);
        boost = flash * 150.0f;
        h = h + (60.0f - h) * flash * 0.5f;
    }

    return HslToArgb(h, s, l, boost);
}

void Visualizer::SpawnParticles(float x, float y, uint32_t color, int count)
{
    for (int i = 0; i < count && particles_.size() < kMaxParticles; ++i) {
        Particle p{};
        p.x = x;
        p.y = y;
        p.vx = (Frand() - 0.5f) * 3.0f;
        p.vy = -Frand() * 4.0f - 2.0f;
        p.life = 1.0f;
        p.decay = 0.02f + Frand() * 0.03f;
        p.size = 2.0f + Frand() * 3.0f;
        p.color = color;
        particles_.push_back(p);
    }
}

void Visualizer::ProcessSnapshot(const std::vector<uint8_t> &bytes)
{
    const size_t size = ActiveSize();
    if (ram_.size() != size)
        EnsureCapacity(size);

    stats_.changeCount = 0;
    stats_.hotspotCount = 0;

    for (size_t i = 0; i < size; ++i) {
        const uint8_t newVal = i < bytes.size() ? bytes[i] : 0;
        const uint8_t oldVal = prev_[i];

        if (newVal != oldVal) {
            stats_.changeCount++;
            age_[i] = 1.0f;
            heat_[i] = std::min(1.0f, heat_[i] + 0.1f);

            if (effect_ == Effect::Particles && std::abs(static_cast<int>(newVal) - static_cast<int>(oldVal)) > 20) {
                const float hue = static_cast<float>(RegionHue(static_cast<uint32_t>(i)));
                const uint32_t color = HslToArgb(hue, 0.8f, 0.7f, 0.0f);
                SpawnParticles(0.0f, 0.0f, color, std::min(5, std::abs(static_cast<int>(newVal) - static_cast<int>(oldVal)) / 30));
            }
        } else {
            if (age_[i] > 0.0f)
                age_[i] = std::max(0.0f, age_[i] - 0.15f);
            if (heat_[i] > 0.0f)
                heat_[i] *= 0.995f;
        }

        if (heat_[i] > 0.3f)
            stats_.hotspotCount++;

        prev_[i] = newVal;
        ram_[i] = newVal;
    }
}

void Visualizer::Draw(uint32_t *pixels, int width, int height, uint64_t frameIndex)
{
    if (!pixels || width <= 0 || height <= 0)
        return;

    const size_t size = ActiveSize();
    std::fill(pixels, pixels + static_cast<size_t>(width) * static_cast<size_t>(height), kBackground);

    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const int tilesX = std::max(1, static_cast<int>(std::round(std::sqrt(static_cast<double>(size) * aspect))));
    const int tilesY = std::max(1, static_cast<int>((size + static_cast<size_t>(tilesX) - 1) / static_cast<size_t>(tilesX)));
    const float tileW = static_cast<float>(width) / static_cast<float>(tilesX);
    const float tileH = static_cast<float>(height) / static_cast<float>(tilesY);

    const float waveTime = static_cast<float>(frameIndex) * 0.05f;

    for (size_t i = 0; i < size; ++i) {
        const float age = age_[i];
        const float heat = std::min(1.0f, heat_[i]);
        const uint32_t color = GetColor(ram_[i], static_cast<uint32_t>(i), age, heat);

        int tx = static_cast<int>((i % static_cast<size_t>(tilesX)) * tileW);
        int ty = static_cast<int>((i / static_cast<size_t>(tilesX)) * tileH);

        if (effect_ == Effect::Wave && age > 0.0f) {
            tx += static_cast<int>(std::sin(waveTime + static_cast<float>(i) * 0.01f) * age * 3.0f);
            ty += static_cast<int>(std::cos(waveTime + static_cast<float>(i) * 0.01f) * age * 2.0f);
        }

        const int col = static_cast<int>(i % static_cast<size_t>(tilesX));
        const int row = static_cast<int>(i / static_cast<size_t>(tilesX));
        const int x0 = static_cast<int>(col * tileW);
        const int x1 = static_cast<int>((col + 1) * tileW);
        const int y0 = static_cast<int>(row * tileH);
        const int y1 = static_cast<int>((row + 1) * tileH);

        const int glowExpand = (effect_ == Effect::Glow && age > 0.5f) ? 1 : 0;
        const int drawX0 = std::max(0, std::min(width, tx - glowExpand));
        const int drawY0 = std::max(0, std::min(height, ty - glowExpand));
        const int drawX1 = std::max(0, std::min(width, tx + (x1 - x0) + glowExpand));
        const int drawY1 = std::max(0, std::min(height, ty + (y1 - y0) + glowExpand));

        for (int y = drawY0; y < drawY1; ++y) {
            uint32_t *rowPtr = pixels + static_cast<size_t>(y) * static_cast<size_t>(width);
            for (int x = drawX0; x < drawX1; ++x) {
                rowPtr[x] = color;
            }
        }
    }

    if (effect_ == Effect::Particles && !particles_.empty()) {
        for (size_t i = particles_.size(); i-- > 0;) {
            Particle &p = particles_[i];
            p.vy += 0.15f;
            p.x += p.vx;
            p.y += p.vy;
            p.life -= p.decay;

            if (p.life <= 0.0f) {
                particles_.erase(particles_.begin() + static_cast<long>(i));
                continue;
            }

            const int px = static_cast<int>(p.x);
            const int py = static_cast<int>(p.y);
            const int radius = std::max(1, static_cast<int>(p.size));

            for (int y = py - radius; y <= py + radius; ++y) {
                if (y < 0 || y >= height) continue;
                for (int x = px - radius; x <= px + radius; ++x) {
                    if (x < 0 || x >= width) continue;
                    pixels[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] = p.color;
                }
            }
        }
    }
}

const Visualizer::Stats &Visualizer::GetStats() const
{
    return stats_;
}
