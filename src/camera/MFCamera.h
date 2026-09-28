#pragma once
// Windows Media Foundation backend: any UVC camera or vendor camera that ships
// a WDM/AVStream driver (as many Leica "HD" cameras do).

#include "camera/Camera.h"

#include <atomic>
#include <mutex>
#include <thread>

struct IMFSourceReader;
struct IMFMediaSource;

namespace lm {

class MFBackend : public CameraBackend {
public:
    std::string name() const override { return "Media Foundation"; }
    std::vector<CameraInfo> enumerate() override;
    std::unique_ptr<Camera> create(const CameraInfo &info) override;
};

class MFCamera : public Camera {
public:
    explicit MFCamera(CameraInfo info);
    ~MFCamera() override;

    CameraInfo info() const override { return m_info; }
    bool open(std::string &error) override;
    void close() override;
    bool isOpen() const override { return m_reader != nullptr; }

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

private:
    void run();
    bool configureType(int index, std::string &error);
    // reads size and row pitch of the reader's current output type (m_mutex held)
    bool readOutputFormat();
    void releaseReader(); // m_mutex held
    void queryControls();

    CameraInfo m_info;
    IMFMediaSource *m_source = nullptr;
    IMFSourceReader *m_reader = nullptr;
    std::vector<Resolution> m_resolutions;
    std::vector<int> m_typeIndex; // native media type index per resolution
    int m_resIndex = 0;
    int m_outW = 0, m_outH = 0;
    int m_outStride = 0; // bytes per row of the output buffers; negative = bottom-up
    std::atomic<bool> m_streaming{false};
    std::thread m_thread;
    Range m_expRange{1, 1000};
    Range m_gainRange{1, 1};
    // read on the capture thread for every frame
    std::atomic<double> m_exposure{33};
    std::atomic<double> m_gain{1};
    bool m_hasExposure = false, m_hasGain = false;
    long m_gainMinRaw = 0, m_gainMaxRaw = 0;
    mutable std::mutex m_mutex;
};

} // namespace lm
