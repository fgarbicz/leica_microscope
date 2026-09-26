#pragma once
// Correcting the pixel size recorded in images that were already saved.
//
// Until the camera adapter was measured, the nominal calibration used an
// assumed 0.7x C-mount adapter. The real one is 1.0x, confirmed against the
// figures LAS X records for the same objectives, so every image saved with the
// nominal (not stage-micrometer calibrated) scale carries a pixel size that is
// too large by 1 / 0.7. That value sits in the file itself - the TIFF
// resolution tags, the PNG pixel density and the JSON metadata - so ImageJ,
// QuPath and this program all read the wrong scale.
//
// This finds those images and rewrites the calibration in place. The pixels are
// never changed.

#include "io/Metadata.h"

#include <QList>
#include <QString>

namespace lm {

struct CalibrationFix {
    QString path;
    double oldUmPerPixel = 0;
    double newUmPerPixel = 0;
    double recordedAdapter = 0;
    // TIFF, PNG and BMP carry the scale in the file and are rewritten losslessly.
    // A JPEG would have to be re-encoded to change it, which would cost image
    // quality, so only its .json sidecar is corrected.
    bool rewritesFile = true;
};

// Images under `folder` whose metadata says they were saved with
// `wrongAdapter` and so need their pixel size multiplied by
// wrongAdapter / rightAdapter. Stage-micrometer calibrated images record the
// adapter they were taken with too, so the test is the recorded adapter, not
// the pixel size.
QList<CalibrationFix> findCalibrationFixes(const QString &folder, bool recursive, double wrongAdapter,
                                           double rightAdapter);

// Applies one fix: rewrites the file's own calibration where the format allows
// it, and always the .json sidecar when there is one. The file is replaced only
// after a complete write.
bool applyCalibrationFix(const CalibrationFix &fix, double rightAdapter, QString *error = nullptr);

} // namespace lm
