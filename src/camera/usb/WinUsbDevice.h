#pragma once
// Thin RAII wrapper around WinUSB for vendor-class devices bound to
// driver/LeicaUsb3Cam.inf (or any WinUSB binding, e.g. from Zadig).

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

class WinUsbDevice {
public:
    ~WinUsbDevice();

    // Lists device interface paths bound to WinUSB with the given VID/PID.
    static std::vector<std::string> find(uint16_t vid, uint16_t pid);
    static std::unique_ptr<WinUsbDevice> open(const std::string &path, std::string &error);

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
    bool setRawIo(uint8_t ep, bool on);
    bool setAutoClearStall(uint8_t ep, bool on);
    bool resetPipe(uint8_t ep);
    bool abortPipe(uint8_t ep);
    bool flushPipe(uint8_t ep);
    uint32_t maxTransferSize(uint8_t ep) const;
    bool selectAltSetting(uint8_t alt);

    std::string getString(uint8_t index, uint16_t lang = 0x0409);
    std::vector<uint8_t> getDescriptor(uint8_t type, uint8_t index, uint16_t lang, uint16_t length);

    unsigned long lastError() const { return m_lastError; }
    std::string lastErrorText() const;

private:
    WinUsbDevice() = default;
    void *m_file = nullptr;     // HANDLE
    void *m_winusb = nullptr;   // WINUSB_INTERFACE_HANDLE
    std::string m_path;
    DeviceDescriptorInfo m_desc;
    std::vector<EndpointInfo> m_endpoints;
    unsigned long m_lastError = 0;
};

// Human readable Win32 error.
std::string win32ErrorText(unsigned long code);

} // namespace lm::usb
