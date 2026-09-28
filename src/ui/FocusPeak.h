#pragma once
// The "peak" the focus assistant compares the current sharpness with.
//
// A plain maximum since the last reset goes stale: after the stage moves to
// another field, or another objective is chosen, the old field's sharpness
// stays as the target and the bar sits near zero whatever the focus. This holds
// the peak through a focus sweep (turning the fine focus past the best point and
// back takes a few seconds), lets it fade slowly after that, and starts afresh
// when the sharpness stays far below it - the sign of a different field.

#include <algorithm>
#include <cmath>

namespace lm {

class FocusPeak {
public:
    // How long the peak takes to fade to 1/e of itself when nothing reaches it.
    static constexpr double kFadeSeconds = 8.0;
    // Below this fraction of the peak for this long means another field.
    static constexpr double kNewFieldFraction = 0.25;
    static constexpr double kNewFieldSeconds = 1.5;

    // value: the current sharpness; seconds: time since the previous update
    // (0 on the first). Returns the peak to show.
    double update(double value, double seconds)
    {
        if (!(value >= 0) || !std::isfinite(value))
            return m_peak;
        if (seconds > 0 && m_peak > 0)
            m_peak *= std::exp(-seconds / kFadeSeconds);
        if (m_peak > 0 && value < kNewFieldFraction * m_peak) {
            m_lowFor += std::max(0.0, seconds);
            if (m_lowFor >= kNewFieldSeconds)
                m_peak = value; // a different field: its own sharpness is the new reference
        } else {
            m_lowFor = 0;
        }
        m_peak = std::max(m_peak, value);
        return m_peak;
    }
    void reset()
    {
        m_peak = 0;
        m_lowFor = 0;
    }
    double peak() const { return m_peak; }

private:
    double m_peak = 0;
    double m_lowFor = 0;
};

} // namespace lm
