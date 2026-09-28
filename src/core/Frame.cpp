#include "Frame.h"

namespace lm {

std::shared_ptr<RawFrame> FramePool::acquire(size_t bytes)
{
    std::unique_ptr<RawFrame> f;
    {
        std::lock_guard<std::mutex> l(m_mutex);
        if (!m_free.empty()) {
            f = std::move(m_free.back());
            m_free.pop_back();
        }
    }
    if (!f)
        f = std::make_unique<RawFrame>();
    if (f->data.size() != bytes)
        f->data.resize(bytes); // only on the first use (or a size change)
    std::weak_ptr<FramePool> weak = weak_from_this();
    return std::shared_ptr<RawFrame>(f.release(), [weak](RawFrame *p) {
        if (auto pool = weak.lock())
            pool->release(p);
        else
            delete p;
    });
}

void FramePool::release(RawFrame *f)
{
    std::lock_guard<std::mutex> l(m_mutex);
    if (m_free.size() < m_maxFree)
        m_free.emplace_back(f);
    else
        delete f;
}

const char *pixelFormatName(PixelFormat f)
{
    switch (f) {
    case PixelFormat::Mono8: return "Mono8";
    case PixelFormat::Mono16: return "Mono16";
    case PixelFormat::BayerRG8: return "BayerRG8";
    case PixelFormat::BayerGR8: return "BayerGR8";
    case PixelFormat::BayerGB8: return "BayerGB8";
    case PixelFormat::BayerBG8: return "BayerBG8";
    case PixelFormat::BayerRG16: return "BayerRG16";
    case PixelFormat::BayerGR16: return "BayerGR16";
    case PixelFormat::BayerGB16: return "BayerGB16";
    case PixelFormat::BayerBG16: return "BayerBG16";
    case PixelFormat::RGB8: return "RGB8";
    case PixelFormat::BGR8: return "BGR8";
    case PixelFormat::BGRA8: return "BGRA8";
    case PixelFormat::RGB16: return "RGB16";
    case PixelFormat::YUYV: return "YUYV";
    case PixelFormat::NV12: return "NV12";
    }
    return "?";
}

int bytesPerPixel(PixelFormat f)
{
    switch (f) {
    case PixelFormat::Mono8:
    case PixelFormat::BayerRG8:
    case PixelFormat::BayerGR8:
    case PixelFormat::BayerGB8:
    case PixelFormat::BayerBG8:
    case PixelFormat::NV12:
        return 1;
    case PixelFormat::Mono16:
    case PixelFormat::BayerRG16:
    case PixelFormat::BayerGR16:
    case PixelFormat::BayerGB16:
    case PixelFormat::BayerBG16:
    case PixelFormat::YUYV:
        return 2;
    case PixelFormat::RGB8:
    case PixelFormat::BGR8:
        return 3;
    case PixelFormat::BGRA8:
        return 4;
    case PixelFormat::RGB16:
        return 6;
    }
    return 1;
}

bool isBayer(PixelFormat f)
{
    switch (f) {
    case PixelFormat::BayerRG8:
    case PixelFormat::BayerGR8:
    case PixelFormat::BayerGB8:
    case PixelFormat::BayerBG8:
    case PixelFormat::BayerRG16:
    case PixelFormat::BayerGR16:
    case PixelFormat::BayerGB16:
    case PixelFormat::BayerBG16:
        return true;
    default:
        return false;
    }
}

PixelFormat widenedTo16(PixelFormat f)
{
    switch (f) {
    case PixelFormat::Mono8: return PixelFormat::Mono16;
    case PixelFormat::BayerRG8: return PixelFormat::BayerRG16;
    case PixelFormat::BayerGR8: return PixelFormat::BayerGR16;
    case PixelFormat::BayerGB8: return PixelFormat::BayerGB16;
    case PixelFormat::BayerBG8: return PixelFormat::BayerBG16;
    case PixelFormat::RGB8: return PixelFormat::RGB16;
    default: return f;
    }
}

bool is16Bit(PixelFormat f)
{
    switch (f) {
    case PixelFormat::Mono16:
    case PixelFormat::BayerRG16:
    case PixelFormat::BayerGR16:
    case PixelFormat::BayerGB16:
    case PixelFormat::BayerBG16:
    case PixelFormat::RGB16:
        return true;
    default:
        return false;
    }
}

bool isMono(PixelFormat f)
{
    return f == PixelFormat::Mono8 || f == PixelFormat::Mono16;
}

} // namespace lm
