// Command-line IHC analysis (for scripting and parameter tuning):
//   ihctool image.tif [--diam um] [--contrast od] [--minstain od] [--dab od]
//                     [--cyto] [--estimate] [--overlay out.png]
#include "imaging/NucleusDetection.h"
#include "imaging/StainAnalysis.h"
#include "io/ImageIO.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>

#include <cstdio>
#include <cstring>

using namespace lm;

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    if (argc < 2) {
        std::printf("usage: ihctool image.tif [--diam um] [--contrast od] [--minstain od] [--dab od] [--cyto] "
                    "[--estimate] [--overlay out.png]\n");
        return 1;
    }
    NucleusOptions no;
    StainOptions so;
    QString overlay;
    bool estimate = false;
    for (int i = 2; i < argc; ++i) {
        const auto next = [&] { return i + 1 < argc ? std::atof(argv[++i]) : 0.0; };
        if (!std::strcmp(argv[i], "--diam"))
            no.diameterUm = next();
        else if (!std::strcmp(argv[i], "--contrast"))
            no.minContrast = next();
        else if (!std::strcmp(argv[i], "--minstain"))
            no.minStain = next();
        else if (!std::strcmp(argv[i], "--dab"))
            so.dabThreshold = no.dabThreshold = next();
        else if (!std::strcmp(argv[i], "--cyto"))
            no.nuclearMarker = false;
        else if (!std::strcmp(argv[i], "--estimate"))
            estimate = true;
        else if (!std::strcmp(argv[i], "--overlay") && i + 1 < argc)
            overlay = QString::fromLocal8Bit(argv[++i]);
    }
    LoadedImage li;
    QString err;
    if (!loadImage(QString::fromLocal8Bit(argv[1]), li, &err)) {
        std::printf("cannot read %s: %s\n", argv[1], qPrintable(err));
        return 1;
    }
    if (estimate) {
        std::string msg;
        if (estimateStainVectors(li.data, so.vectors, true, &msg))
            std::printf("stain vectors: H %.3f %.3f %.3f, DAB %.3f %.3f %.3f (%s)\n", so.vectors.h[0], so.vectors.h[1],
                        so.vectors.h[2], so.vectors.dab[0], so.vectors.dab[1], so.vectors.dab[2], msg.c_str());
        else
            std::printf("stain estimation failed: %s\n", msg.c_str());
    }
    so.umPerPixel = no.umPerPixel = li.meta.umPerPixel;
    const StainResult st = analyzeStains(li.data, so);
    const NucleusResult nr = detectNuclei(st, no);
    std::printf("%s: %.1f um/px, DAB+ area %.1f %%, H-score %.0f; nuclei %d (positive %d, negative %d), "
                "labelling index %.1f %%, %.0f /mm2\n",
                argv[1], li.meta.umPerPixel, st.positiveFraction * 100, st.hScore, nr.positive + nr.negative, nr.positive,
                nr.negative, nr.labellingIndex * 100, nr.densityPerMm2);
    if (!overlay.isEmpty()) {
        QImage img = toQImage8(li.data).convertToFormat(QImage::Format_RGB32);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        const double rad = nr.radiusPx * 0.9;
        for (const auto &n : nr.nuclei) {
            p.setPen(QPen(n.positive ? QColor(230, 30, 30) : QColor(30, 110, 255), 2));
            p.drawEllipse(QPointF(n.x, n.y), rad, rad);
        }
        p.end();
        img.save(overlay);
    }
    return 0;
}
