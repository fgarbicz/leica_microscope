#include "V4l2Camera.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <chrono>
#include <cstring>
#include <string>

namespace lm {

namespace {

constexpr int kBufferCount = 4;

// ioctl retried over EINTR (a signal must not look like a device error).
int xioctl(int fd, unsigned long request, void *arg)
{
    int r;
    do {
        r = ::ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

std::string errnoText()
{
    return std::string(std::strerror(errno)) + " (" + std::to_string(errno) + ")";
}

std::string fourccName(uint32_t f)
{
    char s[5] = {char(f & 0xff), char((f >> 8) & 0xff), char((f >> 16) & 0xff), char((f >> 24) & 0xff), 0};
    return s;
}

// The pixel formats the imaging pipeline can consume without a JPEG decoder.
// MJPEG/compressed modes are deliberately not offered: lmcore has no JPEG
// decoder (see details()).
PixelFormat mapFourcc(uint32_t f, bool &supported)
{
    supported = true;
    switch (f) {
    case V4L2_PIX_FMT_YUYV:
        return PixelFormat::YUYV;
    case V4L2_PIX_FMT_NV12:
        return PixelFormat::NV12;
    case V4L2_PIX_FMT_RGB24:
        return PixelFormat::RGB8;
    case V4L2_PIX_FMT_BGR24:
        return PixelFormat::BGR8;
    case V4L2_PIX_FMT_ABGR32: // V4L2 names it by memory order: B,G,R,A bytes
        return PixelFormat::BGRA8;
    case V4L2_PIX_FMT_GREY:
        return PixelFormat::Mono8;
    case V4L2_PIX_FMT_SRGGB8:
        return PixelFormat::BayerRG8;
    case V4L2_PIX_FMT_SGRBG8:
        return PixelFormat::BayerGR8;
    case V4L2_PIX_FMT_SGBRG8:
        return PixelFormat::BayerGB8;
    case V4L2_PIX_FMT_SBGGR8:
        return PixelFormat::BayerBG8;
    default:
        supported = false;
        return PixelFormat::YUYV;
    }
}

// Preference order when a camera offers several usable formats for one size.
int formatScore(uint32_t f)
{
    switch (f) {
    case V4L2_PIX_FMT_ABGR32:
        return 6;
    case V4L2_PIX_FMT_RGB24:
    case V4L2_PIX_FMT_BGR24:
        return 5;
    case V4L2_PIX_FMT_YUYV:
        return 4;
    case V4L2_PIX_FMT_NV12:
        return 3;
    case V4L2_PIX_FMT_SRGGB8:
    case V4L2_PIX_FMT_SGRBG8:
    case V4L2_PIX_FMT_SGBRG8:
    case V4L2_PIX_FMT_SBGGR8:
        return 2;
    case V4L2_PIX_FMT_GREY:
        return 1;
    default:
        return 0;
    }
}

} // namespace

std::vector<CameraInfo> V4l2Backend::enumerate()
{
    std::vector<CameraInfo> out;
    DIR *dir = ::opendir("/dev");
    if (!dir)
        return out;
    std::vector<std::string> nodes;
    while (dirent *e = ::readdir(dir)) {
        const std::string n(e->d_name);
        if (n.rfind("video", 0) == 0)
            nodes.push_back("/dev/" + n);
    }
    ::closedir(dir);
    std::sort(nodes.begin(), nodes.end());
    for (const auto &path : nodes) {
        const int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0)
            continue;
        v4l2_capability cap{};
        if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
            const uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
            // a UVC device exposes several nodes; only the capture node is usable
            if ((caps & V4L2_CAP_VIDEO_CAPTURE) && (caps & V4L2_CAP_STREAMING)) {
                CameraInfo ci;
                ci.id = path;
                ci.name = reinterpret_cast<const char *>(cap.card);
                ci.model = ci.name;
                ci.backend = name();
                out.push_back(ci);
            }
        }
        ::close(fd);
    }
    return out;
}

std::unique_ptr<Camera> V4l2Backend::create(const CameraInfo &info)
{
    return std::make_unique<V4l2Camera>(info);
}

V4l2Camera::V4l2Camera(CameraInfo info) : m_info(std::move(info)) {}

V4l2Camera::~V4l2Camera()
{
    close();
}

bool V4l2Camera::open(std::string &error)
{
    if (m_fd >= 0)
        return true;
    const int fd = ::open(m_info.id.c_str(), O_RDWR);
    if (fd < 0) {
        error = "Cannot open " + m_info.id + ": " + errnoText()
                + ". Add your user to the 'video' group (sudo usermod -aG video $USER) and log in again.";
        return false;
    }
    v4l2_capability cap{};
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) {
        error = m_info.id + " is not a V4L2 device: " + errnoText();
        ::close(fd);
        return false;
    }
    m_driver = reinterpret_cast<const char *>(cap.driver);
    m_card = reinterpret_cast<const char *>(cap.card);
    m_fd = fd;

    // usable (uncompressed) formats x frame sizes, best format first per size
    struct Candidate {
        Mode mode;
        int score;
    };
    std::vector<Candidate> candidates;
    for (uint32_t fi = 0;; ++fi) {
        v4l2_fmtdesc fmt{};
        fmt.index = fi;
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(m_fd, VIDIOC_ENUM_FMT, &fmt) != 0)
            break;
        bool supported = false;
        mapFourcc(fmt.pixelformat, supported);
        if (!supported)
            continue;
        for (uint32_t si = 0;; ++si) {
            v4l2_frmsizeenum fs{};
            fs.index = si;
            fs.pixel_format = fmt.pixelformat;
            if (xioctl(m_fd, VIDIOC_ENUM_FRAMESIZES, &fs) != 0)
                break;
            if (fs.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                candidates.push_back({{fmt.pixelformat, int(fs.discrete.width), int(fs.discrete.height)},
                                      formatScore(fmt.pixelformat)});
            } else if (fs.type == V4L2_FRMSIZE_TYPE_STEPWISE || fs.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
                // only the largest size of a stepwise range is offered
                candidates.push_back({{fmt.pixelformat, int(fs.stepwise.max_width), int(fs.stepwise.max_height)},
                                      formatScore(fmt.pixelformat)});
                break;
            }
        }
    }
    // largest resolution first; one entry per size, keeping the best format
    std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
        const long long aa = 1LL * a.mode.width * a.mode.height, bb = 1LL * b.mode.width * b.mode.height;
        if (aa != bb)
            return aa > bb;
        return a.score > b.score;
    });
    for (const auto &c : candidates) {
        const bool seen = std::any_of(m_modes.begin(), m_modes.end(), [&](const Mode &m) {
            return m.width == c.mode.width && m.height == c.mode.height;
        });
        if (seen)
            continue;
        m_modes.push_back(c.mode);
        Resolution r;
        r.width = c.mode.width;
        r.height = c.mode.height;
        r.binning = 1;
        r.label = std::to_string(r.width) + " x " + std::to_string(r.height) + " (" + fourccName(c.mode.fourcc) + ")";
        m_resolutions.push_back(r);
    }
    if (m_modes.empty()) {
        error = std::string(m_card) + " offers no uncompressed video format. Compressed (MJPEG/H.264) modes are not "
                                     "supported; use the native Leica driver or a camera with a YUYV or RGB mode.";
        ::close(m_fd);
        m_fd = -1;
        return false;
    }
    if (!m_resolutions.empty())
        m_resolutions.front().label += " (full)";
    if (!configure(0, error)) {
        ::close(m_fd);
        m_fd = -1;
        return false;
    }
    queryControls();
    return true;
}

void V4l2Camera::close()
{
    stopStreaming();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    m_resolutions.clear();
    m_modes.clear();
}

bool V4l2Camera::configure(int index, std::string &error)
{
    if (index < 0 || index >= int(m_modes.size())) {
        error = "invalid resolution index";
        return false;
    }
    const Mode &m = m_modes[size_t(index)];
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = uint32_t(m.width);
    fmt.fmt.pix.height = uint32_t(m.height);
    fmt.fmt.pix.pixelformat = m.fourcc;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(m_fd, VIDIOC_S_FMT, &fmt) != 0) {
        error = "Cannot set the video format: " + errnoText();
        return false;
    }
    // the driver may have adjusted the request
    m_outW = int(fmt.fmt.pix.width);
    m_outH = int(fmt.fmt.pix.height);
    m_outStride = int(fmt.fmt.pix.bytesperline);
    m_fourcc = fmt.fmt.pix.pixelformat;
    m_resIndex = index;
    return true;
}

bool V4l2Camera::setResolutionIndex(int index)
{
    if (index == m_resIndex)
        return true;
    const bool wasStreaming = m_streaming;
    // also when the stream died by itself: VIDIOC_S_FMT fails while buffers are allocated
    stopStreaming();
    std::string error;
    const bool ok = configure(index, error);
    if (wasStreaming) {
        // a refused format leaves the previous one set; either way live must resume
        std::string startError;
        if (!startStreaming(startError)) {
            emitError("The live image did not restart after the format change: " + startError);
            return false;
        }
    }
    return ok;
}

bool V4l2Camera::mapBuffers(std::string &error)
{
    v4l2_requestbuffers req{};
    req.count = kBufferCount;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(m_fd, VIDIOC_REQBUFS, &req) != 0) {
        error = "The device does not support memory mapped streaming: " + errnoText();
        return false;
    }
    if (req.count < 2) {
        error = "The device could not provide enough video buffers";
        return false;
    }
    for (uint32_t i = 0; i < req.count; ++i) {
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (xioctl(m_fd, VIDIOC_QUERYBUF, &b) != 0) {
            error = "Cannot query video buffer: " + errnoText();
            unmapBuffers();
            return false;
        }
        void *p = ::mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, off_t(b.m.offset));
        if (p == MAP_FAILED) {
            error = "Cannot map video buffer: " + errnoText();
            unmapBuffers();
            return false;
        }
        m_buffers.push_back({p, b.length});
        if (xioctl(m_fd, VIDIOC_QBUF, &b) != 0) {
            error = "Cannot queue video buffer: " + errnoText();
            unmapBuffers();
            return false;
        }
    }
    return true;
}

void V4l2Camera::unmapBuffers()
{
    for (auto &b : m_buffers)
        if (b.start)
            ::munmap(b.start, b.length);
    m_buffers.clear();
    if (m_fd >= 0) {
        v4l2_requestbuffers req{};
        req.count = 0;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        xioctl(m_fd, VIDIOC_REQBUFS, &req);
    }
}

bool V4l2Camera::startStreaming(std::string &error)
{
    if (m_streaming)
        return true;
    if (m_fd < 0) {
        error = "camera not open";
        return false;
    }
    // a stream that died on its own still holds its thread and buffers
    stopStreaming();
    if (!mapBuffers(error))
        return false;
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(m_fd, VIDIOC_STREAMON, &type) != 0) {
        error = "Cannot start the video stream: " + errnoText();
        unmapBuffers();
        return false;
    }
    m_stop = false;
    m_streaming = true;
    m_thread = std::thread([this] {
        try {
            run();
        } catch (...) {
            m_streaming = false;
            emitCurrentException("video capture failed: ");
        }
    });
    return true;
}

void V4l2Camera::stopStreaming()
{
    // Always join and release, also when the capture thread already gave up (a
    // joinable std::thread must never be assigned to or destroyed).
    m_stop = true;
    if (m_thread.joinable())
        m_thread.join();
    m_streaming = false;
    if (m_buffers.empty())
        return;
    if (m_fd >= 0) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(m_fd, VIDIOC_STREAMOFF, &type);
    }
    unmapBuffers();
}

void V4l2Camera::run()
{
    auto pool = FramePool::create();
    uint64_t seq = 0;
    int errors = 0;
    bool supported = false;
    const PixelFormat format = mapFourcc(m_fourcc, supported);
    // the stream is dead: tell the application (it closes the camera and
    // reconnects) and stop; stopStreaming() releases the rest
    auto fail = [this](const std::string &message) {
        m_streaming = false;
        emitError(message);
    };
    while (!m_stop) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(m_fd, &fds);
        timeval tv{0, 200000}; // 200 ms, so stopStreaming() is noticed promptly
        const int r = ::select(m_fd + 1, &fds, nullptr, nullptr, &tv);
        if (r == 0)
            continue;
        if (r < 0) {
            if (errno == EINTR)
                continue;
            if (++errors > 20) {
                fail("Video stream error: " + errnoText());
                return;
            }
            continue;
        }
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (xioctl(m_fd, VIDIOC_DQBUF, &b) != 0) {
            if (errno == EAGAIN)
                continue;
            // ENODEV: unplugged, retrying cannot help
            if (errno == ENODEV || ++errors > 20) {
                fail("Video capture stopped: " + errnoText() + " (device disconnected?)");
                return;
            }
            continue;
        }
        errors = 0;
        if (b.index < m_buffers.size() && b.bytesused > 0) {
            const int stride = m_outStride > 0 ? m_outStride : m_outW * bytesPerPixel(format);
            // NV12 carries a half-height chroma plane after the luma plane
            const size_t need = format == PixelFormat::NV12 ? size_t(stride) * m_outH * 3 / 2
                                                            : size_t(stride) * m_outH;
            if (b.bytesused >= need) {
                auto f = pool->acquire(need);
                f->width = m_outW;
                f->height = m_outH;
                f->stride = stride;
                f->format = format;
                f->bitDepth = 8;
                f->sequence = seq++;
                f->timestamp = std::chrono::steady_clock::now();
                f->exposureMs = m_exposure;
                f->gain = m_gain;
                std::memcpy(f->data.data(), m_buffers[b.index].start, need);
                emitFrame(f);
            }
        }
        xioctl(m_fd, VIDIOC_QBUF, &b);
    }
}

bool V4l2Camera::controlRange(uint32_t id, double &min, double &max, double &step, double &value) const
{
    if (m_fd < 0)
        return false;
    v4l2_queryctrl q{};
    q.id = id;
    if (xioctl(m_fd, VIDIOC_QUERYCTRL, &q) != 0 || (q.flags & V4L2_CTRL_FLAG_DISABLED))
        return false;
    min = q.minimum;
    max = q.maximum;
    step = q.step > 0 ? q.step : 1;
    v4l2_control c{};
    c.id = id;
    value = xioctl(m_fd, VIDIOC_G_CTRL, &c) == 0 ? c.value : q.default_value;
    return true;
}

void V4l2Camera::queryControls()
{
    double min = 0, max = 0, step = 1, value = 0;
    // UVC exposure is in 100 µs units
    if (controlRange(V4L2_CID_EXPOSURE_ABSOLUTE, min, max, step, value)) {
        m_hasExposure = true;
        m_expRange = {std::max(0.1, min / 10.0), max / 10.0};
        m_exposure = value / 10.0;
        // manual exposure, so setExposure() takes effect
        v4l2_control c{};
        c.id = V4L2_CID_EXPOSURE_AUTO;
        c.value = V4L2_EXPOSURE_MANUAL;
        xioctl(m_fd, VIDIOC_S_CTRL, &c);
    }
    if (controlRange(V4L2_CID_GAIN, min, max, step, value)) {
        m_hasGain = true;
        m_gainMinRaw = min;
        m_gainMaxRaw = max;
        // report gain as a multiplier: the raw range maps to 1x .. 16x
        m_gainRange = {1.0, max > min ? 16.0 : 1.0};
        m_gain = max > min ? 1.0 + (value - min) / (max - min) * 15.0 : 1.0;
    }
}

bool V4l2Camera::setExposure(double ms)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_fd < 0 || !m_hasExposure)
        return false;
    ms = std::clamp(ms, m_expRange.min, m_expRange.max);
    v4l2_control c{};
    c.id = V4L2_CID_EXPOSURE_ABSOLUTE;
    c.value = int(std::lround(ms * 10.0));
    if (xioctl(m_fd, VIDIOC_S_CTRL, &c) != 0)
        return false;
    m_exposure = ms;
    return true;
}

bool V4l2Camera::setGain(double g)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_fd < 0 || !m_hasGain || m_gainMaxRaw <= m_gainMinRaw)
        return false;
    g = std::clamp(g, m_gainRange.min, m_gainRange.max);
    v4l2_control c{};
    c.id = V4L2_CID_GAIN;
    c.value = int(std::lround(m_gainMinRaw + (g - 1.0) / 15.0 * (m_gainMaxRaw - m_gainMinRaw)));
    if (xioctl(m_fd, VIDIOC_S_CTRL, &c) != 0)
        return false;
    m_gain = g;
    return true;
}

std::vector<CameraProperty> V4l2Camera::properties() const
{
    struct Entry {
        uint32_t id;
        const char *key;
        const char *label;
    };
    static const Entry kEntries[] = {
        {V4L2_CID_BRIGHTNESS, "brightness", "Brightness"},
        {V4L2_CID_CONTRAST, "contrast", "Contrast"},
        {V4L2_CID_SATURATION, "saturation", "Saturation"},
        {V4L2_CID_HUE, "hue", "Hue"},
        {V4L2_CID_GAMMA, "gamma", "Gamma"},
        {V4L2_CID_SHARPNESS, "sharpness", "Sharpness"},
        {V4L2_CID_BACKLIGHT_COMPENSATION, "backlight", "Backlight compensation"},
        {V4L2_CID_WHITE_BALANCE_TEMPERATURE, "wbtemp", "White balance temperature"},
        {V4L2_CID_POWER_LINE_FREQUENCY, "powerline", "Power line frequency"},
    };
    std::vector<CameraProperty> out;
    for (const auto &e : kEntries) {
        double min = 0, max = 0, step = 1, value = 0;
        if (!controlRange(e.id, min, max, step, value))
            continue;
        CameraProperty p;
        p.key = e.key;
        p.label = e.label;
        p.type = CameraProperty::Type::Number;
        p.min = min;
        p.max = max;
        p.step = step;
        p.value = value;
        out.push_back(p);
    }
    return out;
}

bool V4l2Camera::setProperty(const std::string &key, double value)
{
    static const std::pair<const char *, uint32_t> kIds[] = {
        {"brightness", V4L2_CID_BRIGHTNESS},
        {"contrast", V4L2_CID_CONTRAST},
        {"saturation", V4L2_CID_SATURATION},
        {"hue", V4L2_CID_HUE},
        {"gamma", V4L2_CID_GAMMA},
        {"sharpness", V4L2_CID_SHARPNESS},
        {"backlight", V4L2_CID_BACKLIGHT_COMPENSATION},
        {"wbtemp", V4L2_CID_WHITE_BALANCE_TEMPERATURE},
        {"powerline", V4L2_CID_POWER_LINE_FREQUENCY},
    };
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_fd < 0)
        return false;
    for (const auto &[name, id] : kIds) {
        if (key != name)
            continue;
        v4l2_control c{};
        c.id = id;
        c.value = int(std::lround(value));
        return xioctl(m_fd, VIDIOC_S_CTRL, &c) == 0;
    }
    return false;
}

std::vector<std::pair<std::string, std::string>> V4l2Camera::details() const
{
    std::vector<std::pair<std::string, std::string>> d;
    d.emplace_back("Device", m_info.id);
    if (!m_card.empty())
        d.emplace_back("Name", m_card);
    if (!m_driver.empty())
        d.emplace_back("Driver", m_driver);
    if (m_outW > 0)
        d.emplace_back("Format", std::to_string(m_outW) + " x " + std::to_string(m_outH) + " " + fourccName(m_fourcc));
    d.emplace_back("Exposure control", m_hasExposure ? "manual (UVC)" : "not available");
    d.emplace_back("Gain control", m_hasGain ? "available" : "not available");
    return d;
}

} // namespace lm
