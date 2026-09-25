#include "ProcessPage.h"

#include "app/AppSettings.h"
#include "imaging/FocusStacker.h"
#include "imaging/MosaicBuilder.h"
#include "imaging/NucleusDetection.h"
#include "imaging/StainAnalysis.h"
#include "app/AppSettings.h"
#include "ui/Annotations.h"
#include "ui/CollapsibleSection.h"
#include "ui/ImageView.h"
#include "ui/Overlays.h"
#include "ui/SliderSpin.h"

#include <QActionGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QApplication>
#include <QClipboard>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPrintDialog>
#include <QPrinter>
#include <QProgressDialog>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSplitter>
#include <QTableWidget>
#include <QTextStream>
#include <QIcon>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

namespace lm {

namespace {
const char *kFileFilter = "Images (*.tif *.tiff *.png *.jpg *.jpeg *.bmp);;All files (*.*)";
QString typeLabel(Annotation::Type t)
{
    switch (t) {
    case Annotation::Line: return QObject::tr("Length");
    case Annotation::Arrow: return QObject::tr("Arrow");
    case Annotation::Polyline: return QObject::tr("Path");
    case Annotation::Rectangle: return QObject::tr("Rectangle");
    case Annotation::Ellipse: return QObject::tr("Ellipse");
    case Annotation::Polygon: return QObject::tr("Area");
    case Annotation::Angle: return QObject::tr("Angle");
    case Annotation::Text: return QObject::tr("Text");
    case Annotation::Count: return QObject::tr("Count");
    }
    return {};
}
} // namespace

ProcessPage::ProcessPage(QWidget *parent) : QWidget(parent)
{
    m_adjust.srgbEncode = false; // saved images are already display referred

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- toolbar
    auto *tb = new QToolBar(this);
    tb->setToolButtonStyle(Qt::ToolButtonTextOnly);
    tb->addAction(tr("Open…"), this, &ProcessPage::openDialog);
    tb->addAction(tr("Save as…"), this, &ProcessPage::saveAs);
    tb->addAction(tr("Export with overlays…"), this, &ProcessPage::exportWithOverlays);
    tb->addAction(tr("Copy"), this, &ProcessPage::copyToClipboard);
    tb->addAction(tr("Print…"), this, &ProcessPage::print);
    tb->addSeparator();

    m_layer = new AnnotationLayer(this);
    m_tools = new QActionGroup(this);
    m_tools->setExclusive(true);
    struct T { const char *label; const char *tip; AnnotationLayer::Tool tool; const char *icon; };
    const T tools[] = {
        {"Select", "Select, move and edit annotations; drag the image to pan", AnnotationLayer::SelectTool, "select"},
        {"Line", "Measure a distance", AnnotationLayer::LineTool, "line"},
        {"Path", "Measure a curved length (double-click to finish)", AnnotationLayer::PolylineTool, "path"},
        {"Rect", "Rectangle with area", AnnotationLayer::RectTool, "rect"},
        {"Ellipse", "Ellipse / circle with area", AnnotationLayer::EllipseTool, "ellipse"},
        {"Area", "Polygon area (double-click to finish)", AnnotationLayer::PolygonTool, "area"},
        {"Angle", "Measure an angle (3 clicks)", AnnotationLayer::AngleTool, "angle"},
        {"Count", "Count objects (click to add, right-click to remove)", AnnotationLayer::CountTool, "count"},
        {"Arrow", "Arrow annotation", AnnotationLayer::ArrowTool, "arrow"},
        {"Text", "Text label", AnnotationLayer::TextTool, "text"},
    };
    tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    tb->setIconSize(QSize(20, 20));
    for (const auto &t : tools) {
        QAction *a = tb->addAction(QIcon(QStringLiteral(":/icons/tool_%1.svg").arg(QLatin1String(t.icon))), tr(t.label));
        a->setToolTip(tr(t.tip));
        a->setCheckable(true);
        a->setData(int(t.tool));
        m_tools->addAction(a);
        if (t.tool == AnnotationLayer::SelectTool)
            a->setChecked(true);
    }
    connect(m_tools, &QActionGroup::triggered, this, [this](QAction *a) {
        m_layer->setTool(AnnotationLayer::Tool(a->data().toInt()));
        m_view->setCursor(a->data().toInt() == AnnotationLayer::SelectTool ? Qt::ArrowCursor : Qt::CrossCursor);
    });
    tb->addSeparator();
    m_colorBtn = new QToolButton(this);
    m_colorBtn->setToolTip(tr("Annotation colour"));
    auto setBtn = [this](const QColor &c) {
        m_colorBtn->setStyleSheet(QStringLiteral("QToolButton{background:%1; min-width:22px; border-radius:3px;}").arg(c.name()));
    };
    setBtn(m_layer->color());
    connect(m_colorBtn, &QToolButton::clicked, this, [this, setBtn] {
        const QColor c = QColorDialog::getColor(m_layer->color(), this, tr("Annotation colour"));
        if (c.isValid()) {
            m_layer->setColor(c);
            setBtn(c);
        }
    });
    tb->addWidget(m_colorBtn);
    auto *width = new QDoubleSpinBox(this);
    width->setRange(0.5, 12);
    width->setValue(2.0);
    width->setSingleStep(0.5);
    width->setToolTip(tr("Line width"));
    connect(width, &QDoubleSpinBox::valueChanged, m_layer, &AnnotationLayer::setLineWidth);
    tb->addWidget(width);
    tb->addAction(tr("Undo"), m_layer, &AnnotationLayer::undo)->setShortcut(QKeySequence::Undo);
    tb->addAction(tr("Redo"), m_layer, &AnnotationLayer::redo)->setShortcut(QKeySequence::Redo);
    tb->addAction(tr("Delete"), m_layer, &AnnotationLayer::removeSelected);
    tb->addAction(tr("Clear all"), this, [this] {
        if (!m_layer->annotations().isEmpty()
            && QMessageBox::question(this, tr("Clear"), tr("Remove all annotations?")) == QMessageBox::Yes)
            m_layer->clear();
    });
    root->addWidget(tb);

    // ---- content
    auto *split = new QSplitter(Qt::Horizontal, this);
    m_view = new ImageView(split);
    m_view->setPlaceholder(tr("Open an image (Ctrl+O) or capture one in Acquire"));
    m_view->setAnnotationLayer(m_layer);

    auto *side = new QScrollArea(split);
    side->setObjectName(QStringLiteral("PanelScroll"));
    side->setWidgetResizable(true);
    side->setMinimumWidth(300);
    auto *sideContent = new QWidget(side);
    sideContent->setObjectName(QStringLiteral("PanelContent"));
    auto *sl = new QVBoxLayout(sideContent);
    sl->setContentsMargins(0, 0, 0, 0);
    sl->setSpacing(0);

    auto *ms = new CollapsibleSection(tr("Measurements"), sideContent);
    m_table = new QTableWidget(0, 2, sideContent);
    m_table->setHorizontalHeaderLabels({tr("Type"), tr("Result")});
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setMinimumHeight(180);
    ms->contentLayout()->addWidget(m_table);
    auto *csv = new QPushButton(tr("Export measurements (CSV)…"), sideContent);
    ms->contentLayout()->addWidget(csv);
    sl->addWidget(ms);

    auto *adj = new CollapsibleSection(tr("Adjust"), sideContent);
    m_brightness = new SliderSpin(tr("Brightness"), -0.5, 0.5, 3, sideContent);
    m_contrast = new SliderSpin(tr("Contrast"), 0.3, 3.0, 2, sideContent, true);
    m_gamma = new SliderSpin(tr("Gamma"), 0.3, 3.0, 2, sideContent, true);
    m_saturation = new SliderSpin(tr("Saturation"), 0.0, 3.0, 2, sideContent);
    m_sharpen = new SliderSpin(tr("Sharpen"), 0.0, 3.0, 2, sideContent);
    const double defs[] = {0.0, 1.0, 1.0, 1.0, 0.0};
    SliderSpin *sliders[] = {m_brightness, m_contrast, m_gamma, m_saturation, m_sharpen};
    for (int i = 0; i < 5; ++i) {
        sliders[i]->setValue(defs[i]);
        sliders[i]->setDefault(defs[i]);
        adj->contentLayout()->addWidget(sliders[i]);
        connect(sliders[i], &SliderSpin::valueChanged, this, [this] {
            m_adjust.brightness = m_brightness->value();
            m_adjust.contrast = m_contrast->value();
            m_adjust.gamma = m_gamma->value();
            m_adjust.saturation = m_saturation->value();
            m_adjust.sharpenAmount = m_sharpen->value();
            rerender();
        });
    }
    auto *resetAdj = new QPushButton(tr("Reset adjustments"), sideContent);
    adj->contentLayout()->addWidget(resetAdj);
    sl->addWidget(adj);

    auto *ihc = new CollapsibleSection(tr("IHC quantification (DAB)"), sideContent);
    auto *ihcHint = new QLabel(tr("Colour deconvolution into haematoxylin and DAB. Analyses the whole image, or the "
                                  "selected rectangle / ellipse / area annotation."), sideContent);
    ihcHint->setObjectName(QStringLiteral("Hint"));
    ihcHint->setWordWrap(true);
    ihc->contentLayout()->addWidget(ihcHint);
    m_dabThreshold = new SliderSpin(tr("DAB positivity threshold (OD)"), 0.05, 1.0, 2, sideContent);
    m_dabThreshold->setValue(AppSettings::instance().ihc.dabThreshold);
    m_dabThreshold->setDefault(0.15);
    ihc->contentLayout()->addWidget(m_dabThreshold);
    connect(m_dabThreshold, &SliderSpin::valueChanged, this, [](double v) { AppSettings::instance().ihc.dabThreshold = v; });
    // stain vectors: standard H-DAB, or estimated from a representative image of this staining
    m_stainLabel = new QLabel(sideContent);
    m_stainLabel->setObjectName(QStringLiteral("Hint"));
    m_stainLabel->setWordWrap(true);
    ihc->contentLayout()->addWidget(m_stainLabel);
    auto *stainRow = new QHBoxLayout;
    auto *stainEst = new QPushButton(tr("Estimate stain colours"), sideContent);
    stainEst->setToolTip(tr("Measure the haematoxylin and DAB colours of this staining from the current image "
                            "(needs both stains in view). Used for all IHC analyses until reset."));
    auto *stainStd = new QPushButton(tr("Standard"), sideContent);
    stainStd->setToolTip(tr("Use the standard haematoxylin-DAB colour vectors (Ruifrok & Johnston)"));
    stainRow->addWidget(stainEst, 1);
    stainRow->addWidget(stainStd);
    ihc->contentLayout()->addLayout(stainRow);
    updateStainLabel();
    connect(stainEst, &QPushButton::clicked, this, [this] {
        if (m_data.empty())
            return;
        StainVectors v;
        std::string msg;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const bool ok = estimateStainVectors(m_data, v, true, &msg);
        QApplication::restoreOverrideCursor();
        if (!ok) {
            QMessageBox::information(this, tr("Estimate stain colours"),
                                     tr("The stain colours could not be estimated: %1.").arg(QString::fromStdString(msg)));
            return;
        }
        auto &ih = AppSettings::instance().ihc;
        for (int c = 0; c < 3; ++c) {
            ih.h[c] = v.h[c];
            ih.dab[c] = v.dab[c];
        }
        ih.customVectors = true;
        ih.vectorSource = QFileInfo(m_path).fileName();
        AppSettings::instance().save();
        updateStainLabel();
        emit message(tr("Stain colours estimated from %1").arg(ih.vectorSource), 5000);
    });
    connect(stainStd, &QPushButton::clicked, this, [this] {
        auto &ih = AppSettings::instance().ihc;
        const StainVectors def;
        for (int c = 0; c < 3; ++c) {
            ih.h[c] = def.h[c];
            ih.dab[c] = def.dab[c];
        }
        ih.customVectors = false;
        ih.vectorSource.clear();
        AppSettings::instance().save();
        updateStainLabel();
    });
    auto *ihcRow = new QHBoxLayout;
    auto *ihcAll = new QPushButton(tr("Analyse image"), sideContent);
    auto *ihcSel = new QPushButton(tr("Analyse selection"), sideContent);
    ihcRow->addWidget(ihcAll);
    ihcRow->addWidget(ihcSel);
    ihc->contentLayout()->addLayout(ihcRow);
    // nucleus counting (nuclear markers such as Ki-67, p53, ER/PR)
    m_nucleusDiameter = new SliderSpin(tr("Nucleus diameter (µm)"), 3.0, 20.0, 1, sideContent);
    m_nucleusDiameter->setValue(AppSettings::instance().ihc.nucleusDiameterUm);
    m_nucleusDiameter->setDefault(7.0);
    ihc->contentLayout()->addWidget(m_nucleusDiameter);
    connect(m_nucleusDiameter, &SliderSpin::valueChanged, this,
            [](double v) { AppSettings::instance().ihc.nucleusDiameterUm = v; });
    auto *nucForm = new QFormLayout;
    auto *marker = new QComboBox(sideContent);
    marker->addItem(tr("Nuclear (Ki-67, p53, ER/PR)"));
    marker->addItem(tr("Cytoplasmic / membranous"));
    marker->setToolTip(tr("Nuclear: a nucleus is positive if it is DAB stained. Cytoplasmic / membranous: nuclei are "
                          "found from haematoxylin and a cell is positive if the DAB around its nucleus exceeds the threshold."));
    marker->setCurrentIndex(AppSettings::instance().ihc.nuclearMarker ? 0 : 1);
    nucForm->addRow(tr("Marker"), marker);
    auto *sens = new QComboBox(sideContent);
    sens->addItems({tr("Low (strongly stained nuclei)"), tr("Normal"), tr("High (also pale nuclei)")});
    sens->setCurrentIndex(AppSettings::instance().ihc.nucleusSensitivity);
    nucForm->addRow(tr("Sensitivity"), sens);
    ihc->contentLayout()->addLayout(nucForm);
    connect(marker, &QComboBox::currentIndexChanged, this, [](int i) { AppSettings::instance().ihc.nuclearMarker = i == 0; });
    connect(sens, &QComboBox::currentIndexChanged, this, [](int i) { AppSettings::instance().ihc.nucleusSensitivity = i; });
    auto *nucRow = new QHBoxLayout;
    auto *nucAll = new QPushButton(tr("Count nuclei"), sideContent);
    nucAll->setToolTip(tr("Count haematoxylin (negative) and DAB (positive) nuclei: labelling index for nuclear "
                          "markers such as Ki-67, p53 or ER/PR"));
    auto *nucSel = new QPushButton(tr("Count in selection"), sideContent);
    nucRow->addWidget(nucAll);
    nucRow->addWidget(nucSel);
    ihc->contentLayout()->addLayout(nucRow);
    connect(nucAll, &QPushButton::clicked, this, [this] { analyzeIhc(false, true); });
    connect(nucSel, &QPushButton::clicked, this, [this] { analyzeIhc(true, true); });
    m_ihcOverlay = new QCheckBox(tr("Show overlay (red = DAB+, blue = negative tissue)"), sideContent);
    m_ihcOverlay->setChecked(true);
    ihc->contentLayout()->addWidget(m_ihcOverlay);
    m_ihcResult = new QLabel(sideContent);
    m_ihcResult->setWordWrap(true);
    m_ihcResult->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ihc->contentLayout()->addWidget(m_ihcResult);
    auto *ihcCopy = new QPushButton(tr("Copy results"), sideContent);
    ihc->contentLayout()->addWidget(ihcCopy);
    sl->addWidget(ihc);
    connect(ihcAll, &QPushButton::clicked, this, [this] { analyzeIhc(false); });
    connect(ihcSel, &QPushButton::clicked, this, [this] { analyzeIhc(true); });
    connect(m_ihcOverlay, &QCheckBox::toggled, this, [this](bool on) { m_view->setOverlayImage(on ? m_ihcMask : QImage()); });
    connect(ihcCopy, &QPushButton::clicked, this, [this] {
        if (!m_ihcText.isEmpty())
            QApplication::clipboard()->setText(m_ihcText);
    });

    auto *inf = new CollapsibleSection(tr("Image information"), sideContent);
    m_info = new QLabel(sideContent);
    m_info->setObjectName(QStringLiteral("Hint"));
    m_info->setWordWrap(true);
    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    inf->contentLayout()->addWidget(m_info);
    auto *cal = new QPushButton(tr("Set pixel size…"), sideContent);
    inf->contentLayout()->addWidget(cal);
    sl->addWidget(inf);
    sl->addStretch();
    side->setWidget(sideContent);
    split->setStretchFactor(0, 4);
    split->setStretchFactor(1, 1);
    root->addWidget(split, 1);

    connect(resetAdj, &QPushButton::clicked, this, [=] {
        for (int i = 0; i < 5; ++i)
            sliders[i]->setValue(defs[i]);
        m_adjust = ColorSettings();
        m_adjust.srgbEncode = false;
        rerender();
    });
    connect(cal, &QPushButton::clicked, this, &ProcessPage::setCalibration);
    connect(csv, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getSaveFileName(this, tr("Export measurements"),
                                                       QFileInfo(m_path).dir().filePath(QFileInfo(m_path).completeBaseName() + QStringLiteral("_measurements.csv")),
                                                       tr("CSV (*.csv)"));
        if (f.isEmpty())
            return;
        QSaveFile out(f);
        if (!out.open(QIODevice::WriteOnly))
            return;
        QTextStream ts(&out);
        ts << "image,index,type,length_um,area_um2,angle_deg,count,width_um,height_um,summary\n";
        int i = 1;
        for (const auto &a : m_layer->annotations()) {
            const Measurement m = m_layer->measure(a);
            ts << '"' << QFileInfo(m_path).fileName() << "\"," << i++ << ',' << typeLabel(a.type) << ',' << m.lengthUm << ','
               << m.areaUm2 << ',' << m.angleDeg << ',' << m.count << ',' << m.widthUm << ',' << m.heightUm << ",\""
               << QString(m.summary).replace(QLatin1Char('"'), QLatin1Char('\'')) << "\"\n";
        }
        out.commit();
        emit message(tr("Measurements exported to %1").arg(f), 5000);
    });
    connect(m_table, &QTableWidget::cellClicked, this, [this](int row, int) {
        if (row >= 0 && row < m_layer->annotations().size())
            m_layer->select(m_layer->annotations()[row].id);
    });
    connect(m_layer, &AnnotationLayer::changed, this, [this] {
        updateMeasurements();
        m_dirtyAnnotations = true;
        saveAnnotations();
    });
    connect(m_layer, &AnnotationLayer::textRequested, this, [this](const QPointF &pos) {
        bool ok = false;
        const QString t = QInputDialog::getText(this, tr("Text annotation"), tr("Text:"), QLineEdit::Normal, QString(), &ok);
        if (ok)
            m_layer->addText(pos, t);
    });
    connect(m_layer, &AnnotationLayer::editTextRequested, this, [this](int id) {
        for (const auto &a : m_layer->annotations())
            if (a.id == id) {
                bool ok = false;
                const QString t = QInputDialog::getText(this, tr("Edit text"), tr("Text:"), QLineEdit::Normal, a.text, &ok);
                if (ok)
                    m_layer->setAnnotationText(id, t);
                return;
            }
    });
    connect(m_view, &ImageView::contextMenuRequested, this, [this](const QPoint &gp) {
        QMenu m(this);
        m.addAction(tr("Fit to window"), m_view, &ImageView::zoomFit);
        m.addAction(tr("Actual pixels (100%)"), m_view, &ImageView::zoomActual);
        m.addSeparator();
        m.addAction(tr("Copy image"), this, &ProcessPage::copyToClipboard);
        m.addAction(tr("Export with overlays…"), this, &ProcessPage::exportWithOverlays);
        m.exec(gp);
    });
}

void ProcessPage::openDialog()
{
    const QString f = QFileDialog::getOpenFileName(this, tr("Open image"), AppSettings::instance().browseFolder, tr(kFileFilter));
    if (!f.isEmpty())
        openFile(f);
}

bool ProcessPage::openFile(const QString &path)
{
    LoadedImage li;
    QString err;
    if (!loadImage(path, li, &err)) {
        QMessageBox::warning(this, tr("Open image"), tr("Cannot open %1:\n%2").arg(path, err));
        return false;
    }
    openImage(li.data, li.meta, path);
    return true;
}

void ProcessPage::openImage(const Image16 &img, const ImageMetadata &meta, const QString &path)
{
    m_data = img;
    m_meta = meta;
    m_path = path;
    m_ihcMask = QImage();
    m_ihcText.clear();
    m_ihcResult->clear();
    m_view->setOverlayImage(QImage());
    m_layer->blockSignals(true);
    if (!path.isEmpty())
        m_layer->loadSidecar(path);
    else
        m_layer->clear();
    m_layer->blockSignals(false);
    m_view->setUmPerPixel(meta.umPerPixel);
    rerender();
    m_view->zoomFit();
    updateMeasurements();
    updateInfo();
    emit message(path.isEmpty() ? tr("New image (not saved yet)") : tr("Opened %1").arg(QFileInfo(path).fileName()), 3000);
}

void ProcessPage::rerender()
{
    if (m_data.empty()) {
        m_view->clear();
        return;
    }
    ColorPipeline p;
    p.update(m_adjust);
    m_view->setImage(toQImage8(p.render8(m_data)));
}

void ProcessPage::updateMeasurements()
{
    const auto &items = m_layer->annotations();
    m_table->setRowCount(int(items.size()));
    for (int i = 0; i < items.size(); ++i) {
        const Measurement m = m_layer->measure(items[i]);
        m_table->setItem(i, 0, new QTableWidgetItem(QStringLiteral("%1 %2").arg(typeLabel(items[i].type)).arg(i + 1)));
        m_table->setItem(i, 1, new QTableWidgetItem(m.summary));
        if (items[i].id == m_layer->selectedId())
            m_table->selectRow(i);
    }
}

void ProcessPage::updateInfo()
{
    QStringList lines;
    if (!m_path.isEmpty())
        lines << QFileInfo(m_path).fileName();
    lines << tr("%1 × %2 px").arg(m_data.width).arg(m_data.height);
    for (const auto &kv : m_meta.describe())
        if (kv.first != tr("Image size"))
            lines << QStringLiteral("%1: %2").arg(kv.first, kv.second);
    if (m_meta.umPerPixel <= 0)
        lines << tr("Not calibrated — measurements in pixels");
    m_info->setText(lines.join(QLatin1Char('\n')));
}

void ProcessPage::saveAnnotations()
{
    if (!m_path.isEmpty() && m_dirtyAnnotations) {
        m_layer->saveSidecar(m_path);
        m_dirtyAnnotations = false;
    }
}

void ProcessPage::analyzeIhc(bool regionOnly, bool nuclei)
{
    if (m_data.empty())
        return;
    // region mask from the selected annotation
    QImage region;
    QString regionName = tr("whole image");
    if (regionOnly) {
        const Annotation *sel = nullptr;
        for (const auto &a : m_layer->annotations())
            if (a.id == m_layer->selectedId())
                sel = &a;
        if (!sel || !isRegion(*sel)) {
            QMessageBox::information(this, tr("IHC quantification"),
                                     tr("Select a rectangle, ellipse or area annotation first (Select tool), "
                                        "or use \"Analyse image\"."));
            return;
        }
        region = regionMask({sel}, QSize(m_data.width, m_data.height));
        regionName = tr("selected region");
    }
    StainOptions opt;
    opt.dabThreshold = m_dabThreshold->value();
    {
        const auto &ih = AppSettings::instance().ihc;
        for (int c = 0; c < 3; ++c) {
            opt.vectors.h[c] = ih.h[c];
            opt.vectors.dab[c] = ih.dab[c];
        }
    }
    opt.umPerPixel = m_meta.umPerPixel;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const StainResult r = region.isNull()
                              ? analyzeStains(m_data, opt)
                              : analyzeStains(m_data, opt, [&region](int x, int y) { return region.constScanLine(y)[x] != 0; });
    if (nuclei) {
        NucleusOptions no;
        no.diameterUm = m_nucleusDiameter->value();
        no.umPerPixel = m_meta.umPerPixel;
        no.diameterPx = m_meta.umPerPixel > 0 ? 0 : m_nucleusDiameter->value() * 4; // uncalibrated: assume 0.25 µm/px
        no.dabThreshold = opt.dabThreshold;
        {
            const auto &ih = AppSettings::instance().ihc;
            no.nuclearMarker = ih.nuclearMarker;
            static const double contrast[3] = {0.05, 0.03, 0.02}, stain[3] = {0.10, 0.06, 0.04};
            no.minContrast = contrast[std::clamp(ih.nucleusSensitivity, 0, 2)];
            no.minStain = stain[std::clamp(ih.nucleusSensitivity, 0, 2)];
        }
        const NucleusResult nr =
            region.isNull() ? detectNuclei(r, no)
                            : detectNuclei(r, no, [&region](int x, int y) { return region.constScanLine(y)[x] != 0; });
        // overlay: a ring per nucleus
        m_ihcMask = QImage(m_data.width, m_data.height, QImage::Format_ARGB32_Premultiplied);
        m_ihcMask.fill(Qt::transparent);
        {
            QPainter p(&m_ihcMask);
            p.setRenderHint(QPainter::Antialiasing);
            const double rad = nr.radiusPx * 0.9, lw = std::max(1.5, nr.radiusPx / 6);
            const QPen pos(QColor(230, 30, 30), lw), neg(QColor(30, 110, 255), lw);
            p.setBrush(Qt::NoBrush);
            for (const auto &n : nr.nuclei) {
                p.setPen(n.positive ? pos : neg);
                p.drawEllipse(QPointF(n.x, n.y), rad, rad);
            }
            if (!region.isNull()) {
                // outline of the counted region
                p.setPen(QPen(QColor(255, 220, 0, 200), lw));
                for (const auto &a : m_layer->annotations())
                    if (a.id == m_layer->selectedId() && isRegion(a)) {
                        if (a.type == Annotation::Rectangle)
                            p.drawRect(QRectF(a.pts[0], a.pts[1]).normalized());
                        else if (a.type == Annotation::Ellipse)
                            p.drawEllipse(QRectF(a.pts[0], a.pts[1]).normalized());
                        else
                            p.drawPolygon(QPolygonF(a.pts));
                    }
            }
        }
        QApplication::restoreOverrideCursor();
        if (m_ihcOverlay->isChecked())
            m_view->setOverlayImage(m_ihcMask);
        const int total = nr.positive + nr.negative;
        QString text = tr("<b>%1: %2</b> (%3)<br>Positive (DAB): %4 &nbsp; Negative (H): %5<br>"
                          "<b>%6: %7 %</b>")
                           .arg(no.nuclearMarker ? tr("Nuclei") : tr("Cells"))
                           .arg(total)
                           .arg(regionName)
                           .arg(nr.positive)
                           .arg(nr.negative)
                           .arg(no.nuclearMarker ? tr("Labelling index") : tr("Positive cells"))
                           .arg(nr.labellingIndex * 100, 0, 'f', 1);
        if (nr.densityPerMm2 > 0)
            text += tr("<br>Density: %1 nuclei/mm²").arg(nr.densityPerMm2, 0, 'f', 0);
        if (m_meta.umPerPixel <= 0)
            text += tr("<br><i>Image not calibrated: nucleus size assumes 0.25 µm/pixel.</i>");
        text += tr("<br><span style='color:#e22'>red</span> = positive, <span style='color:#37f'>blue</span> = negative");
        m_ihcResult->setText(text);
        m_ihcText = QStringLiteral("image\tregion\tnuclei\tpositive\tnegative\tlabelling_index_%\tdensity_per_mm2\t"
                                   "nucleus_diameter_um\tDAB_threshold_OD\n%1\t%2\t%3\t%4\t%5\t%6\t%7\t%8\t%9\n")
                        .arg(QFileInfo(m_path).fileName(), regionName)
                        .arg(total)
                        .arg(nr.positive)
                        .arg(nr.negative)
                        .arg(nr.labellingIndex * 100, 0, 'f', 2)
                        .arg(nr.densityPerMm2, 0, 'f', 1)
                        .arg(no.diameterUm, 0, 'f', 1)
                        .arg(opt.dabThreshold);
        emit message(no.nuclearMarker ? tr("Nuclei: %1, labelling index %2 %").arg(total).arg(nr.labellingIndex * 100, 0, 'f', 1)
                                     : tr("Cells: %1, %2 % positive").arg(total).arg(nr.labellingIndex * 100, 0, 'f', 1),
                     6000);
        return;
    }
    // overlay
    m_ihcMask = QImage(m_data.width, m_data.height, QImage::Format_ARGB32); // non-premultiplied colours below
    for (int y = 0; y < m_data.height; ++y) {
        QRgb *d = reinterpret_cast<QRgb *>(m_ihcMask.scanLine(y));
        const uint8_t *mk = r.mask.data() + size_t(y) * m_data.width;
        for (int x = 0; x < m_data.width; ++x)
            d[x] = mk[x] == 2 ? qRgba(150, 20, 20, 150) : mk[x] == 1 ? qRgba(20, 60, 170, 90) : 0;
    }
    QApplication::restoreOverrideCursor();
    if (m_ihcOverlay->isChecked())
        m_view->setOverlayImage(m_ihcMask);

    const bool cal = m_meta.umPerPixel > 0;
    auto area = [&](double um2, uint64_t px) { return cal ? formatArea(um2) : tr("%1 px").arg(px); };
    m_ihcResult->setText(tr("<b>DAB positive: %1 %</b> of tissue (%2)<br>"
                            "Tissue area: %3<br>Positive area: %4<br>"
                            "Intensity: weak %5 %, moderate %6 %, strong %7 %<br>"
                            "H-score: <b>%8</b><br>Mean DAB OD (positive): %9")
                             .arg(r.positiveFraction * 100, 0, 'f', 1)
                             .arg(regionName)
                             .arg(area(r.tissueAreaUm2, r.tissuePixels))
                             .arg(area(r.positiveAreaUm2, r.positivePixels))
                             .arg(r.weak * 100, 0, 'f', 1)
                             .arg(r.moderate * 100, 0, 'f', 1)
                             .arg(r.strong * 100, 0, 'f', 1)
                             .arg(r.hScore, 0, 'f', 0)
                             .arg(r.meanDabPositive, 0, 'f', 3));
    m_ihcText = QStringLiteral("image\tregion\tDAB_threshold_OD\tDAB_positive_%\ttissue_area\tpositive_area\tweak_%\tmoderate_%\tstrong_%\tH_score\tmean_DAB_OD_pos\n"
                               "%1\t%2\t%3\t%4\t%5\t%6\t%7\t%8\t%9\t%10\t%11\n")
                    .arg(QFileInfo(m_path).fileName(), regionName)
                    .arg(opt.dabThreshold)
                    .arg(r.positiveFraction * 100, 0, 'f', 2)
                    .arg(area(r.tissueAreaUm2, r.tissuePixels), area(r.positiveAreaUm2, r.positivePixels))
                    .arg(r.weak * 100, 0, 'f', 2)
                    .arg(r.moderate * 100, 0, 'f', 2)
                    .arg(r.strong * 100, 0, 'f', 2)
                    .arg(r.hScore, 0, 'f', 1)
                    .arg(r.meanDabPositive, 0, 'f', 4);
    emit message(tr("IHC: %1 % DAB positive (%2)").arg(r.positiveFraction * 100, 0, 'f', 1).arg(regionName), 6000);
}

void ProcessPage::updateStainLabel()
{
    const auto &ih = AppSettings::instance().ihc;
    auto vec = [](const double v[3]) {
        return QStringLiteral("%1 %2 %3").arg(v[0], 0, 'f', 2).arg(v[1], 0, 'f', 2).arg(v[2], 0, 'f', 2);
    };
    m_stainLabel->setText(ih.customVectors
                              ? tr("Stain colours: estimated from %1 (H %2, DAB %3)")
                                    .arg(ih.vectorSource.isEmpty() ? tr("an image") : ih.vectorSource, vec(ih.h), vec(ih.dab))
                              : tr("Stain colours: standard H-DAB"));
}

void ProcessPage::setCalibration()
{
    bool ok = false;
    const double v = QInputDialog::getDouble(this, tr("Pixel size"), tr("Micrometres per pixel:"),
                                             m_meta.umPerPixel > 0 ? m_meta.umPerPixel : 0.1, 0.0001, 1000, 5, &ok);
    if (!ok)
        return;
    m_meta.umPerPixel = v;
    m_view->setUmPerPixel(v);
    updateMeasurements();
    updateInfo();
}

void ProcessPage::saveAs()
{
    if (m_data.empty())
        return;
    const QString start = m_path.isEmpty() ? AppSettings::instance().nextFileName(QString(), QStringLiteral("processed")) : m_path;
    QString selected;
    const QString f = QFileDialog::getSaveFileName(this, tr("Save image"), start,
                                                   tr("TIFF 16-bit (*.tif);;TIFF 8-bit (*.tif);;PNG (*.png);;JPEG (*.jpg);;BMP (*.bmp)"),
                                                   &selected);
    if (f.isEmpty())
        return;
    SaveOptions opt;
    opt.format = formatFromExtension(f);
    opt.sixteenBit = selected.contains(QLatin1String("16")) || opt.format == FileFormat::Png;
    ColorPipeline p;
    p.update(m_adjust);
    Image16 out = p.render16(m_data);
    QString err;
    if (!saveImage(f, out, m_meta, opt, &err)) {
        QMessageBox::warning(this, tr("Save image"), tr("Could not save:\n%1").arg(err));
        return;
    }
    m_path = f;
    m_dirtyAnnotations = true;
    saveAnnotations();
    updateInfo();
    emit fileSaved(f);
    emit message(tr("Saved %1").arg(f), 5000);
}

void ProcessPage::exportWithOverlays()
{
    if (m_data.empty())
        return;
    const QString base = m_path.isEmpty() ? AppSettings::instance().capture.folder + QStringLiteral("/export")
                                          : QFileInfo(m_path).dir().filePath(QFileInfo(m_path).completeBaseName() + QStringLiteral("_annotated"));
    const QString f = QFileDialog::getSaveFileName(this, tr("Export with annotations and scale bar"), base + QStringLiteral(".png"),
                                                   tr("PNG (*.png);;TIFF (*.tif);;JPEG (*.jpg)"));
    if (f.isEmpty())
        return;
    const QImage img = m_view->renderWithOverlays(AppSettings::instance().overlays.scaleBar, true);
    SaveOptions opt;
    opt.format = formatFromExtension(f);
    opt.sixteenBit = false;
    QString err;
    if (!saveImage(f, img, m_meta, opt, &err))
        QMessageBox::warning(this, tr("Export"), tr("Could not export:\n%1").arg(err));
    else
        emit message(tr("Exported %1").arg(f), 5000);
}

void ProcessPage::copyToClipboard()
{
    if (m_data.empty())
        return;
    QApplication::clipboard()->setImage(m_view->renderWithOverlays(AppSettings::instance().overlays.scaleBar, true));
    emit message(tr("Image copied to the clipboard"), 3000);
}

void ProcessPage::print()
{
    if (m_data.empty())
        return;
    QPrinter printer(QPrinter::HighResolution);
    QPrintDialog dlg(&printer, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    QPainter p(&printer);
    const QImage img = m_view->renderWithOverlays(AppSettings::instance().overlays.scaleBar, true);
    const QRect page = p.viewport();
    QStringList caption;
    caption << (m_path.isEmpty() ? tr("Untitled") : QFileInfo(m_path).fileName());
    if (!m_meta.objective.isEmpty())
        caption << m_meta.objective;
    if (m_meta.acquired.isValid())
        caption << QLocale().toString(m_meta.acquired, QLocale::ShortFormat);
    const int capH = page.height() / 20;
    QSize s = img.size().scaled(page.width(), page.height() - capH * 2, Qt::KeepAspectRatio);
    p.drawImage(QRect(QPoint(page.left() + (page.width() - s.width()) / 2, page.top()), s), img);
    QFont f = p.font();
    f.setPointSize(9);
    p.setFont(f);
    p.drawText(QRect(page.left(), page.top() + s.height() + capH / 2, page.width(), capH), Qt::AlignLeft | Qt::AlignTop,
               caption.join(QStringLiteral("  ·  ")));
}

void ProcessPage::multifocusFromFiles()
{
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Select images of a focus series"),
                                                            AppSettings::instance().browseFolder, tr(kFileFilter));
    if (files.size() < 2)
        return;
    QProgressDialog prog(tr("Merging focus series…"), tr("Cancel"), 0, int(files.size()), this);
    prog.setWindowModality(Qt::WindowModal);
    FocusStacker st;
    st.setMode(FocusStacker::Mode::MaxContrast);
    ImageMetadata meta;
    for (int i = 0; i < files.size(); ++i) {
        prog.setValue(i);
        QApplication::processEvents();
        if (prog.wasCanceled())
            return;
        LoadedImage li;
        if (!loadImage(files[i], li)) {
            QMessageBox::warning(this, tr("Multifocus"), tr("Cannot read %1").arg(files[i]));
            return;
        }
        if (i == 0)
            meta = li.meta;
        st.add(li.data);
    }
    prog.setValue(int(files.size()));
    meta.captureMode = tr("multifocus (%1 images)").arg(files.size());
    meta.acquired = QDateTime::currentDateTime();
    openImage(st.result(), meta, QString());
}

void ProcessPage::stitchFromFiles()
{
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Select overlapping images in acquisition order"),
                                                            AppSettings::instance().browseFolder, tr(kFileFilter));
    if (files.size() < 2)
        return;
    QProgressDialog prog(tr("Stitching…"), tr("Cancel"), 0, int(files.size()), this);
    prog.setWindowModality(Qt::WindowModal);
    MosaicBuilder mb;
    auto o = mb.options();
    o.autoAdd = false;
    mb.setOptions(o);
    ImageMetadata meta;
    int failed = 0;
    for (int i = 0; i < files.size(); ++i) {
        prog.setValue(i);
        QApplication::processEvents();
        if (prog.wasCanceled())
            return;
        LoadedImage li;
        if (!loadImage(files[i], li))
            continue;
        if (i == 0)
            meta = li.meta;
        const auto st = mb.feed(li.data, true);
        if (i > 0 && !st.added)
            ++failed;
    }
    prog.setValue(int(files.size()));
    if (failed)
        QMessageBox::information(this, tr("Stitching"), tr("%1 image(s) could not be placed (insufficient overlap).").arg(failed));
    meta.captureMode = tr("stitched (%1 images)").arg(files.size());
    meta.acquired = QDateTime::currentDateTime();
    openImage(mb.result(), meta, QString());
}

} // namespace lm
