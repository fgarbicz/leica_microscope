#include "LifViewer.h"

#include "ui/GalleryWidget.h"
#include "ui/Icons.h"
#include "ui/ImageView.h"
#include "ui/LifExportDialog.h"
#include "ui/PlatformUi.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QColorDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>

namespace lm {

namespace {

// The most pixels shown at once; a bigger plane (a merged tile scan) is read
// at a reduced resolution for the screen. Exports always use every pixel.
constexpr double kMaxShownPixels = 36e6;

int subsampleFor(qint64 w, qint64 h)
{
    const double n = double(w) * double(h);
    return n <= kMaxShownPixels ? 1 : int(std::ceil(std::sqrt(n / kMaxShownPixels)));
}

QIcon swatch(QRgb c)
{
    QPixmap pm(px(16), px(16));
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(theme().border, 1));
    p.setBrush(QColor(c));
    p.drawRoundedRect(QRectF(0.5, 0.5, pm.width() - 1, pm.height() - 1), px(3), px(3));
    return QIcon(pm);
}

QString umText(double um)
{
    return um >= 1000 ? QObject::tr("%1 mm").arg(um / 1000, 0, 'g', 4) : QObject::tr("%1 µm").arg(um, 0, 'g', 4);
}

// The position along a dimension, in its own unit.
QString positionText(const LifDimension &d, int index)
{
    const double s = d.step();
    if (s <= 0)
        return QString();
    switch (d.id) {
    case LifDimZ: return umText(index * s);
    case LifDimT: {
        const double t = index * s;
        return t >= 120 ? QObject::tr("%1 min").arg(t / 60, 0, 'f', 1) : QObject::tr("%1 s").arg(t, 0, 'g', 4);
    }
    default: return QString();
    }
}

} // namespace

LifViewer::LifViewer(QWidget *parent) : QWidget(parent, Qt::Window)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("Leica .lif viewer"));
    m_readPool.setMaxThreadCount(1);
    m_thumbPool.setMaxThreadCount(1);
    resize(px(1500), px(900));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_toolbar = new QToolBar(this);
    m_toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_toolbar->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    const QColor ic = theme().subText;
    auto *openAct = m_toolbar->addAction(icon(Icon::Open, ic, 16), tr("Open…"));
    openAct->setToolTip(tr("Open another Leica .lif file in this window"));
    m_toolbar->addSeparator();
    auto *procAct = m_toolbar->addAction(icon(Icon::Process, ic, 16), tr("Open in Process"));
    procAct->setToolTip(tr("Measure or quantify the image as shown (the colours and contrast set here)"));
    auto *exportAct = m_toolbar->addAction(icon(Icon::Export, ic, 16), tr("Export…"));
    exportAct->setToolTip(tr("Export this image, the selected images or the whole file: the original data as "
                             "ImageJ TIFF, or pictures as shown"));
    auto *exportAllAct = m_toolbar->addAction(icon(Icon::Save, ic, 16), tr("Export all…"));
    exportAllAct->setToolTip(tr("Export every image in the file"));
    auto *copyAct = m_toolbar->addAction(tr("Copy"));
    copyAct->setToolTip(tr("Copy the image as shown to the clipboard"));
    m_toolbar->addSeparator();
    auto *fitAct = m_toolbar->addAction(icon(Icon::ZoomFit, ic, 16), tr("Fit"));
    auto *actualAct = m_toolbar->addAction(icon(Icon::ZoomActual, ic, 16), tr("100%"));
    m_toolbar->addSeparator();
    auto *xmlAct = m_toolbar->addAction(icon(Icon::Info, ic, 16), tr("Metadata…"));
    xmlAct->setToolTip(tr("Everything LAS X recorded about this image (XML)"));
    auto *revealAct = m_toolbar->addAction(icon(Icon::Folder, ic, 16), revealActionText());
    root->addWidget(m_toolbar);

    m_split = new QSplitter(Qt::Horizontal, this);
    root->addWidget(m_split, 1);

    // ---- the file's tree
    auto *left = new QWidget(m_split);
    auto *ll = new QVBoxLayout(left);
    ll->setContentsMargins(px(6), px(6), px(2), px(6));
    m_filter = new QLineEdit(left);
    m_filter->setPlaceholderText(tr("Filter by name"));
    m_filter->setClearButtonEnabled(true);
    ll->addWidget(m_filter);
    m_tree = new QTreeWidget(left);
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({tr("Image"), tr("Size")});
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->setIconSize(QSize(px(48), px(36)));
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setUniformRowHeights(true);
    m_tree->setMinimumWidth(px(220));
    ll->addWidget(m_tree, 1);
    m_treeInfo = new QLabel(left);
    m_treeInfo->setObjectName(QStringLiteral("Hint"));
    m_treeInfo->setWordWrap(true);
    ll->addWidget(m_treeInfo);

    // ---- the image, and the sliders for its planes
    auto *centre = new QWidget(m_split);
    auto *cl = new QVBoxLayout(centre);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(0);
    m_view = new ImageView(centre);
    m_view->setPlaceholder(tr("Open a Leica .lif file"));
    cl->addWidget(m_view, 1);
    m_dimBox = new QWidget(centre);
    auto *dl = new QVBoxLayout(m_dimBox);
    dl->setContentsMargins(px(8), px(4), px(8), px(4));
    m_dimGrid = new QGridLayout;
    m_dimGrid->setHorizontalSpacing(px(8));
    m_dimGrid->setColumnStretch(1, 1);
    dl->addLayout(m_dimGrid);
    auto *opts = new QHBoxLayout;
    m_projectBox = new QCheckBox(tr("Maximum projection along z"), m_dimBox);
    m_projectBox->setToolTip(tr("Show the brightest value of every pixel through the z stack"));
    m_mergeBox = new QCheckBox(tr("Merge tiles"), m_dimBox);
    m_mergeBox->setToolTip(tr("Put the tiles of a tile scan together where the stage was. They are placed, "
                              "not re-aligned; LAS X's own merged image, if saved, is in the file as well."));
    m_mergeBox->setChecked(true);
    opts->addWidget(m_projectBox);
    opts->addWidget(m_mergeBox);
    opts->addStretch();
    dl->addLayout(opts);
    cl->addWidget(m_dimBox);
    m_values = new QLabel(centre);
    m_values->setObjectName(QStringLiteral("Hint"));
    m_values->setContentsMargins(px(8), px(2), px(8), px(4));
    m_values->setTextInteractionFlags(Qt::TextSelectableByMouse);
    cl->addWidget(m_values);

    // ---- channels and information
    auto *scroll = new QScrollArea(m_split);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMinimumWidth(px(340));
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *right = new QWidget(scroll);
    auto *rl = new QVBoxLayout(right);
    rl->setContentsMargins(px(8), px(8), px(8), px(8));
    auto *chHead = new QHBoxLayout;
    auto *chTitle = new QLabel(tr("<b>Channels</b>"), right);
    chHead->addWidget(chTitle);
    chHead->addStretch();
    auto *autoAll = new QPushButton(tr("Auto"), right);
    autoAll->setToolTip(tr("Stretch the contrast of every channel to the plane shown"));
    auto *resetAll = new QPushButton(tr("Full range"), right);
    resetAll->setToolTip(tr("Show every channel over its full range, as recorded"));
    chHead->addWidget(autoAll);
    chHead->addWidget(resetAll);
    rl->addLayout(chHead);
    m_channelBox = new QWidget(right);
    m_channelLayout = new QVBoxLayout(m_channelBox);
    m_channelLayout->setContentsMargins(0, 0, 0, 0);
    rl->addWidget(m_channelBox);
    rl->addSpacing(px(8));
    rl->addWidget(new QLabel(tr("<b>Image</b>"), right));
    m_info = new QTableWidget(0, 2, right);
    m_info->horizontalHeader()->setVisible(false);
    m_info->verticalHeader()->setVisible(false);
    m_info->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_info->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_info->setSelectionMode(QAbstractItemView::ContiguousSelection);
    m_info->setWordWrap(true);
    m_info->setMinimumHeight(px(260));
    rl->addWidget(m_info, 1);
    scroll->setWidget(right);

    m_split->addWidget(left);
    m_split->addWidget(centre);
    m_split->addWidget(scroll);
    m_split->setStretchFactor(0, 0);
    m_split->setStretchFactor(1, 1);
    m_split->setStretchFactor(2, 0);
    m_split->setSizes({px(300), px(880), px(360)});

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("Hint"));
    m_status->setContentsMargins(px(8), px(3), px(8), px(3));
    root->addWidget(m_status);

    m_playTimer = new QTimer(this);
    m_playTimer->setInterval(150);
    connect(m_playTimer, &QTimer::timeout, this, [this] {
        if (!m_loading) // the next plane once the last one is on screen
            step(m_playDim, +1);
    });

    connect(openAct, &QAction::triggered, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Open Leica .lif"), QFileInfo(m_path).absolutePath(),
                                                       tr("Leica image files (*.lif)"));
        if (!f.isEmpty())
            openFile(f);
    });
    connect(procAct, &QAction::triggered, this, &LifViewer::sendToProcess);
    connect(exportAct, &QAction::triggered, this, [this] { exportImages(LifExportDialog::CurrentImage); });
    connect(exportAllAct, &QAction::triggered, this, [this] { exportImages(LifExportDialog::AllImages); });
    connect(copyAct, &QAction::triggered, this, &LifViewer::copyImage);
    connect(fitAct, &QAction::triggered, m_view, &ImageView::zoomFit);
    connect(actualAct, &QAction::triggered, m_view, &ImageView::zoomActual);
    connect(xmlAct, &QAction::triggered, this, &LifViewer::showXml);
    connect(revealAct, &QAction::triggered, this, [this] {
        if (!m_path.isEmpty())
            revealInFileManager(m_path);
    });
    connect(m_filter, &QLineEdit::textChanged, this, &LifViewer::applyFilter);
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *it) {
        if (it && it->data(0, Qt::UserRole).toInt() >= 0)
            selectImage(it->data(0, Qt::UserRole).toInt());
    });
    m_tree->setContextMenuPolicy(Qt::ActionsContextMenu);
    auto *exportSel = new QAction(icon(Icon::Export, ic, 16), tr("Export selected images…"), m_tree);
    connect(exportSel, &QAction::triggered, this, [this] { exportImages(LifExportDialog::SelectedImages); });
    m_tree->addAction(exportSel);
    connect(m_projectBox, &QCheckBox::toggled, this, &LifViewer::setProjection);
    connect(m_mergeBox, &QCheckBox::toggled, this, &LifViewer::setMergeTiles);
    connect(autoAll, &QPushButton::clicked, this, [this] {
        if (m_planesImage != m_current || !m_display.contains(m_current))
            return;
        QList<LifChannelDisplay> &d = m_display[m_current];
        for (int c = 0; c < d.size() && c < m_planes.size(); ++c)
            autoLifRange(m_planes[c], d[c].low, d[c].high);
        syncChannelControls();
        render();
    });
    connect(resetAll, &QPushButton::clicked, this, [this] {
        if (m_current < 0 || !m_display.contains(m_current))
            return;
        const QList<LifChannelDisplay> full = defaultLifDisplay(m_index.images[m_current]);
        QList<LifChannelDisplay> &d = m_display[m_current];
        for (int c = 0; c < d.size() && c < full.size(); ++c) {
            d[c].low = full[c].low;
            d[c].high = full[c].high;
        }
        syncChannelControls();
        render();
    });
    connect(m_view, &ImageView::cursorMoved, this, &LifViewer::showValues);

    // keys: previous/next image, z and time
    const auto shortcut = [this](const QKeySequence &k, std::function<void()> fn) {
        auto *s = new QShortcut(k, this);
        connect(s, &QShortcut::activated, this, fn);
    };
    shortcut(QKeySequence(Qt::Key_PageDown), [this] { selectRelative(+1); });
    shortcut(QKeySequence(Qt::Key_PageUp), [this] { selectRelative(-1); });
    shortcut(QKeySequence(Qt::Key_Period), [this] { step(LifDimZ, +1); });
    shortcut(QKeySequence(Qt::Key_Comma), [this] { step(LifDimZ, -1); });
    shortcut(QKeySequence(Qt::Key_BracketRight), [this] { step(LifDimT, +1); });
    shortcut(QKeySequence(Qt::Key_BracketLeft), [this] { step(LifDimT, -1); });
    shortcut(QKeySequence::Copy, [this] { copyImage(); });

    const QSettings s;
    restoreGeometry(s.value(QStringLiteral("lifViewer/geometry")).toByteArray());
    m_split->restoreState(s.value(QStringLiteral("lifViewer/split")).toByteArray());
    m_dimBox->hide();
    updateTitle();
}

LifViewer::~LifViewer()
{
    if (m_cancel)
        *m_cancel = true;
    ++(*m_generation);
    ++(*m_thumbGeneration);
    m_thumbPool.clear();
    m_readPool.clear();
    m_thumbPool.waitForDone();
    m_readPool.waitForDone();
}

void LifViewer::closeEvent(QCloseEvent *e)
{
    QSettings s;
    s.setValue(QStringLiteral("lifViewer/geometry"), saveGeometry());
    s.setValue(QStringLiteral("lifViewer/split"), m_split->saveState());
    QWidget::closeEvent(e);
}

void LifViewer::updateTitle()
{
    setWindowTitle(m_path.isEmpty() ? tr("Leica .lif viewer")
                                    : tr("%1 — Leica .lif viewer").arg(QFileInfo(m_path).fileName()));
}

// ---------------------------------------------------------------- the file

void LifViewer::openFile(const QString &path)
{
    m_path = path;
    // whatever was being read belongs to the previous file
    if (m_cancel)
        *m_cancel = true;
    m_loading = false;
    m_pending = false;
    m_loaded = false;
    m_reading = true;
    m_error.clear();
    m_index = LifFileIndex();
    m_current = -1;
    m_display.clear();
    m_planes.clear();
    m_planesImage = -1;
    m_shown = QImage();
    m_playTimer->stop();
    ++(*m_thumbGeneration);
    m_thumbPool.clear();
    m_tree->clear();
    m_items.clear();
    m_view->clear();
    m_view->setBusy(tr("Reading %1…").arg(QFileInfo(path).fileName()));
    m_status->setText(tr("Reading %1…").arg(QDir::toNativeSeparators(path)));
    updateTitle();

    struct Read {
        bool ok = false;
        LifFileIndex index;
        QString error;
    };
    const int gen = ++(*m_generation);
    auto generation = m_generation;
    QtConcurrent::run(&m_readPool, [path] {
        Read r;
        try {
            r.ok = readLif(path, r.index, &r.error);
        } catch (const std::exception &e) {
            r.error = QString::fromUtf8(e.what());
        }
        return r;
    }).then(this, [this, gen, generation, path](const Read &r) {
        if (generation->load() != gen || path != m_path)
            return;
        m_reading = false;
        m_view->setBusy(QString());
        if (!r.ok) {
            m_error = r.error;
            m_view->setPlaceholder(tr("Cannot open %1:\n%2").arg(QFileInfo(path).fileName(), r.error));
            m_status->setText(r.error);
            emit fileLoaded(false);
            return;
        }
        m_index = r.index;
        m_loaded = true;
        buildTree();
        const QFileInfo fi(path);
        QString info = tr("%1 images · %2 MB").arg(m_index.images.size()).arg(fi.size() / 1048576.0, 0, 'f', 1);
        if (!m_index.problems.isEmpty())
            info += QStringLiteral("\n") + tr("%1 could not be read: %2")
                                               .arg(m_index.problems.size())
                                               .arg(m_index.problems.join(QStringLiteral("; ")));
        m_treeInfo->setText(info);
        m_status->setText(QDir::toNativeSeparators(path));
        emit fileLoaded(true);
        if (!m_index.images.isEmpty())
            selectImage(0);
        startThumbnails();
    });
}

void LifViewer::buildTree()
{
    m_tree->clear();
    m_items.clear();
    // the file itself is the top of LAS X's tree: its children are listed
    for (const LifNode &n : m_index.root.children)
        addNode(n, nullptr);
    if (m_index.root.image >= 0)
        addNode(m_index.root, nullptr);
    m_tree->expandAll();
}

void LifViewer::addNode(const LifNode &n, QTreeWidgetItem *parent)
{
    auto *it = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
    it->setText(0, n.name);
    it->setData(0, Qt::UserRole, n.image);
    if (n.image >= 0) {
        const LifEntry &e = m_index.images[n.image];
        it->setText(1, QStringLiteral("%1 × %2").arg(e.width).arg(e.height));
        it->setToolTip(0, e.name + QLatin1Char('\n') + e.summary());
        it->setToolTip(1, e.summary());
        m_items.insert(n.image, it);
    } else {
        it->setIcon(0, icon(Icon::Folder, theme().subText, 16));
        it->setFlags(it->flags() & ~Qt::ItemIsSelectable);
    }
    for (const LifNode &c : n.children)
        addNode(c, it);
}

void LifViewer::applyFilter(const QString &text)
{
    const QString t = text.trimmed();
    // an item is shown when it matches or holds something that matches
    std::function<bool(QTreeWidgetItem *)> walk = [&](QTreeWidgetItem *it) {
        bool any = false;
        for (int i = 0; i < it->childCount(); ++i)
            any = walk(it->child(i)) || any;
        const bool self = t.isEmpty() || it->text(0).contains(t, Qt::CaseInsensitive);
        const bool show = self || any;
        it->setHidden(!show);
        return show;
    };
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
        walk(m_tree->topLevelItem(i));
}

void LifViewer::startThumbnails()
{
    const int gen = ++(*m_thumbGeneration);
    auto generation = m_thumbGeneration;
    const QString path = m_path;
    const QSize size = m_tree->iconSize();
    const qreal dpr = devicePixelRatioF();
    for (int i = 0; i < m_index.images.size(); ++i) {
        const LifEntry e = m_index.images[i];
        QtConcurrent::run(&m_thumbPool, [e, path, gen, generation, size, dpr] {
            if (generation->load() != gen)
                return QImage();
            // the middle of a z stack shows more than its first slice
            LifRequest r;
            for (const LifDimension &d : e.planeDims)
                r.coord << (d.id == LifDimZ ? d.size / 2 : 0);
            r.mergeTiles = e.hasTiles();
            const QSize full = r.mergeTiles ? lifMosaicSize(e) : QSize(e.width, e.height);
            r.subsample = std::max(1, int(std::max(full.width(), full.height()) / (2 * std::max(size.width(), size.height()) * dpr)));
            QList<LifPlane> planes;
            if (!readLifChannels(path, e, r, planes, nullptr))
                return QImage();
            return makeThumbnail(toQImage8(composeLif(planes, autoLifDisplay(e, planes))), size, dpr);
        }).then(this, [this, i, gen, generation](const QImage &thumb) {
            if (generation->load() != gen || thumb.isNull())
                return;
            if (QTreeWidgetItem *it = m_items.value(i))
                it->setIcon(0, thumbnailIcon(thumb));
        });
    }
}

// ---------------------------------------------------------------- one image

void LifViewer::selectImage(int index)
{
    if (index < 0 || index >= m_index.images.size())
        return;
    if (QTreeWidgetItem *it = m_items.value(index); it && m_tree->currentItem() != it) {
        const QSignalBlocker b(m_tree);
        m_tree->setCurrentItem(it);
        m_tree->scrollToItem(it);
    }
    if (index == m_current)
        return;
    m_playTimer->stop();
    m_current = index;
    const LifEntry &e = m_index.images[index];
    m_coord.clear();
    for (int i = 0; i < e.planeDims.size(); ++i)
        m_coord << 0;
    m_project = false;
    {
        const QSignalBlocker b(m_projectBox);
        m_projectBox->setChecked(false);
    }
    buildDimControls();
    buildChannelControls();
    updateInfo();
    requestPlanes();
}

void LifViewer::selectRelative(int delta)
{
    if (m_index.images.isEmpty())
        return;
    selectImage(std::clamp(m_current + delta, 0, int(m_index.images.size()) - 1));
}

void LifViewer::buildDimControls()
{
    while (QLayoutItem *li = m_dimGrid->takeAt(0)) {
        delete li->widget();
        delete li;
    }
    m_dims.clear();
    const LifEntry &e = m_index.images[m_current];
    const bool tiles = e.hasTiles();
    int row = 0;
    for (int i = 0; i < e.planeDims.size(); ++i) {
        const LifDimension &d = e.planeDims[i];
        if (d.size < 2)
            continue;
        DimControl dc;
        dc.dimId = d.id;
        dc.index = i;
        auto *label = new QLabel(d.label(), m_dimBox);
        dc.slider = new QSlider(Qt::Horizontal, m_dimBox);
        dc.slider->setRange(0, d.size - 1);
        dc.slider->setPageStep(std::max(1, d.size / 10));
        dc.spin = new QSpinBox(m_dimBox);
        dc.spin->setRange(1, d.size);
        dc.spin->setSuffix(QStringLiteral(" / %1").arg(d.size));
        dc.value = new QLabel(m_dimBox);
        dc.value->setObjectName(QStringLiteral("Hint"));
        dc.value->setMinimumWidth(px(70));
        dc.play = new QToolButton(m_dimBox);
        dc.play->setIcon(icon(Icon::Play, theme().subText, 16));
        dc.play->setToolTip(tr("Play through %1").arg(d.label()));
        dc.play->setAutoRaise(true);
        m_dimGrid->addWidget(label, row, 0);
        m_dimGrid->addWidget(dc.slider, row, 1);
        m_dimGrid->addWidget(dc.spin, row, 2);
        m_dimGrid->addWidget(dc.value, row, 3);
        m_dimGrid->addWidget(dc.play, row, 4);
        const int id = d.id;
        connect(dc.slider, &QSlider::valueChanged, this, [this, id](int v) { setCoordinate(id, v); });
        connect(dc.spin, &QSpinBox::valueChanged, this, [this, id](int v) { setCoordinate(id, v - 1); });
        connect(dc.play, &QToolButton::clicked, this, [this, id] { togglePlay(id); });
        m_dims.append(dc);
        ++row;
    }
    m_projectBox->setVisible(e.sizeOf(LifDimZ) > 1);
    m_mergeBox->setVisible(tiles);
    {
        const QSignalBlocker b(m_mergeBox);
        m_mergeBox->setChecked(m_merge);
    }
    m_dimBox->setVisible(!m_dims.isEmpty());
    // reflect the coordinate, the projection and the merge
    for (DimControl &dc : m_dims) {
        const QSignalBlocker b1(dc.slider), b2(dc.spin);
        const int v = m_coord.value(dc.index);
        dc.slider->setValue(v);
        dc.spin->setValue(v + 1);
        dc.value->setText(positionText(e.planeDims[dc.index], v));
        const bool off = (dc.dimId == LifDimZ && m_project) || (dc.dimId == LifDimMosaic && m_merge);
        dc.slider->setEnabled(!off);
        dc.spin->setEnabled(!off);
        dc.play->setEnabled(!off);
    }
}

void LifViewer::setCoordinate(int dimId, int value)
{
    if (m_current < 0)
        return;
    const LifEntry &e = m_index.images[m_current];
    const int i = e.dimIndex(dimId);
    if (i < 0)
        return;
    value = std::clamp(value, 0, e.planeDims[i].size - 1);
    if (m_coord.value(i) == value)
        return;
    m_coord[i] = value;
    for (DimControl &dc : m_dims)
        if (dc.index == i) {
            const QSignalBlocker b1(dc.slider), b2(dc.spin);
            dc.slider->setValue(value);
            dc.spin->setValue(value + 1);
            dc.value->setText(positionText(e.planeDims[i], value));
        }
    requestPlanes();
}

void LifViewer::step(int dimId, int delta)
{
    if (m_current < 0)
        return;
    const LifEntry &e = m_index.images[m_current];
    const int i = e.dimIndex(dimId);
    if (i < 0 || e.planeDims[i].size < 2)
        return;
    const int n = e.planeDims[i].size;
    // playing wraps around; a key stops at the end
    const int v = m_playTimer->isActive() && dimId == m_playDim ? (m_coord.value(i) + delta + n) % n
                                                                 : std::clamp(m_coord.value(i) + delta, 0, n - 1);
    setCoordinate(dimId, v);
}

void LifViewer::togglePlay(int dimId)
{
    const bool start = !(m_playTimer->isActive() && m_playDim == dimId);
    m_playTimer->stop();
    m_playDim = start ? dimId : -1;
    for (DimControl &dc : m_dims)
        dc.play->setIcon(icon(start && dc.dimId == dimId ? Icon::Stop : Icon::Play, theme().subText, 16));
    if (start)
        m_playTimer->start();
}

void LifViewer::setProjection(bool on)
{
    if (m_project == on)
        return;
    m_project = on;
    {
        const QSignalBlocker b(m_projectBox);
        m_projectBox->setChecked(on);
    }
    if (m_current >= 0) {
        buildDimControls();
        requestPlanes();
    }
}

void LifViewer::setMergeTiles(bool on)
{
    if (m_merge == on)
        return;
    m_merge = on;
    {
        const QSignalBlocker b(m_mergeBox);
        m_mergeBox->setChecked(on);
    }
    if (m_current >= 0) {
        buildDimControls();
        updateInfo();
        requestPlanes();
    }
}

LifRequest LifViewer::request() const
{
    LifRequest r;
    r.coord = m_coord;
    if (m_current < 0)
        return r;
    const LifEntry &e = m_index.images[m_current];
    r.projectDim = m_project && e.sizeOf(LifDimZ) > 1 ? e.dimIndex(LifDimZ) : -1;
    r.mergeTiles = m_merge && e.hasTiles();
    const QSize full = r.mergeTiles ? lifMosaicSize(e) : QSize(e.width, e.height);
    r.subsample = subsampleFor(full.width(), full.height());
    return r;
}

void LifViewer::requestPlanes()
{
    if (m_current < 0)
        return;
    if (m_loading) {
        // the plane being read is already out of date: stop it, read this one next
        m_pending = true;
        if (m_cancel)
            *m_cancel = true;
        return;
    }
    m_loading = true;
    m_pending = false;
    const int gen = ++(*m_generation);
    auto generation = m_generation;
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    auto cancel = m_cancel;
    const LifEntry e = m_index.images[m_current];
    const LifRequest r = request();
    const QString path = m_path;
    const int image = m_current;
    if (m_planesImage != image)
        m_view->setBusy(tr("Reading %1…").arg(e.name));
    struct Read {
        bool ok = false;
        QList<LifPlane> planes;
        QString error;
    };
    QtConcurrent::run(&m_readPool, [e, r, path, cancel] {
        Read out;
        try {
            out.ok = readLifChannels(path, e, r, out.planes, &out.error, cancel.get());
        } catch (const std::bad_alloc &) {
            out.error = QObject::tr("Not enough memory");
        }
        return out;
    }).then(this, [this, gen, generation, image, r](const Read &out) {
        if (generation->load() != gen)
            return; // a new file
        m_loading = false;
        if (m_pending || image != m_current) {
            requestPlanes();
            return;
        }
        m_view->setBusy(QString());
        if (!out.ok) {
            m_error = out.error;
            m_planes.clear();
            m_planesImage = -1;
            m_view->clear();
            m_view->setPlaceholder(tr("Cannot read this image:\n%1").arg(out.error));
            m_status->setText(out.error);
            emit planeShown();
            return;
        }
        const bool newImage = m_planesImage != image;
        m_planes = out.planes;
        m_planesImage = image;
        m_subsample = r.subsample;
        if (!m_display.contains(image))
            m_display.insert(image, autoLifDisplay(m_index.images[image], m_planes));
        if (newImage)
            syncChannelControls();
        render(newImage);
        const LifEntry &e = m_index.images[image];
        QString s = QStringLiteral("%1  ·  %2").arg(e.name, e.summary());
        if (r.subsample > 1)
            s += tr("  ·  shown at 1/%1 resolution (exports use every pixel)").arg(r.subsample);
        m_status->setText(s);
        emit planeShown();
    });
}

void LifViewer::render(bool resetView)
{
    if (m_planesImage != m_current || m_planes.isEmpty())
        return;
    const LifEntry &e = m_index.images[m_current];
    try {
        m_shown = toQImage8(composeLif(m_planes, m_display.value(m_current)));
    } catch (const std::bad_alloc &) {
        m_status->setText(tr("Not enough memory to show this image"));
        return;
    }
    m_view->setImage(m_shown, resetView);
    m_view->setUmPerPixel(e.umPerPixel * m_subsample);
}

QImage LifViewer::displayedImage() const
{
    return m_shown;
}

void LifViewer::showValues(const QPoint &p, bool inside)
{
    if (!inside || m_planes.isEmpty() || m_planesImage != m_current) {
        m_values->clear();
        return;
    }
    const LifPlane &first = m_planes.first();
    if (p.x() < 0 || p.y() < 0 || p.x() >= first.width || p.y() >= first.height) {
        m_values->clear();
        return;
    }
    const LifEntry &e = m_index.images[m_current];
    QString s = QStringLiteral("x %1  y %2").arg(p.x() * m_subsample).arg(p.y() * m_subsample);
    if (e.umPerPixel > 0)
        s += QStringLiteral(" (%1, %2 µm)")
                 .arg(p.x() * m_subsample * e.umPerPixel, 0, 'f', 2)
                 .arg(p.y() * m_subsample * e.umPerPixel, 0, 'f', 2);
    for (int c = 0; c < m_planes.size() && c < e.channels.size(); ++c)
        s += QStringLiteral("   %1: %2").arg(e.channels[c].name).arg(m_planes[c].at(p.x(), p.y()));
    m_values->setText(s);
}

// ---------------------------------------------------------------- channels

void LifViewer::buildChannelControls()
{
    while (QLayoutItem *li = m_channelLayout->takeAt(0)) {
        delete li->widget();
        delete li;
    }
    m_channels.clear();
    const LifEntry &e = m_index.images[m_current];
    for (int c = 0; c < e.channels.size(); ++c) {
        const LifChannel &ch = e.channels[c];
        auto *box = new QWidget(m_channelBox);
        auto *g = new QGridLayout(box);
        g->setContentsMargins(0, px(2), 0, px(4));
        g->setHorizontalSpacing(px(4));
        ChannelControl cc;
        cc.visible = new QCheckBox(ch.name, box);
        cc.visible->setChecked(true);
        cc.visible->setToolTip(tr("%1 · %2-bit · LUT %3").arg(ch.name).arg(ch.bits).arg(ch.lut));
        cc.colour = new QToolButton(box);
        cc.colour->setAutoRaise(true);
        cc.colour->setToolTip(tr("Colour"));
        cc.low = new QSpinBox(box);
        cc.high = new QSpinBox(box);
        for (QSpinBox *sb : {cc.low, cc.high})
            sb->setRange(0, ch.maxValue());
        cc.low->setToolTip(tr("Shown as black"));
        cc.high->setToolTip(tr("Shown at full intensity"));
        auto *autoBtn = new QToolButton(box);
        autoBtn->setIcon(icon(Icon::Wand, theme().subText, 16));
        autoBtn->setAutoRaise(true);
        autoBtn->setToolTip(tr("Automatic contrast for this channel"));
        g->addWidget(cc.colour, 0, 0);
        g->addWidget(cc.visible, 0, 1, 1, 3);
        g->addWidget(cc.low, 1, 1);
        g->addWidget(new QLabel(QStringLiteral("–"), box), 1, 2);
        g->addWidget(cc.high, 1, 3);
        g->addWidget(autoBtn, 1, 4);
        g->setColumnStretch(1, 1);
        g->setColumnStretch(3, 1);
        m_channelLayout->addWidget(box);
        m_channels.append(cc);

        connect(cc.visible, &QCheckBox::toggled, this, [this, c](bool on) { setChannelVisible(c, on); });
        connect(cc.low, &QSpinBox::valueChanged, this, [this, c](int v) {
            if (m_display.contains(m_current) && c < m_display[m_current].size())
                setChannelRange(c, v, m_display[m_current][c].high);
        });
        connect(cc.high, &QSpinBox::valueChanged, this, [this, c](int v) {
            if (m_display.contains(m_current) && c < m_display[m_current].size())
                setChannelRange(c, m_display[m_current][c].low, v);
        });
        connect(autoBtn, &QToolButton::clicked, this, [this, c] {
            if (m_planesImage != m_current || c >= m_planes.size() || !m_display.contains(m_current))
                return;
            int lo = 0, hi = 0;
            autoLifRange(m_planes[c], lo, hi);
            setChannelRange(c, lo, hi);
        });
        connect(cc.colour, &QToolButton::clicked, this, [this, c] {
            if (!m_display.contains(m_current) || c >= m_display[m_current].size())
                return;
            const QColor picked =
                QColorDialog::getColor(QColor(m_display[m_current][c].colour), this, tr("Channel colour"));
            if (!picked.isValid())
                return;
            m_display[m_current][c].colour = picked.rgb();
            syncChannelControls();
            render();
        });
    }
    m_channelLayout->addStretch();
    syncChannelControls();
}

void LifViewer::syncChannelControls()
{
    if (!m_display.contains(m_current))
        return;
    const QList<LifChannelDisplay> &d = m_display[m_current];
    for (int c = 0; c < m_channels.size() && c < d.size(); ++c) {
        ChannelControl &cc = m_channels[c];
        const QSignalBlocker b1(cc.visible), b2(cc.low), b3(cc.high);
        cc.visible->setChecked(d[c].visible);
        cc.low->setValue(d[c].low);
        cc.high->setValue(d[c].high);
        cc.colour->setIcon(swatch(d[c].colour));
    }
}

void LifViewer::setChannelVisible(int channel, bool on)
{
    if (!m_display.contains(m_current) || channel < 0 || channel >= m_display[m_current].size())
        return;
    m_display[m_current][channel].visible = on;
    syncChannelControls();
    render();
}

void LifViewer::setChannelRange(int channel, int low, int high)
{
    if (!m_display.contains(m_current) || channel < 0 || channel >= m_display[m_current].size())
        return;
    LifChannelDisplay &d = m_display[m_current][channel];
    d.low = std::min(low, high - 1);
    d.high = std::max(high, d.low + 1);
    syncChannelControls();
    render();
}

// ---------------------------------------------------------------- information

void LifViewer::updateInfo()
{
    const LifEntry &e = m_index.images[m_current];
    QList<QPair<QString, QString>> rows;
    rows.append({tr("Name"), e.name});
    QStringList folder = e.path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (!folder.isEmpty())
        folder.removeFirst();
    if (!folder.isEmpty())
        rows.append({tr("Folder"), folder.join(QStringLiteral(" / "))});
    rows.append({tr("Size"), tr("%1 × %2 pixels").arg(e.width).arg(e.height)});
    if (e.umPerPixel > 0) {
        rows.append({tr("Pixel size"), std::abs(e.umPerPixelY - e.umPerPixel) > 1e-3 * e.umPerPixel
                                            ? tr("%1 × %2 µm").arg(e.umPerPixel, 0, 'g', 5).arg(e.umPerPixelY, 0, 'g', 5)
                                            : tr("%1 µm").arg(e.umPerPixel, 0, 'g', 5)});
        rows.append({tr("Field of view"), tr("%1 × %2").arg(umText(e.width * e.umPerPixel), umText(e.height * e.umPerPixelY))});
    } else {
        rows.append({tr("Pixel size"), tr("not recorded")});
    }
    for (const LifDimension &d : e.planeDims) {
        if (d.size < 2)
            continue;
        QString v;
        switch (d.id) {
        case LifDimZ:
            v = tr("%1 slices").arg(d.size);
            if (d.step() > 0)
                v += tr(", %1 apart").arg(umText(d.step()));
            break;
        case LifDimT:
            v = tr("%1 time points").arg(d.size);
            if (d.step() > 0)
                v += tr(", every %1").arg(positionText(d, 1));
            break;
        case LifDimMosaic: {
            v = tr("%1 tiles").arg(d.size);
            const QSize m = lifMosaicSize(e);
            v += tr(", merged %1 × %2 pixels").arg(m.width()).arg(m.height());
            break;
        }
        default: v = QString::number(d.size);
        }
        rows.append({d.label(), v});
    }
    QStringList chans;
    for (const LifChannel &c : e.channels)
        chans << QStringLiteral("%1 (%2-bit)").arg(c.name).arg(c.bits);
    rows.append({tr("Channels"), chans.join(QStringLiteral(", "))});
    rows.append(e.info);
    rows.append({tr("Data"), tr("%1 MB").arg(e.dataBytes / 1048576.0, 0, 'f', 1)});

    m_info->setRowCount(int(rows.size()));
    for (int i = 0; i < rows.size(); ++i) {
        auto *k = new QTableWidgetItem(rows[i].first);
        k->setForeground(palette().color(QPalette::PlaceholderText));
        m_info->setItem(i, 0, k);
        auto *v = new QTableWidgetItem(rows[i].second);
        v->setToolTip(rows[i].second);
        m_info->setItem(i, 1, v);
    }
    m_info->resizeColumnToContents(0);
    m_info->resizeRowsToContents();
}

void LifViewer::showXml()
{
    if (m_current < 0)
        return;
    const LifEntry &e = m_index.images[m_current];
    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(tr("Metadata of %1").arg(e.name));
    dlg->resize(px(900), px(700));
    auto *lay = new QVBoxLayout(dlg);
    auto *find = new QLineEdit(dlg);
    find->setPlaceholderText(tr("Find (Enter for the next match)"));
    lay->addWidget(find);
    auto *text = new QPlainTextEdit(dlg);
    text->setReadOnly(true);
    text->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    text->setFont(mono);
    const QString xml = m_index.elementXml(e);
    text->setPlainText(xml);
    lay->addWidget(text, 1);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
    auto *save = bb->addButton(tr("Save…"), QDialogButtonBox::ActionRole);
    lay->addWidget(bb);
    connect(bb, &QDialogButtonBox::rejected, dlg, &QDialog::close);
    connect(find, &QLineEdit::returnPressed, dlg, [text, find] {
        if (!text->find(find->text())) { // from the top again
            text->moveCursor(QTextCursor::Start);
            text->find(find->text());
        }
    });
    connect(save, &QPushButton::clicked, dlg, [this, dlg, xml, e] {
        const QString suggested = QDir(QFileInfo(m_path).absolutePath()).filePath(lifImageFileName(e) + QStringLiteral(".xml"));
        const QString f = getSaveFileName(dlg, tr("Save metadata"), suggested, tr("XML (*.xml)"));
        if (f.isEmpty())
            return;
        QFile out(f);
        if (!out.open(QIODevice::WriteOnly) || out.write(xml.toUtf8()) < 0)
            QMessageBox::warning(dlg, tr("Save metadata"), tr("Cannot write %1: %2").arg(f, out.errorString()));
    });
    dlg->show();
}

// ---------------------------------------------------------------- out

LifExportItem LifViewer::currentExportItem() const
{
    LifExportItem it;
    it.image = m_current;
    it.request = request();
    it.request.subsample = 1;
    it.display = m_display.value(m_current);
    return it;
}

QList<int> LifViewer::selectedImages() const
{
    QList<int> out;
    for (QTreeWidgetItem *it : m_tree->selectedItems()) {
        const int i = it->data(0, Qt::UserRole).toInt();
        if (i >= 0 && !it->isHidden())
            out << i;
    }
    std::sort(out.begin(), out.end());
    return out;
}

void LifViewer::exportImages(int scope)
{
    if (!m_loaded || m_current < 0)
        return;
    LifExportDialog dlg(m_index, currentExportItem(), selectedImages(), m_display, this);
    dlg.setScope(LifExportDialog::Scope(scope));
    dlg.exec();
}

void LifViewer::sendToProcess()
{
    if (m_current < 0 || m_planesImage != m_current)
        return;
    const LifEntry e = m_index.images[m_current];
    LifRequest r = request();
    const QList<LifChannelDisplay> display = m_display.value(m_current);
    ImageMetadata meta = lifMetadata(e);
    if (r.subsample == 1) {
        const Image16 img = composeLif(m_planes, display);
        meta.width = img.width;
        meta.height = img.height;
        emit openInProcess(img, meta);
        return;
    }
    // shown at a reduced resolution: Process gets every pixel
    r.subsample = 1;
    m_view->setBusy(tr("Reading the full image…"));
    const QString path = m_path;
    QtConcurrent::run([e, r, path, display] {
        QList<LifPlane> planes;
        QString err;
        Image16 img;
        try {
            if (readLifChannels(path, e, r, planes, &err))
                img = composeLif(planes, display);
        } catch (const std::bad_alloc &) {
            err = QObject::tr("Not enough memory");
        }
        return qMakePair(img, err);
    }).then(this, [this, meta](const QPair<Image16, QString> &res) mutable {
        m_view->setBusy(QString());
        if (res.first.empty()) {
            QMessageBox::warning(this, tr("Open in Process"), tr("Cannot read the image: %1").arg(res.second));
            return;
        }
        meta.width = res.first.width;
        meta.height = res.first.height;
        emit openInProcess(res.first, meta);
    });
}

void LifViewer::copyImage()
{
    if (!m_shown.isNull())
        QApplication::clipboard()->setImage(m_shown);
}

} // namespace lm
