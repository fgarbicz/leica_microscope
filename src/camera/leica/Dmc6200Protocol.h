#pragma once
// Native USB protocol driver for the Leica DMC6200 microscope camera
// (Jenoptik GRYPHAX platform, Cypress FX3, Sony IMX174 sensor), VID 0x1711
// PID 0x30E0. The protocol was reconstructed from USB traces; see
// docs/DMC6200_PROTOCOL.md.
//
// Transport (interface 0, WinUSB):
//   EP 0x01 bulk OUT  command channel
//   EP 0x81 bulk IN   command responses
//   EP 0x82 intr IN   64-byte "frame ready" events
//   EP 0x83 bulk IN   image data (16-bit little endian, 12 significant bits,
//                     Bayer GBRG), each frame followed by a 1-byte trailer
//
// Command request : u16 cmd, u16 payloadLen, u32 maxResponseLen, u32 0, payload
// Command response: u16 cmd, u16 payloadLen, u16 status, u16 0x7BBB, payload

#include "camera/usb/WinUsbDevice.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace lm::dmc {

constexpr uint16_t kVendorId = 0x1711;
constexpr uint16_t kProductId = 0x30E0;

namespace Cmd {
constexpr uint16_t MaxPacket = 0x0001;
constexpr uint16_t Capabilities = 0x0002;
constexpr uint16_t Serial = 0x0003;
constexpr uint16_t ImageInfo = 0x0010;
constexpr uint16_t InfoBlock = 0x0011;
constexpr uint16_t InfoBlock2 = 0x0012;
constexpr uint16_t Acquisition = 0x0020;
constexpr uint16_t WriteRegs = 0x0101;
constexpr uint16_t ReadRegs = 0x0102;
constexpr uint16_t SequenceTable = 0x0108;
constexpr uint16_t BoardInfo = 0x1005;
constexpr uint16_t FlashRead = 0x2000;
} // namespace Cmd

namespace Acq {
constexpr uint32_t Stop = 0;
constexpr uint32_t Live = 2;
constexpr uint32_t Flush = 3;
} // namespace Acq

namespace Reg {
constexpr uint32_t SensorWidth = 0x0001;
constexpr uint32_t SensorHeight = 0x0002;
constexpr uint32_t AdcBits = 0x0003;
constexpr uint32_t Reg0004 = 0x0004;
constexpr uint32_t Reg0005 = 0x0005;
constexpr uint32_t ExposureUs = 0x1012;
constexpr uint32_t Reg1013 = 0x1013;   // 0x10000 (16.16 fixed point 1.0)
constexpr uint32_t Reg1020 = 0x1020;
constexpr uint32_t RoiX = 0x1030;
constexpr uint32_t RoiY = 0x1031;
constexpr uint32_t RoiWidth = 0x1032;
constexpr uint32_t RoiHeight = 0x1033;
constexpr uint32_t ShotEnable = 0x1060;  // per sequence entry
constexpr uint32_t GainA = 0x1070;       // per sequence entry, 0..255
constexpr uint32_t GainB = 0x1071;       // per sequence entry, 0..255
constexpr uint32_t Reg1110 = 0x1110;
constexpr uint32_t Reg1111 = 0x1111;
constexpr uint32_t Reg1112 = 0x1112;
} // namespace Reg

// 64-byte frame announcement received on EP 0x82
struct FrameEvent {
    uint16_t type = 0;       // 1 = frame ready
    uint32_t frameNumber = 0;
    uint16_t width = 0, height = 0;
    uint32_t bytes = 0;
    uint8_t bits = 0, bits2 = 0;
    uint32_t exposureUs = 0;
    uint16_t gainA = 0, gainB = 0;
    uint8_t raw[64]{};
    bool parse(const uint8_t *d, size_t n);
};

struct SequenceEntry {
    uint32_t reg, shot, value;
};

class Protocol {
public:
    ~Protocol();

    static std::vector<std::string> findDevices();
    bool open(const std::string &path, std::string &error);
    void close();
    bool isOpen() const { return m_dev != nullptr; }

    // Generic command. Returns false on transport error or non-zero status.
    bool command(uint16_t cmd, const std::vector<uint8_t> &payload, uint32_t maxResponse,
                 std::vector<uint8_t> *response, uint16_t *status = nullptr, unsigned timeoutMs = 1000);

    bool readRegisters(const std::vector<uint32_t> &regs, std::vector<uint32_t> &values);
    bool readRegister(uint32_t reg, uint32_t &value);
    bool writeRegisters(const std::vector<std::pair<uint32_t, uint32_t>> &regValues);
    bool writeRegister(uint32_t reg, uint32_t value) { return writeRegisters({{reg, value}}); }
    bool uploadSequence(const std::vector<SequenceEntry> &entries);
    bool acquisition(uint32_t mode);

    bool readInfoBlock(uint16_t cmd, uint32_t index, uint32_t maxLen, std::vector<uint8_t> &out);
    std::string serial();
    std::string sensorName();

    // Streaming helpers
    bool waitFrameEvent(FrameEvent &ev, unsigned timeoutMs);
    // Reads one frame of `bytes` bytes (+ trailer) into buf. Returns bytes of image data.
    long long readFrame(uint8_t *buf, size_t bytes, unsigned timeoutMs);
    void abortStreaming();
    void resetStreamPipes();

    const std::string &lastError() const { return m_error; }
    usb::WinUsbDevice *device() { return m_dev.get(); }

private:
    std::unique_ptr<usb::WinUsbDevice> m_dev;
    std::mutex m_cmdMutex;
    std::string m_error;
    std::vector<uint8_t> m_trailer;
};

} // namespace lm::dmc
