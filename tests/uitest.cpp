// User interface behaviour that is easy to break and hard to notice.
//
// Above all: the mouse wheel must never change a value. Scrolling a side panel
// used to alter whichever slider, spin box, combo box or tab sat under the
// pointer, silently changing the exposure or the objective.
#include "io/ImageIO.h"
#include "ui/CompareWindow.h"
#include "ui/GalleryWidget.h"
#include "ui/Icons.h"
#include "ui/ImageView.h"
#include "ui/Theme.h"
#include "ui/WheelGuard.h"

#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSpinBox>
#include <QTabBar>
#include <QVBoxLayout>
#include <QStyle>
#include <QWheelEvent>

#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace lm;

static int g_failed = 0;
#define CHECK(c)                                                                      \
    do {                                                                              \
        if (!(c)) {                                                                   \
            ++g_failed;                                                               \
            std::printf("  FAILED line %d: %s\n", __LINE__, #c);                      \
        }                                                                             \
    } while (0)

// Sends a wheel notch to the widget the way a real scroll does.
static void sendWheel(QWidget *w, int degrees = -120)
{
    const QPointF pos(w->width() / 2.0, w->height() / 2.0);
    QWheelEvent e(pos, w->mapToGlobal(pos.toPoint()), QPoint(0, 0), QPoint(0, degrees), Qt::NoButton, Qt::NoModifier,
                  Qt::NoScrollPhase, false);
    QApplication::sendEvent(w, &e);
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    applyTheme(app, QStringLiteral("dark"));
    WheelGuard::install(app);

    // A panel like the ones in Acquire: a tall column of controls in a scroll area.
    QScrollArea scroll;
    scroll.setWidgetResizable(true);
    auto *content = new QWidget;
    auto *lay = new QVBoxLayout(content);
    auto *spin = new QDoubleSpinBox(content);
    spin->setRange(0, 1000);
    spin->setValue(20);
    auto *intSpin = new QSpinBox(content);
    intSpin->setRange(0, 1000);
    intSpin->setValue(5);
    auto *slider = new QSlider(Qt::Horizontal, content);
    slider->setRange(0, 100);
    slider->setValue(50);
    auto *combo = new QComboBox(content);
    combo->addItems({QStringLiteral("2.5x"), QStringLiteral("10x"), QStringLiteral("40x")});
    combo->setCurrentIndex(1);
    auto *tabs = new QTabBar(content);
    tabs->addTab(QStringLiteral("Acquire"));
    tabs->addTab(QStringLiteral("Browse"));
    tabs->setCurrentIndex(0);
    for (QWidget *w : {static_cast<QWidget *>(spin), static_cast<QWidget *>(intSpin),
                       static_cast<QWidget *>(slider), static_cast<QWidget *>(combo),
                       static_cast<QWidget *>(tabs)})
        lay->addWidget(w);
    // make the content taller than the viewport so there is something to scroll
    auto *filler = new QWidget(content);
    filler->setMinimumHeight(2000);
    lay->addWidget(filler);
    scroll.setWidget(content);
    scroll.resize(300, 400);
    scroll.show();
    QApplication::processEvents();

    std::printf("wheel over the controls of a scrollable panel\n");
    const int scrollBefore = scroll.verticalScrollBar()->value();
    sendWheel(spin);
    sendWheel(intSpin);
    sendWheel(slider);
    sendWheel(combo);
    sendWheel(tabs);
    QApplication::processEvents();

    CHECK(spin->value() == 20.0);
    CHECK(intSpin->value() == 5);
    CHECK(slider->value() == 50);
    CHECK(combo->currentIndex() == 1);
    CHECK(tabs->currentIndex() == 0);
    // and the wheel did what the user meant: it scrolled the panel
    CHECK(scroll.verticalScrollBar()->value() > scrollBefore);

    std::printf("the wheel still scrolls a scroll bar itself\n");
    QScrollBar *bar = scroll.verticalScrollBar();
    const int barBefore = bar->value();
    sendWheel(bar);
    QApplication::processEvents();
    CHECK(bar->value() > barBefore);

    std::printf("values still change by other means\n");
    spin->setValue(33);
    CHECK(spin->value() == 33.0);
    slider->setValue(70);
    CHECK(slider->value() == 70);

    std::printf("the interface size scales everything\n");
    applyTheme(app, QStringLiteral("dark"), 100);
    const double font100 = QApplication::font().pointSizeF();
    const int px100 = px(40);
    const QSize icon100 = iconSize(16);
    CHECK(uiScale() == 100);
    CHECK(px(40) == 40);

    applyTheme(app, QStringLiteral("dark"), 150);
    CHECK(uiScale() == 150);
    CHECK(QApplication::font().pointSizeF() > font100);
    CHECK(px(40) > px100);
    CHECK(iconSize(16).width() > icon100.width());
    // the style sheet's own lengths were scaled, not just the font
    CHECK(app.styleSheet().contains(QStringLiteral("px")));
    CHECK(app.style()->pixelMetric(QStyle::PM_SmallIconSize) > 16);

    applyTheme(app, QStringLiteral("dark"), 75);
    CHECK(px(40) < px100);
    CHECK(QApplication::font().pointSizeF() < font100);

    // out-of-range values are clamped, not honoured
    applyTheme(app, QStringLiteral("dark"), 10000);
    CHECK(uiScale() <= maxUiScale());
    applyTheme(app, QStringLiteral("dark"), 1);
    CHECK(uiScale() >= minUiScale());

    std::printf("the size steps walk in both directions and stop\n");
    CHECK(nextUiScale(100, 1) > 100);
    CHECK(nextUiScale(100, -1) < 100);
    CHECK(nextUiScale(maxUiScale(), 1) == maxUiScale());
    CHECK(nextUiScale(minUiScale(), -1) == minUiScale());

    applyTheme(app, QStringLiteral("dark"), 100);

    std::printf("captured images: reel and vertical list\n");
    {
        GalleryWidget gallery;
        // the reel: thumbnails in a row, sized to keep the strip short
        CHECK(!gallery.isVertical());
        CHECK(gallery.viewMode() == QListView::IconMode);
        CHECK(gallery.flow() == QListView::LeftToRight);
        CHECK(gallery.minimumHeight() > 0);
        const int reelWidth = gallery.minimumWidth();

        gallery.setVertical(true);
        CHECK(gallery.isVertical());
        CHECK(gallery.viewMode() == QListView::ListMode);
        CHECK(gallery.flow() == QListView::TopToBottom);
        // a column needs a width, and must not force the old height
        CHECK(gallery.minimumWidth() > reelWidth);
        CHECK(gallery.minimumHeight() == 0);

        gallery.setVertical(false);
        CHECK(!gallery.isVertical());
        CHECK(gallery.viewMode() == QListView::IconMode);
        CHECK(gallery.minimumHeight() > 0);

        // neither layout may lose the images
        gallery.addImage(QStringLiteral("/tmp/a.tif"), QImage(64, 48, QImage::Format_RGB888));
        gallery.addImage(QStringLiteral("/tmp/b.tif"), QImage(64, 48, QImage::Format_RGB888));
        CHECK(gallery.count() == 2);
        gallery.setVertical(true);
        CHECK(gallery.count() == 2);
        CHECK(gallery.paths().size() == 2);
    }

    std::printf("every icon renders\n");
    // A missing or malformed SVG body would give a null pixmap and an invisible
    // button; check the whole set rather than the few used here.
    for (int i = 0; i <= int(Icon::ScaleBar); ++i) {
        const Icon ic = Icon(i);
        if (ic == Icon::None)
            continue;
        const QPixmap pm = iconPixmap(ic, theme().text, 16);
        if (pm.isNull()) {
            ++g_failed;
            std::printf("  FAILED: icon %d does not render\n", i);
        }
    }

    std::printf("the reference overlay blends over a matching image only\n");
    {
        ImageView v;
        v.resize(240, 150);
        QImage black(240, 150, QImage::Format_RGB32), white(240, 150, QImage::Format_RGB32);
        black.fill(Qt::black);
        white.fill(Qt::white);
        v.setImage(black, true);
        auto centre = [&v] { return qGray(v.grab().toImage().pixel(v.width() / 2, v.height() / 2)); };
        CHECK(centre() < 10);
        v.setReferenceOpacity(0.5);
        v.setReferenceImage(white);
        CHECK(v.hasReferenceImage());
        const int half = centre();
        CHECK(half > 100 && half < 155);
        v.setReferenceOpacity(0.25);
        CHECK(centre() < half - 30);
        // a reference of another shape (e.g. over a mosaic preview) is not drawn
        QImage square(150, 150, QImage::Format_RGB32);
        square.fill(Qt::white);
        v.setReferenceImage(square);
        CHECK(centre() < 10);
        v.setReferenceImage(QImage());
        CHECK(!v.hasReferenceImage() && centre() < 10);
    }

    std::printf("compare: aligned views follow the tissue, in the right direction\n");
    {
        // textured "tissue", and the same tissue shifted as on the next section
        const int w = 512, h = 384, dx = 40, dy = -25;
        std::mt19937 rng(3);
        std::vector<std::array<double, 3>> blobs;
        for (int i = 0; i < 160; ++i)
            blobs.push_back({std::uniform_real_distribution<double>(-60, w + 60)(rng),
                             std::uniform_real_distribution<double>(-60, h + 60)(rng),
                             std::uniform_real_distribution<double>(4, 14)(rng)});
        auto tissue = [&](double x, double y) {
            double v = 0;
            for (const auto &b : blobs)
                v += std::exp(-((x - b[0]) * (x - b[0]) + (y - b[1]) * (y - b[1])) / (2 * b[2] * b[2]));
            return std::clamp(1.0 - 0.6 * v, 0.0, 1.0);
        };
        Image16 left(w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < 3; ++c)
                    left.row(y)[x * 3 + c] = uint16_t(tissue(x, y) * 65535);
        QTemporaryDir dir;
        const QString pl = dir.filePath(QStringLiteral("a.tif"));
        CHECK(saveImage(pl, left, ImageMetadata(), SaveOptions()));
        // k = 2: the right image has twice the pixels of the same field (pixel shift vs standard)
        for (int k : {1, 2}) {
            Image16 right(w * k, h * k);
            for (int y = 0; y < h * k; ++y)
                for (int x = 0; x < w * k; ++x)
                    for (int c = 0; c < 3; ++c) // right(p) = left(p / k + d)
                        right.row(y)[x * 3 + c] = uint16_t(tissue(double(x) / k + dx, double(y) / k + dy) * 65535);
            const QString pr = dir.filePath(QStringLiteral("b%1.tif").arg(k));
            CHECK(saveImage(pr, right, ImageMetadata(), SaveOptions()));
            CompareWindow cw;
            cw.resize(1200, 600);
            CHECK(cw.openLeft(pl) && cw.openRight(pr));
            const auto views = cw.findChildren<ImageView *>();
            CHECK(views.size() == 2);
            if (views.size() != 2)
                continue;
            // which pane shows which image (child order is not guaranteed)
            const QImage la = toQImage8(left);
            ImageView *a = views[0]->image() == la ? views[0] : views[1];
            ImageView *b = a == views[0] ? views[1] : views[0];
            CHECK(a->image() == la);
            for (int i = 0; i < 3; ++i)
                sendWheel(a, 120); // zoom in on the left pane
            // the cell at the centre of the left pane is d (left) pixels further up-left on the right
            const QPointF ra = a->relativeCenter(), rb = b->relativeCenter();
            std::printf("  %dx: left centre %.3f,%.3f  right centre %.3f,%.3f\n", k, ra.x(), ra.y(), rb.x(), rb.y());
            CHECK(std::abs((ra.x() - rb.x()) * w - dx) < 2 && std::abs((ra.y() - rb.y()) * h - dy) < 2);
        }
    }

    std::printf(g_failed ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
