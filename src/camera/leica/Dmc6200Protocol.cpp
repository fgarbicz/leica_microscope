#include "Dmc6200Protocol.h"

#include <algorithm>
#include <cstring>

namespace lm::dmc {

namespace {
constexpr uint8_t kEpCmdOut = 0x01;
constexpr uint8_t kEpCmdIn = 0x81;
constexpr uint8_t kEpEvent = 0x82;
constexpr uint8_t kEpImage = 0x83;
constexpr uint16_t kMagic = 0x7BBB;

void put16(std::vector<uint8_t> &v, uint16_t x)
{
    v.push_back(uint8_t(x));
    v.push_back(uint8_t(x >> 8));
}
void put32(std::vector<uint8_t> &v, uint32_t x)
{
    for (int i = 0; i < 4; ++i)
        v.push_back(uint8_t(x >> (8 * i)));
}
uint16_t get16(const uint8_t *d) { return uint16_t(d[0] | d[1] << 8); }
uint32_t get32(const uint8_t *d) { return uint32_t(d[0] | d[1] << 8 | d[2] << 16 | uint32_t(d[3]) << 24); }
} // namespace

bool FrameEvent::parse(const uint8_t *d, size_t n)
{
    if (n < 40)
        return false;
    std::memcpy(raw, d, std::min<size_t>(n, 64));
    type = get16(d);
    frameNumber = get32(d + 4);
    width = get16(d + 8);
    height = get16(d + 10);
    bytes = get32(d + 12);
    bits = d[16];
    bits2 = d[17];
    exposureUs = get32(d + 20);
    gainA = get16(d + 36);
    gainB = get16(d + 38);
    return type == 1 && width > 0 && height > 0 && bytes > 0;
}

Protocol::~Protocol()
{
    close();
}

std::vector<std::string> Protocol::findDevices()
{
    return usb::WinUsbDevice::find(kVendorId, kProductId);
}

bool Protocol::open(const std::string &path, std::string &error)
{
    m_dev = usb::WinUsbDevice::open(path, error);
    if (!m_dev)
        return false;
    for (uint8_t ep : {kEpCmdOut, kEpCmdIn, kEpEvent, kEpImage})
        m_dev->resetPipe(ep);
    m_dev->setAutoClearStall(kEpImage, true);
    m_dev->setAutoClearStall(kEpEvent, true);
    m_trailer.resize(1024);
    return true;
}

void Protocol::close()
{
    m_dev.reset();
}

bool Protocol::command(uint16_t cmd, const std::vector<uint8_t> &payload, uint32_t maxResponse,
                       std::vector<uint8_t> *response, uint16_t *status, unsigned timeoutMs)
{
    std::lock_guard<std::mutex> lock(m_cmdMutex);
    if (!m_dev) {
        m_error = "device not open";
        return false;
    }
    std::vector<uint8_t> req;
    req.reserve(12 + payload.size());
    put16(req, cmd);
    put16(req, uint16_t(payload.size()));
    put32(req, maxResponse);
    put32(req, 0);
    req.insert(req.end(), payload.begin(), payload.end());
    if (m_dev->write(kEpCmdOut, req.data(), req.size(), timeoutMs) != int(req.size())) {
        m_error = "command write failed: " + m_dev->lastErrorText();
        return false;
    }
    std::vector<uint8_t> resp(size_t(maxResponse) + 8 + 512);
    int n = m_dev->read(kEpCmdIn, resp.data(), resp.size(), timeoutMs);
    if (n < 8) {
        m_error = "command response failed: " + (n < 0 ? m_dev->lastErrorText() : std::string("short response"));
        return false;
    }
    const uint16_t rcmd = get16(resp.data()), plen = get16(resp.data() + 2), st = get16(resp.data() + 4),
                   magic = get16(resp.data() + 6);
    if (status)
        *status = st;
    if (rcmd != cmd || magic != kMagic) {
        m_error = "unexpected response header";
        return false;
    }
    if (response) {
        const size_t avail = std::min<size_t>(plen, size_t(n) - 8);
        response->assign(resp.begin() + 8, resp.begin() + 8 + avail);
    }
    if (st != 0) {
        m_error = "camera returned status 0x" + std::to_string(st);
        return false;
    }
    return true;
}

bool Protocol::readRegisters(const std::vector<uint32_t> &regs, std::vector<uint32_t> &values)
{
    std::vector<uint8_t> p;
    put32(p, uint32_t(regs.size()));
    for (auto r : regs)
        put32(p, r);
    std::vector<uint8_t> resp;
    if (!command(Cmd::ReadRegs, p, uint32_t(4 + 8 * regs.size()), &resp))
        return false;
    if (resp.size() < 4 + 8 * regs.size()) {
        m_error = "short register read";
        return false;
    }
    values.resize(regs.size());
    for (size_t i = 0; i < regs.size(); ++i)
        values[i] = get32(resp.data() + 4 + 8 * i + 4);
    return true;
}

bool Protocol::readRegister(uint32_t reg, uint32_t &value)
{
    std::vector<uint32_t> v;
    if (!readRegisters({reg}, v))
        return false;
    value = v[0];
    return true;
}

bool Protocol::writeRegisters(const std::vector<std::pair<uint32_t, uint32_t>> &rv)
{
    std::vector<uint8_t> p;
    put32(p, uint32_t(rv.size()));
    for (auto &[r, v] : rv) {
        put32(p, r);
        put32(p, v);
    }
    return command(Cmd::WriteRegs, p, 0, nullptr);
}

bool Protocol::uploadSequence(const std::vector<SequenceEntry> &entries)
{
    std::lock_guard<std::mutex> lock(m_cmdMutex);
    if (!m_dev)
        return false;
    // stage 1: announce a large command (flag 0x10000) and its total size
    std::vector<uint8_t> body;
    put16(body, Cmd::SequenceTable);
    put16(body, 0);
    put32(body, uint32_t(4 + entries.size() * 16));
    put32(body, uint32_t(entries.size()));
    for (const auto &e : entries) {
        put32(body, e.reg);
        put32(body, e.shot);
        put32(body, e.value);
        put32(body, 0);
    }
    std::vector<uint8_t> hdr;
    put16(hdr, Cmd::SequenceTable);
    put16(hdr, 0);
    put32(hdr, 0x00010000);
    put32(hdr, uint32_t(body.size()));
    if (m_dev->write(kEpCmdOut, hdr.data(), hdr.size(), 1000) != int(hdr.size())
        || m_dev->write(kEpCmdOut, body.data(), body.size(), 1000) != int(body.size())) {
        m_error = "sequence upload failed: " + m_dev->lastErrorText();
        return false;
    }
    uint8_t resp[64];
    int n = m_dev->read(kEpCmdIn, resp, sizeof(resp), 1000);
    if (n < 8 || get16(resp) != Cmd::SequenceTable || get16(resp + 6) != kMagic || get16(resp + 4) != 0) {
        m_error = "sequence upload not acknowledged";
        return false;
    }
    return true;
}

bool Protocol::acquisition(uint32_t mode)
{
    std::vector<uint8_t> p;
    put32(p, mode);
    return command(Cmd::Acquisition, p, 0, nullptr, nullptr, 3000);
}

bool Protocol::readInfoBlock(uint16_t cmd, uint32_t index, uint32_t maxLen, std::vector<uint8_t> &out)
{
    std::vector<uint8_t> p;
    put32(p, index);
    return command(cmd, p, maxLen, &out);
}

std::string Protocol::serial()
{
    std::vector<uint8_t> r;
    if (!command(Cmd::Serial, {}, 0x24, &r))
        return {};
    return std::string(reinterpret_cast<const char *>(r.data()), strnlen(reinterpret_cast<const char *>(r.data()), r.size()));
}

std::string Protocol::sensorName()
{
    std::vector<uint8_t> r;
    if (!readInfoBlock(Cmd::InfoBlock, 5, 0x40, r))
        return {};
    return std::string(reinterpret_cast<const char *>(r.data()), strnlen(reinterpret_cast<const char *>(r.data()), r.size()));
}

bool Protocol::waitFrameEvent(FrameEvent &ev, unsigned timeoutMs)
{
    uint8_t buf[64];
    int n = m_dev->read(kEpEvent, buf, sizeof(buf), timeoutMs);
    if (n <= 0)
        return false;
    return ev.parse(buf, size_t(n));
}

long long Protocol::readFrame(uint8_t *buf, size_t bytes, unsigned timeoutMs)
{
    // read the image in large chunks (WinUSB splits into bursts internally)
    size_t done = 0;
    while (done < bytes) {
        const size_t chunk = std::min<size_t>(bytes - done, 4u << 20);
        int n = m_dev->read(kEpImage, buf + done, chunk, timeoutMs);
        if (n <= 0) {
            m_error = "image read failed: " + (n < 0 ? m_dev->lastErrorText() : std::string("zero length"));
            return -1;
        }
        done += size_t(n);
        if (size_t(n) < chunk)
            break; // short packet: frame ended early
    }
    if (done == bytes) {
        // trailer (a single byte terminating the transfer)
        m_dev->read(kEpImage, m_trailer.data(), m_trailer.size(), 200);
    }
    return (long long)done;
}

void Protocol::abortStreaming()
{
    if (!m_dev)
        return;
    m_dev->abortPipe(kEpImage);
    m_dev->abortPipe(kEpEvent);
}

void Protocol::resetStreamPipes()
{
    if (!m_dev)
        return;
    m_dev->resetPipe(kEpImage);
    m_dev->resetPipe(kEpEvent);
}

} // namespace lm::dmc
