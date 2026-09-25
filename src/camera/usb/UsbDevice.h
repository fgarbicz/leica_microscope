#pragma once
// Portable RAII wrapper around a vendor-class USB device.
//
// One implementation is compiled per platform, all behind the same API:
//   Windows  UsbDeviceWin.cpp     WinUSB (driver/LeicaUsb3Cam.inf, or any
//                                 WinUSB binding, e.g. from Zadig)
//   macOS    UsbDeviceLibusb.cpp  libusb-1.0 (no kernel driver needed for a
//                                 vendor-class interface)
//   Linux    UsbDeviceLibusb.cpp  libusb-1.0 (needs the udev rule in
//                                 driver/99-leica-dmc6200.rules for access
//                                 without root)
//
// The API is synchronous and blocking with per-transfer timeouts, which is what
// the DMC6200 protocol driver needs; the libusb backend implements that on top
// of libusb's asynchronous API so a timeout can cancel cleanly.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lm::usb {

struct DeviceDescriptorInfo {
    uint16_t vid = 0, pid = 0, bcdUSB = 0, bcdDevice = 0;
    std::string manufacturer, product, serial;
    int speed = 0; // 1 low, 2 full, 3 high, 4 super
};

struct EndpointInfo {
    uint8_t address = 0;
    uint8_t type = 0; // 0 control, 1 iso, 2 bulk, 3 interrupt
    uint16_t maxPacket = 0;
};

// One device found by find(): an opaque path used to open it plus what could be
// read about it without opening (the serial number is needed to tell two
// identical cameras apart in the UI).
struct DeviceId {
    std::string path;   // passed to open()
    std::string serial; // may be empty
};

class Device {
public:
    ~Device();

    // Lists the connected devices with the given VID/PID that this process can
    // open (on Windows: those bound to WinUSB).
    static std::vector<DeviceId> find(uint16_t vid, uint16_t pid);
    static std::unique_ptr<Device> open(const std::string &path, std::string &error);

    const DeviceDescriptorInfo &descriptor() const { return m_desc; }
    const std::vector<EndpointInfo> &endpoints() const { return m_endpoints; }
    const std::string &path() const { return m_path; }

    // Control transfer. Returns bytes transferred or -1 (lastError() set).
    int control(uint8_t requestType, uint8_t request, uint16_t value, uint16_t index, void *data, uint16_t length,
                unsigned timeoutMs = 1000);
    // Synchronous bulk/interrupt transfers. Return bytes or -1.
    int write(uint8_t ep, const void *data, size_t len, unsigned timeoutMs = 1000);
    int read(uint8_t ep, void *data, size_t len, unsigned timeoutMs = 1000);

    bool setPipeTimeout(uint8_t ep, unsigned timeoutMs);
    // WinUSB pipe policies. Where the platform has no equivalent these are
    // no-ops that report success: the libusb backend always behaves as if raw
    // I/O were on, and clears a stalled pipe itself.
    bool setRawIo(uint8_t ep, bool on);
    bool setAutoClearStall(uint8_t ep, bool on);
    bool resetPipe(uint8_t ep);
    bool abortPipe(uint8_t ep);
    bool flushPipe(uint8_t ep);
    uint32_t maxTransferSize(uint8_t ep) const;
    bool selectAltSetting(uint8_t alt);
    // USB port reset (re-enumerates the device; the handle is unusable
    // afterwards). Used to recover a camera that stopped responding.
    bool resetDevice();

    std::string getString(uint8_t index, uint16_t lang = 0x0409);
    std::vector<uint8_t> getDescriptor(uint8_t type, uint8_t index, uint16_t lang, uint16_t length);

    unsigned long lastError() const { return m_lastError; }
    std::string lastErrorText() const;

    // Whether this platform needs a driver to be installed/bound before the
    // camera can be opened (true on Windows only). The UI hides the driver
    // installation step where it does not apply.
    static bool needsDriverInstall();
    // One-line description of the USB backend, for the About box and logs.
    static std::string backendDescription();

private:
    Device() = default;
    void *m_file = nullptr;   // Windows: HANDLE           libusb: unused
    void *m_winusb = nullptr; // Windows: WINUSB_INTERFACE_HANDLE   libusb: libusb_device_handle *
    std::string m_path;
    DeviceDescriptorInfo m_desc;
    std::vector<EndpointInfo> m_endpoints;
    unsigned long m_lastError = 0;
    int m_claimedInterface = -1; // libusb
};

// Human readable text for a platform error code (Win32 error / libusb error).
std::string usbErrorText(unsigned long code);

} // namespace lm::usb
