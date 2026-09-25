#include "BatchIhcDialog.h"

#include "imaging/StainAnalysis.h"
#include "io/ImageIO.h"
#include "ui/Annotations.h"
#include "ui/Overlays.h"

#include <QApplication>
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
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QTableWidget>
#include <QThread>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <cmath>

namespace lm {

namespace {

enum Col { ColImage, ColObjective, ColRegion, ColTissue, ColPositive, ColWeak, ColModerate, ColStrong, ColHScore,
           ColMeanOd, ColCount };

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
    resize(1000, 600);
    auto *lay = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    m_threshold = new QDoubleSpinBox(this);
    m_threshold->setRange(0.05, 1.0);
    m_threshold->setSingleStep(0.01);
    m_threshold->setDecimals(2);
    m_threshold->setValue(0.15);
    m_threshold->setToolTip(tr("DAB optical density above which a pixel counts as positive (same as in Process)"));
    form->addRow(tr("DAB positivity threshold (OD)"), m_threshold);
    m_useRegions = new QCheckBox(tr("Analyse only the rectangle / ellipse / area annotations of an image, if it has any"),
                                 this);
    m_useRegions->setChecked(true);
    form->addRow(QString(), m_useRegions);
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
                                        tr("Mean DAB OD")});
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
    // tab separated for pasting into Excel
    connect(m_copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(QString(csv()).replace(QLatin1Char(','), QLatin1Char('\t'))); });
    connect(bb, &QDialogButtonBox::rejected, this, [this] {
        if (m_running)
            m_cancel = true;
        else
            reject();
    });
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
    m_rows.clear();
    m_table->setRowCount(0);
    m_progress->setValue(0);
    StainOptions opt;
    opt.dabThreshold = m_threshold->value();
    const bool useRegions = m_useRegions->isChecked();

    for (int i = 0; i < m_files.size() && !m_cancel; ++i) {
        const QString src = m_files[i];
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
            if (r.tissuePixels == 0)
                row.error = tr("no tissue found");
            if (saveOverlays) {
                QImage ov = overlayImage(img, r);
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
                        for (const auto &a : layer.annotations()) {
                            if (a.type == Annotation::Rectangle)
                                p.drawRect(QRectF(a.pts[0], a.pts[1]).normalized());
                            else if (a.type == Annotation::Ellipse)
                                p.drawEllipse(QRectF(a.pts[0], a.pts[1]).normalized());
                            else if (a.type == Annotation::Polygon && a.pts.size() >= 3)
                                p.drawPolygon(QPolygonF(a.pts));
                        }
                }
                SaveOptions so;
                so.format = FileFormat::Jpeg;
                so.sixteenBit = false;
                so.jpegQuality = 90;
                so.writeSidecar = false;
                const QString dst = QDir(outDir).filePath(QFileInfo(src).completeBaseName() + QStringLiteral("_ihc.jpg"));
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
        const Row row = future.result();
        m_rows.push_back(row);
        addRow(row);
        m_progress->setValue(i + 1);
    }
    m_running = false;
    m_start->setEnabled(true);
    m_export->setEnabled(!m_rows.empty());
    m_copy->setEnabled(!m_rows.empty());

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
        s += QStringLiteral(" — <a href=\"open\">%1</a>").arg(tr("open overlay folder"));
    m_status->setTextFormat(Qt::RichText);
    m_status->setText(s);
    connect(m_status, &QLabel::linkActivated, this, [outDir] { QDesktopServices::openUrl(QUrl::fromLocalFile(outDir)); },
            Qt::UniqueConnection);
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
    const bool cal = r.umPerPixel > 0;
    set(ColTissue, cal ? formatArea(r.tissueArea) : tr("%1 px").arg(qint64(r.tissueArea)));
    set(ColPositive, QString::number(r.positivePct, 'f', 1));
    set(ColWeak, QString::number(r.weakPct, 'f', 1));
    set(ColModerate, QString::number(r.moderatePct, 'f', 1));
    set(ColStrong, QString::number(r.strongPct, 'f', 1));
    set(ColHScore, QString::number(r.hScore, 'f', 0));
    set(ColMeanOd, QString::number(r.meanDabPositive, 'f', 3));
    m_table->scrollToBottom();
}

QString BatchIhcDialog::csv() const
{
    auto q = [](QString s) {
        if (s.contains(QLatin1Char(',')) || s.contains(QLatin1Char('"')))
            s = QLatin1Char('"') + s.replace(QLatin1Char('"'), QStringLiteral("\"\"")) + QLatin1Char('"');
        return s;
    };
    QString out = QStringLiteral("image,objective,region,dab_threshold_od,tissue_area,positive_area,area_unit,"
                                 "dab_positive_pct,weak_pct,moderate_pct,strong_pct,h_score,mean_dab_od_positive,"
                                 "note\n");
    for (const auto &r : m_rows)
        out += QStringLiteral("%1,%2,%3,%4,%5,%6,%7,%8,%9,%10,%11,%12,%13,%14\n")
                   .arg(q(r.file), q(r.objective), q(r.region))
                   .arg(m_threshold->value(), 0, 'f', 2)
                   .arg(r.tissueArea, 0, 'f', r.umPerPixel > 0 ? 1 : 0)
                   .arg(r.positiveArea, 0, 'f', r.umPerPixel > 0 ? 1 : 0)
                   .arg(r.umPerPixel > 0 ? QStringLiteral("um2") : QStringLiteral("px"))
                   .arg(r.positivePct, 0, 'f', 2)
                   .arg(r.weakPct, 0, 'f', 2)
                   .arg(r.moderatePct, 0, 'f', 2)
                   .arg(r.strongPct, 0, 'f', 2)
                   .arg(r.hScore, 0, 'f', 1)
                   .arg(r.meanDabPositive, 0, 'f', 4)
                   .arg(q(r.error));
    return out;
}

void BatchIhcDialog::exportCsv()
{
    const QString def = QFileInfo(m_files.value(0)).dir().filePath(QStringLiteral("ihc_quantification.csv"));
    const QString path = QFileDialog::getSaveFileName(this, tr("Export IHC results"), def, tr("CSV (*.csv)"));
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
    m_status->setText(tr("Saved %1").arg(QDir::toNativeSeparators(path)));
}

} // namespace lm
