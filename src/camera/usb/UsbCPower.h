#pragma once
// Power-cycling a USB-C port through the UCSI connector manager, so a camera
// that is stuck can be rebooted without unplugging it.
//
// Windows' UCSI driver (UcmUcsiCx) has a test interface that passes raw UCSI
// commands to the USB-C PD controller. It is off by default. Turning it on
// (the TestInterfaceEnabled registry value, then a driver restart) needs
// administrator rights; `dmctest --usbc-status` / `--usbc-cycle` use this
// module from an elevated prompt.
//
// A hard CONNECTOR_RESET can make the PD controller switch the port's VBUS
// off and on again, which a USB port reset or controller restart never does.
// The PD firmware does not always act on it (it recovered a stuck camera once
// but was ignored on a working one), so powerCycleCameraPort() watches the
// connector and only reports success when the port really went off and back.

#include <string>
#include <vector>

namespace lm::usbc {

struct ConnectorStatus {
    int number = 0;          // 1-based UCSI connector number
    bool connected = false;
    bool providingPower = false; // this computer supplies VBUS (partner is a sink)
    bool usbPartner = false;
    bool altModePartner = false; // display / dock alternate mode active
    int powerOperationMode = 0;
    int partnerType = 0;
};

// Reads the status of every connector. The test interface must be enabled.
bool queryConnectors(std::vector<ConnectorStatus> &out, std::string &error);

// The one connector that powers a plain USB device (no alternate mode, not
// charging this computer). Returns 0 and sets `error` when there is none, or
// more than one (then it cannot tell which one the camera is on).
int findPoweredUsbDeviceConnector(const std::vector<ConnectorStatus> &connectors, std::string &error);

// Safety check before a reset: the camera (VID 0x1711) must be attached to the
// USB-C (Type-C subsystem) controller and be the only USB device on it, so the one
// connector powering a USB device can only be the camera's. False (with a reason)
// otherwise, e.g. the camera is on a USB-A port or a USB-C drive is also attached.
bool cameraIsOnlyUsbCDevice(std::string &error);

// Hard-resets one connector (VBUS off/on). The test interface must be enabled.
bool resetConnector(int number, std::string &error);

// Turns the UCSI test interface on or off and restarts the UCSI driver so the
// change takes effect. Needs administrator rights.
bool setTestInterface(bool enabled, std::string &error);

// Complete sequence used by the elevated helper: enable the test interface,
// find the camera's connector, reset it, disable the test interface again.
// `log` receives a human readable transcript.
bool powerCycleCameraPort(std::string &log);

} // namespace lm::usbc
