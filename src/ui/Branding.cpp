#include "ui/Branding.hpp"

#include <windows.h>
#include <gdiplus.h>
#include <GLFW/glfw3.h>

#include <array>
#include <cstddef>
#include <vector>

namespace vsm {

namespace {

class GdiPlusScope final {
public:
    GdiPlusScope() {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&token_, &input, nullptr);
    }

    ~GdiPlusScope() {
        if (token_ != 0) {
            Gdiplus::GdiplusShutdown(token_);
        }
    }

private:
    ULONG_PTR token_ = 0;
};

GdiPlusScope& gdiPlus() {
    static GdiPlusScope instance;
    return instance;
}

bool loadBitmap32(const std::filesystem::path& path, ImageRgba& outImage) {
    gdiPlus();

    const auto wide = path.wstring();
    Gdiplus::Bitmap bitmap(wide.c_str());
    if (bitmap.GetLastStatus() != Gdiplus::Ok) {
        return false;
    }

    const UINT width = bitmap.GetWidth();
    const UINT height = bitmap.GetHeight();
    if (width == 0 || height == 0) {
        return false;
    }

    Gdiplus::Rect rect(0, 0, static_cast<INT>(width), static_cast<INT>(height));
    Gdiplus::BitmapData data{};
    if (bitmap.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) != Gdiplus::Ok) {
        return false;
    }

    outImage.width = static_cast<int>(width);
    outImage.height = static_cast<int>(height);
    outImage.pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

    const auto srcStride = static_cast<int>(data.Stride);
    const auto* srcBase = static_cast<const std::uint8_t*>(data.Scan0);

    for (int y = 0; y < outImage.height; ++y) {
        const std::uint8_t* srcRow = srcBase + static_cast<ptrdiff_t>(y) * srcStride;
        std::uint8_t* dstRow = outImage.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(outImage.width) * 4;

        for (int x = 0; x < outImage.width; ++x) {
            const std::uint8_t b = srcRow[x * 4 + 0];
            const std::uint8_t g = srcRow[x * 4 + 1];
            const std::uint8_t r = srcRow[x * 4 + 2];
            const std::uint8_t a = srcRow[x * 4 + 3];

            dstRow[x * 4 + 0] = r;
            dstRow[x * 4 + 1] = g;
            dstRow[x * 4 + 2] = b;
            dstRow[x * 4 + 3] = a;
        }
    }

    bitmap.UnlockBits(&data);
    return true;
}

} // namespace

std::filesystem::path executableDirectory() {
    std::array<wchar_t, 32768> buffer{};
    const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (len == 0 || len >= buffer.size()) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(std::wstring(buffer.data(), len)).parent_path();
}

bool loadPngAsTexture(const std::filesystem::path& path, TextureHandle& outTexture) {
    ImageRgba image;
    if (!loadBitmap32(path, image)) {
        return false;
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (texture == 0) {
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    constexpr GLint clampToEdge = 0x812F;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, clampToEdge);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, clampToEdge);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width, image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image.pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    outTexture.id = texture;
    outTexture.width = image.width;
    outTexture.height = image.height;
    return true;
}

bool loadPngAsRgba(const std::filesystem::path& path, ImageRgba& outImage) {
    return loadBitmap32(path, outImage);
}

void releaseTexture(TextureHandle& texture) {
    if (texture.id != 0) {
        GLuint id = static_cast<GLuint>(texture.id);
        glDeleteTextures(1, &id);
        texture.id = 0;
        texture.width = 0;
        texture.height = 0;
    }
}

}