#pragma once
// A viewer for Leica .lif files: everything LAS X saved in one, browsed the
// way LAS X shows it.
//
//  - the file's tree of folders and images, with thumbnails and a filter;
//  - every channel in its colour, each switched on or off, recoloured and
//    with its own contrast (automatic or set by hand);
//  - z slices, time points, tiles and any other dimension, one slider each,
//    a maximum projection along z and the tiles merged where the stage was;
//  - the settings the image was recorded with, and the full LAS X metadata;
//  - the value of every channel under the cursor;
//  - exporting the image shown, a selection or the whole file (see
//    LifExportDialog), copying, and opening the picture in Process.
//
// Nothing is read on the interface thread: a .lif can be gigabytes, and in a
// OneDrive folder reading it may first download it.

#include "io/LifExport.h"
#include "io/LifFile.h"

#include <QHash>
#include <QImage>
#include <QThreadPool>
#include <QWidget>

#include <atomic>
#include <memory>

class QCheckBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QSlider;
class QSpinBox;
class QSplitter;
class QTableWidget;
class QTimer;
class QToolBar;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;

namespace lm {

class ImageView;

class LifViewer : public QWidget {
    Q_OBJECT
public:
    explicit LifViewer(QWidget *parent = nullptr);
    ~LifViewer() override;

    // Reads the file's index in the background, then shows its first image.
    void openFile(const QString &path);
    QString path() const { return m_path; }

    // What is shown (also for the tests)
    bool isLoaded() const { return m_loaded; }
    bool isReading() const { return m_reading; }
    const LifFileIndex &fileIndex() const { return m_index; }
    int currentImage() const { return m_current; }
    QImage displayedImage() const;
    QList<LifChannelDisplay> display() const { return m_display.value(m_current); }
    LifCoord coordinate() const { return m_coord; }
    int subsample() const { return m_subsample; }
    QString lastError() const { return m_error; }

    void selectImage(int index);
    void setCoordinate(int dimId, int value);
    void setChannelVisible(int channel, bool on);
    void setChannelRange(int channel, int low, int high);
    void setProjection(bool on);
    void setMergeTiles(bool on);

    // The current image as exporting it would use it: plane, contrast, colours.
    LifExportItem currentExportItem() const;
    QList<int> selectedImages() const;

signals:
    void openInProcess(const Image16 &image, const ImageMetadata &meta);
    void fileLoaded(bool ok);  // the index was read, or could not be
    void planeShown();         // a plane finished loading and is displayed

protected:
    void closeEvent(QCloseEvent *e) override;

private:
    void buildTree();
    void addNode(const LifNode &n, QTreeWidgetItem *parent);
    void applyFilter(const QString &text);
    void buildDimControls();
    void buildChannelControls();
    void syncChannelControls();
    void updateInfo();
    void updateTitle();
    void startThumbnails();
    LifRequest request() const;
    void requestPlanes();
    void render(bool resetView = false);
    void showValues(const QPoint &p, bool inside);
    void step(int dimId, int delta);
    void togglePlay(int dimId);
    void exportImages(int scope);
    void sendToProcess();
    void copyImage();
    void showXml();
    void selectRelative(int delta);

    QString m_path;
    LifFileIndex m_index;
    bool m_loaded = false;
    bool m_reading = false;
    QString m_error;
    int m_current = -1;
    LifCoord m_coord;
    bool m_project = false;
    bool m_merge = true;
    int m_subsample = 1;
    // per image: how its channels are shown (kept while browsing the file)
    QHash<int, QList<LifChannelDisplay>> m_display;
    // the planes on screen, for drawing again and for the values under the cursor
    QList<LifPlane> m_planes;
    int m_planesImage = -1;
    QImage m_shown;

    // loading in the background: a newer request makes an older one stale
    QThreadPool m_readPool, m_thumbPool;
    std::shared_ptr<std::atomic<int>> m_generation = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<bool>> m_cancel;
    std::shared_ptr<std::atomic<int>> m_thumbGeneration = std::make_shared<std::atomic<int>>(0);
    bool m_pending = false;     // another plane was asked for while one was loading
    bool m_loading = false;

    QToolBar *m_toolbar = nullptr;
    QSplitter *m_split = nullptr;
    QLineEdit *m_filter = nullptr;
    QTreeWidget *m_tree = nullptr;
    QLabel *m_treeInfo = nullptr;
    ImageView *m_view = nullptr;
    QWidget *m_dimBox = nullptr;
    QGridLayout *m_dimGrid = nullptr;
    QCheckBox *m_projectBox = nullptr;
    QCheckBox *m_mergeBox = nullptr;
    struct DimControl {
        int dimId;
        int index;            // into planeDims
        QSlider *slider;
        QSpinBox *spin;
        QLabel *value;
        QToolButton *play;
    };
    QList<DimControl> m_dims;
    QTimer *m_playTimer = nullptr;
    int m_playDim = -1;
    QWidget *m_channelBox = nullptr;
    QVBoxLayout *m_channelLayout = nullptr;
    struct ChannelControl {
        QCheckBox *visible;
        QToolButton *colour;
        QSpinBox *low;
        QSpinBox *high;
    };
    QList<ChannelControl> m_channels;
    QTableWidget *m_info = nullptr;
    QLabel *m_values = nullptr;
    QLabel *m_status = nullptr;
    QHash<int, QTreeWidgetItem *> m_items; // image index -> tree item
};

} // namespace lm
