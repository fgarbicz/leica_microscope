#include "AvfCamera.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

// AVFoundation video capture. The session is configured to deliver 32-bit BGRA
// so the imaging pipeline sees the same format as the Media Foundation backend
// on Windows.
//
// Camera access needs the NSCameraUsageDescription key in the bundle's
// Info.plist (see resources/macos/Info.plist.in); without it macOS terminates
// the process on the first capture attempt.

@interface LmAvfDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property(nonatomic, assign) lm::AvfCamera *owner;
@end

@implementation LmAvfDelegate

- (void)captureOutput:(AVCaptureOutput *)output
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
           fromConnection:(AVCaptureConnection *)connection
{
    (void)output;
    (void)connection;
    if (!self.owner)
        return;
    CVImageBufferRef image = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!image)
        return;
    if (CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
        return;
    const void *base = CVPixelBufferGetBaseAddress(image);
    const int w = int(CVPixelBufferGetWidth(image));
    const int h = int(CVPixelBufferGetHeight(image));
    const int stride = int(CVPixelBufferGetBytesPerRow(image));
    if (base && w > 0 && h > 0)
        self.owner->deliverFrame(base, w, h, stride);
    CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
}

- (void)captureOutput:(AVCaptureOutput *)output
    didDropSampleBuffer:(CMSampleBufferRef)sampleBuffer
         fromConnection:(AVCaptureConnection *)connection
{
    (void)output;
    (void)sampleBuffer;
    (void)connection;
    // dropped frames are expected when the display cannot keep up
}

@end

namespace lm {

struct AvfCamera::Impl {
    AVCaptureDevice *device = nil;
    AVCaptureSession *session = nil;
    AVCaptureDeviceInput *input = nil;
    AVCaptureVideoDataOutput *output = nil;
    LmAvfDelegate *delegate = nil;
    dispatch_queue_t queue = nil;
    std::vector<AVCaptureDeviceFormat *> formats; // one per entry of m_resolutions
};

namespace {

std::string toStd(NSString *s)
{
    return s ? std::string([s UTF8String]) : std::string();
}

// Every video capture device macOS offers, newest API first.
NSArray<AVCaptureDevice *> *videoDevices()
{
    if (@available(macOS 10.15, *)) {
        NSMutableArray<AVCaptureDeviceType> *types =
            [NSMutableArray arrayWithObject:AVCaptureDeviceTypeBuiltInWideAngleCamera];
        if (@available(macOS 14.0, *)) {
            [types addObject:AVCaptureDeviceTypeExternal];
        } else {
            // the pre-14 name for the same thing; deprecated, not replaceable here
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            [types addObject:AVCaptureDeviceTypeExternalUnknown];
#pragma clang diagnostic pop
        }
        AVCaptureDeviceDiscoverySession *s =
            [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:types
                                                                  mediaType:AVMediaTypeVideo
                                                                   position:AVCaptureDevicePositionUnspecified];
        return s.devices;
    }
    return @[];
}

} // namespace

std::vector<CameraInfo> AvfBackend::enumerate()
{
    std::vector<CameraInfo> out;
    @autoreleasepool {
        for (AVCaptureDevice *d in videoDevices()) {
            CameraInfo ci;
            ci.id = toStd(d.uniqueID);
            ci.name = toStd(d.localizedName);
            ci.model = toStd(d.modelID);
            ci.backend = name();
            out.push_back(ci);
        }
    }
    return out;
}

std::unique_ptr<Camera> AvfBackend::create(const CameraInfo &info)
{
    return std::make_unique<AvfCamera>(info);
}

AvfCamera::AvfCamera(CameraInfo info) : m_info(std::move(info)), m_pool(FramePool::create()) {}

AvfCamera::~AvfCamera()
{
    close();
}

bool AvfCamera::open(std::string &error)
{
    if (m_impl)
        return true;
    @autoreleasepool {
        AVCaptureDevice *device = nil;
        for (AVCaptureDevice *d in videoDevices())
            if (toStd(d.uniqueID) == m_info.id) {
                device = d;
                break;
            }
        if (!device) {
            error = "The camera is no longer connected";
            return false;
        }
        // macOS asks the user once; a denial must not look like a device fault.
        if ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo] == AVAuthorizationStatusDenied) {
            error = "Camera access is denied for DM Imaging. Allow it in System Settings > Privacy & Security > Camera.";
            return false;
        }

        auto impl = std::make_unique<Impl>();
        impl->device = device;
        impl->session = [[AVCaptureSession alloc] init];
        NSError *nsError = nil;
        impl->input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&nsError];
        if (!impl->input || nsError) {
            error = "Cannot open the camera: " + toStd(nsError.localizedDescription);
            return false;
        }
        if (![impl->session canAddInput:impl->input]) {
            error = "The camera cannot be added to a capture session (in use by another application?)";
            return false;
        }
        [impl->session addInput:impl->input];

        impl->output = [[AVCaptureVideoDataOutput alloc] init];
        impl->output.alwaysDiscardsLateVideoFrames = YES;
        impl->output.videoSettings = @{
            (NSString *)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)
        };
        impl->delegate = [[LmAvfDelegate alloc] init];
        impl->delegate.owner = this;
        impl->queue = dispatch_queue_create("com.dmimaging.capture", DISPATCH_QUEUE_SERIAL);
        [impl->output setSampleBufferDelegate:impl->delegate queue:impl->queue];
        if (![impl->session canAddOutput:impl->output]) {
            error = "The camera does not support video data output";
            return false;
        }
        [impl->session addOutput:impl->output];

        // resolutions, largest first
        std::vector<AVCaptureDeviceFormat *> formats;
        for (AVCaptureDeviceFormat *f in device.formats)
            if (CMFormatDescriptionGetMediaType(f.formatDescription) == kCMMediaType_Video)
                formats.push_back(f);
        std::sort(formats.begin(), formats.end(), [](AVCaptureDeviceFormat *a, AVCaptureDeviceFormat *b) {
            const CMVideoDimensions da = CMVideoFormatDescriptionGetDimensions(a.formatDescription);
            const CMVideoDimensions db = CMVideoFormatDescriptionGetDimensions(b.formatDescription);
            return 1LL * da.width * da.height > 1LL * db.width * db.height;
        });
        for (AVCaptureDeviceFormat *f : formats) {
            const CMVideoDimensions d = CMVideoFormatDescriptionGetDimensions(f.formatDescription);
            const bool seen = std::any_of(m_resolutions.begin(), m_resolutions.end(), [&](const Resolution &r) {
                return r.width == d.width && r.height == d.height;
            });
            if (seen)
                continue;
            Resolution r;
            r.width = d.width;
            r.height = d.height;
            r.binning = 1;
            r.label = std::to_string(r.width) + " x " + std::to_string(r.height);
            m_resolutions.push_back(r);
            impl->formats.push_back(f);
        }
        if (m_resolutions.empty()) {
            error = "The camera offers no video format";
            return false;
        }
        m_resolutions.front().label += " (full)";

        // AVFoundation on macOS has no manual exposure or ISO API (only iOS
        // does), so a UVC camera here runs with its own automatic exposure. The
        // exposure/gain controls report themselves as unavailable rather than
        // silently doing nothing; what can be offered is locking the automatic
        // exposure and white balance, in properties().
        m_hasExposure = false;
        m_hasGain = false;

        m_impl = impl.release();
        // apply the largest format
        std::string err;
        setResolutionIndex(0);
        (void)err;
    }
    return true;
}

void AvfCamera::close()
{
    stopStreaming();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_impl)
        return;
    @autoreleasepool {
        [m_impl->output setSampleBufferDelegate:nil queue:nil];
        if (m_impl->delegate)
            m_impl->delegate.owner = nullptr;
        m_impl->delegate = nil;
        m_impl->output = nil;
        m_impl->input = nil;
        m_impl->session = nil;
        m_impl->device = nil;
        m_impl->queue = nil;
    }
    delete m_impl;
    m_impl = nullptr;
    m_resolutions.clear();
}

bool AvfCamera::startStreaming(std::string &error)
{
    if (m_streaming)
        return true;
    if (!m_impl) {
        error = "camera not open";
        return false;
    }
    @autoreleasepool {
        [m_impl->session startRunning];
        if (!m_impl->session.isRunning) {
            error = "The camera did not start. Check System Settings > Privacy & Security > Camera.";
            return false;
        }
    }
    m_streaming = true;
    return true;
}

void AvfCamera::stopStreaming()
{
    if (!m_streaming)
        return;
    m_streaming = false;
    @autoreleasepool {
        if (m_impl && m_impl->session)
            [m_impl->session stopRunning];
    }
}

bool AvfCamera::setResolutionIndex(int index)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_impl || index < 0 || index >= int(m_impl->formats.size()))
        return false;
    @autoreleasepool {
        NSError *err = nil;
        if (![m_impl->device lockForConfiguration:&err])
            return false;
        m_impl->device.activeFormat = m_impl->formats[size_t(index)];
        [m_impl->device unlockForConfiguration];
    }
    m_resIndex = index;
    return true;
}

bool AvfCamera::setExposure(double)
{
    return false; // see open(): macOS AVFoundation has no manual exposure API
}

bool AvfCamera::setGain(double)
{
    return false;
}

std::vector<CameraProperty> AvfCamera::properties() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<CameraProperty> out;
    if (!m_impl)
        return out;
    @autoreleasepool {
        AVCaptureDevice *d = m_impl->device;
        if ([d isWhiteBalanceModeSupported:AVCaptureWhiteBalanceModeContinuousAutoWhiteBalance]) {
            CameraProperty p;
            p.key = "autowb";
            p.label = "Automatic white balance (camera)";
            p.type = CameraProperty::Type::Bool;
            p.min = 0;
            p.max = 1;
            p.value = d.whiteBalanceMode != AVCaptureWhiteBalanceModeLocked ? 1 : 0;
            out.push_back(p);
        }
        if ([d isExposureModeSupported:AVCaptureExposureModeContinuousAutoExposure]
            && [d isExposureModeSupported:AVCaptureExposureModeLocked]) {
            CameraProperty p;
            p.key = "autoexp";
            p.label = "Automatic exposure (camera)";
            p.type = CameraProperty::Type::Bool;
            p.min = 0;
            p.max = 1;
            p.value = d.exposureMode == AVCaptureExposureModeContinuousAutoExposure ? 1 : 0;
            out.push_back(p);
        }
    }
    return out;
}

bool AvfCamera::setProperty(const std::string &key, double value)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_impl)
        return false;
    @autoreleasepool {
        AVCaptureDevice *d = m_impl->device;
        NSError *err = nil;
        if (![d lockForConfiguration:&err])
            return false;
        bool ok = false;
        if (key == "autowb") {
            const AVCaptureWhiteBalanceMode m = value > 0.5 ? AVCaptureWhiteBalanceModeContinuousAutoWhiteBalance
                                                            : AVCaptureWhiteBalanceModeLocked;
            if ([d isWhiteBalanceModeSupported:m]) {
                d.whiteBalanceMode = m;
                ok = true;
            }
        } else if (key == "autoexp") {
            // Locked freezes whatever the camera last metered; Custom (a real
            // manual mode) does not exist on macOS.
            const AVCaptureExposureMode m =
                value > 0.5 ? AVCaptureExposureModeContinuousAutoExposure : AVCaptureExposureModeLocked;
            if ([d isExposureModeSupported:m]) {
                d.exposureMode = m;
                ok = true;
            }
        }
        [d unlockForConfiguration];
        return ok;
    }
}

std::vector<std::pair<std::string, std::string>> AvfCamera::details() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::pair<std::string, std::string>> d;
    d.emplace_back("Name", m_info.name);
    if (!m_info.model.empty())
        d.emplace_back("Model", m_info.model);
    d.emplace_back("Backend", "AVFoundation");
    if (m_impl) {
        @autoreleasepool {
            const CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(m_impl->device.activeFormat.formatDescription);
            d.emplace_back("Format", std::to_string(dim.width) + " x " + std::to_string(dim.height) + " BGRA");
        }
    }
    d.emplace_back("Exposure control", "automatic (macOS has no manual UVC exposure API)");
    d.emplace_back("Gain control", "automatic");
    return d;
}

void AvfCamera::deliverFrame(const void *bgra, int width, int height, int stride)
{
    if (!m_streaming)
        return;
    const size_t need = size_t(width) * 4 * height;
    auto f = m_pool->acquire(need);
    f->width = width;
    f->height = height;
    f->stride = width * 4;
    f->format = PixelFormat::BGRA8;
    f->bitDepth = 8;
    f->sequence = m_sequence++;
    f->timestamp = std::chrono::steady_clock::now();
    f->exposureMs = m_exposure;
    f->gain = m_gain;
    if (stride == f->stride) {
        std::memcpy(f->data.data(), bgra, need);
    } else {
        // CoreVideo pads rows; copy row by row into a tightly packed frame
        const auto *src = static_cast<const uint8_t *>(bgra);
        for (int y = 0; y < height; ++y)
            std::memcpy(f->data.data() + size_t(y) * f->stride, src + size_t(y) * stride, size_t(f->stride));
    }
    emitFrame(f);
}

} // namespace lm
