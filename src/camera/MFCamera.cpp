#include "MFCamera.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <strmif.h>
#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")

namespace lm {

static const DWORD kVideoStream = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

namespace {

template <typename T>
void safeRelease(T *&p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

std::string narrow(const wchar_t *w)
{
    if (!w)
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(std::max(0, n - 1)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string &s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(std::max(0, n - 1)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

struct MFInit {
    MFInit()
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        MFStartup(MF_VERSION);
    }
};

void ensureMF()
{
    static MFInit init;
    // COM must be initialised on each thread that uses MF objects
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
}

std::string hrText(HRESULT hr)
{
    char s[16];
    std::snprintf(s, sizeof s, "0x%08lX", static_cast<unsigned long>(hr));
    return s;
}

// IAMCameraControl exposure is log2(seconds): value v => 2^v seconds.
double expToMs(long v) { return std::pow(2.0, double(v)) * 1000.0; }
long msToExp(double ms) { return long(std::lround(std::log2(std::max(ms, 0.01) / 1000.0))); }

} // namespace

std::vector<CameraInfo> MFBackend::enumerate()
{
    ensureMF();
    std::vector<CameraInfo> out;
    IMFAttributes *attr = nullptr;
    if (FAILED(MFCreateAttributes(&attr, 1)))
        return out;
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate **devices = nullptr;
    UINT32 count = 0;
    if (SUCCEEDED(MFEnumDeviceSources(attr, &devices, &count))) {
        for (UINT32 i = 0; i < count; ++i) {
            WCHAR *name = nullptr, *link = nullptr;
            UINT32 len = 0;
            devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
            devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &len);
            CameraInfo ci;
            ci.name = narrow(name);
            ci.id = "mf:" + narrow(link);
            ci.backend = "Media Foundation";
            ci.model = ci.name;
            out.push_back(ci);
            CoTaskMemFree(name);
            CoTaskMemFree(link);
            devices[i]->Release();
        }
        CoTaskMemFree(devices);
    }
    attr->Release();
    return out;
}

std::unique_ptr<Camera> MFBackend::create(const CameraInfo &info)
{
    return std::make_unique<MFCamera>(info);
}

MFCamera::MFCamera(CameraInfo info) : m_info(std::move(info)) {}

MFCamera::~MFCamera()
{
    close();
}

bool MFCamera::open(std::string &error)
{
    ensureMF();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_reader)
        return true;
    const std::string link = m_info.id.substr(3);
    IMFAttributes *attr = nullptr;
    if (FAILED(MFCreateAttributes(&attr, 2)) || !attr) {
        error = "Cannot open video device (MFCreateAttributes failed)";
        return false;
    }
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    attr->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, widen(link).c_str());
    HRESULT hr = MFCreateDeviceSource(attr, &m_source);
    attr->Release();
    if (FAILED(hr)) {
        error = "Cannot open video device (MFCreateDeviceSource failed, hr=" + hrText(hr) + ")";
        return false;
    }
    IMFAttributes *rattr = nullptr;
    hr = MFCreateAttributes(&rattr, 2);
    if (SUCCEEDED(hr) && rattr) {
        rattr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        rattr->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);
        hr = MFCreateSourceReaderFromMediaSource(m_source, rattr, &m_reader);
        rattr->Release();
    }
    if (FAILED(hr) || !m_reader) {
        m_source->Shutdown();
        safeRelease(m_source);
        error = "Cannot create source reader";
        return false;
    }

    // enumerate native types -> distinct resolutions, prefer uncompressed/high fps
    struct Cand { int w, h, idx; double fps; int pref; };
    std::map<std::pair<int, int>, Cand> best;
    for (DWORD i = 0;; ++i) {
        IMFMediaType *t = nullptr;
        if (FAILED(m_reader->GetNativeMediaType(kVideoStream, i, &t)))
            break;
        UINT32 w = 0, h = 0, num = 0, den = 1;
        MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(t, MF_MT_FRAME_RATE, &num, &den);
        GUID sub{};
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        int pref = sub == MFVideoFormat_RGB24 ? 5 : sub == MFVideoFormat_RGB32 ? 4 : sub == MFVideoFormat_YUY2 ? 3
                   : sub == MFVideoFormat_NV12 ? 2 : sub == MFVideoFormat_MJPG ? 1 : 0;
        double fps = den ? double(num) / den : 0;
        auto key = std::make_pair(int(w), int(h));
        auto it = best.find(key);
        if (it == best.end() || pref > it->second.pref || (pref == it->second.pref && fps > it->second.fps))
            best[key] = {int(w), int(h), int(i), fps, pref};
        t->Release();
    }
    std::vector<Cand> cands;
    for (auto &kv : best)
        cands.push_back(kv.second);
    std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) { return a.w * a.h > b.w * b.h; });
    m_resolutions.clear();
    m_typeIndex.clear();
    for (auto &c : cands) {
        Resolution r;
        r.width = c.w;
        r.height = c.h;
        r.label = std::to_string(c.w) + " x " + std::to_string(c.h) + " @ " + std::to_string(int(c.fps + 0.5)) + " fps";
        m_resolutions.push_back(r);
        m_typeIndex.push_back(c.idx);
    }
    if (m_resolutions.empty()) {
        releaseReader();
        error = "Device reports no video formats";
        return false;
    }
    m_resIndex = 0;
    if (!configureType(0, error)) {
        releaseReader(); // not left half open: isOpen() would report a camera that cannot stream
        return false;
    }
    queryControls();
    return true;
}

bool MFCamera::configureType(int index, std::string &error)
{
    IMFMediaType *native = nullptr;
    if (index < 0 || index >= int(m_typeIndex.size())
        || FAILED(m_reader->GetNativeMediaType(kVideoStream, DWORD(m_typeIndex[size_t(index)]), &native))) {
        error = "Cannot get media type";
        return false;
    }
    HRESULT hr = m_reader->SetCurrentMediaType(kVideoStream, nullptr, native);
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(native, MF_MT_FRAME_SIZE, &w, &h);
    native->Release();
    if (FAILED(hr)) {
        error = "The camera refused the video format (hr=" + hrText(hr) + ")";
        return false;
    }
    // request RGB32 output; the reader inserts decoders/converters as needed
    IMFMediaType *out = nullptr;
    if (FAILED(MFCreateMediaType(&out)) || !out) {
        error = "Cannot create the output media type";
        return false;
    }
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    MFSetAttributeSize(out, MF_MT_FRAME_SIZE, w, h);
    hr = m_reader->SetCurrentMediaType(kVideoStream, nullptr, out);
    out->Release();
    if (FAILED(hr)) {
        error = "Cannot configure RGB32 output (hr=" + hrText(hr) + ")";
        return false;
    }
    if (!readOutputFormat()) {
        error = "Cannot read the configured output format";
        return false;
    }
    return true;
}

bool MFCamera::readOutputFormat()
{
    IMFMediaType *cur = nullptr;
    if (FAILED(m_reader->GetCurrentMediaType(kVideoStream, &cur)) || !cur)
        return false;
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h);
    // The row pitch is not necessarily width * 4, and a negative value means the
    // rows are stored bottom-up. MF_MT_DEFAULT_STRIDE holds a signed value in a
    // UINT32; without it, the minimum stride of the format applies (top-down).
    LONG stride = 0;
    UINT32 attr = 0;
    if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &attr)))
        stride = LONG(INT32(attr));
    else if (FAILED(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, w, &stride)))
        stride = LONG(w) * 4;
    cur->Release();
    if (w == 0 || h == 0 || stride == 0 || std::abs(stride) < LONG(w) * 4)
        return false;
    m_outW = int(w);
    m_outH = int(h);
    m_outStride = int(stride);
    return true;
}

void MFCamera::releaseReader()
{
    safeRelease(m_reader);
    if (m_source) {
        m_source->Shutdown();
        safeRelease(m_source);
    }
}

void MFCamera::queryControls()
{
    IAMCameraControl *cc = nullptr;
    if (SUCCEEDED(m_source->QueryInterface(IID_PPV_ARGS(&cc)))) {
        long mn, mx, step, def, flags;
        if (SUCCEEDED(cc->GetRange(CameraControl_Exposure, &mn, &mx, &step, &def, &flags))) {
            m_hasExposure = true;
            m_expRange = {expToMs(mn), expToMs(mx)};
            long v, f;
            if (SUCCEEDED(cc->Get(CameraControl_Exposure, &v, &f)))
                m_exposure = expToMs(v);
        }
        cc->Release();
    }
    IAMVideoProcAmp *pa = nullptr;
    if (SUCCEEDED(m_source->QueryInterface(IID_PPV_ARGS(&pa)))) {
        long mn, mx, step, def, flags;
        if (SUCCEEDED(pa->GetRange(VideoProcAmp_Gain, &mn, &mx, &step, &def, &flags)) && mx > mn) {
            m_hasGain = true;
            m_gainMinRaw = mn;
            m_gainMaxRaw = mx;
            m_gainRange = {1.0, 1.0 + double(mx - mn) / std::max(1L, (mx - mn) / 8)};
            long v, f;
            if (SUCCEEDED(pa->Get(VideoProcAmp_Gain, &v, &f)))
                m_gain = 1.0 + double(v - mn) / std::max(1L, (mx - mn) / 8);
        }
        pa->Release();
    }
}

void MFCamera::close()
{
    stopStreaming();
    std::lock_guard<std::mutex> lock(m_mutex);
    releaseReader();
}

bool MFCamera::startStreaming(std::string &error)
{
    if (!m_reader) {
        error = "camera not open";
        return false;
    }
    if (m_streaming)
        return true;
    if (m_thread.joinable())
        m_thread.join();
    m_streaming = true;
    m_thread = std::thread([this] {
        try {
            run();
        } catch (...) {
            m_streaming = false;
            emitCurrentException("Video stream stopped: ");
        }
    });
    return true;
}

void MFCamera::stopStreaming()
{
    m_streaming = false;
    {
        // unblock a pending ReadSample
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_reader)
            m_reader->Flush(kVideoStream);
    }
    if (m_thread.joinable())
        m_thread.join();
}

bool MFCamera::setResolutionIndex(int index)
{
    if (index < 0 || index >= int(m_resolutions.size()))
        return false;
    const bool wasStreaming = m_streaming;
    stopStreaming();
    std::string err;
    bool ok = false, usable = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_reader)
            return false;
        ok = configureType(index, err);
        if (ok) {
            m_resIndex = index;
            usable = true;
        } else {
            // a failed change can leave the native type switched but no RGB32
            // output behind it: go back to the format that worked
            std::string restoreErr;
            usable = configureType(m_resIndex, restoreErr);
        }
    }
    if (!usable) {
        // nothing streams in this state; reported so the camera is reopened
        emitError("Cannot change the image format (" + err + "), and the previous format could not be restored");
        return false;
    }
    if (wasStreaming) {
        std::string startErr;
        if (!startStreaming(startErr)) {
            emitError("The live image did not restart after the format change: " + startErr);
            return false;
        }
    }
    return ok;
}

bool MFCamera::setExposure(double ms)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_hasExposure || !m_source)
        return false;
    IAMCameraControl *cc = nullptr;
    if (FAILED(m_source->QueryInterface(IID_PPV_ARGS(&cc))))
        return false;
    bool ok = SUCCEEDED(cc->Set(CameraControl_Exposure, msToExp(ms), CameraControl_Flags_Manual));
    cc->Release();
    if (ok)
        m_exposure = expToMs(msToExp(ms));
    return ok;
}

bool MFCamera::setGain(double g)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_hasGain || !m_source)
        return false;
    IAMVideoProcAmp *pa = nullptr;
    if (FAILED(m_source->QueryInterface(IID_PPV_ARGS(&pa))))
        return false;
    long raw = m_gainMinRaw + long(std::lround((g - 1.0) * std::max(1L, (m_gainMaxRaw - m_gainMinRaw) / 8)));
    raw = std::clamp(raw, m_gainMinRaw, m_gainMaxRaw);
    bool ok = SUCCEEDED(pa->Set(VideoProcAmp_Gain, raw, VideoProcAmp_Flags_Manual));
    pa->Release();
    if (ok)
        m_gain = g;
    return ok;
}

std::vector<CameraProperty> MFCamera::properties() const
{
    std::vector<CameraProperty> out;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_source)
        return out;
    IAMVideoProcAmp *pa = nullptr;
    if (SUCCEEDED(m_source->QueryInterface(IID_PPV_ARGS(&pa)))) {
        struct P { long id; const char *key, *label; };
        const P props[] = {{VideoProcAmp_Brightness, "brightness", "Brightness"},
                           {VideoProcAmp_Contrast, "contrast", "Contrast"},
                           {VideoProcAmp_Saturation, "saturation", "Saturation"},
                           {VideoProcAmp_Sharpness, "sharpness", "Sharpness"},
                           {VideoProcAmp_Gamma, "gamma", "Gamma"},
                           {VideoProcAmp_WhiteBalance, "whitebalance", "White balance (K)"},
                           {VideoProcAmp_BacklightCompensation, "backlight", "Backlight compensation"}};
        for (const auto &p : props) {
            long mn, mx, step, def, flags, v, f;
            if (SUCCEEDED(pa->GetRange(p.id, &mn, &mx, &step, &def, &flags)) && mx > mn
                && SUCCEEDED(pa->Get(p.id, &v, &f))) {
                CameraProperty cp;
                cp.key = p.key;
                cp.label = p.label;
                cp.min = mn;
                cp.max = mx;
                cp.step = std::max(1L, step);
                cp.value = v;
                out.push_back(cp);
            }
        }
        pa->Release();
    }
    return out;
}

bool MFCamera::setProperty(const std::string &key, double value)
{
    static const std::map<std::string, long> ids = {
        {"brightness", VideoProcAmp_Brightness}, {"contrast", VideoProcAmp_Contrast},
        {"saturation", VideoProcAmp_Saturation}, {"sharpness", VideoProcAmp_Sharpness},
        {"gamma", VideoProcAmp_Gamma},           {"whitebalance", VideoProcAmp_WhiteBalance},
        {"backlight", VideoProcAmp_BacklightCompensation}};
    auto it = ids.find(key);
    std::lock_guard<std::mutex> lock(m_mutex);
    if (it == ids.end() || !m_source)
        return false;
    IAMVideoProcAmp *pa = nullptr;
    if (FAILED(m_source->QueryInterface(IID_PPV_ARGS(&pa))))
        return false;
    bool ok = SUCCEEDED(pa->Set(it->second, long(std::lround(value)), VideoProcAmp_Flags_Manual));
    pa->Release();
    return ok;
}

void MFCamera::run()
{
    ensureMF();
    uint64_t seq = 0;
    int errors = 0;
    while (m_streaming) {
        DWORD streamIndex = 0, flags = 0;
        LONGLONG ts = 0;
        IMFSample *sample = nullptr;
        IMFSourceReader *reader = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_reader)
                break;
            reader = m_reader;
            reader->AddRef();
        }
        const HRESULT hr = reader->ReadSample(kVideoStream, 0, &streamIndex, &flags, &ts, &sample);
        reader->Release();
        if (!m_streaming) {
            safeRelease(sample);
            break;
        }
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) {
            safeRelease(sample);
            if (++errors > 20) {
                emitError("Video stream error (device disconnected?)");
                m_streaming = false;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        errors = 0;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            // no more samples will come: every further ReadSample would return
            // at once with this flag again
            safeRelease(sample);
            emitError("The camera stopped delivering video (end of stream; device disconnected?)");
            m_streaming = false;
            break;
        }
        int w, h, stride;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            // the driver may change the output format on its own
            if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) && m_reader)
                readOutputFormat();
            w = m_outW;
            h = m_outH;
            stride = m_outStride;
        }
        if (!sample)
            continue;
        IMFMediaBuffer *buf = nullptr;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf))) {
            const size_t row = size_t(w) * 4;
            auto copy = [&](const BYTE *scan0, LONG pitch) {
                // scan0 is the top row; a negative pitch walks up through memory
                auto f = std::make_shared<RawFrame>();
                f->width = w;
                f->height = h;
                f->format = PixelFormat::BGRA8;
                f->bitDepth = 8;
                f->stride = int(row);
                f->sequence = seq++;
                f->timestamp = std::chrono::steady_clock::now();
                f->exposureMs = m_exposure;
                f->gain = m_gain;
                f->data.resize(row * size_t(h));
                for (int y = 0; y < h; ++y)
                    std::memcpy(f->data.data() + size_t(y) * row, scan0 + ptrdiff_t(y) * pitch, row);
                emitFrame(f);
            };
            // A 2D buffer knows its real pitch and orientation; prefer it.
            // Lock2DSize also gives the buffer's extent, so a frame size that
            // no longer matches the buffer is dropped instead of read past it.
            IMF2DBuffer2 *buf2d = nullptr;
            if (SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&buf2d)))) {
                BYTE *scan0 = nullptr, *start = nullptr;
                LONG pitch = 0;
                DWORD len = 0;
                if (SUCCEEDED(buf2d->Lock2DSize(MF2DBuffer_LockFlags_Read, &scan0, &pitch, &start, &len))) {
                    if (scan0 && start && size_t(std::abs(pitch)) >= row && w > 0 && h > 0) {
                        // the first and last rows in memory, whichever way up
                        const BYTE *lo = pitch > 0 ? scan0 : scan0 + ptrdiff_t(pitch) * (h - 1);
                        const BYTE *hi = (pitch > 0 ? scan0 + ptrdiff_t(pitch) * (h - 1) : scan0) + row;
                        if (lo >= start && hi <= start + len)
                            copy(scan0, pitch);
                    }
                    buf2d->Unlock2D();
                }
                buf2d->Release();
            } else if (stride != 0) {
                BYTE *data = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(buf->Lock(&data, nullptr, &len))) {
                    const size_t pitch = size_t(std::abs(stride));
                    if (data && w > 0 && h > 0 && len >= pitch * size_t(h - 1) + row) {
                        // bottom-up: the top row is the last one in memory
                        const BYTE *scan0 = stride > 0 ? data : data + pitch * size_t(h - 1);
                        copy(scan0, LONG(stride));
                    }
                    buf->Unlock();
                }
            }
            buf->Release();
        }
        sample->Release();
    }
}

} // namespace lm
