#include "ui/Theme.h"
#include "BatchIhcDialog.h"

#include "app/AppSettings.h"
#include "imaging/StainAnalysis.h"
#include "ui/IhcOptions.h"
#include "io/ImageIO.h"
#include "ui/Annotations.h"
#include "ui/Overlays.h"
#include "ui/PlatformUi.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPdfWriter>
#include <QTextDocument>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTableWidget>
#include <QThread>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>

namespace lm {

namespace {

enum Col { ColImage, ColObjective, ColRegion, ColTissue, ColPositive, ColWeak, ColModerate, ColStrong, ColHScore,
           ColMeanOd, ColCells, ColCellPct, ColCount };

// image with DAB-positive pixels tinted red and negative tissue blue
QImage overlayImage(const Image16 &img, const StainResult &r)
{
    QImage out = toQImage8(img).convertToFormat(QImage::Format_RGB32);
    for (int y = 0; y < out.height(); ++y) {
        QRgb *d = reinterpret_cast<QRgb *>(out.scanLine(y));
        const uint8_t *mk = r.mask.data() + size_t(y) * size_t(r.width);
        for (int x = 0; x < out.width(); ++x) {
            if (!mk[x])
                continue;
            const QColor tint = mk[x] == 2 ? QColor(200, 20, 20) : QColor(20, 70, 200);
            const double a = mk[x] == 2 ? 0.55 : 0.3;
            d[x] = qRgb(int(qRed(d[x]) * (1 - a) + tint.red() * a), int(qGreen(d[x]) * (1 - a) + tint.green() * a),
                        int(qBlue(d[x]) * (1 - a) + tint.blue() * a));
        }
    }
    return out;
}

double mean(const std::vector<double> &v)
{
    double s = 0;
    for (double x : v)
        s += x;
    return v.empty() ? 0 : s / double(v.size());
}

double stddev(const std::vector<double> &v)
{
    if (v.size() < 2)
        return 0;
    const double m = mean(v);
    double s = 0;
    for (double x : v)
        s += (x - m) * (x - m);
    return std::sqrt(s / double(v.size() - 1));
}

} // namespace

BatchIhcDialog::BatchIhcDialog(const QStringList &files, QWidget *parent) : QDialog(parent), m_files(files)
{
    setWindowTitle(tr("IHC quantification of %n image(s)", nullptr, int(files.size())));
    resize(px(1000), px(600));
    auto *lay = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    m_threshold = new QDoubleSpinBox(this);
    m_threshold->setRange(0.05, 1.0);
    m_threshold->setSingleStep(0.01);
    m_threshold->setDecimals(2);
    m_threshold->setValue(AppSettings::instance().ihc.dabThreshold);
    m_threshold->setToolTip(tr("DAB optical density above which a pixel counts as positive (same as in Process)"));
    m_threshold->setToolTip(tr("How brown a pixel must be to count as DAB-positive (optical density). Lower = more pixels positive. Default 0.15. Use the same value for all slides of a study."));
    form->addRow(tr("DAB threshold (optical density)"), m_threshold);
    {
        const auto &ih = AppSettings::instance().ihc;
        auto *stains = new QLabel(ih.customVectors
                                      ? tr("estimated from %1 (set in Process → IHC quantification)")
                                            .arg(ih.vectorSource.isEmpty() ? tr("an image") : ih.vectorSource)
                                      : tr("standard H-DAB (estimate them in Process → IHC quantification)"),
                                  this);
        stains->setObjectName(QStringLiteral("Hint"));
        form->addRow(tr("Stain colours"), stains);
    }
    m_useRegions = new QCheckBox(tr("Analyse only the rectangle / ellipse / area annotations of an image, if it has any"),
                                 this);
    m_useRegions->setChecked(true);
    form->addRow(QString(), m_useRegions);
    {
        const auto &ih = AppSettings::instance().ihc;
        m_countCells = new QCheckBox(tr("Also count nuclei / cells (%1 marker, %2 µm nuclei; set in Process)")
                                         .arg(ih.nuclearMarker ? tr("nuclear") : tr("cytoplasmic / membranous"))
                                         .arg(ih.nucleusDiameterUm, 0, 'f', 1),
                                     this);
        m_countCells->setToolTip(tr("Labelling index (nuclear markers) or % positive cells (cytoplasmic / membranous)"));
        form->addRow(QString(), m_countCells);
    }
    m_saveOverlays = new QCheckBox(tr("Save overlay images (red = DAB positive, blue = negative tissue)"), this);
    form->addRow(QString(), m_saveOverlays);
    auto *fr = new QHBoxLayout;
    m_overlayFolder = new QLineEdit(QFileInfo(files.value(0)).dir().filePath(QStringLiteral("ihc")), this);
    auto *browse = new QToolButton(this);
    browse->setText(QStringLiteral("…"));
    fr->addWidget(m_overlayFolder, 1);
    fr->addWidget(browse);
    form->addRow(tr("Overlay folder"), fr);
    lay->addLayout(form);

    m_table = new QTableWidget(0, ColCount, this);
    m_table->setHorizontalHeaderLabels({tr("Image"), tr("Objective"), tr("Region"), tr("Tissue area"),
                                        tr("DAB+ %"), tr("Weak %"), tr("Moderate %"), tr("Strong %"), tr("H-score"),
                                        tr("Mean DAB OD"), tr("Cells"), tr("Positive cells %")});
    m_table->horizontalHeader()->setSectionResizeMode(ColImage, QHeaderView::Stretch);
    for (int c = 1; c < ColCount; ++c)
        m_table->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    m_table->verticalHeader()->hide();
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSortingEnabled(false);
    lay->addWidget(m_table, 1);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, int(files.size()));
    m_progress->setValue(0);
    lay->addWidget(m_progress);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("Hint"));
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    lay->addWidget(m_status);

    auto *bb = new QDialogButtonBox(this);
    m_start = bb->addButton(tr("Analyse"), QDialogButtonBox::AcceptRole);
    m_export = bb->addButton(tr("Export CSV…"), QDialogButtonBox::ActionRole);
    m_copy = bb->addButton(tr("Copy table"), QDialogButtonBox::ActionRole);
    m_pdf = bb->addButton(tr("PDF report…"), QDialogButtonBox::ActionRole);
    m_pdf->setToolTip(tr("Report with settings, summary, results table and every image next to its overlay"));
    m_pdf->setEnabled(false);
    bb->addButton(QDialogButtonBox::Close);
    m_export->setEnabled(false);
    m_copy->setEnabled(false);
    lay->addWidget(bb);

    auto updateFolder = [this] { m_overlayFolder->setEnabled(m_saveOverlays->isChecked()); };
    updateFolder();
    connect(m_saveOverlays, &QCheckBox::toggled, this, updateFolder);
    connect(browse, &QToolButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, tr("Overlay folder"), m_overlayFolder->text());
        if (!d.isEmpty())
            m_overlayFolder->setText(d);
    });
    connect(bb, &QDialogButtonBox::accepted, this, &BatchIhcDialog::run);
    connect(m_export, &QPushButton::clicked, this, &BatchIhcDialog::exportCsv);
    connect(m_pdf, &QPushButton::clicked, this, &BatchIhcDialog::exportPdf);
    connect(m_status, &QLabel::linkActivated, this, [](const QString &url) { QDesktopServices::openUrl(QUrl(url)); });
    // tab separated for pasting into Excel
    connect(m_copy, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(csv(QLatin1Char('\t')));
        m_exported = true;
        m_status->setText(tr("Table copied to the clipboard (paste into Excel)"));
    });
    connect(bb, &QDialogButtonBox::rejected, this, &BatchIhcDialog::reject);
}

void BatchIhcDialog::run()
{
    if (m_running)
        return;
    const bool saveOverlays = m_saveOverlays->isChecked();
    const QString outDir = m_overlayFolder->text();
    if (saveOverlays && !QDir().mkpath(outDir)) {
        m_status->setText(tr("Cannot create %1").arg(outDir));
        return;
    }
    m_running = true;
    m_cancel = false;
    m_start->setEnabled(false);
    m_export->setEnabled(false);
    m_copy->setEnabled(false);
    m_pdf->setEnabled(false);
    m_rows.clear();
    m_exported = false;
    m_table->setRowCount(0);
    m_progress->setValue(0);
    StainOptions opt;
    opt.dabThreshold = m_threshold->value();
    {
        const auto &ih = AppSettings::instance().ihc;
        for (int c = 0; c < 3; ++c) {
            opt.vectors.h[c] = ih.h[c];
            opt.vectors.dab[c] = ih.dab[c];
        }
    }
    const bool useRegions = m_useRegions->isChecked();
    m_runThreshold = opt.dabThreshold;
    m_runUseRegions = useRegions;
    const bool countCells = m_countCells->isChecked();
    // overlay names already given out in this run: a.tif and a.png would
    // otherwise both write a_ihc.jpg, the second over the first
    QSet<QString> overlayNames;

    for (int i = 0; i < m_files.size() && !m_cancel; ++i) {
        const QString src = m_files[i];
        QString overlayName;
        if (saveOverlays) {
            const QString base = QFileInfo(src).completeBaseName() + QStringLiteral("_ihc");
            overlayName = base + QStringLiteral(".jpg");
            for (int n = 2; overlayNames.contains(overlayName.toLower()); ++n)
                overlayName = QStringLiteral("%1_%2.jpg").arg(base).arg(n);
            overlayNames.insert(overlayName.toLower()); // case-insensitive file systems (Windows, macOS)
        }
        m_status->setText(tr("Analysing %1…").arg(QFileInfo(src).fileName()));
        auto future = QtConcurrent::run([=]() -> Row {
            Row row;
            row.file = QFileInfo(src).fileName();
            LoadedImage li;
            QString err;
            if (!loadImage(src, li, &err)) {
                row.error = err.isEmpty() ? tr("cannot read") : err;
                return row;
            }
            const Image16 &img = li.data;
            row.objective = li.meta.objective.isEmpty() && li.meta.magnification > 0
                                ? QStringLiteral("%1x").arg(li.meta.magnification)
                                : li.meta.objective;
            row.umPerPixel = li.meta.umPerPixel;
            row.colorCorrection = li.meta.colorCorrection;
            row.lightFilter = li.meta.lightFilter;
            StainOptions o = opt;
            o.umPerPixel = li.meta.umPerPixel;
            QImage region;
            row.region = tr("whole image");
            if (useRegions) {
                AnnotationLayer layer;
                layer.setImageSize(QSize(img.width, img.height));
                if (layer.loadSidecar(src)) {
                    QVector<const Annotation *> regions;
                    for (const auto &a : layer.annotations())
                        if (isRegion(a))
                            regions.push_back(&a);
                    if (!regions.isEmpty()) {
                        region = regionMask(regions, QSize(img.width, img.height));
                        row.region = tr("%n region(s)", nullptr, int(regions.size()));
                    }
                }
            }
            const StainResult r =
                region.isNull()
                    ? analyzeStains(img, o)
                    : analyzeStains(img, o, [&region](int x, int y) { return region.constScanLine(y)[x] != 0; });
            const bool cal = li.meta.umPerPixel > 0;
            row.tissueArea = cal ? r.tissueAreaUm2 : double(r.tissuePixels);
            row.positiveArea = cal ? r.positiveAreaUm2 : double(r.positivePixels);
            row.positivePct = r.positiveFraction * 100;
            row.weakPct = r.weak * 100;
            row.moderatePct = r.moderate * 100;
            row.strongPct = r.strong * 100;
            row.hScore = r.hScore;
            row.meanDabPositive = r.meanDabPositive;
            if (r.lowBackground())
                row.warning = tr("little bare glass in the field: DAB may be underestimated (include some background)");
            NucleusResult nr;
            if (countCells) {
                const NucleusOptions no = nucleusOptionsFromSettings(li.meta.umPerPixel, o.dabThreshold);
                nr = region.isNull()
                         ? detectNuclei(r, no)
                         : detectNuclei(r, no, [&region](int x, int y) { return region.constScanLine(y)[x] != 0; });
                row.cells = nr.positive + nr.negative;
                row.positiveCells = nr.positive;
                row.positiveCellPct = nr.labellingIndex * 100;
                row.cellDensity = nr.densityPerMm2;
            }
            if (r.tissuePixels == 0)
                row.error = tr("no tissue found (image blank or overexposed?)");
            {
                QImage ov = overlayImage(img, r);
                if (countCells && !nr.nuclei.empty()) {
                    // rings on the counted nuclei (white outline keeps them visible on the tint)
                    QPainter p(&ov);
                    p.setRenderHint(QPainter::Antialiasing);
                    const double rad = nr.radiusPx * 0.9, lw = std::max(1.5, nr.radiusPx / 6);
                    for (const auto &n : nr.nuclei) {
                        p.setPen(QPen(Qt::white, lw * 2));
                        p.drawEllipse(QPointF(n.x, n.y), rad, rad);
                        p.setPen(QPen(n.positive ? QColor(170, 0, 0) : QColor(0, 50, 190), lw));
                        p.drawEllipse(QPointF(n.x, n.y), rad, rad);
                    }
                }
                if (!region.isNull()) {
                    // outline the analysed regions
                    QPainter p(&ov);
                    p.setRenderHint(QPainter::Antialiasing);
                    QPen pen(QColor(255, 220, 0), std::max(2.0, img.width / 800.0));
                    p.setPen(pen);
                    p.setBrush(Qt::NoBrush);
                    AnnotationLayer layer;
                    layer.setImageSize(QSize(img.width, img.height));
                    if (layer.loadSidecar(src))
                        for (const auto &a : layer.annotations())
                            drawRegionShape(p, a);
                }
                // thumbnails for the PDF report
                auto jpeg = [](const QImage &im) {
                    QByteArray b;
                    QBuffer buf(&b);
                    buf.open(QIODevice::WriteOnly);
                    im.scaledToWidth(std::min(900, im.width()), Qt::SmoothTransformation).save(&buf, "JPG", 88);
                    return b;
                };
                row.overlayJpeg = jpeg(ov);
                row.thumbJpeg = jpeg(toQImage8(img));
                if (!saveOverlays)
                    return row;
                SaveOptions so;
                so.format = FileFormat::Jpeg;
                so.sixteenBit = false;
                so.jpegQuality = 90;
                so.writeSidecar = false;
                const QString dst = QDir(outDir).filePath(overlayName);
                QString serr;
                if (!saveImage(dst, ov, li.meta, so, &serr))
                    row.error = tr("overlay not saved: %1").arg(serr);
            }
            return row;
        });
        while (!future.isFinished()) {
            QApplication::processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(10);
        }
        Row row;
        try {
            row = future.result(); // rethrows an exception from the worker (e.g. out of memory)
        } catch (const std::exception &e) {
            row.file = QFileInfo(src).fileName();
            row.error = tr("analysis failed: %1").arg(QString::fromUtf8(e.what()));
        } catch (...) {
            row.file = QFileInfo(src).fileName();
            row.error = tr("analysis failed");
        }
        m_rows.push_back(row);
        addRow(row);
        m_progress->setValue(i + 1);
    }
    m_running = false;
    m_start->setEnabled(true);
    m_export->setEnabled(!m_rows.empty());
    m_copy->setEnabled(!m_rows.empty());
    m_pdf->setEnabled(!m_rows.empty());

    std::vector<double> pos, hs;
    int failed = 0;
    for (const auto &r : m_rows) {
        if (!r.error.isEmpty() && r.tissueArea <= 0) {
            ++failed;
            continue;
        }
        pos.push_back(r.positivePct);
        hs.push_back(r.hScore);
    }
    QString s = tr("%1 image(s) analysed").arg(pos.size());
    if (failed)
        s += tr(", %1 failed").arg(failed);
    if (m_cancel)
        s += tr(" (cancelled)");
    if (!pos.empty())
        s += tr(". DAB positive %1 ± %2 %, H-score %3 ± %4 (mean ± SD)")
                 .arg(mean(pos), 0, 'f', 1)
                 .arg(stddev(pos), 0, 'f', 1)
                 .arg(mean(hs), 0, 'f', 0)
                 .arg(stddev(hs), 0, 'f', 0);
    if (saveOverlays)
        s += QStringLiteral(" — <a href=\"%1\">%2</a>").arg(QUrl::fromLocalFile(outDir).toString(), tr("open overlay folder"));
    if (const QString w = colourMixWarning(); !w.isEmpty())
        s += QStringLiteral("<br><span style='color:#e8a33d'>") + w.toHtmlEscaped() + QStringLiteral("</span>");
    m_status->setTextFormat(Qt::RichText);
    m_status->setText(s);
}

void BatchIhcDialog::addRow(const Row &r)
{
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    auto set = [&](int c, const QString &text, bool num = true) {
        auto *it = new QTableWidgetItem(text);
        if (num)
            it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_table->setItem(row, c, it);
    };
    set(ColImage, r.file, false);
    if (!r.error.isEmpty()) {
        m_table->item(row, ColImage)->setToolTip(r.error);
        m_table->item(row, ColImage)->setForeground(QColor(230, 90, 80));
    }
    set(ColObjective, r.objective, false);
    set(ColRegion, r.error.isEmpty() ? r.region : r.error, false);
    if (!r.warning.isEmpty()) {
        m_table->item(row, ColRegion)->setText(r.region + QStringLiteral(" ⚠"));
        m_table->item(row, ColRegion)->setToolTip(r.warning);
        m_table->item(row, ColRegion)->setForeground(QColor(232, 163, 61));
    }
    const bool cal = r.umPerPixel > 0;
    set(ColTissue, cal ? formatArea(r.tissueArea) : tr("%1 px").arg(qint64(r.tissueArea)));
    set(ColPositive, QString::number(r.positivePct, 'f', 1));
    set(ColWeak, QString::number(r.weakPct, 'f', 1));
    set(ColModerate, QString::number(r.moderatePct, 'f', 1));
    set(ColStrong, QString::number(r.strongPct, 'f', 1));
    set(ColHScore, QString::number(r.hScore, 'f', 0));
    set(ColMeanOd, QString::number(r.meanDabPositive, 'f', 3));
    set(ColCells, r.cells < 0 ? QStringLiteral("–") : QString::number(r.cells));
    set(ColCellPct, r.cells < 0 ? QStringLiteral("–") : QString::number(r.positiveCellPct, 'f', 1));
    m_table->scrollToBottom();
}

void BatchIhcDialog::reject()
{
    // Esc / window close during a run cancels it instead of hiding a running dialog
    if (m_running) {
        m_cancel = true;
        return;
    }
    if (!m_rows.empty() && !m_exported
        && QMessageBox::question(this, tr("IHC quantification"),
                                 tr("Close without saving the results? Use Export CSV…, PDF report… or Copy table to keep them."),
                                 QMessageBox::Close | QMessageBox::Cancel, QMessageBox::Cancel)
               != QMessageBox::Close)
        return;
    QDialog::reject();
}

QString BatchIhcDialog::csv(QChar sep) const
{
    auto q = [sep](QString s) {
        if (s.contains(sep) || s.contains(QLatin1Char('"')) || s.contains(QLatin1Char('\n')))
            s = QLatin1Char('"') + s.replace(QLatin1Char('"'), QStringLiteral("\"\"")) + QLatin1Char('"');
        return s;
    };
    auto num = [](double v, int decimals) { return QString::number(v, 'f', decimals); };
    const auto &ih = AppSettings::instance().ihc;
    const QString stains = ih.customVectors
                               ? QStringLiteral("H %1 %2 %3; DAB %4 %5 %6")
                                     .arg(ih.h[0], 0, 'f', 4)
                                     .arg(ih.h[1], 0, 'f', 4)
                                     .arg(ih.h[2], 0, 'f', 4)
                                     .arg(ih.dab[0], 0, 'f', 4)
                                     .arg(ih.dab[1], 0, 'f', 4)
                                     .arg(ih.dab[2], 0, 'f', 4)
                               : QStringLiteral("standard");
    const QStringList header = {QStringLiteral("image"), QStringLiteral("objective"), QStringLiteral("region"),
                                QStringLiteral("stain_vectors"), QStringLiteral("dab_threshold_od"),
                                QStringLiteral("tissue_area"), QStringLiteral("positive_area"), QStringLiteral("area_unit"),
                                QStringLiteral("dab_positive_pct"), QStringLiteral("weak_pct"), QStringLiteral("moderate_pct"),
                                QStringLiteral("strong_pct"), QStringLiteral("h_score"), QStringLiteral("mean_dab_od_positive"),
                                QStringLiteral("cells"), QStringLiteral("positive_cells"), QStringLiteral("positive_cells_pct"),
                                QStringLiteral("cell_density_per_mm2"), QStringLiteral("note")};
    QString out = header.join(sep) + QLatin1Char('\n');
    for (const auto &r : m_rows) {
        const int areaDecimals = r.umPerPixel > 0 ? 1 : 0;
        const bool counted = r.cells >= 0;
        const QStringList f = {q(r.file), q(r.objective), q(r.region), q(stains), num(m_runThreshold, 2),
                               num(r.tissueArea, areaDecimals), num(r.positiveArea, areaDecimals),
                               r.umPerPixel > 0 ? QStringLiteral("um2") : QStringLiteral("px"), num(r.positivePct, 2),
                               num(r.weakPct, 2), num(r.moderatePct, 2), num(r.strongPct, 2), num(r.hScore, 1),
                               num(r.meanDabPositive, 4), counted ? QString::number(r.cells) : QString(),
                               counted ? QString::number(r.positiveCells) : QString(),
                               counted ? num(r.positiveCellPct, 2) : QString(), counted ? num(r.cellDensity, 1) : QString(),
                               q(r.error.isEmpty() ? r.warning : r.error)};
        out += f.join(sep) + QLatin1Char('\n');
    }
    return out;
}

QString BatchIhcDialog::colourMixWarning() const
{
    // images with and without the camera colour correction have different stain
    // colours, so their DAB results are not directly comparable
    int corrected = 0, other = 0;
    for (const auto &r : m_rows)
        (r.colorCorrection.startsWith(QStringLiteral("camera matrix")) ? corrected : other)++;
    QStringList filters;
    for (const auto &r : m_rows)
        if (!filters.contains(r.lightFilter))
            filters << r.lightFilter;
    QString warning;
    if (corrected && other)
        warning = tr("%1 image(s) were taken with camera colour correction and %2 without (or by an older version); "
                     "their DAB results are not directly comparable.")
                      .arg(corrected)
                      .arg(other);
    if (filters.size() > 1) {
        if (!warning.isEmpty())
            warning += QLatin1Char(' ');
        warning += tr("The images were taken with different light filter settings; "
                      "their DAB results are not directly comparable.");
    }
    return warning;
}

QString BatchIhcDialog::stainDescription() const
{
    const auto &ih = AppSettings::instance().ihc;
    auto vec = [](const double v[3]) {
        return QStringLiteral("%1 / %2 / %3").arg(v[0], 0, 'f', 3).arg(v[1], 0, 'f', 3).arg(v[2], 0, 'f', 3);
    };
    const QString vectors = tr("H %1, DAB %2 (optical density R / G / B)").arg(vec(ih.h), vec(ih.dab));
    return ih.customVectors ? tr("estimated from %1: %2").arg(ih.vectorSource.toHtmlEscaped(), vectors)
                            : tr("standard haematoxylin-DAB (Ruifrok & Johnston): %1").arg(vectors);
}

void BatchIhcDialog::exportPdf()
{
    if (m_rows.empty())
        return;
    const QString def = QFileInfo(m_files.value(0)).dir().filePath(QStringLiteral("ihc_report.pdf"));
    const QString path = lm::getSaveFileName(this, tr("Save IHC report"), def, tr("PDF (*.pdf)"));
    if (path.isEmpty())
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QTextDocument doc;
    // the interface font: Segoe UI exists on Windows only, and elsewhere the
    // report would come out in whatever the PDF engine substitutes
    const QString family = QApplication::font().family();
    doc.setDefaultFont(QFont(family, 9));
    doc.setDefaultStyleSheet(QStringLiteral(
        "body { font-family: '%1', sans-serif; font-size: 9pt; color: #202020; }"
        "h1 { font-size: 16pt; color: #1b4f8a; margin-bottom: 2px; }"
        "h2 { font-size: 12pt; color: #1b4f8a; margin-top: 14px; }"
        "h3 { font-size: 10pt; margin-top: 12px; margin-bottom: 2px; }"
        "td, th { padding: 3px 5px; font-size: 8pt; }"
        "th { background-color: #e4ebf3; text-align: left; }"
        ".num { text-align: right; }"
        ".muted { color: #707070; }").arg(family));

    std::vector<double> pos, hs;
    for (const auto &r : m_rows)
        if (r.tissueArea > 0) {
            pos.push_back(r.positivePct);
            hs.push_back(r.hScore);
        }
    auto median = [](std::vector<double> v) {
        if (v.empty())
            return 0.0;
        std::sort(v.begin(), v.end());
        return v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
    };
    auto minmax = [](const std::vector<double> &v) {
        return v.empty() ? std::pair<double, double>{0, 0}
                         : std::pair<double, double>{*std::min_element(v.begin(), v.end()),
                                                     *std::max_element(v.begin(), v.end())};
    };

    QString h;
    h += QStringLiteral("<h1>%1</h1>").arg(tr("IHC quantification report (DAB)"));
    h += QStringLiteral("<p class='muted'>%1 &middot; %2 &middot; %3</p>")
             .arg(QLocale().toString(QDateTime::currentDateTime(), QLocale::LongFormat),
                  QDir::toNativeSeparators(QFileInfo(m_files.value(0)).absolutePath()).toHtmlEscaped(),
                  tr("DM Imaging %1").arg(QApplication::applicationVersion()));

    h += QStringLiteral("<h2>%1</h2><table>").arg(tr("Method"));
    auto kv = [&](const QString &k, const QString &v) {
        h += QStringLiteral("<tr><td><b>%1</b></td><td>%2</td></tr>").arg(k, v);
    };
    kv(tr("Colour deconvolution"), stainDescription());
    kv(tr("DAB positivity threshold"), tr("%1 OD; intensity classes: weak &lt; 0.35 &le; moderate &lt; 0.6 &le; strong")
                                           .arg(m_runThreshold, 0, 'f', 2));
    kv(tr("H-score"), tr("1 &times; %weak + 2 &times; %moderate + 3 &times; %strong, of the tissue area (0–300)"));
    if (const QString w = colourMixWarning(); !w.isEmpty())
        kv(tr("Warning"), QStringLiteral("<span style='color:#b35c00'>") + w.toHtmlEscaped() + QStringLiteral("</span>"));
    kv(tr("Region"), m_runUseRegions ? tr("rectangle / ellipse / area annotations where present, else whole image")
                                               : tr("whole image"));
    const bool anyCells = std::any_of(m_rows.begin(), m_rows.end(), [](const Row &r) { return r.cells >= 0; });
    if (anyCells) {
        const auto &ihs = AppSettings::instance().ihc;
        kv(tr("Cell counting"),
           ihs.nuclearMarker
               ? tr("nuclear marker: nuclei (H or DAB) of ~%1 µm found by band-pass blob detection; a nucleus is positive "
                    "if its mean DAB exceeds the threshold (labelling index)")
                     .arg(ihs.nucleusDiameterUm, 0, 'f', 1)
               : tr("cytoplasmic / membranous marker: nuclei of ~%1 µm found from haematoxylin; a cell is positive if the "
                    "mean DAB in a ring around its nucleus exceeds the threshold")
                     .arg(ihs.nucleusDiameterUm, 0, 'f', 1));
    }
    h += QStringLiteral("</table>");

    std::vector<double> cellPct;
    for (const auto &r : m_rows)
        if (r.cells > 0)
            cellPct.push_back(r.positiveCellPct);
    const auto pm = minmax(pos), hm = minmax(hs);
    h += QStringLiteral("<h2>%1</h2><table>").arg(tr("Summary"));
    kv(tr("Images analysed"), QString::number(pos.size()) + (pos.size() != m_rows.size()
                                                                 ? tr(" (%1 without tissue or failed)").arg(m_rows.size() - pos.size())
                                                                 : QString()));
    kv(tr("DAB positive"), tr("mean %1 % &plusmn; %2 (SD), median %3 %, range %4–%5 %")
                              .arg(mean(pos), 0, 'f', 1)
                              .arg(stddev(pos), 0, 'f', 1)
                              .arg(median(pos), 0, 'f', 1)
                              .arg(pm.first, 0, 'f', 1)
                              .arg(pm.second, 0, 'f', 1));
    kv(tr("H-score"), tr("mean %1 &plusmn; %2 (SD), median %3, range %4–%5")
                         .arg(mean(hs), 0, 'f', 0)
                         .arg(stddev(hs), 0, 'f', 0)
                         .arg(median(hs), 0, 'f', 0)
                         .arg(hm.first, 0, 'f', 0)
                         .arg(hm.second, 0, 'f', 0));
    if (!cellPct.empty()) {
        const auto cm = minmax(cellPct);
        kv(AppSettings::instance().ihc.nuclearMarker ? tr("Labelling index") : tr("Positive cells"),
           tr("mean %1 % &plusmn; %2 (SD), median %3 %, range %4–%5 %")
               .arg(mean(cellPct), 0, 'f', 1)
               .arg(stddev(cellPct), 0, 'f', 1)
               .arg(median(cellPct), 0, 'f', 1)
               .arg(cm.first, 0, 'f', 1)
               .arg(cm.second, 0, 'f', 1));
    }
    h += QStringLiteral("</table>");

    const bool anyNote = std::any_of(m_rows.begin(), m_rows.end(), [](const Row &r) { return !r.error.isEmpty() || !r.warning.isEmpty(); });
    // "N PLAN 40x/0.65" -> "40x/0.65"
    auto shortObjective = [](const QString &o) {
        const int i = o.indexOf(QRegularExpression(QStringLiteral("[0-9.]+x")));
        return i > 0 ? o.mid(i) : o;
    };
    h += QStringLiteral("<h2>%1</h2><table cellspacing='0' border='0.5' width='100%'>").arg(tr("Results"));
    h += QStringLiteral("<tr><th>%1</th><th>%2</th><th>%3</th><th class='num'>%4</th><th class='num'>%5</th>"
                        "<th class='num'>%6</th><th class='num'>%7</th><th class='num'>%8</th>%9</tr>")
             .arg(tr("Image"), tr("Objective"), tr("Region"), tr("Tissue"), tr("DAB+ %"), tr("W / M / S %"),
                  tr("H-score"), tr("DAB OD"),
                  (anyCells ? QStringLiteral("<th class='num'>%1</th><th class='num'>%2</th>").arg(tr("Cells"), tr("Pos. cells %"))
                            : QString())
                      + (anyNote ? QStringLiteral("<th>%1</th>").arg(tr("Note")) : QString()));
    for (const auto &r : m_rows) {
        const QString tissue = r.umPerPixel > 0 ? formatArea(r.tissueArea) : tr("%1 px").arg(qint64(r.tissueArea));
        auto td = [](const QString &text, bool num, bool bold = false) {
            return QStringLiteral("<td style='white-space:nowrap'") + (num ? QStringLiteral(" class='num'>") : QStringLiteral(">"))
                   + (bold ? QStringLiteral("<b>") + text + QStringLiteral("</b>") : text) + QStringLiteral("</td>");
        };
        QString row = QStringLiteral("<tr>") + td(r.file.toHtmlEscaped(), false)
                      + td(shortObjective(r.objective).toHtmlEscaped(), false) + td(r.region.toHtmlEscaped(), false)
                      + td(tissue.toHtmlEscaped(), true) + td(QString::number(r.positivePct, 'f', 1), true, true)
                      + td(QString::number(r.weakPct, 'f', 1) + QStringLiteral(" / ") + QString::number(r.moderatePct, 'f', 1)
                               + QStringLiteral(" / ") + QString::number(r.strongPct, 'f', 1),
                           true)
                      + td(QString::number(r.hScore, 'f', 0), true, true) + td(QString::number(r.meanDabPositive, 'f', 3), true);
        if (anyCells)
            row += r.cells < 0 ? td(QStringLiteral("–"), true) + td(QStringLiteral("–"), true)
                               : td(QString::number(r.cells), true) + td(QString::number(r.positiveCellPct, 'f', 1), true, true);
        if (anyNote)
            row += QStringLiteral("<td>") + (r.error.isEmpty() ? r.warning : r.error).toHtmlEscaped() + QStringLiteral("</td>");
        h += row + QStringLiteral("</tr>");
    }
    h += QStringLiteral("</table>");

    QPdfWriter pdf(path);
    pdf.setTitle(tr("IHC quantification report"));
    pdf.setCreator(QStringLiteral("DM Imaging"));
    pdf.setPageSize(QPageSize(QPageSize::A4));
    pdf.setPageMargins(QMarginsF(15, 15, 15, 12), QPageLayout::Millimeter);
    pdf.setResolution(300);
    QPainter p(&pdf);
    const double mm = pdf.resolution() / 25.4;
    const double footer = 6 * mm;
    const QRectF content(0, 0, pdf.width(), pdf.height() - footer);
    int pageNo = 1;
    auto drawFooter = [&] {
        p.save();
        QFont f(family, 7);
        p.setFont(f);
        p.setPen(QColor(120, 120, 120));
        p.drawText(QRectF(0, content.bottom() + 2 * mm, content.width(), footer - 2 * mm), Qt::AlignLeft | Qt::AlignVCenter,
                   tr("DM Imaging — IHC quantification report"));
        p.drawText(QRectF(0, content.bottom() + 2 * mm, content.width(), footer - 2 * mm), Qt::AlignRight | Qt::AlignVCenter,
                   tr("Page %1").arg(pageNo));
        p.restore();
    };
    auto newPage = [&] {
        drawFooter();
        pdf.newPage();
        ++pageNo;
    };

    // text part: laid out by QTextDocument at the printer resolution, painted page by page
    doc.documentLayout()->setPaintDevice(&pdf);
    doc.setPageSize(content.size());
    doc.setHtml(QStringLiteral("<html><body>") + h + QStringLiteral("</body></html>"));
    const int textPages = doc.pageCount();
    double y = 0;
    for (int i = 0; i < textPages; ++i) {
        if (i)
            newPage();
        p.save();
        p.translate(0, -i * content.height());
        doc.drawContents(&p, QRectF(0, i * content.height(), content.width(), content.height()));
        p.restore();
    }
    // space used on the last text page
    y = doc.documentLayout()->documentSize().height() - (textPages - 1) * content.height() + 6 * mm;

    // image part: each block (title + image + overlay) is kept on one page
    QFont titleFont(family, 10, QFont::Bold), infoFont(family, 8);
    const double gap = 4 * mm, imgW = (content.width() - gap) / 2;
    const double titleH = 7 * mm;
    auto sectionTitle = [&] {
        p.setFont(QFont(family, 12, QFont::Bold));
        p.setPen(QColor(0x1b, 0x4f, 0x8a));
        p.drawText(QRectF(0, y, content.width(), 8 * mm), Qt::AlignLeft | Qt::AlignVCenter, tr("Images"));
        y += 8 * mm;
        p.setFont(infoFont);
        p.setPen(QColor(110, 110, 110));
        p.drawText(QRectF(0, y, content.width(), 6 * mm), Qt::AlignLeft | Qt::AlignVCenter,
                   tr("Left: image. Right: analysis overlay (red = DAB positive, blue = negative tissue, yellow = analysed regions)."));
        y += 8 * mm;
    };
    bool titled = false;
    for (const auto &r : m_rows) {
        const QImage thumb = QImage::fromData(r.thumbJpeg, "JPG"), overlayThumb = QImage::fromData(r.overlayJpeg, "JPG");
        if (thumb.isNull())
            continue;
        const double imgH = imgW * double(thumb.height()) / std::max(1, thumb.width());
        const double blockH = titleH + imgH + 5 * mm;
        const double need = blockH + (titled ? 0 : 16 * mm);
        if (y + need > content.height()) {
            newPage();
            y = 0;
        }
        if (!titled) {
            sectionTitle();
            titled = true;
        }
        p.setFont(titleFont);
        p.setPen(QColor(30, 30, 30));
        const QString title = r.file;
        p.drawText(QRectF(0, y, content.width(), titleH), Qt::AlignLeft | Qt::AlignVCenter, title);
        const double tw = QFontMetricsF(titleFont, &pdf).horizontalAdvance(title);
        p.setFont(infoFont);
        p.setPen(QColor(110, 110, 110));
        p.drawText(QRectF(tw + 3 * mm, y, content.width() - tw - 3 * mm, titleH), Qt::AlignLeft | Qt::AlignVCenter,
                   r.objective + tr(" · DAB+ %1 % · H-score %2").arg(r.positivePct, 0, 'f', 1).arg(r.hScore, 0, 'f', 0)
                       + (r.cells >= 0 ? tr(" · %1 cells, %2 % positive").arg(r.cells).arg(r.positiveCellPct, 0, 'f', 1)
                                       : QString())
                       + (r.region.isEmpty() ? QString() : QStringLiteral(" · ") + r.region));
        y += titleH;
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(QRectF(0, y, imgW, imgH), thumb);
        p.drawImage(QRectF(imgW + gap, y, imgW, imgH), overlayThumb);
        p.setPen(QPen(QColor(200, 200, 200), 0.3 * mm));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(0, y, imgW, imgH));
        p.drawRect(QRectF(imgW + gap, y, imgW, imgH));
        y += imgH + 5 * mm;
    }
    drawFooter();
    p.end();
    QApplication::restoreOverrideCursor();
    if (!QFileInfo(path).exists() || QFileInfo(path).size() == 0) {
        m_status->setText(tr("Cannot write %1").arg(path));
        return;
    }
    m_status->setTextFormat(Qt::RichText);
    m_exported = true;
    m_status->setText(tr("Report saved: <a href=\"%1\">%2</a>")
                          .arg(QUrl::fromLocalFile(path).toString(), QDir::toNativeSeparators(path).toHtmlEscaped()));
}

void BatchIhcDialog::exportCsv()
{
    const QString def = QFileInfo(m_files.value(0)).dir().filePath(QStringLiteral("ihc_quantification.csv"));
    const QString path = lm::getSaveFileName(this, tr("Export IHC results"), def, tr("CSV (*.csv)"));
    if (path.isEmpty())
        return;
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        m_status->setText(tr("Cannot write %1").arg(path));
        return;
    }
    f.write("\xEF\xBB\xBF"); // UTF-8 BOM so Excel shows µ correctly
    f.write(csv().toUtf8());
    if (!f.commit()) {
        m_status->setText(tr("Cannot write %1").arg(path));
        return;
    }
    m_exported = true;
    m_status->setText(tr("Saved %1").arg(QDir::toNativeSeparators(path)));
}

} // namespace lm
