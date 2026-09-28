#pragma once
// macOS AVFoundation backend: any UVC camera (the counterpart of MFCamera on
// Windows and V4l2Camera on Linux). Implemented in AvfCamera.mm.

#include "camera/Camera.h"

#include <atomic>
#include <mutex>

namespace lm {

class AvfBackend : public CameraBackend {
public:
    std::string name() const override { return "AVFoundation"; }
    std::vector<CameraInfo> enumerate() override;
    std::unique_ptr<Camera> create(const CameraInfo &info) override;
};

class AvfCamera : public Camera {
public:
    explicit AvfCamera(CameraInfo info);
    ~AvfCamera() override;

    CameraInfo info() const override { return m_info; }
    bool open(std::string &error) override;
    void close() override;
    bool isOpen() const override { return m_impl != nullptr; }

    bool startStreaming(std::string &error) override;
    void stopStreaming() override;
    bool isStreaming() const override { return m_streaming; }

    // copies under m_mutex: deliverFrame() corrects the list on the capture queue
    std::vector<Resolution> resolutions() const override;
    int resolutionIndex() const override;
    bool setResolutionIndex(int index) override;

    Range exposureRange() const override { return m_expRange; }
    double exposure() const override { return m_exposure; }
    bool setExposure(double ms) override;
    bool canSetExposure() const override { return false; }

    Range gainRange() const override { return m_gainRange; }
    double gain() const override { return m_gain; }
    bool setGain(double g) override;
    bool canSetGain() const override { return false; }

    std::vector<CameraProperty> properties() const override;
    bool setProperty(const std::string &key, double value) override;
    bool deliversRaw() const override { return false; }

    std::vector<std::pair<std::string, std::string>> details() const override;

    // Called from the AVFoundation delegate (AvfCamera.mm) for each frame.
    void deliverFrame(const void *bgra, int width, int height, int stride);

private:
    struct Impl; // Objective-C objects, defined in AvfCamera.mm
    CameraInfo m_info;
    Impl *m_impl = nullptr;
    std::vector<Resolution> m_resolutions; // guarded by m_mutex
    int m_resIndex = 0;                    // guarded by m_mutex
    std::atomic<bool> m_streaming{false};
    std::atomic<uint64_t> m_sequence{0};
    Range m_expRange{1, 1000};
    Range m_gainRange{1, 1};
    // read on the capture queue for every frame
    std::atomic<double> m_exposure{33};
    std::atomic<double> m_gain{1};
    bool m_hasExposure = false, m_hasGain = false;
    std::shared_ptr<FramePool> m_pool;
    mutable std::mutex m_mutex;
};

} // namespace lm
