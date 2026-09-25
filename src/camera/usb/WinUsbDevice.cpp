#include "WinUsbDevice.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <usbiodef.h>
#include <winusb.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "cfgmgr32.lib")

namespace lm::usb {

std::string win32ErrorText(unsigned long code)
{
    char *msg = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   code, 0, reinterpret_cast<char *>(&msg), 0, nullptr);
    std::string s = msg ? msg : "unknown error";
    if (msg)
        LocalFree(msg);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == '.'))
        s.pop_back();
    return s + " (" + std::to_string(code) + ")";
}

std::vector<std::string> WinUsbDevice::find(uint16_t vid, uint16_t pid)
{
    std::vector<std::string> out;
    char needle[32];
    std::snprintf(needle, sizeof(needle), "vid_%04x&pid_%04x", vid, pid);
    // Every USB device exposes GUID_DEVINTERFACE_USB_DEVICE; opening it with
    // WinUsb_Initialize only succeeds when WinUSB is the function driver.
    ULONG len = 0;
    GUID guid = GUID_DEVINTERFACE_USB_DEVICE;
    if (CM_Get_Device_Interface_List_SizeA(&len, &guid, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS
        || len == 0)
        return out;
    std::vector<char> buf(len);
    if (CM_Get_Device_Interface_ListA(&guid, nullptr, buf.data(), len, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS)
        return out;
    for (const char *p = buf.data(); *p; p += std::strlen(p) + 1) {
        std::string s(p), lower(p);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (lower.find(needle) != std::string::npos)
            out.push_back(s);
    }
    return out;
}

std::unique_ptr<WinUsbDevice> WinUsbDevice::open(const std::string &path, std::string &error)
{
    std::unique_ptr<WinUsbDevice> d(new WinUsbDevice());
    d->m_path = path;
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        error = "Cannot open device: " + win32ErrorText(GetLastError());
        return nullptr;
    }
    d->m_file = f;
    WINUSB_INTERFACE_HANDLE h = nullptr;
    if (!WinUsb_Initialize(f, &h)) {
        DWORD e = GetLastError();
        error = "WinUSB is not bound to this device (" + win32ErrorText(e)
                + "). Install driver/LeicaUsb3Cam.inf with driver/install_driver.ps1.";
        return nullptr;
    }
    d->m_winusb = h;

    USB_DEVICE_DESCRIPTOR dd{};
    ULONG got = 0;
    if (WinUsb_GetDescriptor(h, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0, reinterpret_cast<PUCHAR>(&dd), sizeof(dd), &got)) {
        d->m_desc.vid = dd.idVendor;
        d->m_desc.pid = dd.idProduct;
        d->m_desc.bcdUSB = dd.bcdUSB;
        d->m_desc.bcdDevice = dd.bcdDevice;
        if (dd.iManufacturer)
            d->m_desc.manufacturer = d->getString(dd.iManufacturer);
        if (dd.iProduct)
            d->m_desc.product = d->getString(dd.iProduct);
        if (dd.iSerialNumber)
            d->m_desc.serial = d->getString(dd.iSerialNumber);
    }
    UCHAR speed = 0;
    ULONG slen = sizeof(speed);
    if (WinUsb_QueryDeviceInformation(h, DEVICE_SPEED, &slen, &speed))
        d->m_desc.speed = speed;
    USB_INTERFACE_DESCRIPTOR id{};
    if (WinUsb_QueryInterfaceSettings(h, 0, &id)) {
        for (UCHAR i = 0; i < id.bNumEndpoints; ++i) {
            WINUSB_PIPE_INFORMATION pi{};
            if (WinUsb_QueryPipe(h, 0, i, &pi))
                d->m_endpoints.push_back({pi.PipeId, uint8_t(pi.PipeType), pi.MaximumPacketSize});
        }
    }
    return d;
}

WinUsbDevice::~WinUsbDevice()
{
    if (m_winusb)
        WinUsb_Free(m_winusb);
    if (m_file)
        CloseHandle(m_file);
}

std::string WinUsbDevice::lastErrorText() const
{
    return win32ErrorText(m_lastError);
}

int WinUsbDevice::control(uint8_t requestType, uint8_t request, uint16_t value, uint16_t index, void *data,
                          uint16_t length, unsigned timeoutMs)
{
    WINUSB_SETUP_PACKET sp{};
    sp.RequestType = requestType;
    sp.Request = request;
    sp.Value = value;
    sp.Index = index;
    sp.Length = length;
    ULONG tmo = timeoutMs;
    WinUsb_SetPipePolicy(m_winusb, 0, PIPE_TRANSFER_TIMEOUT, sizeof(tmo), &tmo);
    ULONG done = 0;
    if (!WinUsb_ControlTransfer(m_winusb, sp, static_cast<PUCHAR>(data), length, &done, nullptr)) {
        m_lastError = GetLastError();
        return -1;
    }
    return int(done);
}

namespace {
// Owns a manual-reset Win32 event (h is null if creation failed).
struct EventHandle {
    HANDLE h = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    EventHandle() = default;
    EventHandle(const EventHandle &) = delete;
    EventHandle &operator=(const EventHandle &) = delete;
    ~EventHandle()
    {
        if (h)
            CloseHandle(h);
    }
};

long long syncTransfer(void *h, uint8_t ep, void *data, size_t len, unsigned timeoutMs, bool isRead,
                       unsigned long &lastError)
{
    EventHandle event;
    if (!event.h) {
        lastError = GetLastError();
        return -1;
    }
    OVERLAPPED ov{};
    ov.hEvent = event.h;
    ULONG done = 0;
    BOOL ok = isRead ? WinUsb_ReadPipe(h, ep, static_cast<PUCHAR>(data), ULONG(len), &done, &ov)
                     : WinUsb_WritePipe(h, ep, static_cast<PUCHAR>(const_cast<void *>(data)), ULONG(len), &done, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
        lastError = GetLastError();
        return -1;
    }
    if (WaitForSingleObject(ov.hEvent, timeoutMs) != WAIT_OBJECT_0) {
        WinUsb_AbortPipe(h, ep);
        WinUsb_GetOverlappedResult(h, &ov, &done, TRUE);
        lastError = ERROR_SEM_TIMEOUT;
        return -1;
    }
    if (!WinUsb_GetOverlappedResult(h, &ov, &done, FALSE)) {
        lastError = GetLastError();
        return -1;
    }
    return done;
}
} // namespace

int WinUsbDevice::write(uint8_t ep, const void *data, size_t len, unsigned timeoutMs)
{
    return int(syncTransfer(m_winusb, ep, const_cast<void *>(data), len, timeoutMs, false, m_lastError));
}

int WinUsbDevice::read(uint8_t ep, void *data, size_t len, unsigned timeoutMs)
{
    return int(syncTransfer(m_winusb, ep, data, len, timeoutMs, true, m_lastError));
}

bool WinUsbDevice::setPipeTimeout(uint8_t ep, unsigned timeoutMs)
{
    ULONG v = timeoutMs;
    return WinUsb_SetPipePolicy(m_winusb, ep, PIPE_TRANSFER_TIMEOUT, sizeof(v), &v);
}

bool WinUsbDevice::setRawIo(uint8_t ep, bool on)
{
    UCHAR v = on;
    return WinUsb_SetPipePolicy(m_winusb, ep, RAW_IO, sizeof(v), &v);
}

bool WinUsbDevice::setAutoClearStall(uint8_t ep, bool on)
{
    UCHAR v = on;
    return WinUsb_SetPipePolicy(m_winusb, ep, AUTO_CLEAR_STALL, sizeof(v), &v);
}

bool WinUsbDevice::resetPipe(uint8_t ep) { return WinUsb_ResetPipe(m_winusb, ep); }
bool WinUsbDevice::abortPipe(uint8_t ep) { return WinUsb_AbortPipe(m_winusb, ep); }
bool WinUsbDevice::flushPipe(uint8_t ep) { return WinUsb_FlushPipe(m_winusb, ep); }

uint32_t WinUsbDevice::maxTransferSize(uint8_t ep) const
{
    ULONG v = 0, len = sizeof(v);
    WinUsb_GetPipePolicy(m_winusb, ep, MAXIMUM_TRANSFER_SIZE, &len, &v);
    return v;
}

bool WinUsbDevice::selectAltSetting(uint8_t alt)
{
    if (!WinUsb_SetCurrentAlternateSetting(m_winusb, alt)) {
        m_lastError = GetLastError();
        return false;
    }
    m_endpoints.clear();
    USB_INTERFACE_DESCRIPTOR id{};
    if (WinUsb_QueryInterfaceSettings(m_winusb, alt, &id)) {
        for (UCHAR i = 0; i < id.bNumEndpoints; ++i) {
            WINUSB_PIPE_INFORMATION pi{};
            if (WinUsb_QueryPipe(m_winusb, alt, i, &pi))
                m_endpoints.push_back({pi.PipeId, uint8_t(pi.PipeType), pi.MaximumPacketSize});
        }
    }
    return true;
}

std::vector<uint8_t> WinUsbDevice::getDescriptor(uint8_t type, uint8_t index, uint16_t lang, uint16_t length)
{
    std::vector<uint8_t> buf(length);
    ULONG got = 0;
    if (!WinUsb_GetDescriptor(m_winusb, type, index, lang, buf.data(), length, &got)) {
        m_lastError = GetLastError();
        return {};
    }
    buf.resize(got);
    return buf;
}

std::string WinUsbDevice::getString(uint8_t index, uint16_t lang)
{
    auto d = getDescriptor(USB_STRING_DESCRIPTOR_TYPE, index, lang, 255);
    if (d.size() < 2)
        return {};
    std::wstring w;
    for (size_t i = 2; i + 1 < d.size() && i < d[0]; i += 2)
        w.push_back(wchar_t(d[i] | (d[i + 1] << 8)));
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

} // namespace lm::usb
