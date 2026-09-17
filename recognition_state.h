#pragma once

/*
 * Small, thread-confined confirmation state machine.
 *
 * Ranking identifies a candidate for one feature extraction. This class turns
 * repeated, separated candidate scores into a short-lived confirmation and
 * resets immediately on ambiguous, stale, invalid, or identity-changing input.
 */

#include <chrono>
#include <cmath>
#include <cstdint>

// Used only by the inference thread. Thresholds are development gates, not
// accuracy claims calibrated from a production-scale evaluation set.
class RecognitionState {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr int RequiredHits = 3;
    static constexpr double MinScore = 0.65;
    static constexpr double MinGap = 0.20;
    static constexpr std::chrono::milliseconds Timeout{1500};

    // Clears every field together so callers cannot reuse a stale person id.
    void reset() {
        id = -1;
        hits = 0;
        score = 0;
        lastAccepted = {};
    }

    // Keeps confirmations tied to a recently observed face rather than an old
    // accepted result after the person leaves the camera view.
    void expire(Clock::time_point now) {
        if (hits && now - lastAccepted >= Timeout) reset();
    }

    // Accepts one ranked observation only when both the absolute score and the
    // margin to the second person pass. Returns true for an accepted observation,
    // not necessarily for a fully confirmed identity; inspect confirmed().
    bool observe(std::int64_t candidate, double best, double second,
                 Clock::time_point now) {
        expire(now);
        if (candidate <= 0 || !std::isfinite(best) || !std::isfinite(second) ||
            best < -1 || best > 1 || second < -1 || second > 1 ||
            best < MinScore || best - second < MinGap) {
            reset();
            return false;
        }
        if (id != candidate) {
            reset();
            id = candidate;
        }
        if (hits < RequiredHits) ++hits;
        score = best;
        lastAccepted = now;
        return true;
    }

    bool confirmed() const { return hits == RequiredHits; }
    std::int64_t id = -1;
    int hits = 0;
    double score = 0;
    Clock::time_point lastAccepted{};
};
