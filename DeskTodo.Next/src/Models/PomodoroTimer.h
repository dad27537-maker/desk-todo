#pragma once

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

    if (state.focusPhase) ++state.completedFocusSessions;
    state.focusPhase = !state.focusPhase;
    state.remainingSeconds = PhaseDuration(state);
    return true;
}

}
