#pragma once
// Extended depth of field ("Multifocus"): frames taken at different focus
// positions are fused into one image in which every region is in focus.
// Designed for manual microscopes: frames are added incrementally while the
// user turns the fine focus, and drift between frames is compensated.

#include "core/Frame.h"

#include <mutex>

namespace lm {

class FocusStacker {
public:
    enum class Mode {
        MaxContrast, // take each pixel from the sharpest frame (crisp, can be noisy)
        Weighted,    // contrast-weighted blend (smoother transitions)
    };

    void reset();
    // The mode of a stack is fixed by its first frame: a change made while a
    // stack is in progress applies from the next reset().
    void setMode(Mode m);
    Mode mode() const;
    void setAlign(bool on);
    void setWindow(int radius);

    // Adds a linear frame; returns the fraction of pixels it improved.
    double add(const Image16 &frame);

    int frameCount() const;
    Image16 result() const;
    // Per-pixel index of the sharpest frame, normalised to 0..65535 (a
    // "topography" map similar to LAS Multifocus' height map).
    Image16 depthMap() const;

private:
    mutable std::mutex m_mutex;
    Mode m_mode = Mode::MaxContrast;      // requested
    Mode m_stackMode = Mode::MaxContrast; // of the stack in progress (latched at its first frame)
    bool m_align = true;
    int m_window = 3;
    int m_count = 0;
    Image16 m_composite;
    ImageF m_best;
    ImageF m_depth;
    std::vector<float> m_accum;   // weighted mode
    std::vector<float> m_weights; // weighted mode
    ImageF m_refGray;             // first frame (alignment reference)
};

} // namespace lm
