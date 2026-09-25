#pragma once
// Live image stitching for manual stages (like LAS "Live Image Builder"):
// the user moves the slide by hand; every preview frame is tracked by phase
// correlation and, whenever the view has moved onto enough uncovered area and
// the stage is at rest, the frame is refined against the mosaic and blended in.

#include "core/Frame.h"
#include "imaging/Registration.h"

#include <mutex>

namespace lm {

class MosaicBuilder {
public:
    struct Status {
        bool tracking = false;     // current frame position is known
        bool added = false;        // this frame was added to the mosaic
        double confidence = 0.0;
        double posX = 0.0, posY = 0.0; // top-left of the current frame in mosaic coordinates
        int tiles = 0;
        double newAreaFraction = 0.0;
    };

    struct Options {
        double minNewArea = 0.15;      // add a tile when this fraction of the view is uncovered
        double maxRestMotion = 3.0;    // px/frame (registration scale): slow enough to add a tile
        double minConfidence = 0.04;
        int featherPixels = 64;        // blending ramp width at tile edges
        bool autoAdd = true;
    };

    void reset();
    void setOptions(const Options &o);
    Options options() const;

    // Feeds a linear frame. If forceAdd is true the frame is added regardless
    // of coverage (the "Add tile" button).
    Status feed(const Image16 &frame, bool forceAdd = false);

    Status status() const;
    // Full resolution mosaic cropped to the covered area; uncovered pixels are
    // filled with `background` (65535 = white, for bright field).
    Image16 result(uint16_t background = 65535) const;
    // Downscaled preview (longest side <= maxSize) and the current frame rect.
    Image16 preview(int maxSize, double &scale, uint16_t background = 65535) const;
    // Tile boundaries (mosaic coordinates, relative to result())
    struct TileRect { int x, y, w, h; };
    std::vector<TileRect> tiles() const;

private:
    void ensureCanvas(int x0, int y0, int x1, int y1);
    void paste(const Image16 &frame, int x, int y);
    double uncoveredFraction(int x, int y, int w, int h) const;
    ImageF canvasGray(int x, int y, int w, int h, int factor) const;

    mutable std::mutex m_mutex;
    Options m_opt;
    Status m_status;
    Image16 m_canvas;
    std::vector<uint8_t> m_weight;  // feather weight of stored pixel (0 = empty)
    int m_originX = 0, m_originY = 0; // canvas pixel (0,0) in mosaic coordinates
    ImageF m_prevGray;
    int m_factor = 1;
    double m_posX = 0, m_posY = 0;
    bool m_lost = false;
    std::vector<TileRect> m_tiles;
    int m_frameW = 0, m_frameH = 0;
};

} // namespace lm
