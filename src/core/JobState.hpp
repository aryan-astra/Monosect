// MonoSect — job states.
//
// The state machine exists so that the UI and the job database cannot disagree
// about what a job is doing.  Pausing and cancelling are real states with real
// behaviour, not labels.
#pragma once

#include <string>

namespace monosect::core {

enum class JobState {
    Idle,
    Planning,
    Planned,
    Executing,
    Paused,
    Verifying,
    Publishing,
    Completed,
    Failed,
    Cancelled,
};

[[nodiscard]] const char* jobStateName(JobState state) noexcept;
[[nodiscard]] bool isTerminalState(JobState state) noexcept;
[[nodiscard]] bool isRunningState(JobState state) noexcept;

/// Legal transitions.  Anything else is a programming error and is reported as
/// such by the job runner rather than being silently allowed.
[[nodiscard]] bool isLegalTransition(JobState from, JobState to) noexcept;

/// What "pause" means for a job, spelled out for the user interface.
struct PauseSemantics {
    static const char* title() { return "Pause takes effect at the next part boundary"; }
    static const char* detail() {
        return "FFmpeg is writing one part at a time. Pausing stops MonoSect from starting the next part, so the "
               "part that is being written is always finished first. This keeps every published file complete and "
               "verified; no partial file is ever left behind.";
    }
};

} // namespace monosect::core
