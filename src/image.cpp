#include "image.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wintab {

struct ImageState {
    cairo_surface_t* surface = nullptr;
    //: Owns the pixels when this image was decoded or created here. A view
    //: (crop) leaves this empty and holds its source in `parent` instead.
    std::shared_ptr<std::vector<unsigned char>> buffer;
    std::shared_ptr<ImageState> parent;

    ~ImageState() {
        if (surface) cairo_surface_destroy(surface);
    }
};

namespace {

//: A zeroed pixel buffer. ``vector(bytes)`` already zero-initialises, and an
//: extra std::fill would touch every byte a second time.
std::shared_ptr<std::vector<unsigned char>> make_buffer(std::size_t bytes) {
    return std::make_shared<std::vector<unsigned char>>(bytes);
}

}  // namespace

Image Image::from_state(std::shared_ptr<ImageState> state) {
    Image image;
    if (state) image.state_ = std::move(state);
    return image;
}

Image Image::from_ximage(const void* data, std::size_t length, int width, int height, int stride) {
    if (width <= 0 || height <= 0 || stride < width * 4) return Image();
    if (length < static_cast<std::size_t>(stride) * static_cast<std::size_t>(height)) return Image();

    // Copying through the range constructor allocates and fills in one pass;
    // make_buffer would zero 8MB only to overwrite it with the grab.
    const std::size_t needed = static_cast<std::size_t>(stride) * static_cast<std::size_t>(height);
    const auto* bytes = static_cast<const unsigned char*>(data);
    auto buffer = std::make_shared<std::vector<unsigned char>>(bytes, bytes + needed);
    cairo_surface_t* surface = cairo_image_surface_create_for_data(
        buffer->data(), CAIRO_FORMAT_RGB24, width, height, stride);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return Image();
    }
    auto state = std::make_shared<ImageState>();
    state->surface = surface;
    state->buffer = std::move(buffer);
    return from_state(state);
}

Image Image::create(int width, int height) {
    if (width <= 0 || height <= 0) return Image();
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return Image();
    }
    auto state = std::make_shared<ImageState>();
    state->surface = surface;
    return from_state(state);
}

Image Image::create_rgb(int width, int height) {
    if (width <= 0 || height <= 0) return Image();
    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, width);
    auto buffer = make_buffer(static_cast<std::size_t>(stride) * static_cast<std::size_t>(height));
    cairo_surface_t* surface = cairo_image_surface_create_for_data(
        buffer->data(), CAIRO_FORMAT_RGB24, width, height, stride);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return Image();
    }
    auto state = std::make_shared<ImageState>();
    state->surface = surface;
    state->buffer = std::move(buffer);
    return from_state(state);
}

Image Image::solid(int width, int height, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    Image image = create_rgb(width, height);
    if (!image) return image;
    unsigned char* base = image.data();
    const int stride = image.stride();
    for (int y = 0; y < height; ++y) {
        unsigned char* row = base + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
            row[x * 4 + 0] = b;
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = r;
            row[x * 4 + 3] = 0xFF;
        }
    }
    cairo_surface_mark_dirty(image.surface());
    return image;
}

int Image::width() const {
    if (!state_ || !state_->surface) return 0;
    return cairo_image_surface_get_width(state_->surface);
}

int Image::height() const {
    if (!state_ || !state_->surface) return 0;
    return cairo_image_surface_get_height(state_->surface);
}

int Image::stride() const {
    if (!state_ || !state_->surface) return 0;
    return cairo_image_surface_get_stride(state_->surface);
}

unsigned char* Image::data() {
    if (!state_ || !state_->surface) return nullptr;
    return cairo_image_surface_get_data(state_->surface);
}

const unsigned char* Image::data() const {
    if (!state_ || !state_->surface) return nullptr;
    return cairo_image_surface_get_data(state_->surface);
}

cairo_surface_t* Image::surface() const { return state_ ? state_->surface : nullptr; }

Image Image::crop(const Rect& rect) const {
    if (!state_ || !state_->surface) return Image();
    const int left = std::max(0, rect.x);
    const int top = std::max(0, rect.y);
    const int right = std::min(width(), rect.x + rect.w);
    const int bottom = std::min(height(), rect.y + rect.h);
    const int w = right - left;
    const int h = bottom - top;
    if (w <= 8 || h <= 8) return Image();
    cairo_surface_t* view =
        cairo_surface_create_for_rectangle(state_->surface, left, top, w, h);
    auto state = std::make_shared<ImageState>();
    state->surface = view;
    state->parent = state_;
    return from_state(state);
}

Image Image::clone() const {
    if (!state_ || !state_->surface) return Image();
    Image copy = create_rgb(width(), height());
    if (!copy) return copy;
    const int bytes = std::min(stride(), copy.stride());
    for (int y = 0; y < height(); ++y) {
        std::memcpy(copy.data() + static_cast<std::size_t>(y) * copy.stride(),
                    data() + static_cast<std::size_t>(y) * stride(), static_cast<std::size_t>(bytes));
    }
    cairo_surface_mark_dirty(copy.surface());
    return copy;
}

Image to_rgb(const Image& image) {
    if (!image) return Image();
    if (cairo_image_surface_get_format(image.surface()) == CAIRO_FORMAT_RGB24) return image;
    return image.clone();
}

// ---------------------------------------------------------------------------
// Resampling
// ---------------------------------------------------------------------------

namespace {

using Kernel = double (*)(double, double);

double kernel_bilinear(double x, double) {
    x = std::fabs(x);
    return x < 1.0 ? 1.0 - x : 0.0;
}

double kernel_bicubic(double x, double a) {
    const double ax = std::fabs(x);
    if (ax <= 1.0) return (a + 2.0) * ax * ax * ax - (a + 3.0) * ax * ax + 1.0;
    if (ax < 2.0) return a * ax * ax * ax - 5.0 * a * ax * ax + 8.0 * a * ax - 4.0 * a;
    return 0.0;
}

double kernel_lanczos3(double x, double) {
    // Pillow's LANCZOS uses a 3-lobe kernel.
    const double ax = std::fabs(x);
    if (ax < 1e-9) return 1.0;
    if (ax >= 3.0) return 0.0;
    const double pix = 3.14159265358979323846;
    return 3.0 * std::sin(pix * ax) * std::sin(pix * ax / 3.0) / (pix * pix * ax * ax);
}

struct Weight {
    int index = 0;
    double weight = 0.0;
};

std::vector<std::vector<Weight>> build_weights(int source, int target, Kernel kernel, double a,
                                               double support) {
    std::vector<std::vector<Weight>> table(static_cast<std::size_t>(target));
    const double scale = static_cast<double>(target) / static_cast<double>(source);
    const double filter_scale = std::max(1.0, 1.0 / scale);
    const double radius = support * filter_scale;
    for (int out = 0; out < target; ++out) {
        const double centre = (out + 0.5) / scale - 0.5;
        const int begin = static_cast<int>(std::floor(centre - radius));
        const int end = static_cast<int>(std::ceil(centre + radius));
        auto& weights = table[static_cast<std::size_t>(out)];
        double total = 0.0;
        for (int in = begin; in <= end; ++in) {
            const double weight = kernel((in - centre) / filter_scale, a);
            if (weight == 0.0) continue;
            const int clamped = std::min(source - 1, std::max(0, in));
            weights.push_back(Weight{clamped, weight});
            total += weight;
        }
        if (total == 0.0) {
            const int clamped = std::min(source - 1, std::max(0, static_cast<int>(std::round(centre))));
            weights = {Weight{clamped, 1.0}};
        } else {
            for (auto& weight : weights) weight.weight /= total;
        }
    }
    return table;
}

Image resize_kernel(const Image& source_in, int dst_width, int dst_height, Kernel kernel, double a,
                    double support) {
    Image source = to_rgb(source_in);
    if (!source || dst_width <= 0 || dst_height <= 0) return Image();
    const int src_width = source.width();
    const int src_height = source.height();
    if (src_width == dst_width && src_height == dst_height) return source.clone();

    // Horizontal pass: source -> (dst_width x src_height).
    Image middle = Image::create_rgb(dst_width, src_height);
    if (!middle) return Image();
    {
        const auto weights = build_weights(src_width, dst_width, kernel, a, support);
        const unsigned char* source_data = source.data();
        const int source_stride = source.stride();
        unsigned char* middle_data = middle.data();
        const int middle_stride = middle.stride();
        for (int y = 0; y < src_height; ++y) {
            const unsigned char* row = source_data + static_cast<std::size_t>(y) * source_stride;
            unsigned char* out = middle_data + static_cast<std::size_t>(y) * middle_stride;
            for (int x = 0; x < dst_width; ++x) {
                double b = 0.0;
                double g = 0.0;
                double r = 0.0;
                for (const Weight& weight : weights[static_cast<std::size_t>(x)]) {
                    const unsigned char* px = row + static_cast<std::size_t>(weight.index) * 4;
                    b += px[0] * weight.weight;
                    g += px[1] * weight.weight;
                    r += px[2] * weight.weight;
                }
                unsigned char* px = out + static_cast<std::size_t>(x) * 4;
                px[0] = static_cast<unsigned char>(std::min(255.0, std::max(0.0, b + 0.5)));
                px[1] = static_cast<unsigned char>(std::min(255.0, std::max(0.0, g + 0.5)));
                px[2] = static_cast<unsigned char>(std::min(255.0, std::max(0.0, r + 0.5)));
                px[3] = 0xFF;
            }
        }
        cairo_surface_mark_dirty(middle.surface());
    }

    // Vertical pass: (dst_width x src_height) -> dst.
    Image result = Image::create_rgb(dst_width, dst_height);
    if (!result) return Image();
    {
        const auto weights = build_weights(src_height, dst_height, kernel, a, support);
        const unsigned char* middle_data = middle.data();
        const int middle_stride = middle.stride();
        unsigned char* result_data = result.data();
        const int result_stride = result.stride();
        for (int y = 0; y < dst_height; ++y) {
            unsigned char* out = result_data + static_cast<std::size_t>(y) * result_stride;
            for (int x = 0; x < dst_width; ++x) {
                double b = 0.0;
                double g = 0.0;
                double r = 0.0;
                for (const Weight& weight : weights[static_cast<std::size_t>(y)]) {
                    const unsigned char* px =
                        middle_data + static_cast<std::size_t>(weight.index) * middle_stride +
                        static_cast<std::size_t>(x) * 4;
                    b += px[0] * weight.weight;
                    g += px[1] * weight.weight;
                    r += px[2] * weight.weight;
                }
                unsigned char* px = out + static_cast<std::size_t>(x) * 4;
                px[0] = static_cast<unsigned char>(std::min(255.0, std::max(0.0, b + 0.5)));
                px[1] = static_cast<unsigned char>(std::min(255.0, std::max(0.0, g + 0.5)));
                px[2] = static_cast<unsigned char>(std::min(255.0, std::max(0.0, r + 0.5)));
                px[3] = 0xFF;
            }
        }
        cairo_surface_mark_dirty(result.surface());
    }
    return result;
}

}  // namespace

Image resize_bilinear(const Image& image, int width, int height) {
    return resize_kernel(image, width, height, kernel_bilinear, 0.0, 1.0);
}

Image resize_bicubic(const Image& image, int width, int height) {
    return resize_kernel(image, width, height, kernel_bicubic, -0.5, 2.0);
}

Image resize_lanczos(const Image& image, int width, int height) {
    return resize_kernel(image, width, height, kernel_lanczos3, 0.0, 3.0);
}

Image box_halve(const Image& source_in) {
    Image source = to_rgb(source_in);
    if (!source) return Image();
    const int width = source.width() / 2;
    const int height = source.height() / 2;
    if (width < 1 || height < 1) return source.clone();
    Image result = Image::create_rgb(width, height);
    if (!result) return Image();
    const unsigned char* source_data = source.data();
    const int source_stride = source.stride();
    unsigned char* result_data = result.data();
    const int result_stride = result.stride();
    for (int y = 0; y < height; ++y) {
        const unsigned char* row = source_data + static_cast<std::size_t>(y * 2) * source_stride;
        const unsigned char* next = row + source_stride;
        unsigned char* out = result_data + static_cast<std::size_t>(y) * result_stride;
        for (int x = 0; x < width; ++x) {
            for (int channel = 0; channel < 3; ++channel) {
                const int sum = row[x * 8 + channel] + row[x * 8 + 4 + channel] +
                                next[x * 8 + channel] + next[x * 8 + 4 + channel];
                out[x * 4 + channel] = static_cast<unsigned char>((sum + 2) / 4);
            }
            out[x * 4 + 3] = 0xFF;
        }
    }
    cairo_surface_mark_dirty(result.surface());
    return result;
}

Image thumbnail_resize(const Image& image, int width, int height) {
    if (width <= 0 || height <= 0) return image;
    Image current = to_rgb(image);
    if (!current) return Image();
    while (current.width() >= 2 * width && current.height() >= 2 * height && current.width() > 16 &&
           current.height() > 16) {
        Image halved = box_halve(current);
        if (!halved) break;
        current = halved;
    }
    if (current.width() == width && current.height() == height) return current;
    return resize_lanczos(current, width, height);
}

Image gaussian_blur(const Image& source_in, double sigma) {
    Image source = to_rgb(source_in);
    if (!source || sigma <= 0.0) return source;

    // Box radii whose three passes approximate a Gaussian of this sigma.
    const double n = 3.0;
    const double ideal = std::sqrt(12.0 * sigma * sigma / n + 1.0);
    int low = static_cast<int>(std::floor(ideal));
    if (low % 2 == 0) --low;
    if (low < 1) low = 1;
    const int high = low + 2;
    const double m = std::round((12.0 * sigma * sigma - n * low * low - 4.0 * n * low - 3.0 * n) /
                                (-4.0 * low - 4.0));
    int radii[3] = {low, low, low};
    for (int i = 0; i < 3; ++i) {
        radii[i] = (i < static_cast<int>(m)) ? low : high;
    }

    Image current = source.clone();
    if (!current) return Image();
    const int width = current.width();
    const int height = current.height();

    auto blur_pass = [&](int radius, bool horizontal) {
        if (radius < 1) return;
        Image target = Image::create_rgb(width, height);
        if (!target) return;
        const unsigned char* input = current.data();
        const int input_stride = current.stride();
        unsigned char* output = target.data();
        const int output_stride = target.stride();
        const int lines = horizontal ? height : width;
        const int span = horizontal ? width : height;

        std::vector<double> sums(3, 0.0);
        for (int line = 0; line < lines; ++line) {
            const auto at = [&](int position) {
                const int clamped = std::min(span - 1, std::max(0, position));
                if (horizontal) {
                    return input + static_cast<std::size_t>(line) * input_stride +
                           static_cast<std::size_t>(clamped) * 4;
                }
                return input + static_cast<std::size_t>(clamped) * input_stride +
                       static_cast<std::size_t>(line) * 4;
            };
            sums = {0.0, 0.0, 0.0};
            for (int offset = -radius; offset <= radius; ++offset) {
                const unsigned char* px = at(offset);
                for (int channel = 0; channel < 3; ++channel) sums[channel] += px[channel];
            }
            const double divisor = 2.0 * radius + 1.0;
            for (int position = 0; position < span; ++position) {
                unsigned char* out;
                if (horizontal) {
                    out = output + static_cast<std::size_t>(line) * output_stride +
                          static_cast<std::size_t>(position) * 4;
                } else {
                    out = output + static_cast<std::size_t>(position) * output_stride +
                          static_cast<std::size_t>(line) * 4;
                }
                for (int channel = 0; channel < 3; ++channel) {
                    out[channel] = static_cast<unsigned char>(
                        std::min(255.0, std::max(0.0, sums[channel] / divisor + 0.5)));
                }
                out[3] = 0xFF;

                const unsigned char* leaving = at(position - radius);
                const unsigned char* arriving = at(position + radius + 1);
                for (int channel = 0; channel < 3; ++channel) {
                    sums[channel] += arriving[channel] - leaving[channel];
                }
            }
        }
        cairo_surface_mark_dirty(target.surface());
        current = target;
    };

    for (int pass = 0; pass < 3; ++pass) {
        blur_pass(radii[pass], true);
        blur_pass(radii[pass], false);
    }
    return current;
}

Image blend_to_black(const Image& source_in, double amount) {
    Image source = to_rgb(source_in);
    if (!source) return Image();
    const double keep = 1.0 - std::min(1.0, std::max(0.0, amount));
    Image result = source.clone();
    if (!result) return Image();
    unsigned char* data = result.data();
    const std::size_t bytes = static_cast<std::size_t>(result.stride()) * result.height();
    for (std::size_t i = 0; i + 3 < bytes; i += 4) {
        data[i + 0] = static_cast<unsigned char>(data[i + 0] * keep + 0.5);
        data[i + 1] = static_cast<unsigned char>(data[i + 1] * keep + 0.5);
        data[i + 2] = static_cast<unsigned char>(data[i + 2] * keep + 0.5);
    }
    cairo_surface_mark_dirty(result.surface());
    return result;
}

Image desaturate(const Image& source_in, double factor) {
    Image source = to_rgb(source_in);
    if (!source) return Image();
    Image result = source.clone();
    if (!result) return Image();
    unsigned char* data = result.data();
    const int width = result.width();
    const int height = result.height();
    const int stride = result.stride();
    for (int y = 0; y < height; ++y) {
        unsigned char* row = data + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
            unsigned char* px = row + static_cast<std::size_t>(x) * 4;
            const double b = px[0];
            const double g = px[1];
            const double r = px[2];
            // Pillow's Rec.601 luma, 16-bit fixed point.
            const double luma = (19595.0 * r + 38470.0 * g + 7471.0 * b + 32768.0) / 65536.0;
            px[0] = static_cast<unsigned char>(
                std::min(255.0, std::max(0.0, luma + (b - luma) * factor + 0.5)));
            px[1] = static_cast<unsigned char>(
                std::min(255.0, std::max(0.0, luma + (g - luma) * factor + 0.5)));
            px[2] = static_cast<unsigned char>(
                std::min(255.0, std::max(0.0, luma + (r - luma) * factor + 0.5)));
        }
    }
    cairo_surface_mark_dirty(result.surface());
    return result;
}

double mean_difference(const Image& a_in, const Image& b_in) {
    Image a = to_rgb(a_in);
    Image b = to_rgb(b_in);
    if (!a || !b) return 0.0;
    if (a.width() != b.width() || a.height() != b.height()) {
        b = resize_bilinear(b, a.width(), a.height());
        if (!b) return 0.0;
    }
    const unsigned char* left = a.data();
    const unsigned char* right = b.data();
    const int width = a.width();
    const int height = a.height();
    unsigned long long total = 0;
    for (int y = 0; y < height; ++y) {
        const unsigned char* lrow = left + static_cast<std::size_t>(y) * a.stride();
        const unsigned char* rrow = right + static_cast<std::size_t>(y) * b.stride();
        for (int x = 0; x < width; ++x) {
            for (int channel = 0; channel < 3; ++channel) {
                total += static_cast<unsigned long long>(
                    std::abs(static_cast<int>(lrow[x * 4 + channel]) -
                             static_cast<int>(rrow[x * 4 + channel])));
            }
        }
    }
    const double count = static_cast<double>(width) * height * 3.0;
    return count > 0.0 ? static_cast<double>(total) / count : 0.0;
}

std::uint32_t pixel_at(const Image& image, int x, int y) {
    if (!image || x < 0 || y < 0 || x >= image.width() || y >= image.height()) return 0;
    const unsigned char* px =
        image.data() + static_cast<std::size_t>(y) * image.stride() + static_cast<std::size_t>(x) * 4;
    return (static_cast<std::uint32_t>(px[2]) << 16) | (static_cast<std::uint32_t>(px[1]) << 8) |
           px[0];
}

}  // namespace wintab
