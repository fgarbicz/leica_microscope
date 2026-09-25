#include "camera/Camera.h"
#include "camera/MFCamera.h"
#include "camera/SimulatedCamera.h"
#include "camera/leica/Dmc6200Camera.h"

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
    b.push_back(std::make_unique<MFBackend>());
    b.push_back(std::make_unique<SimBackend>());
    return b;
}

} // namespace lm
