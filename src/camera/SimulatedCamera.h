#pragma once
// Simulated bright-field microscope camera. Renders a procedural IHC-stained
// tissue section (haematoxylin + DAB) with realistic sensor effects: Bayer
// mosaic, 12-bit output, shot/read noise, vignetting, defocus and stage motion.

#include "camera/Camera.h"

#include <atomic>
#include <thread>

namespace lm {

class SimulatedCamera : public Camera {
public:
    SimulatedCamera();
    ~SimulatedCamera() override;

    CameraInfo info() const override;
    bool open(std::string &error) override;
    void close() override;
    bool isOpen() const override { return m_open; }

    bool startStreaming(std::string &error) override;
    void stopStreaming() override;
    bool isStreaming() const override { return m_streaming; }

    std::vector<Resolution> resolutions() const override;
    int resolutionIndex() const override { return m_resIndex; }
    bool setResolutionIndex(int index) override;

    Range exposureRange() const override { return {0.05, 2000.0}; }
    double exposure() const override { return m_exposure; }
    bool setExposure(double ms) override;

    Range gainRange() const override { return {1.0, 16.0}; }
    double gain() const override { return m_gain; }
    bool setGain(double g) override;

    std::vector<int> bitDepths() const override { return {8, 12}; }
    int bitDepth() const override { return m_bitDepth; }
    bool supportsHdr() const override { return true; } // linear raw, exact exposure per frame
    bool setBitDepth(int b) override;

    std::vector<CameraProperty> properties() const override;
    bool setProperty(const std::string &key, double value) override;

private:
    void run();
    void buildSlide();
    RawFramePtr renderFrame(uint64_t seq);

    std::atomic<bool> m_open{false};
    std::atomic<bool> m_streaming{false};
    std::thread m_thread;
    std::atomic<int> m_resIndex{0};
    std::atomic<double> m_exposure{20.0};
    std::atomic<double> m_gain{1.0};
    std::atomic<int> m_bitDepth{12};
    std::atomic<double> m_stageX{0.0}, m_stageY{0.0}; // slide position (pixels)
    std::atomic<double> m_focus{0.0};                 // defocus offset (arb. units)
    std::atomic<double> m_tilt{3.0};                  // section tilt -> depth variation
    std::atomic<double> m_lamp{1.0};                  // illumination intensity
    std::atomic<double> m_driftX{0.0}, m_driftY{0.0}; // automatic stage motion px/frame

    // stain densities (0..255 -> optical density) for the whole slide
    int m_slideW = 0, m_slideH = 0;
    std::vector<uint8_t> m_hema, m_dab, m_eosin;
    std::mutex m_slideMutex;
};

} // namespace lm
