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
// The XML gives each image a Memory element naming its block, Channels that say
// how the samples are interleaved, and Dimensions whose Length (in metres) over
// NumberOfElements is the pixel size. An 8-bit colour image from a Leica camera
// is stored as interleaved BGR, which is what LAS X writes and what this module
// writes back.

#include "core/Frame.h"
#include "io/ImageIO.h" // LoadedImage, shared with the other readers
#include "io/Metadata.h"

#include <QList>
#include <QString>

namespace lm {

// One image inside a .lif, as listed without reading any pixels.
struct LifEntry {
    QString name;          // as shown in LAS X
    QString path;          // folder path inside the file ("Experiment/Series 1")
    int width = 0;
    int height = 0;
    int channels = 0;      // 1 = mono, 3 = colour
    int bitsPerSample = 8; // 8 or 16
    double umPerPixel = 0; // 0 when the file gives no calibration
    QString blockId;
    qint64 dataOffset = 0;
    qint64 dataBytes = 0;
    // Byte offset of each channel within a pixel, from the XML. Leica writes
    // BGR, so this is {2,1,0} for red, green, blue.
    int redOffset = 2, greenOffset = 1, blueOffset = 0;
    int bytesPerPixel = 3;
    qint64 rowStride = 0;
};

// Lists what a .lif holds. Cheap: reads the header only.
bool readLifIndex(const QString &path, QList<LifEntry> &out, QString *error = nullptr);

// Reads one of the images an index listed.
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
