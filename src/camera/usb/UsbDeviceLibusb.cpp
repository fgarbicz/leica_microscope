#include "UsbDevice.h"

#include <libusb.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sys/time.h>

// libusb backend for macOS and Linux.
//
// libusb's synchronous API (libusb_bulk_transfer) is built on the asynchronous
// one and handles events on the calling thread, which deadlocks as soon as two
// threads transfer at once — and this driver does exactly that (the image
// thread reads EP 0x83 while the UI thread sends commands on EP 0x01). So the
// transfers here are submitted asynchronously and waited for with libusb's
// event handling done under libusb_lock_events(), which is the documented
// multi-threaded pattern.

namespace lm::usb {

namespace {

constexpr int kInterface = 0;

libusb_context *context()
{
    // One context for the process, initialised on first use. libusb_exit is
    // deliberately not called: it would have to happen after every device is
    // closed, and the process is about to end anyway.
    static libusb_context *ctx = [] {
        libusb_context *c = nullptr;
        if (libusb_init(&c) != LIBUSB_SUCCESS)
            return static_cast<libusb_context *>(nullptr);
        return c;
    }();
    return ctx;
}

int speedFromLibusb(int s)
{
    switch (s) {
    case LIBUSB_SPEED_LOW:
        return 1;
    case LIBUSB_SPEED_FULL:
        return 2;
    case LIBUSB_SPEED_HIGH:
        return 3;
    case LIBUSB_SPEED_SUPER:
    case LIBUSB_SPEED_SUPER_PLUS:
        return 4;
    default:
        return 0;
    }
}

// The transfer type of one endpoint, from the interface descriptor read at
// open() (bulk if it is not known, which is what the data endpoints are).
uint8_t typeOfEndpoint(const std::vector<EndpointInfo> &eps, uint8_t ep)
{
    for (const auto &e : eps)
        if ((e.address & 0x7f) == (ep & 0x7f))
            return e.type;
    return 2;
}

uint8_t endpointType(uint8_t attributes)
{
    switch (attributes & LIBUSB_TRANSFER_TYPE_MASK) {
    case LIBUSB_TRANSFER_TYPE_CONTROL:
        return 0;
    case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS:
        return 1;
    case LIBUSB_TRANSFER_TYPE_BULK:
        return 2;
    default:
        return 3;
    }
}

// A device address is not stable across a replug, so the path carries the
// serial number as well and open() prefers a serial match.
std::string makePath(libusb_device *dev, const std::string &serial)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "usb:%03d.%03d", libusb_get_bus_number(dev), libusb_get_device_address(dev));
    return serial.empty() ? std::string(buf) : std::string(buf) + "#" + serial;
}

std::string serialFromPath(const std::string &path)
{
    const auto h = path.find('#');
    return h == std::string::npos ? std::string() : path.substr(h + 1);
}

std::string readStringDescriptor(libusb_device_handle *h, uint8_t index)
{
    if (!index)
        return {};
    unsigned char buf[256]{};
    const int n = libusb_get_string_descriptor_ascii(h, index, buf, sizeof(buf) - 1);
    return n > 0 ? std::string(reinterpret_cast<char *>(buf), size_t(n)) : std::string();
}

// One completed asynchronous transfer.
struct TransferState {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    int status = LIBUSB_TRANSFER_ERROR;
    int actual = 0;
};

void LIBUSB_CALL transferCallback(libusb_transfer *t)
{
    auto *st = static_cast<TransferState *>(t->user_data);
    {
        std::lock_guard<std::mutex> l(st->mutex);
        st->status = t->status;
        st->actual = t->actual_length;
        st->done = true;
    }
    st->cv.notify_all();
}

// Submits one bulk/interrupt transfer and waits up to timeoutMs for it.
// Returns the byte count, or -1 with `err` set to a libusb error code.
long long syncTransfer(libusb_device_handle *h, uint8_t ep, void *data, size_t len, unsigned timeoutMs, bool isRead,
                       unsigned long &err, uint8_t epType)
{
    if (!h) {
        err = static_cast<unsigned long>(-LIBUSB_ERROR_NO_DEVICE);
        return -1;
    }
    libusb_transfer *t = libusb_alloc_transfer(0);
    if (!t) {
        err = static_cast<unsigned long>(-LIBUSB_ERROR_NO_MEM);
        return -1;
    }
    TransferState st;
    const uint8_t address = isRead ? uint8_t(ep | LIBUSB_ENDPOINT_IN) : uint8_t(ep & 0x7f);
    // libusb's own timeout would leave the transfer in flight on some backends;
    // waiting here and cancelling explicitly keeps the pipe in a known state.
    if (epType == 3)
        libusb_fill_interrupt_transfer(t, h, address, static_cast<unsigned char *>(data), int(len), transferCallback,
                                       &st, 0);
    else
        libusb_fill_bulk_transfer(t, h, address, static_cast<unsigned char *>(data), int(len), transferCallback, &st, 0);

    const int rc = libusb_submit_transfer(t);
    if (rc != LIBUSB_SUCCESS) {
        libusb_free_transfer(t);
        err = static_cast<unsigned long>(-rc);
        return -1;
    }

    libusb_context *ctx = context();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    bool cancelled = false;
    for (;;) {
        {
            std::unique_lock<std::mutex> l(st.mutex);
            if (st.done)
                break;
        }
        // Only one thread may handle events; the others wait to be woken by it.
        if (libusb_try_lock_events(ctx) == 0) {
            bool stillWaiting;
            {
                std::unique_lock<std::mutex> l(st.mutex);
                stillWaiting = !st.done;
            }
            if (stillWaiting && libusb_event_handling_ok(ctx)) {
                timeval tv{0, 20000}; // 20 ms
                libusb_handle_events_locked(ctx, &tv);
            }
            libusb_unlock_events(ctx);
        } else {
            libusb_lock_event_waiters(ctx);
            std::unique_lock<std::mutex> l(st.mutex);
            const bool waiting = !st.done;
            l.unlock();
            if (waiting) {
                timeval tv{0, 20000};
                libusb_wait_for_event(ctx, &tv);
            }
            libusb_unlock_event_waiters(ctx);
        }
        if (!cancelled && std::chrono::steady_clock::now() >= deadline) {
            libusb_cancel_transfer(t);
            cancelled = true;
            // keep looping until the cancellation completes the transfer
        }
    }

    const int status = st.status;
    const int actual = st.actual;
    libusb_free_transfer(t);
    if (status == LIBUSB_TRANSFER_COMPLETED)
        return actual;
    if (status == LIBUSB_TRANSFER_CANCELLED || status == LIBUSB_TRANSFER_TIMED_OUT) {
        // A cancelled read may still have moved data (a short packet that
        // arrived as the timeout expired): that is a successful short read.
        if (actual > 0)
            return actual;
        err = static_cast<unsigned long>(-LIBUSB_ERROR_TIMEOUT);
        return -1;
    }
    if (status == LIBUSB_TRANSFER_STALL) {
        libusb_clear_halt(h, address); // "auto clear stall"
        err = static_cast<unsigned long>(-LIBUSB_ERROR_PIPE);
        return -1;
    }
    if (status == LIBUSB_TRANSFER_NO_DEVICE) {
        err = static_cast<unsigned long>(-LIBUSB_ERROR_NO_DEVICE);
        return -1;
    }
    if (status == LIBUSB_TRANSFER_OVERFLOW) {
        err = static_cast<unsigned long>(-LIBUSB_ERROR_OVERFLOW);
        return -1;
    }
    err = static_cast<unsigned long>(-LIBUSB_ERROR_IO);
    return -1;
}

} // namespace

std::string usbErrorText(unsigned long code)
{
    const int rc = -int(code);
    return std::string(libusb_strerror(static_cast<libusb_error>(rc))) + " (" + std::to_string(rc) + ")";
}

bool Device::needsDriverInstall()
{
    return false;
}

std::string Device::backendDescription()
{
    const libusb_version *v = libusb_get_version();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "libusb %d.%d.%d", v ? v->major : 0, v ? v->minor : 0, v ? v->micro : 0);
    return buf;
}

std::vector<DeviceId> Device::find(uint16_t vid, uint16_t pid)
{
    std::vector<DeviceId> out;
    libusb_context *ctx = context();
    if (!ctx)
        return out;
    libusb_device **list = nullptr;
    const ssize_t n = libusb_get_device_list(ctx, &list);
    if (n < 0)
        return out;
    for (ssize_t i = 0; i < n; ++i) {
        libusb_device_descriptor dd{};
        if (libusb_get_device_descriptor(list[i], &dd) != LIBUSB_SUCCESS)
            continue;
        if (dd.idVendor != vid || dd.idProduct != pid)
            continue;
        // Reading the serial needs the device open. Failing that (no
        // permission, or already claimed) it still gets listed, so open() can
        // report why it cannot be used.
        std::string serial;
        libusb_device_handle *h = nullptr;
        if (libusb_open(list[i], &h) == LIBUSB_SUCCESS && h) {
            serial = readStringDescriptor(h, dd.iSerialNumber);
            libusb_close(h);
        }
        out.push_back({makePath(list[i], serial), serial});
    }
    libusb_free_device_list(list, 1);
    return out;
}

std::unique_ptr<Device> Device::open(const std::string &path, std::string &error)
{
    libusb_context *ctx = context();
    if (!ctx) {
        error = "libusb could not be initialised";
        return nullptr;
    }
    int bus = -1, addr = -1;
    std::sscanf(path.c_str(), "usb:%d.%d", &bus, &addr);
    const std::string wantSerial = serialFromPath(path);

    libusb_device **list = nullptr;
    const ssize_t n = libusb_get_device_list(ctx, &list);
    if (n < 0) {
        error = "Cannot list USB devices: " + usbErrorText(static_cast<unsigned long>(-n));
        return nullptr;
    }
    // Prefer the serial number: bus/address change when the camera is replugged.
    libusb_device *chosen = nullptr;
    libusb_device *byAddress = nullptr;
    for (ssize_t i = 0; i < n && !chosen; ++i) {
        libusb_device_descriptor dd{};
        if (libusb_get_device_descriptor(list[i], &dd) != LIBUSB_SUCCESS)
            continue;
        if (libusb_get_bus_number(list[i]) == bus && libusb_get_device_address(list[i]) == addr)
            byAddress = list[i];
        if (!wantSerial.empty()) {
            libusb_device_handle *h = nullptr;
            if (libusb_open(list[i], &h) == LIBUSB_SUCCESS && h) {
                const bool match = readStringDescriptor(h, dd.iSerialNumber) == wantSerial;
                libusb_close(h);
                if (match)
                    chosen = list[i];
            }
        }
    }
    if (!chosen)
        chosen = byAddress;
    if (!chosen) {
        libusb_free_device_list(list, 1);
        error = "The camera is no longer connected (" + path + ")";
        return nullptr;
    }

    std::unique_ptr<Device> d(new Device());
    d->m_path = path;
    libusb_device_handle *h = nullptr;
    const int rc = libusb_open(chosen, &h);
    if (rc != LIBUSB_SUCCESS || !h) {
        libusb_free_device_list(list, 1);
        error = "Cannot open the camera: " + usbErrorText(static_cast<unsigned long>(-rc));
        if (rc == LIBUSB_ERROR_ACCESS)
#ifdef __APPLE__
            error += ". Another application (or a system process) is using the camera.";
#else
            error += ". Install the udev rule driver/99-leica-dmc6200.rules "
                     "(sudo cp driver/99-leica-dmc6200.rules /etc/udev/rules.d/ && sudo udevadm control --reload) "
                     "and replug the camera.";
#endif
        return nullptr;
    }
    d->m_winusb = h;

    libusb_device_descriptor dd{};
    if (libusb_get_device_descriptor(chosen, &dd) == LIBUSB_SUCCESS) {
        d->m_desc.vid = dd.idVendor;
        d->m_desc.pid = dd.idProduct;
        d->m_desc.bcdUSB = dd.bcdUSB;
        d->m_desc.bcdDevice = dd.bcdDevice;
        d->m_desc.manufacturer = readStringDescriptor(h, dd.iManufacturer);
        d->m_desc.product = readStringDescriptor(h, dd.iProduct);
        d->m_desc.serial = readStringDescriptor(h, dd.iSerialNumber);
    }
    d->m_desc.speed = speedFromLibusb(libusb_get_device_speed(chosen));

    // On Linux a kernel driver (usbfs aside) may hold the interface; detach it.
    libusb_set_auto_detach_kernel_driver(h, 1);
    const int claim = libusb_claim_interface(h, kInterface);
    if (claim != LIBUSB_SUCCESS) {
        libusb_free_device_list(list, 1);
        error = "Cannot claim the camera's USB interface: " + usbErrorText(static_cast<unsigned long>(-claim))
                + ". Close any other program using the camera.";
        return nullptr; // the destructor closes the handle
    }
    d->m_claimedInterface = kInterface;

    libusb_config_descriptor *cfg = nullptr;
    if (libusb_get_active_config_descriptor(chosen, &cfg) == LIBUSB_SUCCESS && cfg) {
        if (kInterface < cfg->bNumInterfaces) {
            const libusb_interface_descriptor &id = cfg->interface[kInterface].altsetting[0];
            for (uint8_t i = 0; i < id.bNumEndpoints; ++i) {
                const libusb_endpoint_descriptor &ep = id.endpoint[i];
                d->m_endpoints.push_back(
                    {ep.bEndpointAddress, endpointType(ep.bmAttributes), ep.wMaxPacketSize});
            }
        }
        libusb_free_config_descriptor(cfg);
    }
    libusb_free_device_list(list, 1);
    return d;
}

Device::~Device()
{
    auto *h = static_cast<libusb_device_handle *>(m_winusb);
    if (h) {
        if (m_claimedInterface >= 0)
            libusb_release_interface(h, m_claimedInterface);
        libusb_close(h);
    }
}

std::string Device::lastErrorText() const
{
    return usbErrorText(m_lastError);
}

int Device::control(uint8_t requestType, uint8_t request, uint16_t value, uint16_t index, void *data, uint16_t length,
                    unsigned timeoutMs)
{
    auto *h = static_cast<libusb_device_handle *>(m_winusb);
    if (!h) {
        m_lastError = static_cast<unsigned long>(-LIBUSB_ERROR_NO_DEVICE);
        return -1;
    }
    const int rc = libusb_control_transfer(h, requestType, request, value, index,
                                           static_cast<unsigned char *>(data), length, timeoutMs);
    if (rc < 0) {
        m_lastError = static_cast<unsigned long>(-rc);
        return -1;
    }
    return rc;
}

int Device::write(uint8_t ep, const void *data, size_t len, unsigned timeoutMs)
{
    return int(syncTransfer(static_cast<libusb_device_handle *>(m_winusb), ep, const_cast<void *>(data), len, timeoutMs,
                            false, m_lastError, typeOfEndpoint(m_endpoints, ep)));
}

int Device::read(uint8_t ep, void *data, size_t len, unsigned timeoutMs)
{
    return int(syncTransfer(static_cast<libusb_device_handle *>(m_winusb), ep, data, len, timeoutMs, true, m_lastError,
                            typeOfEndpoint(m_endpoints, ep)));
}

bool Device::setPipeTimeout(uint8_t, unsigned)
{
    return true; // every transfer carries its own timeout
}

bool Device::setRawIo(uint8_t, bool)
{
    return true; // libusb transfers are always "raw": no buffering layer
}

bool Device::setAutoClearStall(uint8_t, bool)
{
    return true; // syncTransfer() clears a halt itself
}

bool Device::resetPipe(uint8_t ep)
{
    auto *h = static_cast<libusb_device_handle *>(m_winusb);
    if (!h)
        return false;
    return libusb_clear_halt(h, ep) == LIBUSB_SUCCESS;
}

bool Device::abortPipe(uint8_t ep)
{
    // libusb cancels per transfer, and syncTransfer() already does that on its
    // own timeout, so there is nothing queued to abort here. Clearing a halt is
    // the useful part of what WinUsb_AbortPipe leaves behind.
    return resetPipe(ep);
}

bool Device::flushPipe(uint8_t)
{
    return true; // no host-side buffer to discard
}

uint32_t Device::maxTransferSize(uint8_t) const
{
    // libusb splits large bulk transfers itself; 4 MB matches the chunk size
    // the image reader uses.
    return 4u << 20;
}

bool Device::selectAltSetting(uint8_t alt)
{
    auto *h = static_cast<libusb_device_handle *>(m_winusb);
    if (!h)
        return false;
    const int rc = libusb_set_interface_alt_setting(h, kInterface, alt);
    if (rc != LIBUSB_SUCCESS) {
        m_lastError = static_cast<unsigned long>(-rc);
        return false;
    }
    return true;
}

bool Device::resetDevice()
{
    auto *h = static_cast<libusb_device_handle *>(m_winusb);
    if (!h)
        return false;
    const int rc = libusb_reset_device(h);
    if (rc != LIBUSB_SUCCESS && rc != LIBUSB_ERROR_NOT_FOUND) {
        m_lastError = static_cast<unsigned long>(-rc);
        return false;
    }
    return true; // NOT_FOUND: the device re-enumerated, which is a successful reset
}

std::vector<uint8_t> Device::getDescriptor(uint8_t type, uint8_t index, uint16_t lang, uint16_t length)
{
    std::vector<uint8_t> buf(length);
    const int n = control(LIBUSB_ENDPOINT_IN, LIBUSB_REQUEST_GET_DESCRIPTOR, uint16_t((type << 8) | index), lang,
                          buf.data(), length);
    if (n < 0)
        return {};
    buf.resize(size_t(n));
    return buf;
}

std::string Device::getString(uint8_t index, uint16_t)
{
    return readStringDescriptor(static_cast<libusb_device_handle *>(m_winusb), index);
}

} // namespace lm::usb
