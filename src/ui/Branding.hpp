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
[[nodiscard]] bool loadPngAsRgba(const std::filesystem::path& path, ImageRgba& outImage);
void releaseTexture(TextureHandle& texture);

}