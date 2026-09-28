#pragma once
// Correcting the pixel size recorded in images that were already saved.
//
// Until the camera adapter was measured, the nominal calibration used an
// assumed 0.7x C-mount adapter. The real one is 1.0x, confirmed against the
// figures LAS X records for the same objectives, so every image saved with the
// nominal scale carries a pixel size that is too large by 1 / 0.7. That value
// sits in the file itself - the TIFF resolution tags, the PNG pixel density and
// the JSON metadata - so ImageJ, QuPath and this program all read the wrong
// scale.
//
// The adapter is recorded with every capture, including those whose pixel size
// came from a stage-micrometer calibration or was entered by hand; those were
// right and must not be touched. So an image is only corrected when its
// recorded pixel size is exactly the nominal one at the wrong adapter:
//
//     umPerPixel = sensor pitch / (objective x wrong adapter x k)
//
// with k = 1, 2 or 3 (plain capture, 16- and 36-shot pixel shift), and its
// metadata does not say the scale was measured or set by hand. Images that
// record the wrong adapter but not the nominal scale are listed as left alone.
//
// The corrected value is the nominal one at the right adapter, not the old
// value rescaled, so applying a fix twice (a failed sidecar write, a second
// run) cannot correct an image twice. The pixels are never changed.

#include "io/Metadata.h"

#include <QList>
#include <QString>

namespace lm {

// The sensor pitch nominal scales were computed from (MicroscopeConfig's default).
constexpr double kNominalSensorPixelUm = 5.86;

struct CalibrationFix {
    QString path;
    double oldUmPerPixel = 0;
    double newUmPerPixel = 0; // == oldUmPerPixel for an image left alone
    double recordedAdapter = 0;
    // TIFF, PNG and BMP carry the scale in the file and are rewritten losslessly.
    // A JPEG would have to be re-encoded to change it, which would cost image
    // quality, so only its .json sidecar is corrected - created when it has none.
    bool rewritesFile = true;
    bool createsSidecar = false;
    QString note; // why an image is left alone
};

// Images under `folder` saved with the nominal scale at `wrongAdapter`, which
// need it replaced by the nominal scale at `rightAdapter`. Images that record
// `wrongAdapter` but whose pixel size was measured or set by hand go to
// `leftAlone` (with a note saying why) when it is given.
QList<CalibrationFix> findCalibrationFixes(const QString &folder, bool recursive, double wrongAdapter,
                                           double rightAdapter, QList<CalibrationFix> *leftAlone = nullptr,
                                           double sensorPixelUm = kNominalSensorPixelUm);

// Applies one fix: rewrites the file's own calibration where the format allows
// it, and the .json sidecar when there is one (or, for a JPEG, a new one). Each
// file is replaced only after a complete write.
bool applyCalibrationFix(const CalibrationFix &fix, double rightAdapter, QString *error = nullptr);

} // namespace lm
