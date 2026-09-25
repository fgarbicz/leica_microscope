// Command line test for the native DMC6200 driver (no Leica/Jenoptik software).
#include "camera/leica/Dmc6200Protocol.h"

#include <chrono>
#include <cstdio>
#include <vector>

using namespace lm;
using Clock = std::chrono::steady_clock;

static std::vector<dmc::SequenceEntry> liveSequence(uint32_t gainA, uint32_t gainB)
{
    // same layout as the vendor SDK: 36 shots x (gainA, gainB, enable)
    std::vector<dmc::SequenceEntry> e;
    for (uint32_t i = 0; i < 36; ++i) {
        e.push_back({dmc::Reg::GainA, i, i == 0 ? gainA : 0});
        e.push_back({dmc::Reg::GainB, i, i == 0 ? gainB : 0});
        e.push_back({dmc::Reg::ShotEnable, i, i == 0 ? 1u : 0u});
    }
    return e;
}

int main(int argc, char **argv)
{
    const int frames = argc > 1 ? atoi(argv[1]) : 30;
    auto paths = dmc::Protocol::findDevices();
    if (paths.empty()) {
        printf("no DMC6200 found (is the WinUSB driver installed?)\n");
        return 1;
    }
    dmc::Protocol p;
    std::string err;
    if (!p.open(paths[0], err)) {
        printf("open failed: %s\n", err.c_str());
        return 1;
    }
    auto &d = p.device()->descriptor();
    printf("device: %s / %s / %s  speed=%d\n", d.manufacturer.c_str(), d.product.c_str(), d.serial.c_str(), d.speed);
    std::vector<uint8_t> r;
    p.command(dmc::Cmd::MaxPacket, {}, 4, &r);
    printf("serial: %s  sensor: %s\n", p.serial().c_str(), p.sensorName().c_str());
    std::vector<uint32_t> v;
    if (p.readRegisters({1, 2, 3, 4, 5, 0x1012, 0x1013, 0x1020, 0x1030, 0x1031, 0x1032, 0x1033}, v))
        printf("regs: w=%u h=%u bits=%u r4=%u r5=%x exp=%u r1013=%x r1020=%x roi=%u,%u %ux%u\n", v[0], v[1], v[2], v[3],
               v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11]);
    else
        printf("register read failed: %s\n", p.lastError().c_str());

    p.acquisition(dmc::Acq::Stop);
    p.acquisition(dmc::Acq::Flush);
    if (!p.writeRegister(dmc::Reg::ExposureUs, 20000))
        printf("exposure write failed: %s\n", p.lastError().c_str());
    if (!p.uploadSequence(liveSequence(46, 46)))
        printf("sequence upload failed: %s\n", p.lastError().c_str());
    if (!p.acquisition(dmc::Acq::Live)) {
        printf("start failed: %s\n", p.lastError().c_str());
        return 1;
    }
    std::vector<uint8_t> buf(1920 * 1200 * 2 + 4096);
    auto t0 = Clock::now();
    int got = 0;
    for (int i = 0; i < frames; ++i) {
        if (i == frames / 3) {
            bool ok = p.writeRegister(dmc::Reg::ExposureUs, 5000);
            printf("  -- live exposure write 5000us: %s\n", ok ? "ok" : p.lastError().c_str());
        }
        if (i == 2 * frames / 3) {
            bool ok = p.writeRegister(dmc::Reg::Reg1013, 0x20000);
            printf("  -- live write 0x1013=0x20000 (gain 2.0?): %s\n", ok ? "ok" : p.lastError().c_str());
        }
        dmc::FrameEvent ev;
        if (!p.waitFrameEvent(ev, 3000)) {
            printf("frame %d: no event (%s)\n", i, p.device()->lastErrorText().c_str());
            continue;
        }
        long long n = p.readFrame(buf.data(), ev.bytes, 3000);
        const uint16_t *px = reinterpret_cast<const uint16_t *>(buf.data());
        double mean = 0;
        for (size_t k = 0; k < 1920u * 1200u; k += 97)
            mean += px[k];
        mean /= (1920.0 * 1200.0 / 97.0);
        printf("frame #%u %ux%u bytes=%u read=%lld exp=%uus gains=%u,%u mean=%.1f\n", ev.frameNumber, ev.width,
               ev.height, ev.bytes, n, ev.exposureUs, ev.gainA, ev.gainB, mean);
        if (n == (long long)ev.bytes)
            ++got;
        if (i == 0) {
            FILE *f = fopen("dmc_frame0.raw", "wb");
            if (f) {
                fwrite(buf.data(), 1, ev.bytes, f);
                fclose(f);
            }
        }
    }
    double dt = std::chrono::duration<double>(Clock::now() - t0).count();
    printf("%d/%d frames in %.2fs = %.1f fps\n", got, frames, dt, got / dt);
    p.acquisition(dmc::Acq::Stop);
    p.acquisition(dmc::Acq::Flush);
    p.writeRegister(dmc::Reg::Reg1013, 0x10000);
    printf("restore sequence: %s\n", p.uploadSequence(liveSequence(46, 46)) ? "ok" : p.lastError().c_str());
    return 0;
}
