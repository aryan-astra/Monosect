#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace vsm {

struct TextureHandle {
    unsigned int id = 0;
    int width = 0;
    int height = 0;

    [[nodiscard]] bool valid() const { return id != 0; }
};

struct ImageRgba {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;

    [[nodiscard]] bool valid() const { return width > 0 && height > 0 && !pixels.empty(); }
};

[[nodiscard]] std::filesystem::path executableDirectory();
[[nodiscard]] bool loadPngAsTexture(const std::filesystem::path& path, TextureHandle& outTexture);

/// Loads a dark-ink-on-white logo as a single-colour mark on a transparent
/// background, ready to sit directly on a dark surface.
///
/// The mark keeps its authored shape and coverage; only the page behind it is
/// removed and the ink is flattened to the given colour.  Use this for branding
/// artwork, not for pictures - a photographic image would lose all its shading.
[[nodiscard]] bool loadMarkAsTexture(const std::filesystem::path& path, TextureHandle& outTexture,
                                    std::uint8_t red, std::uint8_t green, std::uint8_t blue);

[[nodiscard]] bool loadPngAsRgba(const std::filesystem::path& path, ImageRgba& outImage);
void releaseTexture(TextureHandle& texture);

}