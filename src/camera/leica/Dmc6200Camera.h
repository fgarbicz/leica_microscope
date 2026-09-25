#pragma once
// Camera backend for the Leica DMC6200 using the native protocol driver.

#include "camera/Camera.h"
#include "camera/leica/Dmc6200Protocol.h"

#include <atomic>
#include <thread>

namespace lm {

class Dmc6200Backend : public CameraBackend {
public:
    std::string name() const override { return "Leica USB"; }
    std::vector<CameraInfo> enumerate() override;
    std::unique_ptr<Camera> create(const CameraInfo &info) override;
};

class Dmc6200Camera : public Camera {
public:
    explicit Dmc6200Camera(CameraInfo info);
    ~Dmc6200Camera() override;

    CameraInfo info() const override { return m_info; }
    bool open(std::string &error) override;
    void close() override;
    bool isOpen() const override { return m_proto.isOpen(); }

    bool startStreaming(std::string &error) override;
    void stopStreaming() override;
    bool isStreaming() const override { return m_streaming; }

    std::vector<Resolution> resolutions() const override;
    int resolutionIndex() const override { return m_resIndex; }
    bool setResolutionIndex(int index) override;

    Range exposureRange() const override { return {0.026, 60000.0}; }
    double exposure() const override { return m_exposureMs; }
    bool setExposure(double ms) override;

    Range gainRange() const override { return {1.0, 16.0}; }
    double gain() const override { return m_gain; }
    bool setGain(double g) override;

    std::vector<int> bitDepths() const override { return {12}; }
    int bitDepth() const override { return 12; }

    std::vector<CameraProperty> properties() const override;
    bool setProperty(const std::string &key, double value) override;

    std::vector<ShotMode> shotModes() const override;
    std::array<double, 9> colorMatrix() const override;
    std::string colorMatrixName() const override { return "IMX174, 3200 K halogen, Jenoptik calibration"; }
    bool captureShots(int modeIndex, std::vector<RawFramePtr> &shots, std::string &error,
                      const std::function<void(int, int)> &progress = {}) override;

    std::vector<std::pair<std::string, std::string>> details() const override;

private:
    struct Roi { int x, y, w, h; };
    bool configure(std::string &error);         // stop + registers + sequence
    bool startLiveLocked(std::string &error);
    void stopLiveLocked();
    void streamLoop();
    RawFramePtr readOneFrame(unsigned timeoutMs, dmc::FrameEvent *evOut = nullptr);
    std::vector<dmc::SequenceEntry> sequenceFor(const std::vector<std::pair<int, int>> &positions) const;

    CameraInfo m_info;
    dmc::Protocol m_proto;
    std::mutex m_ctrlMutex; // serialises start/stop/reconfigure
    std::thread m_thread;
    std::atomic<bool> m_streaming{false};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_threadDone{true};
    std::atomic<double> m_exposureMs{20.0};
    std::atomic<double> m_gain{1.0};
    int m_resIndex = 0;
    std::vector<Roi> m_rois;
    int m_sensorW = 1920, m_sensorH = 1200, m_adcBits = 12;
    int m_piezoRestX = 46, m_piezoRestY = 46;
    std::string m_serial, m_sensor, m_board;
    uint64_t m_seq = 0;
    std::shared_ptr<FramePool> m_pool = FramePool::create(12);
};

} // namespace lm
