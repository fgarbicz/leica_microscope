#include "Dmc6200Camera.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace lm {

namespace {

// Piezo positions (x, y) per shot, as used by the vendor software for this
// camera. Row-major, x varies fastest. Nominal step: 1, 1/2, 1/3 pixel.
const std::vector<std::pair<int, int>> kShots1 = {{46, 46}};
const std::vector<std::pair<int, int>> kShots4 = {{46, 46}, {87, 46}, {39, 86}, {87, 85}};
const std::vector<std::pair<int, int>> kShots16 = {
    {46, 46}, {68, 46}, {87, 46}, {104, 45}, {37, 67}, {63, 66}, {85, 66}, {104, 66},
    {36, 86}, {63, 85}, {84, 85}, {104, 84}, {36, 102}, {63, 101}, {84, 101}, {104, 101}};
const std::vector<std::pair<int, int>> kShots36 = {
    {46, 46}, {61, 46}, {74, 46}, {86, 46}, {98, 45}, {109, 45}, {36, 61}, {55, 60}, {70, 60},
    {84, 59}, {97, 59}, {109, 59}, {36, 73}, {54, 73}, {70, 72}, {84, 72}, {97, 72}, {109, 72},
    {36, 85}, {54, 85}, {70, 84}, {84, 84}, {97, 84}, {109, 84}, {36, 96}, {54, 96}, {69, 95},
    {83, 95}, {96, 95}, {109, 95}, {35, 108}, {53, 107}, {69, 106}, {83, 106}, {96, 106}, {109, 106}};
constexpr int kMaxShots = 36;

std::string serialFromPath(const std::string &path)
{
    // \\?\usb#vid_1711&pid_30e0#<serial>#{guid}
    auto a = path.find('#');
    if (a == std::string::npos)
        return {};
    auto b = path.find('#', a + 1);
    auto c = b == std::string::npos ? std::string::npos : path.find('#', b + 1);
    if (b == std::string::npos || c == std::string::npos)
        return {};
    return path.substr(b + 1, c - b - 1);
}

} // namespace

std::vector<CameraInfo> Dmc6200Backend::enumerate()
{
    std::vector<CameraInfo> out;
    for (const auto &p : dmc::Protocol::findDevices()) {
        CameraInfo ci;
        ci.id = "dmc:" + p;
        ci.serial = serialFromPath(p);
        ci.model = "DMC6200";
        ci.name = "Leica DMC6200" + (ci.serial.empty() ? std::string() : " (" + ci.serial + ")");
        ci.backend = name();
        out.push_back(ci);
    }
    return out;
}

std::unique_ptr<Camera> Dmc6200Backend::create(const CameraInfo &info)
{
    return std::make_unique<Dmc6200Camera>(info);
}

Dmc6200Camera::Dmc6200Camera(CameraInfo info) : m_info(std::move(info)) {}

Dmc6200Camera::~Dmc6200Camera()
{
    close();
}

bool Dmc6200Camera::open(std::string &error)
{
    std::lock_guard<std::mutex> lock(m_ctrlMutex);
    if (m_proto.isOpen())
        return true;
    std::string path = m_info.id.rfind("dmc:", 0) == 0 ? m_info.id.substr(4) : m_info.id;
    if (!m_proto.open(path, error))
        return false;
    std::vector<uint8_t> r;
    m_proto.command(dmc::Cmd::MaxPacket, {}, 4, &r);
    m_serial = m_proto.serial();
    m_sensor = m_proto.sensorName();
    // After a USB-only reset (the camera kept its power) the sensor board is not
    // initialised: the firmware then reports a synthetic placeholder sensor and
    // cannot deliver images. Only a power cycle recovers it.
    if (m_sensor.find("Synth") != std::string::npos || m_sensor.empty()) {
        error = "The camera's sensor is not ready (it reports \"" + (m_sensor.empty() ? std::string("no sensor") : m_sensor)
                + "\"). Unplug the camera's USB cable for 5 seconds and plug it back in; "
                  "the application reconnects automatically.";
        m_proto.close();
        return false;
    }
    std::vector<uint32_t> v;
    if (m_proto.readRegisters({dmc::Reg::SensorWidth, dmc::Reg::SensorHeight, dmc::Reg::AdcBits}, v)) {
        m_sensorW = int(v[0]);
        m_sensorH = int(v[1]);
        m_adcBits = int(v[2]);
    }
    if (m_sensorW <= 0 || m_sensorH <= 0 || m_sensorW > 8192 || m_sensorH > 8192) {
        error = "Camera reported an invalid sensor size";
        m_proto.close();
        return false;
    }
    // ROI modes: full frame and centred crops (faster frame rates)
    m_rois = {{0, 0, m_sensorW, m_sensorH},
              {m_sensorW / 4 & ~7, m_sensorH / 4 & ~7, m_sensorW / 2 & ~7, m_sensorH / 2 & ~7}};
    if (!configure(error)) {
        m_proto.close();
        return false;
    }
    return true;
}

void Dmc6200Camera::close()
{
    stopStreaming();
    std::lock_guard<std::mutex> lock(m_ctrlMutex);
    if (m_proto.isOpen()) {
        m_proto.acquisition(dmc::Acq::Stop);
        m_proto.acquisition(dmc::Acq::Flush);
        m_proto.close();
    }
}

std::vector<dmc::SequenceEntry> Dmc6200Camera::sequenceFor(const std::vector<std::pair<int, int>> &pos) const
{
    std::vector<dmc::SequenceEntry> e;
    e.reserve(kMaxShots * 3);
    for (uint32_t i = 0; i < kMaxShots; ++i) {
        const bool on = i < pos.size();
        e.push_back({dmc::Reg::GainA, i, on ? uint32_t(pos[i].first) : 0u});
        e.push_back({dmc::Reg::GainB, i, on ? uint32_t(pos[i].second) : 0u});
        e.push_back({dmc::Reg::ShotEnable, i, on ? 1u : 0u});
    }
    return e;
}

void Dmc6200Camera::stopAndFlush()
{
    m_proto.acquisition(dmc::Acq::Stop);
    m_proto.acquisition(dmc::Acq::Flush);
    m_proto.resetStreamPipes();
}

bool Dmc6200Camera::configure(std::string &error)
{
    stopAndFlush();
    const Roi &roi = m_rois[size_t(std::clamp(m_resIndex, 0, int(m_rois.size()) - 1))];
    const uint32_t expUs = uint32_t(std::clamp(m_exposureMs.load() * 1000.0, 26.0, 60e6));
    const uint32_t gainFx = uint32_t(std::lround(std::clamp(m_gain.load(), 1.0, 16.0) * 65536.0));
    if (!m_proto.writeRegisters({{dmc::Reg::RoiX, uint32_t(roi.x)},
                                 {dmc::Reg::RoiY, uint32_t(roi.y)},
                                 {dmc::Reg::RoiWidth, uint32_t(roi.w)},
                                 {dmc::Reg::RoiHeight, uint32_t(roi.h)}})) {
        // fall back to full frame if the ROI is rejected
        m_resIndex = 0;
        m_proto.writeRegisters({{dmc::Reg::RoiX, 0}, {dmc::Reg::RoiY, 0},
                                {dmc::Reg::RoiWidth, uint32_t(m_sensorW)}, {dmc::Reg::RoiHeight, uint32_t(m_sensorH)}});
    }
    if (!m_proto.writeRegisters({{dmc::Reg::ExposureUs, expUs}, {dmc::Reg::Reg1013, gainFx}})) {
        error = "Cannot configure exposure/gain: " + m_proto.lastError();
        return false;
    }
    if (!m_proto.uploadSequence(sequenceFor(kShots1))) {
        error = "Cannot upload acquisition sequence: " + m_proto.lastError();
        return false;
    }
    return true;
}

bool Dmc6200Camera::startLiveLocked(std::string &error)
{
    if (!m_proto.acquisition(dmc::Acq::Live)) {
        error = "Cannot start acquisition: " + m_proto.lastError();
        return false;
    }
    if (m_thread.joinable())
        m_thread.join(); // a previous stream thread that gave up on its own
    m_stopRequested = false;
    m_threadDone = false;
    m_streaming = true;
    m_thread = std::thread([this] {
        try {
            streamLoop();
        } catch (...) {
            m_streaming = false;
            emitCurrentException("Streaming stopped: ");
        }
        m_threadDone = true;
    });
    return true;
}

void Dmc6200Camera::stopLiveLocked()
{
    if (!m_streaming && !m_thread.joinable())
        return;
    m_stopRequested = true;
    m_proto.acquisition(dmc::Acq::Stop);
    // abort repeatedly: a single abort can land before the thread's next read
    for (int i = 0; i < 400 && !m_threadDone; ++i) {
        m_proto.abortStreaming();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (m_thread.joinable())
        m_thread.join();
    m_streaming = false;
    stopAndFlush();
}

bool Dmc6200Camera::startStreaming(std::string &error)
{
    std::lock_guard<std::mutex> lock(m_ctrlMutex);
    if (!m_proto.isOpen()) {
        error = "camera not open";
        return false;
    }
    if (m_streaming)
        return true;
    if (!configure(error))
        return false;
    return startLiveLocked(error);
}

void Dmc6200Camera::stopStreaming()
{
    std::lock_guard<std::mutex> lock(m_ctrlMutex);
    stopLiveLocked();
}

RawFramePtr Dmc6200Camera::readOneFrame(unsigned timeoutMs, dmc::FrameEvent *evOut)
{
    dmc::FrameEvent ev;
    if (!m_proto.waitFrameEvent(ev, timeoutMs))
        return nullptr;
    // validate the announcement (a corrupted event must not trigger huge allocations)
    if (ev.width == 0 || ev.height == 0 || ev.width > m_sensorW || ev.height > m_sensorH
        || uint64_t(ev.bytes) != uint64_t(ev.width) * ev.height * 2)
        return nullptr;
    auto f = m_pool->acquire(ev.bytes);
    f->width = ev.width;
    f->height = ev.height;
    f->stride = ev.width * 2;
    f->bitDepth = ev.bits ? ev.bits : m_adcBits;
    // sensor mosaic is GBRG at (0,0); ROIs are 8-aligned so the phase is kept
    f->format = PixelFormat::BayerGB16;
    long long n = m_proto.readFrame(f->data.data(), ev.bytes, timeoutMs);
    if (n != (long long)ev.bytes)
        return nullptr;
    f->sequence = m_seq++;
    f->timestamp = std::chrono::steady_clock::now();
    f->exposureMs = ev.exposureUs / 1000.0;
    f->gain = m_gain;
    if (evOut)
        *evOut = ev;
    return f;
}

void Dmc6200Camera::streamLoop()
{
    int failures = 0;
    while (!m_stopRequested) {
        const unsigned timeout = unsigned(std::min(m_exposureMs.load(), 60000.0)) + 1500;
        RawFramePtr f = readOneFrame(timeout);
        if (m_stopRequested)
            break;
        if (f) {
            failures = 0;
            emitFrame(std::move(f));
            continue;
        }
        // recovery: restart the acquisition a few times before giving up
        if (++failures > 4) {
            m_streaming = false;
            emitError("Camera stopped delivering images (" + m_proto.lastError()
                      + "). Check the USB connection and restart live view.");
            return;
        }
        // each command can block for seconds: give up as soon as a stop is requested
        // (stopLiveLocked() then stops and flushes the camera itself)
        if (m_stopRequested)
            break;
        m_proto.acquisition(dmc::Acq::Stop);
        if (m_stopRequested)
            break;
        m_proto.acquisition(dmc::Acq::Flush);
        if (m_stopRequested)
            break;
        m_proto.resetStreamPipes();
        m_proto.acquisition(dmc::Acq::Live);
    }
}

std::vector<Resolution> Dmc6200Camera::resolutions() const
{
    std::vector<Resolution> out;
    for (size_t i = 0; i < m_rois.size(); ++i) {
        const auto &r = m_rois[i];
        Resolution res;
        res.width = r.w;
        res.height = r.h;
        res.label = std::to_string(r.w) + " x " + std::to_string(r.h) + (i == 0 ? " (full sensor)" : " (centre ROI, fast)");
        out.push_back(res);
    }
    return out;
}

bool Dmc6200Camera::setResolutionIndex(int index)
{
    if (index < 0 || index >= int(m_rois.size()))
        return false;
    std::lock_guard<std::mutex> lock(m_ctrlMutex);
    const bool was = m_streaming;
    stopLiveLocked();
    m_resIndex = index;
    std::string err;
    bool ok = configure(err);
    if (was && ok)
        ok = startLiveLocked(err);
    return ok;
}

bool Dmc6200Camera::setExposure(double ms)
{
    ms = std::clamp(ms, exposureRange().min, exposureRange().max);
    m_exposureMs = ms;
    if (!m_proto.isOpen())
        return true;
    // applied by the camera from the next frame on, also while streaming
    return m_proto.writeRegister(dmc::Reg::ExposureUs, uint32_t(std::lround(ms * 1000.0)));
}

bool Dmc6200Camera::setGain(double g)
{
    g = std::clamp(g, gainRange().min, gainRange().max);
    m_gain = g;
    if (!m_proto.isOpen())
        return true;
    return m_proto.writeRegister(dmc::Reg::Reg1013, uint32_t(std::lround(g * 65536.0)));
}

std::vector<CameraProperty> Dmc6200Camera::properties() const
{
    using T = CameraProperty::Type;
    return {
        {"piezo_x", "Sensor shift rest X", T::Number, 0, 255, 1, double(m_piezoRestX)},
        {"piezo_y", "Sensor shift rest Y", T::Number, 0, 255, 1, double(m_piezoRestY)},
    };
}

bool Dmc6200Camera::setProperty(const std::string &key, double value)
{
    if (key == "piezo_x")
        m_piezoRestX = std::clamp(int(value), 0, 255);
    else if (key == "piezo_y")
        m_piezoRestY = std::clamp(int(value), 0, 255);
    else
        return false;
    return true;
}

std::array<double, 9> Dmc6200Camera::colorMatrix() const
{
    // IMX174 -> CIE XYZ (D50 white) under 3200 K halogen microscope illumination,
    // from the Jenoptik DijSDK colour calibration
    // "XYZ_SP-XYZ-10nm_Axioskop40_3200K4_RGB_C2p4_imx174_Axioskop40_3200K4_Homogen_KLB_P1_G"
    // (rows sum to the D50 white point, so white balanced white stays neutral).
    static const double camToXyz[9] = {0.604850, 0.476785, -0.117435, 0.166162, 1.028150,
                                       -0.194315, 0.038239, -0.390328, 1.176990};
    // XYZ (D50) -> linear sRGB, Bradford adapted (Lindbloom)
    static const double xyzToSrgb[9] = {3.1338561, -1.6168667, -0.4906146, -0.9787684, 1.9161415,
                                        0.0334540, 0.0719453, -0.2289914, 1.4052427};
    std::array<double, 9> m{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k)
                m[size_t(i * 3 + j)] += xyzToSrgb[i * 3 + k] * camToXyz[k * 3 + j];
    return m;
}

std::vector<Camera::ShotMode> Dmc6200Camera::shotModes() const
{
    auto grid = [](int n, int f) {
        std::vector<std::pair<double, double>> o;
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                o.push_back({double(x) / f, double(y) / f});
        return o;
    };
    return {
        {"4-shot true colour (1920 x 1200)", 4, 1, grid(2, 1)},
        {"16-shot high resolution (3840 x 2400)", 16, 2, grid(4, 2)},
        {"36-shot ultra resolution (5760 x 3600)", 36, 3, grid(6, 3)},
    };
}

bool Dmc6200Camera::captureShots(int modeIndex, std::vector<RawFramePtr> &shots, std::string &error,
                                 const std::function<void(int, int)> &progress)
{
    const std::vector<std::pair<int, int>> *table = modeIndex == 0 ? &kShots4 : modeIndex == 1 ? &kShots16
                                                    : modeIndex == 2 ? &kShots36 : nullptr;
    if (!table) {
        error = "unknown shot mode";
        return false;
    }
    std::lock_guard<std::mutex> lock(m_ctrlMutex);
    const bool was = m_streaming;
    stopLiveLocked();
    // pixel shift always uses the full sensor
    const int oldRes = m_resIndex;
    m_resIndex = 0;
    bool ok = configure(error) && m_proto.uploadSequence(sequenceFor(*table));
    if (!ok && error.empty())
        error = m_proto.lastError();
    if (ok && !m_proto.acquisition(dmc::Acq::Sequence)) {
        error = "Cannot start sequence: " + m_proto.lastError();
        ok = false;
    }
    shots.clear();
    if (ok) {
        const unsigned timeout = unsigned(std::min(m_exposureMs.load(), 60000.0)) + 3000;
        for (size_t i = 0; i < table->size(); ++i) {
            RawFramePtr f = readOneFrame(timeout);
            if (!f) {
                error = "Shot " + std::to_string(i + 1) + " failed: " + m_proto.lastError();
                ok = false;
                break;
            }
            shots.push_back(std::move(f));
            if (progress)
                progress(int(i + 1), int(table->size()));
        }
    }
    m_resIndex = oldRes;
    std::string err2;
    if (!configure(err2)) { // restores the single-shot sequence
        emitError("Camera could not be reconfigured after the capture (" + err2
                  + "). Restart live view or reconnect the camera.");
    } else if (was && !startLiveLocked(err2)) {
        emitError("Live view could not be restarted after the capture (" + err2 + "). Restart live view.");
    }
    return ok;
}

std::vector<std::pair<std::string, std::string>> Dmc6200Camera::details() const
{
    return {{"Model", "Leica DMC6200 (Jenoptik GRYPHAX platform)"},
            {"Serial", m_serial},
            {"Sensor", m_sensor + " " + std::to_string(m_sensorW) + " x " + std::to_string(m_sensorH) + ", "
                           + std::to_string(m_adcBits) + "-bit, 5.86 um pixels"},
            {"Interface", "USB 3.0 (native WinUSB driver)"}};
}

} // namespace lm
