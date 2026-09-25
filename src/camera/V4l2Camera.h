#pragma once
// Linux Video4Linux2 backend: any UVC camera (the counterpart of MFCamera on
// Windows and AvfCamera on macOS).

#include "camera/Camera.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace lm {

class V4l2Backend : public CameraBackend {
public:
    std::string name() const override { return "Video4Linux"; }
    std::vector<CameraInfo> enumerate() override;
    std::unique_ptr<Camera> create(const CameraInfo &info) override;
};

class V4l2Camera : public Camera {
public:
    explicit V4l2Camera(CameraInfo info);
    ~V4l2Camera() override;

    CameraInfo info() const override { return m_info; }
    bool open(std::string &error) override;
    void close() override;
    bool isOpen() const override { return m_fd >= 0; }

    bool startStreaming(std::string &error) override;
    void stopStreaming() override;
    bool isStreaming() const override { return m_streaming; }

    std::vector<Resolution> resolutions() const override { return m_resolutions; }
    int resolutionIndex() const override { return m_resIndex; }
    bool setResolutionIndex(int index) override;

    Range exposureRange() const override { return m_expRange; }
    double exposure() const override { return m_exposure; }
    bool setExposure(double ms) override;
    bool canSetExposure() const override { return m_hasExposure; }

    Range gainRange() const override { return m_gainRange; }
    double gain() const override { return m_gain; }
    bool setGain(double g) override;
    bool canSetGain() const override { return m_hasGain; }

    std::vector<CameraProperty> properties() const override;
    bool setProperty(const std::string &key, double value) override;
    bool deliversRaw() const override { return false; }

    std::vector<std::pair<std::string, std::string>> details() const override;

private:
    struct Mode {
        uint32_t fourcc = 0;
        int width = 0, height = 0;
    };
    struct Buffer {
        void *start = nullptr;
        size_t length = 0;
    };

    void run();
    bool configure(int index, std::string &error);
    bool mapBuffers(std::string &error);
    void unmapBuffers();
    void queryControls();
    bool controlRange(uint32_t id, double &min, double &max, double &step, double &value) const;

    CameraInfo m_info;
    int m_fd = -1;
    std::vector<Resolution> m_resolutions;
    std::vector<Mode> m_modes; // one per entry of m_resolutions
    std::vector<Buffer> m_buffers;
    int m_resIndex = 0;
    int m_outW = 0, m_outH = 0, m_outStride = 0;
    uint32_t m_fourcc = 0;
    std::atomic<bool> m_streaming{false};
    std::thread m_thread;
    Range m_expRange{1, 1000};
    Range m_gainRange{1, 1};
    double m_exposure = 33;
    double m_gain = 1;
    bool m_hasExposure = false, m_hasGain = false;
    double m_gainMinRaw = 0, m_gainMaxRaw = 0;
    std::string m_driver, m_card;
    mutable std::mutex m_mutex;
};

} // namespace lm
