#include "ui/Theme.h"
#include "BatchExportDialog.h"

#include "app/AppSettings.h"
#include "io/ImageIO.h"
#include "ui/Annotations.h"
#include "ui/Overlays.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>
#include <QUrl>
#include <QThread>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace lm {

BatchExportDialog::BatchExportDialog(const QStringList &files, QWidget *parent) : QDialog(parent), m_files(files)
{
    setWindowTitle(tr("Export %n image(s)", nullptr, int(files.size())));
    resize(px(560), px(360));
    auto *lay = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    m_format = new QComboBox(this);
    m_format->addItem(tr("JPEG (presentations)"), int(FileFormat::Jpeg));
    m_format->addItem(tr("PNG (lossless)"), int(FileFormat::Png));
    m_format->addItem(tr("TIFF 8-bit"), int(FileFormat::Tiff));
    form->addRow(tr("Format"), m_format);
    m_quality = new QSpinBox(this);
    m_quality->setRange(50, 100);
    m_quality->setValue(92);
    m_quality->setSuffix(QStringLiteral(" %"));
    form->addRow(tr("JPEG quality"), m_quality);
    m_maxWidth = new QSpinBox(this);
    m_maxWidth->setRange(0, 20000);
    m_maxWidth->setSingleStep(100);
    m_maxWidth->setValue(0);
    m_maxWidth->setSpecialValueText(tr("Original size"));
    m_maxWidth->setSuffix(tr(" px"));
    form->addRow(tr("Maximum width"), m_maxWidth);
    m_scaleBar = new QCheckBox(tr("Burn in scale bar"), this);
    m_scaleBar->setChecked(true);
    form->addRow(QString(), m_scaleBar);
    m_annotations = new QCheckBox(tr("Burn in annotations and measurements"), this);
    m_annotations->setChecked(true);
    form->addRow(QString(), m_annotations);
    auto *fr = new QHBoxLayout;
    m_folder = new QLineEdit(QFileInfo(files.value(0)).dir().filePath(QStringLiteral("export")), this);
    auto *browse = new QToolButton(this);
    browse->setText(QStringLiteral("…"));
    fr->addWidget(m_folder, 1);
    fr->addWidget(browse);
    form->addRow(tr("Output folder"), fr);
    lay->addLayout(form);
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, int(files.size()));
    m_progress->setValue(0);
    lay->addWidget(m_progress);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("Hint"));
    m_status->setWordWrap(true);
    lay->addWidget(m_status);
    lay->addStretch();
    auto *bb = new QDialogButtonBox(this);
    m_start = bb->addButton(tr("Export"), QDialogButtonBox::AcceptRole);
    bb->addButton(QDialogButtonBox::Close);
    lay->addWidget(bb);

    connect(browse, &QToolButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, tr("Output folder"), m_folder->text());
        if (!d.isEmpty())
            m_folder->setText(d);
    });
    connect(m_format, &QComboBox::currentIndexChanged, this, [this] {
        m_quality->setEnabled(FileFormat(m_format->currentData().toInt()) == FileFormat::Jpeg);
    });
    connect(bb, &QDialogButtonBox::accepted, this, &BatchExportDialog::run);
    connect(m_status, &QLabel::linkActivated, this, [](const QString &url) { QDesktopServices::openUrl(QUrl(url)); });
    connect(bb, &QDialogButtonBox::rejected, this, [this] {
        if (m_running)
            m_cancel = true;
        else
            reject();
    });
}

void BatchExportDialog::reject()
{
    // Esc / window close during an export cancels it instead of hiding a running dialog
    if (m_running)
        m_cancel = true;
    else
        QDialog::reject();
}

void BatchExportDialog::run()
{
    if (m_running)
        return;
    const QString outDir = m_folder->text();
    if (!QDir().mkpath(outDir)) {
        m_status->setText(tr("Cannot create %1").arg(outDir));
        return;
    }
    m_running = true;
    m_cancel = false;
    m_start->setEnabled(false);
    const FileFormat fmt = FileFormat(m_format->currentData().toInt());
    const int quality = m_quality->value();
    const int maxW = m_maxWidth->value();
    const bool scale = m_scaleBar->isChecked(), ann = m_annotations->isChecked();
    const OverlaySettings ov = AppSettings::instance().overlays;
    int ok = 0, failed = 0;
    for (int i = 0; i < m_files.size() && !m_cancel; ++i) {
        const QString src = m_files[i];
        m_status->setText(tr("Exporting %1…").arg(QFileInfo(src).fileName()));
        // decode + render in the background, keep the dialog responsive
        auto future = QtConcurrent::run([=]() -> QString {
            LoadedImage li;
            QString err;
            if (!loadImage(src, li, &err))
                return err.isEmpty() ? tr("cannot read") : err;
            QImage img = toQImage8(li.data);
            ImageMetadata meta = li.meta;
            if (ann || (scale && meta.umPerPixel > 0)) {
                QPainter p(&img);
                p.setRenderHint(QPainter::Antialiasing);
                if (ann) {
                    AnnotationLayer layer;
                    layer.setUmPerPixel(meta.umPerPixel);
                    layer.setImageSize(img.size());
                    if (layer.loadSidecar(src))
                        layer.paint(p, QTransform(), 1.0, true);
                }
                if (scale && meta.umPerPixel > 0)
                    drawScaleBar(p, QRectF(0, 0, img.width(), img.height()), 1.0 / meta.umPerPixel, ov);
            }
            if (maxW > 0 && img.width() > maxW) {
                const double f = double(maxW) / img.width();
                img = img.scaledToWidth(maxW, Qt::SmoothTransformation);
                if (meta.umPerPixel > 0)
                    meta.umPerPixel /= f;
            }
            SaveOptions opt;
            opt.format = fmt;
            opt.sixteenBit = false;
            opt.jpegQuality = quality;
            opt.writeSidecar = false;
            QString base = QFileInfo(src).completeBaseName();
            QString dst = QDir(outDir).filePath(base + QLatin1Char('.') + extensionFor(fmt));
            for (int n = 2; QFileInfo::exists(dst); ++n)
                dst = QDir(outDir).filePath(QStringLiteral("%1_%2.%3").arg(base).arg(n).arg(extensionFor(fmt)));
            if (!saveImage(dst, img, meta, opt, &err))
                return err;
            return QString();
        });
        while (!future.isFinished()) {
            QApplication::processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(10);
        }
        QString result;
        try {
            result = future.result(); // rethrows an exception from the worker (e.g. out of memory)
        } catch (const std::exception &e) {
            result = QString::fromUtf8(e.what());
        } catch (...) {
            result = tr("export failed");
        }
        if (result.isEmpty())
            ++ok;
        else
            ++failed;
        m_progress->setValue(i + 1);
    }
    m_running = false;
    m_start->setEnabled(true);
    m_status->setText(tr("%1 exported, %2 failed%3 — <a href=\"%4\">open folder</a>")
                          .arg(ok)
                          .arg(failed)
                          .arg(m_cancel ? tr(" (cancelled)") : QString())
                          .arg(QUrl::fromLocalFile(outDir).toString()));
    m_status->setTextFormat(Qt::RichText);
}

} // namespace lm
