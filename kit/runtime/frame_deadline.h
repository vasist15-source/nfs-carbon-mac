#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

// Fractional monotonic deadlines survive millisecond scheduler rounding.
// A late frame starts a new phase instead of trying to catch up in a burst.
class FrameDeadline {
    int rate_ = 0;
    int64_t deadline_ = 0;

  public:
    void begin(int rate, int64_t now_ns) {
        if (rate != 40 && rate != 60 && rate != 120) {
            rate_ = 0;
            deadline_ = 0;
            return;
        }
        const int64_t period = 1000000000LL / rate;
        if (rate_ != rate || !deadline_ || now_ns > deadline_ + period)
            deadline_ = now_ns + period;
        // The guest also reaches begin while inactive, when it skips drawing
        // and never reaches wait_ms. Those passes must not accumulate future
        // slots: returning to the window would otherwise sleep 100 ms on each
        // frame until an arbitrarily large backlog was exhausted.
        else
            deadline_ = std::min(deadline_ + period, now_ns + period);
        rate_ = rate;
    }
    uint32_t wait_ms(int64_t now_ns) const {
        if (!rate_ || now_ns >= deadline_)
            return 0;
        return uint32_t(std::min<int64_t>((deadline_ - now_ns + 999999) / 1000000, 100));
    }
};

// 3.0.2 (owner, 13 Oct: "instead of running at 60 fps, it continually went down to 59.5 ... a frame pacing issue"):
// when the presenter paces at the frame limit itself (60 on a 120 Hz or 60 Hz display), the limit follows the
// presenter's refresh slots on the presenter's own clock, one frame per slot. FrameDeadline's separate wall clock
// drifts against the display's: frames bunched up two to a slot and the presenter's mailbox dropped one (242 of
// 57,900 in the owner's session, about one every 4 s). A frame that waited is given the next slot even when the
// scheduler wakes it a little early; a late frame starts a new phase and never catches up in a burst.
class SlotPacer {
    int64_t last_ = std::numeric_limits<int64_t>::min();

  public:
    static int64_t slot(double now, double period) {
        return int64_t(std::floor(now / period));
    }
    bool started() const {
        return last_ != std::numeric_limits<int64_t>::min();
    }
    // Milliseconds to wait before presenting at `now` (seconds on the presenter's clock): to the start of the slot
    // after the previous presentation's, when this one would land in the same slot. At most 100.
    uint32_t wait_ms(double now, double period) const {
        if (!started() || !(period > 0) || slot(now, period) > last_)
            return 0;
        const double until = double(last_ + 1) * period - now;
        return uint32_t(std::clamp(std::ceil(until * 1000.0), 0.0, 100.0));
    }
    // After the wait: this presentation's slot (never the previous one again).
    void presented(double now, double period) {
        if (!(period > 0))
            return;
        const int64_t s = slot(now, period);
        last_ = started() ? std::max(s, last_ + 1) : s;
    }
};
