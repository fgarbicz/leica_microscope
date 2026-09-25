#pragma once
// Saving and loading of microscope images with metadata.
// TIFF is written by our own encoder (8/16-bit RGB, Deflate + predictor,
// physical resolution tags, JSON metadata in ImageDescription), readable by
// ImageJ/Fiji, QuPath, Photoshop etc. Other formats go through Qt.

#include "core/Frame.h"
#include "io/Metadata.h"

#include <QImage>
#include <QString>

namespace lm {

enum class FileFormat { Tiff, Png, Jpeg, Bmp };

struct SaveOptions {
    FileFormat format = FileFormat::Tiff;
    bool sixteenBit = true;     // TIFF/PNG only
    bool compress = true;       // TIFF: Deflate
    int jpegQuality = 95;
    bool writeSidecar = true;   // .json with metadata next to PNG/JPEG/BMP
};

struct LoadedImage {
    Image16 data;               // RGB, 16-bit scaled (8-bit sources are x257)
    int sourceBitDepth = 8;
    ImageMetadata meta;
    bool hasMeta = false;
    QString path;
};

QString extensionFor(FileFormat f);
FileFormat formatFromExtension(const QString &path);

bool saveImage(const QString &path, const Image16 &img, const ImageMetadata &meta, const SaveOptions &opt,
               QString *error = nullptr);
bool saveImage(const QString &path, const QImage &img, const ImageMetadata &meta, const SaveOptions &opt,
               QString *error = nullptr);
bool loadImage(const QString &path, LoadedImage &out, QString *error = nullptr);
// Reads only the metadata (fast; used by the browser).
bool loadMetadata(const QString &path, ImageMetadata &out);

// Conversions
QImage toQImage8(const Image16 &img);
QImage toQImage8(const Image8 &img);
Image16 fromQImage(const QImage &img);

// Low level TIFF
// Classic TIFF stores 32-bit offsets: larger files cannot be written.
constexpr qint64 kTiffMaxBytes = 0xFFFFFFFFLL;
// Worst-case (uncompressed) TIFF file size in bytes for an RGB image.
qint64 tiffUncompressedSize(qint64 width, qint64 height, int bits);
// Resolutions at or below this (px per metre, i.e. >= 50 um/px) are not a
// microscope calibration (e.g. the 72 dpi screen default) and are ignored.
constexpr double kMinCalibratedPxPerMetre = 20000.0;
bool writeTiff(const QString &path, const Image16 &img, int bits, bool deflate, double umPerPixel,
               const QString &description, QString *error);
bool readTiff(const QString &path, Image16 &img, int &bits, QString &description, double &umPerPixel,
              QString *error);

} // namespace lm
