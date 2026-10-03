#pragma once
// Grey-level image stacks as ImageJ hyperstack TIFFs: channels, z slices and
// time points in one file that ImageJ/Fiji opens as a hyperstack with its
// calibration, channel colours and display ranges, and that Bio-Formats
// (QuPath, napari, Python's tifffile) reads the same way.
//
// The layout follows what ImageJ writes itself: one 8- or 16-bit grey page per
// plane, channel varying fastest, then slice, then frame; an ImageDescription
// starting "ImageJ=" on the first page; the pixel size in XResolution and
// YResolution as pixels per micron; and the channel LUTs and display ranges in
// ImageJ's private tags (50838/50839).

#include <QList>
#include <QRgb>
#include <QString>

#include <cstdint>
#include <functional>
#include <vector>

namespace lm {

struct StackCalibration {
    double umPerPixel = 0;      // x; 0 = uncalibrated
    double umPerPixelY = 0;     // 0 = same as x
    double zStepUm = 0;         // between slices
    double frameIntervalS = 0;  // between frames
};

struct StackChannel {
    QRgb colour = 0xffffffff;   // the LUT ramps from black to this
    double displayMin = 0;
    double displayMax = 255;
};

// Fills `plane` (width * height samples, row by row) with plane `index` in
// ImageJ order: index = c + channels * (z + slices * t). Returns false (with
// a message) to stop.
using StackPlaneSource = std::function<bool(int index, std::vector<uint16_t> &plane, QString *error)>;

// Classic TIFF, so a file is limited to 4 GB; a stack that would be larger is
// refused before anything is written (see imageJStackBytes).
bool writeImageJStack(const QString &path, int width, int height, int bits, int channels, int slices, int frames,
                      const StackPlaneSource &source, const StackCalibration &cal, const QList<StackChannel> &luts,
                      bool deflate, QString *error = nullptr);

// The size of the pixel data of such a stack, uncompressed.
inline qint64 imageJStackBytes(int width, int height, int bits, qint64 planes)
{
    return qint64(width) * height * (bits > 8 ? 2 : 1) * planes;
}

} // namespace lm
