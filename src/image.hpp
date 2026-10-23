// Pixels: screen grabs, window crops, resampling, blur, blending.
//
// X's ZPixmap at depth 24/32 is B,G,R,X per pixel, which is byte-identical to
// Cairo's CAIRO_FORMAT_RGB24 on little-endian -- so a grab becomes one memcpy
// and every later step reads the buffer directly. Nothing here touches GTK.
#pragma once

#include <cairo.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "x11.hpp"

namespace wintab {

struct ImageState;

//: A copyable handle on a Cairo image surface plus the buffer that owns its
//: pixels. Copies are refcount bumps, never memcpys.
class Image {
public:
    Image() = default;

    //: Decode raw ZPixmap bytes (depth 24 or 32, BGRX) into an RGB24 surface.
    static Image from_ximage(const void* data, std::size_t length, int width, int height,
                             int stride);
    //: An empty ARGB32 surface with its own storage.
    static Image create(int width, int height);
    //: An empty RGB24 surface with its own storage (intermediate steps).
    static Image create_rgb(int width, int height);
    static Image solid(int width, int height, std::uint8_t r, std::uint8_t g, std::uint8_t b);

    int width() const;
    int height() const;
    int stride() const;
    unsigned char* data();
    const unsigned char* data() const;
    cairo_surface_t* surface() const;
    explicit operator bool() const { return surface() != nullptr; }

    //: Zero-copy view of a rectangle; keeps the source's pixels alive.
    Image crop(const Rect& rect) const;
    //: A straight copy of the pixels (used where a stable, owned buffer matters).
    Image clone() const;

private:
    static Image from_state(std::shared_ptr<ImageState> state);
    std::shared_ptr<ImageState> state_;
};

//: RGB24 view of any image (ARGB32 inputs are converted by dropping alpha).
Image to_rgb(const Image& image);

Image box_halve(const Image& image);
Image resize_bilinear(const Image& image, int width, int height);
Image resize_bicubic(const Image& image, int width, int height);
Image resize_lanczos(const Image& image, int width, int height);

//: Pillow's behaviour: halve with a box filter first, then finish with Lanczos.
Image thumbnail_resize(const Image& image, int width, int height);

//: Three box passes approximating a Gaussian of `sigma` (Pillow's own method).
Image gaussian_blur(const Image& image, double sigma);

//: Blend toward black: out = in * (1 - amount).
Image blend_to_black(const Image& image, double amount);

//: Pillow's ImageEnhance.Color(factor): mix each channel toward luma.
Image desaturate(const Image& image, double factor);

//: Mean absolute difference per channel, 0 (identical) to 255.
double mean_difference(const Image& a, const Image& b);

//: The pixel at (x, y) as 0xRRGGBB (tests and probes).
std::uint32_t pixel_at(const Image& image, int x, int y);

}  // namespace wintab
