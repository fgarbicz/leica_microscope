#include "camera/Camera.h"
#include "camera/SimulatedCamera.h"
#include "camera/leica/Dmc6200Camera.h"

// One UVC backend per platform; they expose the same controls, so the rest of
// the application (and the UI) does not know which one it is talking to.
#if defined(_WIN32)
#include "camera/MFCamera.h"
#elif defined(__APPLE__)
#include "camera/AvfCamera.h"
#else
#include "camera/V4l2Camera.h"
#endif

namespace lm {

namespace {
class SimBackend : public CameraBackend {
public:
    std::string name() const override { return "Simulator"; }
    std::vector<CameraInfo> enumerate() override { return {SimulatedCamera().info()}; }
    std::unique_ptr<Camera> create(const CameraInfo &) override { return std::make_unique<SimulatedCamera>(); }
};
} // namespace

std::vector<std::unique_ptr<CameraBackend>> createBackends()
{
    std::vector<std::unique_ptr<CameraBackend>> b;
    b.push_back(std::make_unique<Dmc6200Backend>()); // native Leica driver first
#if defined(_WIN32)
    b.push_back(std::make_unique<MFBackend>());
#elif defined(__APPLE__)
    b.push_back(std::make_unique<AvfBackend>());
#else
    b.push_back(std::make_unique<V4l2Backend>());
#endif
    b.push_back(std::make_unique<SimBackend>());
    return b;
}

} // namespace lm
