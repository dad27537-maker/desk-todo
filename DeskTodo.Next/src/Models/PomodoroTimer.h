#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>

struct PomodoroTimerState {
    int remainingSeconds = 25 * 60;
    bool running = false;
    bool focusPhase = true;
    int completedFocusSessions = 0;
};

namespace PomodoroTimer {

constexpr int FocusSeconds = 25 * 60;
constexpr int BreakSeconds = 5 * 60;

inline int PhaseDuration(const PomodoroTimerState& state) {
    return state.focusPhase ? FocusSeconds : BreakSeconds;
}

inline void Reset(PomodoroTimerState& state) {
    state.running = false;
    state.remainingSeconds = PhaseDuration(state);
}

inline bool Tick(PomodoroTimerState& state) {
    if (!state.running) return false;
    if (state.remainingSeconds > 1) {
        --state.remainingSeconds;
        return false;
    }

    if (state.focusPhase && state.completedFocusSessions < (std::numeric_limits<int>::max)()) ++state.completedFocusSessions;
    state.focusPhase = !state.focusPhase;
    state.remainingSeconds = PhaseDuration(state);
    return true;
}

inline bool Advance(PomodoroTimerState& state, uint64_t seconds) {
    if (!state.running || !seconds) return false;
    bool changed = false;
    // Reduce whole cycles first, so waking from sleep never loops per second.
    const uint64_t cycles = seconds / (FocusSeconds + BreakSeconds);
    if (cycles) {
        state.completedFocusSessions = static_cast<int>((std::min)(
            uint64_t{(std::numeric_limits<int>::max)()},
            static_cast<uint64_t>((std::max)(0, state.completedFocusSessions)) + cycles));
        seconds %= FocusSeconds + BreakSeconds;
        changed = true;
    }
    while (seconds >= static_cast<uint64_t>((std::max)(1, state.remainingSeconds))) {
        seconds -= (std::max)(1, state.remainingSeconds);
        state.remainingSeconds = 1;
        Tick(state);
        changed = true;
    }
    state.remainingSeconds -= static_cast<int>(seconds);
    return changed;
}

}
