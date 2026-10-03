#pragma once
// Exporting the images of a Leica .lif: one at a time, a selection, or the
// whole file.
//
// Two kinds of output:
//  - Original data: each image as an ImageJ hyperstack TIFF holding every
//    channel, z slice and time point unscaled (8- or 16-bit grey, the channel
//    colours as LUTs), calibrated, so ImageJ/Fiji, QuPath or Python can measure
//    it. Tiles are merged into one mosaic or written one file per tile.
//  - As shown: a colour picture of each plane (the channels in their colours,
//    with the contrast set in the viewer) as TIFF, PNG or JPEG, for slides and
//    reports. Every plane, the current one, or a maximum projection along z.
//
// A summary (images.csv) and the file's own metadata (the LAS X XML) can be
// written next to the images.

#include "io/ImageIO.h"
#include "io/LifFile.h"

#include <QImage>
#include <QStringList>

#include <functional>

namespace lm {

struct LifExportOptions {
    enum Kind { OriginalData, AsShown };
    enum Planes { AllPlanes, CurrentPlane, MaxProjection };
    Kind kind = OriginalData;
    Planes planes = AllPlanes;  // AsShown: which planes become pictures
    bool mergeTiles = true;     // a tile scan as one mosaic, else one file per tile
    FileFormat format = FileFormat::Tiff; // AsShown
    bool sixteenBit = false;    // AsShown TIFF/PNG
    int jpegQuality = 95;
    bool compress = true;       // TIFF: Deflate
    bool subfolder = true;      // into a folder named after the .lif
    bool summary = true;        // images.csv and the LAS X metadata (XML)
    // AsShown: changes the 8-bit picture before it is saved (a scale bar);
    // called on the exporting thread
    std::function<QImage(const QImage &, double umPerPixel)> decorate;
};

struct LifExportItem {
    int image = -1;                    // index into LifFileIndex::images
    LifRequest request;                // the plane shown in the viewer
    QList<LifChannelDisplay> display;  // empty: automatic (see autoLifDisplay)
};

struct LifExportResult {
    QString folder;                    // where the files went
    QStringList written;               // every file written
    QStringList failed;                // "image: why"
    bool cancelled = false;
};

// Called after every plane; returning false cancels.
using LifExportProgress = std::function<bool(qint64 done, qint64 total, const QString &what)>;

LifExportResult exportLif(const LifFileIndex &index, const QList<LifExportItem> &items, const QString &folder,
                          const LifExportOptions &options, const LifExportProgress &progress = {});

// How an image is shown when nobody chose: a colour camera image as recorded,
// anything else stretched per channel from `planes` (the first plane).
QList<LifChannelDisplay> autoLifDisplay(const LifEntry &e, const QList<LifPlane> &planes);

// A file name for an image: its folders inside the .lif and its name, with
// characters no file system accepts replaced.
QString lifImageFileName(const LifEntry &e);
QString safeFileName(const QString &name);

} // namespace lm
