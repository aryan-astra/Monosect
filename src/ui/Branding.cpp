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

/// Turns a decoded RGBA image into an OpenGL texture.
///
/// Shared by both loaders below so there is exactly one place that knows about
/// texture parameters and the pixel store state they need.
bool uploadRgba(const ImageRgba& image, TextureHandle& outTexture) {
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
    // The images here are not all four-byte aligned in width, and an alignment of 4
    // would make the driver read every row from the wrong offset.  Saved and restored:
    // this is global GL state, and a loader that leaves it changed silently reinterprets
    // the row alignment of every upload made after it - including by code that never
    // touched this file.
    GLint previousAlignment = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &previousAlignment);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width, image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 image.pixels.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, previousAlignment);
    glBindTexture(GL_TEXTURE_2D, 0);

    outTexture.id = texture;
    outTexture.width = image.width;
    outTexture.height = image.height;
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
    return uploadRgba(image, outTexture);
}

bool loadMarkAsTexture(const std::filesystem::path& path, TextureHandle& outTexture, std::uint8_t red,
                       std::uint8_t green, std::uint8_t blue) {
    ImageRgba image;
    if (!loadBitmap32(path, image)) {
        return false;
    }

    // The logo files are a single ink colour on an opaque white page: both report
    // 24bpp/32bpp with a corner alpha of 255, so they carry no transparency at all.
    // Drawn straight onto a dark window the result is a white rectangle with two
    // black bars on it, which reads as a rendering fault rather than as branding -
    // and the only ways out are to recolour the asset or to put a light panel behind
    // it, and both were visibly unwanted.
    //
    // So the mark is derived instead: how dark a pixel is becomes how opaque it is,
    // and every opaque pixel is the same light tint.  The shape is exactly the
    // authored shape - this is not a colour decision about the brand, it is the
    // usual way a one-ink logo is reversed for a dark surface, and the white page it
    // was drawn on simply becomes transparent.  Anti-aliased edges keep their partial
    // coverage, so the rounded corners of the mark stay smooth.
    //
    // Luminance uses the Rec. 601 weights because that is what the eye applies to a
    // greyscale judgement; a flat channel would misread the wordmark.
    const std::size_t pixelCount = static_cast<std::size_t>(image.width) *
                                   static_cast<std::size_t>(image.height);
    for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
        std::uint8_t* rgba = image.pixels.data() + pixel * 4;
        const int luminance = (static_cast<int>(rgba[0]) * 299 + static_cast<int>(rgba[1]) * 587 +
                               static_cast<int>(rgba[2]) * 114) /
                              1000;
        // 255 - luminance: white paper becomes 0 (invisible), full ink becomes 255.
        int ink = 255 - luminance;
        if (ink < 0) {
            ink = 0;
        } else if (ink > 255) {
            ink = 255;
        }
        // Respect any transparency the source did have, so an asset that is already
        // cut out still works.
        rgba[0] = red;
        rgba[1] = green;
        rgba[2] = blue;
        rgba[3] = static_cast<std::uint8_t>((ink * static_cast<int>(rgba[3])) / 255);
    }

    return uploadRgba(image, outTexture);
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