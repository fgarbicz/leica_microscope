#include "UsbCPower.h"

#include <windows.h>

#include <initguid.h>
#include <setupapi.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

namespace lm::usbc {

namespace {

// Test interfaces of the UCSI class extension (UcmUcsiCx), as used by
// Microsoft's UcsiControl.exe (MUTT package).
DEFINE_GUID(kUcsiTestInterfaceA, 0x0d3bc324, 0x0125, 0x4e95, 0xb2, 0x5a, 0x7b, 0x39, 0x33, 0x33, 0xea, 0x9a);
DEFINE_GUID(kUcsiTestInterfaceB, 0x6c846eea, 0x9649, 0x46b3, 0x9c, 0x37, 0x25, 0x56, 0x13, 0x3e, 0x50, 0x06);
constexpr DWORD kIoctlSendCommand = 0x12131018; // in/out: UCSI data block

constexpr wchar_t kUcsiHardwarePrefix[] = L"ACPI\\USBC000";

// UCSI data structure (UCSI specification, section 3)
#pragma pack(push, 1)
struct UcsiDataBlock {
    uint16_t version;
    uint16_t reserved;
    uint32_t cci;
    uint64_t control;
    uint8_t messageIn[16];
    uint8_t messageOut[16];
};
#pragma pack(pop)
static_assert(sizeof(UcsiDataBlock) == 48, "UCSI data block layout");

constexpr uint8_t kCmdConnectorReset = 0x03;
constexpr uint8_t kCmdGetCapability = 0x06;
constexpr uint8_t kCmdGetConnectorStatus = 0x12;
constexpr uint32_t kCciError = 1u << 30;
constexpr uint32_t kCciCommandCompleted = 1u << 31;
constexpr uint32_t kCciNotSupported = 1u << 25;

std::string lastErrorText(const char *what)
{
    const DWORD e = GetLastError();
    char buf[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, buf, sizeof buf, nullptr);
    std::string s = std::string(what) + ": " + buf;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s + " (" + std::to_string(e) + ")";
}

struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~Handle()
    {
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
};

HANDLE openTestInterface(std::string &error)
{
    for (const GUID *g : {&kUcsiTestInterfaceA, &kUcsiTestInterfaceB}) {
        HDEVINFO set = SetupDiGetClassDevsW(g, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set == INVALID_HANDLE_VALUE)
            continue;
        SP_DEVICE_INTERFACE_DATA ifd{sizeof(ifd)};
        HANDLE h = INVALID_HANDLE_VALUE;
        if (SetupDiEnumDeviceInterfaces(set, nullptr, g, 0, &ifd)) {
            DWORD need = 0;
            SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
            std::vector<uint8_t> buf(need + sizeof(wchar_t));
            auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buf.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, need, nullptr, nullptr))
                h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE)
                error = lastErrorText("cannot open the UCSI test interface");
        }
        SetupDiDestroyDeviceInfoList(set);
        if (h != INVALID_HANDLE_VALUE)
            return h;
    }
    if (error.empty())
        error = "the UCSI test interface is not available (not enabled, or no USB-C connector manager)";
    return INVALID_HANDLE_VALUE;
}

bool sendCommand(HANDLE h, uint64_t control, UcsiDataBlock &out, std::string &error)
{
    UcsiDataBlock in{};
    in.control = control;
    std::memset(&out, 0, sizeof out);
    DWORD got = 0;
    if (!DeviceIoControl(h, kIoctlSendCommand, &in, sizeof in, &out, sizeof out, &got, nullptr)) {
        error = lastErrorText("UCSI command failed");
        return false;
    }
    if (!(out.cci & kCciCommandCompleted) || (out.cci & (kCciError | kCciNotSupported))) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "UCSI command 0x%02x rejected by the PD controller (CCI 0x%08x)",
                      unsigned(control & 0xff), out.cci);
        error = buf;
        return false;
    }
    return true;
}

// Runs fn(set, devInfo) for the UCSI connector manager device node.
template <typename Fn>
bool withUcsiDevice(std::string &error, Fn &&fn)
{
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"ACPI", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) {
        error = lastErrorText("cannot enumerate ACPI devices");
        return false;
    }
    bool found = false, ok = false;
    SP_DEVINFO_DATA dev{sizeof(dev)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        wchar_t id[512] = {};
        if (!SetupDiGetDeviceInstanceIdW(set, &dev, id, 512, nullptr))
            continue;
        if (_wcsnicmp(id, kUcsiHardwarePrefix, wcslen(kUcsiHardwarePrefix)) != 0)
            continue;
        found = true;
        ok = fn(set, dev);
        break;
    }
    SetupDiDestroyDeviceInfoList(set);
    if (!found)
        error = "no USB-C connector manager (UCSI) found on this computer";
    return found && ok;
}

} // namespace

bool queryConnectors(std::vector<ConnectorStatus> &out, std::string &error)
{
    out.clear();
    Handle h;
    h.h = openTestInterface(error);
    if (h.h == INVALID_HANDLE_VALUE)
        return false;
    UcsiDataBlock r;
    if (!sendCommand(h.h, kCmdGetCapability, r, error))
        return false;
    const int count = std::min<int>(r.messageIn[4], 8); // bNumConnectors
    for (int n = 1; n <= count; ++n) {
        if (!sendCommand(h.h, kCmdGetConnectorStatus | (uint64_t(n) << 16), r, error))
            return false;
        // bytes 2..3: operation mode (3 bits), connect status, power direction,
        // partner flags (8 bits), partner type (3 bits)
        const uint16_t w = uint16_t(r.messageIn[2] | (r.messageIn[3] << 8));
        ConnectorStatus s;
        s.number = n;
        s.powerOperationMode = w & 7;
        s.connected = (w >> 3) & 1;
        s.providingPower = (w >> 4) & 1;
        const int flags = (w >> 5) & 0xff;
        s.usbPartner = flags & 1;
        s.altModePartner = (flags >> 1) & 1;
        s.partnerType = (w >> 13) & 7;
        out.push_back(s);
    }
    return true;
}

int findPoweredUsbDeviceConnector(const std::vector<ConnectorStatus> &connectors, std::string &error)
{
    int found = 0, count = 0;
    for (const auto &c : connectors)
        if (c.connected && c.providingPower && c.usbPartner && !c.altModePartner) {
            found = c.number;
            ++count;
        }
    if (count == 0) {
        error = "no USB-C port is powering a USB device (is the camera connected through USB-C?)";
        return 0;
    }
    if (count > 1) {
        error = "more than one USB-C port powers a USB device, so the camera's port is ambiguous; "
                "unplug the other USB devices or power-cycle the camera by hand";
        return 0;
    }
    return found;
}

bool resetConnector(int number, std::string &error)
{
    Handle h;
    h.h = openTestInterface(error);
    if (h.h == INVALID_HANDLE_VALUE)
        return false;
    UcsiDataBlock r;
    // bits 16..22 connector number, bit 23 hard reset
    return sendCommand(h.h, kCmdConnectorReset | (uint64_t(number) << 16) | (1ull << 23), r, error);
}

bool setTestInterface(bool enabled, std::string &error)
{
    return withUcsiDevice(error, [&](HDEVINFO set, SP_DEVINFO_DATA &dev) {
        HKEY key = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_SET_VALUE | KEY_QUERY_VALUE);
        if (key == INVALID_HANDLE_VALUE) {
            error = lastErrorText("cannot open the UCSI device parameters (administrator rights needed)");
            return false;
        }
        LSTATUS st;
        if (enabled) {
            const DWORD one = 1;
            st = RegSetValueExW(key, L"TestInterfaceEnabled", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&one),
                                sizeof one);
        } else {
            st = RegDeleteValueW(key, L"TestInterfaceEnabled");
            if (st == ERROR_FILE_NOT_FOUND)
                st = ERROR_SUCCESS;
        }
        RegCloseKey(key);
        if (st != ERROR_SUCCESS) {
            SetLastError(DWORD(st));
            error = lastErrorText("cannot change TestInterfaceEnabled");
            return false;
        }
        // restart the driver so it reads the value (like "pnputil /restart-device")
        SP_PROPCHANGE_PARAMS pc{};
        pc.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
        pc.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
        pc.StateChange = DICS_PROPCHANGE;
        pc.Scope = DICS_FLAG_CONFIGSPECIFIC;
        if (!SetupDiSetClassInstallParamsW(set, &dev, &pc.ClassInstallHeader, sizeof pc)
            || !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &dev)) {
            error = lastErrorText("cannot restart the UCSI driver");
            return false;
        }
        return true;
    });
}

bool powerCycleCameraPort(std::string &log)
{
    auto line = [&](const std::string &s) { log += s + "\n"; };
    std::string err;
    if (!setTestInterface(true, err)) {
        line(err);
        return false;
    }
    line("UCSI test interface enabled");
    bool ok = false;
    // the interface appears once the restarted driver is running
    std::vector<ConnectorStatus> conns;
    for (int i = 0; i < 50; ++i) {
        err.clear();
        if (queryConnectors(conns, err))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (conns.empty()) {
        line(err);
    } else {
        for (const auto &c : conns)
            line("connector " + std::to_string(c.number) + ": " + (c.connected ? "connected" : "empty")
                 + (c.connected ? (c.providingPower ? ", powered by this computer" : ", charging this computer") : "")
                 + (c.usbPartner ? ", USB" : "") + (c.altModePartner ? ", alternate mode" : ""));
        const int n = findPoweredUsbDeviceConnector(conns, err);
        if (!n) {
            line(err);
        } else if (!resetConnector(n, err)) {
            line(err);
        } else {
            line("connector " + std::to_string(n) + " reset requested");
            // The PD controller carries the reset out asynchronously. Watch the
            // connector drop and come back; restarting the UCSI driver earlier
            // (to disable the test interface) cancels the reset.
            using Clock = std::chrono::steady_clock;
            const auto t0 = Clock::now();
            bool dropped = false;
            while (Clock::now() - t0 < std::chrono::seconds(30)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                std::vector<ConnectorStatus> now;
                std::string qerr;
                if (!queryConnectors(now, qerr) || int(now.size()) < n)
                    continue;
                const bool connected = now[size_t(n - 1)].connected;
                const int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());
                if (!connected && !dropped) {
                    dropped = true;
                    line("port power off after " + std::to_string(ms) + " ms");
                } else if (connected && dropped) {
                    line("device attached again after " + std::to_string(ms) + " ms");
                    ok = true;
                    break;
                }
            }
            if (!ok)
                line(dropped ? "the device did not come back within 30 s"
                             : "the PD controller did not switch the port off");
        }
    }
    err.clear();
    if (setTestInterface(false, err))
        line("UCSI test interface disabled");
    else
        line(err);
    return ok;
}

} // namespace lm::usbc
