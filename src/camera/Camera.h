#pragma once
// Camera abstraction. Backends: Leica USB3 (native), DirectShow/Media
// Foundation (any UVC or vendor WDM camera) and a simulator for development.

#include "core/Frame.h"

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace lm {

struct CameraInfo {
    std::string id;        // backend-unique identifier used to open the device
    std::string name;      // user visible
    std::string backend;   // "Leica USB", "Media Foundation", "Simulator"
    std::string serial;
    std::string model;
};

struct Resolution {
    int width = 0;
    int height = 0;
    int binning = 1;       // 1 = full resolution
    std::string label;     // e.g. "2048 x 1536 (full)"
};

// Generic numeric/bool/enum property exposed by a backend beyond the common
// controls (shown in the "Camera > Advanced" panel).
struct CameraProperty {
    enum class Type { Number, Bool, Enum, Action };
    std::string key;
    std::string label;
    Type type = Type::Number;
    double min = 0, max = 1, step = 1, value = 0;
    std::vector<std::string> options; // Enum
    bool readOnly = false;
};

struct Range {
    double min = 0, max = 0;
};

class Camera {
public:
    using FrameCallback = std::function<void(RawFramePtr)>;
    using ErrorCallback = std::function<void(const std::string &)>;

    virtual ~Camera() = default;

    virtual CameraInfo info() const = 0;
    virtual bool open(std::string &error) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    // Live streaming. Frames arrive on a backend thread.
    virtual bool startStreaming(std::string &error) = 0;
    virtual void stopStreaming() = 0;
    virtual bool isStreaming() const = 0;

    // Resolutions available for live and capture.
    virtual std::vector<Resolution> resolutions() const = 0;
    virtual int resolutionIndex() const = 0;
    virtual bool setResolutionIndex(int index) = 0;

    // Exposure in milliseconds
    virtual Range exposureRange() const = 0;
    virtual double exposure() const = 0;
    virtual bool setExposure(double ms) = 0;

    // Analog gain as a multiplier (1.0 = unity)
    virtual Range gainRange() const = 0;
    virtual double gain() const = 0;
    virtual bool setGain(double g) = 0;

    // Bit depth of delivered data (8 or up to 16); setBitDepth may be unsupported.
    virtual std::vector<int> bitDepths() const { return {8}; }
    virtual int bitDepth() const { return 8; }
    virtual bool setBitDepth(int) { return false; }

    // Backend-specific extras
    virtual std::vector<CameraProperty> properties() const { return {}; }
    virtual bool setProperty(const std::string &, double) { return false; }

    // Whether frames are raw sensor data (and the colour pipeline should do
    // white balance etc.) or already processed by camera/driver.
    virtual bool deliversRaw() const { return true; }

    // Multi-shot (sensor shift) capture modes, e.g. "4-shot true colour".
    struct ShotMode {
        std::string name;
        int shots = 1;
        int upscale = 1;      // output resolution factor
        std::vector<std::pair<double, double>> offsets; // nominal sensor offsets (pixels)
    };
    virtual std::vector<ShotMode> shotModes() const { return {}; }

    // Colour correction matrix (row major) from white balanced camera RGB to
    // linear sRGB; identity if the camera has no calibration.
    virtual std::array<double, 9> colorMatrix() const { return {1, 0, 0, 0, 1, 0, 0, 0, 1}; }
    // short description of that calibration (stored in image metadata)
    virtual std::string colorMatrixName() const { return {}; }
    // Captures all shots of a mode. Blocks; live streaming is paused and
    // resumed internally. `progress` is called with (done, total).
    virtual bool captureShots(int modeIndex, std::vector<RawFramePtr> &shots, std::string &error,
                              const std::function<void(int, int)> &progress = {})
    {
        (void)modeIndex; (void)shots; (void)progress;
        error = "multi-shot capture not supported";
        return false;
    }

    // Human readable device details (sensor, firmware, ...), shown in the UI.
    virtual std::vector<std::pair<std::string, std::string>> details() const { return {}; }

    void setFrameCallback(FrameCallback cb)
    {
        std::lock_guard<std::mutex> l(m_cbMutex);
        m_frameCb = std::move(cb);
    }
    void setErrorCallback(ErrorCallback cb)
    {
        std::lock_guard<std::mutex> l(m_cbMutex);
        m_errorCb = std::move(cb);
    }

protected:
    void emitFrame(RawFramePtr f)
    {
        FrameCallback cb;
        {
            std::lock_guard<std::mutex> l(m_cbMutex);
            cb = m_frameCb;
        }
        if (cb)
            cb(std::move(f));
    }
    void emitError(const std::string &e)
    {
        ErrorCallback cb;
        {
            std::lock_guard<std::mutex> l(m_cbMutex);
            cb = m_errorCb;
        }
        if (cb)
            cb(e);
    }

private:
    std::mutex m_cbMutex;
    FrameCallback m_frameCb;
    ErrorCallback m_errorCb;
};

// A backend factory enumerates devices of one kind.
class CameraBackend {
public:
    virtual ~CameraBackend() = default;
    virtual std::string name() const = 0;
    virtual std::vector<CameraInfo> enumerate() = 0;
    virtual std::unique_ptr<Camera> create(const CameraInfo &info) = 0;
};

// Registry of all compiled-in backends.
std::vector<std::unique_ptr<CameraBackend>> createBackends();

} // namespace lm
