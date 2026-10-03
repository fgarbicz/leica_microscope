#pragma once
// Leica Image File format (.lif): the container LAS X saves an experiment in.
// One file holds many images with their names and calibration, which is how the
// microscope's own software organises a session.
//
// The format was read back from the files LAS X writes (see docs/LIF_FORMAT.md).
// Layout:
//
//   header block   int32 0x70
//                  uint32  size of the rest of the header
//                  byte    0x2A
//                  uint32  number of UTF-16 characters
//                  UTF-16LE XML  (<LMSDataContainerHeader Version="2">…)
//
//   then one memory block per image, in the order the XML names them:
//                  int32 0x70
//                  uint32  size of the rest of the block description
//                  byte    0x2A
//                  uint64  size of the pixel data   (uint32 in version 1)
//                  byte    0x2A
//                  uint32  number of UTF-16 characters
//                  UTF-16LE block id  ("MemBlock_180")
//                  the pixel data
//
// The XML gives each image a Memory element naming its block, Channels and
// Dimensions. Every channel and every dimension has a byte increment
// (BytesInc), so a sample lies at
//
//     channel.bytesInc + x * xInc + y * yInc + z * zInc + t * tInc + tile * tileInc …
//
// from the start of the block, whatever order the dimensions were stored in.
// That one rule covers an interleaved camera image (Leica writes BGR, so the
// channels are 0, 1 and 2 bytes apart), planar fluorescence channels (a whole
// plane apart), z stacks, time series, tile scans and anything else LAS X adds.

#include "core/Frame.h"
#include "io/ImageIO.h" // LoadedImage, shared with the other readers
#include "io/Metadata.h"

#include <QDateTime>
#include <QList>
#include <QPair>
#include <QRgb>
#include <QString>

#include <atomic>
#include <cstdint>
#include <vector>

namespace lm {

// DimID values LAS X uses. 1 and 2 are always x and y.
enum LifDimId {
    LifDimX = 1,
    LifDimY = 2,
    LifDimZ = 3,
    LifDimT = 4,
    LifDimLambda = 5,   // emission wavelength (spectral scan)
    LifDimRotation = 6,
    LifDimXT = 7,
    LifDimTSlice = 8,
    LifDimLambdaEx = 9, // excitation wavelength
    LifDimMosaic = 10,  // the tiles of a tile scan
};

struct LifDimension {
    int id = 0;
    int size = 1;          // NumberOfElements
    qint64 bytesInc = 0;
    double origin = 0;
    double length = 0;     // the whole axis, in `unit` (metres for space, seconds for time)
    QString unit;

    // "Z", "T", "Tile", "λ"…
    QString label() const;
    // The distance between two neighbouring elements in µm (z) or seconds (t);
    // 0 when the file does not say.
    double step() const;
};

struct LifChannel {
    QString lut;           // LUTName: "Red", "Green", "Gray"…
    QRgb colour = 0xffffffff; // what the LUT shows at full intensity
    QString name;          // a detector or dye when the file names one, else the LUT
    int bits = 8;          // significant bits (Resolution): 8, 12, 16…
    int sampleBytes = 1;   // storage: 1, 2, or 4 (32-bit float)
    bool isFloat = false;
    qint64 bytesInc = 0;   // offset of the channel's first sample in the block
    int maxValue() const { return isFloat ? 65535 : bits >= 16 ? 65535 : (1 << bits) - 1; }
};

// A tile of a tile scan, as TileScanInfo lists them (in the order of the
// mosaic dimension).
struct LifTile {
    int fieldX = 0, fieldY = 0;
    double posX = 0, posY = 0; // stage position in metres
    bool hasPosition = false;
};

// One image inside a .lif, as listed without reading any pixels.
struct LifEntry {
    QString name;          // as shown in LAS X
    QString path;          // folder path inside the file ("Experiment/Series 1")
    QString uniqueId;
    int width = 0;
    int height = 0;
    qint64 xInc = 0;       // bytes from one pixel to the next
    qint64 yInc = 0;       // bytes from one row to the next
    QList<LifChannel> channels;
    // Every dimension beyond x and y, in the order the file lists them. A
    // dimension of size 1 is kept (it costs nothing and is what LAS X shows).
    QList<LifDimension> planeDims;
    double umPerPixel = 0;  // x; 0 when the file gives no calibration
    double umPerPixelY = 0;
    // red, green and blue channels, i.e. a colour camera image: shown as it is,
    // without a contrast stretch
    bool colour = false;
    int bitsPerSample = 8;  // storage of the deepest channel: 8, 16 or 32
    QList<LifTile> tiles;
    bool tileFlipX = false, tileFlipY = false, tileSwapXY = false;
    QDateTime acquired;     // first time stamp; invalid when there is none
    // Instrument settings worth showing (objective, NA, exposure…), in order.
    QList<QPair<QString, QString>> info;
    // Objective details for the image's metadata.
    QString objective;
    double magnification = 0;
    double numericalAperture = 0;
    double exposureMs = 0;
    QString microscope, camera, software;
    // the image's Element in the header XML (characters)
    qint64 xmlStart = 0, xmlLength = 0;

    QString blockId;
    qint64 dataOffset = 0;
    qint64 dataBytes = 0;

    // index into planeDims of a dimension, or -1
    int dimIndex(int id) const;
    // its size; 1 when the image does not have it
    int sizeOf(int id) const;
    // how many planes (of all channels) the image holds
    qint64 planeCount() const;
    bool hasTiles() const { return sizeOf(LifDimMosaic) > 1; }
    // "1024 × 1024 · 2 ch · 16 z · 4 tiles"
    QString summary() const;
};

// A position in the planes of an image: one index per entry in planeDims.
using LifCoord = QList<int>;

// The tree LAS X shows: folders and images. `image` indexes LifFileIndex::images.
struct LifNode {
    QString name;
    int image = -1;
    QList<LifNode> children;
};

struct LifFileIndex {
    QString path;
    int version = 2;
    QString xml;              // the header, for showing or saving the full metadata
    LifNode root;             // the file itself; its children are what LAS X lists
    QList<LifEntry> images;   // every readable image, in tree order
    QStringList problems;     // images that were left out, and why
    // The XML of one image's Element, indented for reading.
    QString elementXml(const LifEntry &e) const;
};

// Reads the header: the tree, every image and its settings. Reads no pixels.
bool readLif(const QString &path, LifFileIndex &out, QString *error = nullptr);
// The images alone, in tree order.
bool readLifIndex(const QString &path, QList<LifEntry> &out, QString *error = nullptr);

// One channel of one plane, unscaled (a 12-bit image keeps 0..4095).
struct LifPlane {
    int width = 0;
    int height = 0;
    int bits = 8;            // significant bits, for the display range
    std::vector<uint16_t> px;
    bool empty() const { return px.empty(); }
    uint16_t at(int x, int y) const { return px[size_t(y) * width + x]; }
};

// What to read: the plane at `coord`, collapsed along a dimension or with the
// tiles put together.
struct LifRequest {
    LifCoord coord;           // planeDims.size() indices; missing ones are 0
    int projectDim = -1;      // a planeDims index to take the maximum along (z), or -1
    bool mergeTiles = false;  // place every tile of a tile scan where the stage was
    int subsample = 1;        // read every n-th pixel and row (a preview of a huge image)
};

// Reads one channel. `cancel`, when given and set, stops it (returns false
// with an empty error).
bool readLifPlane(const QString &path, const LifEntry &e, int channel, const LifRequest &req, LifPlane &out,
                  QString *error = nullptr, const std::atomic<bool> *cancel = nullptr);
// Every channel, in order.
bool readLifChannels(const QString &path, const LifEntry &e, const LifRequest &req, QList<LifPlane> &out,
                     QString *error = nullptr, const std::atomic<bool> *cancel = nullptr);

// Size of the merged tile scan in pixels (at subsample 1), and where each tile
// goes. Falls back to a grid of FieldX/FieldY when the stage positions are missing.
QSize lifMosaicSize(const LifEntry &e, QList<QPoint> *tileOrigins = nullptr);

// How a channel is shown: its colour, and the raw values mapped to black and
// to full intensity.
struct LifChannelDisplay {
    bool visible = true;
    QRgb colour = 0xffffffff;
    int low = 0;
    int high = 255;
};
// The full range of each channel in its LUT colour: a colour camera image looks
// exactly as recorded.
QList<LifChannelDisplay> defaultLifDisplay(const LifEntry &e);
// A contrast stretch that saturates `fraction` of the pixels at each end:
// 0.35 % in all, as ImageJ's Auto does.
void autoLifRange(const LifPlane &p, int &low, int &high, double fraction = 0.00175);
// Adds the visible channels, each in its colour, into one RGB image.
Image16 composeLif(const QList<LifPlane> &planes, const QList<LifChannelDisplay> &display);

// The metadata the rest of the program keeps with an image (name, pixel size,
// objective, exposure).
ImageMetadata lifMetadata(const LifEntry &e);

// Reads the first plane of an image, every channel in its colour at full range.
bool readLifImage(const QString &path, const LifEntry &entry, LoadedImage &out, QString *error = nullptr);

// An image to write into a .lif.
struct LifImageOut {
    QString name;
    Image16 data;          // RGB; written as 8-bit BGR, or 16-bit when eightBit is false
    double umPerPixel = 0; // 0 leaves the image uncalibrated
    bool eightBit = true;  // LAS X shows 8-bit colour; 16-bit is for our own use
};

// Writes a .lif holding these images. `experimentName` is the top level element
// LAS X shows for the file (empty uses the file name).
bool writeLif(const QString &path, const QList<LifImageOut> &images, const QString &experimentName = QString(),
              QString *error = nullptr);

} // namespace lm
