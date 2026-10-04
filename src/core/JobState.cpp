#include "core/JobState.hpp"

namespace monosect::core {

const char* jobStateName(JobState state) noexcept {
    switch (state) {
        case JobState::Idle: return "idle";
        case JobState::Planning: return "planning";
        case JobState::Planned: return "planned";
        case JobState::Executing: return "writing";
        case JobState::Paused: return "paused";
        case JobState::Verifying: return "verifying";
        case JobState::Publishing: return "publishing";
        case JobState::Completed: return "completed";
        case JobState::Failed: return "failed";
        case JobState::Cancelled: return "cancelled";
    }
    return "unknown";
}

bool isTerminalState(JobState state) noexcept {
    return state == JobState::Completed || state == JobState::Failed || state == JobState::Cancelled;
}

bool isRunningState(JobState state) noexcept {
    return state == JobState::Planning || state == JobState::Executing || state == JobState::Verifying ||
           state == JobState::Publishing;
}

bool isLegalTransition(JobState from, JobState to) noexcept {
    if (from == to) {
        return true;
    }
    if (isTerminalState(from)) {
        return false;
    }
    switch (from) {
        case JobState::Idle:
            return to == JobState::Planning;
        case JobState::Planning:
            return to == JobState::Planned || to == JobState::Failed || to == JobState::Cancelled;
        case JobState::Planned:
            return to == JobState::Executing || to == JobState::Cancelled || to == JobState::Failed;
        case JobState::Executing:
            return to == JobState::Verifying || to == JobState::Paused || to == JobState::Cancelled ||
                   to == JobState::Failed || to == JobState::Completed;
        case JobState::Paused:
            return to == JobState::Executing || to == JobState::Cancelled || to == JobState::Failed;
        case JobState::Verifying:
            return to == JobState::Executing || to == JobState::Publishing || to == JobState::Failed ||
                   to == JobState::Cancelled;
        case JobState::Publishing:
            return to == JobState::Executing || to == JobState::Completed || to == JobState::Failed ||
                   to == JobState::Cancelled;
        default:
            return false;
    }
}

} // namespace monosect::core
