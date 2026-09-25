#pragma once
// Raw and processed image containers shared by the camera, imaging and UI layers.

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace lm {

enum class PixelFormat {
    Mono8,
    Mono16,      // little-endian, significant bits given by Frame::bitDepth
    BayerRG8,    // first row: R G R G ...
    BayerGR8,
    BayerGB8,
    BayerBG8,
    BayerRG16,
    BayerGR16,
    BayerGB16,
    BayerBG16,
    RGB8,        // packed R,G,B
    BGR8,        // packed B,G,R
    BGRA8,
    RGB16,       // packed R,G,B, 16 bit each
    YUYV,        // YUV 4:2:2
    NV12,
};

const char *pixelFormatName(PixelFormat f);
int bytesPerPixel(PixelFormat f);          // for NV12 returns 1 (luma plane)
bool isBayer(PixelFormat f);
bool is16Bit(PixelFormat f);
bool isMono(PixelFormat f);

// A frame as delivered by a camera. Data is owned by the frame.
struct RawFrame {
    int width = 0;
    int height = 0;
    int stride = 0;          // bytes per row
    int bitDepth = 8;        // significant bits per sample
    PixelFormat format = PixelFormat::Mono8;
    uint64_t sequence = 0;
    std::chrono::steady_clock::time_point timestamp{};
    double exposureMs = 0.0; // exposure that produced this frame (if known)
    double gain = 1.0;
    std::vector<uint8_t> data;

    bool empty() const { return data.empty() || width <= 0 || height <= 0; }
};
using RawFramePtr = std::shared_ptr<const RawFrame>;

// Recycles frame buffers so streaming does not allocate (and zero) several
// megabytes per frame. Frames return to the pool when the last reference is
// released; the pool itself is reference counted and may outlive its owner.
class FramePool : public std::enable_shared_from_this<FramePool> {
public:
    static std::shared_ptr<FramePool> create(size_t maxFree = 8) { return std::shared_ptr<FramePool>(new FramePool(maxFree)); }
    // Returns a frame whose data vector holds at least `bytes` (contents undefined).
    std::shared_ptr<RawFrame> acquire(size_t bytes);

private:
    explicit FramePool(size_t maxFree) : m_maxFree(maxFree) {}
    void release(RawFrame *f);
    std::mutex m_mutex;
    std::vector<std::unique_ptr<RawFrame>> m_free;
    size_t m_maxFree;
};

// Linear, 3-channel 16-bit working image (R,G,B interleaved). All processing
// before display/output happens in this space to keep maximal precision.
struct Image16 {
    int width = 0;
    int height = 0;
    std::vector<uint16_t> px; // size = width*height*3

    Image16() = default;
    Image16(int w, int h) : width(w), height(h), px(size_t(w) * h * 3, 0) {}
    bool empty() const { return px.empty(); }
    uint16_t *row(int y) { return px.data() + size_t(y) * width * 3; }
    const uint16_t *row(int y) const { return px.data() + size_t(y) * width * 3; }
};

// 8-bit RGB image (R,G,B interleaved), used for display and 8-bit export.
struct Image8 {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> px; // size = width*height*3

    Image8() = default;
    Image8(int w, int h) : width(w), height(h), px(size_t(w) * h * 3, 0) {}
    bool empty() const { return px.empty(); }
    uint8_t *row(int y) { return px.data() + size_t(y) * width * 3; }
    const uint8_t *row(int y) const { return px.data() + size_t(y) * width * 3; }
};

// Floating point single-channel image, used by focus stacking and stitching.
struct ImageF {
    int width = 0;
    int height = 0;
    std::vector<float> px;
    ImageF() = default;
    ImageF(int w, int h, float v = 0.f) : width(w), height(h), px(size_t(w) * h, v) {}
    float &at(int x, int y) { return px[size_t(y) * width + x]; }
    float at(int x, int y) const { return px[size_t(y) * width + x]; }
};

} // namespace lm
